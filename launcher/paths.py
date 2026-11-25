import os
import sys
from pathlib import Path

APP = Path(__file__).resolve().parents[1]


def _is_release_app(app):
    return app.parent.name == 'app' and (app.parent / 'current.txt').is_file()


def ansi_safe(path):
    if os.name != 'nt':
        return True
    try:
        str(path).encode('mbcs', errors='strict')
        return True
    except (UnicodeEncodeError, LookupError):
        return False


WINDOWS_PATH_MAX = 259
LONGEST_IN_GAME = 128


def long_path_problem(game_dir):
    if os.name != 'nt' or not game_dir:
        return ''
    n, most = len(os.path.abspath(str(game_dir))), WINDOWS_PATH_MAX - 1 - LONGEST_IN_GAME
    if n <= most:
        return ''
    return (f'the path of the game folder is {n} characters long, and the game\'s files need {LONGEST_IN_GAME} more: '
            f'Windows and the build tools stop at {WINDOWS_PATH_MAX + 1} for a file\'s whole path. Choose a folder '
            f'whose path has at most {most} characters, such as D:\\Games\\NX2Saria')


def fixed_drive(drive):
    if os.name != 'nt':
        return True
    try:
        import ctypes
        return ctypes.windll.kernel32.GetDriveTypeW(str(drive)) == 3
    except (AttributeError, OSError):
        return True


def install_folder(path):
    folder = Path(path)
    if (folder.exists() and any(folder.iterdir()) and not (folder / 'build-cache').is_dir()
            and folder.name.lower() not in ('nx2navi', 'nx2saria')):
        return folder / 'NX2Saria'
    return folder


class Layout:
    def __init__(self, root=None, app=None):
        self.app = Path(app or APP)
        self.dev = not _is_release_app(self.app)
        env = os.environ.get('NX2SARIA_LAUNCHER_ROOT', os.environ.get('NX2NAVI_LAUNCHER_ROOT'))
        if root:
            self.root = Path(root)
        elif env:
            self.root = Path(env)
        elif not self.dev:
            self.root = self.app.parents[1]
        else:
            self.root = self.app / 'build' / 'launcher-dev'
        self.apps = self.root / 'app'
        self.python = self.root / 'python'
        self.stub = self.root / 'NX2Saria.exe'
        legacy_stub = self.root / 'NX2navi.exe'
        if not self.stub.is_file() and legacy_stub.is_file():
            self.stub = legacy_stub
        self.pydeps = self.root / 'pydeps'
        self.data = self.root / 'data'
        self.logs = self.data / 'logs'
        self.cache = self.data / 'cache'
        self.downloads = self.data / 'downloads'
        self.settings = self.data / 'launcher.json'
        self.instance_lock = self.data / 'launcher.lock'
        tool_root = self.root
        if not ansi_safe(tool_root):
            tool_root = Path(os.environ.get('SystemDrive', 'C:') + '\\') / 'NX2Saria'
        self.toolchain = tool_root / 'toolchain'
        self.msys_root = self.toolchain / 'msys64'
        self.msys = self.msys_root / 'ucrt64'
        self.pkgs = self.toolchain / 'pkgs'
        self.tools = self.app / 'tools'
        self.make_game = self.tools / 'make_game.py'
        self.lock_file = self.app / 'toolchain.lock.json'
        self.feed_file = self.app / 'feed.json'

    @property
    def version(self):
        v = self.app / 'VERSION'
        return v.read_text(encoding='utf-8').strip() if v.is_file() else '0.0.0'

    def default_game_dir(self):
        here = self.root / 'game'
        try:
            import shutil
            if shutil.disk_usage(self.root.anchor or self.root).free >= 40e9 and ansi_safe(here):
                return here
        except OSError:
            return here
        best = None
        if os.name == 'nt':
            import shutil
            import string
            for letter in string.ascii_uppercase[2:]:
                drive = Path(f'{letter}:\\')
                if not fixed_drive(drive):
                    continue
                try:
                    free = shutil.disk_usage(drive).free
                except OSError:
                    continue
                if best is None or free > best[0]:
                    best = (free, drive / 'Games' / 'NX2Saria')
        return best[1] if best else here

    def python_for_builds(self):
        exe = Path(sys.executable)
        if os.name != 'nt' and not self.dev:
            bundled = self.python / 'bin' / 'python3'
            if bundled.is_file():
                return str(bundled)
        if exe.name.lower() == 'pythonw.exe' and (exe.parent / 'python.exe').is_file():
            return str(exe.parent / 'python.exe')
        return str(exe)

    def ensure(self):
        for d in (self.data, self.logs, self.cache, self.downloads):
            d.mkdir(parents=True, exist_ok=True)
        return self
