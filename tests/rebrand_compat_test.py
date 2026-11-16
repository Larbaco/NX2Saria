"""Branding transitions preserve update discovery, private reports, shortcuts and existing installs. No game files."""
import contextlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'tools')]
from launcher import feed, paths, saves, steam, winsys
import make_launcher
import capture_run


class RebrandCompatibilityTest(unittest.TestCase):
    def test_launcher_renames_a_legacy_settings_file(self):
        from launcher import gameini
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp)
            (game / 'nx2navi.ini').write_text('sharpness = 20\n', encoding='utf-8')
            self.assertEqual(gameini._path(game), game / 'nx2saria.ini')
            self.assertEqual((game / 'nx2saria.ini').read_text(encoding='utf-8'), 'sharpness = 20\n')
            self.assertFalse((game / 'nx2navi.ini').exists())
            # both there: nx2saria.ini is the file, and nx2navi.ini is left as it is
            (game / 'nx2navi.ini').write_text('sharpness = 5\n', encoding='utf-8')
            self.assertEqual(gameini._path(game), game / 'nx2saria.ini')
            self.assertEqual((game / 'nx2navi.ini').read_text(encoding='utf-8'), 'sharpness = 5\n')
            # a fresh folder: nx2saria.ini, nothing renamed
            fresh = game / 'fresh'
            fresh.mkdir()
            self.assertEqual(gameini._path(fresh), fresh / 'nx2saria.ini')
            self.assertEqual(list(fresh.iterdir()), [])

    def test_both_update_manifest_names_discover_current_payloads(self):
        manifest = {'format': 1, 'version': '1.0.0', 'app': {
            'name': 'NX2Saria-app-1.0.0.zip', 'size': 7, 'sha256': 'a' * 64}}
        with tempfile.TemporaryDirectory() as tmp:
            files = make_launcher.write_update_manifests(tmp, manifest)
            self.assertEqual({p.name for p in files}, {feed.MANIFEST, feed.LEGACY_MANIFEST})
            self.assertEqual(files[0].read_bytes(), files[1].read_bytes())
            for name in (feed.MANIFEST, feed.LEGACY_MANIFEST):
                release = {'tag_name': 'v1.0.0', 'assets': [
                    {'name': n, 'browser_download_url': 'https://example.invalid/' + n}
                    for n in (name, manifest['app']['name'])]}
                with mock.patch.object(feed.net, 'get_json', return_value=[release]), \
                     mock.patch.object(feed.net, 'get_bytes', return_value=files[0].read_bytes()):
                    found = feed.latest({'type': 'github', 'repo': 'Larbaco/NX2Saria'})
                self.assertEqual(found.asset('app')[0], 'https://example.invalid/NX2Saria-app-1.0.0.zip')

    def test_reports_scrub_both_prefixed_private_settings(self):
        for prefix in ('', 'NX2SARIA_', 'nx2navi_', 'nX2sArIa_'):
            for key in saves.PRIVATE_SETTINGS:
                text = f'{prefix}{key} = secret-value\r\nsharpness = 80\r\n'.encode()
                scrubbed = saves.scrub_settings(text)
                self.assertNotIn(b'secret-value', scrubbed)
                self.assertIn(saves.PRIVATE_SHOWN, scrubbed)
                self.assertIn(b'sharpness = 80\r\n', scrubbed)

    def test_runtime_report_scrubs_the_same_prefixed_keys(self):
        cc = shutil.which('clang') or shutil.which('cc')
        if not cc:
            self.skipTest('C compiler unavailable')
        source = r'''
#include "cfg_private.h"
int main(void) {
    const char *text = "NX2SARIA_COOP_ROOM = secret-one\nnx2navi_menu_test_set = secret-two\nsharpness = 80\n";
    size_t size;
    char *scrubbed = cfg_ini_scrub(text, strlen(text), &size);
    int bad = !scrubbed || strstr(scrubbed, "secret-") || !strstr(scrubbed, "sharpness = 80");
    free(scrubbed);
    return bad;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'scrub.c', Path(tmp) / 'scrub'
            src.write_text(source)
            subprocess.run([cc, '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'), str(src),
                            '-o', str(exe)], check=True, capture_output=True)
            subprocess.run([str(exe)], check=True)

    def test_legacy_stub_is_reused_until_a_current_stub_exists(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            legacy, current = root / 'NX2navi.exe', root / 'NX2Saria.exe'
            legacy.touch()
            self.assertEqual(paths.Layout(root=root).stub, legacy)
            current.touch()
            self.assertEqual(paths.Layout(root=root).stub, current)

    def test_empty_current_launcher_root_does_not_select_legacy_root(self):
        with mock.patch.dict(os.environ, {'NX2SARIA_LAUNCHER_ROOT': '', 'NX2NAVI_LAUNCHER_ROOT': '/legacy'}):
            self.assertNotEqual(str(paths.Layout().root), '/legacy')

    def test_legacy_capture_uses_its_saved_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            capture = root / 'capture'
            capture.mkdir()
            game = root / 'game'
            (game / 'profile/save').mkdir(parents=True)
            (game / 'profile/save/current').write_bytes(b'current-save')
            with zipfile.ZipFile(capture / 'NX2navi-save-test.zip', 'w') as archive:
                archive.writestr('save/captured', b'captured-save')
            profile = root / 'replay'
            capture_run.make_profile(profile, game, capture)
            self.assertEqual((profile / 'save-import/captured').read_bytes(), b'captured-save')
            self.assertFalse((profile / 'save').exists())

    def test_steam_rename_preserves_id_playtime_and_collections(self):
        original, sid = steam.add_shortcut(b'', 'old-python', 'old-root', 'play', 'icon', name=steam.LEGACY_NAMES[0])
        tree = steam.vdf_load(original)
        entry = steam._get(tree, 'shortcuts')[2][0][2]
        steam._set(entry, steam.T_INT, 'LastPlayTime', struct.pack('<I', 1234))
        steam._set(entry, steam.T_MAP, 'tags', [[steam.T_STR, '0', 'Favorites']])
        updated, kept = steam.add_shortcut(steam.vdf_dump(tree), 'new-python', 'same-root', 'play', 'new-icon')
        entries = steam._get(steam.vdf_load(updated), 'shortcuts')[2]
        self.assertEqual(kept, sid)
        self.assertEqual(len(entries), 1)
        entry = entries[0][2]
        self.assertEqual(steam._get(entry, 'AppName')[2], steam.NAME)
        self.assertEqual(steam._get(entry, 'LastPlayTime')[2], struct.pack('<I', 1234))
        self.assertEqual(steam._get(entry, 'tags')[2], [[steam.T_STR, '0', 'Favorites']])

    def test_uninstall_keeps_a_separate_legacy_apps_entry(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            layout = SimpleNamespace(root=root, toolchain=root / 'toolchain', stub=root / 'NX2Saria.exe')
            removed = []
            fake_registry = SimpleNamespace(
                HKEY_CURRENT_USER=1,
                OpenKey=lambda hive, key: contextlib.nullcontext(key),
                QueryValueEx=lambda key, value: (str(root if key == winsys.UNINSTALL_KEY else root / 'separate'), 1),
                DeleteKey=lambda hive, key: removed.append(key))
            fake_os = SimpleNamespace(name='nt', path=os.path, environ=os.environ)
            with mock.patch.dict(sys.modules, {'winreg': fake_registry}), \
                 mock.patch.object(winsys, 'os', fake_os), \
                 mock.patch.object(winsys, 'stub_run'), mock.patch.object(winsys, 'start_detached'):
                winsys.uninstall(layout)
            self.assertEqual(removed, [winsys.UNINSTALL_KEY])

    def test_committed_icon_matches_generator_and_has_transparent_corners(self):
        try:
            from launcher.ui import art
            from PIL import Image
        except ImportError:
            self.skipTest('numpy and Pillow required for icon verification')
        data = (ROOT / 'runtime' / 'nx2saria.ico').read_bytes()
        self.assertEqual(data, art.sprig_ico())
        with Image.open(io.BytesIO(data)) as icon:
            self.assertEqual(len(icon.ico.sizes()), 10)
            for size in icon.ico.sizes():
                frame = icon.ico.getimage(size).convert('RGBA')
                for point in ((0, 0), (size[0] - 1, 0), (0, size[1] - 1), (size[0] - 1, size[1] - 1)):
                    self.assertEqual(frame.getpixel(point)[3], 0, (size, point))

    def test_both_pgo_build_flags_keep_the_runtime_flush(self):
        cc = shutil.which('clang')
        if not cc or sys.platform != 'linux':
            self.skipTest('Linux clang preprocessing required')
        for macro in ('NX2SARIA_PGO_GEN', 'NX2NAVI_PGO_GEN'):
            result = subprocess.run([cc, '-E', '-P', '-D' + macro, '-I', str(ROOT / 'runtime'),
                                     '-I', str(ROOT / 'runtime/platform'), str(ROOT / 'runtime/platform/linux.c')],
                                    check=True, capture_output=True, text=True)
            self.assertIn('__llvm_profile_dump', result.stdout, macro)


if __name__ == '__main__':
    unittest.main()
