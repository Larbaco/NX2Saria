"""SDL3 for a Linux build on a system that has none: the headers and libSDL3.so.0 a build kit carries.

The runtime's window, input and audio are SDL 3. Ubuntu 24.04 LTS and the distributions built on it (Linux Mint 22 and
others on the noble base) package no SDL3 at all, so a build with the system's compiler there found no SDL3/SDL.h
(issue #11). This is the SDL3 a kit carries for them, in toolchain/sdl3. tools/make_game.py takes it where the
compiler finds no SDL3 headers of the system's (sdl3_choice): the runtime is compiled and the game linked against it,
libSDL3.so.0 goes beside the game (place_sdl3), and the preflight runs a program linked against it, so that a library
it needs and the system lacks stops the build, by name, before the long steps.

It is SDL 3.4.18 as the Steam Runtime 3 builds it, the build the Steam Runtime 3 "sniper" SDK sysroot holds. Built for the Steam Runtime's glibc (its newest symbol version is GLIBC_2.29), it runs on any distribution at
least that new. It links the desktop's client libraries directly instead of loading them when it starts: libX11,
libXext, libXcursor, libXi, libXfixes, libXrandr, libXss, libXtst, libxkbcommon, libwayland-client, -cursor and -egl,
libdecor-0, libpulse, libasound and libfribidi, all of which a desktop install of Ubuntu or Mint has.

The files come from one of two places, checked either way:
  - the runtime's apt pool: the packages libsdl3-0 (the library) and libsdl3-dev (the headers), 2.7 MB, each checked
    against its SHA-256. The pool keeps the current version of a package only, so these go once the Steam Runtime
    moves on;
  - then the dated snapshot of the sniper SDK sysroot that holds the same build, 1.2 GB, read as it downloads and
    checked against its SHA-256 at the end, nothing of it written to disk but the files below.
Both must give exactly the files CONTENT_SHA256 names (content_digest), so the kit's SDL3 is the same from either.

Kept: include/SDL3/*.h, lib/libSDL3.so.0 (the library's file under its soname), the package's copyright file
(SDL3-copyright.txt: SDL is zlib, and the file names the licences of the parts it contains) and pinned.txt. Standard
library only: the build kit's Python runs this.

    python tools/sdl3_pin.py [--dest toolchain/sdl3] [--deb FILE FILE | --sysroot FILE]
"""
import argparse
import hashlib
import http.client
import io
import re
import shutil
import sys
import tarfile
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION = '3.4.18'                                  # SDL's own version, as SDL_version.h defines it
DEB_VERSION = '3.4.18+ds-1+steamrt3.1+bsrt3.1'      # the packages' version in the Steam Runtime 3 apt pool
POOL = 'https://repo.steampowered.com/steamrt3/apt/pool/main/libs/libsdl3'
# package -> (address, SHA-256)
DEBS = {
    'libsdl3-0': (f'{POOL}/libsdl3-0_{urllib.parse.quote(DEB_VERSION)}_amd64.deb',
                  '0f6a16b1f8f658f705a626fdf07742b169e22004390afcbaa29973c5175edcaf'),
    'libsdl3-dev': (f'{POOL}/libsdl3-dev_{urllib.parse.quote(DEB_VERSION)}_amd64.deb',
                    '0036d658efb9690a4c7f24687ccf4ee7b7ae8da57591d9135973588110eaaf5b'),
}
# The sniper SDK sysroot snapshot with libsdl3-0 and libsdl3-dev DEB_VERSION installed.
SYSROOT = ('https://repo.steampowered.com/steamrt-images-sniper/snapshots/3.0.20261021.266025/'
           'com.valvesoftware.SteamRuntime.Sdk-amd64%2Ci386-sniper-sysroot.tar.gz',
           '8f71947035f97925a311b2ed753f22efcf2f13d6f1631945b8e537f8903ce2cf')
SYSROOT_SIZE = 1234880637
# content_digest of the kept files (pinned.txt aside): the 86 headers, the library and the copyright file
CONTENT_SHA256 = '8d0ec6fe22567e60b9692e33ff8fd377c32b4cc866c981fcc4af1a3613d72cac'
FOLDER = Path('toolchain') / 'sdl3'   # below a checkout's or a kit's root: include/, lib/, the copyright file
STAMP = 'pinned.txt'                  # in the folder: the packages' version, written last
LIB = 'libSDL3.so.0'                  # the soname the game is linked against and finds beside itself
NOTICE = 'SDL3-copyright.txt'
# Members of a package's data archive or of the sysroot that are kept; HEADER's file name part takes no '..' and no
# folder, so nothing is written outside the folder.
HEADER = re.compile(r'(?:\./)?usr/include/SDL3/([A-Za-z0-9_]+\.h)')
LIBRARY = re.compile(r'(?:\./)?usr/lib/x86_64-linux-gnu/libSDL3\.so\.0\.\d+\.\d+')
COPYRIGHT = re.compile(r'(?:\./)?usr/share/doc/libsdl3-0/copyright')
# Files a kept member may be a hard link to: SDL3's own, a few MB, held while an archive is read.
NEAR = re.compile(r'(?:\./)?usr/(?:include/SDL3/|lib/x86_64-linux-gnu/libSDL3|share/doc/libsdl3)')


def have(folder):
    """True when folder holds the pinned SDL3, complete (the stamp is written after the last file)."""
    try:
        return (folder / STAMP).read_text(encoding='ascii').strip() == DEB_VERSION and \
            (folder / 'include' / 'SDL3' / 'SDL.h').is_file() and (folder / 'lib' / LIB).is_file()
    except (OSError, ValueError):   # ValueError: a stamp that is not ASCII
        return False


def kept_name(member):
    """Where a member of the packages or the sysroot goes in the folder, None when it is not kept."""
    if m := HEADER.fullmatch(member):
        return Path('include') / 'SDL3' / m.group(1)
    if LIBRARY.fullmatch(member):
        return Path('lib') / LIB
    if COPYRIGHT.fullmatch(member):
        return Path(NOTICE)
    return None


def take(tar, kept):
    """The kept members of an open tar archive into kept ({folder path: bytes}). A kept member that is a hard link to
    one of SDL3's files read earlier (NEAR) takes that file's bytes; symbolic links are not followed (the library is
    taken by its own file name)."""
    seen = {}
    for m in tar:
        name = m.name.removeprefix('./')
        rel = kept_name(m.name)
        if m.isfile() and (rel or NEAR.match(m.name)):
            seen[name] = tar.extractfile(m).read()
            if rel:
                kept[rel] = seen[name]
        elif m.islnk() and rel and m.linkname.removeprefix('./') in seen:
            kept[rel] = seen[m.linkname.removeprefix('./')]


def deb_data(deb):
    """(name, bytes) of the data archive of a Debian package: an ar archive whose data.tar.* member holds the files."""
    if deb[:8] != b'!<arch>\n':
        raise ValueError('not a Debian package (no ar header)')
    at = 8
    while at + 60 <= len(deb):
        head = deb[at:at + 60]
        try:
            name, size = head[:16].decode('ascii').strip().rstrip('/'), int(head[48:58].decode('ascii'))
        except ValueError:
            raise ValueError(f'the package\'s member header at {at} is not an ar header') from None
        if head[58:60] != b'`\n' or not 0 <= size <= len(deb) - at - 60:
            raise ValueError(f'the package\'s member header at {at} is not an ar header, or its member is cut short')
        if name.startswith('data.tar'):
            return name, deb[at + 60:at + 60 + size]
        at += 60 + size + (size & 1)   # members start on even offsets
    raise ValueError('the package has no data.tar member')


def deb_files(deb, kept):
    """The kept files of a Debian package (bytes) into kept."""
    name, body = deb_data(deb)
    mode = {'data.tar.xz': 'r:xz', 'data.tar.gz': 'r:gz', 'data.tar': 'r:'}.get(name)
    if not mode:   # the pinned packages are xz, as the Steam Runtime's dpkg writes them
        raise ValueError(f'the package\'s {name} is not xz, gzip or plain tar')
    try:
        with tarfile.open(fileobj=io.BytesIO(body), mode=mode) as tar:
            take(tar, kept)
    except (tarfile.TarError, EOFError, OSError) as e:
        raise ValueError(f'the package\'s {name} cannot be read ({e})') from None


def content_digest(kept):
    """One SHA-256 over the kept files' paths and SHA-256s: equal for two sources that gave the same files."""
    h = hashlib.sha256()
    for rel in sorted(kept, key=lambda p: p.as_posix()):
        h.update(rel.as_posix().encode() + b'\0' + hashlib.sha256(kept[rel]).hexdigest().encode() + b'\n')
    return h.hexdigest()


def clear(folder):
    """What an earlier unpack wrote into folder, and nothing else of it."""
    (folder / STAMP).unlink(missing_ok=True)
    (folder / NOTICE).unlink(missing_ok=True)
    (folder / 'lib' / LIB).unlink(missing_ok=True)
    shutil.rmtree(folder / 'include' / 'SDL3', ignore_errors=True)


def write(kept, folder, source):
    """The kept files into folder, after they are checked: complete and exactly the pinned ones."""
    for need in (Path('include') / 'SDL3' / 'SDL.h', Path('include') / 'SDL3' / 'SDL_version.h', Path('lib') / LIB,
                 Path(NOTICE)):
        if need not in kept:
            sys.exit(f'sdl3_pin: {source} lacks {need.as_posix()} of SDL {DEB_VERSION}')
    if content_digest(kept) != CONTENT_SHA256:
        sys.exit(f'sdl3_pin: the files of SDL {DEB_VERSION} from {source} are not the pinned ones')
    clear(folder)
    for rel, body in kept.items():
        dst = folder / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(body)
    (folder / 'lib' / LIB).chmod(0o755)
    (folder / STAMP).write_text(DEB_VERSION + '\n', encoding='ascii')
    return len(kept)


def unpack(debs, folder):
    """The headers, the library and its copyright file of the two packages (name -> bytes) into folder."""
    kept = {}
    for data in debs.values():
        deb_files(data, kept)
    return write(kept, folder, 'the packages')


class HashingReader:
    """A file object over a download that hashes what is read through it and counts it."""
    def __init__(self, f):
        self.f, self.h, self.n = f, hashlib.sha256(), 0

    def read(self, n=-1):
        data = self.f.read(n)
        self.h.update(data)
        self.n += len(data)
        return data


def sysroot_files(f, source):
    """The kept files of the sysroot archive read from f (a download or a file), checked against SYSROOT's SHA-256
    once all of it is read."""
    r, kept = HashingReader(f), {}
    try:
        with tarfile.open(fileobj=r, mode='r|gz') as tar:
            take(tar, kept)
        while r.read(1 << 20):   # what follows the archive's end marker, for the hash
            pass
    except (tarfile.TarError, EOFError, OSError, http.client.HTTPException) as e:
        sys.exit(f'sdl3_pin: reading {source} failed after {r.n / 1e6:.0f} MB ({e})')
    if r.h.hexdigest() != SYSROOT[1]:
        sys.exit(f'sdl3_pin: {source} is not the pinned sniper SDK sysroot (' +
                 (f'cut short: {r.n} of {SYSROOT_SIZE} bytes)' if r.n < SYSROOT_SIZE else 'SHA-256 differs)'))
    return kept


def download(url):
    """The bytes at url, None when the server or the network does not give them."""
    try:
        with urllib.request.urlopen(url, timeout=120) as r:
            return r.read()
    except (OSError, http.client.HTTPException) as e:
        print(f'sdl3_pin: {url} cannot be fetched ({e})', flush=True)
        return None


def help_text():
    return ('Fetch these on a machine with internet and pass them with tools/sdl3_pin.py --deb FILE FILE, or the '
            'sysroot with --sysroot FILE:\n' +
            ''.join(f'  {url}\n    sha256 {sha}\n' for url, sha in [*DEBS.values(), SYSROOT]))


def resolve(folder=None, debs=None, sysroot=None):
    """The folder holding the pinned SDL3 (default <root>/toolchain/sdl3), fetched and unpacked there when it does not
    hold it yet. debs: the two packages on disk instead of the download, in any order, each one of the pinned files;
    sysroot: the sniper SDK sysroot archive on disk. Returns the folder: include/ goes on the include path,
    lib/libSDL3.so.0 into the link."""
    folder = Path(folder) if folder else ROOT / FOLDER
    if have(folder):
        return folder
    if sysroot:
        try:
            with open(sysroot, 'rb') as f:
                kept = sysroot_files(f, sysroot)
        except OSError as e:
            sys.exit(f'sdl3_pin: {sysroot} cannot be read ({e})')
        write(kept, folder, sysroot)
        return folder
    by_hash = {sha: name for name, (_, sha) in DEBS.items()}
    got = {}
    for path in debs or ():
        try:
            data = Path(path).read_bytes()
        except OSError as e:
            sys.exit(f'sdl3_pin: {path} cannot be read ({e})')
        name = by_hash.get(hashlib.sha256(data).hexdigest())
        if not name:
            sys.exit(f'sdl3_pin: {path} is neither pinned package of SDL {DEB_VERSION} (SHA-256 differs)')
        got[name] = data
    for name, (url, want) in DEBS.items():
        if name in got:
            continue
        print(f'sdl3_pin: fetching {name} {DEB_VERSION} into {folder}', flush=True)
        data = download(url)
        if data is None:
            break
        if hashlib.sha256(data).hexdigest() != want:
            sys.exit(f'sdl3_pin: {url} is not the pinned package {name} {DEB_VERSION} (SHA-256 differs)')
        got[name] = data
    if len(got) == len(DEBS):
        unpack(got, folder)
        return folder
    url = SYSROOT[0]
    print(f'sdl3_pin: the packages of SDL {DEB_VERSION} are not in the Steam Runtime\'s pool; taking the same files '
          f'from the dated sniper SDK sysroot instead ({SYSROOT_SIZE / 1e9:.1f} GB, read as it downloads, not kept)',
          flush=True)
    try:
        with urllib.request.urlopen(url, timeout=120) as r:
            kept = sysroot_files(r, url)
    except (OSError, http.client.HTTPException) as e:
        sys.exit(f'sdl3_pin: fetching {url} failed ({e}).\n{help_text()}')
    write(kept, folder, url)
    return folder


def header_version(text):
    """(major, minor, micro) as SDL_version.h defines them in text, None when it does not."""
    parts = [re.search(rf'^[ \t]*#[ \t]*define[ \t]+SDL_{k}_VERSION[ \t]+(\d+)', text, re.M)
             for k in ('MAJOR', 'MINOR', 'MICRO')]
    return tuple(int(m.group(1)) for m in parts) if all(parts) else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--dest', type=Path, default=ROOT / FOLDER, help='where SDL3 goes (default toolchain/sdl3)')
    src = ap.add_mutually_exclusive_group()
    src.add_argument('--deb', type=Path, nargs='+', help=f'the packages libsdl3-0 and libsdl3-dev {DEB_VERSION} on '
                                                         'disk instead of the download')
    src.add_argument('--sysroot', type=Path, help='the sniper SDK sysroot archive on disk instead of the download')
    a = ap.parse_args()
    folder = resolve(a.dest, a.deb, a.sysroot)
    n = sum(1 for _ in (folder / 'include' / 'SDL3').glob('*.h'))
    v = header_version((folder / 'include' / 'SDL3' / 'SDL_version.h').read_text(encoding='utf-8', errors='replace'))
    print(f'{folder}: SDL {".".join(map(str, v)) if v else "?"} ({DEB_VERSION}), {n} headers, lib/{LIB}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
