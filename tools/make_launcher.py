import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
import build_shaders  # noqa: E402
import check_content  # noqa: E402
import dlss_pin  # noqa: E402
import pgo  # noqa: E402
import prompt_art  # noqa: E402
import streamline_pin  # noqa: E402
from launcher import toolchain  # noqa: E402
from launcher.paths import Layout  # noqa: E402

DESCRIPTION = 'Build a release of the NX2Saria launcher (docs/launcher.md, "Releases").'
MAIN_ROOT = (ROOT / 'build').resolve().parent
DL = MAIN_ROOT / 'toolchain' / 'launcher-dl'
WORK = MAIN_ROOT / 'toolchain' / 'launcher-work'
PY_VERSION = '3.14.9'
PINS = {
    'python-embed': (f'https://www.python.org/ftp/python/{PY_VERSION}/python-{PY_VERSION}-embed-amd64.zip',
                     '3c01d7cfc666f746a6f064f60fb2156ab0ec464d3d738ab796371a46a9bb24a2'),
    'tcltk': (f'https://www.python.org/ftp/python/{PY_VERSION}/amd64/tcltk.msi',
              'ac87609db3f715404dd41a989c2f1706ea1512f3d86217f938d11c63af8eb505'),
}
TOOLS = ('make_game.py', 'nup.py', 'romfs_index.py', 'check_content.py', 'hostmem.py', 'memgate.py', 'prepare_shaders.py',
         'shader_census.py', 'shader_list.py', 'pipeline_list.py', 'zstd_compat.py', 'pad_prompts.py', 'prompt_art.py', 'button_icons.py', 'romfs_check.py',
         'pack_shader_cache.py', 'mod_files.py', 'cheats.py', 'exefs_patches.py', 'private_paths.py')
TOP = ('LICENSE', 'LICENSE-ADDITIONS', 'THIRD_PARTY_NOTICES.md', 'nx2saria.ini.example')
SKIP_DIRS = {'__pycache__', '.pytest_cache', 'native'}
SKIP_SUFFIXES = {'.o', '.obj', '.pyc', '.exe', '.dll', '.a', '.lib'}
TCLTK_FILES = ('_tkinter.pyd', 'tcl90.dll', 'tcl9tk90.dll', 'zlib1.dll', 'libtommath.dll')
DEFAULT_FEED = 'Larbaco/NX2Saria'


def say(msg):
    print(msg, flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def fetch(name):
    url, want = PINS[name]
    dest = DL / url.rsplit('/', 1)[1]
    if dest.is_file() and sha256_file(dest) == want:
        return dest
    from launcher import net
    say(f'download {url}')
    net.download(url, dest, sha256=want)
    return dest


def run(cmd, **kw):
    say('  ' + ' '.join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def copy_tree(src, dst, skip_dirs=SKIP_DIRS):
    for p in sorted(src.rglob('*')):
        rel = p.relative_to(src)
        if any(part in skip_dirs for part in rel.parts) or p.is_dir() or p.suffix.lower() in SKIP_SUFFIXES:
            continue
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst / rel)


def version():
    v = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
    r = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True)
    return v, f'{v}+{r.stdout.strip()}' if r.returncode == 0 and r.stdout.strip() else v


def tcltk_files():
    dest = WORK / 'tcltk'
    msi = fetch('tcltk')
    stamp, want = dest / '.unpacked-from', f'{msi.name} {msi.stat().st_size}'
    if (dest / 'DLLs' / '_tkinter.pyd').is_file() and stamp.is_file() and stamp.read_text() == want:
        return dest
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    if os.name == 'nt':
        run(['msiexec', '/a', msi, '/qn', f'TARGETDIR={dest}'])
    else:
        run(['msiextract', '-C', dest, msi], stdout=subprocess.DEVNULL)
    if not (dest / 'DLLs' / '_tkinter.pyd').is_file():
        sys.exit(f'{dest}: no _tkinter.pyd in {msi.name}')
    stamp.write_text(want)
    return dest


def runtime_id():
    req = (ROOT / 'tools' / 'requirements-launcher.txt').read_bytes()
    return f'py{PY_VERSION}-{hashlib.sha256(req + PINS["tcltk"][1].encode()).hexdigest()[:8]}'


def build_python(py):
    with zipfile.ZipFile(fetch('python-embed')) as z:
        z.extractall(py)
    (py / 'python314._pth').write_text('python314.zip\n.\nLib/site-packages\n../pydeps\n', encoding='utf-8')
    env = dict(os.environ, UV_CACHE_DIR=str(MAIN_ROOT / 'toolchain' / 'uv-cache'))
    run(['uv', 'pip', 'install', '--target', py / 'Lib' / 'site-packages', '--python', sys.executable,
         '--python-platform', 'windows', '--python-version', '3.14', '--only-binary', ':all:', '-q',
         '-r', ROOT / 'tools' / 'requirements-launcher.txt'], env=env)
    shutil.rmtree(py / 'Lib' / 'site-packages' / 'bin', ignore_errors=True)
    tk = tcltk_files()
    for dll in TCLTK_FILES:
        if (tk / 'DLLs' / dll).is_file():
            shutil.copy2(tk / 'DLLs' / dll, py / dll)
    copy_tree(tk / 'Lib' / 'tkinter', py / 'tkinter', skip_dirs={'__pycache__', 'test'})
    copy_tree(tk / 'tcl', py / 'tcl', skip_dirs={'__pycache__'})
    (py / 'runtime-id.txt').write_text(runtime_id() + '\n', encoding='utf-8')


def python_texts(folder, ver):
    import python_licences
    python_licences.cpython_license(folder / 'app' / ver / 'licenses' / 'python', DL)
    missing = []
    for lib, where in python_licences.WINDOWS_TEXTS.items():
        if where is None:
            continue
        rel, line = where
        path = folder / rel if rel.startswith('python/') else folder / 'app' / ver / rel
        if not path.is_file() or line not in path.read_text(encoding='utf-8', errors='replace'):
            missing.append(f'{lib} ({rel})')
    if missing:
        sys.exit(f'the release has no licence text for {", ".join(missing)}')
    say(f'Python licence texts: {len(python_licences.WINDOWS_TEXTS)} libraries, {python_licences.CPYTHON_TEXT}')


def check_xlate(xlate):
    import build_xlate
    reason = build_xlate.check_built(xlate, 'nx2navi_xlate.exe')
    if reason:
        sys.exit(reason)
    if os.name == 'nt':
        import xlate_smoke
        reason = xlate_smoke.pool_check(xlate / 'nx2navi_xlate.exe')
        if reason:
            sys.exit(f'{xlate / "nx2navi_xlate.exe"}: {reason}')
    have = (xlate / 'nx2navi_xlate.id').read_text(encoding='ascii').strip()
    print(f'translator identity {have[:16]}: built from this checkout\'s sources')


def check_dlssg(dll):
    import build_dlssg
    build_dlssg.check(dll)
    print(f'{dll}: exports the {len(build_dlssg.declared_exports())} functions runtime/dlssg/nx2_sl.h declares')


def build_app(app, ver, xlate, feed_repo, lock, ffx=None, dlss=None, art=None, dlssg=None, pipelines=None, ffx4=None,
              pgo_profile=None):
    build_shaders.build(ROOT / 'runtime')
    copy_tree(ROOT / 'launcher', app / 'launcher')
    for d in ('runtime', 'recomp', 'licenses'):
        copy_tree(ROOT / d, app / d)
    (app / 'tools').mkdir(parents=True)
    for t in TOOLS:
        shutil.copy2(ROOT / 'tools' / t, app / 'tools' / t)
    for t in TOP:
        if (ROOT / t).is_file():
            shutil.copy2(ROOT / t, app / t)
    (app / 'VERSION').write_text(ver + '\n', encoding='utf-8')
    (app / 'bin').mkdir()
    for n in ('nx2navi_xlate.exe', 'nx2navi_xlate.id'):
        shutil.copy2(xlate / n, app / 'bin' / n)
    if ffx:
        shutil.copy2(ffx, app / 'bin' / 'amd_fidelityfx_vk.dll')
    if ffx4:
        for n in ('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll'):
            shutil.copy2(ffx4 / n, app / 'bin' / n)
        if (ffx4 / 'amd_fidelityfx_framegeneration_vk.dll').is_file():
            shutil.copy2(ffx4 / 'amd_fidelityfx_framegeneration_vk.dll', app / 'bin' / 'amd_fidelityfx_framegeneration_vk.dll')
    if dlss:
        dlss_pin.place(dlss, app / 'bin')
    if dlssg:
        streamline_pin.place(dlssg[0], dlssg[1], app / 'bin' / 'dlssg')
    if art:
        n, size = prompt_art.copy_art(art, app / prompt_art.FOLDER)
        say(f'button art: {n} pictures, {size / 1e6:.1f} MB, {prompt_art.source_of(app / prompt_art.FOLDER)}')
    else:
        say('button art: none (--no-prompt-art): the game will keep its own prompts')
    if pipelines:
        (app / 'tools' / 'data').mkdir(parents=True, exist_ok=True)
        shutil.copy2(pipelines, app / 'tools' / 'data' / 'pipeline_list.bin')
        say(f'pipeline list: {pipelines.stat().st_size / 1e6:.1f} MB')
    if pgo_profile:
        placed = pgo.place(pgo_profile, app / 'data')
        say(f'execution profile: {placed.stat().st_size / 1e6:.1f} MB')
    else:
        say('execution profile: none (--pgo): the game\'s code is built without one, at -O1')
    (app / 'toolchain.lock.json').write_text(json.dumps(lock, indent=1), encoding='utf-8')
    (app / 'feed.json').write_text(json.dumps({'type': 'github', 'repo': feed_repo}, indent=1), encoding='utf-8')


def native_compiler(lock):
    lay = Layout(root=MAIN_ROOT / 'toolchain' / 'launcher-msys')
    if not toolchain.is_current(lay, lock):
        say('MSYS2 packages for the native programs')
        toolchain.install(lay, lock, log=say)
    sysroot = lay.msys
    if os.name == 'nt':
        return [str(sysroot / 'bin' / 'clang.exe')], [str(sysroot / 'bin' / 'windres.exe')]
    for tool in ('clang', 'ld.lld'):
        if not shutil.which(tool):
            sys.exit(f'{tool} is needed to cross-compile the launcher programs')
    windres = shutil.which('llvm-windres') or shutil.which('llvm-windres-18') or '/usr/lib/llvm-18/bin/llvm-windres'
    if not Path(windres).exists():
        sys.exit('llvm-windres is needed to compile the launcher programs\' resources')
    return (['clang', '--target=x86_64-w64-mingw32', f'--sysroot={sysroot}', '-fuse-ld=lld'],
            [windres, '--target=x86_64-w64-mingw32'])


def build_runtime_lib(folder, ver, out, wine):
    msys = Layout(root=MAIN_ROOT / 'toolchain' / 'launcher-msys').msys
    app = folder / 'app' / ver
    args = ['--make-runtime-lib', app / 'lib', '--obj', out / 'native' / 'runtime-obj', '--msys', msys]
    if os.name == 'nt':
        run([sys.executable, app / 'tools' / 'make_game.py', *args])
    elif wine:
        def win(p):
            return 'Z:' + str(Path(p).resolve()).replace('/', '\\')
        env = dict(os.environ, WINEDEBUG='-all', NX2SARIA_AR=win(msys / 'bin' / 'ar.exe'))
        run(['wine', folder / 'python' / 'python.exe', win(app / 'tools' / 'make_game.py'),
             *[win(a) if isinstance(a, Path) else a for a in args]], env=env)
    else:
        say('note: no prebuilt runtime (it is built on Windows, or with --wine): players compile runtime/ themselves')
        return
    if not (app / 'lib' / 'libnx2navirt.a').is_file():
        sys.exit('the prebuilt runtime was not written')
    built = json.loads((app / 'lib' / 'runtime-lib.json').read_text(encoding='utf-8'))
    if (app / 'data' / 'pgo-v111.profdata').is_file() and not built.get('pgo'):
        sys.exit('the prebuilt runtime was compiled without the release\'s execution profile (the build above says '
                 'why): the players\' builds would leave the profile out as well')


def build_native(work, cc, windres):
    from launcher.ui import art
    work.mkdir(parents=True, exist_ok=True)
    (work / 'nx2saria.ico').write_bytes(art.sprig_ico())
    shutil.copy2(ROOT / 'launcher' / 'native' / 'app.manifest', work / 'app.manifest')
    shutil.copy2(ROOT / 'launcher' / 'native' / 'res.rc', work / 'res.rc')
    res = (work / 'res.o').resolve()
    pp = [] if 'llvm' in Path(windres[0]).name else [
        '--preprocessor=gcc', '--preprocessor-arg=-E', '--preprocessor-arg=-xc', '--preprocessor-arg=-DRC_INVOKED']
    env = dict(os.environ, PATH=str(Path(windres[0]).parent) + os.pathsep + os.environ.get('PATH', ''))
    run([*windres, *pp, '-I', '.', 'res.rc', '-O', 'coff', '-o', res], env=env, cwd=work)
    flags = ['-municode', '-mwindows', '-O2', '-Wall', '-static', '-s']
    stub = work / 'NX2Saria.exe'
    setup = work / 'setup-stub.exe'
    run([*cc, *flags, ROOT / 'launcher' / 'native' / 'nx2saria.c', res, '-o', stub, '-lshell32', '-lole32', '-luuid'])
    run([*cc, *flags, ROOT / 'launcher' / 'native' / 'setup.c', res, '-o', setup, '-l:libz.a', '-lshell32',
         '-lole32', '-luuid', '-ladvapi32'])
    return stub, setup


def zip_dir(src, dest, prefix=''):
    with zipfile.ZipFile(dest, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(src.rglob('*')):
            if p.is_file():
                z.write(p, (Path(prefix) / p.relative_to(src)).as_posix())
    return dest


def make_setup(stub, payload, dest, ver):
    data = stub.read_bytes()
    off = len(data)
    z = payload.read_bytes()
    trailer = b'NX2NSET1' + off.to_bytes(8, 'little') + len(z).to_bytes(8, 'little') + \
        ver.encode('utf-8')[:24].ljust(24, b'\0')
    dest.write_bytes(data + z + trailer)
    return dest


def check_folder(folder):
    bad = [f'{p.relative_to(folder)}: {m}' for p in sorted(folder.rglob('*')) if p.is_file()
           for m in check_content.problems_for(p, allow_large=True)]
    if bad:
        sys.exit('check_content refuses these release files:\n  ' + '\n  '.join(bad[:20]))


def entry(path):
    return {'name': path.name, 'size': path.stat().st_size, 'sha256': sha256_file(path)}


def write_update_manifests(out, manifest):
    paths = tuple(Path(out) / name for name in ('NX2Saria-update.json', 'NX2navi-update.json'))
    data = json.dumps(manifest, indent=1)
    for path in paths:
        path.write_text(data, encoding='utf-8')
    return paths


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--xlate', type=Path, required=True, help='folder with nx2navi_xlate.exe and nx2navi_xlate.id')
    ap.add_argument('--ffx', type=Path, help="AMD's signed amd_fidelityfx_vk.dll (FidelityFX SDK 1.1.4, MIT) for FSR 3.1")
    ap.add_argument('--ffx4', type=Path, help="the folder with AMD's signed amd_fidelityfx_loader_vk.dll and "
                                              'amd_fidelityfx_upscaler_vk.dll (FSR SDK 2.4.0, MIT) for FSR 4, and its '
                                              'amd_fidelityfx_framegeneration_vk.dll when it is there')
    ap.add_argument('--dlss', type=Path, help=f"NVIDIA's nvngx_dlss.dll (DLSS {dlss_pin.VERSION}) from disk instead of "
                                               "downloading it from NVIDIA's DLSS SDK repository")
    ap.add_argument('--no-dlss', action='store_true', help='leave NVIDIA DLSS out of the release')
    ap.add_argument('--dlssg', type=Path, metavar='DLL',
                    help='nx2navi_dlssg.dll (tools/build_dlssg.py) for DLSS Frame Generation')
    ap.add_argument('--streamline', type=Path, metavar='SDK',
                    help=f'the Streamline {streamline_pin.VERSION} SDK folder whose bin/x64 gives the Streamline and '
                         'nvngx_dlssg.dll files, instead of downloading the SDK release with --dlssg')
    ap.add_argument('--prompt-art', type=Path, nargs='+', metavar='FOLDER',
                    help='the button art: folders holding XGamepad, P5Gamepad and Keyboard_Mouse, or those folders '
                         '(default: the repository\'s prompt-art)')
    ap.add_argument('--no-prompt-art', action='store_true',
                    help='carry no button art (the game keeps its own prompts; the build says so in its log)')
    ap.add_argument('--pipeline-list', type=Path, metavar='FILE',
                    help='the shared pipeline list to carry (tools/pipeline_list.py merge writes it; it is derived from game data and stays out of the repository)')
    ap.add_argument('--pgo', type=Path, metavar='FILE',
                    help='the execution profile to carry, with its record beside it (tools/pgo.py merge writes '
                         'build/pgo/pgo-v111.profdata and .json; derived from the game\'s code, they stay out of the '
                         'repository)')
    ap.add_argument('--out', type=Path, default=MAIN_ROOT / 'build' / 'launcher-release')
    ap.add_argument('--feed', default=DEFAULT_FEED, help='the GitHub repository the launcher looks for releases in')
    ap.add_argument('--notes', type=Path, help='release notes (markdown) for the manifest and the GitHub release')
    ap.add_argument('--lock', type=Path, help='a toolchain.lock.json to reuse instead of resolving today\'s packages')
    ap.add_argument('--version', help='the release version instead of VERSION\'s (test releases)')
    ap.add_argument('--wine', action='store_true', help='on Linux: build the prebuilt runtime with MSYS2\'s clang '
                                                         'through Wine')
    ap.add_argument('--publish', action='store_true', help='upload as GitHub release v<version> with gh')
    ap.add_argument('--draft', action='store_true', help='with --publish: as a draft')
    a = ap.parse_args()
    if a.publish and a.feed.lower() == DEFAULT_FEED.lower():
        sys.exit(f'--publish uploads from this PC: {DEFAULT_FEED} releases are made by .github/workflows/release.yml '
                 'when a tag is pushed. --publish is for a feed of your own (--feed owner/name)')
    if sys.version_info < (3, 14):
        sys.exit('Python 3.14 or later is needed (compression.zstd for the MSYS2 packages)')
    ver, build = version()
    if a.version:
        ver, build = a.version, a.version + build[len(ver):]
    a.prompt_art = prompt_art.release_art(a.prompt_art, a.no_prompt_art)
    if a.prompt_art:
        prompt_art.art_files(a.prompt_art)
    if a.pgo:
        pgo.fitting(a.pgo)
    check_xlate(a.xlate)
    if a.dlssg:
        check_dlssg(a.dlssg)
    out = a.out
    shutil.rmtree(out, ignore_errors=True)
    folder = out / 'NX2Saria'
    t0 = time.time()
    lock = (json.loads(a.lock.read_text(encoding='utf-8')) if a.lock
            else toolchain.make_lock(fast_repo=toolchain.FAST_REPO))
    say(f'release {build}: toolchain lock with {len(lock["packages"])} packages'
        + (f' (CLANG64 clang left out: {lock["fast_skipped"]})' if lock.get('fast_skipped') else ''))
    dlss = None if a.no_dlss else dlss_pin.resolve(a.dlss, DL)
    say(f'NVIDIA DLSS {dlss_pin.VERSION}: {dlss}' if dlss else 'NVIDIA DLSS left out (--no-dlss)')
    if a.streamline and not a.dlssg:
        sys.exit('--streamline goes with --dlssg')
    dlssg = (a.dlssg, streamline_pin.resolve(a.streamline, DL)) if a.dlssg else None
    say(f'DLSS Frame Generation: {a.dlssg}, Streamline {streamline_pin.VERSION}: {dlssg[1]}' if dlssg
        else 'DLSS Frame Generation left out (no --dlssg)')
    if a.pipeline_list:
        import pipeline_list
        if pipeline_list.unpack(a.pipeline_list.read_bytes()) is None:
            sys.exit(f'{a.pipeline_list} is not a pipeline list (tools/pipeline_list.py merge writes one)')
    build_app(folder / 'app' / ver, ver, a.xlate, a.feed, lock, a.ffx, dlss, a.prompt_art, dlssg, a.pipeline_list, a.ffx4,
              a.pgo)
    (folder / 'app' / 'current.txt').write_text(ver + '\n', encoding='utf-8')
    build_python(folder / 'python')
    python_texts(folder, ver)
    cc, windres = native_compiler(lock)
    build_runtime_lib(folder, ver, out, a.wine)
    stub, setup_stub = build_native(out / 'native', cc, windres)
    shutil.copy2(stub, folder / 'NX2Saria.exe')
    check_folder(folder)
    app_zip = zip_dir(folder / 'app' / ver, out / f'NX2Saria-app-{ver}.zip')
    rt_zip = zip_dir(folder / 'python', out / f'NX2Saria-runtime-{runtime_id()}.zip')
    payload = zip_dir(folder, out / 'native' / 'payload.zip')
    setup = make_setup(setup_stub, payload, out / f'NX2Saria-Setup-{ver}.exe', ver)
    notes = a.notes.read_text(encoding='utf-8') if a.notes else ''
    manifest = {'format': 1, 'version': ver, 'build': build, 'notes': notes,
                'app': entry(app_zip), 'runtime': {'id': runtime_id(), **entry(rt_zip)},
                'toolchain': toolchain.lock_digest(lock), 'setup': entry(setup),
                'made': time.strftime('%Y-%m-%d %H:%M')}
    man, legacy_man = write_update_manifests(out, manifest)
    for p in (setup, app_zip, rt_zip, man, legacy_man):
        say(f'{p.name}: {p.stat().st_size / 1e6:.1f} MB')
    say(f'release folder {out} in {time.time() - t0:.0f} s')
    if a.publish:
        cmd = ['gh', 'release', 'create', f'v{ver}', setup, app_zip, rt_zip, man, legacy_man, '--repo', a.feed,
               '--title', f'NX2Saria {ver}']
        cmd += ['--notes-file', a.notes] if a.notes else ['--notes', f'NX2Saria {ver}']
        if a.draft:
            cmd.append('--draft')
        run(cmd)


if __name__ == '__main__':
    main()
