"""Read ESP-IDF partition tables and derive image offsets and slot sizes.

The table is the single source of truth for where the app lives and how much
room it has, so the CI OTA gate, the local merged-image builder and the P4
slave-fw placement all read it from here rather than restating it by hand.

Reads the built partition-table.bin, not the CSV: some tables leave the Offset
column blank for IDF to auto-assign, so a CSV has no numbers in it, and the
built table is what the bootloader reads anyway.
"""

from __future__ import annotations

import pathlib
import struct
import sys
from typing import NamedTuple, Optional

# one on-flash entry, little-endian, no padding:
# magic[2] type[1] subtype[1] offset[4] size[4] label[16] flags[4]
_ENTRY_FMT = "<2sBBLL16sL"
_ENTRY_SIZE = struct.calcsize(_ENTRY_FMT)  # 32
_ENTRY_MAGIC = bytes((0xAA, 0x50))

TYPE_APP = 0x00
TYPE_DATA = 0x01

SUB_APP_FACTORY = 0x00
SUB_APP_OTA_FIRST = 0x10
SUB_APP_OTA_LAST = 0x1F
SUB_DATA_OTA = 0x00

PARTITION_TABLE_OFFSET = 0x8000

# where each chip's ROM expects the second-stage bootloader. a per-chip
# constant, so it cannot be derived, but it needs to live in one place --
# build.py and the workflow each had a copy and each was wrong about one chip.
BOOTLOADER_OFFSET = {
    "esp32": 0x1000,
    "esp32s2": 0x1000,
    "esp32c5": 0x2000,
    "esp32p4": 0x2000,
    "esp32s3": 0x0,
    "esp32c3": 0x0,
    "esp32c6": 0x0,
}


class PartitionError(Exception):
    """Raised when a table is missing something the caller requires."""


class Partition(NamedTuple):
    label: str
    type: int
    subtype: int
    offset: int
    size: int
    flags: int

    @property
    def is_app(self) -> bool:
        return self.type == TYPE_APP

    @property
    def is_ota_data(self) -> bool:
        """true for the otadata partition recording the active ota slot"""
        return self.type == TYPE_DATA and self.subtype == SUB_DATA_OTA

    @property
    def is_ota_slot(self) -> bool:
        return (
            self.is_app
            and SUB_APP_OTA_FIRST <= self.subtype <= SUB_APP_OTA_LAST
        )


class AppLayout(NamedTuple):
    """what a caller needs to place and size the app image"""

    factory_offset: int
    """where to write the app on a fresh flash: the factory partition, else the
    lowest ota_N slot, which is what the bootloader picks when otadata is blank"""

    min_app_size: int
    """smallest app partition; an A/B image has to fit this one"""

    app_partitions: tuple
    ota_slots: tuple
    has_otadata: bool

    @property
    def is_ota(self) -> bool:
        """true when the table supports a two-slot ota scheme"""
        return self.has_otadata and bool(self.ota_slots)


def parse(data: bytes) -> list:
    """return the entries of a built partition-table.bin, in order

    stops at the first entry without the 0xAA50 magic, which is where the
    trailing MD5 record and 0xFF padding start
    """
    partitions = []
    for pos in range(0, len(data) - _ENTRY_SIZE + 1, _ENTRY_SIZE):
        magic, ptype, subtype, offset, size, label, flags = struct.unpack(
            _ENTRY_FMT, data[pos:pos + _ENTRY_SIZE]
        )
        if magic != _ENTRY_MAGIC:
            break
        partitions.append(
            Partition(
                label=label.split(b"\x00", 1)[0].decode("utf-8", "replace"),
                type=ptype,
                subtype=subtype,
                offset=offset,
                size=size,
                flags=flags,
            )
        )
    if not partitions:
        raise PartitionError("no partition entries found (bad magic or empty file)")
    return partitions


def app_layout(data: bytes) -> AppLayout:
    """derive the app offset and slot budget from a built table"""
    partitions = parse(data)
    apps = [p for p in partitions if p.is_app]
    if not apps:
        raise PartitionError("no app partition in table")

    factory = next((p for p in apps if p.subtype == SUB_APP_FACTORY), None)
    ota_slots = tuple(p for p in apps if p.is_ota_slot)
    if factory is not None:
        app_offset = factory.offset
    elif ota_slots:
        app_offset = min(ota_slots, key=lambda p: p.offset).offset
    else:
        raise PartitionError("no factory or ota_N app partition in table")

    return AppLayout(
        factory_offset=app_offset,
        min_app_size=min(p.size for p in apps),
        app_partitions=tuple(apps),
        ota_slots=ota_slots,
        has_otadata=any(p.is_ota_data for p in partitions),
    )


def find_by_label(data: bytes, label: str) -> Partition:
    """look up a partition by name, e.g. the p4's slave_fw slot"""
    for part in parse(data):
        if part.label == label:
            return part
    raise PartitionError(f"partition {label!r} not found in table")


def load(path) -> bytes:
    return pathlib.Path(path).read_bytes()


def bootloader_offset(idf_target: str) -> int:
    try:
        return BOOTLOADER_OFFSET[idf_target]
    except KeyError:
        raise PartitionError(
            f"no bootloader offset known for target {idf_target!r}; "
            f"add it to BOOTLOADER_OFFSET in scripts/partition_table.py"
        )


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def _cmd_bootloader_offset(args) -> int:
    """print where this chip's rom expects the second-stage bootloader"""
    print("0x%x" % bootloader_offset(args.idf_target))
    return 0


def _cmd_offset(args) -> int:
    """print the app offset, for use as a merged-image base"""
    layout = app_layout(load(args.table))
    print("0x%x" % layout.factory_offset)
    return 0


def _cmd_label_offset(args) -> int:
    """print the offset of a named partition, e.g. slave_fw"""
    print("0x%x" % find_by_label(load(args.table), args.label).offset)
    return 0


def _cmd_slot_size(args) -> int:
    """print the smallest app partition, i.e. the real ota slot budget"""
    print(app_layout(load(args.table)).min_app_size)
    return 0


def _cmd_check_fit(args) -> int:
    """fail if the built app does not fit the smallest app partition

    exits 1 with a reason so ci needs no arithmetic of its own
    """
    app_size = pathlib.Path(args.app).stat().st_size
    try:
        layout = app_layout(load(args.table))
    except PartitionError as exc:
        print(f"error: cannot read partition table: {exc}", file=sys.stderr)
        return 1

    slots = ", ".join(p.label for p in layout.app_partitions)
    print(
        f"{pathlib.Path(args.app).name}: {app_size} bytes; "
        f"smallest app partition ({slots}): {layout.min_app_size} bytes"
    )
    if app_size >= layout.min_app_size:
        print(
            f"error: firmware does not fit its app partition "
            f"({app_size} >= {layout.min_app_size}).\n"
            f"       Either trim the build for this target or revisit its "
            f"partition table (see partitions_ota_*.csv).",
            file=sys.stderr,
        )
        return 1
    free = layout.min_app_size - app_size
    print(f"       {free} bytes ({free * 100 // layout.min_app_size}%) free")
    return 0


def _cmd_layout(args) -> int:
    """print the table in human-readable form"""
    data = load(args.table)
    for part in parse(data):
        print(
            f"  {part.label:<14} type={part.type:#04x} sub={part.subtype:#04x} "
            f"offset={part.offset:#010x} size={part.size:#010x}"
        )
    try:
        layout = app_layout(data)
    except PartitionError as exc:
        print(f"  (no usable app layout: {exc})")
        return 0
    print(
        f"  -> app offset {layout.factory_offset:#x}, "
        f"smallest app partition {layout.min_app_size} "
        f"({layout.min_app_size} bytes), A/B={'yes' if layout.is_ota else 'no'}"
    )
    return 0


def _cmd_ota_warning(args) -> int:
    """note an A/B table that the build matrix does not publish ota for

    advisory only, always exits 0. publishing ota for a board is a support
    commitment so it stays a deliberate `ota: true` in the matrix; this just
    makes it visible when someone adds ota_0/ota_1 and forgets the follow-up
    """
    data = load(args.table)
    try:
        layout = app_layout(data)
    except PartitionError:
        return 0
    if not layout.is_ota:
        return 0
    print(
        f"note: {args.name} has a two-slot A/B layout "
        f"({', '.join(p.label for p in layout.ota_slots)}) but its matrix entry "
        f"has no `ota: true`.\n"
        f"      Intended? Add `ota: true` and a `board_key` to publish updates "
        f"for it; nothing is published without that."
    )
    return 0


def main(argv=None) -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser(
        "bootloader-offset",
        help="print the bootloader offset for an IDF target")
    p.add_argument("idf_target")
    p.set_defaults(func=_cmd_bootloader_offset)

    p = sub.add_parser("offset", help="print the app offset (0x...)")
    p.add_argument("table")
    p.set_defaults(func=_cmd_offset)

    p = sub.add_parser("label-offset", help="print the offset of a named partition")
    p.add_argument("table")
    p.add_argument("label")
    p.set_defaults(func=_cmd_label_offset)

    p = sub.add_parser("slot-size", help="print the smallest app partition size")
    p.add_argument("table")
    p.set_defaults(func=_cmd_slot_size)

    p = sub.add_parser("check-fit", help="fail if the app does not fit its slot")
    p.add_argument("app")
    p.add_argument("table")
    p.set_defaults(func=_cmd_check_fit)

    p = sub.add_parser("layout", help="dump the table")
    p.add_argument("table")
    p.set_defaults(func=_cmd_layout)

    p = sub.add_parser("ota-warning", help="advisory note for unpublished A/B boards")
    p.add_argument("table")
    p.add_argument("--name", default="this target")
    p.set_defaults(func=_cmd_ota_warning)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except PartitionError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
