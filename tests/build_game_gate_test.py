"""tools/build_game.py through the memory gate: build.ninja's compile rules run tools/memgate.py wrap and the link and
rc rules stay as they were; the text is the same from one call to the next; each compile's need comes from the lift
manifest's chunk_info (or the source's size, or the runtime's default); the peaks the wrappers record are learned and
summed up; and, where ninja and clang are on PATH, a three-file Ninja build through the wrapper end to end. No game
files, no game build.

Run: python tests/build_game_gate_test.py
"""
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import build_game  # noqa: E402
import memgate  # noqa: E402

MIB = 1 << 20
GIB = 1 << 30


class Tmp(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.tmp = Path(td.name).resolve()
        self.gen = self.tmp / 'gen'
        self.out = self.tmp / 'out'
        (self.gen / 'main').mkdir(parents=True)
        self.out.mkdir()
        (self.out / 'modules.c').write_text('int x;\n')

    def chunks(self, info=None, sizes=(('main_0000.c', 300), ('main_0001.c', 200), ('main_table.c', 50))):
        files = []
        for name, size in sizes:
            f = self.gen / 'main' / name
            f.write_text('/' * size)
            files.append(f)
        manifest = {'chunks': [n for n, _ in sizes if 'table' not in n]}
        if info is not None:
            manifest['chunk_info'] = info
        (self.gen / 'main' / 'manifest.json').write_text(json.dumps(manifest))
        return {'main': files}


def rule(text, name):
    """The indented lines of one rule in build.ninja text."""
    lines = text.splitlines()
    i = lines.index(f'rule {name}')
    body = []
    for line in lines[i + 1:]:
        if not line.startswith('  '):
            break
        body.append(line.strip())
    return body


class NinjaFileTest(Tmp):
    def ninja(self, mods, **kw):
        with mock.patch.object(build_game, 'vma_include', return_value=Path('/usr/include')), \
                mock.patch.object(build_game, 'vulkan_include', return_value=None):
            return build_game.ninja_file(self.gen, self.out, mods, 6, **kw)[0]

    def test_compiles_go_through_the_wrapper(self):
        mods = self.chunks()
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('SCCACHE', None)
            text = self.ninja(mods)
        gate = build_game.gate_command(self.out)
        self.assertIn('memgate.py', gate)
        self.assertIn(' wrap --state ', gate)
        self.assertTrue(gate.endswith(' --name $out -- '))
        self.assertIn(f'command = {gate}$cc $genflags -MD -MF $out.d -c $in -o $out', rule(text, 'cc_gen'))
        self.assertIn(f'command = {gate}$cc $rtflags -MD -MF $out.d -c $in -o $out', rule(text, 'cc_rt'))
        # the link and windres rules as before
        self.assertIn('command = $cc $ldflags @$out.rsp $libs -o $out', rule(text, 'link'))
        self.assertTrue(all('memgate' not in line for line in rule(text, 'rc') + rule(text, 'link')))
        # Ninja runs at most the job count, the pool of generated compiles too
        self.assertIn('pool gen\n  depth = 6', text)
        # the same text from one call to the next: build.ninja is rewritten only when it changes
        self.assertEqual(text, self.ninja(mods))

    def test_a_compiler_cache_leaves_the_gate_out(self):
        """sccache's server runs the compiles, out of the wrapper's reach: the commands are the ones before the gate."""
        mods = self.chunks()
        with mock.patch.dict(os.environ, {'SCCACHE': 'sccache'}):
            text = self.ninja(mods)
            self.assertEqual(build_game.gate_command(self.out), '')
            line = build_game.gate_start_line({'enabled': True}, 4, {}, '')
        self.assertIn('cc = sccache ', text)
        self.assertIn('command = $cc $genflags -MD -MF $out.d -c $in -o $out', rule(text, 'cc_gen'))
        self.assertIn('command = $cc $rtflags -MD -MF $out.d -c $in -o $out', rule(text, 'cc_rt'))
        self.assertNotIn('memgate', text)
        self.assertIn("memory gate: off: the compiles run in sccache's server", line)

    def test_the_command_names_paths_below_the_root_relatively(self):
        """A moved checkout keeps build.ninja: the venv's Python and tools/memgate.py are named from the root."""
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('SCCACHE', None)
            with mock.patch.object(build_game.sys, 'executable', str(ROOT / '.venv' / 'bin' / 'python')):
                gate = build_game.gate_command(ROOT / 'build' / 'v121')
            self.assertTrue(gate.startswith('.venv/bin/python tools/memgate.py wrap --state build/v121/memgate '),
                            gate)
            with mock.patch.object(build_game.sys, 'executable', '/opt/py/bin/python3'):
                self.assertTrue(build_game.gate_command(self.out).startswith('/opt/py/bin/python3 tools/memgate.py '))

    def test_the_state_folder_is_quoted_when_it_holds_a_space(self):
        gate = build_game.gate_command(self.tmp / 'a b' / 'out')
        self.assertIn('--state "', gate)


class NeedsTest(Tmp):
    def test_costs_from_chunk_info_then_bytes_then_size(self):
        mods = self.chunks([{'file': 'main_0000.c', 'bytes': 300, 'cost': 900, 'switch_fn': 120},
                            {'file': 'main_0001.c', 'bytes': 200}])
        runtime = [(build_game.RUNTIME / 'x.c', self.out / 'obj' / 'runtime' / 'x.o'),
                   (build_game.RUNTIME / 'ui' / 'y.cpp', self.out / 'obj' / 'runtime' / 'ui' / 'y.o'),
                   (build_game.RUNTIME / 'nx2saria.rc', self.out / 'obj' / 'runtime' / 'nx2saria.o')]
        todo = build_game.gate_compiles(self.gen, self.out, mods, False, runtime)
        name = lambda o: build_game.flag_path(self.out / 'obj' / o)   # noqa: E731
        self.assertEqual(todo[name('main/main_0000.o')][1:], (300, 900, 120, None))
        self.assertEqual(todo[name('main/main_0001.o')][1:], (200, 200, 0, None))
        self.assertEqual(todo[name('main/main_table.o')][1:], (50, 50, 0, None))   # not in chunk_info: its size
        self.assertEqual(todo[name('runtime/x.o')].default, build_game.DEFAULT_NEED_C)
        self.assertEqual(todo[name('runtime/ui/y.o')].default, build_game.DEFAULT_NEED_CXX)
        self.assertNotIn(name('runtime/nx2saria.o'), todo)   # windres, not the gate
        # objects reused from the main build are not compiled
        self.assertEqual(build_game.gate_compiles(self.gen, self.out, mods, True, []), {})

    def test_needs_and_state(self):
        mods = self.chunks()
        todo = build_game.gate_compiles(self.gen, self.out, mods, False,
                                        [(self.out / 'modules.c', self.out / 'obj' / 'runtime' / 'modules.o')])
        est = memgate.Estimates(self.out / 'memgate' / 'estimates.json')
        rt = build_game.flag_path(self.out / 'obj' / 'runtime' / 'modules.o')
        est.record(rt, 200 * MIB, 1.0, todo[rt].src_bytes, 0)
        needs, hows = build_game.gate_needs(est, todo)
        self.assertEqual(hows, {'model': 3, 'learned': 1})
        self.assertEqual(needs[rt], int(200 * MIB * memgate.MARGIN))
        state = self.out / 'memgate'
        (state).mkdir(parents=True)
        (state / memgate.PEAKS).write_text('{"name": "old"}\n')
        memgate.prepare_state(state, 6, memgate.settings(mock.Mock(mem_limit=None, mem_reserve=None, min_jobs=None,
                                                                   no_mem_gate=False)), needs)
        self.assertEqual(json.loads((state / memgate.NEEDS).read_text()), needs)
        self.assertEqual(json.loads((state / memgate.CONFIG).read_text())['max_jobs'], 6)
        self.assertEqual(memgate.read_peaks(state), [])

    def test_start_line(self):
        s = {'enabled': True, 'min_jobs': 1, 'reserve': 3 * GIB // 2, 'ceiling': None}
        line = build_game.gate_start_line(s, 8, {'learned': 2, 'model': 5, 'default': 1}, '')
        self.assertIn('memory gate: on; up to 8 compiles at once, at least 1; keeps 1.5 GiB free; no ceiling', line)
        self.assertIn('2 learned from earlier builds, 5 from the size model, 1 set at 400 MiB (C) or 1 GiB (C++)', line)
        off = build_game.gate_start_line(dict(s, enabled=False), 8, {}, '--no-mem-gate')
        self.assertEqual(off, 'memory gate: off (--no-mem-gate); up to 8 compiles at once')
        with mock.patch.dict(os.environ, {memgate.ENV_GATE: 'Off'}):
            self.assertEqual(build_game.gate_off_why(mock.Mock(no_mem_gate=False)), f'{memgate.ENV_GATE}=Off')
        with mock.patch.dict(os.environ, {memgate.ENV_GATE: ''}):
            self.assertEqual(build_game.gate_off_why(mock.Mock(no_mem_gate=False)), '')


class LearnTest(Tmp):
    def test_learn_and_summary(self):
        c = build_game.Compile(Path('a.c'), 3_000_000, 3_000_000, 0, None)
        todo = {f'o{i}': c for i in range(7)}
        est = memgate.Estimates(self.tmp / 'e.json')
        for name in todo:
            est.need(name, c.src_bytes, c.cost)
        peaks = [{'name': f'o{i}', 'peak': (100 + i) * MIB, 'seconds': 2.0 + i, 'held': 0.0, 'rc': 0} for i in range(6)]
        peaks[2]['held'] = 4.5
        peaks[4]['held'] = 1.5
        peaks += [{'name': 'o6', 'peak': 900 * MIB, 'seconds': 1.0, 'held': 0.0, 'rc': 1},   # failed: not learned
                  {'name': 'unknown.o', 'peak': 50 * MIB, 'seconds': 1.0, 'held': 0.0, 'rc': 0},
                  {'name': 'o0', 'peak': 0, 'seconds': 1.0, 'held': 0.0, 'rc': 0}]   # no reading
        measured = build_game.learn_peaks(est, todo, peaks)
        self.assertEqual([m[0] for m in measured], [f'o{i}' for i in range(6)])
        self.assertEqual(est.objects['o5']['peak'], 105 * MIB)
        self.assertNotIn('o6', est.objects)
        est.save()
        self.assertEqual(len(memgate.Estimates.load(self.tmp / 'e.json').objects), 6)
        sampler = build_game.TreeSampler(rss=lambda pid: 0, avail=lambda: None)
        sampler.peak, sampler.peak_at, sampler.lowest_avail = 3 * GIB, 12.0, 5 * GIB
        lines = build_game.gate_summary_lines(peaks, measured, est, sampler, True)
        self.assertEqual(lines[0], 'memory gate: 9 compiles ran, 1 failed')
        self.assertEqual(lines[1], 'memory gate: 2 compiles held for memory, 6.0 s in all; longest 4.5 s (o2)')
        self.assertEqual(lines[2], 'compile memory: peak 3.0 GiB for Ninja and all compiles together (at 12 s), '
                                   'least free memory 5.0 GiB')
        self.assertTrue(lines[3].startswith('largest compiles: o5 0.10 GiB 7 s, o4 '))
        self.assertEqual(lines[3].count(' GiB '), build_game.LARGEST_SHOWN)
        self.assertTrue(lines[4].startswith('estimates against the measured peaks: median '))
        # gate off: no held line
        off = build_game.gate_summary_lines(peaks[:1], [], est, None, False)
        self.assertEqual(off[0], 'memory gate: 1 compiles ran')
        self.assertFalse(any('held' in line for line in off))

    def test_sampler(self):
        now = [0.0]
        rss = iter([GIB, 3 * GIB, 2 * GIB])
        avail = iter([8 * GIB, 4 * GIB, 6 * GIB])
        s = build_game.TreeSampler(rss=lambda pid: next(rss), avail=lambda: next(avail), poll=3600,
                                   clock=lambda: now[0])
        s.start(1)
        for t in (1.0, 2.0, 3.0):
            now[0] = t
            s.sample(1)
        s.stop()
        self.assertEqual((s.peak, s.peak_at, s.lowest_avail), (3 * GIB, 2.0, 4 * GIB))
        broken = build_game.TreeSampler(rss=lambda pid: 1 // 0, avail=lambda: GIB, poll=3600)
        broken.start(1)
        broken.sample(1)   # a failed reading loses that sample only
        broken.stop()
        self.assertIsNone(broken.peak_at)


@unittest.skipUnless(shutil.which('ninja') and shutil.which('clang'), 'needs ninja and clang on PATH')
class EndToEndTest(Tmp):
    """Three tiny C files compiled by Ninja through the wrapper, as build.ninja's cc_gen rule does: in a temporary
    folder (absolute $out), and in one below build/ (relative $out, as a real build's)."""

    def test_three_compiles(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('SCCACHE', None)
            self.three_compiles()

    def test_three_compiles_below_the_root(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        below = Path(tempfile.mkdtemp(prefix='gate-test-', dir=ROOT / 'build')).resolve()
        self.addCleanup(shutil.rmtree, below, True)
        self.gen, self.out = below / 'gen', below / 'out'
        (self.gen / 'main').mkdir(parents=True)
        self.out.mkdir()
        self.assertFalse(Path(build_game.flag_path(self.out)).is_absolute())
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('SCCACHE', None)
            self.three_compiles()

    def three_compiles(self):
        files = []
        for i in range(3):
            f = self.gen / 'main' / f'c{i}.c'
            f.write_text(f'int f{i}(int x) {{ return x * {i + 2}; }}\n')
            files.append(f)
        mods = {'main': files}
        todo = build_game.gate_compiles(self.gen, self.out, mods, False, [])
        est = memgate.Estimates.load(self.out / 'memgate' / 'estimates.json')
        needs, _ = build_game.gate_needs(est, todo)
        memgate.prepare_state(self.out / 'memgate', 2, {'min_jobs': 1, 'reserve': 0, 'ceiling': None,
                                                         'enabled': True}, needs)
        objs = [self.out / 'obj' / 'main' / f'c{i}.o' for i in range(3)]
        text = '\n'.join([
            f'builddir = {build_game.ninja_path(self.out)}',   # the Ninja log there, not in the repository
            f'cc = {shutil.which("clang")}',
            'rule cc_gen',
            f'  command = {build_game.gate_command(self.out)} $cc -O1 -MD -MF $out.d -c $in -o $out',
            '  deps = gcc',
            '  depfile = $out.d',
            ''] + [f'build {build_game.ninja_path(o)}: cc_gen {build_game.ninja_path(f)}' for o, f in zip(objs, files)]
            + [''])
        (self.out / 'build.ninja').write_text(text)
        sampler = build_game.TreeSampler(poll=0.01)
        env = {'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'HOME': os.environ.get('HOME', str(self.tmp))}
        code, _, peak_one, _ = build_game.run_measured(['ninja', '-f', str(self.out / 'build.ninja'), '-j', '2'], env,
                                                       on_start=sampler.start)
        sampler.stop()
        self.assertEqual(code, 0)
        self.assertGreater(peak_one, 0)
        self.assertTrue(all(o.is_file() for o in objs))
        peaks = memgate.read_peaks(self.out / 'memgate')
        self.assertEqual(sorted(p['name'] for p in peaks), sorted(todo))   # $out is the name needs.json files it under
        measured = build_game.learn_peaks(est, todo, peaks)
        self.assertEqual(len(measured), 3)
        self.assertTrue(all(peak > 0 for _, peak, _ in measured))
        est.save()
        self.assertEqual(set(memgate.Estimates.load(self.out / 'memgate' / 'estimates.json').objects), set(todo))


if __name__ == '__main__':
    unittest.main()
