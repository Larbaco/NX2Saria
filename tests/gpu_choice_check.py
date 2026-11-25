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
    work = ROOT / 'build' / 'test' / 'gpu_choice'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('gpu_choice_check.exe' if os.name == 'nt' else 'gpu_choice_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    cc = compiler()
    subprocess.check_call([cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', *runtime_include.flags(),
                           str(ROOT / 'tests' / 'gpu_choice_check.c'), '-o', str(exe)], env=env)
    cxx = work / 'gpu_choice_cxx.cpp'
    cxx.write_text('#include "gpu_choice.h"\nint main() { GpucCard c = {"x", GPUC_DISCRETE, 1}; int d, a; char l[256];'
                   ' gpuc_prepare_line(&c, 1, 0, GPUC_BY_AUTO, -1, "x", l, sizeof l);'
                   ' gpuc_legacy_line(&c, 0, 0, l, sizeof l);'
                   ' return gpuc_index(&c, 1, 0, "x", &d) + gpuc_shown(&c, 1, -1, "x", 0) +'
                   ' gpuc_resolve(&c, 1, 0, "x", 0, &d, &a) + gpuc_legacy_applies(&c, 1, 0, "x", 0); }\n',
                   encoding='utf-8')
    subprocess.check_call([cc, '-x', 'c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', *runtime_include.flags(), str(cxx), '-o', str(work / 'gpu_choice_cxx')], env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
