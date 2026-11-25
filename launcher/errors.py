import faulthandler
import os
import signal
import sys
import threading
import time
import traceback

END_SIGNALS = ('SIGTERM', 'SIGHUP')

_fault_file = None


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
    where = ''
    frames = traceback.extract_tb(tb) if tb is not None else []
    if frames:
        f = frames[-1]
        where = f' (in {f.name}, {f.filename.replace(chr(92), "/").rsplit("/", 1)[-1]}:{f.lineno})'
    message = str(exc).splitlines()[0] if str(exc) else ''
    return f'{getattr(exc_type, "__name__", exc_type)}{": " + message if message else ""}{where}'[:300]


def record(ctx, where, exc_type, exc, tb):
    ctx.log(f'error {where}: {summary(exc_type, exc, tb)}; the traceback is in data/logs/crash.log')
    from .context import shorten
    text = ''.join(traceback.format_exception(exc_type, exc, tb))
    _write_crash(ctx, shorten(f'{time.strftime("%Y-%m-%d %H:%M:%S")} launcher {ctx.version}, {where}:\n{text}'))


def install(ctx):
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
    def report(exc_type, exc, tb):
        record(ctx, 'in the window', exc_type, exc, tb)
    return report


def on_end_signals(handler):
    taken = []
    for name in END_SIGNALS:
        sig = getattr(signal, name, None)
        if sig is None:
            continue
        try:
            signal.signal(sig, lambda signum, frame, name=name: handler(name))
            taken.append(name)
        except (OSError, ValueError, RuntimeError):
            pass
    return taken


WAKE_TICK_MS = 250


def wake_for_signals(root, use_socket=None):
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
        except (ValueError, OSError):
            r.close()
            w.close()
        else:
            def drain(fileno, mask):
                try:
                    while r.recv(512):
                        pass
                except OSError:
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
            except tkinter.TclError:
                pass
    root.after(WAKE_TICK_MS, tick)
    return f'checked every {WAKE_TICK_MS / 1000:g} s', lambda: stop.append(1)
