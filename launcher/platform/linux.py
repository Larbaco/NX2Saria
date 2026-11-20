"""Linux services used by the launcher's compatibility facade (launcher/winsys.py)."""
import ctypes
import ctypes.util
import os
import shutil
import subprocess
from pathlib import Path


def _sdl_library():
    """Load the kit's SDL3 first, then the system copy; a missing library disables gamepad support only."""
    root = Path(__file__).resolve().parents[2]
    bundled = root / 'toolchain' / 'sdl3' / 'lib' / 'libSDL3.so.0'
    candidates = [str(bundled), ctypes.util.find_library('SDL3'), 'libSDL3.so.0']
    for candidate in candidates:
        if not candidate:
            continue
        try:
            return ctypes.CDLL(candidate)
        except OSError:
            pass
    return None


class Gamepad:
    """Read SDL3 GameController input, including Steam Input's virtual controller on Steam Deck."""
    BUTTONS = {11: 'up', 12: 'down', 13: 'left', 14: 'right', 0: 'a', 1: 'b', 3: 'y'}
    AXIS_LEFT_X, AXIS_LEFT_Y, AXIS_RIGHT_Y = 0, 1, 3
    INIT_GAMEPAD = 0x00002000

    def __init__(self):
        self.dll = _sdl_library()
        self.pad = None
        self.held = set()
        self.scroll = 0
        if not self.dll:
            return
        try:
            self.dll.SDL_InitSubSystem.argtypes = [ctypes.c_uint32]
            self.dll.SDL_InitSubSystem.restype = ctypes.c_bool
            self.dll.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
            self.dll.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_int32)
            self.dll.SDL_OpenGamepad.argtypes = [ctypes.c_int32]
            self.dll.SDL_OpenGamepad.restype = ctypes.c_void_p
            self.dll.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
            self.dll.SDL_GetGamepadButton.restype = ctypes.c_bool
            self.dll.SDL_GetGamepadAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
            self.dll.SDL_GetGamepadAxis.restype = ctypes.c_int16
            self.dll.SDL_PumpEvents.argtypes = []
            self.dll.SDL_PumpEvents.restype = None
            self.dll.SDL_free.argtypes = [ctypes.c_void_p]
            self.dll.SDL_free.restype = None
            self.dll.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
            self.dll.SDL_CloseGamepad.restype = None
            self.dll.SDL_InitSubSystem(self.INIT_GAMEPAD)
            self._open_first()
        except (AttributeError, OSError):
            self.dll = None
            self.pad = None

    def _open_first(self):
        if not self.dll or self.pad:
            return
        count = ctypes.c_int()
        ids = self.dll.SDL_GetGamepads(ctypes.byref(count))
        if ids and count.value:
            self.pad = self.dll.SDL_OpenGamepad(ids[0])
            self.dll.SDL_free(ids)

    def poll(self, active=True):
        self.scroll = 0
        if not self.dll:
            return set()
        try:
            self.dll.SDL_PumpEvents()
            self._open_first()
            down = set()
            if self.pad:
                down = {name for button, name in self.BUTTONS.items()
                        if self.dll.SDL_GetGamepadButton(self.pad, button)}
                lx = self.dll.SDL_GetGamepadAxis(self.pad, self.AXIS_LEFT_X)
                ly = self.dll.SDL_GetGamepadAxis(self.pad, self.AXIS_LEFT_Y)
                ry = self.dll.SDL_GetGamepadAxis(self.pad, self.AXIS_RIGHT_Y)
                if ly < -16000:
                    down.add('up')
                elif ly > 16000:
                    down.add('down')
                if lx < -16000:
                    down.add('left')
                elif lx > 16000:
                    down.add('right')
                if active and ry < -16000:
                    self.scroll = -1
                elif active and ry > 16000:
                    self.scroll = 1
            pressed = down - self.held if active else set()
            self.held = down
            return pressed
        except (AttributeError, OSError):
            self.held.clear()
            return set()

    def close(self):
        if self.dll and self.pad:
            self.dll.SDL_CloseGamepad(self.pad)
            self.pad = None


_inhibitor = None


def keep_awake(on):
    """Hold a systemd sleep inhibitor for the duration of a foreground build job."""
    global _inhibitor
    if not on:
        if _inhibitor and _inhibitor.poll() is None:
            _inhibitor.terminate()
            try:
                _inhibitor.wait(timeout=2)
            except subprocess.TimeoutExpired:
                _inhibitor.kill()
                _inhibitor.wait()
        _inhibitor = None
        return
    if _inhibitor and _inhibitor.poll() is None:
        return
    inhibitor = shutil.which('systemd-inhibit')
    sleeper = shutil.which('sleep')
    if not inhibitor or not sleeper:
        return
    try:
        _inhibitor = subprocess.Popen(
            [inhibitor, '--what=idle:sleep', '--who=NX2Saria', '--why=Building the game',
             '--mode=block', sleeper, 'infinity'],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True, close_fds=True)
    except OSError:
        _inhibitor = None


def toolchain_missing(which=shutil.which, run=subprocess.run):
    """Check the compiler and headers the Linux build requires, with distro guidance and no package changes."""
    required = ('clang', 'clang++', 'ld.lld')
    missing = [name for name in required if not which(name)]
    clang = which('clang')
    if clang:
        for header in ('opus/opus.h', 'vpx/vpx_decoder.h', 'dav1d/dav1d.h', 'zstd.h'):
            probe = run([clang, '-E', '-x', 'c', '-', '-o', os.devnull], input=f'#include <{header}>\n',
                        capture_output=True, text=True)
            if probe.returncode:
                missing.append(header)
    if not missing:
        return ''
    return (f'Missing Linux build tools or headers: {", ".join(missing)}. Install the compiler and libraries, then '
            'run this again:\n'
            'Arch, CachyOS, Manjaro, SteamOS (developer mode): sudo pacman -S --needed clang lld sdl3 opus libvpx '
            'dav1d zstd vulkan-headers vulkan-icd-loader\n'
            'Fedora: sudo dnf install clang lld SDL3-devel opus-devel libvpx-devel libdav1d-devel libzstd-devel '
            'vulkan-headers vulkan-loader-devel libstdc++-static libatomic\n'
            'Debian, Ubuntu, Linux Mint: sudo apt install clang lld libopus-dev libvpx-dev libdav1d-dev libzstd-dev '
            'libvulkan-dev. SDL3 is carried by the build kit when the distribution has no SDL3 package.\n'
            'NX2Saria will not install system packages for you.')
