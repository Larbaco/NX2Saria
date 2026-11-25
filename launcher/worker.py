import argparse
import json
import os
import queue
import runpy
import sys
import threading
from pathlib import Path

from . import commit, game_session, jobs, locks, mods_job, play, saves, updater, versions, winsys
from .context import Context
from .inspect_installation import inspect_installation
from .paths import Layout

PROTOCOL = 1
MAX_FRAME = 65536
MAX_ACTION_LOGS = 10
_input_buffer = bytearray()
_output = None
_output_disconnected = False
_output_lock = threading.Lock()


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
            ctx = Context(layout)
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
                                              'capabilities': ['inspect', 'play'], 'update_waiting': waiting}})
        counts = {}
        requests = queue.Queue(maxsize=64)
        input_closed = threading.Event()
        threading.Thread(target=read_requests, args=(requests, input_closed), daemon=True).start()
        run = None
        job = None
        job_revision = -1
        failed_mods = []
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
                with job._lock:
                    job_revision = job.rev
                    fraction = job.overall()
                    step = job.current().id
                sequence += 1
                emit({'event': 'job_progress', 'sequence': sequence, 'session': session,
                      'fraction': fraction, 'step': step})
            if job and job.finished is not None:
                winsys.keep_awake(False, ctx.log)
                outcome, failed_mods = mods_job.finish_mods_job(ctx, job, failed_mods, require_finished=True)
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
            if run and not run.running():
                end = game_session.finish_game(run, ctx.log)
                defer_crash_offer(ctx, run, end)
                code = run.code()
                run = None
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
            try:
                timeout = 0.2 if parent and not disconnected else (1 if run or job else None)
                kind, request = requests.get(timeout=timeout)
            except queue.Empty:
                if disconnected or not parent or not parent.exited():
                    continue
                kind, request = 'parent exit', None
            if disconnected:
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
                if job:
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
