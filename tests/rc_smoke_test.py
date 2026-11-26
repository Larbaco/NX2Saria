import json
import os
import sys
import tempfile
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
            self.assertLessEqual(set(got), {'run', 'crash-launcher', 'update'}, p.name)
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
                         ['install', 'build', 'run', 'crash', 'privacy'])
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


if __name__ == '__main__':
    unittest.main()
