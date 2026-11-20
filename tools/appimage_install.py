"""Install or repair the Python launcher from the AppImage, then start the installed copy."""
import json
import os
import shutil
import subprocess
import sys
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
            stream.write(message[:400] + '\n')
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
    lock_path = root / 'data' / 'launcher.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock = lock_path.open('a+')
    import fcntl
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as e:
        lock.close()
        raise RuntimeError('Close the NX2Saria launcher before repairing it with the AppImage, then try again.') from e
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
    _write_wrapper(root)
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
    if bundled_icon.is_file():
        shutil.copy2(bundled_icon, icon)
    pyexe = root / 'python' / 'bin' / 'python3'
    entry = target / 'launcher' / 'start.pyw'
    _write_desktop(root, apps)
    _log(root, f'AppImage installed launcher {version} in {root}')
    if cfg:
        _log(root, 'Adopted existing AppImage settings for dump and game folder')
    return root, pyexe, entry


def _write_wrapper(root):
    wrapper = root / 'nx2saria-launcher'
    wrapper.write_text('#!/bin/sh\n'
                       'ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1\n'
                       'unset PYTHONPATH PYTHONHOME\n'
                       'VERSION=$(cat "$ROOT/app/current.txt") || exit 1\n'
                       '"$ROOT/python/bin/python3" -I "$ROOT/app/$VERSION/launcher/start.pyw" "$@"\n'
                       'status=$?\n'
                       'if [ "$status" -eq 86 ]; then\n'
                       '  PREVIOUS=$(cat "$ROOT/app/previous.txt" 2>/dev/null)\n'
                       '  if [ -n "$PREVIOUS" ] && [ -f "$ROOT/app/$PREVIOUS/launcher/start.pyw" ]; then\n'
                       '    printf "%s\\n" "$PREVIOUS" > "$ROOT/app/current.txt.tmp" && mv "$ROOT/app/current.txt.tmp" "$ROOT/app/current.txt"\n'
                       '    rm -f "$ROOT/app/previous.txt"\n'
                       '    if [ -d "$ROOT/python.old" ]; then rm -rf "$ROOT/python.failed"; mv "$ROOT/python" "$ROOT/python.failed" && mv "$ROOT/python.old" "$ROOT/python"; fi\n'
                       '    "$ROOT/python/bin/python3" -I "$ROOT/app/$PREVIOUS/launcher/start.pyw" "$@"\n'
                       '    status=$?\n'
                       '  fi\n'
                       'fi\n'
                       'if [ "$status" -eq 0 ]; then rm -f "$ROOT/app/previous.txt"; rm -rf "$ROOT/python.old" "$ROOT/python.failed"; fi\n'
                       'exit "$status"\n', encoding='utf-8')
    wrapper.chmod(0o755)


def _write_desktop(root, apps):
    apps.mkdir(parents=True, exist_ok=True)
    icon = root / 'nx2saria.png'
    wrapper = root / 'nx2saria-launcher'
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from appimage_gui import exec_quote
    desktop = ('[Desktop Entry]\nType=Application\nName=NX2Saria\n'
               f'Icon={icon}\n'
               f'Exec={exec_quote(wrapper)}\nTerminal=false\nCategories=Game;\n')
    (apps / 'nx2saria.desktop').write_text(desktop, encoding='utf-8')


def main():
    bundle = Path(sys.argv[1])
    try:
        root, py, entry = install(bundle)
    except RuntimeError as e:
        message = str(e)
        if shutil.which('kdialog'):
            subprocess.run(['kdialog', '--error', message, '--title', 'NX2Saria'])
        elif shutil.which('zenity'):
            subprocess.run(['zenity', '--error', '--title=NX2Saria', f'--text={message}'])
        else:
            print(message, file=sys.stderr)
        return 1
    os.execv(str(py), [str(py), '-I', str(entry)])


if __name__ == '__main__':
    main()
