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

// --------------------------------------------------------------------------
// Home Assistant MQTT discovery
//
// Every entity reads the same retained JSON snapshot through a value_template,
// so HA needs no extra topics, and all of them share the bridge's availability
// topic - when the last will fires, the whole device goes unavailable at once.
// --------------------------------------------------------------------------
const HA_ENTITIES = [
  {
    id: 'session_usage',
    name: 'Session Usage (5h)',
    tpl: "{{ value_json.session.used_pct | default('unknown') }}",
    unit: '%',
    icon: 'mdi:speedometer',
    stateClass: 'measurement',
  },
  {
    id: 'weekly_usage',
    name: 'Weekly Usage (7d)',
    tpl: "{{ value_json.week.used_pct | default('unknown') }}",
    unit: '%',
    icon: 'mdi:calendar-week',
    stateClass: 'measurement',
  },
  {
    id: 'session_resets',
    name: 'Session Limit Resets',
    tpl:
      '{% if value_json.session.resets_at is defined and value_json.session.resets_at > 0 %}' +
      '{{ value_json.session.resets_at | int | as_datetime }}{% else %}unknown{% endif %}',
    deviceClass: 'timestamp',
  },
  {
    id: 'weekly_resets',
    name: 'Weekly Limit Resets',
    tpl:
      '{% if value_json.week.resets_at is defined and value_json.week.resets_at > 0 %}' +
      '{{ value_json.week.resets_at | int | as_datetime }}{% else %}unknown{% endif %}',
    deviceClass: 'timestamp',
  },
  { id: 'status', name: 'Status', tpl: '{{ value_json.status }}', icon: 'mdi:robot' },
  {
    id: 'activity',
    name: 'Activity',
    tpl: "{{ value_json.detail | default('') | truncate(250) }}",
    icon: 'mdi:text-short',
  },
  {
    id: 'context_usage',
    name: 'Context Used',
    tpl: "{{ value_json.context_pct | default('unknown') }}",
    unit: '%',
    icon: 'mdi:memory',
    stateClass: 'measurement',
  },
  {
    // Not device_class monetary: cost resets to 0 on /clear, which would make
    // HA's long-term statistics treat each session as a negative adjustment.
    id: 'session_cost',
    name: 'Session Cost',
    tpl: '{{ value_json.cost_usd | default(0) | round(4) }}',
    unit: 'USD',
    icon: 'mdi:currency-usd',
    stateClass: 'measurement',
  },
  {
    id: 'model',
    name: 'Model',
    tpl: "{{ value_json.model | default('') }}",
    icon: 'mdi:brain',
    category: 'diagnostic',
  },
  {
    id: 'project',
    name: 'Project',
    tpl: "{{ value_json.project | default('') }}",
    icon: 'mdi:folder',
    category: 'diagnostic',
  },
  {
    kind: 'binary_sensor',
    id: 'needs_attention',
    name: 'Needs Attention',
    tpl: "{% if value_json.status == 'needs_you' %}ON{% else %}OFF{% endif %}",
    deviceClass: 'problem',
  },
  {
    kind: 'binary_sensor',
    id: 'working',
    name: 'Working',
    tpl: "{% if value_json.status == 'working' %}ON{% else %}OFF{% endif %}",
    icon: 'mdi:cog-sync',
  },
];

function haDiscoveryMessages(cfg) {
  const ha = cfg.homeassistant || {};
  const prefix = ha.discoveryPrefix || 'homeassistant';
  const node = ha.nodeId || 'claude_code';
  const device = {
    identifiers: [node],
    name: ha.deviceName || 'Claude Code',
    manufacturer: 'Anthropic',
    model: 'Claude Code e-Paper Bridge',
  };

  return HA_ENTITIES.map((e) => ({
    topic: `${prefix}/${e.kind || 'sensor'}/${node}/${e.id}/config`,
    payload: JSON.stringify({
      name: e.name,
      unique_id: `${node}_${e.id}`,
      state_topic: cfg.topics.state,
      value_template: e.tpl,
      availability_topic: cfg.topics.bridge,
      payload_available: 'online',
      payload_not_available: 'offline',
      device,
      ...(e.unit ? { unit_of_measurement: e.unit } : {}),
      ...(e.icon ? { icon: e.icon } : {}),
      ...(e.deviceClass ? { device_class: e.deviceClass } : {}),
      ...(e.stateClass ? { state_class: e.stateClass } : {}),
      ...(e.category ? { entity_category: e.category } : {}),
    }),
  }));
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

  // Created only after we win the port, below. A daemon that loses the race
  // must never open an MQTT session: exiting would drop the connection without
  // a clean DISCONNECT, the broker would fire its last will, and the retained
  // bridge topic would read "offline" while the winning daemon is running -
  // blanking the panel and marking every Home Assistant entity unavailable.
  let client = null;

  // A restarting daemon must not clobber the retained snapshot with its empty
  // defaults - that would blank the panel's gauges until the next statusline
  // arrives, which after a reboot could be a long time. So on first connect we
  // adopt the retained state as our baseline rather than publishing over it.
  let seeded = false;
  let seedTimer = null;

  function finishSeeding() {
    if (seeded) return;
    seeded = true;
    clearTimeout(seedTimer);
    if (client) client.unsubscribe(cfg.topics.state);
  }

  function startMqtt() {
    client = mqttLib.connect(cfg.mqtt.url, {
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

      // Retained and idempotent, so republishing on every connect is what
      // makes the entities reappear after a Home Assistant restart.
      if (cfg.homeassistant && cfg.homeassistant.enabled) {
        const msgs = haDiscoveryMessages(cfg);
        for (const m of msgs) client.publish(m.topic, m.payload, { retain: true });
        log(`published ${msgs.length} Home Assistant discovery configs`);
      }

      if (!seeded) {
        client.subscribe(cfg.topics.state);
        // No retained message arrives if the topic was never published.
        seedTimer = setTimeout(finishSeeding, 1500);
      }
    });

    client.on('message', (topic, payload) => {
      if (seeded || topic !== cfg.topics.state) return;
      try {
        Object.assign(state, JSON.parse(payload.toString()));
        log('seeded in-memory state from retained snapshot');
      } catch (e) {
        log(`retained snapshot unparseable, ignoring: ${e.message}`);
      }
      finishSeeding();
    });

    client.on('error', (e) => log(`mqtt error: ${e.message}`));
    client.on('close', () => log('mqtt connection closed'));
  }

  let publishTimer = null;
  function publish() {
    if (publishTimer) return; // coalesce bursts
    publishTimer = setTimeout(() => {
      publishTimer = null;
      if (!client) return; // broker connection not up yet
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
        // Not 'offline': with an always-on bridge, OFFLINE should mean the
        // bridge itself is gone. Closing Claude Code just means no session,
        // and the usage gauges stay meaningful and on screen.
        state.status = 'idle';
        state.detail =
          liveSessions.size === 0
            ? 'no active session'
            : `${liveSessions.size} session(s) open`;
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
    // Another daemon already owns the port; that one wins. No MQTT session
    // exists yet, so exiting here is silent on the broker.
    log(`not starting, another daemon owns the port: ${e.message}`);
    process.exit(0);
  });

  // Bind first, connect to the broker only once we know we are the daemon.
  server.listen(cfg.port, '127.0.0.1', () => {
    log(`daemon listening on 127.0.0.1:${cfg.port}`);
    startMqtt();
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
    if (!client) process.exit(0);
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

// Publishing an empty payload to a discovery topic is how HA is told to drop
// the entity, so the same code path handles setup and teardown.
async function runDiscovery(remove) {
  const cfg = loadConfig();
  const mqttLib = require('mqtt');
  const msgs = haDiscoveryMessages(cfg);

  await new Promise((resolve) => {
    const client = mqttLib.connect(cfg.mqtt.url, {
      username: cfg.mqtt.username || undefined,
      password: cfg.mqtt.password || undefined,
      clientId: `claude-epaper-discovery-${process.pid}`,
    });
    client.on('error', (e) => {
      console.log(`broker error: ${e.message}`);
      client.end(true, resolve);
    });
    client.on('connect', () => {
      let done = 0;
      for (const m of msgs) {
        client.publish(m.topic, remove ? '' : m.payload, { retain: true }, () => {
          if (++done === msgs.length) {
            console.log(
              `${remove ? 'removed' : 'published'} ${msgs.length} entities on ${cfg.mqtt.url}`
            );
            if (!remove) {
              console.log('look for a "Claude Code" device under Settings > Devices & Services > MQTT');
            }
            client.end(false, resolve);
          }
        });
      }
    });
  });
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
    case 'discovery':
      await runDiscovery(arg === '--remove');
      break;
    default:
      console.log(
        'usage: bridge.js daemon|statusline|hook <Event>|demo|status|discovery [--remove]'
      );
      process.exit(1);
  }
})();
