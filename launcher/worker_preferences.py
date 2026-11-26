import json
import os

LANGUAGES = ('automatic', 'en-US', 'pt-BR', 'ru', 'zh-Hans')


def _read(path):
    try:
        values = json.loads(path.read_text(encoding='utf-8'))
    except FileNotFoundError:
        return {}
    if not isinstance(values, dict):
        raise ValueError('invalid Electron preferences')
    return values


def language(layout):
    try:
        value = _read(layout.data / 'electron-preferences.json').get('locale')
    except (OSError, ValueError):
        value = None
    return value if value in LANGUAGES else 'automatic'


def set_language(layout, value):
    path = layout.data / 'electron-preferences.json'
    values = _read(path)
    if values.get('locale') == value:
        return
    values['locale'] = value
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(values, ensure_ascii=True, indent=2) + '\n', encoding='utf-8')
    os.replace(temporary, path)
