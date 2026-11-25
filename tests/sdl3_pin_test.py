import contextlib
import hashlib
import io
import sys
import tarfile
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import sdl3_pin  # noqa: E402

VERSION_H = b'#define SDL_MAJOR_VERSION   3\n#define SDL_MINOR_VERSION   4\n#define SDL_MICRO_VERSION   18\n'


def ar(members):
    out = io.BytesIO()
    out.write(b'!<arch>\n')
    for name, body in members:
        head = f'{name + "/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(body):<10}`\n'.encode('ascii')
        out.write(head + body + (b'\n' if len(body) & 1 else b''))
    return out.getvalue()


def tar_bytes(files, links=(), hard=(), mode='w:xz'):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode=mode) as tar:
        for name, body in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(body)
            tar.addfile(info, io.BytesIO(body))
        for kind, pairs in ((tarfile.SYMTYPE, links), (tarfile.LNKTYPE, hard)):
            for name, target in pairs:
                info = tarfile.TarInfo(name)
                info.type, info.linkname = kind, target
                tar.addfile(info)
    return buf.getvalue()


def deb(files, links=()):
    return ar([('debian-binary', b'2.0\n'), ('control.tar.xz', tar_bytes({})), ('data.tar.xz', tar_bytes(files, links))])


LIB_DIR = './usr/lib/x86_64-linux-gnu/'
RUNTIME = {LIB_DIR + 'libSDL3.so.0.4.18': b'\x7fELF the library',
           './usr/share/doc/libsdl3-0/copyright': b'Upstream-Name: SDL\nLicense: Zlib\n',
           './usr/share/doc/libsdl3-0/changelog.Debian.gz': b'not kept',
           './usr/include/SDL3/../../../etc/evil.h': b'never written'}
RUNTIME_LINKS = [(LIB_DIR + 'libSDL3.so.0', 'libSDL3.so.0.4.18')]
DEV = {'./usr/include/SDL3/SDL.h': b'#include <SDL3/SDL_version.h>\n', './usr/include/SDL3/SDL_version.h': VERSION_H,
       LIB_DIR + 'libSDL3.a': b'not kept', LIB_DIR + 'cmake/SDL3/SDL3Config.cmake': b'not kept',
       LIB_DIR + 'pkgconfig/sdl3.pc': b'not kept'}
DEV_LINKS = [(LIB_DIR + 'libSDL3.so', 'libSDL3.so.0')]
KEPT = ['SDL3-copyright.txt', 'include/SDL3/SDL.h', 'include/SDL3/SDL_version.h', 'lib/libSDL3.so.0', sdl3_pin.STAMP]


def no_download(*a, **k):
    raise AssertionError('nothing is downloaded here')


class Sdl3PinTest(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.tmp = Path(td.name)
        self.runtime, self.dev = deb(RUNTIME, RUNTIME_LINKS), deb(DEV, DEV_LINKS)
        (self.tmp / 'a.deb').write_bytes(self.runtime)
        (self.tmp / 'b.deb').write_bytes(self.dev)
        self.urls = {'libsdl3-0': 'https://example.invalid/libsdl3-0.deb',
                     'libsdl3-dev': 'https://example.invalid/libsdl3-dev.deb'}
        pinned = {n: (self.urls[n], hashlib.sha256(d).hexdigest()) for n, d in (('libsdl3-0', self.runtime),
                                                                              ('libsdl3-dev', self.dev))}
        kept = {}
        sdl3_pin.deb_files(self.runtime, kept)
        sdl3_pin.deb_files(self.dev, kept)
        files = {k[2:]: v for k, v in {**RUNTIME, **DEV}.items() if 'libsdl3-0/copyright' not in k}
        files.update({'usr/share/doc/libsdl3-dev/copyright': RUNTIME['./usr/share/doc/libsdl3-0/copyright'],
                      'usr/lib/i386-linux-gnu/libSDL3.so.0.4.18': b'the 32-bit library, not kept',
                      'usr/bin/something': b'not kept'})
        self.sysroot = tar_bytes(files, hard=[('usr/share/doc/libsdl3-0/copyright',
                                               './usr/share/doc/libsdl3-dev/copyright')], mode='w:gz')
        for p in (mock.patch.object(sdl3_pin, 'DEBS', pinned),
                  mock.patch.object(sdl3_pin, 'CONTENT_SHA256', sdl3_pin.content_digest(kept)),
                  mock.patch.object(sdl3_pin, 'SYSROOT', ('https://example.invalid/sysroot.tar.gz',
                                                          hashlib.sha256(self.sysroot).hexdigest())),
                  mock.patch.object(sdl3_pin, 'SYSROOT_SIZE', len(self.sysroot)),
                  mock.patch.object(sdl3_pin.urllib.request, 'urlopen', no_download)):
            p.start()
            self.addCleanup(p.stop)

    def files(self, folder):
        return sorted(p.relative_to(folder).as_posix() for p in folder.rglob('*') if p.is_file())

    def resolve(self, *args, **kw):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            try:
                return sdl3_pin.resolve(*args, **kw), '', out.getvalue()
            except SystemExit as e:
                return None, str(e), out.getvalue()

    def urlopen(self, served):
        fetched = []

        def urlopen(url, timeout):
            fetched.append(url)
            if url not in served:
                raise urllib.error.HTTPError(url, 404, 'Not Found', {}, None)
            return io.BytesIO(served[url])
        return urlopen, fetched

    def test_keeps_the_headers_the_library_and_its_copyright_file_only(self):
        dest = self.tmp / 'sdl3'
        self.assertEqual(self.resolve(dest, [self.tmp / 'b.deb', self.tmp / 'a.deb'])[0], dest)
        self.assertEqual(self.files(dest), KEPT)
        lib = dest / 'lib' / sdl3_pin.LIB
        self.assertFalse(lib.is_symlink())
        self.assertEqual(lib.read_bytes(), b'\x7fELF the library')
        self.assertTrue(lib.stat().st_mode & 0o111)
        self.assertFalse((self.tmp / 'etc').exists())
        self.assertTrue(sdl3_pin.have(dest))
        self.assertEqual(sdl3_pin.header_version((dest / 'include' / 'SDL3' / 'SDL_version.h').read_text()), (3, 4, 18))
        self.assertEqual(self.resolve(dest)[0], dest)
        (dest / sdl3_pin.STAMP).unlink()
        self.assertFalse(sdl3_pin.have(dest))
        (dest / sdl3_pin.STAMP).write_bytes('3.4.18é'.encode())
        self.assertFalse(sdl3_pin.have(dest))

    def test_downloads_what_is_not_on_disk_and_checks_it(self):
        dest = self.tmp / 'sdl3'
        urlopen, fetched = self.urlopen({self.urls['libsdl3-dev']: self.dev})
        with mock.patch.object(sdl3_pin.urllib.request, 'urlopen', urlopen):
            self.assertEqual(self.resolve(dest, [self.tmp / 'a.deb'])[0], dest)
        self.assertEqual(fetched, [self.urls['libsdl3-dev']])
        self.assertTrue(sdl3_pin.have(dest))
        other = self.tmp / 'other'
        urlopen, _ = self.urlopen({u: b'something else' for u in self.urls.values()})
        with mock.patch.object(sdl3_pin.urllib.request, 'urlopen', urlopen):
            folder, said, _ = self.resolve(other)
        self.assertIsNone(folder)
        self.assertIn('SHA-256 differs', said)
        self.assertFalse(sdl3_pin.have(other))

    def test_takes_no_other_package(self):
        (self.tmp / 'c.deb').write_bytes(self.dev + b'\n')
        folder, said, _ = self.resolve(self.tmp / 'sdl3', [self.tmp / 'a.deb', self.tmp / 'c.deb'])
        self.assertIsNone(folder)
        self.assertIn('c.deb is neither pinned package', said)
        folder, said, _ = self.resolve(self.tmp / 'sdl3', [self.tmp / 'missing.deb'])
        self.assertIn('missing.deb cannot be read', said)
        self.assertFalse(sdl3_pin.have(self.tmp / 'sdl3'))

    def test_packages_without_what_they_hold_or_with_other_files_stop(self):
        changed = {**DEV, './usr/include/SDL3/SDL.h': b'#include <SDL3/SDL_version.h>\n/* another SDL.h */\n'}
        cases = [(deb({k: v for k, v in RUNTIME.items() if 'libSDL3.so' not in k}), self.dev, 'lacks lib/libSDL3.so.0'),
                 (self.runtime, deb({k: v for k, v in DEV.items() if 'SDL_version.h' not in k}),
                  'lacks include/SDL3/SDL_version.h'),
                 (self.runtime, deb(changed, DEV_LINKS), 'are not the pinned ones')]
        for runtime, dev, want in cases:
            dest = self.tmp / 'sdl3'
            with self.assertRaises(SystemExit) as stop:
                sdl3_pin.unpack({'libsdl3-0': runtime, 'libsdl3-dev': dev}, dest)
            self.assertIn(want, str(stop.exception))
            self.assertFalse(sdl3_pin.have(dest))

    def test_the_sysroot_takes_over_when_the_packages_are_gone(self):
        dest = self.tmp / 'sdl3'
        urlopen, fetched = self.urlopen({sdl3_pin.SYSROOT[0]: self.sysroot})
        with mock.patch.object(sdl3_pin.urllib.request, 'urlopen', urlopen):
            folder, said, out = self.resolve(dest)
        self.assertEqual((folder, said), (dest, ''))
        self.assertEqual(fetched, [self.urls['libsdl3-0'], sdl3_pin.SYSROOT[0]])
        self.assertIn('not in the Steam Runtime\'s pool', out)
        self.assertEqual(self.files(dest), KEPT)
        self.assertEqual((dest / 'lib' / sdl3_pin.LIB).read_bytes(), b'\x7fELF the library')
        self.assertEqual((dest / sdl3_pin.NOTICE).read_bytes(), RUNTIME['./usr/share/doc/libsdl3-0/copyright'])
        local = self.tmp / 'sysroot.tar.gz'
        local.write_bytes(self.sysroot)
        self.assertEqual(self.resolve(self.tmp / 'from-disk', sysroot=local)[0], self.tmp / 'from-disk')
        self.assertEqual(self.files(self.tmp / 'from-disk'), KEPT)
        other = self.tmp / 'other.tar.gz'
        other.write_bytes(self.sysroot[:-1] + bytes([self.sysroot[-1] ^ 1]))
        folder, said, _ = self.resolve(self.tmp / 'refused', sysroot=other)
        self.assertIsNone(folder)
        self.assertRegex(said, r'(SHA-256 differs|reading .* failed)')
        self.assertFalse((self.tmp / 'refused').exists())
        longer = self.sysroot + bytes(100)
        with mock.patch.object(sdl3_pin, 'SYSROOT', (sdl3_pin.SYSROOT[0], hashlib.sha256(longer).hexdigest())), \
                mock.patch.object(sdl3_pin, 'SYSROOT_SIZE', len(longer)):
            folder, said, _ = self.resolve(self.tmp / 'short', sysroot=local)
        self.assertIsNone(folder)
        self.assertIn(f'cut short: {len(self.sysroot)} of {len(self.sysroot) + 100} bytes', said)
        urlopen, _ = self.urlopen({})
        with mock.patch.object(sdl3_pin.urllib.request, 'urlopen', urlopen):
            folder, said, _ = self.resolve(self.tmp / 'offline')
        self.assertIsNone(folder)
        self.assertIn('--deb FILE FILE', said)
        for url, sha in [*sdl3_pin.DEBS.values(), sdl3_pin.SYSROOT]:
            self.assertIn(url, said)
            self.assertIn(sha, said)

    def test_replaces_only_what_an_earlier_unpack_wrote(self):
        dest = self.tmp / 'toolchain'
        (dest / 'vulkan-headers').mkdir(parents=True)
        (dest / 'vulkan-headers' / 'keep.h').write_text('kept')
        (dest / 'include' / 'other').mkdir(parents=True)
        (dest / 'include' / 'other' / 'keep.h').write_text('kept')
        (dest / 'include' / 'SDL3').mkdir()
        (dest / 'include' / 'SDL3' / 'SDL_old.h').write_text('an older unpack')
        self.resolve(dest, [self.tmp / 'a.deb', self.tmp / 'b.deb'])
        self.assertEqual(self.files(dest), sorted(KEPT + ['include/other/keep.h', 'vulkan-headers/keep.h']))

    def test_a_damaged_package_is_an_error_with_a_reason(self):
        good = deb(DEV, DEV_LINKS)
        odd = ar([('debian-binary', b'2.0\n'), ('control.tar.xz', b'xyz'), ('data.tar.xz', tar_bytes(DEV, DEV_LINKS))])
        a, b = {}, {}
        sdl3_pin.deb_files(odd, a)
        sdl3_pin.deb_files(good, b)
        self.assertEqual(a, b)
        negative = b'!<arch>\n' + f'{"data.tar.xz/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{-60:<10}`\n'.encode()
        cut = good[:-200]
        bad_magic = bytearray(good)
        bad_magic[8 + 58:8 + 60] = b'xx'
        for data in (b'PK\x03\x04 a zip file', ar([('debian-binary', b'2.0\n')]), negative, cut, bytes(bad_magic),
                     b'!<arch>\n' + b'\xff' * 60):
            with self.assertRaises(ValueError):
                sdl3_pin.deb_files(data, {})

    def test_the_pins(self):
        self.assertEqual(sdl3_pin.header_version(VERSION_H.decode()), tuple(map(int, sdl3_pin.VERSION.split('.'))))
        self.assertIsNone(sdl3_pin.header_version('#define SDL_MAJOR_VERSION 3\n'))


class RealPinsTest(unittest.TestCase):
    def test_the_real_pins(self):
        self.assertTrue(sdl3_pin.DEB_VERSION.startswith(sdl3_pin.VERSION + '+'))
        for name, (url, sha) in sdl3_pin.DEBS.items():
            self.assertTrue(url.startswith('https://') and f'/{name}_' in url and sdl3_pin.VERSION in url)
            self.assertRegex(sha, r'^[0-9a-f]{64}$')
        self.assertRegex(sdl3_pin.CONTENT_SHA256, r'^[0-9a-f]{64}$')
        self.assertTrue(sdl3_pin.SYSROOT[0].startswith('https://repo.steampowered.com/steamrt-images-sniper/'))
        self.assertRegex(sdl3_pin.SYSROOT[1], r'^[0-9a-f]{64}$')


if __name__ == '__main__':
    unittest.main(verbosity=2)
