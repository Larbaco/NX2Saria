"""Install or repair the Python launcher from the AppImage, then start the installed copy."""
import json
import os
import shutil
import subprocess
from pathlib import Path


def _config():
    cfg_home = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config')
    for brand in ('nx2saria', 'nx2navi'):
        path = cfg_home / brand / 'appimage.json'
        try:
            data = json.loads(path.read_text(encoding='utf-8'))
            if isinstance(data, dict):
                return data
        except (OSError, ValueError):
            continue
    return {}


def install(bundle):
    bundle = Path(bundle).resolve()
    root = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local' / 'share') / 'nx2saria'
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
    try:
        shutil.rmtree(backup, ignore_errors=True)
        if target.exists():
            os.replace(target, backup)
        os.replace(stage, target)
        shutil.rmtree(old_python, ignore_errors=True)
        if live_python.exists():
            os.replace(live_python, old_python)
        os.replace(py_stage, live_python)
        current_tmp = root / 'app' / 'current.txt.tmp'
        current_tmp.write_text(version + '\n', encoding='utf-8')
        os.replace(current_tmp, root / 'app' / 'current.txt')
    except BaseException:
        shutil.rmtree(target, ignore_errors=True)
        if backup.exists():
            os.replace(backup, target)
        shutil.rmtree(live_python, ignore_errors=True)
        if old_python.exists():
            os.replace(old_python, live_python)
        shutil.rmtree(stage, ignore_errors=True)
        shutil.rmtree(py_stage, ignore_errors=True)
        raise
    shutil.rmtree(backup, ignore_errors=True)
    shutil.rmtree(old_python, ignore_errors=True)
    (root / 'app').mkdir(exist_ok=True)
    (root / 'app' / 'current.txt').write_text(version + '\n', encoding='utf-8')
    cfg = _config()
    launcher_settings = root / 'data' / 'launcher.json'
    try:
        settings = json.loads(launcher_settings.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        settings = {}
    # The old AppImage's `out` is the user's existing game folder. Adopt it in place.
    legacy_dump = cfg.get('nup', cfg.get('dump', ''))
    settings.setdefault('dump', legacy_dump)
    settings.setdefault('game_dir', cfg.get('out', str(Path.home() / 'Games' / 'Ocarina')))
    if not settings.get('dump'):
        settings['dump'] = legacy_dump
    if not settings.get('game_dir'):
        settings['game_dir'] = cfg.get('out', str(Path.home() / 'Games' / 'Ocarina'))
    launcher_settings.parent.mkdir(parents=True, exist_ok=True)
    launcher_settings.write_text(json.dumps(settings, indent=1) + '\n', encoding='utf-8')
    legacy_data = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local' / 'share') / 'nx2navi' / 'pydeps'
    new_deps = root / 'pydeps'
    if legacy_data.is_dir() and not new_deps.exists():
        shutil.copytree(legacy_data, new_deps)
    apps = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local' / 'share') / 'applications'
    apps.mkdir(parents=True, exist_ok=True)
    icon = root / 'nx2saria.png'
    bundled_icon = bundle / 'nx2saria.png'
    if bundled_icon.is_file():
        shutil.copy2(bundled_icon, icon)
    pyexe = root / 'python' / 'bin' / 'python3'
    entry = target / 'launcher' / 'start.pyw'
    wrapper = root / 'nx2saria-launcher'
    wrapper.write_text('#!/bin/sh\n'
                       'ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1\n'
                       'unset PYTHONPATH PYTHONHOME\n'
                       'VERSION=$(cat "$ROOT/app/current.txt") || exit 1\n'
                       'exec "$ROOT/python/bin/python3" "$ROOT/app/$VERSION/launcher/start.pyw" "$@"\n',
                       encoding='utf-8')
    wrapper.chmod(0o755)
    desktop = ('[Desktop Entry]\nType=Application\nName=NX2Saria\n'
               f'Icon={icon}\n'
               f'Exec="{wrapper}"\nTerminal=false\nCategories=Game;\n')
    (apps / 'nx2saria.desktop').write_text(desktop, encoding='utf-8')
    return root, pyexe, entry


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
    os.execv(str(py), [str(py), str(entry)])


if __name__ == '__main__':
    main()
