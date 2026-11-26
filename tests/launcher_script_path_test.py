import ast
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / 'launcher'


def own_top_names():
    names = set()
    for p in LAUNCHER.iterdir():
        if p.suffix == '.py':
            names.add(p.stem)
        elif p.is_dir() and (p / '__init__.py').exists():
            names.add(p.name)
    return names


def pythons():
    found = [sys.executable]
    for name in ('python3', 'python'):
        other = shutil.which(name)
        if other and os.path.realpath(other) not in {os.path.realpath(p) for p in found}:
            found.append(other)
    return found


class ScriptPathTest(unittest.TestCase):
    def run_cli(self, python, *args):
        with tempfile.TemporaryDirectory(prefix='nx2saria-cli-path-') as tmp:
            env = {k: v for k, v in os.environ.items() if not k.startswith(('PYTHON', 'NX2SARIA_', 'NX2NAVI_'))}
            env['HOME'] = env['USERPROFILE'] = tmp
            return subprocess.run([python, str(LAUNCHER / 'cli.py'), '--root', str(Path(tmp) / 'root'), *args],
                                  cwd=tmp, env=env, capture_output=True, text=True, timeout=120)

    def test_cli_as_a_script_without_isolation(self):
        for python in pythons():
            with self.subTest(python=python):
                r = self.run_cli(python, 'rebuild')
                out = r.stdout + r.stderr
                self.assertNotIn('Traceback', out, out)
                self.assertEqual(r.returncode, 1, out)
                self.assertIn('error: no game folder', out)

    def test_cli_drops_its_own_folder_from_the_path(self):
        code = ("import runpy, sys; sys.argv = [sys.argv[1], '--help']\n"
                "try:\n    runpy.run_path(sys.argv[0], run_name='__main__')\n"
                "except SystemExit:\n    pass\n"
                "import platform; print(platform.__file__); print(any(__import__('os').path.abspath(p or '.') == "
                "__import__('os').path.dirname(__import__('os').path.abspath(sys.argv[0])) for p in sys.path))")
        r = subprocess.run([sys.executable, '-c', code, str(LAUNCHER / 'cli.py')], cwd=str(LAUNCHER),
                           capture_output=True, text=True, timeout=120)
        lines = r.stdout.strip().splitlines()
        self.assertGreaterEqual(len(lines), 2, r.stdout + r.stderr)
        self.assertNotIn(str(LAUNCHER / 'platform'), lines[-2], 'platform resolved to the launcher package')
        self.assertEqual(lines[-1], 'False', 'the script folder is still on sys.path')

    def test_no_bare_import_of_a_name_the_launcher_owns(self):
        own = own_top_names()
        std = set(getattr(sys, 'stdlib_module_names', ())) or {'platform'}
        clash = own & std
        self.assertIn('platform', clash)
        bad = []
        for path in sorted(LAUNCHER.rglob('*.py')) + [LAUNCHER / 'start.pyw']:
            tree = ast.parse(path.read_text(encoding='utf-8'), str(path))
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    names = [a.name for a in node.names]
                elif isinstance(node, ast.ImportFrom) and not node.level and node.module:
                    names = [node.module]
                else:
                    continue
                for n in names:
                    if n.split('.')[0] in clash:
                        bad.append(f'{path.relative_to(ROOT)}:{node.lineno}: import {n}')
        self.assertEqual(bad, [], 'a launcher module imports a standard module by a name launcher/ also has')


if __name__ == '__main__':
    unittest.main()
