import collections
import gc
import http.server
import json
import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))

from launcher import settings, telemetry  # noqa: E402
import private_paths  # noqa: E402

VERSION = '0.19.0-rc.2'
LOG_HEAD = ('[host] log privacy: personal folders are written short\n'
            f'[host] version: NX2Saria {VERSION}, game 1.1.1\n'
            '[host] libs: vulkan-1\n')


class Server:
    def __init__(self, status=200):
        self.status = status
        self.hits = []
        server = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def answer(self):
                n = int(self.headers.get('Content-Length') or 0)
                body = self.rfile.read(n) if n else b''
                server.hits.append((self.command, self.path, dict(self.headers), body))
                out = b'{"ok": true}' if server.status < 300 else b'{"error": "x"}'
                self.send_response(server.status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(out)))
                self.end_headers()
                self.wfile.write(out)

            do_POST = do_DELETE = do_GET = answer

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
        self.url = f'http://127.0.0.1:{self.httpd.server_address[1]}'
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def summary(end='normal', fatal=None):
    s = telemetry.example_summary()
    s['version'] = VERSION
    s['session']['end'] = end
    s['fatal'] = fatal
    return s


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='nx2saria-telemetry-test-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.game = self.tmp / 'game'
        (self.game / 'logs').mkdir(parents=True)
        self.settings = settings.Settings(self.tmp / 'launcher.json')
        self.lines = []
        self.server = Server()
        self.addCleanup(self.server.close)
        short = private_paths.Shortener(env={'HOME': '/home/ana', 'USER': 'ana', 'HOSTNAME': 'maria-pc'},
                                        windows=False, host=lambda: 'maria-pc')
        p = mock.patch.object(private_paths, '_default', short)
        p.start()
        self.addCleanup(p.stop)
        e = mock.patch.dict(os.environ, {private_paths.SWITCH: '1'})
        e.start()
        self.addCleanup(e.stop)

    def reporter(self, url=None, version=VERSION):
        return telemetry.Reporter(self.settings, version, self.lines.append,
                                  url=self.server.url if url is None else url)

    def write_summary(self, s=None):
        path = self.game / 'logs' / telemetry.SUMMARY
        path.write_text(json.dumps(s or summary()), encoding='utf-8')
        return path

    def telemetry_lines(self):
        return [ln for ln in self.lines if ln.startswith('telemetry:')]


class ConsentTest(Base):
    def test_nothing_is_sent_unless_reports_are_on(self):
        self.write_summary()
        for answer in ('', 'off', 'yes', 'ON'):
            self.settings['telemetry'] = answer
            with mock.patch('launcher.net.request') as req, mock.patch('urllib.request.urlopen') as urlopen:
                got = self.reporter().send_session(self.game)
            self.assertFalse(got.ok, answer)
            req.assert_not_called()
            urlopen.assert_not_called()
        self.assertEqual(self.server.hits, [])
        self.assertTrue((self.game / 'logs' / telemetry.SUMMARY).is_file())

    def test_on_sends_exactly_the_payloads_bytes(self):
        s = summary()
        self.write_summary(s)
        self.settings['telemetry'] = 'on'
        expected = telemetry.body(telemetry.payload(s, VERSION))
        got = self.reporter().send_session(self.game)
        self.assertTrue(got.ok, got.reason)
        method, path, headers, body = self.server.hits[0]
        self.assertEqual((method, path), ('POST', '/v1/session'))
        self.assertEqual(body, expected)
        self.assertEqual(headers['Content-Type'], 'application/json')
        self.assertTrue(headers['User-Agent'].startswith('NX2Saria-Launcher/'))
        sent = json.loads(body)
        self.assertEqual(set(sent), {'launcher_version', 'summary'})
        self.assertEqual(sent['launcher_version'], VERSION)
        self.assertEqual(sent['summary'], s)
        self.assertLessEqual(len(body), telemetry.SESSION_LIMIT)
        self.assertFalse((self.game / 'logs' / telemetry.SUMMARY).exists())
        self.assertTrue((self.game / 'logs' / telemetry.SENT).is_file())
        self.assertIn(f'telemetry: session report sent (HTTP 200, {len(body)} bytes)', self.lines)
        self.assertFalse(self.reporter().send_session(self.game).ok)
        self.assertEqual(len(self.server.hits), 1)

    def test_no_id_is_made_kept_or_sent(self):
        self.write_summary()
        self.settings['telemetry'] = 'on'
        r = self.reporter()
        self.assertTrue(r.send_session(self.game).ok)
        self.assertTrue(r.send_feedback('A comment', False, self.game).ok)
        for _, _, _, body in self.server.hits:
            self.assertNotIn(b'install', body)
            self.assertNotIn(b'_id', body)
        self.settings.save()
        kept = json.loads((self.tmp / 'launcher.json').read_text(encoding='utf-8'))
        self.assertFalse([k for k in kept if 'id' in k.split('_')], kept)

    def test_an_earlier_version_s_id_is_removed_from_the_settings_file(self):
        path = self.tmp / 'launcher.json'
        path.write_text(json.dumps({'telemetry': 'on', 'telemetry_id': '3b241101-e2bb-4255-8caf-4136c566a962',
                                    'sounds': 'off'}), encoding='utf-8')
        s = settings.Settings(path)
        telemetry.forget_install_id(s, self.lines.append)
        kept = json.loads(path.read_text(encoding='utf-8'))
        self.assertNotIn('telemetry_id', kept)
        self.assertNotIn('3b241101', path.read_text(encoding='utf-8'))
        self.assertEqual((kept['telemetry'], kept['sounds']), ('on', 'off'))
        self.assertEqual(len(self.telemetry_lines()), 1)
        self.assertIn('installation id an earlier version kept was removed', self.lines[0])
        telemetry.forget_install_id(s, self.lines.append)
        self.assertEqual(len(self.telemetry_lines()), 1)

    def test_no_server_sends_nothing_and_says_so_once(self):
        self.write_summary()
        self.settings['telemetry'] = 'on'
        with mock.patch.dict(os.environ, {telemetry.URL_ENV: ''}), mock.patch('launcher.net.request') as req, \
                mock.patch.object(telemetry, 'TELEMETRY_URL', ''):
            r = telemetry.Reporter(self.settings, VERSION, self.lines.append)
            self.assertEqual(r.url, '')
            for _ in range(3):
                self.assertFalse(r.send_session(self.game).ok)
            self.assertFalse(r.send_feedback('hello', False, self.game).ok)
        req.assert_not_called()
        self.assertEqual(self.telemetry_lines(), ['telemetry: no server configured; nothing is sent'])

    def test_the_address_comes_from_the_environment(self):
        with mock.patch.dict(os.environ, {telemetry.URL_ENV: 'http://127.0.0.1:8787/'}):
            self.assertEqual(telemetry.Reporter(self.settings, VERSION).url, 'http://127.0.0.1:8787')
        with mock.patch.dict(os.environ, {telemetry.URL_ENV: 'none'}):
            self.assertEqual(telemetry.server_url(), '')
            self.assertFalse(telemetry.enabled())

    def test_the_settings_switch(self):
        r = self.reporter()
        r.set_consent(True)
        self.assertEqual(settings.Settings(self.tmp / 'launcher.json')['telemetry'], 'on')
        r.set_consent(False)
        self.assertEqual(self.settings['telemetry'], 'off')
        self.assertEqual(self.telemetry_lines(), ['telemetry: session reports switched on in Settings',
                                                  'telemetry: session reports switched off in Settings'])
        self.assertFalse(r.question_due())
        self.assertTrue(self.reporter(version='0.25.0').question_due())

    def test_the_question_is_due_once_a_version_while_reports_are_not_on(self):
        r = self.reporter()
        self.assertTrue(r.question_due())
        self.assertFalse(r.answer_question("don't send"))
        self.assertEqual(self.settings['telemetry'], 'off')
        self.assertFalse(r.question_due())
        later = self.reporter(version='0.25.0')
        self.assertTrue(later.question_due())
        self.assertTrue(later.answer_question('send'))
        self.assertEqual(settings.Settings(self.tmp / 'launcher.json')['telemetry'], 'on')
        for version in ('0.25.0', '0.26.0', '1.0.0'):
            self.assertFalse(self.reporter(version=version).question_due())
        self.assertFalse(self.reporter(url='').question_due())

    def test_the_question_s_four_ends(self):
        for how, on, notice in (('send', True, False), ("don't send", False, False), ('closed', True, True),
                                ('timeout', True, True)):
            self.settings.update(telemetry='', telemetry_asked='', telemetry_notice='')
            del self.lines[:]
            r = self.reporter()
            self.assertEqual(r.answer_question(how), on)
            self.assertEqual(self.settings['telemetry'], 'on' if on else 'off')
            self.assertEqual(self.settings['telemetry_asked'], VERSION)
            self.assertEqual(self.telemetry_lines(),
                             [f'telemetry: asked after the update to {VERSION}: {how}{" (on)" if notice else ""}'])
            self.assertEqual(r.take_notice(), notice)
            self.assertFalse(r.take_notice())

    def test_no_notice_when_reports_were_switched_off_since(self):
        r = self.reporter()
        r.answer_question('timeout')
        r.set_consent(False)
        self.assertFalse(r.take_notice())


class ServerAnswerTest(Base):
    def test_410_stops_reports_until_the_next_launcher_version(self):
        self.settings['telemetry'] = 'on'
        self.write_summary()
        self.server.status = 410
        got = self.reporter().send_session(self.game)
        self.assertFalse(got.ok)
        self.assertEqual(self.settings['telemetry_closed'], VERSION)
        self.assertIn('telemetry: the server stopped accepting reports (HTTP 410); none until the next launcher version',
                      self.lines)
        self.server.status = 200
        self.assertFalse(self.reporter().send_session(self.game).ok)
        self.assertEqual(len(self.server.hits), 1)
        got = self.reporter(version='0.19.1').send_session(self.game)
        self.assertTrue(got.ok, got.reason)
        self.assertEqual(len(self.server.hits), 2)

    def test_a_network_error_is_silent_and_logs_at_most_five_lines(self):
        self.settings['telemetry'] = 'on'
        self.write_summary()
        dead = Server()
        url = dead.url
        dead.close()
        r = self.reporter(url=url)
        for _ in range(8):
            got = r.send_session(self.game)
            self.assertFalse(got.ok)
            self.assertEqual(got.reason, 'no connection')
        self.assertFalse(r.send_feedback('hello', False, self.game).ok)
        errors = self.telemetry_lines()
        self.assertEqual(len(errors), telemetry.MAX_ERROR_LINES)
        self.assertTrue(all(ln.startswith('telemetry: session report not sent (no answer: no connection') for ln in errors))
        self.assertTrue((self.game / 'logs' / telemetry.SUMMARY).is_file())

    def test_a_refused_report_is_not_offered_again(self):
        self.settings['telemetry'] = 'on'
        self.write_summary()
        self.server.status = 400
        got = self.reporter().send_session(self.game)
        self.assertEqual(got.reason, 'the server refused it (HTTP 400)')
        self.assertFalse((self.game / 'logs' / telemetry.SUMMARY).exists())
        self.server.status = 429
        self.write_summary()
        self.assertFalse(self.reporter().send_session(self.game).ok)
        self.assertTrue((self.game / 'logs' / telemetry.SUMMARY).is_file())

    def test_a_comment_needs_no_session_consent(self):
        self.settings['telemetry'] = ''
        s = summary()
        self.write_summary(s)
        (self.game / 'logs' / 'nx2saria.log').write_text(
            LOG_HEAD + '[host] save: /home/ana/Games/NX2Saria/profile/save written\n', encoding='utf-8')
        got = self.reporter().send_feedback('  The water temple\r\nstutters.\x07  ', True, self.game)
        self.assertTrue(got.ok, got.reason)
        method, path, headers, body = self.server.hits[0]
        self.assertEqual((method, path), ('POST', '/v1/feedback'))
        sent = json.loads(body)
        self.assertEqual(set(sent), {'launcher_version', 'text', 'summary', 'log_tail'})
        self.assertEqual(sent['text'], 'The water temple\nstutters.')
        self.assertEqual(sent['summary'], s)
        self.assertIn('~/Games/NX2Saria/profile', sent['log_tail'])
        self.assertNotIn('/home/ana', sent['log_tail'])
        self.assertTrue(any(ln.startswith('telemetry: comment sent (HTTP 200, ') for ln in self.lines))
        self.assertTrue((self.game / 'logs' / telemetry.SUMMARY).is_file())
        got = self.reporter().send_feedback('Without the log', False, self.game)
        self.assertTrue(got.ok)
        self.assertIsNone(json.loads(self.server.hits[1][3])['log_tail'])
        self.assertFalse(self.reporter().send_feedback(' \n ', False, self.game).ok)
        self.assertFalse(self.reporter().send_feedback('x' * 4001, False, self.game).ok)
        self.assertEqual(len(self.server.hits), 2)
        self.settings['telemetry'] = 'off'
        got = self.reporter().send_feedback('Reports are off', False, self.game)
        self.assertTrue(got.ok)
        self.assertIsNone(json.loads(self.server.hits[2][3])['summary'])


class CrashSummaryTest(Base):
    FATAL = ('FATAL: crash: EXCEPTION_ACCESS_VIOLATION at ocarina.exe+0x1a2b3c reading 0x0; module '
             '/home/ana/Games/NX2Saria/ocarina.exe; user ana on maria-pc')

    def log(self, text, rotate_before=True):
        logs = self.game / 'logs'
        if rotate_before:
            (logs / 'nx2saria.1.log').write_text('the run before\n', encoding='utf-8')
            old = time.time() - 600
            os.utime(logs / 'nx2saria.1.log', (old, old))
        (logs / 'nx2saria.log').write_text(text, encoding='utf-8')

    def test_a_hard_crash_gets_a_summary_from_its_fatal_line(self):
        self.log(LOG_HEAD + '[host] perf: 60 fps\n' + self.FATAL + ' ' + 'x' * 400 + '\n')
        s = telemetry.synthesize_crash(self.game, self.lines.append)
        self.assertIsNotNone(s)
        self.assertEqual(telemetry.pending_summary(self.game), s)
        self.assertEqual((s['schema'], s['version'], s['game_version']), (telemetry.SCHEMA, VERSION, '1.1.1'))
        self.assertEqual(s['session']['end'], 'crash')
        self.assertEqual(s['os']['family'], 'windows' if os.name == 'nt' else 'linux')
        self.assertTrue(s['fatal'].startswith('crash: EXCEPTION_ACCESS_VIOLATION at ocarina.exe+0x1a2b3c'))
        self.assertIn('~/Games/NX2Saria/ocarina.exe', s['fatal'])
        self.assertNotIn('/home/ana', s['fatal'])
        self.assertNotIn('maria-pc', s['fatal'])
        self.assertLessEqual(len(s['fatal']), 300)
        nulls = {k: v for k, v in s.items() if k not in ('schema', 'version', 'game_version', 'fatal')}
        nulls['os'] = {k: v for k, v in s['os'].items() if k != 'family'}
        nulls['session'] = {k: v for k, v in s['session'].items() if k != 'end'}

        def leaves(v):
            return [x for y in v.values() for x in leaves(y)] if isinstance(v, dict) else [v]
        self.assertEqual(set(leaves(nulls)), {None})
        self.assertEqual(set(s), set(telemetry.example_summary()))
        self.assertIsNone(telemetry.synthesize_crash(self.game))
        telemetry.mark_sent(self.game)
        self.assertIsNone(telemetry.synthesize_crash(self.game))

    def test_no_summary_when_the_game_wrote_one_or_the_log_has_no_version(self):
        self.log(LOG_HEAD + self.FATAL + '\n')
        self.write_summary(summary('fatal', 'x'))
        self.assertIsNone(telemetry.synthesize_crash(self.game))
        self.assertEqual(telemetry.pending_summary(self.game)['session']['end'], 'fatal')
        (self.game / 'logs' / telemetry.SUMMARY).unlink()
        self.log('[host] boot\n' + self.FATAL + '\n')
        self.assertIsNone(telemetry.synthesize_crash(self.game))
        self.log(LOG_HEAD + '[host] gfx: window closed\n')
        self.assertIsNone(telemetry.synthesize_crash(self.game))
        self.log(LOG_HEAD + self.FATAL + '\n[launcher] the game ended with code 71\n')
        self.assertIsNone(telemetry.synthesize_crash(self.game))

    def test_a_summary_of_the_run_before_does_not_stop_it(self):
        old = self.write_summary()
        then = time.time() - 900
        os.utime(old, (then, then))
        self.log(LOG_HEAD + self.FATAL + '\n')
        s = telemetry.synthesize_crash(self.game)
        self.assertEqual(s['session']['end'], 'crash')


class LogTailTest(Base):
    def test_the_last_lines_within_the_caps(self):
        lines = [f'[host] line {i:04d} /home/ana/Games/NX2Saria ' + 'y' * 150 for i in range(600)]
        (self.game / 'logs' / 'nx2saria.log').write_text('\n'.join(lines) + '\n', encoding='utf-8')
        tail = telemetry.log_tail(self.game)
        got = tail.splitlines()
        self.assertLessEqual(len(got), telemetry.TAIL_LINES)
        self.assertLessEqual(len(tail.encode('utf-8')), telemetry.TAIL_LIMIT)
        self.assertTrue(got[-1].startswith('[host] line 0599 ~/Games/NX2Saria'))
        self.assertNotIn('/home/ana', tail)
        short = telemetry.log_tail(self.game, limit=1000)
        self.assertLessEqual(len(short.encode('utf-8')), 1000)
        self.assertTrue(short.splitlines()[-1].startswith('[host] line 0599'))
        few = [f'line {i}' for i in range(300)]
        (self.game / 'logs' / 'nx2saria.log').write_text('\n'.join(few), encoding='utf-8')
        self.assertEqual(telemetry.log_tail(self.game).splitlines(), few[-200:])
        self.assertIsNone(telemetry.log_tail(self.tmp / 'no game'))


def window_toolkit():
    if os.name != 'nt' and not os.environ.get('DISPLAY'):
        return 'no display (run under xvfb-run)'
    try:
        import numpy  # noqa: F401
        import tkinter  # noqa: F401
    except ImportError as e:
        return f'no window toolkit: {e}'
    return ''


@unittest.skipIf(window_toolkit(), window_toolkit())
class WindowBase(Base):
    ARGV = ['--selftest']

    def setUp(self):
        super().setUp()
        import tkinter as tk
        from launcher.context import Context
        from launcher.paths import Layout
        from launcher.ui import app
        self.app_module = app
        layout = Layout(root=self.tmp / 'launcher', app=ROOT).ensure()
        self.ctx = Context(layout)
        self.addCleanup(lambda: self.ctx._file and self.ctx._file.close())
        self.ctx.settings['game_dir'] = str(self.game)
        self.ctx.settings['sounds'] = 'off'
        args = app.parse_args(['--root', str(layout.root), *self.ARGV])
        self.root = tk.Tk(className='NX2Saria')
        self.addCleanup(self.close_window)
        quiet = mock.patch.object(app.App, 'find_dumps', lambda a: None)
        quiet.start()
        self.addCleanup(quiet.stop)
        url = mock.patch.dict(os.environ, {telemetry.URL_ENV: self.server.url})
        url.start()
        self.addCleanup(url.stop)
        self.app = app.App(self.root, self.ctx, args)
        self.root.update()

    def close_window(self):
        for after in self.root.tk.call('after', 'info'):
            self.root.after_cancel(after)
        self.root.destroy()
        if getattr(self, 'app', None):
            self.app.__dict__.clear()
        gc.collect()

    def label(self):
        r = self.app.focused()
        return r.label if r.kind != 'pair' else r.actions[getattr(r, 'pick', 0)][0]

    def logged(self, end):
        return any(ln.rstrip('\n').endswith(end) for ln in self.ctx.log_tail(30))

    def question_line(self):
        return self.app.consent_desc()['text'][0]


@unittest.skipIf(window_toolkit(), window_toolkit())
class WindowTest(WindowBase):
    def test_the_view_shows_the_bytes_that_are_sent(self):
        s = summary()
        self.write_summary(s)
        self.app.open_sent('settings')
        self.assertEqual(self.app.screen, 'sent_view')
        self.assertFalse(self.app.sent_example)
        shown = self.app.overlay.get('1.0', 'end-1c')
        self.assertEqual(shown, self.app.sent_text)
        self.ctx.settings['telemetry'] = 'on'
        self.app.send_pending()
        self.app.tasks.wait()
        self.assertEqual(self.server.hits[0][3], shown.encode('utf-8'))
        self.app.open_sent('settings')
        self.assertTrue(self.app.sent_example)
        self.assertNotIn('install_id', self.app.sent_text)
        self.assertEqual(set(json.loads(self.app.sent_text)), {'launcher_version', 'summary'})

    def test_the_first_start_of_a_version_asks_with_send_focused(self):
        self.write_summary()
        self.app.start_reports()
        self.assertEqual(self.app.screen, 'consent')
        self.assertEqual(self.label(), 'Send')
        self.assertEqual(self.app.consent_desc()['title'], 'Help improve NX2Saria?')
        left = self.app.question_deadline - time.monotonic()
        self.assertTrue(telemetry.QUESTION_SECONDS - 20 < left <= telemetry.QUESTION_SECONDS, left)
        self.app.question_deadline = time.monotonic() + telemetry.QUESTION_SECONDS
        self.assertIn(f'turn on in {telemetry.QUESTION_SECONDS} s or when you close this window', self.question_line())
        self.assertIn("Choose Don't Send to keep them off", self.question_line())
        self.assertEqual(self.server.hits, [])
        self.app.activate()
        self.assertEqual(self.app.screen, 'home')
        self.assertEqual(self.ctx.settings['telemetry'], 'on')
        self.assertEqual(self.ctx.settings['telemetry_asked'], self.app.reporter.version)
        self.assertTrue(self.logged(f'telemetry: asked after the update to {self.app.reporter.version}: send'))
        self.app.tasks.wait()
        self.assertEqual(self.server.hits, [])
        self.assertIsNone(telemetry.pending_summary(self.game))
        self.write_summary()
        self.assertFalse(self.app.after_game_reports())
        self.app.tasks.wait()
        self.assertEqual([h[1] for h in self.server.hits], ['/v1/session'])
        self.app.start_reports()
        self.assertEqual(self.app.screen, 'home')
        self.app.reporter.version = '99.0.0'
        self.app.start_reports()
        self.assertEqual(self.app.screen, 'home')

    def test_dont_send_keeps_them_off_and_the_next_version_asks_again(self):
        self.write_summary()
        self.app.start_reports()
        self.app.move(1)
        self.assertEqual(self.label(), "Don't Send")
        self.app.activate()
        self.assertEqual(self.ctx.settings['telemetry'], 'off')
        self.assertEqual(self.app.screen, 'home')
        self.assertEqual(self.app.message, '')
        self.assertIsNone(telemetry.pending_summary(self.game))
        self.app.start_reports()
        self.assertEqual(self.app.screen, 'home')
        self.write_summary()
        self.assertFalse(self.app.after_game_reports())
        self.assertEqual(self.app.screen, 'home')
        self.app.reporter.version = '99.0.0'
        self.app.start_reports()
        self.assertEqual(self.app.screen, 'consent')
        self.assertEqual(self.label(), 'Send')
        self.app.tasks.wait()
        self.assertEqual(self.server.hits, [])

    def test_see_what_is_sent_from_the_question(self):
        self.write_summary()
        self.app.start_reports()
        self.app.move(2)
        self.assertEqual(self.label(), 'See What Is Sent')
        self.app.activate()
        self.assertEqual(self.app.screen, 'sent_view')
        first = self.app.overlay.yview()[0]
        self.app.move(1)
        self.root.update()
        self.assertGreater(self.app.overlay.yview()[0], first)
        self.app.back()
        self.assertEqual(self.app.screen, 'consent')
        self.assertEqual(self.ctx.settings['telemetry'], '')
        self.assertEqual(self.ctx.settings['telemetry_asked'], '')

    def test_the_countdown_turns_reports_on_and_the_next_start_says_so_once(self):
        self.write_summary()
        self.app.start_reports()
        self.app.question_tick()
        self.assertEqual(self.ctx.settings['telemetry'], '')
        self.app.question_deadline = time.monotonic() - 0.1
        self.app.question_tick()
        self.assertEqual(self.ctx.settings['telemetry'], 'on')
        self.assertEqual(self.app.screen, 'home')
        self.assertTrue(self.logged(f'asked after the update to {self.app.reporter.version}: timeout (on)'))
        self.app.tasks.wait()
        self.assertEqual(self.server.hits, [])
        self.assertIsNone(telemetry.pending_summary(self.game))
        self.app.message = ''
        self.app.start_reports()
        self.assertEqual(self.app.screen, 'home')
        self.assertEqual(self.app.message, self.app_module.REPORTS_ON_NOTICE)
        self.app.message = ''
        self.app.start_reports()
        self.assertEqual(self.app.message, '')

    def test_the_question_s_text_is_whole_inside_its_panel(self):
        app, c = self.app_module, self.app.canvas
        self.app.start_reports()
        for stopped in (False, True):
            if stopped:
                self.app.move(1)
                self.assertIsNone(self.app.question_deadline)
            for focus in self.app.focusables():
                self.app.focus = focus
                self.app.render()
                self.root.update()
                texts = [i for i in c.find_withtag('desc') if c.type(i) == 'text']
                self.assertEqual(c.itemcget(texts[0], 'text'), 'Help improve NX2Saria?')
                self.assertEqual(c.itemcget(texts[1], 'text'), self.question_line())
                boxes = [c.bbox(i) for i in texts]
                where = (stopped, self.app.rows[focus].label, boxes[0], boxes[-1])
                self.assertGreaterEqual(min(b[1] for b in boxes), self.app.X(112), where)
                self.assertLessEqual(max(b[3] for b in boxes), self.app.X(app.H - 30), where)
                self.assertLessEqual(max(b[2] for b in boxes), self.app.X(app.DESC[0] + app.DESC[1]), where)
                self.assertEqual(boxes, sorted(boxes, key=lambda b: b[1]), where)

    def test_the_reports_screen_s_texts_are_whole_inside_their_panel(self):
        app, c = self.app_module, self.app.canvas
        for state, closed in (('', ''), ('on', ''), ('off', self.app.reporter.version)):
            self.ctx.settings.update(telemetry=state, telemetry_closed=closed)
            self.app.show('reports')
            for focus in self.app.focusables():
                self.app.focus = focus
                self.app.render()
                self.root.update()
                boxes = [c.bbox(i) for i in c.find_withtag('desc') if c.type(i) == 'text']
                where = (state, bool(closed), self.app.rows[focus].label)
                self.assertGreaterEqual(min(b[1] for b in boxes), self.app.X(112), where)
                self.assertLessEqual(max(b[3] for b in boxes), self.app.X(app.H - 30), where)

    def test_the_countdown_shows_the_seconds_left(self):
        self.app.start_reports()
        self.app.question_deadline = time.monotonic() + 12.5
        self.assertIn('turn on in 13 s or', self.question_line())
        self.app.question_deadline = time.monotonic() + 0.4
        self.assertIn('turn on in 1 s or', self.question_line())

    def test_any_input_stops_the_countdown(self):
        Event = collections.namedtuple('Event', 'x y x_root y_root')
        for name, touch in (('a key', lambda: self.app.move(1)), ('left or right', lambda: self.app.side(1)),
                            ('the mouse over the window', lambda: self.app.on_motion(Event(300, 300, 300, 300))),
                            ('See What Is Sent', lambda: self.app.open_sent('consent'))):
            self.ctx.settings.update(telemetry='', telemetry_asked='')
            self.app.start_reports()
            self.assertIsNotNone(self.app.question_deadline, name)
            touch()
            self.assertIsNone(self.app.question_deadline, name)
            if self.app.screen == 'sent_view':
                self.app.back()
            self.assertEqual(self.app.screen, 'consent', name)
            self.assertIn('turn on when you choose Send or close this window', self.question_line())
            self.assertNotIn(' s or', self.question_line())
            for _ in range(3):
                self.app.question_tick()
            self.assertEqual(self.ctx.settings['telemetry'], '', name)
            self.app.question = None

    def test_closing_the_question_turns_reports_on(self):
        self.app.start_reports()
        self.app.back()
        self.assertEqual(self.ctx.settings['telemetry'], 'on')
        self.assertEqual(self.app.screen, 'home')
        self.assertEqual(self.app.message, self.app_module.REPORTS_ON_NOTICE)
        self.assertTrue(self.logged(': closed (on)'))

    def test_closing_the_window_over_the_question_turns_reports_on(self):
        self.app.start_reports()
        self.app.move(1)
        with mock.patch.object(self.root, 'destroy'):
            self.app.on_close()
        self.assertEqual(self.ctx.settings['telemetry'], 'on')
        self.assertEqual(self.ctx.settings['telemetry_notice'], '1')
        self.assertTrue(self.logged(': closed (on)'))

    def test_a_signal_that_ends_the_launcher_is_no_answer(self):
        self.app.start_reports()
        with mock.patch.object(self.root, 'destroy'):
            self.app.on_close('ended by SIGTERM', time.monotonic())
        self.assertEqual(self.ctx.settings['telemetry'], '')
        self.assertEqual(self.ctx.settings['telemetry_asked'], '')

    def test_closing_the_window_elsewhere_changes_nothing(self):
        self.app.start_reports()
        self.app.move(1)
        self.app.activate()
        with mock.patch.object(self.root, 'destroy'):
            self.app.on_close()
        self.assertEqual(self.ctx.settings['telemetry'], 'off')

    def test_no_question_over_the_first_run_screen_or_in_a_preview(self):
        self.assertEqual(self.app.screen, 'first_run')
        self.assertIsNone(self.app.question)
        self.app.open_screen('consent')
        self.assertEqual(self.app.screen, 'consent')
        self.assertIsNone(self.app.question_deadline)
        self.assertIn(f'turn on in {telemetry.QUESTION_SECONDS} s', self.question_line())
        self.app.back()
        self.assertEqual(self.ctx.settings['telemetry'], '')
        self.assertEqual(self.ctx.settings['telemetry_asked'], '')

    def test_the_settings_switch_from_not_asked_yet(self):
        self.app.show('reports')
        self.assertEqual(self.app.focused().get(), 'Not Asked Yet')
        self.app.side(1)
        self.assertEqual(self.app.screen, 'reports')
        self.assertEqual(self.ctx.settings['telemetry'], 'on')
        self.app.side(1)
        self.assertEqual(self.ctx.settings['telemetry'], 'off')
        self.assertFalse(self.app.reporter.question_due())

    def test_reports_screen_and_comment_walked_with_keys(self):
        self.app.show('settings')
        self.app.focus = self.app.row_index(self.app.rows, 'Session Reports')
        self.app.activate()
        self.assertEqual(self.app.screen, 'reports')
        self.assertEqual(self.app.focused().get(), 'Not Asked Yet')
        self.app.move(2)
        self.assertEqual(self.label(), 'Send a Comment')
        self.app.activate()
        self.assertEqual(self.app.screen, 'comment')
        self.assertEqual(self.label(), 'Your Comment')
        self.app.activate()
        self.assertTrue(self.app.typing)
        self.app.overlay.insert('end', 'The launcher works well. ' + 'z' * 5000)
        self.root.update()
        self.app.comment_changed()
        self.assertEqual(len(self.app.comment_draft), telemetry.TEXT_LIMIT)
        self.app.back()
        self.assertFalse(self.app.typing)
        self.assertEqual(self.app.screen, 'comment')
        self.app.move(1)
        self.assertEqual(self.label(), 'Attach the Last Log Lines')
        self.app.move(1)
        self.assertEqual(self.label(), 'Send')
        self.app.activate()
        self.app.tasks.wait()
        self.assertEqual(self.app.comment_message, 'Sent. Thank you.')
        self.assertEqual(self.app.comment_draft, '')
        self.assertEqual(json.loads(self.server.hits[0][3])['text'][:25], 'The launcher works well. ')
        self.assertEqual(self.ctx.settings['telemetry'], '')
        self.app.back()
        self.assertEqual(self.app.screen, 'reports')
        self.assertEqual([r.label for r in self.app.rows if r.label],
                         ['Session Reports', 'See What Is Sent', 'Send a Comment', 'Ask After a Crash',
                          'Back'])
        self.assertEqual([h[0] for h in self.server.hits], ['POST'])

    def test_screen_option_opens_a_screen(self):
        for name, screen in (('consent', 'consent'), ('sent', 'sent_view'), ('comment', 'comment'),
                             ('reports', 'reports')):
            self.app.open_screen(name)
            self.root.update()
            self.assertEqual(self.app.screen, screen)
        self.assertEqual(self.server.hits, [])


@unittest.skipIf(window_toolkit(), window_toolkit())
class UpdateStartBase(WindowBase):
    ARGV = ['--continue-update', '0.18.0']
    FAILS = False

    def setUp(self):
        from launcher import build, jobs
        from launcher.ui import app
        test = self

        class UpdateJob(jobs.ContinueUpdateJob):
            claims = ()

            def run(self):
                test.release.wait(20)
                if test.fails:
                    raise RuntimeError('the compiler stopped')

        self.release = threading.Event()
        self.addCleanup(self.release.set)
        self.fails = self.FAILS
        for target, name, new in ((build, 'installed', lambda gd: bool(gd)), (jobs, 'ContinueUpdateJob', UpdateJob),
                                  (jobs, 'interrupted', lambda ctx: None), (app.App, 'check_now', lambda a: None),
                                  (app.winsys, 'keep_awake', lambda on, log: None)):
            p = mock.patch.object(target, name, new)
            p.start()
            self.addCleanup(p.stop)
        super().setUp()

    def job_ends(self):
        self.release.set()
        end = time.monotonic() + 20
        while self.app.job.result is None and time.monotonic() < end:
            time.sleep(0.01)
        self.app.job_rev = self.app.job.rev
        self.app.job_finished()
        self.root.update()


@unittest.skipIf(window_toolkit(), window_toolkit())
class UpdateStartTest(UpdateStartBase):
    def test_the_start_that_finishes_an_update_asks_when_its_job_ends(self):
        self.assertEqual(self.app.screen, 'job')
        self.assertIsNone(self.app.question)
        self.assertTrue(self.logged('telemetry: the question waits for the update to finish'))
        self.job_ends()
        self.assertEqual(self.app.job.result, 'ok')
        self.assertEqual(self.app.screen, 'consent')
        self.assertEqual(self.app.question, 'ask')
        self.assertEqual(self.label(), 'Send')
        self.assertIsNone(self.app.question_deadline)
        self.assertIn('turn on when you choose Send or close this window', self.question_line())
        self.assertTrue(self.logged(f'telemetry: the question opens after {self.app.job.doing()}, without its '
                                    'countdown'))
        self.app.move(1)
        self.app.activate()
        self.assertEqual(self.app.screen, 'home')
        self.assertEqual(self.ctx.settings['telemetry'], 'off')
        self.assertTrue(self.logged(f"telemetry: asked after the update to {self.app.reporter.version}: don't send"))
        self.assertIn('NX2Saria is now', self.app.message)
        self.app.reporter.version = '99.0.0'
        self.app.home_reports()
        self.assertEqual(self.app.screen, 'home')

    def test_the_rest_of_the_start_s_reports_runs_there_too(self):
        self.ctx.settings['telemetry'] = 'on'
        self.ctx.settings['telemetry_notice'] = '1'
        self.write_summary()
        self.job_ends()
        self.assertEqual(self.app.screen, 'home')
        self.app.tasks.wait()
        self.assertEqual([h[1] for h in self.server.hits], ['/v1/session'])
        self.assertIn('NX2Saria is now', self.app.message)
        self.assertEqual(self.ctx.settings['telemetry_notice'], '1')
        self.assertFalse(self.logged('telemetry: told that session reports are on'))


@unittest.skipIf(window_toolkit(), window_toolkit())
class FailedUpdateStartTest(UpdateStartBase):
    FAILS = True

    def test_no_question_over_a_failed_update_until_it_is_left_or_done(self):
        self.job_ends()
        self.assertEqual(self.app.job.result, 'failed')
        self.assertEqual(self.app.screen, 'job')
        self.assertIsNone(self.app.question)
        self.assertEqual(self.ctx.settings['telemetry_asked'], '')
        self.fails = False
        self.release.clear()
        self.app.retry_job()
        self.assertEqual(self.app.screen, 'job')
        self.assertIsNone(self.app.question)
        self.job_ends()
        self.assertEqual(self.app.screen, 'consent')
        self.assertIsNone(self.app.question_deadline)


if __name__ == '__main__':
    unittest.main()
