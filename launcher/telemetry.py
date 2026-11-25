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

TELEMETRY_URL = 'https://nx2saria-telemetry.larbaco-ct.workers.dev'
URL_ENV = 'NX2SARIA_TELEMETRY_URL'
SCHEMA = 'nx2saria.session/1'
TIMEOUT = 10
QUESTION_SECONDS = 30
MAX_ERROR_LINES = 5
SESSION_LIMIT = 8 * 1024
FEEDBACK_LIMIT = 40 * 1024
TEXT_LIMIT = 4000
TAIL_LINES = 200
TAIL_LIMIT = 24 * 1024
FATAL_LIMIT = 300
LOG_TAIL_READ = 256 << 10

SUMMARY = 'session-summary.json'
SENT = 'session-summary.sent.json'
HEADER = 'session-start.json'
HEADER_LINE = b'telemetry: session header written to logs/' + HEADER.encode()
HEADER_KEYS = ('os', 'cpu', 'ram_mb', 'gpu', 'display', 'settings', 'startup', 'patches', 'mods')

VERSION_LINE = re.compile(r'version: NX2Saria (\S+), game (\d+\.\d+\.\d+)')


def server_url():
    url = (os.environ.get(URL_ENV) or TELEMETRY_URL).strip().rstrip('/')
    return '' if url.lower() == 'none' else url


def enabled():
    return bool(server_url())


def os_family():
    return 'windows' if os.name == 'nt' else 'linux'


def empty_summary(version, game_version):
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
    return ''.join(ch for ch in text if ch in keep or unicodedata.category(ch) not in ('Cc', 'Cs'))


def _logs(game_dir):
    return Path(game_dir) / 'logs'


def _read_summary(path):
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) and data.get('schema') == SCHEMA else None


def pending_summary(game_dir):
    if not game_dir:
        return None
    return _read_summary(_logs(game_dir) / SUMMARY)


def last_summary(game_dir, settings=None):
    if not game_dir or (settings is not None and settings.get('telemetry') == 'off'):
        return None
    return pending_summary(game_dir) or _read_summary(_logs(game_dir) / SENT)


def mark_sent(game_dir):
    logs = _logs(game_dir)
    try:
        os.replace(logs / SUMMARY, logs / SENT)
    except OSError:
        pass


def log_started(game_dir):
    log = game_log_path(game_dir)
    try:
        return log.with_name(log.stem + '.1' + log.suffix).stat().st_mtime
    except OSError:
        return 0.0


def _covered(game_dir, since):
    for name in (SUMMARY, SENT):
        try:
            if (_logs(game_dir) / name).stat().st_mtime > since:
                return True
        except OSError:
            pass
    return False


def log_version(game_dir):
    try:
        with open(game_log_path(game_dir), 'rb') as f:
            head = f.read(64 << 10).decode('utf-8', 'replace')
    except OSError:
        return None
    m = VERSION_LINE.search(head)
    return (versions.display(m.group(1)), m.group(2)) if m else None


def _log_has(game_dir, needle):
    keep = len(needle) - 1
    try:
        with open(game_log_path(game_dir), 'rb') as f:
            tail = b''
            while chunk := f.read(1 << 20):
                if needle in tail + chunk:
                    return True
                tail = (tail + chunk)[-keep:]
    except OSError:
        pass
    return False


def _fits(value, like):
    if value is None:
        return True
    if isinstance(like, float):
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return type(value) is type(like)


def session_header(game_dir):
    path = _logs(game_dir) / HEADER
    try:
        raw = path.read_bytes()
    except OSError:
        return None, f'logs/{HEADER} is missing'
    if not raw.strip():
        return None, f'logs/{HEADER} is empty'
    try:
        data = json.loads(raw.decode('utf-8'))
    except ValueError:
        data = None
    if not isinstance(data, dict) or data.get('schema') != SCHEMA:
        return None, f'logs/{HEADER} is not valid JSON'
    if not _log_has(game_dir, HEADER_LINE):
        return None, "the log of the run that crashed has no line of its writing, so it is another run's"
    return data, ''


def _take_header(s, header):
    like = example_summary()
    for key in HEADER_KEYS:
        want, got = s[key], header.get(key)
        if isinstance(want, dict):
            if isinstance(got, dict):
                for sub in want:
                    if _fits(got.get(sub), like[key][sub]):
                        want[sub] = got.get(sub)
        elif key == 'patches':
            if isinstance(got, list) and all(isinstance(x, str) for x in got):
                s[key] = got
        elif _fits(got, like[key]):
            s[key] = got


def _leaves(v):
    return [x for y in v.values() for x in _leaves(y)] if isinstance(v, dict) else [v]


def synthesize_crash(game_dir, log=None):
    if not game_dir:
        return None
    fatal = last_fatal(game_dir)
    if not fatal or _covered(game_dir, log_started(game_dir)):
        return None
    ver = log_version(game_dir)
    if not ver:
        return None
    s = empty_summary(*ver)
    header, why = session_header(game_dir)
    if header:
        _take_header(s, header)
    s['os']['family'] = s['os']['family'] or os_family()
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
        leaves = _leaves(s)
        nulls = f'{sum(v is None for v in leaves)} of {len(leaves)} fields null'
        source = (f'with the hardware and settings of logs/{HEADER}' if header
                  else f'without hardware or settings: {why}')
        log(f'telemetry: the last run ended in a crash; a summary was made from its FATAL line, {source} ({nulls})')
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
    out = log_tail_lines(game_dir, lines, limit)
    return '\n'.join(out) + '\n' if out else None


def payload(summary, launcher_version):
    return {'launcher_version': versions.display(launcher_version), 'summary': summary}


def forget_install_id(settings, log=None):
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
    return json.dumps(data, indent=2, ensure_ascii=False).encode('utf-8')


def clean_text(text):
    text = (text or '').replace('\r\n', '\n').replace('\r', '\n')
    return printable(text, '\n\t').strip()


Request = collections.namedtuple('Request', 'kind method path data')
Result = collections.namedtuple('Result', 'ok reason')


class Reporter:
    def __init__(self, settings, launcher_version, log=None, url=None):
        self.settings = settings
        self.version = versions.display(launcher_version)
        self.log = log or (lambda line: None)
        self.url = server_url() if url is None else url.rstrip('/')
        self.errors = 0
        self.told_no_server = False
        self.shortened = ''

    def error(self, line):
        if self.errors < MAX_ERROR_LINES:
            self.errors += 1
            self.log(line)

    def closed(self):
        return bool(self.settings.get('telemetry_closed')) and self.settings.get('telemetry_closed') == self.version

    def no_server(self):
        if not self.url:
            if not self.told_no_server:
                self.told_no_server = True
                self.log('telemetry: no server configured; nothing is sent')
            return True
        return False

    def post(self, req):
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
        except Exception as e:
            return None, f'no connection ({type(e).__name__}: {e})'

    @staticmethod
    def _answer(status, error):
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

    def prepare_session(self, game_dir):
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
            mark_sent(game_dir)
        self.error(f'telemetry: session report not sent ({self._answer(status, error)})')
        return Result(False, self._why(status, error))

    def send_session(self, game_dir):
        req = self.prepare_session(game_dir)
        if isinstance(req, Result):
            return req
        return self.finish_session(game_dir, req, self.post(req))

    def prepare_feedback(self, text, attach_log, game_dir):
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
        while len(raw) > FEEDBACK_LIMIT and data['log_tail']:
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
        req = self.prepare_feedback(text, attach_log, game_dir)
        if isinstance(req, Result):
            return req
        return self.finish_feedback(req, self.post(req))

    def set_consent(self, on):
        self.settings['telemetry'] = 'on' if on else 'off'
        if not on:
            self.settings['telemetry_asked'] = self.version
        self.settings.save()
        self.log(f'telemetry: session reports switched {"on" if on else "off"} in Settings')

    def question_due(self):
        return (bool(self.url) and self.settings.get('telemetry') != 'on'
                and self.settings.get('telemetry_asked') != self.version)

    def answer_question(self, how):
        on = how != "don't send"
        self.settings['telemetry'] = 'on' if on else 'off'
        self.settings['telemetry_asked'] = self.version
        self.settings['telemetry_notice'] = '1' if how in ('closed', 'timeout') else ''
        self.settings.save()
        self.log(f'telemetry: asked after the update to {self.version}: {how}'
                 + (' (on)' if how in ('closed', 'timeout') else ''))
        return on

    def take_notice(self):
        if not self.settings.get('telemetry_notice'):
            return False
        self.settings['telemetry_notice'] = ''
        self.settings.save()
        if self.settings.get('telemetry') != 'on':
            return False
        self.log('telemetry: told that session reports are on')
        return True


class Tasks:
    def __init__(self):
        self._lock = threading.Lock()
        self._ended = []
        self.running = 0

    def start(self, work, done):
        def run():
            try:
                got = work()
            except Exception as e:
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
        import time
        end = time.time() + timeout
        while self.running and time.time() < end:
            self.poll()
            time.sleep(0.02)
        self.poll()
