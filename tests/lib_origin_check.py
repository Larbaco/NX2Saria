# needs: clang
import os
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'lib_origin'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    variants = [('native', [])] + ([] if os.name == 'nt' else [('windows', ['-D_WIN32'])])
    rc = 0
    for name, defs in variants:
        exe = work / (f'lib_origin_{name}' + ('.exe' if os.name == 'nt' else ''))
        subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', *defs, *runtime_include.flags(),
                               str(ROOT / 'tests' / 'lib_origin_check.c'), '-o', str(exe)], env=env)
        rc |= subprocess.run([str(exe)], env=env, timeout=60).returncode
    return 1 if rc else 0


if __name__ == '__main__':
    sys.exit(main())
