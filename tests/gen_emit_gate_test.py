# needs: py:numpy, py:lz4
"""recomp/gen.py's emission under the memory rule of the compile gate: lift_gate follows the gate's settings (off:
reserve 0; a malformed reserve: the default, with a warning), pick_jobs sizes the pool by the free memory less the
reserve, and dispatch() submits a chunk only while the free memory stays at or above the reserve, keeps one chunk
outstanding whatever the memory, never more than the pool's workers, raises a task's error and gives up on a worker
that died with its task. emit_all retries with half the workers after a MemoryError on exactly the chunks without a
result. A fake pool and a fake free memory reading; no game files. Run with the .venv (numpy):

    .venv/bin/python tests/gen_emit_gate_test.py
"""
import contextlib
import io
import os
import sys
import threading
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'recomp'))

import gen  # noqa: E402

GIB = 1 << 30


class PickJobsTest(unittest.TestCase):
    def pick(self, free, default_reserve, cpus=32, requested=None, env=None, **kw):
        with mock.patch.dict(os.environ, env or {}, clear=True), \
                mock.patch.object(gen.memgate, 'available', return_value=free), \
                mock.patch.object(gen.memgate, 'default_reserve', return_value=default_reserve), \
                mock.patch.object(gen.os, 'cpu_count', return_value=cpus):
            return gen.pick_jobs(requested, **kw)


    def test_share_of_free_memory(self):
        # 19 GiB free: 80 % (15.2) is less than free less a 3 GiB reserve (16): 10 workers of 1.5 GiB
        jobs, why = self.pick(19 * GIB, 3 * GIB)
        self.assertEqual(jobs, 10, why)
        self.assertIn('19.0 GiB free memory allows 10 with a 3.0 GiB reserve', why)

    def test_reserve_binds_when_free_memory_is_small(self):
        # 8 GiB free: 80 % is 6.4 GiB, free less a 3 GiB reserve 5 GiB: 3 workers
        self.assertEqual(self.pick(8 * GIB, 3 * GIB)[0], 3)

    def test_at_least_one_and_at_most_the_cpus(self):
        self.assertEqual(self.pick(2 * GIB, 3 * GIB)[0], 1)
        self.assertEqual(self.pick(512 * GIB, 3 * GIB, cpus=6)[0], 6)

    def test_unknown_free_memory(self):
        self.assertEqual(self.pick(None, 3 * GIB, cpus=12), (12, '12 CPUs, free memory unknown'))

    def test_reserve_given_is_used(self):
        with mock.patch.object(gen, 'lift_gate', side_effect=AssertionError('not asked')):
            self.assertEqual(self.pick(8 * GIB, 3 * GIB, reserve=0)[0], 4)   # 80 % of 8 GiB: 4 workers

    def test_jobs_given_are_kept(self):
        self.assertEqual(self.pick(1 * GIB, 3 * GIB, requested=7), (7, '--jobs'))
        self.assertEqual(self.pick(1 * GIB, 3 * GIB, env={'NX2SARIA_LIFT_JOBS': '5'}), (5, 'NX2SARIA_LIFT_JOBS'))


class LiftGateTest(unittest.TestCase):
    def gate(self, env):
        err = io.StringIO()
        with mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(gen.memgate, 'default_reserve', return_value=3 * GIB), \
                contextlib.redirect_stderr(err):
            return gen.lift_gate(), err.getvalue()

    def test_default_and_set_reserve(self):
        self.assertEqual(self.gate({}), ((True, 3 * GIB, ''), ''))
        self.assertEqual(self.gate({'NX2SARIA_BUILD_MEM_RESERVE': '2'})[0], (True, 2 * GIB, ''))

    def test_off_means_no_reserve(self):
        for value in ('off', '0', 'no', 'false'):
            self.assertEqual(self.gate({'NX2SARIA_BUILD_MEM_GATE': value})[0],
                             (False, 0, 'NX2SARIA_BUILD_MEM_GATE off'))

    def test_malformed_reserve_keeps_the_default(self):
        (on, reserve, why), err = self.gate({'NX2SARIA_BUILD_MEM_RESERVE': 'lots'})
        self.assertEqual((on, reserve), (True, 3 * GIB))
        self.assertIn("ignoring the memory gate settings (NX2SARIA_BUILD_MEM_RESERVE='lots' is not a number)", err)
        (on, reserve, why), err = self.gate({'NX2SARIA_BUILD_MEM_RESERVE': 'lots', 'NX2SARIA_BUILD_MEM_GATE': 'off'})
        self.assertEqual((on, reserve), (False, 0))

    def test_gate_off_never_holds(self):
        # reserve 0 from lift_gate: even no free memory at all lets every worker have a chunk
        pool = FakePool(0, 0)
        results, stats = run_dispatch(pool, jobs=4, reserve=0)
        self.assertEqual(len(results), 12)
        self.assertEqual((pool.most, stats.held), (4, 0))
        self.assertIn('(gate off: NX2SARIA_BUILD_MEM_GATE off)', stats.line(0, 'NX2SARIA_BUILD_MEM_GATE off'))


class FakePool:
    """apply_async runs the task on a timer thread after delay seconds. The free memory it reports (avail) falls by
    per for each task outstanding."""

    def __init__(self, base, per, delay=0.02, never=()):
        self.base, self.per, self.delay = base, per, delay
        self.never = list(never)   # tasks whose worker dies: no result, no error, and the pool replaces it
        self.pids = frozenset({1, 2})
        self.lock = threading.Lock()
        self.outstanding = self.most = 0
        self.submitted = []

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def avail(self):
        if self.base is None:
            return None
        with self.lock:
            return self.base - self.per * self.outstanding

    def apply_async(self, fn, args, callback, error_callback):
        with self.lock:
            self.outstanding += 1
            self.most = max(self.most, self.outstanding)
            self.submitted.append(args[0])
        if args[0] in self.never:
            with self.lock:
                self.pids = frozenset({1, 3})
            return

        def run():
            try:
                r = fn(*args)
            except BaseException as e:   # noqa: BLE001 - handed back as Pool hands back a task's exception
                with self.lock:
                    self.outstanding -= 1
                error_callback(e)
                return
            with self.lock:
                self.outstanding -= 1
            callback(r)
        threading.Timer(self.delay, run).start()


def run_dispatch(pool, jobs, reserve, tasks=range(12), fn=lambda t: t * 10, **kw):
    """(sorted results, GateStats) of dispatch() over tasks on pool, with the pool's own free memory reading."""
    results, stats = [], gen.GateStats()
    gen.dispatch(pool, fn, list(tasks), jobs, reserve, results.append, stats, available=pool.avail, poll=0.01, **kw)
    return sorted(results), stats


class DispatchTest(unittest.TestCase):
    def run_dispatch(self, *a, **kw):
        return run_dispatch(*a, **kw)

    def test_holds_below_the_reserve(self):
        # free 10, 3 per task, reserve 4: 0, 1 and 2 outstanding leave 10, 7 and 4 (submit), 3 would leave 1 (hold)
        pool = FakePool(10, 3)
        results, stats = self.run_dispatch(pool, jobs=8, reserve=4)
        self.assertEqual(results, [t * 10 for t in range(12)])
        self.assertEqual(pool.most, 3)
        self.assertGreaterEqual(stats.held, 1)
        self.assertGreater(stats.held_s, 0)
        self.assertEqual(stats.least, 1)

    def test_always_one_outstanding(self):
        # below the reserve from the start: one chunk at a time, never none
        pool = FakePool(1, 1)
        results, stats = self.run_dispatch(pool, jobs=4, reserve=100, tasks=range(5))
        self.assertEqual(results, [t * 10 for t in range(5)])
        self.assertEqual(pool.most, 1)
        self.assertGreaterEqual(stats.held, 1)

    def test_never_more_than_jobs(self):
        pool = FakePool(10**12, 1)
        results, stats = self.run_dispatch(pool, jobs=3, reserve=1)
        self.assertEqual(len(results), 12)
        self.assertEqual(pool.most, 3)
        self.assertEqual(stats.held, 0)

    def test_unknown_memory_does_not_hold(self):
        pool = FakePool(None, 0)
        results, stats = self.run_dispatch(pool, jobs=4, reserve=10**12)
        self.assertEqual(len(results), 12)
        self.assertEqual(pool.most, 4)
        self.assertEqual((stats.held, stats.least), (0, None))
        self.assertIn('least free unknown, dispatch held 0 times', stats.line(GIB))

    def test_task_error_is_raised(self):
        def fn(t):
            if t == 3:
                raise MemoryError
            return t

        with self.assertRaises(MemoryError):
            self.run_dispatch(FakePool(10**12, 1), jobs=2, reserve=1, fn=fn)

    def test_a_worker_lost_with_its_task_fails(self):
        pool = FakePool(10**12, 1, never={5})
        pids = iter([{1, 2}] * 3 + [{1, 3}] * 10**6)   # the pool replaced worker 2
        with self.assertRaisesRegex(gen.WorkerLost, r'an emit worker ended without a result \(killed for memory\?\): '
                                                    r'1 chunks outstanding'):
            self.run_dispatch(pool, jobs=3, reserve=1, workers=lambda p: next(pids), lost_after=0.1)

    def test_a_slow_task_with_the_same_workers_waits(self):
        pool = FakePool(10**12, 1, delay=0.2)
        results, _ = self.run_dispatch(pool, jobs=2, reserve=1, tasks=range(3), workers=lambda p: {1, 2},
                                       lost_after=0.05)
        self.assertEqual(results, [0, 10, 20])

    def test_empty(self):
        results, stats = self.run_dispatch(FakePool(10, 1), jobs=2, reserve=1, tasks=[])
        self.assertEqual(results, [])

    def test_line(self):
        stats = gen.GateStats()
        for a in (9 * GIB, None, 5 * GIB // 2, 7 * GIB):
            stats.note(a)
        stats.held, stats.held_s = 3, 12.4
        self.assertEqual(stats.line(3 * GIB // 2),
                         'emit memory: least free 2.5 GiB, dispatch held 3 times, 12 s in all (reserve 1.5 GiB)')


class EmitAllTest(unittest.TestCase):
    def test_memory_error_retries_the_chunks_without_a_result(self):
        tasks = [('mod', n, f'mod_{n:04d}.c', []) for n in range(20)]
        pools, results, before = [], [], []

        def make_pool(jobs):
            before.append({r['number'] for r in results})   # the chunks with a result when this pool starts
            pools.append((jobs, FakePool(10**12, 1, delay=0.01)))
            return pools[-1][1]

        def fn(t):
            if len(pools) == 1 and t[1] == 9:
                raise MemoryError
            return {'module': t[0], 'number': t[1]}

        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            gen.emit_all(tasks, 4, 'test', make_pool, fn, 1, results.append, gen.GateStats(), poll=0.01)
        numbers = sorted(r['number'] for r in results)
        self.assertEqual(numbers, list(range(20)))   # each chunk's result once
        self.assertEqual([j for j, _ in pools], [4, 2])
        retried = [t[1] for t in pools[1][1].submitted]
        self.assertEqual(len(retried), len(set(retried)))      # no chunk twice in the second pool
        self.assertEqual(set(retried), set(range(20)) - before[1])   # exactly the chunks without a result
        self.assertIn(9, retried)
        self.assertIn('emit: out of memory with 4 workers, retrying with 2', err.getvalue())
        self.assertIn('emitting with 2 workers (after out of memory)', err.getvalue())

    def test_memory_error_with_one_worker_is_raised(self):
        def fn(t):
            raise MemoryError

        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(MemoryError):
            gen.emit_all([('mod', 0)], 1, 'test', lambda j: FakePool(10**12, 1), fn, 1, lambda r: None,
                         gen.GateStats(), poll=0.01)


    def lost(self, jobs, never):
        """(results, pools as (jobs, FakePool), stderr) of emit_all over 12 chunks where the first pool's worker dies
        with each chunk of never."""
        tasks = [('mod', n) for n in range(12)]
        pools, results = [], []

        def make_pool(n):
            pools.append((n, FakePool(10**12, 1, delay=0.01, never=never if not pools else ())))
            return pools[-1][1]

        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            gen.emit_all(tasks, jobs, 'test', make_pool, lambda t: {'module': t[0], 'number': t[1]}, 1,
                         results.append, gen.GateStats(), poll=0.01, workers=lambda p: p.pids, lost_after=0.1)
        return results, pools, err.getvalue()

    def test_a_lost_worker_retries_with_half_the_workers(self):
        results, pools, err = self.lost(4, [('mod', 5)])
        self.assertEqual(sorted(r['number'] for r in results), list(range(12)))   # each chunk's result once
        self.assertEqual([j for j, _ in pools], [4, 2])
        self.assertIn(('mod', 5), pools[1][1].submitted)
        self.assertIn('emit: an emit worker ended without a result (killed for memory?) with 4 workers, retrying '
                      'with 2', err)
        self.assertIn('emitting with 2 workers (after a lost worker)', err)

    def test_a_lost_worker_with_one_worker_is_raised(self):
        with self.assertRaisesRegex(gen.WorkerLost, r'an emit worker ended without a result \(killed for memory\?\)'):
            self.lost(1, [('mod', 3)])


if __name__ == '__main__':
    unittest.main()
