"""Build and run tests/library_path_check.c (Linux): runtime/platform/linux.c plat_library_path, which the game log's
"libs:" line (runtime/main.c log_libs) names the loaded SDL with, against the system's loader. A library with a soname
is built beside a program linked as the game is (-no-pie, runpath $ORIGIN), and the line must say "beside the game"
with the folder's real path when the program runs from that folder, through a symbolic link to it, and when an empty
LD_LIBRARY_PATH entry (the current folder) found the library, and "not beside the game" for a copy an LD_LIBRARY_PATH
folder supplies; a library preloaded under its full file name still answers for its soname, and a name nothing
loaded gives nothing. linux.c includes SDL3's headers: the system's, else the folder of tools/sdl3_pin.py's headers
(toolchain/sdl3/include) or SDL3_INCLUDE. No game files.

Usage: library_path_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SONAME = 'libprobe.so.0'


def compiler():
    return os.environ.get('CC') or shutil.which('clang') or 'clang'


def sdl3_include(cc):
    """[] when the compiler finds SDL3's headers itself, else ['-I', folder] of the first folder that has them."""
    probe = subprocess.run([cc, '-E', '-x', 'c', '-', '-o', os.devnull], input='#include <SDL3/SDL_clipboard.h>\n',
                           capture_output=True, text=True)
    if probe.returncode == 0:
        return []
    for folder in (os.environ.get('SDL3_INCLUDE'), ROOT / 'toolchain' / 'sdl3' / 'include'):
        if folder and (Path(folder) / 'SDL3' / 'SDL_clipboard.h').is_file():
            return ['-I', str(folder)]
    return None


def run(exe, *args, cwd=None, env=None):
    p = subprocess.run([str(exe), *args], capture_output=True, text=True, cwd=cwd, env=env, timeout=60)
    fields = dict(line.split('=', 1) for line in p.stdout.splitlines() if '=' in line)
    return p.returncode, fields, p.stderr


def main():
    if not sys.platform.startswith('linux'):
        print('skipped: plat_library_path is checked on Linux (runtime/platform/linux.c)')
        return 0
    cc = compiler()
    inc = sdl3_include(cc)
    if inc is None:
        print('skipped: no SDL3 headers (runtime/platform/linux.c includes SDL3/SDL_clipboard.h); set SDL3_INCLUDE or '
              'run tools/sdl3_pin.py')
        return 0
    work = ROOT / 'build' / 'test' / 'library_path'
    shutil.rmtree(work, ignore_errors=True)
    game, other, link = work / 'game', work / 'other', work / 'link'
    game.mkdir(parents=True)
    other.mkdir()
    lib_src = work / 'probe_lib.c'
    lib_src.write_text('int probe_lib_value(void) { return 42; }\n', encoding='ascii')
    for folder in (game, other):
        subprocess.check_call([cc, '-shared', '-fPIC', f'-Wl,-soname,{SONAME}', str(lib_src), '-o', str(folder / SONAME)])
    full_name = other / 'libprobe.so.0.9'   # a library under its full file name, as a distribution installs it
    shutil.copy2(other / SONAME, full_name)
    rt = ROOT / 'runtime'
    obj = work / 'linux.o'
    subprocess.check_call([cc, '-std=gnu11', '-O1', '-w', '-ffunction-sections', '-fdata-sections', '-I', str(rt), *inc,
                           '-c', str(rt / 'platform' / 'linux.c'), '-o', str(obj)])
    exe = game / 'library_path_check'
    lld = ['-fuse-ld=lld'] if shutil.which('ld.lld') else []
    subprocess.check_call([cc, '-std=gnu11', '-O1', '-Wall', '-Werror', *lld, '-no-pie', '-Wl,--gc-sections',
                           '-Wl,-rpath,$ORIGIN', '-I', str(rt), str(ROOT / 'tests' / 'library_path_check.c'),
                           str(rt / 'lib_origin.c'), str(obj), '-L', str(game), f'-l:{SONAME}', '-lm', '-lpthread',
                           '-ldl', '-o', str(exe)])
    link.symlink_to(game, target_is_directory=True)
    real_game = os.path.realpath(game)
    base = {k: v for k, v in os.environ.items() if k not in ('LD_LIBRARY_PATH', 'LD_PRELOAD')}
    cases = [
        ('from its folder', exe, None, {}, f'{real_game}/{SONAME}', 'beside the game'),
        ('through a symbolic link to its folder', link / exe.name, None, {}, f'{real_game}/{SONAME}', 'beside the game'),
        ('an empty LD_LIBRARY_PATH entry', exe, game, {'LD_LIBRARY_PATH': f'{work / "none"}:'},
         f'{real_game}/{SONAME}', 'beside the game'),
        ('an LD_LIBRARY_PATH folder', exe, None, {'LD_LIBRARY_PATH': str(other)},
         f'{os.path.realpath(other)}/{SONAME}', 'not beside the game'),
        ('preloaded under its full file name', exe, None, {'LD_PRELOAD': str(full_name)},
         f'{os.path.realpath(other)}/{full_name.name}', 'not beside the game'),
    ]
    bad = 0
    for what, prog, cwd, extra, want_path, want_where in cases:
        rc, out, err = run(prog, SONAME, cwd=cwd, env={**base, **extra})
        line = out.get('line', '')
        if rc or out.get('found') != '1' or out.get('path') != want_path or f', {want_where}: {want_path}' not in line:
            print(f'FAIL {what}: exit {rc}, found {out.get("found")}, path {out.get("path")}\n  line {line}\n  want '
                  f'{want_where}: {want_path}\n{err}')
            bad += 1
        else:
            print(f'{what}: {want_where}, {want_path}')
    rc, out, err = run(exe, 'libnothing-loaded.so.1', env=base)
    if rc or out.get('found') != '0' or 'its file not found among the loaded modules' not in out.get('line', ''):
        print(f'FAIL a name nothing loaded: exit {rc}, {out}\n{err}')
        bad += 1
    else:
        print('a name nothing loaded: not found')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
