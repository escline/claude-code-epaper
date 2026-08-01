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
 *   node bridge.js demo weather  pin sessions to 0 to show the weather screen
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
    sessionTtlMs: 28800000,
    planUsage: { enabled: true, path: '', pollMs: 60000, freshMs: 900000 },
  };
  try {
    const raw = JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8'));
    return {
      ...defaults,
      ...raw,
      mqtt: { ...defaults.mqtt, ...(raw.mqtt || {}) },
      topics: { ...defaults.topics, ...(raw.topics || {}) },
      planUsage: { ...defaults.planUsage, ...(raw.planUsage || {}) },
    };
  } catch {
    return defaults;
  }
}

// --------------------------------------------------------------------------
// Claude desktop app plan usage
//
// The desktop app polls the account's plan limits every 5 minutes and appends
// the result to plan-usage-history.json, pruned to about two weeks:
//
//   {"t": 1785546766000, "org": "...", "u": {"fh": 4, "sd": 1}}
//
// `fh` and `sd` are the same five-hour and seven-day percentages Claude Code
// reports through rate_limits, and they are account-wide - desktop, web, mobile
// and Claude Code all draw on one quota. Reading this is what keeps the gauges
// honest when Claude Code isn't the thing burning the tokens. A third key `xu`
// shows up on a slower poll with a different scale; it is deliberately ignored
// rather than guessed at.
//
// Only the file's own last sample matters, so it is re-read on mtime change,
// not on a timer.
// --------------------------------------------------------------------------
function defaultPlanUsagePath() {
  if (process.platform === 'win32') {
    const appdata = process.env.APPDATA || path.join(os.homedir(), 'AppData', 'Roaming');
    return path.join(appdata, 'Claude', 'plan-usage-history.json');
  }
  if (process.platform === 'darwin') {
    return path.join(
      os.homedir(),
      'Library',
      'Application Support',
      'Claude',
      'plan-usage-history.json'
    );
  }
  return path.join(os.homedir(), '.config', 'Claude', 'plan-usage-history.json');
}

// Newest sample carrying usage numbers, or null. Never throws: the file belongs
// to another application and may be half-written, absent, or reformatted.
function readLatestPlanSample(file) {
  const raw = fs.readFileSync(file, 'utf8');
  const doc = JSON.parse(raw);
  const samples = Array.isArray(doc) ? doc : doc.samples;
  if (!Array.isArray(samples)) return null;
  for (let i = samples.length - 1; i >= 0; i--) {
    const s = samples[i];
    if (!s || typeof s.t !== 'number' || !s.u) continue;
    const fh = typeof s.u.fh === 'number' ? s.u.fh : null;
    const sd = typeof s.u.sd === 'number' ? s.u.sd : null;
    if (fh === null && sd === null) continue;
    return { t: s.t, fh, sd };
  }
  return null;
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
    // Which surface last moved the gauges: 'claude_code' or 'desktop'. The
    // percentages are account-wide either way, so this only answers "why did
    // usage change while I wasn't in a terminal".
    id: 'usage_source',
    name: 'Usage Source',
    tpl: "{{ value_json.usage_source | default('unknown') }}",
    icon: 'mdi:transit-connection-variant',
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

  // Last prompt text, so PostToolUse can put it back after a permission prompt
  // has been answered instead of leaving the answered question on the panel.
  let promptDetail = '';

  // demo mode may pin the session count, so the panel's weather screen can be
  // exercised without closing every terminal first. null = not pinned, which
  // is the only state any real event can leave it in.
  let demoSessions = null;

  // Desktop-app usage watcher. lastRateLimitAt is when Claude Code last
  // reported rate limits: the two sources describe the same account, so the
  // more recently observed one wins rather than whichever arrived last.
  const planUsageFile = cfg.planUsage.path || defaultPlanUsagePath();
  let planUsageMtime = 0;
  let planSampleT = 0; // `t` of the newest sample already consumed
  let planPrev = null; // its {fh, sd}, for detecting a rise
  let lastRateLimitAt = 0;
  const ACTIVE_ELSEWHERE = 'active in another Claude app';

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
    // Publish once now that seeding is settled. Everything we adopted is kept;
    // the only field this corrects is the session count, which publish() takes
    // from our own (empty) map. Without it, a daemon started at login with
    // Claude Code closed would leave yesterday's retained "1 session" on the
    // broker, and the panel would sit on a stale IDLE forever instead of
    // handing the screen to the weather. Nothing was retained means nothing to
    // clobber, so this is safe on a fresh broker too.
    //
    // Same reason the desktop file is read here rather than waiting out the
    // first poll interval: a daemon started at login with Claude Code closed
    // should come up with usage the desktop app already knows, not a minute of
    // whatever the broker was still holding.
    if (cfg.planUsage.enabled) {
      try {
        pollPlanUsage();
      } catch (e) {
        log(`plan usage seed failed: ${e.message}`);
      }
    }
    publish();
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
      // Always from our own live map, never from the seeded snapshot: a
      // restarted daemon has no sessions no matter what the retained payload
      // claimed, and the panel uses this to decide whether to show weather
      // instead of a status it would be inventing.
      state.sessions = demoSessions ?? liveSessions.size;
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
    // Live and exact, and it carries reset times the desktop file has no field
    // for, so it outranks any desktop sample older than this moment.
    if (rl.five_hour || rl.seven_day) {
      lastRateLimitAt = Date.now();
      state.usage_source = 'claude_code';
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
        promptDetail = shorten(d.user_prompt ?? d.prompt ?? '');
        state.detail = promptDetail;
        break;

      case 'PreToolUse':
        state.status = 'working';
        state.detail = d.tool_name ? `running ${d.tool_name}` : 'working';
        break;

      case 'PostToolUse':
        // The one event that fires between an approved permission prompt and
        // the end of the turn. Without it NEEDS YOU stays on the panel for the
        // whole remainder of the work - 90 s in one logged trace - because
        // nothing else arrives until Stop.
        //
        // Deliberately reports neither the tool nor a fresh detail line when
        // already working: the status zone repaints as often as every 3 s, and
        // a line that changed on every tool call would churn it through a heavy
        // turn for no actionable gain. Returning false here means no publish
        // and no repaint at all, so the cost of registering this hook is one
        // repaint per stale banner rather than one per tool.
        if (state.status === 'working') return false;
        state.status = 'working';
        // Back to what you actually asked for, rather than the permission text
        // that is now answered.
        state.detail = promptDetail || 'working';
        break;

      case 'SubagentStart':
        state.status = 'working';
        state.detail = 'subagent running';
        break;

      case 'Notification':
        // Values, not just keys: Claude Code sends this both for a permission
        // prompt and for the idle "waiting for your input" nudge, and only the
        // first is really NEEDS YOU. Both currently set it, which is suspected
        // in the repeated notifications seen mid-turn at 30-60 s intervals.
        // Logging the type is what will tell us whether to stop treating the
        // idle one as actionable.
        log(
          `notification type=${d.notification_type ?? '(absent)'} ` +
            `message=${JSON.stringify(d.message ?? '')}`
        );
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

  // Fold the desktop app's latest sample into the gauges. Returns true if the
  // snapshot changed and should be published.
  function pollPlanUsage() {
    let st;
    try {
      st = fs.statSync(planUsageFile);
    } catch {
      return false; // desktop app never installed, or a different path
    }
    const mtime = st.mtimeMs;
    if (mtime === planUsageMtime) return false;
    planUsageMtime = mtime;

    let sample;
    try {
      sample = readLatestPlanSample(planUsageFile);
    } catch (e) {
      log(`plan usage unreadable, ignoring: ${e.message}`);
      return false;
    }
    if (!sample || sample.t <= planSampleT) return false;

    const prev = planPrev;
    planSampleT = sample.t;
    planPrev = { fh: sample.fh, sd: sample.sd };

    // A sample only proves what the account looked like when the desktop app
    // last polled. Once the app is closed the file freezes, and a stale 5-hour
    // figure would hold the gauge high long after the window had drained.
    const age = Date.now() - sample.t;
    if (age > cfg.planUsage.freshMs) return false;

    let changed = false;

    // Claude Code's own numbers are exact and carry reset times; only fill in
    // behind them.
    if (sample.t > lastRateLimitAt) {
      if (sample.fh !== null && state.session.used_pct !== sample.fh) {
        state.session = { ...state.session, used_pct: sample.fh };
        changed = true;
      }
      if (sample.sd !== null && state.week.used_pct !== sample.sd) {
        state.week = { ...state.week, used_pct: sample.sd };
        changed = true;
      }
      if (changed) state.usage_source = 'desktop';
    }

    // Quota that went up with no terminal open was spent somewhere else - the
    // desktop app, the web UI, a phone. Worth a line, but not a session: there
    // is no per-message feed behind it, so claiming one would put a status on
    // the panel that nothing can ever clear. The phrase is fixed rather than a
    // ticking "5m ago", which would repaint the zone every minute for nothing.
    const rose =
      prev &&
      ((sample.fh !== null && prev.fh !== null && sample.fh > prev.fh) ||
        (sample.sd !== null && prev.sd !== null && sample.sd > prev.sd));

    if (liveSessions.size === 0 && state.status === 'idle') {
      const want = rose ? ACTIVE_ELSEWHERE : null;
      if (want && state.detail !== want) {
        state.detail = want;
        changed = true;
      } else if (!want && state.detail === ACTIVE_ELSEWHERE) {
        state.detail = 'no active session';
        changed = true;
      }
    }

    if (changed) {
      log(
        `plan usage: 5h ${sample.fh}% 7d ${sample.sd}%` +
          ` (sample ${Math.round(age / 1000)}s old${rose ? ', rose' : ''})`
      );
    }
    return changed;
  }

  if (cfg.planUsage.enabled) {
    setInterval(() => {
      try {
        if (pollPlanUsage()) publish();
      } catch (e) {
        log(`plan usage poll failed: ${e.message}`);
      }
    }, cfg.planUsage.pollMs).unref?.();
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
            const d = msg.data || {};
            // An explicit null releases the pin and hands the count back to
            // the live map, so a plain `demo` undoes a `demo weather`.
            if ('sessions' in d) demoSessions = d.sessions;
            Object.assign(state, d);
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

    // A force-kill leaves a session in the map with no SessionEnd to clear it,
    // and a stuck count would keep the weather screen off the panel forever.
    // The TTL is deliberately long: statusline only fires when there is
    // traffic, so a session left open overnight is silent but genuinely still
    // open - pruning it on the idle timescale would paint weather over a live
    // terminal, which is the behaviour we specifically didn't want.
    const cutoff = Date.now() - cfg.sessionTtlMs;
    let pruned = 0;
    for (const [sid, seenAt] of liveSessions) {
      if (seenAt < cutoff) {
        liveSessions.delete(sid);
        pruned++;
      }
    }
    if (pruned) {
      log(`pruned ${pruned} session(s) last seen over ${cfg.sessionTtlMs} ms ago`);
      if (liveSessions.size === 0 && state.status === 'idle') {
        state.detail = 'no active session';
      }
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

async function runDemo(which) {
  const cfg = loadConfig();
  const now = Math.floor(Date.now() / 1000);

  // `demo weather` drives the panel to the screen it shows when nothing is
  // running. The forecast itself is fetched by the ESP32, not sent from here -
  // all this does is pin the session count to zero so the panel stops having
  // something to report. Plain `demo` releases the pin again.
  const data =
    which === 'weather'
      ? {
          status: 'idle',
          detail: 'no active session',
          sessions: 0,
          session: { used_pct: 42, resets_at: now + 8040 },
          week: { used_pct: 67, resets_at: now + 273600 },
        }
      : {
          status: 'needs_you',
          detail: 'permission: Bash (npm test)',
          model: 'Opus 5',
          project: 'esp-paper',
          sessions: null,
          session: { used_pct: 42, resets_at: now + 8040 },
          week: { used_pct: 67, resets_at: now + 273600 },
          context_pct: 31,
          cost_usd: 1.23,
        };

  const ok = await sendToDaemon(cfg, { kind: 'demo', data });
  console.log(
    ok
      ? `demo state sent${which === 'weather' ? ' (weather screen; run `demo` to release)' : ''}`
      : 'could not reach daemon'
  );
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

  const file = cfg.planUsage.path || defaultPlanUsagePath();
  if (!cfg.planUsage.enabled) {
    console.log('desktop: disabled in config');
  } else {
    try {
      const s = readLatestPlanSample(file);
      if (!s) throw new Error('no usable samples');
      const age = Math.round((Date.now() - s.t) / 1000);
      // Older than freshMs means the desktop app is closed, so its numbers are
      // frozen and the daemon is ignoring them - which is the answer to "why
      // are the gauges not moving".
      console.log(
        `desktop: 5h ${s.fh}% 7d ${s.sd}%, sampled ${age}s ago` +
          (age * 1000 > cfg.planUsage.freshMs ? ' (stale, app closed - ignored)' : '')
      );
    } catch (e) {
      console.log(`desktop: ${file} unavailable (${e.message})`);
    }
  }
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
      await runDemo(arg);
      break;
    case 'status':
      await runStatus();
      break;
    case 'discovery':
      await runDiscovery(arg === '--remove');
      break;
    default:
      console.log(
        'usage: bridge.js daemon|statusline|hook <Event>|demo [weather]|status|discovery [--remove]'
      );
      process.exit(1);
  }
})();
