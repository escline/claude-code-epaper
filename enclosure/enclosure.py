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
    # Film bonded to the front. Established from the test print: the module sat
    # flush with a 3.2mm pocket, so it is 3.2 thick overall, not the 2.8 first
    # assumed.
    "disp_panel_t": 1.6,
    # Play behind the module, between its back face and the screw bosses. The
    # module is trapped between those and the bezel lip, so this wants to be
    # small - it is the only thing stopping the panel shifting in its pocket.
    "pocket_clear": 0.2,
    "active_w": 84.8,         # published active area
    "active_h": 63.6,
    # Offset of the image from the module's centre, measured front-face borders
    # to the white film: left 8.19, right 8.19, top 2.24, bottom 10.5.
    #   dx = (left - right) / 2 = 0
    #   dy = (bottom - top) / 2 = 4.13, taken as 4.0
    # The big bottom border is the panel's FPC tail, which is why the image sits
    # noticeably high on the board.
    "active_dx": 0.0,
    "active_dy": 4.0,
    "bezel_overlap": 0.5,     # bezel covers this much of the active area/side

    # True: the window is centred in the case and the module is mounted
    # off-centre to suit. The case looks symmetric, which is what anyone
    # actually sees. False: module centred, window offset, visibly lopsided
    # bezel (13.5mm above the image, 21.8mm below).
    "center_window": True,

    # --- ESP32-S3 devkit (clone; measured with calipers) ------------------
    "esp_w": 28.2,            # short edge
    "esp_l": 63.5,            # long edge, USB ports on one short end
    "esp_pcb_t": 1.6,
    # Connector bottom is at 16.5; the extra 3 lets the wires turn out of the
    # housing instead of being crushed against the lid.
    "esp_stack_h": 19.5,
    # Clearance in front of the board for the WROOM module and buttons, which
    # face the display.
    "esp_top_clear": 4.0,
    # The two USB-C ports sit side by side across the board's short edge, so
    # the opening is wide in Y (case height) and shallow in Z (case depth).
    # Ports measure 20.7 outer-to-outer; the rest is so a cable overmold can
    # actually seat rather than bottoming out on the wall.
    "usb_span_y": 26.0,
    "usb_span_z": 11.0,
    "esp_shift_x": 12.0,      # board offset toward the USB wall
    "esp_fit": 0.4,           # clearance around the board in its cradle
    # The board is carried on rails running outboard of its long edges, not on
    # pillars under its corners: the Dupont housings plug onto the corner pins
    # (GND and 3V3) and occupy exactly where corner pillars would stand.
    "esp_rail_w": 3.0,
    # The board is caught under its two SHORT ends, not its long edges. The
    # header plastic runs right to the long edges, so a ledge there bears on
    # the plastic rather than the PCB and seats the board ~2.5mm high - enough
    # to drive the WROOM can into the back of the display. Between the two pin
    # columns the short ends are bare.
    "esp_ledge_x": 3.0,       # how far the shelf reaches under the board's end
    "esp_ledge_w": 14.0,      # span across the middle, clear of the corner pins

    # --- construction ------------------------------------------------------
    "fit": 0.4,               # clearance around the display module
    # Total clearance between the lid's register lip and the front shell's
    # cavity, so 0.4 per side. 0.2 per side is inside FDM tolerance and the
    # halves may simply refuse to seat.
    "shell_fit": 0.8,
    # Floor only - the real bezel border is derived so it always clears the
    # screw bosses, whatever size the inserts make them.
    "margin_min": 9.0,
    "bezel_t": 2.0,           # front face thickness
    "wall": 3.0,
    "lid_t": 2.5,
    # Floor only. Actual cavity depth is derived from the ESP32 stack so the
    # two cannot drift apart - at 16.5 of stack the old fixed 24.0 left the
    # WROOM module about 1mm from the back of the display.
    "cavity_d_min": 20.0,

    # --- screws ------------------------------------------------------------
    # M3 brass heat-set inserts rather than self-tapping into plastic. Tapping
    # a printed boss splits it about as often as it holds, and this case gets
    # opened every time the wiring changes.
    # MEASURE: the knurled outer diameter and length of your inserts.
    "insert_od": 4.6,
    "insert_len": 5.7,
    "insert_hole": 4.0,       # slightly under OD so the brass melts in and grips
    "boss_wall": 2.0,         # plastic around the insert
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
    # Boss and bezel border both follow the insert size, so changing inserts
    # cannot leave a boss overhanging the module pocket.
    d["boss_od"] = p["insert_hole"] + 2 * p["boss_wall"]
    # +2.0 keeps the bosses clear of the pocket wall rather than tangent to it;
    # tangent solids meet on coincident faces and upset the booleans.
    d["margin"] = max(p["margin_min"], d["boss_od"] + 2.0)
    # Mounting the module off-centre costs case size on both sides, since the
    # outside stays symmetric.
    d["OW"] = p["disp_w"] + p["fit"] + 2 * d["margin"] + 2 * abs(p["active_dx"])
    d["OH"] = p["disp_h"] + p["fit"] + 2 * d["margin"] + 2 * abs(p["active_dy"])
    d["cavity_d"] = max(p["cavity_d_min"],
                        p["esp_stack_h"] + p["esp_pcb_t"] + p["esp_top_clear"])
    d["pocket_z"] = p["bezel_t"]
    d["pocket_d"] = d["mod_t"] + p["pocket_clear"]
    d["front_depth"] = p["bezel_t"] + d["pocket_d"] + d["cavity_d"]
    d["cavity_z"] = d["pocket_z"] + d["pocket_d"]
    d["win_w"] = p["active_w"] - 2 * p["bezel_overlap"]
    d["win_h"] = p["active_h"] - 2 * p["bezel_overlap"]
    if p["center_window"]:
        d["win_x"] = (d["OW"] - d["win_w"]) / 2
        d["win_y"] = (d["OH"] - d["win_h"]) / 2
        d["pocket_x"] = ((d["OW"] - (p["disp_w"] + p["fit"])) / 2
                         - p["active_dx"])
        d["pocket_y"] = ((d["OH"] - (p["disp_h"] + p["fit"])) / 2
                         - p["active_dy"])
    else:
        d["win_x"] = (d["OW"] - d["win_w"]) / 2 + p["active_dx"]
        d["win_y"] = (d["OH"] - d["win_h"]) / 2 + p["active_dy"]
        d["pocket_x"] = (d["OW"] - (p["disp_w"] + p["fit"])) / 2
        d["pocket_y"] = (d["OH"] - (p["disp_h"] + p["fit"])) / 2

    # Where the image actually lands, for checking the window sits on it.
    d["active_cx"] = d["pocket_x"] + (p["disp_w"] + p["fit"]) / 2 + p["active_dx"]
    d["active_cy"] = d["pocket_y"] + (p["disp_h"] + p["fit"]) / 2 + p["active_dy"]
    d["boss_inset"] = d["boss_od"] / 2 + 1.5
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
                  D["cavity_d"] + 1,
                  P["wall"], P["wall"], D["cavity_z"]))

    # Screw bosses rise from the back of the module pocket, so they also stop
    # the module lifting out of its pocket.
    # Insert goes in from the open back of the boss, so the bore starts at the
    # rear face and stops short of the module.
    for (bx, by) in boss_positions():
        s = s.fuse(cyl(D["boss_od"] / 2, D["cavity_d"], bx, by, D["cavity_z"]))
        bore = P["insert_len"] + 1.0
        s = s.cut(cyl(P["insert_hole"] / 2, bore, bx, by,
                      D["front_depth"] - bore))

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
    # A perimeter ring, not a plate. As a plate it spanned the whole cavity and
    # stole 1.6mm of the depth the Dupont housings and their wire bend need.
    lip_t = 1.6
    lip_w = 3.0
    sf = P["shell_fit"]
    lip_o = box(D["OW"] - 2 * P["wall"] - sf, D["OH"] - 2 * P["wall"] - sf,
                lip_t, P["wall"] + sf / 2, P["wall"] + sf / 2, z0 - lip_t)
    lip_i = box(D["OW"] - 2 * P["wall"] - sf - 2 * lip_w,
                D["OH"] - 2 * P["wall"] - sf - 2 * lip_w, lip_t + 2,
                P["wall"] + sf / 2 + lip_w, P["wall"] + sf / 2 + lip_w,
                z0 - lip_t - 1)
    lid = lid.fuse(lip_o.cut(lip_i))

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
    # Two rails outboard of the board's long edges. The board drops into the
    # channel between them and lands on a thin ledge catching only the PCB
    # edge; the rails retain it sideways. Everything is outside the board
    # footprint except the ledge, so the Dupont housings hang free.
    rw = P["esp_rail_w"]
    lip_h = P["esp_pcb_t"] + 1.5
    f = P["esp_fit"] / 2
    zf = z0 - post - P["esp_pcb_t"] - 1.5   # front face of the rails

    # Side rails retain the board sideways only - no ledge, so nothing intrudes
    # under the header plastic or the housings plugged into it.
    for sy in (-1, 1):
        inner = (ey - f) if sy < 0 else (ey + P["esp_w"] + f)
        ry = inner - rw if sy < 0 else inner
        lid = lid.fuse(box(P["esp_l"], rw, post + lip_h, ex, ry, zf))

    # Pedestals under the middle of each short end, where the PCB is bare.
    # Full height to the lid - as thin shelves they had nothing beneath them
    # and printed as separate floating solids.
    lw = P["esp_ledge_w"]
    ly = ey + (P["esp_w"] - lw) / 2
    for lx in (ex, ex + P["esp_l"] - P["esp_ledge_x"]):
        lid = lid.fuse(box(P["esp_ledge_x"], lw, post, lx, ly, z0 - post))

    # End stop so the board cannot slide along its length. Only at the far end
    # - the USB ports have to reach the opening at the other.
    lid = lid.fuse(box(2.0, P["esp_w"] + P["esp_fit"] + 2 * rw, post + lip_h,
                       ex - 2.0, ey - f - rw, zf))

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
# Test prints - run these before committing to the full parts
#
# Two separate parts, not one. The display module is 103 x 78.5, so once its
# corner is seated in the bezel rail it covers the whole plate - anything else
# printed alongside collides with it and the test cannot be performed.
# ============================================================================
FIT_H = 50.0


def build_fit_display():
    """Bezel corner, module pocket and one screw boss, at full scale."""
    w = 56.0
    base = P["bezel_t"] + D["pocket_d"]
    m = D["margin"]

    c = box(w, FIT_H, base)

    # Module pocket: an L rail the module's corner nests into.
    c = c.cut(box(w - m, FIT_H - m, D["pocket_d"] + 1, m, m, P["bezel_t"]))

    # Lips are taken from the real shell rather than recomputed, so the two can
    # never disagree.
    #
    # This reproduces the TOP-left corner, where the vertical lip is narrowest
    # (4.2mm) because the image sits high on the module. Testing the bottom
    # corner instead would hide a wrong active_dy behind its 12mm lip.
    lip_x = m + (D["win_x"] - D["pocket_x"])
    lip_y = m + ((D["pocket_y"] + P["disp_h"] + P["fit"])
                 - (D["win_y"] + D["win_h"]))
    c = c.cut(box(w - lip_x, FIT_H - lip_y, P["bezel_t"] + 2, lip_x, lip_y, -1))

    # Boss bored for a heat-set insert, at the true corner inset.
    c = c.fuse(cyl(D["boss_od"] / 2, P["insert_len"] + 3.0,
                   D["boss_inset"], D["boss_inset"], base))
    c = c.cut(cyl(P["insert_hole"] / 2, P["insert_len"] + 1.0,
                  D["boss_inset"], D["boss_inset"],
                  base + 2.0))

    return c.removeSplitter()


def build_fit_cradle():
    """A section of the ESP32 cradle rails, at full height.

    Full height matters: at a token 8mm the headers alone hung lower than the
    supports and the board could not seat, so the part tested nothing. At the
    real height it also covers the one dimension nothing else can - whether the
    Dupont housings, and the wires turning out of them, fit in esp_stack_h.
    """
    w = 40.0
    base = 3.0
    post = D["esp_post_h"]
    rw = P["esp_rail_w"]
    lip_h = P["esp_pcb_t"] + 1.5
    f = P["esp_fit"] / 2

    by0 = (FIT_H - P["esp_w"]) / 2
    c = box(w, FIT_H, base)

    # Side rails: retention only.
    for sy in (-1, 1):
        inner = (by0 - f) if sy < 0 else (by0 + P["esp_w"] + f)
        ry = inner - rw if sy < 0 else inner
        c = c.fuse(box(w, rw, post + lip_h, 0, ry, base))

    # One end pedestal, plus the end stop, as on the real lid.
    lw = P["esp_ledge_w"]
    c = c.fuse(box(P["esp_ledge_x"], lw, post,
                   2.0, by0 + (P["esp_w"] - lw) / 2, base))
    c = c.fuse(box(2.0, P["esp_w"] + P["esp_fit"] + 2 * rw, post + lip_h,
                   0, by0 - f - rw, base))

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
    # More than one solid means some feature is not connected to the body -
    # a peg floating in mid-air, printable only on supports. Cheap to check
    # and it catches an entire class of boolean mistake.
    n = len(shape.Solids)
    flag = "" if n == 1 else "   <-- %d DISCONNECTED PIECES" % n
    print("  %-14s %6.1f x %6.1f x %6.1f mm   vol %8.1f cm3   valid=%s%s"
          % (name, bb.XLength, bb.YLength, bb.ZLength,
             shape.Volume / 1000.0, shape.isValid(), flag))
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
    export(build_fit_display(), "test-display")
    export(build_fit_cradle(), "test-cradle")
    print("\nWrote STL + STEP to %s" % OUT)


main()
