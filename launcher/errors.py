"""Errors after the window is up, written where a player's report finds them.

launcher/start.pyw writes an error that keeps the window from opening into data/logs/crash.log. Once the window is
up, Python reports an exception in a window callback (a click, a key, a timer) or in a background thread only on
stderr. stderr is nowhere under pythonw.exe on Windows, and on Linux it is the terminal or the session's error file.
The window then goes on as if nothing happened. A fault in native code (Tk, SDL3 through ctypes) ends the process
with nothing written at all. This module sends the first two to launcher.log (one line) and crash.log (the
traceback), the third to crash.log, and on Linux it logs the signals that end the window.

Native faults are caught off Windows only. On Windows faulthandler also reports exceptions that are raised and then
handled, such as the COM and RPC exceptions of the shell's file chooser and its extensions, and crash.log would then
read like a crash every time the player chose a file.
"""
import faulthandler
import os
import signal
import sys
import threading
import time
import traceback

# The signals that end the window when nobody closed it: SIGTERM (kill, a logout, a shutdown) and SIGHUP (the
# terminal that started it was closed).
END_SIGNALS = ('SIGTERM', 'SIGHUP')

_fault_file = None   # kept open for the process's life: faulthandler writes to it from the fault itself


def crash_log(ctx):
    return ctx.layout.logs / 'crash.log'


def _write_crash(ctx, text):
    try:
        ctx.layout.logs.mkdir(parents=True, exist_ok=True)
        with open(crash_log(ctx), 'a', encoding='utf-8', errors='replace') as f:
            f.write(text if text.endswith('\n') else text + '\n')
    except OSError:
        pass


def summary(exc_type, exc, tb):
    """'<type>: <message> (in <function>, <file>:<line>)' for the innermost frame of the traceback."""
    where = ''
    frames = traceback.extract_tb(tb) if tb is not None else []
    if frames:
        f = frames[-1]
        where = f' (in {f.name}, {f.filename.replace(chr(92), "/").rsplit("/", 1)[-1]}:{f.lineno})'
    message = str(exc).splitlines()[0] if str(exc) else ''
    return f'{getattr(exc_type, "__name__", exc_type)}{": " + message if message else ""}{where}'[:300]


def record(ctx, where, exc_type, exc, tb):
    """One line into launcher.log and the whole traceback into crash.log, which both shorten the account's folders
    (tools/private_paths.py): players attach them to public issues."""
    ctx.log(f'error {where}: {summary(exc_type, exc, tb)}; the traceback is in data/logs/crash.log')
    from .context import shorten
    text = ''.join(traceback.format_exception(exc_type, exc, tb))
    _write_crash(ctx, shorten(f'{time.strftime("%Y-%m-%d %H:%M:%S")} launcher {ctx.version}, {where}:\n{text}'))


def install(ctx):
    """Exceptions in background threads to record(), and, off Windows, fatal faults (a crash in native code) to
    crash.log through faulthandler. Returns True when faulthandler is on. finish() undoes the second."""
    global _fault_file

    def thread_hook(args):
        if args.exc_type is SystemExit:
            return
        name = args.thread.name if args.thread else 'a thread'
        record(ctx, f'in the background ({name})', args.exc_type, args.exc_value, args.exc_traceback)
    threading.excepthook = thread_hook
    if os.name == 'nt':
        return False
    try:
        ctx.layout.logs.mkdir(parents=True, exist_ok=True)
        if _fault_file is None or _fault_file.name != str(crash_log(ctx)):
            if _fault_file is not None:
                faulthandler.disable()
                _fault_file.close()
            _fault_file = open(crash_log(ctx), 'a', encoding='utf-8', errors='replace')
        faulthandler.enable(file=_fault_file, all_threads=True)
        return True
    except (OSError, RuntimeError, ValueError):
        return False


def finish(ctx):
    """At the window's end: faulthandler off, and crash.log removed when it was opened for it and nothing was written:
    a player attaches the file that is there."""
    global _fault_file
    if _fault_file is None:
        return
    faulthandler.disable()
    _fault_file.close()
    try:
        path = crash_log(ctx)
        if path.stat().st_size == 0:
            path.unlink()
    except OSError:
        pass
    _fault_file = None


def window_hook(ctx):
    """For tk.Tk.report_callback_exception: an exception in a window callback is recorded, and the window goes on,
    as Tk lets it."""
    def report(exc_type, exc, tb):
        record(ctx, 'in the window', exc_type, exc, tb)
    return report


def on_end_signals(handler):
    """handler(name) for each of END_SIGNALS this system has, in place of the default that ends the process without a
    word. The names it took."""
    taken = []
    for name in END_SIGNALS:
        sig = getattr(signal, name, None)
        if sig is None:
            continue
        try:
            signal.signal(sig, lambda signum, frame, name=name: handler(name))
            taken.append(name)
        except (OSError, ValueError, RuntimeError):   # not the main thread, or not on this system
            pass
    return taken


WAKE_TICK_MS = 250   # how often a window that cannot listen to a socket runs Python to see a signal (Windows)


def wake_for_signals(root, use_socket=None):
    """Have the window's event loop act on a signal at once. Python runs a signal's handler only when it next runs
    Python code, and an idle Tk window waits inside Tcl for its next event: a SIGTERM to a window nobody touches
    (a logout, a shutdown) waited for that event, and a window that got none never closed. On POSIX the signal now
    writes a byte to a socket (signal.set_wakeup_fd) that the window listens to (createfilehandler), which wakes the
    loop and runs the handler. Tk on Windows cannot listen to a socket: there a timer runs Python every WAKE_TICK_MS.
    use_socket=False takes the timer anywhere (tests). Returns (how, undo): how, for the log, is 'woken through a
    socket' or 'checked every 0.25 s'; undo() puts back what was there before, after the event loop."""
    import tkinter
    if use_socket is None:
        use_socket = os.name != 'nt'
    if use_socket and hasattr(root.tk, 'createfilehandler'):
        import socket
        r, w = socket.socketpair()
        r.setblocking(False)
        w.setblocking(False)
        try:
            before = signal.set_wakeup_fd(w.fileno(), warn_on_full_buffer=False)
        except (ValueError, OSError):   # not the main thread
            r.close()
            w.close()
        else:
            def drain(fileno, mask):   # the handler itself has run by now: Python ran this callback
                try:
                    while r.recv(512):
                        pass
                except OSError:   # empty
                    pass
            root.tk.createfilehandler(r, tkinter.READABLE, drain)

            def undo():
                try:
                    root.tk.deletefilehandler(r)
                except tkinter.TclError:
                    pass
                try:
                    signal.set_wakeup_fd(before)
                except (ValueError, OSError):
                    pass
                r.close()
                w.close()
            return 'woken through a socket', undo
    stop = []

    def tick():
        if not stop:
            try:
                root.after(WAKE_TICK_MS, tick)
            except tkinter.TclError:   # the window is gone
                pass
    root.after(WAKE_TICK_MS, tick)
    return f'checked every {WAKE_TICK_MS / 1000:g} s', lambda: stop.append(1)
