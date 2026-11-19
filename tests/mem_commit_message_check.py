"""Build and run the pure Win32 commit-message selector; no Windows or game files are needed."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    cc = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
    if not cc:
        print('mem_commit_message_check: no C compiler found', file=sys.stderr)
        return 2
    work = ROOT / 'build' / 'test' / 'mem_commit_message'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('mem_commit_message_check.exe' if os.name == 'nt' else 'mem_commit_message_check')
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests' / 'mem_commit_message_check.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    print('mem_commit_message_check: passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
