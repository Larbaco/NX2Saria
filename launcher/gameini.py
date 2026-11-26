import os
import re
from pathlib import Path

PREFIXES = ('NX2SARIA_', 'NX2NAVI_')
BOM = '﻿'


def _path(game_dir, migrate=True):
    folder = Path(game_dir)
    current, legacy = folder / 'nx2saria.ini', folder / 'nx2navi.ini'
    if not current.exists() and legacy.exists():
        if not migrate or not legacy.is_file():
            return legacy
        try:
            if os.name == 'nt':
                legacy.rename(current)
            else:
                os.link(legacy, current)
                legacy.unlink()
        except OSError:
            return legacy if not current.exists() else current
    return current


def _key(text):
    k = text.strip().upper()
    for prefix in PREFIXES:
        if k.startswith(prefix): return k[len(prefix):]
    return k


def _text(game_dir, migrate=True):
    try:
        with open(_path(game_dir, migrate), encoding='utf-8', errors='surrogateescape', newline='') as f:
            return f.read()
    except FileNotFoundError:
        return ''


def _setting(line, choices=False):
    s = line.lstrip(BOM).strip()
    if not s or s[0] in '#;[' or '=' not in s:
        return None
    k, raw = s.split('=', 1)
    v = raw.strip()
    quoted = len(v) >= 2 and v[0] == v[-1] == '"'
    if choices and not quoted:
        m = re.search(r'[ \t][;#]', raw)
        if m:
            v = raw[:m.start()].strip()
    if len(v) >= 2 and v[0] == v[-1] == '"':
        v = v[1:-1]
    return _key(k), v


def get(game_dir, key, choices=False, *, migrate=True):
    want, value = _key(key), ''
    for line in _text(game_dir, migrate).splitlines():
        kv = _setting(line, choices)
        if kv and kv[0] == want and kv[1]:
            value = kv[1]
    return value


def put(game_dir, section, key, value, choices=False):
    text = _text(game_dir)
    bom = BOM if text.startswith(BOM) else ''
    lines = text[len(bom):].splitlines(keepends=True)
    want = _key(key)
    entry = f'{key.lower()} = {value}'
    nl = '\r\n' if lines and lines[0].endswith('\r\n') else '\n'
    mine = [i for i, line in enumerate(lines) if (_setting(line) or ('',))[0] == want]
    valued = [i for i in mine if _setting(lines[i], choices)[1]]
    at = (valued or mine or [-1])[-1]
    if at >= 0:
        end = lines[at][len(lines[at].rstrip('\r\n')):] or nl
        lines[at] = entry + end
    else:
        head = next((i for i, line in enumerate(lines) if line.strip().lower() == f'[{section.lower()}]'), -1)
        if head >= 0:
            if not lines[head].endswith(('\n', '\r')):
                lines[head] += nl
            lines.insert(head + 1, entry + nl)
        else:
            if lines and not lines[-1].endswith(('\n', '\r')):
                lines[-1] += nl
            lines += [nl] if lines else []
            lines += [f'[{section}]{nl}', entry + nl]
    path = _path(game_dir)
    tmp = path.with_name(path.name + '.tmp')
    try:
        with open(tmp, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
            f.write(bom + ''.join(lines))
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)
