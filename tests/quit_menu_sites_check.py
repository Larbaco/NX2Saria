# needs: py:numpy, py:lz4
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'tests'), str(ROOT), str(ROOT / 'recomp')]
from heap_retry_check import MAIN, PAGE  # noqa: E402
from gen import Emitter  # noqa: E402
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402

DESCRIPTION = ("The patch sites of Quit Game in the game's own menus (the set quit_menu of recomp/patches-v111.txt, "
               'runtime/ui/quit_menu.c).')
HELPER = 0x3405bc0
SCREENS, COUNT, LABEL = 0xee0, 0xe20, 0xe10
SITES = {
    0x2d13b0: (0xaa0003f3, ['op', ('page', SCREENS)]),
    0x3405b04: (0x1e201000, [('page', COUNT)]),
    0x3405bfc: (0x2a1303e3, [('page', LABEL)]),
    0x3405b38: (0x52800021, ['op', 'op', 'op', ('main', HELPER)]),
    0x3405b3c: (0x52800022, ['op', 'op']),
    0x3406964: (0xd0004fe2, ['op', 'op', 'op', ('main', HELPER)]),
    0x3406968: (0x91023842, ['op', 'op']),
}
SHARED = {0x2d13b0: 'menu_mouse'}


def main():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', MAIN, PAGE)])
    names = [s[0] for s in table.sets]
    assert names.count('quit_menu') == 1, names
    key = names.index('quit_menu')
    assert table.sets[key] == ('quit_menu', 'NX2SARIA_QUIT_MENU', 1), table.sets[key]
    mine = sorted(off for off, alts in table.sites['main'].items() if any(k == key for k, _, _ in alts))
    assert mine == sorted(SITES), [hex(o) for o in mine]
    lifter = Lifter(0, simd='c')

    class Mod:
        pass
    mod = Mod()
    mod.lifter = lifter
    for off, (original, shape) in SITES.items():
        alts = table.sites['main'][off]
        others = sorted(table.sets[k][0] for k, _, _ in alts if k != key)
        assert others == ([SHARED[off]] if off in SHARED else []), (hex(off), others)
        words, orig = next((w, o) for k, w, o in alts if k == key)
        assert orig == original and len(words) == len(shape), (hex(off), words)
        for k, w, _ in alts:
            assert w == words, f'main+{off:#x}: the sets that share the site do not make the same call'
        for w, want in zip(words, shape):
            insn = lifter.lift(w, MAIN + off)
            if want == 'op':
                assert insn.kind == 'op', (hex(off), hex(w), insn.kind)
            else:
                base = MAIN + PAGE if want[0] == 'page' else MAIN
                assert insn.kind == 'bl' and insn.target == base + want[1], (hex(off), hex(w), insn.kind)
        em = Emitter.__new__(Emitter)
        em.mod, em.patched = mod, {off: [(key, words)]}
        em.replacements(off)
        assert em.patched_effect(lifter.lift(original, MAIN + off), off) == (False, None), hex(off)
    kept = {0x3405b38: [0x52800021], 0x3405b3c: [0x52800022], 0x3406964: [0xd0004fe2], 0x3406968: [0x91023842]}
    after = [w for off in (0x3405b38, 0x3405b3c) for w in next(w for k, w, _ in table.sites['main'][off] if k == key)]
    assert after[4:] == kept[0x3405b38] + kept[0x3405b3c], 'the window keeps its mov w1, #1 and mov w2, #1 after the call'
    after = [w for off in (0x3406964, 0x3406968) for w in next(w for k, w, _ in table.sites['main'][off] if k == key)]
    assert after[4:] == kept[0x3406964] + kept[0x3406968], 'the title keeps its adrp and add after the call'
    assert 0x3406964 >> 12 == 0x3406968 >> 12, 'the adrp moved by one word stays in its page'
    for off, alts in table.sites['main'].items():
        for k, repl, _ in alts:
            if k == key or (off in SHARED and table.sets[k][0] == SHARED[off]):
                continue
            for w in repl:
                if isinstance(w, tuple):
                    continue
                insn = lifter.lift(w, MAIN + off)
                assert not (insn.kind == 'bl' and insn.target in (MAIN + PAGE + COUNT, MAIN + PAGE + LABEL)), \
                    f"set {table.sets[k][0]} at {off:#x} calls one of quit_menu's slots"
    src = (ROOT / 'runtime/ui/quit_menu.c').read_text(encoding='utf-8')
    listed = {int(a, 16): int(w, 16) for a, w in re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]{8})\}', src)}
    assert listed == {off: w for off, (w, _) in SITES.items()}, {hex(k): hex(v) for k, v in listed.items()}
    reg = (ROOT / 'runtime/patches/patches.c').read_text(encoding='utf-8')
    for slot, fn in ((SCREENS, 'screens_host'), (COUNT, 'quit_menu_window_count_host'),
                     (LABEL, 'quit_menu_window_label_host')):
        assert re.search(r'RT_PATCH_PAGE \+ 0x%x, %s\)' % (slot, fn), reg), hex(slot)
    users = sorted(p.name for p in (ROOT / 'runtime').rglob('*.c')
                   if re.search(r'(RT_PATCH_PAGE|HOST_SLOT)\s*\+?\s*0x(e20|e10)\b', p.read_text(encoding='utf-8', errors='replace')))
    assert users == ['patches.c'], users
    print("quit_menu_sites_check: the set quit_menu (NX2SARIA_QUIT_MENU, default 1) owns its 7 sites, so 0 leaves the "
          "game's code as it is at each; the calls go to the page's +0xee0, +0xe20 and +0xe10 and to main+0x3405bc0; "
          'runtime/ui/quit_menu.c lists the same words')
    return 0


if __name__ == '__main__':
    sys.exit(main())
