# needs: linux, display
import http.server
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


ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
import sys
import time
from pathlib import Path
from launcher import jobs, net
from launcher.ui import app
import launcher.platform.linux as linux

root, game, mode = map(Path, sys.argv[1:4])
mode = str(mode)
linux.mounted_drives = lambda *args, **kwargs: []
original_start = app.App.start

class WaitingJob(jobs.Job):
    claims = ('game',)
    task = 'synthetic job'
    def __init__(self, ctx):
        super().__init__(ctx, ['ready'])
        self.game_dir = game
    def run(self):
        (root / 'job-started').write_text('started')
        while not self.cancelled():
            time.sleep(.05)
        (root / 'job-cancelled').write_text('cancelled')
        raise net.Cancelled()
    def cancel(self):
        (root / 'cancel-requested').write_text('cancel')
        super().cancel()

def start(self):
    original_start(self)
    if mode == 'game':
        self.root.after(300, self.play)
    elif mode == 'job':
        self.root.after(300, lambda: self.run_job(WaitingJob(self.ctx), 'home'))
    def watch():
        if (root / 'close-window').exists():
            self.on_close()
        else:
            self.root.after(50, watch)
    self.root.after(50, watch)

app.App.start = start
sys.exit(app.main(['--root', str(root)]))
'''
STEAM = r'''
import sys
from launcher.context import Context
from launcher.paths import Layout
from launcher.ui import app
app.tell = print
ctx = Context(Layout(root=sys.argv[1]))
sys.exit(app.play_only(ctx) or 0)
'''


class LauncherLifecycleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2-lifecycle-')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.root = self.base / 'launcher'
        self.game = self.base / 'game'
        (self.root / 'data').mkdir(parents=True)
        (self.game / 'build-cache').mkdir(parents=True)
        (self.root / 'data' / 'launcher.json').write_text(json.dumps({
            'game_dir': str(self.game), 'auto_check': False, 'hide_while_playing': False,
        }))
        (self.game / 'build-cache' / 'state.json').write_text(json.dumps({'kit': '0.24.0', 'steps': {'compile': {}}}))
        self.exe = self.game / 'ocarina'
        self.exe.write_text('#!/bin/sh\necho $$ > game-started\nwhile [ ! -e game-release ]; do sleep 0.1; done\ntouch game-ended\n')
        self.exe.chmod(0o755)
        self.env = {k: v for k, v in os.environ.items() if k in ('DISPLAY', 'XAUTHORITY', 'PATH', 'LANG')}
        self.env.update(HOME=str(self.base), XDG_DATA_HOME=str(self.base / 'data'),
                        XDG_CONFIG_HOME=str(self.base / 'config'), XDG_CACHE_HOME=str(self.base / 'cache'),
                        XDG_STATE_HOME=str(self.base / 'state'), NX2SARIA_TELEMETRY_URL='none')
        self.processes = []
        self.addCleanup(self.cleanup_processes)

    def cleanup_processes(self):
        (self.game / 'game-release').touch()
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)
        started, ended = self.game / 'game-started', self.game / 'game-ended'
        deadline = time.monotonic() + 3
        while started.exists() and not ended.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        if started.exists() and not ended.exists():
            try:
                pid = int(started.read_text())
                command = (Path('/proc') / str(pid) / 'cmdline').read_bytes().split(b'\0')
                if os.fsencode(self.exe) in command:
                    os.kill(pid, signal.SIGTERM)
            except (OSError, ValueError):
                pass

    def wait_file(self, file, process=None):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if file.exists():
                return
            if process is not None and process.poll() is not None:
                self.fail(f'process ended before {file.name}: {process.communicate()}')
            time.sleep(.05)
        self.fail(f'timed out waiting for {file.name}')

    def start(self, mode):
        process = subprocess.Popen([sys.executable, '-c', HARNESS, str(self.root), str(self.game), mode], cwd=ROOT,
                                   env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(process)
        return process

    def finish_window(self, process, killed):
        if killed:
            process.kill()
        else:
            (self.root / 'close-window').touch()
        return process.communicate(timeout=15)

    def check_game_survives(self, killed):
        process = self.start('game')
        self.wait_file(self.game / 'game-started', process)
        self.finish_window(process, killed)
        self.assertFalse((self.game / 'game-ended').exists())
        (self.game / 'game-release').touch()
        self.wait_file(self.game / 'game-ended')

    def test_closing_window_does_not_stop_game(self):
        self.check_game_survives(False)

    def test_killing_window_does_not_stop_game(self):
        self.check_game_survives(True)

    def test_closing_window_requests_job_cancellation(self):
        process = self.start('job')
        self.wait_file(self.root / 'job-started', process)
        self.finish_window(process, False)
        self.assertTrue((self.root / 'cancel-requested').exists())

    def test_killing_window_leaves_interrupted_job_marker(self):
        process = self.start('job')
        self.wait_file(self.root / 'job-started', process)
        self.finish_window(process, True)
        self.assertTrue((self.root / 'data' / 'job-running.json').is_file())
        self.assertFalse((self.root / 'cancel-requested').exists())

    def test_cli_and_steam_refuse_while_window_runs_job(self):
        process = self.start('job')
        self.wait_file(self.root / 'job-started', process)
        cli = subprocess.run([sys.executable, '-m', 'launcher.cli', '--root', str(self.root), 'tools'], cwd=ROOT,
                             env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(cli.returncode, 1)
        self.assertIn('using', cli.stderr)
        steam = subprocess.run([sys.executable, '-c', STEAM, str(self.root)], cwd=ROOT, env=self.env,
                               capture_output=True, text=True, timeout=30)
        self.assertEqual(steam.returncode, 0)
        self.assertIn('busy with synthetic job', steam.stdout)
        self.assertFalse((self.game / 'game-started').exists())
        self.finish_window(process, False)

    def reports_server(self):
        hits = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                hits.append(self.path)
                self.send_response(201)
                self.send_header('Content-Length', '0')
                self.end_headers()

        httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        self.addCleanup(httpd.server_close)
        self.addCleanup(httpd.shutdown)
        self.env['NX2SARIA_TELEMETRY_URL'] = f'http://127.0.0.1:{httpd.server_address[1]}'
        return hits

    def steam_run(self, last_line):
        self.exe.write_text('#!/bin/sh\nmkdir -p logs\n'
                            "printf '%s\\n' '[host] version: NX2Saria 0.24.0, game 1.1.1' "
                            "'[host] patches: fps60 on' > logs/nx2saria.log\n" + last_line + '\n')
        steam = subprocess.run([sys.executable, '-c', STEAM, str(self.root)], cwd=ROOT, env=self.env,
                               capture_output=True, text=True, timeout=60)
        self.assertEqual(steam.returncode, 0, steam.stderr)
        return (self.root / 'data' / 'logs' / 'launcher.log').read_text(errors='replace')

    def test_a_crash_from_steam_leaves_the_offer_waiting_and_sends_nothing(self):
        hits = self.reports_server()
        log = self.steam_run("echo 'FATAL: vk: the graphics device was lost' >> logs/nx2saria.log\nexit 72")
        offer = json.loads((self.game / 'logs' / 'crash-offer.json').read_text())
        self.assertEqual(offer['kind'], 'device_lost')
        self.assertIn('the graphics device was lost', offer['message'])
        stat = (self.game / 'logs' / 'nx2saria.log').stat()
        self.assertEqual(offer['log'], [stat.st_size, stat.st_mtime_ns])
        self.assertIn("telemetry: crash log offer: waits for the launcher's main screen (end: device_lost)", log)
        self.assertEqual(hits, [])

    def test_a_game_steam_stopped_leaves_no_offer(self):
        hits = self.reports_server()
        log = self.steam_run('kill -TERM $$\nsleep 5')
        self.assertFalse((self.game / 'logs' / 'crash-offer.json').exists())
        self.assertIn('telemetry: crash log offer: not made: the game was asked to close '
                      '(end: code -15 (ended by SIGTERM))', log)
        self.assertEqual(hits, [])

    def test_a_normal_end_from_steam_leaves_no_offer(self):
        hits = self.reports_server()
        log = self.steam_run("echo '[host] gfx: window closed; exiting' >> logs/nx2saria.log\nexit 0")
        self.assertFalse((self.game / 'logs' / 'crash-offer.json').exists())
        self.assertNotIn('crash log offer: waits', log)
        self.assertNotIn('crash log offer: not made', log)
        self.assertEqual(hits, [])

    def test_steam_can_play_while_window_is_idle(self):
        window = self.start('idle')
        log = self.root / 'data' / 'logs' / 'launcher.log'
        self.wait_file(log, window)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if 'window: SIGTERM' in log.read_text(errors='replace'):
                break
            if window.poll() is not None:
                self.fail('window exited during startup')
            time.sleep(.05)
        else:
            self.fail('window did not become ready')
        steam = subprocess.Popen([sys.executable, '-c', STEAM, str(self.root)], cwd=ROOT, env=self.env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.processes.append(steam)
        self.wait_file(self.game / 'game-started', steam)
        (self.game / 'game-release').touch()
        self.assertEqual(steam.communicate(timeout=15)[0], '')
        self.assertEqual(steam.returncode, 0)
        self.finish_window(window, False)


if __name__ == '__main__':
    unittest.main()
