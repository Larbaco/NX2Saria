# needs: clang
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

DESCRIPTION = ("Build and run tests/action_prompt_check.c: runtime/patches/action_prompt.c, the action prompt's "
               'diamond of face buttons laid')
ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
ARCHIVE = 'UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs'
DICTIONARIES = 'Pack/ZsDic.pack.zs'


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def game_args(folder):
    files = {f['path']: f for f in json.loads((folder / 'index.json').read_text(encoding='utf-8'))['files']}
    for rel in (ARCHIVE, DICTIONARIES):
        if rel not in files:
            raise SystemExit(f'{folder / "index.json"} has no {rel}')
    return [str(folder / 'romfs.bin')] + [str(files[rel][k]) for rel in (ARCHIVE, DICTIONARIES)
                                          for k in ('offset', 'size')]


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--romfs', type=Path, help='a folder with romfs.bin and index.json: also check the game\'s archive')
    args = ap.parse_args()
    work = ROOT / 'build' / 'test' / 'action_prompt'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('action_prompt_check.exe' if os.name == 'nt' else 'action_prompt_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    sanitize = ['-fsanitize=undefined', '-fsanitize-trap=all', '-ftrivial-auto-var-init=pattern']
    if os.name != 'nt':
        sanitize.append('-fsanitize=address')
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                           '-Wno-bitwise-instead-of-logical', *sanitize, *runtime_include.flags(rt),
                           str(ROOT / 'tests' / 'action_prompt_check.c'), str(rt / 'patches' / 'action_prompt.c'),
                           str(rt / 'hle' / 'sarc.c'), '-o', str(exe), '-lzstd', '-lm'], env=env)
    run = [str(exe)] + (game_args(args.romfs) if args.romfs else [])
    return subprocess.run(run, env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
