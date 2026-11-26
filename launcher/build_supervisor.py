import argparse
import contextlib
import json
import os
import signal
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


def linux_members(identity):
    from .platform import linux
    try:
        while os.waitpid(-1, os.WNOHANG)[0]:
            pass
    except ChildProcessError:
        pass
    return linux.session_members(identity['session'], exclude=identity['pid'])


def stop_linux_build(identity, stopped_at):
    if stopped_at is None:
        os.killpg(0, signal.SIGTERM)
        return time.monotonic()
    if time.monotonic() - stopped_at >= 5:
        from .platform import linux
        for member in linux.session_members(identity['session'], exclude=identity['pid']):
            linux.signal_verified_member(member, signal.SIGKILL)
    return stopped_at


def wait_for_build(builder, identity, group, cancel):
    stopped_at = None
    while True:
        code = builder.poll()
        if code is not None:
            active = group.active_members() if group else linux_members(identity)
            if not active:
                return code
        if cancel.exists():
            if group:
                group.stop(builder.pid if code is None else None)
            else:
                stopped_at = stop_linux_build(identity, stopped_at)
        time.sleep(0.1)


def supervise(root, run_dir, command):
    with contextlib.ExitStack() as lifetime:
        group = None
        if os.name == 'nt':
            from .platform import windows
            group = lifetime.enter_context(windows.BuildJobGroup(root, run_dir.name))
            identity = group.identity
        elif sys.platform.startswith('linux'):
            identity = linux_identity()
        else:
            raise OSError('platform has no build supervisor')
        try:
            print(json.dumps(identity), flush=True)
        except BrokenPipeError:
            return
        if sys.stdin.readline() != 'start\n':
            return
        if group is None:
            signal.signal(signal.SIGTERM, lambda *_: None)
        with (run_dir / 'output.log').open('wb') as output:
            try:
                builder = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
            except OSError:
                code = None
            else:
                code = wait_for_build(builder, identity, group, run_dir / 'cancel')
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
