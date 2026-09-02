"""Generate an editable ESP32-C5 DevKit reference model in FreeCAD.

Run from FreeCAD with:
    Macro -> Macros... -> Execute

Or from a shell with FreeCAD installed:
    freecadcmd esp32c5_devkit_freecad.py

The dimensions below are in millimeters and are based on caliper measurements.
Unknown placements are called out as adjustable constants.
"""

from __future__ import annotations

import os

import FreeCAD as App
import Part  # noqa: F401  # Required by FreeCAD for Part objects in some environments.

try:
    import FreeCADGui as Gui
except ImportError:
    Gui = None


# Measured dimensions, mm.
TOTAL_LENGTH = 60.0
BOARD_WIDTH = 25.2
TOTAL_DEPTH_WITH_GPIO = 15.2

ANTENNA_OVERHANG = 5.8
ANTENNA_OFFSET_FROM_LEFT_EDGE = 3.4
ANTENNA_WIDTH = 17.8

HEATSPREADER_LENGTH = 19.6
HEATSPREADER_WIDTH = 15.8
HEATSPREADER_DEPTH = 2.5
HEATSPREADER_OFFSET_FROM_ANTENNA_EDGE = 2.4
HEATSPREADER_VISIBLE_PCB_SIDE = 5.0

GPIO_ROW_LENGTH = 40.5
GPIO_NON_ANTENNA_OVERHANG = 12.6
GPIO_PIN_WIDTH = 0.5
GPIO_PIN_GAP = 1.5

USB_C_LENGTH = 7.5
USB_C_WIDTH = 9.0
USB_C_GAP = 5.3


# Assumed/adjustable construction dimensions, mm.
PCB_THICKNESS = 1.6
ANTENNA_COPPER_THICKNESS = 0.12
GPIO_ROW_WIDTH = 2.54
GPIO_ROW_EDGE_INSET = 0.8
USB_C_HEIGHT = HEATSPREADER_DEPTH
GPIO_PIN_PITCH = GPIO_PIN_WIDTH + GPIO_PIN_GAP
GPIO_PIN_COUNT = int((GPIO_ROW_LENGTH + GPIO_PIN_GAP) // GPIO_PIN_PITCH)
GPIO_PIN_USED_LENGTH = (GPIO_PIN_COUNT * GPIO_PIN_WIDTH) + ((GPIO_PIN_COUNT - 1) * GPIO_PIN_GAP)
GPIO_PIN_START_OFFSET = (GPIO_ROW_LENGTH - GPIO_PIN_USED_LENGTH) / 2.0


MAIN_PCB_LENGTH = TOTAL_LENGTH - ANTENNA_OVERHANG
GPIO_HEIGHT = TOTAL_DEPTH_WITH_GPIO - PCB_THICKNESS - HEATSPREADER_DEPTH
PCB_Z = GPIO_HEIGHT
TOP_Z = PCB_Z + PCB_THICKNESS


def make_box(doc, name, length, width, height, x, y, z, color, transparency=0):
    obj = doc.addObject("Part::Box", name)
    obj.Length = length
    obj.Width = width
    obj.Height = height
    obj.Placement = App.Placement(App.Vector(x, y, z), App.Rotation())
    obj.Label = name.replace("_", " ")

    if Gui is not None:
        obj.ViewObject.ShapeColor = color
        obj.ViewObject.Transparency = transparency

    return obj


def make_gpio_pins(doc, row_name, y):
    for index in range(GPIO_PIN_COUNT):
        pin_x = GPIO_NON_ANTENNA_OVERHANG + GPIO_PIN_START_OFFSET + (index * GPIO_PIN_PITCH)
        make_box(
            doc,
            f"{row_name}_Pin_{index + 1:02d}",
            GPIO_PIN_WIDTH,
            GPIO_PIN_WIDTH,
            GPIO_HEIGHT,
            pin_x,
            y,
            0,
            (0.92, 0.68, 0.2),
        )


def add_note(doc):
    note = doc.addObject("App::Annotation", "Model_Notes")
    note.LabelText = ["ESP32C5-Devkit"]
    note.Position = App.Vector(0, -8, TOTAL_DEPTH_WITH_GPIO + 3)
    return note


def build_model():
    doc = App.newDocument("ESP32C5_DevKit_Reference")

    make_box(
        doc,
        "Overall_Depth_Envelope",
        TOTAL_LENGTH,
        BOARD_WIDTH,
        TOTAL_DEPTH_WITH_GPIO,
        0,
        0,
        0,
        (0.8, 0.8, 0.8),
        transparency=85,
    )

    make_box(
        doc,
        "Main_PCB",
        MAIN_PCB_LENGTH,
        BOARD_WIDTH,
        PCB_THICKNESS,
        0,
        0,
        PCB_Z,
        (0.0, 0.0, 0.0),
    )

    make_box(
        doc,
        "Antenna_Overhang_PCB",
        ANTENNA_OVERHANG,
        ANTENNA_WIDTH,
        PCB_THICKNESS,
        MAIN_PCB_LENGTH,
        ANTENNA_OFFSET_FROM_LEFT_EDGE,
        PCB_Z,
        (0.0, 0.0, 0.0),
    )

    make_box(
        doc,
        "Antenna_Copper_Area",
        ANTENNA_OVERHANG,
        ANTENNA_WIDTH,
        ANTENNA_COPPER_THICKNESS,
        MAIN_PCB_LENGTH,
        ANTENNA_OFFSET_FROM_LEFT_EDGE,
        TOP_Z,
        (0.92, 0.68, 0.2),
    )

    heatspreader_x = MAIN_PCB_LENGTH - HEATSPREADER_OFFSET_FROM_ANTENNA_EDGE - HEATSPREADER_LENGTH
    heatspreader_y = HEATSPREADER_VISIBLE_PCB_SIDE
    make_box(
        doc,
        "Heatspreader",
        HEATSPREADER_LENGTH,
        HEATSPREADER_WIDTH,
        HEATSPREADER_DEPTH,
        heatspreader_x,
        heatspreader_y,
        TOP_Z,
        (0.72, 0.72, 0.72),
    )

    left_pin_y = GPIO_ROW_EDGE_INSET + ((GPIO_ROW_WIDTH - GPIO_PIN_WIDTH) / 2.0)
    right_pin_y = BOARD_WIDTH - GPIO_ROW_EDGE_INSET - GPIO_ROW_WIDTH + ((GPIO_ROW_WIDTH - GPIO_PIN_WIDTH) / 2.0)
    make_gpio_pins(doc, "GPIO_Left_Row", left_pin_y)
    make_gpio_pins(doc, "GPIO_Right_Row", right_pin_y)

    usb_pair_width = (USB_C_WIDTH * 2.0) + USB_C_GAP
    usb_y = (BOARD_WIDTH - usb_pair_width) / 2.0
    make_box(
        doc,
        "USB_C_Port_Left",
        USB_C_LENGTH,
        USB_C_WIDTH,
        USB_C_HEIGHT,
        0,
        usb_y,
        TOP_Z,
        (0.62, 0.62, 0.62),
    )

    make_box(
        doc,
        "USB_C_Port_Right",
        USB_C_LENGTH,
        USB_C_WIDTH,
        USB_C_HEIGHT,
        0,
        usb_y + USB_C_WIDTH + USB_C_GAP,
        TOP_Z,
        (0.62, 0.62, 0.62),
    )

    add_note(doc)
    doc.recompute()

    if Gui is not None:
        Gui.SendMsgToActiveView("ViewFit")
        Gui.activeDocument().activeView().viewAxometric()

    return doc


document = build_model()

if Gui is None:
    output_path = os.path.join(os.getcwd(), "esp32c5_devkit_reference.FCStd")
    document.saveAs(output_path)
    print(f"Saved {output_path}")
