"""Package the launcher app and relocatable Python runtime for a Linux release."""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP_DIRS = ('launcher', 'recomp', 'runtime', 'tools', 'prompt-art', 'toolchain')
APP_FILES = ('VERSION', 'LICENSE', 'LICENSE-ADDITIONS', 'THIRD_PARTY_NOTICES.md', 'nx2saria.ini.example')
SKIP = {'vendor', 'xlate', '__pycache__', '.pytest_cache'}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def add_tree(z, src, prefix):
    for p in src.rglob('*'):
        if any(part in SKIP for part in p.relative_to(src).parts):
            continue
        if p.is_file() and not p.name.endswith('.pyc'):
            z.write(p, (Path(prefix) / p.relative_to(src)).as_posix())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--python', type=Path, required=True)
    ap.add_argument('--version', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--notes', type=Path)
    ap.add_argument('--appimage', type=Path, required=True)
    ap.add_argument('--kit', type=Path, required=True)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    ver = a.version.removeprefix('v')
    sys.path.insert(0, str(ROOT / 'tools'))
    from make_appimage import PACKAGES
    from linux_runtime import runtime_id as get_runtime_id
    runtime_id = get_runtime_id(a.python / 'bin' / 'python3', PACKAGES)
    build_sha = subprocess.run(['git', 'rev-parse', '--short=12', 'HEAD'], cwd=ROOT,
                               check=True, capture_output=True, text=True).stdout.strip()
    app = a.out / f'NX2Saria-app-linux-x86_64-{ver}.zip'
    with zipfile.ZipFile(app, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for name in APP_DIRS:
            src = ROOT / name
            if name in ('toolchain', 'prompt-art') and a.kit.joinpath(name).is_dir():
                src = a.kit / name
            if src.is_dir():
                add_tree(z, src, name)
        for name in APP_FILES:
            src = ROOT / name
            if src.is_file():
                z.write(src, name)
        add_tree(z, a.kit / 'bin', 'bin')
        z.writestr('feed.json', json.dumps({'type': 'github', 'repo': 'Larbaco/NX2Saria'}, indent=1) + '\n')
    runtime_prefix = 'linux-x86_64-'
    if not runtime_id.startswith(runtime_prefix):
        raise SystemExit(f'Linux runtime id has no {runtime_prefix!r} prefix: {runtime_id!r}')
    runtime_suffix = runtime_id.removeprefix(runtime_prefix)
    runtime = a.out / f'NX2Saria-runtime-linux-x86_64-{runtime_suffix}.zip'
    with zipfile.ZipFile(runtime, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in a.python.rglob('*'):
            if (p.is_file() and '__pycache__' not in p.parts
                    and p.name not in ('EXTERNALLY-MANAGED', 'runtime-id.txt')):
                z.write(p, p.relative_to(a.python))
        z.writestr('runtime-id.txt', runtime_id + '\n')
    files = {}
    appimage = a.appimage
    for key, path in (('app', app), ('runtime', runtime), ('appimage', appimage)):
        files[key] = {'name': path.name, 'size': path.stat().st_size, 'sha256': digest(path)}
    files['runtime']['id'] = runtime_id
    notes = a.notes.read_text(encoding='utf-8').replace('\r\n', '\n') if a.notes else ''
    manifest = {'format': 1, 'version': ver, 'build': f'{ver}+{build_sha}', 'notes': notes,
                **files, 'toolchain': 'linux-x86_64'}
    (a.out / 'NX2Saria-update-linux.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
