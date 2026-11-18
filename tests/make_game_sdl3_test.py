"""tools/make_game.py on Linux with or without the system's SDL3 (issue #11: Ubuntu 24.04 and Linux Mint 22 package
none):
- sdl3_choice takes the system's SDL3 when the compiler finds its headers, so a distribution that packages SDL3 builds
  with the same commands as before, else the build kit's (tools/sdl3_pin.py); NX2SARIA_SDL3 = kit or system decides
  instead, and a wrong value or a choice that cannot be met stops with the reason;
- the kit's SDL3 goes ahead of the system's folders for the runtime's sources only, the link names the kit's library
  by its path, and the check's program runs with the library beside it, as the game will;
- a program that does not start for want of a library the kit's SDL3 needs names it and the package that holds it;
- the kit's libSDL3.so.0 goes beside the game, and goes away again when the game is linked against the system's, but
  a libSDL3.so.0 this build did not put there stays, with a line that says so;
- the build log's line names the compiler, its version and where SDL3 came from.
No network, no game files.

Run: python tests/make_game_sdl3_test.py
"""
import contextlib
import io
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

import make_game as mg  # noqa: E402
import sdl3_pin  # noqa: E402

VERSION_H = '#define SDL_MAJOR_VERSION {}\n#define SDL_MINOR_VERSION {}\n#define SDL_MICRO_VERSION {}\n'


def stopped(fn, *args):
    """(the value, '') of fn(*args), or (None, the error text make_game.fail printed)."""
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        try:
            return fn(*args), ''
        except SystemExit:
            return None, err.getvalue()


@unittest.skipIf(os.name == 'nt', 'the SDL3 choice is the Linux build\'s')
class Sdl3ChoiceTest(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.tmp = Path(td.name)
        self.kit = self.tmp / 'kit'
        sdl = self.kit / sdl3_pin.FOLDER
        (sdl / 'include' / 'SDL3').mkdir(parents=True)
        (sdl / 'include' / 'SDL3' / 'SDL.h').write_text('#include <SDL3/SDL_version.h>\n')
        (sdl / 'include' / 'SDL3' / 'SDL_version.h').write_text(VERSION_H.format(3, 4, 18))
        (sdl / 'lib').mkdir()
        (sdl / 'lib' / sdl3_pin.LIB).write_bytes(b'\x7fELF the kit\'s library')
        (sdl / sdl3_pin.STAMP).write_text(sdl3_pin.DEB_VERSION + '\n')
        self.sdl = sdl
        self.system = ''   # what sdl3_version finds without the kit's folder
        for p in (mock.patch.object(mg, 'KIT', self.kit), mock.patch.object(mg, 'EVENTS', False),
                  mock.patch.object(mg, 'sdl3_version', self.version),
                  mock.patch.dict(os.environ, {}, clear=False)):
            p.start()
            self.addCleanup(p.stop)
        for name in ('NX2SARIA_SDL3', 'NX2NAVI_SDL3'):
            os.environ.pop(name, None)

    def version(self, clang, env, inc=()):
        return '3.4.18' if inc else self.system

    def test_the_system_s_sdl3_when_the_compiler_finds_it_else_the_kit_s(self):
        self.system = '3.4.20'
        sdl, _ = stopped(mg.sdl3_choice, 'clang', {})
        self.assertEqual((sdl.source, sdl.version, sdl.include, sdl.lib), ('system', '3.4.20', [], '-lSDL3'))
        self.assertEqual(mg.link_libs(sdl), mg.LINK_LIBS)   # the link of before, so nothing links again
        self.system = ''
        sdl, _ = stopped(mg.sdl3_choice, 'clang', {})
        self.assertEqual(sdl.source, 'kit')
        self.assertEqual(sdl.version, '3.4.18')
        self.assertEqual(sdl.include, ['-I', str(self.sdl / 'include')])
        self.assertEqual(sdl.lib, str(self.sdl / 'lib' / sdl3_pin.LIB))
        libs = mg.link_libs(sdl)
        self.assertEqual(libs, [sdl.lib if x == '-lSDL3' else x for x in mg.LINK_LIBS])
        self.assertNotIn('-lSDL3', libs)
        self.assertIn('no SDL3 headers of the system', sdl.why)
        line = mg.sdl3_line(Path('/usr/bin/clang'), 'Ubuntu clang version 18.1.3 (1ubuntu1)', sdl)
        self.assertEqual(line, f'the compiler is Ubuntu clang version 18.1.3 (1ubuntu1) (/usr/bin/clang); SDL3 3.4.18 '
                               f'from the kit ({self.sdl}: the compiler finds no SDL3 headers of the system\'s)')

    def test_the_setting_decides_and_a_choice_that_cannot_be_met_stops(self):
        self.system = '3.4.20'
        os.environ['NX2SARIA_SDL3'] = 'kit'
        sdl, _ = stopped(mg.sdl3_choice, 'clang', {})
        self.assertEqual((sdl.source, sdl.why), ('kit', f'{self.sdl}: NX2SARIA_SDL3=kit'))
        os.environ['NX2SARIA_SDL3'] = 'System'
        sdl, _ = stopped(mg.sdl3_choice, 'clang', {})
        self.assertEqual((sdl.source, sdl.why), ('system', 'NX2SARIA_SDL3=system'))
        self.system = ''
        sdl, said = stopped(mg.sdl3_choice, 'clang', {})
        self.assertIsNone(sdl)
        self.assertIn('NX2SARIA_SDL3=system, but the compiler finds no SDL3', said)
        os.environ['NX2SARIA_SDL3'] = 'bundled'
        sdl, said = stopped(mg.sdl3_choice, 'clang', {})
        self.assertIsNone(sdl)
        self.assertIn('NX2SARIA_SDL3 is "bundled"', said)
        del os.environ['NX2SARIA_SDL3']
        os.environ['NX2NAVI_SDL3'] = 'kit'   # the older spelling is read too (make_game.py setting_env)
        self.system = '3.4.20'
        self.assertEqual(stopped(mg.sdl3_choice, 'clang', {})[0].source, 'kit')

    def test_a_kit_without_its_sdl3_fetches_it_or_says_why_not(self):
        shutil.rmtree(self.sdl)
        with mock.patch.object(sdl3_pin, 'resolve', side_effect=SystemExit('sdl3_pin: fetching it failed (offline)')):
            sdl, said = stopped(mg.sdl3_choice, 'clang', {})
        self.assertIsNone(sdl)
        self.assertIn('sdl3_pin: fetching it failed (offline)', said)
        with mock.patch.object(sdl3_pin, 'resolve', return_value=self.tmp / 'fetched') as fetch:
            sdl, _ = stopped(mg.sdl3_choice, 'clang', {})
        fetch.assert_called_once_with(self.kit / sdl3_pin.FOLDER)
        self.assertEqual(sdl.lib, str(self.tmp / 'fetched' / 'lib' / sdl3_pin.LIB))

    def test_a_read_only_kit_without_its_sdl3_says_so_and_fetches_nothing(self):
        shutil.rmtree(self.sdl)
        self.kit.chmod(0o555)
        self.addCleanup(self.kit.chmod, 0o755)
        with mock.patch.object(sdl3_pin, 'resolve', side_effect=AssertionError('nothing is fetched')):
            sdl, said = stopped(mg.sdl3_choice, 'clang', {})
        self.assertIsNone(sdl)
        self.assertIn('its folder is read-only', said)

    def test_the_link_key_and_the_compile_key(self):
        kit = mg.Sdl3('kit', '3.4.18', ['-I', str(self.sdl / 'include')], str(self.sdl / 'lib' / sdl3_pin.LIB), 'x')
        keyed = mg.link_key_libs(kit)
        sha = mg.sha256_file(self.sdl / 'lib' / sdl3_pin.LIB)
        # the kit by its place in the kit, not by the folder an AppImage is mounted in this time
        self.assertEqual(keyed, [f'<kit>/toolchain/sdl3/lib/libSDL3.so.0@{sha}' if x == kit.lib else x
                                 for x in mg.link_libs(kit)])
        self.assertNotIn(str(self.kit), ' '.join(keyed))
        system = mg.Sdl3('system', '3.4.20', [], '-lSDL3', 'y')
        self.assertEqual(mg.link_key_libs(system), mg.LINK_LIBS)
        self.assertEqual(mg.link_key_libs(None), mg.LINK_LIBS)
        self.assertEqual(mg.sdl3_tag(), '')   # unset: the compile key of every game built so far
        os.environ['NX2SARIA_SDL3'] = 'Kit'
        self.assertEqual(mg.sdl3_tag(), '|sdl3 kit')

    def test_windows_keeps_msys2_s(self):
        with mock.patch.object(mg, 'WIN', True):
            self.assertIsNone(mg.sdl3_choice('clang', {}))
        self.assertEqual(mg.link_libs(None), mg.LINK_LIBS)
        self.assertEqual(mg.sdl3_line('clang.exe', 'clang version 23.1.5', None),
                         'the compiler is clang version 23.1.5 (clang.exe)')

    def test_the_kit_s_library_goes_beside_the_game_and_away_again(self):
        out, cache = self.tmp / 'game', self.tmp / 'cache'
        out.mkdir()
        cache.mkdir()
        kit = mg.Sdl3('kit', '3.4.18', ['-I', str(self.sdl / 'include')], str(self.sdl / 'lib' / sdl3_pin.LIB), 'x')
        system = mg.Sdl3('system', '3.4.20', [], '-lSDL3', 'y')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(mg.place_sdl3(out, cache, system), 'SDL3 3.4.20 from the system')
            self.assertEqual(mg.place_sdl3(out, cache, kit), f'SDL3 3.4.18 from the kit: libSDL3.so.0 beside {mg.EXE_NAME}')
            beside = out / mg.SDL3_SO
            self.assertEqual(beside.read_bytes(), b'\x7fELF the kit\'s library')
            self.assertEqual(mg.place_sdl3(out, cache, kit), f'SDL3 3.4.18 from the kit: libSDL3.so.0 beside {mg.EXE_NAME}')
            line = mg.place_sdl3(out, cache, system)   # the runpath would load the kit's copy ahead of the system's
            self.assertIn('the kit\'s libSDL3.so.0 an earlier build put beside', line)
            self.assertFalse(beside.exists())
            # the build cache deleted since: a copy with the kit's bytes is still the kit's
            mg.place_sdl3(out, cache, kit)
            (cache / mg.SDL3_PLACED).unlink()
            self.assertIn('an earlier build put beside', mg.place_sdl3(out, cache, system))
            self.assertFalse(beside.exists())
            beside.write_bytes(b'one the player put there')
            line = mg.place_sdl3(out, cache, system)
            self.assertIn('which this build did not put there, is the one the game loads', line)
            self.assertEqual(beside.read_bytes(), b'one the player put there')
            self.assertEqual(mg.place_sdl3(out, cache, None), '')

    def test_a_library_the_kit_s_sdl3_needs_is_named_with_its_package(self):
        err = ('./probe: error while loading shared libraries: libdecor-0.so.0: cannot open shared object file: No such '
               'file or directory\n')
        self.assertEqual(mg.missing_library(err), 'libdecor-0.so.0')
        self.assertEqual(mg.missing_library('Segmentation fault'), '')
        self.assertTrue(set(mg.SDL3_NEEDS) >= {'libdecor-0.so.0', 'libXss.so.1', 'libpulse.so.0', 'libasound.so.2'})


@unittest.skipIf(os.name == 'nt', 'the check is the Linux build\'s')
class PreflightWithTheKitSdl3Test(unittest.TestCase):
    """make_game.preflight with the kit's SDL3, every command recorded instead of run."""

    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.tmp = Path(td.name)
        self.lib = self.tmp / 'sdl3' / 'lib' / mg.SDL3_SO
        self.lib.parent.mkdir(parents=True)
        self.lib.write_bytes(b'the kit\'s library')
        self.sdl = mg.Sdl3('kit', '3.4.18', ['-I', str(self.tmp / 'sdl3' / 'include')], str(self.lib), 'no SDL3 here')
        self.src = self.tmp / 'a.c'
        self.src.write_text('int a;\n')

    def check(self, run_result, link_result=(0, '', ''), work=None):
        ran, beside = [], []

        def run(cmd, **kw):
            cmd = [str(c) for c in cmd]
            ran.append(cmd)
            if len(cmd) == 1:   # the check's program
                beside.append((Path(cmd[0]).parent / mg.SDL3_SO).is_file())
                return subprocess.CompletedProcess(cmd, *run_result)
            if '-fsyntax-only' not in cmd:   # its link
                return subprocess.CompletedProcess(cmd, *link_result)
            return subprocess.CompletedProcess(cmd, 0, '', '')
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.multiple(mg, toolchain=lambda msys: (Path('clang'), Path('clang++'), {}, Path('vma')),
                                 clang_version=lambda clang, env: 'clang version 18.1.3',
                                 vulkan_headers=lambda clang, env: ['-I', 'kit/vulkan'],
                                 sdl3_choice=lambda clang, env: self.sdl, runtime_headers=lambda rt: None,
                                 EVENTS=False), \
                mock.patch.object(mg.subprocess, 'run', run), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                mg.preflight('', 1, [self.src], work=work)
                failed = False
            except SystemExit:
                failed = True
        return failed, ran, beside, out.getvalue(), err.getvalue()

    def test_the_kit_s_sdl3_is_compiled_linked_and_run_against(self):
        work = self.tmp / 'build-cache' / 'toolchain-check'
        failed, ran, beside, out, _ = self.check((0, '3.4.18\n', ''), work=work)
        self.assertFalse(failed)
        compile_, link, probe = ran
        self.assertEqual(Path(probe[0]).parent.parent, work)   # built and run in the build cache, not in /tmp
        self.assertLess(compile_.index('kit/vulkan'), compile_.index(self.sdl.include[1]))
        self.assertLess(compile_.index(self.sdl.include[1]), compile_.index(str(mg.KIT / 'runtime')))
        self.assertEqual(link[-len(mg.LINK_LIBS):], mg.link_libs(self.sdl))
        self.assertIn(str(self.lib), link)
        self.assertIn(self.sdl.include[1], link)
        self.assertIn('-Wl,-rpath,$ORIGIN', link)
        self.assertEqual(beside, [True])   # the program finds the kit's library where the game will
        self.assertIn('check: the compiler is clang version 18.1.3 (clang); SDL3 3.4.18 from the kit (no SDL3 here)',
                      out)
        self.assertIn('check: 1 runtime files and the libraries pass, and SDL3 3.4.18 loads', out)

    def test_a_program_that_does_not_start_names_what_is_missing(self):
        failed, _, _, _, err = self.check((127, '', './probe: error while loading shared libraries: libdecor-0.so.0: '
                                                    'cannot open shared object file: No such file or directory\n'))
        self.assertTrue(failed)
        self.assertIn('libdecor-0.so.0 is missing', err)
        self.assertIn('sudo apt install libdecor-0-0', err)
        self.sdl = mg.Sdl3('system', '3.4.18', [], '-lSDL3', 'its headers are found')   # headers without a library
        failed, _, beside, _, err = self.check((127, '', './probe: error while loading shared libraries: libSDL3.so.0: '
                                                         'cannot open shared object file: No such file or directory\n'))
        self.assertTrue(failed)
        self.assertEqual(beside, [False])
        self.assertIn('libSDL3.so.0 is missing. Install the package that holds it, or build with the kit\'s SDL3: '
                      'NX2SARIA_SDL3=kit', err)
        failed, _, _, _, err = self.check((-11, '', ''))   # ended by a signal: nothing on its stderr
        self.assertTrue(failed)
        self.assertIn('does not start here (it was ended by signal 11)', err)
        failed, _, _, _, err = self.check((3, '', 'something went wrong\n'))
        self.assertIn('does not start here (exit code 3; its lines above say why)', err)

    def test_headers_without_a_library_point_to_the_kit_s(self):
        self.sdl = mg.Sdl3('system', '3.4.18', [], '-lSDL3', 'its headers are found')
        failed, ran, _, _, err = self.check((0, '', ''), link_result=(1, '', 'ld.lld: error: unable to find library '
                                                                             '-lSDL3\n'))
        self.assertTrue(failed)
        self.assertEqual(len(ran), 2)   # no program to run without its link
        self.assertIn('the linker no libSDL3: install SDL3\'s development package, or build with the kit\'s SDL3: '
                      'NX2SARIA_SDL3=kit', err)
        failed, _, _, _, err = self.check((0, '', ''), link_result=(1, '', 'ld.lld: error: unable to find library -lvpx\n'))
        self.assertNotIn('NX2SARIA_SDL3', err)


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('clang'), 'needs Linux and clang')
class Sdl3VersionTest(unittest.TestCase):
    def test_reads_the_version_of_the_headers_the_compiler_finds(self):
        with tempfile.TemporaryDirectory() as td:
            inc = Path(td) / 'include'
            (inc / 'SDL3').mkdir(parents=True)
            (inc / 'SDL3' / 'SDL_version.h').write_text(VERSION_H.format(3, 2, 10))
            env = dict(os.environ)
            self.assertEqual(mg.sdl3_version(shutil.which('clang'), env, ['-I', str(inc)]), '3.2.10')
            (inc / 'SDL3' / 'SDL_version.h').write_text('/* no version */\n')
            self.assertEqual(mg.sdl3_version(shutil.which('clang'), env, ['-I', str(inc)]), '?')
            self.assertEqual(mg.sdl3_version(Path(td) / 'no-such-clang', env), '')

    def test_the_check_s_program_compiles(self):
        """make_game.SDL3_PROBE against headers that declare what it uses as SDL3/SDL.h does."""
        with tempfile.TemporaryDirectory() as td:
            inc = Path(td) / 'include'
            (inc / 'SDL3').mkdir(parents=True)
            (inc / 'SDL3' / 'SDL.h').write_text(
                'int SDL_GetVersion(void);\n#define SDL_VERSIONNUM_MAJOR(v) ((v) / 1000000)\n'
                '#define SDL_VERSIONNUM_MINOR(v) (((v) / 1000) % 1000)\n#define SDL_VERSIONNUM_MICRO(v) ((v) % 1000)\n')
            p = subprocess.run([shutil.which('clang'), '-fsyntax-only', '-Wall', '-Werror', '-I', str(inc), '-x', 'c',
                                '-'], input=mg.SDL3_PROBE, capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
