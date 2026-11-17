"""Build and run tests/cache_stamp_check.c: runtime/vk_cache_stamp.h, the translator stamp of the pipeline cache
files (runtime/vk_pipeline.c cache_stamp_check): how pipeline-cache.id is read and written, and what a start does with
the files when the stamp names the current translator, another one whose shaders are still carried, another one that
is not, or none.

What the runtime does with the files (deleting them, writing the stamp) shows in a run's log, in the line
"vk: pipeline cache stamp <id> matches|differs|none: <action>, <MB>"; this check also finds that line in
runtime/vk_pipeline.c, so a change of its words shows here.

Usage: cache_stamp_check.py
"""
import os
import re
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
    return subprocess.run([str(exe)], env=env).returncode | log_lines()


LOG_LINES = ['"vk: pipeline cache stamp %02x%02x%02x%02x %s: %s%s, %\\.1f MB', '"vk: pipeline cache stamp off \\(', 'cache_stamp_state_name\\(state\\), cache_stamp_action_name\\(act\\)']   # the log's lines a run is checked by (the formats in the runtime's source)


def log_lines():
    """0 when every log line this change leaves is in the runtime's source as its format string."""
    src = ''.join((ROOT / 'runtime' / f).read_text(encoding='utf-8', errors='replace') for f in ['vk_pipeline.c'])
    src = re.sub(r'"\s*\n\s*"', '', src)   # adjacent string literals as one
    missing = [p for p in LOG_LINES if not re.search(p, src)]
    for p in missing:
        print('cache_stamp_check: FAIL: no log line matching %s in %s' % (p, ', '.join(['vk_pipeline.c'])))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
