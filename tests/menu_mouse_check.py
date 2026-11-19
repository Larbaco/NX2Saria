"""Build and run tests/menu_mouse_check.c: the arithmetic of the mouse in Ocarina of Time 1.1.1's menus
(runtime/menu_mouse_geom.h): a button's box from its pane, the pointer in layout units on 16:9 and ultrawide pictures
(the UI squeezed into the middle 16:9, NX2SARIA_WIDE_MENUS = full or center), and the box under the pointer. No game
files.

Usage: python tests/menu_mouse_check.py [--cc <clang>]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    with tempfile.TemporaryDirectory(prefix='nx2saria-menu-mouse-') as tmp:
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'),
                               str(ROOT / 'tests' / 'menu_mouse_check.c'),
                               *([] if win else ['-lm']), '-o', str(exe)], env=env)
        return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
