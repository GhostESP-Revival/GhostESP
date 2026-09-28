#!/usr/bin/env python3
"""tests for scripts/partition_table.py

run:  python scripts/test_partition_table.py
      python -m pytest scripts/test_partition_table.py

lives next to the module rather than in tests/ because tests/ is gitignored
(.gitignore:79). move it there when that is un-ignored.
"""

import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import partition_table as pt  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parent.parent

TYPE_APP = pt.TYPE_APP
TYPE_DATA = pt.TYPE_DATA


def entry(label, ptype, subtype, offset, size):
    """build one on-flash partition entry"""
    return struct.pack(
        pt._ENTRY_FMT,
        bytes((0xAA, 0x50)),
        ptype,
        subtype,
        offset,
        size,
        label.encode()[:16].ljust(16, b"\x00"),
        0,
    )


def table(*entries, terminator=True):
    """build a partition-table.bin, with the MD5/padding tail unless told not to"""
    blob = b"".join(entries)
    if terminator:
        blob += struct.pack(pt._ENTRY_FMT, bytes((0xEB, 0xEB)), 0xFF, 0xFF,
                            0xFFFFFFFF, 0xFFFFFFFF, b"md5".ljust(16, b"\x00"), 0xFF)
        blob += b"\xFF" * (32 * 3)
    return blob


def ota_table(slot_size=0x3E0000, n_slots=2, factory=False):
    ents = [entry("nvs", TYPE_DATA, 0x02, 0x9000, 0x7000),
            entry("otadata", TYPE_DATA, 0x00, 0x10000, 0x2000)]
    off = 0x20000
    if factory:
        ents.append(entry("factory", TYPE_APP, 0x00, off, slot_size))
        off += slot_size
    for i in range(n_slots):
        ents.append(entry("ota_%d" % i, TYPE_APP, 0x10 + i, off, slot_size))
        off += slot_size
    return table(*ents)


class TestParse(unittest.TestCase):
    def test_reads_entries_in_order(self):
        data = table(entry("nvs", TYPE_DATA, 0x02, 0x9000, 0x7000),
                     entry("otadata", TYPE_DATA, 0x00, 0x10000, 0x2000))
        parts = pt.parse(data)
        self.assertEqual([p.label for p in parts], ["nvs", "otadata"])
        self.assertEqual(parts[0].offset, 0x9000)
        self.assertEqual(parts[1].subtype, 0x00)
        self.assertTrue(parts[1].is_ota_data)

    def test_stops_at_terminator(self):
        """the MD5 record and 0xFF padding must not parse as partitions"""
        data = ota_table()
        self.assertEqual(len(pt.parse(data)), 2 + 2)  # nvs, otadata, ota_0, ota_1

    def test_truncated_garbage_raises(self):
        with self.assertRaises(pt.PartitionError):
            pt.parse(b"\x00" * 64)

    def test_empty_raises(self):
        with self.assertRaises(pt.PartitionError):
            pt.parse(b"")

    def test_label_is_nul_stripped(self):
        data = table(entry("coredump", TYPE_DATA, 0x03, 0x7E0000, 0x20000))
        self.assertEqual(pt.parse(data)[0].label, "coredump")


class TestAppLayout(unittest.TestCase):
    def test_ab_table_falls_back_to_lowest_ota_slot(self):
        """no factory partition, so the bootloader starts at the lowest ota_N

        this is the case build.py got wrong by hardcoding 0x10000, which lands
        on otadata and gives "No bootable app partitions"
        """
        lay = pt.app_layout(ota_table(n_slots=2))
        self.assertEqual(lay.factory_offset, 0x20000)
        self.assertEqual(lay.min_app_size, 0x3E0000)
        self.assertTrue(lay.is_ota)
        self.assertTrue(lay.has_otadata)

    def test_factory_table_uses_factory_offset(self):
        lay = pt.app_layout(ota_table(factory=True, n_slots=2))
        self.assertEqual(lay.factory_offset, 0x20000)  # factory comes first
        self.assertTrue(lay.is_ota)

    def test_min_app_size_uses_smallest_slot(self):
        """an A/B image must fit the smallest slot, not the largest"""
        ents = [entry("otadata", TYPE_DATA, 0x00, 0x10000, 0x2000),
                entry("ota_0", TYPE_APP, 0x10, 0x20000, 0x600000),
                entry("ota_1", TYPE_APP, 0x11, 0x620000, 0x300000)]
        lay = pt.app_layout(table(*ents))
        self.assertEqual(lay.min_app_size, 0x300000)

    def test_p4_style_equal_slots(self):
        """factory + ota_0 + ota_1 all 0x480000, as partitions_crowpanel_p4.csv"""
        ents = [entry("nvs", TYPE_DATA, 0x02, 0x9000, 0x4000),
                entry("otadata", TYPE_DATA, 0x00, 0xD000, 0x2000),
                entry("phy_init", TYPE_DATA, 0x01, 0xF000, 0x1000),
                entry("factory", TYPE_APP, 0x00, 0x10000, 0x480000),
                entry("ota_0", TYPE_APP, 0x10, 0x490000, 0x480000),
                entry("ota_1", TYPE_APP, 0x11, 0x910000, 0x480000),
                entry("slave_fw", TYPE_DATA, 0x40, 0xD90000, 0x200000)]
        lay = pt.app_layout(table(*ents))
        self.assertEqual(lay.factory_offset, 0x10000)
        self.assertEqual(lay.min_app_size, 0x480000)
        self.assertTrue(lay.is_ota)
        self.assertEqual(pt.find_by_label(table(*ents), "slave_fw").offset, 0xD90000)

    def test_otadata_without_slots_is_not_ota(self):
        """partitions_banshee_c5_factory.csv has otadata but no ota_N slots"""
        data = table(entry("nvs", TYPE_DATA, 0x02, 0x9000, 0x7000),
                     entry("otadata", TYPE_DATA, 0x00, 0x7A0000, 0x2000),
                     entry("app0", TYPE_APP, 0x00, 0x10000, 0x670000))
        lay = pt.app_layout(data)
        self.assertFalse(lay.is_ota)
        self.assertEqual(lay.ota_slots, ())

    def test_factory_only_no_otadata(self):
        data = table(entry("app0", TYPE_APP, 0x00, 0x10000, 0x3E0000))
        lay = pt.app_layout(data)
        self.assertEqual(lay.factory_offset, 0x10000)
        self.assertFalse(lay.is_ota)

    def test_no_app_partition_raises(self):
        data = table(entry("nvs", TYPE_DATA, 0x02, 0x9000, 0x7000))
        with self.assertRaises(pt.PartitionError):
            pt.app_layout(data)


class TestBootloaderOffset(unittest.TestCase):
    def test_known_targets(self):
        self.assertEqual(pt.bootloader_offset("esp32"), 0x1000)
        self.assertEqual(pt.bootloader_offset("esp32s2"), 0x1000)
        self.assertEqual(pt.bootloader_offset("esp32c5"), 0x2000)
        self.assertEqual(pt.bootloader_offset("esp32p4"), 0x2000)
        self.assertEqual(pt.bootloader_offset("esp32s3"), 0x0)
        self.assertEqual(pt.bootloader_offset("esp32c3"), 0x0)
        self.assertEqual(pt.bootloader_offset("esp32c6"), 0x0)

    def test_unknown_target_raises(self):
        with self.assertRaises(pt.PartitionError):
            pt.bootloader_offset("esp32h2")


class TestCli(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name)
        self.table = self.dir / "partitions.bin"
        self.table.write_bytes(ota_table())

    def _run(self, *argv):
        return pt.main([str(a) for a in argv])

    def test_offset_prints_hex(self):
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = self._run("offset", self.table)
        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().strip(), "0x20000")

    def test_slot_size_prints_decimal(self):
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = self._run("slot-size", self.table)
        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().strip(), str(0x3E0000))

    def test_check_fit_passes_and_fails(self):
        app = self.dir / "app.bin"
        app.write_bytes(b"\x00" * 0x1000)
        self.assertEqual(self._run("check-fit", app, self.table), 0)
        app.write_bytes(b"\x00" * 0x3E0000)
        self.assertEqual(self._run("check-fit", app, self.table), 1)

    def test_label_offset(self):
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(self._run("label-offset", self.table, "nvs"), 0)
        self.assertEqual(buf.getvalue().strip(), "0x9000")

    def test_ota_warning_advisory_never_fails(self):
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = self._run("ota-warning", self.table, "--name", "Test Board")
        self.assertEqual(rc, 0)
        self.assertIn("Test Board", buf.getvalue())

    def test_ota_warning_silent_for_factory_only(self):
        import io
        from contextlib import redirect_stdout
        self.table.write_bytes(table(entry("app0", TYPE_APP, 0x00, 0x10000, 0x3E0000)))
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = self._run("ota-warning", self.table)
        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().strip(), "")


class TestRepoConsistency(unittest.TestCase):
    """guard rails against hand-typed numbers disagreeing with the tables"""

    @staticmethod
    def _fields(row):
        """pull the key: "value" pairs out of one - { ... } matrix row

        hand-parsed rather than via pyyaml: this runs as a release gate on a
        bare actions/setup-python interpreter, which has no pyyaml installed
        """
        body = row.strip()
        if body.startswith("{"):
            body = body[1:]
        if body.endswith("}"):
            body = body[:-1]

        out, depth, buf = {}, 0, ""
        for ch in body:
            if ch == "," and depth == 0:
                if buf.strip():
                    k, _, v = buf.partition(":")
                    out[k.strip()] = v.strip().rstrip(",").strip().strip('"')
                buf = ""
                continue
            if ch in "{[":
                depth += 1
            elif ch in "}]":
                depth -= 1
            buf += ch
        if buf.strip():
            k, _, v = buf.partition(":")
            out[k.strip()] = v.strip().rstrip(",").strip().strip('"')
        return out

    def _matrix_targets(self):
        lines = (REPO / ".github" / "workflows" / "compile_all.yml").read_text(
            encoding="utf-8").splitlines()
        start = next(i for i, l in enumerate(lines) if l.strip() == "target:")
        targets = []
        for line in lines[start + 1:]:
            s = line.strip()
            if not s.startswith("- {"):
                break
            targets.append(self._fields(s[2:]))
        self.assertTrue(targets, "failed to parse the build matrix")
        return targets

    @staticmethod
    def _table_for(target):
        import re
        cfg = REPO / target["sdkconfig_file"]
        m = re.search(r'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"',
                      cfg.read_text(errors="replace"))
        return m.group(1) if m else "partitions.csv"

    @staticmethod
    def _app_sizes(csv_path):
        sizes = []
        for line in csv_path.read_text(encoding="utf-8").splitlines():
            line = line.split("#")[0].strip()
            if not line:
                continue
            f = [x.strip() for x in line.split(",")]
            if len(f) >= 5 and f[1] == "app" and f[4]:
                sizes.append(int(f[4], 16))
        return sizes

    def test_every_matrix_target_has_a_bootloader_offset(self):
        """a new idf target has to be added to BOOTLOADER_OFFSET, not defaulted"""
        targets = {t["idf_target"] for t in self._matrix_targets()}
        missing = targets - set(pt.BOOTLOADER_OFFSET)
        self.assertEqual(missing, set(),
                         f"targets missing a bootloader offset: {sorted(missing)}")

    def test_matrix_no_longer_declares_a_slot_size(self):
        """the hand-typed slot size is the bug and must not come back

        matches matrix rows structurally so the prose in the gate step
        explaining where the field used to live does not trip this
        """
        wf = (REPO / ".github" / "workflows" / "compile_all.yml").read_text(
            encoding="utf-8")
        offenders = [
            f"line {n}: {line.strip()[:60]}"
            for n, line in enumerate(wf.splitlines(), 1)
            if line.strip().startswith("- {") and "ota_slot_size" in line
        ]
        self.assertEqual(
            offenders, [],
            f"a build-matrix row declares ota_slot_size ({'; '.join(offenders)}). "
            "slot sizes are facts about a partition table and are derived by "
            "scripts/partition_table.py; a hand-typed value is what let the "
            "gate drift by 128KB on seven boards.",
        )

    def test_derived_slot_size_matches_every_ota_table(self):
        """pin the slot size the gate will now use, for every ota board

        the regression: the matrix typed 4194304 for boards whose slot is
        0x3E0000 (4063232), so the gate would pass an image too big to flash
        """
        expected = {
            "partitions_ota_8mb.csv": 0x3E0000,           # 4063232, not 4194304
            "partitions_ota_16mb.csv": 0x600000,          # 6291456
            "partitions_ota_16mb_c5xip.csv": 0x580000,    # 5767168
            "partitions_ota_s3twatch.csv": 0x400000,     # 4194304
        }
        seen = set()
        checked = 0
        for t in self._matrix_targets():
            if not t.get("ota"):
                continue
            name = self._table_for(t)
            sizes = self._app_sizes(REPO / name)
            self.assertTrue(sizes, f"{t['name']}: no app partition in {name}")
            seen.add(name)
            if name in expected:
                self.assertEqual(
                    min(sizes), expected[name],
                    f"{t['name']} ({name}): smallest app partition changed; "
                    "update expected[] if that was deliberate")
            checked += 1
        self.assertGreater(checked, 0, "no ota: true boards found to check")
        self.assertEqual(seen - set(expected), set(),
                         "an ota: true board uses a table with no expected "
                         "slot size; add it to expected[]")

    def test_ota_8mb_slot_is_not_rounded_up_to_4mb(self):
        """the specific bug: 0x3E0000 is 4063232, not 4194304

        treating it as 0x400000 lets firmware through the gate that cannot
        fit the slot it gets written to
        """
        self.assertEqual(0x3E0000, 4063232)
        self.assertNotEqual(0x3E0000, 0x400000)
        sizes = self._app_sizes(REPO / "partitions_ota_8mb.csv")
        self.assertTrue(sizes and all(s == 0x3E0000 for s in sizes),
                        "partitions_ota_8mb.csv slots are not uniformly "
                        "0x3E0000; re-derive the expectations here")

    # tables declaring otadata with no ota_N slots. each is a known leftover
    # that should be cleaned up; add to it rather than deleting the row so the
    # debt stays visible until someone fixes it.
    KNOWN_OTADATA_WITHOUT_SLOTS = {
        "partitions_banshee_c5_factory.csv":
            "leftover from the reverted banshee self-ota work. otadata is the "
            "last entry (0x7A0000) so removing it shifts nothing and frees "
            "8KB, but it is a shipping board's layout, so fix it deliberately.",
    }

    def test_ota_tables_with_otadata_also_have_slots(self):
        """catches an otadata partition left behind with no ota slots"""
        found = set()
        for csv_path in sorted(REPO.glob("partitions*.csv")):
            has_otadata = False
            has_slots = False
            for line in csv_path.read_text(encoding="utf-8").splitlines():
                line = line.split("#")[0].strip()
                if not line:
                    continue
                f = [x.strip() for x in line.split(",")]
                if len(f) < 3:
                    continue
                if f[1] == "data" and f[2] == "ota":
                    has_otadata = True
                if f[1] == "app" and f[2].startswith("ota_"):
                    has_slots = True
            if has_otadata and not has_slots:
                found.add(csv_path.name)

        unexpected = found - set(self.KNOWN_OTADATA_WITHOUT_SLOTS)
        self.assertEqual(
            unexpected, set(),
            f"otadata with no ota_N slots in {sorted(unexpected)}: add the slots "
            "or drop the leftover otadata. if a new one appears, add it to "
            "KNOWN_OTADATA_WITHOUT_SLOTS with a reason instead of deleting the "
            "row.",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
