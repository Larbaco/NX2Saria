# args: --with-self-test
import argparse
import re
import sys
from pathlib import Path

DESCRIPTION = ('Check THIRD_PARTY_NOTICES.md against what tools/make_launcher.py and tools/make_appimage.py '
               'package')
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
NOTICES = ROOT / 'THIRD_PARTY_NOTICES.md'
WIN, LINUX, BOTH = 'Windows', 'Linux', 'both'
REMOVED = {
    '_dbm': ('Berkeley DB 6.0.19, Sleepycat licence: the complete source of every program that uses it must be offered',
             r'^\s*(import|from)\s+(_dbm|dbm\.ndbm|dbm|shelve)\b'),
}
SEARCHED = ('launcher', 'tools', 'recomp', 'tests', 'installer')
HEADINGS = {WIN: '## Shipped in the Windows launcher release', LINUX: '## Shipped in the Linux AppImage',
            BOTH: '## Shipped in both releases'}


def sections(text):
    out = {k: [] for k in HEADINGS}
    current = None
    for line in text.splitlines():
        if line.startswith('## '):
            current = next((k for k, h in HEADINGS.items() if line.strip() == h), None)
        elif current and line.startswith('| ') and not line.startswith('| Component |'):
            out[current].append(line)
    return out


def requirements(path):
    pins = []
    for line in path.read_text(encoding='utf-8').splitlines():
        m = re.match(r'^([A-Za-z0-9_.-]+)==([^\s;]+)', line.strip())
        if m:
            pins.append((m.group(1), m.group(2)))
    return pins


def components(extra_linux_packages=()):
    import build_game
    import dlss_pin
    import make_appimage
    import make_launcher
    import sdl3_pin
    import streamline_pin
    import vulkan_pin
    comps = []

    pin_rows = {'python-embed': [f'CPython {make_launcher.PY_VERSION} embeddable'], 'tcltk': ['tcltk.msi']}
    for name in make_launcher.PINS:
        comps.append((WIN, f'make_launcher.PINS[{name!r}]', pin_rows.get(name, [f'no row rule for the pin {name}'])))
    for f in make_launcher.TCLTK_FILES:
        comps.append((WIN, f'make_launcher.TCLTK_FILES {f}', [f'`{f}`']))
    for name, ver in requirements(ROOT / 'tools' / 'requirements-launcher.txt'):
        comps.append((WIN, f'requirements-launcher.txt {name}=={ver}', [f'{name} {ver}']))
    launcher_src = (ROOT / 'tools' / 'make_launcher.py').read_text(encoding='utf-8')
    for dll in sorted(set(re.findall(r"'(amd_fidelityfx_\w+\.dll)'", launcher_src))):
        comps.append((WIN, f'make_launcher.py {dll}', [dll]))
    workflow = (ROOT / '.github' / 'workflows' / 'windows-release.yml').read_text(encoding='utf-8')
    for tag in sorted(set(re.findall(r'/(v\d+\.\d+\.\d+)/(?:PrebuiltSignedDLL|Kits)/', workflow))):
        comps.append((WIN, f'windows-release.yml AMD FidelityFX SDK {tag}', ['FidelityFX', f'`{tag}`']))
    comps.append((WIN, f'dlss_pin {dlss_pin.NAME}', [dlss_pin.NAME, dlss_pin.VERSION]))
    for f in streamline_pin.FILES:
        comps.append((WIN, f'streamline_pin.FILES {f}', [f]))
    comps.append((WIN, f'Streamline {streamline_pin.VERSION}', [f'Streamline {streamline_pin.VERSION}']))
    if "'-l:libz.a'" in launcher_src:
        comps.append((WIN, 'zlib linked into the setup (-l:libz.a)', ['zlib', 'static']))
    if "'-static'" in launcher_src:
        comps.append((WIN, 'the MinGW runtime linked into NX2Saria.exe and the setup (-static)',
                      ['mingw-w64', 'winpthreads', 'libgcc']))
    if 'libnx2navirt.a' in launcher_src:
        comps.append((WIN, 'the prebuilt runtime library', ['libnx2navirt.a']))

    appimage_wf = (ROOT / '.github' / 'workflows' / 'linux-appimage.yml').read_text(encoding='utf-8')
    py = re.search(r'uv python install(?: -q)? (\d+\.\d+\.\d+)', appimage_wf)
    comps.append((LINUX, 'the AppImage Python (linux-appimage.yml)',
                  ['python-build-standalone', f'CPython {py.group(1) if py else "?"}']))
    comps.append((LINUX, 'pip, which python-build-standalone bundles', ['pip, as python-build-standalone']))
    for spec in (*make_appimage.PACKAGES, *extra_linux_packages):
        name, ver = spec.split('==')
        comps.append((LINUX, f'make_appimage.PACKAGES {spec}', [f'{name} {ver}']))
    rt = re.search(r'type2-runtime/releases/download/([^/]+)/', appimage_wf)
    comps.append((LINUX, 'the AppImage runtime (linux-appimage.yml RUNTIME_URL)',
                  ['type-2 runtime', rt.group(1) if rt else '?']))
    comps.append((LINUX, f'vulkan_pin {vulkan_pin.VERSION}', [f'Vulkan headers {vulkan_pin.VERSION}']))
    comps.append((LINUX, f'sdl3_pin {sdl3_pin.DEB_VERSION}', [f'SDL {sdl3_pin.VERSION}', sdl3_pin.DEB_VERSION]))
    comps.append((LINUX, f'dlss_pin {dlss_pin.LINUX_NAME}', [dlss_pin.LINUX_NAME]))
    vma = re.search(r'/v(\d+\.\d+\.\d+)/', build_game.VMA_FETCH[0])
    comps.append((LINUX, 'the VMA header (build_game.VMA_FETCH)', [f'Vulkan Memory Allocator {vma.group(1) if vma else "?"}']))
    if 'licenses' not in make_appimage.KIT_DIRS:
        comps.append((LINUX, 'licenses/ in make_appimage.KIT_DIRS', ['make_appimage.py does not ship licenses/']))

    comps.append((BOTH, 'the shader translator', ['nx2navi_xlate', 'Ryujinx']))
    comps.append((BOTH, 'the button art (prompt_art)', ['prompt art', 'CC0']))
    if "'licenses'" not in launcher_src:
        comps.append((BOTH, 'licenses/ in make_launcher.py', ['make_launcher.py does not ship licenses/']))
    for f in sorted((ROOT / 'licenses').rglob('*')):
        if f.is_file():
            rel = f.relative_to(ROOT / 'licenses')
            comps.append((BOTH, f'licenses/{rel.as_posix()}', [f.name]))
    return comps


def check(extra_linux_packages=(), quiet=False):
    rows = sections(NOTICES.read_text(encoding='utf-8'))
    comps = components(extra_linux_packages)
    missing, used = [], set()
    for artifact, what, keys in comps:
        pool = rows[artifact] + (rows[BOTH] if artifact != BOTH else rows[WIN] + rows[LINUX])
        hit = [r for r in pool if all(k.lower() in r.lower() for k in keys)]
        if hit:
            used.update(hit)
        else:
            missing.append(f'{artifact}: {what} (a row holding {", ".join(repr(k) for k in keys)})')
    unmatched = [r for k in HEADINGS for r in rows[k] if r not in used]
    if not quiet:
        for m in missing:
            print(f'shipped_notices_check: FAIL: no row for {m}')
        for r in unmatched:
            print(f'shipped_notices_check: warning: this row names nothing the scripts package: {r[:110]}')
        print(f'shipped_notices_check: {len(comps)} packaged components, {len(missing)} without a row; '
              f'{sum(len(v) for v in rows.values())} rows in the shipped sections, {len(unmatched)} naming none')
    return missing, unmatched


def python_rows(rows):
    import make_launcher
    import python_licences as pl
    bad = []
    wf = (ROOT / '.github' / 'workflows' / 'linux-appimage.yml').read_text(encoding='utf-8')
    m = re.search(r'uv python install(?: -q)? (\d+\.\d+\.\d+)', wf)
    for where, version in (('make_launcher.PY_VERSION', make_launcher.PY_VERSION),
                           ('linux-appimage.yml', m.group(1) if m else '?')):
        if version != pl.VERSION:
            bad.append(f'{where} ships Python {version}, tools/python_licences.py pins texts for {pl.VERSION}')
    for artifact, texts, row_key, folder in ((WIN, pl.WINDOWS_TEXTS, 'CPython 3.14.9 embeddable', 'licenses/python/'),
                                            (LINUX, pl.LINUX_TEXTS, 'python-build-standalone', 'usr/kit/licenses/python/')):
        row = next((r for r in rows[artifact] if row_key in r), '')
        if folder not in row:
            bad.append(f'{artifact}: the Python row does not say its texts are in {folder}')
        tk = next((r for r in rows[artifact] if 'tkinter' in r and 'Tcl/Tk' in r), '')
        for lib in texts:
            if lib == 'CPython':
                continue
            if lib not in row and lib not in tk:
                bad.append(f'{artifact}: the Python row does not name {lib}, whose text tools/python_licences.py ships')
    return bad


def removed_problems(removed=None, python_dir=None):
    import make_appimage
    removed = make_appimage.PYTHON_REMOVED if removed is None else removed
    bad = []
    src = (ROOT / 'tools' / 'make_appimage.py').read_text(encoding='utf-8')
    if 'for f in removed_modules(py):' not in src or 'f.unlink()' not in src:
        bad.append('make_appimage.py no longer deletes removed_modules(py)')
    for name, (why, imports) in REMOVED.items():
        if name not in removed:
            bad.append(f'{name} is packaged again ({why}): it is not in make_appimage.PYTHON_REMOVED')
        pattern = re.compile(imports, re.M)
        for d in SEARCHED:
            for f in sorted((ROOT / d).rglob('*.py')):
                if f.resolve() == Path(__file__).resolve():
                    continue
                for hit in pattern.finditer(f.read_text(encoding='utf-8', errors='replace')):
                    bad.append(f'{f.relative_to(ROOT)} imports {hit.group(2)}, which needs {name} ({why})')
    if python_dir is not None:
        left = [p for p in make_appimage.removed_modules(python_dir)
                if p.name.split('.')[0] in REMOVED and p.name.split('.')[0] not in removed]
        bad += [f'{p} would be packaged' for p in left]
    return bad


def self_test():
    import tempfile
    rc = 0
    missing, _ = check(extra_linux_packages=('plantedpkg==9.9.9',), quiet=True)
    planted = [m for m in missing if 'plantedpkg' in m]
    if planted:
        print(f'shipped_notices_check: self-test: the planted package fails the check ({planted[0]})')
    else:
        print('shipped_notices_check: FAIL: self-test: a planted package without a row passed the check')
        rc = 1
    with tempfile.TemporaryDirectory() as d:
        dyn = Path(d) / 'lib' / 'python3.14' / 'lib-dynload'
        dyn.mkdir(parents=True)
        (dyn / '_dbm.cpython-314-x86_64-linux-gnu.so').write_bytes(b'')
        (dyn / '_json.cpython-314-x86_64-linux-gnu.so').write_bytes(b'')
        import make_appimage
        found = [p.name for p in make_appimage.removed_modules(d)]
        back = removed_problems(removed={}, python_dir=d)
    if found == ['_dbm.cpython-314-x86_64-linux-gnu.so'] and any('_dbm' in b for b in back):
        print(f'shipped_notices_check: self-test: a planted _dbm is found and removed ({found[0]}), and fails the check '
              f'when the removal list drops it ({back[0]})')
    else:
        print(f'shipped_notices_check: FAIL: self-test: planted _dbm: found {found}, problems {back}')
        rc = 1
    return rc


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--self-test', action='store_true', help='only check that a planted unlisted package fails')
    ap.add_argument('--with-self-test', action='store_true', help='the check, then the self-test')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    missing, _ = check()
    rows = sections(NOTICES.read_text(encoding='utf-8'))
    other = python_rows(rows) + removed_problems()
    for b in other:
        print(f'shipped_notices_check: FAIL: {b}')
    if not other:
        import make_appimage
        print(f'shipped_notices_check: the Python rows name every library whose text ships; removed from the AppImage\'s '
              f'Python and imported nowhere: {", ".join(f"{k} ({REMOVED[k][0]})" for k in sorted(make_appimage.PYTHON_REMOVED))}')
    rc = 1 if missing or other else 0
    if a.with_self_test:
        rc |= self_test()
    return rc


if __name__ == '__main__':
    sys.exit(main())
