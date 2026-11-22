# needs: clang
"""Build and run tests/prompts_record_check.c with runtime/input/input_map.c: the game folder's record of where its button
prompts came from (button-prompts.txt), which the game's log repeats. No game files.

Usage: python tests/prompts_record_check.py [--cc <clang>]
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
    cc = args.cc or os.environ.get('CC') or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    sanitize = [] if win else ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    with tempfile.TemporaryDirectory(prefix='nx2saria-prompts-record-') as tmp:
        exe = Path(tmp) / ('check.exe' if win else 'check')
        game = Path(tmp) / 'game'
        game.mkdir()
        # runtime/simd_fp.h FOK ors two compares on purpose; clang 18 warns on it
        subprocess.check_call([cc, '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                               '-Wno-bitwise-instead-of-logical', '-fno-strict-aliasing', *sanitize,
                               '-I', str(ROOT / 'runtime'), str(ROOT / 'tests' / 'prompts_record_check.c'),
                               str(ROOT / 'runtime' / 'input' / 'input_map.c'), *([] if win else ['-lm']), '-o', str(exe)],
                              env=env)
        return subprocess.run([str(exe), str(game)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
