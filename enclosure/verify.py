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
from enclosure import build_front, build_back, build_fit_check, D, P  # noqa: E402

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


def main():
    front = build_front()
    back = build_back()
    coupon = build_fit_check()

    print("\nConnectivity (a floating feature shows up as an extra solid)")
    for nm, sh in [("front-shell", front), ("back-shell", back),
                   ("fit-check", coupon)]:
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
    check("window overlap is symmetric and correct",
          abs(ww - (P["active_w"] - 2 * P["bezel_overlap"])) < 0.05,
          "%.2f mm" % ww)

    print("\nESP32 cradle")
    # Between the L brackets, across the board's width.
    zb = D["front_depth"] - D["esp_post_h"] - P["esp_pcb_t"] / 2
    xb = D["esp_x"] + 1.0
    g = span(back, (xb, D["esp_y"] - 8, zb), (xb, D["esp_y"] + P["esp_w"] + 8, zb))
    check("bracket gap accepts board width", g >= P["esp_w"],
          "%.2f mm free, board is %.1f" % (g, P["esp_w"]))
    check("bracket gap is not sloppy", g <= P["esp_w"] + 1.5,
          "%.2f mm free" % g)

    # Same check on the coupon, which is what actually gets printed first.
    cy = (50.0 - P["esp_w"]) / 2
    cz = P["bezel_t"] + D["pocket_d"] + 8.0 + P["esp_pcb_t"] / 2
    cx = 56.0 + 4.0 + 16.0
    gc = span(coupon, (cx, cy - 8, cz), (cx, cy + P["esp_w"] + 8, cz))
    check("coupon reproduces the same gap", abs(gc - g) < 0.05,
          "coupon %.2f vs shell %.2f" % (gc, g))

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
