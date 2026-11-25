# needs: py:zstandard, py:PIL, py:fontTools, py:tkinter
import contextlib
import io
import os
import sys
import tempfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import appimage_gui  # noqa: E402
import capture_run  # noqa: E402
import make_appimage  # noqa: E402
import package_build  # noqa: E402
import pad_prompts  # noqa: E402


def check(ok, message):
    if not ok:
        raise AssertionError(message)


def package(exe_name, payload, out):
    with tempfile.TemporaryDirectory(prefix='nx2saria-package-', dir=ROOT / 'build') as temp:
        root = Path(temp)
        build = root / 'build'
        extracted = root / 'extracted' / 'v121'
        xlate = root / 'xlate' / 'nx2navi_xlate'
        (build / exe_name).parent.mkdir(parents=True, exist_ok=True)
        (build / exe_name).write_bytes(payload)
        (build / 'romfs-patches').mkdir(parents=True)
        (build / 'romfs-index.bin').write_bytes(b'index')
        (extracted / 'exefs').mkdir(parents=True)
        (root / 'extracted' / 'firmware' / 'shared-fonts').mkdir(parents=True)
        xlate.parent.mkdir(parents=True)
        xlate.write_bytes(b'translator')
        out_dir = root / out
        args = ['package_build.py', '--build', str(build), '--extracted', str(extracted), '--out', str(out_dir),
                '--xlate', str(xlate)]
        with mock.patch.object(sys, 'argv', args), contextlib.redirect_stdout(io.StringIO()):
            package_build.main()
        return ((out_dir / f'nx2saria{package_build.EXE}').read_bytes(),
                (out_dir / f'nx2navi{package_build.EXE}').exists())


def main():
    check('nx2saria.ini.example' in make_appimage.KIT_FILES, 'AppImage kit includes the current settings example')
    check('Name=NX2Saria' in make_appimage.DESKTOP and 'Icon=nx2saria' in make_appimage.DESKTOP,
          'AppImage desktop entry uses the current product and icon names')
    check(appimage_gui.TEXT['en']['title'] == 'NX2Saria for Linux' and
          appimage_gui.TEXT['fr']['title'] == 'NX2Saria pour Linux',
          'AppImage setup window uses the current product name in both supported locales')
    check('nx2saria.png' in make_appimage.APPRUN and 'nx2navi.png' not in make_appimage.APPRUN,
          'AppRun uses the current icon name')

    with tempfile.TemporaryDirectory(prefix='nx2saria-brand-path-') as temp:
        root = Path(temp)
        legacy = root / 'nx2navi'
        legacy.mkdir()
        check(appimage_gui._brand_dir(root) == legacy, 'existing AppImage data folder is reused when no new folder exists')
        current = root / 'nx2saria'
        current.mkdir()
        check(appimage_gui._brand_dir(root) == current, 'current AppImage data folder takes precedence')

        out = root / 'game'
        out.mkdir()
        source_icon = root / 'sprig.png'
        source_icon.write_bytes(b'png')
        app = appimage_gui.App.__new__(appimage_gui.App)
        app.out, app.icon = str(out), source_icon
        apps = root / 'applications'
        with mock.patch.object(appimage_gui, 'APPS', apps):
            app.add_shortcut()
        desktop = apps / 'nx2saria-ocarina.desktop'
        check(desktop.is_file(), 'shortcut is written under the current desktop filename')
        data = desktop.read_text(encoding='utf-8')
        check('Name=Ocarina of Time (NX2Saria)' in data and 'Icon=' + str(out / 'nx2saria.png') in data,
              'shortcut title and icon path use the current brand')
        check((out / 'nx2saria.png').read_bytes() == b'png', 'shortcut copies the leaf icon to the game folder')

        binds = root / 'binds'
        binds.mkdir()
        legacy_ini = binds / 'nx2navi.ini'
        current_ini = binds / 'nx2saria.ini'
        legacy_ini.write_text('bind_a = key:legacy\n', encoding='utf-8')
        check(pad_prompts.default_binds(binds) == legacy_ini, 'prompt generation reuses legacy-only settings')
        current_ini.write_text('bind_a = key:current\n', encoding='utf-8')
        check(pad_prompts.default_binds(binds) == current_ini, 'prompt generation prefers current settings')

        install_base = root / 'LocalAppData'
        legacy_game = install_base / 'Programs' / 'NX2navi' / 'game'
        current_game = install_base / 'Programs' / 'NX2Saria' / 'game'
        legacy_game.mkdir(parents=True)
        with mock.patch.object(capture_run, 'WIN', True), mock.patch.dict(os.environ,
                                                                         {'LOCALAPPDATA': str(install_base)}):
            check(capture_run.default_game() == legacy_game,
                  'capture helper locates an existing legacy launcher game folder')
            current_game.mkdir(parents=True)
            check(capture_run.default_game() == current_game,
                  'capture helper prefers the current launcher game folder when both exist')
        (legacy_game / 'nx2navi.exe').write_bytes(b'legacy')
        check(capture_run.find_exe(legacy_game).name == 'nx2navi.exe',
              'capture helper still recognizes the legacy Windows runtime executable')

    current_payload, current_old = package(f'nx2saria{package_build.EXE}', b'current-build', 'portable-current')
    legacy_payload, legacy_old = package(f'nx2navi{package_build.EXE}', b'legacy-build', 'portable-legacy')
    check(current_payload == b'current-build' and not current_old, 'portable package uses nx2saria.exe for current builds')
    check(legacy_payload == b'legacy-build' and not legacy_old,
          'portable package accepts an older build input but emits nx2saria.exe')
    print('branding_package_check: ok')


if __name__ == '__main__':
    main()
