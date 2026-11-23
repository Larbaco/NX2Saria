import hashlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / 'build' / 'dl'
VERSION = '3.14.9'
CPYTHON_SOURCE = (f'https://www.python.org/ftp/python/{VERSION}/Python-{VERSION}.tar.xz',
                  'e02d9ac21cebda6e46435369744a92c65cc7cd8669697694122cf97ffacd1e53')
CPYTHON_LICENSE = f'Python-{VERSION}/Doc/license.rst'
CPYTHON_TEXT = 'cpython-license.rst'
PBS_BUILD = '20261021'
PBS_FULL = ('https://github.com/astral-sh/python-build-standalone/releases/download/'
            f'{PBS_BUILD}/cpython-{VERSION}%2B{PBS_BUILD}-x86_64-unknown-linux-gnu-pgo%2Blto-full.tar.zst',
            '92f6abbce0283ed317873615396b5c43ccdcdf13ae9c7d68c9a07651df527528')

WINDOWS_TEXTS = {
    'OpenSSL': ('licenses/python/' + CPYTHON_TEXT, 'OpenSSL'),
    'libffi': ('python/LICENSE.txt', 'libffi - Copyright'),
    'bzip2': ('python/LICENSE.txt', 'bzip2/libbzip2'),
    'Zstandard': ('python/LICENSE.txt', 'For Zstandard software'),
    'Tcl/Tk': ('python/LICENSE.txt', 'copyrighted by the Regents of the University of'),
    'expat': ('licenses/python/' + CPYTHON_TEXT, 'expat'),
    'mpdecimal': ('licenses/python/' + CPYTHON_TEXT, 'libmpdec'),
    'zlib': ('licenses/python/' + CPYTHON_TEXT, 'zlib'),
    'SQLite': None,
    'xz': None,
    'libtommath': None,
}
LINUX_TEXTS = {
    'OpenSSL': 'LICENSE.openssl-3.txt', 'SQLite': 'LICENSE.sqlite.txt', 'libffi': 'LICENSE.libffi.txt',
    'zlib': 'LICENSE.zlib.txt', 'zlib-ng': 'LICENSE.zlib-ng.txt', 'bzip2': 'LICENSE.bzip2.txt',
    'xz': 'LICENSE.liblzma.txt', 'Zstandard': 'LICENSE.zstd.txt', 'mpdecimal': 'LICENSE.mpdecimal.txt',
    'expat': 'LICENSE.expat.txt', 'ncurses': 'LICENSE.ncurses.txt', 'libedit': 'LICENSE.libedit.txt',
    'libuuid': 'LICENSE.libuuid.txt', 'Tcl/Tk': 'LICENSE.tcl.txt', 'Tix': 'LICENSE.tix.txt',
    'libX11': 'LICENSE.libX11.txt', 'libXau': 'LICENSE.libXau.txt', 'libxcb': 'LICENSE.libxcb.txt',
    'CPython': 'LICENSE.cpython.txt',
}


def fetch(pin, cache=CACHE):
    url, sha = pin
    dest = cache / url.rsplit('/', 1)[1].replace('%2B', '+')
    if dest.is_file() and hashlib.sha256(dest.read_bytes()).hexdigest() == sha:
        return dest
    cache.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + '.part')
    print(f'download {url}', flush=True)
    with urllib.request.urlopen(url) as r, open(tmp, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    got = hashlib.sha256(tmp.read_bytes()).hexdigest()
    if got != sha:
        tmp.unlink()
        sys.exit(f'{url}: sha256 {got}, pinned {sha}')
    tmp.replace(dest)
    return dest


def cpython_license(dest, cache=CACHE):
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fetch(CPYTHON_SOURCE, cache)) as t:
        (dest / CPYTHON_TEXT).write_bytes(t.extractfile(CPYTHON_LICENSE).read())
    return dest / CPYTHON_TEXT


def check_build(python):
    build = (python / 'BUILD').read_text(encoding='utf-8').strip() if (python / 'BUILD').is_file() else '?'
    r = subprocess.run([str(python / 'bin' / 'python3'), '-I', '-c', 'import platform; print(platform.python_version())'],
                       capture_output=True, text=True)
    version = r.stdout.strip()
    if (build, version) != (PBS_BUILD, VERSION):
        return (f'{python} is python-build-standalone {build}, Python {version or "?"}; the licence texts are pinned '
                f'for {PBS_BUILD}, Python {VERSION} (tools/python_licences.py): give the Python that '
                f'`uv python install {VERSION}` installs, or pin the build it is')
    return None


def pbs_licences(dest, cache=CACHE):
    archive = fetch(PBS_FULL, cache)
    dest.mkdir(parents=True, exist_ok=True)
    tmp = cache / 'pbs-licences'
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir()
    subprocess.run(['tar', '--zstd', '-xf', str(archive), '-C', str(tmp), 'python/licenses'], check=True)
    for f in sorted((tmp / 'python' / 'licenses').iterdir()):
        shutil.copy2(f, dest / f.name)
    shutil.rmtree(tmp, ignore_errors=True)
    missing = [n for n in LINUX_TEXTS.values() if not (dest / n).is_file()]
    if missing:
        sys.exit(f'{PBS_FULL[0]}: no {", ".join(missing)} in python/licenses/')
    return sorted(p.name for p in dest.iterdir())
