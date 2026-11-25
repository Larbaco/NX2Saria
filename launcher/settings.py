import json
import os
import shutil
from pathlib import Path

DEFAULTS = {
    'game_dir': '',
    'dump': '',
    'auto_check': True,
    'auto_install': True,
    'channel': 'stable',
    'hide_while_playing': True,
    'warn_low_commit': True,
    'keep_cache': True,
    'desktop_shortcut': True,
    'save_export_dir': '',
    'export_with_log': True,
    'theme': 'forest',
    'custom_background': '',
    'custom_panel': '',
    'custom_buttons': '',
    'background': '',
    'background_dim': 40,
    'animations': True,
    'sounds': 'low',
    'feed_repo': '',
    'github_token': '',
    'last_check': 0.0,
    'last_seen': '',
    'rolled_back': '',
    'telemetry': '',
    'telemetry_closed': '',
    'telemetry_asked': '',
    'telemetry_notice': '',
    'crash_log_offer': 'on',
}

UNLOGGED = ('last_check', 'last_seen')
SECRET = ('github_token',)


def _shown(key, value):
    if key in SECRET:
        return 'set' if value else 'empty'
    if isinstance(value, bool):
        return 'on' if value else 'off'
    return str(value) if value != '' else "''"


class Settings(dict):
    def __init__(self, path, log=None):
        super().__init__(DEFAULTS)
        self.path = Path(path)
        self.log = log
        self.unreadable = False
        try:
            data = json.loads(self.path.read_text(encoding='utf-8-sig'))
            if isinstance(data, dict):
                self.update({k: v for k, v in data.items() if k in DEFAULTS and type(v) is type(DEFAULTS[k])})
        except OSError:
            pass
        except ValueError:
            self.unreadable = True
            try:
                shutil.copyfile(self.path, self.path.with_name(self.path.name + '.bad'))
            except OSError:
                pass
        self.saved = dict(self)

    def changes(self):
        return [f'setting: {k} {_shown(k, self.saved.get(k))} -> {_shown(k, self.get(k))}'
                for k in DEFAULTS if k not in UNLOGGED and self.get(k) != self.saved.get(k)]

    def _write(self, data):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_name(self.path.name + '.tmp')
        tmp.write_text(json.dumps(data, indent=1), encoding='utf-8')
        os.replace(tmp, self.path)

    def save(self):
        self._write(dict(self))
        lines, self.saved = self.changes(), dict(self)
        if self.log:
            for line in lines:
                self.log(line)

    def save_hide_while_playing(self, value):
        key = 'hide_while_playing'
        try:
            data = json.loads(self.path.read_text(encoding='utf-8-sig'))
        except FileNotFoundError:
            data = {}
        if not isinstance(data, dict):
            raise ValueError('settings are not an object')
        previous = data.get(key, DEFAULTS[key])
        if type(previous) is not bool:
            previous = DEFAULTS[key]
        if previous != value:
            data[key] = value
            self._write(data)
            if self.log:
                self.log(f'setting: {key} {_shown(key, previous)} -> {_shown(key, value)}')
        self[key] = self.saved[key] = value

    def reset(self):
        keep = {k: self[k] for k in ('game_dir', 'dump', 'last_check', 'last_seen', 'feed_repo', 'github_token',
                                     'rolled_back', 'telemetry', 'telemetry_closed', 'telemetry_asked',
                                     'telemetry_notice', 'crash_log_offer')}
        self.clear()
        self.update(DEFAULTS)
        self.update(keep)
