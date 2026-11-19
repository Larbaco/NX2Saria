"""The launcher commit warning's pure threshold and message, with the system probe and dialog stubbed."""
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from launcher import commit


class CommitWarningTest(unittest.TestCase):
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
                mock.patch.object(commit, 'game_commit_need', return_value=24576), \
                mock.patch.object(commit.winsys, 'confirm_play', return_value=False) as dialog:
            self.assertFalse(commit.confirm_play_memory('game', lines.append, root='root'))
        self.assertIn('18000.0 of 27000.0 MiB used, 9000.0 MiB left, 16000.0 MiB RAM', lines[0])
        self.assertIn('(warning)', lines[0])
        self.assertIn('32768 MB', dialog.call_args.args[0])
        self.assertEqual(dialog.call_args.args[1], 'root')
        self.assertEqual(lines[-1], 'play memory: player cancelled')

    def test_enough_commit_logs_the_sample_and_does_not_open_dialog(self):
        lines = []
        with mock.patch.object(commit.winsys, 'system_commit_info', return_value=(1000, 50000, 16000)), \
                mock.patch.object(commit, 'game_commit_need', return_value=24576), \
                mock.patch.object(commit.winsys, 'confirm_play') as dialog:
            self.assertTrue(commit.confirm_play_memory('game', lines.append))
        self.assertIn('(enough)', lines[0])
        dialog.assert_not_called()


if __name__ == '__main__':
    unittest.main(verbosity=2)
