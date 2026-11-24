from dataclasses import dataclass
import time

from . import build, commit, mods, saves, winsys
from .game_session import GameRun
from .platform import game_executable


@dataclass
class PlayPreparation:
    status: str
    exe: object = None
    searched: str = ''
    waiting: object = None
    mod_message: object = None
    home_message: object = None
    memory: object = None


def not_started(log, reason, searched):
    log(f'play: not started: {reason[:160]} ({searched})')


def prepare(ctx, settings, game_dir, current, failed_mods, built=False):
    exe = game_executable(game_dir) if game_dir else None
    searched = str(exe) if exe else 'no game folder is selected'
    if not exe or not exe.is_file():
        not_started(ctx.log, 'the game is not built', searched)
        return PlayPreparation('not_built', exe, searched)
    if current and current.alive():
        not_started(ctx.log, 'this launcher is already watching a game', searched)
        return PlayPreparation('own_game', exe, searched)
    if saves.game_running(game_dir):
        not_started(ctx.log, 'the game is already running', searched)
        return PlayPreparation('external_game', exe, searched)
    last = home = None
    waiting = []
    if not built:
        try:
            for line in mods.prepare(game_dir, log=ctx.log):
                last = line
                if 'was not unpacked' in line:
                    home = line
        except OSError as e:
            ctx.log(f'mods: the mods folder could not be put in order: {e}')
        try:
            waiting = mods.code_waiting(game_dir) if build.can_rebuild(game_dir) else []
        except OSError as e:
            ctx.log(f'mods: {e}')
    if waiting and waiting != failed_mods:
        ctx.log(f'play: the game is built again first for the code of {", ".join(waiting)}')
        return PlayPreparation('rebuild', exe, searched, waiting, last, home)
    if waiting:
        ctx.log(f'play: without the code of {", ".join(waiting)}, whose build did not finish')
    memory = commit.play_memory_request(game_dir, ctx.log, settings)
    return PlayPreparation('memory' if memory is not None else 'ready', exe, searched, waiting, last, home, memory)


def start(ctx, prepared, game_dir, accepted=True):
    if not accepted:
        not_started(ctx.log, 'the memory warning was declined', prepared.searched)
        return None, None
    started = time.time()
    try:
        run = GameRun(winsys.start_game(prepared.exe), game_dir, started)
    except OSError as e:
        not_started(ctx.log, f'could not start the game: {(str(e).splitlines() or [type(e).__name__])[0]}',
                    prepared.searched)
        return None, e
    ctx.log(f'play: {prepared.exe}')
    return run, None
