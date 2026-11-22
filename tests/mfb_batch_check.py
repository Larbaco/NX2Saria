# needs: clang
"""Build and run tests/mfb_batch_check.c: runtime/gfx/mfb_batch.h, the rule of the mesh fallback's batch (vk_exec_draw.c
mfb_draw, NX2SARIA_MESH_FALLBACK_BATCH): which draws are batched and which go one by one (the way back), the parts of the
buffers they take, and a program's history.

No GPU, no game files, any Python.

Usage: mfb_batch_check.py
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
    work = ROOT / 'build' / 'test' / 'mfb_batch'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('mfb_batch_check.exe' if os.name == 'nt' else 'mfb_batch_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'mfb_batch_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
