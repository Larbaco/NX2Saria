import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import make_game as mg


class TsanBuildTest(unittest.TestCase):
    def test_check_does_not_prepare_output(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp) / 'normal'
            for mode in ('0', '1'):
                with self.subTest(mode=mode), mock.patch.object(mg, 'WIN', False), \
                     mock.patch.dict(os.environ, {'NX2SARIA_BENCH_TSAN': mode}), \
                     mock.patch.object(sys, 'argv', ['make_game.py', '--nup', 'game.nup', '--out', str(out), '--check']), \
                     mock.patch.object(mg, 'Source'), mock.patch.object(mg, 'check'), \
                     mock.patch.object(mg, 'prepare_tsan_output') as prepare:
                    mg.build_main()
                    prepare.assert_not_called()
                    self.assertFalse(out.exists())

    def test_prebuilt_runtime_is_never_used_for_tsan(self):
        with mock.patch.object(mg, 'prebuilt_runtime', return_value=('lib', {'rt_memory.o'})) as prebuilt:
            self.assertIsNone(mg.select_prebuilt_runtime('clang', {}, False, True))
            self.assertIsNone(mg.select_prebuilt_runtime('clang', {}, True, False))
            prebuilt.assert_not_called()
            self.assertEqual(mg.select_prebuilt_runtime('clang', {}, False, False), ('lib', {'rt_memory.o'}))
            prebuilt.assert_called_once()

    def test_legacy_adoption_excludes_instrumented_host(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            src, obj = path / 'memory.c', path / 'rt_memory.o'
            src.write_text('int x;')
            obj.write_bytes(b'object')
            cache = mg.ObjectCache(path / 'objects.json')
            cache.record(obj, 'old', path / 'missing.d', src)
            legacy = ['old'] if mg.allow_legacy_object(True, obj) else []
            self.assertFalse(cache.adopt(obj, 'instrumented', legacy))
            self.assertTrue(cache.current(obj, 'old'))
            self.assertTrue(mg.allow_legacy_object(False, obj))
            self.assertTrue(mg.allow_legacy_object(True, Path('main_chunk.o')))
            self.assertTrue(cache.adopt(obj, 'normal', ['old']))

    def test_output_marker_separates_modes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            wrong = root / 'normal'
            with self.assertRaisesRegex(ValueError, 'separate g25-tsan'):
                mg.prepare_tsan_output(wrong, True)
            self.assertFalse(wrong.exists())
            out = root / 'g25-tsan'
            out.mkdir()
            (out / mg.EXE_NAME).touch()
            with self.assertRaisesRegex(ValueError, 'ordinary build'):
                mg.prepare_tsan_output(out, True)
            self.assertFalse((out / '.bench-tsan').exists())
            (out / mg.EXE_NAME).unlink()
            mg.prepare_tsan_output(out, True)
            self.assertTrue((out / '.bench-tsan').is_file())
            (out / mg.EXE_NAME).touch()
            mg.prepare_tsan_output(out, True)
            with self.assertRaisesRegex(ValueError, 'belongs to the bench'):
                mg.prepare_tsan_output(out, False)
            mg.prepare_tsan_output(wrong, False)
            self.assertFalse(wrong.exists())

    def test_host_object_keys_do_not_cross_modes(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            src, obj = path / 'memory.c', path / 'memory.o'
            src.write_text('int x;')
            obj.write_bytes(b'object')
            cache = mg.ObjectCache(path / 'objects.json')
            normal = mg.runtime_cmd('clang', 'clang++', [])
            tsan = mg.runtime_cmd('clang', 'clang++', [], tsan=True)
            keys = [mg.object_key(cmd(src, obj), src, obj, cache, 'compiler') for cmd in (normal, tsan)]
            self.assertNotEqual(*keys)
            for stored, other in (keys, keys[::-1]):
                cache.record(obj, stored, path / 'missing.d', src)
                self.assertTrue(cache.current(obj, stored))
                self.assertFalse(cache.current(obj, other))

    def test_flags_only_for_host(self):
        normal = mg.runtime_cmd('clang', 'clang++', [])
        tsan = mg.runtime_cmd('clang', 'clang++', [], tsan=True)
        for name in ('main_1.o', 'sdk_0.o', 'subsdk0_3.o'):
            self.assertEqual(normal(Path('lift.c'), Path(name)), tsan(Path('lift.c'), Path(name)))
        for name in ('memory.o', 'modules.o', 'vk_memory.o'):
            self.assertNotIn('-fsanitize=thread', normal(Path('host.c'), Path(name)))
            self.assertIn('-fsanitize=thread', tsan(Path('host.c'), Path(name)))

    def test_compile_step_key_separates_modes(self):
        with mock.patch.multiple(mg, exefs_digest=lambda _: 'exefs', lift_digest=lambda: 'lift',
                                 mod_digest=lambda _: '', tree_digest=lambda *_: 'runtime',
                                 sha256_file=lambda _: 'script', kit_version=lambda: 'kit',
                                 cpu_tag=lambda: 'cpu', pgo_tag=lambda: '', sdl3_tag=lambda: ''):
            with mock.patch.dict(os.environ, {'NX2SARIA_BENCH_TSAN': '1'}):
                instrumented = mg.step_key('compile', 'id', Path('.'))
            with mock.patch.dict(os.environ, {'NX2SARIA_BENCH_TSAN': '0'}):
                normal = mg.step_key('compile', 'id', Path('.'))
            self.assertNotEqual(instrumented, normal)


if __name__ == '__main__':
    unittest.main()
