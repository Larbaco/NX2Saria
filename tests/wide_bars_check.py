# needs: clang
"""Check the menus' black bars: the patch set wide_bars (recomp/patches-v111.txt) and runtime/patches/wide_bars.c.

The set's one site, lifted by recomp/lifter.py as the generator lifts it: the screen setup's load of the vtable at
main+0x774068, kept, then a call of the patch page's +0xef0, which patches.c registers for wide_bars_screen_host and no
other set or host function takes. Then tests/wide_bars_check.c builds wide_bars.c with the rest of the runtime stubbed
and plays presents against stand-in screens (menu backgrounds, loading fades, other screens): the sides each state
gives, the log lines, screens destroyed and set up again, a full table, the registers the host function leaves, and a
start without the set. No game files: the module layout is a stand-in written to the test's build folder.

Usage: wide_bars_check.py
"""
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
SITE, ORIGINAL, HOST = 0x774068, 0xf9400268, 0xef0   # ldr x8, [x19]; the page's host slot

LAYOUT = '''// A stand-in for the generated layout.h (tests/wide_bars_check.py): main's image is a buffer of the test.
#pragma once
#include <stdint.h>
extern uint64_t test_main_base;
#define MOD_MAIN_BASE test_main_base
'''


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or shutil.which('clang-20') or 'clang'


def check_site():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', MAIN, PAGE)])
    names = [s[0] for s in table.sets]
    assert names.count('wide_bars') == 1, names
    key = names.index('wide_bars')
    assert table.sets[key] == ('wide_bars', 'NX2SARIA_PATCH_WIDE_BARS', 0), table.sets[key]
    mine = [(off, alts) for off, alts in table.sites['main'].items() if any(k == key for k, _, _ in alts)]
    assert [off for off, _ in mine] == [SITE], [hex(off) for off, _ in mine]
    alts = table.sites['main'][SITE]
    observer = names.index('fov_menu')
    assert [index for index, _, _ in alts] == [key, observer], alts
    _, words, original = alts[0]
    assert alts[1][1:] == (words, original), alts
    assert original == ORIGINAL, hex(original)
    assert len(words) == 2 and not isinstance(words[0], tuple) and not isinstance(words[1], tuple), words
    lifter = Lifter(0, simd='c')
    first, call = lifter.lift(words[0], MAIN + SITE), lifter.lift(words[1], MAIN + SITE)
    assert words[0] == ORIGINAL and first.kind == 'op' and 'c->x[8]' in first.code and 'c->x[19]' in first.code, first.code
    assert call.kind == 'bl' and call.target == MAIN + PAGE + HOST, (call.kind, call.target and hex(call.target))
    # FOV watches the same screen setup even when the bars are disabled.
    for off, alts in table.sites['main'].items():
        for k, repl, _ in alts:
            for w in repl:
                if isinstance(w, tuple) or (k in (key, observer) and off == SITE):
                    continue
                insn = lifter.lift(w, MAIN + off)
                assert not (insn.kind == 'bl' and insn.target == MAIN + PAGE + HOST), \
                    f'set {table.sets[k][0]} at {off:#x} calls the page\'s +{HOST:#x} too'
    users = sorted(p.name for p in (ROOT / 'runtime').rglob('*.c')
                   if re.search(r'RT_PATCH_PAGE\s*\+\s*0x%x\b' % HOST, p.read_text(encoding='utf-8', errors='replace')))
    assert users == ['patches.c'], users
    print(f'wide_bars_check: the site at main+{SITE:#x} keeps ldr x8, [x19] and calls the page\'s +{HOST:#x}; '
          'the FOV observer shares the call at the same site', flush=True)


def main():
    check_site()
    work = ROOT / 'build' / 'test' / 'wide_bars'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    exe = work / ('wide_bars_check.exe' if os.name == 'nt' else 'wide_bars_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-I', str(work), *runtime_include.flags(),
                           str(ROOT / 'tests' / 'wide_bars_check.c'), '-o', str(exe)], env=env)
    subprocess.check_call([str(exe)], env=env)
    subprocess.check_call([str(exe), 'no-set'], env=env)
    return 0


if __name__ == '__main__':
    sys.exit(main())
