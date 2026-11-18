"""tools/make_game.py's compiles through the memory gate (tools/memgate.py):
- compile_all starts the compiles in the order of their cost (a lifted chunk's from the lift's manifest, another
  source's size), or of the seconds an earlier build measured, and the gate admits them in that order;
- each successful compile's measured peak and time go into the estimates, which are saved, and a compile the gate held
  for memory shows in its summary;
- the lines the build prints about the gate;
- chunk_costs reads "cost" and "switch_fn" from a module's manifest.json, "bytes" from an older one, and a file's size
  when the manifest does not list it.
No network, no game files; the compiler is a stand-in script.

Run: python tests/make_game_gate_test.py
"""
import argparse
import collections
import contextlib
import io
import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import make_game as mg  # noqa: E402
import memgate  # noqa: E402

GIB = 1 << 30
MIB = 1 << 20


class RecordingGate(memgate.Gate):
    """The gate, keeping the order in which compiles asked for it."""

    def __init__(self, *args, **kw):
        super().__init__(*args, **kw)
        self.asked = []

    def acquire(self, name, need, cancel=None):
        self.asked.append(name)
        return super().acquire(name, need, cancel)


@unittest.skipIf(os.name == 'nt', 'uses a POSIX script as the compiler')
class CompileAllGateTest(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.tmp = Path(td.name)
        self.src, self.obj = self.tmp / 'src', self.tmp / 'obj'
        self.src.mkdir()
        self.obj.mkdir()
        self.cc = self.tmp / 'fakecc'
        # writes the object and its dependency file after a short wait, so that compiles overlap
        self.cc.write_text(f'#!{sys.executable}\nimport sys, time\nargs = sys.argv[1:]\n'
                           'dep, out, s = args[args.index("-MF") + 1], args[args.index("-o") + 1], args[-1]\n'
                           'time.sleep(0.3)\n'
                           'open(out, "w").close(); open(dep, "w").write(out + ": " + s + "\\n")\n')
        self.cc.chmod(0o755)
        self.quiet = contextlib.redirect_stdout(io.StringIO())
        self.quiet.__enter__()
        self.addCleanup(self.quiet.__exit__, None, None, None)

    def sources(self, sizes):
        srcs = []
        for name, size in sizes:
            (self.src / name).write_text('/*' + 'x' * size + '*/\n')
            srcs.append((self.src / name, self.obj / (Path(name).stem + '.o')))
        return srcs

    def go(self, srcs, gate, estimates, costs, measured=None, plan=None):
        cache = mg.ObjectCache(self.obj / 'objects.json')
        return mg.compile_all(srcs, lambda s, d: [str(self.cc), '-c', '-o', str(d), str(s)], cache, gate.max_jobs,
                              dict(os.environ), gate=gate, estimates=estimates, costs=costs, measured=measured,
                              plan=plan)

    def test_admits_in_cost_order_records_and_saves_the_peaks(self):
        # big.c is the largest file but the cheapest chunk; rt.c is not lifted: its size is its cost
        srcs = self.sources([('big.c', 9000), ('dear.c', 100), ('mid.c', 2000), ('rt.c', 5000)])
        costs = {self.src / 'big.c': (1000, 0), self.src / 'dear.c': (50_000, 4000), self.src / 'mid.c': (3000, 0)}
        flip = threading.Event()
        held_log = []
        # no free memory until flip: the first compile starts (min_jobs), the next is held for memory
        gate = RecordingGate(4, min_jobs=1, reserve=0, avail=lambda: 64 * GIB if flip.is_set() else 0,
                             poll=0.02, log=held_log.append)
        timer = threading.Timer(0.15, flip.set)
        timer.start()
        self.addCleanup(timer.cancel)
        estimates = memgate.Estimates(self.obj / 'compile-memory.json')
        measured, plans = [], []
        try:
            compiled, reused, bad = self.go(srcs, gate, estimates, costs, measured, plans.append)
        finally:
            gate.close()
        self.assertEqual((compiled, reused, bad), (4, 0, []))
        self.assertEqual(gate.asked, ['dear.o', 'rt.o', 'mid.o', 'big.o'])
        self.assertEqual(plans, [collections.Counter({'model': 3, 'default': 1})])

        saved = json.loads((self.obj / 'compile-memory.json').read_text())['objects']
        self.assertEqual(set(saved), {'big.o', 'dear.o', 'mid.o', 'rt.o'})
        for name, e in saved.items():
            self.assertGreater(e['peak'], 0, name)
            self.assertGreater(e['seconds'], 0.2, name)
        self.assertEqual((saved['dear.o']['cost'], saved['dear.o']['switch_fn']), (50_000, 4000))
        self.assertEqual(saved['rt.o']['cost'], 0)   # not a lifted chunk: kept out of the size model's scale
        self.assertEqual(saved['rt.o']['src_bytes'], (self.src / 'rt.c').stat().st_size)
        self.assertEqual(sorted(m[0] for m in measured), ['big.o', 'dear.o', 'mid.o', 'rt.o'])

        s = gate.summary()
        self.assertGreaterEqual(s['holds'], 1)
        self.assertTrue(any('compiles held for memory' in line for line in gate.summary_lines()))
        self.assertTrue(held_log and held_log[0].startswith('memory gate: holding rt.o'))

        lines = mg.compile_memory_lines(gate, estimates, measured)
        self.assertTrue(any(line.startswith('largest compiles (4 of 4): ') for line in lines))
        self.assertTrue(any(line.startswith('estimates against the measured peaks: median ') for line in lines))

    def test_a_learned_time_orders_before_the_cost(self):
        srcs = self.sources([('a.c', 1000), ('b.c', 1000), ('c.c', 1000), ('d.c', 1000)])
        costs = {p: (c, 0) for (p, _), c in zip(srcs, (4000, 3000, 2000, 1000))}
        estimates = memgate.Estimates(self.obj / 'compile-memory.json')
        size = (self.src / 'a.c').stat().st_size
        # measured last time: b.o 1 s, c.o 2 s, d.o 10 s; their median rate (c.o's, 1 s per 1000 bytes of cost) puts
        # a.o, not built before, at 4 s
        for name, seconds, cost in (('b.o', 1.0, 3000), ('c.o', 2.0, 2000), ('d.o', 10.0, 1000)):
            estimates.record(name, 200 * MIB, seconds, size, cost)
        gate = RecordingGate(1, enabled=False)
        try:
            self.assertEqual(self.go(srcs, gate, estimates, costs)[2], [])
        finally:
            gate.close()
        self.assertEqual(gate.asked, ['d.o', 'a.o', 'c.o', 'b.o'])

    def test_stop_ends_the_compiles_held_for_memory(self):
        srcs = self.sources([('a.c', 300), ('b.c', 200), ('c.c', 100)])
        stopped = threading.Event()
        # b.o waits for memory that never comes; the stop ends its wait at once, and neither it nor c.o compiles
        gate = RecordingGate(3, min_jobs=1, reserve=0, avail=lambda: 0, poll=0.02, log=lambda line: None)
        timer = threading.Timer(0.1, stopped.set)
        timer.start()
        self.addCleanup(timer.cancel)
        cache = mg.ObjectCache(self.obj / 'objects.json')
        try:
            compiled, reused, bad = mg.compile_all(srcs, lambda s, d: [str(self.cc), '-c', '-o', str(d), str(s)],
                                                   cache, 3, dict(os.environ), stop=stopped.is_set, gate=gate)
        finally:
            gate.close()
        self.assertEqual(bad, [])
        self.assertTrue((self.obj / 'a.o').exists())
        self.assertFalse((self.obj / 'b.o').exists())
        self.assertFalse((self.obj / 'c.o').exists())
        self.assertEqual(gate.summary()['started'], 1)
        self.assertEqual(gate.asked, ['a.o', 'b.o'])

    def test_a_peak_measured_under_memory_pressure_keeps_a_higher_record(self):
        srcs = self.sources([('a.c', 300), ('b.c', 200)])
        estimates = memgate.Estimates(self.obj / 'compile-memory.json')
        for src, dst in srcs:
            estimates.record(dst.name, 64 * GIB, 1.0, src.stat().st_size, 0)   # far above any real peak
        gate = RecordingGate(1, reserve=GIB, poll=0.02, log=lambda line: None)

        def avail():   # below the 1 GiB reserve while a.o compiles, plenty while b.o does
            return 0 if any(lease.name == 'a.o' for lease in list(gate._running)) else 64 * GIB
        gate._avail = avail
        try:
            self.assertEqual(self.go(srcs, gate, estimates, {})[2], [])
        finally:
            gate.close()
        self.assertEqual(estimates.objects['a.o']['peak'], 64 * GIB)    # pressed: the higher record stays
        self.assertLess(estimates.objects['b.o']['peak'], 64 * GIB)     # not pressed: the measured peak


class OrderTest(unittest.TestCase):
    def test_without_learned_times_the_cost_decides(self):
        est = memgate.Estimates()
        self.assertEqual(mg.compile_order([(0, 'a', 10, 'model'), (1, 'b', 30, 'model'), (2, 'c', 20, 'default')],
                                          est), [1, 2, 0])


class LinesTest(unittest.TestCase):
    def test_the_start_line(self):
        gate = memgate.Gate(16, min_jobs=2, reserve=int(3.2 * GIB), ceiling=None)
        hows = collections.Counter(learned=900, model=40)
        self.assertEqual(mg.gate_start_line(gate, hows),
                         'memory gate: on; up to 16 at once, at least 2; keeps 3.2 GiB free; no ceiling; estimates: '
                         '900 learned from earlier builds, 40 from the size model')
        gate = memgate.Gate(8, reserve=2 * GIB, ceiling=12 * GIB)
        self.assertEqual(mg.gate_start_line(gate, collections.Counter(model=3, default=2)),
                         'memory gate: on; up to 8 at once, at least 1; keeps 2.0 GiB free; ceiling 12.0 GiB; '
                         'estimates: 0 learned from earlier builds, 3 from the size model, 2 set at 400 MiB (C) or '
                         '1 GiB (C++) until measured')
        self.assertEqual(mg.gate_start_line(memgate.Gate(8, enabled=False), collections.Counter()),
                         'memory gate: off; up to 8 at once')

    def jobs_line(self, argv, env=None):
        ap = argparse.ArgumentParser()
        memgate.add_arguments(ap)
        with mock.patch.object(mg, 'GATE_ARGS', ap.parse_args(argv)), \
                mock.patch.dict(os.environ, env or {}, clear=False):
            for name in (memgate.ENV_GATE, memgate.ENV_LIMIT, memgate.ENV_RESERVE):
                if not env or name not in env:
                    os.environ.pop(name, None)
            return mg.jobs_line(16, '16 processors')

    def test_the_jobs_line(self):
        self.assertEqual(self.jobs_line(['--mem-reserve', '3.2']),
                         'compiles at a time: up to 16 (16 processors); the memory gate starts each one when the free '
                         'memory holds it (keeps 3.2 GiB free, no ceiling)')
        self.assertEqual(self.jobs_line(['--mem-reserve', '2', '--mem-limit', '20']),
                         'compiles at a time: up to 16 (16 processors); the memory gate starts each one when the free '
                         'memory holds it (keeps 2.0 GiB free, at most 20.0 GiB estimated in all)')
        self.assertEqual(self.jobs_line([], {memgate.ENV_GATE: 'off'}),
                         'compiles at a time: up to 16 (16 processors) (memory gate off: NX2SARIA_BUILD_MEM_GATE=off)')
        self.assertEqual(self.jobs_line(['--no-mem-gate']),
                         'compiles at a time: up to 16 (16 processors) (memory gate off: --no-mem-gate)')

    def test_the_closing_lines(self):
        gate = memgate.Gate(4, reserve=GIB)
        gate.peak_rss, gate.peak_rss_at, gate.lowest_avail = int(5.5 * GIB), 42.0, int(1.25 * GIB)
        est = memgate.Estimates()
        est.need('a.o', 1000, 1000)
        est.need('b.o', 1000, 1000)
        est.record('a.o', est._estimated['a.o'][1] // 2, 3.0, 1000, 1000)   # half the estimate
        est.record('b.o', est._estimated['b.o'][1] * 2, 70.0, 1000, 1000)   # twice it
        lines = mg.compile_memory_lines(gate, est, [('a.o', GIB // 2, 3.0), ('b.o', int(0.92 * GIB), 78.4)])
        self.assertEqual(lines[2], 'compile memory: peak 5.5 GiB for all compiles together (at 42 s), least free '
                                   'memory 1.2 GiB')
        self.assertEqual(lines[3], 'largest compiles (2 of 2): b.o 0.92 GiB 78 s, a.o 0.50 GiB 3 s')
        self.assertEqual(lines[4], 'estimates against the measured peaks: median 0.80x, lowest 0.50x (b.o)')
        self.assertEqual(len(lines), 5)


class ChunkCostsTest(unittest.TestCase):
    def test_costs_from_the_manifest(self):
        with tempfile.TemporaryDirectory() as td:
            lift = Path(td)
            main, sdk = lift / 'main', lift / 'sdk'
            main.mkdir()
            sdk.mkdir()
            for p, size in ((main / 'main_0000.c', 500), (main / 'main_0001.c', 600), (main / 'main_table.c', 700),
                            (sdk / 'sdk_0000.c', 800)):
                p.write_bytes(b'x' * size)
            (main / 'manifest.json').write_text(json.dumps({'chunk_info': [
                {'file': 'main_0000.c', 'bytes': 500, 'cost': 9000, 'switch_fn': 300},
                {'file': 'main_0001.c', 'bytes': 650}]}))   # a manifest from before cost
            costs = mg.chunk_costs(lift)
        self.assertEqual(costs, {main / 'main_0000.c': (9000, 300), main / 'main_0001.c': (650, 0),
                                 main / 'main_table.c': (700, 0), sdk / 'sdk_0000.c': (800, 0)})


if __name__ == '__main__':
    unittest.main()
