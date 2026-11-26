import base64
import collections
import math
import os
import runpy
import time
from pathlib import Path

from . import telemetry, updater, versions

SEND_SWITCH = 'NX2SARIA_ELECTRON_TEST_SEND_REPORTS'
PREVIEW_CHUNK = 16 * 1024
QUESTION_ANSWERS = ('send', "don't send", 'closed')


def summary_stamp(game):
    try:
        stat = (Path(game) / 'logs' / telemetry.SUMMARY).stat()
    except OSError:
        return None
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


def preview_frame(data, offset, example=False):
    if data is None or offset > len(data):
        return None
    return {'chunk': base64.b64encode(data[offset:offset + PREVIEW_CHUNK]).decode('ascii'),
            'size': len(data), 'example': example}


def installed_crash_offer(ctx):
    layout = ctx.layout
    try:
        active = layout.app
        if layout.dev:
            version = updater.current(layout)
            versions.parse(version)
            active = (layout.apps / version).resolve()
            if active.parent != layout.apps.resolve():
                return None
        source = active / 'launcher' / 'crash_offer.py'
        return runpy.run_path(str(source), run_name='launcher._installed_crash_offer') if source.is_file() else None
    except Exception:
        ctx.log('worker: crash offer unavailable')
        return None


class CrashOffers:
    def __init__(self, ctx, reporter, tasks, send_enabled):
        self.ctx = ctx
        self.reporter = reporter
        self.tasks = tasks
        self.send_enabled = send_enabled
        self.source = installed_crash_offer(ctx)
        self.offers = self.source['Offers']() if self.source else None
        self.current = None
        self.opened = None
        self.result = {'status': 'idle'}

    def preference(self):
        return self.source['on'](self.ctx.settings) if self.source else None

    def set_preference(self, value):
        if not self.source:
            return
        self.ctx.settings.save_preference(self.source['SETTING'], 'on' if value else 'off')
        self.source['log_state'](self.ctx.settings, self.ctx.log)

    def guarded(self):
        if self.opened is None:
            return 0
        left = self.source['GUARD_SECONDS'] - (time.monotonic() - self.opened)
        return max(0, math.ceil(left * 1000))

    def opened_view(self):
        _, data, sending = self.current
        return {'status': 'shown', 'kind': data['kind'][:48], 'message': data['message'][:900],
                'head_lines': sending.head_lines, 'tail_lines': sending.tail_lines, 'bytes': sending.log_bytes,
                'summary': sending.summary, 'whole': sending.whole, 'guard_ms': self.guarded(),
                'can_send': self.send_enabled and not self.reporter.closed()}

    def show(self, game):
        if not self.source or self.result['status'] == 'sending':
            return {'status': 'none'}
        if self.current:
            return self.opened_view()
        log = self.ctx.log
        data = self.source['pending'](game, self.ctx.settings, log)
        if data is None:
            return {'status': 'none'}
        if self.offers.refused:
            self.source['forget'](game)
            log(f'telemetry: crash log offer: not shown: the server refused one in this run (end: {data["kind"]})')
            return {'status': 'none'}
        sending = self.source['prepare'](self.reporter, game)
        if isinstance(sending, telemetry.Result):
            self.source['forget'](game)
            log(f'telemetry: crash log offer: not shown: {sending.reason} (end: {data["kind"]})')
            return {'status': 'none'}
        self.offers.shown += 1
        log(f'telemetry: crash log offer: shown (end: {data["kind"]}; {sending.head_lines} + {sending.tail_lines} '
            f'lines, {sending.log_bytes} bytes of log)')
        self.current = (game, data, sending)
        self.opened = time.monotonic()
        return self.opened_view()

    def preview(self, offset):
        return preview_frame(self.current[2].request.data, offset) if self.current else None

    def choose(self, action, window_closed=False):
        if not self.current:
            return {'status': 'none'}
        if not window_closed and self.guarded():
            return {'status': 'guarded'}
        if action == 'send' and (not self.send_enabled or self.reporter.closed()):
            return {'status': 'disabled'}
        game, data, sending = self.current
        self.current = self.opened = None
        current = self.source['pending'](game, self.ctx.settings, self.ctx.log)
        if not current or current['log'] != data['log']:
            return {'status': 'none'}
        self.source['forget'](game)
        if action != 'send':
            self.offers.declined += 1
            how = "don't send" if action == 'decline' else action
            self.ctx.log(f'telemetry: crash log offer: declined ({how}; end: {data["kind"]}; {self.offers.counts()})')
            return {'status': 'comment' if action == 'comment' else 'declined'}

        def done(answer):
            try:
                result = self.source['finish'](self.reporter, self.offers, sending, answer)
            except (OSError, ValueError):
                self.ctx.log('telemetry: Electron could not save the crash log result')
                result = telemetry.Result(False, 'could not save the server status')
            self.result = {'status': 'ok'} if result.ok else {'status': 'failed', 'reason': result.reason}

        self.result = {'status': 'sending'}
        self.tasks.start(lambda: self.reporter.post(sending.request), done)
        return self.result


class Reports:
    def __init__(self, ctx):
        self.log = ctx.log
        self.reporter = telemetry.Reporter(ctx.settings, ctx.version, self.log)
        self.send_enabled = os.environ.get(SEND_SWITCH) == '1'
        self.tasks = telemetry.Tasks()
        self.crash = CrashOffers(ctx, self.reporter, self.tasks, self.send_enabled)
        self.started = False
        self.question = False
        self.deadline = None
        self.notice = False
        self.deferred = False
        self.preview_data = None
        self.preview_example = False
        self.sessions = collections.deque()
        self.session_request = None
        self.session_result = None
        self.comment_result = None

    @property
    def available(self):
        return bool(self.reporter.url)

    @property
    def busy(self):
        return bool(self.tasks.running or self.session_request or self.sessions)

    def view(self):
        return {'available': self.available, 'send_enabled': self.send_enabled, 'question': self.question,
                'seconds': max(0, math.ceil(self.deadline - time.monotonic())) if self.deadline is not None else None,
                'consent': 'on' if self.reporter.settings.get('telemetry') == 'on' else 'off',
                'notice': self.notice, 'closed': self.reporter.closed(), 'text_limit': telemetry.TEXT_LIMIT,
                'crash_ask': self.crash.preference()}

    def home(self, game, after):
        if self.started or not self.available:
            return
        self.started = True
        if self.crash.source:
            self.crash.source['log_state'](self.reporter.settings, self.log)
        telemetry.synthesize_crash(game, self.log)
        self.send_session(game)
        self.notice = self._persist(self.reporter.take_notice)
        if self.reporter.question_due():
            self.question = True
            if after:
                self.log(f'telemetry: the question opens after {after}, without its countdown')
            else:
                self.deadline = time.monotonic() + telemetry.QUESTION_SECONDS

    def touch(self):
        if self.question and self.deadline is not None:
            self.deadline = None
            self.log("telemetry: the question's countdown stopped at the first input")

    def _persist(self, action, *args):
        settings = self.reporter.settings
        previous = dict(settings)
        try:
            return action(*args)
        except (OSError, ValueError):
            settings.clear()
            settings.update(previous)
            raise

    def answer(self, game, value):
        if not self.question:
            return
        self._persist(self.reporter.answer_question, value)
        self.question = False
        self.deadline = None
        self.notice = value in ('closed', 'timeout')
        telemetry.mark_sent(game)

    def tick(self, game):
        if self.deadline is not None and time.monotonic() >= self.deadline:
            self.deadline = None
            try:
                self.answer(game, 'timeout')
            except (OSError, ValueError):
                self.log('telemetry: Electron could not save the session reports answer')

    def set_consent(self, game, value):
        self._persist(self.reporter.set_consent, value)
        if value:
            self.send_session(game)
        else:
            self.sessions.clear()

    def preview(self, game, offset):
        if offset == 0:
            self.touch()
            summary = telemetry.pending_summary(game)
            self.preview_example = summary is None
            self.preview_data = telemetry.body(telemetry.payload(summary or telemetry.example_summary(),
                                                                 self.reporter.version))
        return preview_frame(self.preview_data, offset, example=self.preview_example)

    def send_session(self, game):
        if not self.available or self.reporter.settings.get('telemetry') != 'on' or not game:
            return
        stamp = summary_stamp(game)
        if stamp is None:
            return
        if not self.send_enabled:
            if not self.deferred:
                self.log('telemetry: Electron test sending is off; session summary left for the installed launcher')
                self.deferred = True
            return
        identity = (game, stamp)
        if (self.session_request and self.session_request[:2] == identity
                or any(entry[:2] == identity for entry in self.sessions)):
            return
        req = self.reporter.prepare_session(game)
        if isinstance(req, telemetry.Result) or summary_stamp(game) != stamp:
            return
        self.sessions.append((game, stamp, req))
        self._start_session()

    def _start_session(self):
        if self.session_request or not self.sessions:
            return
        if self.reporter.closed() or self.reporter.settings.get('telemetry') != 'on':
            self.sessions.clear()
            return
        self.session_request = self.sessions.popleft()
        req = self.session_request[2]
        self.tasks.start(lambda: self.reporter.post(req), self._answer_session)

    def _answer_session(self, answer):
        self.session_result = answer

    def poll(self, game_active=False):
        self.tasks.poll()
        if not self.session_result or game_active:
            return
        game, stamp, req = self.session_request
        answer = self.session_result
        self.session_request = self.session_result = None
        if answer[0] == 410 or summary_stamp(game) == stamp:
            try:
                self.reporter.finish_session(game, req, answer)
            except (OSError, ValueError):
                self.log('telemetry: Electron could not save the session report result')
        else:
            status = f'HTTP {answer[0]}' if answer[0] is not None else 'unavailable'
            self.log(f'telemetry: session report finished ({status}); a newer session summary was left intact')
        self._start_session()

    def flush(self):
        until = time.monotonic() + 15
        while self.busy and time.monotonic() < until:
            self.poll()
            if self.busy:
                time.sleep(0.02)
        self.poll()

    def comment(self, game, text, attach_log):
        if not self.send_enabled:
            return {'status': 'failed', 'reason': 'test_sending_disabled'}
        if self.comment_result is not None and self.comment_result['status'] == 'sending':
            return self.comment_result
        req = self.reporter.prepare_feedback(text, attach_log, game)
        if isinstance(req, telemetry.Result):
            return {'status': 'failed', 'reason': req.reason}
        self.comment_result = {'status': 'sending'}

        def done(answer):
            try:
                result = self.reporter.finish_feedback(req, answer)
            except (OSError, ValueError):
                self.log('telemetry: Electron could not save the comment result')
                result = telemetry.Result(False, 'could not save the server status')
            self.comment_result = {'status': 'ok'} if result.ok else {'status': 'failed', 'reason': result.reason}

        self.tasks.start(lambda: self.reporter.post(req), done)
        return self.comment_result

    def comment_status(self):
        return self.comment_result or {'status': 'idle'}
