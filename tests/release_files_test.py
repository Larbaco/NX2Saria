"""tools/release_files.py: what the release workflow (.github/workflows/release.yml) refuses before it attests and
publishes, and the release page text. Standard library only, so the content guard workflow runs it.

    python tests/release_files_test.py
"""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import release_files as rf  # noqa: E402

NOTES = ('- One thing a player notices.\n\nYour saves, mods and settings stay where they are when you update.\n'
         'This update includes 0.18.2 to 0.18.5, which came out as pre-releases.\n')


def repo(tmp, version, notes=NOTES, tag=None):
    root = Path(tmp) / 'repo'
    (root / 'docs' / 'releases').mkdir(parents=True)
    (root / 'VERSION').write_text(version + '\n', encoding='utf-8')
    if notes is not None:
        (root / 'docs' / 'releases' / f'{tag or "v" + version}.md').write_text(notes, encoding='utf-8')
    return root


def release(tmp, ver, notes=NOTES, runtime='NX2Saria-runtime-py3.14.9-1.zip'):
    """A folder with the files the Windows and Linux builds hand to the release workflow."""
    d = Path(tmp) / 'dist'
    d.mkdir()
    entries = {}
    for key, name in (('setup', f'NX2Saria-Setup-{ver}.exe'), ('app', f'NX2Saria-app-{ver}.zip'),
                      ('runtime', runtime)):
        data = f'{key} of {ver}'.encode()
        (d / name).write_bytes(data)
        entries[key] = {'name': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    entries['runtime']['id'] = 'py3.14.9-1'
    linux_entries = {}
    for key, name in (('app', f'NX2Saria-app-linux-x86_64-{ver}.zip'),
                      ('runtime', 'NX2Saria-runtime-linux-x86_64-py3.14.9-1.zip'),
                      ('appimage', f'NX2Saria-Linux-{ver}-x86_64.AppImage')):
        data = f'linux {key} of {ver}'.encode()
        (d / name).write_bytes(data)
        linux_entries[key] = {'name': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    linux_entries['runtime']['id'] = 'py3.14.9-1'
    m = {'format': 1, 'version': ver, 'build': f'{ver}+abc1234', 'notes': notes, **entries, 'toolchain': 'x'}
    for name in rf.MANIFESTS:
        (d / name).write_text(json.dumps(m, indent=1), encoding='utf-8')
    lm = {'format': 1, 'version': ver, 'build': f'{ver}+abc1234', 'notes': notes,
          **linux_entries, 'toolchain': 'linux-x86_64'}
    (d / rf.LINUX_MANIFEST).write_text(json.dumps(lm, indent=1), encoding='utf-8')
    return d


class Tag(unittest.TestCase):
    def test_rc_and_stable(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn('a release candidate', rf.check_tag('v0.19.0-rc.1', repo(tmp, '0.19.0-rc.1'))[0])
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn('no suffix', rf.check_tag('v0.19.0', repo(tmp, '0.19.0'))[0])

    def test_refused(self):
        cases = [('v0.19.0', '0.19.0-rc.1', NOTES, 'VERSION'),
                 ('v0.19.0-rc.0', '0.19.0-rc.0', NOTES, 'not a release tag'),
                 ('v0.19.0-beta', '0.19.0-beta', NOTES, 'not a release tag'),
                 ('0.19.0', '0.19.0', NOTES, 'not a release tag'),
                 ('v0.19.0', '0.19.0', None, 'missing or empty'),
                 ('v0.19.0', '0.19.0', ' \n', 'missing or empty'),
                 ('v0.19.0', '0.19.0', NOTES + 'Pre-release: a test build.\n', 'pre-release line'),
                 ('v0.19.0', '0.19.0', NOTES + '**Prerelease** for now.\n', 'pre-release line'),
                 ('v0.19.0', '0.19.0', NOTES + '_Pre-release:_ for now.\n', 'pre-release line'),
                 ('v0.19.0', '0.19.0', NOTES + 'Pre release: for now.\n', 'pre-release line'),
                 ('v0.19.0', '0.19.0', NOTES + 'Pre\u2011release: for now.\n', 'pre-release line')]
        for tag, version, notes, why in cases:
            with self.subTest(tag=tag, version=version), tempfile.TemporaryDirectory() as tmp:
                with self.assertRaisesRegex(rf.Problem, why):
                    rf.check_tag(tag, repo(tmp, version, notes, tag=tag if tag.startswith('v') else None))

    def test_rc_may_say_it_is_a_pre_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            rf.check_tag('v0.19.0-rc.2', repo(tmp, '0.19.0-rc.2', NOTES + 'Pre-release: a test build.\n'))


class Files(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.notes = Path(self.tmp.name) / 'notes.md'
        self.notes.write_text(NOTES, encoding='utf-8')

    def tearDown(self):
        self.tmp.cleanup()

    def test_good_release_gets_its_sums(self):
        d = release(self.tmp.name, '0.19.0-rc.1')
        out = rf.check_files('v0.19.0-rc.1', self.notes, d)
        self.assertEqual(out[-1], 'SHA256SUMS.txt: 9 files')
        sums = (d / rf.SUMS).read_text(encoding='utf-8').splitlines()
        self.assertEqual(len(sums), 9)
        for line in sums:
            digest, name = line.split('  ')
            self.assertEqual(digest, hashlib.sha256((d / name).read_bytes()).hexdigest())
        rf.check_files('v0.19.0-rc.1', self.notes, d)   # a second run over its own SHA256SUMS.txt

    def test_crlf_notes_are_the_same_notes(self):
        d = release(self.tmp.name, '0.19.0', notes=NOTES.replace('\n', '\r\n'))
        rf.check_files('v0.19.0', self.notes, d)

    def refused(self, why, change):
        d = release(self.tmp.name, '0.19.0')
        change(d)
        with self.assertRaisesRegex(rf.Problem, why):
            rf.check_files('v0.19.0', self.notes, d)

    def manifest(self, d, edit):
        m = json.loads((d / rf.MANIFESTS[0]).read_text(encoding='utf-8'))
        edit(m)
        for name in rf.MANIFESTS:
            (d / name).write_text(json.dumps(m), encoding='utf-8')

    def test_other_notes(self):
        self.refused('notes', lambda d: self.manifest(d, lambda m: m.update(notes=NOTES + 'Edited.\n')))

    def test_other_version(self):
        self.refused('version', lambda d: self.manifest(d, lambda m: m.update(version='0.19.1')))

    def test_manifests_differ(self):
        self.refused('differ', lambda d: (d / rf.MANIFESTS[1]).write_text('{}', encoding='utf-8'))

    def test_manifest_missing(self):
        self.refused('missing', lambda d: (d / rf.MANIFESTS[1]).unlink())

    def test_changed_file(self):
        self.refused('sha256', lambda d: (d / 'NX2Saria-app-0.19.0.zip').write_bytes(b'app of 0.19.X'))

    def test_extra_file(self):
        self.refused('unexpected', lambda d: (d / 'notes.txt').write_bytes(b'x'))

    def test_no_appimage(self):
        self.refused('missing', lambda d: (d / 'NX2Saria-Linux-0.19.0-x86_64.AppImage').unlink())

    def test_linux_manifest_missing(self):
        self.refused('missing', lambda d: (d / rf.LINUX_MANIFEST).unlink())

    def test_linux_files_must_match_manifest(self):
        self.refused('sha256', lambda d: (d / 'NX2Saria-app-linux-x86_64-0.19.0.zip').write_bytes(b'changed'))

    def test_same_name_twice(self):
        self.refused('two files', lambda d: self.manifest(d, lambda m: m['runtime'].update(name=m['app']['name'])))

    def test_other_setup_name(self):
        self.refused('setup file', lambda d: self.manifest(d, lambda m: m['setup'].update(name='Setup.exe')))

    def test_odd_runtime_name(self):
        self.refused('runtime file', lambda d: self.manifest(d, lambda m: m['runtime'].update(name='runtime.exe')))

    def test_path_in_a_name(self):
        self.refused('plain file name', lambda d: self.manifest(d, lambda m: m['app'].update(name='../x.zip')))


class Main(unittest.TestCase):
    def test_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = repo(tmp, '0.19.0')
            self.assertEqual(rf.main(['tag', '--tag', 'v0.19.0', '--root', str(root)]), 0)
            self.assertEqual(rf.main(['tag', '--tag', 'v0.19.1', '--root', str(root)]), 1)


class Promote(unittest.TestCase):
    def test_order(self):
        tags = ['v0.18.9', 'v0.19.0-rc.1', 'v0.19.0-rc.2', 'v0.19.0-rc.10', 'v0.19.0', 'v0.19.1-rc.1', 'v1.0.0']
        self.assertEqual(sorted(reversed(tags), key=rf.version_key), tags)

    def test_promote(self):
        rf.check_promote('v0.19.0', ['v0.18.6', 'v0.9.2', 'v0.10.0'])
        rf.check_promote('v0.19.0', [])
        rf.check_promote('v0.19.0', ['v0.18.6', 'nightly'])
        for tag, stable, why in (('v0.19.0-rc.1', ['v0.18.6'], 'release candidate'),
                                 ('v0.19.0', ['v0.18.6', 'v0.19.0'], 'not older'),
                                 ('v0.19.0', ['v0.19.1', 'v0.18.6'], 'not older'),
                                 ('v0.19', ['v0.18.6'], 'not a release tag')):
            with self.subTest(tag=tag, stable=stable), self.assertRaisesRegex(rf.Problem, why):
                rf.check_promote(tag, stable)


class Body(unittest.TestCase):
    def test_states(self):
        with tempfile.TemporaryDirectory() as tmp:
            notes = Path(tmp) / 'n.md'
            notes.write_text(NOTES, encoding='utf-8')
            rc = rf.body('v0.19.0-rc.1', notes, 'prerelease')
            pre = rf.body('v0.19.0', notes, 'prerelease')
            stable = rf.body('v0.19.0', notes, 'stable')
            notes.write_text('Pre-release: a test build.\n' + NOTES, encoding='utf-8')
            rc_own = rf.body('v0.19.0-rc.1', notes, 'prerelease')
        self.assertTrue(rc.startswith('**Pre-release:** a release candidate'))
        self.assertTrue(pre.startswith('**Pre-release:** 0.19.0 becomes the stable release'))
        self.assertTrue(stable.startswith(NOTES.splitlines()[0]))
        self.assertEqual(rc_own.count('Pre-release'), 1)   # the notes' own line only, no second one on top
        self.assertTrue(rc_own.startswith('Pre-release: a test build.'))
        for b in (rc, pre, stable):
            self.assertIn(NOTES.rstrip(), b)
            self.assertIn('https://github.com/Larbaco/NX2Saria/blob/main/docs/install.md#verify-your-download', b)


if __name__ == '__main__':
    unittest.main()
