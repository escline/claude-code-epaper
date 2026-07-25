# esp-paper

Driving a **Waveshare 4.2" e-Paper Module (rev2.1, 400x300 B/W)** from an
**ESP32-S3-N16R8**, built with PlatformIO.

The firmware in `src/main.cpp` is a bring-up test: on boot it does one full
refresh of a test pattern, five partial refreshes of a counter, then hibernates.

## Wiring

The module is 4-wire SPI plus reset and busy. Waveshare ships a coloured
8-way cable; the colours below are the usual ones, but trust the silkscreen
over the colour if they disagree.

| Module | Cable  | ESP32-S3 | Notes                          |
| ------ | ------ | -------- | ------------------------------ |
| VCC    | grey   | 3V3      | see voltage note below         |
| GND    | brown  | GND      |                                |
| DIN    | blue   | GPIO 11  | SPI MOSI                       |
| CLK    | yellow | GPIO 12  | SPI SCK                        |
| CS     | orange | GPIO 10  | chip select, active low        |
| DC     | green  | GPIO 9   | data / command                 |
| RST    | white  | GPIO 8   | reset, active low              |
| BUSY   | purple | GPIO 7   | busy, driven by the panel      |

Pin choice avoids GPIO 26-37, which the N16R8 uses for its 16 MB flash and
8 MB octal PSRAM, and the strapping/USB pins 0, 3, 19, 20, 45, 46. Any other
free GPIO works — just update the `#define`s at the top of `src/main.cpp`.

**Voltage:** rev2.1 and later added a level-shifting circuit, so the board
tolerates 3.3 V or 5 V logic. The ESP32-S3 is a 3.3 V part, so run VCC at 3.3 V
and everything is native — no shifter needed. Do not feed 5 V logic into S3
pins.

## Build and run

```
pio run -t upload -t monitor
```

Or use the PlatformIO toolbar in VSCode. Serial is 115200.

## Panel variants

Waveshare has shipped this size with two different controllers, and GxEPD2
needs the matching driver class. There are two environments:

| Env       | Panel / controller             | Use when                        |
| --------- | ------------------------------ | ------------------------------- |
| `uc8176`  | GDEW042T2, UC8176 (IL0398)     | default; older panels           |
| `ssd1683` | GDEY042T81, SSD1683            | newer "V2" panels               |

Start with the default. If the display stays blank, comes up heavily ghosted,
or shows a scrambled/torn image, switch:

```
pio run -e ssd1683 -t upload -t monitor
```

`rev2.1` on the silkscreen refers to the *driver board*, not the panel, so it
doesn't by itself tell you which controller you have — trying both is the
quickest answer.

## What the test pattern checks

- **Nested borders + four corner blocks** — the controller is addressing all
  four edges. A missing edge or corner usually means the wrong driver class.
- **Checkerboard** — pixel-level addressing. Smearing instead of clean squares
  points at a byte-order or line-pitch mismatch.
- **Widening vertical lines** — dropped columns, and how thin a line the panel
  actually resolves.
- **Counter box** — partial refresh works and leaves the rest of the image
  untouched.

## Troubleshooting

**Nothing happens, no serial output.** The N16R8 needs
`board_build.arduino.memory_type = qio_opi`; with the wrong value it boot-loops
before `setup()`. Already set in `platformio.ini`.

**Serial output appears but the screen never changes.** Check BUSY first — if
it's not connected, GxEPD2 waits on a pin that never asserts and the refresh
appears to hang. Then re-check DC and CS.

**Image is inverted or mirrored.** Call `display.setRotation(0..3)` in `setup()`.

**Ghosting after repeated partial updates.** Normal for e-paper. Do a periodic
full refresh (`display.setFullWindow()` + a redraw) every ~10 partial updates.

**Don't refresh in a tight loop.** These panels are rated for a refresh every
~180 s for a long service life. Fine for a test, but don't leave a fast loop
running unattended.

## References

- [GxEPD2 library](https://github.com/ZinggJM/GxEPD2)
- [Waveshare 4.2inch e-Paper Module wiki](https://www.waveshare.com/wiki/4.2inch_e-Paper_Module_Manual)
