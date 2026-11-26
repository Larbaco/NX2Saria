import base64
import collections
import math
import os
import time
from pathlib import Path

from . import telemetry

SEND_SWITCH = 'NX2SARIA_ELECTRON_TEST_SEND_REPORTS'
PREVIEW_CHUNK = 16 * 1024
QUESTION_ANSWERS = ('send', "don't send", 'closed')


def summary_stamp(game):
    try:
        stat = (Path(game) / 'logs' / telemetry.SUMMARY).stat()
    except OSError:
        return None
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


class Reports:
    def __init__(self, ctx):
        self.log = ctx.log
        self.reporter = telemetry.Reporter(ctx.settings, ctx.version, self.log)
        self.send_enabled = os.environ.get(SEND_SWITCH) == '1'
        self.tasks = telemetry.Tasks()
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
                'notice': self.notice, 'closed': self.reporter.closed(), 'text_limit': telemetry.TEXT_LIMIT}

    def home(self, game, after):
        if self.started or not self.available:
            return
        self.started = True
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
        if self.preview_data is None or offset > len(self.preview_data):
            return None
        data = self.preview_data
        return {'chunk': base64.b64encode(data[offset:offset + PREVIEW_CHUNK]).decode('ascii'),
                'size': len(data), 'example': self.preview_example}

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
