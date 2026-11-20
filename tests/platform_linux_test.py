"""Fast checks for Linux platform services; no display, game data or build tools required."""
import ctypes
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


class LinuxPlatformTest(unittest.TestCase):
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
