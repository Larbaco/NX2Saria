import collections
import os
import signal
import time

from . import commit, saves, telemetry, winsys
from .build import FATAL_TAIL, game_log_path, last_fatal
from .platform import game_executable

EXIT_CODES = {
    70: 'a fatal error: the FATAL line near the end of its log says which, often memory that ran out',
    71: 'the crash report ended the game',
    72: 'the graphics device was lost: the GPU or its driver stopped, and the vk: lines near the end of its log say '
        'what it was doing; a graphics driver update may help',
    0xC0000005: 'access violation',
    0xC00000FD: 'stack overflow',
    0xC0000142: 'a DLL failed to start',
    0xC0000374: 'heap corruption',
    0xC0000409: 'fail-fast: a security check or abort ended the process without an exception',
    0xC0000417: 'invalid parameter to a C runtime function',
    0xC000041D: 'a fault inside a Windows callback',
    0xE06D7363: 'a C++ exception no one caught',
}


FATAL_CODES = (70, 71, 72)


ASKED_SIGNALS = tuple(getattr(signal, name) for name in ('SIGTERM', 'SIGINT', 'SIGHUP') if hasattr(signal, name))


def exit_code_text(code):
    if code < 0 and os.name != 'nt':
        try:
            name = signal.Signals(-code).name
        except ValueError:
            name = f'signal {-code}'
        return f'{code} (ended by {name})'
    code &= 0xFFFFFFFF
    meaning = EXIT_CODES.get(code)
    shown = f'0x{code:08X}' if code >= 0x80000000 else str(code)
    return f'{shown} ({meaning})' if meaning else shown


FATAL_SHOWN = 240


OWN_EXIT_LINES = ('gfx: window closed', 'guest ExitProcess', 'boot: frames', 'FATAL: ', 'exit: ')


LOG_CUT_LINE = 'log: the file reached 64 MiB'


def log_has_own_exit(game_dir):
    if not game_dir:
        return None
    try:
        with open(game_log_path(game_dir), 'rb') as f:
            f.seek(max(0, f.seek(0, 2) - FATAL_TAIL))
            lines = f.read().decode('utf-8', 'replace').splitlines()
    except OSError:
        return None
    for line in reversed(lines):
        if line.startswith('[launcher] the game ended'):
            return False
        body = line.split('] ', 1)[1] if line.startswith('[') and '] ' in line else line
        if body.startswith(OWN_EXIT_LINES):
            return True
        if body.startswith(LOG_CUT_LINE):
            return None
    return False


def note_exit_in_log(game_dir, text):
    if not game_dir:
        return
    log = game_log_path(game_dir)
    try:
        with open(log, 'a', encoding='utf-8') as f:
            f.write(f'[launcher] the game ended with code {text} at {time.strftime("%Y-%m-%d %H:%M:%S")}\n')
    except OSError:
        pass


def log_is_new(game_dir, started):
    if not game_dir or started is None:
        return False
    try:
        return game_log_path(game_dir).stat().st_mtime >= started - 2
    except OSError:
        return False


def shown_path(*parts):
    return ('\\' if os.name == 'nt' else '/').join(parts)


LOG_WHERE = f'Its log is {shown_path("logs", "nx2saria.log")} (older builds: nx2navi.log).'


GameEnd = collections.namedtuple('GameEnd', 'log message note alarm')


def shown_fatal(fatal):
    fatal = commit.fatal_summary(fatal) or fatal
    return fatal if len(fatal) <= FATAL_SHOWN else fatal[:FATAL_SHOWN].rstrip() + '…'


def game_end(game_dir, code, started=None, restarted=False):
    how = ' after restarting itself' if restarted else ''
    if code not in (0, None) and code < 0 and os.name != 'nt':
        text = exit_code_text(code)
        return GameEnd(f'the game ended with code {text}{how}', f'The game ended with code {text}. {LOG_WHERE}', text,
                       -code not in ASKED_SIGNALS)
    if code not in (0, None):
        code &= 0xFFFFFFFF
        text = exit_code_text(code)


        fatal = last_fatal(game_dir) if code in FATAL_CODES else ''
        if fatal:
            return GameEnd(f'the game ended with code {code}{how}; its log says: FATAL: {fatal}',
                           f'The game ended with a fatal error (code {code}): {shown_fatal(fatal)}. {LOG_WHERE}',
                           text, True)
        return GameEnd(f'the game ended with code {text}{how}', f'The game ended with code {text}. {LOG_WHERE}', text,
                       code in FATAL_CODES or code >= 0x80000000)




    fatal = last_fatal(game_dir) if log_is_new(game_dir, started) else ''
    if fatal:
        return GameEnd(f'the game ended after restarting itself; its log says: FATAL: {fatal}',
                       f"A copy started by the game's Restart ended with a fatal error: {shown_fatal(fatal)}. "
                       f'{LOG_WHERE}', '', True)



    own = log_has_own_exit(game_dir)
    if code is None:
        return GameEnd('the game ended after restarting itself; the exit code of its last run is not known', '',
                       "unknown (the game had restarted itself; the log has no line of the game's own end before "
                       'this one)' if own is False else '', False)
    if own is False:
        return GameEnd(f'the game ended with code 0{how}, and its log has no line of its own end', '',
                       "0 (the log has no line of the game's own end before this one)", False)
    return GameEnd(f'the game ended with code 0{how}', '', '', False)


END_KINDS = {70: 'fatal', 71: 'crash', 72: 'device_lost'}


def end_kind(code, end):
    if not end.message:
        return ''
    if code in (0, None):
        return 'fatal'
    if code < 0 and os.name != 'nt':
        return f'code {exit_code_text(code)}'
    code &= 0xFFFFFFFF
    return END_KINDS.get(code) or f'code {exit_code_text(code)}'


def end_asked(game_dir, code):
    return (code is not None and code < 0 and os.name != 'nt' and -code in ASKED_SIGNALS
            and not last_fatal(game_dir))


class GameRun:
    def __init__(self, proc, game_dir, started):
        self.proc, self.game_dir, self.started, self.restarted = proc, game_dir, started, False
        self.last_pid = None
        self.kind, self.asked = '', False

    def alive(self):
        return self.proc is not None and self.proc.poll() is None and getattr(self.proc, 'ended', False) is not True

    def running(self):
        if self.alive():
            return True
        if not (self.game_dir and saves.game_running(self.game_dir)):
            self.finished()
            return False
        if self.proc is not None and self.proc.returncode == 0:
            self.restarted = True
            self.last_pid = self.proc.pid
            self.proc = winsys.game_copy(game_executable(self.game_dir), skip=self.last_pid)
        elif self.restarted and self.proc is None:
            self.proc = winsys.game_copy(game_executable(self.game_dir), skip=self.last_pid)
        return True

    def finished(self):
        if not self.game_dir:
            return
        codes = winsys.game_finished(game_executable(self.game_dir),
                                     skip=None if self.proc is None else self.proc.pid)
        if codes and (self.proc is None or self.proc.returncode == 0):
            self.restarted = True
            self.proc = collections.namedtuple('EndedCopy', 'pid returncode ended')(None, codes[-1], True)

    def code(self):
        return None if self.proc is None else self.proc.returncode

    def end(self):
        return game_end(self.game_dir, None if self.proc is None else self.proc.returncode, self.started,
                        self.restarted)


def finish_game(run, log):
    end = run.end()
    log(end.log)
    run.kind, run.asked = end_kind(run.code(), end), end_asked(run.game_dir, run.code())
    if telemetry.enabled():
        telemetry.synthesize_crash(run.game_dir, log)
    if end.note:
        note_exit_in_log(run.game_dir, end.note)
    return end
