import hashlib
from pathlib import Path

TITLE = '0400486024DEC000'
PLACES = ('{}', f'atmosphere/contents/{TITLE}/{{}}', f'contents/{TITLE}/{{}}', f'{TITLE}/{{}}')
PATCH_SUFFIXES = ('.ips', '.pchtxt')


def mod_folders(mods):
    mods = Path(mods)
    if not mods.is_dir():
        return []
    return sorted((d for d in mods.iterdir() if d.is_dir()), key=lambda d: d.name)


def part(mod, name):
    return next((Path(mod) / w.format(name) for w in PLACES if (Path(mod) / w.format(name)).is_dir()), None)


def cheat_files(mods, bid):
    out = []
    for mod in mod_folders(mods):
        folder = part(mod, 'cheats')
        if folder:
            f = next((p for p in sorted(folder.iterdir()) if p.is_file() and p.name.lower() == f'{bid}.txt'.lower()),
                     None)
            if f:
                out.append((mod.name, f))
    return out


def patch_files(mods):
    out = []
    for mod in mod_folders(mods):
        folders = []
        exefs = part(mod, 'exefs')
        if exefs:
            folders.append(exefs)
        patches = mod / 'atmosphere' / 'exefs_patches'
        if patches.is_dir():
            folders += sorted(d for d in patches.iterdir() if d.is_dir())
        for folder in folders:
            for f in sorted(folder.iterdir()):
                if f.is_file() and f.suffix.lower() in PATCH_SUFFIXES:
                    out.append((mod.name, f.relative_to(mod).as_posix(), f))
    return out


def main_build_id(main):
    try:
        with open(main, 'rb') as f:
            head = f.read(0x48)
    except OSError:
        return ''
    return head[0x40:0x48].hex().upper() if len(head) == 0x48 and head[:4] == b'NSO0' else ''


def code_files(mods, bid):
    return ([(m, f.relative_to(Path(mods) / m).as_posix(), f) for m, f in cheat_files(mods, bid)]
            + patch_files(mods))


def code_record(mods, bid):
    out = {}
    for mod, rel, f in code_files(mods, bid):
        try:
            out[f'{mod}/{rel}'] = hashlib.sha256(f.read_bytes()).hexdigest()
        except OSError:
            pass
    return out
