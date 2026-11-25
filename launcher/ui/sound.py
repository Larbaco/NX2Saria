import ctypes
import os
import shutil
import struct
import subprocess

import numpy as np

LEVELS = {'off': 0.0, 'low': 0.18, 'medium': 0.4, 'high': 0.75}
DEFAULT_LEVEL = 'low'
EFFECTS = ('select', 'back')


def wav_bytes(samples, level, rate, channels):
    pcm = (np.clip(samples * level, -1, 1) * 32767).astype('<i2').tobytes()
    return (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt '
            + struct.pack('<IHHIIHH', 16, 1, channels, rate, rate * 2 * channels, 2 * channels, 16)
            + b'data' + struct.pack('<I', len(pcm)) + pcm)


def read_wav(data):
    if len(data) < 44 or data[:4] != b'RIFF' or data[8:16] != b'WAVEfmt ' or data[36:40] != b'data':
        return None
    kind, channels, rate, _bytes, _align, bits = struct.unpack_from('<HHIIHH', data, 20)
    size = min(struct.unpack_from('<I', data, 40)[0], len(data) - 44)
    if kind != 1 or bits != 16 or channels not in (1, 2) or size < 2 * channels:
        return None
    a = np.frombuffer(data, dtype='<i2', count=size // 2, offset=44).astype(np.float32)
    peak = float(np.abs(a).max())
    return rate, channels, a / peak if peak > 0 else a


class Sounds:
    def __init__(self, cache_dir, level=DEFAULT_LEVEL, log=None):
        self.cache = cache_dir
        self.log = log or (lambda line: None)
        self.level = None
        self.game = {}
        self.wavs = {}
        self.files = {}
        self.player = None
        self.winmm = None
        self.failed = False
        if os.name == 'nt':
            try:
                self.winmm = ctypes.WinDLL('winmm')
                self.winmm.PlaySoundW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32]
                self.winmm.PlaySoundW.restype = ctypes.c_int
            except (OSError, AttributeError) as e:
                self.log(f'sounds: {e}')
        else:
            self.player = next((p for p in ('pw-play', 'paplay', 'aplay') if shutil.which(p)), None)
        self.set_level(level)
        self.load_game()

    def load_game(self):
        game = {}
        for name in EFFECTS:
            try:
                got = read_wav((self.cache / 'sounds' / f'{name}.wav').read_bytes())
            except OSError:
                got = None
            if got:
                game[name] = got
        if set(game) != set(self.game):
            self.stop()
            self.game, self.wavs, self.files = game, {}, {}
        return bool(game)

    def set_level(self, level):
        level = level if level in LEVELS else DEFAULT_LEVEL
        if level == self.level:
            return
        self.stop()
        self.level = level
        self.wavs, self.files = {}, {}

    def wav(self, name):
        if name not in self.wavs:
            rate, channels, samples = self.game[name]
            self.wavs[name] = wav_bytes(samples, LEVELS[self.level], rate, channels)
        return self.wavs[name]

    def stop(self):
        if self.winmm:
            try:
                self.winmm.PlaySoundW(None, None, 0)
            except OSError:
                pass

    def play(self, name):
        if LEVELS.get(self.level, 0) <= 0 or self.failed or name not in self.game:
            return
        try:
            if self.winmm:
                self.winmm.PlaySoundW(self.wav(name), None, 0x0001 | 0x0002 | 0x0004)
            elif self.player:
                if name not in self.files:
                    path = self.cache / f'sound-{name}-{self.level}.wav'
                    path.write_bytes(self.wav(name))
                    self.files[name] = path
                subprocess.Popen([self.player, str(self.files[name])], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        except (OSError, ValueError) as e:
            self.failed = True
            self.log(f'sounds: {e}')
