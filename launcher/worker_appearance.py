import os
import shutil
import tempfile
from pathlib import Path

import numpy as np

from . import gamefiles
from .ui import art, picture, sound

COLORS = tuple(f'custom_{slot}' for slot in art.CUSTOM_SLOTS)


def view(settings):
    name = settings['theme']
    theme = name if name in art.THEMES else art.DEFAULT_THEME
    colors = {slot: settings[f'custom_{slot}'].lower() for slot in art.CUSTOM_SLOTS}
    palette = art.custom_palette(colors) if theme == 'custom' else art.theme_palette(theme)
    colors = {slot: color if art.HEX.fullmatch(color) else art.CUSTOM_DEFAULT[slot]
              for slot, color in colors.items()}
    button = palette['orange']
    dim = min(picture.DIMS, key=lambda candidate: abs(candidate - settings['background_dim']))
    level = settings['sounds'] if settings['sounds'] in sound.LEVELS else sound.DEFAULT_LEVEL
    return {'theme': theme, 'colors': colors, 'animations': settings['animations'],
            'sounds': level, 'sound_gain': sound.LEVELS[level],
            'background_dim': dim, 'background': Path(settings['background']).name if settings['background'] else '',
            'palette': {'background': palette['window'], 'panel': palette['desc'], 'text': palette['text_bright'],
                        'muted': palette['muted'], 'button': button, 'border': palette['frame'],
                        'button_text': '#101810' if art.luminance(button) > 0.48 else '#fff8e7'}}


def preference(settings, key, value):
    if key == 'theme':
        valid = isinstance(value, str) and value in art.THEMES
    elif key in COLORS:
        valid = isinstance(value, str) and (value == '' or art.HEX.fullmatch(value.lower()))
        if valid:
            value = value.lower()
    elif key == 'background_dim':
        valid = type(value) is int and value in picture.DIMS
    elif key == 'animations':
        valid = type(value) is bool
    elif key == 'sounds':
        valid = isinstance(value, str) and value in sound.LEVELS
    else:
        valid = False
    if not valid:
        raise ValueError('invalid appearance preference')
    settings.save_preference(key, value)
    return view(settings)


def saved_background(layout, settings):
    if not settings['background']:
        return None
    try:
        image = np.load(layout.data / 'background.npy', mmap_mode='r', allow_pickle=False)
    except (OSError, ValueError, EOFError):
        return None
    if not isinstance(image, np.ndarray) or image.dtype != np.uint8 or image.ndim != 3 or image.shape[2] != 3:
        return None
    height, width = image.shape[:2]
    if min(height, width) < 8 or image.nbytes > picture.KEEP_W * picture.KEEP_H * 3:
        return None
    rgba = np.empty((696, 1200, 4), dtype=np.uint8)
    rgba[:, :, :3] = picture.cover(image, 1200, 696)
    rgba[:, :, 3] = 255
    return art.png_bytes(rgba)


def change_background(layout, settings, source):
    store = layout.data / 'background.npy'
    if source is None:
        settings.save_preference('background', '')
        try:
            store.unlink(missing_ok=True)
        except OSError:
            layout_log = settings.log
            if layout_log:
                layout_log('background: cached image could not be removed')
        return {'status': 'ok', 'appearance': view(settings)}
    if not isinstance(source, str) or '\0' in source or not Path(source).is_absolute():
        return {'status': 'invalid_picture'}
    with tempfile.TemporaryDirectory(prefix='background-', dir=layout.data) as scratch:
        proposed, previous = Path(scratch) / 'next.npy', Path(scratch) / 'previous.npy'
        try:
            picture.keep(source, proposed)
        except (picture.PictureError, ValueError, MemoryError):
            return {'status': 'invalid_picture'}
        if store.exists():
            shutil.copyfile(store, previous)
        os.replace(proposed, store)
        try:
            settings.save_preference('background', source)
        except (OSError, ValueError):
            if previous.exists():
                os.replace(previous, store)
            else:
                store.unlink(missing_ok=True)
            raise
    return {'status': 'ok', 'appearance': view(settings)}


def cached_assets(layout):
    try:
        fonts = gamefiles.cached_fonts(layout)
    except (OSError, ValueError):
        fonts = {}
    try:
        sounds = gamefiles.cached_sounds(layout)
    except OSError:
        sounds = {}
    return fonts, sounds


def asset_manifest(layout):
    fonts, sounds = cached_assets(layout)
    return {'status': 'ready', 'title': 'title' in fonts, 'effects': [name for name in sound.EFFECTS if name in sounds]}


def extract_assets(ctx, game, dump, complete):
    fonts, sounds = cached_assets(ctx.layout)
    if not fonts:
        try:
            gamefiles.extract_fonts(ctx.layout, game, dump)
        except Exception as e:
            ctx.log(f'fonts: {e}')
    if not sounds:
        try:
            gamefiles.extract_sounds(ctx.layout, game, dump)
        except Exception as e:
            ctx.log(f'sounds: {e}')
    result = asset_manifest(ctx.layout)
    ctx.log(f'worker: appearance assets title={result["title"]} effects={len(result["effects"])}')
    complete.set_result(result)


def asset_bytes(layout, asset):
    try:
        if asset == 'title':
            file = gamefiles.cached_fonts(layout).get('title', (None, ''))[0]
            maximum = 16 * 1024 * 1024
        else:
            file = gamefiles.cached_sounds(layout).get(asset)
            maximum = 4 * 1024 * 1024
        if not file or not file.resolve().is_relative_to(layout.cache.resolve()) or file.stat().st_size > maximum:
            return None
        with file.open('rb') as source:
            data = source.read(maximum + 1)
        if len(data) > maximum:
            return None
        if asset == 'title':
            return data if data.startswith((b'OTTO', b'\x00\x01\x00\x00')) else None
        pcm = sound.read_wav(data)
        if pcm:
            rate, channels, samples = pcm
            return sound.wav_bytes(samples, 1.0, rate, channels)
    except (OSError, ValueError, RuntimeError):
        pass
    return None
