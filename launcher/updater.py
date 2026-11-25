import json
import os
import shutil
import zipfile
from pathlib import Path

from . import net, versions

STAGED = 'staged.json'
STAGED_PYTHON = 'python.staged'


class UpdateError(Exception):
    pass


def read_text(path, default=''):
    try:
        return Path(path).read_text(encoding='utf-8').strip()
    except OSError:
        return default


def write_text(path, text):
    path = Path(path)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(text + '\n', encoding='utf-8')
    os.replace(tmp, path)


def current(layout):
    return read_text(layout.apps / 'current.txt') or layout.version


def previous(layout):
    return read_text(layout.apps / 'previous.txt')


def runtime_id(layout):
    return read_text(layout.python / 'runtime-id.txt')


def _linux_runtime(layout):
    return os.name != 'nt' and (layout.python / 'bin' / 'python3').is_file()


def safe_extract(zip_path, dest):
    dest = Path(dest)
    root = dest.resolve()
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            name = info.filename.replace('\\', '/')
            if name.startswith('/') or ':' in name.split('/')[0] or '..' in name.split('/'):
                raise UpdateError(f'{Path(zip_path).name}: unsafe path {info.filename}')
            target = (dest / name).resolve()
            if target != root and root not in target.parents:
                raise UpdateError(f'{Path(zip_path).name}: unsafe path {info.filename}')
        full = net.space_problem(dest, sum(info.file_size for info in z.infolist()),
                                 f'the files of {Path(zip_path).name}')
        if full:
            raise UpdateError(full)
        z.extractall(dest)
        if os.name != 'nt':
            for info in z.infolist():
                mode = (info.external_attr >> 16) & 0o777
                if mode:
                    target = dest / info.filename
                    if target.exists() and not target.is_dir():
                        target.chmod(mode)


def _fetch(release, key, layout, progress, cancel, version, log=None):
    url, headers, size, sha = release.asset(key)
    entry = release.manifest[key]
    dest = layout.downloads / entry['name']
    net.download(url, dest, sha256=sha, size=size, headers=headers, progress=progress, cancel=cancel,
                 version=version, log=log)
    return dest


def stage_app(layout, release, progress=None, cancel=None, version='dev', log=None):
    zip_path = _fetch(release, 'app', layout, progress, cancel, version, log)
    ver = versions.display(release.version)
    final = layout.apps / ver
    part = layout.apps / f'{ver}.part'
    shutil.rmtree(part, ignore_errors=True)
    part.mkdir(parents=True)
    try:
        safe_extract(zip_path, part)
        got = read_text(part / 'VERSION')
        if versions.display(got) != ver or not (part / 'launcher' / 'start.pyw').is_file():
            raise UpdateError(f'{zip_path.name} holds version {got or "?"} without a launcher, not {ver}')
        if final.exists():
            if final.resolve() == layout.app.resolve():
                raise UpdateError(f'{ver} is the version that runs now')
            shutil.rmtree(final)
        os.replace(part, final)
    except BaseException:
        shutil.rmtree(part, ignore_errors=True)
        raise
    zip_path.unlink(missing_ok=True)
    return final


def runtime_needed(layout, release):
    rt = release.manifest.get('runtime')
    return bool(rt) and not layout.dev and rt.get('id') != runtime_id(layout)


def stage_runtime(layout, release, progress=None, cancel=None, version='dev', dest=None, log=None):
    if not runtime_needed(layout, release):
        return False
    zip_path = _fetch(release, 'runtime', layout, progress, cancel, version, log)
    nxt = Path(dest) if dest else layout.root / 'python.next'
    shutil.rmtree(nxt, ignore_errors=True)
    part = nxt.with_name(nxt.name + '.part')
    shutil.rmtree(part, ignore_errors=True)
    part.mkdir()
    safe_extract(zip_path, part)
    runtime_ok = ((part / 'bin' / 'python3').is_file() if _linux_runtime(layout) else (part / 'python.exe').is_file())
    if not runtime_ok or read_text(part / 'runtime-id.txt') != release.manifest['runtime']['id']:
        shutil.rmtree(part, ignore_errors=True)
        raise UpdateError(f'{zip_path.name} is not the runtime {release.manifest["runtime"]["id"]}')
    os.replace(part, nxt)
    zip_path.unlink(missing_ok=True)
    return True


def prerelease(release):
    try:
        return bool(release.prerelease) or versions.parse(release.version)[1] != (1,)
    except ValueError:
        return True


def stage(layout, release, progress=None, cancel=None, version='dev', log=None):
    ver = versions.display(release.version)
    folder = stage_app(layout, release, progress=progress, cancel=cancel, version=version, log=log)
    runtime = ''
    if runtime_needed(layout, release):
        stage_runtime(layout, release, progress=progress, cancel=cancel, version=version,
                      dest=folder / STAGED_PYTHON, log=log)
        runtime = release.manifest['runtime']['id']
    write_text(folder / STAGED, json.dumps({'version': ver, 'runtime': runtime, 'prerelease': prerelease(release),
                                            'notes': release.notes}))
    keep = {ver, current(layout), previous(layout)}
    for d in layout.apps.iterdir():
        if (d.is_dir() and d.name not in keep and (d / STAGED).is_file()
                and d.resolve() != Path(layout.app).resolve()):
            shutil.rmtree(d, ignore_errors=True)
    return folder


def staged_info(layout, ver):
    folder = layout.apps / ver
    try:
        m = json.loads((folder / STAGED).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if not isinstance(m, dict) or m.get('version') != ver:
        return None
    if versions.display(read_text(folder / 'VERSION')) != ver or not (folder / 'launcher' / 'start.pyw').is_file():
        return None
    rt = m.get('runtime') or ''
    if rt and rt != runtime_id(layout) and read_text(folder / STAGED_PYTHON / 'runtime-id.txt') != rt:
        return None
    return m


def staged(layout, channel='stable', skip=''):
    best = None
    if not layout.apps.is_dir():
        return None
    for d in layout.apps.iterdir():
        if not d.is_dir() or d.name == skip or not (d / STAGED).is_file():
            continue
        try:
            if not versions.newer(d.name, layout.version) or (best and not versions.newer(d.name, best['version'])):
                continue
        except ValueError:
            continue
        m = staged_info(layout, d.name)
        if m and (channel == 'preview' or not m.get('prerelease')):
            best = m
    return best


def install_staged(layout, ver, log=None):
    m = staged_info(layout, ver)
    if not m:
        raise UpdateError(f'app/{ver} is not a complete staged release')
    py = layout.apps / ver / STAGED_PYTHON
    nxt = layout.root / 'python.next'
    moved = bool(m.get('runtime')) and m['runtime'] != runtime_id(layout)
    if moved and _linux_runtime(layout):
        live = layout.python
        old = layout.root / 'python.old'
        if live.exists():
            shutil.rmtree(old, ignore_errors=True)
            os.replace(live, old)
        try:
            os.replace(py, live)
            switch_to(layout, ver)
        except BaseException:
            if live.exists():
                shutil.rmtree(live, ignore_errors=True)
            if old.exists():
                os.replace(old, live)
            raise
        if log:
            log(f'Linux runtime switched to {runtime_id(layout)}; previous runtime kept until startup succeeds')
        (layout.apps / ver / STAGED).unlink(missing_ok=True)
        return
    if moved:
        shutil.rmtree(nxt, ignore_errors=True)
        os.replace(py, nxt)
    try:
        switch_to(layout, ver)
    except BaseException:
        if moved:
            os.replace(nxt, py)
        raise
    shutil.rmtree(py, ignore_errors=True)
    (layout.apps / ver / STAGED).unlink(missing_ok=True)


def switch_to(layout, ver):
    if not (layout.apps / ver / 'launcher' / 'start.pyw').is_file():
        raise UpdateError(f'app/{ver} is not a complete release')
    now = current(layout)
    if now != ver:
        write_text(layout.apps / 'previous.txt', now)
    write_text(layout.apps / 'current.txt', ver)


def rollback(layout):
    prev = previous(layout)
    if not prev or not (layout.apps / prev / 'launcher' / 'start.pyw').is_file():
        return False
    write_text(layout.apps / 'current.txt', prev)
    (layout.apps / 'previous.txt').unlink(missing_ok=True)
    return True


def prune(layout):
    now = current(layout)
    keep = {now, previous(layout)}
    if layout.apps.is_dir():
        for d in layout.apps.iterdir():
            if d.is_dir() and d.name not in keep and not _waiting(layout, d.name, now):
                shutil.rmtree(d, ignore_errors=True)
    if layout.downloads.is_dir():
        for f in layout.downloads.iterdir():
            if f.suffix in ('.part', '.zip', net.PART_OF):
                f.unlink(missing_ok=True)


def _waiting(layout, name, now):
    try:
        return versions.newer(name, now) and staged_info(layout, name) is not None
    except ValueError:
        return False
