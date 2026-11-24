import collections
import json
import os
import time
from pathlib import Path

from . import telemetry
from .build import game_log_path

OFFER = 'crash-offer.json'
SETTING = 'crash_log_offer'
HEAD_LINES = 100
HEAD_LIMIT = 16 * 1024
HEAD_END = 'patches:'
TAIL_LINES = 200
TAIL_LIMIT = 56 * 1024
BODY_LIMIT = 96 * 1024
HEAD_READ = 128 << 10
TAIL_READ = 512 << 10
GUARD_SECONDS = 1.0

CrashLog = collections.namedtuple('CrashLog', 'request head_lines tail_lines log_bytes whole summary shortened')


class Offers:
    def __init__(self):
        self.shown = self.sent = self.declined = self.failed = 0
        self.refused = False

    def counts(self):
        return (f'this run of the launcher: {self.shown} shown, {self.sent} sent, {self.declined} declined, '
                f'{self.failed} failed')


def on(settings):
    return settings.get(SETTING) != 'off'


def log_state(settings, log):
    log(f'telemetry: crash log offer: {"on" if on(settings) else "off"} (Settings > Session Reports > Ask After a '
        'Crash)')


def offer_path(game_dir):
    return Path(game_dir) / 'logs' / OFFER


def log_stamp(game_dir):
    try:
        st = game_log_path(game_dir).stat()
    except OSError:
        return None
    return [st.st_size, st.st_mtime_ns]


def forget(game_dir):
    try:
        offer_path(game_dir).unlink()
    except OSError:
        pass


def game_ended(settings, game_dir, kind, asked, message, log):
    if not kind or not game_dir or not telemetry.enabled():
        return False
    if asked:
        log(f'telemetry: crash log offer: not made: the game was asked to close (end: {kind})')
        return False
    if not on(settings):
        log(f'telemetry: crash log offer: not made: off in Settings (end: {kind})')
        return False
    stamp = log_stamp(game_dir)
    if stamp is None:
        log(f'telemetry: crash log offer: not made: the game left no log (end: {kind})')
        return False
    path = offer_path(game_dir)
    tmp = path.with_name(path.name + '.tmp')
    try:
        tmp.write_text(json.dumps({'kind': kind, 'message': message, 'log': stamp, 'at': int(time.time())}),
                       encoding='utf-8')
        os.replace(tmp, path)
    except OSError as e:
        log(f'telemetry: crash log offer: not made: {e} (end: {kind})')
        return False
    log(f"telemetry: crash log offer: waits for the launcher's main screen (end: {kind})")
    return True


def read_offer(game_dir):
    try:
        data = json.loads(offer_path(game_dir).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict) or not isinstance(data.get('kind'), str) or not isinstance(data.get('log'), list):
        return None
    if not isinstance(data.get('message'), str):
        data['message'] = ''
    return data


def pending(game_dir, settings, log):
    if not game_dir or not offer_path(game_dir).is_file():
        return None
    data = read_offer(game_dir)
    if data is None:
        forget(game_dir)
        log('telemetry: crash log offer: dropped: its file could not be read')
        return None
    if not on(settings):
        forget(game_dir)
        log(f'telemetry: crash log offer: dropped: switched off in Settings since (end: {data["kind"]})')
        return None
    if data['log'] != log_stamp(game_dir):
        forget(game_dir)
        log("telemetry: crash log offer: dropped: the game's log is no longer the one of that session "
            f'(end: {data["kind"]})')
        return None
    return data


def clean(raw):
    return telemetry.sent_line(raw.decode('utf-8', 'replace').rstrip('\r\n'))


def ends_head(line):
    body = line.split('] ', 1)[1] if line.startswith('[') and '] ' in line else line
    return body.startswith(HEAD_END)


def log_parts(game_dir, tail_limit=TAIL_LIMIT):
    try:
        with open(game_log_path(game_dir), 'rb') as f:
            size = f.seek(0, 2)
            f.seek(0)
            first = f.read(HEAD_READ)
            start = max(0, size - TAIL_READ)
            f.seek(start)
            last = f.read()
    except OSError:
        return [], [], False
    raw = last.splitlines(keepends=True)
    if start and raw:
        raw = raw[1:]
    tail, total, taken = [], 0, 0
    for ln in reversed(raw[-TAIL_LINES:]):
        text = clean(ln)
        n = len(text.encode('utf-8')) + 1
        if total + n > tail_limit:
            break
        tail.append(text)
        total += n
        taken += len(ln)
    tail.reverse()
    tail_at = size - taken
    head, total, at = [], 0, 0
    for ln in first.splitlines(keepends=True):
        if at + len(ln) > tail_at or len(head) >= HEAD_LINES or not ln.endswith(b'\n'):
            break
        text = clean(ln)
        n = len(text.encode('utf-8')) + 1
        if total + n > HEAD_LIMIT:
            break
        head.append(text)
        total += n
        at += len(ln)
        if ends_head(text):
            break
    return head, tail, bool(tail) and at == tail_at


def session_summary(game_dir, settings):
    if settings.get('telemetry') == 'off':
        return None
    since = telemetry.log_started(game_dir)
    for name in (telemetry.SUMMARY, telemetry.SENT):
        path = Path(game_dir) / 'logs' / name
        try:
            if path.stat().st_mtime <= since:
                continue
        except OSError:
            continue
        data = telemetry._read_summary(path)
        if data is not None:
            return data
    return None


def joined(lines):
    return '\n'.join(lines) + '\n' if lines else None


def build(reporter, head, tail, whole, summary):
    data = {'kind': 'crash', 'launcher_version': reporter.version, 'text': '', 'summary': summary,
            'log_tail': joined(tail), 'log_head': joined(head)}
    raw = telemetry.body(data)
    while len(raw) > BODY_LIMIT and len(tail) > 1:
        tail = tail[len(tail) // 4 + 1:]
        whole = False
        data['log_tail'] = joined(tail)
        raw = telemetry.body(data)
    if len(raw) > BODY_LIMIT:
        return telemetry.Result(False, 'the log is too big')
    size = len((data['log_head'] or '').encode('utf-8')) + len(data['log_tail'].encode('utf-8'))
    return CrashLog(telemetry.Request('crash', 'POST', '/v1/feedback', raw), len(head), len(tail), size, whole,
                    summary is not None, telemetry.shortened_note(head + tail))


def prepare(reporter, game_dir):
    if reporter.closed():
        return telemetry.Result(False, 'the server no longer takes reports')
    if reporter.no_server():
        return telemetry.Result(False, 'no server is set up')
    head, tail, whole = log_parts(game_dir)
    if not tail:
        return telemetry.Result(False, "the game's log is empty")
    return build(reporter, head, tail, whole, session_summary(game_dir, reporter.settings))


EXAMPLE_HEAD = ('[host] cfg: read ~/Games/NX2Saria/nx2saria.ini: 19 keys',
                '[host] version: NX2Saria 0.19.0, game 1.1.1',
                '[host] vk: using gpu 0: NVIDIA GeForce RTX 3060',
                '[host] patches: fps60 on, ostep on, zstd on')
EXAMPLE_TAIL = ('[host] vk: frame 109800 presented',
                'FATAL: vk: the graphics device was lost (VK_ERROR_DEVICE_LOST)',
                '[launcher] the game ended with code 72 at 2026-01-10 21:14:03')


def example(reporter):
    return build(reporter, list(EXAMPLE_HEAD), list(EXAMPLE_TAIL), False, telemetry.example_summary())


def what_goes(sending):
    kb = max(1, round(sending.log_bytes / 1024))
    lines = sending.head_lines + sending.tail_lines
    if sending.whole or not sending.head_lines:
        what = (f"the whole log of this session ({lines} lines, {kb} KB)" if sending.whole
                else f"the last {lines} lines of the game's log ({kb} KB)")
    else:
        what = (f"{lines} lines of the game's log ({kb} KB): its first {sending.head_lines} and its last "
                f'{sending.tail_lines}')
    report = ', and the report of that session' if sending.summary else ''
    return (f'Send the Log sends {what}, with your home folder, account name and computer name written short'
            f'{report}. No id of this installation.')


def finish(reporter, offers, sending, answer):
    status, error = answer
    req = sending.request
    if sending.shortened:
        reporter.log(sending.shortened)
    if status in (200, 201):
        offers.sent += 1
        reporter.log(f'telemetry: crash log sent (HTTP {status}, {sending.head_lines} + {sending.tail_lines} lines, '
                     f'{sending.log_bytes} bytes of log, {len(req.data)} bytes in all; {offers.counts()})')
        return telemetry.Result(True, '')
    offers.failed += 1
    if status == 410:
        reporter._closed_now()
        return telemetry.Result(False, 'the server no longer takes reports')
    if status == 400:
        offers.refused = True
        reporter.log('telemetry: crash log not sent (HTTP 400): this server does not take it; not tried again, and '
                     f'no more offers in this run of the launcher ({offers.counts()})')
        return telemetry.Result(False, 'the server refused it (HTTP 400)')
    reporter.error(f'telemetry: crash log not sent ({reporter._answer(status, error)}); not tried again '
                   f'({offers.counts()})')
    return telemetry.Result(False, reporter._why(status, error))
