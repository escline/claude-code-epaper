# Enclosure

Parametric desk enclosure for the Waveshare 4.2" e-Paper Module (Rev2.1) and an
ESP32-S3-N16R8 devkit. Integral 18° wedge, M3 screws, sized to swallow the
Dupont jumper stack so nothing needs rewiring.

> **Not yet verified against hardware.** The display outline and active area come
> from Waveshare's published spec, but the ESP32 board is a clone with no public
> drawing and several dimensions were estimated from photographs. **Print
> `fit-check.stl` first** — see below.

## Build

```
"C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" enclosure/enclosure.py
```

Writes STL (printable) and STEP (editable in FreeCAD) to `enclosure/out/`.
Every dimension lives in the `P` dict at the top of `enclosure.py`.

After any change, run the checks:

```
"C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" enclosure/verify.py
```

It asserts what a render and a bounding box will not: that each part is a
single connected solid (a floating feature shows up as an extra solid), and
that the slots the hardware drops into measure what the hardware measures. Gaps
are the longest *contiguous* free run along a ray — total free length counts
the space either side of a pair of brackets and reports a slot far wider than
the one the board actually has to fit.

For a visual sense of the layout:

```
"C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" enclosure/preview.py
```

prints an ASCII side section showing the bezel, pocket, cavity, cradle and
wedge.

## Parts

| Part | Size | Print orientation |
| --- | --- | --- |
| `front-shell` | 121.4 × 96.9 × 30.3 mm | **Face down** (bezel on the plate) |
| `back-shell` | 121.4 × 113.4 × 50.8 mm | **Angled wedge face down** |
| `fit-check` | 104 × 50 × 16.3 mm | Flat, as oriented |

Neither shell needs supports in the orientation above. Face-down printing also
puts the plate's finish on the bezel, which is the surface you look at.

Suggested: 0.2 mm layers, 3 perimeters, 15% infill, PLA or PETG. PETG if it
sits in direct sun — a black PLA case in a sunny window can creep, and e-paper
itself shouldn't be baked either.

## Print the fit-check first

`fit-check.stl` is a 104 × 50 mm coupon reproducing only the fits that can ruin
a full print. It takes minutes rather than hours. Two zones: the bezel corner
with its module pocket and an M3 boss, and one short end of the ESP32 cradle
with both pillars and their L brackets at true spacing.

Check that:

1. The display module's corner drops into the pocket without force, and the
   bezel lip covers the edge of the glass without intruding on the image.
2. An M3 self-tapping screw bites in the boss without splitting it.
3. The ESP32 board's short edge drops between the two brackets — snug, not
   tight. This is the measurement most likely to be wrong.

The build prints a warning if any part comes out as more than one solid, which
is what a feature floating unsupported in mid-air looks like.

## Dimensions

Measured with calipers:

| Parameter | Value | Note |
| --- | --- | --- |
| `esp_l` × `esp_w` | 63.5 × 28.2 | ESP32 PCB |
| `esp_stack_h` | 19.5 | Dupont housings end at 16.5; the extra 3 lets the wires turn out of the housing rather than being crushed against the lid |
| `usb_span_y` | 26.0 | Ports are 20.7 outer-to-outer; the rest is cable-overmold clearance |

Still estimated — the fit-check coupon is what confirms these:

| Parameter | Current | What to measure |
| --- | --- | --- |
| `disp_panel_t` | 1.2 | Glass/film thickness bonded to the module's front |
| `active_dx`, `active_dy` | 0, 0 | Offset of the visible image from the module centre. Measure front-face edge to glass edge on all four sides; if left ≠ right, that difference ÷ 2 is `active_dx` |

`disp_w`, `disp_h`, `active_w` and `active_h` are Waveshare's published figures
(103.0 × 78.5, active 84.8 × 63.6).

`cavity_d` is **derived**, not set: it follows `esp_stack_h + esp_pcb_t +
esp_top_clear`. It used to be a fixed 24.0, which at the real stack height would
have left the WROOM module about 1 mm off the back of the display.

## Assembly

1. Drop the display module face-down into the front shell pocket. The bezel lip
   retains it; the screw bosses stop it lifting.
2. Seat the ESP32 on the four cradle posts, USB ports toward the opening in the
   right wall. The Dupont housings hang down between the posts.
3. Route the display ribbon through the cable slot at the top left.
4. Close and fasten with four M3×8 self-tapping screws from the back.

The brass standoffs on the module aren't used — the pocket locates it by its
outline instead, which tolerates hole-position error. Remove them, or add
clearance pockets to the lid if you'd rather keep them fitted.
