# args: --self-test
import argparse
import ast
import bisect
import fnmatch
import io
import re
import shutil
import string
import subprocess
import sys
import tempfile
import tokenize
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from check_content import GENERATED_MARKER  # noqa: E402

USAGE = '''Count the comments left in the source code, per area, against the rule in AGENTS.md ("No comments in the
code"): source code has no comments, the explanation lives in docs/code/. The comments are removed area by area; until
the last area is done the check only warns. docs/code/README.md ("The checks") says what is read and what is kept.

  python tests/source_comments_check.py                 the table per area; exit 0 (warning mode)
  python tests/source_comments_check.py --strict [P...] exit 1 when a comment outside the exceptions is left, in the
                                                        areas or paths P (all of them when none is given)
  python tests/source_comments_check.py --list [P...]   each comment line counted, as path:line: text
  python tests/source_comments_check.py --kept [P...]   each line kept, with the exception that keeps it
  python tests/source_comments_check.py --exceptions    the exceptions and the reason for each
  python tests/source_comments_check.py --self-test     planted cases first, then the table'''

STRICT = False

TREES = ('runtime', 'recomp', 'tools', 'launcher', 'tests')

C_LIKE = {'.c', '.h', '.cpp', '.hpp', '.cc', '.inc', '.cs', '.comp', '.vert', '.frag', '.glsl', '.mesh', '.task',
          '.geom', '.tesc', '.tese', '.rc'}
PYTHON = {'.py', '.pyw'}
SPVASM = {'.spvasm'}

EXCLUDED = (
    ('tools/vendor/*', 'vendored code, kept byte-identical to its source (AGENTS.md, "Reusing code")'),
    ('runtime/third_party/*', 'third-party libraries, unmodified, each with its licence (THIRD_PARTY_NOTICES.md)'),
    ('runtime/shaders/ffx/*', 'AMD FidelityFX headers, unmodified (MIT, THIRD_PARTY_NOTICES.md)'),
    ('runtime/shaders/ffx3/*', 'AMD FidelityFX SDK headers, unmodified (MIT, THIRD_PARTY_NOTICES.md)'),
    ('runtime/nvn/nvn.h', 'the NVN type definitions, another project\'s header, unmodified (MIT, '
                          'THIRD_PARTY_NOTICES.md)'),
    ('runtime/shaders/smaa.glsl', 'the SMAA reference implementation, unmodified (MIT, THIRD_PARTY_NOTICES.md)'),
    ('tools/xlate/*', 'the shader translator: its identity (tools/build_xlate.py identity()) reads the patch files '
                      'byte for byte, and a new identity makes every player prepare the shaders again; its comments '
                      'go with the shader translator\'s next identity change'),
    ('recomp/patches-*.txt', 'data, not code: recomp/patches.py reads these files, # lines included'),
    ('generated/*', 'generated from game code, never committed (AGENTS.md, "Game content boundary")'),
    ('build/*', 'build output, never committed'),
    ('*_spv.h', 'shader headers that tools/build_shaders.py writes, never committed'),
)

OWED = (
    (('runtime/cpu.h', 'runtime/simd.h', 'runtime/simd_fp.h'), '2026-11-24',
     'inputs of the lifted objects\' cache',
     'every lifted object depends on them and tools/make_game.py compares an object\'s dependencies by SHA-256, so '
     'one changed byte makes every player\'s PC recompile the lifted code',
     'they leave with the next change that rebuilds the lifted code'),
    (('tools/cheats.py', 'tools/exefs_patches.py', 'tools/mod_files.py'), '2026-11-24',
     'inputs of the lift key of a game folder with mod code',
     'tools/make_game.py mod_digest() hashes their bytes into that key, so one changed byte makes the PC of every '
     'player with code cheats or ExeFS patches lift the game again',
     'they leave with the next change that alters that key'),
)

KEPT_REASONS = {
    'license': 'the licence header or notice of copied code: the licence requires it (THIRD_PARTY_NOTICES.md)',
    'generated': 'the GENERATED_MARKER of tools/check_content.py, which the content guard looks for',
    'shebang': 'the #! line the system reads to start the script',
    'encoding': 'the coding line Python reads to decode the file (PEP 263)',
    'directive': 'a line a tool or compiler reads: # noqa, # type: ignore, # pragma, # fmt:, # pylint:, the '
                 'tests/run.py markers (# needs:, # args:), // clang-format off/on, // NOLINT, // IWYU pragma, '
                 '// build_shaders: keep functions (tools/build_shaders.py), a fall-through marker (GCC '
                 '-Wimplicit-fallthrough reads it; clang does not)',
    'runtime doc': 'the part of a module docstring the program prints (argparse description=__doc__, '
                   'print(__doc__)); it becomes an explicit string when its area is cleaned',
}

NOT_COMMENTS = ('#pragma, #extension, #version and the other preprocessor directives are code, not comments, and are '
                'never counted; neither are string literals that hold //, /*, # or a URL.')

LICENSE_FULL = re.compile(
    r'Permission is hereby granted|Permission is granted to anyone|THE SOFTWARE IS PROVIDED|The above copyright '
    r'notice|Redistribution and use in source|SPDX-License-Identifier|Licensed under the|All rights reserved|'
    r'This file is part of the FidelityFX SDK|provided .as-is.|must not be misrepresented|Altered source versions|'
    r'This notice may not be removed', re.I)
COPYRIGHT = re.compile(r'Copyright\s*(?:\([cC]\)|©|\d)')
SENTENCE_END = re.compile(r'THIRD_PARTY_NOTICES|\.\)?\s*$')
NOTICE_START = re.compile(r'^[#/*;\s]*(?:Contains code|Portions|Copyright)\b')
LICENSE_FILE = re.compile(r'\((?:MIT|BSD[-\w ]*|Apache[-\w. ]*|zlib|ISC|MPL[-\w. ]*),\s*'
                          r'((?:[\w.-]+/)*(?:LICEN[SC]E|COPYING)(?:\.txt|\.md)?)\)')

PY_DIRECTIVE = re.compile(r'#\s*(?:noqa\b|type:\s*ignore\b|pragma\b|fmt:\s*(?:off|on|skip)\b|isort:|pylint:|mypy:|'
                          r'pyright:|nosec\b)')
RUN_MARKER = re.compile(r'#\s(?:needs|args):')
C_DIRECTIVE = re.compile(r'clang-format\s+(?:off|on)\b|\bNOLINT(?:NEXTLINE|BEGIN|END)?\b|IWYU pragma:|'
                         r'^build_shaders: keep functions$|'
                         r'^[ \t.!]*(?:else,? |intentional(?:ly)? )?fall(?:s | |-)?thr(?:ough|u)[ \t.!]*(?:-.*)?$',
                         re.I)
CODING = re.compile(r'^[ \t\f]*#.*?coding[:=][ \t]*[-\w.]+')

RAW_STRING = re.compile(r'(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(')
INCLUDE_ANGLE = re.compile(r'[ \t]*#[ \t]*include[ \t]*<[^>\n]*>')
PP_TEXT = re.compile(r'[ \t]*#[ \t]*(?:error|warning)\b[^\n]*')


def excluded(rel):
    for pattern, reason in EXCLUDED:
        if fnmatch.fnmatchcase(rel, pattern):
            return pattern, reason
    return None


def owed(rel):
    for entry in OWED:
        if rel in entry[0]:
            return entry
    return None


def area_of(rel):
    parts = rel.split('/')
    if parts[0] == 'runtime':
        return 'runtime (top level)' if len(parts) == 2 else f'runtime/{parts[1]}'
    return parts[0]


def line_starts(text):
    starts = [0]
    for m in re.finditer('\n', text):
        starts.append(m.end())
    return starts


def skip_quoted(text, i, quote):
    n = len(text)
    j = i + 1
    while j < n:
        c = text[j]
        if c == '\\':
            j += 2
            continue
        if c == quote:
            return j + 1
        if c == '\n':
            return j if quote == '"' else i + 1
        j += 1
    return n


def c_comments(text, csharp=False):
    out = []
    i, n = 0, len(text)
    at_line_start = True
    while i < n:
        c = text[i]
        if at_line_start:
            m = INCLUDE_ANGLE.match(text, i) or PP_TEXT.match(text, i)
            if m:
                i = m.end()
                at_line_start = False
                continue
        if c == '\n':
            at_line_start = True
            i += 1
            continue
        if c in ' \t\r\f':
            i += 1
            continue
        at_line_start = False
        two = text[i:i + 2]
        if two == '//':
            j = i
            while True:
                k = text.find('\n', j)
                if k < 0:
                    k = n
                    break
                if text[k - 1] == '\\' or text[k - 2:k] == '\\\r':
                    j = k + 1
                    continue
                break
            out.append((i, k))
            i = k
            continue
        if two == '/*':
            k = text.find('*/', i + 2)
            end = n if k < 0 else k + 2
            out.append((i, end))
            i = end
            continue
        if c in 'uULR' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            m = RAW_STRING.match(text, i)
            if m:
                close = text.find(')' + m.group(1) + '"', m.end())
                i = n if close < 0 else close + len(m.group(1)) + 2
                continue
        if csharp and (two == '@"' or text[i:i + 3] in ('$@"', '@$"')):
            j = text.index('"', i) + 1
            while j < n:
                if text[j] == '"':
                    if text[j + 1:j + 2] == '"':
                        j += 2
                        continue
                    j += 1
                    break
                j += 1
            i = j
            continue
        if c == '"':
            i = skip_quoted(text, i, '"')
            continue
        if c == "'":
            prev = text[i - 1] if i else ''
            nxt = text[i + 1] if i + 1 < n else ''
            if prev and nxt and prev in string.hexdigits and nxt in string.hexdigits and not csharp:
                i += 1
                continue
            i = skip_quoted(text, i, "'")
            continue
        if c.isalnum() or c == '_':
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] == '_'):
                j += 1
            i = j
            continue
        i += 1
    return out


def spvasm_comments(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == ';':
            k = text.find('\n', i)
            k = n if k < 0 else k
            out.append((i, k))
            i = k
            continue
        if c == '"':
            i = skip_quoted(text, i, '"')
            continue
        i += 1
    return out


def comment_lines(text, spans):
    starts = line_starts(text)
    lines = {}
    for a, b in spans:
        first = bisect.bisect_right(starts, a)
        last = bisect.bisect_right(starts, max(a, b - 1))
        for ln in range(first, last + 1):
            s = max(a, starts[ln - 1])
            e = min(b, starts[ln] - 1 if ln < len(starts) else len(text))
            piece = text[s:e]
            piece = re.sub(r'^\s*(?://+|/\*+|;+)', '', piece) if s == a else re.sub(r'^\s*\*+(?!/)', '', piece)
            piece = re.sub(r'\*+/\s*$', '', piece)
            piece = piece.rstrip('\\').strip()
            lines[ln] = f'{lines[ln]} {piece}'.strip() if ln in lines else piece
    return lines


def python_parts(text):
    comments, docs, doc_kept = {}, {}, set()
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (tokenize.TokenError, SyntaxError, IndentationError):
        tokens = []
    for tok in tokens:
        if tok.type == tokenize.COMMENT:
            ln = tok.start[0]
            body = tok.string
            comments[ln] = f'{comments[ln]} {body}' if ln in comments else body
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return comments, docs, doc_kept, False
    src_lines = text.split('\n')
    for node in ast.walk(tree):
        if isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant) and isinstance(node.value.value, str):
            for ln in range(node.lineno, (node.end_lineno or node.lineno) + 1):
                docs[ln] = src_lines[ln - 1].strip().strip('"\'').strip() if ln <= len(src_lines) else ''
    module_doc = tree.body[0] if tree.body and isinstance(tree.body[0], ast.Expr) and \
        isinstance(tree.body[0].value, ast.Constant) and isinstance(tree.body[0].value.value, str) else None
    if module_doc is not None:
        used = runtime_doc_use(tree)
        if used:
            value = module_doc.value.value
            start, end = module_doc.lineno, module_doc.end_lineno or module_doc.lineno
            if used == 'whole':
                doc_kept.update(range(start, end + 1))
            else:
                value_lines = value.split('\n')
                if used == 'first line':
                    take = 1
                else:
                    take = next((k for k, v in enumerate(value_lines) if not v.strip() and k), len(value_lines))
                last = min(end, start + take - 1)
                if last == end - 1 and src_lines[end - 1].strip().strip('"\'') == '':
                    last = end
                doc_kept.update(range(start, last + 1))
    return comments, docs, doc_kept, True


def runtime_doc_use(tree):
    parents = {}
    for node in ast.walk(tree):
        for child in ast.iter_child_nodes(node):
            parents[child] = node
    uses = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Name) and node.id == '__doc__':
            up = parents.get(node)
            kind = 'whole'
            if isinstance(up, ast.Attribute) and up.attr in ('splitlines', 'split', 'strip'):
                call = parents.get(up)
                sub = parents.get(call)
                arg = call.args[0].value if isinstance(call, ast.Call) and call.args and \
                    isinstance(call.args[0], ast.Constant) else None
                first = isinstance(sub, ast.Subscript) and isinstance(sub.slice, ast.Constant) and sub.slice.value == 0
                if first and (up.attr == 'splitlines' or arg == '\n'):
                    kind = 'first line'
                elif first and arg == '\n\n':
                    kind = 'first paragraph'
            uses.append(kind)
    if not uses:
        return None
    if 'whole' in uses:
        return 'whole'
    return 'first paragraph' if 'first paragraph' in uses else 'first line'


def license_lines(lines):
    kept = set()
    nums = sorted(lines)
    runs, run = [], []
    for ln in nums:
        if run and ln != run[-1] + 1:
            runs.append(run)
            run = []
        run.append(ln)
    if run:
        runs.append(run)
    for run in runs:
        paras, para = [], []
        for ln in run:
            if not lines[ln].strip('#/*; \t'):
                if para:
                    paras.append(para)
                para = []
                continue
            para.append(ln)
        if para:
            paras.append(para)
        for para in paras:
            if any(LICENSE_FULL.search(lines[ln]) for ln in para):
                kept.update(para)
                continue
            k = 0
            while k < len(para):
                if COPYRIGHT.search(lines[para[k]]):
                    j = k
                    while j < len(para) - 1 and not SENTENCE_END.search(lines[para[j]]):
                        j += 1
                    s = k
                    while (s > 0 and not NOTICE_START.search(lines[para[k]]) and para[s - 1] not in kept
                           and not SENTENCE_END.search(lines[para[s - 1]])):
                        s -= 1
                    kept.update(para[s:j + 1])
                    k = j + 1
                else:
                    k += 1
        if kept:
            for idx, ln in enumerate(run):
                if ln in kept or lines[ln].strip('#/*; \t'):
                    continue
                before = next((x for x in reversed(run[:idx]) if lines[x].strip('#/*; \t')), None)
                after = next((x for x in run[idx + 1:] if lines[x].strip('#/*; \t')), None)
                if before in kept or after in kept:
                    kept.add(ln)
    return kept


def names_license_file(content, rel, root):
    m = LICENSE_FILE.search(content)
    if not m or root is None:
        return False
    return any((base / m.group(1)).is_file() for base in ((root / rel).parent, root))


def classify(rel, text, root=None):
    suffix = Path(rel).suffix.lower()
    kept = {}
    counted = {}
    if GENERATED_MARKER in text:
        return counted, {0: ('generated', 'the whole file carries GENERATED_MARKER')}, True
    if suffix in PYTHON:
        comments, docs, doc_kept, parsed = python_parts(text)
        lic = license_lines(comments) | license_lines(docs)
        for ln, body in comments.items():
            content = body.lstrip('#').strip()
            if ln == 1 and body.startswith('#!'):
                kept[ln] = ('shebang', body)
            elif ln <= 2 and CODING.match(body):
                kept[ln] = ('encoding', body)
            elif PY_DIRECTIVE.search(body) or (rel.startswith('tests/') and RUN_MARKER.match(body)):
                kept[ln] = ('directive', body)
            elif GENERATED_MARKER in body:
                kept[ln] = ('generated', body)
            elif ln in lic:
                kept[ln] = ('license', body)
            else:
                counted[ln] = content
        for ln, body in docs.items():
            if ln in doc_kept:
                kept.setdefault(ln, ('runtime doc', body))
            elif ln in lic:
                kept.setdefault(ln, ('license', body))
            elif ln not in kept:
                counted[ln] = body
        return counted, kept, parsed
    if suffix in C_LIKE:
        spans = c_comments(text, csharp=suffix == '.cs')
    elif suffix in SPVASM:
        spans = spvasm_comments(text)
    else:
        return counted, kept, True
    lines = comment_lines(text, spans)
    lic = license_lines(lines) | {ln for ln, content in lines.items() if names_license_file(content, rel, root)}
    for ln, content in lines.items():
        if C_DIRECTIVE.search(content):
            kept[ln] = ('directive', content)
        elif GENERATED_MARKER in content:
            kept[ln] = ('generated', content)
        elif ln in lic:
            kept[ln] = ('license', content)
        else:
            counted[ln] = content
    return counted, kept, True


def source_files(root):
    if (root / '.git').exists() and shutil.which('git'):
        r = subprocess.run(['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others', '--exclude-standard',
                            '--', *TREES], capture_output=True)
        if r.returncode == 0:
            names = [p for p in r.stdout.decode('utf-8', errors='replace').split('\0') if p]
            return sorted(p for p in names if (root / p).is_file())
    found = []
    for tree in TREES:
        base = root / tree
        if base.is_dir():
            found.extend(p.relative_to(root).as_posix() for p in base.rglob('*') if p.is_file()
                         and '__pycache__' not in p.parts)
    return sorted(found)


def scan(root):
    report = {'areas': {}, 'kept': {}, 'excluded': {}, 'files': {}, 'unparsed': [], 'owed': {}}
    for rel in source_files(root):
        suffix = Path(rel).suffix.lower()
        ex = excluded(rel)
        if suffix not in C_LIKE | PYTHON | SPVASM:
            if ex:
                report['excluded'].setdefault(ex[0], [0, 0])
                report['excluded'][ex[0]][0] += 1
            continue
        try:
            text = (root / rel).read_text(encoding='utf-8-sig', errors='replace')
        except OSError:
            continue
        counted, kept, parsed = classify(rel, text, root)
        if ex:
            entry = report['excluded'].setdefault(ex[0], [0, 0])
            entry[0] += 1
            entry[1] += len(counted) + len([k for k in kept if k])
            continue
        if not parsed:
            report['unparsed'].append(rel)
        area = area_of(rel)
        a = report['areas'].setdefault(area, {'files': 0, 'with': 0, 'lines': 0})
        a['files'] += 1
        if owed(rel):
            report['owed'][rel] = len(counted)
            counted = {}
        if counted:
            a['with'] += 1
            a['lines'] += len(counted)
        for ln, (reason, body) in kept.items():
            report['kept'].setdefault(reason, []).append((rel, ln, body))
        report['files'][rel] = counted
    return report


def selected(rel, filters):
    if not filters:
        return True
    for f in filters:
        f = f.rstrip('/')
        if rel == f or rel.startswith(f + '/') or area_of(rel) == f:
            return True
    return False


def print_table(report):
    areas = report['areas']
    width = max([len(a) for a in areas] + [20])
    print(f'{"area":{width}}  {"files":>6}  {"with comments":>13}  {"comment lines":>13}')
    total_files = total_with = total_lines = 0
    for name, a in sorted(areas.items(), key=lambda x: (-x[1]['lines'], x[0])):
        print(f'{name:{width}}  {a["files"]:>6}  {a["with"]:>13}  {a["lines"]:>13,}')
        total_files += a['files']
        total_with += a['with']
        total_lines += a['lines']
    print(f'{"total":{width}}  {total_files:>6}  {total_with:>13}  {total_lines:>13,}')
    kept = report['kept']
    print('kept: ' + ', '.join(f'{reason} {len(kept.get(reason, ())):,}' for reason in KEPT_REASONS))
    print('not read: ' + ', '.join(f'{pattern} ({files} files, {lines:,} comment lines)'
                                   for pattern, (files, lines) in sorted(report['excluded'].items())))
    for files, since, what, why, until in OWED:
        there = [rel for rel in files if rel in report['owed']]
        if there:
            lines = sum(report['owed'][rel] for rel in there)
            print(f'kept with comments: {len(there)} files, {lines:,} comment lines ({what}; {until}; since '
                  f'{since}): {", ".join(there)}')
    if report['unparsed']:
        print(f'docstrings not read (the file does not parse): {", ".join(report["unparsed"])}')
    return total_lines


def print_exceptions():
    print('Files and folders not read:')
    for pattern, reason in EXCLUDED:
        print(f'  {pattern}: {reason}')
    print('Files of ours kept with their comments for now, counted and named at every run:')
    for files, since, what, why, until in OWED:
        print(f'  {", ".join(files)} (since {since}): {what}: {why}; {until}')
    print('Lines kept in the files read:')
    for name, reason in KEPT_REASONS.items():
        print(f'  {name}: {reason}')
    print(f'Not comments: {NOT_COMMENTS}')


def self_test():
    cases = {
        'runtime/audio/plant.c': (
            '// Contains code Copyright (c) 2026 Ana Souza (MIT; see\n'
            '// THIRD_PARTY_NOTICES.md).\n'
            '// a plain comment, counted\n'
            '#include <a//b.h>\n'
            '#pragma once\n'
            'static const char *u = "http://example.com/a//b /* not a comment */";\n'
            "static const char q = '\"';\n"
            'static const char *r = "\\" // still a string";\n'
            'int f(int x) {\n'
            '    switch (x) {\n'
            '    case 1: x++;   // fall through\n'
            '    case 2: return x;   /* a trailing block, counted */\n'
            '    }\n'
            '    // clang-format off\n'
            '    return 0;   // NOLINT\n'
            '}\n'
            '/*\n'
            ' * two lines of a block,\n'
            ' * both counted\n'
            ' */\n'),
        'runtime/diag/plant.c': (
            '// What the file does, counted.\n'
            '// The declarations (Alpha, Beta and the\n'
            '// Gamma signatures) contain code Copyright (c) 2020 Ana Souza (MIT; see THIRD_PARTY_NOTICES.md).\n'
            'int g;\n'
            '// ---- a heading, counted\n'
            '// Contains code Copyright (c) 2021 Ana Souza (MIT; see THIRD_PARTY_NOTICES.md).\n'
            'int h;\n'
            '// lib/a.h is another project\'s header, unmodified (MIT, lib/LICENSE.txt).\n'
            '// lib/b.h is said to be one too (MIT, lib/MISSING.txt), counted\n'
            '// gone/c.h names a licence file that is not there (MIT, gone/LICENSE.txt), counted\n'
            'int k;\n'),
        'runtime/diag/lib/LICENSE.txt': 'the licence text\n',
        'runtime/shaders/plant.frag': (
            '#version 450\n'
            '// build_shaders: keep functions\n'
            '//\n'
            '// Copyright (c) 2025 Ana Souza\n'
            '//\n'
            '// Permission is hereby granted, free of charge, to any person obtaining a copy of this software\n'
            '//\n'
            '// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.\n'
            '//\n'
            '// What the shader does, counted.\n'
            'void main() {}\n'),
        'runtime/plant.spvasm': (
            '; the SPIR-V comment, counted\n'
            '               OpCapability Shader\n'
            '               OpString "; not a comment"\n'),
        'tools/plant.py': (
            '#!/usr/bin/env python3\n'
            '# -*- coding: utf-8 -*-\n'
            '"""The first line, used by --help.\n'
            '\n'
            'The rest of the docstring, counted.\n'
            '"""\n'
            'import argparse  # noqa: F401\n'
            'URL = "https://example.com/#anchor"   # a trailing comment, counted\n'
            "HASH = '# not a comment'\n"
            'TEMPLATE = f"{URL}#frag"\n'
            '\n'
            '\n'
            'def f():\n'
            '    """A function docstring, counted."""\n'
            '    return argparse.ArgumentParser(description=__doc__.splitlines()[0])\n'),
        'tools/whole.py': (
            '"""Printed whole by print(__doc__).\n'
            '\n'
            'Both paragraphs."""\n'
            'print(__doc__)\n'),
        'tests/plant_check.py': (
            '#' + ' needs: git\n'
            '#' + ' args: --self-test\n'
            'X = 1\n'),
        'tools/vendor/lib/plant.c': '// vendored, not read\n',
        'tools/xlate/Plant.cs': '// the translator, not read\n',
        'recomp/patches-v999.txt': '# data the generator reads\n',
        'runtime/third_party/lib/plant.h': '// third party, not read\n',
        'runtime/shaders/ffx/plant.h': '// AMD, not read\n',
        'runtime/nvn/nvn.h': '// the NVN types, not read\n',
        'runtime/shaders/smaa.glsl': '// the SMAA reference implementation, not read\n',
        'runtime/gen_plant.c': '// ' + GENERATED_MARKER + '\n// a generated file, not counted\n',
        'runtime/simd_fp.h': '// a plain comment in a file kept with its comments\nint x; // and another\n',
        'runtime/plant_top.c': '// a plain comment beside it\n',
    }
    want_counted = {
        'runtime/audio/plant.c': {3, 12, 17, 18, 19, 20},
        'runtime/diag/plant.c': {1, 5, 9, 10},
        'runtime/shaders/plant.frag': {10},
        'runtime/plant.spvasm': {1},
        'tools/plant.py': {4, 5, 6, 8, 14},
        'tools/whole.py': set(),
        'tests/plant_check.py': set(),
        'runtime/gen_plant.c': set(),
        'runtime/simd_fp.h': set(),
        'runtime/plant_top.c': {1},
    }
    want_kept = {
        ('runtime/audio/plant.c', 1): 'license', ('runtime/audio/plant.c', 2): 'license',
        ('runtime/audio/plant.c', 11): 'directive', ('runtime/audio/plant.c', 14): 'directive',
        ('runtime/audio/plant.c', 15): 'directive',
        ('runtime/diag/plant.c', 2): 'license', ('runtime/diag/plant.c', 3): 'license',
        ('runtime/diag/plant.c', 6): 'license', ('runtime/diag/plant.c', 8): 'license',
        ('runtime/shaders/plant.frag', 2): 'directive', ('runtime/shaders/plant.frag', 3): 'license',
        ('runtime/shaders/plant.frag', 4): 'license', ('runtime/shaders/plant.frag', 9): 'license',
        ('runtime/shaders/plant.frag', 6): 'license', ('runtime/shaders/plant.frag', 8): 'license',
        ('tools/plant.py', 1): 'shebang', ('tools/plant.py', 2): 'encoding', ('tools/plant.py', 3): 'runtime doc',
        ('tools/plant.py', 7): 'directive',
        ('tools/whole.py', 1): 'runtime doc', ('tools/whole.py', 2): 'runtime doc',
        ('tools/whole.py', 3): 'runtime doc',
        ('tests/plant_check.py', 1): 'directive', ('tests/plant_check.py', 2): 'directive',
    }
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in cases.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding='utf-8')
        report = scan(root)
        for rel, want in want_counted.items():
            got = set(report['files'].get(rel, {}))
            ok = got == want
            print(f'self-test: {rel}: counted lines {sorted(got)}: {"ok" if ok else f"WRONG, want {sorted(want)}"}')
            bad += not ok
        kept = {(rel, ln): reason for reason, items in report['kept'].items() for rel, ln, _ in items}
        for key, reason in want_kept.items():
            ok = kept.get(key) == reason
            print(f'self-test: {key[0]}:{key[1]} kept as {reason}: {"ok" if ok else f"WRONG ({kept.get(key)})"}')
            bad += not ok
        for rel in ('tools/vendor/lib/plant.c', 'tools/xlate/Plant.cs', 'runtime/third_party/lib/plant.h',
                    'runtime/shaders/ffx/plant.h', 'runtime/nvn/nvn.h', 'runtime/shaders/smaa.glsl'):
            ok = rel not in report['files'] and excluded(rel) is not None
            print(f'self-test: {rel} not read: {"ok" if ok else "WRONG"}')
            bad += not ok
        ok = excluded('recomp/patches-v999.txt') is not None and 'recomp/patches-v999.txt' not in report['files']
        print(f'self-test: recomp/patches-v999.txt not read: {"ok" if ok else "WRONG"}')
        bad += not ok
        got = report['owed']
        ok = got == {'runtime/simd_fp.h': 2}
        print(f'self-test: runtime/simd_fp.h kept with its 2 comment lines, outside its area\'s count: '
              f'{"ok" if ok else f"WRONG ({got})"}')
        bad += not ok
        strict = run_strict(report, ['runtime/simd_fp.h'])
        print(f'self-test: --strict runtime/simd_fp.h passes: {"ok" if strict == 0 else "WRONG"}')
        bad += strict != 0
        strict = run_strict(report, ['runtime/plant_top.c'])
        print(f'self-test: --strict runtime/plant_top.c fails on the plain comment beside it: '
              f'{"ok" if strict == 1 else "WRONG"}')
        bad += strict != 1
        strict = run_strict(report, ['runtime/audio'])
        print(f'self-test: --strict runtime/audio fails on the planted comment: {"ok" if strict == 1 else "WRONG"}')
        bad += strict != 1
        strict = run_strict(report, ['tests'])
        print(f'self-test: --strict tests passes with only directives there: {"ok" if strict == 0 else "WRONG"}')
        bad += strict != 0
    print(f'source_comments_check self-test: {"FAILED" if bad else "ok"}')
    return bad


def run_strict(report, filters):
    left = sum(len(c) for rel, c in report['files'].items() if selected(rel, filters))
    return 1 if left else 0


def main():
    ap = argparse.ArgumentParser(description=USAGE, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='*', help='areas (runtime/vk, tools) or paths to look at')
    ap.add_argument('--strict', action='store_true', help='exit 1 when a comment outside the exceptions is left')
    ap.add_argument('--list', action='store_true', help='print each comment line counted')
    ap.add_argument('--kept', action='store_true', help='print each line kept, with its exception')
    ap.add_argument('--exceptions', action='store_true', help='print the exceptions and their reasons')
    ap.add_argument('--self-test', action='store_true', help='check the scanner on planted files, then the tree')
    ap.add_argument('--root', default=str(ROOT), help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.exceptions:
        print_exceptions()
        return 0
    failed_self = self_test() if args.self_test else 0
    report = scan(Path(args.root))
    if args.list:
        for rel, counted in report['files'].items():
            if selected(rel, args.paths):
                for ln, text in sorted(counted.items()):
                    print(f'{rel}:{ln}: {text[:150]}')
    if args.kept:
        for reason, items in report['kept'].items():
            for rel, ln, text in items:
                if selected(rel, args.paths):
                    print(f'{reason}: {rel}:{ln}: {text[:150]}')
    total = print_table(report)
    strict = args.strict or STRICT
    left = sum(len(c) for rel, c in report['files'].items() if selected(rel, args.paths))
    scope = ', '.join(args.paths) if args.paths else 'the source trees'
    if strict:
        print(f'source_comments_check: {left:,} comment lines left in {scope}: {"FAILED" if left else "clean"}')
    else:
        print(f'source_comments_check: warning: {total:,} comment lines left (warning mode until the last area is '
              f'done; --strict fails on them)')
    if failed_self:
        return 1
    return 1 if strict and left else 0


if __name__ == '__main__':
    sys.exit(main())
