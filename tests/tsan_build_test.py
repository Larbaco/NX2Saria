"""Check the bench sanitizer compile and cache boundaries without game data."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import make_game as mg


class TsanBuildTest(unittest.TestCase):
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
