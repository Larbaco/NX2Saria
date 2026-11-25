# needs: clang
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

SDL_CLIPBOARD = '''// A stand-in for SDL3's clipboard header (tests/motion_fusion_check.py): an empty clipboard.
#pragma once
#include <stdbool.h>
static inline bool SDL_SetClipboardText(const char *text) { (void)text; return false; }
static inline char *SDL_GetClipboardText(void) { return 0; }
static inline void SDL_free(void *p) { (void)p; }
'''


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def static_checks():
    src = re.sub(r'//[^\n]*', '', (ROOT / 'runtime' / 'input' / 'motion_fusion.c').read_text(encoding='utf-8'))
    bad = 0
    for sig in ('void motion_feed(', 'void motion_source('):
        body = function(src, sig)
        held = body[body.index('plat_mutex_lock('):body.rindex('plat_mutex_unlock(')]
        ok = 'rt_log' not in held
        print(f'static: motion_fusion.c {sig[5:-1]} logs after unlocking: {"ok" if ok else "FAIL"}', flush=True)
        bad += not ok
    return bad


def main():
    bad = static_checks()
    work = ROOT / 'build' / 'test' / 'motion_fusion'
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('motion_fusion_check.exe' if win else 'motion_fusion_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    stub = []
    if not win:
        (work / 'sdl_stub' / 'SDL3').mkdir(parents=True, exist_ok=True)
        (work / 'sdl_stub' / 'SDL3' / 'SDL_clipboard.h').write_text(SDL_CLIPBOARD)
        stub = ['-I', str(work / 'sdl_stub')]
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    common = [compiler(), '-O1', '-g', '-fno-strict-aliasing', *runtime_include.flags(rt), '-I', str(work), *stub]
    objs = []
    for src in plat:
        obj = work / (src.stem + '.o')
        subprocess.check_call([*common, '-w', '-c', str(src), '-o', str(obj)], env=env)
        objs.append(obj)
    subprocess.check_call([*common, '-Wall', '-Wextra', '-Werror',
                           '-Wno-bitwise-instead-of-logical', '-Wno-unused-function',
                           str(ROOT / 'tests' / 'motion_fusion_check.c'), *map(str, objs), '-o', str(exe), '-lm', *libs],
                          env=env)
    rc = subprocess.run([str(exe)], env=env, timeout=120).returncode
    if bad:
        print('motion_fusion_check: the static checks FAILED')
    return 1 if bad or rc else 0


if __name__ == '__main__':
    sys.exit(main())
