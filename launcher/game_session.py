import collections
import os
import signal
import time

from . import commit, saves, winsys
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
    """The exit code as the game's log and Windows show it: hex for a Windows status, with its meaning when known.
    Elsewhere a negative code is subprocess's for a process a signal ended, shown with the signal's name."""
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
    """Whether the end of the current or legacy game log holds a line of the game's own end (OWN_EXIT_LINES); None
    when the log cannot be read, or when it was cut at its size cap (LOG_CUT_LINE) and so cannot tell. A line this
    launcher added for an earlier run's end (note_exit_in_log) ends the search."""
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
    """Adds the exit code to the end of the game's log, so a log a player sends shows how the run ended even when the
    process died before it could write a line itself."""
    if not game_dir:
        return
    log = game_log_path(game_dir)
    try:
        with open(log, 'a', encoding='utf-8') as f:
            f.write(f'[launcher] the game ended with code {text} at {time.strftime("%Y-%m-%d %H:%M:%S")}\n')
    except OSError:
        pass


def log_is_new(game_dir, started):
    """Whether the selected game log was written at or after started (time.time() before the game was started). A run that
    ends before it moves and opens its log (runtime/main.c log_open) leaves the log of a run before it in place. FAT
    and exFAT drives keep a file's time to 2 s, hence the slack."""
    if not game_dir or started is None:
        return False
    try:
        return game_log_path(game_dir).stat().st_mtime >= started - 2
    except OSError:
        return False


def shown_path(*parts):
    """A path inside the game folder as the player's system writes it, for the window's sentences: logs\\nx2saria.log
    on Windows, logs/nx2saria.log elsewhere."""
    return ('\\' if os.name == 'nt' else '/').join(parts)


LOG_WHERE = f'Its log is {shown_path("logs", "nx2saria.log")} (older builds: nx2navi.log).'


GameEnd = collections.namedtuple('GameEnd', 'log message note alarm')


def shown_fatal(fatal):
    fatal = commit.fatal_summary(fatal) or fatal
    return fatal if len(fatal) <= FATAL_SHOWN else fatal[:FATAL_SHOWN].rstrip() + '…'


def game_end(game_dir, code, started=None, restarted=False):
    """What the launcher says about an end of the game (App.watch_game, play_only): a GameEnd of the line for
    launcher.log, the sentence for the player ('' for none), the text note_exit_in_log adds to the game's log ('' for
    none), and alarm: whether the end is worth a message box where no window shows the sentence (a FATAL line, a code
    of FATAL_CODES or a Windows status; not 0, nor 1, which Task Manager's End task gives). code is the exit code of
    the run's last process, or None when it is not known, which happens only for a copy the game's own Restart started
    that could not be followed (GameRun). started is time.time() before the game was started, restarted whether it
    restarted itself on the way."""
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


class GameRun:
    """A Play of the game, followed through the copies its own Restart starts. The Restart (runtime/gfx/gfx_present.c
    gfx_window_closed) starts the exe again (runtime/platform/win32.c plat_restart) and ends the process before it
    with code 0, so the code the game really ended with is its last copy's: once the process followed has ended with
    0 while the exe still runs, the copy is looked for (winsys.game_copy) and followed from then on. On Linux the
    Restart (runtime/platform/linux.c plat_restart) spawns the copy as a child of the old process; winsys.start_game
    makes the launcher a child subreaper, so the copy becomes the launcher's child once the old process has ended and
    its code can be waited for. proc is the process followed, None while a copy could not be found (it ended before
    the next look, or the system did not show it): it is looked for again each second while the game still runs, and
    the end's code is not known if none is found. started is time.time() before the start (game_end's
    log_is_new)."""

    def __init__(self, proc, game_dir, started):
        self.proc, self.game_dir, self.started, self.restarted = proc, game_dir, started, False
        self.last_pid = None

    def alive(self):
        """Whether the process followed runs; nothing else is looked at. A winsys.GameCopy whose exit code could not
        be read has ended all the same."""
        return self.proc is not None and self.proc.poll() is None and getattr(self.proc, 'ended', False) is not True

    def running(self):
        """Whether the game still runs, this launcher's process or another of the same exe (build.game_running);
        asked once a second until it says False."""
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
        """The run is over: a copy the Restart started that ended before running() looked for it is the run's last
        process, and its code is the end's (winsys.game_finished)."""
        if not self.game_dir:
            return
        codes = winsys.game_finished(game_executable(self.game_dir),
                                     skip=None if self.proc is None else self.proc.pid)
        if codes and (self.proc is None or self.proc.returncode == 0):
            self.restarted = True
            self.proc = collections.namedtuple('EndedCopy', 'pid returncode ended')(None, codes[-1], True)

    def end(self):
        """game_end for this run, once running() said False."""
        return game_end(self.game_dir, None if self.proc is None else self.proc.returncode, self.started,
                        self.restarted)
