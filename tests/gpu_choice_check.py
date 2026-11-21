"""Build and run tests/gpu_choice_check.c: runtime/gpu_choice.h, which graphics card gpu_index and gpu_name pick when
the cards are listed in another order than when the settings were written, and the log lines about the choice.

Needs a C compiler only: no Vulkan headers, no GPU and no game files. The header is built as C++ too, as the menu
(runtime/ui/menu.cpp) includes it.

Usage: gpu_choice_check.py
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
    work = ROOT / 'build' / 'test' / 'gpu_choice'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('gpu_choice_check.exe' if os.name == 'nt' else 'gpu_choice_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    cc = compiler()
    subprocess.check_call([cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'gpu_choice_check.c'), '-o', str(exe)], env=env)
    # the menu is C++: the header alone must compile there too
    cxx = work / 'gpu_choice_cxx.cpp'
    cxx.write_text('#include "gpu_choice.h"\nint main() { GpucCard c = {"x", GPUC_DISCRETE, 1}; int d;'
                   ' return gpuc_index(&c, 1, 0, "x", &d) + gpuc_shown(&c, 1, -1, "x"); }\n', encoding='utf-8')
    subprocess.check_call([cc, '-x', 'c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-I',
                           str(ROOT / 'runtime'), str(cxx), '-o', str(work / 'gpu_choice_cxx')], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
