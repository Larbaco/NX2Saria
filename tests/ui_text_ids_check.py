# args: --with-self-test
import argparse
import re
import sys
from pathlib import Path

DESCRIPTION = ('Check that every text id of runtime/ui/ui_text.h is used, that id arithmetic is declared here, and '
               'that the language tables of runtime/ui/ui_text.c hold a string for each id and for no other')
ROOT = Path(__file__).resolve().parents[1]
HEADER = 'runtime/ui/ui_text.h'
SOURCE = 'runtime/ui/ui_text.c'
SEARCHED = ('runtime', 'tools')
LEFT_OUT = ('runtime/third_party/',)
SUFFIXES = ('.c', '.cpp', '.h', '.inc', '.py')
ENGLISH = 'text_en'
MENU_TABLES = {'text_en': 'en', 'text_ru': 'ru', 'text_pt_br': 'pt-BR', 'text_zh_hans': 'zh-Hans'}
MARKERS = ('UI_MENU_FIRST', 'UI_MENU_LAST_ID', 'UI_TEXT_COUNT')
ARITHMETIC = (
    ('runtime/ui/binds_page.cpp', 'UI_BIND_A + a', 'UI_BIND_A', 'UI_BIND_SHAKE', True,
     'the label of a bind action, by action number'),
    ('runtime/ui/binds_page.cpp', 'UI_BIND_DESC_A + a', 'UI_BIND_DESC_A', 'UI_BIND_DESC_DOWN', True,
     'what a bind action does, by action number up to the D-pad'),
    ('runtime/ui/binds_page.cpp', 'UI_BIND_ACTION_A + a', 'UI_BIND_ACTION_A', 'UI_BIND_ACTION_DOWN', True,
     'the short label of a game button, by action number up to the D-pad'),
    ('runtime/ui/binds_page.cpp', 'UI_KEY_SPACE + (k - KEY_SPACE)', 'UI_KEY_SPACE', 'UI_KEY_PAGEDOWN', True,
     'the name of a key, by key id'),
    ('runtime/ui/binds_page.cpp', 'UI_PAD_SOUTH + (p - PAD_SOUTH)', 'UI_PAD_SOUTH', 'UI_PAD_RPADDLE2', True,
     'the name of a gamepad button, by button id'),
    ('runtime/ui/menu.cpp', 'UI_MENU_PRESET_LOW + (tier', 'UI_MENU_PRESET_LOW', 'UI_MENU_PRESET_ULTRA', True,
     'the name of a preset tier, by tier number'),
    ('runtime/ui/binds_page.cpp', 'UI_BIND_SHAKE - UI_BIND_A ==', 'UI_BIND_A', 'UI_BIND_SHAKE', False,
     'static_assert: the bind labels follow the actions'),
    ('runtime/ui/binds_page.cpp', 'UI_BIND_DESC_DOWN - UI_BIND_DESC_A ==', 'UI_BIND_DESC_A', 'UI_BIND_DESC_DOWN', False,
     'static_assert: the bind descriptions follow the actions'),
    ('runtime/ui/binds_page.cpp', 'UI_BIND_ACTION_DOWN - UI_BIND_ACTION_A ==', 'UI_BIND_ACTION_A',
     'UI_BIND_ACTION_DOWN', False, 'static_assert: the short labels follow the game buttons'),
    ('runtime/ui/binds_page.cpp', 'UI_KEY_PAGEDOWN - UI_KEY_SPACE ==', 'UI_KEY_SPACE', 'UI_KEY_PAGEDOWN', False,
     'static_assert: the key names follow the key ids'),
    ('runtime/ui/binds_page.cpp', 'UI_PAD_RPADDLE2 - UI_PAD_SOUTH ==', 'UI_PAD_SOUTH', 'UI_PAD_RPADDLE2', False,
     'static_assert: the gamepad names follow the button ids'),
    ('runtime/hle/ov_probe.c', 'id = UI_MENU_FIRST; id <= UI_MENU_LAST_ID', 'UI_MENU_FIRST', 'UI_MENU_LAST_ID', False,
     'the probe: every menu text has its four languages and leaves room for a note'),
    ('runtime/hle/ov_probe.c', 'id = UI_MENU_SAVES; id <= UI_MENU_SAVE_E_UNSAFE', 'UI_MENU_SAVES',
     'UI_MENU_SAVE_E_UNSAFE', False, 'the probe: the formats of the Saves group'),
    ('runtime/hle/ov_probe.c', 'id = UI_MENU_LANGNAME_EN; id <= UI_MENU_LANGNAME_ES_419', 'UI_MENU_LANGNAME_EN',
     'UI_MENU_LANGNAME_ES_419', False, 'the probe: a language name is the same in every language'),
)
NAME = r'UI_[A-Z0-9_]+'
TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|^[ \t]*#.*$|[A-Za-z_][A-Za-z0-9_]*|\d+|\s+|.', re.M)
CAST = r'(?:\(\s*[A-Za-z_ ]+\)\s*)?'
STEP = re.compile(r'\b(%s)\s*(?:\+|-(?!>)|<=|>=|<(?!<)|>(?!>))|(?:\+|(?<!-)-|<=|>=|(?<!<)<|(?<!>)(?<!-)>)\s*%s(%s)\b'
                  % (NAME, CAST, NAME))


def declared(header):
    body = header[header.index('typedef enum {') + 14:header.index('} UiTextId;')]
    ids, markers = [], {}
    for part in body.replace('\n', ' ').split(','):
        m = re.fullmatch(r'\s*(%s)\s*(?:=\s*(%s))?\s*' % (NAME, NAME), part)
        if not m:
            continue
        name, same_as = m.groups()
        if name in MARKERS:
            markers[name] = same_as or len(ids)
            continue
        if same_as in markers:
            markers[same_as] = len(ids)
        ids.append(name)
    position = {name: k for k, name in enumerate(ids)}
    for name, at in markers.items():
        markers[name] = position[at] if isinstance(at, str) else at
    return ids, position, markers


def tables(source):
    toks = [m.group(0) for m in TOKEN.finditer(source)]
    out, shared, outside, doubled = {}, set(), [], []
    for macro in ('LANG_NAMES', 'SWKBD'):
        m = re.search(r'#define %s\b.*?(?:\\\n.*?)*\n' % macro, source)
        shared.update(re.findall(r'\[(%s)\]' % NAME, m.group(0)))
    k, table = 0, None
    while k < len(toks):
        t = toks[k]
        if table is None:
            if t.startswith('text_') and toks[k + 1:k + 3] == ['[', 'UI_TEXT_COUNT']:
                table = out.setdefault(t, {})
                k = toks.index('{', k)
            elif re.fullmatch(NAME, t):
                outside.append(t)
        elif t == '}' and toks[k + 1] == ';':
            table = None
        elif t == '[' and re.fullmatch(NAME, toks[k + 1]) and toks[k + 2] == ']':
            name = toks[k + 1]
            if name in table:
                doubled.append((next(n for n, tab in out.items() if tab is table), name))
            table[name] = True
            k += 2
        k += 1
    for name in re.findall(r'SWKBD_ONLY\((text_\w+),', source):
        out.setdefault(name, {})
    return out, shared, outside, doubled


def sources(root):
    out = {}
    for folder in SEARCHED:
        for path in sorted((root / folder).rglob('*')):
            rel = path.relative_to(root).as_posix()
            if path.suffix not in SUFFIXES or rel in (HEADER, SOURCE) or rel.startswith(LEFT_OUT) or not path.is_file():
                continue
            text = path.read_text(encoding='utf-8', errors='replace')
            if 'UI_' in text:
                out[rel] = text
    return out


def check(header, source, others):
    problems = []
    ids, position, markers = declared(header)
    known = set(ids) | set(markers)
    tabs, shared, outside, doubled = tables(source)
    named = set(outside)
    for text in others.values():
        named.update(re.findall(r'\b%s\b' % NAME, text))

    reached = {}
    for path, needle, first, last, shows, why in ARITHMETIC:
        if needle not in others.get(path, ''):
            problems.append(f'ARITHMETIC: {path} no longer holds "{needle}" ({why}): update the list')
            continue
        lo, hi = (markers[n] if n in markers else position.get(n) for n in (first, last))
        if lo is None or hi is None or lo > hi:
            problems.append(f'ARITHMETIC: {first} to {last} is not a range of {HEADER} ({why})')
        elif shows:
            for name in ids[lo:hi + 1]:
                reached[name] = why

    needles = {}
    for path, needle, *_ in ARITHMETIC:
        needles.setdefault(path, []).append(needle)
    for path, text in sorted(others.items()):
        if not path.endswith(('.c', '.cpp', '.h', '.inc')):
            continue
        for number, line in enumerate(text.split('\n'), 1):
            found = [a or b for a, b in STEP.findall(line)]
            found = [n for n in found if n in known and n != 'UI_TEXT_COUNT']
            if found and not any(needle in line for needle in needles.get(path, ())):
                problems.append(f'{path}:{number}: arithmetic or an ordered comparison on {found[0]} that ARITHMETIC '
                                f'in tests/ui_text_ids_check.py does not declare')

    for name in ids:
        if name not in named and name not in reached:
            problems.append(f'{HEADER}: {name} is declared and nothing uses it: no code names it and no declared '
                            f'arithmetic reaches it. Remove it with its strings in every language, or use it')

    english = set(tabs.get(ENGLISH, {})) | shared
    falls = {}
    for table, name in doubled:
        problems.append(f'{SOURCE}: {table} holds two strings for {name}; the compiler keeps the last')
    for table, strings in tabs.items():
        for name in strings:
            if name not in position:
                problems.append(f'{SOURCE}: {table} holds a string for {name}, which {HEADER} does not declare')
            elif name not in english:
                problems.append(f'{SOURCE}: {table} holds a string for {name} and {ENGLISH} has none')
        if table not in MENU_TABLES:
            continue
        for name in ids:
            if name in strings or name in shared:
                continue
            if table == ENGLISH:
                problems.append(f'{SOURCE}: {name} has no English string, so it shows as nothing in every language')
            else:
                falls[table] = falls.get(table, 0) + 1
    return problems, {'ids': len(ids), 'named': sum(1 for n in ids if n in named),
                      'reached': sum(1 for n in ids if n not in named and n in reached),
                      'strings': {MENU_TABLES[t]: len(tabs[t]) + len(shared) for t in MENU_TABLES if t in tabs},
                      'falls': {MENU_TABLES[t]: falls.get(t, 0) for t in MENU_TABLES if t != ENGLISH}}


def load(root):
    return ((root / HEADER).read_text(encoding='utf-8'), (root / SOURCE).read_text(encoding='utf-8'), sources(root))


def self_test(root):
    header, source, others = load(root)
    base, counts = check(header, source, others)
    if base:
        print('ui_text_ids_check: FAIL: self-test: the tree itself does not pass')
        return 1

    def add_id(text, name):
        return text.replace('    UI_TEXT_COUNT\n', f'    {name},\n    UI_TEXT_COUNT\n')

    def add_string(text, table, name):
        head = f'{table}[UI_TEXT_COUNT] = {{\n'
        return text.replace(head, f'{head}    [{name}] = "planted",\n')

    def drop_string(text, table, name):
        start = text.index(f'{table}[UI_TEXT_COUNT]')
        at = text.index(f'[{name}] = ', start)
        return text[:at] + text[text.index('",', at) + 2:]

    everywhere = source
    for table in MENU_TABLES:
        everywhere = add_string(everywhere, table, 'UI_MENU_PLANTED')
    binds = 'runtime/ui/binds_page.cpp'
    cases = (
        ('an id with its four strings that nothing names', add_id(header, 'UI_MENU_PLANTED'), everywhere, others,
         'UI_MENU_PLANTED is declared and nothing uses it'),
        ('a string for an id that is not declared', header, add_string(source, 'text_ru', 'UI_MENU_GONE'), others,
         'text_ru holds a string for UI_MENU_GONE, which'),
        ('an id without its English string', header, drop_string(source, 'text_en', 'UI_MENU_QUIT'), others,
         'UI_MENU_QUIT has no English string'),
        ('two strings for one id in one table', header, add_string(source, 'text_zh_hans', 'UI_MENU_QUIT'), others,
         'text_zh_hans holds two strings for UI_MENU_QUIT'),
        ('id arithmetic the list does not declare', header, source,
         {**others, binds: others[binds] + '\nint planted(int k) { return UI_MENU_ON + k; }\n'},
         'arithmetic or an ordered comparison on UI_MENU_ON'),
        ('a declared expression that left the code', header, source,
         {**others, binds: others[binds].replace('UI_BIND_A + a', 'UI_BIND_A + b')}, 'no longer holds "UI_BIND_A + a"'),
    )
    rc = 0
    _, fewer = check(header, drop_string(source, 'text_pt_br', 'UI_MENU_QUIT'), others)
    if fewer['falls']['pt-BR'] == counts['falls']['pt-BR'] + 1:
        print('ui_text_ids_check: self-test: an id without its Brazilian Portuguese string passes and is counted')
    else:
        print(f'ui_text_ids_check: FAIL: self-test: a dropped translation was not counted: {fewer["falls"]}')
        rc = 1
    for what, h, s, o, want in cases:
        problems, _ = check(h, s, o)
        hit = [p for p in problems if want in p]
        if hit:
            print(f'ui_text_ids_check: self-test: {what} fails the check ({hit[0]})')
        else:
            print(f'ui_text_ids_check: FAIL: self-test: {what} passed the check; problems: {problems}')
            rc = 1
    return rc


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('root', nargs='?', default=str(ROOT), help='the repository (default: this checkout)')
    ap.add_argument('--self-test', action='store_true', help='only check that each planted fault fails')
    ap.add_argument('--with-self-test', action='store_true', help='the check, then the self-test')
    a = ap.parse_args()
    root = Path(a.root)
    if a.self_test:
        return self_test(root)
    problems, n = check(*load(root))
    for p in problems:
        print(f'ui_text_ids_check: FAIL: {p}')
    falls = ', '.join(f'{lang} {count}' for lang, count in n['falls'].items())
    strings = ', '.join(f'{lang} {count}' for lang, count in n['strings'].items())
    print(f'ui_text_ids_check: {n["ids"]} ids: {n["named"]} named in code, {n["reached"]} reached only by declared '
          f'arithmetic, {n["ids"] - n["named"] - n["reached"]} unused; strings: {strings}; ids without a string '
          f'of their own, shown in English: {falls}; {len(problems)} problem(s)')
    rc = 1 if problems else 0
    if a.with_self_test:
        rc |= self_test(root)
    return rc


if __name__ == '__main__':
    sys.exit(main())
