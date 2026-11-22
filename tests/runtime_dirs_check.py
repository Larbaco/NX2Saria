"""The runtime's folders by subsystem (docs/architecture.md, "Runtime folders") as the build scripts see them.

- tools/build_game.py RUNTIME_DIRS, tools/make_game.py RUNTIME_DIRS and launcher/gameart.py RUNTIME_DIRS are one list,
  and it names every folder of runtime/ that holds sources except platform/, shaders/, third_party/ and dlssg/ (a source
  in a folder it leaves out would not be compiled).
- The sources include headers by name with each folder on the include path, so no two headers on that path share a
  name: the one the compiler found first would be taken for both.
- build_game.py and make_game.py compile the same files, and make_game.py's object names do not collide.
- Each INSTALL_SIDE path names a source.

No compiler, no game files.

Usage: runtime_dirs_check.py
"""
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'runtime'
sys.path.insert(0, str(ROOT / 'tools'))
import build_game  # noqa: E402
import make_game  # noqa: E402

NOT_SOURCES = {'platform', 'shaders', 'third_party', 'dlssg'}   # compiled apart, or not part of the game's compile


def main():
    bad = []
    gameart = re.search(r'^RUNTIME_DIRS = (\(.*?\))', (ROOT / 'launcher' / 'gameart.py').read_text(encoding='utf-8'),
                        re.M | re.S)
    lists = {'tools/build_game.py': build_game.RUNTIME_DIRS, 'tools/make_game.py': make_game.RUNTIME_DIRS,
             'launcher/gameart.py': eval(gameart.group(1)) if gameart else None}
    for where, dirs in lists.items():
        if dirs != build_game.RUNTIME_DIRS:
            bad.append(f'{where} RUNTIME_DIRS is {dirs}, tools/build_game.py\'s {build_game.RUNTIME_DIRS}')
    on_disk = sorted(d.name for d in RT.iterdir() if d.is_dir() and d.name not in NOT_SOURCES
                     and any(p.suffix in ('.c', '.cpp', '.h', '.inc') for p in d.iterdir()))
    if on_disk != sorted(build_game.RUNTIME_DIRS):
        bad.append(f'the source folders of runtime/ are {on_disk}, RUNTIME_DIRS {sorted(build_game.RUNTIME_DIRS)}')

    path = build_game.runtime_include_dirs(RT) + [RT / 'third_party' / 'imgui']
    headers = collections.defaultdict(list)
    for d in path:
        for p in d.iterdir():
            if p.suffix in ('.h', '.inc'):
                headers[p.name].append(p.relative_to(RT).as_posix())
    for name, where in sorted(headers.items()):
        if len(where) > 1:
            bad.append(f'{name} is in more than one folder on the include path: {", ".join(where)}')

    ours = {p.relative_to(RT).as_posix() for p in build_game.runtime_sources() if p.suffix != '.rc'}
    theirs = make_game.runtime_sources()
    kit = {p.relative_to(make_game.KIT / 'runtime').as_posix() for p, _ in theirs}
    if ours != kit:
        bad.append(f'build_game.py compiles {sorted(ours - kit)} and make_game.py {sorted(kit - ours)} besides the rest')
    objects = collections.Counter(o for _, o in theirs)
    bad += [f'make_game.py gives {n} sources the object {o}' for o, n in objects.items() if n > 1]
    bad += [f'INSTALL_SIDE {n} is no runtime source' for n in sorted(build_game.INSTALL_SIDE) if not (RT / n).is_file()]

    for line in bad:
        print(f'runtime_dirs_check: FAIL: {line}')
    if bad:
        return 1
    print(f'runtime_dirs_check: {len(build_game.RUNTIME_DIRS)} folders in all three lists, {len(headers)} header names '
          f'on the include path, {len(ours)} sources compiled by both scripts, {len(build_game.INSTALL_SIDE)} '
          'install-side sources')
    return 0


if __name__ == '__main__':
    sys.exit(main())
