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
            f'{winsys.WINDOWS_LOW_COMMIT_ADVICE}\n'
            "Don't warn me again: turn off Warn About Low Commit in launcher Settings, or set mem_need_mb = 0 "
            'in nx2saria.ini.\n\nPlay anyway?')


def fatal_summary(fatal):
    """Keep the actionable Windows commit advice visible in the launcher's shortened fatal message."""
    names = {1455: 'ERROR_COMMITMENT_LIMIT', 8: 'ERROR_NOT_ENOUGH_MEMORY', 1450: 'ERROR_NO_SYSTEM_RESOURCES'}
    for code, name in names.items():
        if f'Win32 {code} ({name})' in fatal:
            return (f'Windows ran out of memory it can promise (RAM plus page file), Win32 {code} {name}. '
                    'Close memory-heavy programs; turn on or enlarge the page file (fixed 32768 MB for 16 GB RAM), '
                    'then restart Windows.')
    return ''


def set_warning_enabled(settings, enabled, log):
    """Persist the launcher's warning choice and leave its value in launcher.log."""
    settings['warn_low_commit'] = bool(enabled)
    settings.save()
    log(f'launcher setting: warn_low_commit = {int(bool(enabled))}')


def confirm_play_memory(game_dir, log, root=None, settings=None):
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
    try:
        page_file_managed = winsys.page_file_is_system_managed()
    except (AttributeError, OSError) as e:
        page_file_managed = None
        log(f'play memory: page-file mode unavailable ({e})')
    warning_enabled = settings is None or settings.get('warn_low_commit', True)
    should_warn = warning and page_file_managed is False and warning_enabled
    if need <= 0:
        status = 'disabled by mem_need_mb'
    elif page_file_managed is True:
        status = 'system-managed page file; log only'
    elif page_file_managed is None:
        status = 'page-file mode unknown; log only'
    elif not warning_enabled:
        status = 'disabled in launcher Settings'
    else:
        status = 'warning' if warning else 'enough'
    log(f'play memory: commit {used:.1f} of {limit:.1f} MiB used, {remaining:.1f} MiB left, '
        f'{ram:.1f} MiB RAM, game estimate {need:g} MiB, page file '
        f'{"system-managed" if page_file_managed is True else "fixed or off" if page_file_managed is False else "unknown"} '
        f'({status})')
    if not should_warn:
        return True
    try:
        play = winsys.confirm_play(warning, root)
    except (AttributeError, OSError) as e:
        log(f'play memory: warning dialog unavailable ({e}); continuing')
        return True
    log('play memory: player chose to play anyway' if play else 'play memory: player cancelled')
    return play
