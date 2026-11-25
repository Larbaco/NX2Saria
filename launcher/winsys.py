import ctypes
import os
import subprocess
import sys
from pathlib import Path

CREATE_NO_WINDOW = 0x08000000
DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200
UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\NX2Saria'
WINDOWS_LOW_COMMIT_ADVICE = (
    'Close memory-heavy programs and turn on or enlarge the Windows page file. '
    'For a PC with 16 GB of RAM, use a fixed 32 GB page file (32768 MB). '
    'Restart Windows after changing the page file.'
)


def dpi_aware():
    if os.name != 'nt':
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass


def app_id():
    if os.name == 'nt':
        try:
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('NX2Saria.Launcher')
        except (AttributeError, OSError):
            pass


GWL_STYLE, GWL_EXSTYLE = -16, -20
WS_MINIMIZEBOX, WS_SYSMENU = 0x00020000, 0x00080000
WS_EX_APPWINDOW, WS_EX_TOOLWINDOW = 0x00040000, 0x00000080


def _hwnd(root):
    user32 = ctypes.windll.user32
    user32.GetParent.restype = ctypes.c_void_p
    user32.GetParent.argtypes = [ctypes.c_void_p]
    return user32.GetParent(root.winfo_id()) or root.winfo_id()


def _post(hwnd, msg, wparam, lparam):
    user32 = ctypes.windll.user32
    user32.PostMessageW.restype = ctypes.c_int
    user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    return user32.PostMessageW(hwnd, msg, wparam, lparam)


def frameless(root):
    if os.name != 'nt':
        return False
    try:
        root.overrideredirect(True)
        root.update_idletasks()
        user32 = ctypes.windll.user32
        get, put = user32.GetWindowLongPtrW, user32.SetWindowLongPtrW
        get.restype, get.argtypes = ctypes.c_ssize_t, [ctypes.c_void_p, ctypes.c_int]
        put.restype, put.argtypes = ctypes.c_ssize_t, [ctypes.c_void_p, ctypes.c_int, ctypes.c_ssize_t]
        hwnd = _hwnd(root)
        put(hwnd, GWL_EXSTYLE, (get(hwnd, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW)
        put(hwnd, GWL_STYLE, get(hwnd, GWL_STYLE) | WS_MINIMIZEBOX | WS_SYSMENU)
        root.withdraw()
        root.after(10, root.deiconify)
        return True
    except (AttributeError, OSError, Exception):
        return False


def minimize(root):
    if os.name == 'nt':
        ctypes.windll.user32.ShowWindowAsync(ctypes.c_void_p(_hwnd(root)), 6)
    else:
        root.iconify()


def start_drag(root):
    if os.name != 'nt':
        return False
    try:
        ctypes.windll.user32.ReleaseCapture()
        return bool(_post(_hwnd(root), 0x00A1, 2, 0))
    except (AttributeError, OSError):
        return False


_ICONS = []


def set_window_icon(root, ico):
    if os.name != 'nt':
        return False
    try:
        user32 = ctypes.windll.user32
        user32.LoadImageW.restype = ctypes.c_void_p
        user32.LoadImageW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_uint]
        hwnd = _hwnd(root)
        for which, (mx, my) in ((1, (11, 12)), (0, (49, 50))):
            h = user32.LoadImageW(None, str(ico), 1, user32.GetSystemMetrics(mx), user32.GetSystemMetrics(my),
                                  0x10)
            if h:
                _ICONS.append(h)
                _post(hwnd, 0x0080, which, h)
        return True
    except (AttributeError, OSError):
        return False


def register_font(path):
    path = str(path)
    if os.name == 'nt':
        try:
            return ctypes.windll.gdi32.AddFontResourceExW(ctypes.c_wchar_p(path), 0x10, None) > 0
        except (AttributeError, OSError):
            return False
    try:
        fc = ctypes.CDLL('libfontconfig.so.1')
        fc.FcConfigAppFontAddFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        return bool(fc.FcConfigAppFontAddFile(None, path.encode()))
    except OSError:
        return False


def unregister_font(path):
    if os.name != 'nt':
        return
    try:
        ctypes.windll.gdi32.RemoveFontResourceExW(ctypes.c_wchar_p(str(path)), 0x10, None)
    except (AttributeError, OSError):
        pass


def open_path(path):
    path = str(path)
    if os.name == 'nt':
        os.startfile(path)   # noqa: S606 — opens a folder in Explorer
    else:
        subprocess.Popen(['xdg-open', path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def reveal(path):
    path = Path(path)
    if os.name == 'nt':
        subprocess.Popen(['explorer', f'/select,{path}'])   # noqa: S603, S607
    else:
        open_path(path.parent)


KNOWN_FOLDERS = {'downloads': '{374DE290-123F-4565-9164-39C4925E467B}',
                 'desktop': '{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}',
                 'documents': '{FDD39AD0-238F-46AF-ADB4-6C85480369C7}',
                 'pictures': '{33E28130-4E1E-4676-835A-98395C3BC3BB}'}


def known_folder(name):
    if sys.platform.startswith('linux'):
        from .platform.linux import user_dir
        return user_dir(name)
    if os.name != 'nt' or name not in KNOWN_FOLDERS:
        return None
    try:
        from ctypes import wintypes

        class GUID(ctypes.Structure):
            _fields_ = [('d1', wintypes.DWORD), ('d2', wintypes.WORD), ('d3', wintypes.WORD),
                        ('d4', ctypes.c_ubyte * 8)]
        ole32, shell32 = ctypes.WinDLL('ole32'), ctypes.WinDLL('shell32')
        ole32.CLSIDFromString.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(GUID))
        ole32.CoTaskMemFree.argtypes = (ctypes.c_void_p,)
        shell32.SHGetKnownFolderPath.argtypes = (ctypes.POINTER(GUID), wintypes.DWORD, wintypes.HANDLE,
                                                 ctypes.POINTER(ctypes.c_void_p))
        g, out = GUID(), ctypes.c_void_p()
        if ole32.CLSIDFromString(KNOWN_FOLDERS[name], ctypes.byref(g)) != 0:
            return None
        if shell32.SHGetKnownFolderPath(ctypes.byref(g), 0, None, ctypes.byref(out)) != 0:
            return None
        try:
            return Path(ctypes.wstring_at(out.value))
        finally:
            ole32.CoTaskMemFree(out)
    except (AttributeError, OSError):
        return None


def pick(root, kind, title, initialdir, filetypes=(), log=None, mustexist=False):
    from tkinter import filedialog
    tool, path = None, ''
    patterns = [(label, '*' if pattern in ('*', '*.*') else pattern) for label, pattern in filetypes]
    if sys.platform.startswith('linux'):
        from .platform.linux import pick_path
        tool, path = pick_path(kind, title, initialdir, patterns)
    if tool is None:
        tool = 'the system\'s' if os.name == 'nt' else 'Tk\'s (no zenity or kdialog)'
        if kind == 'folder':
            path = filedialog.askdirectory(parent=root, title=title, initialdir=initialdir, mustexist=mustexist)
        else:
            path = filedialog.askopenfilename(parent=root, title=title, initialdir=initialdir,
                                              filetypes=list(filetypes) or [('All files', '*.*')])
    path = str(path or '')
    if log:
        log(f'file chooser ({tool}): {title}: ' + (path if path else 'cancelled'))
    return path


class _PerformanceInformation(ctypes.Structure):
    _fields_ = [('cb', ctypes.c_uint32), ('CommitTotal', ctypes.c_size_t), ('CommitLimit', ctypes.c_size_t),
                ('CommitPeak', ctypes.c_size_t), ('PhysicalTotal', ctypes.c_size_t),
                ('PhysicalAvailable', ctypes.c_size_t), ('SystemCache', ctypes.c_size_t),
                ('KernelTotal', ctypes.c_size_t), ('KernelPaged', ctypes.c_size_t),
                ('KernelNonpaged', ctypes.c_size_t), ('PageSize', ctypes.c_size_t),
                ('HandleCount', ctypes.c_uint32), ('ProcessCount', ctypes.c_uint32),
                ('ThreadCount', ctypes.c_uint32)]


def system_commit_info():
    if os.name != 'nt':
        return None
    query = ctypes.WinDLL('psapi', use_last_error=True).GetPerformanceInfo
    query.argtypes = [ctypes.POINTER(_PerformanceInformation), ctypes.c_uint32]
    query.restype = ctypes.c_int
    info = _PerformanceInformation(cb=ctypes.sizeof(_PerformanceInformation))
    if not query(ctypes.byref(info), info.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    if not info.PageSize or not info.CommitLimit:
        raise OSError('GetPerformanceInfo returned an unknown commit limit')
    page_mib = info.PageSize / (1 << 20)
    return info.CommitTotal * page_mib, info.CommitLimit * page_mib, info.PhysicalTotal * page_mib


def page_file_is_system_managed():
    if os.name != 'nt':
        return None
    try:
        import winreg
        access = winreg.KEY_READ | getattr(winreg, 'KEY_WOW64_64KEY', 0)
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r'SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management',
                            0, access) as key:
            values, kind = winreg.QueryValueEx(key, 'PagingFiles')
    except (ImportError, OSError):
        return None
    if kind != winreg.REG_MULTI_SZ:
        return None
    return page_files_system_managed(values)


def page_files_system_managed(values):
    if not values:
        return False
    managed = True
    for entry in values:
        if not isinstance(entry, str):
            return None
        fields = entry.rsplit(None, 2)
        if not fields or not fields[0] or len(fields) == 2:
            return None
        path = fields[0]
        try:
            initial, maximum = (int(fields[1]), int(fields[2])) if len(fields) == 3 else (0, 0)
        except ValueError:
            return None
        own = path.startswith('?') or (initial == 0 and maximum == 0)
        if not own:
            managed = False
    return managed


def confirm_play(text, root=None):
    box = ctypes.WinDLL('user32', use_last_error=True).MessageBoxW
    box.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
    box.restype = ctypes.c_int
    flags = 0x4 | 0x30 | 0x10000
    result = box(_hwnd(root) if root is not None else None, text, 'NX2Saria', flags)
    if not result:
        raise ctypes.WinError(ctypes.get_last_error())
    return result != 7


def start_game(exe):
    exe = Path(exe)
    if os.name == 'nt':
        flags = {'creationflags': CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP}
    else:
        if sys.platform.startswith('linux'):
            from .platform.linux import child_subreaper
            try:
                child_subreaper(True)
            except (AttributeError, OSError):
                pass
        flags = {'start_new_session': True}
    return subprocess.Popen([str(exe)], cwd=str(exe.parent), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, close_fds=True, **flags)


SYNCHRONIZE = 0x00100000
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
WAIT_TIMEOUT = 0x102


_k32 = None


def _kernel32():
    global _k32
    if _k32:
        return _k32
    k = ctypes.WinDLL('kernel32')
    k.OpenProcess.restype = ctypes.c_void_p
    k.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
    k.CloseHandle.argtypes = [ctypes.c_void_p]
    k.WaitForSingleObject.restype = ctypes.c_ulong
    k.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    k.GetCurrentProcess.restype = ctypes.c_void_p
    filetime = ctypes.POINTER(ctypes.c_ulonglong)
    k.GetProcessTimes.argtypes = [ctypes.c_void_p, filetime, filetime, filetime, filetime]
    k.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    k.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_wchar_p,
                                             ctypes.POINTER(ctypes.c_ulong)]
    k.K32EnumProcesses.argtypes = [ctypes.POINTER(ctypes.c_ulong), ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]
    _k32 = k
    return k


class LauncherParent:
    def __init__(self, handle):
        self.handle = handle

    @classmethod
    def open(cls, pid):
        if pid <= 0 or pid != os.getppid():
            return None
        k = _kernel32()
        handle = k.OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            return None
        parent_started, self_started = ctypes.c_ulonglong(), ctypes.c_ulonglong()
        ignored = ctypes.c_ulonglong()
        if (k.GetProcessTimes(handle, ctypes.byref(parent_started), ctypes.byref(ignored), ctypes.byref(ignored),
                              ctypes.byref(ignored))
                and k.GetProcessTimes(k.GetCurrentProcess(), ctypes.byref(self_started), ctypes.byref(ignored),
                                      ctypes.byref(ignored), ctypes.byref(ignored))
                and parent_started.value < self_started.value
                and k.WaitForSingleObject(handle, 0) == WAIT_TIMEOUT):
            return cls(handle)
        k.CloseHandle(handle)
        return None

    def exited(self):
        return _kernel32().WaitForSingleObject(self.handle, 0) != WAIT_TIMEOUT

    def close(self):
        _kernel32().CloseHandle(self.handle)


class GameCopy:
    def __init__(self, handle, pid):
        self.handle, self.pid, self.returncode, self.ended = handle, pid, None, False

    def poll(self):
        if self.handle:
            k = _kernel32()
            if k.WaitForSingleObject(self.handle, 0) == WAIT_TIMEOUT:
                return None
            code = ctypes.c_ulong()
            if k.GetExitCodeProcess(self.handle, ctypes.byref(code)):
                self.returncode = code.value
            k.CloseHandle(self.handle)
            self.handle, self.ended = None, True
        return self.returncode


def game_finished(exe, skip=None):
    if not sys.platform.startswith('linux'):
        return []
    from .platform import linux
    try:
        linux.child_subreaper(False)
    except (AttributeError, OSError):
        pass
    return linux.ended_copies(exe, skip=skip)


def game_copy(exe, skip=None):
    if os.name != 'nt':
        if sys.platform.startswith('linux'):
            from .platform.linux import game_copy as linux_game_copy
            return linux_game_copy(exe, skip=skip)
        return None
    try:
        k = _kernel32()
        n = 1024
        while True:
            pids, got = (ctypes.c_ulong * n)(), ctypes.c_ulong()
            if not k.K32EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got)):
                return None
            if got.value < ctypes.sizeof(pids):
                break
            n *= 4
        want = os.path.normcase(os.path.abspath(str(exe)))
        name = ctypes.create_unicode_buffer(32768)
        for pid in pids[:got.value // ctypes.sizeof(ctypes.c_ulong)]:
            if not pid or pid == skip:
                continue
            h = k.OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            if not h:
                continue
            size = ctypes.c_ulong(len(name))
            image = name.value if k.QueryFullProcessImageNameW(h, 0, name, ctypes.byref(size)) else ''
            if (image and os.path.basename(os.path.normcase(image)) == os.path.basename(want)
                    and _same_file(image, want) and k.WaitForSingleObject(h, 0) == WAIT_TIMEOUT):
                return GameCopy(h, pid)
            k.CloseHandle(h)
    except (AttributeError, OSError):
        pass
    return None


def _same_file(a, b):
    if os.path.normcase(a) == b:
        return True
    try:
        return os.path.samefile(a, b)
    except OSError:
        return False


def start_detached(cmd, cwd=None):
    flags = {'creationflags': DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' \
        else {'start_new_session': True}
    subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     close_fds=True, **flags)


def restart(layout, args=()):
    if os.name != 'nt':
        if layout.dev:
            return False
        exe = layout.root / 'nx2saria-launcher'
        if not exe.is_file():
            return False
        os.chdir(layout.root)
        os.execv(str(exe), [str(exe), *args])
    if not layout.stub.is_file():
        return False
    start_detached([str(layout.stub), *args], cwd=str(layout.root))
    return True


def make_game_toolchain_args(layout):
    if os.name != 'nt':
        from .platform.linux import make_game_toolchain_args as linux_args
        return linux_args(layout)
    return ['--msys', str(layout.msys)]


def stub_run(layout, args, timeout=30):
    if not layout.stub.is_file():
        return False
    flags = {'creationflags': CREATE_NO_WINDOW} if os.name == 'nt' else {}
    try:
        return subprocess.run([str(layout.stub), *args], timeout=timeout, capture_output=True, **flags).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def desktop_shortcut(layout, enable):
    return stub_run(layout, ['--desktop-shortcut', 'on' if enable else 'off'])


def keep_awake(on, log=None):
    if os.name != 'nt':
        from .platform.linux import keep_awake as linux_keep_awake
        return linux_keep_awake(on, log)
    try:
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | (ES_SYSTEM_REQUIRED if on else 0))
    except (AttributeError, OSError):
        pass


def background_thread():
    if os.name != 'nt':
        return False
    try:
        from ctypes import wintypes
        k = ctypes.WinDLL('kernel32')
        k.GetCurrentThread.restype = wintypes.HANDLE
        k.SetThreadPriority.argtypes = (wintypes.HANDLE, ctypes.c_int)
        return bool(k.SetThreadPriority(k.GetCurrentThread(), 0x00010000))
    except (AttributeError, OSError):
        return False


OWNED = ('NX2Saria.exe', 'NX2navi.exe', 'app', 'python', 'python.next', 'pydeps', 'data', 'toolchain')


def uninstall_plan(layout, remove_game=''):
    root = Path(layout.root)
    gone = [Path(remove_game)] if remove_game else []
    gone += [root / n for n in OWNED]
    try:
        gone += sorted(p for p in root.glob('python.old-*'))
    except OSError:
        pass
    empty = [root]
    if Path(layout.toolchain).parent != root:
        gone.append(Path(layout.toolchain))
        empty.append(Path(layout.toolchain).parent)
    return gone, empty


def uninstall_script(gone, empty):
    steps = ['ping -n 3 127.0.0.1 >nul']
    for p in gone:
        if '%' not in str(p):
            steps.append(f'(if exist "{p}\\*" (rmdir /s /q "{p}") else if exist "{p}" (del /f /q "{p}"))')
    steps += [f'rmdir "{p}" 2>nul' for p in empty if '%' not in str(p)]
    return ' & '.join(steps)


def uninstall(layout, remove_game='', log=print, keep=()):
    stub_run(layout, ['--shortcuts', 'off'])
    gone, empty = uninstall_plan(layout, remove_game)
    if os.name == 'nt':
        try:
            import winreg
            for key in (UNINSTALL_KEY, r'Software\Microsoft\Windows\CurrentVersion\Uninstall\NX2navi'):
                try:
                    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as entry:
                        installed, _ = winreg.QueryValueEx(entry, 'InstallLocation')
                    if os.path.normcase(os.path.abspath(installed)) == os.path.normcase(os.path.abspath(layout.root)):
                        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, key)
                except OSError:
                    pass
        except OSError:
            pass
        kept = [str(p) for p in list(gone) + list(empty) if '%' in str(p) and os.path.exists(p)]
        if kept:
            log('uninstall: remove these yourself (their names hold %): ' + ', '.join(kept))
        start_detached(f'cmd.exe /c "{uninstall_script(gone, empty)}"', cwd=os.environ.get('TEMP', 'C:\\'))
    elif getattr(layout, 'dev', False):
        log('uninstall: a checkout is not uninstalled; nothing was removed')
    else:
        from .platform import linux
        if not linux.appimage_install(layout.root):
            log(f'uninstall: {layout.root} holds no AppImage install; nothing was removed')
            return
        gone, empty, kept = linux.uninstall_plan(layout.root, remove_game, keep)
        log('uninstall: removing once the window has closed: ' +
            (', '.join(str(p) for p in gone if p.exists() or p.is_symlink()) or 'nothing'))
        if kept:
            log('uninstall: kept: ' + '; '.join(f'{p} ({why})' for p, why in kept))
        start_detached(linux.uninstall_command(os.getpid(), gone, empty, Path(layout.logs) / 'launcher.log'),
                       cwd='/')


class _XInputGamepad(ctypes.Structure):
    _fields_ = [('wButtons', ctypes.c_ushort), ('bLeftTrigger', ctypes.c_ubyte), ('bRightTrigger', ctypes.c_ubyte),
                ('sThumbLX', ctypes.c_short), ('sThumbLY', ctypes.c_short), ('sThumbRX', ctypes.c_short),
                ('sThumbRY', ctypes.c_short)]


class _XInputState(ctypes.Structure):
    _fields_ = [('dwPacketNumber', ctypes.c_uint), ('Gamepad', _XInputGamepad)]


BUTTONS = {0x0001: 'up', 0x0002: 'down', 0x0004: 'left', 0x0008: 'right', 0x1000: 'a', 0x2000: 'b', 0x8000: 'y',
           0x4000: 'x', 0x0010: 'start'}


def in_front(root):
    if os.name != 'nt':
        if os.environ.get('GAMESCOPE_WAYLAND_DISPLAY') or os.environ.get('SteamDeck'):
            return True
        try:
            return root.focus_displayof() is not None
        except Exception:
            return True
    try:
        user32 = ctypes.windll.user32
        user32.GetForegroundWindow.restype = ctypes.c_void_p
        user32.GetWindowThreadProcessId.restype = ctypes.c_uint
        user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
        hwnd = user32.GetForegroundWindow()
        if not hwnd:
            return False
        pid = ctypes.c_uint(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        return pid.value == os.getpid()
    except (AttributeError, OSError):
        return True


class _WindowsGamepad:
    def __init__(self):
        self.dll = None
        self.held = set()
        self.scroll = 0
        if os.name == 'nt':
            for name in ('xinput1_4', 'xinput1_3', 'xinput9_1_0'):
                try:
                    self.dll = getattr(ctypes.windll, name)
                    break
                except (AttributeError, OSError):
                    continue
        self.description = 'XInput' if self.dll else 'none (XInput unavailable)'

    def poll(self, active=True):
        self.scroll = 0
        if not self.dll:
            return set()
        st = _XInputState()
        down = set()
        for pad in range(4):
            try:
                if self.dll.XInputGetState(pad, ctypes.byref(st)) == 0:
                    g = st.Gamepad
                    if active and g.sThumbRY > 16000:
                        self.scroll = -1
                    elif active and g.sThumbRY < -16000:
                        self.scroll = 1
                    down = {name for bit, name in BUTTONS.items() if g.wButtons & bit}
                    if g.sThumbLY > 16000:
                        down.add('up')
                    elif g.sThumbLY < -16000:
                        down.add('down')
                    if g.sThumbLX > 16000:
                        down.add('right')
                    elif g.sThumbLX < -16000:
                        down.add('left')
                    break
            except OSError:
                return set()
        pressed = down - self.held if active else set()
        self.held = down
        return pressed


class Gamepad:
    def __init__(self):
        if os.name != 'nt':
            from .platform.linux import Gamepad as LinuxGamepad
            self.backend = LinuxGamepad()
        else:
            self.backend = _WindowsGamepad()

    @property
    def scroll(self):
        return self.backend.scroll

    @property
    def description(self):
        return self.backend.description

    def poll(self, active=True):
        return self.backend.poll(active)


def is_windows():
    return os.name == 'nt'


def frozen_python():
    return Path(sys.executable).parent
