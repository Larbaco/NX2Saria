import json
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from launcher import commit
from launcher import settings
from launcher import winsys


class CommitWarningTest(unittest.TestCase):
    def test_dont_warn_choice_is_saved_and_logged(self):
        class MemorySettings(dict):
            saved = False

            def save(self):
                self.saved = True

        chosen = MemorySettings(warn_low_commit=True)
        lines = []
        commit.set_warning_enabled(chosen, False, lines.append)
        self.assertFalse(chosen['warn_low_commit'])
        self.assertTrue(chosen.saved)
        self.assertEqual(lines, ['launcher setting: warn_low_commit = 0'])

    def test_threshold_is_strict_and_zero_need_disables_warning(self):
        self.assertFalse(commit.needs_commit_warning(24576, 24576))
        self.assertFalse(commit.needs_commit_warning(30000, 24576))
        self.assertFalse(commit.needs_commit_warning(0, 0))
        self.assertFalse(commit.needs_commit_warning(0, float('-inf')))
        self.assertTrue(commit.needs_commit_warning(24575, 24576))
        self.assertTrue(commit.needs_commit_warning(24576, float('inf')))

    def test_warning_names_the_shortfall_and_advice(self):
        text = commit.commit_warning(9000, 24576)
        self.assertIn('Windows has 9000 MiB', text)
        self.assertIn('needs about 24576 MiB', text)
        self.assertIn('RAM plus page file', text)
        self.assertIn('32768 MB', text)
        self.assertIn('Restart Windows', text)
        self.assertIn("Don't warn me again", text)
        self.assertIn('Warn About Low Commit', text)
        self.assertIn('mem_need_mb = 0', text)
        self.assertTrue(text.endswith('Play anyway?'))
        self.assertEqual(commit.commit_warning(24576, 24576), '')

    def test_short_fatal_keeps_memory_advice_visible(self):
        fatal = ('memory: commit physical memory 0x558200000+0x31fc000 failed (52412416 bytes, Win32 1455 '
                 '(ERROR_COMMITMENT_LIMIT): Windows ran out of memory it can promise ..., commit 22000 of 22000 MB)')
        text = commit.fatal_summary(fatal)
        self.assertIn('Win32 1455 ERROR_COMMITMENT_LIMIT', text)
        self.assertIn('RAM plus page file', text)
        self.assertIn('32768 MB', text)
        self.assertIn('restart Windows', text)
        self.assertEqual(commit.fatal_summary('memory: commit failed (Linux errno 12)'), '')

    def test_check_logs_numbers_and_allows_player_choice(self):
        lines = []
        with mock.patch.object(commit.winsys, 'system_commit_info', return_value=(18000, 27000, 16000)), \
                mock.patch.object(commit.winsys, 'page_file_is_system_managed', return_value=False), \
                mock.patch.object(commit, 'game_commit_need', return_value=24576), \
                mock.patch.object(commit.winsys, 'confirm_play', return_value=False) as dialog:
            self.assertFalse(commit.confirm_play_memory('game', lines.append, root='root',
                                                        settings={'warn_low_commit': True}))
        self.assertIn('18000.0 of 27000.0 MiB used, 9000.0 MiB left, 16000.0 MiB RAM', lines[0])
        self.assertIn('(warning)', lines[0])
        self.assertIn('32768 MB', dialog.call_args.args[0])
        self.assertEqual(dialog.call_args.args[1], 'root')
        self.assertEqual(lines[-1], 'play memory: player cancelled')

    def test_unbounded_memory_request_is_json_safe_and_keeps_warning(self):
        lines = []
        with mock.patch.object(commit.winsys, 'system_commit_info', return_value=(18000, 27000, 16000)), \
                mock.patch.object(commit.winsys, 'page_file_is_system_managed', return_value=False), \
                mock.patch.object(commit, 'game_commit_need', return_value=float('inf')):
            request = commit.play_memory_request('game', lines.append)
        self.assertEqual(request.remaining, 9000)
        self.assertIsNone(request.need)
        self.assertTrue(request.unbounded)
        json.dumps(request._asdict(), allow_nan=False)
        self.assertIn('needs about inf MiB', commit.play_memory_warning(request))

    def test_enough_commit_logs_the_sample_and_does_not_open_dialog(self):
        lines = []
        with mock.patch.object(commit.winsys, 'system_commit_info', return_value=(1000, 50000, 16000)), \
                mock.patch.object(commit.winsys, 'page_file_is_system_managed', return_value=False), \
                mock.patch.object(commit, 'game_commit_need', return_value=24576), \
                mock.patch.object(commit.winsys, 'confirm_play') as dialog:
            self.assertTrue(commit.confirm_play_memory('game', lines.append))
        self.assertIn('(enough)', lines[0])
        dialog.assert_not_called()

    def test_system_managed_page_file_logs_but_never_warns(self):
        lines = []
        with mock.patch.object(commit.winsys, 'system_commit_info', return_value=(18000, 27000, 16000)), \
                mock.patch.object(commit.winsys, 'page_file_is_system_managed', return_value=True), \
                mock.patch.object(commit, 'game_commit_need', return_value=24576), \
                mock.patch.object(commit.winsys, 'confirm_play') as dialog:
            self.assertTrue(commit.confirm_play_memory('game', lines.append, settings=settings.DEFAULTS))
        self.assertIn('9000.0 MiB left', lines[0])
        self.assertIn('system-managed page file; log only', lines[0])
        dialog.assert_not_called()

    def test_unknown_page_file_mode_logs_but_does_not_guess(self):
        lines = []
        with mock.patch.object(commit.winsys, 'system_commit_info', return_value=(18000, 27000, 16000)), \
                mock.patch.object(commit.winsys, 'page_file_is_system_managed', return_value=None), \
                mock.patch.object(commit, 'game_commit_need', return_value=24576), \
                mock.patch.object(commit.winsys, 'confirm_play') as dialog:
            self.assertTrue(commit.confirm_play_memory('game', lines.append, settings=settings.DEFAULTS))
        self.assertIn('9000.0 MiB left', lines[0])
        self.assertIn('page-file mode unknown; log only', lines[0])
        dialog.assert_not_called()

    def test_launcher_setting_disables_warning_but_keeps_numbers_and_logs_choice(self):
        lines = []
        with mock.patch.object(commit.winsys, 'system_commit_info', return_value=(18000, 27000, 16000)), \
                mock.patch.object(commit.winsys, 'page_file_is_system_managed', return_value=False), \
                mock.patch.object(commit, 'game_commit_need', return_value=24576), \
                mock.patch.object(commit.winsys, 'confirm_play') as dialog:
            self.assertTrue(commit.confirm_play_memory('game', lines.append, settings={'warn_low_commit': False}))
        self.assertIn('9000.0 MiB left', lines[0])
        self.assertIn('disabled in launcher Settings', lines[0])
        dialog.assert_not_called()

    def test_page_file_mode_classification_matches_runtime_policy(self):
        self.assertTrue(winsys.page_files_system_managed([r'C:\pagefile.sys 0 0']))
        self.assertTrue(winsys.page_files_system_managed([r'?:\pagefile.sys']))
        self.assertFalse(winsys.page_files_system_managed([r'C:\pagefile.sys 32768 32768']))
        self.assertFalse(winsys.page_files_system_managed([r'C:\pagefile.sys 0 0', r'D:\pagefile.sys 32768 32768']))
        self.assertFalse(winsys.page_files_system_managed([]))
        self.assertIsNone(winsys.page_files_system_managed(['']))
        self.assertIsNone(winsys.page_files_system_managed(['C:\\pagefile.sys 32768']))
        self.assertIsNone(winsys.page_files_system_managed(['C:\\pagefile.sys large 32768']))

    def test_yes_is_the_default_confirmation_button(self):
        user32 = mock.MagicMock()
        user32.MessageBoxW.return_value = 6
        with mock.patch.object(winsys.ctypes, 'WinDLL', return_value=user32, create=True):
            self.assertTrue(winsys.confirm_play('warning'))
        flags = user32.MessageBoxW.call_args.args[3]
        self.assertEqual(flags & 0x300, 0)
        self.assertTrue(settings.DEFAULTS['warn_low_commit'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
