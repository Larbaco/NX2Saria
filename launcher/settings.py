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

    def _read_for_write(self):
        try:
            data = json.loads(self.path.read_text(encoding='utf-8-sig'))
        except FileNotFoundError:
            data = {}
        if not isinstance(data, dict):
            raise ValueError('settings are not an object')
        return data

    def save_install_choices(self, dump, game_dir):
        data = self._read_for_write()
        choices = {'dump': dump, 'game_dir': game_dir}
        previous = {key: data.get(key, DEFAULTS[key]) for key in choices}
        if previous != choices:
            data.update(choices)
            self._write(data)
        for key, value in choices.items():
            self[key] = self.saved[key] = value
            if self.log and previous[key] != value:
                self.log(f'setting: {key} {_shown(key, previous[key])} -> {_shown(key, value)}')

    def save_preference(self, key, value):
        data = self._read_for_write()
        default = DEFAULTS[key]
        previous = data.get(key, default)
        if type(previous) is not type(default):
            previous = default
        if previous != value:
            data[key] = value
            self._write(data)
            if self.log:
                self.log(f'setting: {key} {_shown(key, previous)} -> {_shown(key, value)}')
        self[key] = self.saved[key] = value

    def record_update_check(self, checked_at, release=None):
        data = self._read_for_write()
        changes = {'last_check': checked_at}
        if release:
            changes['last_seen'] = release
        data.update(changes)
        self._write(data)
        self.update(changes)
        self.saved.update(changes)

    def save_hide_while_playing(self, value):
        self.save_preference('hide_while_playing', value)

    def reset(self):
        keep = {k: self[k] for k in ('game_dir', 'dump', 'last_check', 'last_seen', 'feed_repo', 'github_token',
                                     'rolled_back', 'telemetry', 'telemetry_closed', 'telemetry_asked',
                                     'telemetry_notice', 'crash_log_offer')}
        self.clear()
        self.update(DEFAULTS)
        self.update(keep)
