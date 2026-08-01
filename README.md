# claude-code-epaper

A **Claude Code status and usage display**: a Waveshare 4.2" e-Paper Module
(rev2.1, 400x300 B/W) driven by an ESP32-S3-N16R8, showing live 5-hour and
weekly rate-limit usage, and flipping to a large **NEEDS YOU** banner the moment
Claude blocks on a permission prompt.

![The e-paper panel showing an idle Claude Code session: a CLAUDE CODE header
with a clock, a large IDLE banner reading "waiting for you", progress bars for
5-hour and weekly rate-limit usage with reset countdowns, and a footer showing
the model, project, context usage and session cost](docs/panel-idle.jpg)

When Claude blocks on a permission prompt, the banner inverts to a
full-width **NEEDS YOU** with the prompt text beneath it — readable across a
room and hard to miss in peripheral vision.

## How it works

```
Claude Code                    this PC                     LAN            panel
-----------                    -------                     ---            -----
statusLine  --stdin JSON-->  bridge.js statusline --,
                                                     +--> daemon --MQTT--> ESP32-S3
hooks       --stdin JSON-->  bridge.js hook <Event> -'    (retained)        e-paper
                                                                              ^
Open-Meteo  ------------------------ HTTPS, every 20 min ----------------------'
```

Two independent data sources feed one snapshot:

- **`statusLine`** supplies the numbers. Claude Code pipes JSON to it on nearly
  every message, including `rate_limits.five_hour.used_percentage`,
  `rate_limits.seven_day.used_percentage`, their `resets_at` epochs,
  `cost.total_cost_usd`, and `context_window.used_percentage`.
  `rate_limits` is populated only for Claude.ai Pro/Max accounts, and only after
  the first API response in a session.
- **Hooks** supply the state. `Notification` means Claude is blocked on you,
  `UserPromptSubmit` means work started, `Stop` means it finished,
  `SessionStart`/`SessionEnd` track presence.
- **The Claude desktop app** supplies usage as a fallback. It records the
  account's 5-hour and 7-day percentages to `plan-usage-history.json` every five
  minutes, and the quota is shared across Claude Code, desktop, web and mobile —
  so the daemon reads that file to keep the gauges true on days you never open a
  terminal. Claude Code's own numbers outrank it whenever they are more recent,
  and the file is ignored once its last sample goes stale, since it stops being
  written the moment the desktop app closes. A rise with no terminal open shows
  as `active in another Claude app`; it deliberately does not count as a session,
  because the desktop app exposes no per-message events that could ever clear it.
  Configure or disable under `planUsage` in `config.json`.

The bridge merges both into one retained MQTT message. Retained matters: an
ESP32 that reboots gets the correct screen back within a second of reconnecting,
with no request needed. The bridge also sets a last-will of `offline`, so a
crashed bridge shows as OFFLINE rather than a convincingly stale screen.

### Why a daemon rather than publishing directly

`statusLine` runs on nearly every message. Doing an MQTT connect/publish/
disconnect each time would add latency to your status line and thrash the
broker. Instead the thin clients hand one line of JSON to a long-lived local
process over `127.0.0.1:8787` and exit. The daemon auto-spawns on first use.

### Refresh policy

These panels are rated for roughly one refresh per 180 s, but the bridge pushes
on nearly every message. Repainting naively would visibly wear the panel, so the
screen is split into four zones, each with its own content signature and minimum
repaint interval:

| Zone | Interval | Rationale |
| --- | --- | --- |
| Status banner | 3 s | the actionable signal; needs to be fast |
| Usage gauges | 60 s | percentages move slowly |
| Header / footer | 60 s | clock only needs minute resolution |
| Full refresh | 30 min, or every 20 partials | clears accumulated ghosting |

A zone repaints only when what it *would* draw differs from what is on the
glass. An unchanged 42% pushed a hundred times costs zero refreshes.

### The weather screen

A wall panel that only says something while Claude Code is open is blank most of
the week, so when there is nothing live to report it shows current conditions
and a five-day forecast instead.

"Nothing live to report" is two cases, and both count:

- **OFFLINE** — the bridge's last will fired, nothing arrived for 15 minutes, or
  the state topic has nothing retained on it at all.
- **IDLE with `sessions: 0`** — the bridge is up and reports no Claude Code
  session open.

The weather footer still carries the usage numbers, as `5h 42%` / `7d 67%` with
a proportional bar under each — the same two values the status screen gauges
show. Since the bridge learned to read the desktop app's plan usage, this screen
is on the panel exactly when quota can still be moving with no terminal open, so
the bars are what make that legible from across the room rather than only up
close.

An IDLE session that is merely waiting on you is *not* one of them. That is a
state worth showing, and burying it under a forecast would defeat the display.
The distinction needs a session count, so the bridge publishes `sessions`
alongside the rest of the snapshot; it is taken from the daemon's own live map
on every publish, never from the retained snapshot it seeded from, because a
restarted daemon has no sessions no matter what the broker still remembers.

That count is also the one field that is *debounced* on its way out, by
`sessionGraceMs` (10 s). Claude Code in the desktop app opens a session whenever
it mounts a project view and discards it again — three of them inside two
minutes in one log, each `SessionStart` answered by a `SessionEnd` under a
second later, none of which ever wrote a transcript. Published raw, each of
those cost the panel two full refreshes and read as IDLE flashing up and falling
straight back to the forecast. So a new count has to hold for the grace window
before it reaches the snapshot, in both directions: holding only the drop to
zero would have turned a 0.4 s flash into a 10 s one, since it is the
`SessionStart` that swaps the screen. Nothing else waits — while the weather
screen is up, the zones that show status and detail are inactive anyway.

Switching screens forces a full refresh rather than a run of partials — every
pixel below the header changes, and a full pass is also the cheapest way to
clear the ghost of the layout being replaced.

**The ESP32 fetches the forecast itself**, over HTTPS from
[Open-Meteo](https://open-meteo.com) (no API key, no account), rather than
having the bridge relay it over MQTT. This is the whole point: the screen exists
for the hours Claude Code isn't running, which usually means the PC hosting the
bridge is off. A weather panel that goes stale exactly when it becomes visible
would be worse than none. Conditions older than three hours are labelled stale
rather than presented as current.

Icons are drawn from primitives rather than stored as bitmaps. The same eight
symbols are needed at 64 px and at 32 px; as bitmaps that is sixteen blobs and a
generator to keep them in step, and on a 1-bit panel the output is identical.

#### Two things that are not the API's defaults

**The model is pinned to ECMWF, not `best_match`.** Checked against the nearest
NWS station on a hot afternoon, `best_match` resolved to the GFS family and ran
3 °F warm on air temperature and **14 °F dry on dew point** — 28% relative
humidity against an observed 50%. Humidity that wrong collapses the heat index,
so the panel read "feels 100" on a day that genuinely felt like 110. ECMWF
matched the observed dew point to within 0.2 °F. It's `WEATHER_MODEL` in
`include/config.h`, and it's the first knob to turn if the panel disagrees with
a station near you — one sample in one place is a sensible default, not a law.

**"Feels like" is computed on-device, not taken from the API.** Open-Meteo's
`apparent_temperature` is a different quantity: it folds in wind cooling and
solar radiation, and it reads several degrees *below* the heat index in humid
heat — precisely the condition the number exists to warn you about. `apparentF`
in `src/weather.cpp` implements the NWS pair instead: the Rothfusz heat index
above 80 °F, wind chill at or below 50 °F with wind over 3 mph, and plain air
temperature in between, which is what US weather apps show. Validated against
the published NWS charts: worst deviation 2.1 °F on heat index (Rothfusz's own
fit error is ±1.3 °F), 0.5 °F on wind chill.

## Setup

### 1. Wire the panel

| Module | Cable | ESP32-S3 | Notes |
| ------ | ------ | -------- | ------------------------- |
| VCC | grey | 3V3 | see voltage note below |
| GND | brown | GND | |
| DIN | blue | GPIO 11 | SPI MOSI |
| CLK | yellow | GPIO 12 | SPI SCK |
| CS | orange | GPIO 10 | chip select, active low |
| DC | green | GPIO 9 | data / command |
| RST | white | GPIO 8 | reset, active low |
| BUSY | purple | GPIO 7 | busy, driven by the panel |

Cable colours are the usual Waveshare ones; trust the silkscreen if they
disagree. Pins avoid GPIO 26-37 (the N16R8's 16 MB flash and 8 MB octal PSRAM)
and the strapping/USB pins 0, 3, 19, 20, 45, 46. Change them in
`include/config.h` if you need to.

**Voltage:** rev2.1 added level shifting, so the board tolerates 3.3 V or 5 V
logic. The ESP32-S3 is a 3.3 V part, so run VCC at 3.3 V and it is all native.
Never feed 5 V logic into S3 pins.

Note the board must stay mains-powered on WiFi — deep sleep would break the push
model that makes the NEEDS YOU alert immediate.

### 2. Test the panel first

Before involving WiFi or MQTT, confirm the wiring and work out which panel
controller you have:

```
pio run -e paneltest -t upload -t monitor
```

You should get a test pattern, then a counter doing five partial refreshes.
If the screen stays blank, is heavily ghosted, or looks torn, open
`include/config.h` and switch `EPD_PANEL_UC8176` to `EPD_PANEL_SSD1683`.
`rev2.1` is the *driver board* revision and doesn't tell you which panel you
have, so trying both is the quickest answer.

What the pattern checks: nested borders and corner blocks prove the controller
addresses all four edges; the checkerboard proves pixel-level addressing
(smearing means a byte-order or line-pitch mismatch); the widening vertical
lines expose dropped columns.

### 3. Configure secrets

```
cp include/secrets.h.example include/secrets.h     # gitignored
cp bridge/config.example.json bridge/config.json   # gitignored
```

Fill in WiFi and your broker in both. They are separate files because the ESP32
and the bridge connect to the broker independently.

`include/secrets.h` also holds `WEATHER_LAT` / `WEATHER_LON` for the weather
screen, in decimal degrees (south and west negative). They live there rather
than in the committed `config.h` purely so your home coordinates stay out of the
repository. Two decimal places is finer than any forecast resolves.

The build fails with a clear error if they are missing, deliberately: a default
would render a plausible forecast for somewhere else entirely, which is a much
worse failure than not compiling. Set `WEATHER_ENABLED 0` in `include/config.h`
if you don't want the screen at all, and `WEATHER_IMPERIAL 0` for °C and km/h.

### 4. Flash the monitor firmware

```
pio run -t upload -t monitor
```

### 5. Install the bridge

```
npm install --prefix bridge
node bridge/bridge.js status     # should print your broker and 'daemon: not running'
node bridge/bridge.js demo       # publishes a fake state; panel should light up
```

`demo` auto-spawns the daemon and pushes a fake NEEDS YOU state with plausible
numbers. This is the fastest way to confirm the whole chain works before
touching your Claude Code config.

### 6. Wire up Claude Code

Merge the two keys from `docs/claude-settings-snippet.json` into
`~/.claude/settings.json`, replacing `CLAUDE_EPAPER_PATH` with the absolute path
to your clone. **Merge, don't replace** — that file has your other settings in
it, and invalid JSON silently disables all of them.

`PostToolUse` is in the snippet and is not optional. Nothing else fires between
an answered permission prompt and the end of the turn, so without it the
**NEEDS YOU** banner stays up for the entire rest of the work — 90 s in one
logged trace. It is `async`, and the bridge ignores it while already working, so
it publishes nothing and repaints nothing except when there is a stale banner to
clear.

The snippet still leaves out `PreToolUse`. Adding it shows which tool is running,
but spawns node before *every* tool call, adding roughly 100 ms each, and
rewrites the detail line often enough to churn the status zone. The optional
config is at the bottom of the snippet file if you want it.

Restart Claude Code. The status line should appear at the bottom of your
terminal, and the panel should start tracking.

### 7. Keep the daemon running (optional)

You do not strictly need this. The daemon auto-spawns whenever a hook or
statusline call finds it missing, and `refreshInterval: 30` guarantees a call
every 30 seconds while Claude Code is open — measured recovery after being
killed is about 16 seconds, unattended.

Install autostart only if you want the panel and Home Assistant showing
last-known usage while Claude Code is **closed**:

```
powershell -ExecutionPolicy Bypass -File bridge\install-autostart.ps1
```

It prefers a scheduled task (which also restarts a crashed daemon), but that
needs elevation on most machines; without it, it falls back to a Startup-folder
launcher that needs no admin. Both start the daemon with no console window.
Re-run it if you move the repo, since the launcher hardcodes absolute paths.

To remove: `... -File bridge\install-autostart.ps1 -Uninstall`

Note the numbers only *change* while Claude Code is running — statusline is the
only source. With autostart, a closed editor shows the last known values rather
than OFFLINE.

## Home Assistant

The bridge publishes MQTT discovery configs, so HA creates the entities itself.
Set `homeassistant.enabled` in `bridge/config.json` (on by default) and make
`discoveryPrefix` match your MQTT integration's setting. The daemon republishes
on every broker connect, so entities reappear after an HA restart.

A **Claude Code** device shows up under Settings → Devices & Services → MQTT
with 12 entities:

| Entity | Notes |
| --- | --- |
| `sensor.session_usage` | 5-hour limit used, % |
| `sensor.weekly_usage` | 7-day limit used, % |
| `sensor.session_resets`, `sensor.weekly_resets` | `device_class: timestamp` |
| `sensor.context_usage` | context window used, % |
| `sensor.session_cost` | USD |
| `sensor.status`, `sensor.activity` | `idle`/`working`/`needs_you`, and detail text |
| `sensor.model`, `sensor.project` | diagnostic |
| `binary_sensor.needs_attention` | ON when Claude is blocked on you |
| `binary_sensor.working` | ON while processing |

All share the bridge's availability topic, so they go unavailable together when
the last will fires.

`binary_sensor.needs_attention` is the useful automation trigger — flash a lamp
or push a phone notification when Claude blocks on a permission prompt:

```yaml
automation:
  - alias: Claude needs me
    trigger:
      - platform: state
        entity_id: binary_sensor.needs_attention
        to: "on"
    action:
      - service: light.turn_on
        target: { entity_id: light.desk }
        data: { flash: short }
```

Republish or remove the entities without restarting the daemon:

```
node bridge/bridge.js discovery            # republish
node bridge/bridge.js discovery --remove   # delete from HA
```

`session_cost` is deliberately not `device_class: monetary` — cost resets to 0
on `/clear`, which would make HA's long-term statistics read each new session as
a negative adjustment.

## Commands

```
pio run -t upload -t monitor        # flash + serial (monitor env is default)
pio run -e paneltest -t upload      # panel bring-up test
node bridge/bridge.js status        # config, broker, is the daemon up
node bridge/bridge.js demo          # push a fake state
node bridge/bridge.js demo weather  # force the weather screen
node bridge/bridge.js daemon        # run in foreground to watch it
node bridge/bridge.js discovery     # republish Home Assistant entities
npm test --prefix bridge            # bridge test suite (needs the broker)
```

Careful with `demo`: it writes fake values to the retained topic, so the panel
shows them until the next real update.

`demo weather` pins the published session count to zero, which is what drives
the panel to the weather screen without you having to close every terminal
first. The forecast itself is not sent from here — the ESP32 fetches it — so
this only changes whether the panel has something else to show. Plain `demo`
releases the pin and hands the count back to the live session map.

`npm test` spawns a second daemon on its own port, topics and log, so it can be
run while the real one is driving the panel. It needs a reachable broker —
without one it skips rather than fails. Most of its 40 s is spent waiting out
grace windows.

The daemon auto-spawns detached. To stop it:
`Get-Process node | Where-Object { $_.CommandLine -like '*bridge.js*' } | Stop-Process`.
Its log is `bridge/bridge.log`.

## Troubleshooting

**Read the banner first.** Until the first snapshot arrives the panel shows only
a banner — `Starting`, `Connecting` with your SSID, `No WiFi`, `No broker` with
the host, or `Waiting for Claude Code`. Whichever one it is stuck on names the
step to chase below. The gauges and footer stay blank until there is something
real to put in them.

**Nothing happens, no serial output.** The N16R8 needs
`board_build.arduino.memory_type = qio_opi`; with any other value the board
boot-loops before `setup()` runs. Already set in `platformio.ini`.

**Serial output but the screen never changes.** Check BUSY first — if it is not
connected, GxEPD2 waits on a pin that never asserts and the refresh appears to
hang. Then re-check DC and CS.

**Panel shows "No broker".** The ESP32 can't reach MQTT. Verify the broker is on
the *same subnet* as both devices — check with `ipconfig` that your PC and the
broker IP agree in the first three octets.

**Panel shows OFFLINE.** The bridge's last will fired, or nothing has been
received for 15 minutes. Run `node bridge/bridge.js status`. If the weather
screen is enabled and a fetch has succeeded, you'll see the forecast instead,
with the reason on the footer line.

**Weather screen never appears.** It needs both a successful fetch and a state
that says nothing is running. Check the serial log for `[wx]` lines — a failed
GET or an unparseable response is logged there. If fetches are fine, the state
is the culprit: `node bridge/bridge.js demo weather` forces it, and if that
works but normal use doesn't, a session is being left open in the daemon's map;
`bridge.log` now records `session start source=` and `session end reason=` for
every one, so the transcript-less desktop probes are distinguishable from a real
terminal at a glance. A stuck session is what `sessionTtlMs` exists to clean up,
deliberately after 8 hours —
`statusLine` only fires when there is traffic, so a session left open overnight
is silent but genuinely still open, and pruning it on the idle timescale would
paint weather over a live terminal.

**Weather says "stale".** No successful fetch in three hours. Usually WiFi;
check the `[wx]` serial lines. The last known values stay on screen rather than
being blanked, labelled so you know not to trust them.

**Temperature or "feels like" disagrees with your phone.** The `[wx]` serial
line prints temperature, feels-like, humidity and the model in use. Check the
humidity first — it drives the whole heat index, and a model that is dry by
15 points will read 10 °F low on "feels like" while looking almost right on air
temperature. Compare against a real observation rather than another app: for US
locations, `api.weather.gov/points/<lat>,<lon>` leads to the nearest station and
reports `heatIndex` directly. If the model is the problem, change
`WEATHER_MODEL` in `include/config.h` — `gem_seamless` and `icon_seamless` are
the next ones worth trying. Expect a couple of degrees of disagreement
regardless: this is model output, not a thermometer, and phone apps differ from
official observations too.

**Gauges show `--`.** `rate_limits` hasn't arrived. It only appears for
Claude.ai Pro/Max accounts, and only after the first API response in a session,
so it is normal for the first few seconds. If it never appears, run
`node bridge/bridge.js daemon` in the foreground and check the log.

**Ghosting after many partial updates.** Normal for e-paper. A full refresh runs
every 30 minutes or every 20 partials; lower `PARTIALS_BEFORE_FULL` in
`include/config.h` if you want it more aggressive.

**Status line missing in the terminal.** The `statusLine` key didn't merge
correctly. Check `~/.claude/settings.json` is still valid JSON.

## Not included: credits

Credit / spend balance has no field in the statusline payload, no hook, and
nothing on disk. `~/.claude/stats-cache.json` exists but is not maintained live.
There is currently no supported local source, so the display omits it.

## Layout of this repo

- `include/config.h` — panel selection, pins, layout, refresh policy
- `include/claude_state.h`, `src/claude_state.cpp` — the state snapshot + parser
- `include/weather.h`, `src/weather.cpp` — Open-Meteo fetch, WMO code mapping
- `include/weather_icons.h`, `src/weather_icons.cpp` — the glyphs, drawn not stored
- `include/ui.h`, `src/ui.cpp` — zone-based rendering and repaint throttling
- `src/main.cpp` — WiFi, MQTT, main loop
- `src/paneltest.cpp` — standalone bring-up test (`-e paneltest`)
- `bridge/bridge.js` — daemon + statusline/hook clients
- `bridge/test/` — end-to-end tests against a real daemon and broker
- `docs/claude-settings-snippet.json` — what to merge into Claude Code settings

## References

- [GxEPD2](https://github.com/ZinggJM/GxEPD2)
- [Claude Code statusline reference](https://code.claude.com/docs/en/statusline)
- [Claude Code hooks reference](https://code.claude.com/docs/en/hooks)
- [Waveshare 4.2inch e-Paper Module wiki](https://www.waveshare.com/wiki/4.2inch_e-Paper_Module_Manual)

## License

MIT — see [LICENSE](LICENSE).

## Disclaimer

An independent hobby project, not affiliated with or endorsed by Anthropic or
Waveshare. "Claude" and "Claude Code" are trademarks of Anthropic.

It reads Claude Code's documented `statusLine` and hook interfaces. Those are
public and stable enough to build on, but they are not a versioned API — field
names have shifted before (`UserPromptSubmit` delivers `prompt`, not the
documented `user_prompt`), so expect occasional breakage after upgrades. The
bridge logs each hook's payload keys to `bridge/bridge.log` to make that quick
to diagnose.

`rate_limits` is only populated for Claude.ai Pro/Max accounts. On API/Console
billing the gauges will stay at `--`; everything else still works.
