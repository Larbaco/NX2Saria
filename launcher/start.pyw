import os
import sys
import traceback

APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STARTUP_FAILED = 86
STUB_WAITS = 5.0


def launcher_root():
    return os.environ.get('NX2SARIA_LAUNCHER_ROOT') or os.environ.get('NX2NAVI_LAUNCHER_ROOT') or os.path.dirname(os.path.dirname(APP))


def rollback_target(root):
    try:
        with open(os.path.join(root, 'app', 'previous.txt'), encoding='utf-8-sig') as f:
            prev = (f.read().splitlines() or [''])[0].strip()
    except (OSError, UnicodeDecodeError):
        return ''
    if not prev or prev == os.path.basename(APP):
        return ''
    return prev if os.path.isfile(os.path.join(root, 'app', prev, 'launcher', 'start.pyw')) else ''


def process_age():
    try:
        import ctypes
        from ctypes import wintypes
        k = ctypes.windll.kernel32
        k.GetCurrentProcess.restype = wintypes.HANDLE
        k.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        made, ended, kernel, user, now = (wintypes.FILETIME() for _ in range(5))
        if not k.GetProcessTimes(k.GetCurrentProcess(), *(ctypes.byref(t) for t in (made, ended, kernel, user))):
            return float('inf')
        k.GetSystemTimeAsFileTime(ctypes.byref(now))
        ticks = [t.dwHighDateTime << 32 | t.dwLowDateTime for t in (made, now)]
        return max(0.0, (ticks[1] - ticks[0]) / 1e7)
    except Exception:
        return float('inf')


def quiet_exit(root, argv, age):
    return bool(rollback_target(root)) and '--play' not in argv and age < STUB_WAITS


def short_paths(text):
    try:
        sys.path.insert(0, os.path.join(APP, 'tools'))
        import private_paths
        return private_paths.shorten(text)
    except Exception:
        return text


def crash(text, box=True):
    text = short_paths(text)
    try:
        logs = os.path.join(launcher_root(), 'data', 'logs')
        os.makedirs(logs, exist_ok=True)
        with open(os.path.join(logs, 'crash.log'), 'a', encoding='utf-8') as f:
            f.write(text + '\n')
    except OSError:
        pass
    if not box:
        return
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, 'NX2Saria could not start.\n\n' + text[-1500:], 'NX2Saria', 0x10)
    except Exception:
        print(text, file=sys.stderr)


def run():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    sys.path[:] = [p for p in sys.path if os.path.abspath(p or os.curdir) != script_dir]
    sys.path.insert(0, APP)
    if os.name != 'nt':
        try:
            from launcher.platform.linux import refresh_installed_wrapper
            refresh_installed_wrapper(launcher_root(), APP)
        except Exception:
            pass
    try:
        from launcher.ui.app import main
    except Exception:
        crash(traceback.format_exc(), box=not quiet_exit(launcher_root(), sys.argv[1:], process_age()))
        sys.exit(STARTUP_FAILED)
    try:
        code = main()
    except SystemExit:
        raise
    except Exception:
        crash(traceback.format_exc())
        sys.exit(1)
    sys.exit(code or 0)


if __name__ == '__main__':
    run()
