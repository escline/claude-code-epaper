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
npm test --prefix bridge            # bridge test suite, ~40 s
```

`pio` is at `~/.platformio/penv/Scripts/pio.exe`. A clean build takes about a
minute. There is no hardware-in-the-loop test, so **always build both envs
before reporting a firmware change as done** — compiling is the only automated
check the firmware has. For bridge changes, run the test below; `demo` plus a
subscriber covers whatever it doesn't.

`bridge/test/` holds the automated tests. Each spawns the real `bridge.js` on
its own port, topics and log (via `EPAPER_BRIDGE_CONFIG` / `EPAPER_BRIDGE_LOG`),
feeds it hooks the way Claude Code does, and asserts on what comes back off the
broker — so they need a reachable broker, skip without one, and leave the daemon
driving the panel alone. Most of the runtime is deliberate waiting on grace and
poll windows. Both have been mutation-checked: publishing the raw session count
fails four of `session-grace`'s ten, and dropping the seen-a-file-first guard
fails two of `session-liveness`'s ten.

**SCons decides staleness by content hash, not timestamp, so `touch` does not
force a recompile.** A build that "succeeds with no warnings" right after
touching files may have compiled nothing at all — this already hid a macro
collision through several green builds. To actually re-examine a file's
warnings, change its content or `pio run -t clean -e <env>` first. Incremental
builds finish in ~13 s and clean ones in ~60 s; if a "rebuild" was suspiciously
fast, it didn't happen.

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
- **`zones[]` spans two screens and only one is live at a time.** The header is
  on both; everything else belongs to the status screen or the weather screen,
  and `zoneActive()` gates every loop over the table — `drawFull`, the dirty
  check, and the partial-repaint scan. Miss one and the hidden screen's zones
  either paint over the visible one or drag it through a refresh a minute as
  the gauge countdown ticks behind it.
- **The ESP32 fetches weather itself; the bridge is not involved.** That screen
  shows when Claude Code isn't running, which usually means the PC hosting the
  bridge is off — relaying it over MQTT would make it stale exactly when it
  becomes visible. `WEATHER_LAT`/`WEATHER_LON` live in `secrets.h` and are a
  hard `#error` if missing, on purpose: a default renders a believable forecast
  for the wrong place.
- **`WEATHER_MODEL` is pinned to ECMWF and `apparent_temperature` is unused.**
  Both are deliberate and both were measured, not guessed. Open-Meteo's
  `best_match` picked the GFS family here and was 14 °F dry on dew point against
  the nearest NWS station, which collapses the heat index; and
  `apparent_temperature` is not the US heat index — it folds in wind and solar
  radiation and reads low in humid heat. `apparentF()` in `src/weather.cpp`
  implements the NWS heat index / wind chill pair instead, validated against the
  published charts. **When a weather number looks wrong, check humidity before
  the formula** — the `[wx]` serial line prints it. Ground truth for US
  locations is `api.weather.gov/points/<lat>,<lon>`, which reports `heatIndex`
  directly.
- **Numbers and condition codes come from different models, in two requests.**
  `WEATHER_MODEL` (ECMWF) is right for temperature and dew point and wrong for
  "is it raining *here*": 0.25° snaps the grid point up to ~20 km away, a
  625 km² grid-box mean smears any shower across the whole cell, and `current`
  interpolates between hourly steps — which is how the panel drew *light
  drizzle* under a cloudless sky while the station 10 km away reported Clear.
  `WEATHER_CONDITION_MODEL` (`best_match`, so HRRR/NBM in the US, ~2 km) supplies
  the current code and the strip's icons via a second 530-byte request; if it
  fails the ECMWF codes are kept, and the `[wx]` line's `[model + model]` tag
  says which was used. **Multi-model can't collapse this into one request** —
  `models=a,b` on a `current` block silently returns just one model's values,
  unsuffixed. Only `hourly` returns suffixed per-model keys.
- **Under the banner the gauge and footer zones paint nothing**, and both of the
  things that makes necessary are easy to miss. `sigGauge()` hashes `showBanner`
  first, or an all-unknown snapshot would hash identically to the banner state
  and the gauges would never be drawn in. And `uiTick()` forces a *full* refresh
  when the banner clears, the same way it does for a weather-screen swap: the
  gauge zone is on a 60 s floor, so a partial would leave the middle of the
  screen blank for up to a minute after the first snapshot had already landed.
  Empty bars labelled `--` were the previous behaviour and read as a reading of
  zero rather than as no data yet. A boot checklist was tried here and removed —
  on a healthy boot the snapshot lands about two seconds after the first paint,
  so every row was still unticked for the whole time it was visible.
- **Adafruit GFX fonts stop at ASCII 126**, so there is no `°` glyph. `drawTemp`
  in `src/ui.cpp` draws it as a ring, sized from the font's measured cap height
  rather than a per-font constant.
- **`textWidth()` and `textAdvance()` are not interchangeable.** `textWidth` wraps
  `getTextBounds`, which measures the *ink* box: it drops both side bearings and
  a trailing space contributes nothing at all (`"   H "` measures 11 px against a
  33 px advance). That is correct for centring and right-alignment. For placing
  anything *after* a string — `textRun`, `drawTemp` — use `textAdvance`, which
  sums `xAdvance` the way the cursor does. Confusing the two drew the degree ring
  on top of the last digit and ate the space after "feels"; two-digit values hid
  it, because those happen to have zero bearing slack.

### Bridge

- **`statusline` mode must never throw and never block.** It runs on nearly every
  message and its stdout *is* the user's status line. Every path is wrapped;
  daemon sends are fire-and-forget with a 400 ms timeout.
- `rate_limits` only exists for Claude.ai Pro/Max accounts and only after the
  first API response in a session. Absent is normal, not an error — the panel
  distinguishes unknown from zero, so don't default these to 0.
- **The gauges have a second source: the desktop app's
  `plan-usage-history.json`.** Quota is account-wide — desktop, web, mobile and
  Claude Code spend from one pool — but `rate_limits` only arrives when Claude
  Code gets an API response, so a day spent in the desktop app used to leave the
  gauges frozen at whatever the last terminal saw. The desktop app appends
  `{"t":…,"u":{"fh":5h%,"sd":7d%}}` every 5 minutes, and `pollPlanUsage()` reads
  the newest sample on mtime change. Three rules make it safe, and all three are
  tested in the daemon: **the more recently *observed* source wins** (a sample
  older than the last `rate_limits` is ignored, which is also how reset times
  survive — the file has no field for them); **a sample older than `freshMs` is
  ignored entirely**, because the file freezes when the desktop app closes and a
  stale 5-hour figure would hold the gauge high long after the window drained;
  and **a rise never creates a session**, only the `active in another Claude app`
  detail line, and only when no terminal is open. There is no per-message feed
  behind desktop activity — its log carries nothing finer than 5-minute polls —
  so a status set from it could never be cleared. The `xu` key in the same file
  is on a different poll and a different scale; it is ignored, not guessed at.
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
- **`PostToolUse` is what clears NEEDS YOU, and removing it brings the bug
  back.** `Notification` sets `needs_you`; with only the other five hooks
  registered, *nothing* fires between an answered permission prompt and `Stop`,
  so the banner stayed up for the whole remainder of the turn — 90 s in one
  logged trace, and the symptom was "it says NEEDS YOU while it's obviously
  working". The handler returns `false` when already `working`, so a normal turn
  publishes nothing extra and repaints nothing; the status zone can repaint every
  3 s, and a detail line naming each tool would churn it all turn for no
  actionable gain. It restores the last prompt text rather than leaving the
  answered question on screen. `Notification` now logs `notification_type` and
  `message` **values**, because Claude Code fires it both for permission prompts
  and for the idle "waiting for your input" nudge and only the first is really
  NEEDS YOU — check the log before assuming a repeat is a real prompt.
- **Quitting the desktop app fires no `SessionEnd`** — the process is killed and
  the hook never runs — so the session used to sit in the map until
  `sessionTtlMs`, eight hours later, with the panel holding a status screen for
  a window that had been closed all morning. `pollSessionFiles()` watches
  `~/.claude/sessions/<pid>.json`, one file per running instance, carrying the
  `sessionId` and `pid`. **It may only ever end a session, never start one, and
  only one it has already seen a file for** — an empty directory has to stay
  indistinguishable from a Claude Code that doesn't write them, or a version
  that renames the directory would paint weather over a live terminal. Anything
  it hasn't seen keeps the TTL. `bridge.js status` lists the files it can see.
- **`sessions` is debounced by `sessionGraceMs`; nothing else in the snapshot
  is.** The desktop app opens a session when it mounts a project view and ends
  it under a second later — three in two minutes in one log, no transcript
  written by any of them — and each one published raw cost two full refreshes
  and flashed IDLE up over the weather screen. `publish()` sends
  `publishedSessions`, which follows the live count only once the new value has
  held for the window. **Mutate the map through `touchSession`/`dropSession`**,
  or a count change whose event doesn't publish will never arm the settle timer.
  The delay is symmetric on purpose: holding only the drop to zero turns a 0.4 s
  flash into a 10 s one, because it is the `SessionStart` that swaps the screen.
  Nothing else needs it — under the weather screen the status and detail zones
  are inactive, so that churn paints nothing anyway. `demo` sets
  `publishedSessions` directly so it stays instant.
- **`sessions` is set inside `publish()` from `liveSessions`, never from the
  seeded snapshot.** The panel uses it to tell "idle, waiting on you" from
  "nothing open" and hand the screen to the weather; a restarted daemon has no
  sessions regardless of what the broker still remembers. `finishSeeding()`
  publishes once for exactly this reason — otherwise a daemon started at login
  with Claude Code closed leaves yesterday's count retained and the panel sits
  on a stale IDLE forever. `demo` may pin the value; only `demo` can unpin it.
- HA discovery is retained and republished on every connect, which is what makes
  entities survive an HA restart. Adding an entity means adding it to
  `HA_ENTITIES`; removing one needs `discovery --remove` first, or its retained
  config lingers on the broker.

### Credentials

`include/secrets.h` and `bridge/config.json` hold WiFi and broker credentials and
are gitignored. The `.example` counterparts are committed. Never paste real
values into committed files, docs, or commit messages. `WEATHER_LAT` /
`WEATHER_LON` are in `secrets.h` for the same reason — not a credential, but a
home address by another name, and it does not belong in git either.

## Layout

- `include/config.h` — panel selection, pins, layout constants, refresh policy
- `src/ui.cpp` — zone rendering + repaint throttling (the tricky part)
- `src/weather.cpp` — Open-Meteo fetch and WMO code mapping
- `src/weather_icons.cpp` — glyphs drawn from primitives, scaled by a parameter
  rather than stored at each size
- `src/main.cpp` — WiFi, MQTT, loop
- `src/paneltest.cpp` — bring-up test, excluded from the monitor build via
  `build_src_filter`
- `bridge/bridge.js` — single file, mode-dispatched on argv
