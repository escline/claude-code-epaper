#!/usr/bin/env node
'use strict';

/*
 * Claude Code -> MQTT bridge for the e-paper display.
 *
 * Modes:
 *   node bridge.js daemon        long-running publisher (auto-spawned)
 *   node bridge.js statusline    called by Claude Code's statusLine setting
 *   node bridge.js hook <Event>  called by a Claude Code hook
 *   node bridge.js status        diagnostics
 *   node bridge.js demo          publish a fake state to exercise the panel
 *
 * Why a daemon: statusline runs on nearly every message. Doing an MQTT
 * connect/publish/disconnect each time would add latency to the status line and
 * thrash the broker, so the thin clients just hand a line of JSON to a
 * long-lived process over localhost TCP and exit.
 */

const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const CONFIG_PATH = path.join(__dirname, 'config.json');
const LOG_PATH = path.join(__dirname, 'bridge.log');

// --------------------------------------------------------------------------
// Config
// --------------------------------------------------------------------------
function loadConfig() {
  const defaults = {
    mqtt: { url: 'mqtt://127.0.0.1:1883', username: '', password: '' },
    topics: { state: 'claude/display/state', bridge: 'claude/display/bridge' },
    port: 8787,
    idleAfterMs: 300000,
  };
  try {
    const raw = JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8'));
    return {
      ...defaults,
      ...raw,
      mqtt: { ...defaults.mqtt, ...(raw.mqtt || {}) },
      topics: { ...defaults.topics, ...(raw.topics || {}) },
    };
  } catch {
    return defaults;
  }
}

// --------------------------------------------------------------------------
// Shared helpers
// --------------------------------------------------------------------------

// The detail line is one row of FreeSans9pt7b across 372px, so ~44 chars.
function shorten(s, max = 44) {
  if (!s) return '';
  const flat = String(s).replace(/\s+/g, ' ').trim();
  return flat.length <= max ? flat : flat.slice(0, max - 1) + '…';
}

function basename(p) {
  if (!p) return '';
  return path.basename(p.replace(/[\\/]+$/, ''));
}

function readStdin() {
  return new Promise((resolve) => {
    let buf = '';
    if (process.stdin.isTTY) return resolve('');
    process.stdin.setEncoding('utf8');
    process.stdin.on('data', (d) => (buf += d));
    process.stdin.on('end', () => resolve(buf));
    process.stdin.on('error', () => resolve(buf));
  });
}

// --------------------------------------------------------------------------
// Thin client: hand one JSON line to the daemon and get out of the way.
// --------------------------------------------------------------------------
function sendToDaemon(cfg, payload, { spawnIfDown = true } = {}) {
  return new Promise((resolve) => {
    let settled = false;
    const done = (ok) => {
      if (!settled) {
        settled = true;
        resolve(ok);
      }
    };

    const sock = net.createConnection({ host: '127.0.0.1', port: cfg.port });
    sock.setTimeout(400);

    sock.on('connect', () => {
      sock.end(JSON.stringify(payload) + '\n', () => done(true));
    });
    sock.on('timeout', () => {
      sock.destroy();
      done(false);
    });
    sock.on('error', (err) => {
      sock.destroy();
      if (err.code === 'ECONNREFUSED' && spawnIfDown) {
        startDaemon();
        // Give it a moment to bind, then try once more.
        setTimeout(() => {
          sendToDaemon(cfg, payload, { spawnIfDown: false }).then(done);
        }, 600);
      } else {
        done(false);
      }
    });
  });
}

function startDaemon() {
  try {
    const child = spawn(process.execPath, [__filename, 'daemon'], {
      detached: true,
      stdio: 'ignore',
      windowsHide: true,
    });
    child.unref();
  } catch {
    /* nothing useful to do from a hook */
  }
}

// --------------------------------------------------------------------------
// Daemon
// --------------------------------------------------------------------------
function runDaemon() {
  const cfg = loadConfig();
  let mqttLib;
  try {
    mqttLib = require('mqtt');
  } catch {
    console.error('mqtt package missing. Run: npm install --prefix bridge');
    process.exit(1);
  }

  function log(msg) {
    const line = `${new Date().toISOString()} ${msg}\n`;
    try {
      fs.appendFileSync(LOG_PATH, line);
    } catch {}
  }

  // The published snapshot. Fields stay absent until something reports them,
  // so the panel can tell "unknown" from "zero".
  const state = {
    status: 'idle',
    detail: '',
    model: '',
    project: '',
    session: {},
    week: {},
    ts: 0,
  };

  // Multi-session policy is "most recent wins", but a SessionEnd from one
  // terminal shouldn't blank the display while another session is still up, so
  // track which sessions are alive.
  const liveSessions = new Map();
  let lastEventAt = Date.now();

  const client = mqttLib.connect(cfg.mqtt.url, {
    username: cfg.mqtt.username || undefined,
    password: cfg.mqtt.password || undefined,
    clientId: `claude-epaper-bridge-${os.hostname()}-${process.pid}`,
    reconnectPeriod: 5000,
    will: {
      topic: cfg.topics.bridge,
      payload: 'offline',
      qos: 0,
      retain: true,
    },
  });

  client.on('connect', () => {
    log(`connected to ${cfg.mqtt.url}`);
    client.publish(cfg.topics.bridge, 'online', { retain: true });
    publish();
  });
  client.on('error', (e) => log(`mqtt error: ${e.message}`));
  client.on('close', () => log('mqtt connection closed'));

  let publishTimer = null;
  function publish() {
    if (publishTimer) return; // coalesce bursts
    publishTimer = setTimeout(() => {
      publishTimer = null;
      state.ts = Math.floor(Date.now() / 1000);
      const json = JSON.stringify(state);
      // Retained, so a rebooting ESP32 repaints correct state immediately.
      client.publish(cfg.topics.state, json, { retain: true, qos: 0 });
    }, 200);
  }

  function applyStatusline(d) {
    if (d.session_id) liveSessions.set(d.session_id, Date.now());

    if (d.model && d.model.display_name) state.model = d.model.display_name;
    if (d.workspace && d.workspace.current_dir) {
      state.project = basename(d.workspace.current_dir);
    }

    const rl = d.rate_limits || {};
    if (rl.five_hour && rl.five_hour.used_percentage != null) {
      state.session = {
        used_pct: Math.round(rl.five_hour.used_percentage),
        resets_at: rl.five_hour.resets_at ?? 0,
      };
    }
    if (rl.seven_day && rl.seven_day.used_percentage != null) {
      state.week = {
        used_pct: Math.round(rl.seven_day.used_percentage),
        resets_at: rl.seven_day.resets_at ?? 0,
      };
    }

    const cw = d.context_window || {};
    if (cw.used_percentage != null) {
      state.context_pct = Math.round(cw.used_percentage);
    } else if (cw.remaining_percentage != null) {
      state.context_pct = Math.round(100 - cw.remaining_percentage);
    }

    if (d.cost && d.cost.total_cost_usd != null) {
      state.cost_usd = Number(d.cost.total_cost_usd.toFixed(4));
    }
  }

  function applyHook(event, d) {
    const sid = d.session_id;
    if (sid) liveSessions.set(sid, Date.now());
    if (d.cwd) state.project = basename(d.cwd);

    // Hook payload field names have drifted before. These events are low
    // frequency, so recording the shape is cheap and makes a blank detail line
    // diagnosable from the log rather than guesswork.
    log(`hook ${event} keys=[${Object.keys(d).join(',')}]`);

    switch (event) {
      case 'SessionStart':
        state.status = 'idle';
        state.detail = 'session started';
        break;

      case 'UserPromptSubmit':
        state.status = 'working';
        state.detail = shorten(d.user_prompt ?? d.prompt ?? '');
        break;

      case 'PreToolUse':
        state.status = 'working';
        state.detail = d.tool_name ? `running ${d.tool_name}` : 'working';
        break;

      case 'SubagentStart':
        state.status = 'working';
        state.detail = 'subagent running';
        break;

      case 'Notification':
        // This is the event the whole display exists for.
        state.status = 'needs_you';
        state.detail =
          d.notification_type === 'permission_prompt'
            ? shorten(d.message || 'permission needed')
            : shorten(d.message || 'needs your attention');
        break;

      case 'Stop':
        state.status = 'idle';
        state.detail = 'waiting for you';
        break;

      case 'SessionEnd':
        if (sid) liveSessions.delete(sid);
        if (liveSessions.size === 0) {
          state.status = 'offline';
          state.detail = '';
        } else {
          state.status = 'idle';
          state.detail = `${liveSessions.size} session(s) open`;
        }
        break;

      default:
        return false;
    }
    return true;
  }

  const server = net.createServer((sock) => {
    let buf = '';
    sock.setEncoding('utf8');
    sock.on('data', (chunk) => {
      buf += chunk;
      let nl;
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl);
        buf = buf.slice(nl + 1);
        if (!line.trim()) continue;
        try {
          const msg = JSON.parse(line);
          lastEventAt = Date.now();
          if (msg.kind === 'statusline') {
            applyStatusline(msg.data || {});
            publish();
          } else if (msg.kind === 'hook') {
            if (applyHook(msg.event, msg.data || {})) publish();
          } else if (msg.kind === 'demo') {
            Object.assign(state, msg.data || {});
            publish();
          }
        } catch (e) {
          log(`bad line: ${e.message}`);
        }
      }
    });
    sock.on('error', () => {});
  });

  server.on('error', (e) => {
    // Another daemon already owns the port; that one wins.
    log(`server error: ${e.message}`);
    process.exit(0);
  });

  server.listen(cfg.port, '127.0.0.1', () => {
    log(`daemon listening on 127.0.0.1:${cfg.port}`);
  });

  // If Claude Code dies without firing Stop/SessionEnd, don't leave the panel
  // reading WORKING forever.
  setInterval(() => {
    if (
      state.status === 'working' &&
      Date.now() - lastEventAt > cfg.idleAfterMs
    ) {
      state.status = 'idle';
      state.detail = 'idle';
      publish();
    }
  }, 30000).unref?.();

  const shutdown = () => {
    try {
      client.publish(cfg.topics.bridge, 'offline', { retain: true }, () => {
        client.end(true, () => process.exit(0));
      });
      setTimeout(() => process.exit(0), 1500);
    } catch {
      process.exit(0);
    }
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

// --------------------------------------------------------------------------
// statusline mode: forward usage data, then print the terminal status line.
// Must never throw and never block for long.
// --------------------------------------------------------------------------
async function runStatusline() {
  const cfg = loadConfig();
  const raw = await readStdin();
  let d = {};
  try {
    d = JSON.parse(raw);
  } catch {}

  sendToDaemon(cfg, { kind: 'statusline', data: d }).catch(() => {});

  const bits = [];
  if (d.model && d.model.display_name) bits.push(d.model.display_name);
  if (d.workspace && d.workspace.current_dir) {
    bits.push(basename(d.workspace.current_dir));
  }
  const rl = d.rate_limits || {};
  if (rl.five_hour && rl.five_hour.used_percentage != null) {
    bits.push(`5h ${Math.round(rl.five_hour.used_percentage)}%`);
  }
  if (rl.seven_day && rl.seven_day.used_percentage != null) {
    bits.push(`7d ${Math.round(rl.seven_day.used_percentage)}%`);
  }
  const cw = d.context_window || {};
  if (cw.used_percentage != null) {
    bits.push(`ctx ${Math.round(cw.used_percentage)}%`);
  }
  if (d.cost && d.cost.total_cost_usd != null) {
    bits.push(`$${d.cost.total_cost_usd.toFixed(2)}`);
  }
  process.stdout.write(bits.join('  |  '));
}

async function runHook(event) {
  const cfg = loadConfig();
  const raw = await readStdin();
  let d = {};
  try {
    d = JSON.parse(raw);
  } catch {}
  await sendToDaemon(cfg, { kind: 'hook', event, data: d }).catch(() => {});
}

async function runDemo() {
  const cfg = loadConfig();
  const ok = await sendToDaemon(cfg, {
    kind: 'demo',
    data: {
      status: 'needs_you',
      detail: 'permission: Bash (npm test)',
      model: 'Opus 5',
      project: 'esp-paper',
      session: {
        used_pct: 42,
        resets_at: Math.floor(Date.now() / 1000) + 8040,
      },
      week: {
        used_pct: 67,
        resets_at: Math.floor(Date.now() / 1000) + 273600,
      },
      context_pct: 31,
      cost_usd: 1.23,
    },
  });
  console.log(ok ? 'demo state sent' : 'could not reach daemon');
}

async function runStatus() {
  const cfg = loadConfig();
  console.log(`config:  ${fs.existsSync(CONFIG_PATH) ? CONFIG_PATH : 'MISSING (using defaults)'}`);
  console.log(`broker:  ${cfg.mqtt.url}`);
  console.log(`topics:  ${cfg.topics.state} , ${cfg.topics.bridge}`);
  const ok = await sendToDaemon(cfg, { kind: 'ping' }, { spawnIfDown: false });
  console.log(`daemon:  ${ok ? `running on 127.0.0.1:${cfg.port}` : 'not running'}`);
}

// --------------------------------------------------------------------------
const [, , mode, arg] = process.argv;
(async () => {
  switch (mode) {
    case 'daemon':
      runDaemon();
      break;
    case 'statusline':
      await runStatusline();
      break;
    case 'hook':
      await runHook(arg);
      break;
    case 'demo':
      await runDemo();
      break;
    case 'status':
      await runStatus();
      break;
    default:
      console.log('usage: bridge.js daemon|statusline|hook <Event>|demo|status');
      process.exit(1);
  }
})();
