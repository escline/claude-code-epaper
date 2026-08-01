#!/usr/bin/env node
'use strict';

/*
 * Expiring a session whose Claude Code instance is gone.
 *
 *   node bridge/test/session-liveness.test.js
 *
 * Same shape as session-grace.test.js: the real bridge.js, on its own port,
 * topics and log, asserted against what comes off the broker. The instance
 * directory is a temp dir this test writes itself, so nothing here depends on
 * a running Claude Code or on the real ~/.claude.
 *
 * The rule being tested is the conservative half of the mechanism. A force-kill
 * or a crash fires no SessionEnd, so a vanished instance file is the only
 * timely evidence the session is over - but an empty directory must never be
 * read as "nothing is running", or a version that stopped writing these files
 * would paint weather over a live terminal. Hence: only a session an instance
 * file was actually seen for can be expired this way, a file never creates a
 * session, and everything else waits out sessionTtlMs.
 */

const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');

const BRIDGE = path.join(__dirname, '..', 'bridge.js');
const GRACE_MS = 3000;
const POLL_MS = 1000;

let mqtt;
try {
  mqtt = require('mqtt');
} catch {
  console.error('mqtt package missing. Run: npm install --prefix bridge');
  process.exit(1);
}

let brokerCfg;
try {
  brokerCfg = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'config.json'), 'utf8')).mqtt;
  if (!brokerCfg || !brokerCfg.url) throw new Error('no mqtt.url');
} catch (e) {
  console.log(`SKIP  bridge/config.json has no usable broker (${e.message}).`);
  process.exit(0);
}

const tag = `epaper-live-${process.pid}`;
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'epaper-live-'));
const sessionsDir = path.join(tmp, 'sessions');
fs.mkdirSync(sessionsDir);

const cfg = {
  mqtt: brokerCfg,
  topics: { state: `claude/test/${tag}/state`, bridge: `claude/test/${tag}/bridge` },
  port: 20000 + ((process.pid + 1) % 9000),
  sessionGraceMs: GRACE_MS,
  sessionFiles: { enabled: true, path: sessionsDir, pollMs: POLL_MS },
  planUsage: { enabled: false },
  homeassistant: { enabled: false },
};
const configPath = path.join(tmp, 'config.json');
fs.writeFileSync(configPath, JSON.stringify(cfg, null, 2));

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const settle = () => sleep(GRACE_MS + POLL_MS + 1500);

// The shape Claude Code writes, minus the fields the bridge doesn't read.
function writeInstance(sessionId, pid, entrypoint = 'claude-desktop') {
  fs.writeFileSync(
    path.join(sessionsDir, `${pid}.json`),
    JSON.stringify({ pid, sessionId, cwd: '/x', version: '2.1.219', entrypoint })
  );
}
const removeInstance = (pid) => fs.rmSync(path.join(sessionsDir, `${pid}.json`), { force: true });

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

const seen = [];
let failures = 0;
const current = () => (seen.length ? seen[seen.length - 1] : {});
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
      seen.push(d);
      console.log(`        sessions=${d.sessions} status=${d.status} detail="${d.detail}"`);
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
  process.on('exit', () => daemon.kill());
  await sleep(3000);
  seen.length = 0;

  console.log(`\nbroker ${cfg.mqtt.url}, instance dir ${sessionsDir}\n`);

  // ----------------------------------------------------------------------
  console.log('1. an instance file alone must not invent a session');
  writeInstance('ghost', 999999);
  await settle();
  check('count stays at zero', (current().sessions ?? 0) === 0,
    seen.length ? `sessions=${current().sessions}` : 'nothing published at all');
  removeInstance(999999);

  // ----------------------------------------------------------------------
  console.log('\n2. a session with a live instance file survives');
  writeInstance('S1', process.pid); // this test's own pid: certainly alive
  await hook('SessionStart', 'S1', { source: 'startup' });
  await hook('UserPromptSubmit', 'S1', { prompt: 'do a thing' });
  await settle();
  check('session is live', current().sessions === 1, `sessions=${current().sessions}`);
  await settle();
  check('and stays live while its file is there', current().sessions === 1,
    `sessions=${current().sessions}`);

  // ----------------------------------------------------------------------
  console.log('\n3. the instance dies: file vanishes, no SessionEnd ever fires');
  removeInstance(process.pid);
  await settle();
  check('session is expired', current().sessions === 0, `sessions=${current().sessions}`);
  check('status is IDLE, not left WORKING', current().status === 'idle',
    `status=${current().status}`);
  check('detail says no session', current().detail === 'no active session',
    `detail="${current().detail}"`);

  // ----------------------------------------------------------------------
  console.log('\n4. a force-kill leaves the file behind but the pid is dead');
  const dead = spawn(process.execPath, ['-e', 'setTimeout(()=>{},60000)'], { stdio: 'ignore' });
  writeInstance('S2', dead.pid);
  await hook('SessionStart', 'S2', { source: 'startup' });
  await settle();
  check('session is live while the pid runs', current().sessions === 1,
    `sessions=${current().sessions}`);
  dead.kill('SIGKILL');
  await settle();
  check('dead pid expires it even with the file present', current().sessions === 0,
    `sessions=${current().sessions}`);
  removeInstance(dead.pid);

  // ----------------------------------------------------------------------
  // The guard that matters: a session this mechanism has never described must
  // not be touched by it, or a Claude Code that doesn't write instance files
  // would be expired the moment it went quiet.
  console.log('\n5. a session with no instance file is left to the TTL');
  await hook('SessionStart', 'S3', { source: 'startup' });
  await settle();
  check('session is live', current().sessions === 1, `sessions=${current().sessions}`);
  await settle();
  await settle();
  check('never expired by an empty directory', current().sessions === 1,
    `sessions=${current().sessions}`);

  daemon.kill();
  await new Promise((r) => client.publish(cfg.topics.state, '', { retain: true }, r));
  await new Promise((r) => client.publish(cfg.topics.bridge, '', { retain: true }, r));
  client.end(true);
  fs.rmSync(tmp, { recursive: true, force: true });
  console.log(`\n${failures ? `${failures} FAILURE(S)` : 'all checks passed'}`);
  process.exit(failures ? 1 : 0);
})();
