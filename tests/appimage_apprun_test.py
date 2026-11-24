import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import make_appimage  # noqa: E402
import sdl3_pin  # noqa: E402

HEADERS = ('opus/opus.h', 'vpx/vpx_decoder.h', 'dav1d/dav1d.h', 'zstd.h')
TOOLS = ('dirname', 'readlink', 'basename', 'cat', 'awk', 'nproc', 'chmod', 'mkdir', 'rm', 'mv', 'head')
CLANG = '''#!/bin/bash
[ "$*" = "-E -x c - -o /dev/null" ] || { echo "clang stand-in: unexpected arguments: $*" >&2; exit 2; }
read -r line; h=${line#'#include <'}; h=${h%'>'}
IFS=: read -ra dirs <<< "${CPATH:-}"
for d in "${dirs[@]}"; do [ -n "$d" ] && [ -e "$d/$h" ] && exit 0; done
echo "fatal error: '$h' file not found" >&2; exit 1
'''
PYTHON = '''#!/bin/bash
printf '%s\\n' "$*" >> "$PY_LOG"
exit 0
'''


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('bash'), 'AppRun is a bash script for Linux')
class AppRunTest(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.tmp = Path(td.name)
        app = self.tmp / 'NX2Saria.AppDir'
        (app / 'usr' / 'kit').mkdir(parents=True)
        (app / 'usr' / 'kit' / 'VERSION').write_text('0.19.0\n')
        (app / 'usr' / 'python' / 'bin').mkdir(parents=True)
        self.write(app / 'usr' / 'python' / 'bin' / 'python3', PYTHON)
        self.write(app / 'AppRun', make_appimage.APPRUN)
        self.apprun = app / 'AppRun'
        self.bin = self.tmp / 'bin'
        self.bin.mkdir()
        for tool in TOOLS:
            found = shutil.which(tool)
            if found:
                (self.bin / tool).symlink_to(found)
        self.cc = self.tmp / 'cc'
        self.cc.mkdir()
        self.write(self.cc / 'clang', CLANG)
        for tool in ('clang++', 'ld.lld'):
            self.write(self.cc / tool, '#!/bin/bash\nexit 0\n')
        self.inc = self.tmp / 'include'
        self.nup = self.tmp / 'game.nup'
        self.nup.write_bytes(b'a dump')
        self.log = self.tmp / 'python.log'

    def write(self, path, text):
        path.write_text(text, encoding='utf-8')
        path.chmod(0o755)

    def headers(self, names):
        for h in names:
            (self.inc / h).parent.mkdir(parents=True, exist_ok=True)
            (self.inc / h).write_text('/* stand-in */\n')

    def run_apprun(self, *args, compiler=True):
        env = {'PATH': os.pathsep.join([*([str(self.cc)] if compiler else []), str(self.bin)]),
               'HOME': str(self.tmp / 'home'), 'XDG_DATA_HOME': str(self.tmp / 'data'), 'CPATH': str(self.inc),
               'PY_LOG': str(self.log), 'NX2SARIA_NO_DLSS': '1'}
        p = subprocess.run([shutil.which('bash'), str(self.apprun), *map(str, args)], capture_output=True, text=True,
                           env=env, timeout=60)
        ran = self.log.read_text().splitlines() if self.log.is_file() else []
        return p.returncode, p.stdout, p.stderr, ran

    def test_check_reads_the_dump_without_a_compiler(self):
        rc, out, err, ran = self.run_apprun('--check', self.nup, compiler=False)
        self.assertEqual(rc, 0, err)
        self.assertNotIn('Missing:', err)
        self.assertTrue(any(f'--nup {self.nup} --check' in r for r in ran), ran)
        rc, out, err, ran = self.run_apprun('--make-game', '--help', compiler=False)
        self.assertEqual(rc, 0, err)
        self.assertNotIn('Missing:', err)

    def test_a_build_without_the_compiler_names_it_and_the_packages(self):
        rc, out, err, ran = self.run_apprun(self.nup, self.tmp / 'game', compiler=False)
        self.assertEqual(rc, 1)
        self.assertIn('Missing: clang clang++ ld.lld\n', err)
        self.assertEqual(ran, [])

    def test_the_headers_are_asked_of_the_compiler_and_sdl3_is_not_asked_for(self):
        self.headers(HEADERS[:1] + HEADERS[2:])
        rc, out, err, ran = self.run_apprun(self.nup, self.tmp / 'game')
        self.assertEqual(rc, 1)
        self.assertIn('Missing: vpx/vpx_decoder.h\n', err)
        self.assertNotIn('SDL3', err.split('Install')[0])
        apt = [line.strip() for line in err.splitlines() if line.strip().startswith('sudo apt install')]
        self.assertEqual(apt, ['sudo apt install clang lld libopus-dev libvpx-dev libdav1d-dev libzstd-dev '
                               'libvulkan-dev'])
        self.assertIn('Debian, Ubuntu, Linux Mint', err)
        self.assertEqual(ran, [])

    def test_with_everything_there_it_says_so_and_builds(self):
        self.headers(HEADERS)
        out_dir = self.tmp / 'game'
        out_dir.mkdir()
        rc, out, err, ran = self.run_apprun(self.nup, out_dir)
        self.assertEqual(rc, 0, err)
        self.assertIn('check: clang, clang++, ld.lld and the headers of Opus, libvpx, dav1d and zstd are found', out)
        build = [r for r in ran if f'--nup {self.nup} --out {out_dir}' in r]
        self.assertNotIn('--jobs', build[0] if build else '')
        self.assertEqual(len(build), 1, ran)
        self.assertTrue((out_dir / 'play.sh').is_file())

    def test_the_window_gets_its_error_line(self):
        rc, out, err, ran = self.run_apprun('--events', self.nup, self.tmp / 'game')
        self.assertEqual(rc, 1)
        self.assertIn('@@{"ev":"error","step":"deps","message":"missing packages"}', out)
        self.assertTrue(err.startswith('Missing: '))


class CarrySdl3Test(unittest.TestCase):
    def test_the_kit_carries_the_pinned_sdl3(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / 'checkout-sdl3'
            (src / 'include' / 'SDL3').mkdir(parents=True)
            (src / 'include' / 'SDL3' / 'SDL.h').write_text('/* SDL */\n')
            (src / 'lib').mkdir()
            (src / 'lib' / sdl3_pin.LIB).write_bytes(b'the library')
            (src / sdl3_pin.NOTICE).write_text('License: Zlib\n')
            (src / sdl3_pin.STAMP).write_text(sdl3_pin.DEB_VERSION + '\n')
            pinned = sdl3_pin.content_digest({f.relative_to(src): f.read_bytes() for f in src.rglob('*')
                                              if f.is_file() and f.name != sdl3_pin.STAMP})
            kit = Path(td) / 'kit'
            with mock.patch.object(sdl3_pin, 'resolve', return_value=src) as resolve, \
                    mock.patch.object(sdl3_pin, 'CONTENT_SHA256', pinned), mock.patch('builtins.print'):
                dst = make_appimage.carry_sdl3(kit)
            resolve.assert_called_once_with(make_appimage.ROOT / sdl3_pin.FOLDER)
            self.assertEqual(dst, kit / 'toolchain' / 'sdl3')
            self.assertTrue(sdl3_pin.have(dst))
            self.assertEqual((dst / 'lib' / sdl3_pin.LIB).read_bytes(), b'the library')
            self.assertTrue((dst / sdl3_pin.NOTICE).is_file())
            (src / 'lib' / sdl3_pin.LIB).write_bytes(b'another library')
            with mock.patch.object(sdl3_pin, 'resolve', return_value=src), \
                    mock.patch.object(sdl3_pin, 'CONTENT_SHA256', pinned), mock.patch('builtins.print'):
                with self.assertRaises(SystemExit):
                    make_appimage.carry_sdl3(Path(td) / 'kit2')
            (src / 'lib' / sdl3_pin.LIB).write_bytes(b'the library')
            (src / sdl3_pin.STAMP).unlink()
            with mock.patch.object(sdl3_pin, 'resolve', return_value=src), \
                    mock.patch.object(sdl3_pin, 'CONTENT_SHA256', pinned), mock.patch('builtins.print'):
                with self.assertRaises(SystemExit):
                    make_appimage.carry_sdl3(Path(td) / 'kit3')

    def test_apprun_no_longer_names_libsdl3_dev(self):
        self.assertNotIn('libsdl3-dev', make_appimage.APPRUN)
        self.assertNotIn('SDL3/SDL.h', make_appimage.APPRUN)
        self.assertNotIn('/usr/include/$h', make_appimage.APPRUN)


if __name__ == '__main__':
    unittest.main(verbosity=2)
