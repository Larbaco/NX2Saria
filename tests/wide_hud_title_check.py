# needs: clang
"""Build and run tests/wide_hud_title_check.c: runtime/patches/wide_hud.c's second archive, the title screen's layouts
(UI/Layout2Archive/Title.Product.*) with TitleMenuBG_00's backdrop over the whole picture (NX2SARIA_WIDE_MENUS = full),
on layout files and archives the test makes itself. No game files.

With --romfs, also on the game's own title archive: the folder holds romfs.bin and its index.json, and the program
reads the archive and Pack/ZsDic.pack.zs from romfs.bin at the offsets the index gives, edits it for four shapes of
picture (21:9, 32:9 and two windows) and prints the log lines of each. Nothing is written.

The program is built with the undefined-behaviour sanitizer (traps), with locals that start as a pattern rather than
whatever the stack held, and with AddressSanitizer where the toolchain has it (not MSYS2's).

Usage: wide_hud_title_check.py [--romfs DIR]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
ARCHIVE = 'UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs'
DICTIONARIES = 'Pack/ZsDic.pack.zs'


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def game_args(folder):
    """romfs.bin and the offset and size of the title archive and the dictionaries in it."""
    files = {f['path']: f for f in json.loads((folder / 'index.json').read_text(encoding='utf-8'))['files']}
    for rel in (ARCHIVE, DICTIONARIES):
        if rel not in files:
            raise SystemExit(f'{folder / "index.json"} has no {rel}')
    return [str(folder / 'romfs.bin')] + [str(files[rel][k]) for rel in (ARCHIVE, DICTIONARIES)
                                          for k in ('offset', 'size')]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--romfs', type=Path, help='a folder with romfs.bin and index.json: also check the game\'s archive')
    args = ap.parse_args()
    work = ROOT / 'build' / 'test' / 'wide_hud'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('wide_hud_title_check.exe' if os.name == 'nt' else 'wide_hud_title_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    sanitize = ['-fsanitize=undefined', '-fsanitize-trap=all', '-ftrivial-auto-var-init=pattern']
    if os.name != 'nt':
        sanitize.append('-fsanitize=address')
    rt = ROOT / 'runtime'
    # runtime/simd_fp.h FOK ors two compares on purpose; clang 18 warns on it
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-Wno-bitwise-instead-of-logical', *sanitize, '-I', str(rt),
                           str(ROOT / 'tests' / 'wide_hud_title_check.c'), str(rt / 'patches' / 'wide_hud.c'), str(rt / 'hle' / 'sarc.c'),
                           '-o', str(exe), '-lzstd', '-lm'], env=env)
    run = [str(exe)] + (game_args(args.romfs) if args.romfs else [])
    return subprocess.run(run, env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
