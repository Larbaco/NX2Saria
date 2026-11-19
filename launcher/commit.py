"""Windows commit preflight for Play, kept independent of the launcher's window and graphics dependencies."""
import math
import os

from . import gameini, winsys

DEFAULT_MEM_NEED_MB = 24576   # runtime/mem_need.h MEM_NEED_DEFAULT_MB


def game_commit_need(game_dir):
    """The game's estimated commit in play, with the same settings precedence as runtime/cfg.c."""
    value = os.environ.get('NX2SARIA_MEM_NEED_MB', os.environ.get('NX2NAVI_MEM_NEED_MB'))
    if value is None:
        try:
            value = gameini.get(game_dir, 'MEM_NEED_MB', choices=True)
        except OSError:
            value = ''
    if not value:
        return DEFAULT_MEM_NEED_MB
    try:
        need = float(value)
    except ValueError:
        return 0
    return 0 if math.isnan(need) else need


def needs_commit_warning(remaining, need):
    """Whether current Windows commit headroom is below the game's estimated need, in MiB."""
    return need > 0 and remaining < need


def commit_warning(remaining, need):
    """The optional Play prompt, using current system commit headroom and the game's estimate in MiB."""
    if not needs_commit_warning(remaining, need):
        return ''
    return (f'Windows has {int(remaining)} MiB of committed memory left (RAM plus page file). '
            f'The game typically needs about {need:g} MiB while you play.\n\n'
            f'{winsys.WINDOWS_LOW_COMMIT_ADVICE}\n\nPlay anyway?')


def fatal_summary(fatal):
    """Keep the actionable Windows commit advice visible in the launcher's shortened fatal message."""
    names = {1455: 'ERROR_COMMITMENT_LIMIT', 8: 'ERROR_NOT_ENOUGH_MEMORY', 1450: 'ERROR_NO_SYSTEM_RESOURCES'}
    for code, name in names.items():
        if f'Win32 {code} ({name})' in fatal:
            return (f'Windows ran out of memory it can promise (RAM plus page file), Win32 {code} {name}. '
                    'Close memory-heavy programs; turn on or enlarge the page file (fixed 32768 MB for 16 GB RAM), '
                    'then restart Windows.')
    return ''


def confirm_play_memory(game_dir, log, root=None):
    """Sample Windows commit just before launch. An unavailable check or dialog still allows Play."""
    try:
        info = winsys.system_commit_info()
    except (AttributeError, OSError) as e:
        log(f'play memory: commit check unavailable ({e}); continuing')
        return True
    if info is None:
        return True
    used, limit, ram = info
    remaining = max(0, limit - used)
    need = game_commit_need(game_dir)
    warning = commit_warning(remaining, need)
    status = 'disabled' if need <= 0 else 'warning' if warning else 'enough'
    log(f'play memory: commit {used:.1f} of {limit:.1f} MiB used, {remaining:.1f} MiB left, '
        f'{ram:.1f} MiB RAM, game estimate {need:g} MiB ({status})')
    if not warning:
        return True
    try:
        play = winsys.confirm_play(warning, root)
    except (AttributeError, OSError) as e:
        log(f'play memory: warning dialog unavailable ({e}); continuing')
        return True
    log('play memory: player chose to play anyway' if play else 'play memory: player cancelled')
    return play
