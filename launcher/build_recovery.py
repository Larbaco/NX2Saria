import json
import os
import re
import sys
from dataclasses import dataclass


from .jobs import MARKER

MAX_MARKER = 65536
KINDS = ('install', 'rebuild', 'update')


@dataclass(frozen=True)
class Recovery:
    state: str
    reason: str
    kind: str = 'other'
    identity: dict | None = None

    def view(self):
        return {'state': self.state, 'reason': self.reason, 'kind': self.kind}


def blocked(layout):
    try:
        (layout.data / MARKER).stat()
    except FileNotFoundError:
        return False
    except OSError:
        return True
    return True


def complete_record(layout, token):
    path = layout.data / 'build-supervisor' / token / 'result.json'
    try:
        result = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return False
    return isinstance(result, dict) and 'code' in result and (result['code'] is None or type(result['code']) is int)


def linux_status(identity):
    from .platform import linux
    pid = identity['pid']
    if (identity.get('session') != pid or identity.get('group') != pid
            or not isinstance(identity.get('boot'), str)
            or not re.fullmatch(r'[a-f0-9-]{36}', identity['boot'])):
        return 'unverified', 'invalid_identity'
    if linux.boot_identity() != identity['boot']:
        return 'gone', 'boot_changed'
    try:
        leader = linux.process_identity(pid)
    except (FileNotFoundError, ProcessLookupError):
        leader = None
    if leader and leader['start'] != identity['start']:
        return 'gone', 'pid_reused'
    if leader and (leader['session'] != pid or leader['group'] != pid):
        return 'unverified', 'identity_mismatch'
    if (leader and leader['state'] != 'Z') or linux.session_members(pid, exclude=pid):
        return 'alive', 'process_group'
    return 'gone', 'process_group_finished'


def windows_status(layout, identity):
    from .platform import windows
    if identity['kind'] == 'windows_job':
        name = windows.build_job_name(layout.root, identity['token'])
        if name != identity.get('name'):
            return 'unverified', 'invalid_identity'
        return ('alive', 'job_object') if windows.active_build_job(name) else ('gone', 'job_object_finished')
    status = windows.build_process_status(identity['pid'], identity['start'])
    if status == 'alive':
        return 'alive', 'process_identity'
    if status == 'reused':
        return 'gone', 'pid_reused'
    if complete_record(layout, identity['token']):
        return 'gone', 'process_finished'
    return 'unverified', 'no_group_result'


def inspect_marker(layout):
    try:
        with (layout.data / MARKER).open('rb') as marker:
            raw = marker.read(MAX_MARKER + 1)
        if len(raw) > MAX_MARKER:
            return Recovery('unverified', 'invalid_marker')
        data = json.loads(raw.decode('utf-8'))
    except FileNotFoundError:
        return Recovery('none', 'no_marker')
    except OSError:
        return Recovery('unverified', 'unreadable_marker')
    except (ValueError, RecursionError):
        return Recovery('unverified', 'invalid_marker')
    if not isinstance(data, dict):
        return Recovery('unverified', 'invalid_marker')
    kind = data.get('kind') if data.get('kind') in KINDS else 'other'
    identity = data.get('build')
    if identity is None:
        return Recovery('unverified', 'legacy_marker', kind)
    if (not isinstance(identity, dict) or type(identity.get('version')) is not int or identity['version'] != 1
            or not isinstance(identity.get('token'), str) or not re.fullmatch(r'[a-f0-9]{32}', identity['token'])
            or type(identity.get('pid')) is not int or not 0 < identity['pid'] < 2**32
            or type(identity.get('start')) is not int or identity['start'] <= 0):
        return Recovery('unverified', 'invalid_identity', kind)
    try:
        if sys.platform.startswith('linux') and identity.get('kind') == 'linux_group':
            state, reason = linux_status(identity)
        elif os.name == 'nt' and identity.get('kind') in ('windows_job', 'windows_pid'):
            state, reason = windows_status(layout, identity)
        else:
            state, reason = 'unverified', 'invalid_identity'
    except (OSError, ValueError, OverflowError, IndexError):
        state, reason = 'unverified', 'identity_unavailable'
    return Recovery(state, reason, kind, identity)
