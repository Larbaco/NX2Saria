# needs: clang
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
import runtime_include

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
SOURCE = ROOT / 'runtime' / 'vk' / 'vk_dlaa.c'
ENTRY = re.compile(r'N\.(?:init|req_ext|caps|alloc_params|destroy_params|create|evaluate|release|shutdown1)\s*\(|'
                   r'N\.feat_inst_req\)\s*\(|\(NgxParam \*\)\)cb\)\s*\(')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def unlocked(text):
    bad = []
    for no, line in enumerate(text.splitlines(), 1):
        for m in ENTRY.finditer(line):
            if 'NGX_CALL(' not in line[:m.start()]:
                bad.append((no, line.strip()))
    return bad


def main():
    bad = unlocked(SOURCE.read_text(encoding='utf-8'))
    for no, line in bad:
        print(f'ngx_lock_check: {SOURCE.relative_to(ROOT).as_posix()}:{no}: an NGX call outside NGX_CALL: {line}')
    print(f'ngx_lock_check: {len(bad)} NGX calls in vk_dlaa.c outside the lock')
    work = ROOT / 'build' / 'test' / 'ngx_lock'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('ngx_lock_check.exe' if os.name == 'nt' else 'ngx_lock_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-pthread', *runtime_include.flags(),
                           str(ROOT / 'tests' / 'ngx_lock_check.c'), '-o', str(exe)], env=env)
    run = subprocess.run([str(exe)], env=env).returncode
    return 1 if bad or run else 0


if __name__ == '__main__':
    sys.exit(main())
