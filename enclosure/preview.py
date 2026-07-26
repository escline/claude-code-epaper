"""
ASCII cross-section through the assembled enclosure, for sanity-checking the
geometry without a GUI.

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" enclosure/preview.py

Prints a side view (the YZ plane at mid-width): front of the case on the left,
back on the right, desk surface at the bottom. Verifies by inspection that the
bezel, module pocket, cavity, ESP32 posts and wedge all land where intended.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import FreeCAD as App  # noqa: E402
from enclosure import build_front, build_back, D, P  # noqa: E402


def section(shapes, x, z0, z1, y0, y1, cols=104, rows=40):
    """Sample a YZ plane and return rows of characters."""
    grid = []
    for r in range(rows):
        y = y1 - (y1 - y0) * (r + 0.5) / rows
        line = []
        for c in range(cols):
            z = z0 + (z1 - z0) * (c + 0.5) / cols
            ch = " "
            for mark, s in shapes:
                if s.isInside(App.Vector(x, y, z), 0.01, True):
                    ch = mark
                    break
            line.append(ch)
        grid.append("".join(line))
    return grid


def main():
    front = build_front()
    back = build_back()

    z0, z1 = -2.0, D["wedge_z"] + 2.0
    y0, y1 = -D["drop"] - 2.0, D["OH"] + 2.0

    print("Side view at mid-width (X = %.1f)" % (D["OW"] / 2))
    print("  '#' front shell   '=' back shell + wedge")
    print("  front of case at left, desk surface at bottom")
    print("  Z %.0f..%.0f mm across, Y %.0f..%.0f mm down\n"
          % (z0, z1, y1, y0))

    for line in section([("#", front), ("=", back)], D["OW"] / 2,
                        z0, z1, y0, y1):
        print("  |" + line + "|")

    # Second cut through the ESP32 cradle, to confirm the posts and the USB
    # opening line up with where the board actually sits.
    xs = D["esp_x"] + 2.0
    print("\n\nSide view through the ESP32 cradle (X = %.1f)" % xs)
    for line in section([("#", front), ("=", back)], xs,
                        z0, z1, y0, y1):
        print("  |" + line + "|")

    print("\nKey Z landmarks (front to back):")
    print("  0.0            bezel front face")
    print("  %5.1f          module pocket starts" % D["pocket_z"])
    print("  %5.1f          cavity starts (module rear face)" % D["cavity_z"])
    print("  %5.1f          ESP32 board sits here" % D["esp_board_z"])
    print("  %5.1f          lid inner face" % D["front_depth"])
    print("  %5.1f          lid outer face" % D["total_z"])
    print("  %5.1f          rear of wedge" % D["wedge_z"])


main()
