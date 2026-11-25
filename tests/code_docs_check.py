# needs: git
# args: --self-test
import argparse
import ast
import functools
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from source_comments_check import C_LIKE, PYTHON, TREES, c_comments  # noqa: E402

USAGE = '''Check that every file and function docs/code/ cites exists: the drift between the implementation notes and
the code. The citation format is in docs/code/README.md ("Citing the code"). Until the last area is done the check
only warns.

  python tests/code_docs_check.py                each stale citation of docs/code/*.md; exit 0 (warning mode)
  python tests/code_docs_check.py --strict       exit 1 on a stale citation
  python tests/code_docs_check.py --all-docs     also read docs/*.md
  python tests/code_docs_check.py --self-test    planted citations first, then the docs'''

STRICT = False

DOCS = 'docs/code'

SPAN = re.compile(r'`([^`\n]+)`')
PATH = re.compile(r'(?:%s)/[\w./+-]*' % '|'.join(TREES))
LINE_HINT = re.compile(r':\d+$')
FUNC = re.compile(r'([A-Za-z_]\w*(?:(?:::|\.)[A-Za-z_]\w*)*)\(\)')
BETWEEN = re.compile(r'(?:\s*(?:,|and)?\s*)')
HEADING = re.compile(r'^(#{1,6})\s+(.*?)\s*#*\s*$')
FENCE = re.compile(r'^\s*(```|~~~)')
ELSEWHERE = re.compile(r'^\s*<!--\s*code-docs:\s*other repository\b.*-->\s*$')
AFTER_PARAMS = re.compile(r'\s*(?:const\b\s*|noexcept\b\s*|override\b\s*|final\b\s*|->\s*[\w:<>*&\s]+?)*([{;])')


def as_path(span):
    span = LINE_HINT.sub('', span.strip())
    if not PATH.fullmatch(span) or '..' in span:
        return None
    return span


def spans_only(text, kind):
    rest = SPAN.sub('', text).strip(' ,')
    found = SPAN.findall(text)
    if not found or re.sub(r'\band\b', '', rest).strip(' ,'):
        return []
    out = []
    for s in found:
        if kind == 'path':
            p = as_path(s)
            if p is None:
                return []
            out.append(p)
        else:
            m = FUNC.fullmatch(s.strip())
            if not m:
                return []
            out.append(m.group(1))
    return out


def citations(text):
    found = []
    in_fence = False
    stack = []
    level = 0
    elsewhere = None
    for n, line in enumerate(text.split('\n'), 1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        if ELSEWHERE.match(line):
            elsewhere = level
            continue
        h = HEADING.match(line)
        if h:
            level = len(h.group(1))
            if elsewhere is not None and level <= elsewhere:
                elsewhere = None
            while stack and stack[-1][0] >= level:
                stack.pop()
        if elsewhere is not None:
            continue
        if h:
            paths = spans_only(h.group(2), 'path')
            if len(paths) == 1:
                found.append((n, paths[0], None))
                stack.append((level, paths[0]))
                continue
            funcs = spans_only(h.group(2), 'func')
            if funcs and stack:
                for f in funcs:
                    found.append((n, stack[-1][1], f))
                continue
        spans = list(SPAN.finditer(line))
        k = 0
        while k < len(spans):
            path = as_path(spans[k].group(1))
            if path is None:
                k += 1
                continue
            found.append((n, path, None))
            end = spans[k].end()
            j = k + 1
            while j < len(spans):
                gap = line[end:spans[j].start()]
                m = FUNC.fullmatch(spans[j].group(1).strip())
                if not m or not BETWEEN.fullmatch(gap):
                    break
                found.append((n, path, m.group(1)))
                end = spans[j].end()
                j += 1
            k = j
    return found


def blank_comments(text):
    out = list(text)
    for a, b in c_comments(text):
        for i in range(a, b):
            if out[i] != '\n':
                out[i] = ' '
    return ''.join(out)


def close_paren(text, i):
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def defined_in_c(code, name, header):
    last = re.split(r'::|\.', name)[-1]
    if re.search(r'^[ \t]*#[ \t]*define[ \t]+%s\b' % re.escape(last), code, re.M):
        return True
    for m in re.finditer(r'(?<![\w.>])%s\s*\(' % re.escape(last), code):
        end = close_paren(code, m.end() - 1)
        if end < 0:
            continue
        after = AFTER_PARAMS.match(code, end + 1)
        if after and (after.group(1) == '{' or header):
            line_start = code.rfind('\n', 0, m.start()) + 1
            lead = code[line_start:m.start()]
            if re.search(r'\b(?:return|if|while|for|switch|sizeof)\b|[=,(]\s*$', lead):
                continue
            return True
    return False


def defined_in_python(text, tree, name):
    if tree is None:
        return re.search(r'\b(?:def|class)\s+%s\b' % re.escape(name.split('.')[-1]), text) is not None
    parts = name.split('.')
    kinds = (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
    if len(parts) == 1:
        return any(isinstance(n, kinds) and n.name == parts[0] for n in ast.walk(tree))
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == parts[-2]:
            if any(isinstance(n, kinds) and n.name == parts[-1] for n in node.body):
                return True
    return False


@functools.lru_cache(maxsize=None)
def source_of(path):
    text = path.read_text(encoding='utf-8-sig', errors='replace')
    suffix = path.suffix.lower()
    if suffix in PYTHON:
        try:
            return text, ast.parse(text)
        except SyntaxError:
            return text, None
    return (blank_comments(text) if suffix in C_LIKE else text), None


@functools.lru_cache(maxsize=None)
def defined(path, name):
    text, tree = source_of(path)
    suffix = path.suffix.lower()
    if suffix in PYTHON:
        return defined_in_python(text, tree, name)
    if suffix in C_LIKE:
        return defined_in_c(text, name, header=suffix in ('.h', '.hpp', '.inc'))
    return re.search(r'\b%s\s*\(' % re.escape(name.split('.')[-1].split('::')[-1]), text) is not None


def ignored(root, path):
    try:
        r = subprocess.run(['git', '-C', str(root), 'check-ignore', '-q', '--', path], capture_output=True)
    except OSError:
        return False
    return r.returncode == 0


def check_page(root, page):
    problems, generated = [], []
    rel = page.relative_to(root).as_posix()
    text = page.read_text(encoding='utf-8')
    count = 0
    for n, path, func in citations(text):
        count += 1
        target = root / path
        where = f'{rel}:{n}'
        if not target.exists() and ignored(root, path):
            generated.append(f'{where}: `{path}` is generated (git ignores it): not checked')
            continue
        if func is None:
            if not target.exists():
                problems.append(f'{where}: `{path}` does not exist')
            continue
        if target.is_dir():
            problems.append(f'{where}: `{path}` is a folder: `{func}()` needs a file')
        elif target.is_file() and not defined(target, func):
            problems.append(f'{where}: `{func}()` is not defined in `{path}`')
    return problems, generated, count


def pages(root, all_docs):
    found = sorted((root / DOCS).glob('*.md')) if (root / DOCS).is_dir() else []
    if all_docs:
        found += sorted((root / 'docs').glob('*.md'))
    return found


def check(root, all_docs=False):
    problems, generated, count, read = [], [], 0, 0
    for page in pages(root, all_docs):
        p, g, c = check_page(root, page)
        problems += p
        generated += g
        count += c
        read += 1
    return problems, generated, count, read


def self_test():
    files = {
        'runtime/x/a.c': ('#include "a.h"\n'
                          '#define TWICE(v) ((v) * 2)\n'
                          'static int helper(int v)\n'
                          '{\n'
                          '    return TWICE(v);\n'
                          '}\n'
                          'int x_build(int v) { return helper(v); }\n'
                          '// int ghost(void) { return 0; }\n'
                          'void x_use(void) { x_build(1); }\n'),
        'runtime/x/a.h': 'int x_build(int v);\nint x_only_declared(void);\n',
        'tools/t.py': ('class Box:\n'
                       '    def open(self):\n'
                       '        return 1\n'
                       '\n'
                       '\n'
                       'def main():\n'
                       '    return Box().open()\n'),
        'docs/code/x.md': ('# x\n'
                           '\n'
                           '`runtime/x/a.c` `x_build()`, `helper()` and `TWICE()`: all there.\n'
                           '`runtime/x/a.c:7` `x_build()`: a line hint is fine.\n'
                           '`runtime/x/gone.c` `x_build()`: the file is gone.\n'
                           '`runtime/x/a.c` `ghost()`: only in a comment.\n'
                           '`runtime/x/a.c` `x_use()` calls `missing_but_not_cited()` on its own: not a citation.\n'
                           '`runtime/x/a.h` `x_only_declared()`: a declaration in a header counts.\n'
                           '`runtime/x/` `x_build()`: a folder is not a file.\n'
                           '`tools/t.py` `Box.open()` and `main()`; `tools/t.py` `Box.close()` is not.\n'
                           '`runtime/x/*.c` and `runtime/<area>/` are patterns, not citations.\n'
                           '\n'
                           '## `runtime/x/a.c`\n'
                           '\n'
                           '### `helper()`\n'
                           '\n'
                           '### `renamed()`\n'
                           '\n'
                           '```\n'
                           '`runtime/x/fenced.c` `nothing()`\n'
                           '```\n'
                           '\n'
                           '## Other notes\n'
                           '\n'
                           '### `helper()`\n'
                           '\n'
                           '`runtime/x/gen.h` is written by the build, `runtime/x/gen.h` `made()` too.\n'
                           '\n'
                           '## Another repository\n'
                           '\n'
                           '<!-- code-docs: other repository -->\n'
                           '`launcher/Other.cs` `Run()` is in the other repository.\n'
                           '\n'
                           '### Deeper\n'
                           '\n'
                           '`launcher/also.mjs` too.\n'
                           '\n'
                           '## Back here\n'
                           '\n'
                           '`runtime/x/missing.c` is checked again.\n'),
        '.gitignore': '/runtime/x/gen.h\n',
    }
    want = {
        'docs/code/x.md:5: `runtime/x/gone.c` does not exist',
        'docs/code/x.md:6: `ghost()` is not defined in `runtime/x/a.c`',
        'docs/code/x.md:9: `runtime/x/` is a folder: `x_build()` needs a file',
        'docs/code/x.md:10: `Box.close()` is not defined in `tools/t.py`',
        'docs/code/x.md:17: `renamed()` is not defined in `runtime/x/a.c`',
        'docs/code/x.md:40: `runtime/x/missing.c` does not exist',
    }
    want_generated = {
        'docs/code/x.md:27: `runtime/x/gen.h` is generated (git ignores it): not checked',
    }
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding='utf-8')
        subprocess.run(['git', 'init', '-q', str(root)], check=True)
        problems, generated, count, _ = check(root)
        got = set(problems)
        for w in sorted(want_generated):
            ok = w in generated
            print(f'self-test: {w}: {"ok" if ok else "MISSED"}')
            bad += not ok
        if len(generated) != 3:
            print(f'self-test: {len(generated)} generated citations, want 3 (the path twice and its function): WRONG')
            bad += 1
        for w in sorted(want, key=lambda w: int(w.split(':')[1])):
            ok = w in got
            print(f'self-test: {w}: {"caught" if ok else "MISSED"}')
            bad += not ok
        for g in sorted(got - want):
            print(f'self-test: reported but not planted: {g}')
            bad += 1
        print(f'self-test: {count} citations read')
    print(f'code_docs_check self-test: {"FAILED" if bad else "ok"}')
    return bad


def main():
    ap = argparse.ArgumentParser(description=USAGE, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--strict', action='store_true', help='exit 1 on a stale citation')
    ap.add_argument('--all-docs', action='store_true', help='also read docs/*.md')
    ap.add_argument('--self-test', action='store_true', help='check the reader on planted citations, then the docs')
    ap.add_argument('--root', default=str(ROOT), help=argparse.SUPPRESS)
    args = ap.parse_args()
    failed_self = self_test() if args.self_test else 0
    problems, generated, count, read = check(Path(args.root), args.all_docs)
    for g in generated:
        print(g)
    for p in problems:
        print(p)
    strict = args.strict or STRICT
    label = '' if strict else 'warning: '
    print(f'code_docs_check: {label}{read} pages, {count} citations, {len(generated)} of generated files, '
          f'{len(problems)} stale'
          + ('' if strict else ' (warning mode until the last area is done; --strict fails on them)'))
    if failed_self:
        return 1
    return 1 if strict and problems else 0


if __name__ == '__main__':
    sys.exit(main())
