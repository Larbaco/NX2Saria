"""Rename the NX2NAVI_* setting names still written in the code to their current NX2SARIA_* spelling.

The game, the launcher and the build tools read every setting under both names, NX2SARIA_* first (runtime/cfg.c
cfg_env, launcher/gameini.py, tools/make_game.py setting_env, recomp/gen.py setting_env). This script changes how the
code spells the names it reads, writes and tests, so that a reader of the code sees the current name. It is
mechanical and can be run again: on a branch made before the rename, or after a rebase that brought new code.

It rewrites the files git tracks under runtime/, launcher/, tools/, recomp/ and tests/, and leaves alone:
- tools/vendor/ (kept byte-identical to its source) and runtime/third_party/;
- tests/legacy_names_check.py, whose planted lines spell the older names on purpose;
- tools/xlate/: its files make the shader translator's identity (tools/build_xlate.py identity), which keys every
  player's shader cache, so a change there would make every player translate and prepare their shaders again;
- a line that also holds an NX2SARIA_ name, or that says the NX2NAVI_ one is the older, legacy, alias or earlier
  spelling: that is the fallback itself, its description or its test;
- the bare prefix "NX2NAVI_" (followed by a quote, a blank, '*' or the end of the line): the code that strips or
  matches either prefix, and the GENERATED_MARKER of tools/check_content.py;
- NX2NAVI_PGO_GEN, the compile-time flag still accepted beside NX2SARIA_PGO_GEN (docs/branding.md);
- the lines of KEEP: tests of the older names whose line does not say so.

    python tools/rename_legacy_names.py           rewrite the files; one line per file changed
    python tools/rename_legacy_names.py --check   print what would change; exit 1 when anything would
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIRS = ('runtime', 'launcher', 'tools', 'recomp', 'tests')
SKIP_DIRS = ('tools/vendor/', 'tools/xlate/', 'runtime/third_party/')
SELF = ('tools/rename_legacy_names.py', 'tests/legacy_names_check.py')   # this script, and its check's planted names

OLD, NEW = 'NX2NAVI_', 'NX2SARIA_'
NAME = re.compile(r'NX2NAVI_(?![\'"`\s*]|$)')
PAIRED = re.compile(r'nx2saria_', re.I)
SAYS_OLDER = re.compile(r'\b(?:legacy|older|earlier|formerly|alias(?:es)?)\b|still works|still accepted', re.I)
KEPT_NAMES = ('NX2NAVI_PGO_GEN',)

# (file name, text on the line): tests of the older spelling whose line does not say so. Matched by file name, not
# path, so that a file moved to another folder keeps its entry.
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
    """The line with its NX2NAVI_ setting names in the current spelling; the line itself when it is kept."""
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
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
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
