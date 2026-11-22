# needs: clang
"""Build tests/tracker_api_check.c (runtime/tracker_api.c over the real platform layer, with a guest memory of its own
at the game's addresses) and talk to it as a map tracker does: newline-delimited JSON requests, one at a time, each
answer matched by its id. No game files and no game build: the module layout and SDL's clipboard header are
stand-ins written to the test's build folder.

Checks the answers a tracker relies on (status, read_memory, search_memory, the port's own nx2saria.state and
nx2saria.game_data, over a stand-in game-data manager: every kind, keys, digests, and each way of the stand-in's to
spoil a read, refused instead of answered with made-up values), what is
refused (bad parameters, memory the game cannot read, the host's own memory beside the game's, unknown and writing
methods), what closes a connection (a first byte that is not '{', such as a web page's HTTP request or TLS handshake,
JSON that is not one object, numbers JSON does not spell that way, an overlong id, a line of 4096 bytes, a request
left unfinished), that searches never make a page of the heap resident, the searches' minute of bytes, the client
limit, and the log lines the bench reads (the start line, client lines, first answers and first refusals).

Usage: tracker_api_check.py          (TRACKER_API_CHECK_LOG=1 prints the server's log on success too)
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
LAYOUT = '''// A stand-in for the generated layout.h (tests/tracker_api_check.py).
#pragma once
#define MOD_MAIN_BASE 0x7000000000ull
#define MOD_MAIN_SIZE 0x4ea0000ull
'''
SDL_CLIPBOARD = '''// A stand-in for SDL's clipboard header (tests/tracker_api_check.py): the platform layer's clipboard calls.
#pragma once
int SDL_SetClipboardText(const char *t);
char *SDL_GetClipboardText(void);
void SDL_free(void *p);
'''
MAIN = 0x7000000000
MOD0 = MAIN + 0x3c4705c
HEAP = 0x1030000000
HOST = 0x2000000000   # the host's own memory in tests/tracker_api_check.c
PIECE = 1 << 20
SEARCH_MIB_PER_MIN = 512   # the searches' minute in this build (runtime/tracker_api.c SEARCH_BYTES_PER_MIN)

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print('FAIL:', what)


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


class Client:
    """A tracker's connection: one request in flight, answers read line by line."""

    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.buf = b''
        self.next_id = 1

    def raw(self, data):
        self.s.sendall(data)

    def line(self):
        while b'\n' not in self.buf:
            got = self.s.recv(1 << 20)
            if not got:
                return None
            self.buf += got
        line, self.buf = self.buf.split(b'\n', 1)
        return json.loads(line)

    def call(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        self.raw((json.dumps({'id': rid, 'method': method, 'params': params or {}}) + '\n').encode())
        ans = self.line()
        check(ans is not None and ans.get('id') == rid, f'{method}: the answer carries id {rid}: {ans}')
        return ans

    def closed(self, within=5):
        """1 when the server closed the connection (EOF within the timeout)."""
        try:
            self.s.settimeout(within)
            return self.s.recv(1) == b''
        except (ConnectionResetError, ConnectionAbortedError):
            return True
        except socket.timeout:
            return False

    def close(self):
        self.s.close()


def run_checks(port):
    c = Client(port)

    st = c.call('status', {'fps_window_ms': 0})['result']
    check(st['powered_on'] is True, f'status powered_on: {st}')
    check(st['program_id'] == '0400486024DEC000', f'status program_id: {st}')
    check(st['emulator'] == 'nx2saria', f'status emulator: {st}')
    check(st['build_id'].startswith('e0f3ccfaf50065ae'), f'status build_id: {st}')
    check(st['game_version'] == '1.1.1' and st['api_version'] == 1 and st['read_only'] is True, f'status: {st}')
    check(st['methods'] == ['status', 'read_memory', 'search_memory', 'nx2saria.state', 'nx2saria.game_data'],
          f'status methods: {st}')
    check(st['save_dir'] == 'C:\\Users\\Jo\u00e3o "x"\\profile\\save\\0123', f'status save_dir, escaped as JSON: {st}')

    # main by its MOD0 header, as a tracker finds it: a range below the module window first (no hit), then all regions
    pattern = '4d4f44305467030124fb0f016c342501'
    low = c.call('search_memory', {'pattern': pattern, 'start': '0x08000000', 'end': '0x200000000', 'regions': 'all',
                                   'max_hits': 4, 'budget_ms': 1000})['result']
    check(low['hits'] == [] and low['complete'] is True, f'search below the modules: {low}')
    t0 = time.perf_counter()
    hit = c.call('search_memory', {'pattern': pattern, 'regions': 'all', 'max_hits': 4, 'budget_ms': 1500})['result']
    ms = (time.perf_counter() - t0) * 1000
    check(hit['hits'] == [hex(MOD0)] and hit['complete'] is True, f'MOD0 found at main+0x3c4705c: {hit}')
    check(ms < 500, f'"all" ends after the modules when they hold the pattern, though fewer than max_hits ({ms:.0f} ms)')
    owl = c.call('search_memory', {'pattern': '4f776c2e6e7373004d4f4430', 'max_hits': 4})['result']
    check(owl['hits'] == [hex(MOD0 - 8)], f'"Owl.nss\\0MOD0" found 8 bytes before MOD0: {owl}')

    head = c.call('read_memory', {'address': hex(MAIN), 'size': 8, 'as': 'hex'})['result']
    check(head == {'hex': '010000005c70c403'}, f'main+0 reads 1 and the MOD0 offset: {head}')
    ramp = c.call('read_memory', {'address': hex(MAIN + 0x100000), 'size': 0x1000})['result']['hex']
    check(ramp == bytes(i & 255 for i in range(0x1000)).hex(), 'a 4 KiB read comes back byte for byte, lowercase hex')
    big = c.call('read_memory', {'address': hex(MAIN + 0x100000), 'size': 256 << 10})
    check('result' in big and len(big['result']['hex']) == 512 << 10, 'a 256 KiB read is answered')
    num = c.call('read_memory', {'address': MAIN, 'size': 4})
    check(num.get('result') == {'hex': '01000000'}, f'an address given as a JSON number: {num}')

    for params, why in [({'address': '0x0x7000000000', 'size': 8}, 'an address with two 0x'),
                        ({'address': ' 0x7000000000', 'size': 8}, 'an address with a blank'),
                        ({'address': hex(MAIN), 'size': 8.5}, 'a size that is not whole'),
                        ({'address': hex(MAIN), 'size': (256 << 10) + 1}, 'a read over 256 KiB'),
                        ({'address': hex(MAIN), 'size': 0}, 'a read of 0 bytes'),
                        ({'size': 8}, 'a read without an address'),
                        ({'address': 'zz', 'size': 8}, 'an address that is not hex'),
                        ({'address': hex(MAIN), 'size': 8, 'as': 'base64'}, 'an encoding other than hex')]:
        ans = c.call('read_memory', params)
        check(ans.get('error', {}).get('code') == -32602 and 'result' not in ans, f'{why} is refused: {ans}')
    for addr, why in [(0x1000, 'the low 4 GiB'), (HEAP + 0x400000, 'the hole the lazy heap never committed'),
                      (HEAP + 0x800000, 'a page nobody may read'), (MAIN + 0x4000000, "main's uncommitted tail"),
                      (MAIN + 0x4ea0000 - 4, 'a read past the end of main'), ((1 << 39) - 4, 'the top of the space'),
                      ((1 << 64) - 4, 'an address past 2^39'), (HOST + 0x100, "the host's own memory"),
                      (HEAP + 0x1000000 - 4, 'a read past the end of the heap')]:
        ans = c.call('read_memory', {'address': hex(addr), 'size': 8})
        check(ans.get('error', {}).get('code') == -32001, f'a read of {why} is refused as unreadable: {ans}')

    one = c.call('search_memory', {'pattern': b'NEEDLE-ONE'.hex(), 'regions': 'heap'})['result']
    check(one['hits'] == [hex(HEAP + 0x123450)], f'a heap search: {one}')
    both = c.call('search_memory', {'pattern': b'NEEDLE-ONE'.hex()})['result']
    check(both['hits'] == [hex(HEAP + 0x123450)], f'"all" goes on to the heap when the modules do not hold it: {both}')
    secret = c.call('search_memory', {'pattern': b'SECRET-KEY'.hex(), 'start': '0x0', 'end': hex(1 << 39)})['result']
    check(secret['hits'] == [], f"a search never reaches the host's own memory: {secret}")
    two = c.call('search_memory', {'pattern': b'NEEDLE-TWO'.hex(), 'regions': 'heap', 'max_hits': 8})['result']
    check(two['hits'] == [hex(HEAP + PIECE - 5), hex(HEAP + 0xa00000)],
          f'a pattern across two copies is found once, the hole and the dark page are stepped over: {two}')
    cap = c.call('search_memory', {'pattern': b'NEEDLE-TWO'.hex(), 'regions': 'heap', 'max_hits': 1})['result']
    check(cap['hits'] == [hex(HEAP + PIECE - 5)], f'max_hits stops the search: {cap}')
    rng = c.call('search_memory', {'pattern': b'NEEDLE-TWO'.hex(), 'start': hex(HEAP + 0x900000),
                                   'end': hex(HEAP + 0x1000000)})['result']
    check(rng['hits'] == [hex(HEAP + 0xa00000)], f'start and end clip the search: {rng}')
    for params, why in [({'pattern': 'abc'}, 'an odd number of digits'), ({'pattern': ''}, 'an empty pattern'),
                        ({'pattern': 'ab' * 65}, 'a pattern over 64 bytes'), ({'pattern': 'zz'}, 'a pattern not in hex'),
                        ({'pattern': 'ab', 'regions': 'gpu'}, 'an unknown region set'),
                        ({'pattern': 'ab', 'start': '0x2000', 'end': '0x1000'}, 'a start above the end')]:
        ans = c.call('search_memory', params)
        check(ans.get('error', {}).get('code') == -32602, f'a search with {why} is refused: {ans}')

    state = c.call('nx2saria.state')['result']
    check(state['area'] == 'HyruleField_Adult' and state['player'] == {'x': 12.5, 'y': -3.25, 'z': 100.0,
          'rotation': [0, 0, 1, 0, 1, 0, -1, 0, 0]}, f'nx2saria.state: {state}')
    check(state['game_data'] is False, f'nx2saria.state: no game data before the title screen: {state}')
    check(state['main'] == hex(MAIN) and state['heap'] == {'base': hex(HEAP), 'size': hex(0x1000000)},
          f'nx2saria.state places: {state}')

    for method in ['write_memory', 'pause', '', 'bad\nINJECTED fake log line', 'x' * 60]:
        ans = c.call(method)
        check(ans.get('error', {}).get('code') == -32601, f'"{method[:20]}" is refused as unknown: {ans}')

    # ids: a string comes back as sent, no id gets an answer without one, two requests in one write get two answers
    c.raw(b'{"id":"abc","method":"status"}\n')
    check(c.line().get('id') == 'abc', 'a string id comes back')
    c.raw(b'{"method":"status","params":{}}\r\n')
    check('id' not in c.line(), 'a request without an id is answered without one')
    c.raw(b'{"id":1.5e1,"method":"status"}\n')
    check(c.line().get('id') == 15, 'a numeric id comes back as a JSON number')
    c.raw(b'\r\n  \n{"id":16,"method":"status"}\n')
    check(c.line().get('id') == 16, 'blank lines are skipped')
    c.raw(b'{"id":7,"method":"status"}\n\n{"id":8,"method":"read_memory","params":{"address":"0x7000000000","size":1}}\n')
    a, b = c.line(), c.line()
    check(a.get('id') == 7 and b.get('id') == 8 and b['result'] == {'hex': '01'}, f'two requests in one write: {a} {b}')
    c.close()

    # four clients at once; a fifth is refused
    many = [Client(port) for _ in range(4)]
    for m in many:
        check(m.call('status')['result']['powered_on'] is True, 'each of four clients is answered')
    extra = Client(port)
    check(extra.closed(), 'a fifth client is closed at once')
    extra.close()
    for m in many:
        m.close()
    time.sleep(0.5)
    # a tracker checking whether the port is taken: connect, close, no request; three of them log one line
    for _ in range(3):
        Client(port).close()
    time.sleep(0.3)

    # what closes a connection: at once when the first byte tells
    for data, why in [(b'POST / HTTP/1.1\r\nHost: 127.0.0.1:6545\r\n\r\n', "a web page's HTTP request"),
                      (b'\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03', 'a TLS handshake (no line end)'),
                      (b'GET / HTTP/1.1', 'an HTTP request line before its end'),
                      (b'[1,2,3]\n', 'a JSON array')]:
        x = Client(port)
        x.raw(data)
        check(x.closed(within=1), f'{why} closes the connection at once')
        x.close()
    for data, why in [(b'{"id":1,"method":"status"} trailing\n', 'text after the object'),
                      (b'{} trailing\n', 'text after an empty object'),
                      (b'{"id":+1,"method":"status"}\n', 'an id with a plus sign'),
                      (b'{"id":.5,"method":"status"}\n', 'an id without a leading digit'),
                      (b'{"id":01,"method":"status"}\n', 'an id with a leading zero'),
                      (b'{"id":"' + b'i' * 70 + b'","method":"status"}\n', 'a string id too long to send back'),
                      (b'{"id":1,"method":' + b'"' + b'a' * 5000 + b'"}\n', 'a line of 4096 bytes')]:
        x = Client(port)
        x.raw(data)
        check(x.closed(), f'{why} closes the connection')
        x.close()
    x = Client(port)
    x.raw(b'{"id":1,')
    t0 = time.perf_counter()
    check(x.closed(within=8) and 4 < time.perf_counter() - t0 < 8, 'a request left unfinished closes after 5 s')
    x.close()
    x = Client(port)   # a request that keeps getting a blank now and then is still unfinished
    x.raw(b'{')
    t0, gone = time.perf_counter(), False
    while time.perf_counter() - t0 < 9 and not gone:
        time.sleep(0.8)
        try:
            x.raw(b' ')
        except OSError:
            gone = True
    check(gone or x.closed(within=1), f'a request fed a blank every 0.8 s closes after 5 s ({time.perf_counter() - t0:.1f} s)')
    x.close()

    again = Client(port)
    check(again.call('status')['result']['powered_on'] is True, 'a client is answered again once the others left')

    # the searches' minute of bytes: main's 64 MiB a search, until a search stops short
    stopped = None
    for _ in range(SEARCH_MIB_PER_MIN // 64 + 4):
        r = again.call('search_memory', {'pattern': b'ABSENT-PATTERN'.hex(), 'budget_ms': 3000})['result']
        if r['complete'] is False:
            stopped = r
            break
    check(stopped is not None and stopped['hits'] == [], f'searches stop when the minute\'s bytes are spent: {stopped}')
    again.close()


def records(res, size):
    """A game_data answer's records as (hash, the record's bytes after the hash)."""
    raw = bytes.fromhex(res['records'])
    check(len(raw) == res['count'] * size, f'{res["kind"]}: {res["count"]} records of {size} bytes: {len(raw)}')
    return [(int.from_bytes(raw[i:i + 4], 'little'), raw[i + 4:i + size]) for i in range(0, len(raw), size)]


def lines_until(srv, lines, word):
    for line in srv.stdout:
        lines.append(line)
        if line.strip() == word:
            return True
    return False


def fnv1a(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return f'{h:016x}'


def spoil(srv, lines, word, answer):
    srv.stdin.write(word + '\n')
    srv.stdin.flush()
    check(lines_until(srv, lines, answer), f'the stand-in game data: {word}')


def refused(c, params, code, words, why):
    ans = c.call('nx2saria.game_data', params)
    err = ans.get('error', {})
    check(err.get('code') == code and words in err.get('message', ''), f'{why}: {ans}')
    return ans


def run_game_data_checks(port, srv, lines):
    """nx2saria.game_data: refused before the game data is there, then each kind as a tracker reads it, then each way
    the game data can fail to read, refused instead of answered."""
    c = Client(port)
    refused(c, {'kind': 'Bool'}, -32002, 'not loaded its game data yet', 'game_data before the game makes it waits')
    spoil(srv, lines, 'EMPTY', 'EMPTIED')
    check(c.call('nx2saria.state')['result']['game_data'] is False, 'nx2saria.state: no game data before a save loads')
    for kind in ('Bool', 'Bool64bitKey'):
        refused(c, {'kind': kind}, -32002, 'not loaded its game data yet', f'{kind} before a save loads waits')
    spoil(srv, lines, 'LOAD', 'LOADED')
    check(c.call('nx2saria.state')['result']['game_data'] is True, 'nx2saria.state: the game data is there')

    b = c.call('nx2saria.game_data', {'kind': 'Bool'})['result']
    check(b['kind'] == 'Bool' and b['format'] == 1 and b['count'] == 3 and b['record_size'] == 8 and
          b['presents'] == 1234, f'Bool: {b}')
    check(b['digest'] == fnv1a(bytes.fromhex(b['records'])), f'Bool: the digest is FNV-1a of the records: {b}')
    got = [(h, int.from_bytes(v, 'little')) for h, v in records(b, 8)]
    check(got == [(0x5, 1), (0x11111111, 1), (0x22222222, 0)], f'Bool values (0 or 1), sorted by hash: {got}')
    same = c.call('nx2saria.game_data', {'kind': 'Bool', 'if_digest': b['digest']})['result']
    check(same.get('unchanged') is True and 'records' not in same and same['digest'] == b['digest'],
          f'Bool with its digest: unchanged: {same}')
    other = c.call('nx2saria.game_data', {'kind': 'Bool', 'if_digest': '0' * 16})['result']
    check(other.get('records') == b['records'], f'Bool with another digest: the records again: {other}')

    i = c.call('nx2saria.game_data', {'kind': 'Int'})['result']
    got = [(h, int.from_bytes(v, 'little', signed=True)) for h, v in records(i, 8)]
    check(got == [(0x04735978, 57), (0xfbe01da1, -1)], f'Int values: {got}')
    one = c.call('nx2saria.game_data', {'kind': 'Int', 'hash': '0x04735978'})['result']
    check([(h, int.from_bytes(v, 'little')) for h, v in records(one, 8)] == [(0x04735978, 57)], f'Int by hash: {one}')
    two = c.call('nx2saria.game_data', {'kind': 'Int', 'hash': '0xfbe01da1'})['result']
    check([(h, int.from_bytes(v, 'little', signed=True)) for h, v in records(two, 8)] == [(0xfbe01da1, -1)],
          f'Int by the hash of its second entry: {two}')
    none = c.call('nx2saria.game_data', {'kind': 'Int', 'hash': 0x12345678})['result']
    check(none['count'] == 0 and none['records'] == '' and none['digest'] == fnv1a(b''),
          f'Int by a hash the game does not have: {none}')
    none = c.call('nx2saria.game_data', {'kind': 'Int', 'hash': 0x12345678, 'if_digest': none['digest']})['result']
    check(none.get('unchanged') is True, f'an empty answer with its digest: unchanged: {none}')

    e = c.call('nx2saria.game_data', {'kind': 'Enum'})['result']
    check([(h, int.from_bytes(v, 'little')) for h, v in records(e, 8)] == [(0x33333333, 0x4c0a63f4)], f'Enum: {e}')
    t = c.call('nx2saria.game_data', {'kind': 'String64'})['result']
    got = records(t, 68)
    check(t['record_size'] == 68 and got == [(0x1d6189da, b'HyruleField_Adult'.ljust(64, b'\0'))], f'String64: {got}')
    t1 = c.call('nx2saria.game_data', {'kind': 'String64', 'hash': 0x1d6189da})['result']
    check(t1['records'] == t['records'], f'String64 by hash: {t1}')
    a = c.call('nx2saria.game_data', {'kind': 'String64Array'})['result']
    raw = bytes.fromhex(a['records'])
    texts = [raw[8 + k * 64:8 + (k + 1) * 64].split(b'\0')[0] for k in range(int.from_bytes(raw[4:8], 'little'))]
    check(a['record_size'] == 0 and a['count'] == 1 and int.from_bytes(raw[:4], 'little') == 0xc458f1eb and
          texts == [b'HyruleField', b'LakeHylia'] and len(raw) == 8 + 2 * 64, f'String64Array: {texts} {a}')
    l64 = c.call('nx2saria.game_data', {'kind': 'Int64'})['result']
    check([(h, int.from_bytes(v, 'little')) for h, v in records(l64, 12)] == [(0xd457ee3a, 0x100000002)],
          f'Int64: {l64}')
    l1 = c.call('nx2saria.game_data', {'kind': 'Int64', 'hash': 0xd457ee3a})['result']
    check(l1['records'] == l64['records'], f'Int64 by hash: {l1}')
    k = c.call('nx2saria.game_data', {'kind': 'Bool64bitKey'})['result']
    raw = bytes.fromhex(k['records'])
    keys = [int.from_bytes(raw[j:j + 8], 'little') for j in range(0, len(raw), 16)]
    check(k['count'] == 2 and keys == [0x10, 0x8000000000000001], f'Bool64bitKey, the set ones: {keys} {k}')
    k = c.call('nx2saria.game_data', {'kind': 'Bool64bitKey', 'filter': 'all'})['result']
    raw = bytes.fromhex(k['records'])
    entries = [(int.from_bytes(raw[j:j + 8], 'little'), int.from_bytes(raw[j + 8:j + 12], 'little'), raw[j + 12])
               for j in range(0, len(raw), 16)]
    check(entries == [(0x10, 0x18, 1), (0x3f93661d8e17c1d0, 0x58, 0), (0x8000000000000001, 0x118, 1),
                      (0xfedcba9876543210, 0x18, 0)], f'Bool64bitKey, all: {entries}')
    check(all(raw[j + o] == 0 for j in range(0, len(raw), 16) for o in (13, 14, 15)),
          f'Bool64bitKey: only the key, the class and the set bit: {raw.hex()}')

    for params, why in [({}, 'no kind'), ({'kind': 'Float'}, 'a kind it does not answer'),
                        ({'kind': 'bool'}, 'a kind spelled otherwise'), ({'kind': 'Int', 'hash': 0}, 'hash 0'),
                        ({'kind': 'Int', 'hash': '0x100000000'}, 'a hash over 32 bits'),
                        ({'kind': 'Int', 'hash': 'abc'}, 'a hash that does not parse'),
                        ({'kind': 'Bool64bitKey', 'hash': 5}, 'a hash for the placed objects'),
                        ({'kind': 'Bool', 'filter': 'set'}, 'a filter for Bool'),
                        ({'kind': 'Bool64bitKey', 'filter': 'some'}, 'an unknown filter'),
                        ({'kind': 'Bool', 'if_digest': 5}, 'a digest that is not a string')]:
        ans = c.call('nx2saria.game_data', params)
        check(ans.get('error', {}).get('code') == -32602, f'game_data with {why} is refused: {ans}')

    # The ways the game data can fail to read: refused, never answered with made-up values, nothing of the host read
    spoil(srv, lines, 'POINT', 'POINTED')
    for kind in ('String64', 'String64Array'):
        ans = refused(c, {'kind': kind}, -32002, 'cannot be read now', f'{kind} pointing outside the game is refused')
        check('5345435245542d4b4559' not in json.dumps(ans), f'{kind}: the host\'s memory is not read: {ans}')
    spoil(srv, lines, 'MANY', 'MANIED')
    refused(c, {'kind': 'String64Array'}, -32002, 'more than 64 texts', 'a String64Array value of 65 texts')
    spoil(srv, lines, 'DUPHASH', 'DUPHASHED')
    refused(c, {'kind': 'Int'}, -32002, 'table changed while it was read', 'an Int table with one hash twice')
    spoil(srv, lines, 'DUP', 'DUPPED')
    refused(c, {'kind': 'Int'}, -32002, 'table changed while it was read', 'an Int table with two pairs for one entry')
    spoil(srv, lines, 'UNSORT', 'UNSORTED')
    refused(c, {'kind': 'Bool64bitKey'}, -32002, 'not sorted', 'placed objects out of order')
    spoil(srv, lines, 'ZERO', 'ZEROED')
    refused(c, {'kind': 'Enum'}, -32002, 'still being made', 'an Enum entry without its vtable yet')
    spoil(srv, lines, 'BREAK', 'BROKEN')
    for _ in range(3):
        refused(c, {'kind': 'Bool'}, -32003, 'not laid out', 'a Bool entry with another vtable is not answered')
    check('result' in c.call('nx2saria.game_data', {'kind': 'Int64'}), 'the other kinds still answer')
    c.close()

def check_log(log):
    want = [
        'tracker api: on, read-only, listening on 127.0.0.1:',
        "the game's heap at 0x1030000000, above the 0x1000000000 trackers take heap pointers from",
        'tracker api: client 1 (127.0.0.1:',
        ') asks: its first request is status (1 open)',
        'tracker api: a program connected and left without a request',
        'tracker api: first status answered: powered_on 1, game 1.1.1, build e0f3ccfaf50065ae, saves in C:\\Users\\Jo',
        'tracker api: first search_memory answered: a 16-byte pattern in all regions, 0 hits',
        'tracker api: first search_memory that found its pattern: a 16-byte pattern in all regions, 1 hit, the first at 0x7003c4705c',
        f'tracker api: searches read {SEARCH_MIB_PER_MIN} MiB in the last minute',
        'tracker api: first read_memory answered: 8 bytes at 0x7000000000',
        'tracker api: read_memory refused: read_memory takes an address and a size of 1 to 262144 bytes',
        'tracker api: read_memory refused: 8 bytes at 0x1000 are not memory the game can read now',
        'tracker api: first nx2saria.state answered: area HyruleField_Adult, Link at (12.50, -3.25, 100.00)',
        'tracker api: write_memory refused: unknown method',
        'nx2saria.state, nx2saria.game_data; API version 1',
        'tracker api: nx2saria.game_data asked before the game loaded its game data (the first such answer, for Bool',
        'tracker api: first nx2saria.game_data of Bool answered: 3 values, 24 bytes, in ',
        'tracker api: first nx2saria.game_data of Int answered: 2 values, 16 bytes',
        'tracker api: first nx2saria.game_data of String64Array answered: 1 value, 136 bytes',
        'tracker api: first nx2saria.game_data of Bool64bitKey answered: 2 values, 32 bytes',
        'tracker api: nx2saria.game_data refused: nx2saria.game_data takes kind (Bool',
        "tracker api: nx2saria.game_data Bool could not be answered: this kind's entries are not laid out as version "
        '1.1.1 lays them out (error -32003; the first such answer for this kind and error)',
        'tracker api: nx2saria.game_data Enum could not be answered: this kind\'s entries are still being made',
        'tracker api: nx2saria.game_data Bool64bitKey could not be answered: the placed objects\' flags are not sorted',
        'tracker api: nx2saria.game_data String64 could not be answered: a text of this kind cannot be read now',
        'tracker api: nx2saria.game_data Int could not be answered: this kind\'s table changed while it was read',
        '49 requests (status 6, read_memory 22, search_memory 15, nx2saria.state 1, other 5), 28 refused',
        '1 request (status 1), 0 refused',
        'sent something that is not a JSON request ("POST / HTTP/1.1',
        'left a request unfinished for 5 s',
        'refused: 4 clients are connected already',
        'it sent 4096 bytes without a line end',
    ]
    for w in want:
        check(w in log, f'the log has "{w}"')
    check(log.count('first status answered') == 1, 'the first status line is logged once')
    check(log.count('nx2saria.game_data Bool could not be answered') == 1, 'a kind\'s refusals log one line')
    check('refused: Bool:' not in log and log.count('asked before the game loaded') == 1,
          'game data refusals are logged once, by their own line')
    check('first nx2saria.game_data of Bool answered: 3 values, 24 bytes' in log, 'the first Bool answer is logged')
    check(not any(line.startswith('INJECTED') for line in log.splitlines()), 'a request cannot start a log line')
    before = after = None
    for line in log.splitlines():
        if line.startswith('READY '):
            before = int(line.split()[2])
        if line.startswith('RESIDENT '):
            after = int(line.split()[1])
    check(before is not None and before == after, f'searches leave the heap\'s resident pages as they were: {before} -> {after}')
    check(log.count('left without a request') == 1, 'port checks log one line')
    check('asks: its first request' in log and 'connected from' not in log, 'a client is logged at its first request')


def main():
    work = ROOT / 'build' / 'test' / 'tracker_api'
    (work / 'SDL3').mkdir(parents=True, exist_ok=True)
    (work / 'layout.h').write_text(LAYOUT)
    (work / 'SDL3' / 'SDL_clipboard.h').write_text(SDL_CLIPBOARD)
    win = os.name == 'nt'
    exe = work / ('tracker_api_check.exe' if win else 'tracker_api_check')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    plat = [rt / 'platform' / ('win32.c' if win else 'linux.c'), rt / 'platform' / 'common.c']
    libs = ['-lbcrypt', '-lws2_32', '-lpsapi', '-lwinmm', '-ldbghelp', '-lshell32', '-lole32', '-luser32', '-ladvapi32',
            '-lpdh', '-lrstrtmgr', '-ldwmapi'] if win else ['-lpthread', '-ldl']
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-function',
                           '-Wno-sign-compare', '-Wno-missing-field-initializers', '-Wno-bitwise-instead-of-logical',
                           '-fno-strict-aliasing',
                           '-march=x86-64-v3', f'-DSEARCH_BYTES_PER_MIN=({SEARCH_MIB_PER_MIN}ull<<20)', '-DCLIENT_LOGS=100',
                           '-I', str(rt), '-I', str(work), str(ROOT / 'tests' / 'tracker_api_check.c'),
                           *map(str, plat), '-o', str(exe), '-lm', *libs], env=env)
    env['NX2SARIA_TRACKER_API'] = '1'
    env['NX2SARIA_TRACKER_API_PORT'] = str(40000 + os.getpid() % 20000)
    srv = subprocess.Popen([str(exe)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    lines = []
    port = None
    for line in srv.stdout:
        lines.append(line)
        if line.startswith('READY '):
            port = int(line.split()[1])
            break
        if line.startswith('FAIL'):
            break
    check(port is not None, 'the server listens: ' + ''.join(lines))
    if port is not None:
        try:
            run_checks(port)
            run_game_data_checks(port, srv, lines)
        finally:
            time.sleep(0.5)   # the client threads log their closing lines
            srv.stdin.close()
    else:
        srv.stdin.close()
    out = srv.stdout.read()
    srv.wait(timeout=30)
    log = ''.join(lines) + out
    if port is not None:
        check_log(log)
    if fails or os.environ.get('TRACKER_API_CHECK_LOG'):
        print(log)
    print('tracker_api_check:', 'FAILED' if fails else 'passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
