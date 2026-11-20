"""Install or repair the Python launcher from the AppImage, then start the installed copy."""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def _xdg(name, fallback):
    value = os.environ.get(name, '')
    path = Path(value) if value else Path(fallback)
    return path if path.is_absolute() else Path(fallback)


def _config():
    cfg_home = _xdg('XDG_CONFIG_HOME', Path.home() / '.config')
    for brand in ('nx2saria', 'nx2navi'):
        path = cfg_home / brand / 'appimage.json'
        try:
            data = json.loads(path.read_text(encoding='utf-8'))
            if isinstance(data, dict):
                return data
        except (OSError, ValueError):
            continue
    return {}


def _log(root, message):
    path = root / 'data' / 'logs' / 'launcher.log'
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('a', encoding='utf-8') as stream:
            try:
                sys.path.insert(0, str(Path(__file__).resolve().parent))
                from private_paths import shorten
                message = shorten(message)
            except Exception:
                pass
            stream.write(f'{time.strftime("%H:%M:%S")} {message[:380]}\n')
    except OSError:
        pass


def _version_key(value):
    try:
        base, _, rc = value.partition('-rc.')
        stable = tuple(int(part) for part in base.split('.'))
        return (*stable, 0, int(rc)) if rc else (*stable, 1, 0)
    except ValueError:
        return ()


def install(bundle):
    bundle = Path(bundle).resolve()
    data_home = _xdg('XDG_DATA_HOME', Path.home() / '.local' / 'share')
    root = data_home / 'nx2saria'
    app = bundle / 'usr' / 'kit'
    py = bundle / 'usr' / 'python'
    version = (app / 'VERSION').read_text(encoding='utf-8').strip()
    identity = _appimage_identity()
    if identity and _already_installed(root, app, py, version, identity):
        entry = root / 'app' / version / 'launcher' / 'start.pyw'
        pyexe = root / 'python' / 'bin' / 'python3'
        _log(root, f'AppImage: {version} already installed')
        return root, pyexe, entry
    lock_path = root / 'data' / 'launcher.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock = lock_path.open('a+')
    import fcntl
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as e:
        lock.close()
        message = 'Close the NX2Saria launcher before repairing it with the AppImage, then try again.'
        _log(root, f'AppImage install refused: {message}')
        raise RuntimeError(message) from e
    try:
        return _install(bundle, root)
    finally:
        fcntl.flock(lock, fcntl.LOCK_UN)
        lock.close()


def _install(bundle, root):
    bundle = Path(bundle).resolve()
    app = bundle / 'usr' / 'kit'
    py = bundle / 'usr' / 'python'
    version = (app / 'VERSION').read_text(encoding='utf-8').strip()
    if not (app / 'launcher' / 'start.pyw').is_file() or not (py / 'bin' / 'python3').is_file():
        raise RuntimeError('The AppImage does not carry the launcher and Python runtime.')
    target = root / 'app' / version
    stage = root / 'app' / f'{version}.part'
    backup = root / 'app' / f'{version}.old'
    live_python = root / 'python'
    py_stage = root / 'python.new'
    old_python = root / 'python.old'
    data_home = _xdg('XDG_DATA_HOME', Path.home() / '.local' / 'share')
    apps = data_home / 'applications'
    # An older AppImage can repair the desktop entry, but cannot replace a newer working installation.
    try:
        current_version = (root / 'app' / 'current.txt').read_text(encoding='utf-8').strip()
        installed = root / 'app' / current_version
        current_ok = (installed / 'VERSION').read_text(encoding='utf-8').strip() == current_version
        current_ok &= (root / 'python' / 'bin' / 'python3').is_file()
    except OSError:
        current_version, current_ok = '', False
    if current_ok and _version_key(current_version) > _version_key(version):
        entry = installed / 'launcher' / 'start.pyw'
        if not entry.is_file():
            raise RuntimeError('A newer launcher version is selected but is incomplete; repair it with the latest AppImage.')
        _write_desktop(root, apps)
        _log(root, f'AppImage older than installed {current_version}; repaired desktop shortcut only')
        message = f'NX2Saria {current_version} is newer than this AppImage ({version}). The existing installation was kept.'
        if shutil.which('kdialog'):
            subprocess.run(['kdialog', '--sorry', message, '--title', 'NX2Saria'])
        elif shutil.which('zenity'):
            subprocess.run(['zenity', '--warning', '--title=NX2Saria', f'--text={message}'])
        return root, root / 'python' / 'bin' / 'python3', entry
    _write_wrapper(root)
    if not target.exists() and backup.exists():
        os.replace(backup, target)
    if not live_python.exists() and old_python.exists():
        os.replace(old_python, live_python)
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    for name in ('launcher', 'recomp', 'runtime', 'tools', 'bin', 'prompt-art', 'toolchain'):
        src = app / name
        if src.is_dir():
            shutil.copytree(src, stage / name)
    for name in ('VERSION', 'LICENSE', 'LICENSE-ADDITIONS', 'THIRD_PARTY_NOTICES.md', 'nx2saria.ini.example'):
        src = app / name
        if src.is_file():
            shutil.copy2(src, stage / name)
    shutil.rmtree(py_stage, ignore_errors=True)
    shutil.copytree(py, py_stage, symlinks=True)
    target.parent.mkdir(parents=True, exist_ok=True)
    target_moved = target_installed = python_moved = python_installed = False
    try:
        shutil.rmtree(backup, ignore_errors=True)
        if target.exists():
            os.replace(target, backup)
            target_moved = True
        os.replace(stage, target)
        target_installed = True
        shutil.rmtree(old_python, ignore_errors=True)
        if live_python.exists():
            os.replace(live_python, old_python)
            python_moved = True
        os.replace(py_stage, live_python)
        python_installed = True
        current_tmp = root / 'app' / 'current.txt.tmp'
        current_tmp.write_text(version + '\n', encoding='utf-8')
        os.replace(current_tmp, root / 'app' / 'current.txt')
    except BaseException:
        if target_installed:
            shutil.rmtree(target, ignore_errors=True)
        if target_moved and backup.exists():
            os.replace(backup, target)
        if python_installed:
            shutil.rmtree(live_python, ignore_errors=True)
        if python_moved and old_python.exists():
            os.replace(old_python, live_python)
        shutil.rmtree(stage, ignore_errors=True)
        shutil.rmtree(py_stage, ignore_errors=True)
        raise
    shutil.rmtree(backup, ignore_errors=True)
    shutil.rmtree(old_python, ignore_errors=True)
    (root / 'app' / 'previous.txt').unlink(missing_ok=True)
    cfg = _config()
    launcher_settings = root / 'data' / 'launcher.json'
    try:
        settings = json.loads(launcher_settings.read_text(encoding='utf-8'))
        if not isinstance(settings, dict):
            raise ValueError('launcher settings are not an object')
    except (OSError, ValueError):
        if launcher_settings.exists():
            launcher_settings.replace(launcher_settings.with_suffix('.json.bad'))
        settings = {}
    # The old AppImage's `out` is the user's existing game folder. Adopt it in place.
    legacy_dump = cfg.get('nup', cfg.get('dump', ''))
    settings.setdefault('dump', legacy_dump)
    settings.setdefault('game_dir', cfg.get('out', ''))
    if not settings.get('dump'):
        settings['dump'] = legacy_dump
    if not settings.get('game_dir'):
        settings['game_dir'] = cfg.get('out', '')
    launcher_settings.parent.mkdir(parents=True, exist_ok=True)
    launcher_settings.write_text(json.dumps(settings, indent=1) + '\n', encoding='utf-8')
    legacy_data = data_home / 'nx2navi' / 'pydeps'
    new_deps = root / 'pydeps'
    if legacy_data.is_dir() and not new_deps.exists():
        shutil.copytree(legacy_data, new_deps)
    icon = root / 'nx2saria.png'
    bundled_icon = bundle / 'nx2saria.png'
    pyexe = root / 'python' / 'bin' / 'python3'
    entry = target / 'launcher' / 'start.pyw'
    _write_desktop(root, apps)
    if bundled_icon.is_file():
        _replace_if_different(icon, bundled_icon.read_bytes(), 0o644)
    _write_install_manifest(root, app, py, version, _appimage_identity())
    _log(root, f'AppImage installed launcher {version} in {root}')
    if cfg:
        _log(root, 'Adopted existing AppImage settings for dump and game folder')
    return root, pyexe, entry


def _write_wrapper(root):
    wrapper = root / 'nx2saria-launcher'
    staged = wrapper.with_name(wrapper.name + '.new')
    staged.write_text('#!/bin/sh\n'
                       'ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1\n'
                       'unset PYTHONPATH PYTHONHOME\n'
                       'VERSION=$(cat "$ROOT/app/current.txt") || exit 1\n'
                       'started=$(date +%s%N)\n'
                       '"$ROOT/python/bin/python3" -I "$ROOT/app/$VERSION/launcher/start.pyw" "$@"\n'
                       'status=$?\n'
                       'elapsed=$(( $(date +%s%N) - started ))\n'
                       'if [ "$status" -ne 0 ] && [ "$elapsed" -le 6000000000 ]; then\n'
                       '  PREVIOUS=$(cat "$ROOT/app/previous.txt" 2>/dev/null)\n'
                       '  if [ -n "$PREVIOUS" ] && [ "$PREVIOUS" != "$VERSION" ] && [ -f "$ROOT/app/$PREVIOUS/launcher/start.pyw" ]; then\n'
                       '    mkdir -p "$ROOT/data/logs"\n'
                       '    printf "%s wrapper: %s exited %s; back to %s\\n" "$(date +%H:%M:%S)" "$VERSION" "$status" "$PREVIOUS" >> "$ROOT/data/logs/launcher.log"\n'
                       '    if printf "%s\\n" "$PREVIOUS" > "$ROOT/app/current.txt.tmp" && mv "$ROOT/app/current.txt.tmp" "$ROOT/app/current.txt"; then\n'
                       '      if [ -d "$ROOT/python.old" ]; then rm -rf "$ROOT/python.failed"; mv "$ROOT/python" "$ROOT/python.failed" && mv "$ROOT/python.old" "$ROOT/python"; fi\n'
                       '      "$ROOT/python/bin/python3" -I "$ROOT/app/$PREVIOUS/launcher/start.pyw" "$@" --rolled-back "$VERSION"\n'
                       '      status=$?\n'
                       '    fi\n'
                       '  fi\n'
                       'fi\n'
                       'exit "$status"\n', encoding='utf-8')
    staged.chmod(0o755)
    os.replace(staged, wrapper)


def _write_desktop(root, apps):
    apps.mkdir(parents=True, exist_ok=True)
    icon = root / 'nx2saria.png'
    wrapper = root / 'nx2saria-launcher'
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from appimage_gui import exec_quote
    desktop = ('[Desktop Entry]\nType=Application\nName=NX2Saria\n'
               f'Icon={icon}\n'
               f'Exec={exec_quote(wrapper)}\nTerminal=false\nCategories=Game;\n')
    _replace_if_different(apps / 'nx2saria.desktop', desktop.encode('utf-8'), 0o644)


def _replace_if_different(path, content, mode=0o644):
    path = Path(path)
    try:
        if path.read_bytes() == content:
            return False
    except OSError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    staged = path.with_name(path.name + '.new')
    staged.write_bytes(content)
    staged.chmod(mode)
    os.replace(staged, path)
    return True


def _appimage_identity():
    source = os.environ.get('APPIMAGE', '')
    if not source:
        return None
    try:
        path = Path(source).resolve(strict=True)
        info = path.stat()
        if not path.is_file():
            return None
        return {'path': str(path), 'device': info.st_dev, 'inode': info.st_ino,
                'size': info.st_size, 'mtime_ns': info.st_mtime_ns}
    except OSError:
        return None


def _inventory(root, names=None):
    root = Path(root)
    files = []
    paths = [root / name for name in names] if names is not None else [root]
    for path in paths:
        if path.is_file():
            files.append([path.relative_to(root).as_posix(), path.stat().st_size])
        elif path.is_dir():
            files.extend([child.relative_to(root).as_posix(), child.stat().st_size]
                         for child in path.rglob('*') if child.is_file())
    return sorted(files)


def _install_manifest(root):
    try:
        value = json.loads((Path(root) / 'data' / 'appimage-install.json').read_text(encoding='utf-8'))
        return value if isinstance(value, dict) else None
    except (OSError, ValueError):
        return None


def _write_install_manifest(root, app, py, version, identity):
    value = {'version': version, 'appimage': identity,
             'app': _inventory(app, ('launcher', 'recomp', 'runtime', 'tools', 'bin', 'prompt-art', 'toolchain',
                                     'VERSION', 'LICENSE', 'LICENSE-ADDITIONS', 'THIRD_PARTY_NOTICES.md',
                                     'nx2saria.ini.example')),
             'python': _inventory(py)}
    _replace_if_different(Path(root) / 'data' / 'appimage-install.json',
                          (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode('utf-8'))


def _already_installed(root, app, py, version, identity):
    manifest = _install_manifest(root)
    if not manifest or manifest.get('version') != version or manifest.get('appimage') != identity:
        return False
    target = Path(root) / 'app' / version
    live_python = Path(root) / 'python'
    if not (target / 'launcher' / 'start.pyw').is_file() or not (live_python / 'bin' / 'python3').is_file():
        return False
    app_names = ('launcher', 'recomp', 'runtime', 'tools', 'bin', 'prompt-art', 'toolchain',
                 'VERSION', 'LICENSE', 'LICENSE-ADDITIONS', 'THIRD_PARTY_NOTICES.md', 'nx2saria.ini.example')
    return (manifest.get('app') == _inventory(app, app_names) == _inventory(target, app_names)
            and manifest.get('python') == _inventory(py) == _inventory(live_python))


def main():
    data_home = _xdg('XDG_DATA_HOME', Path.home() / '.local' / 'share')
    root = data_home / 'nx2saria'
    try:
        bundle = Path(sys.argv[1])
        root, py, entry = install(bundle)
    except Exception as e:
        message = str(e)
        _log(root, f'AppImage installer failed: {type(e).__name__}: {message}')
        if shutil.which('kdialog'):
            subprocess.run(['kdialog', '--error', message, '--title', 'NX2Saria'])
        elif shutil.which('zenity'):
            subprocess.run(['zenity', '--error', '--title=NX2Saria', f'--text={message}'])
        else:
            print(message, file=sys.stderr)
        return 1
    source = Path(os.environ.get('APPIMAGE', bundle)).resolve().parent
    try:
        os.execv(str(py), [str(py), '-I', str(entry), '--from', str(source)])
    except OSError as e:
        _log(root, f'AppImage launcher start failed: {type(e).__name__}: {e}')
        print(f'Could not start the installed NX2Saria launcher: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    main()
