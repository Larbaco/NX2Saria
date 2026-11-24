# needs: clang
import os
import shutil
import subprocess
import sys
from pathlib import Path

from platform_stand_ins import stand_ins
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
    work = ROOT / 'build' / 'test' / 'frame_owed'
    work.mkdir(parents=True, exist_ok=True)
    win = os.name == 'nt'
    exe = work / ('frame_owed_check.exe' if win else 'frame_owed_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    plat += stand_ins(work)
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', *runtime_include.flags(rt), str(ROOT / 'tests' / 'frame_owed_check.c'),
                           *map(str, plat), '-o', str(exe), '-lm', *libs], env=env)
    try:
        return subprocess.run([str(exe)], env=env, timeout=300).returncode
    except subprocess.TimeoutExpired:
        print('frame_owed_check: FAIL: the check did not end within 300 s')
        return 1


if __name__ == '__main__':
    sys.exit(main())
