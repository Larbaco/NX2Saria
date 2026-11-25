# needs: clang
import os
import platform
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
    work = ROOT / 'build' / 'test' / 'flight_flash'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('flight_flash_check.exe' if os.name == 'nt' else 'flight_flash_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    arch = ['-march=x86-64-v3'] if platform.machine().lower() in ('amd64', 'x86_64') else []
    subprocess.check_call([compiler(), '-std=gnu11', '-O2', *arch, '-Wall', '-Wextra', '-Werror',
                           *runtime_include.flags(), str(ROOT / 'tests' / 'flight_flash_check.c'), '-o', str(exe)],
                          env=env)
    return subprocess.run([str(exe), *sys.argv[1:2]], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
