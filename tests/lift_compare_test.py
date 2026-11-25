import io
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import lift_compare  # noqa: E402

FNS = {
    0x100: 'void f_100(Cpu *c) {\n    FN_ENTER(0x100);\n    c->x[30] = 0x108; f_200(c);\n    return;\n}',
    0x200: ('static void fb_200(Cpu *c);\nvoid f_200(Cpu *c) {\n    FN_ENTER(0x200);\n    c->pc = 0x200;\n'
            '    __attribute__((musttail)) return fb_200(c);\n}\nstatic void fb_200(Cpu *c) {\n'
            '    switch (c->pc) { case 0x200: goto L_200; default: break; }\nL_200:;\n'
            '    __attribute__((musttail)) return f_300(c);\n}'),
    0x300: 'void f_300(Cpu *c) {\n    FN_ENTER(0x300);\n    x = 1;\n    return;\n}',
    0x400: 'void f_400(Cpu *c) {\n    FN_ENTER(0x400);\n    c->x[30] = 0x408; f_100(c);\n    return;\n}',
}


def chunk(addrs, fns=FNS, protos=None):
    refs = protos if protos is not None else sorted({int(t[2:], 16) for a in addrs
                                                     for t in lift_compare.CALL.findall(fns[a])})
    return ('// chunk\n#include "cpu.h"\n#include "simd.h"\n\n' + ''.join(f'void f_{r:x}(Cpu *c);\n' for r in refs)
            + '\n' + '\n\n'.join(fns[a] for a in addrs) + '\n')


def table(addrs):
    return ('#include "cpu.h"\n\n' + ''.join(f'void f_{a:x}(Cpu *c);\n' for a in addrs)
            + '\nconst uint64_t mod_fn_addrs[] = {\n' + ',\n'.join(f'0x{a:x}ull' for a in addrs) + '\n};\n')


def tree(root, groups, fns=FNS, protos=None, listed=None):
    (root / 'mod').mkdir(parents=True)
    for name in ('layout.json', 'layout.h', 'patches.h'):
        (root / name).write_text(f'{name}\n')
    if listed is None:
        listed = sorted({a for g in groups for a in g})
    (root / 'mod' / 'mod_table.c').write_text(table(listed), newline='')
    for k, g in enumerate(groups):
        (root / 'mod' / f'mod_{k:04d}.c').write_text(chunk(g, fns, protos), newline='')
    return root


class LiftCompareTest(unittest.TestCase):
    def run_compare(self, old_groups, new_groups, new_fns=FNS, new_protos=None, edit=None):
        with tempfile.TemporaryDirectory() as d:
            old = tree(Path(d) / 'old', old_groups)
            new = tree(Path(d) / 'new', new_groups, new_fns, new_protos)
            if edit:
                edit(new)
            out = io.StringIO()
            return lift_compare.compare(old, new, out), out.getvalue()

    def test_whitespace_and_line_endings_differ(self):
        for old, new in (('    x = 1;\n', '    x = 1; \n'), ('    x = 1;\n', '    x = 1;\r\n'),
                         ('    x = 1;\n', '    x = 1;\n    \n'), ('    x = 1;\n', '    x = 1;\n\n')):
            fns = dict(FNS)
            fns[0x300] = fns[0x300].replace(old, new)
            n, out = self.run_compare([[0x300, 0x400]], [[0x300, 0x400]], fns)
            self.assertGreaterEqual(n, 1, (new, out))
            self.assertIn('f_300', out, new)

    def test_extra_blank_line_between_definitions_differs(self):
        def edit(lift):
            p = lift / 'mod' / 'mod_0000.c'
            p.write_text(p.read_text().replace('}\n\nvoid f_400', '}\n\n\nvoid f_400'), newline='')
        n, out = self.run_compare([[0x300, 0x400]], [[0x300, 0x400]], edit=edit)
        self.assertEqual(n, 1, out)
        self.assertIn('differing: f_300', out)

    def test_unexpected_preamble_line(self):
        def edit(lift):
            p = lift / 'mod' / 'mod_0001.c'
            p.write_text(p.read_text().replace('#include "simd.h"\n', '#include "simd.h"\nint stray;\n'), newline='')
        n, out = self.run_compare([[0x100, 0x200], [0x300, 0x400]], [[0x100, 0x200], [0x300, 0x400]], edit=edit)
        self.assertEqual(n, 1, out)
        self.assertIn("mod_0001.c:4: unexpected line before the first definition: 'int stray;\\n'", out)

    def test_parse_error_fallback_without_definition(self):
        fns = dict(FNS)
        fns[0x200] = fns[0x200].replace('static void fb_200(Cpu *c);\nvoid f_200', 'static void fb_200(Cpu *c);\n'
                                        '    x = 0;\nvoid f_200')
        n, out = self.run_compare([[0x100, 0x200]], [[0x100, 0x200]], fns)
        self.assertGreaterEqual(n, 1, out)
        self.assertIn('fallback declaration fb_200 not followed by f_200', out)

    def test_table_must_list_the_definitions(self):
        def edit(lift):
            (lift / 'mod' / 'mod_table.c').write_text(table([0x100, 0x200, 0x300, 0x500]), newline='')
        n, out = self.run_compare([[0x100, 0x200, 0x300, 0x400]], [[0x100, 0x200], [0x300, 0x400]], edit=edit)
        self.assertGreaterEqual(n, 3, out)
        self.assertIn('1 functions in the table but not defined: f_500', out)
        self.assertIn('1 functions defined but not in the table: f_400', out)

    def test_module_without_table(self):
        n, out = self.run_compare([[0x100, 0x200]], [[0x100, 0x200]],
                                  edit=lambda lift: (lift / 'mod' / 'mod_table.c').unlink())
        self.assertGreaterEqual(n, 2, out)
        self.assertIn('mod_table.c: missing', out)

    def test_five_digit_chunk_numbers(self):
        def edit(lift):
            (lift / 'mod' / 'mod_0001.c').rename(lift / 'mod' / 'mod_10000.c')
        n, out = self.run_compare([[0x100, 0x200], [0x300, 0x400]], [[0x100, 0x200], [0x300, 0x400]], edit=edit)
        self.assertEqual(n, 0, out)
        self.assertIn('chunks 2 -> 2', out)

    def test_regrouped_is_same(self):
        n, out = self.run_compare([[0x100, 0x200], [0x300, 0x400]], [[0x100], [0x200, 0x300, 0x400]])
        self.assertEqual(n, 0, out)
        self.assertIn('4 functions, 4 identical', out)
        self.assertIn('chunks 2 -> 2', out)

    def test_changed_byte_differs(self):
        fns = dict(FNS)
        fns[0x300] = fns[0x300].replace('x = 1', 'x = 2')
        n, out = self.run_compare([[0x100, 0x200], [0x300, 0x400]], [[0x100, 0x200], [0x300, 0x400]], fns)
        self.assertEqual(n, 1, out)
        self.assertIn('1 differing', out)
        self.assertIn('f_300', out)

    def test_fallback_belongs_to_its_function(self):
        fns = dict(FNS)
        fns[0x200] = fns[0x200].replace('static void fb_200(Cpu *c);\n', '')
        n, out = self.run_compare([[0x100, 0x200]], [[0x100], [0x200]], fns)
        self.assertEqual(n, 1, out)
        self.assertIn('differing: f_200', out)

    def test_missing_and_twice(self):
        n, out = self.run_compare([[0x100, 0x200], [0x300, 0x400]], [[0x100, 0x200], [0x300], [0x300]])
        self.assertGreaterEqual(n, 2, out)
        self.assertIn('1 missing', out)
        self.assertIn('f_300 defined in mod_0001.c, mod_0002.c', out)

    def test_call_without_prototype(self):
        n, out = self.run_compare([[0x100, 0x200]], [[0x100, 0x200]], new_protos=[0x300])
        self.assertGreaterEqual(n, 1, out)
        self.assertIn('f_100 calls f_200 with no declaration', out)

    def test_main_exit_code(self):
        with tempfile.TemporaryDirectory() as d:
            old = tree(Path(d) / 'old', [[0x100, 0x200, 0x300, 0x400]])
            new = tree(Path(d) / 'new', [[0x100], [0x200], [0x300, 0x400]])
            sys.stdout, saved = io.StringIO(), sys.stdout
            try:
                self.assertEqual(lift_compare.main([str(old), str(new)]), 0)
                (new / 'layout.h').write_text('other\n')
                self.assertEqual(lift_compare.main([str(old), str(new)]), 1)
            finally:
                sys.stdout = saved


if __name__ == '__main__':
    unittest.main()
