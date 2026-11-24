# needs: clang
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
sys.path[:0] = [str(ROOT), str(ROOT / 'recomp')]
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402
from tools.check_content import GENERATED_MARKER  # noqa: E402
import runtime_include

PAGE = 0x4ea0000
FDIV = 0x1e201900
HOST_SITES = [('ostep', 0x3cdfc0, 0x1e380008, [], 0xf60), ('ostep', 0x475f8, 0x1e380009, [], 0xf50),
              ('owater', 0x3876014, FDIV, [FDIV], 0xf20), ('owater', 0x38827f8, FDIV, [FDIV], 0xf20),
              ('owater', 0x632cfc, FDIV, [FDIV], 0xf20),
              ('osound', 0x41531c, 0x910083fd, [0x910083fd], 0xf10), ('osound', 0x415334, 0xbd401100, [0xbd401100], 0xf00),
              ('ohorse', 0x2c3ae18, 0x1e2e1009, [0x1e2e1009], 0xea0),
              ('ogohma', 0x261eb1c, 0x1e280800, [0x1e280800], 0xe80)]


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'

LAYOUT = '''// A stand-in for the generated layout.h (tests/frame_step_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''


def check_sites():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', 0x7000000000, PAGE)])
    bad = 0
    for name, site, original, before, slot in HOST_SITES:
        (key, words, have), = table.sites['main'][site]
        bl = words[-1] if isinstance(words[-1], int) else 0
        target = site + (((bl & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4
        ok = (table.sets[key][0] == name and have == original and words[:-1] == before and bl >> 26 == 0x25 and
              target == PAGE + slot)
        if not ok:
            print(f'FAIL: the site main+{site:#x} of set {name} is {words} over {have:#x}')
            bad += 1
    return bad


SOLVER_SITES = (0x38aebe4, 0x38aebf0, 0x38aebf4)
SOLVER_COPY = (0x38afd10, 0x38afd1c, 0x38afd20)
SCVTF = 0x5e21d821


def solver_functions():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', 0x7000000000, PAGE)])
    names = {i: t[0] for i, t in enumerate(table.sets)}
    variants = {'game': [], 'osolver': [], 'osolverfull': []}
    for site, copy in zip(SOLVER_SITES, SOLVER_COPY):
        alts = {names[key]: (words, have) for key, words, have in table.sites['main'][site]}
        copies = {names[key]: (words, have) for key, words, have in table.sites['main'][copy]}
        assert set(alts) == {'osolver', 'osolverfull'} and alts == copies, (hex(site), alts, copies)
        original = alts['osolver'][1]
        variants['game'].append([original])
        for name in ('osolver', 'osolverfull'):
            variants[name].append(alts[name][0])
    lift = Lifter(0, simd='c')
    out = [f'// {GENERATED_MARKER}: the solver step count words lifted by tests/frame_step_check.py.',
           '#include "cpu.h"', '#include "simd.h"', '']
    for name, sites in variants.items():
        body = []
        for k, words in enumerate(sites):
            for w in words:
                insn = lift.lift(w, 0x7000000000 + SOLVER_SITES[k])
                assert insn.kind == 'op', (name, hex(w), insn.kind)
                body.append('    ' + insn.code)
            if k == 0:
                body.append('    ' + lift.lift(SCVTF, 0x70038aebec).code)
        out.append(f'void solver_{name}(Cpu *c) {{\n' + '\n'.join(body) + '\n}\n')
    return '\n'.join(out)


FOLLOW_SITES = (0x13d704, 0x13a064, 0x389ac20)
FOLLOW_GAME = (0x52a85908, 0x1e270101)
FOLLOW_LOAD = 0xbd410501
FCMP = 0x1e212000


def check_follow():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', 0x7000000000, PAGE)])
    names = {i: t[0] for i, t in enumerate(table.sets)}
    bad = 0
    for site in FOLLOW_SITES:
        first = {names[k]: (words, have) for k, words, have in table.sites['main'].get(site, [])}
        second = {names[k]: (words, have) for k, words, have in table.sites['main'].get(site + 4, [])}
        if (first != {'ofollow': ([('page', 8)], FOLLOW_GAME[0])} or
                second != {'ofollow': ([FOLLOW_LOAD], FOLLOW_GAME[1])}):
            print(f'FAIL: the floor-carry gate at main+{site:#x} is {first} / {second}')
            bad += 1
    return bad


def follow_functions():
    lift = Lifter(0, simd='c')
    out = []
    for name, words in (('game', FOLLOW_GAME + (FCMP,)), ('ofollow', (FOLLOW_LOAD, FCMP))):
        body = []
        for k, w in enumerate(words):
            insn = lift.lift(w, 0x7000000000 + FOLLOW_SITES[0] + 4 * k + (4 if name == 'ofollow' else 0))
            assert insn.kind == 'op', (name, hex(w), insn.kind)
            body.append('    ' + insn.code)
        out.append(f'void follow_{name}(Cpu *c) {{\n' + '\n'.join(body) + '\n}\n')
    return '\n'.join(out)


STAFF_GATE = [0xb943092a, 0xb9030920, 0x0a2a0000]
STAFF_SITES = ((0x3292c84, 0xb94286c3, [0xb94286c3, ('page', 9)] + STAFF_GATE),
               (0x3292ed4, 0x36000b80, [('page', 9)] + STAFF_GATE + [0x36000b80]))


def check_staff():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', 0x7000000000, PAGE)])
    names = {i: t[0] for i, t in enumerate(table.sets)}
    bad = 0
    for site, original, words in STAFF_SITES:
        alts = {names[k]: (w, have) for k, w, have in table.sites['main'].get(site, [])}
        if alts != {'ostaff': (words, original)}:
            print(f'FAIL: the staff gate at main+{site:#x} is {alts}')
            bad += 1
    return bad


def staff_functions():
    lift = Lifter(0, simd='c')
    body = []
    for k, w in enumerate(STAFF_GATE):
        insn = lift.lift(w, 0x7000000000 + STAFF_SITES[0][0] + 4 * k)
        assert insn.kind == 'op', (hex(w), insn.kind)
        body.append('    ' + insn.code)
    return 'void staff_ostaff(Cpu *c) {\n' + '\n'.join(body) + '\n}\n'


def lift_ops(lift, offset, words):
    body = []
    for i, word in enumerate(words):
        insn = lift.lift(word, 0x7000000000 + offset + i * 4)
        assert insn.kind == 'op', (hex(offset + i * 4), hex(word), insn.kind)
        body.append('    ' + insn.code + '\n')
    return ''.join(body)


def horse_function():
    lift = Lifter(0, simd='c')
    return ('void frame_rate_horse_jump_host(Cpu *c);\n'
            'int horse_compare(Cpu *c, int enabled) {\n'
            + lift_ops(lift, 0x2c3ae18, [0x1e2e1009])
            + '    if (enabled) frame_rate_horse_jump_host(c);\n'
            + lift_ops(lift, 0x2c3ae1c, [0x1e292140])
            + '    return COND_PL(c);\n}\n')


def gohma_function():
    lift = Lifter(0, simd='c')
    return ('void frame_rate_gohma_turn_host(Cpu *c);\n'
            'void gohma_clamp(Cpu *c, int enabled) {\n'
            + lift_ops(lift, 0x261eb18, [0x1e210801, 0x1e280800])
            + '    if (enabled) frame_rate_gohma_turn_host(c);\n'
            + lift_ops(lift, 0x261eb28, [0x1e202040, 0x1e22cc00, 0x1e212040, 0x1e204c20])
            + '}\n')


def main():
    work = ROOT / 'build' / 'test' / 'frame_step'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    exe = work / ('frame_step_check.exe' if os.name == 'nt' else 'frame_step_check')
    solver = work / 'solver_sites.c'
    solver.write_text(solver_functions() + '\n' + follow_functions() + '\n' + staff_functions() +
                      '\n' + horse_function() + '\n' + gohma_function())
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           *runtime_include.flags(), '-I', str(work),
                           str(ROOT / 'tests' / 'frame_step_check.c'), str(solver), '-o', str(exe), '-lm'], env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    return subprocess.run([str(exe), 'mean'], env=env).returncode or rc or check_sites() + check_follow() + check_staff()


if __name__ == '__main__':
    sys.exit(main())
