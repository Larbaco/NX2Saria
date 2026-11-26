from pathlib import Path

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
    return {'theme': theme, 'colors': colors, 'animations': settings['animations'],
            'sounds': settings['sounds'] if settings['sounds'] in sound.LEVELS else sound.DEFAULT_LEVEL,
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
