"""Build and run tests/sha256_paths_check.c with every clang this machine has: runtime/sha256.c's portable C and SHA-NI
block functions give the FIPS 180-4 hashes and the same hash of every message, and its CPUID check finds the SHA
extensions exactly where the processor reports them (/proc/cpuinfo's sha_ni on Linux; __builtin_cpu_supports where
the compiler takes "sha", clang 19 and later). The compilers are the one the build uses (CC, MSYS2 UCRT64's clang on
Windows, else clang on PATH) and, on Linux, each /usr/lib/llvm-*/bin/clang, so that a distribution's older default
(Ubuntu 24.04's clang 18) is covered beside a newer one; each builds the file generic (as tools/make_game.py builds the
runtime), for x86-64-v2 (tools/build_game.py --cpu v2) and, where this CPU runs it, x86-64-v3. No game files.

Usage: sha256_paths_check.py
"""
import glob
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def llvm_major(path):
    m = re.search(r'llvm-(\d+)', path)
    return int(m.group(1)) if m else 0


def compilers(env):
    """(label, path) of each distinct clang to try, the build's own first; a CC that does not resolve fails."""
    found = []
    if os.environ.get('CC'):
        cc = shutil.which(os.environ['CC']) or os.environ['CC']
        if not os.path.isfile(cc):
            raise SystemExit(f'FAIL: CC={os.environ["CC"]} is not a compiler on PATH')
        found.append(cc)
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        found.append(str(MSYS_BIN / 'clang.exe'))
    if shutil.which('clang', path=env.get('PATH')):
        found.append(shutil.which('clang', path=env.get('PATH')))
    if sys.platform.startswith('linux'):
        found += sorted(glob.glob('/usr/lib/llvm-*/bin/clang'), key=llvm_major)
    seen, out = set(), []
    for cc in found:
        real = os.path.realpath(cc)
        if real in seen or not os.path.isfile(real):
            continue
        seen.add(real)
        # On Windows only a MinGW clang builds the runtime; an MSVC-targeting one lacks its headers here
        if os.name == 'nt':
            target = subprocess.run([cc, '-dumpmachine'], capture_output=True, text=True, errors='replace').stdout
            if not target.strip().endswith('-windows-gnu'):
                continue
        v = subprocess.run([cc, '--version'], capture_output=True, text=True, errors='replace').stdout
        m = re.search(r'clang version (\S+)', v)
        out.append((f'{cc} ({m.group(1) if m else "unknown version"})', cc))
    return out


def cpu_flags():
    """The processor's feature flags from /proc/cpuinfo (Linux), None where there is none to read."""
    try:
        flags = re.search(r'^flags\s*:(.*)$', Path('/proc/cpuinfo').read_text(), re.M)
    except OSError:
        return None
    return set(flags.group(1).split()) if flags else None


def main():
    work = ROOT / 'build' / 'test' / 'sha256_paths'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    ccs = compilers(env)
    if not ccs:
        print('FAIL: no clang found')
        return 1
    flags = cpu_flags()
    want = None if flags is None else {'sha_ni', 'sse4_1', 'ssse3'} <= flags
    marches = [[], ['-march=x86-64-v2']]
    if flags is not None and {'avx2', 'fma', 'bmi2', 'movbe', 'f16c'} <= flags:
        marches.append(['-march=x86-64-v3'])
    bad = 0
    for i, (label, cc) in enumerate(ccs):
        for march in marches:
            name = f'{label}{" " + march[0] if march else ""}'
            exe = work / (f'sha256_paths_{i}{march[0][7:] if march else ""}' + ('.exe' if os.name == 'nt' else ''))
            p = subprocess.run([cc, '-std=gnu11', '-O2', *march, '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                                str(ROOT / 'tests' / 'sha256_paths_check.c'), '-o', str(exe)],
                               capture_output=True, text=True, env=env, errors='replace')
            if p.returncode:
                print(f'FAIL {name}: runtime/sha256.c does not compile\n{p.stderr[-1500:]}')
                bad += 1
                continue
            try:
                r = subprocess.run([str(exe)], capture_output=True, text=True, env=env, errors='replace', timeout=120)
            except subprocess.TimeoutExpired:
                print(f'FAIL {name}: did not finish in 120 s')
                bad += 1
                continue
            out = r.stdout
            ni = re.search(r'^cpuid sha_ni=(\d)', out, re.M)
            builtin = re.search(r'^builtin sha_ni=(\d)', out, re.M)
            compared = re.search(r'^compared ni=(\d)(.*)$', out, re.M)
            if r.returncode or not ni or not compared:
                print(f'FAIL {name}: exit {r.returncode}\n{out}{r.stderr}')
                bad += 1
                continue
            found = bool(int(ni.group(1)))
            if want is not None and found != want:
                print(f'FAIL {name}: sha256_ni found {int(found)}, /proc/cpuinfo says the CPU '
                      f'{"has" if want else "lacks"} sha_ni, sse4_1 and ssse3')
                bad += 1
                continue
            if builtin and bool(int(builtin.group(1))) != found:
                print(f'FAIL {name}: sha256_ni found {int(found)}, __builtin_cpu_supports {builtin.group(1)}')
                bad += 1
                continue
            if compared.group(1) == '1':
                counts = dict(re.findall(r'(\w+)=(\d+)', compared.group(2)))
                print(f'{name}: the FIPS vectors pass in portable C and on SHA-NI, and both agree on '
                      f'{counts.get("blocks", "?")} block runs and {counts.get("messages", "?")} messages')
            else:
                print(f'{name}: the FIPS vectors pass in portable C; this CPU has no SHA extensions, so SHA-NI did not '
                      f'run')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
