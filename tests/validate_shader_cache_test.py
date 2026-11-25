import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import pack_shader_cache as psc  # noqa: E402
import validate_shader_cache as vsc  # noqa: E402

RES = b'{"stage":"cs","textures":[]}'


def name(seed):
    return hashlib.sha256(seed.encode()).hexdigest()


def insn(op, *operands):
    return struct.pack('<%dI' % (1 + len(operands)), (1 + len(operands)) << 16 | op, *operands)


def text(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return struct.unpack('<%dI' % (len(b) // 4), b)


def module(memory_model=True):
    body = insn(17, 1)
    if memory_model:
        body += insn(14, 0, 1)
    body += insn(15, 5, 1, *text('main'))
    body += insn(16, 1, 17, 1, 1, 1)
    body += insn(19, 2) + insn(33, 3, 2)
    body += insn(54, 2, 1, 0, 3) + insn(248, 4) + insn(253) + insn(56)
    return struct.pack('<5I', vsc.SPIRV_MAGIC, 0x00010300, 0, 5, 0) + body


def cut_module():
    return module() + struct.pack('<I', 3 << 16 | 0)


def put(cache, seed, spv=None, res=None):
    if spv is not None:
        (cache / (name(seed) + '.spv')).write_bytes(spv)
    if res is not None:
        (cache / (name(seed) + '.res.json')).write_bytes(res)
    return name(seed)


class ValidateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cache = Path(self.tmp.name) / 'shader-cache'
        self.cache.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def check(self, **kw):
        checked, failures = vsc.validate(self.cache, say=lambda s: None, **kw)
        return checked, {k: (o, p) for k, o, p in failures}

    def test_game_checks(self):
        good = module()
        ok = put(self.cache, 'ok', good, RES)
        cut = put(self.cache, 'cut', good[:-6], RES)
        short = put(self.cache, 'short', cut_module(), RES)
        zero = put(self.cache, 'zero', good[:40] + b'\0' * (len(good) - 40), RES)
        magic = put(self.cache, 'magic', b'\x01' + good[1:], RES)
        nul = put(self.cache, 'nul', good, b'\0' * len(RES))
        stage = put(self.cache, 'stage', good, b'{"stage":"xx"}')
        array = put(self.cache, 'array', good, b'[1]')
        no_res = put(self.cache, 'no-res', good, None)
        no_spv = put(self.cache, 'no-spv', None, RES)
        checked, bad = self.check()
        self.assertEqual(checked, 10)
        self.assertNotIn(ok, bad)
        self.assertIn('not a whole SPIR-V module', bad[cut][1])
        self.assertIn('past the end', bad[short][1])
        self.assertIn('zero words', bad[zero][1])
        self.assertIn('magic', bad[magic][1])
        self.assertIn('NUL', bad[nul][1])
        self.assertIn('no stage', bad[stage][1])
        self.assertIn('not one JSON object', bad[array][1])
        self.assertEqual(bad[no_res][1], 'no .res.json')
        self.assertEqual(bad[no_spv][1], 'no .spv')
        self.assertEqual(len(bad), 9)
        self.assertTrue(all(o == 'loose' for o, _ in bad.values()))

    def test_stages_and_spaces(self):
        for st in ('vs', 'ps', 'cs', 'ts', 'ms'):
            self.assertEqual(vsc.reflection_problem(b' \n{ "stage" : "%s" }\n' % st.encode()), '')
        self.assertNotEqual(vsc.reflection_problem(b'{"stage":"gs"}'), '')
        self.assertNotEqual(vsc.reflection_problem(b'{}'), '')
        self.assertEqual(vsc.spirv_problem(module()), '')

    def test_pack_and_loose(self):
        good = module()
        packed_ok = put(self.cache, 'packed-ok', good, RES)
        packed_bad = put(self.cache, 'packed-bad', cut_module(), RES)
        fixed = put(self.cache, 'fixed', cut_module(), RES)
        psc.pack(self.cache)
        self.assertFalse(list(self.cache.glob('*.spv')))
        put(self.cache, 'fixed', good, None)
        loose_bad = put(self.cache, 'loose-bad', good, b'{}')
        checked, bad = self.check()
        self.assertEqual(checked, 4)
        self.assertEqual(set(bad), {packed_bad, loose_bad})
        self.assertEqual(bad[packed_bad][0], 'pack')
        self.assertEqual(bad[loose_bad][0], 'loose')
        self.assertNotIn(fixed, bad)
        self.assertNotIn(packed_ok, bad)

    def test_every_and_out(self):
        for i in range(10):
            put(self.cache, 'n%d' % i, cut_module() if i % 2 else module(), RES)
        checked, bad = self.check()
        self.assertEqual((checked, len(bad)), (10, 5))
        checked, _ = self.check(every=3)
        self.assertEqual(checked, 4)
        out = Path(self.tmp.name) / 'failed.txt'
        self.check(out=out)
        lines = out.read_text(encoding='utf-8').splitlines()
        self.assertEqual(len(lines), 5)
        self.assertTrue(all(ln.split()[0] in bad and ln.split()[1] == 'loose' for ln in lines))

    def test_groups(self):
        a = vsc.group_of("spirv-val: error: line 12: ID '34[%34]' has not been defined")
        b = vsc.group_of("spirv-val: error: line 907: ID '5[%main]' has not been defined")
        self.assertEqual(a, b)

    @unittest.skipUnless(vsc.find_spirv_val(), 'spirv-val is not installed')
    def test_spirv_val(self):
        ok = put(self.cache, 'ok', module(), RES)
        invalid = put(self.cache, 'invalid', module(memory_model=False), RES)
        checked, bad = self.check(val=vsc.find_spirv_val())
        self.assertEqual(checked, 2)
        self.assertNotIn(ok, bad)
        self.assertTrue(bad[invalid][1].startswith('spirv-val: '), bad[invalid][1])
        _, without = self.check()
        self.assertEqual(without, {})


if __name__ == '__main__':
    unittest.main()
