# Copyright (c) 2026 seedds (MIT; see THIRD_PARTY_NOTICES.md).
# needs: py:capstone, py:lz4
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))

from spin_waits import Search  # noqa: E402

POLL = [0xb4000434, 0xb901028b, 0xb940012e, 0x710005df, 0x54ffffc0, 0x885ffd2e, 0x3400006e, 0xd5033f5f, 0x17fffffd,
        0x880e7d2a, 0x35ffff6e, 0xb901028c, 0x394262ae, 0x7100001f, 0x7a4019c4, 0xb94096b9, 0x7a401b20, 0x1a9fd7ee,
        0x710005df, 0x54000101, 0xb94092ad, 0x4b1901a8, 0x0b19010f, 0xb90096af, 0x1a9f37ed, 0x14000002, 0xd5033f5f,
        0x885f7d2f, 0x710005ff, 0x54ffffa1, 0x880ffd3f, 0x35ffff8f, 0x34fffc0e, 0xd65f03c0]


def region(words, T, bes):
    img = struct.pack('<' + 'I' * len(words), *words) + bytes(64)
    return Search(img, 4 * len(words), set()).region(T, bes)


def patched(**at):
    w = list(POLL)
    for off, word in at.items():
        w[int(off[1:], 16) // 4] = word
    return w


class SpinWaits(unittest.TestCase):
    def test_lock_poll_is_a_wait(self):
        g = region(POLL, 0, [0x80])
        self.assertIsNotNone(g)
        self.assertEqual(g['valid'], {0x80})
        self.assertNotIn(0x5c, g['R'])
        self.assertTrue(set(g['kinds']) >= {'acq', 'rel', 'tst', 'stc'})

    def test_found_by_the_scan(self):
        self.assertIn(0, {g['T'] for g in Search(struct.pack('<' + 'I' * len(POLL), *POLL), 4 * len(POLL), set()).all()})

    def test_patch_site_inside(self):
        img = struct.pack('<' + 'I' * len(POLL), *POLL) + bytes(64)
        self.assertIsNone(Search(img, 4 * len(POLL), {0x3c}).region(0, [0x80]))

    def test_store_of_a_changing_value(self):
        self.assertIsNone(region(patched(o2c=0xb901028e), 0, [0x80]))

    def test_lock_not_released(self):
        self.assertIsNone(region(patched(o4c=0x540001a1), 0, [0x80]))

    def test_nothing_polled(self):
        self.assertIsNone(region([0xf900011f, 0x17ffffff], 0, [4]))

    def test_plain_test_loop(self):
        g = region([0xb9400008, 0x34ffffe8], 0, [4])
        self.assertIsNotNone(g)
        self.assertEqual(g['valid'], {4})

    def test_counted_loop(self):
        self.assertIsNone(region([0xb9400008, 0x71000421, 0x54ffffc1], 0, [8]))


if __name__ == '__main__':
    unittest.main()
