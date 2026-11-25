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
    work = ROOT / 'build' / 'test' / 'part_file'
    shutil.rmtree(work / 'files', ignore_errors=True)
    (work / 'files').mkdir(parents=True)
    win = os.name == 'nt'
    exe = work / ('part_file_check.exe' if win else 'part_file_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    plat += stand_ins(work)
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32'] if win \
        else ['-lpthread']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing',
                           '-march=x86-64-v3', *runtime_include.flags(rt),
                           str(ROOT / 'tests' / 'part_file_check.c'), *map(str, plat), '-o', str(exe), '-lm', *libs],
                          env=env)
    files = work / 'files'
    code = subprocess.run([str(exe), str(files)], env=env, timeout=120).returncode
    if code:
        return code

    killed = files / 'killed.tsv'
    killed.write_text('the whole older file\n', encoding='utf-8')
    p = subprocess.Popen([str(exe), str(files), 'hang'], env=env, stdout=subprocess.PIPE, text=True)
    try:
        said = p.stdout.readline().strip()
    finally:
        p.kill()
        p.wait()
        p.stdout.close()
    fails = []
    if said != 'writing':
        fails.append(f'the writer did not start ({said!r})')
    if killed.read_text(encoding='utf-8') != 'the whole older file\n':
        fails.append('the older file changed although the new one was never complete')
    part = killed.with_name('killed.tsv.part')
    if not part.is_file() or part.read_text(encoding='utf-8') != 'the first half of the new file\n':
        fails.append('the half that was written is not in killed.tsv.part')
    for f in fails:
        print('FAIL:', f)
    print('part_file_check: the killed writer left %s' % ('a wrong file' if fails else 'the older file whole'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
