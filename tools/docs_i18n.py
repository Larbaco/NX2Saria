import argparse
import difflib
import hashlib
import os
import re
import subprocess
import sys
import unicodedata
from pathlib import Path

DESCRIPTION = ("Check the repository's Markdown links and keep the translated player docs in step with their "
               'English sources.')

ROOT = Path(__file__).resolve().parent.parent

LANGS = {
    'en': 'English',
    'pt-BR': 'Português (Brasil)',
    'ru': 'Русский',
    'zh-Hans': '简体中文',
}

ISSUES = 'https://github.com/Larbaco/NX2Saria/issues'

NOTICE = {
    'pt-BR': 'Tradução da [versão em inglês]({src}). Se as duas divergirem, vale a versão em inglês.',
    'ru': 'Перевод [английской версии]({src}). Если они расходятся, действует английская версия.',
    'zh-Hans': '本页译自[英文版]({src})。如两者有出入，以英文版为准。',
}
UNREVIEWED = {
    'pt-BR': f'Esta tradução ainda não foi revisada por um falante nativo. Correções são bem-vindas no '
             f'[rastreador de issues]({ISSUES}).',
    'ru': f'Этот перевод ещё не проверен носителем языка. Исправления приветствуются в [трекере задач]({ISSUES}).',
    'zh-Hans': f'本译文尚未经母语者审校。欢迎在[问题跟踪器]({ISSUES})中提出修改。',
}

SKIP_PREFIXES = ('refs/', 'runtime/third_party/', 'tools/vendor/', 'build/', 'generated/')
ENGLISH_ONLY = ('docs/code/',)

HEADER_RE = re.compile(r'<!-- translation: lang=(\S+) source=(\S+) source-hash=([0-9a-f]{40}) reviewed=(yes|no) -->')
BLOCK_RE = re.compile(r'<!-- languages -->\n(.*?)<!-- /languages -->\n', re.S)
FENCE_RE = re.compile(r'^(```|~~~).*?^\1[^\n]*$', re.S | re.M)
LINK_RES = (
    re.compile(r'\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)'),
    re.compile(r'\b(?:href|src)="([^"]+)"'),
    re.compile(r'^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s|$)', re.M),
)
SCHEME_RE = re.compile(r'^[A-Za-z][A-Za-z0-9+.-]*:')


def rel(p: Path) -> str:
    return p.relative_to(ROOT).as_posix()


def without_block(text: str) -> str:
    return re.sub(BLOCK_RE.pattern + r'\n*', '', text, count=1, flags=BLOCK_RE.flags)


def source_hash(text: str) -> str:
    return hashlib.sha1(without_block(text).encode('utf-8')).hexdigest()


def markdown_files() -> list[Path]:
    out = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard', '*.md'], cwd=ROOT, capture_output=True,
                         text=True, check=True).stdout.split('\n')
    return sorted(ROOT / f for f in out if f and not f.startswith(SKIP_PREFIXES) and (ROOT / f).is_file())


def slug(heading: str) -> str:
    text = re.sub(r'!?\[([^\]]*)\]\([^)]*\)', r'\1', heading)
    text = re.sub(r'<[^>]+>', '', text)
    text = text.strip().lower()
    kept = ''.join(c for c in text if c in ' -' or unicodedata.category(c)[0] in 'LMN' or
                   unicodedata.category(c) == 'Pc')
    return kept.replace(' ', '-')


_anchor_cache: dict[Path, set[str]] = {}


def anchors(path: Path) -> set[str]:
    if path not in _anchor_cache:
        found: set[str] = set()
        counts: dict[str, int] = {}
        text = FENCE_RE.sub('', path.read_text(encoding='utf-8'))
        for line in text.split('\n'):
            m = re.match(r'^ {0,3}#{1,6}\s+(.*?)(?:\s+#+)?\s*$', line)
            if m:
                a = slug(m.group(1))
                n = counts.get(a, 0)
                counts[a] = n + 1
                found.add(a if n == 0 else f'{a}-{n}')
            found.update(re.findall(r'<a\s+(?:name|id)="([^"]+)"', line))
        _anchor_cache[path] = found
    return _anchor_cache[path]


def links(text: str) -> list[str]:
    text = FENCE_RE.sub('', text)
    text = re.sub(r'`[^`\n]*`', '', text)
    text = re.sub(r'<!--.*?-->', '', text, flags=re.S)
    found = []
    for r in LINK_RES:
        found.extend(r.findall(text))
    return found


def resolve(page: Path, target: str) -> tuple[Path, str]:
    path, _, anchor = target.partition('#')
    dest = page if not path else Path(os.path.normpath(page.parent / path))
    return dest, anchor


class Translation:
    def __init__(self, path: Path, lang: str, source: Path, hash_: str, reviewed: bool):
        self.path, self.lang, self.source, self.hash, self.reviewed = path, lang, source, hash_, reviewed

    def stale(self) -> bool:
        return source_hash(self.source.read_text(encoding='utf-8')) != self.hash


def translation_path(source: Path, lang: str) -> Path:
    if source.parent == ROOT:
        return ROOT / f'{source.stem}.{lang}.md'
    return source.parent / lang / source.name


def page_language(path: Path) -> str | None:
    if path.parent == ROOT:
        m = re.match(r'^[^.]+\.(.+)\.md$', path.name)
        return m.group(1) if m and m.group(1) in LANGS else None
    return path.parent.name if path.parent.name in LANGS else None


def read_translations(files: list[Path], errors: list[str]) -> list[Translation]:
    found = []
    for f in files:
        lang = page_language(f)
        first = f.read_text(encoding='utf-8').split('\n', 1)[0]
        m = HEADER_RE.fullmatch(first.strip())
        if lang is None and not m:
            continue
        if not m:
            errors.append(f'{rel(f)}: a translation must start with the header line '
                          f'<!-- translation: lang=.. source=.. source-hash=.. reviewed=.. -->')
            continue
        hlang, src, hash_, reviewed = m.groups()
        source = ROOT / src
        if hlang not in LANGS or hlang == 'en':
            errors.append(f'{rel(f)}: unknown translation language {hlang!r} (known: {", ".join(LANGS)})')
            continue
        if not source.is_file():
            errors.append(f'{rel(f)}: its source {src} does not exist')
            continue
        if src.startswith(ENGLISH_ONLY):
            errors.append(f'{rel(f)}: {src} is a code note, and docs/code/ stays in English only '
                          f'(docs/code/README.md)')
            continue
        if translation_path(source, hlang) != f:
            errors.append(f'{rel(f)}: the {hlang} translation of {src} belongs at {rel(translation_path(source, hlang))}')
            continue
        found.append(Translation(f, hlang, source, hash_, reviewed == 'yes'))
    return found


def link_to(page: Path, target: Path) -> str:
    return Path(os.path.relpath(target, page.parent)).as_posix()


def language_block(page: Path, source: Path, versions: dict[str, Path], tr: Translation | None) -> str:
    lang = tr.lang if tr else 'en'
    html = source.parent == ROOT
    items = []
    for code, name in LANGS.items():
        if code not in versions:
            continue
        if code == lang:
            items.append(f'<strong>{name}</strong>' if html else f'**{name}**')
        else:
            href = link_to(page, versions[code])
            items.append(f'<a href="{href}">{name}</a>' if html else f'[{name}]({href})')
    bar = ' · '.join(items)
    out = f'<p align="center">{bar}</p>\n' if html else f'{bar}\n'
    if tr:
        out += '\n> ' + NOTICE[lang].format(src=link_to(page, source)) + '\n'
        if not tr.reviewed:
            out += '>\n> ' + UNREVIEWED[lang] + '\n'
    return out


def translated_sets(translations: list[Translation]) -> dict[Path, dict[str, Path]]:
    sets: dict[Path, dict[str, Path]] = {}
    for t in translations:
        sets.setdefault(t.source, {'en': t.source})[t.lang] = t.path
    return {s: dict(sorted(v.items(), key=lambda kv: list(LANGS).index(kv[0]))) for s, v in sets.items()}


def expected_blocks(translations: list[Translation]) -> dict[Path, str]:
    by_path = {t.path: t for t in translations}
    want = {}
    for source, versions in translated_sets(translations).items():
        for page in versions.values():
            want[page] = language_block(page, source, versions, by_path.get(page))
    return want


def current_block(text: str) -> str | None:
    m = BLOCK_RE.search(text)
    return m.group(1) if m else None


def with_block(path: Path, text: str, block: str) -> str:
    wrapped = f'<!-- languages -->\n{block}<!-- /languages -->\n'
    if BLOCK_RE.search(text):
        return BLOCK_RE.sub(lambda _: wrapped, text, count=1)
    lines = text.split('\n')
    at = 1 if HEADER_RE.fullmatch(lines[0].strip()) else 0
    return '\n'.join(lines[:at] + [wrapped.rstrip('\n'), ''] + lines[at:])


def cmd_check(strict: bool) -> int:
    errors: list[str] = []
    warnings: list[str] = []
    files = markdown_files()
    translations = read_translations(files, errors)
    sets = translated_sets(translations)

    for page in files:
        text = page.read_text(encoding='utf-8')
        lang = page_language(page)
        body = BLOCK_RE.sub('', text)
        for target in links(text):
            if SCHEME_RE.match(target) or target.startswith('//'):
                continue
            dest, anchor = resolve(page, target)
            if not dest.exists():
                errors.append(f'{rel(page)}: link to a missing file: {target}')
                continue
            if anchor and dest.suffix == '.md' and anchor not in anchors(dest):
                errors.append(f'{rel(page)}: link to a missing anchor: {target}')
        if lang:
            for target in links(body):
                if SCHEME_RE.match(target) or target.startswith('//'):
                    continue
                dest, _ = resolve(page, target)
                own = sets.get(dest, {}).get(lang)
                if own and own != page:
                    errors.append(f'{rel(page)}: links to {rel(dest)}; link to its {lang} version, '
                                  f'{link_to(page, own)}')

    for page, block in expected_blocks(translations).items():
        have = current_block(page.read_text(encoding='utf-8'))
        if have is None:
            errors.append(f'{rel(page)}: no language block (python tools/docs_i18n.py bars --write adds it)')
        elif have != block:
            errors.append(f'{rel(page)}: the language block differs from the generated one '
                          f'(python tools/docs_i18n.py bars --write)')

    for t in translations:
        if t.stale():
            warnings.append(f'{rel(t.path)}: older than {rel(t.source)} (python tools/docs_i18n.py diff {rel(t.path)})')

    for w in warnings:
        print(f'warning: {w}')
    for e in errors:
        print(f'error: {e}')
    failed = bool(errors) or (strict and bool(warnings))
    print(f'{len(files)} Markdown files, {len(translations)} translations: {len(errors)} errors, '
          f'{len(warnings)} stale')
    return 1 if failed else 0


def cmd_status() -> int:
    errors: list[str] = []
    translations = read_translations(markdown_files(), errors)
    by_key = {(t.source, t.lang): t for t in translations}
    langs = [c for c in LANGS if c != 'en']
    print(f'{"source":32}' + ''.join(f'{c:>18}' for c in langs))
    for source in sorted({t.source for t in translations}):
        cells = []
        for c in langs:
            t = by_key.get((source, c))
            cells.append('missing' if not t else ('stale' if t.stale() else 'ok') + ('' if t.reviewed else ', unrev.'))
        print(f'{rel(source):32}' + ''.join(f'{x:>18}' for x in cells))
    for e in errors:
        print(f'error: {e}')
    return 1 if errors else 0


def find_translation(arg: str) -> Translation:
    errors: list[str] = []
    path = Path(arg).resolve()
    for t in read_translations([path], errors):
        return t
    sys.exit(errors[0] if errors else f'{arg}: not a translation')


def cmd_diff(arg: str) -> int:
    t = find_translation(arg)
    src = rel(t.source)
    commits = subprocess.run(['git', 'rev-list', 'HEAD', '--', src], cwd=ROOT, capture_output=True, text=True,
                             check=True).stdout.split()
    for c in commits:
        shown = subprocess.run(['git', 'show', f'{c}:{src}'], cwd=ROOT, capture_output=True)
        if shown.returncode != 0:
            continue
        old = shown.stdout.decode('utf-8')
        if source_hash(old) == t.hash:
            break
    else:
        sys.exit(f'{rel(t.path)}: no commit of {src} in this clone has the recorded version (fetch the full history)')
    new = without_block(t.source.read_text(encoding='utf-8'))
    sys.stdout.writelines(difflib.unified_diff(without_block(old).splitlines(True), new.splitlines(True),
                                               f'{src} at {c[:12]} (translated)', f'{src} (now)'))
    return 0


def cmd_stamp(args: list[str], reviewed: bool) -> int:
    for arg in args:
        t = find_translation(arg)
        text = t.path.read_text(encoding='utf-8')
        first, rest = text.split('\n', 1)
        hash_ = source_hash(t.source.read_text(encoding='utf-8'))
        header = (f'<!-- translation: lang={t.lang} source={rel(t.source)} source-hash={hash_} '
                  f'reviewed={"yes" if reviewed or t.reviewed else "no"} -->')
        t.path.write_text(header + '\n' + rest, encoding='utf-8', newline='\n')
        print(f'{rel(t.path)}: {header}')
    return cmd_bars(write=True)


def cmd_bars(write: bool) -> int:
    errors: list[str] = []
    translations = read_translations(markdown_files(), errors)
    for e in errors:
        print(f'error: {e}')
    differ = 0
    for page, block in expected_blocks(translations).items():
        text = page.read_text(encoding='utf-8')
        if current_block(text) == block:
            continue
        differ += 1
        if write:
            page.write_text(with_block(page, text, block), encoding='utf-8', newline='\n')
            print(f'wrote {rel(page)}')
        else:
            print(f'--- {rel(page)}\n{block}')
    if not write and differ:
        print(f'{differ} language blocks differ; --write writes them')
    return 1 if errors else 0


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('check')
    p.add_argument('--strict', action='store_true', help='fail on a stale translation too')
    sub.add_parser('status')
    p = sub.add_parser('diff')
    p.add_argument('translation')
    p = sub.add_parser('stamp')
    p.add_argument('translation', nargs='+')
    p.add_argument('--reviewed', action='store_true')
    p = sub.add_parser('bars')
    p.add_argument('--write', action='store_true')
    a = ap.parse_args()
    if a.cmd == 'check':
        return cmd_check(a.strict)
    if a.cmd == 'status':
        return cmd_status()
    if a.cmd == 'diff':
        return cmd_diff(a.translation)
    if a.cmd == 'stamp':
        return cmd_stamp(a.translation, a.reviewed)
    return cmd_bars(a.write)


if __name__ == '__main__':
    sys.exit(main())
