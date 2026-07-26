# Enclosure

Parametric desk enclosure for the Waveshare 4.2" e-Paper Module (Rev2.1) and an
ESP32-S3-N16R8 devkit. Integral 18° wedge, M3 heat-set inserts, sized to swallow
the Dupont jumper stack so nothing needs rewiring.

> **Not yet verified against hardware.** Print the two small test parts first —
> see below.

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

It asserts what a render and a bounding box will not: that each part is a single
connected solid (a floating feature shows up as an extra solid), that the slots
the hardware drops into measure what the hardware measures, and that nothing
obstructs the display module once it is seated.

For a visual sense of the layout:

```
"C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" enclosure/preview.py
```

prints an ASCII side section showing the bezel, pocket, cavity, cradle and wedge.

## Parts

| Part | Size | Print orientation |
| --- | --- | --- |
| `front-shell` | 122.4 × 97.9 × 30.3 mm | **Face down** (bezel on the plate) |
| `back-shell` | 122.4 × 114.4 × 50.8 mm | **Angled wedge face down** |
| `test-display` | 56 × 50 × 13.9 mm | Flat, as oriented |
| `test-cradle` | 44 × 50 × 14.1 mm | Flat, as oriented |

Neither shell needs supports in the orientation above. Face-down printing also
puts the plate's finish on the bezel, which is the surface you look at.

Suggested: 0.2 mm layers, 3 perimeters, 15% infill, PLA or PETG. PETG if it sits
in direct sun — a black PLA case in a sunny window can creep, and e-paper itself
shouldn't be baked either.

## Print the test parts first

`test-display` and `test-cradle` are small parts reproducing only the fits that
can ruin a full print. Minutes each rather than hours.

**They are two separate parts on purpose.** The display module is 103 × 78.5 mm,
so once its corner is seated in the bezel rail it covers the whole plate —
anything printed alongside collides with it and the test cannot be performed.

### test-display

An L-shaped rail with a bezel lip and a screw boss.

1. **Pocket fit.** Set the module *face down* with its corner nested inside the
   L rail. It should drop in without pushing, and not rock. Run a fingernail
   from the rail onto the module's back: it should sit about 0.4 mm *below* the
   rail. If it stands proud, `disp_panel_t` is too small and the real case will
   not close — increase it by however far it stands out.
2. **Bezel overlap.** Look through the window from the underside. You should see
   the white panel with the lip covering its edge evenly on both sides.
3. **Heat-set insert.** Melt an M3 insert into the boss with a soldering iron at
   roughly 200 °C, pressing until flush. It should go in square without bulging
   the boss wall. Then check an M3 screw threads into it.

### test-cradle

Two pillars with L brackets.

4. **Cradle width.** Lower the ESP32's **short end** between the two brackets so
   the PCB rests on both pillar tops. Only one end is supported — you are
   testing width. It should drop in with barely perceptible side play. Needing
   force means `esp_w` is short.

### What the test parts do not cover

- `esp_stack_h`. The test pillars are 8 mm; the real cradle is 19.5 mm. Get this
  wrong and the back shell will not close. Measure it with calipers.
- The wedge angle, and the closure of the two shells against each other.

## Dimensions

Measured with calipers:

| Parameter | Value | Note |
| --- | --- | --- |
| `esp_l` × `esp_w` | 63.5 × 28.2 | ESP32 PCB |
| `esp_stack_h` | 19.5 | Dupont housings end at 16.5; the extra 3 lets the wires turn out of the housing rather than being crushed against the lid |
| `usb_span_y` | 26.0 | Ports are 20.7 outer-to-outer; the rest is cable-overmold clearance |

Still estimated:

| Parameter | Current | What to measure |
| --- | --- | --- |
| `disp_panel_t` | 1.2 | Glass/film thickness bonded to the module's front. Total module thickness minus the 1.6 PCB |
| `active_dx`, `active_dy` | 0, 0 | See below |
| `insert_od`, `insert_len`, `insert_hole` | 4.6, 5.7, 4.0 | Your heat-set inserts. Hole should be a few tenths under the knurled OD |

### Active area vs visible panel

These are **not the same** and the difference matters:

- **Active area** — the pixel array, 84.8 × 63.6 mm. That is 400 × 0.212 and
  300 × 0.212 exactly, from Waveshare's dot pitch.
- **Visible white panel** — the e-paper film, measured 86.00 × 65.48 mm. About
  0.6 mm wider per side and 0.94 mm taller per side than the pixels.

The window is sized to the **active area** (currently 83.8 × 62.6, i.e. 0.5 mm
of bezel overlap per side) so no dead white margin shows around the image.

To position it, measure on the module's **front** face, from each PCB edge to
the nearest edge of the white panel, with the display the way up it will be
mounted:

```
active_dx = (left_border  - right_border)  / 2
active_dy = (bottom_border - top_border)   / 2
```

`disp_w`, `disp_h`, `active_w` and `active_h` are Waveshare's published figures
(103.0 × 78.5, active 84.8 × 63.6).

`cavity_d`, `boss_od` and `margin` are **derived**, not set — from the ESP32
stack and the insert size respectively, so they cannot drift out of step with
the parts they have to clear.

## Assembly

1. Melt four M3 heat-set inserts into the front shell bosses.
2. Drop the display module face-down into the front shell pocket. The bezel lip
   retains it; the bosses stop it lifting.
3. Seat the ESP32 on the four cradle pillars, USB ports toward the opening in
   the right wall. The Dupont housings hang down between the pillars.
4. Route the display ribbon through the cable slot at the top left.
5. Close and fasten with four M3 screws from the back.

The brass standoffs on the module aren't used — the pocket locates it by its
outline instead, which tolerates hole-position error. Remove them, or add
clearance pockets to the lid if you'd rather keep them fitted.
