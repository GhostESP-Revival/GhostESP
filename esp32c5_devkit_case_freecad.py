"""Generate a simple editable ESP32-C5 DevKit case in FreeCAD.

Run from FreeCAD:
    File -> Open -> esp32c5_devkit_case_freecad.py, then press F6

This intentionally uses plain Part::Box objects with numeric dimensions.
In FreeCAD, click any part and edit Length/Width/Height/Placement in the Data
tab. The perforated plates (bottom floor, lid top) are a single Part::Feature
each, built by boolean-cutting their cutouts from one solid slab rather than
tiling many small boxes, so there are no seam lines through solid areas.
"""

from __future__ import annotations

import os

import FreeCAD as App
import Part  # noqa: F401

try:
    import Mesh
except ImportError:
    Mesh = None

try:
    import FreeCADGui as Gui
except ImportError:
    Gui = None


# Board measurements, mm.
TOTAL_LENGTH = 60.0
BOARD_WIDTH = 25.2
TOTAL_DEPTH_WITH_GPIO = 15.2
PCB_THICKNESS = 1.6

ANTENNA_OVERHANG = 5.8
ANTENNA_OFFSET_FROM_LEFT_EDGE = 3.4
ANTENNA_WIDTH = 17.8

HEATSPREADER_LENGTH = 19.6
HEATSPREADER_WIDTH = 15.8
HEATSPREADER_DEPTH = 2.5
HEATSPREADER_OFFSET_FROM_ANTENNA_EDGE = 2.4
HEATSPREADER_VISIBLE_PCB_SIDE = (BOARD_WIDTH - HEATSPREADER_WIDTH) / 2.0

GPIO_ROW_LENGTH = 40.5
GPIO_NON_ANTENNA_OVERHANG = 12.6
GPIO_ROW_WIDTH = 2.54
GPIO_ROW_EDGE_INSET = 0.8
GPIO_BODY_LENGTH = 40.7
GPIO_BODY_WIDTH = 5.5

USB_C_WIDTH = 9.0
USB_C_GAP = 5.3
# How far the USB-C receptacle shells extend inward from the wall opening.
USB_CONNECTOR_DEPTH = 6.5
USB_TOP_OPENING_CLEARANCE = 0.8
USB_PCB_HOOK_WIDTH = 2.0
USB_PCB_HOOK_STEM_THICKNESS = 0.5
USB_PCB_HOOK_TOOTH_LENGTH = 1.0
USB_PCB_HOOK_TOOTH_HEIGHT = 0.35
USB_PCB_HOOK_EDGE_CLEARANCE = 0.1
USB_PCB_HOOK_TOOTH_OVERLAP = 0.15
USB_PCB_HOOK_UNDER_PCB_CLEARANCE = 0.05
USB_PCB_HOOK_FLOOR_CLEARANCE = 0.25
PCB_SIDE_CLIP_LENGTH = 3.0
PCB_SIDE_CLIP_USB_OFFSET = 1.0
PCB_SIDE_CLIP_OVERLAP = 0.45
PCB_SIDE_CLIP_HEIGHT = 0.35
PCB_SIDE_CLIP_TOP_CLEARANCE = 0.05
PCB_SIDE_CLIP_STEM_CLEARANCE = 0.15


# Case tuning, mm.
CASE_CLEARANCE = 0.6
WALL_THICKNESS = 1.6
FLOOR_THICKNESS = 1.2
LID_THICKNESS = 1.4
LID_LIP_HEIGHT = 2.0
LID_FIT_CLEARANCE = 0.35
TOP_CLEARANCE = 1.0

USB_OPENING_CLEARANCE = 0.8
ANTENNA_OPENING_CLEARANCE = 0.8
HEATSPREADER_OPENING_CLEARANCE = 0.3
BUTTON_OPENING_CLEARANCE = 0.6
JUMPER_OPENING_CLEARANCE = 0.6
GPIO_TOP_SLOT_END_CLEARANCE = 0.25
GPIO_TOP_SLOT_OUTER_CLEARANCE = 0.45
GPIO_TOP_SLOT_INNER_CLEARANCE = 0.10
LED_OPENING_CLEARANCE = 0.4
TOP_SOLDER_CLEARANCE = 1.0
GPIO_BOTTOM_SLOT_CLEARANCE = 0.8
BOTTOM_GPIO_END_CLEARANCE = 0.25

# Any solid remnant thinner than this is unprintable, so it is merged into the
# adjacent opening instead of being generated as a sliver of material.
MIN_PRINTABLE_WALL = 0.8

# Top IO cutouts use PCB-local measurements; bottom is the USB/non-antenna end.
BUTTON_BOTTOM_OFFSET = 11.0
BUTTON_LENGTH = 4.0
BUTTON_HEIGHT = 3.0
BUTTON_GAP = 5.7

JUMPER_BOTTOM_OFFSET = 23.0
JUMPER_RIGHT_EDGE_OFFSET = 16.9
JUMPER_HEATSPREADER_GAP = 2.6
JUMPER_LEFT_EDGE_OFFSET = 3.5

BOTTOM_GPIO_END_FROM_BOTTOM = 24.1
BOTTOM_GPIO_END_FROM_TOP = 31.2
BOTTOM_GPIO_END_WIDTH = 4.0
BOTTOM_GPIO_END_HEIGHT = 2.0
BOTTOM_GPIO_END_ANTENNA_SHIFT = 1.0

LED_TOP_FROM_BOTTOM = 20.0
LED_RIGHT_FROM_LEFT = 8.5
LED_LEFT_FROM_RIGHT = 20.6
LED_BOTTOM_FROM_TOP = 37.5

CLIP_LENGTH = 6.0
CLIP_WIDTH = 1.2
CLIP_HEIGHT = 1.0
CLIP_END_INSET = 4.0
CLIP_CLEARANCE = 0.2

LID_DISPLAY_GAP = 8.0
EXPORT_STL = True


MAIN_PCB_LENGTH = TOTAL_LENGTH - ANTENNA_OVERHANG
GPIO_HEIGHT = TOTAL_DEPTH_WITH_GPIO - PCB_THICKNESS - HEATSPREADER_DEPTH

INTERNAL_LENGTH = TOTAL_LENGTH + (CASE_CLEARANCE * 2.0)
INTERNAL_WIDTH = BOARD_WIDTH + (CASE_CLEARANCE * 2.0)
INTERNAL_HEIGHT = PCB_THICKNESS + TOP_CLEARANCE

EXTERIOR_LENGTH = INTERNAL_LENGTH + (WALL_THICKNESS * 2.0)
EXTERIOR_WIDTH = INTERNAL_WIDTH + (WALL_THICKNESS * 2.0)
TRAY_HEIGHT = FLOOR_THICKNESS + INTERNAL_HEIGHT
WALL_HEIGHT = TRAY_HEIGHT - FLOOR_THICKNESS

BOARD_X = WALL_THICKNESS + CASE_CLEARANCE
BOARD_Y = WALL_THICKNESS + CASE_CLEARANCE
BOARD_Z = FLOOR_THICKNESS
BOARD_TOP_Z = BOARD_Z + PCB_THICKNESS


def make_box(doc, name, length, width, height, x, y, z, color, transparency=0):
    if length <= 0 or width <= 0 or height <= 0:
        return None

    obj = doc.addObject("Part::Box", name)
    obj.Label = name.replace("_", " ")
    obj.Length = length
    obj.Width = width
    obj.Height = height
    obj.Placement = App.Placement(App.Vector(x, y, z), App.Rotation())

    if Gui is not None:
        obj.ViewObject.ShapeColor = color
        obj.ViewObject.Transparency = transparency

    return obj


def output_directory():
    out_dir = r"I:\GhostESP2\Ghost_ESP\models"
    os.makedirs(out_dir, exist_ok=True)
    return out_dir


def make_print_solid(doc, name, objects, color):
    solid = doc.addObject("Part::Feature", name)
    solid.Label = name.replace("_", " ")

    objects = [obj for obj in objects if obj is not None]
    shapes = [obj.Shape for obj in objects]
    shape = shapes[0].copy()
    for next_shape in shapes[1:]:
        shape = shape.fuse(next_shape)

    solid.Shape = shape.removeSplitter()

    if Gui is not None:
        solid.ViewObject.ShapeColor = color
        for obj in objects:
            obj.ViewObject.Visibility = False

    return solid


def hide_objects(objects):
    if Gui is None:
        return

    for obj in objects:
        if hasattr(obj, "ViewObject"):
            obj.ViewObject.Visibility = False


def export_print_stls(bottom_case, lid):
    if not EXPORT_STL or Mesh is None:
        return

    out_dir = output_directory()
    Mesh.export([bottom_case], os.path.join(out_dir, "bottom_case.stl"))
    Mesh.export([lid], os.path.join(out_dir, "lid.stl"))


def expanded_window(name, x, y, length, width, clearance):
    return {
        "name": name,
        "x1": x - clearance,
        "x2": x + length + clearance,
        "y1": y - clearance,
        "y2": y + width + clearance,
    }


def bridge_close_windows(windows, min_gap):
    """Return small extra cutouts that fill any gap narrower than min_gap
    between two windows that otherwise run alongside each other.

    A gap that thin can't print as a wall, so it's cut away too instead of
    being left as a hairline remnant. Only the gap itself is bridged (not
    the two windows' full bounding box), so unrelated windows further away
    are never pulled into the same opening.
    """
    bridges = []
    for i, a in enumerate(windows):
        for b in windows[i + 1:]:
            x_lo, x_hi = max(a["x1"], b["x1"]), min(a["x2"], b["x2"])
            y_lo, y_hi = max(a["y1"], b["y1"]), min(a["y2"], b["y2"])
            x_gap = max(a["x1"], b["x1"]) - min(a["x2"], b["x2"])
            y_gap = max(a["y1"], b["y1"]) - min(a["y2"], b["y2"])

            if x_hi > x_lo and 0 < y_gap < min_gap:
                bridges.append({"x1": x_lo, "x2": x_hi, "y1": min(a["y2"], b["y2"]), "y2": max(a["y1"], b["y1"])})
            if y_hi > y_lo and 0 < x_gap < min_gap:
                bridges.append({"x1": min(a["x2"], b["x2"]), "x2": max(a["x1"], b["x1"]), "y1": y_lo, "y2": y_hi})

    return bridges


def make_plate_with_windows(doc, name, length, width, height, x, y, z, windows, color):
    plate = Part.makeBox(length, width, height, App.Vector(x, y, z))

    cutters = []
    for window in windows + bridge_close_windows(windows, MIN_PRINTABLE_WALL):
        x1 = max(x, window["x1"])
        x2 = min(x + length, window["x2"])
        y1 = max(y, window["y1"])
        y2 = min(y + width, window["y2"])
        if x2 <= x1 or y2 <= y1:
            continue

        cutters.append(Part.makeBox(x2 - x1, y2 - y1, height, App.Vector(x1, y1, z)))

    if cutters:
        cutter = cutters[0]
        for next_cutter in cutters[1:]:
            cutter = cutter.fuse(next_cutter)
        plate = plate.cut(cutter)

    obj = doc.addObject("Part::Feature", name)
    obj.Label = name.replace("_", " ")
    obj.Shape = plate.removeSplitter()

    if Gui is not None:
        obj.ViewObject.ShapeColor = color

    return obj


def make_lid_top_with_windows(doc, lid_y, windows, color):
    return make_plate_with_windows(doc, "Lid_Top_Plate", EXTERIOR_LENGTH, EXTERIOR_WIDTH, LID_THICKNESS, 0, lid_y, 0, windows, color)


def make_snap_ramp(doc, name, length, width, height, x, y, z, direction, color):
    """Make a wedge clip extruded along X.

    direction=-1 creates a front-side clip that protrudes toward lower Y.
    direction=1 creates a back-side clip that protrudes toward higher Y.

    The lid is modeled inside-up for printing, so the ramp profile is mirrored
    in Z: after assembly, the clip has a flat upper catch face and beveled lower
    insertion face.
    """
    if direction < 0:
        inner_y = y + width
        outer_y = y
    else:
        inner_y = y
        outer_y = y + width

    points = [
        App.Vector(x, inner_y, z + height),
        App.Vector(x, inner_y, z),
        App.Vector(x, outer_y, z),
        App.Vector(x, inner_y, z + height),
    ]
    face = Part.Face(Part.makePolygon(points))
    shape = face.extrude(App.Vector(length, 0, 0))

    obj = doc.addObject("Part::Feature", name)
    obj.Shape = shape
    obj.Label = name.replace("_", " ")

    if Gui is not None:
        obj.ViewObject.ShapeColor = color

    return obj


def make_pcb_side_retainer(doc, name, x, side, color):
    tab_z = BOARD_TOP_Z + PCB_SIDE_CLIP_TOP_CLEARANCE
    tab_w = CASE_CLEARANCE + PCB_SIDE_CLIP_OVERLAP
    stem_w = CASE_CLEARANCE - PCB_SIDE_CLIP_STEM_CLEARANCE

    if side == "front":
        tab_y = WALL_THICKNESS
        stem_y = WALL_THICKNESS
    else:
        tab_y = BOARD_Y + BOARD_WIDTH - PCB_SIDE_CLIP_OVERLAP
        stem_y = BOARD_Y + BOARD_WIDTH + PCB_SIDE_CLIP_STEM_CLEARANCE

    return [
        make_box(doc, f"{name}_Stem", PCB_SIDE_CLIP_LENGTH, stem_w, tab_z + PCB_SIDE_CLIP_HEIGHT - FLOOR_THICKNESS, x, stem_y, FLOOR_THICKNESS, color),
        make_box(doc, f"{name}_Top_Tab", PCB_SIDE_CLIP_LENGTH, tab_w, PCB_SIDE_CLIP_HEIGHT, x, tab_y, tab_z, color),
    ]


def snap_clip_positions():
    """Return (label, x, length) for each snap clip.

    Each clip must sit fully on top of its matching lid lip segment so the
    ramp fuses to the lip instead of hanging off the end unsupported.
    """
    solder_end = BOARD_X + GPIO_NON_ANTENNA_OVERHANG + GPIO_ROW_LENGTH + TOP_SOLDER_CLEARANCE

    max_antenna_x = EXTERIOR_LENGTH - WALL_THICKNESS - LID_FIT_CLEARANCE - CLIP_LENGTH
    antenna_x = min(max_antenna_x, max(EXTERIOR_LENGTH - CLIP_END_INSET - CLIP_LENGTH, solder_end))

    return (("Antenna", antenna_x, CLIP_LENGTH),)


def make_side_rail_with_snap_windows(doc, name, y, color):
    clip_z = LID_THICKNESS + LID_LIP_HEIGHT - CLIP_HEIGHT
    # The lid is modeled inside-up for printing; mirror its clip Z range into the assembled tray wall.
    window_bottom = TRAY_HEIGHT + LID_THICKNESS - (clip_z + CLIP_HEIGHT) - CLIP_CLEARANCE
    window_top = TRAY_HEIGHT + LID_THICKNESS - clip_z + CLIP_CLEARANCE
    window_bottom = max(FLOOR_THICKNESS, window_bottom)
    window_top = min(TRAY_HEIGHT, window_top)

    lower_h = window_bottom - FLOOR_THICKNESS
    upper_h = TRAY_HEIGHT - window_top
    window_h = window_top - window_bottom

    parts = [
        make_box(doc, f"{name}_Lower_Frame", EXTERIOR_LENGTH, WALL_THICKNESS, lower_h, 0, y, FLOOR_THICKNESS, color),
        make_box(doc, f"{name}_Upper_Catch_Frame", EXTERIOR_LENGTH, WALL_THICKNESS, upper_h, 0, y, window_top, color),
    ]

    snap_windows = []
    for label, x, length in snap_clip_positions():
        window_x = max(0, x - CLIP_CLEARANCE)
        window_end = min(EXTERIOR_LENGTH, x + length + CLIP_CLEARANCE)
        snap_windows.append((label, window_x, window_end))

    segment_start = 0
    for label, window_x, window_end in snap_windows:
        parts.append(
            make_box(
                doc,
                f"{name}_Middle_Frame_Before_{label}",
                window_x - segment_start,
                WALL_THICKNESS,
                window_h,
                segment_start,
                y,
                window_bottom,
                color,
            )
        )
        segment_start = window_end

    parts.append(
        make_box(
            doc,
            f"{name}_Middle_Frame_After_Snaps",
            EXTERIOR_LENGTH - segment_start,
            WALL_THICKNESS,
            window_h,
            segment_start,
            y,
            window_bottom,
            color,
        )
    )

    return parts


def add_note(doc):
    note = doc.addObject("App::Annotation", "Model_Notes")
    note.LabelText = ["ESP32C5-Devkit"]
    note.Position = App.Vector(0, -7, TRAY_HEIGHT + 3)


def add_bottom_floor_with_gpio_slots(doc, color):
    row_x = BOARD_X + GPIO_NON_ANTENNA_OVERHANG
    row_center_x = row_x + (GPIO_ROW_LENGTH / 2.0)
    slot_x = row_center_x - (GPIO_BODY_LENGTH / 2.0) - GPIO_BOTTOM_SLOT_CLEARANCE
    slot_len = GPIO_BODY_LENGTH + (GPIO_BOTTOM_SLOT_CLEARANCE * 2.0)

    left_row_center_y = BOARD_Y + GPIO_ROW_EDGE_INSET + (GPIO_ROW_WIDTH / 2.0)
    right_row_center_y = BOARD_Y + BOARD_WIDTH - GPIO_ROW_EDGE_INSET - (GPIO_ROW_WIDTH / 2.0)
    left_slot_y = left_row_center_y - (GPIO_BODY_WIDTH / 2.0) - GPIO_BOTTOM_SLOT_CLEARANCE
    right_slot_y = right_row_center_y - (GPIO_BODY_WIDTH / 2.0) - GPIO_BOTTOM_SLOT_CLEARANCE
    slot_w = GPIO_BODY_WIDTH + (GPIO_BOTTOM_SLOT_CLEARANCE * 2.0)
    left_slot_end_y = left_slot_y + slot_w
    right_slot_end_y = right_slot_y + slot_w

    # Keep bottom GPIO cutouts inside the tray walls so the floor edge lines up with the side rails.
    left_slot_y = max(WALL_THICKNESS, left_slot_y)
    left_slot_end_y = min(EXTERIOR_WIDTH - WALL_THICKNESS, left_slot_end_y)
    right_slot_y = max(WALL_THICKNESS, right_slot_y)
    right_slot_end_y = min(EXTERIOR_WIDTH - WALL_THICKNESS, right_slot_end_y)

    bottom_gpio_end_span_x = BOARD_X + BOTTOM_GPIO_END_FROM_BOTTOM
    bottom_gpio_end_span_len = TOTAL_LENGTH - BOTTOM_GPIO_END_FROM_BOTTOM - BOTTOM_GPIO_END_FROM_TOP
    jumper_w = BOARD_WIDTH - JUMPER_LEFT_EDGE_OFFSET - JUMPER_RIGHT_EDGE_OFFSET
    bottom_jumper_y = BOARD_Y + BOARD_WIDTH - JUMPER_LEFT_EDGE_OFFSET - jumper_w
    bottom_gpio_end_gap = bottom_gpio_end_span_len - (BOTTOM_GPIO_END_HEIGHT * 2.0)
    bottom_gpio_end_x = bottom_gpio_end_span_x + ((bottom_gpio_end_span_len - BOTTOM_GPIO_END_HEIGHT) / 2.0) + BOTTOM_GPIO_END_ANTENNA_SHIFT
    bottom_gpio_end_pair_w = (BOTTOM_GPIO_END_WIDTH * 2.0) + bottom_gpio_end_gap
    # Bottom-side features are mirrored across the PCB width relative to top-side jumper measurements.
    bottom_gpio_end_y = bottom_jumper_y + ((jumper_w - bottom_gpio_end_pair_w) / 2.0)

    usb_pair_w = (USB_C_WIDTH * 2.0) + USB_C_GAP
    usb_center_y = BOARD_Y + ((BOARD_WIDTH - usb_pair_w) / 2.0) + USB_C_WIDTH + (USB_C_GAP / 2.0)
    hook_tooth_x = BOARD_X - USB_PCB_HOOK_TOOTH_OVERLAP
    hook_stem_x = BOARD_X - USB_PCB_HOOK_STEM_THICKNESS - USB_PCB_HOOK_EDGE_CLEARANCE

    floor_windows = [
        {"name": "GPIO_Left_Body", "x1": slot_x, "x2": slot_x + slot_len, "y1": left_slot_y, "y2": left_slot_end_y},
        {"name": "GPIO_Right_Body", "x1": slot_x, "x2": slot_x + slot_len, "y1": right_slot_y, "y2": right_slot_end_y},
        {
            "name": "USB_PCB_Hook_Clearance",
            "x1": hook_stem_x - USB_PCB_HOOK_FLOOR_CLEARANCE,
            "x2": hook_tooth_x + USB_PCB_HOOK_TOOTH_LENGTH + USB_PCB_HOOK_FLOOR_CLEARANCE,
            "y1": usb_center_y - (USB_PCB_HOOK_WIDTH / 2.0) - USB_PCB_HOOK_FLOOR_CLEARANCE,
            "y2": usb_center_y + (USB_PCB_HOOK_WIDTH / 2.0) + USB_PCB_HOOK_FLOOR_CLEARANCE,
        },
        expanded_window(
            "Bottom_GPIO_End_1",
            bottom_gpio_end_x,
            bottom_gpio_end_y,
            BOTTOM_GPIO_END_HEIGHT,
            BOTTOM_GPIO_END_WIDTH,
            BOTTOM_GPIO_END_CLEARANCE,
        ),
        expanded_window(
            "Bottom_GPIO_End_2",
            bottom_gpio_end_x,
            bottom_gpio_end_y + BOTTOM_GPIO_END_WIDTH + bottom_gpio_end_gap,
            BOTTOM_GPIO_END_HEIGHT,
            BOTTOM_GPIO_END_WIDTH,
            BOTTOM_GPIO_END_CLEARANCE,
        ),
    ]
    return make_plate_with_windows(doc, "Bottom_Plate", EXTERIOR_LENGTH, EXTERIOR_WIDTH, FLOOR_THICKNESS, 0, 0, 0, floor_windows, color)


def add_lower_shell(doc, color):
    parts = make_side_rail_with_snap_windows(doc, "Left_Side_Rail", 0, color)
    parts += make_side_rail_with_snap_windows(doc, "Right_Side_Rail", EXTERIOR_WIDTH - WALL_THICKNESS, color)
    side_clip_x = BOARD_X + USB_CONNECTOR_DEPTH + PCB_SIDE_CLIP_USB_OFFSET
    parts += make_pcb_side_retainer(doc, "PCB_Side_Retainer_Front_USB", side_clip_x, "front", color)
    parts += make_pcb_side_retainer(doc, "PCB_Side_Retainer_Back_USB", side_clip_x, "back", color)

    usb_pair_w = (USB_C_WIDTH * 2.0) + USB_C_GAP
    usb_y = BOARD_Y + ((BOARD_WIDTH - usb_pair_w) / 2.0)
    usb1_start = usb_y - USB_OPENING_CLEARANCE
    usb1_end = usb_y + USB_C_WIDTH + USB_OPENING_CLEARANCE
    usb2_start = usb_y + USB_C_WIDTH + USB_C_GAP - USB_OPENING_CLEARANCE
    usb2_end = usb_y + (USB_C_WIDTH * 2.0) + USB_C_GAP + USB_OPENING_CLEARANCE

    parts.append(make_box(doc, "USB_End_Wall_Left", WALL_THICKNESS, usb1_start, WALL_HEIGHT, 0, 0, FLOOR_THICKNESS, color))
    parts.append(make_box(doc, "USB_End_Wall_Center", WALL_THICKNESS, usb2_start - usb1_end, WALL_HEIGHT, 0, usb1_end, FLOOR_THICKNESS, color))
    parts.append(make_box(doc, "USB_End_Wall_Right", WALL_THICKNESS, EXTERIOR_WIDTH - usb2_end, WALL_HEIGHT, 0, usb2_end, FLOOR_THICKNESS, color))

    antenna_y = BOARD_Y + ANTENNA_OFFSET_FROM_LEFT_EDGE
    antenna_open_start = antenna_y - ANTENNA_OPENING_CLEARANCE
    antenna_open_end = antenna_y + ANTENNA_WIDTH + ANTENNA_OPENING_CLEARANCE
    antenna_wall_x = EXTERIOR_LENGTH - WALL_THICKNESS

    parts.append(make_box(doc, "Antenna_End_Wall_Left", WALL_THICKNESS, antenna_open_start, WALL_HEIGHT, antenna_wall_x, 0, FLOOR_THICKNESS, color))
    parts.append(
        make_box(
            doc,
            "Antenna_End_Wall_Right",
            WALL_THICKNESS,
            EXTERIOR_WIDTH - antenna_open_end,
            WALL_HEIGHT,
            antenna_wall_x,
            antenna_open_end,
            FLOOR_THICKNESS,
            color,
        )
    )

    return parts


def add_lid(doc, color):
    lid_y = EXTERIOR_WIDTH + LID_DISPLAY_GAP

    heat_x = BOARD_X + MAIN_PCB_LENGTH - HEATSPREADER_OFFSET_FROM_ANTENNA_EDGE - HEATSPREADER_LENGTH
    heat_y = lid_y + BOARD_Y + HEATSPREADER_VISIBLE_PCB_SIDE
    window_x = heat_x - HEATSPREADER_OPENING_CLEARANCE
    window_y = heat_y - HEATSPREADER_OPENING_CLEARANCE
    window_len = HEATSPREADER_LENGTH + (HEATSPREADER_OPENING_CLEARANCE * 2.0)
    window_w = HEATSPREADER_WIDTH + (HEATSPREADER_OPENING_CLEARANCE * 2.0)

    button_pair_w = (BUTTON_LENGTH * 2.0) + BUTTON_GAP
    button_x = BOARD_X + BUTTON_BOTTOM_OFFSET
    button_y = lid_y + BOARD_Y + ((BOARD_WIDTH - button_pair_w) / 2.0)

    jumper_x = BOARD_X + JUMPER_BOTTOM_OFFSET
    jumper_end_x = heat_x - JUMPER_HEATSPREADER_GAP
    jumper_y = lid_y + BOARD_Y + JUMPER_LEFT_EDGE_OFFSET
    jumper_w = BOARD_WIDTH - JUMPER_LEFT_EDGE_OFFSET - JUMPER_RIGHT_EDGE_OFFSET

    # Top GPIO slots clear the protruding pin rows; the bottom slots clear the wider plastic bodies.
    gpio_x = BOARD_X + GPIO_NON_ANTENNA_OVERHANG
    left_gpio_y = lid_y + BOARD_Y + GPIO_ROW_EDGE_INSET
    right_gpio_y = lid_y + BOARD_Y + BOARD_WIDTH - GPIO_ROW_EDGE_INSET - GPIO_ROW_WIDTH

    led_x = BOARD_X + LED_TOP_FROM_BOTTOM
    led_len = TOTAL_LENGTH - LED_BOTTOM_FROM_TOP - LED_TOP_FROM_BOTTOM
    led_y = lid_y + BOARD_Y + BOARD_WIDTH - LED_LEFT_FROM_RIGHT
    led_w = LED_RIGHT_FROM_LEFT - (BOARD_WIDTH - LED_LEFT_FROM_RIGHT)

    usb_pair_w = (USB_C_WIDTH * 2.0) + USB_C_GAP
    usb_y = lid_y + BOARD_Y + ((BOARD_WIDTH - usb_pair_w) / 2.0)
    usb_cutout_end_x = BOARD_X + USB_CONNECTOR_DEPTH + USB_TOP_OPENING_CLEARANCE

    top_windows = [
        {
            "name": "USB_C_Left_Clearance",
            "x1": 0,
            "x2": usb_cutout_end_x,
            "y1": usb_y - USB_TOP_OPENING_CLEARANCE,
            "y2": usb_y + USB_C_WIDTH + USB_TOP_OPENING_CLEARANCE,
        },
        {
            "name": "USB_C_Right_Clearance",
            "x1": 0,
            "x2": usb_cutout_end_x,
            "y1": usb_y + USB_C_WIDTH + USB_C_GAP - USB_TOP_OPENING_CLEARANCE,
            "y2": usb_y + (USB_C_WIDTH * 2.0) + USB_C_GAP + USB_TOP_OPENING_CLEARANCE,
        },
        {"name": "Heatspreader", "x1": window_x, "x2": window_x + window_len, "y1": window_y, "y2": window_y + window_w},
        expanded_window("Boot_Button", button_x, button_y, BUTTON_HEIGHT, BUTTON_LENGTH, BUTTON_OPENING_CLEARANCE),
        expanded_window("Reset_Button", button_x, button_y + BUTTON_LENGTH + BUTTON_GAP, BUTTON_HEIGHT, BUTTON_LENGTH, BUTTON_OPENING_CLEARANCE),
        {
            "name": "Jumper",
            "x1": jumper_x - JUMPER_OPENING_CLEARANCE,
            "x2": jumper_end_x + JUMPER_OPENING_CLEARANCE,
            "y1": jumper_y - JUMPER_OPENING_CLEARANCE,
            "y2": jumper_y + jumper_w + JUMPER_OPENING_CLEARANCE,
        },
        expanded_window("Status_LED", led_x, led_y, led_len, led_w, LED_OPENING_CLEARANCE),
        {
            "name": "GPIO_Left_Row",
            "x1": gpio_x - GPIO_TOP_SLOT_END_CLEARANCE,
            "x2": gpio_x + GPIO_ROW_LENGTH + GPIO_TOP_SLOT_END_CLEARANCE,
            "y1": left_gpio_y - GPIO_TOP_SLOT_OUTER_CLEARANCE,
            "y2": left_gpio_y + GPIO_ROW_WIDTH + GPIO_TOP_SLOT_INNER_CLEARANCE,
        },
        {
            "name": "GPIO_Right_Row",
            "x1": gpio_x - GPIO_TOP_SLOT_END_CLEARANCE,
            "x2": gpio_x + GPIO_ROW_LENGTH + GPIO_TOP_SLOT_END_CLEARANCE,
            "y1": right_gpio_y - GPIO_TOP_SLOT_INNER_CLEARANCE,
            "y2": right_gpio_y + GPIO_ROW_WIDTH + GPIO_TOP_SLOT_OUTER_CLEARANCE,
        },
    ]
    parts = [make_lid_top_with_windows(doc, lid_y, top_windows, color)]

    lip_x = WALL_THICKNESS + LID_FIT_CLEARANCE
    lip_y = lid_y + WALL_THICKNESS + LID_FIT_CLEARANCE
    lip_len = EXTERIOR_LENGTH - (lip_x * 2.0)
    lip_w = EXTERIOR_WIDTH - ((WALL_THICKNESS + LID_FIT_CLEARANCE) * 2.0)
    solder_end = BOARD_X + GPIO_NON_ANTENNA_OVERHANG + GPIO_ROW_LENGTH + TOP_SOLDER_CLEARANCE

    parts.append(make_box(doc, "Lid_Front_Lip_Antenna_End", lip_x + lip_len - solder_end, WALL_THICKNESS, LID_LIP_HEIGHT, solder_end, lip_y, LID_THICKNESS, color))
    parts.append(
        make_box(
            doc,
            "Lid_Back_Lip_Antenna_End",
            lip_x + lip_len - solder_end,
            WALL_THICKNESS,
            LID_LIP_HEIGHT,
            solder_end,
            lip_y + lip_w - WALL_THICKNESS,
            LID_THICKNESS,
            color,
        )
    )
    usb1_start = usb_y - USB_OPENING_CLEARANCE
    usb1_end = usb_y + USB_C_WIDTH + USB_OPENING_CLEARANCE
    usb2_start = usb_y + USB_C_WIDTH + USB_C_GAP - USB_OPENING_CLEARANCE
    usb2_end = usb_y + (USB_C_WIDTH * 2.0) + USB_C_GAP + USB_OPENING_CLEARANCE

    antenna_y = lid_y + BOARD_Y + ANTENNA_OFFSET_FROM_LEFT_EDGE
    antenna_open_start = antenna_y - ANTENNA_OPENING_CLEARANCE
    antenna_open_end = antenna_y + ANTENNA_WIDTH + ANTENNA_OPENING_CLEARANCE
    antenna_lip_x = lip_x + lip_len - WALL_THICKNESS

    parts.append(make_box(doc, "Lid_USB_End_Lip_Left", WALL_THICKNESS, usb1_start - lip_y, LID_LIP_HEIGHT, lip_x, lip_y, LID_THICKNESS, color))
    parts.append(make_box(doc, "Lid_USB_End_Lip_Center", WALL_THICKNESS, usb2_start - usb1_end, LID_LIP_HEIGHT, lip_x, usb1_end, LID_THICKNESS, color))
    parts.append(make_box(doc, "Lid_USB_End_Lip_Right", WALL_THICKNESS, lip_y + lip_w - usb2_end, LID_LIP_HEIGHT, lip_x, usb2_end, LID_THICKNESS, color))

    hook_center_y = usb_y + USB_C_WIDTH + (USB_C_GAP / 2.0)
    hook_y = hook_center_y - (USB_PCB_HOOK_WIDTH / 2.0)
    hook_stem_x = BOARD_X - USB_PCB_HOOK_STEM_THICKNESS - USB_PCB_HOOK_EDGE_CLEARANCE
    hook_tooth_x = BOARD_X - USB_PCB_HOOK_TOOTH_OVERLAP
    hook_tooth_z = TRAY_HEIGHT + LID_THICKNESS - (BOARD_Z - USB_PCB_HOOK_UNDER_PCB_CLEARANCE)
    hook_stem_h = hook_tooth_z + USB_PCB_HOOK_TOOTH_HEIGHT - LID_THICKNESS
    parts.append(make_box(doc, "Lid_USB_PCB_Hook_Stem", USB_PCB_HOOK_STEM_THICKNESS, USB_PCB_HOOK_WIDTH, hook_stem_h, hook_stem_x, hook_y, LID_THICKNESS, color))
    parts.append(make_box(doc, "Lid_USB_PCB_Hook_Tooth", USB_PCB_HOOK_TOOTH_LENGTH, USB_PCB_HOOK_WIDTH, USB_PCB_HOOK_TOOTH_HEIGHT, hook_tooth_x, hook_y, hook_tooth_z, color))

    parts.append(make_box(doc, "Lid_Antenna_End_Lip_Left", WALL_THICKNESS, antenna_open_start - lip_y, LID_LIP_HEIGHT, antenna_lip_x, lip_y, LID_THICKNESS, color))
    parts.append(make_box(doc, "Lid_Antenna_End_Lip_Right", WALL_THICKNESS, lip_y + lip_w - antenna_open_end, LID_LIP_HEIGHT, antenna_lip_x, antenna_open_end, LID_THICKNESS, color))

    clip_z = LID_THICKNESS + LID_LIP_HEIGHT - CLIP_HEIGHT
    front_tab_y = lip_y - CLIP_WIDTH
    back_tab_y = lip_y + lip_w
    for label, x, length in snap_clip_positions():
        parts.append(make_snap_ramp(doc, f"Lid_Snap_Ramp_{label}_Front", length, CLIP_WIDTH, CLIP_HEIGHT, x, front_tab_y, clip_z, -1, color))
        parts.append(make_snap_ramp(doc, f"Lid_Snap_Ramp_{label}_Back", length, CLIP_WIDTH, CLIP_HEIGHT, x, back_tab_y, clip_z, 1, color))

    return parts


def add_board_reference(doc):
    make_box(doc, "PCB_Reference", TOTAL_LENGTH, BOARD_WIDTH, PCB_THICKNESS, BOARD_X, BOARD_Y, BOARD_Z, (0.0, 0.0, 0.0), transparency=65)
    make_box(
        doc,
        "Antenna_Keepout",
        ANTENNA_OVERHANG,
        ANTENNA_WIDTH,
        PCB_THICKNESS,
        BOARD_X + TOTAL_LENGTH - ANTENNA_OVERHANG,
        BOARD_Y + ANTENNA_OFFSET_FROM_LEFT_EDGE,
        BOARD_Z,
        (0.92, 0.68, 0.2),
        transparency=45,
    )
    make_box(
        doc,
        "Heatspreader_Keepout",
        HEATSPREADER_LENGTH,
        HEATSPREADER_WIDTH,
        HEATSPREADER_DEPTH,
        BOARD_X + MAIN_PCB_LENGTH - HEATSPREADER_OFFSET_FROM_ANTENNA_EDGE - HEATSPREADER_LENGTH,
        BOARD_Y + HEATSPREADER_VISIBLE_PCB_SIDE,
        BOARD_TOP_Z,
        (0.72, 0.72, 0.72),
        transparency=35,
    )
    make_box(
        doc,
        "GPIO_Left_Bottom_Clearance",
        GPIO_ROW_LENGTH,
        GPIO_ROW_WIDTH,
        GPIO_HEIGHT,
        BOARD_X + GPIO_NON_ANTENNA_OVERHANG,
        BOARD_Y + GPIO_ROW_EDGE_INSET,
        BOARD_Z - GPIO_HEIGHT,
        (0.92, 0.68, 0.2),
        transparency=45,
    )
    make_box(
        doc,
        "GPIO_Right_Bottom_Clearance",
        GPIO_ROW_LENGTH,
        GPIO_ROW_WIDTH,
        GPIO_HEIGHT,
        BOARD_X + GPIO_NON_ANTENNA_OVERHANG,
        BOARD_Y + BOARD_WIDTH - GPIO_ROW_EDGE_INSET - GPIO_ROW_WIDTH,
        BOARD_Z - GPIO_HEIGHT,
        (0.92, 0.68, 0.2),
        transparency=45,
    )


def build_case():
    doc = App.newDocument("ESP32C5_DevKit_Case")
    case_color = (0.08, 0.08, 0.08)
    lid_color = (0.12, 0.12, 0.12)

    bottom_plate = add_bottom_floor_with_gpio_slots(doc, case_color)
    lower_shell_parts = add_lower_shell(doc, case_color)
    lid_parts = add_lid(doc, lid_color)
    doc.recompute()

    bottom_parts = [bottom_plate] + lower_shell_parts
    bottom_case = make_print_solid(doc, "Bottom_Case_Print", bottom_parts, case_color)
    lid = make_print_solid(doc, "Lid_Print", lid_parts, lid_color)

    add_board_reference(doc)
    add_note(doc)
    hide_objects([obj for obj in doc.Objects if obj not in (bottom_case, lid)])

    doc.recompute()
    export_print_stls(bottom_case, lid)

    if Gui is not None:
        Gui.SendMsgToActiveView("ViewFit")
        Gui.activeDocument().activeView().viewAxometric()

    return doc


document = build_case()

if Gui is None:
    output_path = os.path.join(output_directory(), "esp32c5_devkit_case.FCStd")
    document.saveAs(output_path)
    print(f"Saved {output_path}")
