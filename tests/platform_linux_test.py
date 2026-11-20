"""Fast checks for Linux platform services; no display, game data or build tools required."""
import ctypes
import hashlib
import json
import os
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path
import unittest
from unittest import mock

from launcher.platform import linux
from launcher import winsys
from launcher import build
from launcher import feed
from launcher import jobs
from launcher import updater
from launcher.paths import Layout
from launcher.context import Context


class LinuxPlatformTest(unittest.TestCase):
    def _installer_bundle(self, base):
        bundle = base / 'bundle'
        kit = bundle / 'usr' / 'kit'
        py = bundle / 'usr' / 'python' / 'bin'
        (kit / 'launcher').mkdir(parents=True)
        (kit / 'launcher' / 'start.pyw').write_text('')
        (kit / 'VERSION').write_text('0.19.0\n')
        py.mkdir(parents=True)
        exe = py / 'python3'
        exe.write_text('#!/bin/sh\nexit 0\n')
        exe.chmod(0o755)
        (bundle / 'nx2saria.png').write_bytes(b'icon')
        return bundle

    def test_linux_build_args_use_system_toolchain_without_msys(self):
        class Layout:
            msys = Path('/not-used/msys')
        with mock.patch.object(build.os, 'name', 'posix'):
            args = build.install_args(Layout(), '/tmp/dump.nup', '/tmp/game')
        self.assertNotIn('--msys', args)

    def test_linux_game_running_checks_profile_lock(self):
        with tempfile.TemporaryDirectory() as tmp:
            lock_path = Path(tmp) / 'profile' / 'instance.lock'
            lock_path.parent.mkdir()
            lock_path.touch()
            with lock_path.open('a+b') as owner:
                linux.fcntl.flock(owner, linux.fcntl.LOCK_EX | linux.fcntl.LOCK_NB)
                self.assertTrue(linux.game_running(tmp))
            self.assertFalse(linux.game_running(tmp))

    def test_appimage_installer_adopts_existing_game_folder(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = base / 'bundle'
            kit = bundle / 'usr' / 'kit'
            py = bundle / 'usr' / 'python' / 'bin'
            (kit / 'launcher').mkdir(parents=True)
            (kit / 'launcher' / 'start.pyw').write_text('')
            (kit / 'VERSION').write_text('0.19.0\n')
            py.mkdir(parents=True)
            (py / 'python3').write_text('python')
            (bundle / 'nx2saria.png').write_bytes(b'icon')
            config = base / 'config' / 'nx2navi'
            config.mkdir(parents=True)
            old_game = base / 'existing game'
            dump = base / 'game dump.nup'
            (config / 'appimage.json').write_text(json.dumps({'nup': str(dump), 'out': str(old_game)}))
            env = {'XDG_DATA_HOME': str(base / 'share'), 'XDG_CONFIG_HOME': str(base / 'config')}
            with mock.patch.dict(os.environ, env, clear=False):
                root, exe, entry = appimage_install.install(bundle)
                settings = json.loads((root / 'data' / 'launcher.json').read_text())
                desktop = (base / 'share' / 'applications' / 'nx2saria.desktop').read_text()
            self.assertEqual(settings['game_dir'], str(old_game))
            self.assertEqual(settings['dump'], str(dump))
            self.assertEqual(entry, root / 'app' / '0.19.0' / 'launcher' / 'start.pyw')
            wrapper = root / 'nx2saria-launcher'
            self.assertIn(f'Exec="{wrapper}"', desktop)
            self.assertTrue(os.access(wrapper, os.X_OK))
            self.assertIn('app/current.txt', wrapper.read_text())
            self.assertFalse(wrapper.with_name(wrapper.name + '.new').exists())
            self.assertNotIn(str(bundle), desktop)
            log = (root / 'data' / 'logs' / 'launcher.log').read_text().splitlines()
            self.assertRegex(log[0], r'^\d\d:\d\d:\d\d AppImage installed launcher 0\.19\.0 in ')

    def test_appimage_installer_script_runs_as_subprocess(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = self._installer_bundle(base)
            env = dict(os.environ, HOME=str(base / 'home'), XDG_DATA_HOME=str(base / 'data'),
                       XDG_CONFIG_HOME=str(base / 'config'))
            result = subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] / 'tools' /
                                                           'appimage_install.py'), str(bundle)],
                                    env=env, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((base / 'data' / 'nx2saria' / 'app' / 'current.txt').is_file())

    def test_appimage_main_passes_source_folder_to_launcher(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = self._installer_bundle(base)
            image = base / 'NX2Saria.AppImage'
            image.write_bytes(b'appimage')
            captured = []
            env = {'APPIMAGE': str(image), 'XDG_DATA_HOME': str(base / 'share')}
            with mock.patch.dict(os.environ, env), \
                 mock.patch.object(appimage_install.sys, 'argv', ['appimage_install.py', str(bundle)]), \
                 mock.patch.object(appimage_install.os, 'execv', side_effect=lambda *args: captured.append(args)):
                self.assertIsNone(appimage_install.main())
            self.assertEqual(captured[0][1][-2:], ['--from', str(base)])

    def test_same_appimage_install_is_idempotent_and_does_not_touch_shortcuts(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = self._installer_bundle(base)
            image = base / 'NX2Saria.AppImage'
            image.write_bytes(b'appimage')
            env = {'APPIMAGE': str(image), 'XDG_DATA_HOME': str(base / 'share'),
                   'XDG_CONFIG_HOME': str(base / 'config')}
            root = base / 'share' / 'nx2saria'
            desktop = base / 'share' / 'applications' / 'nx2saria.desktop'
            icon = root / 'nx2saria.png'
            with mock.patch.dict(os.environ, env):
                appimage_install.install(bundle)
                app_dir = root / 'app' / '0.19.0'
                app_pycache = app_dir / 'launcher' / '__pycache__'
                app_pycache.mkdir()
                (app_pycache / 'x.cpython-314.pyc').write_bytes(b'compiled launcher bytecode')
                python_pycache = root / 'python' / 'lib' / 'python3.14' / 'site-packages' / '__pycache__'
                python_pycache.mkdir(parents=True)
                (python_pycache / 'x.cpython-314.pyc').write_bytes(b'compiled runtime bytecode')
                app_dir_mtime = app_dir.stat().st_mtime_ns
                app_cache_mtime = app_pycache.stat().st_mtime_ns
                python_dir_mtime = (root / 'python').stat().st_mtime_ns
                python_cache_mtime = python_pycache.stat().st_mtime_ns
                desktop_mtime = desktop.stat().st_mtime_ns
                icon_mtime = icon.stat().st_mtime_ns
                time.sleep(0.01)
                appimage_install.install(bundle)
            self.assertEqual((root / 'app' / '0.19.0').stat().st_mtime_ns, app_dir_mtime)
            self.assertEqual(app_pycache.stat().st_mtime_ns, app_cache_mtime)
            self.assertEqual((root / 'python').stat().st_mtime_ns, python_dir_mtime)
            self.assertEqual(python_pycache.stat().st_mtime_ns, python_cache_mtime)
            self.assertEqual(desktop.stat().st_mtime_ns, desktop_mtime)
            self.assertEqual(icon.stat().st_mtime_ns, icon_mtime)
            log = (root / 'data' / 'logs' / 'launcher.log').read_text().splitlines()
            self.assertTrue(any('AppImage: 0.19.0 already installed' in line for line in log))

    def test_appimage_install_rollback_preserves_live_data_at_each_rename(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        for fail_at in (1, 2, 3, 4, 5):
            with self.subTest(rename=fail_at), tempfile.TemporaryDirectory() as tmp:
                base = Path(tmp)
                bundle = self._installer_bundle(base)
                root = base / 'share' / 'nx2saria'
                old_app = root / 'app' / '0.19.0'
                old_app.mkdir(parents=True)
                (old_app / 'working.marker').write_text('old app')
                (root / 'app' / 'current.txt').write_text('0.19.0\n')
                old_python = root / 'python' / 'bin'
                old_python.mkdir(parents=True)
                (old_python / 'python3').write_text('old python')
                original_replace = appimage_install.os.replace
                calls = 0

                def fail_one(src, dst):
                    nonlocal calls
                    calls += 1
                    if calls == fail_at:
                        raise OSError(f'rename {fail_at} failed')
                    return original_replace(src, dst)

                with mock.patch.dict(os.environ, {'XDG_DATA_HOME': str(base / 'share'),
                                                  'XDG_CONFIG_HOME': str(base / 'config')}), \
                     mock.patch.object(appimage_install.os, 'replace', side_effect=fail_one):
                    with self.assertRaisesRegex(OSError, f'rename {fail_at} failed'):
                        appimage_install._install(bundle, root)
                self.assertEqual((old_app / 'working.marker').read_text(), 'old app')
                self.assertEqual((old_python / 'python3').read_text(), 'old python')

    def test_successful_appimage_install_clears_stale_previous_version(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = self._installer_bundle(base)
            root = base / 'share' / 'nx2saria'
            (root / 'app').mkdir(parents=True)
            (root / 'app' / 'previous.txt').write_text('0.18.0\n')
            with mock.patch.dict(os.environ, {'XDG_DATA_HOME': str(base / 'share')}):
                appimage_install._install(bundle, root)
            self.assertFalse((root / 'app' / 'previous.txt').exists())

    def test_relative_xdg_paths_are_ignored_and_corrupt_settings_are_preserved(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = self._installer_bundle(base)
            home = base / 'home'
            root = home / '.local' / 'share' / 'nx2saria'
            (root / 'data').mkdir(parents=True)
            settings = root / 'data' / 'launcher.json'
            settings.write_text('{broken')
            with mock.patch.dict(os.environ, {'HOME': str(home), 'XDG_DATA_HOME': 'relative-data',
                                              'XDG_CONFIG_HOME': 'relative-config'}, clear=False):
                appimage_install._install(bundle, root)
            self.assertTrue((root / 'data' / 'launcher.json.bad').is_file())
            loaded = json.loads(settings.read_text())
            self.assertEqual(loaded['game_dir'], '')

    def test_appimage_rollback_wrapper_restores_previous_app_and_runtime(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'share' / 'nx2saria'
            for version in ('0.1.0', '0.2.0'):
                entry = root / 'app' / version / 'launcher' / 'start.pyw'
                entry.parent.mkdir(parents=True)
                entry.write_text('')
            (root / 'app' / 'current.txt').write_text('0.2.0\n')
            (root / 'app' / 'previous.txt').write_text('0.1.0\n')
            (root / 'python' / 'bin').mkdir(parents=True)
            (root / 'python' / 'bin' / 'python3').write_text('#!/bin/sh\ncase "$*" in *0.2.0*) exit 86;; *) exit 0;; esac\n')
            (root / 'python' / 'bin' / 'python3').chmod(0o755)
            (root / 'python' / 'runtime-id.txt').write_text('new\n')
            (root / 'python.old' / 'bin').mkdir(parents=True)
            old_exe = root / 'python.old' / 'bin' / 'python3'
            old_exe.write_text('#!/bin/sh\nexit 0\n')
            old_exe.chmod(0o755)
            (root / 'python.old' / 'runtime-id.txt').write_text('old\n')
            appimage_install._write_wrapper(root)
            args_file = Path(tmp) / 'rolled-back-args'
            fake_python = ('#!/bin/sh\ncase "$2" in *0.2.0*) exit 86;; '
                           '*) printf "%s\\n" "$@" > "$ROLLED_BACK_ARGS"; exit 0;; esac\n')
            (root / 'python' / 'bin' / 'python3').write_text(fake_python)
            old_exe.write_text(fake_python)
            with mock.patch.dict(os.environ, {'ROLLED_BACK_ARGS': str(args_file)}):
                result = subprocess.run([str(root / 'nx2saria-launcher')], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(args_file.read_text().splitlines(),
                             ['-I', str(root / 'app' / '0.1.0' / 'launcher' / 'start.pyw'),
                              '--rolled-back', '0.2.0'])
            self.assertEqual((root / 'app' / 'current.txt').read_text().strip(), '0.1.0')
            self.assertEqual((root / 'python' / 'runtime-id.txt').read_text().strip(), 'old')
            self.assertFalse((root / 'python.old').exists())
            log = (root / 'data' / 'logs' / 'launcher.log').read_text().splitlines()
            self.assertRegex(log[0], r'^\d\d:\d\d:\d\d wrapper: 0\.2\.0 exited 86; back to 0\.1\.0$')
            self.assertEqual((root / 'app' / 'previous.txt').read_text().strip(), '0.1.0')

    def test_wrapper_rolls_back_quick_nonzero_exits(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        for failure in ('126', '127', 'signal'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp) / 'share' / 'nx2saria'
                for version in ('0.1.0', '0.2.0'):
                    entry = root / 'app' / version / 'launcher' / 'start.pyw'
                    entry.parent.mkdir(parents=True)
                    entry.write_text('')
                (root / 'app' / 'current.txt').write_text('0.2.0\n')
                (root / 'app' / 'previous.txt').write_text('0.1.0\n')
                python = root / 'python' / 'bin' / 'python3'
                python.parent.mkdir(parents=True)
                args_file = Path(tmp) / 'args.txt'
                code = ('#!/bin/sh\n'
                        'case "$2" in *0.2.0*)\n'
                        f'  {"kill -TERM $$" if failure == "signal" else f"exit {failure}"};;\n'
                        '  *) printf "%s\\n" "$@" > "$ARGS_FILE"; exit 0;;\n'
                        'esac\n')
                python.write_text(code)
                python.chmod(0o755)
                appimage_install._write_wrapper(root)
                with mock.patch.dict(os.environ, {'ARGS_FILE': str(args_file)}):
                    result = subprocess.run([str(root / 'nx2saria-launcher')], capture_output=True,
                                            text=True, timeout=10)
                expected = 143 if failure == 'signal' else int(failure)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(args_file.read_text().splitlines(),
                                 ['-I', str(root / 'app' / '0.1.0' / 'launcher' / 'start.pyw'),
                                  '--rolled-back', '0.2.0'])
                self.assertEqual((root / 'app' / 'current.txt').read_text().strip(), '0.1.0')
                log = (root / 'data' / 'logs' / 'launcher.log').read_text()
                self.assertIn(f'wrapper: 0.2.0 exited {expected}; back to 0.1.0', log)
                self.assertEqual((root / 'app' / 'previous.txt').read_text().strip(), '0.1.0')

    def test_wrapper_keeps_previous_on_clean_exit_and_skips_same_version_rollback(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        for exit_code in (0, 86):
            with self.subTest(exit_code=exit_code), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                entry = root / 'app' / '0.2.0' / 'launcher' / 'start.pyw'
                entry.parent.mkdir(parents=True)
                entry.write_text('')
                (root / 'app' / 'current.txt').write_text('0.2.0\n')
                (root / 'app' / 'previous.txt').write_text('0.2.0\n')
                python = root / 'python' / 'bin' / 'python3'
                python.parent.mkdir(parents=True)
                python.write_text(f'#!/bin/sh\nexit {exit_code}\n')
                python.chmod(0o755)
                appimage_install._write_wrapper(root)
                result = subprocess.run([str(root / 'nx2saria-launcher')], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, exit_code)
                self.assertEqual((root / 'app' / 'current.txt').read_text().strip(), '0.2.0')
                self.assertEqual((root / 'app' / 'previous.txt').read_text().strip(), '0.2.0')
                self.assertFalse((root / 'data' / 'logs' / 'launcher.log').exists())

    def test_running_new_app_can_refresh_the_installed_wrapper(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'share' / 'nx2saria'
            app = root / 'app' / '0.2.0'
            tools = app / 'tools'
            tools.mkdir(parents=True)
            (tools / 'appimage_install.py').write_text(
                'def _write_wrapper(root):\n'
                '    (root / "nx2saria-launcher.new").write_text("new")\n'
                '    (root / "nx2saria-launcher.new").replace(root / "nx2saria-launcher")\n')
            root.mkdir(parents=True, exist_ok=True)
            wrapper = root / 'nx2saria-launcher'
            wrapper.write_text('old')
            linux.refresh_installed_wrapper(root, app)
            self.assertEqual(wrapper.read_text(), 'new')
            self.assertFalse(wrapper.with_name(wrapper.name + '.new').exists())

    def test_old_appimage_does_not_replace_a_newer_installation(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        import appimage_install
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            bundle = self._installer_bundle(base)
            root = base / 'share' / 'nx2saria'
            current = root / 'app' / '0.20.0-rc.2'
            entry = current / 'launcher' / 'start.pyw'
            entry.parent.mkdir(parents=True)
            entry.write_text('')
            (current / 'VERSION').write_text('0.20.0-rc.2\n')
            (root / 'app' / 'current.txt').write_text('0.20.0-rc.2\n')
            py = root / 'python' / 'bin'
            py.mkdir(parents=True)
            (py / 'python3').write_text('new python')
            wrapper = root / 'nx2saria-launcher'
            wrapper.write_text('new wrapper')
            wrapper.chmod(0o755)
            (root / 'nx2saria.png').write_bytes(b'icon')
            with mock.patch.dict(os.environ, {'XDG_DATA_HOME': str(base / 'share')}), \
                 mock.patch.object(appimage_install.shutil, 'which', return_value=None):
                _, _, chosen = appimage_install._install(bundle, root)
            self.assertEqual(chosen, entry)
            self.assertEqual((root / 'app' / 'current.txt').read_text().strip(), '0.20.0-rc.2')
            self.assertEqual((py / 'python3').read_text(), 'new python')
            self.assertEqual(wrapper.read_text(), 'new wrapper')
            self.assertEqual((root / 'app' / '0.19.0' / 'VERSION').exists(), False)

    def test_linux_runtime_id_tracks_python_version_and_pins(self):
        import sys as py_sys
        py_sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
        from linux_runtime import runtime_id
        with tempfile.TemporaryDirectory() as tmp:
            python = Path(tmp) / 'python3'
            python.write_text('#!/bin/sh\nprintf "3.14.9\\n"\n')
            python.chmod(0o755)
            first = runtime_id(python, ('numpy==2.0', 'lz4==4.0'))
            second = runtime_id(python, ('numpy==2.1', 'lz4==4.0'))
            self.assertIn('py3.14.9-', first)
            self.assertNotEqual(first, second)

    def test_linux_runtime_and_app_switch_together(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old = root / 'app' / '0.1.0'
            new = root / 'app' / '0.2.0'
            for app in (old, new):
                (app / 'launcher').mkdir(parents=True)
                (app / 'launcher' / 'start.pyw').write_text('')
                (app / 'VERSION').write_text(app.name + '\n')
            (root / 'app' / 'current.txt').write_text('0.1.0\n')
            python = root / 'python'
            (python / 'bin').mkdir(parents=True)
            (python / 'bin' / 'python3').write_bytes(b'old interpreter')
            (python / 'runtime-id.txt').write_text('runtime-old\n')
            (root / 'python.old').mkdir()
            (root / 'python.old' / 'stale.marker').write_text('stale')
            staged = new / updater.STAGED_PYTHON
            (staged / 'bin').mkdir(parents=True)
            (staged / 'bin' / 'python3').write_bytes(b'new interpreter')
            (staged / 'runtime-id.txt').write_text('runtime-new\n')
            (new / updater.STAGED).write_text(json.dumps({'version': '0.2.0', 'runtime': 'runtime-new'}))
            layout = Layout(root=root, app=old)
            updater.install_staged(layout, '0.2.0')
            self.assertEqual(updater.current(layout), '0.2.0')
            self.assertEqual(updater.runtime_id(layout), 'runtime-new')
            self.assertEqual((python / 'bin' / 'python3').read_bytes(), b'new interpreter')
            self.assertFalse((new / updater.STAGED).exists())
            self.assertTrue((root / 'python.old' / 'runtime-id.txt').is_file())
            self.assertFalse((root / 'python.old' / 'stale.marker').exists())

    def test_runtime_change_switches_on_updates_job_and_cli_update(self):
        from types import SimpleNamespace
        from launcher import cli

        for path in ('updates', 'cli'):
            with self.subTest(path=path), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                old = root / 'app' / '0.20.0'
                (old / 'launcher').mkdir(parents=True)
                (old / 'launcher' / 'start.pyw').write_text('')
                (old / 'VERSION').write_text('0.20.0\n')
                (root / 'app' / 'current.txt').write_text('0.20.0\n')
                python = root / 'python'
                (python / 'bin').mkdir(parents=True)
                (python / 'bin' / 'python3').write_text('old interpreter')
                (python / 'runtime-id.txt').write_text('linux-x86_64-py3.14.8-old\n')
                layout = Layout(root=root, app=old)
                ctx = Context(layout, echo=False)
                self.addCleanup(lambda context=ctx: context._file.close())
                release = SimpleNamespace(version='0.21.0-rc.1', manifest={'runtime': {
                    'id': 'linux-x86_64-py3.14.9-new'}})

                def stage(_layout, _release, **_kwargs):
                    folder = root / 'app' / '0.21.0-rc.1'
                    (folder / 'launcher').mkdir(parents=True)
                    (folder / 'launcher' / 'start.pyw').write_text('')
                    (folder / 'VERSION').write_text('0.21.0-rc.1\n')
                    staged_python = folder / updater.STAGED_PYTHON
                    (staged_python / 'bin').mkdir(parents=True)
                    (staged_python / 'bin' / 'python3').write_text('new interpreter')
                    (staged_python / 'runtime-id.txt').write_text('linux-x86_64-py3.14.9-new\n')
                    (folder / updater.STAGED).write_text(json.dumps({
                        'version': '0.21.0-rc.1', 'runtime': 'linux-x86_64-py3.14.9-new'}))
                    return folder

                with mock.patch.object(updater, 'stage', side_effect=stage):
                    if path == 'updates':
                        job = jobs.UpdateDownloadJob(ctx, release)
                        job.run_inline()
                        self.assertEqual(job.result, 'ok', job.error)
                    else:
                        check = SimpleNamespace(result='ok', release=release, error='')
                        class Held:
                            released = False
                            def release(self):
                                self.released = True
                        held = Held()
                        arg = SimpleNamespace(cmd='update')
                        with mock.patch.object(cli.jobs, 'CheckJob', return_value=SimpleNamespace(
                                run_inline=lambda: check)), \
                             mock.patch.object(cli, 'follow', side_effect=lambda job: (job.run_inline(), 0)[1]), \
                             mock.patch.object(cli.subprocess, 'call', return_value=0) as finish, \
                             mock.patch.object(build, 'game_running', return_value=False):
                            self.assertEqual(cli.run(arg, ctx, held), 0)
                        self.assertTrue(held.released)
                        self.assertTrue(finish.called)
                self.assertEqual(updater.current(layout), '0.21.0-rc.1')
                self.assertEqual(updater.runtime_id(layout), 'linux-x86_64-py3.14.9-new')
                self.assertEqual((python / 'bin' / 'python3').read_text(), 'new interpreter')

    def test_linux_runtime_asset_name_from_manifest_is_used_by_updater(self):
        runtime_id = 'linux-x86_64-py3.14.9-16aa56671d40'
        name = 'NX2Saria-runtime-linux-x86_64-py3.14.9-16aa56671d40.zip'
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            archive = root / name
            with zipfile.ZipFile(archive, 'w') as zf:
                zf.writestr('bin/python3', 'new python')
                zf.writestr('runtime-id.txt', runtime_id + '\n')
            data = archive.read_bytes()
            manifest = {'version': '0.21.0-rc.1', 'runtime': {
                'id': runtime_id, 'name': name, 'size': len(data),
                'sha256': hashlib.sha256(data).hexdigest()}}
            release = feed.Release(manifest, assets={name: 'https://example.invalid/' + name})
            current = root / 'app' / '0.20.0'
            (current / 'launcher').mkdir(parents=True)
            (current / 'launcher' / 'start.pyw').write_text('')
            (root / 'app' / 'current.txt').write_text('0.20.0\n')
            python = root / 'python' / 'bin'
            python.mkdir(parents=True)
            (python / 'python3').write_text('old python')
            (root / 'python' / 'runtime-id.txt').write_text('linux-x86_64-py3.14.8-old\n')
            layout = Layout(root=root, app=current).ensure()
            downloads = []

            def download(url, destination, **_kwargs):
                downloads.append((url, Path(destination).name))
                shutil.copyfile(archive, destination)

            with mock.patch.object(updater.net, 'download', side_effect=download):
                self.assertTrue(updater.stage_runtime(layout, release, version='0.20.0'))
            self.assertEqual(downloads, [('https://example.invalid/' + name, name)])
            self.assertEqual(updater.runtime_id(layout), 'linux-x86_64-py3.14.8-old')
            self.assertEqual(updater.read_text(root / 'python.next' / 'runtime-id.txt'), runtime_id)
    def test_missing_sdl_disables_gamepad(self):
        with mock.patch.object(linux, '_sdl_library', return_value=(None, 'none (not found)')):
            pad = linux.Gamepad()
        self.assertEqual(pad.poll(), set())
        self.assertEqual(pad.scroll, 0)

    def test_gamepad_reopens_a_reconnected_device(self):
        class Function:
            def __init__(self, fn):
                self.fn = fn

            def __call__(self, *args):
                return self.fn(*args)

        class FakeSDL:
            def __init__(self):
                self.calls = []
                self.buffers = []
                self.enumerations = 0
                self.SDL_InitSubSystem = Function(lambda flags: True)
                self.SDL_GetGamepads = Function(self.get_gamepads)
                self.SDL_OpenGamepad = Function(lambda device: 100 + device)
                self.SDL_GetGamepadButton = Function(lambda pad, button: False)
                self.SDL_GetGamepadAxis = Function(lambda pad, axis: 0)
                self.SDL_PumpEvents = Function(lambda: None)
                self.SDL_free = Function(lambda ids: None)
                self.SDL_CloseGamepad = Function(lambda pad: self.calls.append(('close', pad)))
                self.SDL_GamepadConnected = Function(lambda pad: pad != 101)

            def get_gamepads(self, count):
                self.enumerations += 1
                count._obj.value = 1
                ids = (ctypes.c_int32 * 1)(self.enumerations)
                self.buffers.append(ids)
                return ctypes.cast(ids, ctypes.POINTER(ctypes.c_int32))

        fake = FakeSDL()
        with mock.patch.object(linux, '_sdl_library', return_value=(fake, 'SDL3 from system')):
            pad = linux.Gamepad()
        self.assertEqual(pad.pad, 101)
        self.assertEqual(pad.poll(), set())
        self.assertEqual(pad.pad, 102)
        self.assertEqual(fake.calls, [('close', 101)])
        self.assertEqual(fake.enumerations, 2)

    def test_winsys_gamepad_uses_platform_backend(self):
        with mock.patch('launcher.platform.linux.Gamepad') as gamepad:
            with mock.patch.object(winsys.os, 'name', 'posix'):
                instance = winsys.Gamepad()
        gamepad.assert_called_once_with()
        self.assertIs(instance.backend, gamepad.return_value)

    def test_gamepad_facade_forwards_presses_and_scroll(self):
        backend = mock.Mock()
        backend.poll.return_value = {'a'}
        backend.scroll = -1
        with mock.patch('launcher.platform.linux.Gamepad', return_value=backend):
            with mock.patch.object(winsys.os, 'name', 'posix'):
                pad = winsys.Gamepad()
        self.assertEqual(pad.poll(False), {'a'})
        self.assertEqual(pad.scroll, -1)
        backend.poll.assert_called_once_with(False)

    def test_toolchain_diagnosis_names_missing_tools_and_distro_guidance(self):
        missing = linux.toolchain_missing(lambda name: None if name in ('clang', 'glslc') else f'/usr/bin/{name}')
        self.assertIn('Missing Linux build tools or headers: clang.', missing)
        self.assertIn('Debian, Ubuntu, Linux Mint:', missing)
        self.assertIn('Fedora:', missing)
        self.assertIn('will not install system packages', missing)

    def test_toolchain_diagnosis_is_empty_when_present(self):
        run = lambda *args, **kwargs: type('Result', (), {'returncode': 0})()
        self.assertEqual(linux.toolchain_missing(lambda name: f'/usr/bin/{name}', run), '')

    def test_toolchain_diagnosis_checks_required_headers(self):
        def run(cmd, **kwargs):
            return type('Result', (), {'returncode': int('vpx' in kwargs['input'])})()

        missing = linux.toolchain_missing(lambda name: f'/usr/bin/{name}', run)
        self.assertIn('vpx/vpx_decoder.h', missing)
        self.assertNotIn('opus/opus.h', missing)

    def test_awake_release_without_an_active_inhibitor_is_safe(self):
        with mock.patch.object(linux, '_inhibitor', None):
            log = []
            linux.keep_awake(False, log.append)
        self.assertEqual(log, [])

    def test_keep_awake_starts_and_releases_systemd_inhibitor(self):
        process = mock.Mock()
        process.poll.return_value = None
        process.stdin = mock.Mock()
        log = []
        with mock.patch.object(linux, '_inhibitor', None), \
                mock.patch.object(linux.shutil, 'which', return_value='/usr/bin/systemd-inhibit'), \
                mock.patch.object(linux.subprocess, 'Popen', return_value=process) as popen:
            linux.keep_awake(True, log.append)
            linux.keep_awake(False, log.append)
        self.assertIn('systemd-inhibit', popen.call_args.args[0][0])
        self.assertEqual(popen.call_args.kwargs['stdin'], subprocess.PIPE)
        self.assertNotIn('start_new_session', popen.call_args.kwargs)
        self.assertEqual(popen.call_args.args[0][-1], 'cat')
        process.stdin.close.assert_called_once_with()
        process.wait.assert_called_once_with(timeout=2)
        self.assertEqual(log, ['keep awake: systemd-inhibit held', 'keep awake: systemd-inhibit released'])

    def test_udmabuf_detection_is_injectable_and_offers_only_when_needed(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            device = base / 'udmabuf'
            parameter = base / 'size_limit_mb'
            os_release = base / 'os-release'
            device.touch()
            parameter.write_text('64\n')
            os_release.write_text('ID=ubuntu\n')
            status = linux.udmabuf_status(device, parameter, which=lambda name: '/usr/bin/pkexec',
                                          accessible=lambda path: False, os_release=os_release)
            self.assertEqual(status, {'accessible': False, 'limit_mb': 64, 'needs_fix': True,
                                      'offered': True, 'reason': '', 'steamos': False})
            parameter.write_text('1024\n')
            status = linux.udmabuf_status(device, parameter, which=lambda name: '/usr/bin/pkexec',
                                          accessible=lambda path: True, os_release=os_release)
            self.assertFalse(status['needs_fix'])
            self.assertFalse(status['offered'])

    def test_udmabuf_missing_module_and_restricted_platforms_are_not_offered(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            missing = linux.udmabuf_status(base / 'device', base / 'missing',
                                           which=lambda name: '/usr/bin/pkexec', os_release=base / 'os-release')
            self.assertIsNone(missing)
            parameter, release = base / 'size', base / 'os-release'
            parameter.write_text('64')
            release.write_text('ID=steamos\n')
            steam = linux.udmabuf_status(base / 'device', parameter, which=lambda name: '/usr/bin/pkexec',
                                         accessible=lambda path: False, os_release=release)
            self.assertFalse(steam['offered'])
            self.assertIn('SteamOS', steam['reason'])
            release.write_text('ID=ubuntu\n')
            no_pkexec = linux.udmabuf_status(base / 'device', parameter, which=lambda name: None,
                                             accessible=lambda path: False, os_release=release)
            self.assertFalse(no_pkexec['offered'])
            self.assertEqual(no_pkexec['reason'], 'pkexec not installed')

    def test_udmabuf_confirmation_runs_only_the_privileged_script(self):
        calls = []

        def run(command, **kwargs):
            calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 0, stdout='complete', stderr='')

        script = Path(linux.__file__).with_name('udmabuf_fix.sh')
        source = script.read_text(encoding='utf-8')
        self.assertIn('KERNEL=="udmabuf", SUBSYSTEM=="misc", TAG+="uaccess"', source)
        self.assertIn('size_limit_mb - - - - 1024', source)
        self.assertIn('apply)', source)
        self.assertIn('remove)', source)
        ok, detail = linux.run_udmabuf_fix(script, 'apply', run=run)
        self.assertTrue(ok)
        self.assertEqual(detail, 'complete')
        self.assertEqual(calls[0][0][:4], ['pkexec', '/bin/sh', '-c', source])
        self.assertEqual(calls[0][0][4:], ['udmabuf_fix', 'apply'])
        self.assertEqual(calls[0][1]['timeout'], 60)
        syntax = subprocess.run(['sh', '-n', str(script)], capture_output=True, text=True)
        self.assertEqual(syntax.returncode, 0, syntax.stderr)

    def test_udmabuf_policykit_cancel_and_script_errors_are_distinct(self):
        script = Path(linux.__file__).with_name('udmabuf_fix.sh')

        def exit_with(code, stderr=''):
            return lambda command, **kwargs: subprocess.CompletedProcess(command, code, stdout='', stderr=stderr)

        self.assertEqual(linux.run_udmabuf_fix(script, 'apply', run=exit_with(126)),
                         (False, 'authorization cancelled'))
        self.assertEqual(linux.run_udmabuf_fix(script, 'apply', run=exit_with(127, 'pkexec: error')),
                         (False, 'pkexec: error'))

    def test_udmabuf_after_action_log_records_measured_state(self):
        self.assertEqual(linux.udmabuf_after_log_line('apply',
                         {'accessible': True, 'limit_mb': 1024}),
                         'udmabuf: after apply: accessible=yes; size_limit_mb=1024')
        self.assertIn('accessible=no; size_limit_mb=unknown',
                      linux.udmabuf_after_log_line('remove', None))

    def test_udmabuf_screen_applies_fix_when_state_needs_it_even_if_files_exist(self):
        from types import SimpleNamespace
        from launcher.ui.app import App
        app = object.__new__(App)
        app.ctx = SimpleNamespace(udmabuf={'accessible': True, 'limit_mb': 64, 'needs_fix': True,
                                           'offered': True, 'reason': '', 'steamos': False})
        with mock.patch.object(linux, 'udmabuf_fix_installed', return_value=True), \
                mock.patch.object(app, 'apply_udmabuf') as apply:
            rows = app.rows_udmabuf()
            rows[2].actions[0][1]()
        apply.assert_called_once_with('apply')

    def test_udmabuf_success_refreshes_status_and_prompts_for_session_restart(self):
        from types import SimpleNamespace
        from launcher.ui.app import App
        app = object.__new__(App)
        lines = []
        app.ctx = SimpleNamespace(log=lines.append, udmabuf=None)
        app.show = lambda screen: None
        after = {'accessible': False, 'limit_mb': 1024, 'needs_fix': True, 'offered': True,
                 'reason': '', 'steamos': False}
        with mock.patch.object(linux, 'run_udmabuf_fix', return_value=(True, 'completed')) as run, \
                mock.patch.object(linux, 'udmabuf_status', return_value=after):
            app.apply_udmabuf('apply')
        run.assert_called_once()
        self.assertIs(app.ctx.udmabuf, after)
        self.assertIn('udmabuf: after apply: accessible=no; size_limit_mb=1024', lines)
        self.assertIn('Log out and back in', app.message)

    def test_udmabuf_startup_state_is_logged_once(self):
        from launcher.context import Context
        status = {'accessible': False, 'limit_mb': 64, 'needs_fix': True, 'offered': True,
                  'reason': '', 'steamos': False}
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(linux, 'udmabuf_status', return_value=status):
            ctx = Context(Layout(root=Path(tmp)))
            try:
                ctx._file.flush()
                log = (Path(tmp) / 'data' / 'logs' / 'launcher.log').read_text()
                self.assertIn('udmabuf: accessible=no; size_limit_mb=64; offer=offered', log)
            finally:
                ctx._file.close()

    @unittest.skipUnless(sys.platform.startswith('linux'), 'the parent-death pipe test needs Linux /proc')
    def test_parent_death_closes_inhibitor_pipe_and_child_exits(self):
        with tempfile.TemporaryDirectory(prefix='nx2saria-inhibitor-test-') as tmp:
            fake = Path(tmp) / 'systemd-inhibit'
            fake.write_text('#!/bin/sh\nexec /usr/bin/cat\n', encoding='utf-8')
            fake.chmod(0o755)
            code = ('import time; from launcher.platform import linux; '
                    'linux.keep_awake(True); print(linux._inhibitor.pid, flush=True); time.sleep(60)')
            env = dict(os.environ, PATH=f'{tmp}:/usr/bin:/bin', PYTHONPATH=str(Path(__file__).resolve().parents[1]))
            parent = subprocess.Popen([sys.executable, '-c', code], env=env, stdout=subprocess.PIPE, text=True)
            try:
                child_pid = int(parent.stdout.readline().strip())
                parent.send_signal(signal.SIGKILL)
                parent.wait(timeout=5)
                deadline = time.monotonic() + 5
                exited = False
                while time.monotonic() < deadline:
                    try:
                        stat = Path(f'/proc/{child_pid}/stat').read_text()
                        if stat[stat.rfind(')') + 2] == 'Z':
                            exited = True
                            break
                    except (FileNotFoundError, ProcessLookupError):
                        exited = True
                        break
                    time.sleep(0.05)
                self.assertTrue(exited, 'the systemd-inhibit child remained alive after its parent died')
            finally:
                if parent.poll() is None:
                    parent.kill()
                    parent.wait()
                if parent.stdout:
                    parent.stdout.close()



class UserDirsTest(unittest.TestCase):
    """The desktop's own Downloads, Desktop and Documents folders (user-dirs.dirs), which a desktop set up in another
    language translates: the dump search and Export Save look there too (launcher/winsys.py known_folder)."""

    def setUp(self):
        self.home = Path(tempfile.mkdtemp(prefix='nx2saria-userdirs-'))
        self.addCleanup(__import__('shutil').rmtree, self.home, True)
        (self.home / '.config').mkdir()
        (self.home / '.config' / 'user-dirs.dirs').write_text(
            '# written by xdg-user-dirs-update\n'
            'XDG_DESKTOP_DIR="$HOME/Área de Trabalho"\n'
            'XDG_DOWNLOAD_DIR="$HOME/Transferências"\n'
            'XDG_DOCUMENTS_DIR="/mnt/dados/Documentos"\n'
            'XDG_PICTURES_DIR="$HOME/"\n', encoding='utf-8')

    def test_translated_folders_are_read(self):
        env = {}
        self.assertEqual(linux.user_dir('desktop', env, self.home), self.home / 'Área de Trabalho')
        self.assertEqual(linux.user_dir('downloads', env, self.home), self.home / 'Transferências')
        self.assertEqual(linux.user_dir('documents', env, self.home), Path('/mnt/dados/Documentos'))
        self.assertIsNone(linux.user_dir('pictures', env, self.home))   # set to the home folder: turned off
        self.assertIsNone(linux.user_dir('music', env, self.home))
        other = self.home / 'cfg'
        other.mkdir()
        self.assertIsNone(linux.user_dir('desktop', {'XDG_CONFIG_HOME': str(other)}, self.home))   # no file there

    def test_the_dump_search_looks_in_them(self):
        from launcher import gamefiles
        for d in ('Área de Trabalho', 'Transferências', 'Downloads'):
            (self.home / d).mkdir()
        with mock.patch.dict(os.environ, {'XDG_CONFIG_HOME': ''}), \
                mock.patch.object(Path, 'home', lambda: self.home), mock.patch.object(winsys.sys, 'platform', 'linux'):
            self.assertEqual(winsys.known_folder('desktop'), self.home / 'Área de Trabalho')
            dirs = gamefiles.candidate_dirs()
        self.assertEqual(dirs, [self.home / 'Transferências', self.home / 'Downloads', self.home / 'Área de Trabalho',
                                self.home])

    def test_os_name_and_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            release = Path(tmp) / 'os-release'
            release.write_text('NAME="Fedora Linux"\nVERSION_ID=41\n')
            self.assertEqual(linux.os_name(release), 'Fedora Linux 41')
            self.assertEqual(linux.os_name(Path(tmp) / 'none'), 'Linux')
        self.assertEqual(linux.session_text({'WAYLAND_DISPLAY': 'wayland-0', 'XDG_CURRENT_DESKTOP': 'KDE'}),
                         'wayland session, desktop KDE')
        self.assertEqual(linux.session_text({}), 'unknown session, desktop unknown')


class OtherDrivesTest(unittest.TestCase):
    """The dump search also looks on the disks mounted under /mnt and /media (a partition shared with Windows), and
    the dump and folder rows open the desktop's chooser, which reaches them."""

    def test_only_local_disks_mounted_now(self):
        with tempfile.TemporaryDirectory() as tmp:
            mounts = Path(tmp) / 'mounts'
            mounts.write_text(
                '/dev/nvme0n1p2 / ext4 rw 0 0\n'
                'systemd-1 /mnt/dados autofs rw 0 0\n'            # the placeholder: listing it would mount the disk
                '/dev/sda1 /mnt/dados fuseblk rw 0 0\n'           # the disk itself, mounted
                'systemd-1 /mnt/backup autofs rw 0 0\n'           # not mounted now: left alone
                'server:/share /mnt/rede nfs4 rw 0 0\n'           # a network share: left alone
                '/dev/sdb1 /media/ana/Disco\\040Externo exfat rw 0 0\n'
                '/dev/sdc1 /media/bia/Pendrive vfat rw 0 0\n'     # another account's
                'tmpfs /mnt/tmp tmpfs rw 0 0\n'
                '/dev/mmcblk0p1 /run/media/mmcblk0p1 ext4 rw 0 0\n'   # SteamOS's SD card
                '/dev/sdd1 /mnt ext4 rw 0 0\n')                   # /mnt itself is no drive below it
            self.assertEqual(linux.mounted_drives(mounts, 'ana'),
                             [Path('/mnt/dados'), Path('/media/ana/Disco Externo'), Path('/run/media/mmcblk0p1')])
            self.assertEqual(linux.mounted_drives(Path(tmp) / 'none', 'ana'), [])

    def test_the_search_goes_two_levels_into_a_drive_and_stops_in_time(self):
        from launcher import gamefiles
        with tempfile.TemporaryDirectory() as tmp:
            drive = Path(tmp) / 'dados'
            (drive / 'Jogos' / 'Switch').mkdir(parents=True)
            (drive / 'Jogos' / 'Switch' / 'game.nup').write_bytes(b'x')
            (drive / 'Jogos' / 'a' / 'b').mkdir(parents=True)
            (drive / 'Jogos' / 'a' / 'b' / 'deep.nup').write_bytes(b'x')   # three levels: not looked at
            home = Path(tmp) / 'home'
            home.mkdir()
            with mock.patch.object(gamefiles, 'is_game_nup', lambda layout, f: True), \
                    mock.patch.object(Path, 'home', lambda: home), \
                    mock.patch.object(winsys, 'known_folder', lambda name: None):
                report = {}
                found = gamefiles.find_dumps(None, drives=[drive], report=report)
                self.assertEqual(found, [drive / 'Jogos' / 'Switch' / 'game.nup'])
                self.assertEqual(report, {'folders': 1 + 1 + 1 + 2, 'stopped': False})   # home; drive, Jogos, 2 below
                with mock.patch.object(gamefiles, 'DRIVES_SECONDS', -1):
                    report = {}
                    self.assertEqual(gamefiles.find_dumps(None, drives=[drive], report=report), [])
                    self.assertTrue(report['stopped'])

    def run_as(self, results):
        calls = []

        def run(cmd, **kw):
            calls.append(cmd)
            code, out = results[cmd[0]]
            return subprocess.CompletedProcess(cmd, code, out, '')
        return calls, run

    def test_the_desktops_chooser(self):
        which = lambda tool: f'/usr/bin/{tool}'
        calls, run = self.run_as({'zenity': (0, b'/mnt/dados/Jogos/game\xff.nup\n'), 'kdialog': (0, '')})
        got = linux.pick_path('file', 'Choose', '/home/ana', [('Game dump', '*.nup')], which, run, env={})
        self.assertEqual(got, ('zenity', os.fsdecode(b'/mnt/dados/Jogos/game\xff.nup')))   # a name that is no UTF-8
        self.assertEqual(calls[0], ['zenity', '--file-selection', '--title', 'Choose', '--filename', '/home/ana/',
                                    '--file-filter=Game dump | *.nup'])
        calls, run = self.run_as({'zenity': (1, ''), 'kdialog': (0, '/x')})   # cancelled: kdialog is not asked too
        self.assertEqual(linux.pick_path('folder', 'Choose', '/home/ana', (), which, run, env={}), ('zenity', ''))
        self.assertIn('--directory', calls[0])
        self.assertEqual(len(calls), 1)
        calls, run = self.run_as({'kdialog': (0, '/mnt/dados/NX2Saria\n')})   # KDE: its own first
        self.assertEqual(linux.pick_path('folder', 'Choose', '/home/ana', (), which, run,
                                         env={'XDG_CURRENT_DESKTOP': 'KDE'}), ('kdialog', '/mnt/dados/NX2Saria'))
        self.assertEqual(calls[0], ['kdialog', '--title', 'Choose', '--getexistingdirectory', '/home/ana'])
        calls, run = self.run_as({'zenity': (255, ''), 'kdialog': (0, '/a.nup')})   # zenity broken: kdialog
        self.assertEqual(linux.pick_path('file', 'C', '/', [('D', '*.nup')], which, run, env={}), ('kdialog', '/a.nup'))
        self.assertEqual(calls[1][-1], '*.nup|D')
        self.assertEqual(linux.pick_path('file', 'C', '/', (), lambda t: None, run, env={}), (None, ''))

    def test_the_chooser_says_in_the_log_which_it_was_and_what_it_gave(self):
        lines, asked = [], []
        with mock.patch.object(winsys.sys, 'platform', 'linux'), \
                mock.patch.object(linux, 'pick_path', lambda *a: asked.append(a) or ('zenity', '/mnt/dados/Jogos/game.nup')):
            self.assertEqual(winsys.pick(None, 'file', 'Choose your dump', '/home/ana',
                                         [('Game dump', '*.nup'), ('All files', '*.*')], lines.append),
                             '/mnt/dados/Jogos/game.nup')
        with mock.patch.object(winsys.sys, 'platform', 'linux'), \
                mock.patch.object(linux, 'pick_path', lambda *a: ('kdialog', '')):
            self.assertEqual(winsys.pick(None, 'folder', 'Choose the folder', '/home/ana', log=lines.append), '')
        # "All files" stays: the desktop's patterns are case-sensitive, and GAME.NUP would not match *.nup
        self.assertEqual(asked[0][3], [('Game dump', '*.nup'), ('All files', '*')])
        self.assertEqual(lines, ['file chooser (zenity): Choose your dump: /mnt/dados/Jogos/game.nup',
                                 'file chooser (kdialog): Choose the folder: cancelled'])

if __name__ == '__main__':
    unittest.main()
