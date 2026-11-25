"""Session reports and comments (docs/telemetry.md): what the launcher sends to the project's own server, and when.
The window asks about session reports at the first start of each version while they are not on (Reporter.question_due,
answer_question): Don't Send keeps them off; Send, closing the question and its countdown turn them on.

- A session report is the summary the game writes when it ends (logs/session-summary.json in the game folder,
  runtime side), sent with the install's random id and the launcher's version. Nothing is sent unless the settings'
  `telemetry` is "on": Reporter.prepare_session refuses everything else itself, so no caller can send without it.
- A comment is text the player wrote in Send a Comment, with the last session's summary and, when asked for, the last
  lines of the game's log. Its own Send button is the player's consent: it does not need `telemetry` on.
- Neither carries an id of the installation, and the server keeps no IP address: nothing sent can be deleted on
  request, because the server cannot tell whose it is. It deletes every report and comment after 180 days.

A run that ended in a hard crash writes no summary; the launcher makes one from the crash's FATAL line
(synthesize_crash), with every value it cannot know left null.

The server's address is TELEMETRY_URL, which NX2SARIA_TELEMETRY_URL replaces (a local server for tests). With no
address nothing is sent. No Tk here: the window (launcher/ui/app.py) runs the requests on a thread through Tasks.
"""
import collections
import json
import os
import re
import threading
import unicodedata
import urllib.error
from pathlib import Path

from . import net, versions
from .build import game_log_path, last_fatal
from .context import short_paths_on, shorten_always

# The server's address (telemetry/, the project's own Worker). Empty, the window shows nothing of session reports.
TELEMETRY_URL = 'https://nx2saria-telemetry.larbaco-ct.workers.dev'
URL_ENV = 'NX2SARIA_TELEMETRY_URL'
SCHEMA = 'nx2saria.session/1'
TIMEOUT = 10                   # seconds a request may take; a report is never worth a longer wait
QUESTION_SECONDS = 30          # the question after an update turns reports on after this long without any input
MAX_ERROR_LINES = 5            # launcher.log lines about failed requests in one launcher run
SESSION_LIMIT = 8 * 1024       # bytes of a session report's body the server takes
FEEDBACK_LIMIT = 40 * 1024     # ... of a comment's body
TEXT_LIMIT = 4000              # characters of a comment
TAIL_LINES = 200               # lines of the game's log a comment may carry
TAIL_LIMIT = 24 * 1024         # ... and their bytes
FATAL_LIMIT = 300              # characters of the FATAL line in a summary
LOG_TAIL_READ = 256 << 10      # bytes read from the end of the game's log for its last lines

SUMMARY = 'session-summary.json'
SENT = 'session-summary.sent.json'

VERSION_LINE = re.compile(r'version: NX2Saria (\S+), game (\d+\.\d+\.\d+)')


def server_url():
    """The server's address without a trailing slash: NX2SARIA_TELEMETRY_URL when set, else TELEMETRY_URL.
    NX2SARIA_TELEMETRY_URL=none is no server at all: nothing is asked, shown or sent (enabled)."""
    url = (os.environ.get(URL_ENV) or TELEMETRY_URL).strip().rstrip('/')
    return '' if url.lower() == 'none' else url


def enabled():
    """Whether this launcher has a server to send to. Without one the window shows nothing of session reports or
    comments: no question, no Session Reports screen, so no build without a server asks what it cannot do."""
    return bool(server_url())


def os_family():
    return 'windows' if os.name == 'nt' else 'linux'


def empty_summary(version, game_version):
    """A summary with every leaf null but the ones the format requires (docs/telemetry.md)."""
    return {
        'schema': SCHEMA, 'version': version, 'game_version': game_version,
        'os': {'family': None, 'name': None, 'wine': None},
        'cpu': {'name': None, 'threads': None},
        'ram_mb': None,
        'gpu': {'name': None, 'vendor': None, 'device': None, 'driver': None, 'vram_mb': None},
        'display': {'output': None, 'render_scale': None, 'fullscreen': None, 'game_aspect': None, 'picture': None},
        'settings': {'preset': None, 'upscaler': None, 'taa': None, 'dlss': None, 'framegen': None,
                     'fps_limit': None, 'vk_present': None},
        'session': {'duration_s': None, 'frames': None, 'end': None},
        'frametime': {'fps_mean': None, 'fps_low_1pct': None, 'worst_ms': None, 'over_25ms': None,
                      'over_100ms': None},
        'startup': {'shader_prewarm_ms': None, 'shaders_translated': None, 'pipelines_built': None,
                    'pipeline_build_ms': None},
        'patches': None,
        'mods': {'count': None, 'code': None},
        'fatal': None,
    }


def example_summary():
    """The summary "See What Is Sent" shows before the game has written one: made-up values of a plausible PC."""
    s = empty_summary('0.19.0', '1.1.1')
    s.update(os={'family': 'windows', 'name': 'Windows 11 (build 26100)', 'wine': False},
             cpu={'name': 'AMD Ryzen 5 5600 6-Core Processor', 'threads': 12}, ram_mb=16384,
             gpu={'name': 'NVIDIA GeForce RTX 3060', 'vendor': '0x10de', 'device': '0x2504',
                  'driver': 'NVIDIA 581.57', 'vram_mb': 12288},
             display={'output': '1920x1080', 'render_scale': 'game', 'fullscreen': True, 'game_aspect': '16:9',
                      'picture': '2560x1440'},
             settings={'preset': 'high', 'upscaler': 'fsr', 'taa': 'dlss', 'dlss': 'dlaa', 'framegen': 'off',
                       'fps_limit': 60, 'vk_present': 'fifo'},
             session={'duration_s': 1834, 'frames': 109800, 'end': 'normal'},
             frametime={'fps_mean': 59.9, 'fps_low_1pct': 52.1, 'worst_ms': 88.4, 'over_25ms': 14, 'over_100ms': 0},
             startup={'shader_prewarm_ms': 5400, 'shaders_translated': 120, 'pipelines_built': 900,
                      'pipeline_build_ms': 3100},
             patches=['fps60', 'ostep', 'zstd'], mods={'count': 1, 'code': 0})
    return s


def printable(text, keep=''):
    """text without control characters (those in keep stay): the server refuses them in a report."""
    return ''.join(ch for ch in text if ch in keep or unicodedata.category(ch) not in ('Cc', 'Cs'))


# ---------------------------------------------------------------- the summary files

def _logs(game_dir):
    return Path(game_dir) / 'logs'


def _read_summary(path):
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) and data.get('schema') == SCHEMA else None


def pending_summary(game_dir):
    """The summary of logs/session-summary.json, which nothing has sent or declined yet, or None."""
    if not game_dir:
        return None
    return _read_summary(_logs(game_dir) / SUMMARY)


def last_summary(game_dir, settings=None):
    """The last session's summary for a comment: the pending one, else the one sent or declined last. None when the
    player turned session reports off (settings' telemetry "off"): a comment never carries a report they said no to."""
    if not game_dir or (settings is not None and settings.get('telemetry') == 'off'):
        return None
    return pending_summary(game_dir) or _read_summary(_logs(game_dir) / SENT)


def mark_sent(game_dir):
    """The pending summary becomes session-summary.sent.json (replacing the one before it): sent, refused by the
    server, or declined by the player. It is not offered again."""
    logs = _logs(game_dir)
    try:
        os.replace(logs / SUMMARY, logs / SENT)
    except OSError:
        pass


def log_started(game_dir):
    """When the current game log was begun, as near as the files tell: the game moves the log before it to
    nx2saria.1.log when it starts (runtime/main.c log_open), so that file's last write came before this run. 0 when
    there is no such file (the first run)."""
    log = game_log_path(game_dir)
    try:
        return log.with_name(log.stem + '.1' + log.suffix).stat().st_mtime
    except OSError:
        return 0.0


def _covered(game_dir, since):
    """Whether a summary (pending or already sent) was written during the run whose log began at since."""
    for name in (SUMMARY, SENT):
        try:
            if (_logs(game_dir) / name).stat().st_mtime > since:
                return True
        except OSError:
            pass
    return False


def log_version(game_dir):
    """(NX2Saria version, game version) of the game log's "version:" line (runtime/main.c main_run), or None."""
    try:
        with open(game_log_path(game_dir), 'rb') as f:
            head = f.read(64 << 10).decode('utf-8', 'replace')
    except OSError:
        return None
    m = VERSION_LINE.search(head)
    return (versions.display(m.group(1)), m.group(2)) if m else None


def synthesize_crash(game_dir, log=None):
    """A summary for a run that ended in a hard crash, which writes none (runtime/main.c crash_handler exits with 71
    after its FATAL lines): when the game's log holds a FATAL line of its last run (build.last_fatal) and no summary
    was written during that run, logs/session-summary.json is made with session.end "crash", the FATAL line with the
    account's folders written short and cut to 300 characters, the versions of the log's "version:" line and this
    PC's os.family; everything else null, nothing taken from an earlier summary. Without a version line nothing is
    made. Returns the summary made, or None.

    It must run before the launcher adds its own line to the log (app.py note_exit_in_log), which ends last_fatal's
    search."""
    if not game_dir:
        return None
    fatal = last_fatal(game_dir)
    if not fatal or _covered(game_dir, log_started(game_dir)):
        return None
    ver = log_version(game_dir)
    if not ver:
        return None
    s = empty_summary(*ver)
    s['os']['family'] = os_family()
    s['session']['end'] = 'crash'
    s['fatal'] = printable(shorten_always(fatal)).strip()[:FATAL_LIMIT]
    path = _logs(game_dir) / SUMMARY
    tmp = path.with_name(path.name + '.tmp')
    try:
        tmp.write_text(json.dumps(s, indent=1), encoding='utf-8')
        os.replace(tmp, path)
    except OSError as e:
        if log:
            log(f'telemetry: the crash summary could not be written: {e}')
        return None
    if log:
        log('telemetry: the last run ended in a crash; a summary was made from its FATAL line')
    return s


class Line(str):
    changed = False


def sent_line(raw):
    short = shorten_always(raw)
    line = Line(printable(short, '\t'))
    line.changed = short != raw
    return line


def shortened_note(lines):
    changed = sum(1 for ln in lines if getattr(ln, 'changed', False))
    if not changed and short_paths_on():
        return ''
    why = 'the log had them in full' if short_paths_on() else 'NX2SARIA_LOG_SHORT_PATHS=0 leaves them in full there'
    return (f'telemetry: personal folders written short in {changed} of {len(lines)} log lines for sending; the log '
            f'on disk is not changed ({why})')


def log_tail_lines(game_dir, lines=TAIL_LINES, limit=TAIL_LIMIT):
    try:
        with open(game_log_path(game_dir), 'rb') as f:
            f.seek(max(0, f.seek(0, 2) - LOG_TAIL_READ))
            text = f.read().decode('utf-8', 'replace')
    except OSError:
        return []
    kept = [sent_line(ln) for ln in text.splitlines()[-lines:]]
    total = 0
    out = []
    for ln in reversed(kept):
        size = len(ln.encode('utf-8')) + 1
        if total + size > limit:
            break
        out.append(ln)
        total += size
    out.reverse()
    return out


def log_tail(game_dir, lines=TAIL_LINES, limit=TAIL_LIMIT):
    """The last lines of the game's log for a comment: at most lines lines and limit bytes (UTF-8), the last ones
    kept, the account's folders written short. None when there is no log."""
    out = log_tail_lines(game_dir, lines, limit)
    return '\n'.join(out) + '\n' if out else None


# ---------------------------------------------------------------- what is sent

def payload(summary, launcher_version):
    """The session report exactly as it is sent: what "See What Is Sent" shows and send_session posts. It carries no
    id of the installation: nothing in it tells two reports of one PC from those of two PCs alike."""
    return {'launcher_version': versions.display(launcher_version), 'summary': summary}


def forget_install_id(settings, log=None):
    """Removes the installation id an earlier version of the launcher made and kept in the settings file. Reports
    and comments carry none, so the launcher keeps none. The settings no longer know the key (settings.py DEFAULTS),
    so it is looked for in the file itself, and saving writes the file without it."""
    try:
        data = json.loads(settings.path.read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return
    if not isinstance(data, dict) or 'telemetry_id' not in data:
        return
    settings.save()
    if log:
        log('telemetry: the installation id an earlier version kept was removed from the settings; reports and '
            'comments carry no id')


def body(data):
    """The bytes of a request's JSON body. "See What Is Sent" shows these same bytes as text."""
    return json.dumps(data, indent=2, ensure_ascii=False).encode('utf-8')


def clean_text(text):
    """A comment as it is sent: line ends as \\n, no other control characters but tabs, no blank lines around it."""
    text = (text or '').replace('\r\n', '\n').replace('\r', '\n')
    return printable(text, '\n\t').strip()


# ---------------------------------------------------------------- requests

Request = collections.namedtuple('Request', 'kind method path data')   # kind: 'session' or 'feedback'
Result = collections.namedtuple('Result', 'ok reason')   # reason: for the player, '' when ok


class Reporter:
    """Sends for one launcher run. Each of send_session and send_feedback is prepare_*
    (checks and the request, on the caller's thread), post (the network, which the window runs on a thread) and
    finish (the settings and the files, on the caller's thread again)."""

    def __init__(self, settings, launcher_version, log=None, url=None):
        self.settings = settings
        self.version = versions.display(launcher_version)
        self.log = log or (lambda line: None)
        self.url = server_url() if url is None else url.rstrip('/')
        self.errors = 0
        self.told_no_server = False
        self.shortened = ''

    def error(self, line):
        """A failed request's line for launcher.log; MAX_ERROR_LINES in one run, then silence."""
        if self.errors < MAX_ERROR_LINES:
            self.errors += 1
            self.log(line)

    def closed(self):
        """Whether the server said it takes no more reports (410) to this launcher version."""
        return bool(self.settings.get('telemetry_closed')) and self.settings.get('telemetry_closed') == self.version

    def no_server(self):
        if not self.url:
            if not self.told_no_server:
                self.told_no_server = True
                self.log('telemetry: no server configured; nothing is sent')
            return True
        return False

    # ---- the network

    def post(self, req):
        """Sends req: (HTTP status, None), or (None, why) when no answer came. Never raises."""
        headers = {'Accept': 'application/json'}
        if req.data is not None:
            headers['Content-Type'] = 'application/json'
        try:
            with net.request(self.url + req.path, headers, TIMEOUT, self.version, data=req.data,
                             method=req.method) as r:
                r.read(4096)
                return getattr(r, 'status', 200), None
        except urllib.error.HTTPError as e:
            e.close()
            return e.code, None
        except net.CertificateError:
            return None, "the server's certificate was not accepted"
        except Exception as e:   # no connection, a timeout, a refused address: all the same to the player
            return None, f'no connection ({type(e).__name__}: {e})'

    @staticmethod
    def _answer(status, error):
        """The answer as launcher.log names it: the HTTP status, or why none came."""
        return f'HTTP {status}' if status is not None else f'no answer: {error}'

    @staticmethod
    def _why(status, error):
        if status is None:
            return error.split(' (')[0]
        if status == 429:
            return 'too many in a short time (HTTP 429)'
        if status >= 500:
            return f'the server had a problem (HTTP {status})'
        return f'the server refused it (HTTP {status})'

    def _closed_now(self):
        self.settings['telemetry_closed'] = self.version
        self.settings.save()
        self.log('telemetry: the server stopped accepting reports (HTTP 410); none until the next launcher version')

    # ---- session reports

    def prepare_session(self, game_dir):
        """The request of the pending session report, or a Result saying why there is none. The consent check
        lives here, not only in the window: nothing is sent unless the settings' telemetry is "on"."""
        if self.settings.get('telemetry') != 'on':
            return Result(False, 'session reports are off')
        if self.closed():
            return Result(False, 'the server no longer takes reports')
        if self.no_server():
            return Result(False, 'no server is set up')
        summary = pending_summary(game_dir)
        if summary is None:
            return Result(False, 'no session to report')
        data = body(payload(summary, self.version))
        if len(data) > SESSION_LIMIT:
            mark_sent(game_dir)
            self.error(f'telemetry: session report not sent: {len(data)} bytes, more than the server takes')
            return Result(False, 'the report is too big')
        return Request('session', 'POST', '/v1/session', data)

    def finish_session(self, game_dir, req, answer):
        status, error = answer
        if status in (200, 201):
            mark_sent(game_dir)
            self.log(f'telemetry: session report sent (HTTP {status}, {len(req.data)} bytes)')
            return Result(True, '')
        if status == 410:
            self._closed_now()
            return Result(False, 'the server no longer takes reports')
        if status is not None and status < 500 and status != 429:
            mark_sent(game_dir)   # the server will refuse it again: it is not offered on every start
        self.error(f'telemetry: session report not sent ({self._answer(status, error)})')
        return Result(False, self._why(status, error))

    def send_session(self, game_dir):
        """Sends the pending session report now, on this thread; a Result."""
        req = self.prepare_session(game_dir)
        if isinstance(req, Result):
            return req
        return self.finish_session(game_dir, req, self.post(req))

    # ---- comments

    def prepare_feedback(self, text, attach_log, game_dir):
        """The request of a comment, or a Result saying why it cannot go. Sending a comment is its own consent: the
        player wrote it and pressed Send, so it does not need session reports on (telemetry "on"); it goes with the
        last session's summary (when there is one) and, with attach_log, the last lines of the game's log."""
        text = clean_text(text)
        if not text:
            return Result(False, 'the comment is empty')
        if len(text) > TEXT_LIMIT:
            return Result(False, f'the comment is longer than {TEXT_LIMIT} characters')
        if self.closed():
            return Result(False, 'the server no longer takes comments')
        if self.no_server():
            return Result(False, 'no server is set up')
        lines = log_tail_lines(game_dir) if attach_log and game_dir else []
        data = {'launcher_version': self.version, 'text': text,
                'summary': last_summary(game_dir, self.settings),
                'log_tail': '\n'.join(lines) + '\n' if lines else None}
        raw = body(data)
        while len(raw) > FEEDBACK_LIMIT and data['log_tail']:   # escaped characters can make the log lines grow
            lines = lines[len(lines) // 4 + 1:] if len(lines) > 1 else []
            data['log_tail'] = '\n'.join(lines) + '\n' if lines else None
            raw = body(data)
        self.shortened = shortened_note(lines) if lines else ''
        if len(raw) > FEEDBACK_LIMIT:
            return Result(False, 'the comment is too big')
        return Request('feedback', 'POST', '/v1/feedback', raw)

    def finish_feedback(self, req, answer):
        status, error = answer
        if self.shortened:
            self.log(self.shortened)
            self.shortened = ''
        if status in (200, 201):
            self.log(f'telemetry: comment sent (HTTP {status}, {len(req.data)} bytes)')
            return Result(True, '')
        if status == 410:
            self._closed_now()
            return Result(False, 'the server no longer takes comments')
        self.error(f'telemetry: comment not sent ({self._answer(status, error)})')
        return Result(False, self._why(status, error))

    def send_feedback(self, text, attach_log, game_dir):
        """Sends a comment now, on this thread; a Result. See prepare_feedback for why it needs no telemetry "on"."""
        req = self.prepare_feedback(text, attach_log, game_dir)
        if isinstance(req, Result):
            return req
        return self.finish_feedback(req, self.post(req))

    # ---- consent

    def set_consent(self, on):
        """Session reports switched in Settings. Switching them off there counts as this version's answer: the
        question comes again with the next version (question_due), not at the next start."""
        self.settings['telemetry'] = 'on' if on else 'off'
        if not on:
            self.settings['telemetry_asked'] = self.version
        self.settings.save()
        self.log(f'telemetry: session reports switched {"on" if on else "off"} in Settings')

    # ---- the question after an update (launcher/ui/app.py, the consent screen)

    def question_due(self):
        """Whether this start asks about session reports: there is a server, reports are not on, and this launcher
        version has not asked yet. Reports that are on are never asked about; whoever answered Don't Send, switched
        them off in Settings or never answered is asked once by each new version."""
        return (bool(self.url) and self.settings.get('telemetry') != 'on'
                and self.settings.get('telemetry_asked') != self.version)

    def answer_question(self, how):
        """The question's end. how: 'send' and "don't send" are the two buttons; 'closed' (the window's close, Alt+F4,
        Esc) and 'timeout' (QUESTION_SECONDS without any input) turn reports on too, and are told once at the next
        start (take_notice). Only Don't Send keeps them off. True when reports are on now."""
        on = how != "don't send"
        self.settings['telemetry'] = 'on' if on else 'off'
        self.settings['telemetry_asked'] = self.version
        self.settings['telemetry_notice'] = '1' if how in ('closed', 'timeout') else ''
        self.settings.save()
        self.log(f'telemetry: asked after the update to {self.version}: {how}'
                 + (' (on)' if how in ('closed', 'timeout') else ''))
        return on

    def take_notice(self):
        """True once after reports were turned on by the countdown or by closing the question, if they are still
        on: the next start tells it."""
        if not self.settings.get('telemetry_notice'):
            return False
        self.settings['telemetry_notice'] = ''
        self.settings.save()
        if self.settings.get('telemetry') != 'on':
            return False
        self.log('telemetry: told that session reports are on')
        return True


class Tasks:
    """Requests run on threads and finished on the window's: start() runs work() on a thread, and poll(), called by
    the window's timer (app.py App.tick), calls done(result) for each that ended, as jobs.BackgroundUpdate.poll hands
    the window a staging's end."""

    def __init__(self):
        self._lock = threading.Lock()
        self._ended = []
        self.running = 0

    def start(self, work, done):
        def run():
            try:
                got = work()
            except Exception as e:   # Reporter.post never raises; anything else must not lose done()
                got = (None, f'no connection ({e!r})')
            with self._lock:
                self._ended.append((done, got))
        self.running += 1
        threading.Thread(target=run, daemon=True).start()

    def poll(self):
        with self._lock:
            ended, self._ended = self._ended, []
        for done, got in ended:
            self.running -= 1
            done(got)
        return bool(ended)

    def wait(self, timeout=15.0):
        """For tests: poll until no request runs."""
        import time
        end = time.time() + timeout
        while self.running and time.time() < end:
            self.poll()
            time.sleep(0.02)
        self.poll()
