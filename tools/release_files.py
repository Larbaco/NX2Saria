"""The checks and the page text of a release that GitHub Actions builds and publishes (.github/workflows/release.yml).
Standard library only: it runs with the runner's own Python, before anything is installed.

    python tools/release_files.py tag   --tag v0.19.0-rc.1 [--root .]
        The tag names VERSION's version (vX.Y.Z or vX.Y.Z-rc.N), docs/releases/<tag>.md exists, and the notes of a
        version without a suffix have no pre-release line: the manifest carries those notes for good, and the
        version is stable once promoted (docs/releases/README.md).
    python tools/release_files.py files --tag T --notes docs/releases/T.md --dir dist
        dist holds exactly nine payload/manifest files, then writes SHA256SUMS.txt for ten published files total.
        Windows manifests remain identical; each platform manifest names its own files, version, notes, real sizes
        and sha256.
    python tools/release_files.py body  --tag T --notes docs/releases/T.md --state prerelease|stable [--repo o/n]
        The text of the GitHub release page: the notes, a pre-release line on top while it is one (unless the notes of
        a release candidate have their own), and the line that tells players how to verify their download. It is page
        text only; no file of the release changes with it.
    python tools/release_files.py promote --tag T [--stable TAG ...]
        T may become the stable release: it has no suffix, and no stable release among the tags given is as new.

Each command prints what it found, one line per finding, and exits 1 on a problem.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

TAG = re.compile(r'v(\d+)\.(\d+)\.(\d+)(-rc\.([1-9]\d*))?')
# The pre-release line of the notes (docs/releases/README.md "The notes"): a line that starts with it. A sentence
# that only mentions earlier pre-releases ("This update includes 0.18.2 to 0.18.5, which came out as pre-releases.")
# is not one.
PRERELEASE_LINE = re.compile(r'^[\W_]*pre[-\s\u2010\u2011]?release', re.IGNORECASE)
MANIFESTS = ('NX2Saria-update.json', 'NX2navi-update.json')
LINUX_MANIFEST = 'NX2Saria-update-linux.json'
SUMS = 'SHA256SUMS.txt'
DEFAULT_REPO = 'Larbaco/NX2Saria'


class Problem(Exception):
    pass


def text(path):
    """A text file's content with LF line ends, as git checks it out on Linux (.gitattributes: eol=lf)."""
    return Path(path).read_text(encoding='utf-8').replace('\r\n', '\n')


def version_of(tag):
    m = TAG.fullmatch(tag)
    if not m:
        raise Problem(f'{tag!r} is not a release tag (vX.Y.Z or vX.Y.Z-rc.N)')
    return tag[1:], bool(m.group(4))


def version_key(tag):
    """The order of release tags as semver orders their versions: v0.19.0-rc.1 < v0.19.0-rc.10 < v0.19.0."""
    m = TAG.fullmatch(tag)
    if not m:
        raise Problem(f'{tag!r} is not a release tag (vX.Y.Z or vX.Y.Z-rc.N)')
    base = tuple(int(m.group(i)) for i in (1, 2, 3))
    return base + ((0, int(m.group(5))) if m.group(4) else (1, 0))


def check_promote(tag, stable):
    """[finding] when tag may be made the stable release beside the stable releases' tags; raises Problem otherwise.
    Tags that are not release tags (none so far) are left out."""
    if version_of(tag)[1]:
        raise Problem(f'{tag} is a release candidate: candidates stay pre-releases')
    newest = max((t for t in stable if TAG.fullmatch(t)), key=version_key, default='')
    if newest and version_key(newest) >= version_key(tag):
        raise Problem(f'{newest} is a stable release already, not older than {tag}')
    return [f'{tag} may become the stable release (the newest stable one now: {newest or "none"})']


def check_tag(tag, root):
    """[finding] for a tag; raises Problem when it may not be released."""
    ver, rc = version_of(tag)
    root = Path(root)
    found = (root / 'VERSION').read_text(encoding='utf-8').strip()
    if found != ver:
        raise Problem(f'the tag {tag} names {ver}, but VERSION at its commit says {found}')
    notes = root / 'docs' / 'releases' / f'{tag}.md'
    if not notes.is_file() or not text(notes).strip():
        raise Problem(f'{notes.relative_to(root).as_posix()} is missing or empty at the tag\'s commit')
    lines = [n for n, line in enumerate(text(notes).splitlines(), 1) if PRERELEASE_LINE.match(line)]
    if lines and not rc:
        raise Problem(f'{notes.relative_to(root).as_posix()}:{lines[0]}: a pre-release line in the notes of {ver}, '
                      'which has no -rc suffix: the update manifest keeps these notes after the release is made '
                      'stable. The release page says it is a pre-release by itself (docs/releases/README.md)')
    visible = [line.strip() for line in text(notes).splitlines() if line.strip()]
    for line in visible[:6]:
        residue = re.sub(r'\[[^]]+\]\(https?://[^)]+\)', '', line)
        residue = re.sub(r'https?://\S+', '', residue)
        if not re.sub(r'[·|\s]', '', residue):
            raise Problem(f'{notes.relative_to(root).as_posix()}: links must come after the first six non-empty lines')
    return [f'tag {tag}: version {ver}, {"a release candidate" if rc else "no suffix"}, '
            f'notes {notes.relative_to(root).as_posix()} ({len(text(notes).split())} words)']


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def check_files(tag, notes, folder):
    """[finding] for the built files in folder; raises Problem on the first that is wrong. Writes SHA256SUMS.txt."""
    ver, _ = version_of(tag)
    folder = Path(folder)
    for name in MANIFESTS:
        if not (folder / name).is_file():
            raise Problem(f'{name} is missing')
    raw = [(folder / name).read_bytes() for name in MANIFESTS]
    if raw[0] != raw[1]:
        raise Problem(f'{MANIFESTS[0]} and {MANIFESTS[1]} differ')
    try:
        m = json.loads(raw[0])
    except ValueError as e:
        raise Problem(f'{MANIFESTS[0]} is not JSON: {e}') from e
    if not isinstance(m, dict) or m.get('format') != 1:
        raise Problem(f'{MANIFESTS[0]} is not an update manifest of format 1')
    if m.get('version') != ver:
        raise Problem(f'{MANIFESTS[0]} names version {m.get("version")!r}, the tag {ver}')
    if (m.get('notes') or '').replace('\r\n', '\n') != text(notes):
        raise Problem(f'the notes in {MANIFESTS[0]} are not {notes} of the tag\'s commit')
    want = {}
    for key in ('setup', 'app', 'runtime'):
        e = m.get(key)
        if not isinstance(e, dict) or not isinstance(e.get('name'), str) or Path(e['name']).name != e['name']:
            raise Problem(f'{MANIFESTS[0]} gives no plain file name for its {key} file')
        if e['name'] in want:
            raise Problem(f'{MANIFESTS[0]} names {e["name"]} for two files')
        want[e['name']] = (key, e.get('size'), e.get('sha256'))
    expect = {f'NX2Saria-Setup-{ver}.exe': 'setup', f'NX2Saria-app-{ver}.zip': 'app'}
    for name, key in expect.items():
        if want.get(name, (None,))[0] != key:
            raise Problem(f'{MANIFESTS[0]} does not name {name} as its {key} file')
    runtime = next(n for n, w in want.items() if w[0] == 'runtime')
    if not re.fullmatch(r'NX2Saria-runtime-[A-Za-z0-9._-]+\.zip', runtime):
        raise Problem(f'{MANIFESTS[0]} names its runtime file {runtime!r}')
    appimage = f'NX2Saria-Linux-{ver}-x86_64.AppImage'
    if not (folder / LINUX_MANIFEST).is_file():
        raise Problem(f'{LINUX_MANIFEST} is missing')
    linux_raw = (folder / LINUX_MANIFEST).read_bytes()
    try:
        lm = json.loads(linux_raw)
    except ValueError as e:
        raise Problem(f'{LINUX_MANIFEST} is not JSON: {e}') from e
    if not isinstance(lm, dict) or lm.get('format') != 1 or lm.get('version') != ver:
        raise Problem(f'{LINUX_MANIFEST} is not an update manifest of format 1 for {ver}')
    if 'setup' in lm:
        raise Problem(f'{LINUX_MANIFEST} must name the AppImage instead of a setup file')
    if (lm.get('notes') or '').replace('\r\n', '\n') != text(notes):
        raise Problem(f'the notes in {LINUX_MANIFEST} are not {notes} of the tag\'s commit')
    linux_want = {}
    for key in ('app', 'runtime', 'appimage'):
        e = lm.get(key)
        if not isinstance(e, dict) or not isinstance(e.get('name'), str) or Path(e['name']).name != e['name']:
            raise Problem(f'{LINUX_MANIFEST} gives no plain file name for its {key} file')
        if e['name'] in linux_want:
            raise Problem(f'{LINUX_MANIFEST} names {e["name"]} for two files')
        linux_want[e['name']] = (key, e.get('size'), e.get('sha256'))
    expected_linux = {f'NX2Saria-app-linux-x86_64-{ver}.zip': 'app',
                      f'NX2Saria-Linux-{ver}-x86_64.AppImage': 'appimage'}
    for name, key in expected_linux.items():
        if linux_want.get(name, (None,))[0] != key:
            raise Problem(f'{LINUX_MANIFEST} does not name {name} as its {key} file')
    runtime_linux = next((n for n, w in linux_want.items() if w[0] == 'runtime'), '')
    if not re.fullmatch(r'NX2Saria-runtime-linux-x86_64-[A-Za-z0-9._-]+\.zip', runtime_linux):
        raise Problem(f'{LINUX_MANIFEST} names its runtime file {runtime_linux!r}')
    runtime_entry = lm['runtime']
    runtime_ident = runtime_entry.get('id')
    runtime_prefix = 'linux-x86_64-'
    if (not isinstance(runtime_ident, str) or not re.fullmatch(r'[A-Za-z0-9._-]+', runtime_ident)
            or not runtime_ident.startswith(runtime_prefix)
            or runtime_linux != f'NX2Saria-runtime-linux-x86_64-{runtime_ident.removeprefix(runtime_prefix)}.zip'):
        raise Problem(f'{LINUX_MANIFEST} runtime id does not match {runtime_linux!r}')
    names = sorted([*want, *MANIFESTS, LINUX_MANIFEST, *linux_want])
    present = sorted(p.name for p in folder.iterdir() if p.is_file() and p.name != SUMS)
    if present != names:
        extra, missing = sorted(set(present) - set(names)), sorted(set(names) - set(present))
        raise Problem(f'the release files are not the expected ones: missing {missing}, unexpected {extra}')
    out = []
    for name, (key, size, digest) in sorted(want.items()):
        p = folder / name
        if p.stat().st_size != size or sha256(p) != digest:
            raise Problem(f'{name}: its size or sha256 is not what {MANIFESTS[0]} says')
        out.append(f'{name}: {size} bytes, sha256 as the manifest says ({key})')
    for name, (key, size, digest) in sorted(linux_want.items()):
        p = folder / name
        if (not isinstance(size, int) or isinstance(size, bool) or size <= 0 or p.stat().st_size != size
                or not isinstance(digest, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', digest)
                or sha256(p) != digest):
            raise Problem(f'{name}: its size or sha256 is not what {LINUX_MANIFEST} says')
        out.append(f'{name}: {size} bytes, sha256 as the manifest says ({key})')
    lines = [f'{sha256(folder / n)}  {n}\n' for n in names]
    (folder / SUMS).write_text(''.join(lines), encoding='utf-8', newline='\n')
    out.append(f'{SUMS}: {len(lines)} files')
    return out


def body(tag, notes, state, repo=DEFAULT_REPO):
    ver, rc = version_of(tag)
    head = ''
    own = any(PRERELEASE_LINE.match(line) for line in text(notes).splitlines())
    if state == 'prerelease' and not (rc and own):
        head = ('**Pre-release:** a release candidate, offered only to launchers on the Preview channel.\n\n' if rc else
                f'**Pre-release:** {ver} becomes the stable release once it has been tried on Windows. Until then '
                'only launchers on the Preview channel offer it.\n\n')
    verify = (f'Every file of this release was built and uploaded by GitHub Actions and has a provenance attestation: '
              f'[verify your download](https://github.com/{repo}/blob/main/docs/install.md#verify-your-download).')
    return f'{head}{text(notes).rstrip()}\n\n{verify}\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    t = sub.add_parser('tag')
    t.add_argument('--tag', required=True)
    t.add_argument('--root', default='.')
    f = sub.add_parser('files')
    f.add_argument('--tag', required=True)
    f.add_argument('--notes', required=True)
    f.add_argument('--dir', required=True)
    b = sub.add_parser('body')
    b.add_argument('--tag', required=True)
    b.add_argument('--notes', required=True)
    b.add_argument('--state', choices=('prerelease', 'stable'), required=True)
    b.add_argument('--repo', default=DEFAULT_REPO)
    pr = sub.add_parser('promote')
    pr.add_argument('--tag', required=True)
    pr.add_argument('--stable', nargs='*', default=[], help='the tags of the stable releases there are')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'tag':
            out = check_tag(a.tag, a.root)
        elif a.cmd == 'files':
            out = check_files(a.tag, a.notes, a.dir)
        elif a.cmd == 'promote':
            out = check_promote(a.tag, a.stable)
        else:
            sys.stdout.write(body(a.tag, a.notes, a.state, a.repo))
            return 0
    except (Problem, OSError) as e:
        print(f'::error::{e}')
        return 1
    for line in out:
        print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
