import shutil
import sys
from pathlib import Path

ART_SETS = ('XGamepad', 'P5Gamepad', 'Keyboard_Mouse')
FOLDER = 'prompt-art'
REPO_ART = Path(__file__).resolve().parents[1] / FOLDER
SOURCE = 'SOURCE.md'
SOURCE_LINE = 'Source: '


def _sticks(folder, stick, press, rotate):
    out = {}
    for side, name in (('L', stick[0]), ('R', stick[1])):
        base = f'{folder}{name}'
        out.update({f'{side}STICK': f'{base}.png', f'{side}STICK_UP': f'{base}_UP.png',
                    f'{side}STICK_DOWN': f'{base}_Down.png', f'{side}STICK_LEFT': f'{base}_Left.png',
                    f'{side}STICK_RIGHT': f'{base}_Right.png', f'{side}STICK_UPDOWN': f'{base}_Y.png',
                    f'{side}STICK_LEFTRIGHT': f'{base}_X.png', f'{side}STICK_PRESS': f'{folder}{press[side == "R"]}'})
    out['RSTICK_ROTATE'] = f'{folder}{rotate}'
    return out


def _dpad(folder, name, up):
    return {'DPAD': f'{folder}{name}.png', 'DPAD_UP': f'{folder}{name}_{up}.png', 'DPAD_DOWN': f'{folder}{name}_Down.png',
            'DPAD_LEFT': f'{folder}{name}_Left.png', 'DPAD_RIGHT': f'{folder}{name}_Right.png',
            'DPAD_UPDOWN': f'{folder}{name}_Y.png', 'DPAD_LEFTRIGHT': f'{folder}{name}_X.png'}


X, XL, P, PL = 'XGamepad/Default/T_X_', 'XGamepad/Light/T_X_', 'P5Gamepad/Default/T_P5_', 'P5Gamepad/Light/T_P5_'
OPTIONS = 'options'
SHARE = 'XGamepad/Default/T_X_Share-1.png'
STYLES = {
    'xbox': {
        'glyph': {'south': f'{X}A_White.png', 'east': f'{X}B_White.png', 'west': f'{X}X_White.png',
                  'north': f'{X}Y_White.png', 'L': f'{X}LB.png', 'R': f'{X}RB.png', 'ZL': f'{X}LT.png',
                  'ZR': f'{X}RT.png', 'PLUS': f'{X}X.png', 'MINUS': f'{X}Share.png',
                  **_dpad(X, 'Dpad', 'Up'), **_sticks(X, ('L', 'R'), ('Left_Stick_Click.png', 'Right_Stick_Click.png'),
                                                      'R_2D.png')},
        'note': {'south': f'{XL}A_Color_Light.png', 'east': f'{XL}B_Color_Light.png', 'west': f'{XL}X_Color_Light.png',
                 'north': f'{XL}Y_Color_Light.png', 'L': f'{XL}LB_Light.png', 'R': f'{XL}RB_Light.png'},
    },
    'playstation': {
        'glyph': {'south': f'{P}Cross.png', 'east': f'{P}Circle.png', 'west': f'{P}Square.png',
                  'north': f'{P}Triangle.png', 'L': f'{P}L1.png', 'R': f'{P}R1.png', 'ZL': f'{P}L2.png',
                  'ZR': f'{P}R2.png', 'PLUS': OPTIONS, 'MINUS': SHARE,
                  **_dpad(P, 'Dpad', 'UP'), **_sticks(P, ('L', 'R'), ('L3.png', 'R3.png'), 'R_2D.png')},
        'note': {'south': f'{PL}Cross_Color_Light.png', 'east': f'{PL}Circle_Color_Light.png',
                 'west': f'{PL}Square_Color_Light.png', 'north': f'{PL}Triangle_Color_Light.png',
                 'L': f'{PL}L1_Light.png', 'R': f'{PL}R1_Light.png'},
    },
    'steamdeck': {
        'glyph': {'south': f'{X}A_White.png', 'east': f'{X}B_White.png', 'west': f'{X}X_White.png',
                  'north': f'{X}Y_White.png', 'L': f'{P}L1.png', 'R': f'{P}R1.png', 'ZL': f'{P}L2.png',
                  'ZR': f'{P}R2.png', 'PLUS': f'{X}X.png', 'MINUS': f'{X}Share.png',
                  **_dpad(X, 'Dpad', 'Up'), **_sticks(X, ('L', 'R'), ('Left_Stick_Click.png', 'Right_Stick_Click.png'),
                                                      'R_2D.png'),
                  'LSTICK_PRESS': f'{P}L3.png', 'RSTICK_PRESS': f'{P}R3.png'},
        'note': {'south': f'{XL}A_White_Light.png', 'east': f'{XL}B_White_Light.png', 'west': f'{XL}X_White_Light.png',
                 'north': f'{XL}Y_White_Light.png', 'L': f'{PL}L1_Light.png', 'R': f'{PL}R1_Light.png'},
    },
}

KM = 'Keyboard_Mouse/'
SHADES = ('Dark', 'White')
BLANKS = {'Dark': f'{KM}Blanks/T_Blank_Black_Normal.png', 'White': f'{KM}Blanks/T_Blank_White_Normal.png'}


def overlay_folder(style, layout=None):
    return f'romfs-patches-{style}' + ('-label' if layout == 'label' else '')


OVERLAYS = tuple(overlay_folder(s, layout) for s in sorted(STYLES) for layout in ('label', 'position')) + \
    (overlay_folder('pc'),)


def art_roots(folders):
    ranked = ({}, {})
    for f in map(Path, folders):
        if not f.is_dir():
            sys.exit(f'{f}: no such folder')
        rank, found = (0, [(f.name, f.parent)]) if f.name in ART_SETS else \
            (1, [(n, f) for n in ART_SETS if (f / n).is_dir()])
        if not found:
            sys.exit(f'{f} holds none of {", ".join(ART_SETS)}')
        for name, root in found:
            seen = ranked[rank].get(name)
            if seen and seen.resolve() != root.resolve():
                sys.exit(f'{name} is in both {seen} and {root}: name the one to use')
            ranked[rank][name] = root
    roots = {**ranked[1], **ranked[0]}
    missing = [n for n in ART_SETS if n not in roots]
    if missing:
        sys.exit(f'no {", ".join(missing)} folder in {", ".join(map(str, folders))}')
    return roots


def art_files(folders):
    roots = art_roots(folders)
    wanted = {rel for style in STYLES.values() for part in style.values() for rel in part.values() if rel != OPTIONS}
    wanted |= {SHARE, *BLANKS.values()}
    files, missing = {}, []
    for rel in sorted(wanted):
        src = roots[rel.split('/')[0]] / rel
        if src.is_file():
            files[rel] = src
        else:
            missing.append(str(src))
    for shade in SHADES:
        keys = sorted((roots['Keyboard_Mouse'] / KM / shade).glob('*.png'))
        if not keys:
            missing.append(str(roots['Keyboard_Mouse'] / KM / shade / '*.png'))
        files.update({f'{KM}{shade}/{p.name}': p for p in keys})
    if missing:
        sys.exit('button art missing:\n  ' + '\n  '.join(missing[:20]))
    return files


def release_art(folders, none):
    if none:
        if folders:
            sys.exit('--prompt-art and --no-prompt-art name opposite things')
        return None
    return folders or [REPO_ART]


def source_of(art):
    try:
        text = (Path(art) / SOURCE).read_text(encoding='utf-8')
    except OSError:
        return f'{art} (it has no {SOURCE} saying where the pictures came from)'
    for line in text.splitlines():
        if line.startswith(SOURCE_LINE):
            return line[len(SOURCE_LINE):].strip()
    return f'{art} (its {SOURCE} has no "{SOURCE_LINE.strip()}" line)'


def copy_art(folders, dest):
    files = art_files(folders)
    shutil.rmtree(dest, ignore_errors=True)
    for rel, src in files.items():
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest / rel)
    source = next((Path(f) / SOURCE for f in folders if (Path(f) / SOURCE).is_file()), None)
    if source:
        shutil.copy2(source, dest / SOURCE)
    return len(files), sum(p.stat().st_size for p in files.values())
