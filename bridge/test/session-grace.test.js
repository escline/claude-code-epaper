#!/usr/bin/env node
'use strict';

/*
 * Session-count debounce: what the panel is actually told.
 *
 *   node bridge/test/session-grace.test.js     (or: npm test --prefix bridge)
 *
 * There is no hardware in the loop and no mock broker, so this drives the real
 * bridge.js the way Claude Code does - a daemon process fed one JSON line per
 * hook over localhost TCP - and asserts on the snapshots that come back off the
 * broker. It runs on its own port, its own topics and its own log, so the
 * daemon driving the panel is untouched; the only thing it borrows from
 * bridge/config.json is the broker address, and it skips rather than fails if
 * there isn't one.
 *
 * What is under test is the rule that the published session count follows the
 * live one only after the new value has held for sessionGraceMs. That count
 * picks the screen, and each screen swap is a full-panel refresh on a display
 * rated for one per 180 s, so a session that opens and closes inside the window
 * must never reach the panel at all. The desktop app produces exactly that:
 * a session per project view mounted, ended under a second later.
 *
 * The window is shortened to 3 s here. Total runtime is about 40 s, most of it
 * deliberate waiting.
 */

const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const BRIDGE = path.join(__dirname, '..', 'bridge.js');
const GRACE_MS = 3000;

let mqtt;
try {
  mqtt = require('mqtt');
} catch {
  console.error('mqtt package missing. Run: npm install --prefix bridge');
  process.exit(1);
}

// The broker address is the one piece of real config needed. config.json is
// gitignored and machine-specific, so a checkout without one skips.
let brokerCfg;
try {
  brokerCfg = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'config.json'), 'utf8')).mqtt;
  if (!brokerCfg || !brokerCfg.url) throw new Error('no mqtt.url');
} catch (e) {
  console.log(`SKIP  bridge/config.json has no usable broker (${e.message}).`);
  console.log('      Copy config.example.json and point mqtt.url at your broker.');
  process.exit(0);
}

// Unique per run: several of these can run at once, and a crashed run must not
// leave anything retained where the next one will read it.
const tag = `epaper-test-${process.pid}`;
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'epaper-test-'));
const cfg = {
  mqtt: brokerCfg,
  topics: { state: `claude/test/${tag}/state`, bridge: `claude/test/${tag}/bridge` },
  port: 20000 + (process.pid % 9000),
  sessionGraceMs: GRACE_MS,
  // Both off: this is about the session count, and neither the desktop usage
  // file nor Home Assistant discovery has any bearing on it.
  planUsage: { enabled: false },
  homeassistant: { enabled: false },
};
const configPath = path.join(tmp, 'config.json');
fs.writeFileSync(configPath, JSON.stringify(cfg, null, 2));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --------------------------------------------------------------------------
// Talking to the daemon exactly as the thin clients do.
// --------------------------------------------------------------------------
function send(payload) {
  return new Promise((resolve) => {
    const s = net.createConnection({ host: '127.0.0.1', port: cfg.port }, () => {
      s.end(JSON.stringify(payload) + '\n', resolve);
    });
    s.on('error', () => resolve());
  });
}

const hook = (event, session_id, extra = {}) =>
  send({ kind: 'hook', event, data: { session_id, cwd: '/x/esp-paper', ...extra } });
const statusline = (session_id) =>
  send({ kind: 'statusline', data: { session_id, model: { display_name: 'Opus 5' } } });

// --------------------------------------------------------------------------
const seen = []; // every snapshot published, with a relative timestamp
let t0 = Date.now();
const at = () => Date.now() - t0;
const since = (ms) => seen.filter((s) => s.t >= ms).map((s) => s.sessions);
const current = () => (seen.length ? seen[seen.length - 1].sessions : undefined);

let failures = 0;
function check(name, ok, detail = '') {
  console.log(`  ${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  (${detail})` : ''}`);
  if (!ok) failures++;
}

(async () => {
  const client = mqtt.connect(cfg.mqtt.url, {
    username: cfg.mqtt.username || undefined,
    password: cfg.mqtt.password || undefined,
    clientId: tag,
    connectTimeout: 5000,
    reconnectPeriod: 0,
  });

  const connected = await new Promise((resolve) => {
    client.once('connect', () => resolve(true));
    client.once('error', () => resolve(false));
    setTimeout(() => resolve(false), 6000);
  });
  if (!connected) {
    console.log(`SKIP  no broker at ${cfg.mqtt.url}.`);
    client.end(true);
    fs.rmSync(tmp, { recursive: true, force: true });
    process.exit(0);
  }

  client.subscribe(cfg.topics.state);
  client.on('message', (_t, p) => {
    const raw = p.toString();
    if (!raw) return;
    try {
      const d = JSON.parse(raw);
      seen.push({ t: at(), sessions: d.sessions, status: d.status, detail: d.detail });
      console.log(`        [${String(at()).padStart(5)}ms] sessions=${d.sessions} ` +
        `status=${d.status} detail="${d.detail}"`);
    } catch {}
  });

  const daemon = spawn(process.execPath, [BRIDGE, 'daemon'], {
    stdio: 'ignore',
    env: {
      ...process.env,
      EPAPER_BRIDGE_CONFIG: configPath,
      EPAPER_BRIDGE_LOG: path.join(tmp, 'bridge.log'),
    },
  });

  const cleanup = async () => {
    daemon.kill();
    // Retained, so leaving these behind would strand test payloads on the
    // broker forever.
    await new Promise((r) => client.publish(cfg.topics.state, '', { retain: true }, r));
    await new Promise((r) => client.publish(cfg.topics.bridge, '', { retain: true }, r));
    client.end(true);
    fs.rmSync(tmp, { recursive: true, force: true });
  };
  process.on('exit', () => daemon.kill());

  // Nothing retained on a fresh topic, so the daemon waits out its seed timer.
  await sleep(3000);
  t0 = Date.now();
  seen.length = 0;

  console.log(`\nbroker ${cfg.mqtt.url}, grace ${GRACE_MS}ms, port ${cfg.port}\n`);

  // ----------------------------------------------------------------------
  console.log('1. desktop probe: SessionStart, then SessionEnd 400ms later');
  await hook('SessionStart', 'A', { source: 'startup' });
  await sleep(400);
  await hook('SessionEnd', 'A', { reason: 'other' });
  await sleep(GRACE_MS + 2000);
  check('a probe never reaches the panel', !since(0).some((n) => n > 0),
    `published [${since(0).join(',')}]`);

  // ----------------------------------------------------------------------
  console.log('\n2. a real session opens');
  await hook('SessionStart', 'B', { source: 'startup' });
  await sleep(GRACE_MS - 1000);
  check('count is held during the window', current() !== 1, `sessions=${current()}`);
  await sleep(1500);
  check('count settles to 1 after it', current() === 1, `sessions=${current()}`);

  // ----------------------------------------------------------------------
  console.log('\n3. a second session, seen only through statusline');
  await statusline('C');
  await sleep(GRACE_MS + 1000);
  check('statusline promotes an unseen session', current() === 2, `sessions=${current()}`);

  // ----------------------------------------------------------------------
  console.log('\n4. both close');
  await hook('SessionEnd', 'B', { reason: 'exit' });
  await hook('SessionEnd', 'C', { reason: 'exit' });
  await sleep(GRACE_MS - 1500);
  check('screen is not handed over immediately', current() === 2, `sessions=${current()}`);
  await sleep(2000);
  check('settles to 0 once it holds', current() === 0, `sessions=${current()}`);

  // ----------------------------------------------------------------------
  console.log('\n5. close and reopen inside the window');
  await hook('SessionStart', 'D', { source: 'startup' });
  await sleep(GRACE_MS + 1000);
  check('D is live', current() === 1, `sessions=${current()}`);
  const mark = at();
  await hook('SessionEnd', 'D', { reason: 'clear' });
  await sleep(500);
  await hook('SessionStart', 'D', { source: 'resume' });
  await sleep(GRACE_MS + 2000);
  check('a bounce never drops the count', !since(mark).some((n) => n === 0),
    `published [${since(mark).join(',')}]`);

  // ----------------------------------------------------------------------
  console.log('\n6. demo still applies at once');
  await send({ kind: 'demo', data: { status: 'idle', detail: 'no active session', sessions: 0 } });
  await sleep(700);
  check('demo weather pins with no delay', current() === 0, `sessions=${current()}`);
  await send({ kind: 'demo', data: { status: 'needs_you', detail: 'x', sessions: null } });
  await sleep(700);
  check('demo releases back to the live count', current() === 1, `sessions=${current()}`);

  await cleanup();
  console.log(`\n${failures ? `${failures} FAILURE(S)` : 'all checks passed'}`);
  process.exit(failures ? 1 : 0);
})();
