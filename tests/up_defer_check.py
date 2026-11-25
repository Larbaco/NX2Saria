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
    work = ROOT / 'build' / 'test' / 'up_defer'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    src = str(ROOT / 'tests' / 'up_defer_check.c')
    rc = 0
    for lang, flags in (('c', ['-std=gnu11']), ('c++', ['-x', 'c++', '-std=gnu++17', '-pedantic'])):
        exe = work / (f'up_defer_check_{lang.replace("+", "x")}' + ('.exe' if os.name == 'nt' else ''))
        subprocess.check_call([compiler()] + flags + ['-O1', '-Wall', '-Wextra', '-Werror', *runtime_include.flags(),
                               src, '-o', str(exe)], env=env)
        print(f'up_defer_check: as {lang}:', flush=True)
        rc |= subprocess.run([str(exe)], env=env).returncode
    return rc


if __name__ == '__main__':
    sys.exit(main())
