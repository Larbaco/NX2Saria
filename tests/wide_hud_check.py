# needs: clang
"""Build and run tests/wide_hud_check.c: runtime/patches/wide_hud.c, the HUD's edge elements moved out to the sides of a picture
wider than 16:9 (NX2SARIA_WIDE_HUD = edges), on layout files and archives the test makes itself. No game files.

With --romfs, also on the game's own archive: the folder holds romfs.bin and its index.json, and the program reads the
archive, Pack/ZsDic.pack.zs and the resource size table from romfs.bin at the offsets the index gives, edits the
archive for several shapes of picture and after the action prompt's edit of the same archive, prints the log lines of
the 3440x1440 and 21:9 edits, and raises the table's entry for the archive the menus' redesign makes larger. The
pause menu's backdrop drawn in pieces (runtime/patches/wide_hud.c split_layout) is compared byte for byte with
tools/layout_dump.py split_picture, the reference, at u 640 and 1920: this script reads the game's PlusMenu_00.buil
(the zstd command unpacks the archive), splits it with the same pieces and writes the results to build/test/wide_hud/
for the program. Nothing else is written.

The program is built with the undefined-behaviour sanitizer (traps), with locals that start as a pattern rather than
whatever the stack held, and with AddressSanitizer where the toolchain has it (not MSYS2's).

Usage: wide_hud_check.py [--romfs DIR]
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
ARCHIVE = 'UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs'
DICTIONARIES = 'Pack/ZsDic.pack.zs'
RESTBL = 'System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs'
# runtime/patches/wide_hud.c menu_bg_pieces: P_MenuBg_00 of PlusMenu_00 in pieces, as split_picture takes them
PIECES = [{'src': [1880, 2010], 'at': [-320, -320], 'f': [0, 1]},
          {'src': [-320, 3380], 'at': [-320, 3380], 'f': [1, 1]},
          {'src': [1880, 2010], 'at': [3380, 3380], 'f': [1, 2]},
          {'src': [3380, 4980], 'at': [3380, 4980], 'f': [2, 2]},
          {'src': [1880, 2010], 'at': [4980, 4980], 'f': [2, 3]},
          {'src': [4980, 8880], 'at': [4980, 8880], 'f': [3, 3]},
          {'src': [1880, 2010], 'at': [8880, 8880], 'f': [3, 4]}]


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def game_args(folder):
    """romfs.bin and the offset and size of the archive, the dictionaries and the resource size table in it."""
    files = {f['path']: f for f in json.loads((folder / 'index.json').read_text(encoding='utf-8'))['files']}
    for rel in (ARCHIVE, DICTIONARIES, RESTBL):
        if rel not in files:
            raise SystemExit(f'{folder / "index.json"} has no {rel}')
    return [str(folder / 'romfs.bin')] + [str(files[rel][k]) for rel in (ARCHIVE, DICTIONARIES, RESTBL)
                                          for k in ('offset', 'size')]


def reference_splits(folder, work):
    """The game's first PlusMenu_00.buil split by tools/layout_dump.py at u 640 and 1920, written under work."""
    sys.path.insert(0, str(ROOT / 'tools'))
    import layout_dump as ld
    common = dict(ld.read_archives(folder))['Common']
    name, a, b = next(m for m in ld.sarc_members(common) if m[0] == 'PlusMenu_00.buil')
    layout = ld.Layout('PlusMenu_00', bytearray(common[a:b]))
    out = []
    for u in (640.0, 1920.0):
        data, _lines = ld.split_picture(layout, {'op': 'split', 'pane': 'P_MenuBg_00', 'pieces': PIECES}, u)
        path = work / f'PlusMenu_00.split-{u:g}.buil'
        path.write_bytes(bytes(data))
        out.append(str(path))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--romfs', type=Path, help='a folder with romfs.bin and index.json: also check the game\'s archive')
    args = ap.parse_args()
    work = ROOT / 'build' / 'test' / 'wide_hud'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('wide_hud_check.exe' if os.name == 'nt' else 'wide_hud_check')
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
                           str(ROOT / 'tests' / 'wide_hud_check.c'), str(rt / 'patches' / 'wide_hud.c'),
                           str(rt / 'patches' / 'action_prompt.c'), str(rt / 'hle' / 'sarc.c'), str(rt / 'hle' / 'restbl.c'), '-o', str(exe),
                           '-lzstd', '-lm'], env=env)
    run = [str(exe)] + (game_args(args.romfs) + reference_splits(args.romfs, work) if args.romfs else [])
    return subprocess.run(run, env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
