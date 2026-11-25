import argparse
import re
import subprocess
import sys
from pathlib import Path

DESCRIPTION = 'Rename the NX2NAVI_* setting names still written in the code to their current NX2SARIA_* spelling.'
ROOT = Path(__file__).resolve().parent.parent
DIRS = ('runtime', 'launcher', 'tools', 'recomp', 'tests')
SKIP_DIRS = ('tools/vendor/', 'tools/xlate/', 'runtime/third_party/')
SELF = ('tools/rename_legacy_names.py', 'tests/legacy_names_check.py')

OLD, NEW = 'NX2NAVI_', 'NX2SARIA_'
NAME = re.compile(r'NX2NAVI_(?![\'"`\s*]|$)')
PAIRED = re.compile(r'nx2saria_', re.I)
SAYS_OLDER = re.compile(r'\b(?:legacy|older|earlier|formerly|alias(?:es)?)\b|still works|still accepted', re.I)
KEPT_NAMES = ('NX2NAVI_PGO_GEN',)

KEEP = (
    ('cfg_parse_check.py', "{'NX2NAVI_FULLSCREEN': '1'}"),
    ('host_probes_test.py', 'NX2NAVI_LIFT_JOBS'),
    ('host_probes_test.py', 'NX2NAVI_LIFT_TEST_OOM_ABOVE'),
    ('make_game_sdl3_test.py', "os.environ['NX2NAVI_SDL3'] = 'kit'"),
    ('pgo_build_test.py', "{'NX2NAVI_PGO': 'gen'}"),
    ('pgo_build_test.py', "{'NX2NAVI_GUEST_OPT': '2'}"),
)


def files():
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z', '--', *DIRS], capture_output=True, check=True)
    for rel in out.stdout.decode('utf-8').split('\0'):
        if rel and rel not in SELF and not rel.startswith(SKIP_DIRS):
            yield rel


def rename_line(name, line):
    if OLD not in line or PAIRED.search(line) or SAYS_OLDER.search(line):
        return line
    if any(name == f and text in line for f, text in KEEP):
        return line

    def sub(m):
        if line.startswith(KEPT_NAMES, m.start()):
            return m.group(0)
        return NEW

    return NAME.sub(sub, line)


def rename_text(name, text):
    lines = text.splitlines(keepends=True)
    new = [rename_line(name, ln) for ln in lines]
    return ''.join(new), [(i + 1, a, b) for i, (a, b) in enumerate(zip(lines, new)) if a != b]


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--check', action='store_true', help='print what would change and exit 1 when anything would')
    args = ap.parse_args()
    total = names = 0
    for rel in files():
        path = ROOT / rel
        data = path.read_bytes()
        if b'\0' in data or OLD.encode() not in data:
            continue
        try:
            text = data.decode('utf-8')
        except UnicodeDecodeError:
            continue
        new, changed = rename_text(path.name, text)
        if not changed:
            continue
        total += len(changed)
        names += sum(len(NAME.findall(a)) - len(NAME.findall(b)) for _, a, b in changed)
        if args.check:
            for n, a, b in changed:
                print(f'{rel}:{n}: {a.strip()}\n{" " * (len(rel) + len(str(n)) + 2)}-> {b.strip()}')
        else:
            path.write_bytes(new.encode('utf-8'))
            print(f'{rel}: {len(changed)} line{"s" if len(changed) != 1 else ""}')
    print(f'rename_legacy_names: {names} name{"s" if names != 1 else ""} on {total} line{"s" if total != 1 else ""}'
          f'{" would change" if args.check else " changed"}')
    return 1 if args.check and total else 0


if __name__ == '__main__':
    sys.exit(main())
