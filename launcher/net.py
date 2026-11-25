import email.utils
import errno
import hashlib
import json
import os
import shutil
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import timezone
from http.client import IncompleteRead
from pathlib import Path

from . import NAME

CHUNK = 1 << 20
CLOCK_TOLERANCE = 3600


class Cancelled(Exception):
    pass


class DownloadError(Exception):
    pass


class CertificateError(DownloadError):
    pass
def user_agent(version='dev'):
    return f'{NAME}-Launcher/{version}'


def _under_wine():
    try:
        import ctypes
        return hasattr(ctypes.WinDLL('ntdll'), 'wine_get_version')
    except Exception:
        return False


def _windows_check():
    if os.name != 'nt' or _under_wine():
        return None
    try:
        import truststore
        return truststore.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    except Exception:
        return None


_CHECKS = None
_ACCEPTED = {}


def _checks():
    global _CHECKS
    if _CHECKS is None:
        checks = [("Python's check", ssl.create_default_context())]
        windows = _windows_check()
        if windows is not None:
            checks.append(("Windows' check", windows))
        _CHECKS = checks
    return _CHECKS


LOOPBACK = ('127.0.0.1', 'localhost', '::1')


def _plain_ok(url):
    return urllib.parse.urlsplit(url).hostname in LOOPBACK


class _Redirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme.lower() != 'https' and not _plain_ok(newurl):
            raise urllib.error.HTTPError(newurl, code, f'redirect to {newurl} refused: not https', headers, fp)
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and urllib.parse.urlsplit(newurl).hostname != urllib.parse.urlsplit(req.full_url).hostname:
            new.remove_header('Authorization')
        return new


def _open(url, h, timeout, ctx=None, data=None, method=None):
    handlers = [_Redirect()] + ([urllib.request.HTTPSHandler(context=ctx)] if ctx is not None else [])
    return urllib.request.build_opener(*handlers).open(urllib.request.Request(url, data=data, headers=h, method=method),
                                                       timeout=timeout)


def request(url, headers=None, timeout=30, version='dev', data=None, method=None):
    h = {'User-Agent': user_agent(version)}
    h.update(headers or {})
    if urllib.parse.urlsplit(url).scheme.lower() != 'https':
        if not _plain_ok(url):
            raise DownloadError(f'{url}: not an https address')
        return _open(url, h, timeout, data=data, method=method)
    host = urllib.parse.urlsplit(url).hostname or ''
    checks = _checks()
    first = min(_ACCEPTED.get(host, 0), len(checks) - 1)
    refused = []
    for i in [first] + [i for i in range(len(checks)) if i != first]:
        name, ctx = checks[i]
        try:
            r = _open(url, h, timeout, ctx, data, method)
        except urllib.error.URLError as e:
            if not isinstance(e.reason, ssl.SSLCertVerificationError):
                raise
            refused.append((name, e.reason))
            continue
        _ACCEPTED[host] = i
        return r
    raise CertificateError(certificate_problem(url, refused, version)) from refused[0][1]


def server_clock(url, timeout=10, version='dev'):
    class Stay(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None

    req = urllib.request.Request(url, method='HEAD', headers={'User-Agent': user_agent(version)})
    try:
        with urllib.request.build_opener(Stay).open(req, timeout=timeout) as r:
            date = r.headers.get('Date')
    except urllib.error.HTTPError as e:
        date = e.headers.get('Date')
        e.close()
    except Exception:
        return None
    if not date:
        return None
    try:
        when = email.utils.parsedate_to_datetime(date)
    except (TypeError, ValueError):
        return None
    return (when if when.tzinfo else when.replace(tzinfo=timezone.utc)).timestamp()


def _said(err):
    said = getattr(err, 'verify_message', None) or (str(err.args[-1]) if err.args else type(err).__name__)
    return said.strip().rstrip('.')


def _local(t):
    return time.strftime('%Y-%m-%d %H:%M', time.localtime(t))


def certificate_problem(url, refused, version='dev'):
    host = urllib.parse.urlsplit(url).hostname or url
    said = '; '.join(f'{name}: {_said(e)}' for name, e in refused)
    text = f'{url}: the certificate of {host} was not accepted ({said}).'
    where = 'Windows Settings > Time & language > Date & time' if os.name == 'nt' else "the system's settings"
    ours, theirs = time.time(), server_clock(f'http://{host}/', version=version)
    if theirs is not None and abs(ours - theirs) > CLOCK_TOLERANCE:
        return (f"{text} This PC's clock reads {_local(ours)}, but {host} says it is {_local(theirs)}: set the date, "
                f'time and time zone in {where}, then try again.')
    clock = (f"This PC's clock agrees with {host}'s." if theirs is not None else
             f"Check that this PC's date, time and time zone are right ({where}).")
    return (f'{text} {clock} Security software that scans encrypted connections (HTTPS or SSL scanning) or a proxy '
            'may be replacing the certificate: let NX2Saria through it or turn that scanning off, then try again.')


def _check_eof(response):
    remaining = getattr(response, 'length', None)
    if remaining:
        raise ConnectionError(f'connection closed with {remaining} response bytes still expected')


def get_bytes(url, headers=None, timeout=30, version='dev', limit=64 << 20, retries=3):
    last_error = None
    for attempt in range(retries):
        try:
            with request(url, headers, timeout, version) as r:
                data = r.read(limit + 1)
                if len(data) > limit:
                    raise DownloadError(f'{url}: more than {limit >> 20} MB')
                _check_eof(r)
            break
        except urllib.error.HTTPError as e:
            if e.code != 429 and e.code < 500:
                raise
            last_error = e
        except (urllib.error.URLError, OSError, IncompleteRead) as e:
            last_error = e
        if attempt + 1 < retries:
            time.sleep(min(2 ** attempt, 8))
    else:
        raise last_error
    return data


def get_json(url, headers=None, timeout=30, version='dev'):
    return json.loads(get_bytes(url, headers, timeout, version).decode('utf-8'))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(CHUNK):
            h.update(b)
    return h.hexdigest()


SPACE_MARGIN = 32 << 20


class NoSpace(DownloadError):
    pass
def free_bytes(folder):
    p = Path(folder).absolute()
    for d in (p, *p.parents):
        try:
            return shutil.disk_usage(d).free
        except FileNotFoundError:
            continue
        except OSError:
            return None
    return None


def space_problem(folder, need, what):
    free = free_bytes(folder)
    if free is None or free >= max(0, need) + SPACE_MARGIN:
        return ''
    p = Path(folder).absolute()
    return (f'there is no room for {what} on {p.anchor or p} ({(max(0, need) + SPACE_MARGIN) / 1e6:,.0f} MB needed, '
            f'{free / 1e6:,.0f} MB free): make room there and try again')


PART_OF = '.of'


def download(url, dest, sha256=None, size=None, progress=None, cancel=None, headers=None, retries=4, version='dev',
             log=None):
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    if sha256 and dest.is_file() and sha256_file(dest) == sha256.lower():
        if progress:
            progress(dest.stat().st_size, dest.stat().st_size)
        return dest
    part = dest.with_name(dest.name + '.part')
    note = part.with_name(part.name + PART_OF)
    wanted = f'sha256 {sha256.lower()}' if sha256 else url
    if part.is_file():
        try:
            begun_for = note.read_text(encoding='utf-8').strip()
        except (OSError, ValueError):
            begun_for = ''
        if begun_for and begun_for != wanted:
            part.unlink(missing_ok=True)
            if log:
                log(f'{part.name} was begun for another file ({begun_for}), not this one ({wanted}): it is deleted '
                    'and the download starts over')
    try:
        note.write_text(wanted, encoding='utf-8')
    except OSError:
        pass
    if size:
        full = space_problem(dest.parent, size - (part.stat().st_size if part.is_file() else 0), dest.name)
        if full:
            raise NoSpace(full)
    last_error = None
    for attempt in range(retries):
        have = part.stat().st_size if part.is_file() else 0
        h = dict(headers or {})
        if have:
            h['Range'] = f'bytes={have}-'
        try:
            with request(url, h, 60, version) as r:
                status = getattr(r, 'status', 200)
                if have and status != 206:
                    have = 0
                coming = int(r.headers.get('Content-Length') or 0)
                total = size or (coming + have if coming else 0)
                if coming and not size:
                    full = space_problem(dest.parent, coming, dest.name)
                    if full:
                        raise NoSpace(full)
                with open(part, 'ab' if have else 'wb') as f:
                    done = have
                    while True:
                        if cancel and cancel():
                            raise Cancelled()
                        b = r.read(CHUNK)
                        if not b:
                            _check_eof(r)
                            break
                        f.write(b)
                        done += len(b)
                        if size and done > size:
                            raise DownloadError(f'{url}: more than the expected {size} bytes')
                        if progress:
                            progress(done, total or done)
            break
        except (Cancelled, CertificateError, NoSpace):
            raise
        except DownloadError:
            part.unlink(missing_ok=True)
            raise
        except urllib.error.HTTPError as e:
            if e.code == 416 and have:
                part.unlink(missing_ok=True)
                last_error = e
                continue
            if e.code in (401, 403, 404, 410) or 300 <= e.code < 400:
                raise DownloadError(f'{url}: HTTP {e.code}') from e
            last_error = e
        except (urllib.error.URLError, OSError, IncompleteRead) as e:
            if isinstance(e, OSError) and e.errno == errno.ENOSPC:
                p = dest.parent.absolute()
                raise NoSpace(f'{p.anchor or p} is full: {dest.name} could not be written. Make room there and try '
                              'again') from e
            last_error = e
        time.sleep(min(2 ** attempt, 16))
    else:
        raise DownloadError(f'{url}: {last_error}')
    got = part.stat().st_size
    if size and got != size:
        part.unlink(missing_ok=True)
        raise DownloadError(f'{url}: {got} bytes, expected {size}')
    if sha256:
        digest = sha256_file(part)
        if digest != sha256.lower():
            part.unlink(missing_ok=True)
            raise DownloadError(f'{url}: sha256 {digest}, expected {sha256.lower()}')
    os.replace(part, dest)
    note.unlink(missing_ok=True)
    return dest
