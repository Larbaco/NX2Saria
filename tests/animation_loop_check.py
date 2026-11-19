"""Check the 1.1.1 single-frame animation patch table and run its host against stand-in animation entries."""
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
    for offset, original in [(0x2AFE04, 0x1E210902), (0x2B01D0, 0x1E200901), (0x924E50, 0xBD0002C0)]:
        (key, [bl], have), = table.sites['main'][offset]
        insn = lift.lift(bl, MAIN + offset)
        assert table.sets[key] == ('ozora', 'NX2SARIA_FROZEN_ZORA_FIX', 1)
        assert have == original and insn.kind == 'bl' and insn.target == MAIN + PAGE + 0xE90

    work = ROOT / 'build' / 'test' / 'animation_loop'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(f'#define MOD_MAIN_BASE {MAIN:#x}ull\n')
    executable = work / ('animation_loop_check.exe' if os.name == 'nt' else 'animation_loop_check')
    subprocess.check_call([compiler(), '-O1', '-fno-strict-aliasing', '-I', str(work), '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests/animation_loop_check.c'), str(ROOT / 'runtime/animation_loop.c'),
                           '-o', str(executable), '-lm'])
    return subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    sys.exit(main())
