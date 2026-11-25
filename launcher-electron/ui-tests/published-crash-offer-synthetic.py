import argparse
import http.server
import json
import os
import subprocess
import sys
import tempfile
import threading
import zipfile
from pathlib import Path

WORKER = ('import os,runpy,sys; source,app=sys.argv[1:3]; del sys.argv[1:3]; '
          'sys.path[:0]=[source,app,os.path.join(app,"tools")]; '
          'runpy.run_module("launcher.worker",run_name="__main__")')
READER = ('import sys; sys.path.insert(0,sys.argv[1]); from launcher import crash_offer; '
          'offer=crash_offer.pending(sys.argv[2],{},print); '
          'assert offer and offer["kind"] == "crash", offer')


class NoRequests(http.server.BaseHTTPRequestHandler):
    count = 0

    def unexpected(self):
        type(self).count += 1
        self.send_response(500)
        self.end_headers()

    do_GET = do_POST = unexpected

    def log_message(self, *args):
        pass


def one_session(root, app, source, version, server_url, *, normal=False, opted_out=False, expected=False):
    game = root / 'game'
    game.mkdir()
    (game / 'build-cache').mkdir()
    (game / 'build-cache' / 'state.json').write_text(json.dumps({'kit': version, 'steps': {'compile': {}}}))
    (root / 'data' / 'launcher.json').write_text(json.dumps({
        'game_dir': str(game), 'crash_log_offer': 'off' if opted_out else 'on',
    }))
    (game / 'logs').mkdir()
    code = 0 if normal else 71
    exit_line = '[host] gfx: window closed; exiting' if normal else 'FATAL: synthetic game failure'
    fake = game / 'ocarina'
    fake.write_text("#!/bin/sh\nprintf '%s\\n' " +
                    f"'[host] version: NX2Saria {version}, game 1.1.1' '{exit_line}' > logs/nx2saria.log\n" +
                    f"sleep 0.2\nexit {code}\n")
    fake.chmod(0o755)
    env = dict(os.environ, NX2SARIA_TELEMETRY_URL=server_url)
    frames = [{'id': 1, 'method': 'hello', 'protocol': 1}, {'id': 2, 'method': 'play'}]
    result = subprocess.run([sys.executable, '-I', '-B', '-c', WORKER, str(source), str(app),
                             '--root', str(root), '--app-version', version],
                            input=''.join(json.dumps(frame) + '\n' for frame in frames),
                            text=True, capture_output=True, env=env, timeout=15)
    assert result.returncode == 0, result.stderr
    replies = [json.loads(line) for line in result.stdout.splitlines()]
    assert {'id': 2, 'result': {'status': 'started'}} in replies, replies
    log = (root / 'data' / 'logs' / 'launcher.log').read_text()
    assert f'the game ended with code {code}' in log, log
    assert 'worker: crash offer unavailable' not in log, log
    marker = game / 'logs' / 'crash-offer.json'
    assert marker.is_file() == expected, f'{version} code {code}, opt-out {opted_out}: missing or unexpected marker'
    if expected:
        subprocess.run([sys.executable, '-I', '-B', '-c', READER, str(app), str(game)],
                       text=True, env=env, check=True, timeout=15)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('archives', nargs=2, type=Path)
    parser.add_argument('--worker-source', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    if os.name != 'posix':
        parser.error('this harness uses a Unix shell fixture')
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), NoRequests)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for version, archive, supported in zip(('0.25.0', '0.26.0-rc.1'), args.archives, (False, True), strict=True):
            cases = ((False, False, supported), (True, False, False), (False, True, False)) if supported else ((False, False, False),)
            for normal, opted_out, expected in cases:
                with tempfile.TemporaryDirectory(prefix='nx2-published-crash-') as folder:
                    root = Path(folder)
                    app = root / 'app' / version
                    app.mkdir(parents=True)
                    with zipfile.ZipFile(archive) as release:
                        release.extractall(app)
                    (root / 'data').mkdir()
                    (root / 'app' / 'current.txt').write_text(version)
                    one_session(root, app, args.worker_source.resolve(), version,
                                f'http://127.0.0.1:{server.server_address[1]}', normal=normal,
                                opted_out=opted_out, expected=expected)
            print(f'Published {version}: crash-offer handoff passed')
        assert NoRequests.count == 0, f'unexpected network requests: {NoRequests.count}'
        print('No telemetry requests')
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


if __name__ == '__main__':
    main()
