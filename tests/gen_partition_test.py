# needs: py:numpy, py:lz4
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))

import gen  # noqa: E402


def covers(chunks, n):
    pos = 0
    for i0, i1 in chunks:
        if i0 != pos or i1 <= i0:
            return False
        pos = i1
    return pos == n


class EstimateCostsTest(unittest.TestCase):
    def test_quadratic_only_with_br(self):
        size = np.array([1000, 1000, 490_000, 980_000], dtype=np.int64)
        with_br = np.array([False, True, True, False])
        cost = gen.estimate_costs(size, with_br)
        self.assertEqual(cost.tolist(), [1000, 1000 + 1000 * 1000 // gen.COST_Q, 980_000, 980_000])
        self.assertEqual(cost[2], 2 * 490_000)

    def test_large_values_do_not_overflow(self):
        cost = gen.estimate_costs(np.array([5_000_000]), np.array([True]))
        self.assertEqual(int(cost[0]), 5_000_000 + 25 * 10**12 // gen.COST_Q)


class PartitionTest(unittest.TestCase):
    def test_costly_function_gets_its_own_file(self):
        self.assertEqual(gen.partition(np.array([10, 400_000, 10]), 3000, 3_000_000, 300_000),
                         [(0, 1), (1, 2), (2, 3)])

    def test_big_fn_is_exclusive(self):
        self.assertEqual(gen.partition(np.array([10, 300_000, 10]), 3000, 3_000_000, 300_000), [(0, 3)])
        self.assertEqual(gen.partition(np.array([300_001, 300_001]), 3000, 3_000_000, 300_000), [(0, 1), (1, 2)])

    def test_chunk_cost_limit(self):
        cost = np.array([100, 100, 100, 100, 100])
        self.assertEqual(gen.partition(cost, 3000, 250, 1000), [(0, 2), (2, 4), (4, 5)])
        self.assertEqual(gen.partition(cost, 3000, 200, 1000), [(0, 2), (2, 4), (4, 5)])
        self.assertEqual(gen.partition(cost, 3000, 199, 1000), [(0, 1), (1, 2), (2, 3), (3, 4), (4, 5)])

    def test_max_fns(self):
        cost = np.ones(7, dtype=np.int64)
        self.assertEqual(gen.partition(cost, 3, 10**9, 10**9), [(0, 3), (3, 6), (6, 7)])
        self.assertEqual(gen.partition(cost, 7, 10**9, 10**9), [(0, 7)])
        self.assertEqual(gen.partition(cost, 1, 10**9, 10**9), [(i, i + 1) for i in range(7)])

    def test_empty(self):
        self.assertEqual(gen.partition(np.array([], dtype=np.int64), 3000, 3_000_000, 300_000), [])

    def test_limit_slice_and_cover(self):
        rng = np.random.default_rng(1)
        size = rng.integers(40, 400_000, 5000)
        cost = gen.estimate_costs(size, rng.random(5000) < 0.1)
        for n in (0, 1, 17, 1234, 5000):
            for args in ((3000, 3_000_000, 300_000), (50, 1_000_000, 100_000), (3000, 10, 5)):
                chunks = gen.partition(cost[:n], *args)
                self.assertTrue(covers(chunks, n), (n, args))
                for i0, i1 in chunks:
                    self.assertLessEqual(i1 - i0, args[0])
                    if i1 - i0 > 1:
                        self.assertLessEqual(int(cost[i0:i1].sum()), args[1])
                        self.assertTrue((cost[i0:i1] <= args[2]).all())


class FakeEmitter:
    def __init__(self, kinds):
        self.kinds = kinds
        self.stats = Counter()
        self.uses_patches = False

    def function(self, entry, end):
        kind = self.kinds[entry]
        lines = [f'void f_{entry:x}(Cpu *c) {{', f'    FN_ENTER(0x{entry:x}ull);']
        if kind == 'table':
            self.stats['jump_tables'] += 2
            lines.append('    switch (x8) { case 0x10ull: goto L_10; default: break; }')
        elif kind == 'fallback':
            self.stats['all_label_fallbacks'] += 1
            lines.insert(0, f'static void fb_{entry:x}(Cpu *c);')
        elif kind == 'dispatch':
            self.stats['br_dispatch'] += 1
            lines.append('    c->pc = x8; __attribute__((musttail)) return rt_dispatch(c);')
        lines.append('}')
        return lines, {0x999}


class ChunkTextTest(unittest.TestCase):
    def test_sizes_and_switch_flags(self):
        kinds = {0x100: 'plain', 0x200: 'table', 0x300: 'dispatch', 0x400: 'fallback'}
        e = FakeEmitter(kinds)
        fns = [(a, a + 0x100) for a in kinds]
        sizes = []
        text = gen.chunk_text('mod', 0, e, fns, sizes)
        self.assertEqual([sw for _, sw in sizes], [False, True, False, True])
        bodies = ['\n'.join(e.function(a, b)[0]) for a, b in fns]
        self.assertEqual([n for n, _ in sizes], [len(b) for b in bodies])
        self.assertTrue(text.endswith('\n\n'.join(bodies) + '\n'))
        self.assertIn('void f_999(Cpu *c);\n', text)
        self.assertEqual(gen.chunk_text('mod', 0, FakeEmitter(kinds), fns), text)


class RemoveStaleTest(unittest.TestCase):
    def test_five_digit_numbers(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for name in ('mod_0000.c', 'mod_0001.c', 'mod_10000.c', 'mod_table.c', 'other_0005.c'):
                (d / name).write_text('x')
            errors = []
            self.assertEqual(gen.remove_stale(d, 'mod', 1, errors), 2)
            self.assertEqual(sorted(p.name for p in d.iterdir()), ['mod_0000.c', 'mod_table.c', 'other_0005.c'])
            self.assertEqual(errors, [])


if __name__ == '__main__':
    unittest.main()
