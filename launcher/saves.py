import contextlib
import json
import os
import re
import shutil
import stat
import tempfile
import time
import zipfile
import zlib
from pathlib import Path

from .context import shorten


def _rmtree(path):
    def writable(func, p, _exc):
        try:
            os.chmod(p, stat.S_IREAD | stat.S_IWRITE)
            func(p)
        except OSError:
            pass
    if Path(path).exists():
        shutil.rmtree(path, onexc=writable)

README = '''NX2Saria saves folder

To import a save, put it here and select Saves, then Import a Save, in the NX2Saria launcher:
- a save someone shared: the NX2Saria-save-....zip that NX2Saria exported
- the files of a Switch save backup, or the folder that holds them
- one save slot (a folder such as slot_00): only that slot is replaced
- an emulator's save folder for the game
- the profile folder (or the whole game folder) of another NX2Saria install

You can also select Import a Save and choose the zip or the folder instead.

Before a save is imported, the game's current save is copied here into a folder named "backup" and the date. An
imported save moves into a folder named "imported" and the date. The game itself keeps its save in profile\\save.
'''
HEX32 = re.compile(r'^[0-9a-fA-F]{32}$')
SLOT = re.compile(r'^slot_\d\d$')
SLOT_FILES = ('progress.sav', 'caption.sav')
MAX_FILES, MAX_BYTES = 4000, 1 << 30
OURS = re.compile(r'^(backup|imported) \d{4}-\d\d-\d\d')


class SaveError(Exception):
    pass


def saves_dir(game):
    return Path(game) / 'saves'


def ensure_saves_dir(game):
    d = saves_dir(game)
    d.mkdir(parents=True, exist_ok=True)
    readme = d / 'README.txt'
    try:
        current = readme.read_text(encoding='utf-8')
    except (OSError, ValueError):
        current = None
    if current != README:
        readme.write_text(README, encoding='utf-8', newline='\r\n')
    return d


def account_dir(game):
    prof = Path(game) / 'profile'
    f = prof / 'account.uid'
    data = f.read_bytes() if f.is_file() else b''
    if not data:
        data = bytes(16)
        while data == bytes(16):
            data = os.urandom(16)
        prof.mkdir(parents=True, exist_ok=True)
        tmp = f.with_name('account.uid.tmp')
        tmp.write_bytes(data)
        os.replace(tmp, f)
    if len(data) != 16 or data == bytes(16):
        raise SaveError(f'{f} is not an account file of the game')
    lo, hi = int.from_bytes(data[:8], 'little'), int.from_bytes(data[8:], 'little')
    return prof / 'save' / f'{hi:016x}{lo:016x}'


def _fresh(folder, name):
    p, n = folder / name, 2
    while p.exists():
        p, n = folder / f'{name} ({n})', n + 1
    return p


def _entries(folder, skip=()):
    return [e for e in Path(folder).iterdir() if e.name not in skip and not e.name.startswith('.')]


def find_save(folder):
    p = Path(folder)
    if not p.is_dir():
        return None, ()
    for base in (p / 'profile' / 'save', p / 'save'):
        if base.is_dir():
            ready = _ready(base.parent / 'save-tx')
            if ready is not None and _has_file(ready):
                return ready, ()
            accounts = [d for d in base.iterdir() if d.is_dir() and HEX32.match(d.name)]
            if accounts:
                return max(accounts, key=lambda d: d.stat().st_mtime), ()
    for _ in range(8):
        entries = _entries(p)
        dirs = [e for e in entries if e.is_dir()]
        if len(entries) != 1 or len(dirs) != 1:
            break
        p = dirs[0]
    return (p, ('save-tx',) if (p / 'save-tx').is_dir() else ()) if _has_file(p) else (None, ())


def _one_slot(src):
    p = Path(src)
    if not (p / 'progress.sav').is_file() or any(e.is_dir() and SLOT.match(e.name) for e in p.iterdir()):
        return None
    return p.name if SLOT.match(p.name) else 'slot_00'


def waiting(game):
    d = saves_dir(game)
    if not d.is_dir():
        return None, ()
    skip = {'README.txt'} | {e.name for e in d.iterdir() if e.is_dir() and OURS.match(e.name)}
    entries = _entries(d, skip)
    if not entries:
        return None, ()
    zips = [e for e in entries if e.is_file() and e.suffix.lower() == '.zip']
    if zips:
        return max(zips, key=lambda e: e.stat().st_mtime), ()
    if len(entries) == 1 and entries[0].is_dir():
        return find_save(entries[0])
    return (d, tuple(sorted(skip))) if any(e.is_file() or _has_file(e) for e in entries) else (None, ())


def _has_file(folder):
    return any(files for _, _, files in os.walk(folder))


def _files(src, skip):
    out, size = [], 0
    for root, dirs, files in os.walk(src):
        if Path(root) == Path(src):
            dirs[:] = [d for d in dirs if d not in skip and not d.startswith('.')]
            files = [f for f in files if f not in skip and not f.startswith('.')]
        for f in files:
            p = Path(root) / f
            out.append(p.relative_to(src))
            size += p.stat().st_size
            if len(out) > MAX_FILES or size > MAX_BYTES:
                raise SaveError(f'{src} holds more than a save ({len(out)} files, {size / 1e6:.0f} MB so far); '
                                'choose the folder with the save\'s files')
    if not out:
        raise SaveError(f'{src} holds no files')
    return out


def _ready(tx):
    ready = []
    if Path(tx).is_dir():
        for d in Path(tx).iterdir():
            m = re.fullmatch(r'ready-(\d+)', d.name)
            if m and d.is_dir():
                ready.append((int(m.group(1)), d))
    return max(ready)[1] if ready else None


def _newest(game, account):
    tx = Path(game) / 'profile' / 'save-tx'
    for d in (_ready(tx), account, tx / 'new', tx / 'old'):
        if d is not None and d.is_dir():
            return d
    return None


def game_running(game):
    from .build import game_running as running
    return running(game)


RUNNING = 'the game is running: quit it first, it keeps its save open while it runs'


@contextlib.contextmanager
def profile_lock(game):
    prof = Path(game) / 'profile'
    path = prof / 'instance.lock'
    try:
        prof.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        k = ctypes.WinDLL('kernel32', use_last_error=True)
        k.CreateFileW.restype = wintypes.HANDLE
        k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                  wintypes.DWORD, wintypes.HANDLE]
        k.CloseHandle.argtypes = [wintypes.HANDLE]
        h = k.CreateFileW(str(path), 0xC0000000, 0, None, 4, 0x80, None)
        if h in (None, wintypes.HANDLE(-1).value):
            h = None
            if ctypes.get_last_error() == 32:
                raise SaveError(RUNNING)
        try:
            yield
        finally:
            if h is not None:
                k.CloseHandle(h)
        return
    f = None
    try:
        f = open(path, 'a+b')
    except OSError:
        pass
    try:
        if f is not None:
            import fcntl
            try:
                fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise SaveError(RUNNING) from None
            except OSError:
                pass
        yield
    finally:
        if f is not None:
            f.close()


def pending_import(game):
    p = Path(game) / 'profile' / PENDING
    return p if p.is_dir() else None


def import_save(game, folder=None, log=print):
    game = Path(game)
    if folder is not None and Path(folder).resolve() == saves_dir(game).resolve():
        folder = None
    if folder is not None and Path(folder).is_file():
        return import_zip(game, folder, log=log)
    from_saves = folder is None
    src, skip = waiting(game) if from_saves else find_save(folder)
    if src is None:
        raise SaveError('the saves folder holds no save' if from_saves else f'{folder} holds no save files')
    stamp = time.strftime('%Y-%m-%d %H.%M.%S')
    if Path(src).is_file():
        r = import_zip(game, src, log=log, stamp=stamp)
        _put_away(game, [Path(src).name], stamp)
        return r
    prof = (game / 'profile').resolve()
    if Path(src).resolve() == prof or prof in Path(src).resolve().parents:
        raise SaveError('that is the game\'s own save')
    files = _files(src, set(skip))
    slot = _one_slot(src)
    account, backup = _replace(game, src, files, slot, stamp, log)
    if from_saves:
        saves = saves_dir(game)
        if Path(src) == saves:
            top = sorted({rel.parts[0] for rel in files})
        else:
            top = [Path(src).relative_to(saves).parts[0]]
        _put_away(game, top, stamp)
    log(f'saves: imported {len(files)} files from {src} into {account / slot if slot else account}' +
        (f'; the old save is in {backup}' if backup else ''))
    return {'files': len(files), 'source': Path(src), 'backup': backup, 'slot': slot}


def _put_away(game, names, stamp):
    saves = saves_dir(game)
    done = _fresh(saves, f'imported {stamp}')
    done.mkdir()
    for name in names:
        shutil.move(str(saves / name), str(done / name))


def _replace(game, src, files, slot, stamp, log):
    with profile_lock(game):
        return _replace_held(Path(game), src, files, slot, stamp, log)


def _replace_held(game, src, files, slot, stamp, log):
    account = account_dir(game)
    saves = ensure_saves_dir(game)
    backup = None
    current = _newest(game, account)
    if current is not None and _has_file(current):
        backup = _fresh(saves, f'backup {stamp}')
        shutil.copytree(current, backup)
    tx = game / 'profile' / 'save-tx'
    tx.mkdir(parents=True, exist_ok=True)
    stage = tx / 'import'
    shutil.rmtree(stage, ignore_errors=True)
    if slot and current is not None:
        shutil.copytree(current, stage, ignore=lambda d, _names: SLOT_FILES if Path(d) == Path(current) else ())
        _rmtree(stage / slot)
    target = stage / slot if slot else stage
    for rel in files:
        (target / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(Path(src) / rel, target / rel)
        os.chmod(target / rel, stat.S_IREAD | stat.S_IWRITE)
    moved = []
    for d in list(tx.iterdir()):
        if d == stage:
            continue
        if _has_file(d) if d.is_dir() else d.stat().st_size:
            if backup is None:
                backup = _fresh(saves, f'backup {stamp}')
                backup.mkdir()
            (backup / 'save-tx').mkdir(exist_ok=True)
            shutil.move(str(d), str(backup / 'save-tx' / d.name))
            moved.append(d)
        else:
            _rmtree(d) if d.is_dir() else d.unlink(missing_ok=True)
    account.parent.mkdir(parents=True, exist_ok=True)
    gone = None
    try:
        if account.exists():
            gone = _fresh(tx, 'replaced')
            os.replace(account, gone)
        try:
            os.replace(stage, account)
        except OSError:
            if gone is not None:
                os.replace(gone, account)
            raise
    except OSError:
        for d in moved:
            try:
                shutil.move(str(backup / 'save-tx' / d.name), str(d))
            except OSError as e:
                log(f'saves: could not put {d.name} back into {tx}: {e}; it is in {backup / "save-tx"}')
        _rmtree(stage)
        raise
    if gone is not None:
        _rmtree(gone)
    for name in (PENDING, PENDING + '.part'):
        if (game / 'profile' / name).exists():
            log(f'saves: the save import the game\'s menu left for its next start ({name}) is dropped; '
                'this import replaces it')
            _rmtree(game / 'profile' / name)
    return account, backup


FORMAT, FORMAT_VERSION = 'nx2navi-save', 1
MANIFEST, SAVE_DIR, REPORT_DIR = 'manifest.json', 'save/', 'bug-report/'
GAME, GAME_VERSION = 'The Legend of Zelda: Ocarina of Time', '1.1.1'
PENDING = 'save-import'
ZIP_MAX_FILES, ZIP_MAX_BYTES = 4000, 256 << 20
SAVE_FILE = re.compile(r'^(account\.sav|option\.sav|slot_\d\d/progress\.sav)$')


def _title_hex():
    from .gamefiles import TITLE_ID
    return f'{TITLE_ID:016x}'


def _key_file(rel):
    n = Path(rel).name.lower()
    return n.endswith('.keys') or n.endswith('.keys.txt')


def default_export_dir():
    from .winsys import known_folder
    for p in (known_folder('downloads'), Path.home() / 'Downloads', known_folder('desktop'), Path.home() / 'Desktop'):
        if p and Path(p).is_dir():
            return Path(p)
    return Path.home()


def export_destination(settings):
    saved = settings.get('save_export_dir')
    return Path(saved) if saved and Path(saved).is_dir() else default_export_dir()


def _fresh_file(folder, stem, suffix):
    p, n = folder / f'{stem}{suffix}', 2
    while p.exists():
        p, n = folder / f'{stem} ({n}){suffix}', n + 1
    return p


NO_SAVE = 'the game has no save yet: play until it saves, then export'


PRIVATE_SETTINGS = ('coop_room', 'coop_server', 'coop_name', 'coop_accept', 'coop_remove', 'menu_test_set')
PRIVATE_SHOWN = b'(set, not shown)'


def scrub_settings(data):
    out = []
    for n, line in enumerate(data.split(b'\n')):
        text = line[:-1] if line.endswith(b'\r') else line
        start = 3 if n == 0 and text.startswith(b'\xef\xbb\xbf') else 0
        body = text[start:].lstrip(b' \t')
        key, eq, value = body.partition(b'=')
        normalized = key.strip(b' \t').lower()
        for prefix in (b'nx2saria_', b'nx2navi_'):
            if normalized.startswith(prefix):
                normalized = normalized[len(prefix):]
                break
        if (eq and value.strip(b' \t') and body[:1] not in (b'#', b';', b'[')
                and normalized in tuple(k.encode() for k in PRIVATE_SETTINGS)):
            line = text[:text.index(b'=') + 1] + b' ' + PRIVATE_SHOWN + line[len(text):]
        out.append(line)
    return b'\n'.join(out)


def short_paths(data):
    return shorten(data.decode('utf-8', 'surrogateescape')).encode('utf-8', 'surrogateescape')


def _add_report_file(z, f, archive_name):
    if f.name.lower() in ('nx2navi.ini', 'nx2saria.ini'):
        z.writestr(REPORT_DIR + archive_name, short_paths(scrub_settings(f.read_bytes())))
    else:
        z.writestr(REPORT_DIR + archive_name, short_paths(f.read_bytes()))


def _report_files(game):
    current_log, legacy_log = game / 'logs' / 'nx2saria.log', game / 'logs' / 'nx2navi.log'
    current_ini, legacy_ini = game / 'nx2saria.ini', game / 'nx2navi.ini'
    log = current_log if current_log.is_file() else legacy_log
    ini = current_ini if current_ini.is_file() else legacy_ini
    return [(p, name) for p, name in ((log, 'nx2saria.log'), (ini, 'nx2saria.ini')) if p.is_file()]


def _export_log(game, dest, version, log):
    extras = _report_files(game)
    if not extras or extras[0][1] != 'nx2saria.log':
        raise SaveError(NO_SAVE)
    dest.mkdir(parents=True, exist_ok=True)
    out = _fresh_file(dest, time.strftime('NX2Saria-log-%Y-%m-%d_%H-%M-%S') + '-bug-report', '.zip')
    tmp = out.with_name(out.name + '.part')
    manifest = {'format': FORMAT, 'format_version': FORMAT_VERSION, 'game': GAME, 'title_id': _title_hex(),
                'game_version': GAME_VERSION, 'nx2navi': version,
                'exported': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'exported_by': 'launcher',
                'files': 0, 'bug_report': True}
    try:
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr(MANIFEST, json.dumps(manifest, indent=1) + '\n')
            for f, name in extras:
                _add_report_file(z, f, name)
        os.replace(tmp, out)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise
    log(f'saves: the game has no save yet; exported {", ".join(name for _, name in extras)} to {out}')
    return {'path': out, 'files': 0, 'extras': [name for _, name in extras]}


def export_save(game, dest=None, bug_report=False, version='', log=print):
    game = Path(game)
    dest = Path(dest) if dest else default_export_dir()
    if not (game / 'profile' / 'account.uid').is_file():
        if bug_report:
            return _export_log(game, dest, version, log)
        raise SaveError(NO_SAVE)
    account = account_dir(game)
    dest.mkdir(parents=True, exist_ok=True)
    stem = time.strftime('NX2Saria-save-%Y-%m-%d_%H-%M-%S') + ('-bug-report' if bug_report else '')
    extras = _report_files(game) if bug_report else []
    for attempt in range(3):
        current = _newest(game, account)
        if current is None or not _has_file(current):
            if bug_report:
                return _export_log(game, dest, version, log)
            raise SaveError(NO_SAVE)
        files = [rel for rel in _files(current, set()) if not _key_file(rel)]
        out = _fresh_file(dest, stem, '.zip')
        tmp = out.with_name(out.name + '.part')
        manifest = {'format': FORMAT, 'format_version': FORMAT_VERSION, 'game': GAME, 'title_id': _title_hex(),
                    'game_version': GAME_VERSION, 'nx2navi': version,
                    'exported': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'exported_by': 'launcher',
                    'files': len(files), 'bug_report': bool(bug_report)}
        try:
            with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
                z.writestr(MANIFEST, json.dumps(manifest, indent=1) + '\n')
                for rel in files:
                    z.write(current / rel, SAVE_DIR + rel.as_posix())
                for f, name in extras:
                    _add_report_file(z, f, name)
            os.replace(tmp, out)
        except FileNotFoundError:
            tmp.unlink(missing_ok=True)
            if attempt == 2:
                raise
            time.sleep(0.2)
            continue
        except BaseException:
            tmp.unlink(missing_ok=True)
            raise
        log(f'saves: exported {len(files)} files from {current} to {out}' +
            (f' with {", ".join(name for _, name in extras)}' if extras else ''))
        return {'path': out, 'files': len(files), 'extras': [name for _, name in extras]}


def _member_path(name):
    parts = name.split('/')
    if '\\' in name or name.startswith('/') or any(p in ('', '.', '..') for p in parts) or ':' in name:
        raise SaveError(f'it holds a file outside the save ({SAVE_DIR}{name})')
    return Path(*parts)


def check_manifest(m):
    if not isinstance(m, dict) or m.get('format') != FORMAT:
        raise SaveError(f'its {MANIFEST} is not an NX2Saria save manifest')
    v = m.get('format_version')
    if type(v) is not int or v < 1:
        raise SaveError(f'its {MANIFEST} has no format version')
    if v > FORMAT_VERSION:
        raise SaveError(f'it was made by a newer NX2Saria ({m.get("nx2navi") or "format " + str(v)}): update NX2Saria '
                        'first')
    if str(m.get('title_id', '')).lower() != _title_hex():
        raise SaveError(f'it is a save of another game ({m.get("game") or m.get("title_id") or "not named"})')


def read_zip(path):
    name = Path(path).name
    try:
        z = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError) as e:
        raise SaveError(f'{name} cannot be read as a zip file ({e})') from None
    with z:
        try:
            info = z.getinfo(MANIFEST)
        except KeyError:
            raise SaveError(f'{name} is not a save exported by NX2Saria (it has no {MANIFEST}); to import a save '
                            'folder, unpack the zip and choose the folder') from None
        try:
            if info.file_size > 1 << 16:
                raise ValueError('too large')
            m = json.loads(z.read(info).decode('utf-8'))
        except (ValueError, zipfile.BadZipFile, OSError, EOFError, zlib.error) as e:
            raise SaveError(f'the {MANIFEST} of {name} cannot be read ({e})') from None
        check_manifest(m)
        members, total = [], 0
        for info in z.infolist():
            if not info.filename.startswith(SAVE_DIR) or info.is_dir():
                continue
            rel = _member_path(info.filename[len(SAVE_DIR):])
            if (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise SaveError(f'it holds a link ({info.filename})')
            if _key_file(rel):
                continue
            total += info.file_size
            members.append((info, rel))
            if len(members) > ZIP_MAX_FILES or total > ZIP_MAX_BYTES:
                raise SaveError(f'it holds more than a save ({len(members)} files, {total / 1e6:.0f} MB so far)')
        if not any(SAVE_FILE.match(rel.as_posix()) for _, rel in members):
            raise SaveError(f'it holds no save of {GAME}' if members else 'it holds no save files')
    return m, members


def import_zip(game, path, log=print, stamp=None):
    game = Path(game)
    m, members = read_zip(path)
    stage = Path(tempfile.mkdtemp(prefix='nx2navi-save-'))
    try:
        try:
            with zipfile.ZipFile(path) as z:
                for info, rel in members:
                    out = stage / rel
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(info) as f, open(out, 'wb') as g:
                        shutil.copyfileobj(f, g)
        except (zipfile.BadZipFile, EOFError, zlib.error) as e:
            raise SaveError(f'{Path(path).name} is damaged ({e})') from None
        files = [rel for _, rel in members]
        account, backup = _replace(game, stage, files, None, stamp or time.strftime('%Y-%m-%d %H.%M.%S'), log)
    finally:
        _rmtree(stage)
    log(f'saves: imported {len(files)} files from {path} (exported {m.get("exported", "?")} by NX2Saria '
        f'{m.get("nx2navi") or "?"}, game {m.get("game_version", "?")}) into {account}' +
        (f'; the old save is in {backup}' if backup else ''))
    return {'files': len(files), 'source': Path(path), 'backup': backup, 'slot': None, 'manifest': m}
