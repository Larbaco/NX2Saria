# needs: clang
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

DESCRIPTION = ("Build and run tests/quit_menu_check.c: Quit Game in the game's own menus (runtime/ui/quit_menu_core.h, "
               'runtime/ui/quit_menu_text.c).')
ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
DICTIONARIES = 'Pack/ZsDic.pack.zs'


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def game_args(folder):
    files = {f['path']: f for f in json.loads((folder / 'index.json').read_text(encoding='utf-8'))['files']}
    if DICTIONARIES not in files:
        raise SystemExit(f'{folder / "index.json"} has no {DICTIONARIES}')
    archives = sorted(p for p in files if p.startswith('Mals/') and p.endswith('.sarc.zs'))
    if not archives:
        raise SystemExit(f'{folder / "index.json"} has no message archive (Mals/*.sarc.zs)')
    out = [str(folder / 'romfs.bin'), str(files[DICTIONARIES]['offset']), str(files[DICTIONARIES]['size'])]
    for rel in archives:
        out += [rel, str(files[rel]['offset']), str(files[rel]['size'])]
    return out


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--romfs', type=Path,
                    help="a folder with romfs.bin and index.json: also edit every message archive of the game")
    args = ap.parse_args()
    work = ROOT / 'build' / 'test' / 'quit_menu'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('quit_menu_check.exe' if os.name == 'nt' else 'quit_menu_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    sanitize = ['-fsanitize=undefined', '-fsanitize-trap=all', '-ftrivial-auto-var-init=pattern']
    if os.name != 'nt':
        sanitize.append('-fsanitize=address')
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wextra', '-Werror', *sanitize,
                           *runtime_include.flags(rt), str(ROOT / 'tests' / 'quit_menu_check.c'),
                           str(rt / 'ui' / 'quit_menu_text.c'), str(rt / 'hle' / 'sarc.c'), '-o', str(exe), '-lzstd'],
                          env=env)
    run = [str(exe)] + (game_args(args.romfs) if args.romfs else [])
    return subprocess.run(run, env=env, timeout=600).returncode


if __name__ == '__main__':
    sys.exit(main())
