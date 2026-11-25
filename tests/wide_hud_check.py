# needs: clang
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

DESCRIPTION = "Build and run tests/wide_hud_check.c: runtime/patches/wide_hud.c, the HUD's edge elements moved out to the sides of a picture"

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
ARCHIVE = 'UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs'
DICTIONARIES = 'Pack/ZsDic.pack.zs'
RESTBL = 'System/Resource/ResourceSizeTable.Product.111.Nin_Ounce_NVN2.rsizetable.zs'
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
    files = {f['path']: f for f in json.loads((folder / 'index.json').read_text(encoding='utf-8'))['files']}
    for rel in (ARCHIVE, DICTIONARIES, RESTBL):
        if rel not in files:
            raise SystemExit(f'{folder / "index.json"} has no {rel}')
    return [str(folder / 'romfs.bin')] + [str(files[rel][k]) for rel in (ARCHIVE, DICTIONARIES, RESTBL)
                                          for k in ('offset', 'size')]


def reference_splits(folder, work):
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
    ap = argparse.ArgumentParser(description=DESCRIPTION)
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
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-Wno-bitwise-instead-of-logical', *sanitize, *runtime_include.flags(rt),
                           str(ROOT / 'tests' / 'wide_hud_check.c'), str(rt / 'patches' / 'wide_hud.c'),
                           str(rt / 'patches' / 'action_prompt.c'), str(rt / 'hle' / 'sarc.c'), str(rt / 'hle' / 'restbl.c'), '-o', str(exe),
                           '-lzstd', '-lm'], env=env)
    run = [str(exe)] + (game_args(args.romfs) + reference_splits(args.romfs, work) if args.romfs else [])
    return subprocess.run(run, env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
