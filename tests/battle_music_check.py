# needs: clang
"""Check the 1.1.1 battle-music patch table and run its gate and flag hooks with a stand-in battle manager."""
import os
from pathlib import Path
import subprocess
import sys

from sprint_check import compiler

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'recomp')]
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402

MAIN = 0x7000000000
PAGE = 0x4EA0000


def main():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', MAIN, PAGE)])
    lift = Lifter(MAIN, simd='c')
    for offset in [0x3353680, 0x33537C0, 0x3353950, 0x3353A4C]:
        (key, [kept, bl], have), = table.sites['main'][offset]
        insn = lift.lift(bl, MAIN + offset)
        assert table.sets[key] == ('obgm', 'NX2SARIA_BATTLE_MUSIC_FIX', 1)
        assert have == kept == 0x39626348 and insn.kind == 'bl' and insn.target == MAIN + PAGE + 0xE70

    work = ROOT / 'build' / 'test' / 'battle_music'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(f'#define MOD_MAIN_BASE {MAIN:#x}ull\n')
    executable = work / ('battle_music_check.exe' if os.name == 'nt' else 'battle_music_check')
    subprocess.check_call([compiler(), '-O1', '-fno-strict-aliasing', '-I', str(work), '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests/battle_music_check.c'), str(ROOT / 'runtime/battle_music.c'),
                           '-o', str(executable), '-lm'])
    return subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    sys.exit(main())
