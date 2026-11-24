import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class WorkerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2-worker-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'launcher'
        (self.root / 'app').mkdir(parents=True)
        (self.root / 'app' / 'current.txt').write_text('0.0.1')
        (self.root / 'data').mkdir()
        (self.root / 'data' / 'launcher.json').write_text('{}')

    def tree(self):
        result = {}
        for path in self.root.rglob('*'):
            name = str(path.relative_to(self.root))
            if not path.is_file():
                result[name] = None
                continue
            try:
                result[name] = path.read_bytes()
            except PermissionError:
                if path.name != 'launcher.lock':
                    raise
                stat = path.stat()
                result[name] = ('locked', stat.st_size, stat.st_mtime_ns)
        return result

    def run_worker(self, request, root=None):
        return subprocess.run([sys.executable, '-m', 'launcher.worker', '--root', str(root or self.root)],
                              input=json.dumps(request) + '\n', text=True, capture_output=True,
                              cwd=ROOT, timeout=15)

    def test_protocol_mismatch_does_not_write_to_installation(self):
        before = self.tree()
        result = self.run_worker({'id': 1, 'method': 'hello', 'protocol': 999})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'protocol_mismatch')
        self.assertEqual(self.tree(), before)

    def test_valid_handshake_inspection_and_idle_eof(self):
        frames = [{'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'inspect'}]
        result = subprocess.run([sys.executable, '-m', 'launcher.worker', '--root', str(self.root)],
                                input=''.join(json.dumps(frame) + '\n' for frame in frames), text=True,
                                capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        hello, inspection = map(json.loads, result.stdout.splitlines())
        self.assertEqual(hello['id'], 1)
        self.assertEqual(hello['result']['protocol'], 1)
        self.assertEqual(hello['result']['lock'], 'held')
        self.assertIn('inspect', hello['result']['capabilities'])
        self.assertFalse(hello['result']['update_waiting'])
        self.assertEqual(inspection['id'], 2)
        self.assertEqual(inspection['result']['status'], 'not_configured')
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: started', log)
        self.assertIn('protocol=1', log)
        self.assertIn('lock=held', log)
        self.assertIn('worker: inspect', log)
        self.assertIn('worker: eof idle', log)

    def test_malformed_settings_are_rejected_without_recovery_writes(self):
        (self.root / 'data' / 'launcher.json').write_text('{not valid json')
        before = self.tree()
        result = self.run_worker({'id': 1, 'method': 'hello', 'protocol': 1})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'invalid_installation')
        self.assertEqual(self.tree(), before)

    def test_unrecognized_root_is_not_changed(self):
        (self.root / 'app' / 'current.txt').unlink()
        before = self.tree()
        result = self.run_worker({'id': 1, 'method': 'hello', 'protocol': 1})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'invalid_root')
        self.assertEqual(self.tree(), before)

    def start_worker(self):
        process = subprocess.Popen([sys.executable, '-m', 'launcher.worker', '--root', str(self.root)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, process)
        process.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        process.stdin.flush()
        return process, json.loads(process.stdout.readline())

    @staticmethod
    def stop_worker(process):
        if process.stdin and not process.stdin.closed:
            process.stdin.close()
        if process.poll() is None:
            process.wait(timeout=15)
        process.stdout.close()
        process.stderr.close()

    def test_second_worker_and_mutating_cli_are_refused_then_lock_is_released(self):
        worker, hello = self.start_worker()
        self.assertEqual(hello['result']['lock'], 'held')
        before = self.tree()
        second = self.run_worker({'id': 3, 'method': 'hello', 'protocol': 1})
        self.assertEqual(self.tree(), before)
        self.assertEqual(json.loads(second.stdout)['error']['code'], 'busy')
        self.assertNotEqual(second.returncode, 0)
        cli = subprocess.run([sys.executable, '-m', 'launcher.cli', '--root', str(self.root), 'tools'],
                             text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(cli.returncode, 1)
        self.assertIn('using', cli.stderr.lower())
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        successor = self.run_worker({'id': 4, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

    def test_unopenable_lock_is_visible_and_logged(self):
        script = ("import builtins,runpy,sys\n"
                  "original=builtins.open\n"
                  "def deny(path,*a,**kw):\n"
                  " if str(path).endswith('launcher.lock'): raise PermissionError('synthetic')\n"
                  " return original(path,*a,**kw)\n"
                  "builtins.open=deny\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "runpy.run_module('launcher.worker',run_name='__main__')\n")
        result = subprocess.run([sys.executable, '-c', script, str(self.root)],
                                input=json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n',
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['result']['lock'], 'unavailable')
        self.assertIn('lock=unavailable', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_steam_play_remains_available_when_idle_and_refuses_active_job(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        exe = game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')
        exe.touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        worker, hello = self.start_worker()
        self.assertEqual(hello['result']['lock'], 'held')
        script = ("import sys\n"
                  "from unittest import mock\n"
                  "from launcher.context import Context\n"
                  "from launcher.paths import Layout\n"
                  "from launcher.ui import app\n"
                  "ctx=Context(Layout(root=sys.argv[1]))\n"
                  "end=app.GameEnd('ended','','',False)\n"
                  "with mock.patch.object(app.mods,'prepare',return_value=[]), "
                  "mock.patch.object(app.build,'can_rebuild',return_value=False), "
                  "mock.patch.object(app.commit,'confirm_play_memory',return_value=True), "
                  "mock.patch.object(app.winsys,'start_game') as spawn, "
                  "mock.patch.object(app,'GameRun') as run, "
                  "mock.patch.object(app.time,'sleep'), mock.patch.object(app,'tell'):\n"
                  " run.return_value.running.return_value=False\n"
                  " run.return_value.end.return_value=end\n"
                  " app.play_only(ctx)\n"
                  " print('started' if spawn.called else 'refused')\n")
        def steam():
            return subprocess.run([sys.executable, '-c', script, str(self.root)], text=True,
                                  capture_output=True, cwd=ROOT, timeout=15, check=True).stdout.strip()
        self.assertEqual(steam(), 'started')
        marker = self.root / 'data' / 'job-running.json'
        marker.write_text(json.dumps({'task': 'synthetic job', 'kind': 'rebuild', 'started': 1}))
        self.assertEqual(steam(), 'refused')
        self.assertTrue(marker.exists())

    def test_repeated_actions_have_bounded_logging(self):
        worker, hello = self.start_worker()
        for request_id in range(20):
            worker.stdin.write(json.dumps({'id': request_id, 'method': 'inspect'}) + '\n')
            worker.stdin.flush()
            self.assertEqual(json.loads(worker.stdout.readline())['id'], request_id)
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertEqual(log.count('worker: inspect status='), 10)
        self.assertEqual(log.count('worker: inspect further events suppressed'), 1)

    def test_waiting_update_is_reported_without_touching_staged_files(self):
        staged = self.root / 'app' / '999.0.0'
        (staged / 'launcher').mkdir(parents=True)
        (staged / 'launcher' / 'start.pyw').write_text('synthetic')
        (staged / 'VERSION').write_text('999.0.0')
        (staged / 'staged.json').write_text(json.dumps({'version': '999.0.0', 'runtime': ''}))
        before = {str(p.relative_to(staged)): p.read_bytes() for p in staged.rglob('*') if p.is_file()}
        result = self.run_worker({'id': 1, 'method': 'hello', 'protocol': 1})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)['result']['update_waiting'])
        self.assertEqual({str(p.relative_to(staged)): p.read_bytes() for p in staged.rglob('*') if p.is_file()}, before)
        self.assertIn('update_waiting=True', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_invalid_frames_are_rejected_before_writing(self):
        for frame in ('not json\n', '{"id":true,"method":"hello","protocol":1}\n',
                      '{"id":1,"method":"hello","protocol":true}\n',
                      '{"id":1,"method":"hello","protocol":1,"extra":NaN}\n',
                      '[' * 1100 + '0' + ']' * 1100 + '\n', 'x' * 65537 + '\n'):
            with self.subTest(frame=frame[:40]):
                before = self.tree()
                result = subprocess.run([sys.executable, '-m', 'launcher.worker', '--root', str(self.root)],
                                        input=frame, text=True, capture_output=True, cwd=ROOT, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(json.loads(result.stdout)['error']['code'], ('invalid_request', 'protocol_mismatch'))
                self.assertEqual(self.tree(), before)

    def test_unknown_method_returns_structured_error(self):
        worker, hello = self.start_worker()
        worker.stdin.write(json.dumps({'id': 'two', 'method': 'unexpected'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response, {'id': 'two', 'error': {'code': 'unknown_method'}})


if __name__ == '__main__':
    unittest.main(verbosity=2)
