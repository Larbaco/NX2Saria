"""Fast checks for Linux platform services; no display, game data or build tools required."""
import ctypes
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
import unittest
from unittest import mock

from launcher.platform import linux
from launcher import winsys
from launcher import build
from launcher import updater
from launcher.paths import Layout


class LinuxPlatformTest(unittest.TestCase):
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
            self.assertNotIn(str(bundle), desktop)

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
                    except FileNotFoundError:
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


if __name__ == '__main__':
    unittest.main()
