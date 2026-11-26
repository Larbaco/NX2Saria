import json
import os
import queue
import shutil
import signal
import stat
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

from . import winsys
from .platform import game_executable

RUN = ('import os, runpy, sys; p = sys.argv[1]; s = sys.argv[2]; sys.argv = sys.argv[2:]; '
       'sys.path[:0] = [x for x in p.split(os.pathsep) if x] + [os.path.dirname(os.path.abspath(s))]; '
       'runpy.run_path(s, run_name="__main__")')
CREATE_NO_WINDOW = 0x08000000
CREATE_NEW_PROCESS_GROUP = 0x00000200
BELOW_NORMAL_PRIORITY_CLASS = 0x00004000


def command(layout, args):
    return [layout.python_for_builds(), '-I', '-c', RUN, str(layout.pydeps), str(layout.make_game), *map(str, args),
            '--events']


def has_prompt_art(layout):
    return (layout.app / 'prompt-art').is_dir()


def install_args(layout, dump, game_dir):
    dump = str(dump)
    if os.path.isdir(dump):
        src = ['--exefs', os.path.join(dump, 'exefs'), '--romfs', os.path.join(dump, 'romfs')]
    else:
        src = ['--nup', dump]
    return [*src, '--out', str(game_dir), *winsys.make_game_toolchain_args(layout)]


def rebuild_args(layout, game_dir, retune=False):
    return ['--rebuild', *(['--retune'] if retune else []), '--out', str(game_dir),
            *winsys.make_game_toolchain_args(layout)]


def check_args(dump):
    dump = str(dump)
    if os.path.isdir(dump):
        return ['--exefs', os.path.join(dump, 'exefs'), '--romfs', os.path.join(dump, 'romfs'), '--check']
    return ['--nup', dump, '--check']


class BuildError(Exception):
    pass


class Run:

    def __init__(self, cmd, on_event, on_line, cwd=None):
        self.error = None
        kw = {}
        if os.name == 'nt':
            kw['creationflags'] = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS
        else:
            kw['start_new_session'] = True
        env = dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     cwd=cwd, env=env, text=True, encoding='utf-8', errors='replace', bufsize=1, **kw)
        self.cancelled = False
        self.dropped = 0
        self._reader = threading.Thread(target=self._read, args=(on_event, on_line), daemon=True)
        self._reader.start()

    def _read(self, on_event, on_line):
        self._read_lines(self.proc.stdout, on_event, on_line)

    def _read_lines(self, lines, on_event, on_line):
        def call(handler, arg):
            try:
                handler(arg)
            except Exception:   # noqa: BLE001 - any failure of a handler must leave the pipe read
                self.dropped += 1

        for line in lines:
            line = line.rstrip('\r\n')
            if line.startswith('@@'):
                try:
                    ev = json.loads(line[2:])
                except ValueError:
                    call(on_line, line)
                    continue
                if isinstance(ev, dict):
                    if ev.get('ev') == 'error':
                        self.error = ev.get('message')
                    call(on_event, ev)
                else:
                    call(on_line, line)
            elif line.startswith('error: ') and not self.error:
                self.error = line[7:]
                call(on_line, line)
            else:
                call(on_line, line)

    def wait(self):
        code = self.proc.wait()
        self._reader.join(5)
        return code

    def cancel(self):
        self.cancelled = True
        if self.proc.poll() is not None:
            return
        if os.name == 'nt':
            subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.proc.pid)], capture_output=True,
                           creationflags=CREATE_NO_WINDOW)
        else:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                pass


SUPERVISOR = ('import runpy,sys; sys.path.insert(0,sys.argv.pop(1)); '
              'runpy.run_module("launcher.build_supervisor",run_name="__main__")')


class SupervisedRun(Run):
    def __init__(self, layout, marker, cmd, on_event, log, cwd=None):
        self.error = None
        self.dropped = 0
        self.supervisor_exited = False
        self.needs_recovery = False
        self.run_dir = layout.data / 'build-supervisor' / uuid.uuid4().hex
        self.run_dir.mkdir(parents=True)
        command = [sys.executable, '-I', '-c', SUPERVISOR, str(Path(__file__).resolve().parents[1]),
                   '--install-root', str(layout.root), '--run-dir', str(self.run_dir), '--', *cmd]
        flags = ({'creationflags': CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS}
                 if os.name == 'nt' else {'start_new_session': True})
        env = dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')
        try:
            self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                         cwd=cwd, env=env, text=True, encoding='utf-8', **flags)
        except OSError:
            shutil.rmtree(self.run_dir, ignore_errors=True)
            raise
        frames = queue.Queue()
        reader = threading.Thread(target=lambda: frames.put(self.proc.stdout.readline(4096)), daemon=True)
        reader.start()
        try:
            identity = json.loads(frames.get(timeout=10))
            if identity['pid'] != self.proc.pid or identity['kind'] not in ('linux_group', 'windows_job', 'windows_pid'):
                raise ValueError('invalid supervisor identity')
            data = json.loads(marker.read_text(encoding='utf-8'))
            data['build'] = {'version': 1, 'token': self.run_dir.name, **identity}
            tmp = marker.with_name(marker.name + '.tmp')
            tmp.write_text(json.dumps(data), encoding='utf-8')
            os.replace(tmp, marker)
        except (OSError, KeyError, ValueError, TypeError, queue.Empty) as e:
            self.proc.stdin.close()
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait()
            reader.join(1)
            self.proc.stdout.close()
            shutil.rmtree(self.run_dir, ignore_errors=True)
            raise BuildError('could not register the build process') from e
        log(f'worker: build supervisor ready mode={identity["kind"]}')
        try:
            self.proc.stdin.write('start\n')
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.stdout.close()
        self.reader = threading.Thread(target=self._read_lines, args=(self.output_lines(), on_event, log), daemon=True)
        self.reader.start()

    def output_lines(self):
        path = self.run_dir / 'output.log'
        while not path.is_file():
            if self.proc.poll() is not None:
                return
            time.sleep(0.1)
        fragment = b''
        with path.open('rb') as output:
            while True:
                part = output.readline()
                if part.endswith(b'\n'):
                    yield (fragment + part).decode('utf-8', 'replace')
                    fragment = b''
                else:
                    fragment += part
                    if self.supervisor_exited:
                        if fragment:
                            yield fragment.decode('utf-8', 'replace')
                        return
                    time.sleep(0.1)

    def cancel(self):
        try:
            (self.run_dir / 'cancel').touch(exist_ok=True)
        except OSError:
            pass

    def wait(self):
        self.proc.wait()
        self.supervisor_exited = True
        self.reader.join(5)
        try:
            code = json.loads((self.run_dir / 'result.json').read_text(encoding='utf-8'))['code']
        except (OSError, ValueError, KeyError, TypeError):
            self.needs_recovery = True
            raise BuildError('the build outcome could not be confirmed')
        return code if type(code) is int else -1

    def clean(self):
        shutil.rmtree(self.run_dir, ignore_errors=True)


ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION = 5, 32


def _write_open_error(path):
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateFileW.restype = wintypes.HANDLE
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                              wintypes.DWORD, wintypes.HANDLE]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    h = k.CreateFileW(str(path), 0x40000000, 7, None, 3, 0x80, None)
    if h in (None, wintypes.HANDLE(-1).value):
        return ctypes.get_last_error() or -1
    k.CloseHandle(h)
    return 0


def game_running(game_dir, exe=None, log=None):
    if exe is None:
        path = game_executable(game_dir)
    else:
        path = Path(exe)
        if not path.is_absolute():
            path = Path(game_dir) / path
    if os.name != 'nt':
        from .platform.linux import game_running as linux_game_running
        return linux_game_running(game_dir, exe=path)
    path = os.fspath(path)
    if not os.path.isfile(path):
        return False
    for attempt in range(2):
        err = _write_open_error(path)
        if err == ERROR_SHARING_VIOLATION:
            return True
        if err != ERROR_ACCESS_DENIED:
            return False
        if attempt or not os.stat(path).st_file_attributes & stat.FILE_ATTRIBUTE_READONLY:
            break
        try:
            os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
        except OSError:
            break
    if log:
        log(f'{path} cannot be opened for writing (access denied), though the game does not run it: a build cannot '
            'replace it until it can')
    return False


def build_state(game_dir):
    try:
        with open(os.path.join(str(game_dir), 'build-cache', 'state.json'), encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def built_version(game_dir):
    st = build_state(game_dir)
    if 'compile' in st.get('steps', {}) and game_executable(game_dir).is_file():
        return st.get('kit', '')
    return ''


def scan_report(game_dir):
    try:
        with open(os.path.join(str(game_dir), 'romfs-check.json'), encoding='utf-8') as f:
            report = json.load(f)
        return sorted(report['damaged']), len(report.get('repaired') or ())
    except (OSError, ValueError, KeyError, TypeError):
        return None


def can_rebuild(game_dir):
    st = build_state(game_dir)
    return st.get('version') == 2 and all(s in st.get('steps', {}) for s in ('exefs', 'romfs'))


def unpacked(game_dir):
    gd = str(game_dir)
    return all(os.path.isfile(os.path.join(gd, *parts)) for parts in
               (('exefs', 'main'), ('romfs', 'romfs.bin'), ('romfs', 'index.json')))


def installed(game_dir):
    if not game_dir:
        return False
    if built_version(game_dir):
        return True
    return game_executable(game_dir).is_file() and can_rebuild(game_dir)


FATAL_TAIL = 256 << 10


def game_log_paths(game_dir):
    logs = Path(game_dir) / 'logs'
    return logs / 'nx2saria.log', logs / 'nx2navi.log'


def game_log_path(game_dir):
    current, legacy = game_log_paths(game_dir)
    return current if current.is_file() or not legacy.is_file() else legacy


def last_fatal(game_dir):
    if not game_dir:
        return ''
    try:
        with open(game_log_path(game_dir), 'rb') as f:
            f.seek(max(0, f.seek(0, 2) - FATAL_TAIL))
            lines = f.read().decode('utf-8', 'replace').splitlines()
    except OSError:
        return ''
    for line in reversed(lines):
        if line.startswith('[launcher] the game ended'):
            return ''
        if line.startswith('FATAL: '):
            return line[len('FATAL: '):].strip()
    return ''
