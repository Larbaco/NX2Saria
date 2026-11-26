import fcntl
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import rc_smoke  # noqa: E402

CRASH = ('FATAL: access violation (0xc0000005) at host 0x7f12a4c01234 (ocarina+0x1c01234), reading address 0x0')


class Pieces(unittest.TestCase):
    def test_version_order(self):
        k = rc_smoke.version_key
        self.assertLess(k('0.26.0'), k('0.27.0-rc.1'))
        self.assertLess(k('0.27.0-rc.1'), k('0.27.0-rc.2'))
        self.assertLess(k('0.27.0-rc.2'), k('0.27.0'))
        self.assertEqual(k('nonsense'), ())
        self.assertEqual(rc_smoke.tag_version('v0.27.0-rc.1'), '0.27.0-rc.1')

    def test_sums(self):
        text = ('a' * 64 + '  NX2Saria-Linux-0.27.0-rc.1-x86_64.AppImage\n' + 'b' * 64 + ' *SHA256SUMS-other.txt\n'
                + 'not a line\n')
        self.assertEqual(rc_smoke.sums_for(text, 'NX2Saria-Linux-0.27.0-rc.1-x86_64.AppImage'), 'a' * 64)
        self.assertEqual(rc_smoke.sums_for(text, 'SHA256SUMS-other.txt'), 'b' * 64)
        self.assertIsNone(rc_smoke.sums_for(text, 'missing.zip'))

    def test_expect_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'x.txt'
            p.write_text('run: telemetry: session header written\n\n# a note\nupdate: Translate the Code: Unchanged\n'
                         'run: second\n', encoding='utf-8')
            got = rc_smoke.read_expect(p)
        self.assertEqual(got, {'run': ['telemetry: session header written', 'second'],
                               'update': ['Translate the Code: Unchanged']})
        self.assertEqual(rc_smoke.read_expect(Path('/nonexistent/x.txt')), {})
        self.assertEqual(rc_smoke.missing_lines('a b c', ['b', 'd']), ['d'])

    def test_every_version_file_parses(self):
        for p in sorted((ROOT / 'tools' / 'data' / 'rc-smoke').glob('*.txt')):
            got = rc_smoke.read_expect(p)
            self.assertTrue(got, p.name)
            self.assertLessEqual(set(got), {'run', 'crash-launcher', 'update', 'window-update'}, p.name)
            self.assertTrue(rc_smoke.version_key(p.stem), p.name)

    def test_sent_lines(self):
        log = '\n'.join([
            '01:00:00 telemetry: no server configured; nothing is sent',
            '01:00:01 telemetry: session reports are off',
            '01:00:02 telemetry: session report sent (HTTP 201, 812 bytes)',
            '01:00:03 telemetry: crash log sent (HTTP 200, 40 + 200 lines, 9000 bytes)',
            '01:00:04 telemetry: comment sent (HTTP 201, 30 bytes)',
            '01:00:05 telemetry: session report not sent: no answer (HTTP 503)',
            '01:00:06 a summary was made from its FATAL line, with the hardware',
            '01:00:07 game: the game sends nothing',
        ])
        got = rc_smoke.sent_lines(log)
        self.assertEqual([ln[:8] for ln in got], ['01:00:02', '01:00:03', '01:00:04', '01:00:05'])

    def test_fatal_lines(self):
        text = '\n'.join(['[host] boot: started', CRASH, '[1.5] FATAL: probe-fatal-busy: a fatal error'])
        self.assertEqual(rc_smoke.fatal_lines(text), [CRASH, 'FATAL: probe-fatal-busy: a fatal error'])
        self.assertEqual(rc_smoke.fatal_problem(CRASH), '')
        self.assertEqual(rc_smoke.fatal_problem(''), 'no FATAL line in the log')
        self.assertIn('another log line', rc_smoke.fatal_problem(
            'FATAL: access violation (0xc0000005) at host 0x7f12 ([host] log: cut'))
        self.assertIn('[line cut]', rc_smoke.fatal_problem('FATAL: something long [line cut]'))
        self.assertIn('does not end', rc_smoke.fatal_problem(
            'FATAL: access violation (0xc0000005) at host 0x7f12 (ocarina+0x1'))
        self.assertEqual(rc_smoke.fatal_problem(
            'FATAL: access violation (0xc0000005) at host 0x7f12 (outside every module)'), '')

    def test_summary(self):
        fatal = CRASH[len('FATAL: '):]
        good = {'session': {'end': 'crash'}, 'fatal': fatal, 'cpu': {'name': 'AMD Ryzen 5 5600'},
                'gpu': {'name': 'AMD Radeon RX 6700'}, 'ram_mb': 32000}
        self.assertEqual(rc_smoke.summary_problems(good, fatal, True), [])
        bare = dict(good, cpu={'name': None}, gpu={'name': None}, ram_mb=None)
        self.assertEqual(len(rc_smoke.summary_problems(bare, fatal, True)), 3)
        self.assertEqual(rc_smoke.summary_problems(bare, fatal, False), [])
        self.assertEqual(rc_smoke.summary_problems(None, fatal, True), ['no logs/session-summary.json'])
        other = dict(good, session={'end': 'normal'}, fatal='something else')
        self.assertEqual(len(rc_smoke.summary_problems(other, fatal, True)), 2)

    def test_png(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('no PIL')
        with tempfile.TemporaryDirectory() as d:
            flat, busy, tiny = Path(d) / 'flat.png', Path(d) / 'busy.png', Path(d) / 'tiny.png'
            Image.new('RGB', (200, 200), (10, 20, 30)).save(flat, compress_level=0)
            im = Image.new('RGB', (200, 200), (10, 20, 30))
            for i in range(200):
                im.putpixel((i, i), (250, 250, 250))
            im.save(busy, compress_level=0)
            tiny.write_bytes(b'\x89PNG')
            self.assertEqual(rc_smoke.png_problem(flat), 'one colour only')
            self.assertEqual(rc_smoke.png_problem(busy), '')
            self.assertEqual(rc_smoke.png_problem(tiny), '4 bytes')
            self.assertEqual(rc_smoke.png_problem(Path(d) / 'none.png'), 'missing')

    def test_stages(self):
        def args(**kw):
            base = {'quick': False, 'only': None, 'skip': None}
            base.update(kw)
            return type('A', (), base)()
        self.assertEqual(rc_smoke.chosen(args()), list(rc_smoke.STAGES))
        self.assertNotIn('update', rc_smoke.chosen(args(quick=True)))
        self.assertEqual(rc_smoke.chosen(args(only='crash,install')), ['install', 'crash'])
        self.assertEqual(rc_smoke.chosen(args(quick=True, skip='window')),
                         ['install', 'cli', 'build', 'run', 'crash', 'privacy'])
        self.assertIn('cli', rc_smoke.QUICK)
        self.assertNotIn('window-update', rc_smoke.QUICK)
        self.assertEqual(rc_smoke.chosen(args(only='privacy,window-update')), ['window-update', 'privacy'])
        for stage in rc_smoke.STAGES:
            self.assertTrue(callable(getattr(rc_smoke.Smoke, 'stage_' + stage.replace('-', '_'))), stage)
        with self.assertRaises(SystemExit):
            rc_smoke.chosen(args(only='nope'))

    def test_env_stays_inside(self):
        with tempfile.TemporaryDirectory() as d:
            a = type('A', (), {'tag': 'v9.9.9-rc.1', 'root': d, 'bench': d, 'games': os.path.join(d, 'g'), 'logs': d,
                               'expect_file': None, 'expect': []})()
            old = os.environ.get('NX2SARIA_TELEMETRY_URL')
            os.environ['NX2SARIA_TELEMETRY_URL'] = 'https://example.invalid'
            try:
                s = rc_smoke.Smoke(a)
                env = s.env('clean')
            finally:
                if old is None:
                    os.environ.pop('NX2SARIA_TELEMETRY_URL', None)
                else:
                    os.environ['NX2SARIA_TELEMETRY_URL'] = old
            for key in ('HOME', 'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_STATE_HOME'):
                self.assertTrue(env[key].startswith(d), key)
            self.assertEqual(env['NX2SARIA_TELEMETRY_URL'], 'none')
            self.assertEqual(s.root('clean'), Path(d) / 'clean' / 'home' / '.local' / 'share' / 'nx2saria')
            self.assertEqual(json.loads(json.dumps(s.expect)), {})


def smoke(d, **kw):
    base = {'tag': 'v9.9.9-rc.1', 'root': d, 'bench': d, 'games': os.path.join(d, 'g'), 'logs': d,
            'expect_file': None, 'expect': [], 'pin_url': False, 'lock_held': False, 'ini': [], 'run_env': []}
    base.update(kw)
    s = rc_smoke.Smoke(type('A', (), base)())
    s.runs.mkdir(parents=True, exist_ok=True)
    return s


class Options(unittest.TestCase):
    def test_ancestors_and_locks_table(self):
        self.assertEqual(rc_smoke.ancestors()[0], os.getpid())
        self.assertIn(os.getppid(), rc_smoke.ancestors())
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / 'lock'
            f.touch()
            st = os.stat(f)
            ident = f'{os.major(st.st_dev):02x}:{os.minor(st.st_dev):02x}:{st.st_ino}'
            table = Path(d) / 'locks'
            table.write_text(f'1: POSIX  ADVISORY  WRITE 11 {ident} 0 EOF\n'
                             f'2: FLOCK  ADVISORY  WRITE 22 {ident} 0 EOF\n'
                             f'2: -> FLOCK  ADVISORY  WRITE 33 {ident} 0 EOF\n'
                             f'3: FLOCK  ADVISORY  WRITE 44 00:00:1 0 EOF\n', encoding='ascii')
            self.assertEqual(rc_smoke.lock_holders(f, table), [22])

    def test_ini_with(self):
        text = '# kept\nfps_limit = 60\nfoliage_distance = 1\n  foliage_distance=3\n'
        got = rc_smoke.ini_with(text, [('foliage_distance', '2'), ('pause_settings_tab', '1')])
        self.assertEqual(got, '# kept\nfps_limit = 60\nfoliage_distance = 2\npause_settings_tab = 1\n')
        self.assertEqual(rc_smoke.ini_with('', [('a', 'b')]), 'a = b\n')

    def test_pairs(self):
        self.assertEqual(rc_smoke.pairs(['A=1', ' b = two words '], '--ini'), [('A', '1'), ('b', 'two words')])
        for bad in ('novalue', '=1', 'a b=1'):
            with self.assertRaises(SystemExit):
                rc_smoke.pairs([bad], '--ini')

    def test_feed(self):
        with tempfile.TemporaryDirectory() as d:
            s = smoke(d)
            s._releases = [{'tag_name': 'v9.9.9-rc.2', 'assets': [{'name': rc_smoke.LINUX_MANIFEST}]},
                           {'tag_name': 'v9.9.9-rc.1', 'assets': [{'name': rc_smoke.LINUX_MANIFEST}]}]
            with self.assertRaises(rc_smoke.Failed) as e:
                s.feed()
            self.assertIn('v9.9.9-rc.2 is', str(e.exception))
            s._releases.pop(0)
            self.assertIn('real GitHub feed', s.feed())
            s.a.pin_url = True
            self.assertIn('pinned URL', s.feed())

    def test_lock_held(self):
        with tempfile.TemporaryDirectory() as d:
            s = smoke(d, lock_held=True)
            self.assertFalse(s.take_lock())
            with open(s.lock_path, 'a') as held:
                fcntl.flock(held, fcntl.LOCK_EX)
                self.assertEqual(rc_smoke.lock_holders(s.lock_path), [os.getpid()])
                self.assertTrue(s.take_lock())
            self.assertIsNone(s.lock)
            s.release_lock()
            other = subprocess.Popen([sys.executable, '-c', 'import fcntl, sys, time; f = open(sys.argv[1], "a"); '
                                      'fcntl.flock(f, fcntl.LOCK_EX); print("held", flush=True); time.sleep(30)',
                                      str(s.lock_path)], stdout=subprocess.PIPE, text=True)
            try:
                self.assertEqual(other.stdout.readline().strip(), 'held')
                self.assertEqual(rc_smoke.lock_holders(s.lock_path), [other.pid])
                self.assertFalse(s.take_lock())
            finally:
                other.kill()
                other.wait()
            free = smoke(d)
            self.assertTrue(free.take_lock())
            free.release_lock()

    def test_build_frees_the_other_game_folders(self):
        for keep in (False, True):
            with tempfile.TemporaryDirectory() as d:
                s = smoke(d, keep_game=keep)
                for which in rc_smoke.ROOTS:
                    (s.game(which) / 'ocarina').parent.mkdir(parents=True)
                    (s.game(which) / 'ocarina').write_text('x', encoding='utf-8')

                def stop():
                    raise rc_smoke.Failed('stop')
                s.wait_for_disk = stop
                with self.assertRaises(rc_smoke.Failed):
                    s.build('winupd', 'b')
                left = sorted(w for w in rc_smoke.ROOTS if s.game(w).exists())
                self.assertEqual(left, ['clean', 'update'] if keep else [])

    def test_watch_stops_the_whole_tree(self):
        child = ('import subprocess, sys, time; '
                 'g = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], start_new_session=True); '
                 'print("grandchild", g.pid, flush=True); time.sleep(60)')
        old = rc_smoke.WATCH_EVERY
        rc_smoke.WATCH_EVERY = 0.2
        try:
            with tempfile.TemporaryDirectory() as d:
                s = smoke(d)
                calls = []

                def watch():
                    calls.append(1)
                    return 'stopped the build: 1.0 GB left' if len(calls) >= 3 else ''
                t0 = time.time()
                code, out = s.run_logged('w', [sys.executable, '-c', child], dict(os.environ), watch=watch)
                self.assertLess(time.time() - t0, 30)
                self.assertEqual(code, 125)
                self.assertEqual(s.stopped, 'stopped the build: 1.0 GB left')
                grand = int(re.search(r'grandchild (\d+)', out).group(1))
                self.assertFalse(Path(f'/proc/{grand}').exists() and
                                 Path(f'/proc/{grand}/stat').read_text().split()[2] != 'Z')
                code, out = s.run_logged('t', [sys.executable, '-c', 'import time; time.sleep(60)'], dict(os.environ),
                                         timeout=0.5)
                self.assertEqual(code, 124)
                code, out = s.run_logged('ok', [sys.executable, '-c', 'print("fine")'], dict(os.environ),
                                         watch=lambda: '')
                self.assertEqual((code, s.stopped), (0, ''))
                self.assertIn('fine', out)
        finally:
            rc_smoke.WATCH_EVERY = old

    def test_root_deleted_only_after_a_pass(self):
        for results, keep, kept in (([('a', True, '', 0)], False, False), ([('a', True, '', 0), ('b', False, '', 0)],
                                    False, True), ([('a', True, '', 0)], True, True), ([], False, True)):
            with tempfile.TemporaryDirectory() as d:
                s = smoke(d, keep_root=keep, keep_game=False)
                s.base = Path(d) / 'root'
                (s.base / 'clean').mkdir(parents=True)
                s.results = results
                s.cleanup()
                self.assertEqual(s.base.exists(), kept, (results, keep))
                self.assertEqual(bool(s.notes), kept, (results, keep))

    def fake_install(self, d, shadow):
        s = smoke(d)
        root = s.root('clean')
        launcher = root / 'app' / '9.9.9-rc.1' / 'launcher'
        launcher.mkdir(parents=True)
        (root / 'app' / 'current.txt').write_text('9.9.9-rc.1\n', encoding='utf-8')
        (root / 'python' / 'bin').mkdir(parents=True)
        os.symlink(sys.executable, root / 'python' / 'bin' / 'python3')
        (launcher / 'cli.py').write_text('import platform, sys\nplatform.release()\n'
                                         'print("up to date: 9.9.9-rc.1" if sys.argv[-1] == "check" else "ok")\n',
                                         encoding='utf-8')
        if shadow:
            (launcher / 'platform').mkdir()
            (launcher / 'platform' / '__init__.py').write_text('', encoding='utf-8')
        return s

    def test_cli_stage_runs_without_isolation(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIn('check exit 0, tools exit 0', self.fake_install(d, False).stage_cli())
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(rc_smoke.Failed) as e:
                self.fake_install(d, True).stage_cli()
            self.assertIn('without -I', str(e.exception))
            self.assertIn("no attribute 'release'", str(e.exception))


if __name__ == '__main__':
    unittest.main()
