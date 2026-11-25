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
    work = ROOT / 'build' / 'test' / 'cache_stamp'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('cache_stamp_check.exe' if os.name == 'nt' else 'cache_stamp_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Werror', *runtime_include.flags(),
                           str(ROOT / 'tests' / 'cache_stamp_check.c'), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env).returncode | log_lines() | uses()


LOG_LINES = ['"vk: pipeline cache stamp %02x%02x%02x%02x %s: %s%s, %\\.1f MB', '"vk: pipeline cache stamp off \\(', 'cache_stamp_state_name\\(state\\), cache_stamp_action_name\\(act\\)']


def log_lines():
    src = ''.join((ROOT / 'runtime' / f).read_text(encoding='utf-8', errors='replace') for f in ['vk/vk_pipeline.c'])
    src = re.sub(r'"\s*\n\s*"', '', src)
    missing = [p for p in LOG_LINES if not re.search(p, src)]
    for p in missing:
        print('cache_stamp_check: FAIL: no log line matching %s in %s' % (p, ', '.join(['vk/vk_pipeline.c'])))
    return 1 if missing else 0


USES = [
    r'P\.base_at_exit = P\.cache_path\[0\] && cache_base_at_exit\(prep_mode, plat_file_size\(P\.cache_path\)\);',
    r'if \(cache_save_writes_base\(P\.play != VK_NULL_HANDLE, P\.fold, P\.base_at_exit, exiting\)\)\s*'
    r'ok = cache_save_one\(0, wait_ms, exiting\);',
    r'"vk_pipeline: what this run compiles goes into %s; %s %s", P\.play_path, P\.cache_path,\s*'
    r'P\.base_at_exit \? "was not there at the start, so the save at this run\'s exit writes it once',
    r'int at_exit = cache_base_at_exit\(prep_mode, plat_file_size\(P\.cache_path\)\);',
    r'P\.saved\[0\] = !size \|\| kept == size \? kept : 0;',
]


def uses():
    src = (ROOT / 'runtime' / 'vk' / 'vk_pipeline.c').read_text(encoding='utf-8', errors='replace')
    src = re.sub(r'"\s*\n\s*"', '', src)
    missing = [p for p in USES if not re.search(p, src)]
    for p in missing:
        print('cache_stamp_check: FAIL: vk_pipeline.c does not apply the rule as %s' % p)
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
