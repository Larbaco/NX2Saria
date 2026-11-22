# needs: clang
"""Build and run tests/gpu_choice_check.c: runtime/vk/gpu_choice.h, which graphics card gpu_index and gpu_name pick when
the cards are listed in another order than when the settings were written (a gpu_index alone that lists a card that is not a
discrete one gives way to Auto), and the log lines about the choice, the shader preparation's among them.

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
    cxx.write_text('#include "gpu_choice.h"\nint main() { GpucCard c = {"x", GPUC_DISCRETE, 1}; int d, a; char l[256];'
                   ' gpuc_prepare_line(&c, 1, 0, GPUC_BY_AUTO, -1, "x", l, sizeof l);'
                   ' gpuc_legacy_line(&c, 0, 0, l, sizeof l);'
                   ' return gpuc_index(&c, 1, 0, "x", &d) + gpuc_shown(&c, 1, -1, "x", 0) +'
                   ' gpuc_resolve(&c, 1, 0, "x", 0, &d, &a) + gpuc_legacy_applies(&c, 1, 0, "x", 0); }\n',
                   encoding='utf-8')
    subprocess.check_call([cc, '-x', 'c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-I',
                           str(ROOT / 'runtime'), str(cxx), '-o', str(work / 'gpu_choice_cxx')], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
