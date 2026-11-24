import argparse
import ctypes
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from ctypes import wintypes
from pathlib import Path

if os.name == 'nt':
    import msvcrt
else:
    import fcntl

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from launcher import saves   # noqa: E402  (Import Saves' search, for a save from elsewhere)
DESCRIPTION = ("Run a build of the game unattended on a copy of the user's profile with a backed-up save and "
               'scripted input.')
PROFILE = ROOT / 'build' / 'profile'
SAVES = ROOT / 'build' / 'save-backup'
SLOTS = ROOT / 'build' / 'run-slots'
GPU_LOCK = ROOT / 'build' / '.gpu-run.lock'
LOCK_OFFSET = 1 << 16
MAX_RUNS = int(os.environ.get('NX2SARIA_MAX_RUNS', os.environ.get('NX2NAVI_MAX_RUNS', '3')))
GAME_EXES = ('nx2saria.exe', 'ocarina.exe', 'ocarina', 'nx2navi.exe', 'nx2navi')
NINJA = 'ninja.exe' if os.name == 'nt' else 'ninja'


def count(image):
    if os.name == 'nt':
        out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {image}', '/NH'], capture_output=True, text=True).stdout
        return sum(line.lower().startswith(image) for line in out.splitlines())
    n = 0
    for comm in Path('/proc').glob('[0-9]*/comm'):
        try:
            n += comm.read_text().strip() == image[:15]
        except OSError:
            pass
    return n


def busy(game):
    return count(game) + count(NINJA)


def game_exe(exe_dir):
    for name in GAME_EXES:
        if (exe_dir / name).is_file():
            return exe_dir / name
    sys.exit(f'run_game: {exe_dir} holds none of {", ".join(GAME_EXES)}')


def keep_in_background(proc):
    user32 = ctypes.windll.user32
    user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user32.SetWindowPos.argtypes = (wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                    wintypes.UINT)
    found = []
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def match(hwnd, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == proc.pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    callback = enum_proc(match)
    while proc.poll() is None and not found:
        user32.EnumWindows(callback, 0)
        time.sleep(0.05)
    for hwnd in found:
        user32.SetWindowPos(hwnd, 1, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)


def take_slot():
    SLOTS.mkdir(parents=True, exist_ok=True)
    while True:
        for i in range(MAX_RUNS):
            f = open(SLOTS / f'slot{i}.lock', 'a+b')
            if try_lock(f):
                return f
            f.close()
        time.sleep(5)


def try_lock(f):
    try:
        if os.name == 'nt':
            f.seek(LOCK_OFFSET)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return False
    return True


def holder_of(path):
    try:
        return path.read_text(encoding='utf-8', errors='replace').strip() or 'unknown'
    except OSError:
        return 'unknown'


def take_gpu_lock(exe_dir, poll=2.0):
    GPU_LOCK.parent.mkdir(parents=True, exist_ok=True)
    f = open(GPU_LOCK, 'a+b')
    waiting = False
    while not try_lock(f):
        if not waiting:
            print(f'run_game: --quiet waits for the GPU run lock held by {holder_of(GPU_LOCK)}', flush=True)
            waiting = True
        time.sleep(poll)
    f.seek(0)
    f.truncate()
    f.write(f'pid {os.getpid()}, build {exe_dir}'.encode())
    f.flush()
    return f


def release_gpu_lock(f):
    if f is None:
        return
    try:
        if os.name == 'nt':
            f.seek(LOCK_OFFSET)
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
    except OSError:
        pass
    f.close()


def save_source(save, profile, scratch):
    if Path(save).name == save:
        for d in (SAVES / save, profile / 'save-backup' / save):
            if d.is_dir():
                return d, None, ()
    path = Path(save).expanduser()
    if path.is_file() and zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            if sum(i.file_size for i in z.infolist()) > saves.MAX_BYTES:
                sys.exit(f'run_game: {path} unpacks to more than a save holds')
            z.extractall(scratch)
        path = scratch
    found, skip = saves.find_save(path)
    if found is None:
        sys.exit(f'run_game: {save} is no backup in {SAVES} or {profile / "save-backup"}, and no save folder or .zip')
    return found, saves._one_slot(found), skip


def profile_copy(dest, src, save=None, slot=None, skip=()):
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest)
    if save:
        users = [d for d in (dest / 'save').iterdir() if d.is_dir()]
        if len(users) != 1:
            sys.exit(f'run_game: expected one save directory in {dest / "save"}, found {len(users)}')
        target = users[0] / slot if slot else users[0]
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(save, target, ignore=lambda d, names: [n for n in names if Path(d) == Path(save) and n in skip])
        shutil.rmtree(dest / 'save-tx', ignore_errors=True)


def boot_map_report(log, asked, rc):
    area = asked.split(':', 1)[0]
    text = log.read_text(encoding='utf-8', errors='replace')
    lines = [l.split('] ', 1)[-1] for l in text.splitlines() if 'boot map: ' in l]
    for l in lines:
        print(l)
    arrived = any(re.match(rf'boot map: arrived in {re.escape(area)} \(asked {re.escape(area)}\)', l) for l in lines)
    if not arrived:
        print(f'run_game: no "boot map: arrived in {area}" line in {log}')
    return rc if arrived or rc not in (0, 124) else 3


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('exe_dir', type=Path, help='build directory or game folder holding ' + ', '.join(GAME_EXES))
    ap.add_argument('--save', help="the save to run: a backup's name in build/save-backup or the profile's save-backup, "
                                   "or a save folder or .zip from elsewhere (a crash report's zipped profile folder); "
                                   'default: the profile as it is')
    ap.add_argument('--script', type=Path, help='NX2SARIA_INPUT_SCRIPT (default: tools/input/load_save.txt, or '
                                                 'tools/input/boot_map.txt with --boot-map)')
    ap.add_argument('--boot-map', metavar='AREA[:START]', help='start in that area without the title screen (NX2SARIA_BOOT_MAP)')
    ap.add_argument('--boot-map-slot', metavar='N', help='the slot it loads, 0 to 5 or latest (NX2SARIA_BOOT_MAP_SLOT)')
    ap.add_argument('--boot-map-age', choices=('child', 'adult', 'save'), help='NX2SARIA_BOOT_MAP_AGE')
    ap.add_argument('--boot-map-time', metavar='HH:MM', help='the time of day it starts at (NX2SARIA_BOOT_MAP_TIME)')
    ap.add_argument('--seconds', type=float, required=True,
                    help='game time to run: the run ends once the world clock (game time since the save loaded) passed '
                         'this many seconds (shot is taken of that frame)')
    ap.add_argument('--wall', type=float, help='stop the game after this many seconds of wall time if it has not ended '
                                               '(a script that never reaches the world clock)')
    ap.add_argument('--tag', default='run', help='names the profile copy and the log')
    ap.add_argument('--shot', type=Path, help='PNG of the last frame; turns async pipeline builds off (boot.c)')
    ap.add_argument('--log', type=Path, help='default: <exe dir>/logs/<tag>.log')
    ap.add_argument('--env', action='append', default=[], metavar='KEY=VALUE', help='extra environment, repeatable')
    ap.add_argument('--quiet', action='store_true', help='wait until no other copy of the game or Ninja runs (benchmarks)')
    ap.add_argument('--show', action='store_true',
                    help='Windows: open the window on top; by default it opens behind other windows')
    a = ap.parse_args()
    if a.script is None:
        a.script = ROOT / 'tools' / 'input' / ('boot_map.txt' if a.boot_map else 'load_save.txt')

    exe_dir = a.exe_dir.resolve()
    exe = game_exe(exe_dir)
    profile = exe_dir / 'profile' if (exe_dir / 'profile').is_dir() else PROFILE
    log = (a.log or exe_dir / 'logs' / f'{a.tag}.log').resolve()
    log.parent.mkdir(parents=True, exist_ok=True)
    gpu = take_gpu_lock(exe_dir) if a.quiet else None
    try:
        slot = take_slot()
        if a.quiet:
            while busy(exe.name):
                time.sleep(10)
        prof = exe_dir / f'prof-{a.tag}'
        scratch = exe_dir / f'prof-{a.tag}.import'
        profile_copy(prof, profile, *(save_source(a.save, profile, scratch) if a.save else ()))
        shutil.rmtree(scratch, ignore_errors=True)

        env = dict(os.environ, NX2SARIA_ASSUME_FOCUS='1', NX2SARIA_INPUT='none', NX2SARIA_INPUT_SCRIPT=str(a.script.resolve()))
        if not a.show and os.name == 'nt':
            env['NX2SARIA_BACKGROUND_WINDOW'] = '1'
        for key, value in (('BOOT_MAP', a.boot_map), ('BOOT_MAP_SLOT', a.boot_map_slot),
                           ('BOOT_MAP_AGE', a.boot_map_age), ('BOOT_MAP_TIME', a.boot_map_time)):
            if value:
                env['NX2SARIA_' + key] = value
        for kv in a.env:
            k, _, v = kv.partition('=')
            env[k] = v
        args = [str(exe), '--profile', str(prof), '--seconds', str(a.seconds)]
        if a.shot:
            args += ['--shot', str(a.shot.resolve())]
        try:
            with open(log, 'w', encoding='utf-8', errors='replace') as f:
                f.write(f'run_game: others running before: {exe.name} {count(exe.name)}, {NINJA} {count(NINJA)}\n')
                f.flush()
                start = time.time()
                proc = subprocess.Popen(args, cwd=exe_dir, env=env, stdout=f, stderr=subprocess.STDOUT)
                if not a.show and os.name == 'nt':
                    threading.Thread(target=keep_in_background, args=(proc,), daemon=True).start()
                try:
                    rc = proc.wait(timeout=a.wall)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    rc = 124
                    f.write(f'run_game: stopped at --wall {a.wall:g} s\n')
                f.write(f'run_game: rc={rc} wall={time.time() - start:.0f}s; others running after: '
                        f'{exe.name} {count(exe.name)}, {NINJA} {count(NINJA)}\n')
        finally:
            shutil.rmtree(prof, ignore_errors=True)
    finally:
        release_gpu_lock(gpu)
    if a.boot_map:
        rc = boot_map_report(log, a.boot_map, rc)
    print(f'{log}: rc={rc}')
    sys.exit(rc)


if __name__ == '__main__':
    main()
