"""
Parametric desk enclosure for the Waveshare 4.2" e-Paper Module (Rev2.1) and an
ESP32-S3-N16R8 devkit.

Build with FreeCAD's headless CLI:

    "C:/Program Files/FreeCAD 1.1/bin/freecadcmd.exe" enclosure/enclosure.py

Outputs STL (printable) and STEP (editable) into enclosure/out/.

------------------------------------------------------------------------------
Coordinate system
    X  width   (0 .. OW), left to right looking at the screen
    Y  height  (0 .. OH), 0 at the bottom
    Z  depth   0 at the front (bezel face), increasing toward the back

Parts
    front_shell  bezel tray holding the display. Print FACE DOWN, no supports.
    back_shell   lid + integral 18 deg wedge base. Print ANGLED BOTTOM DOWN.
    fit_check    small coupon reproducing only the critical fits. Print this
                 first - it takes minutes instead of hours and validates the
                 dimensions marked MEASURE below.

Anything tagged MEASURE is an estimate. The display outline and active area are
from Waveshare's published spec; the ESP32 board is a clone with no public
drawing, so those came off the photographs and are the most likely to be wrong.
------------------------------------------------------------------------------
"""

import os
import math

import FreeCAD as App
import Part
import Mesh

# ============================================================================
# Parameters - edit these
# ============================================================================
P = {
    # --- Waveshare 4.2" e-Paper Module Rev2.1 (published outline) ----------
    "disp_w": 103.0,
    "disp_h": 78.5,
    "disp_pcb_t": 1.6,
    "disp_panel_t": 1.2,      # MEASURE: glass/film bonded to the front
    "active_w": 84.8,         # published active area
    "active_h": 63.6,
    "active_dx": 0.0,         # MEASURE: active area offset from module centre
    "active_dy": 0.0,
    "bezel_overlap": 0.5,     # bezel covers this much of the active area/side

    # --- ESP32-S3 devkit (clone - all MEASURE) ----------------------------
    "esp_w": 25.5,            # short edge
    "esp_l": 63.5,            # long edge, USB ports on one short end
    "esp_pcb_t": 1.6,
    "esp_stack_h": 16.0,      # PCB underside to bottom of Dupont housings
    # The two USB-C ports sit side by side across the board's short edge, so
    # the opening is wide in Y (case height) and shallow in Z (case depth).
    "usb_span_y": 24.0,
    "usb_span_z": 9.0,
    "esp_shift_x": 12.0,      # board offset toward the USB wall

    # --- construction ------------------------------------------------------
    "fit": 0.4,               # clearance around the display module
    "margin": 9.0,            # bezel border, sized to clear the screw bosses
    "bezel_t": 2.0,           # front face thickness
    "wall": 3.0,
    "lid_t": 2.5,
    "cavity_d": 24.0,         # clear depth behind the module for electronics

    # --- screws ------------------------------------------------------------
    "boss_od": 7.0,
    "boss_pilot": 2.5,        # M3 self-tapping pilot
    "screw_clear": 3.4,       # clearance hole in the lid
    "screw_head": 6.2,        # counterbore
    "screw_head_d": 2.2,

    # --- stand -------------------------------------------------------------
    "tilt_deg": 18.0,
    "rear_overhang": 18.0,    # wedge projection behind the lid, for stability
    "foot_wall": 3.0,

    # --- misc --------------------------------------------------------------
    "cable_slot_w": 14.0,     # display ribbon route past the electronics
    "cable_slot_h": 9.0,
    "vent": True,
}


def derived(p):
    d = dict(p)
    d["mod_t"] = p["disp_panel_t"] + p["disp_pcb_t"]
    d["OW"] = p["disp_w"] + p["fit"] + 2 * p["margin"]
    d["OH"] = p["disp_h"] + p["fit"] + 2 * p["margin"]
    d["front_depth"] = p["bezel_t"] + d["mod_t"] + 0.4 + p["cavity_d"]
    d["pocket_z"] = p["bezel_t"]
    d["pocket_d"] = d["mod_t"] + 0.4
    d["cavity_z"] = d["pocket_z"] + d["pocket_d"]
    d["win_w"] = p["active_w"] - 2 * p["bezel_overlap"]
    d["win_h"] = p["active_h"] - 2 * p["bezel_overlap"]
    d["win_x"] = (d["OW"] - d["win_w"]) / 2 + p["active_dx"]
    d["win_y"] = (d["OH"] - d["win_h"]) / 2 + p["active_dy"]
    d["pocket_x"] = (d["OW"] - (p["disp_w"] + p["fit"])) / 2
    d["pocket_y"] = (d["OH"] - (p["disp_h"] + p["fit"])) / 2
    d["boss_inset"] = p["boss_od"] / 2 + 1.5
    d["total_z"] = d["front_depth"] + p["lid_t"]
    d["wedge_z"] = d["total_z"] + p["rear_overhang"]
    d["drop"] = math.tan(math.radians(p["tilt_deg"])) * d["wedge_z"]

    # ESP32 placement, shared by both shells so the USB opening in the front
    # wall and the cradle on the lid cannot drift apart.
    d["esp_x"] = (d["OW"] - p["esp_l"]) / 2 + p["esp_shift_x"]
    d["esp_y"] = p["wall"] + 6.0
    d["esp_post_h"] = p["esp_stack_h"]
    d["esp_board_z"] = d["front_depth"] - d["esp_post_h"]  # board sits here
    return d


D = derived(P)

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
os.makedirs(OUT, exist_ok=True)


# ============================================================================
# Helpers
# ============================================================================
def box(dx, dy, dz, x=0.0, y=0.0, z=0.0):
    b = Part.makeBox(dx, dy, dz)
    b.translate(App.Vector(x, y, z))
    return b


def cyl(r, h, x=0.0, y=0.0, z=0.0, dr=(0, 0, 1)):
    c = Part.makeCylinder(r, h, App.Vector(x, y, z), App.Vector(*dr))
    return c


def boss_positions():
    """Screw boss centres, inset from each corner."""
    i = D["boss_inset"]
    return [
        (i, i),
        (D["OW"] - i, i),
        (i, D["OH"] - i),
        (D["OW"] - i, D["OH"] - i),
    ]


# ============================================================================
# Front shell - bezel tray
# ============================================================================
def build_front():
    s = box(D["OW"], D["OH"], D["front_depth"])

    # Viewing window through the bezel face. Slightly smaller than the active
    # area so the bezel overlaps its edge and hides any misalignment.
    s = s.cut(box(D["win_w"], D["win_h"], P["bezel_t"] + 2,
                  D["win_x"], D["win_y"], -1))

    # Pocket the display module drops into, located by its outline.
    s = s.cut(box(P["disp_w"] + P["fit"], P["disp_h"] + P["fit"],
                  D["pocket_d"] + 0.01,
                  D["pocket_x"], D["pocket_y"], D["pocket_z"]))

    # Main cavity for the electronics.
    s = s.cut(box(D["OW"] - 2 * P["wall"], D["OH"] - 2 * P["wall"],
                  P["cavity_d"] + 1,
                  P["wall"], P["wall"], D["cavity_z"]))

    # Screw bosses rise from the back of the module pocket, so they also stop
    # the module lifting out of its pocket.
    for (bx, by) in boss_positions():
        b = cyl(P["boss_od"] / 2, P["cavity_d"], bx, by, D["cavity_z"])
        s = s.fuse(b)
        s = s.cut(cyl(P["boss_pilot"] / 2, P["cavity_d"] + 1,
                      bx, by, D["cavity_z"]))

    # USB-C access through the right wall, aligned to where the board actually
    # sits on its posts. Both ports are on one short edge of the board.
    usb_cy = D["esp_y"] + P["esp_w"] / 2
    usb_cz = D["esp_board_z"] + P["esp_pcb_t"] / 2
    s = s.cut(box(P["wall"] + 2, P["usb_span_y"], P["usb_span_z"],
                  D["OW"] - P["wall"] - 1,
                  usb_cy - P["usb_span_y"] / 2,
                  usb_cz - P["usb_span_z"] / 2))

    return s.removeSplitter()


# ============================================================================
# Back shell - lid plus integral wedge base
# ============================================================================
def build_back():
    z0 = D["front_depth"]
    lid = box(D["OW"], D["OH"], P["lid_t"], 0, 0, z0)

    # Register lip that drops into the front shell's cavity.
    lip_t = 1.6
    lid = lid.fuse(box(D["OW"] - 2 * P["wall"] - 0.4,
                       D["OH"] - 2 * P["wall"] - 0.4,
                       lip_t,
                       P["wall"] + 0.2, P["wall"] + 0.2, z0 - lip_t))

    # Counterbored clearance holes, screws entering from behind.
    for (bx, by) in boss_positions():
        lid = lid.cut(cyl(P["screw_clear"] / 2, P["lid_t"] + lip_t + 2,
                          bx, by, z0 - lip_t - 1))
        lid = lid.cut(cyl(P["screw_head"] / 2, P["screw_head_d"],
                          bx, by, z0 + P["lid_t"] - P["screw_head_d"]))

    # ESP32 cradle: four posts tall enough for the Dupont housings to hang
    # between them, with corner brackets locating the board.
    ex = D["esp_x"]
    ey = D["esp_y"]
    post = D["esp_post_h"]
    for (px, py) in [(ex, ey), (ex + P["esp_l"], ey),
                     (ex, ey + P["esp_w"]), (ex + P["esp_l"], ey + P["esp_w"])]:
        lid = lid.fuse(cyl(3.0, post, px, py, z0 - post))
    # Corner brackets: short walls just outside the board outline.
    br = 1.8
    for sx in (-1, 1):
        bxp = ex - br if sx < 0 else ex + P["esp_l"]
        lid = lid.fuse(box(br, P["esp_w"], P["esp_pcb_t"] + 1.5,
                           bxp, ey, z0 - post - P["esp_pcb_t"] - 1.5))

    # Cable slot so the display ribbon can pass the board.
    lid = lid.cut(box(P["cable_slot_w"], P["cable_slot_h"], P["lid_t"] + 2,
                      P["wall"] + 4, D["OH"] - P["wall"] - P["cable_slot_h"] - 4,
                      z0 - 1))

    # ---- wedge base ------------------------------------------------------
    # Spans the full depth so the unit rests on the front bottom edge and the
    # rear of the wedge. Hollow with an open bottom to keep print time sane.
    w = box(D["OW"], D["drop"] + 1, D["wedge_z"], 0, -D["drop"], 0)
    inner = box(D["OW"] - 2 * P["foot_wall"], D["drop"] + 1,
                D["wedge_z"] - 2 * P["foot_wall"],
                P["foot_wall"], -D["drop"] - 1, P["foot_wall"])
    w = w.cut(inner)

    # Trim everything below the desk plane: through (Y=0, Z=0), dropping
    # tan(tilt) per unit of Z. That plane IS the printed bottom face.
    #
    # Sign matters: a positive rotation about +X tips the cutter's top face
    # DOWN as Z increases, giving a wedge that is thickest at the back. The
    # negative rotation sloped it the other way and removed the entire wedge.
    cutter = box(D["OW"] + 20, D["drop"] * 3 + 40, D["wedge_z"] + 40,
                 -10, -(D["drop"] * 3 + 40), -20)
    cutter.rotate(App.Vector(0, 0, 0), App.Vector(1, 0, 0), P["tilt_deg"])
    w = w.cut(cutter)

    # Ridge locating the front shell's bottom edge on top of the wedge.
    w = w.cut(box(D["OW"] - 2 * P["foot_wall"], 2.0, D["total_z"],
                  P["foot_wall"], -2.0, 0))

    return lid.fuse(w).removeSplitter()


# ============================================================================
# Fit-check coupon - print this first
# ============================================================================
def build_fit_check():
    """One corner of the bezel/pocket, one ESP32 post pair, one screw boss.

    Everything here is at full scale, so if this drops onto the hardware
    correctly the full print will too.
    """
    cw, ch = 55.0, 45.0
    c = box(cw, ch, P["bezel_t"] + D["pocket_d"])

    # Bezel corner: window edge and module pocket, same geometry as the shell.
    c = c.cut(box(cw, ch, P["bezel_t"] + 2,
                  P["margin"] + (P["disp_w"] - P["active_w"]) / 2
                  + P["bezel_overlap"],
                  P["margin"] + (P["disp_h"] - P["active_h"]) / 2
                  + P["bezel_overlap"], -1))
    c = c.cut(box(cw, ch, D["pocket_d"] + 1,
                  P["margin"], P["margin"], P["bezel_t"]))

    # A screw boss at true inset, to test the M3 pilot.
    bz = P["bezel_t"] + D["pocket_d"]
    c = c.fuse(cyl(P["boss_od"] / 2, 8.0, D["boss_inset"], D["boss_inset"], bz))
    c = c.cut(cyl(P["boss_pilot"] / 2, 9.0, D["boss_inset"], D["boss_inset"], bz))

    # A pair of ESP32 posts at true spacing along the short edge, to check the
    # board width and bracket fit.
    c = c.fuse(cyl(3.0, 8.0, cw - 12.0, 8.0, bz))
    c = c.fuse(cyl(3.0, 8.0, cw - 12.0, 8.0 + P["esp_w"], bz))

    return c.removeSplitter()


# ============================================================================
# Build and export
# ============================================================================
def export(shape, name):
    step = os.path.join(OUT, name + ".step")
    stl = os.path.join(OUT, name + ".stl")
    shape.exportStep(step)
    m = Mesh.Mesh()
    m.addFacets(shape.tessellate(0.05))
    m.write(stl)
    bb = shape.BoundBox
    print("  %-14s %6.1f x %6.1f x %6.1f mm   vol %8.1f cm3   solid=%s"
          % (name, bb.XLength, bb.YLength, bb.ZLength,
             shape.Volume / 1000.0, shape.isValid()))
    return stl


def main():
    print("Enclosure build")
    print("  outer          %.1f x %.1f x %.1f mm (excl. wedge)"
          % (D["OW"], D["OH"], D["total_z"]))
    print("  window         %.1f x %.1f mm" % (D["win_w"], D["win_h"]))
    print("  bezel border   %.1f mm" % ((D["OW"] - D["win_w"]) / 2))
    print("  wedge drop     %.1f mm over %.1f mm depth (%.0f deg)"
          % (D["drop"], D["wedge_z"], P["tilt_deg"]))
    print("")
    export(build_front(), "front-shell")
    export(build_back(), "back-shell")
    export(build_fit_check(), "fit-check")
    print("\nWrote STL + STEP to %s" % OUT)


main()
