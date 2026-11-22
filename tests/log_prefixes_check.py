"""Static check of the log prefixes the runtime writes against the table in docs/logging.md. Standard library only.

Every line of logs/nx2saria.log starts with a word or a few that say which part of the game wrote it ("vk:",
"frame rate:", "boot map:"). This check finds the prefix of each log call in runtime/ and fails on a prefix that has
no row in docs/logging.md, so a new prefix comes with its row and a reader of a player's log can look it up. A row
of the main table that no call uses any more is a warning: the table is behind, the code is not wrong.

What a log call is:
- rt_log(fmt, ...) and rt_logf(fmt, ...) of runtime/host.h;
- a macro whose body calls rt_log or rt_logf with __VA_ARGS__ as the format, after string literals or none
  (vk_pipeline.c LOG_ONCE, nvn_api.h NVN_LOG with its "nvn: ", romfs.c BAD);
- a function with a const char * parameter that it passes on as rt_log's or rt_logf's format (ov_input.c once).
  A macro or function defined in a .c or .cpp file counts in that file only; one in a header counts everywhere.

The prefix of a format string:
- the text before the first colon that ends a word (a colon followed by a space, a "\\n" or the end), when that text
  is one to four words of letters, digits and _ . : / + - ("vk:", "host save import:", "aud:u:", "nn::codec:");
- otherwise the format's first word, without a colon ("frame %llu: submits ..." gives "frame"); the table writes it
  without a colon too;
- a format that starts with white space continues the line before it (a block such as the crash report's registers)
  and has no prefix of its own;
- an rt_logf format, written as it is with no "[host] " and no newline, is a prefix only when it has the colon form
  above: the others are pieces of a line another rt_logf started ("outside every module)", " %08x");
- a format chosen by ?: gives one prefix for each of its string literals.

A format that starts with a conversion ("%s", "%s: ...") takes its prefix from the text put in its place, which this
check reads only when the first argument is a plain name that the same function filled before the call with
snprintf(name, size, "...") or sprintf(name, "..."), or set with name = "...". Every other such call, and a call whose
format is not a string literal, is counted as not seen; `--unseen` lists them. docs/logging.md lists the prefixes those
lines are known to carry in a table of their own, which is not checked for use.

Not read: rt_fatal and rt_fatal_code (their line is "FATAL: <message>", dispatch.c fatal_end), the "[host] " every
rt_log line starts with, runtime/third_party/ and the generated *_spv.h headers. Code under #if 0 and in every #ifdef
branch is read like the rest.

Usage: python tests/log_prefixes_check.py [repo]          one line per problem; exit 1 if any
       python tests/log_prefixes_check.py --unseen        also list the calls whose prefix the check cannot read
       python tests/log_prefixes_check.py --self-test     inject each kind of fault into copies and check it is caught
"""
import re
import sys
from pathlib import Path

from cfg_rows_check import call_args, functions, line_of, strip_comments

DOC = 'docs/logging.md'
MAIN_HEADING = '## Prefixes'
UNSEEN_HEADING = '## Prefixes of lines the check cannot see'
LOGGERS = ('rt_log', 'rt_logf')
LIT = r'"(?:[^"\\\n]|\\.)*"'
RUN = r'(?:' + LIT + r'\s*)+'
CONVERSION = r'%[-+ #0-9.*]*(?:hh|ll|[hlzjtL])?[diouxXeEfFgGaAcspn%]'
PREFIX_TEXT = re.compile(r'[A-Za-z_][A-Za-z0-9_.:/+\-]*(?: [A-Za-z0-9_.:/+\-]+){0,3}')


def literal_text(run):
    """The text of adjacent string literals, escapes left as written."""
    return ''.join(m[1:-1] for m in re.findall(LIT, run))


def prefix_of(text, raw=False):
    """The prefix of a format string's text, '' for a continuation, None for a format that starts with a conversion."""
    if not text or text[0] in ' \t' or text.startswith('\\n'):
        return ''
    if text[0] == '%':
        return None
    m = re.search(r':(?=\s|\\n|$)', text)
    if m and PREFIX_TEXT.fullmatch(text[:m.start()]):
        return text[:m.start() + 1]
    if raw:
        return ''
    w = re.match(r'[^\s%(,;:]+', text)
    return w.group(0) if w else ''


def wrappers(src):
    """{name: (format argument index, prefix literal text, logger, position of the wrapped call)} defined in src."""
    out = {}
    for m in re.finditer(r'#define\s+(\w+)\(\.\.\.\)([^\n]*(?:\\\n[^\n]*)*)', src):
        body = m.group(2)
        c = re.search(r'\b(rt_logf?)\s*\(\s*(' + RUN + r')?__VA_ARGS__', body)
        if c:
            out[m.group(1)] = (0, literal_text(c.group(2) or ''), c.group(1), m.start(2) + c.start())
    for name, params, b, e in functions(src):
        if name in LOGGERS:
            continue
        body = src[b:e]
        for idx, prm in enumerate(x.strip() for x in params.split(',')):
            pm = re.fullmatch(r'const\s+char\s*\*\s*(\w+)', prm)
            if not pm:
                continue
            c = re.search(r'\b(rt_logf?)\s*\(\s*%s\s*[,)]' % pm.group(1), body)
            if c:
                out[name] = (idx, '', c.group(1), b + c.start())
    return out


def filled_with(src, body_start, pos, name):
    """The format texts a function wrote into the buffer `name` before pos, or the literals it set it to."""
    texts = []
    for m in re.finditer(r'\b(snprintf|sprintf)\s*\(\s*%s\s*,' % re.escape(name), src[body_start:pos]):
        args = call_args(src, body_start + m.start() + m.group(0).index('('))
        k = 2 if m.group(1) == 'snprintf' else 1
        r = re.match(r'\s*(' + RUN + ')', args[k]) if len(args) > k else None
        if r:
            texts.append(literal_text(r.group(1)))
    for m in re.finditer(r'\b%s\s*=\s*(%s)' % (re.escape(name), RUN), src[body_start:pos]):
        texts.append(literal_text(m.group(1)))
    return texts


def scan(sources):
    """The prefixes the runtime writes: ({prefix: [file:line, ...]}, [unseen call file:line and text], counts)."""
    stripped = {p: strip_comments(t) for p, t in sources.items()}
    shared, local, skip = {}, {}, {}
    for p, s in stripped.items():
        w = wrappers(s)
        (shared if p.endswith(('.h', '.inc')) else local.setdefault(p, {})).update(w)
        skip[p] = {v[3] for v in w.values()}   # the wrapper's own call, whose format is its parameter
    used, unseen = {}, []
    counts = {'calls': 0, 'continuations': 0, 'from buffers': 0}
    for p, s in stripped.items():
        names = {n: (0, '', n, -1) for n in LOGGERS}
        names.update(shared)
        names.update(local.get(p, {}))
        bodies = functions(s)
        pat = re.compile(r'\b(%s)\s*\(' % '|'.join(sorted(map(re.escape, names), key=len, reverse=True)))
        for m in pat.finditer(s):
            if m.start() in skip[p]:
                continue
            ahead = s[max(0, m.start() - 40):m.start()]
            if re.search(r'(?:\bvoid|#define)\s*\**\s*$', ahead):
                continue   # a declaration or definition
            idx, lead, logger, _ = names[m.group(1)]
            args = call_args(s, m.end() - 1)
            if idx >= len(args):
                continue
            arg = args[idx]
            where = f'{p}:{line_of(s, m.start())}'
            counts['calls'] += 1
            if re.match(r'\s*"', arg):
                texts = [lead + literal_text(re.match(RUN, arg).group(0))]
            elif re.search(r'\?', arg) and re.search(LIT, arg):
                texts = [lead + literal_text(r) for r in re.findall(RUN, arg)]
            elif lead:
                texts = [lead]
            else:
                unseen.append(f'{where}: format {arg[:60]}')
                continue
            for text in texts:
                pre = prefix_of(text, raw=logger == 'rt_logf')
                if pre is None:
                    # the text of the first argument in place of the conversion: string literals in it (one per
                    # branch of a ?:), or what the function wrote into the buffer it names
                    first = args[idx + 1].strip() if len(args) > idx + 1 else ''
                    rest = text[len(re.match(CONVERSION, text).group(0)):]
                    if re.search(LIT, first):
                        filled = [literal_text(r) for r in re.findall(RUN, first)]
                    elif re.fullmatch(r'\w+', first):
                        body = next((b for _, _, b, e in bodies if b < m.start() < e), 0)
                        filled = filled_with(s, body, m.start(), first)
                    else:
                        filled = []
                    pres = {prefix_of(t + rest, raw=logger == 'rt_logf') for t in filled} - {None, ''}
                    if not pres:
                        unseen.append(f'{where}: "{text[:60]}" with {first[:40]}')
                        continue
                    counts['from buffers'] += 1
                    for q in pres:
                        used.setdefault(q, []).append(where)
                elif pre == '':
                    counts['continuations'] += 1
                else:
                    used.setdefault(pre, []).append(where)
    return used, unseen, counts


def table(doc):
    """({prefix: line} of the main table, {prefix: line} of the table of lines the check cannot see, problems). A row's
    prefixes are the code spans of its first cell."""
    main, extra, problems, section = {}, {}, [], None
    for n, line in enumerate(doc.split('\n'), 1):
        if line.startswith('## '):
            section = 'main' if line.strip() == MAIN_HEADING else 'unseen' if line.strip() == UNSEEN_HEADING else None
            continue
        cells = line.split('|')
        if section is None or len(cells) < 3 or cells[0].strip() or not cells[1].strip().startswith('`'):
            continue
        into = main if section == 'main' else extra
        for pre in re.findall(r'`([^`]+)`', cells[1]):   # a row may name several prefixes: `a:`, `b:`
            if pre in into:
                problems.append(f'{DOC}:{n}: `{pre}` has a second row (the first at line {into[pre]})')
            into.setdefault(pre, n)
    if not main:
        problems.append(f'{DOC}: no table under "{MAIN_HEADING}"')
    return main, extra, problems


def check(sources, doc):
    """(problems, warnings, used prefixes, unseen calls, counts)."""
    used, unseen, counts = scan(sources)
    main, extra, problems = table(doc)
    for pre in sorted(used, key=str.lower):
        if pre not in main and pre not in extra:
            at = used[pre]
            problems.append(f'{at[0]}: the log prefix `{pre}` has no row in {DOC} '
                            f'({len(at)} call{"" if len(at) == 1 else "s"}: {", ".join(at[:3])}{", ..." if len(at) > 3 else ""})')
    warnings = [f'{DOC}:{n}: `{pre}` is in the table but no log call in runtime/ writes it'
                for pre, n in sorted(main.items(), key=lambda x: x[1]) if pre not in used]
    return problems, warnings, used, unseen, counts, len(main), len(extra)


def read_repo(repo):
    rt = repo / 'runtime'
    files = sorted(p for p in rt.rglob('*') if p.suffix in ('.c', '.cpp', '.h', '.inc') and 'third_party' not in p.parts
                   and not p.name.endswith('_spv.h'))
    sources = {p.relative_to(repo).as_posix(): p.read_text(encoding='utf-8-sig', errors='replace') for p in files}
    doc = repo / DOC
    return sources, doc.read_text(encoding='utf-8') if doc.exists() else ''


def self_test(repo):
    """Each kind of fault, put into a copy of the doc or a made-up source file, must be reported."""
    sources, doc = read_repo(repo)
    base, base_warn, *_ = check(sources, doc)
    bad = 0
    if base:
        print(f'self-test: the repository itself has {len(base)} problems; the cases below are judged on new ones')

    def case(what, want, extra=None, doc_text=None, warn=False):
        nonlocal bad
        got, got_warn, *_ = check({**sources, **(extra or {})}, doc if doc_text is None else doc_text)
        new = [p for p in (got_warn if warn else got) if p not in (base_warn if warn else base)]
        ok = any(re.search(want, p) for p in new)
        print(f'self-test: {what}: {"caught" if ok else "MISSED"}')
        if not ok:
            bad += 1
            for p in new:
                print('   ', p)

    fake = r'''
#define MY_ONCE(...) do { static int once_; if (!once_++) rt_log("made up wrapped: " __VA_ARGS__); } while (0)
static void say_it(int n, const char *fmt) { rt_log(fmt, n); }
void fake(int a) {
    rt_log("made up prefix: %d things", a);
    rt_log(a ? "made up yes: on" : "made up no: off");
    rt_log("madeupword %d without a colon", a);
    rt_log("  a continuation: not a prefix");
    rt_logf("made up raw: %d\n", a);
    rt_logf("made up piece without a prefix %d", a);
    MY_ONCE("%d", a);
    say_it(a, "made up passed on: %d");
    char line[64];
    snprintf(line, sizeof line, "made up buffer: %d", a);
    rt_log("%s", line);
}
'''
    extra = {'runtime/fake_check.c': fake}
    case('a new prefix', r'`made up prefix:` has no row', extra)
    case('both prefixes of a ?: format', r'`made up no:` has no row', extra)
    case('a prefix without a colon', r'`madeupword` has no row', extra)
    case('an rt_logf line with a prefix', r'`made up raw:` has no row', extra)
    case('a macro with a prefix of its own', r'`made up wrapped:` has no row', extra)
    case('a function that passes its format on', r'`made up passed on:` has no row', extra)
    case('a buffer filled with snprintf', r'`made up buffer:` has no row', extra)
    got, *_ = check({**sources, **extra}, doc)
    for wrong in ('`a continuation:`', '`made`', '`made up piece'):
        if any(wrong in p for p in got):
            print(f'self-test: {wrong} reported as a prefix: WRONG')
            bad += 1
    row = re.search(r'^\|\s*`vk_pipeline:`.*\n', doc, re.M)
    if row:
        case('a used prefix whose row is gone', r'`vk_pipeline:` has no row', doc_text=doc.replace(row.group(0), ''))
    else:
        print('self-test: no `vk_pipeline:` row to remove: MISSED')
        bad += 1
    i = doc.find('\n', doc.find('|---', doc.find(MAIN_HEADING))) + 1
    case('a row no call writes', r'`made up unused:` is in the table but no log call',
         doc_text=doc[:i] + '| `made up unused:` | nothing | none |\n' + doc[i:], warn=True)
    case('a row written twice', r'`vk:` has a second row', doc_text=doc[:i] + '| `vk:` | again | none |\n' + doc[i:])
    print(f'log_prefixes_check self-test: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


def main():
    args = sys.argv[1:]
    repo = Path(__file__).resolve().parents[1]
    rest = [a for a in args if not a.startswith('--')]
    if rest:
        repo = Path(rest[0])
    if '--self-test' in args:
        return self_test(repo)
    problems, warnings, used, unseen, counts, nmain, nextra = check(*read_repo(repo))
    for p in problems:
        print(p)
    for w in warnings:
        print('warning:', w)
    if '--unseen' in args:
        for u in unseen:
            print('unseen:', u)
    print(f'log_prefixes_check: {counts["calls"]} log calls, {len(used)} prefixes ({counts["from buffers"]} read from a '
          f'buffer the function filled, {counts["continuations"]} continuations and rt_logf pieces, {len(unseen)} not '
          f'seen); {DOC}: {nmain} rows, {nextra} for lines the check cannot see; {len(problems)} '
          f'problem{"" if len(problems) == 1 else "s"}, {len(warnings)} warning{"" if len(warnings) == 1 else "s"}')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
