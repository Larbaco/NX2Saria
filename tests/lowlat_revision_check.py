# needs: clang, vma, vulkan
import os
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
REVISIONS = ('1', '2', '3')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def vma_include():
    for d in (os.environ.get('VMA_INCLUDE'), MSYS_BIN.parent / 'include' / 'vma', ROOT / 'toolchain' / 'vma',
              Path('/usr/include')):
        if d and (Path(d) / 'vk_mem_alloc.h').is_file():
            return Path(d)
    sys.exit('lowlat_revision_check: vk_mem_alloc.h not found (docs/building.md)')


def main():
    work = ROOT / 'build' / 'test' / 'lowlat_revision'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('lowlat_revision_check.exe' if os.name == 'nt' else 'lowlat_revision_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function',
                           '-Wno-bitwise-instead-of-logical', '-fno-strict-aliasing', *runtime_include.flags(),
                           '-I', str(vma_include()), str(ROOT / 'tests' / 'lowlat_revision_check.c'), '-o', str(exe)],
                          env=env)
    bad = 0
    for rev in REVISIONS:
        bad += subprocess.run([str(exe), rev], env=env).returncode != 0
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
