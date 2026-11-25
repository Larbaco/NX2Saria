import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from launcher import mods_job


class ModsJobCoreTest(unittest.TestCase):
    def test_worker_waits_for_finished_before_dropping_cache(self):
        with tempfile.TemporaryDirectory(prefix='nx2-mods-core-') as temp:
            game = Path(temp) / 'game'
            cache = game / 'build-cache' / 'lift'
            cache.mkdir(parents=True)
            (cache / 'item').write_text('synthetic')
            ctx = SimpleNamespace(settings={'game_dir': str(game), 'keep_cache': False})
            job = SimpleNamespace(result='ok', finished=None)
            self.assertEqual(mods_job.finish_mods_job(ctx, job, [], require_finished=True), ('pending', []))
            self.assertTrue(cache.is_dir())
            job.finished = 1
            self.assertEqual(mods_job.finish_mods_job(ctx, job, [], require_finished=True), ('resume', []))
            self.assertFalse(cache.exists())


if __name__ == '__main__':
    unittest.main()
