import json
import http.server
import threading
import os
import shutil
import subprocess
import sys
import tempfile
import time
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

    def test_core_stdout_does_not_corrupt_protocol_frames(self):
        script = ("import runpy,sys\n"
                  "from launcher import inspect_installation as inspection\n"
                  "original=inspection.inspect_installation\n"
                  "def noisy(*args,**kwargs):\n"
                  " print('synthetic library output',flush=True)\n"
                  " return original(*args,**kwargs)\n"
                  "inspection.inspect_installation=noisy\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "runpy.run_module('launcher.worker',run_name='__main__')\n")
        frames = [{'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'inspect'}]
        result = subprocess.run([sys.executable, '-c', script, str(self.root)],
                                input=''.join(json.dumps(frame) + '\n' for frame in frames),
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([json.loads(line)['id'] for line in result.stdout.splitlines()], [1, 2])
        self.assertIn('synthetic library output', result.stderr)

    def test_play_without_built_game_reports_refusal(self):
        worker, hello = self.start_worker()
        self.assertIn('play', hello['result']['capabilities'])
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        result = json.loads(worker.stdout.readline())
        self.assertEqual(result, {'id': 2, 'result': {'status': 'not_built'}})
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertIn('worker: play status=not_built', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_play_emits_confirmed_start_and_end_without_another_request(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(0.5)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'started')
        started = json.loads(worker.stdout.readline())
        ended = json.loads(worker.stdout.readline())
        self.assertEqual((started['event'], ended['event']), ('game_started', 'game_ended'))
        self.assertEqual((started['sequence'], ended['sequence']), (1, 2))
        self.assertEqual(started['session'], ended['session'])
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)

    def test_play_refuses_external_game(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(0.5)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=True), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'external_game')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertNotIn('worker: spawn started', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_second_play_is_refused_while_game_is_running(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(2)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'started')
        started = json.loads(worker.stdout.readline())
        self.assertEqual(started['event'], 'game_started')
        worker.stdin.write(json.dumps({'id': 3, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['result']['status'], 'own_game')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_ended')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertEqual(log.count('worker: spawn started'), 1)

    def test_session_summary_is_preserved_without_network(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        hits = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                hits.append(self.path)
                self.send_response(200)
                self.end_headers()
            do_GET = do_POST
            def log_message(self, *args):
                pass
        server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        env = dict(os.environ, NX2SARIA_TELEMETRY_URL=f'http://127.0.0.1:{server.server_port}')
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import pathlib,sys; p=pathlib.Path(sys.argv[1])/\"logs\"; p.mkdir(exist_ok=True); (p/\"session-summary.json\").write_text(\"synthetic-summary\")',str(exe.parent)], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT, env=env)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'started')
        started = json.loads(worker.stdout.readline())
        ended = json.loads(worker.stdout.readline())
        self.assertEqual((started['event'], ended['event']), ('game_started', 'game_ended'))
        self.assertEqual((started['sequence'], ended['sequence']), (1, 2))
        self.assertEqual(started['session'], ended['session'])
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertEqual((game / 'logs' / 'session-summary.json').read_text(), 'synthetic-summary')
        self.assertEqual(hits, [])

    def test_game_session_does_not_modify_waiting_update(self):
        staged = self.root / 'app' / '999.0.0'
        (staged / 'launcher').mkdir(parents=True)
        (staged / 'launcher' / 'start.pyw').write_text('synthetic')
        (staged / 'VERSION').write_text('999.0.0')
        (staged / 'staged.json').write_text(json.dumps({'version': '999.0.0', 'runtime': ''}))
        before = {str(p.relative_to(staged)): p.read_bytes() for p in staged.rglob('*') if p.is_file()}
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(0.5)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'started')
        started = json.loads(worker.stdout.readline())
        ended = json.loads(worker.stdout.readline())
        self.assertEqual((started['event'], ended['event']), ('game_started', 'game_ended'))
        self.assertTrue(ended['update_waiting'])
        self.assertEqual({str(p.relative_to(staged)): p.read_bytes() for p in staged.rglob('*') if p.is_file()}, before)
        self.assertEqual((started['sequence'], ended['sequence']), (1, 2))
        self.assertEqual(started['session'], ended['session'])
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)

    def test_memory_decline_does_not_start_game(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(0.5)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=commit.MemoryRequest(100,None,True)):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'memory')
        self.assertEqual(response['result']['memory'], {'remaining': 100, 'need': None, 'unbounded': True})
        attempt = response['result']['attempt']
        worker.stdin.write(json.dumps({'id': 3, 'method': 'memory_response', 'attempt': attempt, 'accept': False}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['result']['status'], 'declined')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertIn('play memory: player cancelled', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_memory_accept_starts_and_follows_game(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(0.5)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=commit.MemoryRequest(100,None,True)):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'memory')
        self.assertEqual(response['result']['memory'], {'remaining': 100, 'need': None, 'unbounded': True})
        attempt = response['result']['attempt']
        worker.stdin.write(json.dumps({'id': 3, 'method': 'memory_response', 'attempt': attempt, 'accept': True}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['result']['status'], 'started')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_ended')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertIn('play memory: player chose to play anyway',
                      (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_memory_dialog_failure_continues_play(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(0.5)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=commit.MemoryRequest(100,None,True)):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'memory')
        self.assertEqual(response['result']['memory'], {'remaining': 100, 'need': None, 'unbounded': True})
        attempt = response['result']['attempt']
        worker.stdin.write(json.dumps({'id': 3, 'method': 'memory_response', 'attempt': attempt, 'dialog_error': True}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['result']['status'], 'started')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_ended')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertIn('play memory: warning dialog unavailable',
                      (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_old_memory_answer_is_rejected_without_starting(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(0.5)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=commit.MemoryRequest(100,None,True)):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'memory')
        self.assertEqual(response['result']['memory'], {'remaining': 100, 'need': None, 'unbounded': True})
        attempt = response['result']['attempt']
        worker.stdin.write(json.dumps({'id': 3, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        newer = json.loads(worker.stdout.readline())
        self.assertNotEqual(newer['result']['attempt'], attempt)
        worker.stdin.write(json.dumps({'id': 4, 'method': 'memory_response', 'attempt': attempt, 'accept': True}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['error']['code'], 'stale_or_invalid_decision')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertNotIn('play: ', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_memory_accept_refuses_if_external_game_started(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(0.5)'], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',side_effect=[False,True]), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=commit.MemoryRequest(100,None,True)):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        worker.stdin.write(json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['id'], 1)
        worker.stdin.write(json.dumps({'id': 2, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'memory')
        self.assertEqual(response['result']['memory'], {'remaining': 100, 'need': None, 'unbounded': True})
        attempt = response['result']['attempt']
        worker.stdin.write(json.dumps({'id': 3, 'method': 'memory_response', 'attempt': attempt, 'accept': True}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['result']['status'], 'external_game')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertIn('play: not started: the game is already running', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_eof_keeps_worker_and_lock_until_synthetic_game_ends(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(2)'], "
                  "stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            self.assertIn('result', json.loads(worker.stdout.readline()))
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        worker.stdin.close()
        self.assertIsNone(worker.poll())
        busy = self.run_worker({'id': 3, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(busy.stdout)['error']['code'], 'busy')
        self.assertEqual(worker.wait(timeout=15), 0)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: eof game running', log)
        self.assertIn('worker: game ended', log)
        successor = self.run_worker({'id': 4, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

    def test_killed_worker_does_not_kill_synthetic_game(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "marker=sys.argv[2]\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import pathlib,sys,time; time.sleep(1); pathlib.Path(sys.argv[1]).write_text(\"done\")',marker], "
                  "stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root), str(Path(self.tmp.name) / 'game-finished')], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            self.assertIn('result', json.loads(worker.stdout.readline()))
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        worker.kill()
        worker.wait(timeout=15)
        marker = Path(self.tmp.name) / 'game-finished'
        deadline = time.monotonic() + 10
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(marker.read_text(), 'done')
        successor = self.run_worker({'id': 4, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

    @unittest.skipIf(sys.platform == 'win32', 'synthetic executable uses a Unix script')
    def test_native_spawn_survives_killed_worker(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        exe = game / 'ocarina'
        exe.write_text('#!/usr/bin/env python3\nimport pathlib,time\ntime.sleep(1)\npathlib.Path(\"game-finished\").write_text(\"done\")\n')
        exe.chmod(0o755)
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            self.assertIn('result', json.loads(worker.stdout.readline()))
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        worker.kill()
        worker.wait(timeout=15)
        marker = game / 'game-finished'
        deadline = time.monotonic() + 10
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(marker.read_text(), 'done')
        successor = self.run_worker({'id': 4, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

    @unittest.skipUnless(sys.platform == 'win32' and os.environ.get('NX2_TEST_FAKE_GAME'), 'needs synthetic Windows executable')
    def test_windows_native_spawn_survives_killed_worker(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        shutil.copyfile(os.environ['NX2_TEST_FAKE_GAME'], game / 'ocarina.exe')
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            self.assertIn('result', json.loads(worker.stdout.readline()))
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        worker.kill()
        worker.wait(timeout=15)
        marker = game / 'game-finished'
        deadline = time.monotonic() + 10
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(marker.read_text(), 'done')
        successor = self.run_worker({'id': 4, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

    @unittest.skipUnless(sys.platform == 'win32' and os.environ.get('NX2_TEST_FAKE_GAME'), 'needs synthetic Windows executable')
    def test_windows_native_game_finishes_after_all_pipes_close(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        shutil.copyfile(os.environ['NX2_TEST_FAKE_GAME'], game / 'ocarina.exe')
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            self.assertIn('result', json.loads(worker.stdout.readline()))
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        worker.stdout.close()
        worker.stderr.close()
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertEqual((game / 'game-finished').read_text(), 'done')
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: eof game running', log)
        self.assertIn('worker: game ended', log)

    def test_broken_output_keeps_worker_and_lock_until_game_ends(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(2)'], "
                  "stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            self.assertIn('result', json.loads(worker.stdout.readline()))
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        worker.stdout.close()
        worker.stdin.write(json.dumps({'id': 5, 'method': 'inspect'}) + '\n')
        worker.stdin.flush()
        self.assertIsNone(worker.poll())
        busy = self.run_worker({'id': 3, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(busy.stdout)['error']['code'], 'busy')
        self.assertEqual(worker.wait(timeout=15), 0)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: game ended', log)
        self.assertIn('worker: game ended', log)
        successor = self.run_worker({'id': 4, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

    def test_output_broken_before_end_exits_after_game(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(2)'], "
                  "stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            self.assertIn('result', json.loads(worker.stdout.readline()))
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        worker.stdout.close()
        self.assertIsNone(worker.poll())
        busy = self.run_worker({'id': 3, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(busy.stdout)['error']['code'], 'busy')
        self.assertEqual(worker.wait(timeout=15), 0)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: game ended', log)
        self.assertIn('worker: game ended', log)
        successor = self.run_worker({'id': 4, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

    def test_invalid_frame_keeps_worker_and_lock_until_game_ends(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,subprocess,sys\n"
                  "from unittest import mock\n"
                  "from launcher import winsys,saves,mods,build,commit\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(winsys,'start_game',side_effect=lambda exe: "
                  "subprocess.Popen([sys.executable,'-c','import time; time.sleep(2)'], "
                  "stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=False), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            self.assertIn('result', json.loads(worker.stdout.readline()))
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        worker.stdin.write('invalid json\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['error']['code'], 'invalid_request')
        worker.stdin.close()
        self.assertIsNone(worker.poll())
        busy = self.run_worker({'id': 3, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(busy.stdout)['error']['code'], 'busy')
        self.assertEqual(worker.wait(timeout=15), 1)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: invalid request', log)
        self.assertIn('worker: game ended', log)
        successor = self.run_worker({'id': 4, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

    def test_mod_rebuild_success_resumes_play_after_finished(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,sys\n"
                  "from types import SimpleNamespace\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,mods,build,saves,commit,play,game_session,winsys\n"
                  "class Fake(jobs.Job):\n"
                  " claims=('game',); kind='rebuild'\n"
                  " def __init__(self,ctx,game): super().__init__(ctx,[]); self.game_dir=game; self.after='play'\n"
                  " def run(self): pass\n"
                  "run=SimpleNamespace(running=lambda:False,proc=SimpleNamespace(returncode=0))\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None), "
                  "mock.patch.object(play,'start',return_value=(run,None)), "
                  "mock.patch.object(game_session,'finish_game',return_value=SimpleNamespace(alarm=False)), "
                  "mock.patch.object(winsys,'keep_awake'):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'building')
        frames = [json.loads(worker.stdout.readline()) for _ in range(4)]
        self.assertEqual([frame['event'] for frame in frames],
                         ['job_started', 'job_ended', 'game_started', 'game_ended'])
        self.assertEqual(frames[1]['outcome'], 'resume')
        self.assertFalse((self.root / 'data' / 'job-running.json').exists())
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)

    def test_mod_rebuild_requests_memory_decision_before_play(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,sys\n"
                  "from types import SimpleNamespace\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,mods,build,saves,commit,play,game_session,winsys\n"
                  "class Fake(jobs.Job):\n"
                  " claims=('game',); kind='rebuild'\n"
                  " def __init__(self,ctx,game): super().__init__(ctx,[]); self.game_dir=game; self.after='play'\n"
                  " def run(self): pass\n"
                  "run=SimpleNamespace(running=lambda:False,proc=SimpleNamespace(returncode=0))\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']), "
                  "mock.patch.object(commit,'play_memory_request',return_value=commit.MemoryRequest(100,200,False)), "
                  "mock.patch.object(play,'start',return_value=(run,None)), "
                  "mock.patch.object(game_session,'finish_game',return_value=SimpleNamespace(alarm=False)), "
                  "mock.patch.object(winsys,'keep_awake'):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'building')
        frames = [json.loads(worker.stdout.readline()) for _ in range(3)]
        self.assertEqual([frame['event'] for frame in frames], ['job_started', 'job_ended', 'memory_required'])
        attempt = frames[2]['decision']['attempt']
        worker.stdin.write(json.dumps({'id': 3, 'method': 'memory_response', 'attempt': attempt, 'accept': False}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['result']['status'], 'declined')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertNotIn('worker: spawn started', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_failed_mod_rebuild_allows_next_play_without_rebuild(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,sys\n"
                  "from types import SimpleNamespace\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,mods,build,saves,commit,play,game_session,winsys\n"
                  "class Fake(jobs.Job):\n"
                  " claims=('game',); kind='rebuild'\n"
                  " def __init__(self,ctx,game): super().__init__(ctx,[]); self.game_dir=game; self.after='play'\n"
                  " def run(self): raise OSError(\"synthetic failure\")\n"
                  "run=SimpleNamespace(running=lambda:False,proc=SimpleNamespace(returncode=0))\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None), "
                  "mock.patch.object(play,'start',return_value=(run,None)), "
                  "mock.patch.object(game_session,'finish_game',return_value=SimpleNamespace(alarm=False)), "
                  "mock.patch.object(winsys,'keep_awake'):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'building')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'job_started')
        ended = json.loads(worker.stdout.readline())
        self.assertEqual((ended['result'], ended['outcome']), ('failed', 'failed'))
        self.assertFalse((self.root / 'data' / 'job-running.json').exists())
        worker.stdin.write(json.dumps({'id': 3, 'method': 'play'}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['result']['status'], 'started')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_started')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'game_ended')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('whose build did not finish', log)

    def test_mod_job_does_not_start_game_after_output_disconnect(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,sys,time\n"
                  "from types import SimpleNamespace\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,mods,build,saves,commit,play,game_session,winsys\n"
                  "class Fake(jobs.Job):\n"
                  " claims=('game',); kind='rebuild'\n"
                  " def __init__(self,ctx,game): super().__init__(ctx,[]); self.game_dir=game; self.after='play'\n"
                  " def run(self): time.sleep(.4)\n"
                  "run=SimpleNamespace(running=lambda:False,proc=SimpleNamespace(returncode=0))\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None), "
                  "mock.patch.object(play,'start',side_effect=lambda ctx,*a: ((ctx.layout.data/'unexpected-start').touch(),(run,None))[1]), "
                  "mock.patch.object(game_session,'finish_game',return_value=SimpleNamespace(alarm=False)), "
                  "mock.patch.object(winsys,'keep_awake'):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'building')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'job_started')
        worker.stdout.close()
        self.assertEqual(worker.wait(timeout=10), 0)
        self.assertFalse((self.root / 'data' / 'unexpected-start').exists())
        self.assertFalse((self.root / 'data' / 'job-running.json').exists())

    def test_mod_rebuild_cancel_waits_for_cleanup(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,sys,time\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,net,mods,build,saves\n"
                  "class Fake(jobs.Job):\n"
                  " claims=('game',); kind='rebuild'\n"
                  " def __init__(self,ctx,game): super().__init__(ctx,[]); self.game_dir=game; self.after='play'\n"
                  " def run(self):\n"
                  "  while not self.cancelled(): time.sleep(.02)\n"
                  "  raise net.Cancelled()\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'building')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'job_started')
        worker.stdin.write(json.dumps({'id': 3, 'method': 'cancel_job'}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['result']['status'], 'cancelling')
        ended = json.loads(worker.stdout.readline())
        self.assertEqual((ended['event'], ended['result']), ('job_ended', 'cancelled'))
        self.assertFalse((self.root / 'data' / 'job-running.json').exists())
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)

    def test_killed_mod_job_leaves_recoverable_marker(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,sys,time\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,net,mods,build,saves\n"
                  "class Fake(jobs.Job):\n"
                  " claims=('game',); kind='rebuild'\n"
                  " def __init__(self,ctx,game): super().__init__(ctx,[]); self.game_dir=game; self.after='play'\n"
                  " def run(self):\n"
                  "  while not self.cancelled(): time.sleep(.02)\n"
                  "  raise net.Cancelled()\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'building')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'job_started')
        marker = self.root / 'data' / 'job-running.json'
        deadline = time.monotonic() + 10
        while not marker.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(marker.exists())
        worker.kill()
        worker.wait(timeout=15)
        self.assertTrue(marker.exists())
        successor, hello = self.start_worker()
        self.assertEqual(hello['result']['lock'], 'held')
        successor.stdin.write(json.dumps({'id': 2, 'method': 'inspect'}) + '\n')
        successor.stdin.flush()
        self.assertTrue(json.loads(successor.stdout.readline())['result']['job_marker_present'])
        successor.stdin.close()
        self.assertEqual(successor.wait(timeout=15), 0)
        self.assertTrue(marker.exists())

    def test_mod_job_emits_sanitized_progress(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,sys,time\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,net,mods,build,saves\n"
                  "class Fake(jobs.Job):\n"
                  " claims=('game',); kind='rebuild'\n"
                  " def __init__(self,ctx,game): super().__init__(ctx,['compile']); self.game_dir=game; self.after='play'\n"
                  " def run(self):\n"
                  "  self.begin(\"compile\")\n"
                  "  while not self.cancelled(): time.sleep(.02)\n"
                  "  raise net.Cancelled()\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'building')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'job_started')
        progress = json.loads(worker.stdout.readline())
        self.assertEqual(progress['event'], 'job_progress')
        self.assertEqual(progress['step'], 'compile')
        self.assertGreaterEqual(progress['fraction'], 0)
        self.assertLessEqual(progress['fraction'], 1)
        worker.stdin.write(json.dumps({'id': 3, 'method': 'cancel_job'}) + '\n')
        worker.stdin.flush()
        self.assertEqual(json.loads(worker.stdout.readline())['result']['status'], 'cancelling')
        ended = json.loads(worker.stdout.readline())
        self.assertEqual((ended['event'], ended['result']), ('job_ended', 'cancelled'))
        self.assertFalse((self.root / 'data' / 'job-running.json').exists())
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)

    def test_eof_cancels_mod_job_and_waits_for_cleanup(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        script = ("import runpy,sys,time\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,net,mods,build,saves\n"
                  "class Fake(jobs.Job):\n"
                  " claims=('game',); kind='rebuild'\n"
                  " def __init__(self,ctx,game): super().__init__(ctx,[]); self.game_dir=game; self.after='play'\n"
                  " def run(self):\n"
                  "  while not self.cancelled(): time.sleep(.02)\n"
                  "  raise net.Cancelled()\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        for request in ({'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}):
            worker.stdin.write(json.dumps(request) + '\n')
            worker.stdin.flush()
            response = json.loads(worker.stdout.readline())
        self.assertEqual(response['result']['status'], 'building')
        self.assertEqual(json.loads(worker.stdout.readline())['event'], 'job_started')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertFalse((self.root / 'data' / 'job-running.json').exists())
        self.assertIn('worker: job result=cancelled', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_protocol_frames_use_lf_without_carriage_return(self):
        result = subprocess.run([sys.executable, '-m', 'launcher.worker', '--root', str(self.root)],
                                input=b'{"id":1,"method":"hello","protocol":1}\n',
                                capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.endswith(b'\n'))
        self.assertNotIn(b'\r', result.stdout)
        self.assertEqual(json.loads(result.stdout)['id'], 1)

    def test_malformed_settings_are_rejected_without_recovery_writes(self):
        (self.root / 'data' / 'launcher.json').write_text('{not valid json')
        before = self.tree()
        result = self.run_worker({'id': 1, 'method': 'hello', 'protocol': 1})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'invalid_installation')
        self.assertEqual(self.tree(), before)

    @unittest.skipIf(sys.platform == 'win32', 'creating symlinks may require Windows privileges')
    def test_cyclic_root_is_rejected_without_a_traceback(self):
        loop = Path(self.tmp.name) / 'loop'
        loop.symlink_to(loop)
        result = self.run_worker({'id': 1, 'method': 'hello', 'protocol': 1}, root=loop)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'invalid_root')
        self.assertNotIn('Traceback', result.stderr)

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

    def test_startup_failure_is_sanitized_and_releases_the_lock(self):
        script = ("import runpy,sys\n"
                  "from unittest import mock\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch('launcher.context.Context',side_effect=OSError('private-sentinel')):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        result = subprocess.run([sys.executable, '-c', script, str(self.root)],
                                input=json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n',
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'startup_failed')
        self.assertNotIn('private-sentinel', result.stdout + result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        successor = self.run_worker({'id': 2, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

    def test_waiting_update_read_failure_is_sanitized_and_logged(self):
        script = ("import runpy,sys\n"
                  "from unittest import mock\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch('launcher.updater.staged',side_effect=OSError('private-sentinel')):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        result = subprocess.run([sys.executable, '-c', script, str(self.root)],
                                input=json.dumps({'id': 1, 'method': 'hello', 'protocol': 1}) + '\n',
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'startup_failed')
        self.assertNotIn('private-sentinel', result.stdout + result.stderr)
        self.assertIn('worker: startup failed', (self.root / 'data' / 'logs' / 'launcher.log').read_text())
        successor = self.run_worker({'id': 2, 'method': 'hello', 'protocol': 1})
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
                  " run.return_value.code.return_value=0\n"
                  " app.play_only(ctx)\n"
                  " print('started' if spawn.called else 'refused')\n")
        def steam():
            result = subprocess.run([sys.executable, '-c', script, str(self.root)], text=True,
                                    capture_output=True, cwd=ROOT, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            return result.stdout.strip()
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

    def test_inspection_failure_is_sanitized_and_does_not_abort_the_worker(self):
        script = ("import runpy,sys\n"
                  "from unittest import mock\n"
                  "from launcher.inspect_installation import inspect_installation\n"
                  "root=sys.argv[1]\n"
                  "initial=inspect_installation(root)\n"
                  "sys.argv=['launcher.worker','--root',root]\n"
                  "with mock.patch('launcher.inspect_installation.inspect_installation',"
                  "side_effect=[initial,OSError('private-sentinel')]):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        frames = [{'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'inspect'}]
        result = subprocess.run([sys.executable, '-c', script, str(self.root)],
                                input=''.join(json.dumps(frame) + '\n' for frame in frames),
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        hello, failure = map(json.loads, result.stdout.splitlines())
        self.assertEqual(hello['result']['protocol'], 1)
        self.assertEqual(failure, {'id': 2, 'error': {'code': 'inspection_failed'}})
        self.assertNotIn('private-sentinel', result.stdout + result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        self.assertIn('worker: inspect failed', (self.root / 'data' / 'logs' / 'launcher.log').read_text())

    def test_unknown_method_returns_structured_error(self):
        worker, hello = self.start_worker()
        worker.stdin.write(json.dumps({'id': 'two', 'method': 'unexpected'}) + '\n')
        worker.stdin.flush()
        response = json.loads(worker.stdout.readline())
        self.assertEqual(response, {'id': 'two', 'error': {'code': 'unknown_method'}})


if __name__ == '__main__':
    unittest.main(verbosity=2)
