# needs: clang
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
try:
    from common import MSYS_BIN, compiler  # noqa: E402
except ImportError:
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
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', *runtime_include.flags(),
                           str(ROOT / 'tests' / 'play_gate_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode | log_lines()


LOG_LINES = ['"vk: background builds paused in play: %llu frames, %llu builds deferred', '"vk: background builds rest in play: while the frame shown has %d draws or more', '"vk: background builds rest in play: off \\(']


def log_lines():
    src = ''.join((ROOT / 'runtime' / f).read_text(encoding='utf-8', errors='replace') for f in ['vk/vk_pipeline.c'])
    src = re.sub(r'"\s*\n\s*"', '', src)
    missing = [p for p in LOG_LINES if not re.search(p, src)]
    for p in missing:
        print('play_gate_check: FAIL: no log line matching %s in %s' % (p, ', '.join(['vk/vk_pipeline.c'])))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
