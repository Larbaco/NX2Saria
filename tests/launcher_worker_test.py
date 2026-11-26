import json
import http.server
import threading
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


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

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux AppImage install')
    def test_start_line_identifies_electron_on_an_appimage_install(self):
        app = self.root / 'app' / '0.0.1'
        (app / 'tools').mkdir(parents=True)
        (app / 'tools' / 'make_game.py').touch()
        (app / 'VERSION').write_text('0.0.1')
        (self.root / 'nx2saria-launcher').touch()
        result = subprocess.run([sys.executable, '-m', 'launcher.worker', '--root', str(self.root),
                                 '--app-version', '0.0.1'],
                                input='{"id":1,"method":"hello","protocol":1}\n',
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text().splitlines()
        self.assertIn('--- NX2Saria launcher 0.0.1 (AppImage install, started by Electron)', log[0])

    def test_invalid_parent_does_not_open_installation(self):
        before = self.tree()
        result = subprocess.run([sys.executable, '-m', 'launcher.worker', '--root', str(self.root),
                                 '--parent-pid', '0'], input='{"id":1,"method":"hello","protocol":1}\n',
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'invalid_parent')
        self.assertEqual(self.tree(), before)

    def test_older_path_helper_keeps_report_paths_private(self):
        script = ("import os,sys\n"
                  "from pathlib import Path\n"
                  "from tools import private_paths\n"
                  "del private_paths.shorten_always\n"
                  "sys.modules['private_paths']=private_paths\n"
                  "os.environ['NX2SARIA_LOG_SHORT_PATHS']='0'\n"
                  "from launcher import telemetry,worker\n"
                  "raw=str(Path.home() / 'private' / 'synthetic.log')\n"
                  "assert private_paths.shorten(raw) == raw\n"
                  "assert telemetry.sent_line(raw).changed\n"
                  "assert str(Path.home()) not in telemetry.sent_line(raw)\n")
        result = subprocess.run([sys.executable, '-c', script], text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows parent handle')
    def test_parent_exit_releases_idle_worker_with_pipe_still_open(self):
        script = ("import os,subprocess,sys;"
                  "p=subprocess.Popen([sys.executable,'-m','launcher.worker','--root',sys.argv[1],"
                  "'--parent-pid',str(os.getpid())]);p.wait()")
        parent = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, parent)
        parent.stdin.write('{"id":1,"method":"hello","protocol":1}\n')
        parent.stdin.flush()
        frames = []
        reader = threading.Thread(target=lambda: frames.append(parent.stdout.readline()), daemon=True)
        reader.start()
        reader.join(10)
        self.assertTrue(frames, 'worker did not report ready')
        handshake = json.loads(frames[0])
        self.assertEqual(handshake.get('result', {}).get('lock'), 'held', handshake)
        parent.kill()
        parent.wait(timeout=10)
        reader = threading.Thread(target=parent.stdout.read, daemon=True)
        reader.start()
        reader.join(10)
        self.assertFalse(reader.is_alive(), 'worker retained its output pipe after the parent died')
        self.assertFalse(parent.stdin.closed)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: parent exit idle', log)
        successor = self.run_worker({'id': 2, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(successor.stdout)['result']['lock'], 'held')

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

    def test_first_run_destination_proposal_does_not_save_settings_or_create_a_folder(self):
        from launcher.paths import Layout

        selected = Path(self.tmp.name) / 'games'
        selected.mkdir()
        (selected / 'unrelated.txt').write_text('keep')
        long = selected / ('g' * 140) / 'NX2Saria'
        image = long / 'romfs' / 'romfs.bin'
        image.parent.mkdir(parents=True)
        image.write_text('complete stand-in image')
        frames = [
            {'id': 1, 'method': 'hello', 'protocol': 1},
            {'id': 2, 'method': 'install_destination'},
            {'id': 3, 'method': 'install_destination', 'folder': str(selected)},
            {'id': 4, 'method': 'install_destination', 'folder': 'relative'},
            {'id': 5, 'method': 'install_destination', 'folder': str(long)},
        ]
        result = subprocess.run([sys.executable, '-m', 'launcher.worker', '--root', str(self.root)],
                                input=''.join(json.dumps(frame) + '\n' for frame in frames),
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        _, default, proposed, invalid, existing = map(json.loads, result.stdout.splitlines())
        self.assertEqual(default['result']['folder'], str(Layout(root=self.root.resolve()).default_game_dir()))
        self.assertEqual(default['result']['source'], 'default')
        self.assertEqual(proposed['result']['folder'], str(selected / 'NX2Saria'))
        self.assertEqual(proposed['result']['source'], 'selected')
        self.assertGreaterEqual(proposed['result']['available_gb'], 0)
        self.assertEqual(proposed['result']['required_gb'], 34)
        self.assertFalse(proposed['result']['complete_image'])
        self.assertIs(type(proposed['result']['path_too_long']), bool)
        self.assertEqual(invalid, {'id': 4, 'error': {'code': 'invalid_destination'}})
        self.assertEqual({entry.name for entry in selected.iterdir()}, {'unrelated.txt', long.parent.name})
        self.assertEqual((selected / 'unrelated.txt').read_text(), 'keep')
        self.assertEqual(existing['result']['folder'], str(long))
        self.assertTrue(existing['result']['complete_image'])
        self.assertEqual(existing['result']['path_too_long'], os.name == 'nt')
        self.assertEqual(image.read_text(), 'complete stand-in image')
        self.assertEqual((self.root / 'data' / 'launcher.json').read_text(), '{}')
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: install destination default', log)
        self.assertIn('worker: install destination selected', log)
        self.assertIn('worker: install destination rejected', log)

    def test_first_run_dump_proposal_checks_the_title_and_preserves_settings(self):
        from fixtures.worker_install_dump import write_dump

        root = Path(self.tmp.name)
        valid = root / 'synthetic-game.nup'
        wrong = root / 'different-title.nup'
        write_dump(valid)
        write_dump(wrong, title=1)
        frames = [
            {'id': 1, 'method': 'hello', 'protocol': 1},
            {'id': 2, 'method': 'install_dump'},
            {'id': 3, 'method': 'install_dump', 'file': str(valid)},
            {'id': 4, 'method': 'install_dump', 'file': str(wrong)},
        ]
        result = subprocess.run([sys.executable, '-m', 'launcher.worker', '--root', str(self.root)],
                                input=''.join(json.dumps(frame) + '\n' for frame in frames),
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        _, empty, selected, rejected = map(json.loads, result.stdout.splitlines())
        self.assertEqual(empty, {'id': 2, 'result': {'file': None, 'source': 'unset'}})
        self.assertEqual(selected, {'id': 3, 'result': {'file': str(valid), 'source': 'selected'}})
        self.assertEqual(rejected, {'id': 4, 'error': {'code': 'invalid_dump'}})
        self.assertEqual((self.root / 'data' / 'launcher.json').read_text(), '{}')
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: install dump selected', log)
        self.assertIn('worker: install dump rejected', log)
        self.assertNotIn(valid.name, log)
        self.assertNotIn(wrong.name, log)

    def test_first_run_finds_newest_dump_and_keeps_a_manual_selection(self):
        from fixtures.worker_install_dump import write_dump

        base = Path(self.tmp.name)
        downloads = base / 'Downloads'
        downloads.mkdir()
        old = downloads / 'older.nup'
        newest = downloads / 'newest.nup'
        write_dump(old)
        write_dump(newest)
        os.utime(old, (100, 100))
        os.utime(newest, (200, 200))
        release = base / 'allow-scan'
        script = ("import contextlib,runpy,sys,time\n"
                  "from pathlib import Path\n"
                  "from unittest import mock\n"
                  "from launcher import gamefiles\n"
                  "downloads,release=map(Path,sys.argv[2:])\n"
                  "def directories(extra):\n"
                  " until=time.monotonic()+10\n"
                  " while not release.exists() and time.monotonic()<until: time.sleep(0.025)\n"
                  " return [downloads]\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "drives=(mock.patch('launcher.platform.linux.mounted_drives',return_value=[]) "
                  "if sys.platform.startswith('linux') else contextlib.nullcontext())\n"
                  "with mock.patch.object(gamefiles,'candidate_dirs',directories), drives:\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")

        def search(pick):
            release.unlink(missing_ok=True)
            worker = subprocess.Popen([sys.executable, '-c', script, str(self.root), str(downloads), str(release)],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True, cwd=ROOT)
            self.addCleanup(self.stop_worker, worker)
            frames = queue.Queue()
            def collect():
                for line in worker.stdout:
                    frames.put(json.loads(line))
                frames.put(None)

            threading.Thread(target=collect, daemon=True).start()

            def request(request_id, method, **fields):
                worker.stdin.write(json.dumps({'id': request_id, 'method': method, **fields}) + '\n')
                worker.stdin.flush()
                response = frames.get(timeout=10)
                if response is None:
                    self.fail(worker.stderr.read())
                self.assertEqual(response['id'], request_id)
                return response['result']

            self.assertEqual(request(1, 'hello', protocol=1)['lock'], 'held')
            self.assertEqual(request(2, 'search_install_dump')['status'], 'searching')
            self.assertEqual(request(3, 'inspect')['status'], 'not_configured')
            if pick:
                self.assertEqual(request(4, 'install_dump', file=str(pick))['file'], str(pick))
            release.touch()
            for i in range(5, 205):
                result = request(i, 'search_install_dump')
                if result['status'] != 'searching':
                    break
                time.sleep(0.025)
            self.assertEqual(result, {'status': 'complete', 'file': str(pick or newest)})
            worker.stdin.close()
            self.assertEqual(worker.wait(timeout=15), 0)

        search(None)
        search(old)
        self.assertEqual((self.root / 'data' / 'launcher.json').read_text(), '{}')
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: install dump_search result=found count=2', log)
        self.assertIn('worker: install dump_search result=kept count=2', log)
        self.assertNotIn(newest.name, log)
        self.assertNotIn(old.name, log)

    def test_first_run_starts_install_job_only_after_confirmed_proposals(self):
        from fixtures.worker_install_dump import write_dump

        root = Path(self.tmp.name)
        dump = root / 'setup-source.nup'
        write_dump(dump)
        folder = root / 'target'
        folder.mkdir()
        occupied = root / 'NX2navi'
        (occupied / 'build-cache').mkdir(parents=True)
        (occupied / 'build-cache' / 'state.json').write_text('{"kit":"0.0.1","steps":{"compile":{}}}')
        from launcher.platform import game_executable
        game_executable(occupied).write_text('stand-in that cannot be run')
        enough_space = root / 'enough-space'
        settings = self.root / 'data' / 'launcher.json'
        settings.write_text('{"desktop_shortcut": false, "telemetry": "off"}')
        script = ("import runpy,sys\n"
                  "from pathlib import Path\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,paths,winsys\n"
                  "enough_space=Path(sys.argv[2])\n"
                  "def build(job):\n"
                  " job.begin('check')\n"
                  " Path(job.game_dir,'fake-install-done').write_text(Path(job.dump).name)\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs.InstallJob,'run',build), "
                  "mock.patch.object(paths,'free_gb',side_effect=lambda _: 100 if enough_space.exists() else 1), "
                  "mock.patch.object(winsys,'keep_awake'):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root), str(enough_space)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        frames = queue.Queue()

        def collect():
            for line in worker.stdout:
                frames.put(json.loads(line))

        threading.Thread(target=collect, daemon=True).start()

        def request(**message):
            worker.stdin.write(json.dumps(message) + '\n')
            worker.stdin.flush()
            response = frames.get(timeout=10)
            self.assertEqual(response['id'], message['id'])
            return response

        self.assertEqual(request(id=1, method='hello', protocol=1)['result']['lock'], 'held')
        self.assertEqual(request(id=2, method='install')['result']['status'], 'choose_source')
        self.assertEqual(json.loads(settings.read_text()), {'desktop_shortcut': False, 'telemetry': 'off'})
        self.assertEqual(request(id=3, method='install_destination', folder=str(folder))['result']['folder'],
                         str(folder))
        self.assertEqual(request(id=4, method='install_dump', file=str(dump))['result']['file'], str(dump))
        self.assertEqual(request(id=5, method='install')['result']['status'], 'need_space')
        self.assertEqual(json.loads(settings.read_text()), {'desktop_shortcut': False, 'telemetry': 'off'})
        self.assertEqual(list(folder.iterdir()), [])
        enough_space.touch()
        self.assertEqual(request(id=6, method='install_destination', folder=str(occupied))['result']['folder'],
                         str(occupied))
        self.assertEqual(request(id=7, method='install')['result']['status'], 'already_installed')
        self.assertEqual(game_executable(occupied).read_text(), 'stand-in that cannot be run')
        self.assertEqual(request(id=8, method='install_destination', folder=str(folder))['result']['folder'],
                         str(folder))
        self.assertEqual(request(id=9, method='install')['result']['status'], 'building')
        started = frames.get(timeout=10)
        self.assertEqual(started['event'], 'job_started')
        event = frames.get(timeout=10)
        while event['event'] == 'job_progress':
            event = frames.get(timeout=10)
        self.assertEqual((event['event'], event['result'], event['outcome']), ('job_ended', 'ok', 'complete'))
        self.assertEqual(event['session'], started['session'])
        self.assertEqual((folder / 'fake-install-done').read_text(), dump.name)
        self.assertFalse((self.root / 'data' / 'job-running.json').exists())
        self.assertEqual(json.loads(settings.read_text()), {
            'desktop_shortcut': False, 'telemetry': 'off', 'game_dir': str(folder), 'dump': str(dump)})
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: install started', log)
        self.assertIn('job install: ok', log)
        self.assertNotIn('worker: spawn started', log)

    def test_manual_rebuild_protects_the_current_game_and_never_starts_play(self):
        from launcher.platform import game_executable

        folder = Path(self.tmp.name) / 'game'
        cache = folder / 'build-cache'
        (cache / 'obj').mkdir(parents=True)
        (cache / 'obj' / 'stand-in').touch()
        (cache / 'state.json').write_text('{"kit":"0.0.1","steps":{"compile":{}}}')
        executable = game_executable(folder)
        executable.write_text('last finished version')
        settings = self.root / 'data' / 'launcher.json'
        settings.write_text(json.dumps({'game_dir': str(folder), 'dump': 'retained-source', 'keep_cache': False}))
        marker = self.root / 'data' / 'job-running.json'
        marker.write_text('existing marker')
        script = ("import runpy,sys\n"
                  "from pathlib import Path\n"
                  "from unittest import mock\n"
                  "from launcher import jobs,winsys\n"
                  "def rebuild(job):\n"
                  " assert job.dump=='retained-source'\n"
                  " job.begin('compile_changed')\n"
                  " Path(job.game_dir,'build-finished').touch()\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs.RebuildJob,'run',rebuild), mock.patch.object(winsys,'keep_awake'):\n"
                  " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', script, str(self.root)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        frames = queue.Queue()

        def collect():
            for line in worker.stdout:
                frames.put(json.loads(line))

        threading.Thread(target=collect, daemon=True).start()

        def request(request_id, method, **fields):
            worker.stdin.write(json.dumps({'id': request_id, 'method': method, **fields}) + '\n')
            worker.stdin.flush()
            response = frames.get(timeout=10)
            self.assertEqual(response['id'], request_id)
            return response['result']

        self.assertEqual(request(1, 'hello', protocol=1)['lock'], 'held')
        self.assertEqual(request(2, 'rebuild')['status'], 'unavailable')
        self.assertEqual(marker.read_text(), 'existing marker')
        marker.unlink()
        self.assertEqual(request(3, 'rebuild')['status'], 'building')
        self.assertEqual(frames.get(timeout=10)['event'], 'job_started')
        event = frames.get(timeout=10)
        while event['event'] == 'job_progress':
            event = frames.get(timeout=10)
        self.assertEqual((event['event'], event['outcome']), ('job_ended', 'complete'))
        self.assertFalse(marker.exists())
        self.assertTrue((folder / 'build-finished').is_file())
        self.assertFalse((cache / 'obj').exists())
        self.assertEqual(executable.read_text(), 'last finished version')
        self.assertEqual(request(4, 'inspect')['status'], 'ready')
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        log = (self.root / 'data' / 'logs' / 'launcher.log').read_text()
        self.assertIn('worker: rebuild started', log)
        self.assertNotIn('worker: spawn started', log)

    def test_build_supervisor_requires_acknowledgement_and_keeps_output_without_control(self):
        scratch = Path(self.tmp.name)
        child_started = scratch / 'builder-started'
        child_release = scratch / 'builder-release'
        child = ("import pathlib,sys,time\n"
                 "started,release=map(pathlib.Path,sys.argv[1:])\n"
                 "started.touch()\n"
                 "print('synthetic output',flush=True)\n"
                 "until=time.monotonic()+10\n"
                 "while not release.exists() and time.monotonic()<until: time.sleep(.02)\n"
                 "sys.exit(0 if release.exists() else 2)\n")

        def supervise(run_dir):
            run_dir.mkdir()
            supervisor = subprocess.Popen(
                [sys.executable, '-m', 'launcher.build_supervisor', '--install-root', str(self.root),
                 '--run-dir', str(run_dir), '--', sys.executable, '-c', child, str(child_started), str(child_release)],
                cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                start_new_session=sys.platform.startswith('linux'))
            self.addCleanup(self.stop_worker, supervisor)
            ready = queue.Queue()
            threading.Thread(target=lambda: ready.put(supervisor.stdout.readline()), daemon=True).start()
            greeting = ready.get(timeout=10)
            self.assertTrue(greeting, supervisor.stderr.read() if not greeting else None)
            identity = json.loads(greeting)
            self.assertEqual(identity['pid'], supervisor.pid)
            self.assertIn(identity['kind'], ('linux_group', 'windows_job', 'windows_pid'))
            return supervisor

        abandoned = scratch / '78a4f94fdd6d427381b4d5347e625ce0'
        supervisor = supervise(abandoned)
        supervisor.stdin.close()
        self.assertEqual(supervisor.wait(timeout=10), 0)
        self.assertFalse(child_started.exists())
        self.assertFalse((abandoned / 'result.json').exists())

        allowed = scratch / '53e981a6529b41c7b8482685990e477e'
        supervisor = supervise(allowed)
        supervisor.stdin.write('start\n')
        supervisor.stdin.flush()
        supervisor.stdin.close()
        deadline = time.monotonic() + 10
        while not child_started.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(child_started.is_file())
        self.assertIsNone(supervisor.poll())
        child_release.touch()
        self.assertEqual(supervisor.wait(timeout=15), 0)
        self.assertEqual((allowed / 'output.log').read_text().strip(), 'synthetic output')
        self.assertEqual(json.loads((allowed / 'result.json').read_text())['code'], 0)

        child_started.unlink()
        child_release.unlink()
        stopped = scratch / 'aefc9690f3ef4ab890540155d48654ff'
        supervisor = supervise(stopped)
        supervisor.stdin.write('start\n')
        supervisor.stdin.flush()
        supervisor.stdin.close()
        deadline = time.monotonic() + 10
        while not child_started.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(child_started.is_file())
        (stopped / 'cancel').touch()
        self.assertEqual(supervisor.wait(timeout=15), 0)
        self.assertNotEqual(json.loads((stopped / 'result.json').read_text())['code'], 0)


    def test_worker_publishes_build_identity_before_start_and_build_survives_worker_death(self):
        from launcher import jobs
        from launcher.platform import game_executable
        from types import SimpleNamespace

        base = Path(self.tmp.name)
        game = base / 'game'
        cache = game / 'build-cache'
        cache.mkdir(parents=True)
        (cache / 'state.json').write_text('{"kit":"0.0.1","steps":{"compile":{}}}')
        game_executable(game).write_text('the last finished build')
        data = self.root / 'data'
        marker = data / 'job-running.json'
        (data / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
        started, release = base / 'build-saw-marker.json', base / 'end-build'
        builder = ("import pathlib,sys,time\n"
                   "marker,started,release=map(pathlib.Path,sys.argv[1:])\n"
                   "started.write_text(marker.read_text())\n"
                   "until=time.monotonic()+15\n"
                   "while not release.exists() and time.monotonic()<until: time.sleep(.02)\n"
                   "sys.exit(0 if release.exists() else 2)\n")
        worker_code = ("import runpy,sys\n"
                       "from unittest import mock\n"
                       "from launcher import build,jobs,winsys\n"
                       "root=sys.argv[1]\n"
                       "command=[sys.executable,'-c',*sys.argv[2:]]\n"
                       "sys.argv=['launcher.worker','--root',root]\n"
                       "with (mock.patch.object(jobs.RebuildJob,'run',lambda job: job.make_game([], {})), "
                       "mock.patch.object(build,'command',return_value=command), "
                       "mock.patch.object(winsys,'keep_awake')):\n"
                       " runpy.run_module('launcher.worker',run_name='__main__')\n")
        worker = subprocess.Popen([sys.executable, '-c', worker_code, str(self.root), builder,
                                   str(marker), str(started), str(release)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, cwd=ROOT)
        self.addCleanup(self.stop_worker, worker)
        self.addCleanup(release.touch)
        frames = queue.Queue()

        def collect():
            for line in worker.stdout:
                frames.put(json.loads(line))

        threading.Thread(target=collect, daemon=True).start()
        worker.stdin.write('{"id":1,"method":"hello","protocol":1}\n')
        worker.stdin.flush()
        self.assertEqual(frames.get(timeout=10)['result']['lock'], 'held')
        worker.stdin.write('{"id":2,"method":"rebuild"}\n')
        worker.stdin.flush()
        self.assertEqual(frames.get(timeout=10)['result']['status'], 'building')
        self.assertEqual(frames.get(timeout=10)['event'], 'job_started')
        deadline = time.monotonic() + 10
        captured = None
        while time.monotonic() < deadline:
            try:
                captured = json.loads(started.read_text())
                break
            except (FileNotFoundError, ValueError):
                time.sleep(.02)
        self.assertIsNotNone(captured)
        identity = captured['build']
        self.assertIn(identity['kind'], ('linux_group', 'windows_job', 'windows_pid'))
        self.assertNotEqual(identity['pid'], worker.pid)
        self.assertEqual(captured['kind'], 'rebuild')
        self.assertIsNone(worker.poll())
        worker.kill()
        worker.wait(timeout=10)
        result = data / 'build-supervisor' / identity['token'] / 'result.json'
        self.assertFalse(result.exists())
        reopened, hello = self.start_worker()
        self.assertEqual(hello['result']['lock'], 'held')
        reopened.stdin.write('{"id":5,"method":"inspect"}\n')
        reopened.stdin.flush()
        inspection = json.loads(reopened.stdout.readline())['result']
        self.assertEqual(inspection['recovery']['state'], 'alive')
        self.assertTrue(inspection['job_marker_present'])
        reopened.stdin.write('{"id":2,"method":"play"}\n')
        reopened.stdin.flush()
        self.assertEqual(json.loads(reopened.stdout.readline())['error']['code'], 'job_running')
        reopened.stdin.write('{"id":8,"method":"recover_build","action":"wait"}\n')
        reopened.stdin.flush()
        self.assertEqual(json.loads(reopened.stdout.readline())['result']['status'], 'waiting')
        self.assertEqual(json.loads(marker.read_text())['build'], identity)
        release.touch()
        frames = queue.Queue()
        threading.Thread(target=lambda: frames.put(reopened.stdout.readline()), daemon=True).start()
        event = json.loads(frames.get(timeout=10))
        self.assertEqual((event['event'], event['inspection']['recovery']['state']), ('recovery_changed', 'interrupted'))
        self.assertFalse(event['inspection']['job_marker_present'])
        self.assertFalse(marker.exists())
        self.assertEqual(json.loads(result.read_text())['code'], 0)
        reopened.stdin.close()
        self.assertEqual(reopened.wait(timeout=10), 0)
        marker.write_text(json.dumps(captured))
        self.assertIn('worker: build supervisor ready', (data / 'logs' / 'launcher.log').read_text())
        recovered = jobs.interrupted(SimpleNamespace(layout=SimpleNamespace(data=data)))
        self.assertEqual(recovered['task'], 'the rebuild of the game')
        self.assertEqual(recovered['game_dir'], str(game))
        self.assertFalse(marker.exists())

        marker.write_text(json.dumps(captured))
        recovered_worker, _ = self.start_worker()
        recovered_worker.stdin.write('{"id":5,"method":"inspect"}\n')
        recovered_worker.stdin.flush()
        inspection = json.loads(recovered_worker.stdout.readline())['result']
        self.assertEqual(inspection['recovery']['state'], 'interrupted')
        self.assertEqual(inspection['recovery']['kind'], 'rebuild')
        self.assertFalse(inspection['job_marker_present'])
        recovered_worker.stdin.close()
        self.assertEqual(recovered_worker.wait(timeout=10), 0)


        identity = {**identity, 'pid': os.getpid(), 'start': 1}
        if sys.platform.startswith('linux'):
            identity.update(session=os.getpid(), group=os.getpid())
        else:
            identity['kind'] = 'windows_pid'
        marker.write_text(json.dumps({**captured, 'build': identity}))
        reused, _ = self.start_worker()
        reused.stdin.write('{"id":6,"method":"inspect"}\n')
        reused.stdin.flush()
        inspection = json.loads(reused.stdout.readline())['result']
        self.assertEqual(inspection['recovery']['reason'], 'pid_reused')
        self.assertFalse(marker.exists())
        invalid = b'{broken marker'
        marker.write_bytes(invalid)
        reused.stdin.write('{"id":7,"method":"inspect"}\n')
        reused.stdin.flush()
        inspection = json.loads(reused.stdout.readline())['result']
        self.assertEqual(inspection['recovery']['state'], 'unverified')
        self.assertTrue(inspection['job_marker_present'])
        self.assertEqual(marker.read_bytes(), invalid)
        reused.stdin.close()
        self.assertEqual(reused.wait(timeout=10), 0)


    def test_stop_recovery_ends_a_verified_previous_build(self):
        from launcher.platform import game_executable
        from uuid import uuid4

        data = self.root / 'data'
        folder = Path(self.tmp.name) / 'game'
        (folder / 'build-cache').mkdir(parents=True)
        (folder / 'build-cache' / 'state.json').write_text('{"kit":"0.0.1","steps":{"compile":{}}}')
        game_executable(folder).write_text('last finished build')
        (data / 'launcher.json').write_text(json.dumps({'game_dir': str(folder)}))
        token = uuid4().hex
        run_dir = data / 'build-supervisor' / token
        run_dir.mkdir(parents=True)
        began = run_dir / 'began'
        builder = 'import pathlib,sys,time; pathlib.Path(sys.argv[1]).touch(); time.sleep(12)'
        supervisor = subprocess.Popen(
            [sys.executable, '-m', 'launcher.build_supervisor', '--install-root', str(self.root),
             '--run-dir', str(run_dir), '--', sys.executable, '-c', builder, str(began)],
            cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            start_new_session=sys.platform.startswith('linux'))
        self.addCleanup(self.stop_worker, supervisor)
        identities = queue.Queue()
        threading.Thread(target=lambda: identities.put(supervisor.stdout.readline()), daemon=True).start()
        identity = {**json.loads(identities.get(timeout=10)), 'version': 1, 'token': token}
        marker = data / 'job-running.json'
        marker.write_text(json.dumps({'build': identity, 'kind': 'rebuild', 'task': 'synthetic previous rebuild',
                                      'game_dir': str(folder)}))
        supervisor.stdin.write('start\n')
        supervisor.stdin.close()
        deadline = time.monotonic() + 5
        while not began.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(began.exists())
        self.assertIsNone(supervisor.poll())
        worker, _ = self.start_worker()
        worker.stdin.write('{"id":6,"method":"recover_build","action":"stop"}\n')
        worker.stdin.flush()
        frames = queue.Queue()

        def collect():
            for line in worker.stdout:
                frames.put(json.loads(line))

        threading.Thread(target=collect, daemon=True).start()
        response = frames.get(timeout=5)
        self.assertEqual(response['result']['status'], 'stopping')
        event = frames.get(timeout=5)
        self.assertEqual(event['event'], 'recovery_changed')
        self.assertEqual(event['inspection']['recovery']['state'], 'interrupted')
        self.assertFalse(marker.exists())
        status = supervisor.wait(timeout=2)
        if identity['kind'] == 'windows_job':
            self.assertNotEqual(status, 0)
        else:
            self.assertNotEqual(json.loads((run_dir / 'result.json').read_text())['code'], 0)
        self.assertEqual(game_executable(folder).read_text(), 'last finished build')

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

    def test_native_game_restart_retains_the_play_session_and_lock(self):
        game = Path(self.tmp.name) / 'game'
        game.mkdir()
        exe = game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')
        if sys.platform == 'win32':
            fixture = os.environ.get('NX2_TEST_RESTART_GAME')
            if not fixture:
                self.skipTest('needs the synthetic Windows restart executable')
            shutil.copyfile(fixture, exe)
        elif sys.platform.startswith('linux'):
            cc = shutil.which('cc')
            if not cc:
                self.skipTest('needs a compiler for the stand-in process')
            subprocess.run([cc, str(ROOT / 'tests' / 'fixtures' / 'worker_restart_game.c'), '-o', str(exe)],
                           check=True, timeout=15)
        else:
            self.skipTest('Linux or Windows native restart')
        (self.root / 'data' / 'launcher.json').write_text(
            json.dumps({'game_dir': str(game), 'warn_low_commit': False}))
        worker, hello = self.start_worker()
        self.assertEqual(hello['result']['lock'], 'held')
        release = game / 'exit-restarted'
        self.addCleanup(release.touch)
        frames = queue.Queue()

        def collect():
            for line in worker.stdout:
                frames.put(json.loads(line))

        threading.Thread(target=collect, daemon=True).start()
        worker.stdin.write('{"id":2,"method":"play"}\n')
        worker.stdin.flush()
        self.assertEqual(frames.get(timeout=10)['result']['status'], 'started')
        started = frames.get(timeout=10)
        self.assertEqual(started['event'], 'game_started')
        log_file = self.root / 'data' / 'logs' / 'launcher.log'
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if (game / 'restarted').is_file() and 'worker: game restart observed' in log_file.read_text():
                break
            time.sleep(0.05)
        self.assertTrue((game / 'restarted').is_file(), 'the stand-in did not start its second copy')
        self.assertEqual(log_file.read_text().count('worker: game restart observed'), 1)
        competitor = self.run_worker({'id': 3, 'method': 'hello', 'protocol': 1})
        self.assertEqual(json.loads(competitor.stdout)['error']['code'], 'busy')
        worker.stdin.write('{"id":4,"method":"inspect"}\n')
        worker.stdin.flush()
        self.assertEqual(frames.get(timeout=10)['id'], 4)
        release.touch()
        ended = frames.get(timeout=10)
        self.assertEqual(ended['event'], 'game_ended')
        self.assertEqual(ended['sequence'], started['sequence'] + 1)
        self.assertEqual(ended['session'], started['session'])
        self.assertEqual(ended['code'], 7)
        worker.stdin.close()
        self.assertEqual(worker.wait(timeout=15), 0)
        self.assertIn('the game ended with code 7 after restarting itself', log_file.read_text())

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

    def assert_game_finished(self, marker):
        deadline = time.monotonic() + 10
        observed = None
        while time.monotonic() < deadline:
            try:
                observed = marker.read_text()
            except FileNotFoundError:
                pass
            if observed == 'done':
                return
            time.sleep(0.05)
        self.assertEqual(observed, 'done')

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
        self.assert_game_finished(marker)
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
        self.assert_game_finished(marker)
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
        self.assert_game_finished(marker)
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
                  "run=SimpleNamespace(running=lambda:False,code=lambda:0,kind='',restarted=False)\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None), "
                  "mock.patch.object(play,'start',return_value=(run,None)), "
                  "mock.patch.object(game_session,'finish_game',return_value=game_session.GameEnd('','','',False)), "
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
                  "run=SimpleNamespace(running=lambda:False,code=lambda:0,kind='',restarted=False)\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']), "
                  "mock.patch.object(commit,'play_memory_request',return_value=commit.MemoryRequest(100,200,False)), "
                  "mock.patch.object(play,'start',return_value=(run,None)), "
                  "mock.patch.object(game_session,'finish_game',return_value=game_session.GameEnd('','','',False)), "
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
                  "run=SimpleNamespace(running=lambda:False,code=lambda:0,kind='',restarted=False)\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None), "
                  "mock.patch.object(play,'start',return_value=(run,None)), "
                  "mock.patch.object(game_session,'finish_game',return_value=game_session.GameEnd('','','',False)), "
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
                  "run=SimpleNamespace(running=lambda:False,code=lambda:0,kind='',restarted=False)\n"
                  "sys.argv=['launcher.worker','--root',sys.argv[1]]\n"
                  "with mock.patch.object(jobs,'ModsRebuildJob',Fake), "
                  "mock.patch.object(saves,'game_running',return_value=False), "
                  "mock.patch.object(mods,'prepare',return_value=[]), "
                  "mock.patch.object(build,'can_rebuild',return_value=True), "
                  "mock.patch.object(mods,'code_waiting',return_value=['Example']), "
                  "mock.patch.object(commit,'play_memory_request',return_value=None), "
                  "mock.patch.object(play,'start',side_effect=lambda ctx,*a: ((ctx.layout.data/'unexpected-start').touch(),(run,None))[1]), "
                  "mock.patch.object(game_session,'finish_game',return_value=game_session.GameEnd('','','',False)), "
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
                                input='{"id":1,"method":"hello","protocol":1}\n'
                                      '{"id":2,"method":"set_hide_while_playing","value":false}\n',
                                text=True, capture_output=True, cwd=ROOT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        hello, preference = map(json.loads, result.stdout.splitlines())
        self.assertEqual(hello['result']['lock'], 'unavailable')
        self.assertEqual(preference['error']['code'], 'preference_unavailable')
        self.assertEqual((self.root / 'data' / 'launcher.json').read_text(), '{}')
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
