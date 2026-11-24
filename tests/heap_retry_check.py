# needs: clang
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'recomp')]
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402
import runtime_include

MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
MAIN, PAGE = 0x7000000000, 0x4ea0000
HOST_CPU, HOST_GPU = 0xed0, 0xec0
SITES = {
    0x436910: (0xa90d577f, ('keep', HOST_CPU)),
    0x436db4: (0xa90b5b7f, ('keep', HOST_CPU)),
    0x436f18: (0x94032c4e, (HOST_GPU,)),
    0x437094: (0x94032bef, (HOST_GPU,)),
}
GPU_ALLOC = 0x502050

LAYOUT = '''// A stand-in for the generated layout.h (tests/heap_retry_check.py): main is a number the test adds and compares.
#pragma once
#include <stdint.h>
extern uint64_t test_main_base;
#define MOD_MAIN_BASE test_main_base
'''
PATCHES = '''// A stand-in for the generated patches.h (tests/heap_retry_check.py).
#pragma once
#define RT_PATCH_PAGE 0x7004ea0000ull
'''


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or shutil.which('clang-20') or 'clang'


def check_sites():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', MAIN, PAGE)])
    names = [s[0] for s in table.sets]
    assert names.count('heap_retry') == 1, names
    key = names.index('heap_retry')
    assert table.sets[key] == ('heap_retry', 'NX2SARIA_HEAP_RETRY', 1), table.sets[key]
    mine = sorted(off for off, alts in table.sites['main'].items() if any(k == key for k, _, _ in alts))
    assert mine == sorted(SITES), [hex(o) for o in mine]
    lifter = Lifter(0, simd='c')
    bars = table.sites['main'][0x774068][0][1][-1]
    assert lifter.lift(bars, MAIN + 0x774068).target == MAIN + PAGE + 0xef0, 'the patch page is not at main+0x4ea0000'
    for off, (original, want) in SITES.items():
        alts = table.sites['main'][off]
        assert len(alts) == 1, f'another set patches main+{off:#x} too'
        _, words, orig = alts[0]
        assert orig == original, (hex(off), hex(orig))
        assert len(words) == len(want) and not any(isinstance(w, tuple) for w in words), (hex(off), words)
        first = lifter.lift(original, MAIN + off)
        if original >> 26 == 0x25:
            assert first.kind == 'bl' and first.target == MAIN + GPU_ALLOC, (hex(off), first.kind)
        else:
            assert first.kind == 'op' and 'ST64' in first.code and 'c->x[27]' in first.code, first.code
        for w, part in zip(words, want):
            insn = lifter.lift(w, MAIN + off)
            if part == 'keep':
                assert w == original, (hex(off), hex(w))
            else:
                assert insn.kind == 'bl' and insn.target == MAIN + PAGE + part, \
                    (hex(off), insn.kind, insn.target and hex(insn.target))
    for off, alts in table.sites['main'].items():
        for k, repl, _ in alts:
            if k == key:
                continue
            for w in repl:
                if isinstance(w, tuple):
                    continue
                insn = lifter.lift(w, MAIN + off)
                assert not (insn.kind == 'bl' and insn.target in (MAIN + PAGE + HOST_CPU, MAIN + PAGE + HOST_GPU)), \
                    f'set {table.sets[k][0]} at {off:#x} calls a slot of heap_retry'
    for slot in (HOST_CPU, HOST_GPU):
        users = sorted(p.name for p in (ROOT / 'runtime').rglob('*.c')
                       if re.search(r'RT_PATCH_PAGE\s*\+\s*(0x%x|%d)\b' % (slot, slot), p.read_text(encoding='utf-8', errors='replace'), re.I))
        assert users == [], (hex(slot), users)
    text = (ROOT / 'runtime' / 'patches' / 'heap_retry.c').read_text(encoding='utf-8')
    assert '#define HOST_CPU 0x%x' % HOST_CPU in text and '#define HOST_GPU 0x%x' % HOST_GPU in text
    others = sorted(p.name for p in (ROOT / 'runtime').rglob('*.c') if p.name != 'heap_retry.c'
                    and re.search(r'RT_PATCH_PAGE\s*\+\s*(HOST_CPU|HOST_GPU)\b', p.read_text(encoding='utf-8',
                                                                                           errors='replace')))
    assert others == [], others
    print('heap_retry_check: the four sites call the page\'s +0xed0 (after the kept stp) and +0xec0 (in place of '
          'main+0x502050); no other set or host function uses those slots', flush=True)


def main():
    check_sites()
    work = ROOT / 'build' / 'test' / 'heap_retry'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    (work / 'patches.h').write_text(PATCHES)
    exe = work / ('heap_retry_check.exe' if os.name == 'nt' else 'heap_retry_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-Wno-bitwise-instead-of-logical',
                           '-fno-strict-aliasing', '-I', str(work), *runtime_include.flags(),
                           str(ROOT / 'tests' / 'heap_retry_check.c'), '-o', str(exe)], env=env)
    rc = 0
    for mode in ('main', 'test', 'shipped', 'off', 'mismatch', 'no-set'):
        rc = subprocess.run([str(exe), mode], env=env).returncode or rc
    return rc


if __name__ == '__main__':
    sys.exit(main())
