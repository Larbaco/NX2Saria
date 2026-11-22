# needs: clang
"""Check the 1.1.1 voice language patch's lifted call and callback against stubbed game-data reads."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402
import runtime_include  # noqa: E402

MAIN, PAGE = 0x7000000000, 0x4ea0000
SITE, WAS_WRITTEN, HOST = 0xa7ca98, 0xa7d1c0, 0xe60
UCRT = Path(r'C:\msys64\ucrt64\bin')


def check_site():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', MAIN, PAGE)])
    set_index = table.sets.index(('voice_language', 'NX2SARIA_VOICE_LANGUAGE_FIX', 1))
    mine = [off for off, alts in table.sites['main'].items() if any(k == set_index for k, _, _ in alts)]
    assert mine == [SITE], mine
    [(key, replacement, original)] = table.sites['main'][SITE]
    [word] = replacement
    lift = Lifter(MAIN, simd='c').lift
    assert key == set_index and original == 0x940001ca
    stock, hook = lift(original, MAIN + SITE), lift(word, MAIN + SITE)
    assert (stock.kind, stock.target) == ('bl', MAIN + WAS_WRITTEN)
    assert (hook.kind, hook.target) == ('bl', table.page + HOST)
    print('voice_language_check: the set switches the game-data write check to the callback', flush=True)


def check_runtime():
    work = ROOT / 'build' / 'test' / 'voice_language'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(f'#pragma once\n#define MOD_MAIN_BASE {MAIN:#x}ull\n')
    exe = work / ('voice_language_check.exe' if os.name == 'nt' else 'voice_language_check')
    cc = os.environ.get('CC') or (str(UCRT / 'clang.exe') if os.name == 'nt' and (UCRT / 'clang.exe').is_file()
                                  else shutil.which('clang') or 'clang-20')
    env = dict(os.environ)
    if os.name == 'nt' and UCRT.is_dir():
        env['PATH'] = str(UCRT) + os.pathsep + env.get('PATH', '')
    subprocess.run([cc, '-O2', '-Wall', '-I', str(work), *runtime_include.flags(),
                    str(ROOT / 'tests' / 'voice_language_check.c'), str(ROOT / 'runtime' / 'patches' / 'voice_language.c'),
                    '-o', str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)


if __name__ == '__main__':
    check_site()
    check_runtime()
