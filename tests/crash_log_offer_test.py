import json
import os
import sys
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))

from launcher import crash_offer, game_session, telemetry  # noqa: E402
import private_paths  # noqa: E402
from telemetry_launcher_test import Base, VERSION, WindowBase, summary, window_toolkit  # noqa: E402

HEAD = ['[host] cfg: read /home/ana/Games/NX2Saria/nx2saria.ini: 19 keys',
        f'[host] version: NX2Saria {VERSION}, game 1.1.1',
        '[host] vk: using gpu 0: NVIDIA GeForce RTX 3060',
        '[host] vk caps: driver NVIDIA 581.57',
        '[host] patches: fps60 on, ostep on, zstd on']
FATAL = 'FATAL: vk: the graphics device was lost (VK_ERROR_DEVICE_LOST); user ana on maria-pc'
NOTE = '[launcher] the game ended with code 72 at 2026-01-10 21:14:03'
MESSAGE = 'The game ended with a fatal error (code 72): vk: the graphics device was lost.'


def play_lines(n, width=40):
    return [f'[host] frame {i:05d} ' + 'y' * width for i in range(n)]


class LogBase(Base):
    def write_log(self, lines, rotate_before=True):
        logs = self.game / 'logs'
        if rotate_before:
            (logs / 'nx2saria.1.log').write_text('the run before\n', encoding='utf-8')
            old = time.time() - 600
            os.utime(logs / 'nx2saria.1.log', (old, old))
        (logs / 'nx2saria.log').write_text('\n'.join(lines) + '\n', encoding='utf-8')

    def crash_log(self, play=600):
        self.write_log(HEAD + play_lines(play) + [FATAL, NOTE])

    def offer_file(self):
        return self.game / 'logs' / crash_offer.OFFER

    def make_offer(self, kind='device_lost', asked=False):
        with mock.patch.dict(os.environ, {telemetry.URL_ENV: self.server.url}):
            return crash_offer.game_ended(self.settings, self.game, kind, asked, MESSAGE, self.lines.append)

    def offer_lines(self):
        return [ln for ln in self.lines if ln.startswith('telemetry: crash log')]


class EndKindTest(unittest.TestCase):
    def end(self, message='The game ended.'):
        return game_session.GameEnd('log', message, '', False)

    def test_the_three_fatal_codes_have_names(self):
        self.assertEqual(game_session.end_kind(70, self.end()), 'fatal')
        self.assertEqual(game_session.end_kind(71, self.end()), 'crash')
        self.assertEqual(game_session.end_kind(72, self.end()), 'device_lost')

    def test_another_code_is_named_by_its_text(self):
        self.assertEqual(game_session.end_kind(3, self.end()), 'code 3')
        self.assertEqual(game_session.end_kind(0xC0000005, self.end()), 'code 0xC0000005 (access violation)')

    def test_an_end_without_a_sentence_has_no_kind(self):
        self.assertEqual(game_session.end_kind(0, self.end('')), '')
        self.assertEqual(game_session.end_kind(None, self.end('')), '')

    def test_a_fatal_line_after_a_restart_is_fatal(self):
        self.assertEqual(game_session.end_kind(0, self.end()), 'fatal')
        self.assertEqual(game_session.end_kind(None, self.end()), 'fatal')

    @unittest.skipIf(os.name == 'nt', 'signals end a process on Linux')
    def test_a_signal_is_named(self):
        self.assertEqual(game_session.end_kind(-9, self.end()), 'code -9 (ended by SIGKILL)')


@unittest.skipIf(os.name == 'nt', 'signals end a process on Linux')
class AskedEndTest(LogBase):
    def test_sigterm_sigint_and_sighup_without_a_fatal_line_are_asked(self):
        self.write_log(HEAD + play_lines(5))
        for code in (-15, -2, -1):
            self.assertTrue(game_session.end_asked(self.game, code), code)

    def test_a_fatal_line_makes_an_asked_signal_an_error(self):
        self.write_log(HEAD + play_lines(5) + [FATAL])
        self.assertFalse(game_session.end_asked(self.game, -15))

    def test_a_kill_a_fault_an_abort_and_an_exit_code_are_not_asked(self):
        self.write_log(HEAD + play_lines(5))
        for code in (-9, -11, -6, 1, 72, 0, None):
            self.assertFalse(game_session.end_asked(self.game, code), code)


class GameEndedTest(LogBase):
    def test_an_abnormal_end_leaves_the_offer_waiting_and_sends_nothing(self):
        self.crash_log()
        self.assertTrue(self.make_offer())
        data = json.loads(self.offer_file().read_text(encoding='utf-8'))
        self.assertEqual((data['kind'], data['message']), ('device_lost', MESSAGE))
        self.assertEqual(data['log'], crash_offer.log_stamp(self.game))
        self.assertEqual(self.offer_lines(),
                         ["telemetry: crash log offer: waits for the launcher's main screen (end: device_lost)"])
        self.assertEqual(self.server.hits, [])

    def test_a_normal_end_leaves_nothing(self):
        self.crash_log()
        self.assertFalse(self.make_offer(kind=''))
        self.assertFalse(self.offer_file().exists())
        self.assertEqual(self.offer_lines(), [])

    def test_an_asked_close_leaves_nothing_and_says_so(self):
        self.crash_log()
        self.assertFalse(self.make_offer(kind='code -15 (ended by SIGTERM)', asked=True))
        self.assertFalse(self.offer_file().exists())
        self.assertEqual(self.offer_lines(), ['telemetry: crash log offer: not made: the game was asked to close '
                                              '(end: code -15 (ended by SIGTERM))'])

    def test_off_in_settings_leaves_nothing_and_says_so(self):
        self.crash_log()
        self.settings[crash_offer.SETTING] = 'off'
        self.assertFalse(self.make_offer())
        self.assertFalse(self.offer_file().exists())
        self.assertEqual(self.offer_lines(), ['telemetry: crash log offer: not made: off in Settings '
                                              '(end: device_lost)'])

    def test_no_server_leaves_nothing(self):
        self.crash_log()
        with mock.patch.dict(os.environ, {telemetry.URL_ENV: 'none'}):
            self.assertFalse(crash_offer.game_ended(self.settings, self.game, 'crash', False, MESSAGE,
                                                    self.lines.append))
        self.assertFalse(self.offer_file().exists())
        self.assertEqual(self.lines, [])

    def test_the_offer_is_on_by_default_and_the_start_line_says_it(self):
        self.assertTrue(crash_offer.on(self.settings))
        crash_offer.log_state(self.settings, self.lines.append)
        self.settings[crash_offer.SETTING] = 'off'
        crash_offer.log_state(self.settings, self.lines.append)
        self.assertTrue(self.lines[0].startswith('telemetry: crash log offer: on ('))
        self.assertTrue(self.lines[1].startswith('telemetry: crash log offer: off ('))


class PendingTest(LogBase):
    def test_the_offer_stays_while_the_log_is_that_session_s(self):
        self.crash_log()
        self.make_offer()
        data = crash_offer.pending(self.game, self.settings, self.lines.append)
        self.assertEqual(data['kind'], 'device_lost')
        self.assertTrue(self.offer_file().exists())

    def test_another_session_s_log_drops_it_without_asking(self):
        self.crash_log()
        self.make_offer()
        self.write_log(HEAD + play_lines(3))
        self.assertIsNone(crash_offer.pending(self.game, self.settings, self.lines.append))
        self.assertFalse(self.offer_file().exists())
        self.assertEqual(self.offer_lines()[-1], "telemetry: crash log offer: dropped: the game's log is no longer "
                                                 'the one of that session (end: device_lost)')

    def test_a_log_that_grew_drops_it(self):
        self.crash_log()
        self.make_offer()
        with open(self.game / 'logs' / 'nx2saria.log', 'a', encoding='utf-8') as f:
            f.write('[host] one more line\n')
        self.assertIsNone(crash_offer.pending(self.game, self.settings, self.lines.append))

    def test_switched_off_since_drops_it(self):
        self.crash_log()
        self.make_offer()
        self.settings[crash_offer.SETTING] = 'off'
        self.assertIsNone(crash_offer.pending(self.game, self.settings, self.lines.append))
        self.assertFalse(self.offer_file().exists())
        self.assertIn('dropped: switched off in Settings since', self.offer_lines()[-1])

    def test_a_broken_file_is_dropped(self):
        self.crash_log()
        self.offer_file().write_text('{not json', encoding='utf-8')
        self.assertIsNone(crash_offer.pending(self.game, self.settings, self.lines.append))
        self.assertFalse(self.offer_file().exists())

    def test_nothing_pending_logs_nothing(self):
        self.assertIsNone(crash_offer.pending(self.game, self.settings, self.lines.append))
        self.assertEqual(self.lines, [])


class LogPartsTest(LogBase):
    def test_the_start_goes_to_the_patches_line_and_the_end_is_the_last_200_lines(self):
        self.crash_log()
        head, tail, whole = crash_offer.log_parts(self.game)
        self.assertEqual(len(head), len(HEAD))
        self.assertTrue(head[-1].startswith('[host] patches:'))
        self.assertEqual(len(tail), crash_offer.TAIL_LINES)
        self.assertEqual(tail[-1], NOTE)
        self.assertFalse(whole)

    def test_folders_account_and_computer_are_written_short(self):
        self.crash_log()
        head, tail, _ = crash_offer.log_parts(self.game)
        text = '\n'.join(head + tail)
        self.assertIn('~/Games/NX2Saria/nx2saria.ini', text)
        for private in ('/home/ana', 'maria-pc'):
            self.assertNotIn(private, text)

    def test_a_short_log_goes_whole_without_a_repeated_line(self):
        lines = HEAD + play_lines(20) + [FATAL, NOTE]
        self.write_log(lines)
        head, tail, whole = crash_offer.log_parts(self.game)
        self.assertEqual(head, [])
        self.assertEqual(len(tail), len(lines))
        self.assertTrue(whole)

    def test_the_start_stops_where_the_end_begins(self):
        lines = HEAD + play_lines(crash_offer.TAIL_LINES - 4) + [FATAL, NOTE]
        self.write_log(lines)
        head, tail, whole = crash_offer.log_parts(self.game)
        self.assertEqual(len(head) + len(tail), len(lines))
        self.assertEqual(len(tail), crash_offer.TAIL_LINES)
        self.assertEqual(len(head), len(lines) - crash_offer.TAIL_LINES)
        self.assertTrue(whole)
        self.assertEqual(len(set(head + tail)), len(lines))

    def test_without_a_patches_line_the_start_goes_to_its_caps(self):
        self.write_log(play_lines(900))
        head, tail, _ = crash_offer.log_parts(self.game)
        self.assertEqual(len(head), crash_offer.HEAD_LINES)
        self.write_log(play_lines(900, width=400))
        head, tail, _ = crash_offer.log_parts(self.game)
        self.assertLess(len(head), crash_offer.HEAD_LINES)
        self.assertLessEqual(len('\n'.join(head).encode('utf-8')) + 1, crash_offer.HEAD_LIMIT)
        self.assertGreater(len('\n'.join(head + ['x' * 420]).encode('utf-8')) + 1, crash_offer.HEAD_LIMIT)

    def test_the_end_keeps_to_56_kb_and_keeps_the_last_lines(self):
        self.write_log(HEAD + play_lines(900, width=500) + [NOTE])
        head, tail, _ = crash_offer.log_parts(self.game)
        size = len('\n'.join(tail).encode('utf-8')) + 1
        self.assertLessEqual(size, crash_offer.TAIL_LIMIT)
        self.assertGreater(size, crash_offer.TAIL_LIMIT - 600)
        self.assertLess(len(tail), crash_offer.TAIL_LINES)
        self.assertEqual(tail[-1], NOTE)

    def test_200_lines_of_a_real_width_fit_whole(self):
        self.write_log(HEAD + play_lines(900, width=210) + [NOTE])
        head, tail, _ = crash_offer.log_parts(self.game)
        self.assertEqual(len(tail), crash_offer.TAIL_LINES)
        self.assertGreater(len('\n'.join(tail).encode('utf-8')), telemetry.TAIL_LIMIT)

    def test_no_log_gives_nothing(self):
        self.assertEqual(crash_offer.log_parts(self.tmp / 'no game'), ([], [], False))

    def test_a_log_far_larger_than_what_is_read(self):
        self.write_log(HEAD + play_lines(20000, width=80) + [FATAL, NOTE])
        head, tail, whole = crash_offer.log_parts(self.game)
        self.assertEqual(len(head), len(HEAD))
        self.assertEqual(len(tail), crash_offer.TAIL_LINES)
        self.assertTrue(tail[0].startswith('[host] frame 19802 '))
        self.assertFalse(whole)


class RequestTest(LogBase):
    def prepare(self):
        return crash_offer.prepare(self.reporter(), self.game)

    def test_the_body_is_a_crash_log_without_an_id(self):
        self.crash_log()
        s = summary('device_lost', 'vk: the graphics device was lost')
        self.write_summary(s)
        sending = self.prepare()
        self.assertEqual((sending.request.method, sending.request.path), ('POST', '/v1/feedback'))
        sent = json.loads(sending.request.data)
        self.assertEqual(set(sent), {'kind', 'launcher_version', 'text', 'summary', 'log_tail', 'log_head'})
        self.assertEqual((sent['kind'], sent['text'], sent['launcher_version']), ('crash', '', VERSION))
        self.assertEqual(sent['summary'], s)
        self.assertEqual(len(sent['log_head'].splitlines()), sending.head_lines)
        self.assertEqual(len(sent['log_tail'].splitlines()), sending.tail_lines)
        self.assertEqual(sending.log_bytes, len((sent['log_head'] + sent['log_tail']).encode('utf-8')))
        self.assertTrue(sending.summary)
        self.assertLessEqual(len(sending.request.data), crash_offer.BODY_LIMIT)

    def test_the_report_goes_also_before_the_question_was_answered_and_never_when_reports_are_off(self):
        self.crash_log()
        self.write_summary()
        self.settings['telemetry'] = ''
        self.assertIsNotNone(json.loads(self.prepare().request.data)['summary'])
        self.settings['telemetry'] = 'off'
        sending = self.prepare()
        self.assertIsNone(json.loads(sending.request.data)['summary'])
        self.assertFalse(sending.summary)

    def test_the_report_of_an_earlier_session_is_left_out(self):
        self.crash_log()
        old = self.write_summary()
        then = time.time() - 900
        os.utime(old, (then, then))
        self.assertIsNone(json.loads(self.prepare().request.data)['summary'])

    def test_the_report_already_sent_for_that_session_goes(self):
        self.crash_log()
        self.write_summary()
        telemetry.mark_sent(self.game)
        self.assertIsNotNone(json.loads(self.prepare().request.data)['summary'])

    def test_a_short_log_has_no_start(self):
        self.write_log(HEAD + [FATAL, NOTE])
        sending = self.prepare()
        sent = json.loads(sending.request.data)
        self.assertIsNone(sent['log_head'])
        self.assertTrue(sending.whole)
        self.assertEqual(sending.head_lines, 0)

    def test_an_empty_or_missing_log_cannot_be_offered(self):
        self.assertEqual(self.prepare(), telemetry.Result(False, "the game's log is empty"))
        (self.game / 'logs' / 'nx2saria.log').write_text('', encoding='utf-8')
        self.assertEqual(self.prepare(), telemetry.Result(False, "the game's log is empty"))

    def test_escapes_that_grow_the_body_cost_the_oldest_lines_of_the_end(self):
        self.write_log(HEAD + [f'[host] {i:04d} ' + '\\' * 270 for i in range(400)] + [NOTE])
        sending = self.prepare()
        self.assertLessEqual(len(sending.request.data), crash_offer.BODY_LIMIT)
        self.assertLess(sending.tail_lines, crash_offer.TAIL_LINES)
        self.assertTrue(json.loads(sending.request.data)['log_tail'].endswith(NOTE + '\n'))

    def test_a_closed_server_and_no_server_prepare_nothing(self):
        self.crash_log()
        self.settings['telemetry_closed'] = VERSION
        self.assertFalse(self.prepare().ok)
        self.settings['telemetry_closed'] = ''
        self.assertFalse(crash_offer.prepare(self.reporter(url=''), self.game).ok)

    def test_what_goes_names_the_lines(self):
        self.crash_log()
        text = crash_offer.what_goes(self.prepare())
        self.assertIn("205 lines of the game's log", text)
        self.assertIn('its first 5 and its last 200', text)
        self.assertNotIn('report of that session', text)
        self.write_log(HEAD + [FATAL, NOTE])
        self.write_summary()
        text = crash_offer.what_goes(self.prepare())
        self.assertIn('the whole log of this session (7 lines, 1 KB)', text)
        self.assertIn('and the report of that session', text)

    def test_a_comment_keeps_its_four_keys_and_its_24_kb(self):
        self.write_log(HEAD + play_lines(900, width=210) + [NOTE])
        got = self.reporter().send_feedback('It fell in the Forest Temple.', True, self.game)
        self.assertTrue(got.ok, got.reason)
        sent = json.loads(self.server.hits[0][3])
        self.assertEqual(set(sent), {'launcher_version', 'text', 'summary', 'log_tail'})
        self.assertLessEqual(len(sent['log_tail'].encode('utf-8')), telemetry.TAIL_LIMIT)


class AlwaysShortTest(LogBase):
    def setUp(self):
        super().setUp()
        off = mock.patch.dict(os.environ, {private_paths.SWITCH: '0'})
        off.start()
        self.addCleanup(off.stop)
        self.crash_log()
        self.on_disk = (self.game / 'logs' / 'nx2saria.log').read_bytes()

    def test_the_switch_is_off_for_the_launcher_s_own_lines(self):
        self.assertEqual(private_paths.shorten('/home/ana/Games'), '/home/ana/Games')

    def test_a_crash_log_is_written_short_all_the_same(self):
        reporter = self.reporter()
        sending = crash_offer.prepare(reporter, self.game)
        text = sending.request.data.decode('utf-8')
        self.assertIn('~/Games/NX2Saria/nx2saria.ini', text)
        for private in ('/home/ana', 'maria-pc'):
            self.assertNotIn(private, text)
        self.assertEqual((self.game / 'logs' / 'nx2saria.log').read_bytes(), self.on_disk)
        self.assertIn(b'/home/ana', self.on_disk)
        crash_offer.finish(reporter, crash_offer.Offers(), sending, reporter.post(sending.request))
        self.assertEqual(self.server.hits[0][3], sending.request.data)
        self.assertIn('telemetry: personal folders written short in 2 of 205 log lines for sending; the log on disk '
                      'is not changed (NX2SARIA_LOG_SHORT_PATHS=0 leaves them in full there)', self.lines)
        self.assertFalse(any('ana' in ln.split() or '/home/ana' in ln for ln in self.lines))

    def test_a_comment_s_log_lines_are_written_short_all_the_same(self):
        self.write_log(HEAD + [FATAL, NOTE])
        on_disk = (self.game / 'logs' / 'nx2saria.log').read_bytes()
        got = self.reporter().send_feedback('It fell in the Forest Temple.', True, self.game)
        self.assertTrue(got.ok, got.reason)
        sent = json.loads(self.server.hits[0][3])
        self.assertIn('~/Games/NX2Saria/nx2saria.ini', sent['log_tail'])
        for private in ('/home/ana', 'maria-pc'):
            self.assertNotIn(private, sent['log_tail'])
        self.assertEqual((self.game / 'logs' / 'nx2saria.log').read_bytes(), on_disk)
        self.assertIn('telemetry: personal folders written short in 2 of 7 log lines for sending; the log on disk '
                      'is not changed (NX2SARIA_LOG_SHORT_PATHS=0 leaves them in full there)', self.lines)

    def test_a_comment_without_the_log_says_nothing_of_it(self):
        self.assertTrue(self.reporter().send_feedback('No log.', False, self.game).ok)
        self.assertFalse(any('written short' in ln for ln in self.lines))

    def test_the_crash_summary_s_fatal_line_is_written_short_all_the_same(self):
        self.write_log(['[host] version: NX2Saria ' + VERSION + ', game 1.1.1',
                        'FATAL: crash at /home/ana/Games/NX2Saria/ocarina on maria-pc'])
        made = telemetry.synthesize_crash(self.game)
        self.assertEqual(made['fatal'], 'crash at ~/Games/NX2Saria/ocarina on %COMPUTERNAME%')


class ShortNoteTest(LogBase):
    def test_a_log_already_short_adds_no_line(self):
        self.write_log([ln.replace('/home/ana', '~') for ln in HEAD] + play_lines(30) + [NOTE])
        reporter = self.reporter()
        sending = crash_offer.prepare(reporter, self.game)
        self.assertEqual(sending.shortened, '')
        crash_offer.finish(reporter, crash_offer.Offers(), sending, reporter.post(sending.request))
        self.assertFalse(any('written short' in ln for ln in self.lines))

    def test_a_log_the_game_wrote_in_full_is_counted(self):
        self.crash_log()
        sending = crash_offer.prepare(self.reporter(), self.game)
        self.assertEqual(sending.shortened, 'telemetry: personal folders written short in 2 of 205 log lines for '
                                            'sending; the log on disk is not changed (the log had them in full)')

    def test_lines_trimmed_from_the_body_are_not_counted(self):
        self.write_log(HEAD + ['[host] /home/ana/x ' + '\\' * 270 for i in range(400)] + [NOTE])
        sending = crash_offer.prepare(self.reporter(), self.game)
        self.assertLess(sending.tail_lines, crash_offer.TAIL_LINES)
        lines = sending.head_lines + sending.tail_lines
        self.assertIn(f'written short in {lines - 4 - 1} of {lines} log lines', sending.shortened)


class AnswerTest(LogBase):
    def send(self, status):
        self.crash_log()
        self.server.status = status
        reporter = self.reporter()
        offers = crash_offer.Offers()
        offers.shown = 1
        sending = crash_offer.prepare(reporter, self.game)
        return crash_offer.finish(reporter, offers, sending, reporter.post(sending.request)), offers, sending

    def test_sent_says_lines_and_bytes(self):
        got, offers, sending = self.send(201)
        self.assertTrue(got.ok)
        self.assertEqual(self.server.hits[0][3], sending.request.data)
        self.assertEqual(self.offer_lines(), [
            f'telemetry: crash log sent (HTTP 201, 5 + 200 lines, {sending.log_bytes} bytes of log, '
            f'{len(sending.request.data)} bytes in all; this run of the launcher: 1 shown, 1 sent, 0 declined, '
            '0 failed)'])

    def test_a_server_older_than_crash_logs_answers_400_once(self):
        got, offers, _ = self.send(400)
        self.assertFalse(got.ok)
        self.assertTrue(offers.refused)
        self.assertEqual(len(self.server.hits), 1)
        self.assertEqual(len(self.offer_lines()), 1)
        self.assertIn('crash log not sent (HTTP 400): this server does not take it; not tried again',
                      self.offer_lines()[0])
        self.assertIn('1 shown, 0 sent, 0 declined, 1 failed', self.offer_lines()[0])

    def test_410_closes_reports_for_this_version(self):
        got, offers, _ = self.send(410)
        self.assertFalse(got.ok)
        self.assertEqual(self.settings['telemetry_closed'], VERSION)

    def test_a_server_error_is_not_tried_again(self):
        got, offers, _ = self.send(503)
        self.assertEqual(got, telemetry.Result(False, 'the server had a problem (HTTP 503)'))
        self.assertFalse(offers.refused)
        self.assertEqual(len(self.server.hits), 1)
        self.assertIn('crash log not sent (HTTP 503); not tried again', self.offer_lines()[0])

    def test_no_connection_is_said_once(self):
        self.crash_log()
        reporter = self.reporter(url='http://127.0.0.1:9')
        offers = crash_offer.Offers()
        sending = crash_offer.prepare(reporter, self.game)
        got = crash_offer.finish(reporter, offers, sending, reporter.post(sending.request))
        self.assertEqual(got.reason, 'no connection')
        self.assertEqual(offers.failed, 1)
        self.assertEqual(len(self.offer_lines()), 1)


@unittest.skipIf(window_toolkit(), window_toolkit())
class OfferWindowTest(WindowBase, LogBase):
    def setUp(self):
        super().setUp()
        self.ctx.settings['telemetry'] = 'off'
        self.ctx.settings['telemetry_asked'] = self.app.reporter.version
        self.app.show('home')
        self.root.update()

    def pend(self, kind='device_lost', play=600):
        self.crash_log(play)
        self.assertTrue(crash_offer.game_ended(self.ctx.settings, self.game, kind, False, MESSAGE, self.ctx.log))

    def open_offer(self, **kw):
        self.pend(**kw)
        self.assertTrue(self.app.offer_crash_log())
        self.app.offer_opened -= 5
        self.root.update()

    def test_the_offer_opens_on_the_main_screen_with_send_the_log_focused(self):
        self.open_offer()
        self.assertEqual(self.app.screen, 'crash_offer')
        self.assertEqual([r.label for r in self.app.rows if r.label],
                         ['Send the Log', "Don't Send", 'See What Is Sent', 'Write a Comment'])
        self.assertEqual(self.label(), 'Send the Log')
        desc = self.app.crash_offer_desc()
        self.assertEqual(desc['text'], [MESSAGE])
        self.assertIn("205 lines of the game's log", desc['fine'][0])
        self.assertTrue(self.logged('telemetry: crash log offer: shown (end: device_lost; 5 + 200 lines, '
                                    f'{self.app.offer["sending"].log_bytes} bytes of log)'))
        self.assertEqual(self.server.hits, [])
        self.assertTrue(self.offer_file().exists())

    def test_send_the_log_sends_the_bytes_the_view_shows(self):
        self.open_offer()
        self.app.move(2)
        self.assertEqual(self.label(), 'See What Is Sent')
        self.app.activate()
        self.assertEqual(self.app.screen, 'sent_view')
        shown = self.app.overlay.get('1.0', 'end-1c')
        self.assertFalse(self.app.sent_example)
        self.assertIn('205 lines', self.app.sent_note)
        self.app.back()
        self.assertEqual(self.app.screen, 'crash_offer')
        self.assertEqual(self.label(), 'See What Is Sent')
        self.assertEqual(self.server.hits, [])
        self.app.move(-2)
        self.app.activate()
        self.assertEqual(self.app.screen, 'home')
        self.assertEqual(self.app.message, 'Sending the log…')
        self.app.tasks.wait()
        self.assertEqual(self.app.message, 'The log was sent. Thank you.')
        self.assertEqual(len(self.server.hits), 1)
        method, path, _headers, body = self.server.hits[0]
        self.assertEqual((method, path), ('POST', '/v1/feedback'))
        self.assertEqual(body, shown.encode('utf-8'))
        sent = json.loads(body)
        self.assertEqual(sent['kind'], 'crash')
        self.assertIsNone(sent['summary'])
        self.assertNotIn('/home/ana', body.decode('utf-8'))
        self.assertFalse(self.offer_file().exists())
        self.assertFalse(self.app.offer_crash_log())
        self.assertEqual(self.ctx.settings['telemetry'], 'off')

    def test_dont_send_sends_nothing_and_does_not_come_back(self):
        self.open_offer()
        self.app.move(1)
        self.assertEqual(self.label(), "Don't Send")
        self.app.activate()
        self.assertEqual(self.app.screen, 'home')
        self.assertFalse(self.offer_file().exists())
        self.assertFalse(self.app.offer_crash_log())
        self.app.tasks.wait()
        self.assertEqual(self.server.hits, [])
        self.assertTrue(self.logged("telemetry: crash log offer: declined (don't send; end: device_lost; this run of "
                                    'the launcher: 1 shown, 0 sent, 1 declined, 0 failed)'))

    def test_esc_is_dont_send(self):
        self.open_offer()
        self.app.back()
        self.assertEqual(self.app.screen, 'home')
        self.assertFalse(self.offer_file().exists())
        self.assertEqual(self.server.hits, [])
        self.assertTrue(self.logged('telemetry: crash log offer: declined (closed; end: device_lost; this run of the '
                                    'launcher: 1 shown, 0 sent, 1 declined, 0 failed)'))

    def test_closing_the_window_is_dont_send(self):
        self.open_offer()
        with mock.patch.object(self.root, 'destroy'):
            self.app.on_close()
        self.assertFalse(self.offer_file().exists())
        self.assertEqual(self.server.hits, [])
        self.assertTrue(any('telemetry: crash log offers in this run of the launcher: 1 shown, 0 sent, 1 declined, '
                            '0 failed' in ln for ln in self.ctx.log_tail(30)))

    def test_a_signal_that_ends_the_launcher_leaves_the_offer_for_the_next_start(self):
        self.open_offer()
        with mock.patch.object(self.root, 'destroy'):
            self.app.on_close('ended by SIGTERM', time.monotonic())
        self.assertTrue(self.offer_file().exists())
        self.assertEqual(self.server.hits, [])

    def test_the_first_second_takes_no_input(self):
        self.pend()
        self.assertTrue(self.app.offer_crash_log())
        self.app.activate()
        self.app.back()
        self.assertEqual(self.app.screen, 'crash_offer')
        self.app.tasks.wait()
        self.assertEqual(self.server.hits, [])
        self.app.offer_opened -= crash_offer.GUARD_SECONDS
        self.app.activate()
        self.app.tasks.wait()
        self.assertEqual(len(self.server.hits), 1)

    def test_write_a_comment_opens_the_comment_with_the_log_attached(self):
        self.open_offer()
        self.app.move(3)
        self.assertEqual(self.label(), 'Write a Comment')
        self.app.activate()
        self.assertEqual(self.app.screen, 'comment')
        self.assertTrue(self.app.comment_log)
        self.assertFalse(self.offer_file().exists())
        self.assertEqual(self.server.hits, [])
        self.assertTrue(self.logged('telemetry: crash log offer: declined (comment; end: device_lost; this run of the '
                                    'launcher: 1 shown, 0 sent, 1 declined, 0 failed)'))
        self.app.overlay.insert('end', 'It fell in the Forest Temple.')
        self.root.update()
        self.app.send_comment()
        self.app.tasks.wait()
        sent = json.loads(self.server.hits[0][3])
        self.assertEqual(set(sent), {'launcher_version', 'text', 'summary', 'log_tail'})
        self.assertTrue(sent['log_tail'].endswith(NOTE + '\n'))

    def test_the_question_about_reports_comes_first(self):
        self.ctx.settings['telemetry'] = ''
        self.ctx.settings['telemetry_asked'] = ''
        self.pend()
        self.app.start_reports()
        self.assertEqual(self.app.screen, 'consent')
        self.assertIsNone(self.app.offer)
        self.app.move(1)
        self.app.activate()
        self.assertEqual(self.ctx.settings['telemetry'], 'off')
        self.assertEqual(self.app.screen, 'crash_offer')
        self.assertEqual(self.server.hits, [])

    def test_a_start_without_a_question_offers_what_waited(self):
        self.pend(kind='crash')
        self.app.start_reports()
        self.assertEqual(self.app.screen, 'crash_offer')
        self.assertTrue(any('crash log offer: shown (end: crash;' in ln for ln in self.ctx.log_tail(30)))

    def test_a_log_of_another_session_drops_what_waited_without_asking(self):
        self.pend()
        self.write_log(HEAD + play_lines(3))
        self.app.start_reports()
        self.assertEqual(self.app.screen, 'home')
        self.assertFalse(self.offer_file().exists())
        self.assertTrue(self.logged("telemetry: crash log offer: dropped: the game's log is no longer the one of "
                                    'that session (end: device_lost)'))

    def test_never_over_another_screen_or_while_the_game_runs(self):
        self.pend()
        for screen in ('settings', 'reports', 'saves'):
            self.app.show(screen)
            self.assertFalse(self.app.offer_crash_log(), screen)
        self.app.show('home')
        with mock.patch('launcher.saves.game_running', return_value=True):
            self.assertFalse(self.app.offer_crash_log())
        self.app.game_run = object()
        self.assertFalse(self.app.offer_crash_log())
        self.app.game_run = None
        self.assertTrue(self.offer_file().exists())
        self.assertTrue(self.app.offer_crash_log())

    def test_off_in_settings_never_offers(self):
        self.app.show('reports')
        row = self.app.row_index(self.app.rows, 'Ask After a Crash')
        self.assertTrue(self.app.rows[row].get())
        self.app.focus = row
        self.app.activate()
        self.assertEqual(self.ctx.settings[crash_offer.SETTING], 'off')
        self.assertTrue(any('telemetry: crash log offer: off (' in ln for ln in self.ctx.log_tail(30)))
        self.app.show('home')
        self.crash_log()
        self.assertFalse(crash_offer.game_ended(self.ctx.settings, self.game, 'crash', False, MESSAGE, self.ctx.log))
        self.assertFalse(self.app.offer_crash_log())
        self.assertEqual(self.app.screen, 'home')

    def test_a_400_ends_the_offers_of_this_run(self):
        self.server.status = 400
        self.open_offer()
        self.app.activate()
        self.app.tasks.wait()
        self.assertEqual(self.app.message, 'The log could not be sent: the server refused it (HTTP 400).')
        self.assertEqual(len(self.server.hits), 1)
        self.pend()
        self.assertFalse(self.app.offer_crash_log())
        self.assertFalse(self.offer_file().exists())
        self.assertEqual(len(self.server.hits), 1)
        self.assertTrue(self.logged('telemetry: crash log offer: not shown: the server refused one in this run '
                                    '(end: device_lost)'))

    def test_each_end_is_offered_and_counted(self):
        for n in (1, 2, 3):
            self.open_offer()
            self.app.back()
        self.assertEqual((self.app.offers.shown, self.app.offers.declined), (3, 3))
        self.assertTrue(self.logged('this run of the launcher: 3 shown, 0 sent, 3 declined, 0 failed)'))

    def test_the_offer_s_text_is_whole_inside_its_panel(self):
        app, c = self.app_module, self.app.canvas
        fatal = ('the graphics device was lost ' * 9)[:game_session.FATAL_SHOWN]
        longest = f'The game ended with a fatal error (code 72): {fatal}. {game_session.LOG_WHERE}'
        self.crash_log()
        self.write_summary()
        crash_offer.game_ended(self.ctx.settings, self.game, 'device_lost', False, longest, self.ctx.log)
        self.assertTrue(self.app.offer_crash_log())
        for focus in self.app.focusables():
            self.app.focus = focus
            self.app.render()
            self.root.update()
            texts = [i for i in c.find_withtag('desc') if c.type(i) == 'text']
            self.assertEqual(c.itemcget(texts[0], 'text'), 'Send the log of this crash?')
            self.assertEqual(c.itemcget(texts[1], 'text'), longest.replace(' ' + game_session.LOG_WHERE, ''))
            boxes = [c.bbox(i) for i in texts]
            where = (self.app.rows[focus].label, boxes[0], boxes[-1])
            self.assertGreaterEqual(min(b[1] for b in boxes), self.app.X(112), where)
            self.assertLessEqual(max(b[3] for b in boxes), self.app.X(app.H - 30), where)
            self.assertLessEqual(max(b[2] for b in boxes), self.app.X(app.DESC[0] + app.DESC[1]), where)

    def test_the_screen_option_shows_an_example_and_sends_nothing(self):
        self.app.open_screen('crash')
        self.root.update()
        self.assertEqual(self.app.screen, 'crash_offer')
        self.app.move(2)
        self.app.activate()
        self.assertTrue(self.app.sent_example)
        self.assertIn('"kind": "crash"', self.app.sent_text)
        self.app.back()
        self.app.move(-2)
        self.app.activate()
        self.app.tasks.wait()
        self.assertEqual(self.app.screen, 'home')
        self.assertEqual(self.server.hits, [])
        self.assertEqual(self.app.offers.shown, 0)


@unittest.skipIf(window_toolkit(), window_toolkit())
class NoServerWindowTest(WindowBase, LogBase):
    def test_a_launcher_without_a_server_shows_none_of_it(self):
        self.crash_log()
        self.offer_file().write_text(json.dumps({'kind': 'crash', 'message': MESSAGE,
                                                 'log': crash_offer.log_stamp(self.game)}), encoding='utf-8')
        self.app.show('home')
        with mock.patch.dict(os.environ, {telemetry.URL_ENV: 'none'}):
            self.assertFalse(self.app.offer_crash_log())
            self.assertIsNone(self.app.row_index(self.app.rows_settings(), 'Session Reports'))
        self.assertEqual(self.app.screen, 'home')
        self.assertTrue(self.offer_file().exists())
        self.assertEqual(self.server.hits, [])


if __name__ == '__main__':
    unittest.main()
