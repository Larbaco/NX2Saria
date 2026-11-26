# needs: clang
import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT / 'tools'), str(ROOT / 'recomp')]
import runtime_include  # noqa: E402

DESCRIPTION = """The foliage's draw distances (runtime/patches/foliage_distance.c, foliage_distance.h): builds the file with
stand-ins for the log, the settings, the loader and the page protection, for game 1.1.1 and for 1.1.0, and checks the
factor's reading and limits (0.5 to 3, anything else 1), the caps of the ornaments and the low trees' leaves, the eight
values written into a made-up main and the log line; that 1 writes nothing; that values other than the game's are left
alone; that another version writes nothing. With --exefs, also that the game's main holds the eight values at
main+0x3eefd38.

Usage: python tests/foliage_distance_check.py [--cc <clang>] [--exefs <folder with main>]
"""

MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    ap.add_argument('--exefs', help="a folder with the game's main (1.1.1): check the values at main+0x3eefd38")
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    work = ROOT / 'build' / 'test' / 'foliage_distance'
    work.mkdir(parents=True, exist_ok=True)
    for version in (111, 110):
        exe = work / (f'foliage_distance_check_{version}' + ('.exe' if win else ''))
        subprocess.check_call([cc, '-O1', '-Wall', '-Wno-unused-function', '-Wno-bitwise-instead-of-logical',
                               '-fno-strict-aliasing', f'-DGAME_VERSION={version}', *runtime_include.flags(),
                               str(ROOT / 'tests' / 'foliage_distance_check.c'), '-o', str(exe)], env=env)
        if subprocess.run([str(exe)], env=env).returncode:
            return 1
    if args.exefs:
        from cheats import Nso
        image = bytes(Nso(str(Path(args.exefs) / 'main')).image)
        got = struct.unpack_from('<8f', image, 0x3eefd38)
        if got != (40.0, 60.0, 35.0, 30.0, 40.0, 135.0, 10.0, 45.0):
            print(f'foliage_distance_check: main+0x3eefd38 holds {got}, not the 1.1.1 distances')
            return 1
        print('foliage_distance_check: main+0x3eefd38 holds 40, 60, 35, 30, 40, 135, 10, 45')
    return 0


if __name__ == '__main__':
    sys.exit(main())
