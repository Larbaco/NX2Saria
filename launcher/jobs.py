import collections
import json
import os
import threading
import time
import traceback

from . import build, feed, gamefiles, locks, net, paths, toolchain, updater, versions, winsys
from .context import shorten

RECHECK = 3600
FORCED_CHECK_PAUSE = 60

PENDING, RUNNING, DONE, SKIPPED, FAILED = 'pending', 'running', 'done', 'skipped', 'failed'

PLAIN_FAILURES = (feed.FeedError, build.BuildError, locks.Busy)


MARKER = 'job-running.json'


def interrupted(ctx):
    path = ctx.layout.data / MARKER
    try:
        with open(path, encoding='utf-8') as f:
            left = json.load(f)
    except (OSError, ValueError):
        left = None
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass
    return left if isinstance(left, dict) and left.get('task') else None


def running_job(ctx):
    try:
        with open(ctx.layout.data / MARKER, encoding='utf-8') as f:
            job = json.load(f)
    except (OSError, ValueError):
        return None
    return job if isinstance(job, dict) and job.get('task') else None


def failure_line(e):
    cause = e.__cause__
    return f'{type(e).__name__}: {e}' + (f' ({type(cause).__name__}: {cause})' if cause else '')

STEPS = {
    'tools': ('Gather Build Tools', 'Downloading the compiler and libraries the build uses. This happens once and '
                                    'takes about 170 MB.'),
    'tools_update': ('Update Build Tools', 'Bringing the compiler and libraries to the versions this release was '
                                           'tested with.'),
    'check': ('Inspect Your Game', 'Making sure the dump is Ocarina of Time 1.1.1.'),
    'exefs': ('Unpack the Program', "Unpacking the game's program files."),
    'romfs': ('Pack the Game Data', 'Checking every game file against the dump and packing them into one image.'),
    'lift': ('Translate the Code', "Each function of your game's program becomes C code for the compiler."),
    'compile': ('Compile', 'Compiling the translated code and the port into a Windows program.'),
    'compile_changed': ('Compile Changed Files', 'Compiling the files this version changed. Files that did not '
                                                 'change are kept from the last build.'),
    'shaders': ('Prepare the Shaders', "Building the game's known shaders for your graphics card ahead of time, so "
                                       'play stops for them far less often.'),
    'prompts': ('Draw the Button Prompts', 'Drawing the Xbox, PlayStation, Steam Deck and keyboard and mouse button prompts the '
                                           "game's Controls menu offers under Button prompts."),
    'scan': ('Check the Game Files', 'Decompressing every game file once to find damage, and standing in for damaged '
                                     'textures so the world still loads.'),
    'ready': ('Ready to Play', 'The game is built.'),
    'download': ('Download {ver}', 'Downloading the new version of NX2Saria.'),
    'verify': ('Verify the Download', 'Checking the download against its checksum.'),
    'swap': ('Swap In the New Build', 'Linking the new build. It replaces the old one only once it is complete.'),
}
WEIGHTS = {'tools': 12, 'tools_update': 4, 'check': 1, 'exefs': 2, 'romfs': 14, 'lift': 14, 'compile': 55,
           'compile_changed': 55, 'shaders': 18, 'prompts': 4, 'scan': 5, 'ready': 0, 'download': 8, 'verify': 1, 'swap': 6}


SCAN_EXPECTED = frozenset((
    'Tex/CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx.zs',
    'Tex/RBake_Scene_GanonStage_G_2_1_d0_t0.bntx.zs',
    'Tex/Rock_WaterTemple_Gate_M_01_Nrm.bntx.zs',
    'gsys/EnvObjSetMgrParam/Artwork.gsys__EnvObjSetMgrParam.bgyml',
))


def scan_summary(damaged, stood_in):
    n = len(damaged)
    if not n:
        return '', ''
    files = '1 game file' if n == 1 else f'{n} game files'
    stand = ('none got a stand-in' if not stood_in else '1 texture got a stand-in' if stood_in == 1 else
             f'{stood_in} textures got a stand-in')
    value = f'{n} Flagged, {stood_in} Stand-In' + ('' if stood_in == 1 else 's')
    if set(damaged) <= SCAN_EXPECTED:
        return value, (f'The file check flagged {files}, the same as in every copy of the game: {stand}, and nothing '
                       'needs doing.')
    return value, (f'The file check flagged {files} as damaged: {stand}, so what uses them still loads. '
                   'romfs-check.json in the game folder lists them.')


def prompt_rows(ctx):
    return ['prompts'] if build.has_prompt_art(ctx.layout) else []


def mmss(seconds):
    s = int(round(seconds))
    return f'{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}' if s >= 3600 else f'{s // 60}:{s % 60:02d}'


class Step:
    def __init__(self, sid, ver=''):
        label, desc = STEPS[sid]
        self.id, self.label, self.desc = sid, label.format(ver=ver), desc
        self.state, self.value, self.fraction, self.detail = PENDING, 'Waiting', 0.0, ''
        self.t0, self.seconds = None, 0.0
        self.pace = collections.deque(maxlen=400)


class Job:
    kind = 'job'
    title = ''
    claims = ()
    task = ''

    def __init__(self, ctx, step_ids, ver=''):
        self.ctx = ctx
        self.steps = [Step(s, ver) for s in step_ids]
        self.rev = 0
        self.cancel_flag = threading.Event()
        self.proc = None
        self.supervised_build = None
        self.result = None
        self.error = ''
        self.lines = collections.deque(maxlen=500)
        self.started = time.time()
        self.finished = None
        self.after = None
        self.note = ''
        self._lock = threading.RLock()

    def step(self, sid):
        for s in self.steps:
            if s.id == sid:
                return s
        return None

    def current(self):
        for s in self.steps:
            if s.state in (RUNNING, FAILED):
                return s
        return next((s for s in self.steps if s.state == PENDING), self.steps[-1])

    def overall(self):
        total = sum(WEIGHTS[s.id] for s in self.steps) or 1
        got = 0.0
        for s in self.steps:
            w = WEIGHTS[s.id]
            got += w if s.state in (DONE, SKIPPED) else w * s.fraction if s.state == RUNNING else 0
        return min(1.0, got / total)

    def eta(self):
        now = time.time()
        if now - self.started < 20:
            return None
        cur = next((s for s in self.steps if s.state == RUNNING), None)
        left = 0.0
        if cur:
            recent = [p for p in cur.pace if now - p[0] <= 60]
            if len(recent) < 2 or recent[-1][0] - recent[0][0] < 10 or recent[-1][1] <= recent[0][1]:
                return None
            rate = (recent[-1][1] - recent[0][1]) / (recent[-1][0] - recent[0][0])
            left = (1 - cur.fraction) / rate
        done_w = sum(WEIGHTS[s.id] for s in self.steps if s.state == DONE and s.seconds > 0)
        done_s = sum(s.seconds for s in self.steps if s.state == DONE and s.seconds > 0)
        todo_w = sum(WEIGHTS[s.id] for s in self.steps if s.state == PENDING)
        if todo_w:
            if not done_w:
                return None
            left += todo_w * done_s / done_w
        return left

    def elapsed(self):
        return (self.finished or time.time()) - self.started

    def locked(self):
        return self._lock

    def _touch(self):
        with self._lock:
            self.rev += 1

    def log(self, line):
        line = shorten(str(line))
        with self._lock:
            self.lines.append(line)
        self.ctx.log(line)
        self._touch()

    def last_lines(self, n):
        with self._lock:
            return list(self.lines)[-n:]

    def begin(self, sid, detail=''):
        with self._lock:
            s = self.step(sid)
            if s and s.state != RUNNING:
                s.state, s.fraction, s.detail, s.t0, s.value = RUNNING, 0.0, detail, time.time(), '0%'
                s.pace.clear()
                self._touch()

    def progress(self, sid, fraction, detail=''):
        with self._lock:
            s = self.step(sid)
            if s:
                if s.state == PENDING:
                    self.begin(sid)
                s.fraction = max(0.0, min(1.0, fraction))
                s.pace.append((time.time(), s.fraction))
                s.value = f'{int(s.fraction * 100)}%'
                if detail:
                    s.detail = detail
                self._touch()

    def finish(self, sid, value=None):
        with self._lock:
            s = self.step(sid)
            if s:
                s.seconds = time.time() - s.t0 if s.t0 else 0.0
                s.state, s.fraction = DONE, 1.0
                s.value = value or f'Done · {mmss(s.seconds)}'
                self._touch()

    def skip(self, sid, value='Unchanged'):
        with self._lock:
            s = self.step(sid)
            if s:
                s.state, s.value = SKIPPED, value
                self._touch()

    def cancelled(self):
        return self.cancel_flag.is_set()

    def cancel(self):
        self.cancel_flag.set()
        winsys.keep_awake(False, self.log)
        if self.proc:
            self.proc.cancel()
        self._touch()

    def start(self):
        threading.Thread(target=self._main, name=f'job-{self.kind}', daemon=True).start()
        return self

    def run_inline(self):
        self._main()
        return self

    def doing(self):
        return self.task or f'a {self.kind} job'

    def _hold(self):
        if not self.claims:
            return None, None
        layout = self.ctx.layout
        try:
            file = locks.FileLock(layout.instance_lock, self.doing(), where=layout.root)
        except locks.Busy as e:
            raise locks.Busy(f'{e}. Close it, or wait until it is done, and try again') from None
        try:
            return file, locks.claim(layout.root, self.claims, self.doing())
        except locks.Busy:
            file.release()
            raise

    def _mark(self):
        if 'game' not in self.claims:
            return False
        try:
            path = self.ctx.layout.data / MARKER
            tmp = path.with_name(path.name + '.tmp')
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump({'task': self.doing(), 'kind': self.kind, 'game_dir': str(getattr(self, 'game_dir', '')),
                           'started': time.time()}, f)
            os.replace(tmp, path)
            return True
        except OSError:
            return False

    def _main(self):
        file = claimed = None
        marked = False
        try:
            file, claimed = self._hold()
            marked = self._mark()
            self.run()
            self.result = 'ok'
        except net.Cancelled:
            self.result = 'cancelled'
        except Exception as e:
            if self.cancelled():
                self.result = 'cancelled'
            else:
                self.result, self.error = 'failed', str(e) or type(e).__name__
                self.ctx.log(failure_line(e) if isinstance(e, PLAIN_FAILURES) else traceback.format_exc())
        finally:
            with self._lock:
                for s in self.steps:
                    if s.state == RUNNING:
                        s.state = FAILED if self.result == 'failed' else PENDING
                        s.value = 'Stopped' if self.result == 'failed' else 'Cancelled'
            if marked and not (self.supervised_build and self.supervised_build.needs_recovery):
                try:
                    (self.ctx.layout.data / MARKER).unlink(missing_ok=True)
                except OSError:
                    pass
                else:
                    if self.supervised_build:
                        self.supervised_build.clean()
            locks.release_claim(claimed)
            if file:
                file.release()
            self.finished = time.time()
            self._touch()

    def run(self):
        raise NotImplementedError

    def note_scan(self, game_dir):
        s = self.step('scan')
        found = build.scan_report(game_dir)
        if not s or s.state not in (DONE, SKIPPED) or not found:
            return
        value, note = scan_summary(*found)
        if not value:
            return
        if s.state == DONE:
            s.value = value
            self.note = note
            self.ctx.log(f'file check: {note}')
        elif s.value == 'Unchanged':
            s.value = f'Unchanged · {value}'
        self._touch()

    def ensure_tools(self, sid):
        layout = self.ctx.layout
        self.begin(sid)
        missing = toolchain.zstd_missing(layout)
        if missing:
            raise toolchain.ToolchainError(missing)
        if os.name != 'nt':
            from .platform.linux import ensure_toolchain
            ensure_toolchain(layout, cancel=self.cancelled, log=self.log, version=self.ctx.version)
            self.skip(sid, 'System tools ready')
            return
        lock = toolchain.load_lock(layout, self.ctx.version)
        if toolchain.is_current(layout, lock):
            have_tools = True
        else:
            have_tools = False

            def prog(phase, done, total):
                if phase == 'download':
                    self.progress(sid, 0.9 * done / max(total, 1),
                                  f'{done / 1e6:.0f} of {total / 1e6:.0f} MB downloaded')
                else:
                    self.progress(sid, 0.9 + 0.1 * done / max(total, 1), f'Unpacking {done} of {total} packages')
            toolchain.install(layout, lock, progress=prog, cancel=self.cancelled, log=self.log,
                              version=self.ctx.version)
        toolchain.ensure_zbic(layout, cancel=self.cancelled, log=self.log, version=self.ctx.version)
        if have_tools:
            self.skip(sid, 'Unchanged' if sid == 'tools_update' else 'Ready')
        else:
            self.finish(sid)

    def make_game(self, args, step_map, link_step=None):
        def on_event(ev):
            kind = ev.get('ev')
            sid = step_map.get(ev.get('step'))
            if kind == 'step' and sid:
                st = ev.get('state')
                if st == 'start':
                    self.begin(sid)
                elif st == 'done':
                    if sid == 'check':
                        self.finish(sid, 'Version 1.1.1')
                    elif link_step and sid in ('compile', 'compile_changed'):
                        self.finish(sid)
                        self.finish(link_step)
                    else:
                        self.finish(sid)
                elif st == 'skip':
                    self.skip(sid, 'Kept' if sid in ('exefs', 'romfs') else 'Unchanged')
                    if link_step and sid in ('compile', 'compile_changed'):
                        self.skip(link_step, 'Unchanged')
            elif kind == 'progress' and sid and 'fraction' in ev:
                self.progress(sid, float(ev.get('fraction') or 0), ev.get('detail', ''))
            elif kind == 'progress' and sid:
                done, total, unit = ev.get('done', 0), max(ev.get('total', 1), 1), ev.get('unit', '')
                if unit == 'bytes':
                    detail = f'{done / 1e9:.1f} of {total / 1e9:.1f} GB'
                else:
                    detail = f'{done:,} of {total:,} {unit}'
                self.progress(sid, done / total, detail)
            elif kind == 'phase' and ev.get('phase') == 'link':
                if link_step:
                    s = self.step(step_map.get('compile'))
                    if s and s.state == RUNNING:
                        self.finish(s.id)
                    self.begin(link_step, 'Linking')
                else:
                    self.progress(sid or 'compile', 0.99, 'Linking')
            elif kind == 'error':
                self.error = ev.get('message', '')

        if self.cancelled():
            raise net.Cancelled()
        layout = self.ctx.layout
        command = build.command(layout, args)
        if getattr(self.ctx, 'electron_worker', False) is True:
            self.proc = self.supervised_build = build.SupervisedRun(layout, layout.data / MARKER, command, on_event, self.log)
        else:
            self.proc = build.Run(command, on_event, self.log)
        if self.cancelled():
            self.proc.cancel()
        code = self.proc.wait()
        err = self.proc.error
        self.proc = None
        if self.cancelled():
            raise net.Cancelled()
        if code:
            raise build.BuildError(err or f'the build stopped (exit code {code})')


class InstallJob(Job):
    kind = 'install'
    title = 'Installing'
    claims = ('game',)
    task = 'the installation of the game'

    def __init__(self, ctx, dump, game_dir):
        super().__init__(ctx, ['tools', 'check', 'exefs', 'romfs', 'lift', 'compile', 'shaders', *prompt_rows(ctx),
                               'scan', 'ready'])
        self.dump, self.game_dir = str(dump), str(game_dir)

    def args(self):
        if build.can_rebuild(self.game_dir) and build.unpacked(self.game_dir):
            return build.rebuild_args(self.ctx.layout, self.game_dir, retune=True)
        return build.install_args(self.ctx.layout, self.dump, self.game_dir)

    def run(self):
        too_long = paths.long_path_problem(self.game_dir)
        if too_long:
            raise build.BuildError(too_long)
        self.ensure_tools('tools')
        self.make_game(self.args(),
                       {s: s for s in ('check', 'exefs', 'romfs', 'lift', 'compile', 'shaders', 'prompts', 'scan')})
        for sid in ('shaders', 'prompts', 'scan'):
            if self.step(sid) and self.step(sid).state == PENDING:
                self.skip(sid)
        self.note_scan(self.game_dir)
        self.finish('ready', 'Ready')
        try:
            gamefiles.extract_fonts(self.ctx.layout, self.game_dir)
        except Exception as e:
            self.log(f'fonts: {e}')


class RebuildJob(Job):
    kind = 'rebuild'
    title = 'Rebuilding'
    claims = ('game',)
    task = 'the rebuild of the game'
    retune = True

    def __init__(self, ctx, game_dir, dump=''):
        super().__init__(ctx, ['tools_update', 'lift', 'compile_changed', 'swap', 'shaders', *prompt_rows(ctx), 'scan'])
        self.game_dir, self.dump = str(game_dir), dump

    def run(self):
        self.ensure_tools('tools_update')
        layout = self.ctx.layout
        if build.game_running(self.game_dir, log=self.ctx.log):
            raise build.BuildError('the game is running: close it and try again')
        if build.can_rebuild(self.game_dir):
            args = build.rebuild_args(layout, self.game_dir, self.retune)
        elif self.dump:
            args = build.install_args(layout, self.dump, self.game_dir)
        else:
            raise build.BuildError('the game folder has no earlier build: install the game again')
        self.make_game(args, {'lift': 'lift', 'compile': 'compile_changed', 'shaders': 'shaders', 'prompts': 'prompts', 'scan': 'scan'},
                       link_step='swap')
        for sid in ('lift', 'compile_changed', 'swap', 'shaders', 'prompts', 'scan'):
            if self.step(sid) and self.step(sid).state == PENDING:
                self.skip(sid)
        self.note_scan(self.game_dir)


class ModsRebuildJob(RebuildJob):
    title = 'Building Your Mods In'
    retune = False

    def __init__(self, ctx, game_dir):
        super().__init__(ctx, game_dir)
        self.after = 'play'


class UpdateDownloadJob(Job):
    kind = 'update'
    claims = ('app',)

    def __init__(self, ctx, release):
        ver = versions.display(release.version)
        super().__init__(ctx, ['download', 'verify', 'tools_update', 'lift', 'compile_changed', 'swap', 'shaders'], ver)
        self.release, self.ver = release, ver
        self.title = f'Updating to {ver}'
        self.task = f'the update to {ver}'

    def run(self):
        layout = self.ctx.layout
        self.begin('download')
        prog = lambda d, t: self.progress('download', d / max(t, 1), f'{d / 1e6:.1f} of {t / 1e6:.1f} MB')  # noqa: E731
        linux_runtime = updater._linux_runtime(layout)
        if linux_runtime:
            folder = updater.stage(layout, self.release, progress=prog, cancel=self.cancelled,
                                   version=self.ctx.version, log=self.log)
        else:
            folder = updater.stage_app(layout, self.release, progress=prog, cancel=self.cancelled,
                                       version=self.ctx.version, log=self.log)
            if updater.runtime_needed(layout, self.release):
                updater.stage_runtime(layout, self.release, progress=prog, cancel=self.cancelled,
                                      version=self.ctx.version, log=self.log)
        self.finish('download')
        self.begin('verify')
        if not (folder / 'launcher' / 'start.pyw').is_file():
            raise updater.UpdateError(f'{folder} is incomplete')
        self.finish('verify', 'Checksum OK')
        if layout.dev:
            self.after = 'staged'
            return
        if linux_runtime:
            updater.install_staged(layout, self.ver, log=self.log)
        else:
            updater.switch_to(layout, self.ver)
        self.after = 'restart'


class ContinueUpdateJob(RebuildJob):
    retune = False
    kind = 'update'
    claims = ('game', 'app')

    def __init__(self, ctx, game_dir, old, dump=''):
        Job.__init__(self, ctx, ['download', 'verify', 'tools_update', 'lift', 'compile_changed', 'swap', 'shaders',
                                 *prompt_rows(ctx), 'scan'], ctx.layout.version)
        self.game_dir, self.dump, self.old = str(game_dir), dump, old
        self.title = f'Updating to {versions.display(ctx.layout.version)}'
        self.task = f'the update to {versions.display(ctx.layout.version)}'
        for sid, value in (('download', 'Done'), ('verify', 'Checksum OK')):
            self.step(sid).state, self.step(sid).value, self.step(sid).fraction = DONE, value, 1.0

    def run(self):
        if self.game_dir and build.can_rebuild(self.game_dir):
            RebuildJob.run(self)
        else:
            self.ensure_tools('tools_update')
            for sid in ('lift', 'compile_changed', 'swap', 'shaders', 'prompts', 'scan'):
                self.skip(sid, 'No game yet')
        updater.prune(self.ctx.layout)


class CheckJob(Job):
    kind = 'check'

    def __init__(self, ctx):
        super().__init__(ctx, [])
        self.release = None

    def run(self):
        s = self.ctx.settings
        self.release = feed.latest(feed.load(self.ctx.layout, s), s.get('channel', 'stable'),
                                   s.get('github_token', ''), self.ctx.version)

    def overall(self):
        return 0.0


def update_available(ctx, release):
    return bool(release) and versions.newer(release.version, ctx.layout.version)


class StageJob(Job):
    kind = 'stage'
    claims = ('app',)

    def __init__(self, ctx, release):
        ver = versions.display(release.version)
        super().__init__(ctx, ['download'], ver)
        self.release, self.ver = release, ver
        self.task = f'the background download of {ver}'

    def run(self):
        winsys.background_thread()
        self.begin('download')
        prog = lambda d, t: self.progress('download', d / max(t, 1), f'{d / 1e6:.1f} of {t / 1e6:.1f} MB')  # noqa: E731
        updater.stage(self.ctx.layout, self.release, progress=prog, cancel=self.cancelled, version=self.ctx.version,
                      log=self.log)
        self.finish('download')


class BackgroundUpdate:
    def __init__(self, ctx, restart, enabled=True):
        self.ctx, self.restart, self.enabled = ctx, restart, enabled
        self.job = None
        self.tried = ''
        self.asked = False
        self.waiting = False
        self.restarting = False
        self.error = ''

    def staging(self):
        return self.job is not None and self.job.result is None

    def ready(self, release=None):
        if not self.enabled:
            return None
        s = self.ctx.settings
        m = updater.staged(self.ctx.layout, s.get('channel', 'stable'), s.get('rolled_back', ''))
        if m and release and versions.newer(m['version'], release.version):
            return None
        return m

    def checked(self):
        self.tried = ''

    def poll(self, release, busy=False):
        j = self.job
        if j and j.result is not None and not getattr(j, 'seen', False):
            j.seen = True
            if j.result == 'ok':
                self.ctx.log(f'background update: {j.ver} is staged')
                return 'staged'
            self.ctx.log(f'background update: {j.ver} was not staged ({j.result}: {j.error}); it is tried again '
                         'after the next update check')
            return 'failed'
        if not self.enabled or busy or self.staging() or not release:
            return None
        ver = versions.display(release.version)
        if ver == self.tried:
            return None
        self.tried = ver
        if (not update_available(self.ctx, release) or ver == self.ctx.settings.get('rolled_back')
                or updater.staged_info(self.ctx.layout, ver)):
            return None
        self.ctx.log(f'background update: staging {ver}')
        self.job = StageJob(self.ctx, release).start()
        return 'started'

    def install(self, game_dir, release=None, why='', asked=False, running=False):
        if not self.enabled or not (asked or self.asked or self.ctx.settings.get('auto_install')):
            return ''
        m = self.ready(release)
        if not m:
            return ''
        ver = m['version']
        why_not = ('the game is running' if running or (game_dir and build.game_running(game_dir)) else
                   f'{self.job.ver} is downloading' if self.staging() else '')
        if why_not:
            if not self.waiting:
                self.ctx.log(f'background update: {ver} waits: {why_not}')
            self.waiting = True
            return ''
        old = versions.display(self.ctx.layout.version)
        try:
            updater.install_staged(self.ctx.layout, ver, log=self.ctx.log)
        except (OSError, updater.UpdateError) as e:
            self.error = str(e)
            self.waiting = False
            self.ctx.log(f'background update: {ver} could not be installed: {e}')
            return ''
        self.waiting = self.asked = False
        self.error = ''
        self.ctx.log(f'background update: installing {ver}' + (f' ({why})' if why else ''))
        try:
            self.restarting = bool(self.restart(['--continue-update', old]))
        except OSError as e:
            self.ctx.log(f'background update: the launcher could not restart: {e}')
            self.restarting = False
        return ver
