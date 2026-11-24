import argparse
import contextlib
import datetime
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import bench  # noqa: E402


class BuildRevisionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.game = Path(self.tmp.name) / 'test-build'
        self.game.mkdir()
        (self.game / 'ocarina').write_bytes(b'test executable')
        self.stamp = datetime.datetime.fromisoformat('2026-11-20T01:02:03-03:00').timestamp()
        os.utime(self.game / 'ocarina', (self.stamp, self.stamp))
        self.log = self.game / 'build-commits.txt'
        self.first = 'a' * 40
        self.second = 'b' * 40

    def record(self, sha=None, name='test-build', rc=0, stamp='2026-11-20T01:02:03-03:00'):
        return f'{sha or self.first} {name} {stamp} rc={rc}\n'

    def test_last_record_must_be_successful_and_match_output(self):
        self.assertIsNone(bench.recorded_build_commit(self.game))
        self.log.write_text(self.record() + self.record(self.second))
        self.assertEqual(bench.recorded_build_commit(self.game), self.second)
        self.log.write_text(self.record() + self.record(self.second, rc=1))
        self.assertIsNone(bench.recorded_build_commit(self.game))
        self.log.write_text(self.record(name='another-build'))
        self.assertIsNone(bench.recorded_build_commit(self.game))
        self.log.write_text(self.record() + 'interrupted\n')
        self.assertIsNone(bench.recorded_build_commit(self.game))

    def test_requires_full_object_id_and_valid_timestamp(self):
        for sha in ('a' * 39, 'g' * 40, '0' * 40, 'A' * 40):
            with self.subTest(sha=sha):
                self.log.write_text(self.record(sha))
                self.assertIsNone(bench.recorded_build_commit(self.game))
        self.log.write_text(self.record('c' * 64))
        self.assertEqual(bench.recorded_build_commit(self.game), 'c' * 64)
        for stamp in ('not-a-date', '2026-11-20T01:02:03'):
            self.log.write_text(self.record(stamp=stamp))
            self.assertIsNone(bench.recorded_build_commit(self.game))
        self.log.write_text(self.record())
        (self.game / 'ocarina').unlink()
        self.assertIsNone(bench.recorded_build_commit(self.game))

    def test_executable_newer_than_record_is_unknown(self):
        self.log.write_text(self.record())
        exe = self.game / 'ocarina'
        os.utime(exe, (self.stamp + 1, self.stamp + 1))
        self.assertEqual(bench.recorded_build_commit(self.game), self.first)
        exe.write_bytes(b'replacement executable')
        os.utime(exe, (self.stamp + 2, self.stamp + 2))
        self.assertIsNone(bench.recorded_build_commit(self.game))

    def test_metadata_and_summary_distinguish_build_from_checkout(self):
        self.log.write_text(self.record(self.first))
        out = Path(self.tmp.name) / 'results'
        args = argparse.Namespace(game=str(self.game), out=str(out), label='baseline', scenes='unused', only=None,
                                  set=[], runs=0, shot=False, boot_map=True, mangohud=False)
        def fake_git(*args):
            return self.second if args[-1] == 'HEAD' else 'c' * 40
        with mock.patch.object(bench, 'git', side_effect=fake_git), mock.patch.object(bench, 'machine', return_value={}), \
             mock.patch.object(bench, 'load_scenes', return_value=[]), contextlib.redirect_stdout(io.StringIO()):
            bench.cmd_run(args)
        meta = json.loads((out / 'meta.json').read_text())
        self.assertEqual(meta['build_commit'], self.first)
        self.assertEqual(meta['head'], self.second)
        summary = (out / 'summary.md').read_text()
        self.assertIn('Build commit: ' + self.first[:10], summary)
        self.assertIn('Checkout: HEAD ' + self.second[:10], summary)
        meta.pop('build_commit')
        (out / 'meta.json').write_text(json.dumps(meta))
        with contextlib.redirect_stdout(io.StringIO()):
            bench.summarize(out)
        self.assertIn('Build commit: unknown', (out / 'summary.md').read_text())


if __name__ == '__main__':
    unittest.main()
