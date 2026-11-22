"""The runtime's folders by subsystem (docs/architecture.md, "Runtime folders") as the build scripts see them.

- tools/build_game.py RUNTIME_DIRS, tools/make_game.py RUNTIME_DIRS and launcher/gameart.py RUNTIME_DIRS are one list,
  and it names every folder of runtime/ that holds sources except platform/, shaders/, third_party/ and dlssg/ (a source
  in a folder it leaves out would not be compiled).
- The sources include headers by name with each folder on the include path, so no two headers on that path share a
  name: the one the compiler found first would be taken for both.
- For each OS, Linux and Windows alike whichever this runs on (the scripts' WIN switched): build_game.py and
  make_game.py compile the same files (build_game.py also the Windows resource script); with the other OS's files
  (*_win.* or *_sdl.*, platform/win32.c or platform/linux.c, nx2saria.rc) they are every .c, .cpp and .rc under
  runtime/ but dlssg/ (tools/build_dlssg.py builds that DLL apart), so no source is left out; no two have one file
  name (--skip-runtime, INSTALL_SIDE's split and make_game.py's per-install split go by it) and make_game.py's object
  names do not collide; each INSTALL_SIDE path names a source both compile.

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

    every = {p.relative_to(RT).as_posix() for p in RT.rglob('*') if p.suffix in ('.c', '.cpp', '.rc')
             and p.relative_to(RT).parts[0] != 'dlssg'}
    counts = {}
    for win in (False, True):
        bad += sources_for(win, every)
        counts['Windows' if win else 'Linux'] = len(compiled(win)[0])

    for line in bad:
        print(f'runtime_dirs_check: FAIL: {line}')
    if bad:
        return 1
    print(f'runtime_dirs_check: {len(build_game.RUNTIME_DIRS)} folders in all three lists, {len(headers)} header names '
          f'on the include path; {", ".join(f"{os_} {n}" for os_, n in counts.items())} sources compiled by both '
          f'scripts, none left out of the {len(every)} under runtime/; {len(build_game.INSTALL_SIDE)} install-side '
          'sources')
    return 0


def compiled(win):
    """(build_game.py's sources, make_game.py's (source, object) pairs) as each script picks them on Windows (win) or
    elsewhere: both read their module's WIN when called."""
    old = build_game.WIN, make_game.WIN
    build_game.WIN = make_game.WIN = win
    try:
        return ([p.relative_to(RT).as_posix() for p in build_game.runtime_sources()],
                [(p.relative_to(make_game.KIT / 'runtime').as_posix(), o) for p, o in make_game.runtime_sources()])
    finally:
        build_game.WIN, make_game.WIN = old


def sources_for(win, every):
    os_ = 'Windows' if win else 'Linux'
    bad = []
    ours, pairs = compiled(win)
    kit = [s for s, _ in pairs]
    ours_c = {s for s in ours if not s.endswith('.rc')}
    if ours_c != set(kit):
        bad.append(f'{os_}: build_game.py compiles {sorted(ours_c - set(kit))} and make_game.py '
                   f'{sorted(set(kit) - ours_c)} besides the rest')
    old = build_game.WIN
    build_game.WIN = win
    try:
        foreign = {s for s in every if build_game.other_os(Path(s).name)}
    finally:
        build_game.WIN = old
    left = every - set(ours) - foreign
    if left:
        bad.append(f'{os_}: no build compiles {sorted(left)}')
    names = collections.Counter(Path(s).name for s in ours)
    bad += [f'{os_}: {n} compiled sources are named {name}' for name, n in names.items() if n > 1]
    objects = collections.Counter(o for _, o in pairs)
    bad += [f'{os_}: make_game.py gives {n} sources the object {o}' for o, n in objects.items() if n > 1]
    bad += [f'{os_}: INSTALL_SIDE {n} is no source the build compiles' for n in sorted(build_game.INSTALL_SIDE)
            if n not in ours_c or n not in kit]
    return bad

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
