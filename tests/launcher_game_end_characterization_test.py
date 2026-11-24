import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from launcher.ui import app


class GameEndCharacterizationTest(unittest.TestCase):
    def setUp(self):
        self.events = []
        self.tmp = tempfile.TemporaryDirectory(prefix='nx2-end-')
        self.addCleanup(self.tmp.cleanup)
        self.game = Path(self.tmp.name) / 'game'
        (self.game / 'logs').mkdir(parents=True)
        self.game_log = self.game / 'logs' / 'nx2saria.log'
        self.window = mock.MagicMock()
        self.window.focus = 3
        self.window.message = ''
        self.window.install_staged.return_value = False
        self.window.after_game_reports.return_value = False
        self.window.root.deiconify.side_effect = lambda: self.events.append('restore')
        self.window.root.lift.side_effect = lambda: self.events.append('lift')
        self.window.root.focus_force.side_effect = lambda: self.events.append('focus')
        self.window.root.after.side_effect = lambda delay, callback: self.events.append(('timer', delay))
        self.window.ctx.log.side_effect = lambda text: self.events.append(('log', text))
        self.window.install_staged.side_effect = self.update
        self.window.after_game_reports.side_effect = lambda: self.events.append('reports') or False
        self.window.show.side_effect = lambda screen, focus: self.events.append(('screen', screen, focus))
        self.run = mock.Mock()
        self.run.game_dir = str(self.game)
        self.run.running.return_value = False
        self.run.end.return_value = app.GameEnd('the game ended with code 71', 'A fatal error.', '71', True)
        self.window.game_run = self.run
        self.telemetry = mock.patch.object(app.telemetry, 'enabled', return_value=True)
        self.crash = mock.patch.object(app.telemetry, 'synthesize_crash',
                                       side_effect=self.synthesize)
        self.telemetry.start()
        self.crash.start()
        self.addCleanup(self.telemetry.stop)
        self.addCleanup(self.crash.stop)

    def synthesize(self, game, log):
        self.assertFalse(self.game_log.exists())
        self.events.append(('crash', game))

    def update(self, why):
        if self.game_log.exists():
            self.assertIn('[launcher] the game ended with code 71 at ', self.game_log.read_text())
            self.events.append(('note', str(self.game), '71'))
        self.events.append(('update', why))
        return False

    def test_running_game_schedules_another_check_without_end_effects(self):
        self.run.running.return_value = True
        app.App.watch_game(self.window)
        self.assertEqual(self.events, [('timer', 1000)])
        self.assertIs(self.window.game_run, self.run)
        self.run.end.assert_not_called()

    def test_ended_game_restores_logs_reports_notes_and_then_returns_home(self):
        app.App.watch_game(self.window)
        self.assertIsNone(self.window.game_run)
        self.assertEqual(self.window.message, 'A fatal error.')
        self.assertEqual(self.events, ['restore', 'lift', 'focus', ('log', 'the game ended with code 71'),
                                       ('crash', str(self.game)), ('note', str(self.game), '71'),
                                       ('update', 'the game exited'), 'reports', ('screen', 'home', 3)])

    def test_staged_update_short_circuits_reports_and_home(self):
        self.window.install_staged.side_effect = lambda why: self.events.append(('update', why)) or True
        app.App.watch_game(self.window)
        self.assertEqual(self.events[-1], ('update', 'the game exited'))
        self.assertNotIn('reports', self.events)
        self.window.show.assert_not_called()

    def test_reports_short_circuit_home_after_update_is_checked(self):
        self.window.after_game_reports.side_effect = lambda: self.events.append('reports') or True
        app.App.watch_game(self.window)
        self.assertEqual(self.events[-2:], [('update', 'the game exited'), 'reports'])
        self.window.show.assert_not_called()

    def test_disabled_reports_and_empty_note_do_not_synthesize_or_write(self):
        self.run.end.return_value = app.GameEnd('the game ended with code 0', '', '', False)
        with mock.patch.object(app.telemetry, 'enabled', return_value=False), \
                mock.patch.object(app.telemetry, 'synthesize_crash') as crash:
            app.App.watch_game(self.window)
        crash.assert_not_called()
        self.assertFalse(self.game_log.exists())
        self.assertEqual(self.window.message, '')
        self.assertEqual(self.events, ['restore', 'lift', 'focus', ('log', 'the game ended with code 0'),
                                       ('update', 'the game exited'), 'reports', ('screen', 'home', 3)])

    def test_missing_run_restores_window_and_checks_update_and_reports(self):
        self.window.game_run = None
        app.App.watch_game(self.window)
        self.assertEqual(self.events, ['restore', 'lift', 'focus', ('update', 'the game exited'),
                                       'reports', ('screen', 'home', 3)])

    def test_focus_error_does_not_prevent_end_processing(self):
        self.window.root.focus_force.side_effect = app.tk.TclError('synthetic focus error')
        app.App.watch_game(self.window)
        self.assertIn(('log', 'the game ended with code 71'), self.events)
        self.assertEqual(self.events[-1], ('screen', 'home', 3))


if __name__ == '__main__':
    unittest.main(verbosity=2)
