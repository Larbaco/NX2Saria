"""tools/memgate.py: the admission rule, the in-process Gate over fake free memory, resident sizes and clock, the
Estimates file, run()'s exact peak on a real small child, and the `wrap` command line end to end with two wrappers at
once. No game files; stdlib only; the children allocate at most about 50 MB each.

Run: python tests/memgate_test.py
"""
import io
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import memgate  # noqa: E402

LINUX = unittest.skipUnless(sys.platform.startswith('linux'), 'reads /proc')
MIB = 1 << 20
GIB = 1 << 30
ALLOC = 'x = b"x" * (50 << 20)\n'   # 50 MB, written, so resident


class Tmp(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.tmp = Path(td.name)


class AdmitTest(unittest.TestCase):
    """The rule both gates share."""

    def admit(self, running, need, avail, reserve=0, ceiling=None, min_jobs=1, max_jobs=8, total=None):
        return memgate.admit(running, need, avail, reserve, ceiling, len(running), min_jobs, max_jobs, total)

    def test_below_min_jobs_always(self):
        self.assertTrue(self.admit([], 100 * GIB, 0))
        self.assertTrue(self.admit([(GIB, None)], 100 * GIB, 0, min_jobs=2))
        self.assertFalse(self.admit([(GIB, None)], 100 * GIB, 0, min_jobs=1))

    def test_max_jobs_caps(self):
        running = [(MIB, MIB)] * 4
        self.assertFalse(self.admit(running, MIB, 100 * GIB, max_jobs=4))
        self.assertTrue(self.admit(running, MIB, 100 * GIB, max_jobs=5))

    def test_owed_is_need_less_what_is_resident(self):
        # 2 GiB needed, 0.5 GiB resident: 1.5 GiB still owed; a compile without a pid yet owes all its need.
        self.assertEqual(memgate.owed([(2 * GIB, GIB // 2), (GIB, None), (GIB, 3 * GIB)]), 3 * GIB // 2 + GIB)
        running = [(2 * GIB, GIB // 2)]
        self.assertTrue(self.admit(running, GIB, 5 * GIB // 2))
        self.assertFalse(self.admit(running, GIB, 5 * GIB // 2 - 1))

    def test_reserve_is_kept(self):
        self.assertTrue(self.admit([(MIB, MIB)], GIB, 3 * GIB, reserve=2 * GIB))
        self.assertFalse(self.admit([(MIB, MIB)], GIB, 3 * GIB - 1, reserve=2 * GIB))

    def test_ceiling_bounds_the_sum_of_needs(self):
        running = [(3 * GIB, 3 * GIB)]
        self.assertTrue(self.admit(running, GIB, 100 * GIB, ceiling=4 * GIB))
        self.assertFalse(self.admit(running, GIB + 1, 100 * GIB, ceiling=4 * GIB))
        self.assertFalse(self.admit(running, GIB + 1, None, ceiling=4 * GIB))

    def test_unknown_free_memory_is_bounded_by_the_ram(self):
        running = [(4 * GIB, None), (4 * GIB, 4 * GIB)]
        self.assertTrue(self.admit(running, 6 * GIB, None, reserve=2 * GIB, total=16 * GIB))
        self.assertFalse(self.admit(running, 6 * GIB + 1, None, reserve=2 * GIB, total=16 * GIB))
        self.assertFalse(self.admit([(MIB, None)] * 4, MIB, None, max_jobs=4, total=100 * GIB))

    def test_unknown_free_memory_and_ram_leave_min_jobs(self):
        self.assertTrue(self.admit([(MIB, None)], MIB, None, min_jobs=2))
        self.assertFalse(self.admit([(MIB, None)] * 2, MIB, None, min_jobs=2))


class Fake:
    """Free memory, resident sizes by pid, a clock and a log the tests set."""

    def __init__(self, avail):
        self.free = avail
        self.sizes = {}
        self.now = 0.0
        self.lines = []

    def avail(self):
        return self.free

    def rss(self, pid):
        return self.sizes.get(pid, 0)

    def clock(self):
        return self.now


def wait_until(cond, seconds=5.0):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.005)
    return False


class GateTest(unittest.TestCase):
    def gate(self, avail, **kw):
        self.f = Fake(avail)
        kw.setdefault('max_jobs', 8)
        kw.setdefault('reserve', 0)
        g = memgate.Gate(avail=self.f.avail, rss=self.f.rss, clock=self.f.clock, poll=0.01, log=self.f.lines.append,
                         **kw)
        self.addCleanup(g.close)
        return g

    def background(self, g, name, need, done):
        """acquire in a thread; the lease is appended to done once admitted."""
        t = threading.Thread(target=lambda: done.append(g.acquire(name, need)), daemon=True)
        t.start()
        self.addCleanup(t.join, 5)
        return t

    def test_one_compile_always_runs(self):
        g = self.gate(0)
        lease = g.acquire('huge', 1 << 50)
        self.assertEqual(g.summary()['started'], 1)
        g.release(lease)

    def test_owed_memory_holds_until_the_compile_is_resident(self):
        g = self.gate(3 * GIB)
        first = g.acquire('a', 2 * GIB)   # no pid yet: owes 2 GiB, 1 GiB left
        done = []
        self.background(g, 'b', 2 * GIB, done)
        self.assertTrue(wait_until(lambda: g.summary()['holds'] == 1))
        self.assertEqual(done, [])
        first.started(4242)
        self.f.sizes[4242] = GIB   # 1 GiB of a's need is resident and counted in the free memory already
        self.f.free = 3 * GIB      # 3 GiB free less 1 GiB still owed covers 2 GiB
        self.assertTrue(wait_until(lambda: len(done) == 1))
        g.release(first)
        g.release(done[0])

    def test_free_memory_change_is_seen_without_a_release(self):
        g = self.gate(GIB)
        a = g.acquire('a', GIB)
        a.started(1)
        self.f.sizes[1] = GIB
        done = []
        self.background(g, 'b', 2 * GIB, done)
        self.assertTrue(wait_until(lambda: g.summary()['holds'] == 1))
        self.f.free = 2 * GIB
        self.assertTrue(wait_until(lambda: len(done) == 1))
        self.assertEqual(g.summary()['most_at_once'], 2)

    def test_max_jobs_waits_without_counting_a_hold(self):
        g = self.gate(100 * GIB, max_jobs=2)
        a, b = g.acquire('a', MIB), g.acquire('b', MIB)
        done = []
        self.background(g, 'c', MIB, done)
        time.sleep(0.1)
        self.assertEqual(done, [])
        g.release(a)
        self.assertTrue(wait_until(lambda: len(done) == 1))
        self.assertEqual(g.summary()['holds'], 0)
        self.assertEqual(g.summary()['most_at_once'], 2)

    def test_min_jobs_run_whatever_the_memory(self):
        g = self.gate(0, min_jobs=3)
        leases = [g.acquire(str(i), GIB) for i in range(3)]
        self.assertEqual(g.summary()['most_at_once'], 3)
        done = []
        self.background(g, 'fourth', GIB, done)
        self.assertTrue(wait_until(lambda: g.summary()['holds'] == 1))
        g.release(leases[0])
        self.assertTrue(wait_until(lambda: len(done) == 1))

    def test_reserve_and_ceiling(self):
        g = self.gate(10 * GIB, reserve=4 * GIB, ceiling=5 * GIB)
        a = g.acquire('a', MIB)
        a.started(1)
        self.assertTrue(memgate.admit(g._state(), 5 * GIB, 10 * GIB, 4 * GIB, None, 1, 1, 8))
        done = []
        self.background(g, 'over-ceiling', 5 * GIB, done)   # the memory holds it, but 5 GiB + 1 MiB is over the limit
        self.assertTrue(wait_until(lambda: g.summary()['holds'] == 1))
        g.release(a)   # alone it runs: min_jobs
        self.assertTrue(wait_until(lambda: len(done) == 1))

    def test_unknown_free_memory_admits_what_the_ram_holds(self):
        g = self.gate(None, max_jobs=8, total=10 * GIB)
        leases = [g.acquire(str(i), 3 * GIB) for i in range(3)]   # 9 GiB promised
        self.assertEqual(g.summary()['holds'], 0)
        done = []
        self.background(g, 'fourth', 2 * GIB, done)
        self.assertTrue(wait_until(lambda: g.summary()['holds'] == 1))
        self.assertTrue(self.f.lines[0].startswith('memory gate: holding fourth (needs 2.0 GiB; unknown free'))
        g.release(leases[0])
        self.assertTrue(wait_until(lambda: len(done) == 1))

    def test_gate_off_is_a_plain_job_cap(self):
        g = self.gate(0, max_jobs=3, ceiling=GIB, enabled=False)
        leases = [g.acquire(str(i), 100 * GIB) for i in range(3)]
        done = []
        self.background(g, 'fourth', MIB, done)
        time.sleep(0.05)
        self.assertEqual(done, [])
        g.release(leases[0])
        self.assertTrue(wait_until(lambda: len(done) == 1))
        self.assertEqual(g.summary()['holds'], 0)
        self.assertIn('off', g.summary_lines()[0])

    def test_waiters_are_served_in_order(self):
        g = self.gate(GIB)
        a = g.acquire('a', MIB)
        a.started(1)
        leases = {}

        def take(name, need):
            leases[name] = g.acquire(name, need)

        big = threading.Thread(target=take, args=('big', 4 * GIB), daemon=True)
        big.start()
        self.assertTrue(wait_until(lambda: g.summary()['holds'] == 1))
        small = threading.Thread(target=take, args=('small', MIB), daemon=True)
        small.start()
        time.sleep(0.1)
        self.assertEqual(leases, {})   # small would fit, but does not pass big
        self.f.free = 5 * GIB
        big.join(5)
        small.join(5)
        # The order is the one the gate gave out under its own lock.
        self.assertEqual((a.order, leases['big'].order, leases['small'].order), (1, 2, 3))

    def test_holds_are_counted_and_timed(self):
        g = self.gate(0)
        a = g.acquire('a', MIB)
        a.started(1)
        for name, wait in (('x', 3.0), ('y', 7.0)):
            done = []
            holds = g.holds
            self.background(g, name, GIB, done)
            self.assertTrue(wait_until(lambda: g.summary()['holds'] == holds + 1))
            self.f.now += wait
            self.f.free = 2 * GIB
            self.assertTrue(wait_until(lambda: len(done) == 1))
            self.assertEqual(done[0].held, wait)
            g.release(done[0])
            self.f.free = 0
        s = g.summary()
        self.assertEqual((s['holds'], s['held_seconds'], s['longest_hold'], s['longest_hold_name']), (2, 10.0, 7.0, 'y'))
        self.assertTrue(any('2 compiles held' in line for line in g.summary_lines()))

    def test_the_hold_log_is_rate_limited(self):
        g = self.gate(0)
        a = g.acquire('a', MIB)
        a.started(1)

        def hold(name, at):
            self.f.now = at
            done = []
            holds = g.holds
            self.background(g, name, GIB, done)
            self.assertTrue(wait_until(lambda: g.summary()['holds'] == holds + 1))
            self.f.free = 2 * GIB
            self.assertTrue(wait_until(lambda: len(done) == 1))
            g.release(done[0])
            self.f.free = 0

        hold('one', 0.0)
        hold('two', 5.0)
        hold('three', 14.0)
        hold('four', 16.0)
        self.assertEqual(len(self.f.lines), 2, self.f.lines)
        self.assertTrue(self.f.lines[0].startswith('memory gate: holding one (needs 1.0 GiB; 0.0 GiB free, '
                                                   '0.0 GiB promised to the 1 compiles running, reserve 0.0 GiB)'),
                        self.f.lines[0])
        self.assertTrue(self.f.lines[1].startswith('memory gate: holding four'), self.f.lines[1])
        self.assertTrue(self.f.lines[1].endswith('+2 more held'), self.f.lines[1])

    def test_the_sampler_records_peak_and_lowest_free(self):
        g = self.gate(8 * GIB)
        a, b = g.acquire('a', GIB), g.acquire('b', GIB)
        a.started(1)
        b.started(mock.Mock(pid=2))
        self.f.sizes.update({1: GIB, 2: 2 * GIB})
        self.f.now = 12.0
        self.f.free = 3 * GIB
        self.assertTrue(wait_until(lambda: g.summary()['peak_rss'] == 3 * GIB and
                                   g.summary()['lowest_avail'] == 3 * GIB))
        self.assertEqual(g.summary()['peak_rss_at'], 12.0)
        self.assertEqual((a.sampled_peak, b.sampled_peak), (GIB, 2 * GIB))
        lines = g.summary_lines()
        self.assertTrue(any('peak memory of all compiles together 3.0 GiB, 12 s in' in line for line in lines), lines)
        self.assertTrue(any('least free memory 3.0 GiB' in line for line in lines), lines)

    def test_lease_as_context(self):
        g = self.gate(GIB)
        with g.acquire('a', MIB):
            self.assertEqual(len(g._running), 1)
        self.assertEqual(g._running, [])

    def test_cancel_ends_a_hold(self):
        """A compile held for memory leaves the gate at once when its cancel turns true: acquire returns None, the
        waiters behind it move up, and nothing is left to release."""
        g = self.gate(0)
        a = g.acquire('a', GIB)
        cancelled = threading.Event()
        done = []
        t = threading.Thread(target=lambda: done.append(g.acquire('b', GIB, cancel=cancelled.is_set)), daemon=True)
        t.start()
        self.assertTrue(wait_until(lambda: g.summary()['holds'] == 1))
        cancelled.set()
        t.join(5)
        self.assertEqual(done, [None])
        self.assertEqual(g.summary()['started'], 1)
        self.assertEqual(list(g._queue), [])
        g.release(a)
        self.assertIsNone(g.acquire('c', GIB, cancel=lambda: True))   # cancelled before it is even its turn

    def test_a_lease_knows_the_least_free_memory_while_it_ran(self):
        g = self.gate(8 * GIB, reserve=2 * GIB, min_jobs=1)
        first = g.acquire('a', GIB)            # admitted by min_jobs
        second = g.acquire('b', GIB)           # admitted on 8 GiB free
        self.assertEqual(second.lowest_avail, 8 * GIB)
        self.assertFalse(second.pressed)
        self.f.free = GIB                      # below the 2 GiB reserve while both run
        self.assertTrue(wait_until(lambda: first.lowest_avail == GIB and second.lowest_avail == GIB))
        self.assertTrue(first.pressed and second.pressed)
        g.release(first)
        g.release(second)


class EstimatesTest(Tmp):
    def test_learned_within_twenty_percent(self):
        e = memgate.Estimates()
        e.record('a.o', 100 * MIB, 3.0, 1000, 500)
        self.assertEqual(e.need('a.o', 1200, 500), (int(100 * MIB * memgate.MARGIN), 'learned'))
        self.assertEqual(e.need('a.o', 800, 500)[1], 'learned')
        self.assertEqual(e.need('a.o', 1201, 500)[1], 'model')
        self.assertEqual(e.need('b.o', 1000, 500)[1], 'model')

    def test_learned_only_while_the_switch_is_alike(self):
        """After a re-chunk, a chunk of the same name and size can hold another switch: its switch_fn must be within
        20 % too, unless both are small."""
        e = memgate.Estimates()
        e.record('a.o', 100 * MIB, 3.0, 1000, 500, switch_fn=500_000)
        self.assertEqual(e.need('a.o', 1000, 500, switch_fn=590_000)[1], 'learned')
        self.assertEqual(e.need('a.o', 1000, 500, switch_fn=650_000)[1], 'model')
        self.assertEqual(e.need('a.o', 1000, 500, switch_fn=0)[1], 'model')
        e.record('b.o', 100 * MIB, 3.0, 1000, 500, switch_fn=20_000)
        self.assertEqual(e.need('b.o', 1000, 500, switch_fn=90_000)[1], 'learned')   # both below SWITCH_SMALL
        self.assertEqual(e.need('b.o', 1000, 500, switch_fn=0)[1], 'learned')
        self.assertEqual(e.need('b.o', 1000, 500, switch_fn=200_000)[1], 'model')

    def test_a_peak_under_memory_pressure_does_not_lower_the_record(self):
        e = memgate.Estimates()
        e.record('a.o', 500 * MIB, 3.0, 1000, 500)
        e.record('a.o', 300 * MIB, 4.0, 1000, 500, keep_higher=True)    # measured while the memory was short
        self.assertEqual((e.objects['a.o']['peak'], e.objects['a.o']['seconds']), (500 * MIB, 4.0))
        e.record('a.o', 700 * MIB, 3.0, 1000, 500, keep_higher=True)
        self.assertEqual(e.objects['a.o']['peak'], 700 * MIB)
        e.record('a.o', 300 * MIB, 3.0, 1000, 500)                      # without pressure the new peak is taken
        self.assertEqual(e.objects['a.o']['peak'], 300 * MIB)

    def test_a_tiny_peak_meets_the_floor(self):
        e = memgate.Estimates()
        e.record('a.o', MIB, 0.1, 100, 10)
        self.assertEqual(e.need('a.o', 100, 10), (memgate.FLOOR, 'learned'))

    def test_model_before_enough_samples(self):
        e = memgate.Estimates()
        for i in range(memgate.SCALE_MIN_SAMPLES - 1):
            e.record(f'{i}.o', 10 * GIB, 1.0, 100, MIB)
        self.assertEqual(e.scale(), 1.0)
        need, how = e.need('new.o', 3_000_000, 5_000_000, switch_fn=2_000_000)
        self.assertEqual(how, 'model')
        self.assertEqual(need, int((120 * MIB + 150 * MIB + 300 * MIB) * memgate.MARGIN))

    def test_the_model_follows_the_measurements(self):
        """The line against the peaks it was drawn from (clang 20, -O1), within 15 %."""
        for src, switch, peak in ((10_000, 0, 119), (3_000_000, 0, 270), (3_900_000, 0, 340), (2_500_000, 0, 276),
                                  (3_900_000, 3_900_000, 922), (2_800_000, 2_800_000, 663),
                                  (1_800_000, 1_800_000, 465)):
            self.assertAlmostEqual(memgate.model(src, switch) / (peak * MIB), 1.0, delta=0.15, msg=(src, switch))

    def test_model_with_learned_scale(self):
        e = memgate.Estimates()
        for i in range(10):
            size = (i + 1) * 300_000
            e.record(f'{i}.o', int(2 * memgate.model(size, size // 2)), 1.0, size, size * 2, switch_fn=size // 2)
        e.record('runtime.o', 50 * GIB, 1.0, 100, 0)   # no cost: not part of the scale
        self.assertAlmostEqual(e.scale(), 2.0, places=6)
        need, how = e.need('new.o', 1_000_000, 1, switch_fn=500_000)
        self.assertEqual(how, 'model')
        self.assertAlmostEqual(need / (memgate.model(1_000_000, 500_000) * 2.0 * memgate.MARGIN), 1.0, places=6)
        for i in range(10):   # far off: the scale is clamped
            e.record(f'{i}.o', 10 * memgate.model(MIB), 1.0, MIB, MIB)
        self.assertEqual(e.scale(), 3.0)
        for i in range(10):
            e.record(f'{i}.o', int(memgate.model(MIB) // 10), 1.0, MIB, MIB)
        self.assertEqual(e.scale(), 0.5)

    def test_default_for_sources_without_a_cost(self):
        e = memgate.Estimates()
        self.assertEqual(e.need('vk.cpp.o', 4000, 0, default=300 * MIB), (300 * MIB, 'default'))
        e.record('vk.cpp.o', 500 * MIB, 2.0, 4000, 0)
        self.assertEqual(e.need('vk.cpp.o', 4000, 0, default=300 * MIB)[1], 'learned')

    def test_stats(self):
        e = memgate.Estimates()
        e.record('a.o', 100 * MIB, 1.0, 1000, 0)
        na, _ = e.need('a.o', 1000, 0)
        nb, _ = e.need('b.o', 1000, MIB)
        e.need('c.cpp.o', 10, 0, default=GIB)
        e.record('a.o', na, 1.0, 1000, 0)
        e.record('b.o', 2 * nb, 1.0, 1000, MIB)
        s = e.stats()
        self.assertEqual((s['learned'], s['model'], s['default'], s['measured']), (1, 1, 1, 2))
        self.assertAlmostEqual(s['median'], 1.5)
        self.assertAlmostEqual(s['worst'], 2.0)
        self.assertEqual(s['worst_name'], 'b.o')

    def test_only_a_peak_is_recorded(self):
        e = memgate.Estimates()
        e.record('a.o', 0, 1.0, 10, 10)
        self.assertEqual(e.objects, {})

    def test_save_and_load(self):
        path = self.tmp / 'sub' / 'estimates.json'
        e = memgate.Estimates.load(path)
        self.assertEqual(e.objects, {})
        e.record('a.o', 123 * MIB, 4.5, 1000, 900, switch_fn=400)
        e.save()
        self.assertEqual(sorted(p.name for p in path.parent.iterdir()), ['estimates.json'])
        data = json.loads(path.read_text())
        self.assertEqual(data, {'version': 1, 'objects': {'a.o': {'peak': 123 * MIB, 'seconds': 4.5,
                                                                  'src_bytes': 1000, 'cost': 900,
                                                                  'switch_fn': 400}}})
        self.assertEqual(memgate.Estimates.load(path).objects, e.objects)

    def test_save_is_atomic(self):
        path = self.tmp / 'estimates.json'
        e = memgate.Estimates(path)
        e.record('a.o', MIB, 1.0, 1, 1)
        e.save()
        before = path.read_text()
        e.record('b.o', MIB, 1.0, 1, 1)
        with mock.patch.object(memgate.os, 'replace', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                e.save()
        self.assertEqual(path.read_text(), before)
        self.assertEqual([p.name for p in self.tmp.iterdir()], ['estimates.json'])

    def test_a_refused_rename_is_retried_then_skipped(self):
        path = self.tmp / 'estimates.json'
        e = memgate.Estimates(path)
        e.record('a.o', MIB, 1.0, 1, 1)
        self.assertTrue(e.save())
        before = path.read_text()
        e.record('b.o', MIB, 1.0, 1, 1)
        calls = []

        def refuse(*args):
            calls.append(args)
            raise PermissionError('in use')

        with mock.patch.object(memgate, 'REPLACE_TRIES', (0.01, 0.01)), \
                mock.patch.object(memgate.os, 'replace', side_effect=refuse):
            self.assertFalse(e.save())
        self.assertEqual(len(calls), 3)
        self.assertEqual(path.read_text(), before)
        self.assertEqual([p.name for p in self.tmp.iterdir()], ['estimates.json'])
        with mock.patch.object(memgate.os, 'replace', side_effect=[PermissionError('in use'), None]) as replace:
            self.assertTrue(e.save())
        self.assertEqual(replace.call_count, 2)

    def test_threads_share_it(self):
        e = memgate.Estimates(self.tmp / 'estimates.json')
        errors = []

        def work(t):
            try:
                for i in range(200):
                    name = f'{t}-{i % 20}.o'
                    e.need(name, 1000, 1000, switch_fn=i)
                    e.record(name, (i + 1) * MIB, 1.0, 1000, 1000, switch_fn=i)
                    e.scale()
                    if i % 50 == 0:
                        e.stats()
                        e.save()
            except Exception as x:   # noqa: BLE001 - any error fails the test
                errors.append(x)

        threads = [threading.Thread(target=work, args=(t,)) for t in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(30)
        self.assertEqual(errors, [])
        self.assertEqual(len(e.objects), 160)
        self.assertEqual(e.stats()['measured'], 160)
        e.save()
        self.assertEqual(len(memgate.Estimates.load(self.tmp / 'estimates.json').objects), 160)

    def test_garbled_files_are_empty(self):
        path = self.tmp / 'estimates.json'
        for text in ('{not json', '[]', '{"version": 2, "objects": {}}', '{"version": 1, "objects": []}', '\xff'):
            path.write_text(text, encoding='latin-1')
            self.assertEqual(memgate.Estimates.load(path).objects, {}, text)
        path.write_bytes(b'\xff\xfe\x00')
        self.assertEqual(memgate.Estimates.load(path).objects, {})
        path.write_text(json.dumps({'version': 1, 'objects': {
            'good.o': {'peak': 5, 'seconds': 1, 'src_bytes': 2, 'cost': 3},   # from before switch_fn: 0
            'bad.o': {'peak': 'lots'}, 'worse.o': 7}}))
        self.assertEqual(memgate.Estimates.load(path).objects,
                         {'good.o': {'peak': 5, 'seconds': 1.0, 'src_bytes': 2, 'cost': 3, 'switch_fn': 0}})


class RunTest(unittest.TestCase):
    def test_exact_peak_of_a_real_child(self):
        seen = []
        r = memgate.run([sys.executable, '-c', ALLOC], on_start=lambda p: seen.append(p.pid))
        self.assertEqual(r.returncode, 0)
        self.assertEqual(len(seen), 1)
        self.assertGreater(r.seconds, 0)
        if sys.platform.startswith('linux'):
            self.assertGreaterEqual(r.peak_bytes, 40 * MIB)
            self.assertLess(r.peak_bytes, 200 * MIB)
        else:
            self.assertGreater(r.peak_bytes, 0)

    def test_peak_counts_a_waited_for_grandchild(self):
        if not hasattr(os, 'wait4'):
            self.skipTest('POSIX only')
        inner = f'import subprocess, sys; subprocess.run([sys.executable, "-c", {ALLOC!r}])'
        r = memgate.run([sys.executable, '-c', inner])
        self.assertGreaterEqual(r.peak_bytes, 40 * MIB)

    def test_exit_code_and_stderr_tail(self):
        code = 'import sys; sys.stdout.write("o" * 100000); sys.stderr.write("a" * 3000 + "THE END"); sys.exit(3)'
        r = memgate.run([sys.executable, '-c', code])
        self.assertEqual(r.returncode, 3)
        self.assertEqual(len(r.stderr_tail), memgate.STDERR_TAIL)
        self.assertTrue(r.stderr_tail.endswith('aTHE END'))

    def test_a_large_output_does_not_block(self):
        code = 'import sys; sys.stdout.write("o" * 3000000); sys.stderr.write("e" * 3000000)'
        r = memgate.run([sys.executable, '-c', code])
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stderr_tail, 'e' * memgate.STDERR_TAIL)

    @unittest.skipUnless(hasattr(os, 'wait4'), 'POSIX only')
    def test_without_wait4_status_the_exit_code_stays(self):
        """SIGCHLD ignored: wait4 finds no child to wait for. The code comes from Popen, the peak is unknown."""
        with mock.patch.object(memgate.os, 'wait4', side_effect=ChildProcessError):
            r = memgate.run([sys.executable, '-c', 'import sys; sys.exit(5)'])
        self.assertEqual((r.returncode, r.peak_bytes), (5, 0))

    @unittest.skipUnless(hasattr(os, 'wait4'), 'POSIX only')
    def test_a_descendant_holding_the_pipes_does_not_hang_it(self):
        with tempfile.TemporaryDirectory() as td:
            pidfile = Path(td) / 'pid'
            code = ('import subprocess, sys\n'
                    'p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"])\n'
                    f'open({str(pidfile)!r}, "w").write(str(p.pid))\n'
                    'sys.stderr.write("left behind\\n")\n')
            start = time.monotonic()
            try:
                with mock.patch.object(memgate, 'PIPE_GRACE', 0.5):
                    r = memgate.run([sys.executable, '-c', code])
            finally:
                try:
                    os.kill(int(pidfile.read_text()), signal.SIGKILL)
                except (OSError, ValueError):
                    pass
            self.assertLess(time.monotonic() - start, 10)
            self.assertEqual(r.returncode, 0)
            self.assertEqual(r.stderr_tail, 'left behind\n')

    @unittest.skipUnless(hasattr(os, 'wait4'), 'POSIX only')
    def test_a_signal_is_a_negative_code(self):
        r = memgate.run([sys.executable, '-c', 'import os, signal; os.kill(os.getpid(), signal.SIGTERM)'])
        self.assertEqual(r.returncode, -15)


@LINUX
class AvailableTest(unittest.TestCase):
    def test_windows_counts_the_commit_left(self):
        """On Windows the free memory is no more than what can still be committed (a small page file runs out first)."""
        with mock.patch.object(memgate, 'WINDOWS', True), \
                mock.patch.object(memgate.hostmem, 'available_ram', return_value=10 * GIB):
            with mock.patch.object(memgate, '_commit_left', return_value=3 * GIB):
                self.assertEqual(memgate.available(), 3 * GIB)
            with mock.patch.object(memgate, '_commit_left', return_value=30 * GIB):
                self.assertEqual(memgate.available(), 10 * GIB)
            with mock.patch.object(memgate, '_commit_left', side_effect=OSError):
                self.assertEqual(memgate.available(), 10 * GIB)
        with mock.patch.object(memgate, 'WINDOWS', True), \
                mock.patch.object(memgate.hostmem, 'available_ram', return_value=None), \
                mock.patch.object(memgate, '_commit_left', return_value=3 * GIB):
            self.assertEqual(memgate.available(), 3 * GIB)

    def test_elsewhere_it_is_hostmem_s(self):
        with mock.patch.object(memgate, 'WINDOWS', False), \
                mock.patch.object(memgate.hostmem, 'available_ram', return_value=7 * GIB), \
                mock.patch.object(memgate, '_commit_left', side_effect=AssertionError('not on Linux')):
            self.assertEqual(memgate.available(), 7 * GIB)
        self.assertIs(memgate.Gate(1).__dict__['_avail'], memgate.available)


class RssNowTest(unittest.TestCase):
    def test_a_process_and_its_descendants(self):
        child = f'{ALLOC}import sys; sys.stdout.write("ready\\n"); sys.stdout.flush(); sys.stdin.read()'
        parent = (f'import subprocess, sys; p = subprocess.Popen([sys.executable, "-c", {child!r}], stdin=sys.stdin); '
                  'p.wait()')
        p = subprocess.Popen([sys.executable, '-c', parent], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        try:
            self.assertEqual(p.stdout.readline().strip(), b'ready')
            self.assertGreaterEqual(memgate.rss_now(p.pid), 40 * MIB)
        finally:
            p.stdin.close()
            p.wait(10)
            p.stdout.close()
        self.assertEqual(memgate.rss_now(p.pid), 0)
        self.assertEqual(memgate.rss_now(None), 0)

    def test_a_reused_pid_is_not_alive(self):
        me = os.getpid()
        started = memgate.start_time(me)
        self.assertIsInstance(started, int)
        self.assertTrue(memgate._alive(me, started))
        self.assertTrue(memgate._alive(me))
        self.assertFalse(memgate._alive(me, started + 1))
        p = subprocess.Popen([sys.executable, '-c', 'pass'])
        p.wait()
        self.assertFalse(memgate._alive(p.pid))
        self.assertIsNone(memgate.start_time(p.pid))


class OptionsTest(unittest.TestCase):
    def parse(self, argv, env=None):
        import argparse
        parser = argparse.ArgumentParser()
        memgate.add_arguments(parser)
        clean = {k: v for k, v in os.environ.items() if not k.startswith('NX2SARIA_BUILD_')}
        with mock.patch.dict(os.environ, dict(clean, **(env or {})), clear=True):
            return memgate.settings(parser.parse_args(argv))

    def test_defaults(self):
        with mock.patch.object(memgate, 'total_ram', return_value=64 * GIB):
            s = self.parse([])
        self.assertEqual(s, {'min_jobs': 1, 'reserve': int(6.4 * GIB), 'ceiling': None, 'enabled': True})
        self.assertEqual(memgate.default_reserve(8 * GIB), 3 * GIB // 2)

    def test_options_then_environment(self):
        env = {'NX2SARIA_BUILD_MEM_LIMIT': '20', 'NX2SARIA_BUILD_MEM_RESERVE': '2.5', 'NX2SARIA_BUILD_MIN_JOBS': '2'}
        self.assertEqual(self.parse([], env), {'min_jobs': 2, 'reserve': int(2.5 * GIB), 'ceiling': 20 * GIB,
                                               'enabled': True})
        self.assertEqual(self.parse(['--mem-limit', '10', '--mem-reserve', '1', '--min-jobs', '3'], env),
                         {'min_jobs': 3, 'reserve': GIB, 'ceiling': 10 * GIB, 'enabled': True})

    def test_gate_off(self):
        self.assertFalse(self.parse(['--no-mem-gate'])['enabled'])
        self.assertFalse(self.parse([], {'NX2SARIA_BUILD_MEM_GATE': 'off'})['enabled'])
        self.assertTrue(self.parse([], {'NX2SARIA_BUILD_MEM_GATE': 'on'})['enabled'])

    def test_a_bad_number_is_an_error(self):
        with self.assertRaises(SystemExit):
            self.parse([], {'NX2SARIA_BUILD_MIN_JOBS': 'many'})

    def test_from_args(self):
        import argparse
        parser = argparse.ArgumentParser()
        memgate.add_arguments(parser)
        g = memgate.from_args(parser.parse_args(['--min-jobs', '2', '--mem-reserve', '1']), 6, log=lambda s: None)
        self.addCleanup(g.close)
        self.assertEqual((g.max_jobs, g.min_jobs, g.reserve, g.ceiling, g.enabled), (6, 2, GIB, None, True))


STAMP = ('import sys, time\n'
         'from pathlib import Path\n'
         'out = Path(sys.argv[1])\n'
         'out.with_suffix(".start").write_text(repr(time.time()))\n'
         'if len(sys.argv) > 4:\n'   # wait for a file another process makes
         '    other, end = Path(sys.argv[4]), time.monotonic() + 15\n'
         '    while not other.exists():\n'
         '        if time.monotonic() > end:\n'
         '            sys.exit(9)\n'
         '        time.sleep(0.01)\n'
         'time.sleep(float(sys.argv[2]))\n'
         'out.with_suffix(".end").write_text(repr(time.time()))\n'
         'sys.stdout.write("out of " + out.stem + "\\n")\n'
         'sys.stderr.write("err of " + out.stem + "\\n")\n'
         'sys.exit(int(sys.argv[3]))\n')


class WrapTest(Tmp):
    """Wrappers at once over one state folder, as Ninja runs them."""

    def setUp(self):
        super().setUp()
        self.state = self.tmp / 'gate'
        self.script = self.tmp / 'stamp.py'
        self.script.write_text(STAMP)
        self.procs = []
        self.addCleanup(self.kill_all)

    def kill_all(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
                p.communicate()

    def prepare(self, enabled, needs):
        memgate.prepare_state(self.state, 4, {'min_jobs': 1, 'reserve': 0, 'ceiling': None, 'enabled': enabled},
                              needs)

    def spawn(self, name, seconds=0, rc=0, wait_for=None, cmd=None):
        if cmd is None:
            cmd = [sys.executable, str(self.script), str(self.tmp / name), str(seconds), str(rc)]
            if wait_for is not None:
                cmd.append(str(wait_for))
        p = subprocess.Popen([sys.executable, str(ROOT / 'tools' / 'memgate.py'), 'wrap', '--state', str(self.state),
                              '--name', name, '--', *cmd], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(p)
        return p

    def finish(self, procs, timeout=30):
        """(exit code, stdout, stderr) of each; a wrapper still running after timeout seconds fails the test."""
        out = []
        end = time.monotonic() + timeout
        for p in procs:
            try:
                o, e = p.communicate(timeout=max(0.1, end - time.monotonic()))
            except subprocess.TimeoutExpired:
                self.kill_all()
                self.fail(f'a wrapper still runs after {timeout} s')
            out.append((p.returncode, o.decode(), e.decode()))
        return out

    def started(self, name):
        return (self.tmp / name).with_suffix('.start').exists()

    def span(self, name):
        return (float((self.tmp / name).with_suffix('.start').read_text()),
                float((self.tmp / name).with_suffix('.end').read_text()))

    def ledger(self):
        return json.loads((self.state / memgate.LEDGER).read_text())

    def waiting(self):
        try:
            return [e['name'] for e in self.ledger()['waiting']]
        except (OSError, ValueError, KeyError):
            return []

    def test_gate_off_runs_both_at_once(self):
        # Each compile waits until the other has started: both end well only if they ran at the same time.
        self.prepare(False, {'a.o': 1 << 50, 'b.o': 1 << 50})
        results = self.finish([self.spawn('a.o', wait_for=self.tmp / 'b.start'),
                               self.spawn('b.o', wait_for=self.tmp / 'a.start')])
        self.assertEqual([r[0] for r in results], [0, 0], results)
        peaks = memgate.read_peaks(self.state)
        self.assertEqual(sorted(p['name'] for p in peaks), ['a.o', 'b.o'])
        self.assertTrue(all(p['rc'] == 0 and p['held'] == 0 and 'peak_commit' in p for p in peaks), peaks)

    def test_a_huge_need_serialises_them(self):
        self.prepare(True, {'a.o': 1 << 50, 'b.o': 1 << 50})
        results = self.finish([self.spawn('a.o', 0.3), self.spawn('b.o', 0.3)])
        self.assertEqual([r[0] for r in results], [0, 0], results)
        (a0, a1), (b0, b1) = self.span('a.o'), self.span('b.o')
        self.assertTrue(a1 <= b0 or b1 <= a0, 'one compile ran after the other')
        peaks = memgate.read_peaks(self.state)
        self.assertEqual(len(peaks), 2)
        self.assertTrue(all(p['peak'] > 0 for p in peaks), peaks)
        self.assertEqual(sum(p['held'] > 0 for p in peaks), 1, peaks)
        self.assertEqual((self.ledger()['running'], self.ledger()['waiting']), ([], []))

    def test_waiters_keep_their_order(self):
        """A small compile arriving after a held big one waits behind it, though the memory would hold it now."""
        self.prepare(True, {'a.o': MIB, 'big.o': 1 << 50, 'c.o': MIB})
        release = self.tmp / 'release'
        a = self.spawn('a.o', wait_for=release)
        self.assertTrue(wait_until(lambda: self.started('a.o'), 15))
        big = self.spawn('big.o', 0.3)
        self.assertTrue(wait_until(lambda: self.waiting() == ['big.o'], 15))
        c = self.spawn('c.o')
        self.assertTrue(wait_until(lambda: self.waiting() == ['big.o', 'c.o'], 15))
        time.sleep(0.5)
        self.assertFalse(self.started('c.o'))
        self.assertFalse(self.started('big.o'))   # a runs: alone the big one may not start yet
        release.write_text('')
        results = self.finish([a, big, c])
        self.assertEqual([r[0] for r in results], [0, 0, 0], results)
        (a0, a1), (big0, big1), (c0, c1) = self.span('a.o'), self.span('big.o'), self.span('c.o')
        self.assertLessEqual(a1, big0)
        self.assertLessEqual(big1, c0)   # the big one, once running, owes all its need: c waits for it to end
        held = {p['name']: p['held'] for p in memgate.read_peaks(self.state)}
        self.assertGreater(held['c.o'], 0.5)

    def test_a_failing_compile_passes_its_code_and_output(self):
        self.prepare(True, {'a.o': MIB, 'b.o': MIB})
        results = self.finish([self.spawn('a.o'), self.spawn('b.o', rc=7)])
        self.assertEqual(results[0][0], 0)
        self.assertEqual(results[1][0], 7)
        self.assertEqual(results[1][1], 'out of b\n')
        self.assertEqual(results[1][2], 'err of b\n')
        peaks = {p['name']: p for p in memgate.read_peaks(self.state)}
        self.assertEqual((peaks['a.o']['rc'], peaks['b.o']['rc']), (0, 7))

    @unittest.skipUnless(hasattr(signal, 'SIGTERM') and os.name == 'posix', 'POSIX signals')
    def test_a_killed_compile_exits_as_a_shell_says(self):
        self.prepare(True, {})
        code = 'import os, signal; os.kill(os.getpid(), signal.SIGTERM)'
        results = self.finish([self.spawn('a.o', cmd=[sys.executable, '-c', code])])
        self.assertEqual(results[0][0], 128 + signal.SIGTERM)
        self.assertEqual(memgate.read_peaks(self.state)[0]['rc'], -signal.SIGTERM)

    def test_a_missing_compiler(self):
        self.prepare(True, {'a.o': MIB})
        results = self.finish([self.spawn('a.o', cmd=[str(self.tmp / 'no-such-compiler')])])
        self.assertEqual(results[0][0], 127)
        self.assertIn('cannot run', results[0][2])
        self.assertEqual((self.ledger()['running'], self.ledger()['waiting']), ([], []))

    def test_a_missing_state_folder_is_made(self):
        self.state = self.tmp / 'new' / 'gate'
        results = self.finish([self.spawn('a.o')])
        self.assertEqual(results[0][0], 0, results)
        self.assertEqual([p['name'] for p in memgate.read_peaks(self.state)], ['a.o'])

    def test_ended_and_reused_entries_are_dropped(self):
        self.prepare(True, {'a.o': 1 << 50, 'b.o': 1 << 50})
        dead = subprocess.Popen([sys.executable, '-c', 'pass'])
        dead.wait()
        running = [{'pid': dead.pid, 'need': 1 << 50, 'ticket': 0, 'name': 'gone.o'}]
        started = memgate.start_time(os.getpid())
        if started is not None:
            # this test's own pid, as a wrapper that started at another time would leave it
            running.append({'pid': os.getpid(), 'start': started + 1, 'need': 1 << 50, 'ticket': 1,
                            'name': 'reused.o'})
        (self.state / memgate.LEDGER).write_text(json.dumps({'next': 5, 'running': running, 'waiting': [
            {'pid': dead.pid, 'need': 1 << 50, 'ticket': 2, 'name': 'gone-waiting.o'}]}))
        results = self.finish([self.spawn('a.o'), self.spawn('b.o')], timeout=20)
        self.assertEqual([r[0] for r in results], [0, 0])
        self.assertEqual((self.ledger()['running'], self.ledger()['waiting']), ([], []))

    @LINUX
    def test_a_long_wait_is_said_once(self):
        self.prepare(True, {'n.o': GIB})
        holder = subprocess.Popen([sys.executable, '-c', 'import sys; sys.stdin.read()'], stdin=subprocess.PIPE)
        self.procs.append(holder)
        self.assertTrue(wait_until(lambda: memgate.start_time(holder.pid) is not None))
        (self.state / memgate.LEDGER).write_text(json.dumps({'next': 1, 'waiting': [], 'running': [
            {'pid': holder.pid, 'start': memgate.start_time(holder.pid), 'need': 1 << 50, 'ticket': 0,
             'name': 'holder.o'}]}))
        err = io.TextIOWrapper(io.BytesIO(), write_through=True)
        rc = []
        with mock.patch.object(memgate, 'WAIT_NOTE', 0.2), mock.patch.object(sys, 'stderr', err):
            t = threading.Thread(target=lambda: rc.append(memgate.wrap(
                self.state, 'n.o', [sys.executable, '-c', 'pass'], poll=0.02)), daemon=True)
            t.start()
            self.assertTrue(wait_until(lambda: b'waits for memory' in err.buffer.getvalue()))
            time.sleep(0.3)
            holder.stdin.close()
            holder.wait(10)
            t.join(15)
        self.assertEqual(rc, [0])
        lines = err.buffer.getvalue().decode().splitlines()
        self.assertEqual(len(lines), 1, lines)
        self.assertRegex(lines[0], r'^memgate: n\.o waits for memory \(needs 1\.0 GiB, \d+\.\d GiB free, '
                                   r'\d+\.\d GiB promised to 1 running\)$')
        self.assertGreater(memgate.read_peaks(self.state)[0]['held'], 0.2)


class CgroupLimitTest(Tmp):
    """total_ram() under a cgroup v2 limit: the tightest memory.max on the way up, so the reserve and the rule for
    unknown free memory go by what the build may use."""

    def fake(self, limits):
        proc, cg = self.tmp / 'proc', self.tmp / 'cg'
        (proc / 'self').mkdir(parents=True)
        (proc / 'self' / 'cgroup').write_text('0::/a/b\n')
        for rel, value in limits.items():
            d = cg / rel if rel else cg
            d.mkdir(parents=True, exist_ok=True)
            (d / 'memory.max').write_text(value + '\n')
        return mock.patch.object(memgate.hostmem, 'PROC', proc), mock.patch.object(memgate.hostmem, 'CGROUP', cg)

    @unittest.skipIf(memgate.WINDOWS, 'cgroups are Linux')
    def test_the_tightest_limit_counts(self):
        a, b = self.fake({'a': str(8 * GIB), 'a/b': str(6 * GIB)})
        with a, b, mock.patch.object(memgate.os, 'sysconf', side_effect=lambda k: {'SC_PAGE_SIZE': 4096, 'SC_PHYS_PAGES': (32 * GIB) // 4096}[k]):
            self.assertEqual(memgate.total_ram(), 6 * GIB)
            self.assertEqual(memgate.default_reserve(), int(1.5 * GIB))   # 10 % of 6 GiB is less than the minimum

    @unittest.skipIf(memgate.WINDOWS, 'cgroups are Linux')
    def test_no_limit_is_the_physical_memory(self):
        a, b = self.fake({'a': 'max', 'a/b': 'max'})
        with a, b, mock.patch.object(memgate.os, 'sysconf', side_effect=lambda k: {'SC_PAGE_SIZE': 4096, 'SC_PHYS_PAGES': (32 * GIB) // 4096}[k]):
            self.assertEqual(memgate.total_ram(), 32 * GIB)
            self.assertEqual(memgate.default_reserve(), int(3.2 * GIB))


if __name__ == '__main__':
    unittest.main()
