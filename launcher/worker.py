import argparse
import json
import sys
from pathlib import Path

from . import locks, updater
from .context import Context
from .inspect_installation import inspect_installation
from .paths import Layout

PROTOCOL = 1
MAX_FRAME = 65536
MAX_ACTION_LOGS = 10


def emit(payload):
    sys.stdout.write(json.dumps(payload, ensure_ascii=True, allow_nan=False) + '\n')
    sys.stdout.flush()


def reject_constant(value):
    raise ValueError('non-finite number')


def read_request():
    raw = sys.stdin.buffer.readline(MAX_FRAME + 1)
    if not raw:
        return None
    if len(raw) > MAX_FRAME or not raw.endswith(b'\n'):
        raise ValueError('invalid frame')
    request = json.loads(raw.decode('utf-8'), parse_constant=reject_constant)
    if not isinstance(request, dict) or type(request.get('id')) not in (int, str):
        raise ValueError('invalid request')
    return request


def error(request_id, code):
    emit({'id': request_id, 'error': {'code': code}})


def action_log(ctx, counts, action, detail):
    count = counts.get(action, 0)
    counts[action] = count + 1
    if count < MAX_ACTION_LOGS:
        ctx.log(f'worker: {action} {detail}')
    elif count == MAX_ACTION_LOGS:
        ctx.log(f'worker: {action} further events suppressed')


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', required=True)
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
    root = Path(args.root).resolve()
    if not root.is_dir() or not (root / 'app' / 'current.txt').is_file() or not (root / 'data').is_dir():
        error(request['id'], 'invalid_root')
        return 1
    if inspect_installation(root)['status'] == 'inspection_error':
        error(request['id'], 'invalid_installation')
        return 1
    layout = Layout(root=root)
    try:
        held = locks.FileLock(layout.instance_lock, 'the launcher worker', where=layout.root)
    except locks.Busy:
        error(request['id'], 'busy')
        return 1
    ctx = None
    try:
        ctx = Context(layout)
        lock_state = 'held' if held.key is not None else 'unavailable'
        waiting = bool(updater.staged(layout, channel=ctx.settings.get('channel', 'stable'),
                                      skip=ctx.settings.get('rolled_back', '')))
        ctx.log(f'worker: started version={ctx.version} protocol={PROTOCOL} lock={lock_state} update_waiting={waiting}')
        emit({'id': request['id'], 'result': {'protocol': PROTOCOL, 'version': ctx.version, 'lock': lock_state,
                                              'capabilities': ['inspect'], 'update_waiting': waiting}})
        counts = {}
        while True:
            try:
                request = read_request()
            except (ValueError, UnicodeError, RecursionError):
                error(None, 'invalid_request')
                ctx.log('worker: invalid request')
                return 1
            if request is None:
                ctx.log('worker: eof idle')
                return 0
            if request.get('method') == 'inspect':
                result = inspect_installation(root)
                action_log(ctx, counts, 'inspect', f'status={result["status"]}')
                emit({'id': request['id'], 'result': result})
            else:
                action_log(ctx, counts, 'unknown', 'method')
                error(request['id'], 'unknown_method')
    finally:
        if ctx and ctx._file:
            ctx._file.close()
        held.release()


if __name__ == '__main__':
    sys.exit(main())
