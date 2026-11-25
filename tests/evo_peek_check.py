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
    work = ROOT / 'build' / 'test' / 'evo_peek'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('evo_peek_check.exe' if os.name == 'nt' else 'evo_peek_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    extra = [] if os.name == 'nt' else ['-pthread']
    subprocess.check_call([compiler(), '-std=gnu11', '-O2', '-Wall', '-Werror', *runtime_include.flags(),
                           str(ROOT / 'tests' / 'evo_peek_check.c'), '-o', str(exe), *extra], env=env)
    try:
        return subprocess.run([str(exe)], env=env, timeout=120).returncode
    except subprocess.TimeoutExpired:
        print('evo_peek_check: FAIL: the check did not end within 120 s')
        return 1


if __name__ == '__main__':
    sys.exit(main())
