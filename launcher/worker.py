import argparse
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time
from concurrent.futures import Future
from functools import partial
from pathlib import Path

from . import build, build_recovery, commit, feed, game_session, gamefiles, gameini, jobs, locks, mods, mods_job, paths, play, saves, steam, telemetry, updater, versions, winsys
from .build import game_log_path, game_log_paths
from .context import Context, shorten_always
from .inspect_installation import inspect_installation
from .paths import Layout, install_folder
from . import worker_preferences
from .worker_reports import QUESTION_ANSWERS, Reports, installed_crash_offer, preview_frame

PROTOCOL = 1
MAX_FRAME = 65536
MAX_ACTION_LOGS = 10
MODS_PAGE_SIZE = 6
SETTINGS_FLAGS = ('auto_check', 'auto_install', 'keep_cache')
_input_buffer = bytearray()
_output = None
_output_disconnected = False
_output_lock = threading.Lock()


class GameWindowLog:
    def __init__(self, game_dir):
        self.game_dir = game_dir
        self.identity = None
        self.offset = 0
        self.partial = b''
        self.previous = {}
        for path in game_log_paths(game_dir):
            try:
                info = path.stat()
            except OSError:
                continue
            self.previous[(info.st_dev, info.st_ino)] = info.st_size

    def focused(self):
        path = game_log_path(self.game_dir)
        try:
            info = path.stat()
            identity = (info.st_dev, info.st_ino)
            if identity == self.identity and info.st_size == self.offset:
                return False
            with path.open('rb') as log:
                info = os.fstat(log.fileno())
                identity = (info.st_dev, info.st_ino)
                if identity != self.identity:
                    self.identity, self.offset, self.partial = identity, self.previous.get(identity, 0), b''
                if info.st_size < self.offset:
                    self.offset, self.partial = 0, b''
                log.seek(self.offset)
                data = self.partial + log.read(MAX_FRAME)
                self.offset = log.tell()
        except OSError:
            return False
        *lines, tail = data.split(b'\n')
        self.partial = tail[:256]
        return any(line.startswith(b'[host] gfx: window gained the focus at ') for line in lines)


def emit(payload):
    global _output_disconnected
    frame = (json.dumps(payload, ensure_ascii=True, allow_nan=False) + '\n').encode('utf-8')
    with _output_lock:
        if _output_disconnected:
            return
        try:
            _output.write(frame)
            _output.flush()
        except (BrokenPipeError, OSError):
            _output_disconnected = True


def reject_constant(value):
    raise ValueError('non-finite number')


def read_request():
    while True:
        end = _input_buffer.find(b'\n')
        if end >= 0:
            raw = bytes(_input_buffer[:end + 1])
            del _input_buffer[:end + 1]
            if len(raw) > MAX_FRAME:
                raise ValueError('invalid frame')
            break
        if len(_input_buffer) >= MAX_FRAME:
            raise ValueError('invalid frame')
        chunk = os.read(sys.stdin.fileno(), min(4096, MAX_FRAME + 1 - len(_input_buffer)))
        if not chunk:
            if _input_buffer:
                raise ValueError('invalid frame')
            return None
        _input_buffer.extend(chunk)
    request = json.loads(raw.decode('utf-8'), parse_constant=reject_constant)
    if not isinstance(request, dict) or type(request.get('id')) not in (int, str):
        raise ValueError('invalid request')
    return request


def read_requests(requests, input_closed):
    while True:
        try:
            request = read_request()
        except (ValueError, UnicodeError, RecursionError):
            input_closed.set()
            requests.put(('invalid', None))
            return
        if request is None:
            input_closed.set()
        requests.put(('request', request))
        if request is None:
            return


def error(request_id, code):
    emit({'id': request_id, 'error': {'code': code}})


def action_log(ctx, counts, action, detail):
    count = counts.get(action, 0)
    counts[action] = count + 1
    if count < MAX_ACTION_LOGS:
        ctx.log(f'worker: {action} {detail}')
    elif count == MAX_ACTION_LOGS:
        ctx.log(f'worker: {action} further events suppressed')


def finish_recovery(ctx, observed, reason):
    try:
        jobs.interrupted(ctx)
    except RecursionError:
        try:
            (ctx.layout.data / jobs.MARKER).unlink(missing_ok=True)
        except OSError:
            pass
    if build_recovery.blocked(ctx.layout):
        return build_recovery.Recovery('unverified', 'marker_remove_failed', observed.kind,
                                       fingerprint=observed.fingerprint)
    build_recovery.forget_observation(ctx.layout)
    return build_recovery.Recovery('interrupted', reason, observed.kind)


def inspect_recovery(ctx, counts, previous):
    observed = build_recovery.inspect_marker(ctx.layout)
    if observed.state == 'gone':
        observed = finish_recovery(ctx, observed, observed.reason)
    elif observed.state == 'unverified' and build_recovery.rebooted_since_observation(ctx.layout, observed):
        observed = finish_recovery(ctx, observed, 'boot_changed')
    if observed.state == 'none':
        return previous if previous and previous.state == 'interrupted' else None
    if not previous or observed.view() != previous.view():
        action_log(ctx, counts, 'recovery', f'state={observed.state} reason={observed.reason} kind={observed.kind}')
    return observed


def first_run_requirements(ctx):
    if ctx.zstd_missing:
        return {'status': 'runtime_zstd_missing'}
    if sys.platform.startswith('linux'):
        from .platform.linux import toolchain_missing
        try:
            missing = toolchain_missing(run=partial(subprocess.run, timeout=2))
        except (OSError, subprocess.SubprocessError):
            return {'status': 'linux_check_failed'}
        if missing:
            return {'status': 'linux_tools_missing', 'message': missing}
    return {'status': 'ready'}


def pending_rebuild(ctx, game):
    if ctx.layout.dev or not game:
        return {}
    built = build.built_version(game)
    if not built:
        return {'rebuild_required': True} if build.installed(game) else {}
    try:
        if isinstance(built, str) and len(built) <= 64 and len(ctx.version) <= 64 and versions.parse(built) != versions.parse(ctx.version):
            return {'rebuild_required': True, 'build_version': versions.display(built), 'app_version': versions.display(ctx.version)}
    except (TypeError, ValueError):
        pass
    return {}


def prepare_worker_play(ctx, game, run, failed_mods, built=False):
    rebuild = pending_rebuild(ctx, game)
    if rebuild and 'build_version' not in rebuild:
        return play.PlayPreparation('rebuild_required')
    return play.prepare(ctx, ctx.settings, game, run, failed_mods, built=built)


def start_worker_play(ctx, prepared, game):
    rebuild = pending_rebuild(ctx, game)
    if 'build_version' in rebuild:
        ctx.log(f'play: game build {rebuild["build_version"]} under app {rebuild["app_version"]}, rebuild pending')
    return play.start(ctx, prepared, game)


def installation_view(ctx, recovery, counts):
    result = inspect_installation(ctx.layout.root)
    if result['status'] == 'ready':
        result.update(pending_rebuild(ctx, result['game_dir']))
    if result['status'] in ('not_configured', 'not_ready'):
        settings = ctx.settings
        requirements = first_run_requirements(ctx)
        result['first_run_requirements'] = requirements
        action_log(ctx, counts, 'install', f'requirements status={requirements["status"]}')
        result['first_run_preferences'] = {
            'channel': 'preview' if settings['channel'] == 'preview' else 'stable',
            'auto_check': settings['auto_check'],
            'desktop_shortcut': settings['desktop_shortcut'] if os.name == 'nt' else None,
        }
    if recovery:
        result['recovery'] = recovery.view()
        if recovery.state in ('alive', 'unverified'):
            result['job_marker_present'] = True
    return result


def bounded_text(text, units):
    return text.encode('utf-16-le', 'replace')[:units * 2].decode('utf-16-le', 'ignore')


def checked_update(ctx, check, counts, channel):
    if channel != ctx.settings.get('channel'):
        result = {'status': 'superseded'}
    elif check.result != 'ok':
        ctx.log(f'update check: {check.error or "cancelled"}')
        result = {'status': 'failed'}
    else:
        release = check.release
        try:
            if release:
                version = versions.display(release.version)
                if not version or len(version) > 64 or not version.isascii():
                    raise ValueError('invalid release version')
                new = jobs.update_available(ctx, release)
                notes = [bounded_text(line, 512) for line in feed.plain_notes(release.notes, limit=20)]
                result = {'status': 'available' if new else 'up_to_date', 'version': version, 'notes': notes}
            else:
                result = {'status': 'no_release'}
            ctx.settings.record_update_check(time.time(), release.version if release else None)
            ctx.log(f'update check: {release.version if release else "no release"}')
        except (OSError, ValueError, TypeError):
            ctx.log('update check: could not record the result')
            result = {'status': 'failed'}
    detail = f'result={result["status"]}'
    if 'version' in result:
        detail += f' version={result["version"]} notes={len(result["notes"])}'
    action_log(ctx, counts, 'check', detail)
    return result


def export_save(ctx, game, counts):
    settings = ctx.settings
    try:
        result = saves.export_save(game, saves.export_destination(settings), bug_report=settings['export_with_log'],
                                   version=versions.display(build.built_version(game) or ctx.version), log=ctx.log)
    except (saves.SaveError, OSError) as e:
        status = 'no_save' if isinstance(e, saves.SaveError) and str(e) == saves.NO_SAVE else 'failed'
        if status == 'failed':
            ctx.log(f'saves: export failed: {e}')
        action_log(ctx, counts, 'saves', f'export result={status}')
        return {'status': status}
    with_extras = bool(result['extras'])
    try:
        winsys.reveal(result['path'])
    except OSError:
        action_log(ctx, counts, 'saves', 'reveal failed')
    action_log(ctx, counts, 'saves', f'export result=ok files={result["files"]} report={with_extras}')
    return {'status': 'ok', 'name': result['path'].name, 'files': result['files'], 'included_log': with_extras}


def import_save(ctx, game, run, counts, request):
    source = request.get('source')
    raw = request.get('path')
    folder = None
    valid = source == 'waiting' and raw is None
    if source in ('archive', 'folder') and isinstance(raw, str) and '\0' not in raw:
        folder = Path(raw)
        valid = folder.is_absolute() and (folder.is_file() if source == 'archive' else folder.is_dir())
    if not valid:
        action_log(ctx, counts, 'saves', 'import result=invalid_source')
        return {'status': 'invalid_source'}
    try:
        if (run and run.alive()) or saves.game_running(game):
            raise saves.SaveError(saves.RUNNING)
        imported = saves.import_save(game, folder, log=ctx.log)
    except (saves.SaveError, OSError) as e:
        if isinstance(e, saves.SaveError):
            status = 'game_running' if str(e) == saves.RUNNING else 'invalid_save'
        else:
            status = 'failed'
        action_log(ctx, counts, 'saves', f'import result={status}')
        ctx.log(f'saves: import refused: {e}')
        detail = shorten_always(str(e)).splitlines()
        return {'status': status, 'detail': detail[0][:260] if detail else ''}
    backup = imported['backup'].name if imported['backup'] else None
    action_log(ctx, counts, 'saves', f'import result=ok files={imported["files"]} backup={bool(backup)}')
    return {'status': 'ok', 'files': imported['files'], 'slot': imported['slot'], 'backup': backup}


def mods_view(game, page):
    entries = mods.scan(game)
    pages = max(1, (len(entries) + MODS_PAGE_SIZE - 1) // MODS_PAGE_SIZE)
    page = min(page, pages - 1)
    start = page * MODS_PAGE_SIZE
    conflicts = mods.overlaps(entries)
    items = []
    for mod in entries[start:start + MODS_PAGE_SIZE]:
        items.append({'name': mod.name, 'enabled': mod.enabled, 'usable': mod.usable, 'waiting': mod.waiting,
                      'files': len(mod.files), 'patches': mod.patches, 'cheats': bool(mod.cheat_file),
                      'other_version': mod.other_version, 'problem': bounded_text(mod.problem, 256),
                      'overlaps': [{'name': name, 'files': files, 'wins': wins}
                                   for name, files, wins in conflicts.get(mod.name, [])[:3]]})
    return {'page': page, 'pages': pages, 'count': len(entries), 'items': items}


def change_mod(ctx, game, run, counts, request):
    method = request['method']
    action = method.removeprefix('mod_') if method != 'open_mods_dir' else 'open'
    result = {'status': 'ok'}
    try:
        if method == 'open_mods_dir':
            winsys.open_path(mods.ensure_mods_dir(game))
        elif method == 'mod_add':
            source = request.get('path')
            if (not isinstance(source, str) or '\0' in source or not Path(source).is_absolute() or
                    not (Path(source).is_file() or Path(source).is_dir())):
                raise mods.ModError('Choose an existing mod file or folder.')
            target = mods.add(game, source, log=ctx.log)
            added = next((mod for mod in mods.scan(game) if mod.name == target.name), None)
            result['name'] = target.name
            result['problem'] = bounded_text(added.problem, 256) if added else ''
        else:
            if (run and run.alive()) or saves.game_running(game):
                action_log(ctx, counts, 'mods', f'{action} result=game_running')
                return {'status': 'game_running'}
            name = request.get('name')
            selection = next((mod for mod in mods.scan(game, details=method == 'mod_enabled') if mod.name == name), None)
            if selection is None:
                raise mods.ModError('The selected mod is no longer present.')
            if method == 'mod_enabled':
                value = request.get('value')
                if type(value) is not bool:
                    raise mods.ModError('Choose whether the mod is enabled.')
                if not selection.usable:
                    raise mods.ModError(selection.problem)
                mods.set_enabled(game, selection.name, value)
            else:
                direction = request.get('direction')
                if type(direction) is not int or direction not in (-1, 1):
                    raise mods.ModError('Move the mod one place up or down.')
                mods.move(game, selection.name, direction)
                updated = [mod.name for mod in mods.scan(game, details=False)]
                if selection.name in updated:
                    result['page'] = updated.index(selection.name) // MODS_PAGE_SIZE
    except (mods.ModError, OSError) as e:
        ctx.log(f'mods: {action} refused: {e}')
        result = {'status': 'invalid_mod' if isinstance(e, mods.ModError) else 'failed',
                  'detail': bounded_text(shorten_always(str(e)), 256)}
    action_log(ctx, counts, 'mods', f'{action} result={result["status"]}')
    return result


def steam_view():
    try:
        found = steam.find_steam()
        if not found:
            return {'status': 'not_found', 'running': False}
        linux = sys.platform.startswith('linux')
        if linux and steam.is_flatpak_root(found[0]):
            return {'status': 'flatpak_unsupported', 'running': False}
        added = steam.added(None if linux else found[0])
        return {'status': 'added' if added else 'available', 'running': steam.steam_running()}
    except OSError:
        return {'status': 'unavailable', 'running': False}


def add_to_steam(ctx, game, dump, request):
    try:
        detail = steam.add(ctx.layout, game, dump, ctx.log)
    except (OSError, steam.SteamError) as e:
        detail, status = str(e), 'failed'
    except Exception as e:
        detail, status = str(e) or 'Adding to Steam failed.', 'failed'
    else:
        status = 'ok'
    detail = bounded_text(shorten_always(detail), 256)
    ctx.log(f'steam: {detail}')
    request.set_result({'status': status, 'detail': detail})


def udmabuf_script(ctx):
    return ctx.layout.app / 'launcher' / 'platform' / 'udmabuf_fix.sh'


def udmabuf_action(ctx):
    status = ctx.udmabuf
    if not sys.platform.startswith('linux') or not status or not udmabuf_script(ctx).is_file():
        return None
    from .platform import linux
    if status['offered']:
        return 'apply'
    if not status['needs_fix'] and linux.udmabuf_fix_installed() and linux.udmabuf_change_available():
        return 'remove'
    return None


def change_udmabuf(ctx, action, confirmed):
    if not sys.platform.startswith('linux'):
        return 'unavailable'
    from .platform import linux
    ctx.udmabuf = linux.udmabuf_status()
    if action != udmabuf_action(ctx):
        return 'unavailable'
    if not confirmed:
        ctx.log(f'udmabuf: offer refused; action={action}')
        return 'cancelled'

    ctx.log(f'udmabuf: offer accepted; action={action}')
    ok, detail = linux.run_udmabuf_fix(udmabuf_script(ctx), action)
    cancelled = not ok and detail == 'authorization cancelled'
    if cancelled:
        ctx.log('udmabuf: offer refused; PolicyKit dialog dismissed')
    status = ctx.udmabuf = linux.udmabuf_status()
    ctx.log(linux.udmabuf_after_log_line(action, status))
    ctx.log(f'udmabuf: fix {"succeeded" if ok else "failed"}: {detail}')
    if not ok:
        return 'cancelled' if cancelled else 'failed'
    if action == 'remove':
        return 'removed'
    if status is None:
        return 'module_absent'
    if not status['accessible']:
        return 'no_access'
    if status['limit_mb'] < linux.UDMABUF_MINIMUM_MB:
        return 'low_limit'
    return 'applied'


def installed_settings(ctx):
    settings = ctx.settings
    game = settings['game_dir']
    discord = None
    if game and Path(game).is_dir():
        try:
            discord = gameini.get(game, 'discord_rpc', migrate=False) != '0'
        except OSError:
            discord = True
    return {**{key: settings[key] for key in SETTINGS_FLAGS},
            'game_dir': settings['game_dir'], 'dump': settings['dump'],
            'channel': 'preview' if settings['channel'] == 'preview' else 'stable',
            'warn_low_commit': settings['warn_low_commit'] if os.name == 'nt' else None,
            'uninstall_available': winsys.uninstall_available(ctx.layout), 'discord_status': discord,
            'udmabuf_action': udmabuf_action(ctx)}


def write_discord_setting(ctx, value):
    game = ctx.settings['game_dir']
    if type(value) is not bool:
        return None, 'invalid_preference'
    if not game or not Path(game).is_dir() or saves.game_running(game):
        return None, 'settings_unavailable'
    try:
        gameini.put(game, 'discord', 'discord_rpc', '1' if value else '0')
    except OSError:
        return None, 'write_failed'
    return {'enabled': value}, None


def write_installed_setting(ctx, request):
    key, value = request.get('key'), request.get('value')
    if key == 'channel':
        valid = value in ('stable', 'preview')
    else:
        valid = (key in SETTINGS_FLAGS or (key == 'warn_low_commit' and os.name == 'nt')) and type(value) is bool
    if not valid:
        return None, 'invalid_preference'
    try:
        ctx.settings.save_preference(key, value)
    except (OSError, ValueError):
        return None, 'preference_save_failed'
    return {'key': key, 'value': value}, None


def write_installed_dump(ctx, request):
    try:
        selected = install_dump(ctx, request)
        if not selected or selected['source'] != 'selected':
            return None, 'invalid_dump'
        file = selected['file']
        ctx.settings.save_preference('dump', file)
    except (OSError, ValueError):
        return None, 'preference_save_failed'
    return {'file': file}, None


def open_settings_folder(ctx, target):
    if target == 'logs':
        folder = ctx.layout.logs
    elif target == 'game':
        folder = ctx.settings.get('game_dir')
        if not folder or not Path(folder).is_dir():
            return None, 'settings_unavailable'
    else:
        return None, 'invalid_target'
    try:
        winsys.open_path(folder)
    except OSError:
        return None, 'open_failed'
    return {'status': 'ok'}, None


def install_space(folder):
    complete = (Path(folder) / 'romfs' / 'romfs.bin').is_file()
    available = paths.free_gb(folder) + paths.partial_image_gb(folder)
    return {'available_gb': int(available), 'required_gb': paths.INSTALL_SPACE_GB,
            'complete_image': complete, 'path_too_long': bool(paths.long_path_problem(folder))}


def install_destination(ctx, request):
    if 'folder' in request:
        folder = request['folder']
        if not isinstance(folder, str) or '\0' in folder or not Path(folder).is_absolute() or not Path(folder).is_dir():
            return None
        folder, source = str(install_folder(folder)), 'selected'
    else:
        saved = ctx.settings.get('game_dir')
        folder, source = (saved, 'saved') if saved else (str(ctx.layout.default_game_dir()), 'default')
    return {'folder': folder, 'source': source, **install_space(folder)}


def install_dump(ctx, request):
    selected = 'file' in request
    file = request['file'] if selected else ctx.settings.get('dump')
    if (isinstance(file, str) and '\0' not in file and Path(file).is_absolute()
            and Path(file).is_file() and gamefiles.is_game_nup(ctx.layout, file)):
        return {'file': file, 'source': 'selected' if selected else 'saved'}
    if not selected:
        return {'file': None, 'source': 'unset'}
    return None


def search_install_dump(layout, messages):
    started = time.monotonic()
    report = {}
    try:
        drives = []
        if sys.platform.startswith('linux'):
            from .platform.linux import mounted_drives
            drives = mounted_drives()
        found = gamefiles.find_dumps(layout, [str(layout.root.parent)], drives=drives, report=report)
    except Exception:
        found, failed = [], True
    else:
        failed = False
    messages.put(('dump_search', (found, report, failed, time.monotonic() - started)))


def prepare_install(ctx, choices):
    dump, folder = choices.get('dump'), choices.get('destination')
    if not dump or not folder:
        return 'choose_source'
    if not install_dump(ctx, {'file': dump}):
        return 'invalid_dump'
    destination = Path(folder)
    if not destination.is_absolute() or (destination.exists() and not destination.is_dir()):
        return 'invalid_destination'
    space = install_space(folder)
    if space['path_too_long']:
        return 'path_too_long'
    saved = ctx.settings.get('game_dir')
    if build.installed(folder) or (saved and saved != folder and build.installed(saved)):
        return 'already_installed'
    if saves.game_running(folder) or (saved and saved != folder and saves.game_running(saved)):
        return 'game_running'
    complete_image = space['complete_image']
    if not complete_image and space['available_gb'] < paths.INSTALL_SPACE_GB:
        return 'need_space'
    paths.drop_partial_image(folder)
    if not complete_image and paths.free_gb(folder) < paths.INSTALL_SPACE_GB:
        return 'need_space'
    return 'ready'


def defer_crash_offer(ctx, run, end):
    if not run.kind:
        return
    source = installed_crash_offer(ctx)
    if source:
        try:
            source['game_ended'](ctx.settings, run.game_dir, run.kind, run.asked, end.message, ctx.log)
        except Exception:
            ctx.log('worker: crash offer unavailable')


def main(argv=None):
    global _output
    _output = sys.stdout.buffer
    sys.stdout = sys.stderr
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', required=True)
    parser.add_argument('--parent-pid', type=int)
    parser.add_argument('--app-version')
    args = parser.parse_args(argv)
    try:
        request = read_request()
    except (ValueError, UnicodeError, RecursionError):
        error(None, 'invalid_request')
        return 1
    if request is None:
        return 0
    if request.get('method') != 'hello':
        error(request['id'], 'invalid_request')
        return 1
    if type(request.get('protocol')) is not int or request['protocol'] != PROTOCOL:
        error(request['id'], 'protocol_mismatch')
        return 1
    try:
        root = Path(args.root).resolve()
    except (OSError, RuntimeError):
        error(request['id'], 'invalid_root')
        return 1
    if not root.is_dir() or not (root / 'app' / 'current.txt').is_file() or not (root / 'data').is_dir():
        error(request['id'], 'invalid_root')
        return 1
    try:
        valid = inspect_installation(root)['status'] != 'inspection_error'
    except (OSError, ValueError, RuntimeError):
        valid = False
    if not valid:
        error(request['id'], 'invalid_installation')
        return 1
    layout = Layout(root=root)
    parent = None
    if args.parent_pid is not None:
        if os.name == 'nt':
            parent = winsys.LauncherParent.open(args.parent_pid)
        if not parent:
            error(request['id'], 'invalid_parent')
            return 1
    try:
        held = locks.FileLock(layout.instance_lock, 'the launcher worker', where=layout.root)
    except locks.Busy:
        if parent:
            parent.close()
        error(request['id'], 'busy')
        return 1
    ctx = reports = run = steam_work = appearance_work = None
    try:
        if args.app_version:
            try:
                versions.parse(args.app_version)
                active = (layout.apps / args.app_version).resolve()
                if (args.app_version != updater.current(layout) or active.parent != layout.apps.resolve()
                        or not (active / 'tools' / 'make_game.py').is_file()):
                    raise ValueError('active version changed or is incomplete')
                layout = Layout(root=root, app=active)
            except (OSError, ValueError, RuntimeError):
                error(request['id'], 'runtime_incompatible')
                return 1
        try:
            ctx = Context(layout, electron_worker=True)
            waiting = bool(updater.staged(layout, channel=ctx.settings.get('channel', 'stable'),
                                          skip=ctx.settings.get('rolled_back', '')))
        except Exception:
            if ctx:
                ctx.log('worker: startup failed')
            error(request['id'], 'startup_failed')
            return 1
        lock_state = 'held' if held.key is not None else 'unavailable'
        ctx.log(f'worker: started version={ctx.version} protocol={PROTOCOL} lock={lock_state} update_waiting={waiting}')
        ctx.log(f'worker: runtime={sys.executable} python={sys.version.split()[0]} app={layout.app}')
        capabilities = ['inspect', 'play', 'install', 'rebuild', 'recovery', 'check_updates',
                        'saves', 'mods', 'settings', 'reports', 'steam', 'language', 'uninstall']
        try:
            from . import worker_appearance
        except (ImportError, OSError):
            worker_appearance = None
            ctx.log('worker: appearance unavailable')
        else:
            capabilities.append('appearance')
        emit({'id': request['id'], 'result': {'protocol': PROTOCOL, 'version': ctx.version, 'lock': lock_state,
                                              'capabilities': capabilities, 'update_waiting': waiting}})
        counts = {}
        reports = Reports(ctx) if lock_state == 'held' else None
        requests = queue.Queue(maxsize=64)
        input_closed = threading.Event()
        threading.Thread(target=read_requests, args=(requests, input_closed), daemon=True).start()
        run = None
        window_log = None
        job = None
        job_revision = -1
        job_action = None
        failed_mods = []
        install_choices = {}
        dump_search = 'idle'
        session = 0
        attempt = 0
        pending = None
        sequence = 0
        disconnected = False
        shutdown_status = 0
        recovery = None
        recovery_poll = None
        stopping = None
        check = None
        check_started = None
        check_started_at = 0.0
        check_channel = None
        home_check_started = False
        steam_result = {'status': 'idle'}
        background_image = None
        appearance_source = None
        appearance_bytes = {}
        while True:
            if steam_work and steam_work.done():
                steam_result = steam_work.result()
                steam_work = None
                action_log(ctx, counts, 'steam', f'add result={steam_result["status"]}')
            if reports:
                reports.poll(game_active=run is not None)
                if not disconnected:
                    reports.tick(ctx.settings.get('game_dir'))
            if _output_disconnected:
                disconnected = True
                if job and not job.cancelled():
                    job.cancel()
                if not run and not job:
                    ctx.log('worker: output disconnected idle')
                    return 0
            if check and check.finished is not None and not disconnected:
                update = checked_update(ctx, check, counts, check_channel)
                check = None
                sequence += 1
                emit({'event': 'update_checked', 'sequence': sequence, 'session': 0, 'update': update})
            if recovery_poll is not None and time.monotonic() >= recovery_poll and not disconnected:
                previous = recovery
                recovery = inspect_recovery(ctx, counts, recovery)
                if recovery and recovery.state == 'alive' and stopping and recovery.identity == stopping[0]:
                    if recovery.identity['kind'] == 'linux_group' and time.monotonic() >= stopping[1]:
                        try:
                            build_recovery.signal_linux_orphan(recovery.identity, signal.SIGKILL)
                        except OSError:
                            action_log(ctx, counts, 'recovery', 'stop escalation_failed')
                if not recovery or recovery.state == 'interrupted':
                    recovery_poll = stopping = None
                else:
                    recovery_poll = time.monotonic() + 0.5
                if recovery != previous:
                    sequence += 1
                    emit({'event': 'recovery_changed', 'sequence': sequence, 'session': 0,
                          'inspection': installation_view(ctx, recovery, counts)})
            if job and job.finished is None and not job.cancelled() and job.steps and job.rev != job_revision and not disconnected:
                with job.locked():
                    job_revision = job.rev
                    fraction = job.overall()
                    step = job.current().id
                sequence += 1
                emit({'event': 'job_progress', 'sequence': sequence, 'session': session,
                      'fraction': fraction, 'step': step})
            if job and job.finished is not None:
                winsys.keep_awake(False, ctx.log)
                if job_action == 'play':
                    outcome, failed_mods = mods_job.finish_mods_job(ctx, job, failed_mods, require_finished=True)
                else:
                    outcome = 'complete' if job.result == 'ok' else job.result
                    if outcome == 'complete':
                        failed_mods = []
                        if not ctx.settings.get('keep_cache'):
                            try:
                                mods_job.drop_build_cache(ctx)
                            except OSError:
                                action_log(ctx, counts, 'job', 'cache_cleanup_failed')
                ctx.log(f'job {job.kind}: {job.result} {job.error}')
                action_log(ctx, counts, 'job', f'result={job.result}')
                result = job.result
                job_game_dir = job.game_dir
                job = None
                if disconnected and not run:
                    return shutdown_status
                if not disconnected:
                    sequence += 1
                    emit({'event': 'job_ended', 'sequence': sequence, 'session': session, 'result': result,
                          'outcome': outcome})
                    if _output_disconnected or input_closed.is_set():
                        return shutdown_status
                    if parent and parent.exited():
                        ctx.log('worker: parent exit after job')
                        return shutdown_status
                    if outcome == 'resume':
                        prepared = prepare_worker_play(ctx, job_game_dir, run, failed_mods, built=True)
                        action_log(ctx, counts, 'play', f'status={prepared.status}')
                        if prepared.status == 'memory':
                            attempt += 1
                            pending = (attempt, prepared, job_game_dir)
                            memory = prepared.memory
                            sequence += 1
                            emit({'event': 'memory_required', 'sequence': sequence, 'session': session,
                                  'decision': {'status': 'memory', 'attempt': attempt,
                                               'memory': {'remaining': memory.remaining, 'need': memory.need,
                                                          'unbounded': memory.unbounded}}})
                        elif prepared.status == 'ready':
                            window_log = GameWindowLog(job_game_dir)
                            run, failure = start_worker_play(ctx, prepared, job_game_dir)
                            if failure:
                                action_log(ctx, counts, 'spawn', 'failed')
                                sequence += 1
                                emit({'event': 'play_failed', 'sequence': sequence, 'session': session,
                                      'reason': 'start_failed'})
                            else:
                                action_log(ctx, counts, 'spawn', 'started')
                                session += 1
                                sequence += 1
                                emit({'event': 'game_started', 'sequence': sequence, 'session': session})
                        else:
                            sequence += 1
                            emit({'event': 'play_failed', 'sequence': sequence, 'session': session,
                                  'reason': prepared.status})
            running = False
            if run:
                restarted = run.restarted
                running = run.running()
                if run.restarted and not restarted:
                    ctx.log('worker: game restart observed')
            if run and not running:
                end = game_session.finish_game(run, ctx.log)
                defer_crash_offer(ctx, run, end)
                if reports:
                    reports.send_session(run.game_dir)
                code = run.code()
                run = None
                window_log = None
                waiting = bool(updater.staged(layout, channel=ctx.settings.get('channel', 'stable'),
                                              skip=ctx.settings.get('rolled_back', '')))
                ctx.log(f'worker: game ended update_waiting={waiting}')
                if disconnected:
                    return shutdown_status
                sequence += 1
                emit({'event': 'game_ended', 'sequence': sequence, 'session': session,
                      'update_waiting': waiting, 'code': code, 'alarm': end.alarm, 'message': end.message})
                if _output_disconnected:
                    continue
            elif run and window_log and not disconnected and window_log.focused():
                window_log = None
                ctx.log('worker: game window focused')
                sequence += 1
                emit({'event': 'game_window_ready', 'sequence': sequence, 'session': session})
            try:
                report_active = reports and (reports.busy or reports.deadline is not None)
                timeout = (0.2 if parent and not disconnected or report_active else
                           0.5 if recovery_poll is not None or check else 1 if run or job else None)
                kind, request = requests.get(timeout=timeout)
            except queue.Empty:
                if disconnected or not parent or not parent.exited():
                    continue
                kind, request = 'parent exit', None
            if disconnected:
                continue
            if kind == 'dump_search':
                found, report, failed, seconds = request
                dump_search = 'complete'
                if failed:
                    result = 'failed'
                elif install_choices.get('dump'):
                    result = 'kept'
                elif found:
                    install_choices['dump'] = str(found[0])
                    result = 'found'
                else:
                    result = 'none'
                action_log(ctx, counts, 'install',
                           f'dump_search result={result} count={len(found)} folders={report.get("folders", 0)} '
                           f'seconds={seconds:.2f} stopped={bool(report.get("stopped"))}')
                continue
            if kind == 'invalid':
                error(None, 'invalid_request')
                ctx.log('worker: invalid request')
                if run or job:
                    disconnected = True
                    if job and not job.cancelled():
                        job.cancel()
                    shutdown_status = 1
                    continue
                return 1
            if request is None:
                reason = 'parent exit' if kind == 'parent exit' else 'eof'
                if run or job:
                    ctx.log(f'worker: {reason} active')
                    if job and not job.cancelled():
                        job.cancel()
                    if run:
                        ctx.log(f'worker: {reason} game running')
                    disconnected = True
                    continue
                ctx.log(f'worker: {reason} idle')
                return 0
            if request.get('method') == 'cancel_job':
                if not job:
                    error(request['id'], 'no_active_job')
                else:
                    job.cancel()
                    action_log(ctx, counts, 'job', 'cancelling')
                    emit({'id': request['id'], 'result': {'status': 'cancelling'}})
                continue
            if request.get('method') in ('play', 'memory_response'):
                if job or build_recovery.blocked(layout):
                    action_log(ctx, counts, 'play', 'refused status=job_running')
                    error(request['id'], 'job_running')
                    continue
                game_dir = ctx.settings.get('game_dir')
                if request['method'] == 'play':
                    attempt += 1
                    pending = None
                    prepared = prepare_worker_play(ctx, game_dir, run, failed_mods)
                    action_log(ctx, counts, 'play', f'status={prepared.status}')
                    if prepared.status == 'rebuild':
                        job = jobs.ModsRebuildJob(ctx, game_dir)
                        job_action = 'play'
                        job_revision = -1
                        job.waiting = prepared.waiting
                        winsys.keep_awake(True, ctx.log)
                        job.start()
                        session += 1
                        action_log(ctx, counts, 'job', 'started')
                        emit({'id': request['id'], 'result': {'status': 'building'}})
                        sequence += 1
                        emit({'event': 'job_started', 'sequence': sequence, 'session': session})
                        continue
                    if prepared.status == 'memory':
                        pending = (attempt, prepared, game_dir)
                        memory = prepared.memory
                        emit({'id': request['id'], 'result': {'status': 'memory', 'attempt': attempt,
                              'memory': {'remaining': memory.remaining, 'need': memory.need,
                                         'unbounded': memory.unbounded}}})
                        continue
                else:
                    if (not pending or type(request.get('attempt')) is not int or
                            request['attempt'] != pending[0] or
                            not ((type(request.get('accept')) is bool and 'dialog_error' not in request) or
                                 (request.get('dialog_error') is True and 'accept' not in request))):
                        action_log(ctx, counts, 'memory', 'invalid decision')
                        error(request['id'], 'stale_or_invalid_decision')
                        continue
                    _, prepared, game_dir = pending
                    pending = None
                    accepted = (commit.play_memory_response(ctx.log, error=OSError('dialog unavailable'))
                                if request.get('dialog_error') is True else
                                commit.play_memory_response(ctx.log, play=request['accept']))
                    action_log(ctx, counts, 'memory', 'accepted' if accepted else 'declined')
                    if not accepted:
                        play.start(ctx, prepared, game_dir, False)
                        emit({'id': request['id'], 'result': {'status': 'declined'}})
                        continue
                    if run and run.alive():
                        play.not_started(ctx.log, 'this launcher is already watching a game', prepared.searched)
                        emit({'id': request['id'], 'result': {'status': 'own_game'}})
                        continue
                    if saves.game_running(game_dir):
                        play.not_started(ctx.log, 'the game is already running', prepared.searched)
                        emit({'id': request['id'], 'result': {'status': 'external_game'}})
                        continue
                    rebuild = pending_rebuild(ctx, game_dir)
                    if rebuild and 'build_version' not in rebuild:
                        action_log(ctx, counts, 'play', 'status=rebuild_required')
                        emit({'id': request['id'], 'result': {'status': 'rebuild_required'}})
                        continue
                    prepared.status = 'ready'
                if prepared.status == 'ready':
                    window_log = GameWindowLog(game_dir)
                    run, failure = start_worker_play(ctx, prepared, game_dir)
                    if failure:
                        action_log(ctx, counts, 'spawn', 'failed')
                        error(request['id'], 'start_failed')
                    else:
                        action_log(ctx, counts, 'spawn', 'started')
                        session += 1
                        emit({'id': request['id'], 'result': {'status': 'started'}})
                        sequence += 1
                        emit({'event': 'game_started', 'sequence': sequence, 'session': session})
                else:
                    emit({'id': request['id'], 'result': {'status': prepared.status}})
            elif request.get('method') in ('install_destination', 'install_dump'):
                choosing_dump = request['method'] == 'install_dump'
                subject = 'dump' if choosing_dump else 'destination'
                propose = install_dump if choosing_dump else install_destination
                try:
                    saved = ctx.settings.get('game_dir')
                    unavailable = run or job or pending or (saved and build.installed(saved))
                    proposal = None if unavailable else propose(ctx, request)
                except (OSError, ValueError, RuntimeError, TypeError, AttributeError):
                    proposal = None
                if proposal:
                    install_choices[subject] = proposal['file' if choosing_dump else 'folder']
                    detail = f'{subject} {proposal["source"]}'
                    if not choosing_dump:
                        detail += (f' available_gb={proposal["available_gb"]} required_gb={proposal["required_gb"]}'
                                   f' complete_image={proposal["complete_image"]} path_too_long={proposal["path_too_long"]}')
                    action_log(ctx, counts, 'install', detail)
                    emit({'id': request['id'], 'result': proposal})
                else:
                    action_log(ctx, counts, 'install', f'{subject} rejected')
                    error(request['id'], f'invalid_{subject}')
            elif request.get('method') == 'search_install_dump':
                saved = ctx.settings.get('game_dir')
                try:
                    unavailable = run or job or pending or (saved and build.installed(saved))
                except (OSError, ValueError, TypeError, AttributeError):
                    unavailable = True
                if unavailable:
                    error(request['id'], 'invalid_dump')
                    continue
                if dump_search == 'idle':
                    dump_search = 'searching'
                    threading.Thread(target=search_install_dump, args=(ctx.layout, requests), daemon=True).start()
                emit({'id': request['id'], 'result': {'status': dump_search, 'file': install_choices.get('dump')
                                                      if dump_search == 'complete' else None}})
            elif request.get('method') in ('install', 'rebuild'):
                action = request['method']
                selected_job = None
                status = 'unavailable'
                try:
                    if lock_state == 'held' and not (run or job or pending or build_recovery.blocked(layout)):
                        if action == 'install':
                            status = prepare_install(ctx, install_choices)
                            if status == 'ready':
                                dump, folder = install_choices['dump'], install_choices['destination']
                                selected_job = jobs.InstallJob(ctx, dump, folder)
                                ctx.settings.save_install_choices(dump, folder)
                                if ctx.settings.get('desktop_shortcut'):
                                    winsys.desktop_shortcut(layout, True)
                        else:
                            folder = ctx.settings.get('game_dir')
                            if not build.installed(folder):
                                status = 'not_built'
                            elif saves.game_running(folder):
                                status = 'game_running'
                            else:
                                selected_job = jobs.RebuildJob(ctx, folder, ctx.settings.get('dump'))
                                status = 'ready'
                except (OSError, ValueError, RuntimeError, TypeError, AttributeError):
                    status = 'unavailable'
                if status != 'ready':
                    action_log(ctx, counts, action, f'refused status={status}')
                    emit({'id': request['id'], 'result': {'status': status}})
                    continue
                recovery = None
                job = selected_job
                job_action = action
                job_revision = -1
                winsys.keep_awake(True, ctx.log)
                job.start()
                session += 1
                action_log(ctx, counts, action, 'started')
                emit({'id': request['id'], 'result': {'status': 'building'}})
                sequence += 1
                emit({'event': 'job_started', 'sequence': sequence, 'session': session})
            elif request.get('method') == 'first_run_preference':
                key, value = request.get('key'), request.get('value')
                valid = (key == 'channel' and value in ('stable', 'preview')
                         or key == 'auto_check' and type(value) is bool
                         or key == 'desktop_shortcut' and os.name == 'nt' and type(value) is bool)
                status = 'unavailable'
                if valid:
                    try:
                        folder = ctx.settings.get('game_dir')
                        if (lock_state == 'held' and not (job or run or pending or build_recovery.blocked(layout))
                                and not build.installed(folder)):
                            ctx.settings.save_preference(key, value)
                            status = 'saved'
                            if key == 'desktop_shortcut':
                                shortcut = winsys.desktop_shortcut(layout, value)
                                action_log(ctx, counts, 'settings',
                                           'first_run desktop_shortcut result=' + ('updated' if shortcut else 'unavailable'))
                    except (OSError, ValueError, RuntimeError, TypeError, AttributeError):
                        status = 'save_failed'
                    action_log(ctx, counts, 'settings', f'first_run key={key} result={status}')
                else:
                    action_log(ctx, counts, 'settings', 'first_run invalid')
                if status == 'saved':
                    emit({'id': request['id'], 'result': {'key': key, 'value': value}})
                else:
                    error(request['id'], 'preference_unavailable')
            elif request.get('method') == 'set_hide_while_playing':
                value = request.get('value')
                if type(value) is not bool:
                    error(request['id'], 'invalid_preference')
                elif lock_state != 'held' or run or job or pending or build_recovery.blocked(layout):
                    error(request['id'], 'preference_unavailable')
                else:
                    try:
                        ctx.settings.save_hide_while_playing(value)
                    except (OSError, ValueError):
                        action_log(ctx, counts, 'settings', 'hide save_failed')
                        error(request['id'], 'preference_save_failed')
                    else:
                        emit({'id': request['id'], 'result': {'hide_while_playing': value}})
            elif request.get('method') in ('saves_info', 'save_preference', 'export_save', 'import_save', 'open_saves_dir'):
                method = request['method']
                game = ctx.settings.get('game_dir')
                if not game or not Path(game).is_dir():
                    error(request['id'], 'saves_unavailable')
                    continue
                if method == 'saves_info':
                    try:
                        result = {'destination': str(saves.export_destination(ctx.settings)),
                                  'with_log': ctx.settings['export_with_log'],
                                  'waiting': saves.waiting(game)[0] is not None,
                                  'pending': saves.pending_import(game) is not None}
                    except OSError:
                        error(request['id'], 'saves_unavailable')
                        continue
                elif lock_state != 'held' or job or build_recovery.blocked(layout):
                    action_log(ctx, counts, 'saves', 'request refused')
                    error(request['id'], 'saves_unavailable')
                    continue
                elif method == 'save_preference':
                    key, value = request.get('key'), request.get('value')
                    valid = (key == 'export_with_log' and type(value) is bool or
                             key == 'save_export_dir' and isinstance(value, str) and '\0' not in value and
                             Path(value).is_absolute() and Path(value).is_dir())
                    if not valid:
                        action_log(ctx, counts, 'saves', 'preference rejected')
                        error(request['id'], 'invalid_preference')
                        continue
                    try:
                        ctx.settings.save_preference(key, value)
                    except (OSError, ValueError):
                        action_log(ctx, counts, 'saves', 'preference save_failed')
                        error(request['id'], 'preference_save_failed')
                        continue
                    action_log(ctx, counts, 'saves', f'preference key={key} result=saved')
                    result = {'key': key, 'value': value}
                elif method == 'export_save':
                    result = export_save(ctx, game, counts)
                elif method == 'import_save':
                    result = import_save(ctx, game, run, counts, request)
                else:
                    try:
                        winsys.open_path(saves.ensure_saves_dir(game))
                    except OSError:
                        result = {'status': 'failed'}
                    else:
                        result = {'status': 'ok'}
                    action_log(ctx, counts, 'saves', f'open result={result["status"]}')
                emit({'id': request['id'], 'result': result})
            elif request.get('method') in ('mods_list', 'mod_add', 'mod_enabled', 'mod_move', 'open_mods_dir'):
                method = request['method']
                game = ctx.settings.get('game_dir')
                if not game or not Path(game).is_dir():
                    error(request['id'], 'mods_unavailable')
                    continue
                if method == 'mods_list':
                    page = request.get('page', 0)
                    if type(page) is not int or page < 0:
                        error(request['id'], 'invalid_page')
                        continue
                    try:
                        result = mods_view(game, page)
                    except OSError:
                        error(request['id'], 'mods_unavailable')
                        continue
                    action_log(ctx, counts, 'mods list', f'count={result["count"]} page={result["page"]}')
                elif lock_state != 'held' or job or build_recovery.blocked(layout):
                    action_log(ctx, counts, 'mods', 'request refused')
                    error(request['id'], 'mods_unavailable')
                    continue
                else:
                    result = change_mod(ctx, game, run, counts, request)
                emit({'id': request['id'], 'result': result})
            elif request.get('method') in ('language_info', 'language_preference'):
                if request['method'] == 'language_info':
                    value = worker_preferences.language(layout)
                    action_log(ctx, counts, 'language', f'choice={value} read')
                else:
                    value = request.get('value')
                    if value not in worker_preferences.LANGUAGES:
                        error(request['id'], 'invalid_preference')
                        continue
                    if lock_state != 'held' or job or pending or build_recovery.blocked(layout):
                        error(request['id'], 'language_unavailable')
                        continue
                    try:
                        worker_preferences.set_language(layout, value)
                    except (OSError, ValueError):
                        action_log(ctx, counts, 'language', 'result=failed')
                        error(request['id'], 'language_unavailable')
                        continue
                    action_log(ctx, counts, 'language', f'choice={value} result=saved')
                emit({'id': request['id'], 'result': {'locale': value}})
            elif request.get('method') in ('appearance_info', 'appearance_preference',
                                               'appearance_background', 'appearance_image', 'appearance_assets', 'appearance_asset'):
                method = request['method']
                if method in ('appearance_preference', 'appearance_background') and (
                        lock_state != 'held' or job or pending or build_recovery.blocked(layout)):
                    error(request['id'], 'appearance_unavailable')
                    continue
                if worker_appearance is None:
                    error(request['id'], 'appearance_unavailable')
                    continue
                try:
                    if method == 'appearance_info':
                        result = worker_appearance.view(ctx.settings)
                        action_log(ctx, counts, 'appearance', f'theme={result["theme"]} read')
                    elif method == 'appearance_assets':
                        if appearance_work and appearance_work.done():
                            appearance_work = None
                        source = ctx.settings['game_dir'], ctx.settings['dump']
                        if (not appearance_work and appearance_source != source and any(source) and lock_state == 'held'
                                and not (job or pending or build_recovery.blocked(layout))):
                            appearance_source = source
                            appearance_work = Future()
                            threading.Thread(target=worker_appearance.extract_assets,
                                             args=(ctx, *source, appearance_work), daemon=True).start()
                        result = {'status': 'loading'} if appearance_work else worker_appearance.asset_manifest(layout)
                    elif method == 'appearance_asset':
                        asset, offset = request.get('asset'), request.get('offset')
                        if asset not in ('title', 'select', 'back') or type(offset) is not int or offset < 0:
                            error(request['id'], 'invalid_asset')
                            continue
                        if offset == 0:
                            appearance_bytes[asset] = worker_appearance.asset_bytes(layout, asset)
                        result = preview_frame(appearance_bytes.get(asset), offset)
                    elif method == 'appearance_image':
                        offset = request.get('offset')
                        if type(offset) is not int or offset < 0:
                            error(request['id'], 'invalid_offset')
                            continue
                        if offset == 0:
                            background_image = worker_appearance.saved_background(layout, ctx.settings)
                            if ctx.settings['background']:
                                action_log(ctx, counts, 'appearance', 'background read=' +
                                           ('ready' if background_image else 'unavailable'))
                        result = preview_frame(background_image, offset)
                    elif method == 'appearance_background':
                        result = (worker_appearance.change_background(layout, ctx.settings, request['path'])
                                  if 'path' in request else {'status': 'invalid_picture'})
                        background_image = None
                        action_log(ctx, counts, 'appearance background', f'result={result["status"]}')
                    else:
                        key = request.get('key')
                        result = worker_appearance.preference(ctx.settings, key, request.get('value'))
                        action_log(ctx, counts, f'appearance key={key}', 'result=saved')
                except (ValueError, OSError, ImportError) as failure:
                    if method == 'appearance_background':
                        action_log(ctx, counts, 'appearance background', 'result=failed')
                        result = {'status': 'failed'}
                    else:
                        invalid = method == 'appearance_preference' and isinstance(failure, ValueError)
                        action_log(ctx, counts, 'appearance', 'invalid preference' if invalid else 'unavailable')
                        error(request['id'], 'invalid_preference' if invalid else 'appearance_unavailable')
                        continue
                emit({'id': request['id'], 'result': result})
            elif request.get('method') in ('steam_info', 'steam_add', 'steam_result'):
                method = request['method']
                if method == 'steam_info':
                    result = steam_view()
                    action_log(ctx, counts, 'steam', f'detection={result["status"]}')
                elif method == 'steam_result':
                    result = {'status': 'adding'} if steam_work else steam_result
                elif steam_work:
                    result = {'status': 'adding'}
                elif lock_state != 'held' or job or pending or build_recovery.blocked(layout):
                    result = {'status': 'unavailable'}
                else:
                    game = ctx.settings.get('game_dir')
                    status = steam_view()['status']
                    if status not in ('available', 'added'):
                        result = {'status': status}
                    elif not game or not steam.game_executable(game).is_file():
                        result = {'status': 'not_built'}
                    else:
                        steam_work = Future()
                        threading.Thread(target=add_to_steam,
                                         args=(ctx, game, ctx.settings.get('dump'), steam_work), daemon=True).start()
                        result = {'status': 'adding'}
                    action_log(ctx, counts, 'steam', f'add request={result["status"]}')
                emit({'id': request['id'], 'result': result})
            elif request.get('method') == 'settings_udmabuf':
                action, confirmed = request.get('action'), request.get('confirmed')
                if action not in ('apply', 'remove') or type(confirmed) is not bool:
                    error(request['id'], 'invalid_preference')
                    continue
                game = ctx.settings['game_dir']
                if (lock_state != 'held' or run or job or pending or build_recovery.blocked(layout)
                        or (game and saves.game_running(game))):
                    status = 'busy'
                else:
                    status = change_udmabuf(ctx, action, confirmed)
                action_log(ctx, counts, 'settings udmabuf', f'action={action} result={status}')
                emit({'id': request['id'], 'result': {'status': status, 'action': udmabuf_action(ctx)}})
            elif request.get('method') == 'uninstall':
                remove = request.get('remove_game')
                if type(remove) is not bool:
                    error(request['id'], 'invalid_uninstall_request')
                    continue
                game = ctx.settings['game_dir']
                if (lock_state != 'held' or run or job or pending or check or steam_work
                        or (appearance_work and not appearance_work.done()) or (reports and reports.busy) or build_recovery.blocked(layout)
                        or (game and saves.game_running(game))):
                    status = 'busy'
                elif not winsys.uninstall_available(layout) or ctx.settings.unreadable:
                    status = 'unsupported'
                elif remove and (not game or winsys.unsafe_uninstall_game(layout, game)
                                 or (ctx.settings['dump'] and winsys.contains_path(game, ctx.settings['dump']))):
                    status = 'unsafe_game'
                else:
                    keep = [ctx.settings['dump'], '' if remove else game]
                    try:
                        winsys.uninstall(layout, game if remove else '', ctx.log,
                                         keep=keep, protect_windows_paths=True)
                    except (OSError, ValueError):
                        status = 'failed'
                    else:
                        status = 'scheduled'
                action_log(ctx, counts, 'uninstall', f'remove_game={remove} result={status}')
                emit({'id': request['id'], 'result': {'status': status}})
                if status == 'scheduled':
                    return 0
            elif request.get('method') in ('settings_info', 'settings_preference', 'settings_dump', 'settings_open', 'settings_discord'):
                method = request['method']
                if method == 'settings_info':
                    emit({'id': request['id'], 'result': installed_settings(ctx)})
                    continue
                if method == 'settings_open':
                    target = request.get('target')
                    result, refusal = open_settings_folder(ctx, target)
                    action = f'open target={target}' if target in ('game', 'logs') else 'open'
                elif lock_state != 'held' or run or job or pending or build_recovery.blocked(layout):
                    action, result, refusal = 'change', None, 'settings_unavailable'
                elif method == 'settings_discord':
                    value = request.get('value')
                    result, refusal = write_discord_setting(ctx, value)
                    action = f'discord enabled={value}' if type(value) is bool else 'discord'
                elif method == 'settings_dump':
                    action = 'dump'
                    result, refusal = write_installed_dump(ctx, request)
                else:
                    result, refusal = write_installed_setting(ctx, request)
                    action = f'key={result["key"]}' if result else 'preference'
                status = refusal or ('ok' if method == 'settings_open' else 'saved')
                action_log(ctx, counts, f'settings {action}', f'result={status}')
                if refusal == 'invalid_dump':
                    emit({'id': request['id'], 'result': {'status': refusal}})
                elif refusal:
                    error(request['id'], refusal)
                else:
                    emit({'id': request['id'], 'result': result})
            elif request.get('method') in ('reports_home', 'reports_status', 'reports_touch', 'reports_answer',
                                               'reports_close', 'reports_consent', 'reports_preview',
                                               'reports_comment', 'reports_comment_status', 'reports_crash_open',
                                               'reports_crash_preview', 'reports_crash_answer', 'reports_crash_status',
                                               'reports_crash_preference'):
                method = request['method']
                game = ctx.settings.get('game_dir')
                if not reports or not game or not build.installed(game):
                    error(request['id'], 'reports_unavailable')
                    continue
                try:
                    if method == 'reports_status':
                        result = reports.view()
                    elif not reports.available:
                        result = reports.view() if method == 'reports_home' else None
                    elif job or run or pending or build_recovery.blocked(layout):
                        result = None
                    elif method == 'reports_home':
                        after = request.get('after')
                        if after not in ('', 'installation', 'update'):
                            error(request['id'], 'invalid_report_request')
                            continue
                        reports.home(game, after)
                        result = reports.view()
                    elif method == 'reports_touch':
                        reports.touch()
                        result = reports.view()
                    elif method in ('reports_answer', 'reports_close'):
                        answer = 'closed' if method == 'reports_close' else request.get('answer')
                        if answer not in QUESTION_ANSWERS:
                            error(request['id'], 'invalid_report_request')
                            continue
                        reports.answer(game, answer)
                        if method == 'reports_close':
                            reports.crash.choose('closed', window_closed=True)
                        result = reports.view()
                    elif method == 'reports_consent':
                        value = request.get('value')
                        if type(value) is not bool or reports.question:
                            error(request['id'], 'invalid_report_request')
                            continue
                        reports.set_consent(game, value)
                        result = reports.view()
                    elif method == 'reports_preview':
                        offset = request.get('offset')
                        result = reports.preview(game, offset) if type(offset) is int and offset >= 0 else None
                    elif method == 'reports_crash_open':
                        result = (reports.crash.show(game) if reports.started and not reports.question
                                  and not saves.game_running(game) else {'status': 'none'})
                    elif method == 'reports_crash_preview':
                        offset = request.get('offset')
                        result = reports.crash.preview(offset) if type(offset) is int and offset >= 0 else None
                    elif method == 'reports_crash_answer':
                        answer = request.get('answer')
                        result = (reports.crash.choose(answer)
                                  if answer in ('send', 'decline', 'comment', 'closed') else None)
                    elif method == 'reports_crash_status':
                        result = reports.crash.result
                    elif method == 'reports_crash_preference':
                        value = request.get('value')
                        if type(value) is not bool or reports.crash.current or not reports.crash.source:
                            error(request['id'], 'invalid_report_request')
                            continue
                        reports.crash.set_preference(value)
                        result = reports.view()
                    elif method == 'reports_comment':
                        text, attach = request.get('text'), request.get('attach')
                        if not isinstance(text, str) or len(text) > telemetry.TEXT_LIMIT or type(attach) is not bool:
                            error(request['id'], 'invalid_report_request')
                            continue
                        result = reports.comment(game, text, attach)
                    else:
                        result = reports.comment_status()
                except (OSError, ValueError):
                    ctx.log('telemetry: Electron reports request failed')
                    error(request['id'], 'reports_unavailable')
                    continue
                if result is None:
                    error(request['id'], 'reports_unavailable')
                else:
                    emit({'id': request['id'], 'result': result})
            elif request.get('method') == 'check_updates':
                automatic = request.get('automatic')
                if automatic not in (None, 'home', 'installed', 'minute'):
                    error(request['id'], 'invalid_request')
                    continue
                now = time.time()
                last = max(ctx.settings['last_check'], check_started_at)
                game = ctx.settings['game_dir']
                if lock_state != 'held' or job or build_recovery.blocked(layout):
                    status = 'unavailable'
                elif automatic and (not ctx.settings['auto_check'] or not build.installed(game)):
                    status = 'idle'
                elif check:
                    status = 'checking'
                elif automatic in ('home', 'installed') and home_check_started:
                    status = 'idle'
                elif (automatic == 'home' and waiting and not layout.dev and ctx.settings['auto_install']
                      and not build.game_running(game)):
                    home_check_started = True
                    status = 'idle'
                elif automatic == 'minute' and now - last < jobs.RECHECK:
                    status = 'idle'
                elif not automatic and check_started is not None and time.monotonic() - check_started < jobs.FORCED_CHECK_PAUSE:
                    status = 'recent_check'
                else:
                    if automatic in ('home', 'installed'):
                        home_check_started = True
                    check_started = time.monotonic()
                    check_started_at = now
                    check_channel = ctx.settings['channel']
                    action_log(ctx, counts, 'check', f'started automatic={automatic}' if automatic else 'started')
                    check = jobs.CheckJob(ctx).start()
                    status = 'checking'
                emit({'id': request['id'], 'result': {'status': status}})
            elif request.get('method') == 'recover_build':
                action = request.get('action')
                if lock_state != 'held' or run or job or pending or action not in ('wait', 'stop', 'confirm'):
                    error(request['id'], 'recovery_unavailable')
                    continue
                previous = recovery
                recovery = inspect_recovery(ctx, counts, recovery)
                if action == 'confirm' and recovery and recovery.state != 'interrupted':
                    if (recovery.state != 'unverified' or not previous or not previous.fingerprint or
                            previous.state != 'unverified' or previous.fingerprint != recovery.fingerprint):
                        action_log(ctx, counts, 'recovery', 'confirmation refused')
                        error(request['id'], 'recovery_unavailable')
                        continue
                    recovery = finish_recovery(ctx, recovery, 'player_confirmed')
                    action_log(ctx, counts, 'recovery', f'confirm state={recovery.state} reason={recovery.reason}')
                    if recovery.state != 'interrupted':
                        error(request['id'], 'recovery_unavailable')
                        continue
                if not recovery or recovery.state == 'interrupted':
                    recovery_poll = stopping = None
                    emit({'id': request['id'], 'result': {'status': 'finished', 'inspection': installation_view(ctx, recovery, counts)}})
                    continue
                if action == 'stop':
                    if recovery.state != 'alive':
                        action_log(ctx, counts, 'recovery', 'stop refused')
                        error(request['id'], 'recovery_unavailable')
                        continue
                    try:
                        build_recovery.request_stop(layout, recovery.identity)
                    except (OSError, ValueError):
                        action_log(ctx, counts, 'recovery', 'stop failed')
                        error(request['id'], 'recovery_unavailable')
                        continue
                    stopping = (recovery.identity, time.monotonic() + 5)
                status = 'stopping' if action == 'stop' else 'waiting'
                recovery_poll = time.monotonic() + 0.1
                action_log(ctx, counts, 'recovery', f'{action} result={status}')
                emit({'id': request['id'], 'result': {'status': status}})
            elif request.get('method') == 'inspect':
                try:
                    if not job and lock_state == 'held':
                        recovery = inspect_recovery(ctx, counts, recovery)
                    result = installation_view(ctx, recovery, counts)
                except (OSError, ValueError, RuntimeError):
                    action_log(ctx, counts, 'inspect', 'failed')
                    error(request['id'], 'inspection_failed')
                    continue
                detail = f'status={result["status"]}'
                if result.get('rebuild_required'):
                    detail += ' rebuild_required=True'
                action_log(ctx, counts, 'inspect', detail)
                emit({'id': request['id'], 'result': result})
            else:
                action_log(ctx, counts, 'unknown', 'method')
                error(request['id'], 'unknown_method')
    finally:
        if appearance_work:
            appearance_work.result()
        if steam_work:
            steam_result = steam_work.result()
            action_log(ctx, counts, 'steam', f'add result={steam_result["status"]}')
        if reports and not run:
            reports.flush()
        if ctx and ctx._file:
            ctx._file.close()
        held.release()
        if parent:
            parent.close()


if __name__ == '__main__':
    sys.exit(main())
