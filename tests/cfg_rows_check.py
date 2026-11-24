import re
import sys
from pathlib import Path

KEY = r'[A-Z][A-Z0-9_]*'
TABLES = {
    'known[]': ('static const char *const known[]', r'"(' + KEY + r')"'),
    'metas[]': ('static const CfgMeta metas[]', r'\{\s*"(' + KEY + r')"'),
    'sections[]': ('sections[] = {', r'\{\s*"(' + KEY + r')"'),
    'DECK[]': ('DECK[] = {', r'\{\s*"(' + KEY + r')"'),
}
VALUE_FNS = ('cfg_value', 'cfg_int', 'cfg_bool', 'cfg_double')
GUARD_FNS = ('cfg_get', 'cfg_is_set', 'cfg_meta')
LIT = r'"NX2SARIA_(' + KEY + r')"'


def matching(src, open_at):
    pair = {'{': '}', '(': ')'}[src[open_at]]
    depth, k, n = 0, open_at, len(src)
    while k < n:
        c = src[k]
        if src.startswith('//', k):
            k = src.find('\n', k)
            if k < 0:
                return n
        elif src.startswith('/*', k):
            k = src.find('*/', k + 2)
            if k < 0:
                return n
            k += 1
        elif c == '"' or c == "'":
            k += 1
            while k < n and src[k] != c:
                k += 2 if src[k] == '\\' else 1
        elif c == src[open_at]:
            depth += 1
        elif c == pair:
            depth -= 1
            if depth == 0:
                return k
        k += 1
    return n


def split_comment(line):
    i, n, quote = 0, len(line), None
    while i < n:
        c = line[i]
        if quote:
            if c == '\\':
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in '"\'':
            quote = c
        elif line.startswith('//', i):
            return line[:i], line[i:]
        i += 1
    return line, ''


def strip_comments(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and src[j] != c and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * src.count('\n', i, j))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def line_of(src, pos):
    return src.count('\n', 0, pos) + 1


def tables(cfg, problems):
    out = {}
    for name, (decl, row) in TABLES.items():
        i = cfg.find(decl)
        if i < 0:
            problems.append(f'runtime/cfg/cfg.c: no {name} table ("{decl}")')
            out[name] = []
            continue
        j = cfg.index('{', i + len(decl) - 1)
        body = cfg[j + 1:matching(cfg, j)]
        line0 = line_of(cfg, j)
        names, branch, seen_branch = [], [], {}
        for n, line in enumerate(body.split('\n')):
            st = line.strip()
            if st.startswith('#if'):
                branch.append(0)
                continue
            if st.startswith(('#else', '#elif')):
                if branch:
                    branch[-1] += 1
                continue
            if st.startswith('#endif'):
                if branch:
                    branch.pop()
                continue
            code, comment = split_comment(line)
            if '/*' in code:
                problems.append(f'runtime/cfg/cfg.c:{line0 + n}: a /* comment in {name}, which this check does not read')
            hidden = re.findall(row, comment)
            if hidden:
                problems.append(f'runtime/cfg/cfg.c:{line0 + n}: {name} rows after // are not compiled: {", ".join(hidden)}')
            if branch and branch[-1] > 0:
                continue
            for k in re.findall(row, code):
                names.append((k, line0 + n))
        out[name] = names
    return out


def functions(src):
    out = []
    for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(([^;{}()]*)\)\s*(?:const\s*)?\{', src):
        name = m.group(1)
        if name in ('if', 'for', 'while', 'switch', 'return', 'sizeof', 'catch'):
            continue
        b = m.end() - 1
        out.append((name, m.group(2), b, matching(src, b)))
    return out


def call_args(src, open_at):
    end = matching(src, open_at)
    args, depth, cur, k = [], 0, '', open_at + 1
    while k < end:
        c = src[k]
        if c == '"':
            j = k + 1
            while j < end and src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            cur += src[k:j + 1]
            k = j + 1
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == ',' and depth == 0:
            args.append(cur.strip())
            cur = ''
            k += 1
            continue
        cur += c
        k += 1
    args.append(cur.strip())
    return args


def check(cfg_text, sources, patches):
    problems = []
    tabs = tables(cfg_text, problems)
    known = [k for k, _ in tabs['known[]']]
    known_s = set(known)
    metas_s = {k for k, _ in tabs['metas[]']}
    for k in sorted({k for k in known if known.count(k) > 1}):
        problems.append(f'runtime/cfg/cfg.c: known[] lists {k} twice')
    seen = {}
    for k, n in tabs['metas[]']:
        if k in seen:
            problems.append(f'runtime/cfg/cfg.c:{n}: metas[] has a second row for {k} (the first at line {seen[k]})')
        seen.setdefault(k, n)
    for k, n in tabs['metas[]']:
        if k not in known_s:
            problems.append(f'runtime/cfg/cfg.c:{n}: metas[] row {k} is not in known[]')

    stripped = {p: strip_comments(t) for p, t in sources.items()}
    wrappers = {}
    for p, s in stripped.items():
        for fname, params, b, e in functions(s):
            if fname in VALUE_FNS or fname in GUARD_FNS or fname == 'rt_knob':
                continue
            body = s[b:e]
            for idx, prm in enumerate(x.strip() for x in params.split(',')):
                pm = re.fullmatch(r'const\s+char\s*\*\s*(\w+)', prm)
                if not pm:
                    continue
                v = pm.group(1)
                if re.search(r'\b(?:%s)\s*\(\s*%s\s*\)' % ('|'.join(VALUE_FNS), v), body):
                    wrappers[fname] = (idx, 'value', True)
                elif re.search(r'\bcfg_get\s*\(\s*%s\s*\)' % v, body):
                    wrappers.setdefault(fname, (idx, 'name', True))
                elif re.search(r'"NX2SARIA_%%s"\s*,\s*%s\b' % v, body):
                    wrappers.setdefault(fname, (idx, 'name', False))

    value_sites, name_sites = [], []
    for p, s in stripped.items():
        start = s.index('static int is_known') if p.endswith('runtime/cfg/cfg.c') and 'static int is_known' in s else 0
        for m in re.finditer(r'\b(%s)\s*\(\s*' % '|'.join(VALUE_FNS) + LIT, s):
            if m.start() >= start:
                value_sites.append((m.group(2), p, m.start(), m.group(1)))
        for m in re.finditer(r'\b(%s)\s*\(\s*' % '|'.join(GUARD_FNS) + LIT, s):
            if m.start() >= start:
                name_sites.append((m.group(2), p, m.start(), m.group(1)))
        for m in re.finditer(r'\brt_knob\s*\(\s*"(' + KEY + r')"', s):
            name_sites.append((m.group(1), p, m.start(), 'rt_knob'))
        for w, (idx, kind, prefixed) in wrappers.items():
            for m in re.finditer(r'\b' + re.escape(w) + r'\s*\(', s):
                args = call_args(s, m.end() - 1)
                if idx >= len(args):
                    continue
                lm = re.fullmatch(r'"(NX2SARIA_)?(' + KEY + r')"', args[idx])
                if not lm or bool(lm.group(1)) != prefixed:
                    continue
                (value_sites if kind == 'value' else name_sites).append((lm.group(2), p, m.start(), w))
    for n, line in enumerate(patches.split('\n'), 1):
        m = re.match(r'\s*set\s+\S+\s+NX2SARIA_(' + KEY + r')\b', line)
        if m:
            name_sites.append((m.group(1), 'recomp/patches-v111.txt', -n, 'a patch set'))

    def where(p, pos):
        return f'{p}:{-pos if pos < 0 else line_of(stripped[p], pos)}'

    bodies = {p: functions(s) for p, s in stripped.items()}
    for k, p, pos, how in value_sites:
        if k in metas_s:
            continue
        body = next(((b, e) for _, _, b, e in bodies[p] if b < pos < e), None)
        text = stripped[p][body[0]:pos] if body else ''
        if re.search(r'\b(?:%s)\s*\(\s*"NX2SARIA_%s"' % ('|'.join(GUARD_FNS), k), text):
            continue
        problems.append(f'{where(p, pos)}: {how}("NX2SARIA_{k}") has no metas[] row and no cfg_get guard before it: '
                        f'the game would end with "has no row in the settings table"')
    missing = {}
    for k, p, pos, how in name_sites + value_sites:
        if k not in known_s and not k.startswith('CHEAT_'):
            missing.setdefault(k, []).append(f'{where(p, pos)} {how}')
    for k, ws in sorted(missing.items()):
        problems.append(f'{ws[0].split()[0]}: {k} is read ({"; ".join(ws[:3])}{"; ..." if len(ws) > 3 else ""}) but '
                        f'is not in cfg.c known[]: set in nx2navi.ini it logs "unknown key"')
    return problems, len(known), len(tabs['metas[]']), len(value_sites), len(name_sites), sorted(wrappers)


def read_repo(repo):
    rt = repo / 'runtime'
    files = sorted(p for p in rt.rglob('*') if p.suffix in ('.c', '.cpp', '.h', '.inc') and 'third_party' not in p.parts
                   and not p.name.endswith('_spv.h') and p.name != 'cfg_probe.c')
    sources = {p.relative_to(repo).as_posix(): p.read_text(encoding='utf-8-sig', errors='replace') for p in files}
    patches = repo / 'recomp' / 'patches-v111.txt'
    return sources['runtime/cfg/cfg.c'], sources, patches.read_text(encoding='utf-8') if patches.exists() else ''


def self_test(repo):
    cfg, sources, patches = read_repo(repo)
    base, *_ = check(cfg, sources, patches)
    bad = 0
    if base:
        print(f'self-test: the repository itself has {len(base)} problems; the cases below are judged on new ones')

    def case(what, want, cfg_text=None, extra=None, patch_text=None):
        nonlocal bad
        src = dict(sources)
        src['runtime/cfg/cfg.c'] = cfg_text if cfg_text is not None else cfg
        src.update(extra or {})
        got, *_ = check(src['runtime/cfg/cfg.c'], src, patch_text if patch_text is not None else patches)
        new = [p for p in got if p not in base]
        ok = any(re.search(want, p) for p in new)
        print(f'self-test: {what}: {"caught" if ok else "MISSED"}')
        if not ok:
            bad += 1
            for p in new:
                print('   ', p)

    def add_line(table_decl, line):
        i = cfg.index(table_decl)
        j = cfg.index('\n', cfg.index('{', i + len(table_decl) - 1)) + 1
        return cfg[:j] + '    ' + line + '\n' + cfg[j:]

    case('a metas[] row after //', r'metas\[\] rows after // are not compiled: HIDDEN_ROW',
         add_line(TABLES['metas[]'][0], '{"FULLSCREEN_X", "0", "0|1", LIVE}, // {"HIDDEN_ROW", "0", "0|1", LIVE},'))
    case('a known[] name after //', r'known\[\] rows after // are not compiled: HIDDEN_NAME',
         add_line(TABLES['known[]'][0], '"AB_X",   // "HIDDEN_NAME",'))
    case('a sections[] entry after //', r'sections\[\] rows after // are not compiled: HIDDEN_SECTION',
         add_line(TABLES['sections[]'][0], '// {"HIDDEN_SECTION", "display"},'))
    case('a /* in a table', r'a /\* comment in known\[\]', add_line(TABLES['known[]'][0], '/* "X", */'))
    case('a name twice in known[]', r'known\[\] lists AA twice', add_line(TABLES['known[]'][0], '"AA",'))
    case('two metas[] rows for one key', r'second row for FULLSCREEN',
         add_line(TABLES['metas[]'][0], '{"FULLSCREEN", "0", "0|1", LIVE},'))
    case('a metas[] row not in known[]', r'metas\[\] row ROW_NOT_KNOWN is not in known\[\]',
         add_line(TABLES['metas[]'][0], '{"ROW_NOT_KNOWN", "0", "0|1", LIVE},'))
    at = cfg.index('{"FULLSCREEN"', cfg.index(TABLES['metas[]'][0]))
    case('a value read on a row hidden by // (FULLSCREEN)', r'FULLSCREEN"\) has no metas\[\] row',
         cfg[:at] + '// ' + cfg[at:])
    fake = '''
static int my_opt(const char *key) { return cfg_int(key); }
static unsigned my_env(const char *name) { return cfg_get(name) ? 1 : 0; }
void fake(void) {
    int a = cfg_int("NX2SARIA_NO_ROW_INT");
    if (cfg_get("NX2SARIA_GUARDED_BOOL") && cfg_bool("NX2SARIA_GUARDED_BOOL")) a++;
    a += my_opt("NX2SARIA_WRAPPED_NO_ROW");
    a += rt_knob("KNOB_NOT_KNOWN", 0);
    a += (int)my_env("NX2SARIA_ENV_NOT_KNOWN");
}
'''
    extra = {'runtime/fake_check.c': fake}
    case('an unguarded cfg_int without a row', r'cfg_int\("NX2SARIA_NO_ROW_INT"\) has no metas', extra=extra)
    case('a value read through a wrapper without a row', r'my_opt\("NX2SARIA_WRAPPED_NO_ROW"\) has no metas', extra=extra)
    case('a guarded read without a row is not reported as such, its name is',
         r'GUARDED_BOOL is read .* not in cfg.c known', extra=extra)
    got, *_ = check(cfg, {**sources, **extra}, patches)
    guarded_flagged = any('GUARDED_BOOL") has no metas' in p for p in got)
    print(f'self-test: a read after a cfg_get guard in the same function: {"REPORTED" if guarded_flagged else "passes"}')
    bad += guarded_flagged
    case('an rt_knob name not in known[]', r'KNOB_NOT_KNOWN is read', extra=extra)
    case('a name read through a cfg_get wrapper', r'ENV_NOT_KNOWN is read', extra=extra)
    case('a patch set\'s name not in known[]', r'PATCH_SET_NOT_KNOWN is read',
         patch_text=patches + '\nset some_patch NX2SARIA_PATCH_SET_NOT_KNOWN 1\n')
    typed = {'runtime/fake_typed.c': '''
void fake_typed(void) {
    int a = cfg_bool("NX2SARIA_G42_BOOL");
    a += (int)cfg_double("NX2SARIA_G42_DOUBLE");
    a += cfg_value("NX2SARIA_G42_VALUE")[0];
    (void)a;
}
'''}
    known_g42 = add_line(TABLES['known[]'][0], '"G42_BOOL", "G42_DOUBLE", "G42_VALUE",')
    for fn, key in (('cfg_bool', 'G42_BOOL'), ('cfg_double', 'G42_DOUBLE'), ('cfg_value', 'G42_VALUE')):
        case(f'a known key read with {fn} without a metas[] row (G42)',
             r'%s\("NX2SARIA_%s"\) has no metas\[\] row' % (fn, key), known_g42, extra=typed)
    print(f'cfg_rows_check self-test: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


def main():
    args = sys.argv[1:]
    repo = Path(__file__).resolve().parents[1]
    if args and args[0] != '--self-test':
        repo = Path(args[0])
    if '--self-test' in args:
        return self_test(repo)
    problems, nk, nm, nv, nn, wr = check(*read_repo(repo))
    for p in problems:
        print(p)
    print(f'cfg_rows_check: known[] {nk} names, metas[] {nm} rows, {nv} value reads and {nn} name reads (wrappers: '
          f'{", ".join(wr)}): {len(problems)} problem{"" if len(problems) == 1 else "s"}')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
