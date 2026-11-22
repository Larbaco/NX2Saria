# needs: clang
"""Build and run tests/play_gate_check.c: runtime/vk_play_gate.h, the play gate of the background pipeline builds
(runtime/vk_pipeline.c play_rest): they rest while the frame shown in play has bg_build_play_draws draws or more and
was shown within half a second, never in a light moment (an area load, the boot screen's warm, a preparation) or with
the setting at 0; and what the log's line "background builds paused in play: N frames, M builds deferred" counts.

What the builders do at rest, and what it changes in the frame rate, shows only in a run: the start's line
"background builds rest in play", the line above at each save in play, and docs/performance.md
"Background builds in play" for a measurement with tools/bench.py.

Usage: play_gate_check.py
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
    work = ROOT / 'build' / 'test' / 'play_gate'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('play_gate_check.exe' if os.name == 'nt' else 'play_gate_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'play_gate_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode | log_lines()


LOG_LINES = ['"vk: background builds paused in play: %llu frames, %llu builds deferred', '"vk: background builds rest in play: while the frame shown has %d draws or more', '"vk: background builds rest in play: off \\(']   # the log's lines a run is checked by (the formats in the runtime's source)


def log_lines():
    """0 when every log line this change leaves is in the runtime's source as its format string."""
    src = ''.join((ROOT / 'runtime' / f).read_text(encoding='utf-8', errors='replace') for f in ['vk_pipeline.c'])
    src = re.sub(r'"\s*\n\s*"', '', src)   # adjacent string literals as one
    missing = [p for p in LOG_LINES if not re.search(p, src)]
    for p in missing:
        print('play_gate_check: FAIL: no log line matching %s in %s' % (p, ', '.join(['vk_pipeline.c'])))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
