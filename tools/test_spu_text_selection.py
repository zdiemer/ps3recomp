"""SPU startup segment regression tests; no game binaries are needed."""
import unittest
import struct

from find_spu_functions import pick_text, collect_lifecycle_targets, detect_functions
from build_spu_workloads import overlay_code_span


def segment(off, vaddr, size, flags=5, kind=1, memsz=None):
    return dict(type=kind, off=off, vaddr=vaddr, filesz=size,
                memsz=size if memsz is None else memsz, flags=flags)


class TextSelectionTests(unittest.TestCase):
    def test_overlay_code_ownership_excludes_writable_tail(self):
        self.assertEqual(overlay_code_span(0x3080, 0x1AFC0, 0x17100), 0x14080)
        self.assertEqual(overlay_code_span(0x3080, 0x1AFC0), 0x1AFC0)
        for end in (0x3080, 0x3081, 0x20000):
            with self.assertRaises(ValueError):
                overlay_code_span(0x3080, 0x1AFC0, end)
    def test_overlay_entry_called_only_by_resident_segment(self):
        data = bytearray(0x120)
        data[:6] = b"\x7fELF\x01\x02"
        struct.pack_into(">H", data, 18, 23)
        struct.pack_into(">II", data, 24, 0x20080, 52)
        struct.pack_into(">HH", data, 42, 32, 2)
        struct.pack_into(">8I", data, 52, 1, 0x100, 0x20080, 0x20080, 4, 4, 5, 128)
        struct.pack_into(">8I", data, 84, 1, 0x110, 0x3080, 0x40000, 8, 8, 7, 128)
        # brsl r0,0x3084 at resident address 0x20080.
        branch = 0x33000000 | (((0x3084 - 0x20080) // 4 & 0xFFFF) << 7)
        struct.pack_into(">I", data, 0x100, branch)
        data[0x110:0x118] = bytes.fromhex("00200000 35000000")  # lnop; bi r0
        funcs, _ = detect_functions(data, verbose=False, segment_index=1)
        self.assertIn((0x3084, 0x3088), funcs)
        relocated, _ = detect_functions(data, verbose=False, segment_index=1,
                                        base_override=0x4080)
        self.assertIn((0x4084, 0x4088), relocated)
        # Data bytes that happen to decode as branches must not seed code.
        struct.pack_into(">I", data, 52 + 24, 6)
        funcs, _ = detect_functions(data, verbose=False, segment_index=1)
        self.assertEqual(funcs, [(0x3080, 0x3088)])

    def test_explicit_overlay_is_separate_from_resident_code(self):
        data = bytearray(0x110)
        data[:6] = b"\x7fELF\x01\x02"
        struct.pack_into(">H", data, 18, 23)
        struct.pack_into(">II", data, 24, 0x20080, 52)
        struct.pack_into(">HH", data, 42, 32, 2)
        struct.pack_into(">8I", data, 52, 1, 0x100, 0x20080, 0x20080, 4, 4, 5, 128)
        struct.pack_into(">8I", data, 84, 1, 0x108, 0x3080, 0x40000, 4, 4, 7, 128)
        data[0x100:0x104] = data[0x108:0x10C] = bytes.fromhex("35000000")  # bi r0
        resident, mapping = detect_functions(data, verbose=False)
        self.assertEqual(mapping, (0x100, 0x20080, 4))
        overlay, mapping = detect_functions(data, verbose=False, segment_index=1)
        self.assertEqual(mapping, (0x108, 0x3080, 4))
        self.assertEqual(overlay, [(0x3080, 0x3084)])
        self.assertEqual(resident, [(0x20080, 0x20084)])
        with self.assertRaises(ValueError):
            detect_functions(data, verbose=False, segment_index=2)

    def test_single_constructor_between_sentinels(self):
        data = bytes.fromhex("ffffffff 00020190 00000000")
        section = dict(name=".ctors", off=0, size=len(data))
        self.assertEqual(collect_lifecycle_targets(data, [section]), {0x20190})

    def test_invalid_lifecycle_extent_ignored(self):
        data = bytes.fromhex("00020190")
        sections = [dict(name=".ctors", off=0, size=8),
                    dict(name=".dtors", off=0, size=3),
                    dict(name=".data", off=0, size=4)]
        self.assertEqual(collect_lifecycle_targets(data, sections), set())

    def test_entry_in_second_segment(self):
        phs = [segment(0x100, 0x3000, 16), segment(0x180, 0x20080, 0x163B0)]
        self.assertEqual(pick_text(phs, 0x200F0), (0x180, 0x20080, 0x163B0))

    def test_single_segment(self):
        self.assertEqual(pick_text([segment(0x100, 0x80, 0x100)], 0x80), (0x100, 0x80, 0x100))

    def test_legacy_caller(self):
        self.assertEqual(pick_text([segment(0x100, 0x3000, 16), segment(0x180, 0x20080, 128)]),
                         (0x100, 0x3000, 16))

    def test_non_executable_entry_rejected(self):
        with self.assertRaises(SystemExit):
            pick_text([segment(0, 0x100, 128, flags=6)], 0x100)

    def test_segment_end_excluded(self):
        with self.assertRaises(SystemExit):
            pick_text([segment(0, 0x100, 128)], 0x180)

    def test_zero_fill_is_not_code(self):
        with self.assertRaises(SystemExit):
            pick_text([segment(0, 0x100, 16, memsz=128)], 0x120)

    def test_non_load_segment_ignored(self):
        with self.assertRaises(SystemExit):
            pick_text([segment(0, 0x100, 128, kind=4)], 0x100)


if __name__ == "__main__":
    unittest.main()
