import ctypes
import ctypes.util
import fcntl
import importlib.util
import os
import shutil
import signal
import subprocess
from pathlib import Path

from . import game_executable


def make_game_toolchain_args(layout):
    return []


def ensure_toolchain(layout, cancel=None, log=None, version=''):
    missing = toolchain_missing()
    if missing:
        if log:
            log('toolchain: Linux check missing ' + missing.splitlines()[0].split(': ', 1)[1].rstrip('.'))
        from ..toolchain import ToolchainError
        raise ToolchainError(missing)
    if log:
        log('toolchain: Linux system requirements ok')
    from .. import toolchain
    toolchain.ensure_zbic(layout, cancel=cancel, log=log, version=version)


def refresh_installed_wrapper(root, app):
    root, app = Path(root), Path(app)
    if not (root / 'nx2saria-launcher').is_file():
        return
    script = app / 'tools' / 'appimage_install.py'
    if not script.is_file():
        return
    try:
        spec = importlib.util.spec_from_file_location('_nx2saria_appimage_install', script)
        if spec and spec.loader:
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            module._write_wrapper(root)
    except (ImportError, OSError, AttributeError, ValueError):
        pass


def game_running(game_dir, exe=None):
    folder = Path(game_dir)
    lock = folder / 'profile' / 'instance.lock'
    try:
        with lock.open('rb') as f:
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return True
            fcntl.flock(f, fcntl.LOCK_UN)
    except FileNotFoundError:
        pass
    except OSError:
        pass
    exe = Path(exe or game_executable(folder))
    if not exe.is_file():
        return False
    try:
        expected = exe.resolve()
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():
                continue
            try:
                actual = Path(os.readlink(proc / 'exe').removesuffix(' (deleted)'))
                if actual == expected:
                    return True
            except (FileNotFoundError, PermissionError, OSError):
                continue
    except OSError:
        return False
    return False


def child_subreaper(on, prctl=None):
    if prctl is None:
        prctl = ctypes.CDLL(None, use_errno=True).prctl
    if prctl(36, 1 if on else 0, 0, 0, 0):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return True


def _runs(proc, expected, readlink=os.readlink):
    try:
        return Path(readlink(proc / 'exe').removesuffix(' (deleted)')) == expected
    except OSError:
        return False


class GameCopy:
    def __init__(self, pid, exe, proc_root='/proc', readlink=os.readlink):
        self.pid, self.exe, self.proc_root, self.readlink = int(pid), Path(exe), Path(proc_root), readlink
        self.returncode, self.ended = None, False

    def poll(self):
        if self.ended:
            return self.returncode
        try:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
        except ChildProcessError:
            if not _runs(self.proc_root / str(self.pid), self.exe, self.readlink):
                self.ended = True
            return self.returncode
        if pid:
            self.returncode = os.waitstatus_to_exitcode(status)
            self.ended = True
        return self.returncode


def game_copy(exe, skip=None, proc_root='/proc', readlink=os.readlink):
    expected = Path(exe).resolve()
    try:
        for proc in Path(proc_root).iterdir():
            if proc.name.isdigit() and int(proc.name) != skip and _runs(proc, expected, readlink):
                return GameCopy(proc.name, expected, proc_root, readlink)
    except OSError:
        pass
    return None


def ended_copies(exe, skip=None, proc_root='/proc', me=None, wait=None):
    me = os.getpid() if me is None else me
    wait = wait or (lambda pid: os.waitpid(pid, os.WNOHANG))
    name, found = Path(exe).name[:15], []
    try:
        procs = list(Path(proc_root).iterdir())
    except OSError:
        return []
    for proc in procs:
        if not proc.name.isdigit() or int(proc.name) == skip:
            continue
        try:
            text = (proc / 'stat').read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        comm, _, rest = text.partition('(')[2].rpartition(')')
        fields = rest.split()
        if comm != name or len(fields) < 20 or fields[0] != 'Z' or fields[1] != str(me):
            continue
        try:
            pid, status = wait(int(proc.name))
        except ChildProcessError:
            continue
        if pid:
            found.append((int(fields[19]), os.waitstatus_to_exitcode(status)))
    return [code for _start, code in sorted(found)]


APPIMAGE_OWNED = ('nx2saria-launcher', 'nx2saria-launcher.new', 'nx2saria.png', 'nx2saria.png.new', 'app', 'python',
                  'python.new', 'python.old', 'python.failed', 'pydeps', 'toolchain')
KEPT_DATA = ('logs',)


def desktop_entry(env=None, home=None):
    env = os.environ if env is None else env
    home = Path.home() if home is None else Path(home)
    value = env.get('XDG_DATA_HOME', '')
    data_home = Path(value) if value and Path(value).is_absolute() else home / '.local' / 'share'
    return data_home / 'applications' / 'nx2saria.desktop'


def appimage_install(root):
    root = Path(root)
    return (root / 'nx2saria-launcher').is_file() or (root / 'data' / 'appimage-install.json').is_file()


def _exec_line(wrapper):
    out = str(wrapper)
    for ch in '\\"`$':
        out = out.replace(ch, '\\' + ch)
    return 'Exec="' + out.replace('\\', '\\\\').replace('%', '%%') + '"'


def _starts(entry, wrapper):
    try:
        text = entry.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return False
    return _exec_line(wrapper) in text.splitlines()


def _within(path, folder):
    pairs = [(Path(os.path.abspath(path)), Path(os.path.abspath(folder)))]
    try:
        pairs.append((Path(path).resolve(), Path(folder).resolve()))
    except (OSError, RuntimeError):
        return True
    return any(p == f or f in p.parents for p, f in pairs)


def _unsafe_game_folder(game, root, home):
    if _within(root, game):
        return "it holds the launcher's folder"
    if _within(home, game):
        return 'it holds the home folder'
    try:
        if os.path.ismount(game) or Path(game).resolve() == Path('/'):
            return 'it is the root of a drive'
    except (OSError, RuntimeError):
        return 'it cannot be checked'
    return ''


def uninstall_plan(root, remove_game='', keep=(), entry=None, home=None):
    root = Path(root)
    home = Path.home() if home is None else Path(home)
    entry = desktop_entry(home=home) if entry is None else Path(entry)
    keep = [Path(k) for k in keep if k]
    gone, kept = [], []
    if remove_game:
        why = _unsafe_game_folder(remove_game, root, home)
        if why:
            kept.append((Path(remove_game), f'the game folder, as {why}'))
        else:
            gone.append(Path(remove_game))
    gone += [root / name for name in APPIMAGE_OWNED]
    try:
        gone += sorted(p for p in (root / 'data').iterdir() if p.name not in KEPT_DATA)
    except OSError:
        pass
    kept += [(root / 'data' / name, "the launcher's logs") for name in KEPT_DATA if (root / 'data' / name).exists()]
    if _starts(entry, root / 'nx2saria-launcher'):
        gone += [entry, entry.with_name(entry.name + '.new')]
    for path in list(gone):
        held = next((k for k in keep if _within(k, path)), None)
        if held is not None:
            gone.remove(path)
            kept.append((path, f'it holds {held}'))
    return gone, [root / 'data', root], kept


UNINSTALL_SCRIPT = r"""pid=$1; log=$2; shift 2
i=0
while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
n=0; failed=0
while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do
  if [ -e "$1" ] || [ -L "$1" ]; then
    if rm -rf -- "$1"; then n=$((n + 1)); else failed=$((failed + 1)); fi
  fi
  shift
done
[ "$#" -gt 0 ] && shift
for d in "$@"; do rmdir -- "$d" 2>/dev/null; done
if [ -d "$(dirname -- "$log")" ]; then
  printf '%s uninstall: removed %s paths after the window closed, %s could not be removed
'     "$(date +%H:%M:%S)" "$n" "$failed" >> "$log"
fi
"""


def uninstall_command(pid, gone, empty, log):
    return ['sh', '-c', UNINSTALL_SCRIPT, 'sh', str(pid), str(log), *map(str, gone), '--', *map(str, empty)]


UDMABUF_MINIMUM_MB = 1024


def udmabuf_status(device='/dev/udmabuf', parameter='/sys/module/udmabuf/parameters/size_limit_mb',
                  which=shutil.which, read_text=None, accessible=None, os_release='/etc/os-release'):
    device, parameter = Path(device), Path(parameter)
    read_text = read_text or (lambda path: path.read_text(encoding='utf-8').strip())
    accessible = accessible or (lambda path: os.access(path, os.R_OK | os.W_OK))
    try:
        limit = int(read_text(parameter))
    except (OSError, ValueError):
        return None
    device_accessible = bool(accessible(device))
    steamos = False
    try:
        steamos = any(line.strip() == 'ID=steamos' for line in Path(os_release).read_text(encoding='utf-8').splitlines())
    except OSError:
        pass
    needs_fix = not device_accessible or limit < UDMABUF_MINIMUM_MB
    reason = ('module writable only on SteamOS' if steamos else 'pkexec not installed') if needs_fix and (
        steamos or not which('pkexec')) else ''
    offered = bool(needs_fix and not reason)
    return {'accessible': device_accessible, 'limit_mb': limit, 'needs_fix': needs_fix,
            'offered': offered, 'reason': reason, 'steamos': steamos}


def run_udmabuf_fix(script, action, run=subprocess.run):
    if action not in ('apply', 'remove'):
        raise ValueError('unknown udmabuf action')
    try:
        content = Path(script).read_text(encoding='utf-8')
        result = run(['pkexec', '/bin/sh', '-c', content, 'udmabuf_fix', action],
                     capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as error:
        return False, str(error)[:240]
    if result.returncode:
        if result.returncode == 126:
            return False, 'authorization cancelled'
        detail = (result.stderr or result.stdout or f'exit status {result.returncode}').strip()
        return False, detail[:240]
    return True, (result.stdout or 'completed').strip()[:240]


def udmabuf_after_log_line(action, status):
    if status is None:
        return f'udmabuf: after {action}: accessible=no; size_limit_mb=unknown (module absent)'
    accessible = 'yes' if status['accessible'] else 'no'
    return f"udmabuf: after {action}: accessible={accessible}; size_limit_mb={status['limit_mb']}"[:240]


def udmabuf_log_line(status):
    if status is None:
        return 'udmabuf: module absent; offer=none'
    accessible = 'yes' if status['accessible'] else 'no'
    offered = 'yes' if status['offered'] else 'no'
    reason = f" ({status['reason']})" if status['reason'] else ''
    return (f"udmabuf: accessible={accessible}; size_limit_mb={status['limit_mb']}; "
            f"offer={'offered' if offered == 'yes' else 'none'}{reason}")[:240]


WRITE_WATCH_MIN = (6, 7)
_USERFAULTFD = {'x86_64': 323, 'aarch64': 282}
_UFFDIO_API = 0xC018AA3F
_UFFD_FEATURES = (1 << 15) | (1 << 13)


def _kernel_old(release):
    parts = []
    for piece in (release or '').split('.')[:2]:
        digits = ''
        for ch in piece[:6]:
            if not ch.isdigit():
                break
            digits += ch
        if not digits:
            break
        parts.append(int(digits))
        if len(digits) < len(piece):
            break
    if not parts:
        return None
    return tuple(parts + [0])[:2] < WRITE_WATCH_MIN


def write_watch_verdict(release, probe_err):
    old = _kernel_old(release)
    if probe_err is None:
        return 'old' if old else 'on'
    if not probe_err:
        return 'on'
    return 'old' if old else 'refused'


def write_watch_probe(machine=None):
    nr = _USERFAULTFD.get(machine or os.uname().machine)
    if nr is None:
        return None
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        fd = libc.syscall(ctypes.c_long(nr), ctypes.c_int(os.O_CLOEXEC | os.O_NONBLOCK | 1))
        if fd < 0:
            return ctypes.get_errno() or None
        try:
            api = (ctypes.c_uint64 * 3)(0xAA, _UFFD_FEATURES, 0)
            if libc.ioctl(ctypes.c_int(fd), ctypes.c_ulong(_UFFDIO_API), api) != 0:
                return ctypes.get_errno() or None
        finally:
            os.close(fd)
        try:
            os.close(os.open('/proc/self/pagemap', os.O_RDONLY | os.O_CLOEXEC))
        except OSError as error:
            return error.errno or None
        return 0
    except (OSError, AttributeError):
        return None


def write_watch_status(release=None, probe=write_watch_probe, env=None):
    import errno
    env = os.environ if env is None else env
    release = os.uname().release if release is None else release
    forced = env.get('NX2SARIA_TEST_NO_UFFD', '')
    if forced and forced != '0':
        err = errno.ENOSYS if forced == '1' else errno.EINVAL
        release = release if forced == '1' else forced
    else:
        err = probe()
    return {'verdict': write_watch_verdict(release, err), 'release': release, 'error': err, 'forced': bool(forced)
            and forced != '0'}


def write_watch_log_line(status):
    err = status['error']
    probe = 'not probed' if err is None else 'probe ok' if not err else f'probe failed: {os.strerror(err)}'
    forced = '; forced by NX2SARIA_TEST_NO_UFFD' if status.get('forced') else ''
    return f"write watch: {status['verdict']} (kernel {status['release']}; {probe}{forced})"[:240]


def write_watch_text(status):
    verdict, release = status['verdict'], status['release'] or 'unknown'
    need = f'{WRITE_WATCH_MIN[0]}.{WRITE_WATCH_MIN[1]}'
    if verdict == 'old':
        return ('Kernel Too Old', f'Linux {release} is older than {need}: the game cannot track which memory it wrote '
                'and runs much slower, at about 1 fps where its memory is not shared with the GPU through udmabuf. '
                f'Update the system to Linux {need} or later before you play.')
    if verdict == 'refused':
        err = status['error']
        why = f' ({os.strerror(err)})' if err else ''
        return ('Memory Tracking Off', f'Linux {release} refused the memory tracking the game needs{why}: it runs '
                'much slower, at about 1 fps where its memory is not shared with the GPU through udmabuf. The game '
                f'needs Linux {need} or later built with userfaultfd, and no sandbox that blocks it (a Flatpak or '
                'container profile).')
    return None


def udmabuf_fix_installed(rules='/etc/udev/rules.d/70-udmabuf-uaccess.rules',
                           tmpfiles='/etc/tmpfiles.d/udmabuf.conf'):
    return Path(rules).is_file() or Path(tmpfiles).is_file()


def udmabuf_change_available(which=shutil.which, os_release='/etc/os-release'):
    try:
        steamos = any(line.strip() == 'ID=steamos'
                      for line in Path(os_release).read_text(encoding='utf-8').splitlines())
    except OSError:
        steamos = False
    return not steamos and bool(which('pkexec'))


def os_name(os_release='/etc/os-release'):
    fields = {}
    try:
        for line in Path(os_release).read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, value = line.partition('=')
            if sep:
                fields[key.strip()] = value.strip().strip('"\'')
    except OSError:
        pass
    return (fields.get('PRETTY_NAME') or ' '.join(v for v in (fields.get('NAME'), fields.get('VERSION_ID')) if v)
            or 'Linux')


def session_text(env=None):
    env = os.environ if env is None else env
    kind = env.get('XDG_SESSION_TYPE') or ('wayland' if env.get('WAYLAND_DISPLAY') else
                                           'x11' if env.get('DISPLAY') else 'unknown')
    desktop = env.get('XDG_CURRENT_DESKTOP') or env.get('DESKTOP_SESSION') or 'unknown'
    return f'{kind} session, desktop {desktop}'


USER_DIRS = {'downloads': 'XDG_DOWNLOAD_DIR', 'desktop': 'XDG_DESKTOP_DIR', 'documents': 'XDG_DOCUMENTS_DIR',
             'pictures': 'XDG_PICTURES_DIR'}


def user_dir(name, env=None, home=None):
    env = os.environ if env is None else env
    home = Path(home or Path.home())
    key = USER_DIRS.get(name)
    config = env.get('XDG_CONFIG_HOME', '')
    config = Path(config) if config.startswith('/') else home / '.config'
    if not key:
        return None
    try:
        lines = (config / 'user-dirs.dirs').read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return None
    for line in lines:
        k, sep, value = line.strip().partition('=')
        if not sep or k.strip() != key:
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        if value.startswith('$HOME'):
            path = Path(str(home) + value[len('$HOME'):])
        elif value.startswith('/'):
            path = Path(value)
        else:
            return None
        return None if path.resolve() == home.resolve() else path
    return None


LOCAL_FILESYSTEMS = {'ext2', 'ext3', 'ext4', 'btrfs', 'xfs', 'f2fs', 'jfs', 'reiserfs', 'zfs', 'bcachefs', 'ntfs',
                     'ntfs3', 'fuseblk', 'exfat', 'vfat', 'msdos', 'hfsplus', 'udf'}


def _unescape_mount(path):
    import re
    return re.sub(r'\\([0-7]{3})', lambda m: chr(int(m.group(1), 8)), path)


def mounted_drives(mounts='/proc/self/mounts', user=None):
    user = user or os.environ.get('USER') or os.environ.get('LOGNAME') or ''
    bases = ['/mnt/', '/run/media/'] + ([f'/media/{user}/'] if user else [])
    out = []
    try:
        lines = Path(mounts).read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return out
    for line in lines:
        fields = line.split()
        if len(fields) < 3 or fields[2] not in LOCAL_FILESYSTEMS:
            continue
        target = _unescape_mount(fields[1])
        if any(target.startswith(b) and len(target) > len(b) for b in bases) and Path(target) not in out:
            out.append(Path(target))
    return out


def pick_path(kind, title, start, patterns=(), which=shutil.which, run=subprocess.run, env=None):
    env = os.environ if env is None else env
    start = str(start or Path.home())
    order = ['kdialog', 'zenity'] if 'KDE' in env.get('XDG_CURRENT_DESKTOP', '').upper() else ['zenity', 'kdialog']
    for tool in order:
        if not which(tool):
            continue
        if tool == 'zenity':
            cmd = ['zenity', '--file-selection', '--title', title, '--filename', start.rstrip('/') + '/']
            if kind == 'folder':
                cmd.append('--directory')
            cmd += [f'--file-filter={label} | {pattern}' for label, pattern in patterns]
        else:
            if kind == 'folder':
                cmd = ['kdialog', '--title', title, '--getexistingdirectory', start]
            else:
                cmd = ['kdialog', '--title', title, '--getopenfilename', start,
                       '\n'.join(f'{pattern}|{label}' for label, pattern in patterns)]
        try:
            r = run(cmd, capture_output=True)
        except (OSError, subprocess.SubprocessError):
            continue
        if r.returncode == 0:
            out = r.stdout if isinstance(r.stdout, str) else os.fsdecode(r.stdout)
            return tool, out.strip('\n')
        if r.returncode == 1:
            return tool, ''
    return None, ''


def _sdl_library():
    bundled = next((root / 'toolchain' / 'sdl3' / 'lib' / 'libSDL3.so.0'
                    for root in Path(__file__).resolve().parents
                    if (root / 'toolchain' / 'sdl3' / 'lib' / 'libSDL3.so.0').is_file()), None)
    system = ctypes.util.find_library('SDL3') or 'libSDL3.so.0'
    candidates = ([(str(bundled), 'SDL3 from the kit')] if bundled else []) + [(system, 'SDL3 from system')]
    for candidate, source in candidates:
        if not candidate:
            continue
        try:
            return ctypes.CDLL(candidate), source
        except OSError:
            pass
    return None, 'none (SDL3 library not found)'


class Gamepad:
    BUTTONS = {11: 'up', 12: 'down', 13: 'left', 14: 'right', 0: 'a', 1: 'b', 3: 'y'}
    AXIS_LEFT_X, AXIS_LEFT_Y, AXIS_RIGHT_Y = 0, 1, 3
    INIT_GAMEPAD = 0x00002000

    def __init__(self):
        self.dll, self.description = _sdl_library()
        self.pad = None
        self.held = set()
        self.scroll = 0
        if not self.dll:
            return
        try:
            self.dll.SDL_InitSubSystem.argtypes = [ctypes.c_uint32]
            self.dll.SDL_InitSubSystem.restype = ctypes.c_bool
            self.dll.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
            self.dll.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_int32)
            self.dll.SDL_OpenGamepad.argtypes = [ctypes.c_int32]
            self.dll.SDL_OpenGamepad.restype = ctypes.c_void_p
            self.dll.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
            self.dll.SDL_GetGamepadButton.restype = ctypes.c_bool
            self.dll.SDL_GetGamepadAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
            self.dll.SDL_GetGamepadAxis.restype = ctypes.c_int16
            self.dll.SDL_PumpEvents.argtypes = []
            self.dll.SDL_PumpEvents.restype = None
            self.dll.SDL_free.argtypes = [ctypes.c_void_p]
            self.dll.SDL_free.restype = None
            self.dll.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
            self.dll.SDL_CloseGamepad.restype = None
            self.dll.SDL_GamepadConnected.argtypes = [ctypes.c_void_p]
            self.dll.SDL_GamepadConnected.restype = ctypes.c_bool
            if not self.dll.SDL_InitSubSystem(self.INIT_GAMEPAD):
                self.description = 'none (SDL3 gamepad initialization failed)'
                self.dll = None
                return
            self._open_first()
        except (AttributeError, OSError):
            self.description = 'none (SDL3 gamepad API unavailable)'
            self.dll = None
            self.pad = None

    def _open_first(self):
        if not self.dll or self.pad:
            return
        count = ctypes.c_int()
        ids = self.dll.SDL_GetGamepads(ctypes.byref(count))
        if ids and count.value:
            self.pad = self.dll.SDL_OpenGamepad(ids[0])
            self.dll.SDL_free(ids)

    def poll(self, active=True):
        self.scroll = 0
        if not self.dll:
            return set()
        try:
            self.dll.SDL_PumpEvents()
            if self.pad and not self.dll.SDL_GamepadConnected(self.pad):
                self.dll.SDL_CloseGamepad(self.pad)
                self.pad = None
            self._open_first()
            down = set()
            if self.pad:
                down = {name for button, name in self.BUTTONS.items()
                        if self.dll.SDL_GetGamepadButton(self.pad, button)}
                lx = self.dll.SDL_GetGamepadAxis(self.pad, self.AXIS_LEFT_X)
                ly = self.dll.SDL_GetGamepadAxis(self.pad, self.AXIS_LEFT_Y)
                ry = self.dll.SDL_GetGamepadAxis(self.pad, self.AXIS_RIGHT_Y)
                if ly < -16000:
                    down.add('up')
                elif ly > 16000:
                    down.add('down')
                if lx < -16000:
                    down.add('left')
                elif lx > 16000:
                    down.add('right')
                if active and ry < -16000:
                    self.scroll = -1
                elif active and ry > 16000:
                    self.scroll = 1
            pressed = down - self.held if active else set()
            self.held = down
            return pressed
        except (AttributeError, OSError):
            self.held.clear()
            return set()

    def close(self):
        if self.dll and self.pad:
            self.dll.SDL_CloseGamepad(self.pad)
            self.pad = None


_inhibitor = None


def keep_awake(on, log=None):
    global _inhibitor
    if not on:
        held = _inhibitor is not None
        if held:
            try:
                if _inhibitor.poll() is None:
                    _inhibitor.stdin.close()
                    _inhibitor.wait(timeout=2)
            except subprocess.TimeoutExpired:
                _inhibitor.kill()
                _inhibitor.wait()
            except (AttributeError, OSError, ValueError):
                pass
        _inhibitor = None
        if held and log:
            log('keep awake: systemd-inhibit released')
        return
    if _inhibitor and _inhibitor.poll() is None:
        return
    if _inhibitor:
        _inhibitor = None
    inhibitor = shutil.which('systemd-inhibit')
    if not inhibitor:
        if log:
            log('keep awake: systemd-inhibit unavailable')
        return
    try:
        _inhibitor = subprocess.Popen(
            [inhibitor, '--what=idle:sleep', '--who=NX2Saria', '--why=Building the game',
             '--mode=block', 'cat'],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
        if _inhibitor.poll() is None:
            if log:
                log('keep awake: systemd-inhibit held')
        else:
            _inhibitor = None
            if log:
                log('keep awake: systemd-inhibit unavailable')
    except OSError:
        _inhibitor = None
        if log:
            log('keep awake: systemd-inhibit unavailable')


def toolchain_missing(which=shutil.which, run=subprocess.run):
    required = ('clang', 'clang++', 'ld.lld')
    missing = [name for name in required if not which(name)]
    clang = which('clang')
    if clang:
        for header in ('opus/opus.h', 'vpx/vpx_decoder.h', 'dav1d/dav1d.h', 'zstd.h'):
            probe = run([clang, '-E', '-x', 'c', '-', '-o', os.devnull], input=f'#include <{header}>\n',
                        capture_output=True, text=True)
            if probe.returncode:
                missing.append(header)
    if not missing:
        return ''
    return (f'Missing Linux build tools or headers: {", ".join(missing)}. Install the compiler and libraries, then '
            'run this again:\n'
            'Arch, CachyOS, Manjaro, SteamOS (developer mode): sudo pacman -S --needed clang lld sdl3 opus libvpx '
            'dav1d zstd vulkan-headers vulkan-icd-loader\n'
            'Fedora: sudo dnf install clang lld SDL3-devel opus-devel libvpx-devel libdav1d-devel libzstd-devel '
            'vulkan-headers vulkan-loader-devel libstdc++-static libatomic\n'
            'Debian, Ubuntu, Linux Mint: sudo apt install clang lld libopus-dev libvpx-dev libdav1d-dev libzstd-dev '
            'libvulkan-dev. SDL3 is carried by the build kit when the distribution has no SDL3 package.\n'
            'NX2Saria will not install system packages for you.')


def boot_identity():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


def process_identity(pid):
    stat = (Path('/proc') / str(pid) / 'stat').read_text(encoding='utf-8', errors='replace')
    fields = stat[stat.rindex(')') + 1:].split()
    return {'pid': int(pid), 'state': fields[0], 'group': int(fields[2]), 'session': int(fields[3]),
            'start': int(fields[19])}


def session_members(session, exclude=None):
    members = []
    for item in Path('/proc').iterdir():
        if not item.name.isdigit() or int(item.name) == exclude:
            continue
        try:
            identity = process_identity(int(item.name))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
        if identity['session'] == session and identity['state'] != 'Z':
            members.append(identity)
    return members

def signal_verified_member(member, sig):
    try:
        pid = member['pid']
        fd = os.pidfd_open(pid)
    except (OSError, AttributeError):
        return
    try:
        current = process_identity(pid)
        if (current['start'], current['session'], current['group']) == (member['start'], member['session'], member['group']):
            signal.pidfd_send_signal(fd, sig)
    except (OSError, AttributeError):
        pass
    finally:
        os.close(fd)
