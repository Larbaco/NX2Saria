"""Build and run tests/cache_stamp_check.c: runtime/vk_cache_stamp.h, the translator stamp of the pipeline cache
files (runtime/vk_pipeline.c cache_stamp_check): how pipeline-cache.id is read and written, and what a start does with
the files when the stamp names the current translator, another one whose shaders are still carried, another one that
is not, or none.

What the runtime does with the files (deleting them, writing the stamp) shows in a run's log, in the line
"vk_pipeline: pipeline cache stamp <id> matches|differs|none: <action>, <MB>".

Usage: cache_stamp_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
try:
    from common import MSYS_BIN, compiler  # noqa: E402
except ImportError:   # a Python without the differential tests' packages: the same compiler choice without them
    MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

    def compiler():
        if os.environ.get('CC'):
            return os.environ['CC']
        if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
            return str(MSYS_BIN / 'clang.exe')
        return shutil.which('clang') or 'clang'


def main():
    work = ROOT / 'build' / 'test' / 'cache_stamp'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('cache_stamp_check.exe' if os.name == 'nt' else 'cache_stamp_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'cache_stamp_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
