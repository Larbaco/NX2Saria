import argparse
import json
import os
import sys
from pathlib import Path


def _inspect_installation(root):
    root = Path(root).resolve()
    if not root.is_dir():
        return {'status': 'inspection_error', 'reason': 'root_unavailable', 'root': str(root), 'game_dir': None}
    settings = root / 'data' / 'launcher.json'
    try:
        data = json.loads(settings.read_text(encoding='utf-8-sig'))
    except FileNotFoundError:
        return {'status': 'not_configured', 'root': str(root), 'game_dir': None}
    except (OSError, ValueError, UnicodeError):
        return {'status': 'inspection_error', 'reason': 'settings_unreadable', 'root': str(root), 'game_dir': None}
    if not isinstance(data, dict):
        return {'status': 'inspection_error', 'reason': 'settings_invalid', 'root': str(root), 'game_dir': None}
    preference = data.get('hide_while_playing', True)
    if not isinstance(preference, bool):
        preference = True
    game = data.get('game_dir', '')
    if not isinstance(game, str):
        return {'status': 'inspection_error', 'reason': 'game_path_invalid', 'root': str(root), 'game_dir': None, 'hide_while_playing': preference}
    if not game:
        return {'status': 'not_configured', 'root': str(root), 'game_dir': None, 'hide_while_playing': preference}
    from .build import installed
    try:
        ready = installed(game)
    except (OSError, TypeError, ValueError, AttributeError):
        return {'status': 'inspection_error', 'reason': 'build_state_invalid', 'root': str(root), 'game_dir': game, 'hide_while_playing': preference}
    return {'status': 'ready' if ready else 'not_ready', 'root': str(root), 'game_dir': game, 'hide_while_playing': preference}


def _descends_from(pid, owner):
    seen = set()
    while pid > 1 and pid not in seen:
        if pid == owner:
            return True
        seen.add(pid)
        try:
            stat = (Path('/proc') / str(pid) / 'stat').read_text()
            pid = int(stat[stat.rfind(')') + 1:].split()[1])
        except (OSError, ValueError, IndexError):
            return False
    return False


def inspect_installation(root, owner_pid=None):
    result = _inspect_installation(root)
    result.setdefault('hide_while_playing', None)
    result['game_running'] = None
    if sys.platform.startswith('linux') and result.get('game_dir'):
        from .platform import game_executable
        exe = game_executable(result['game_dir']).resolve()
        try:
            for proc in Path('/proc').iterdir():
                if proc.name.isdigit():
                    try:
                        if (Path(os.readlink(proc / 'exe').removesuffix(' (deleted)')) == exe
                                and (owner_pid is None or _descends_from(int(proc.name), owner_pid))):
                            result['game_running'] = True
                            break
                    except OSError:
                        continue
        except OSError:
            pass
    if sys.platform == 'win32' and result.get('game_dir'):
        from .platform import game_executable
        from .platform.windows import game_running
        if game_running(str(game_executable(result['game_dir'])), owner_pid):
            result['game_running'] = True
    result['job_marker_present'] = (Path(root) / 'data' / 'job-running.json').is_file()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('root')
    parser.add_argument('--owner-pid', type=int)
    args = parser.parse_args()
    print(json.dumps(inspect_installation(args.root, args.owner_pid), ensure_ascii=True))


if __name__ == '__main__':
    main()
