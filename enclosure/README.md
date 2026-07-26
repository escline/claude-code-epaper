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
| `front-shell` | 123.4 × 106.9 × 30.5 mm | **Face down** (bezel on the plate) |
| `back-shell` | 123.4 × 123.5 × 51.0 mm | **Angled wedge face down** |
| `test-display` | 56 × 50 × 13.9 mm | Flat, as oriented |
| `test-cradle` | 40 × 50 × 25.6 mm | Flat, as oriented |

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

An L-shaped rail with a bezel lip and a screw boss. It reproduces the
**top-left** corner, where the vertical lip is narrowest.

1. **Pocket fit.** Set the module *face down* with its **top-left** corner
   nested inside the L rail. It should drop in without pushing, and not rock.
   Run a fingernail from the rail onto the module's back: it should sit about
   0.4 mm *below* the rail. If it stands proud, `disp_panel_t` is too small and
   the real case will not close — increase it by however far it stands out.
2. **Bezel overlap.** Look through the window from the underside. Along the top
   edge you should see about 0.5 mm of the image covered by the lip. This is
   the check for `active_dy`: too much lip means the offset is overstated, and
   any bare white film or PCB showing means it is understated. The top corner
   is used precisely because its 4.2 mm lip exposes that error — the bottom
   corner's 12.2 mm lip would swallow it.
3. **Heat-set insert.** Melt an M3 insert into the boss with a soldering iron at
   roughly 200 °C, pressing until flush. It should go in square without bulging
   the boss wall. Then check an M3 screw threads into it.

### test-cradle

A section of the two rails, at the **real** 19.5 mm height.

4. **Cradle width.** Drop the ESP32 into the channel between the rails. It
   should land on the ledges with barely perceptible side play. Needing force
   means `esp_w` is short.
5. **Ledge clearance.** The ledge reaches 1.2 mm in under the PCB edge. Check
   it catches the board without fouling any Dupont housing — the housings start
   roughly 1.3 mm in from the edge, so this is the tightest fit in the design.
6. **Stack depth.** With the board seated, the housings hang in the open gap
   under it. Check they clear the base plate, and that the wires can turn out
   sideways without being pinched. This is the check for `esp_stack_h` — get it
   wrong and the back shell will not close.

### Why rails and not corner pillars

The first cradle stood four pillars under the board's corners. The Dupont
housings plug onto the corner pins — GND and 3V3 — so the pillars and the
connectors wanted the same space. Moving the pillars inboard does not help
either; connectors are scattered along both header rows.

Rails run *outboard* of the board's long edges, so the entire support is
outside the board's footprint except for a 1.2 mm ledge catching the PCB edge.
Everything hanging below the board is then in free air.

### What the test parts do not cover

- The wedge angle, and the closure of the two shells against each other.

## Dimensions

Measured with calipers:

| Parameter | Value | Note |
| --- | --- | --- |
| `esp_l` × `esp_w` | 63.5 × 28.2 | ESP32 PCB |
| `esp_stack_h` | 19.5 | Dupont housings end at 16.5; the extra 3 lets the wires turn out of the housing rather than being crushed against the lid |
| `usb_span_y` | 26.0 | Ports are 20.7 outer-to-outer; the rest is cable-overmold clearance |

Front-face borders, PCB edge to white film: left 8.19, right 8.19, top 2.24,
bottom 10.5. Those give:

| Parameter | Value | Derivation |
| --- | --- | --- |
| `active_dx` | 0.0 | (left − right) / 2 |
| `active_dy` | 4.0 | (bottom − top) / 2 = 4.13 |

Inserts are [ruthex](https://www.ruthex.de/en/collections/gewindeeinsatze/m3)
RX-M3x5.7: 4.6 mm OD, 5.7 mm long, 4.0 mm hole. The short RX-M3Sx4.0 also fits
the same hole, but the boss is 25 mm deep so there is no reason to give up the
thread engagement.

Established from the test prints:

| Parameter | Value | How |
| --- | --- | --- |
| `disp_panel_t` | 1.6 | Module sat flush in a 3.2 mm pocket, so it is 3.2 thick overall — 1.6 of film over the 1.6 PCB |
| `pocket_clear` | 0.2 | Play behind the module. It is trapped between the bezel lip and the boss faces, and this is the only thing stopping it shifting |

### Active area vs visible panel

These are **not the same** and the difference matters:

- **Active area** — the pixel array, 84.8 × 63.6 mm. That is 400 × 0.212 and
  300 × 0.212 exactly, from Waveshare's dot pitch.
- **Visible white panel** — the e-paper film, measured 86.00 × 65.48 mm. About
  0.6 mm wider per side and 0.94 mm taller per side than the pixels.

The window is sized to the **active area** (83.8 × 62.6, i.e. 0.5 mm of bezel
overlap per side) so no dead white margin shows around the image.

### The image is not centred on the module

It sits 4 mm high, because the panel's FPC tail needs a wide border along the
bottom. Measured borders to the film are 2.24 mm at the top against 10.5 mm at
the bottom.

With `center_window` true (the default) the **window** is centred in the case
and the module is mounted 4 mm low to suit, so the outside looks symmetric:

| | Left/right | Top/bottom |
| --- | --- | --- |
| Bezel border | 19.8 mm | 22.15 mm |

Internally the lip is necessarily lopsided — 9.8 mm at the sides, 4.15 mm above
the image, 12.15 mm below. Mounting the module off-centre costs case height on
both sides, since the outside stays symmetric; that is why the case is 106.9 mm
tall rather than 98. Set `center_window` false to centre the module instead and
get a visibly lopsided bezel.

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
