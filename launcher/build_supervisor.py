import argparse
import contextlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def linux_identity():
    from .platform import linux
    state = linux.process_identity(os.getpid())
    if state['pid'] != state['session'] or state['pid'] != state['group']:
        raise OSError('build supervisor requires a dedicated process group')
    linux.child_subreaper(True)
    return {'kind': 'linux_group', 'boot': linux.boot_identity(), **state}


def linux_drain(identity):
    from .platform import linux
    while True:
        try:
            while os.waitpid(-1, os.WNOHANG)[0]:
                pass
        except ChildProcessError:
            pass
        if not linux.session_members(identity['session'], exclude=identity['pid']):
            return
        time.sleep(0.1)


def supervise(root, run_dir, command):
    with contextlib.ExitStack() as lifetime:
        if os.name == 'nt':
            from .platform import windows
            group = lifetime.enter_context(windows.BuildJobGroup(root, run_dir.name))
            identity = group.identity
            drain = group.drain
        elif sys.platform.startswith('linux'):
            identity = linux_identity()
            drain = lambda: linux_drain(identity)
        else:
            raise OSError('platform has no build supervisor')
        try:
            print(json.dumps(identity), flush=True)
        except BrokenPipeError:
            return
        if sys.stdin.readline() != 'start\n':
            return
        with (run_dir / 'output.log').open('wb') as output:
            try:
                builder = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
                code = builder.wait()
            except OSError:
                code = None
        drain()
        result = run_dir / 'result.json'
        temp = result.with_suffix('.tmp')
        temp.write_text(json.dumps({'code': code}), encoding='utf-8')
        os.replace(temp, result)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--install-root', type=Path, required=True)
    parser.add_argument('--run-dir', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if len(args.command) < 2 or args.command[0] != '--':
        parser.error('a builder command is required after --')
    supervise(args.install_root, args.run_dir, args.command[1:])


if __name__ == '__main__':
    main()
