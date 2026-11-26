import argparse
import json
import os
import queue
import runpy
import sys
import threading
import time
from pathlib import Path

from . import build, commit, game_session, gamefiles, jobs, locks, mods_job, paths, play, saves, updater, versions, winsys
from .build import game_log_path, game_log_paths
from .context import Context
from .inspect_installation import inspect_installation
from .paths import Layout, install_folder

PROTOCOL = 1
MAX_FRAME = 65536
MAX_ACTION_LOGS = 10
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
    layout = ctx.layout
    try:
        active = layout.app
        if layout.dev:
            version = updater.current(layout)
            versions.parse(version)
            active = (layout.apps / version).resolve()
            if active.parent != layout.apps.resolve():
                return
        source = active / 'launcher' / 'crash_offer.py'
        if source.is_file():
            offer = runpy.run_path(str(source), run_name='launcher._installed_crash_offer')
            offer['game_ended'](ctx.settings, run.game_dir, run.kind, run.asked, end.message, ctx.log)
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
    ctx = None
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
        emit({'id': request['id'], 'result': {'protocol': PROTOCOL, 'version': ctx.version, 'lock': lock_state,
                                              'capabilities': ['inspect', 'play', 'install', 'rebuild'], 'update_waiting': waiting}})
        counts = {}
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
        while True:
            if _output_disconnected:
                disconnected = True
                if job and not job.cancelled():
                    job.cancel()
                if not run and not job:
                    ctx.log('worker: output disconnected idle')
                    return 0
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
                        prepared = play.prepare(ctx, ctx.settings, job_game_dir, run, failed_mods, built=True)
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
                            run, failure = play.start(ctx, prepared, job_game_dir)
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
                timeout = 0.2 if parent and not disconnected else (1 if run or job else None)
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
                if job or (layout.data / jobs.MARKER).exists():
                    action_log(ctx, counts, 'play', 'refused status=job_running')
                    error(request['id'], 'job_running')
                    continue
                game_dir = ctx.settings.get('game_dir')
                if request['method'] == 'play':
                    attempt += 1
                    pending = None
                    prepared = play.prepare(ctx, ctx.settings, game_dir, run, failed_mods)
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
                    prepared.status = 'ready'
                if prepared.status == 'ready':
                    window_log = GameWindowLog(game_dir)
                    run, failure = play.start(ctx, prepared, game_dir)
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
                    if lock_state == 'held' and not (run or job or pending or (layout.data / jobs.MARKER).exists()):
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
            elif request.get('method') == 'set_hide_while_playing':
                value = request.get('value')
                if type(value) is not bool:
                    error(request['id'], 'invalid_preference')
                elif lock_state != 'held' or run or job or pending or (layout.data / jobs.MARKER).exists():
                    error(request['id'], 'preference_unavailable')
                else:
                    try:
                        ctx.settings.save_hide_while_playing(value)
                    except (OSError, ValueError):
                        action_log(ctx, counts, 'settings', 'hide save_failed')
                        error(request['id'], 'preference_save_failed')
                    else:
                        emit({'id': request['id'], 'result': {'hide_while_playing': value}})
            elif request.get('method') == 'inspect':
                try:
                    result = inspect_installation(root)
                except (OSError, ValueError, RuntimeError):
                    action_log(ctx, counts, 'inspect', 'failed')
                    error(request['id'], 'inspection_failed')
                    continue
                action_log(ctx, counts, 'inspect', f'status={result["status"]}')
                emit({'id': request['id'], 'result': result})
            else:
                action_log(ctx, counts, 'unknown', 'method')
                error(request['id'], 'unknown_method')
    finally:
        if ctx and ctx._file:
            ctx._file.close()
        held.release()
        if parent:
            parent.close()


if __name__ == '__main__':
    sys.exit(main())
