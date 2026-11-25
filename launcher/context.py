"""What every part of the launcher shares: the folder layout, the settings and the log (data/logs/launcher.log,
the run before it in launcher.1.log)."""
import os
import sys
import threading
import time
from pathlib import Path

from .paths import Layout
from .settings import Settings

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from private_paths import describe, on as short_paths_on, shorten, shorten_always  # noqa: E402

LOG_LIMIT = 4 << 20


def install_kind(layout, env=None):
    """How this launcher was installed, for the start line: a checkout of the repository; on Windows the setup's
    install (NX2Saria.exe beside app/) or an unpacked release; on Linux the AppImage's install (the nx2saria-launcher
    script beside app/, which the AppImage writes and the menu entry starts), and whether the AppImage started this
    run (it passes APPIMAGE on) or the menu entry did."""
    env = os.environ if env is None else env
    if layout.dev:
        return 'checkout'
    if os.name == 'nt':
        return 'installed by the setup' if layout.stub.is_file() else 'release folder'
    if (layout.root / 'nx2saria-launcher').is_file():
        return 'AppImage install, ' + ('started by the AppImage' if env.get('APPIMAGE') else
                                       'started from its menu entry or script')
    return 'release folder'


def system_text(env=None, os_release='/etc/os-release'):
    """The system for the start line: the Windows version, or the Linux distribution and the graphical session."""
    if os.name == 'nt':
        try:
            v = sys.getwindowsversion()
            return f'Windows {v.major}.{v.minor} build {v.build}'
        except AttributeError:
            return 'Windows'
    if sys.platform.startswith('linux'):
        from .platform.linux import os_name, session_text
        return f'{os_name(os_release)}, {session_text(env)}'
    return sys.platform


class Context:
    def __init__(self, layout=None, echo=False):
        self.layout = (layout or Layout()).ensure()
        self.settings = Settings(self.layout.settings)
        self.version = self.layout.version
        self.echo = echo
        self._lock = threading.Lock()
        path = self.layout.logs / 'launcher.log'
        try:
            if path.is_file() and path.stat().st_size > LOG_LIMIT:
                os.replace(path, path.with_name('launcher.1.log'))
        except OSError:
            pass
        try:
            self._file = open(path, 'a', encoding='utf-8', errors='replace')
        except OSError:
            self._file = None
        self.settings.log = self.log   # each changed setting gets a line when it is saved (Settings.save)
        self.log(f'--- NX2Saria launcher {self.version} ({install_kind(self.layout)}) in {self.layout.root}; '
                 f'platform {sys.platform}; {system_text()}; {describe()}')
        if sys.platform.startswith('linux'):
            from .platform.linux import udmabuf_log_line, udmabuf_status
            self.udmabuf = udmabuf_status()
            self.log(udmabuf_log_line(self.udmabuf))
        else:
            self.udmabuf = None
        from .toolchain import zstd_missing
        self.zstd_missing = zstd_missing(self.layout)
        if self.zstd_missing:
            self.log(self.zstd_missing)
        if self.settings.unreadable:
            self.log(f'{self.layout.settings} is not valid JSON: the settings start from their defaults; the file as '
                     'it was is kept as launcher.json.bad')

    def log(self, line):
        """One line (or several) into launcher.log. A log that cannot be written (a full disk, a file closed under
        it) loses the line and nothing else: the callers log from the build's reader thread and from the window's
        timers, where an exception would stop the build's output from being read or the timer from running. The
        account's folders are written short (tools/private_paths.py): players attach this log to public issues."""
        line = shorten(str(line))
        stamp = time.strftime('%H:%M:%S')
        with self._lock:
            if self._file:
                try:
                    for part in str(line).splitlines() or ['']:
                        self._file.write(f'{stamp} {part}\n')
                    self._file.flush()
                except (OSError, ValueError):   # ValueError: the file was closed
                    pass
        if self.echo:
            print(line, flush=True)

    def log_tail(self, n=80):
        try:
            with open(self.layout.logs / 'launcher.log', encoding='utf-8', errors='replace') as f:
                return f.readlines()[-n:]
        except OSError:
            return []
