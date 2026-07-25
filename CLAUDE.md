# esp-paper

PlatformIO firmware driving a Waveshare 4.2" e-Paper Module (rev2.1, 400x300
B/W) from an ESP32-S3-N16R8. See `README.md` for wiring and troubleshooting.

## Build

```
pio run                              # default env (uc8176)
pio run -e ssd1683                   # other panel variant
pio run -t upload -t monitor         # flash and watch serial at 115200
```

The `pio` CLI is at `~/.platformio/penv/Scripts/pio.exe` on this machine.
A build from clean takes about a minute. Always build before reporting a
firmware change as done — there is no hardware-in-the-loop test here, so
compiling is the only automated check.

## Layout

- `src/main.cpp` — the whole application. Pin `#define`s and the panel-class
  selection are at the top.
- `platformio.ini` — one `[env]` base plus two per-panel envs that differ only
  in a `-DEPD_PANEL_*` flag.

## Things that bite

- **`board_build.arduino.memory_type = qio_opi` is load-bearing.** The N16R8
  has octal PSRAM; any other value boot-loops before `setup()` runs, with no
  serial output to explain why.
- **Avoid GPIO 26-37** for peripherals — flash and octal PSRAM use them. Also
  avoid 0, 3, 19, 20, 45, 46 (strapping and USB).
- **Panel controller is ambiguous.** `rev2.1` is the driver board revision, not
  the panel. UC8176 and SSD1683 both ship in this form factor and need
  different GxEPD2 classes. If a display symptom looks like a driver problem,
  try the other env before debugging further.
- **`delay(2000)` at the top of `setup()`** is deliberate: the S3's USB CDC
  port needs time to enumerate or the first prints are lost.
- Always `display.hibernate()` when finished drawing — leaving the controller
  powered causes ghosting and shortens panel life.
- Panels are rated for roughly one refresh per 180 s. Don't add fast refresh
  loops that would be left running.
