# claude-code-epaper

Claude Code status + usage display: Waveshare 4.2" e-Paper (rev2.1, 400x300 B/W)
on an ESP32-S3-N16R8, fed over MQTT by a Node bridge that merges Claude Code
statusline data with hook events. See `README.md` for architecture and wiring.

## Build and verify

```
pio run                             # monitor env (default)
pio run -e paneltest                # standalone panel test
pio run -t upload -t monitor        # flash + serial at 115200
node bridge/bridge.js status        # broker + daemon health
node bridge/bridge.js demo          # push a fake state
```

`pio` is at `~/.platformio/penv/Scripts/pio.exe`. A clean build takes about a
minute. There is no hardware-in-the-loop test, so **always build both envs
before reporting a firmware change as done** — compiling is the only automated
check. For bridge changes, `demo` plus a subscriber is the equivalent.

## Things that bite

### Firmware

- **`board_build.arduino.memory_type = qio_opi` is load-bearing.** The N16R8 has
  octal PSRAM; any other value boot-loops before `setup()` runs, with no serial
  output to explain why.
- **Avoid GPIO 26-37** for peripherals — flash and octal PSRAM use them. Also
  avoid 0, 3, 19, 20, 45, 46 (strapping and USB).
- **Panel controller is ambiguous.** `rev2.1` is the driver board revision, not
  the panel. UC8176 and SSD1683 both ship in this form factor and need different
  GxEPD2 classes. Selected in `include/config.h`. If a symptom looks like a
  driver problem, flip it before debugging further.
- **PubSubClient's default buffer is 256 bytes** and silently truncates the state
  snapshot. `setBufferSize(2048)` in `main.cpp` — don't remove it.
- **`delay(2000)` at the top of `setup()`** is deliberate: the S3's USB CDC port
  needs time to enumerate or the first prints are lost.
- Panels are rated for roughly one refresh per 180 s. `src/ui.cpp` enforces this
  with per-zone signatures and minimum intervals. **Any new screen content must
  be added to both a `paint*` function and its matching `sig*` function** — a
  field that isn't hashed will never trigger a repaint, and a field hashed at too
  fine a resolution (e.g. seconds) will repaint constantly and wear the panel.

### Bridge

- **`statusline` mode must never throw and never block.** It runs on nearly every
  message and its stdout *is* the user's status line. Every path is wrapped;
  daemon sends are fire-and-forget with a 400 ms timeout.
- `rate_limits` only exists for Claude.ai Pro/Max accounts and only after the
  first API response in a session. Absent is normal, not an error — the panel
  distinguishes unknown from zero, so don't default these to 0.
- The daemon auto-spawns detached from the first client call. A second daemon
  losing the port race exits quietly by design.
- State is published **retained**, with a last-will of `offline`. Both matter:
  retained makes ESP32 reboots recover instantly, the will stops a dead bridge
  from leaving a convincing but stale screen.
- **Bind the port before connecting to MQTT.** A daemon that loses the race for
  8787 must exit without ever opening an MQTT session — otherwise its hard exit
  fires the last will, and the retained bridge topic reads `offline` while the
  winner is running, blanking the panel and marking every HA entity
  unavailable. `startMqtt()` is called from the `server.listen` callback for
  exactly this reason; do not hoist it.
- **On startup, seed from the retained snapshot rather than publishing.** A
  fresh daemon's empty defaults would otherwise clobber known-good usage
  numbers — invisible when a statusline call respawns it, ugly when autostart
  runs it with Claude Code closed.
- Hook payload field names have drifted from the docs before: `UserPromptSubmit`
  carries `prompt`, not the documented `user_prompt`. `applyHook` logs each
  event's payload keys to `bridge.log` so the next drift is a lookup.
- HA discovery is retained and republished on every connect, which is what makes
  entities survive an HA restart. Adding an entity means adding it to
  `HA_ENTITIES`; removing one needs `discovery --remove` first, or its retained
  config lingers on the broker.

### Credentials

`include/secrets.h` and `bridge/config.json` hold WiFi and broker credentials and
are gitignored. The `.example` counterparts are committed. Never paste real
values into committed files, docs, or commit messages.

## Layout

- `include/config.h` — panel selection, pins, layout constants, refresh policy
- `src/ui.cpp` — zone rendering + repaint throttling (the tricky part)
- `src/main.cpp` — WiFi, MQTT, loop
- `src/paneltest.cpp` — bring-up test, excluded from the monitor build via
  `build_src_filter`
- `bridge/bridge.js` — single file, mode-dispatched on argv
