"""SPU startup segment regression tests; no game binaries are needed."""
import unittest

from find_spu_functions import pick_text, collect_lifecycle_targets


def segment(off, vaddr, size, flags=5, kind=1, memsz=None):
    return dict(type=kind, off=off, vaddr=vaddr, filesz=size,
                memsz=size if memsz is None else memsz, flags=flags)


class TextSelectionTests(unittest.TestCase):
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
