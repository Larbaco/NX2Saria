# needs: clang, py:numpy, py:lz4
"""Check the neighbouring map's hold: the patch set changebank_hold (recomp/patches-v111.txt) and
runtime/patches/changebank_hold.c.

The set's one site, lifted by recomp/lifter.py as the generator lifts it: main+0x3299d28, the loop's add x10, keeps
the add and then calls the patch page's +0xe30. The generator's own check of a patched site (gen.py
Emitter.patched_effect) takes it: no alternative uses flags, which a patched call may not. (Whether flags are live
across the call depends on the whole function, which needs the game's main: on the bench, the update's generated C
with and without the set differs only in the site's line.) The host writes w15 and reads
x10, which recomp/regs.py does not count as callee-saved, so the lifted code has them in the Cpu around the call. No other set calls the slot, and only
changebank_hold.c registers it. Then
tests/changebank_hold_check.c builds changebank_hold.c with the rest of the runtime stubbed, runs the rule over
stand-in units and calls the host function over a stand-in manager. No game files: the module layout and patch page
are stand-ins written to the test's build folder.

Usage: changebank_hold_check.py
"""
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT), str(ROOT / 'recomp')]
from heap_retry_check import LAYOUT, MAIN, MSYS_BIN, PAGE, PATCHES, compiler  # noqa: E402
import regs  # noqa: E402
from gen import Emitter  # noqa: E402
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402

SITE, ORIGINAL, SLOT = 0x3299d28, 0x9100054a, 0xe30   # add x10, x10, #1


def check_site():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', MAIN, PAGE)])
    names = [s[0] for s in table.sets]
    assert names.count('changebank_hold') == 1, names
    key = names.index('changebank_hold')
    assert table.sets[key] == ('changebank_hold', 'NX2SARIA_CHANGEBANK_HOLD', 1), table.sets[key]
    mine = sorted(off for off, alts in table.sites['main'].items() if any(k == key for k, _, _ in alts))
    assert mine == [SITE], [hex(o) for o in mine]
    alts = table.sites['main'][SITE]
    assert len(alts) == 1, 'another set patches main+0x3299d28 too'
    _, words, orig = alts[0]
    assert orig == ORIGINAL and len(words) == 2 and words[0] == ORIGINAL, (hex(orig), words)
    lifter = Lifter(0, simd='c')
    kept = lifter.lift(ORIGINAL, MAIN + SITE)
    assert kept.kind == 'op' and kept.code.startswith('c->x[10] = ') and 'subs' not in kept.code, kept.code
    call = lifter.lift(words[1], MAIN + SITE)
    assert call.kind == 'bl' and call.target == MAIN + PAGE + SLOT, (call.kind, call.target and hex(call.target))

    # the generator's check of the site, as gen.py makes it for the module (a patched call may not use flags)
    class Mod:
        pass
    mod = Mod()
    mod.lifter = lifter
    em = Emitter.__new__(Emitter)
    em.mod, em.patched = mod, {SITE: [(key, words)]}
    assert em.patched_effect(kept, SITE) == (False, None)
    assert {10, 15, 17}.isdisjoint(regs.CALLEE_SAVED) and 19 in regs.CALLEE_SAVED
    for off, alts in table.sites['main'].items():
        for k, repl, _ in alts:
            if k == key:
                continue
            for w in repl:
                if isinstance(w, tuple):
                    continue
                insn = lifter.lift(w, MAIN + off)
                assert not (insn.kind == 'bl' and insn.target == MAIN + PAGE + SLOT), \
                    f'set {table.sets[k][0]} at {off:#x} calls changebank_hold\'s slot'
    users = sorted(p.name for p in (ROOT / 'runtime').glob('*.c')
                   if re.search(r'RT_PATCH_PAGE\s*\+\s*(0x%x|%d)\b' % (SLOT, SLOT),
                                p.read_text(encoding='utf-8', errors='replace'), re.I))
    assert users == [], users   # changebank_hold.c names it through HOST_SLOT
    assert '#define HOST_SLOT 0x%x' % SLOT in (ROOT / 'runtime' / 'patches' / 'changebank_hold.c').read_text(encoding='utf-8')
    print('changebank_hold_check: main+0x3299d28 keeps its add and calls the page\'s +0xe30, the generator takes the site, '
          'x10 and w15 are in the Cpu around the call; no other set or host function uses that slot', flush=True)


def main():
    check_site()
    work = ROOT / 'build' / 'test' / 'changebank_hold'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    (work / 'patches.h').write_text(PATCHES)
    exe = work / ('changebank_hold_check.exe' if os.name == 'nt' else 'changebank_hold_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-Wno-bitwise-instead-of-logical', '-fno-strict-aliasing',
                           '-I', str(work), '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'changebank_hold_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
