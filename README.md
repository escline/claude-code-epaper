# esp-paper

A **Claude Code status and usage display**: a Waveshare 4.2" e-Paper Module
(rev2.1, 400x300 B/W) driven by an ESP32-S3-N16R8, showing live 5-hour and
weekly rate-limit usage, and flipping to a large **NEEDS YOU** banner the moment
Claude blocks on a permission prompt.

```
+----------------------------------------------------+
| CLAUDE CODE                                  14:32  |   inverted header
+----------------------------------------------------+
|                                                    |
|              #  NEEDS YOU  #                       |   status banner
|         Claude needs permission to use Bash        |
|                                                    |
+----------------------------------------------------+
|  SESSION (5h)                                 42%  |
|  [##############                            ]      |
|  resets in 2h 14m                                  |
|                                                    |
|  WEEK (7d)                                    68%  |
|  [######################                    ]      |
|  resets in 3d 4h                                   |
+----------------------------------------------------+
|  Opus 5 | esp-paper                                |
|  ctx 31%   $1.23                                   |
+----------------------------------------------------+
```

## How it works

```
Claude Code                    this PC                     LAN            panel
-----------                    -------                     ---            -----
statusLine  --stdin JSON-->  bridge.js statusline --,
                                                     +--> daemon --MQTT--> ESP32-S3
hooks       --stdin JSON-->  bridge.js hook <Event> -'    (retained)        e-paper
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
`~/.claude/settings.json`. **Merge, don't replace** — that file has your other
settings in it. Adjust the absolute path if you move the repo.

The snippet deliberately leaves out `PreToolUse`. Adding it shows which tool is
running, but spawns node before *every* tool call, adding roughly 100 ms each.
The optional config is at the bottom of the snippet file if you want it.

Restart Claude Code. The status line should appear at the bottom of your
terminal, and the panel should start tracking.

## Commands

```
pio run -t upload -t monitor        # flash + serial (monitor env is default)
pio run -e paneltest -t upload      # panel bring-up test
node bridge/bridge.js status        # config, broker, is the daemon up
node bridge/bridge.js demo          # push a fake state
node bridge/bridge.js daemon        # run in foreground to watch it
```

The daemon auto-spawns detached. To stop it:
`Get-Process node | Where-Object { $_.CommandLine -like '*bridge.js*' } | Stop-Process`.
Its log is `bridge/bridge.log`.

## Troubleshooting

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
received for 15 minutes. Run `node bridge/bridge.js status`.

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
- `include/ui.h`, `src/ui.cpp` — zone-based rendering and repaint throttling
- `src/main.cpp` — WiFi, MQTT, main loop
- `src/paneltest.cpp` — standalone bring-up test (`-e paneltest`)
- `bridge/bridge.js` — daemon + statusline/hook clients
- `docs/claude-settings-snippet.json` — what to merge into Claude Code settings

## References

- [GxEPD2](https://github.com/ZinggJM/GxEPD2)
- [Claude Code statusline reference](https://code.claude.com/docs/en/statusline)
- [Claude Code hooks reference](https://code.claude.com/docs/en/hooks)
- [Waveshare 4.2inch e-Paper Module wiki](https://www.waveshare.com/wiki/4.2inch_e-Paper_Module_Manual)
