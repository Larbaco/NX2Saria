import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from launcher import commit
from launcher.ui import app


class PlayCharacterizationTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2-play-')
        self.addCleanup(self.tmp.cleanup)
        self.game = Path(self.tmp.name) / 'game'
        self.game.mkdir()
        self.exe = app.game_executable(self.game)
        self.exe.touch()
        self.lines = []
        self.window = mock.MagicMock()
        self.window.settings = {'game_dir': str(self.game), 'hide_while_playing': False}
        self.window.ctx.settings = self.window.settings
        self.window.ctx.log.side_effect = self.lines.append
        self.window.game_run = None
        self.window.mods_failed = []
        self.window.message = ''
        self.window.mods_message = ''
        self.window.mods_before_play.side_effect = lambda gd: app.App.mods_before_play(self.window, gd)
        self.prepare_mods = mock.patch.object(app.mods, 'prepare', return_value=[])
        self.can_rebuild = mock.patch.object(app.build, 'can_rebuild', return_value=True)
        self.waiting_mods = mock.patch.object(app.mods, 'code_waiting', return_value=[])
        self.prepare_mods.start()
        self.can_rebuild.start()
        self.code_waiting = self.waiting_mods.start()
        self.addCleanup(self.prepare_mods.stop)
        self.addCleanup(self.can_rebuild.stop)
        self.addCleanup(self.waiting_mods.stop)
        self.running = mock.patch.object(app.saves, 'game_running', return_value=False)
        self.spawn = mock.patch.object(app.winsys, 'start_game', return_value=mock.Mock())
        self.running.start()
        self.start_game = self.spawn.start()
        self.addCleanup(self.running.stop)
        self.addCleanup(self.spawn.stop)

    def play(self, built=False):
        app.App.play(self.window, built=built)

    def test_missing_game_and_spawn_failure_are_visible(self):
        self.exe.unlink()
        self.play()
        self.assertEqual(self.lines, [f'play: not started: the game is not built ({self.exe})'])
        self.assertEqual(self.window.message, 'The game is not built yet.')
        self.code_waiting.assert_not_called()
        self.start_game.assert_not_called()
        self.exe.touch()
        self.lines.clear()
        with mock.patch.object(app.commit, 'confirm_play_memory', return_value=True), \
                mock.patch.object(app.winsys, 'start_game', side_effect=OSError('synthetic failure')):
            self.play()
        self.assertEqual(self.lines, [f'play: not started: could not start the game: synthetic failure ({self.exe})'])
        self.assertEqual(self.window.message, 'The game could not start: synthetic failure')
        self.assertIsNone(self.window.game_run)

    def test_success_hides_only_when_requested_and_schedules_watch(self):
        for hide in (False, True):
            with self.subTest(hide=hide):
                self.window.settings['hide_while_playing'] = hide
                self.window.game_run = None
                self.window.root.reset_mock()
                self.lines.clear()
                with mock.patch.object(app.commit, 'confirm_play_memory', return_value=True):
                    self.play()
                self.assertEqual(self.lines, [f'play: {self.exe}'])
                self.assertEqual(self.window.root.withdraw.call_count, int(hide))
                self.window.root.after.assert_called_once_with(1000, self.window.watch_game)
                self.assertIsInstance(self.window.game_run, app.GameRun)

    def test_existing_session_and_external_session_refuse_play(self):
        self.window.game_run = mock.Mock()
        self.window.game_run.alive.return_value = True
        self.play()
        self.assertEqual(self.lines, [f'play: not started: this launcher is already watching a game ({self.exe})'])
        self.assertEqual(self.window.message, '')
        self.code_waiting.assert_not_called()
        self.start_game.assert_not_called()
        self.lines.clear()
        self.window.game_run = None
        with mock.patch.object(app.saves, 'game_running', return_value=True):
            self.play()
        self.assertEqual(self.lines, [f'play: not started: the game is already running ({self.exe})'])
        self.assertEqual(self.window.message, 'The game is already running.')
        self.start_game.assert_not_called()

    def test_pending_mods_start_rebuild_before_memory_or_spawn(self):
        self.code_waiting.return_value = ['Example']
        with mock.patch.object(app.commit, 'confirm_play_memory') as memory:
            self.play()
        job, screen = self.window.run_job.call_args.args
        self.assertIsInstance(job, app.jobs.ModsRebuildJob)
        self.assertEqual(job.waiting, ['Example'])
        self.assertEqual(screen, 'home')
        self.assertEqual(self.lines, ['play: the game is built again first for the code of Example'])
        self.assertEqual(self.window.message, 'Building the game with the code of Example; it starts after.')
        memory.assert_not_called()
        self.start_game.assert_not_called()

    def test_built_and_failed_mods_allow_play(self):
        self.window.mods_failed = ['Example']
        self.code_waiting.return_value = ['Example']
        with mock.patch.object(app.commit, 'confirm_play_memory', return_value=True):
            self.play()
        self.assertEqual(self.lines, ['play: without the code of Example, whose build did not finish', f'play: {self.exe}'])
        self.start_game.assert_called_once_with(self.exe)
        self.window.run_job.assert_not_called()
        self.code_waiting.reset_mock()
        self.start_game.reset_mock()
        self.window.game_run = None
        self.lines.clear()
        with mock.patch.object(app.commit, 'confirm_play_memory', return_value=True):
            self.play(built=True)
        self.code_waiting.assert_not_called()
        self.assertEqual(self.lines, [f'play: {self.exe}'])
        self.start_game.assert_called_once_with(self.exe)

    def test_mod_preparation_warnings_and_errors_are_reported(self):
        warning = 'Example was not unpacked'
        with mock.patch.object(app.mods, 'prepare', return_value=[warning]), \
                mock.patch.object(app.build, 'can_rebuild', return_value=True), \
                mock.patch.object(app.mods, 'code_waiting', return_value=['Example']):
            self.play()
        self.assertEqual(self.window.mods_message, warning)
        self.assertEqual(self.window.message, 'Building the game with the code of Example; it starts after.')
        self.assertEqual(self.lines, ['play: the game is built again first for the code of Example'])
        self.lines.clear()
        with mock.patch.object(app.mods, 'prepare', side_effect=OSError('prepare failed')), \
                mock.patch.object(app.build, 'can_rebuild', return_value=True), \
                mock.patch.object(app.mods, 'code_waiting', side_effect=OSError('read failed')):
            self.play()
        self.assertEqual(self.lines, ['mods: the mods folder could not be put in order: prepare failed',
                                      'mods: read failed', f'play: {self.exe}'])

    def test_memory_warning_accept_and_refuse_with_identical_text_and_logs(self):
        for accepted in (True, False):
            with self.subTest(accepted=accepted):
                self.lines.clear()
                self.start_game.reset_mock()
                self.window.game_run = None
                with mock.patch.object(commit.winsys, 'system_commit_info', return_value=(18000, 27000, 16000)), \
                        mock.patch.object(commit.winsys, 'page_file_is_system_managed', return_value=False), \
                        mock.patch.object(commit, 'game_commit_need', return_value=24576), \
                        mock.patch.object(commit.winsys, 'confirm_play', return_value=accepted) as dialog:
                    self.play()
                self.assertEqual(dialog.call_args.args[0],
                                 'Windows has 9000 MiB of committed memory left (RAM plus page file). '
                                 'The game typically needs about 24576 MiB while you play.\n\n'
                                 'Close memory-heavy programs and turn on or enlarge the Windows page file. '
                                 'For a PC with 16 GB of RAM, use a fixed 32 GB page file (32768 MB). '
                                 'Restart Windows after changing the page file.\n'
                                 "Don't warn me again: turn off Warn About Low Commit in launcher Settings, or set "
                                 'mem_need_mb = 0 in nx2saria.ini.\n\nPlay anyway?')
                self.assertIs(dialog.call_args.args[1], self.window.root)
                self.assertEqual(self.lines[0], 'play memory: commit 18000.0 of 27000.0 MiB used, 9000.0 MiB left, '
                                 '16000.0 MiB RAM, game estimate 24576 MiB, page file fixed or off (warning)')
                self.assertEqual(self.lines[1], 'play memory: player chose to play anyway' if accepted else
                                 'play memory: player cancelled')
                self.assertEqual(self.lines[2], f'play: {self.exe}' if accepted else
                                 f'play: not started: the memory warning was declined ({self.exe})')
                self.assertEqual(self.start_game.call_count, int(accepted))

    def test_memory_probe_and_dialog_failure_allow_play(self):
        with mock.patch.object(commit.winsys, 'system_commit_info', side_effect=OSError('probe unavailable')):
            self.play()
        self.assertEqual(self.lines, ['play memory: commit check unavailable (probe unavailable); continuing',
                                      f'play: {self.exe}'])
        self.window.game_run = None
        self.lines.clear()
        with mock.patch.object(commit.winsys, 'system_commit_info', return_value=(18000, 27000, 16000)), \
                mock.patch.object(commit.winsys, 'page_file_is_system_managed', return_value=False), \
                mock.patch.object(commit, 'game_commit_need', return_value=float('inf')), \
                mock.patch.object(commit.winsys, 'confirm_play', side_effect=OSError('dialog unavailable')) as dialog:
            self.play()
        self.assertIn('needs about inf MiB', dialog.call_args.args[0])
        self.assertIn('game estimate inf MiB', self.lines[0])
        self.assertEqual(self.lines[1:], ['play memory: warning dialog unavailable (dialog unavailable); continuing',
                                          f'play: {self.exe}'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
