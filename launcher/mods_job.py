import shutil
from pathlib import Path


def drop_build_cache(self):
    """Keep Build Cache off: delete the lifted C and the objects (about 1.5 GB) but keep state.json, which an
        update needs to rebuild without the dump; the next build then lifts and compiles everything again."""
    gd = self.settings.get('game_dir')
    if not gd:
        return
    cache = Path(gd) / 'build-cache'
    for name in ('lift', 'obj', 'gen'):
        shutil.rmtree(cache / name, ignore_errors=True)
    for name in ('link.rsp', 'link.key'):
        (cache / name).unlink(missing_ok=True)

def finish_mods_job(ctx, job, failed_mods, require_finished=False):
    if require_finished and job.finished is None:
        return 'pending', failed_mods
    if job.result == 'ok':
        if not ctx.settings.get('keep_cache'):
            drop_build_cache(ctx)
        return 'resume', []
    if job.result == 'cancelled':
        return 'cancelled', getattr(job, 'waiting', failed_mods)
    return 'failed', getattr(job, 'waiting', failed_mods)
