"""
Side view of the ASSEMBLED unit as it sits on a desk:

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" enclosure/assembly.py

Everything else is drawn with the case upright, which makes it very hard to see
which way the thing actually leans - three separate stand errors survived the
numeric checks and were caught by looking at renders. This puts the desk
horizontal and shows the screen, so the lean is obvious.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import FreeCAD as App  # noqa: E402
from enclosure import build_front, build_back, build_stand, D, P  # noqa: E402


def assembled():
    """Fuse the three parts and rotate the desk plane horizontal."""
    a = build_front().fuse(build_back()).fuse(build_stand(for_print=False))
    # In case coordinates the desk is y = z*tan(tilt) - offset. Rotating by
    # +tilt about X makes it horizontal, which is the assembled orientation.
    a.rotate(App.Vector(0, 0, 0), App.Vector(1, 0, 0), P["tilt_deg"])
    a.translate(App.Vector(0, -a.BoundBox.YMin, 0))
    return a


def screen_normal():
    """Where the display faces, in world coordinates."""
    import math
    t = math.radians(P["tilt_deg"])
    # Case-frame front face normal is -Z; apply the same rotation.
    return (0.0, math.sin(t), -math.cos(t))


def main():
    a = assembled()
    bb = a.BoundBox
    x = bb.XMin + bb.XLength / 2

    cols, rows = 96, 40
    z0, z1 = bb.ZMin - 3, bb.ZMax + 3
    y0, y1 = -2.0, bb.YMax + 3

    print("\nAssembled, viewed from the side. Desk is the '~~~' line.")
    print("Screen is the left-hand face. Front of the unit at the left.\n")

    for r in range(rows):
        y = y1 - (y1 - y0) * (r + 0.5) / rows
        line = []
        for c in range(cols):
            z = z0 + (z1 - z0) * (c + 0.5) / cols
            if a.isInside(App.Vector(x, y, z), 0.01, True):
                line.append("#")
            elif -0.9 < y < 0.0:
                line.append("~")
            else:
                line.append(" ")
        print("  " + "".join(line))

    ny, nz = screen_normal()[1], screen_normal()[2]
    print("\n  overall  %.0f wide x %.0f high x %.0f deep mm"
          % (bb.XLength, bb.YLength, bb.ZLength))
    print("  screen normal points %s and %s"
          % ("UP" if ny > 0 else "DOWN", "forward" if nz < 0 else "BACKWARD"))
    print("  so the screen leans %s by %.0f degrees"
          % ("BACK, facing up" if ny > 0 else "FORWARD, facing down",
             P["tilt_deg"]))


main()
