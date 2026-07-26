"""
Assertions about the built geometry. Run after any change to enclosure.py:

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" enclosure/verify.py

Checks the things a render or a bounding box will not tell you: that nothing is
floating, and that the slots the hardware has to drop into are actually the size
the hardware is.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import FreeCAD as App  # noqa: E402
from enclosure import (  # noqa: E402
    build_front, build_back, build_fit_display, build_fit_cradle, FIT_H, D, P)

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


def solid_span(shape, p0, p1, samples=600):
    """Longest contiguous run of material along the segment p0->p1."""
    import math
    best = run = 0
    for i in range(samples):
        t = (i + 0.5) / samples
        p = [p0[j] + (p1[j] - p0[j]) * t for j in range(3)]
        if solid(shape, p):
            run += 1
            best = max(best, run)
        else:
            run = 0
    return best / samples * math.dist(p0, p1)


def main():
    front = build_front()
    back = build_back()
    t_disp = build_fit_display()
    t_crad = build_fit_cradle()

    print("\nConnectivity (a floating feature shows up as an extra solid)")
    for nm, sh in [("front-shell", front), ("back-shell", back),
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
    cz = 3.0 + 8.0 + P["esp_pcb_t"] / 2
    gc = span(t_crad, (16.0, cy - 8, cz), (16.0, cy + P["esp_w"] + 8, cz))
    check("test print reproduces the same gap", abs(gc - g) < 0.05,
          "test %.2f vs shell %.2f" % (gc, g))

    print("\nTest print clearance")
    # The module is 103 x 78.5, so once its corner is in the rail it covers the
    # whole plate. Nothing may stand above the pocket floor inside that
    # footprint or the module cannot seat - which is how a cradle sharing the
    # plate with the bezel corner went unnoticed.
    zf = P["bezel_t"] + D["pocket_d"] + 0.5
    obstructed = []
    for i in range(40):
        for j in range(40):
            x = D["margin"] + (56.0 - D["margin"]) * (i + 0.5) / 40
            y = D["margin"] + (FIT_H - D["margin"]) * (j + 0.5) / 40
            if solid(t_disp, (x, y, zf)):
                obstructed.append((round(x, 1), round(y, 1)))
    check("nothing obstructs the seated module", not obstructed,
          "%d sample points blocked" % len(obstructed))

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
    lip = solid_span(back, (0, y, zl), (D["OW"], y, zl), n)  # lip run on lid
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

    print("\nStand")
    # Front bottom edge and the rear of the wedge must both reach the desk.
    check("wedge drops the back by tan(tilt) x depth",
          abs(D["drop"] - D["wedge_z"] * __import__("math").tan(
              __import__("math").radians(P["tilt_deg"]))) < 0.01,
          "%.2f mm" % D["drop"])

    print("")
    if FAILS:
        print("%d CHECK(S) FAILED: %s" % (len(FAILS), ", ".join(FAILS)))
    else:
        print("All checks passed.")


main()
