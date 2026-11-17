"""Build and run tests/boot_map_check.c: runtime/boot_map_core.h, the bytes a boot map changes in what the game reads
(the title's and the slot's sequence graphs, the save), on made-up files, and the values of its settings.

No GPU, no game files, any Python.

Usage: boot_map_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'boot_map'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('boot_map_check.exe' if os.name == 'nt' else 'boot_map_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'boot_map_check.c'), '-lm', '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
