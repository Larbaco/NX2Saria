import json
import os
import threading
import time

_guard = threading.Lock()
_files = {}
_claims = {}


class Busy(Exception):
    pass
def _key(path):
    return os.path.normcase(os.path.abspath(str(path)))


def _os_lock(f):
    if os.name == 'nt':
        import msvcrt
        f.seek(0)
        msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
    else:
        import fcntl
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)


def owner(path):
    try:
        with open(str(path) + '.owner', encoding='utf-8') as f:
            o = json.load(f)
        return f'{o["what"]} (process {int(o["pid"])}, since {time.strftime("%H:%M", time.localtime(o["since"]))})'
    except (OSError, ValueError, KeyError, TypeError, OverflowError):
        return 'another NX2Saria'


class FileLock:
    def __init__(self, path, what, wait=0.0, where=None):
        self.path, self.key = path, _key(path)
        deadline = time.time() + wait
        while True:
            with _guard:
                if self._take(what):
                    return
            if time.time() >= deadline:
                raise Busy(f'{owner(path)} is using {where or os.path.dirname(os.path.abspath(str(path)))}')
            time.sleep(0.25)

    def _take(self, what):
        entry = _files.get(self.key)
        if entry:
            entry[1] += 1
            return True
        try:
            f = open(self.path, 'a+')
        except OSError:
            self.key = None
            return True
        try:
            _os_lock(f)
        except OSError:
            f.close()
            return False
        _files[self.key] = [f, 1]
        try:
            with open(str(self.path) + '.owner', 'w', encoding='utf-8') as o:
                json.dump({'what': what, 'pid': os.getpid(), 'since': time.time()}, o)
        except OSError:
            pass
        return True

    def release(self):
        with _guard:
            key, self.key = self.key, None
            entry = _files.get(key) if key else None
            if entry:
                entry[1] -= 1
                if entry[1] <= 0:
                    del _files[key]
                    entry[0].close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()


def claim(folder, names, who):
    keys = [(_key(folder), n) for n in names]
    with _guard:
        for k in keys:
            if k in _claims:
                raise Busy(f'{_claims[k]} is still running: wait for it to end, or cancel it')
        for k in keys:
            _claims[k] = who
    return keys


def release_claim(keys):
    with _guard:
        for k in keys or ():
            _claims.pop(k, None)
