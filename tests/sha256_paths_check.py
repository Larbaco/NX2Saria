import glob
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def llvm_major(path):
    m = re.search(r'llvm-(\d+)', path)
    return int(m.group(1)) if m else 0


def compilers(env):
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
        if os.name == 'nt':
            target = subprocess.run([cc, '-dumpmachine'], capture_output=True, text=True, errors='replace').stdout
            if not target.strip().endswith('-windows-gnu'):
                continue
        v = subprocess.run([cc, '--version'], capture_output=True, text=True, errors='replace').stdout
        m = re.search(r'clang version (\S+)', v)
        out.append((f'{cc} ({m.group(1) if m else "unknown version"})', cc))
    return out


def cpu_flags():
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
            p = subprocess.run([cc, '-std=gnu11', '-O2', *march, '-Wall', '-Werror', *runtime_include.flags(),
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
