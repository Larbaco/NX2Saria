# needs: clang
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path
import runtime_include

DESCRIPTION = """Build and run tests/window_idle_check.c with runtime/input/window_idle.c: the cursor that hides after 3 s still over
the window, and the sound muted without the focus, on made-up times. No game files.

Usage: python tests/window_idle_check.py [--cc <clang>]
"""

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='nx2navi-window-idle-') as tmp:
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-fno-strict-aliasing', *runtime_include.flags(),
                               str(ROOT / 'tests' / 'window_idle_check.c'), str(ROOT / 'runtime' / 'input' / 'window_idle.c'),
                               *([] if win else ['-lm']), '-o', str(exe)], env=env)
        return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
