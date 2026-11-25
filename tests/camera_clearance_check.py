# needs: clang, py:numpy, py:lz4
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT), str(ROOT / 'recomp')]
from heap_retry_check import LAYOUT, MAIN, MSYS_BIN, PAGE, PATCHES, compiler  # noqa: E402
from gen import Emitter  # noqa: E402
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402
import runtime_include

SLOT = 0xe00
SITES = {0x71a844: (0xbd451e6a, 10), 0x71b624: (0xbd451e6e, 14)}


def check_sites():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', MAIN, PAGE)])
    names = [s[0] for s in table.sets]
    assert names.count('camera_clearance') == 1, names
    key = names.index('camera_clearance')
    assert table.sets[key] == ('camera_clearance', 'NX2SARIA_CAMERA_CLEARANCE_FIX', 1), table.sets[key]
    mine = sorted(off for off, alts in table.sites['main'].items() if any(k == key for k, _, _ in alts))
    assert mine == sorted(SITES), [hex(o) for o in mine]
    lifter = Lifter(0, simd='c')

    class Mod:
        pass
    mod = Mod()
    mod.lifter = lifter
    for off, (original, reg) in SITES.items():
        alts = table.sites['main'][off]
        assert len(alts) == 1, f'another set patches main+{off:#x} too'
        _, words, orig = alts[0]
        assert orig == original and len(words) == 2 and words[0] == original, (hex(off), words)
        kept = lifter.lift(original, MAIN + off)
        assert kept.kind == 'op' and f'VSET({reg},' in kept.code and '0x51c' in kept.code, kept.code
        call = lifter.lift(words[1], MAIN + off)
        assert call.kind == 'bl' and call.target == MAIN + PAGE + SLOT, (hex(off), call.kind)
        em = Emitter.__new__(Emitter)
        em.mod, em.patched = mod, {off: [(key, words)]}
        assert em.patched_effect(kept, off) == (False, None)
    for off, alts in table.sites['main'].items():
        for k, repl, _ in alts:
            if k == key:
                continue
            for w in repl:
                if isinstance(w, tuple):
                    continue
                insn = lifter.lift(w, MAIN + off)
                assert not (insn.kind == 'bl' and insn.target == MAIN + PAGE + SLOT), \
                    f'set {table.sets[k][0]} at {off:#x} calls camera_clearance\'s slot'
    users = sorted(p.name for p in (ROOT / 'runtime').rglob('*.c')
                   if re.search(r'RT_PATCH_PAGE\s*\+\s*(0x%x|%d)\b' % (SLOT, SLOT),
                                p.read_text(encoding='utf-8', errors='replace'), re.I))
    assert users == [], users
    assert '#define HOST_SLOT 0x%x' % SLOT in (ROOT / 'runtime' / 'patches' / 'camera_clearance.c').read_text(encoding='utf-8')
    print('camera_clearance_check: main+0x71a844 and main+0x71b624 keep their load and call the page\'s +0xe00, the '
          'generator takes the sites; no other set or host function uses that slot', flush=True)


def main():
    check_sites()
    work = ROOT / 'build' / 'test' / 'camera_clearance'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    (work / 'patches.h').write_text(PATCHES)
    exe = work / ('camera_clearance_check.exe' if os.name == 'nt' else 'camera_clearance_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-Wno-bitwise-instead-of-logical',
                           '-fno-strict-aliasing', '-I', str(work), *runtime_include.flags(),
                           str(ROOT / 'tests' / 'camera_clearance_check.c'), '-o', str(exe), '-lm'], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
