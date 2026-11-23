import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class InspectionTest(unittest.TestCase):
    def test_unconfigured_installation_is_reported_without_writes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root)],
                                    capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)['status'], 'not_configured')
            self.assertEqual(list(root.iterdir()), [])

    def test_malformed_settings_do_not_create_backup_or_expose_contents(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            data = root / 'data'
            data.mkdir()
            (data / 'launcher.json').write_text('secret-invalid-json', encoding='utf-8')
            result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root)],
                                    capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)['status'], 'inspection_error')
            self.assertNotIn('secret-invalid-json', result.stdout)
            self.assertEqual(sorted(p.name for p in data.iterdir()), ['launcher.json'])

    def test_finished_build_is_ready(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game = root / 'game'
            (root / 'data').mkdir()
            (game / 'build-cache').mkdir(parents=True)
            (root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
            (game / ('ocarina.exe' if sys.platform == 'win32' else 'ocarina')).touch()
            (game / 'build-cache' / 'state.json').write_text(json.dumps({'kit': '1.0', 'steps': {'compile': {}}}))
            result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root)],
                                    capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)['status'], 'ready')

    def test_nonexistent_root_is_not_treated_as_new_installation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'missing'
            result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root)],
                                    capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)['status'], 'inspection_error')
            self.assertFalse(root.exists())

    def test_invalid_build_state_is_reported_as_error(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game = root / 'game'
            (root / 'data').mkdir()
            (game / 'build-cache').mkdir(parents=True)
            (root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
            (game / 'build-cache' / 'state.json').write_text(json.dumps({'steps': None}))
            result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root)], capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)['status'], 'inspection_error')

    def test_job_marker_is_reported_without_assuming_it_was_interrupted(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'data').mkdir()
            marker = root / 'data' / 'job-running.json'
            marker.write_text(json.dumps({'task': 'Rebuild'}))
            result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root)], capture_output=True, text=True, check=True)
            self.assertTrue(json.loads(result.stdout)['job_marker_present'])
            self.assertTrue(marker.exists())

    def test_game_liveness_is_explicitly_unknown_when_not_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root)], capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)['game_running'], None)

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('sleep'), 'Linux process test')
    def test_running_synthetic_executable_is_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game = root / 'game'
            (root / 'data').mkdir()
            (game / 'build-cache').mkdir(parents=True)
            (root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
            (game / 'build-cache' / 'state.json').write_text(json.dumps({'kit': '1.0', 'steps': {'compile': {}}}))
            exe = game / 'ocarina'
            shutil.copy2(shutil.which('sleep'), exe)
            proc = subprocess.Popen([str(exe), '30'])
            try:
                result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root)], capture_output=True, text=True, check=True)
                self.assertTrue(json.loads(result.stdout)['game_running'])
            finally:
                proc.terminate()
                proc.wait(timeout=5)

    def test_hide_preference_is_read_without_exposing_settings(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'data').mkdir()
            (root / 'data' / 'launcher.json').write_text(json.dumps({'hide_while_playing': False, 'github_token': 'secret'}))
            result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root)], capture_output=True, text=True, check=True)
            self.assertIs(json.loads(result.stdout)['hide_while_playing'], False)
            self.assertNotIn('secret', result.stdout)

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('sleep'), 'Linux process test')
    def test_running_game_must_descend_from_selected_launcher(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game = root / 'game'
            (root / 'data').mkdir()
            (game / 'build-cache').mkdir(parents=True)
            (root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
            (game / 'build-cache' / 'state.json').write_text(json.dumps({'kit': '1.0', 'steps': {'compile': {}}}))
            exe = game / 'ocarina'
            shutil.copy2(shutil.which('sleep'), exe)
            proc = subprocess.Popen([str(exe), '30'])
            try:
                def query(owner):
                    result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root), '--owner-pid', str(owner)], capture_output=True, text=True, check=True)
                    return json.loads(result.stdout)['game_running']
                self.assertTrue(query(os.getpid()))
                self.assertIsNone(query(99999999))
            finally:
                proc.terminate()
                proc.wait(timeout=5)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows process test')
    def test_windows_running_game_must_descend_from_selected_launcher(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game = root / 'game'
            (root / 'data').mkdir()
            (game / 'build-cache').mkdir(parents=True)
            (root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
            (game / 'build-cache' / 'state.json').write_text(json.dumps({'kit': '1.0', 'steps': {'compile': {}}}))
            exe = game / 'ocarina.exe'
            shutil.copy2(Path(os.environ['SystemRoot']) / 'System32' / 'ping.exe', exe)
            proc = subprocess.Popen([str(exe), '-n', '15', '127.0.0.1'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                def query(owner):
                    result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root), '--owner-pid', str(owner)], capture_output=True, text=True, check=True)
                    return json.loads(result.stdout)['game_running']
                self.assertTrue(query(os.getpid()))
                self.assertIsNone(query(99999999))
            finally:
                proc.terminate()
                proc.wait(timeout=5)

    @unittest.skipUnless(sys.platform == 'win32', 'Windows process test')
    def test_windows_same_named_process_in_another_folder_is_not_the_game(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game = root / 'game'
            other = root / 'other'
            (root / 'data').mkdir()
            game.mkdir()
            other.mkdir()
            (root / 'data' / 'launcher.json').write_text(json.dumps({'game_dir': str(game)}))
            shutil.copy2(Path(os.environ['SystemRoot']) / 'System32' / 'ping.exe', game / 'ocarina.exe')
            shutil.copy2(Path(os.environ['SystemRoot']) / 'System32' / 'ping.exe', other / 'ocarina.exe')
            proc = subprocess.Popen([str(other / 'ocarina.exe'), '-n', '15', '127.0.0.1'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                result = subprocess.run([sys.executable, '-m', 'launcher.inspect_installation', str(root), '--owner-pid', str(os.getpid())], capture_output=True, text=True, check=True)
                self.assertIsNone(json.loads(result.stdout)['game_running'])
            finally:
                proc.terminate()
                proc.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
