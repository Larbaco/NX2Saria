"""Build tests/tracker_api_check.c (runtime/tracker_api.c over the real platform layer, with a guest memory of its own
at the game's addresses) and talk to it as a map tracker does: newline-delimited JSON requests, one at a time, each
answer matched by its id. No game files and no game build: the module layout and SDL's clipboard header are
stand-ins written to the test's build folder.

Checks the answers a tracker relies on (status, read_memory, search_memory, the port's own nx2saria.state), what is
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
    check(state['area'] == 'HyruleField_Adult' and state['player'] == {'x': 12.5, 'y': -3.25, 'z': 100.0},
          f'nx2saria.state: {state}')
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


def check_log(log):
    want = [
        'tracker api: on, read-only, listening on 127.0.0.1:',
        "the game's heap at 0x1030000000, above the 0x1000000000 trackers take heap pointers from",
        'tracker api: client 1 (127.0.0.1:',
        ') asks: its first request is status (1 open)',
        'tracker api: a program connected and left without a request',
        'tracker api: first status answered: powered_on 1, game 1.1.1, build e0f3ccfaf50065ae',
        'tracker api: first search_memory answered: a 16-byte pattern in all regions, 0 hits',
        'tracker api: first search_memory that found its pattern: a 16-byte pattern in all regions, 1 hit, the first at 0x7003c4705c',
        f'tracker api: searches read {SEARCH_MIB_PER_MIN} MiB in the last minute',
        'tracker api: first read_memory answered: 8 bytes at 0x7000000000',
        'tracker api: read_memory refused: read_memory takes an address and a size of 1 to 262144 bytes',
        'tracker api: read_memory refused: 8 bytes at 0x1000 are not memory the game can read now',
        'tracker api: first nx2saria.state answered: area HyruleField_Adult, Link at (12.50, -3.25, 100.00)',
        'tracker api: write_memory refused: unknown method',
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
