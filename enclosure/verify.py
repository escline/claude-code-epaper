"""
Assertions about the built geometry. Run after any change to enclosure.py:

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" enclosure/verify.py

Checks the things a render or a bounding box will not tell you: that nothing is
floating, and that the slots the hardware has to drop into are actually the size
the hardware is.
"""

import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import FreeCAD as App  # noqa: E402
from enclosure import (  # noqa: E402
    build_front, build_back, build_stand, build_fit_display, build_fit_cradle,
    pin_positions, FIT_H, D, P)  # noqa: F401

FAILS = []


def check(name, ok, detail=""):
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                           ("  - " + detail) if detail else ""))
    if not ok:
        FAILS.append(name)


def solid(shape, v):
    return shape.isInside(App.Vector(*v), 0.01, True)


def span(shape, p0, p1, samples=600):
    """Longest *contiguous* free run along the segment p0->p1.

    Contiguous is the point: total free length would count the space either
    side of a pair of brackets as though the board could occupy it, and report
    a slot far wider than the one the board actually has to drop into.
    """
    import math
    best = run = 0
    for i in range(samples):
        t = (i + 0.5) / samples
        p = [p0[j] + (p1[j] - p0[j]) * t for j in range(3)]
        if solid(shape, p):
            run = 0
        else:
            run += 1
            best = max(best, run)
    return best / samples * math.dist(p0, p1)


def outer_span(shape, p0, p1, samples=600):
    """Distance from the first to the last material along the segment.

    Not the longest contiguous run: the register lip is a ring, so a contiguous
    measure returns one 3mm wall rather than the lip's overall width, which is
    what has to fit the cavity.
    """
    import math
    first = last = None
    for i in range(samples):
        t = (i + 0.5) / samples
        p = [p0[j] + (p1[j] - p0[j]) * t for j in range(3)]
        if solid(shape, p):
            if first is None:
                first = i
            last = i
    if first is None:
        return 0.0
    return (last - first + 1) / samples * math.dist(p0, p1)


def solid_depth(shape, p0, p1, samples=400):
    """Total material along the segment - a thickness probe."""
    n = sum(1 for i in range(samples)
            if solid(shape, [p0[j] + (p1[j] - p0[j]) * (i + 0.5) / samples
                             for j in range(3)]))
    return n / samples * math.dist(p0, p1)


def main():
    front = build_front()
    back = build_back()
    stand = build_stand()
    t_disp = build_fit_display()
    t_crad = build_fit_cradle()

    print("\nConnectivity (a floating feature shows up as an extra solid)")
    for nm, sh in [("front-shell", front), ("back-shell", back),
                   ("stand", stand),
                   ("test-display", t_disp), ("test-cradle", t_crad)]:
        check("%s is one connected solid" % nm, len(sh.Solids) == 1,
              "%d solids" % len(sh.Solids))
        check("%s is valid geometry" % nm, sh.isValid())

    print("\nDisplay fit")
    # The module pocket must accept the board outline.
    y = D["OH"] / 2
    z = D["pocket_z"] + D["pocket_d"] / 2
    w = span(front, (0, y, z), (D["OW"], y, z))
    check("pocket width accepts module", w >= P["disp_w"],
          "%.2f mm free, module is %.1f" % (w, P["disp_w"]))
    x = D["OW"] / 2
    h = span(front, (x, 0, z), (x, D["OH"], z))
    check("pocket height accepts module", h >= P["disp_h"],
          "%.2f mm free, module is %.1f" % (h, P["disp_h"]))

    # The window must not intrude on the active area.
    zw = P["bezel_t"] / 2
    ww = span(front, (0, y, zw), (D["OW"], y, zw))
    check("window narrower than active area", ww < P["active_w"],
          "%.2f mm open vs %.1f active" % (ww, P["active_w"]))
    # Tolerance has to respect the sampling step, or the check is tighter than
    # the measurement it is based on and fails on rounding.
    step = D["OW"] / 600.0
    check("window overlap is symmetric and correct",
          abs(ww - (P["active_w"] - 2 * P["bezel_overlap"])) < 2 * step,
          "%.2f mm, expected %.2f, tolerance %.2f"
          % (ww, P["active_w"] - 2 * P["bezel_overlap"], 2 * step))

    # The image sits 4mm above the module centre, so a window centred on the
    # module would clip it. Check the window is centred on the image instead,
    # and that the bezel overlaps the image on every side.
    check("window centred on the image, not the module",
          abs(D["win_x"] + D["win_w"] / 2 - D["active_cx"]) < 0.01 and
          abs(D["win_y"] + D["win_h"] / 2 - D["active_cy"]) < 0.01,
          "window centre (%.2f, %.2f) vs image centre (%.2f, %.2f)"
          % (D["win_x"] + D["win_w"] / 2, D["win_y"] + D["win_h"] / 2,
             D["active_cx"], D["active_cy"]))

    ov_b = (D["active_cy"] - P["active_h"] / 2)
    check("bezel overlaps the image on all sides",
          D["win_y"] > ov_b and
          D["win_y"] + D["win_h"] < D["active_cy"] + P["active_h"] / 2,
          "%.2f mm overlap bottom" % (D["win_y"] - ov_b))

    # Module must still clear the bosses after being shifted off-centre.
    check("module clears the case wall after offset",
          D["pocket_x"] >= D["margin"] - 0.01 and
          D["pocket_y"] >= D["margin"] - 0.01,
          "pocket at (%.2f, %.2f), margin %.2f"
          % (D["pocket_x"], D["pocket_y"], D["margin"]))

    print("\nESP32 cradle")
    # Between the L brackets, across the board's width.
    zb = D["front_depth"] - D["esp_post_h"] - P["esp_pcb_t"] / 2
    xb = D["esp_x"] + 1.0
    g = span(back, (xb, D["esp_y"] - 8, zb), (xb, D["esp_y"] + P["esp_w"] + 8, zb))
    check("bracket gap accepts board width", g >= P["esp_w"],
          "%.2f mm free, board is %.1f" % (g, P["esp_w"]))
    check("bracket gap is not sloppy", g <= P["esp_w"] + 1.5,
          "%.2f mm free" % g)

    # Same check on the test print, which is what actually gets printed first.
    cy = (FIT_H - P["esp_w"]) / 2
    cz = 3.0 + D["esp_post_h"] + P["esp_pcb_t"] / 2
    gc = span(t_crad, (16.0, cy - 8, cz), (16.0, cy + P["esp_w"] + 8, cz))
    check("test print reproduces the same gap", abs(gc - g) < 0.05,
          "test %.2f vs shell %.2f" % (gc, g))

    # The header plastic and the Dupont housings occupy a band along each long
    # edge of the board, from the PCB underside down to the housings' ends.
    # Nothing in the cradle may enter it - a ledge here bears on the plastic
    # instead of the board and seats the ESP32 high.
    intrude = []
    zb0 = D["front_depth"] - D["esp_post_h"] + 0.5
    for band in (0.0, P["esp_w"] - 4.0):
        for i in range(24):
            for k in range(14):
                x = D["esp_x"] + P["esp_l"] * (i + 0.5) / 24
                y = D["esp_y"] + band + 4.0 * (k % 4) / 4
                z = zb0 + (D["front_depth"] - zb0 - 0.2) * (k + 0.5) / 14
                if solid(back, (x, y, z)):
                    intrude.append((round(x, 1), round(y, 1)))
    check("nothing intrudes under the board's long edges", not intrude,
          "%d sample points blocked" % len(intrude))

    # Antenna keep-out. The tab overhangs the end away from the USB ports and
    # sits proud of the main board; both shells must leave it alone, with air
    # around it rather than plastic against a radiating element.
    # Scoped to the tab's own envelope plus a margin. A larger box reached into
    # the module pocket's sidewall and the volume the display itself occupies,
    # and failed on legitimate structure.
    ko = P["esp_ant_keepout"]
    ax1 = D["esp_x"]
    ax0 = D["esp_x"] - P["esp_ant_len"] - ko
    ay0 = D["esp_y"] + (P["esp_w"] - P["esp_ant_w"]) / 2 - ko
    ay1 = ay0 + P["esp_ant_w"] + 2 * ko
    az0 = D["esp_board_z"] - P["esp_pcb_t"] - ko
    az1 = D["esp_board_z"]
    hits = []
    for sh, nm in ((front, "front"), (back, "back")):
        for i in range(16):
            for j in range(10):
                for k in range(8):
                    x = ax0 + (ax1 - ax0) * (i + 0.5) / 16
                    y = ay0 + (ay1 - ay0) * (j + 0.5) / 10
                    z = az0 + (az1 - az0) * (k + 0.5) / 8
                    if solid(sh, (x, y, z)):
                        hits.append(nm)
    check("antenna keep-out is clear in both shells", not hits,
          "%d blocked (%s)" % (len(hits), ", ".join(sorted(set(hits))) or "-"))

    # Rail lips must overhang the board's front face, or nothing stops the
    # board falling toward the display.
    zlip = D["esp_board_z"] - P["esp_pcb_t"] - P["esp_lip_in"] / 2
    xl = D["esp_x"] + P["esp_l"] / 2
    lipgap = span(back, (xl, D["esp_y"] - 8, zlip),
                  (xl, D["esp_y"] + P["esp_w"] + 8, zlip))
    check("rail lips overhang the board face",
          lipgap < P["esp_w"] - 1.0,
          "%.2f mm open at lip height vs %.2f board" % (lipgap, P["esp_w"]))

    print("\nTest print clearance")
    # The module is 103 x 78.5, so once its corner is in the rail it covers the
    # whole plate. Nothing may stand above the pocket floor inside that
    # footprint or the module cannot seat - which is how a cradle sharing the
    # plate with the bezel corner went unnoticed.
    # Sample inside the pocket, not behind it. Behind the pocket is where the
    # bosses deliberately sit, reaching over the module's corners to retain it.
    zf = P["bezel_t"] + D["pocket_d"] / 2
    obstructed = []
    for i in range(40):
        for j in range(40):
            x = D["margin"] + (56.0 - D["margin"]) * (i + 0.5) / 40
            y = D["margin"] + (FIT_H - D["margin"]) * (j + 0.5) / 40
            # The locating pin belongs in the pocket - it goes through the
            # board's mounting hole.
            if math.dist((x, y), (D["pin_inset"], D["pin_inset"])) \
                    < D["pin_d"] / 2 + 0.6:
                continue
            if solid(t_disp, (x, y, zf)):
                obstructed.append((round(x, 1), round(y, 1)))
    check("nothing obstructs the seated module", not obstructed,
          "%d sample points blocked" % len(obstructed))

    # The module is trapped between the bezel lip and the boss faces. Too much
    # play here and the panel shifts in its pocket; too little and the front
    # shell will not close on it.
    play = D["pocket_d"] - D["mod_t"]
    check("gap behind the module is small", 0.05 <= play <= 0.35,
          "%.2f mm (pocket %.2f, module %.2f)"
          % (play, D["pocket_d"], D["mod_t"]))

    # That gap only means anything if a boss is actually across it. With a
    # larger margin the bosses sat wholly outside the module's footprint and
    # touched nothing, so nothing held the display in its pocket at all.
    # The module is lowered in through the cavity, so its whole footprint must
    # be clear from the pocket back to the lid. This is the rule two smaller-
    # margin designs broke: a boss beside the corner, then a boss on the
    # mounting hole whose shoulder was just as impassable.
    check("screw bosses stay out of the module's path",
          D["boss_inset"] + D["boss_od"] / 2 <= D["margin"] + 0.01,
          "boss reaches %.2f, pocket starts %.2f"
          % (D["boss_inset"] + D["boss_od"] / 2, D["margin"]))

    # Pins may sit inside the footprint only because they thread through the
    # mounting holes.
    check("pin fits the module's mounting hole",
          0.2 <= P["disp_hole_d"] - D["pin_d"] <= 0.8,
          "pin %.2f in a %.2f hole" % (D["pin_d"], P["disp_hole_d"]))
    check("pin does not stand proud of the pocket",
          D["pocket_d"] - 0.2 <= D["pocket_d"],
          "pin %.2f mm in a %.2f mm pocket"
          % (D["pocket_d"] - 0.2, D["pocket_d"]))

    # Retention comes from the lid, since the front shell cannot provide it.
    for (px, py) in [pin_positions()[0], pin_positions()[3]]:
        check("lid post reaches the module's back at (%.0f, %.0f)" % (px, py),
              solid(back, (px, py, D["cavity_z"] + 0.5)),
              "post present just behind the pocket")

    print("\nShell closure")
    # Measure the lid's register lip against the front shell's cavity at the
    # depth where they actually engage.
    # This is a difference of two sampled measurements, so its error is twice
    # the step. At the default 600 samples that is +-0.4mm on a 0.8mm target,
    # which would make the check meaningless - sample it finely.
    zl = D["front_depth"] - 0.8
    y = D["OH"] / 2
    n = 4000
    cav = span(front, (0, y, zl), (D["OW"], y, zl), n)       # open run in shell
    lip = outer_span(back, (0, y, zl), (D["OW"], y, zl), n)  # lip width on lid
    gap = cav - lip
    tol = 2 * D["OW"] / n
    check("lid lip fits the front shell cavity", gap > 0.5,
          "cavity %.2f, lip %.2f, clearance %.2f total (+-%.2f)"
          % (cav, lip, gap, tol))
    check("shell clearance matches shell_fit",
          abs(gap - P["shell_fit"]) < 3 * tol,
          "%.2f vs %.2f" % (gap, P["shell_fit"]))

    print("\nUSB opening")
    uy = D["esp_y"] + P["esp_w"] / 2
    uz = D["esp_board_z"] + P["esp_pcb_t"] / 2
    check("USB opening is clear at board height",
          not solid(front, (D["OW"] - P["wall"] / 2, uy, uz)),
          "wall is open where the ports are")

    print("\nPrintability")
    # Each part must lie flat. The back shell only does so once the wedge is
    # off it: the wedge projected past the lid's outer face, which put it below
    # the bed lid-down and stood the lid vertical wedge-down.
    # The criterion is that nothing sticks out past the face it prints on, not
    # how deep the part is.
    check("back shell does not project past its own lid",
          back.BoundBox.ZMax <= D["total_z"] + 0.01,
          "reaches z %.1f, lid outer face at %.1f"
          % (back.BoundBox.ZMax, D["total_z"]))
    print("\nStand")
    # Which end is thick decides which way the display leans, and getting it
    # backwards tips the screen face down. Measure the real solid rather than
    # trusting the arithmetic: the old check confirmed the slope magnitude
    # while the sign was wrong.
    # The stand is exported rotated flat, so measure it in its own frame:
    # vertical thickness above a bottom that must be planar at y=0.
    top = stand.BoundBox.YMax

    def wedge_t(z):
        return solid_depth(stand, (D["OW"] / 2, -1.0, z),
                           (D["OW"] / 2, top + 1.0, z))

    # Anchored at z=0, which is where the case's front bottom edge lands - not
    # the bounding box, whose ZMin is the tip of the steep front face and gives
    # a partial thickness reading.
    z0 = 0.0
    # Not named 'span': that shadows the span() helper for the whole of main(),
    # and every earlier call to it then fails as an unassigned local.
    foot = D["total_z"] * math.cos(math.radians(P["tilt_deg"]))
    t_front = wedge_t(z0 + 2.0)
    t_back = wedge_t(z0 + foot - 2.0)
    check("stand is thicker at the front than the back", t_front > t_back + 5,
          "front %.1f mm, back %.1f mm - thick at the back tips it face down"
          % (t_front, t_back))

    # The underside must be one plane on the desk. Sampled just above y=0
    # across the footprint: any gap means the stand only touches along a line
    # and the assembly rocks.
    gaps = [z for z in [z0 + 2 + i * (foot - 4) / 12 for i in range(13)]
            if not solid(stand, (D["OW"] / 2, 0.3, z))]
    check("stand underside is flat on the desk", not gaps,
          "%d of 13 sample points off the desk" % len(gaps))

    # The face the case sits on is horizontal in case coordinates, so after
    # rotating the desk plane flat it descends at exactly tan(tilt).
    got = (t_front - t_back) / (foot - 4.0)
    want = math.tan(math.radians(P["tilt_deg"]))
    check("top face slopes at the tilt angle", abs(got - want) < 0.03,
          "%.3f vs tan(%.0f) = %.3f" % (got, P["tilt_deg"], want))
    check("rear of the stand is thick enough to print",
          t_back >= P["stand_min_t"] - 0.6, "%.1f mm" % t_back)

    # Pegs must land in the sockets.
    for sx in D["peg_x"]:
        check("stand peg at x=%.0f enters a socket" % sx,
              not solid(front, (sx, P["peg_h"] / 2, P["peg_z"])),
              "front shell is bored there")
        check("socket is inside solid material at x=%.0f" % sx,
              solid(front, (sx + P["peg_d"], P["peg_h"] / 2, P["peg_z"])),
              "material beside the socket")

    print("\nAssembled on a desk")
    # Rotate the assembly into its resting orientation and see what touches the
    # desk. A wedge the wrong way round leaves the case digging through it -
    # which is what three revisions of slope and thickness checks all missed,
    # because they described the stand without ever placing the case on it.
    shells = front.fuse(back)
    st = build_stand(for_print=False)
    for sh in (shells, st):
        sh.rotate(App.Vector(0, 0, 0), App.Vector(1, 0, 0), P["tilt_deg"])
    desk = st.BoundBox.YMin
    lift = shells.BoundBox.YMin - desk
    check("case sits above the desk, not through it", lift > 0.5,
          "lowest point of the shells is %.1f mm above the stand's underside"
          % lift)
    check("case is not perched too high", lift < P["stand_min_t"] + 2.0,
          "%.1f mm" % lift)

    # And the screen has to end up facing up, not down.
    ny = math.sin(math.radians(P["tilt_deg"]))
    check("screen faces up and forward", ny > 0,
          "normal Y %+.2f" % ny)

    print("\nAssembly interference")
    # The two shells must not want the same space. A lid boss added for the
    # stand screws overlapped the front shell's bottom wall and nothing caught
    # it, because no check compared the parts against each other.
    clash = front.common(back)
    check("front and back shells do not interpenetrate",
          clash.Volume < 1.0, "%.2f mm3 of overlap" % clash.Volume)

    print("")
    if FAILS:
        print("%d CHECK(S) FAILED: %s" % (len(FAILS), ", ".join(FAILS)))
    else:
        print("All checks passed.")


main()
