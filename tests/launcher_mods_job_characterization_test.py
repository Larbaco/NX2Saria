import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from launcher.ui import app


class ModsJobCharacterizationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nx2-mods-job-')
        self.addCleanup(self.temp.cleanup)

    def make_window(self, result, keep_cache):
        window = mock.MagicMock()
        game = Path(self.temp.name) / 'game'
        cache = game / 'build-cache' / 'lift'
        cache.mkdir(parents=True, exist_ok=True)
        (cache / 'item').write_text('synthetic')
        window.settings = {'keep_cache': keep_cache, 'game_dir': str(game)}
        window.ctx.settings = window.settings
        window.drop_build_cache.side_effect = lambda: app.App.drop_build_cache(window)
        window.mods_failed = ['earlier']
        window.message = 'old message'
        window.job_return = 'home'
        window.job_back = 'settings'
        window.job = SimpleNamespace(kind='rebuild', result=result, after='play', error='', waiting=['Example'],
                                     doing=lambda: 'synthetic')
        window.home_row.return_value = 5
        return window

    def test_tick_observes_result_even_before_finished_is_set(self):
        window = self.make_window('ok', False)
        window.job.rev = 1
        window.job.finished = None
        window.job_rev = 0
        window.check = None
        window.screen = 'job'
        window.bg.poll.return_value = None
        app.App.tick(window)
        window.job_finished.assert_called_once_with()
        self.assertIsNone(window.job.finished)

    def test_success_drops_cache_then_resumes_play_when_cache_disabled(self):
        window = self.make_window('ok', False)
        events = []
        cache = Path(window.settings['game_dir']) / 'build-cache' / 'lift'
        def show(*args):
            self.assertFalse(cache.exists())
            events.append(('show', *args))
        window.show.side_effect = show
        window.play.side_effect = lambda **kwargs: events.append(('play', kwargs))
        with mock.patch.object(app.winsys, 'keep_awake'):
            app.App.job_finished(window)
        self.assertEqual(events, [('show', 'home', 5), ('play', {'built': True})])
        self.assertEqual(window.mods_failed, [])
        self.assertEqual(window.message, '')
        window.ctx.log.assert_called_once_with('job rebuild: ok ')

    def test_success_retains_cache_when_enabled(self):
        window = self.make_window('ok', True)
        with mock.patch.object(app.winsys, 'keep_awake'):
            app.App.job_finished(window)
        window.drop_build_cache.assert_not_called()
        window.play.assert_called_once_with(built=True)
        self.assertEqual(window.mods_failed, [])

    def test_cancelled_remembers_waiting_mods_and_returns_to_previous_screen(self):
        window = self.make_window('cancelled', False)
        with mock.patch.object(app.winsys, 'keep_awake'):
            app.App.job_finished(window)
        self.assertEqual(window.mods_failed, ['Example'])
        window.show.assert_called_once_with('settings')
        window.home_reports.assert_called_once_with('synthetic')
        window.play.assert_not_called()
        window.drop_build_cache.assert_not_called()

    def test_failed_remembers_waiting_mods_and_remains_on_job_screen(self):
        window = self.make_window('failed', False)
        with mock.patch.object(app.winsys, 'keep_awake'):
            app.App.job_finished(window)
        self.assertEqual(window.mods_failed, ['Example'])
        window.show.assert_called_once_with('job')
        window.play.assert_not_called()
        window.home_reports.assert_not_called()
        window.drop_build_cache.assert_not_called()


if __name__ == '__main__':
    unittest.main()
