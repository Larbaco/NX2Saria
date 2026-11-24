#!/usr/bin/env python3
import argparse
import json
import math
import socket
import struct
import sys
import time
from pathlib import Path

DESCRIPTION = ('Asks a running game for what a map tracker reads through the tracker API (tracker_api = 1, '
               'runtime/diag/tracker_api.c),')
MOD0_PATTERN = '4d4f44305467030124fb0f016c342501'
MOD0_OFFSET = 0x3c4705c
HEAP_POINTER_FLOOR = 0x1000000000
PLAYER_SLOTS = (0x4e6c7a8, 0x4e6c938)
PLAYER_OBJECT_VTABLE = 0x4ba27c8
GAME_DATA_SLOT = 0x4e90ce8
GAME_DATA_VTABLE = 0x4c58270
SKULLTULA_TOKENS = 0x04735978
PORT_PLAYER_MANAGER = 0x4e371d0
PORT_GAME_DATA_SLOT = 0x4c7e180


class Api:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=5)
        self.buf = b''
        self.id = 0
        self.times = []

    def call(self, method, timeout=5.0, **params):
        self.id += 1
        t0 = time.perf_counter()
        self.s.settimeout(timeout)
        self.s.sendall((json.dumps({'id': self.id, 'method': method, 'params': params}) + '\n').encode())
        while b'\n' not in self.buf:
            got = self.s.recv(1 << 20)
            if not got:
                raise ConnectionError('the game closed the connection')
            self.buf += got
        line, self.buf = self.buf.split(b'\n', 1)
        self.times.append((time.perf_counter() - t0) * 1000)
        ans = json.loads(line)
        if ans.get('id') != self.id:
            raise ValueError(f'answer {ans.get("id")} to request {self.id}')
        return ans.get('result'), ans.get('error')

    def read(self, addr, n):
        res, _ = self.call('read_memory', address=hex(addr), size=n, **{'as': 'hex'})
        return bytes.fromhex(res['hex']) if res else None

    def u64(self, addr):
        b = self.read(addr, 8) if addr else None
        return struct.unpack('<Q', b)[0] if b else 0

    def u32(self, addr):
        b = self.read(addr, 4) if addr else None
        return struct.unpack('<I', b)[0] if b else None


def say(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what)
    return ok


def text(b):
    return b.split(b'\0', 1)[0].decode('ascii', 'replace') if b else ''


def floats(b):
    return struct.unpack('<3f', b) if b and len(b) >= 12 else None


def tracker_player(api, main, direction=False):
    for slot in PLAYER_SLOTS:
        single = api.u64(main + slot)
        obj = api.u64(single + 0x10) if single >= HEAP_POINTER_FLOOR else 0
        if obj < HEAP_POINTER_FLOOR or api.u64(obj) != main + PLAYER_OBJECT_VTABLE:
            continue
        count, parts = api.u32(obj + 0x50), api.u64(obj + 0x58)
        if count is None or not 37 <= count <= 128 or parts < HEAP_POINTER_FLOOR:
            continue
        sub36, sub7 = api.u64(parts + 36 * 8), api.u64(parts + 7 * 8)
        pos = floats(api.read(sub36 + 0x1c, 12)) if sub36 else None
        scene = text(api.read(sub7 + 0x34, 0x80)) if sub7 else ''
        if direction:
            d = api.read(sub36 + 0x28, 12) if sub36 else None
            return pos, scene, slot, (struct.unpack('<3f', d)[0::2] if d else None)
        return pos, scene, slot
    return (None, '', None, None) if direction else (None, '', None)


def port_player(api, main):
    manager = api.u64(main + PORT_PLAYER_MANAGER)
    actor = api.u64(manager + 0x480) if manager else 0
    name = api.u64(actor + 0x20) if actor else 0
    if not name or text(api.read(name, 7)) != 'Player':
        return None
    return floats(api.read(actor + 0x31c, 12))


def int_flag(api, section, h):
    count, entries, table, size = api.u32(section + 0x10), api.u64(section + 0x18), api.u64(section + 0x20), \
        api.u32(section + 0x2c)
    if not count or not entries or not table or not size or size > 100000:
        return None
    i = h % size
    for _ in range(size):
        pair = api.read(table + i * 8, 8)
        if not pair:
            return None
        ph, idx = struct.unpack('<II', pair)
        if ph == 0:
            return None
        if ph == h:
            if idx >= count:
                return None
            e = entries + idx * 0x28
            tag, value = api.u32(e + 8), api.read(e + 0x1c, 4)
            return (tag, struct.unpack('<i', value)[0]) if value else None
        i = i + 1 if i + 1 < size else 0
    return None


GAME_DATA_KINDS = {'Bool': 8, 'Int': 8, 'Enum': 8, 'String64': 68, 'String64Array': 0, 'Int64': 12, 'Bool64bitKey': 16}
SAVE_KINDS = {0: 'Bool', 2: 'Int', 6: 'Enum'}


def save_values(folder):
    p = Path(folder)
    files = [p] if p.is_file() else sorted(p.rglob('progress.sav'), key=lambda f: f.stat().st_mtime)
    if not files:
        return None, None, None
    b = files[-1].read_bytes()
    if len(b) < 0x28 or struct.unpack_from('<I', b, 0)[0] != 0x01020304:
        return None, None, files[-1]
    start, kind, out, keys = struct.unpack_from('<I', b, 8)[0], 0, {k: {} for k in SAVE_KINDS.values()}, set()
    for o in range(0x28, min(start, len(b)) - 7, 8):
        h, v = struct.unpack_from('<II', b, o)
        if h == 0:
            kind = v
        elif kind in SAVE_KINDS:
            out[SAVE_KINDS[kind]][h] = struct.unpack('<i', struct.pack('<I', v))[0] if kind == 2 else v
        elif kind == 32 and v:
            keys = {k for k in struct.unpack_from(f'<{(len(b) - v) // 8}Q', b, v) if k}
    return out, keys, files[-1]


def live_values(res):
    raw = bytes.fromhex(res.get('records', ''))
    fmt = '<i' if res['kind'] == 'Int' else '<I'
    return {struct.unpack_from('<I', raw, o)[0]: struct.unpack_from(fmt, raw, o + 4)[0] for o in range(0, len(raw), 8)}


MANAGER_VTABLES = (0x4c1f120, 0x4c1f140)


def check_game_data(api, save, main):
    good = True
    answers = {}
    mgr = api.u64(api.u64(main + PORT_GAME_DATA_SLOT))
    words = (api.u64(mgr), api.u64(mgr + 8)) if mgr else (0, 0)
    shown = ', '.join(f'main+{w - main:#x}' if main <= w < main + 0x10000000 else f'{w:#x}' for w in words)
    say(words == tuple(main + v for v in MANAGER_VTABLES), f'the manager at {mgr:#x} starts with {shown} '
        '(main+0x4c1f120, main+0x4c1f140 expected; to pin once a run shows it)')
    for kind, size in GAME_DATA_KINDS.items():
        t0 = time.perf_counter()
        res, err = api.call('nx2saria.game_data', kind=kind)
        ms = (time.perf_counter() - t0) * 1000
        if not res:
            good &= say(False, f'{kind}: {err}')
            continue
        answers[kind] = res
        n = len(res.get('records', '')) // 2
        good &= say(res['record_size'] == size and (not size or n == res['count'] * size),
                    f'{kind}: {res["count"]} values, {n} bytes, {ms:.1f} ms')
    if 'Bool' in answers:
        res, _ = api.call('nx2saria.game_data', kind='Bool', if_digest=answers['Bool']['digest'])
        say(bool(res) and res.get('unchanged') is True, 'Bool asked again with its digest: unchanged '
            '(not when a flag changed in between)')
    if 'String64' in answers:
        raw = bytes.fromhex(answers['String64']['records'])
        stage = {struct.unpack_from('<I', raw, o)[0]: text(raw[o + 4:o + 68]) for o in range(0, len(raw), 68)}
        good &= say(0x1d6189da in stage, f'Sequence_CurrentBanc (0x1d6189da): {stage.get(0x1d6189da)!r}')
    if 'String64Array' in answers:
        raw = bytes.fromhex(answers['String64Array']['records'])
        o, lists = 0, {}
        while o + 8 <= len(raw):
            h, n = struct.unpack_from('<II', raw, o)
            lists[h] = [text(raw[o + 8 + i * 64:o + 8 + (i + 1) * 64]) for i in range(n)]
            o += 8 + n * 64
        say(0xc458f1eb in lists, f'the place within the area (0xc458f1eb): {lists.get(0xc458f1eb)}')
    if not save:
        say(True, 'no --save: the values are not compared with a save')
        return good
    saved, keys, path = save_values(save)
    if saved is None:
        return say(False, f'no readable progress.sav under {save} ({path})') and good
    print(f'  compared with {path} (written {time.strftime("%H:%M:%S", time.localtime(path.stat().st_mtime))})')
    for kind in SAVE_KINDS.values():
        if kind not in answers:
            continue
        live = live_values(answers[kind])
        common = [h for h in saved[kind] if h in live]
        differ = [h for h in common if live[h] != saved[kind][h]]
        missing = len(saved[kind]) - len(common)
        ok = missing == 0 and (kind == 'Int' or len(differ) <= len(common) // 100)
        good &= say(ok, f'{kind}: {len(saved[kind])} in the save, {missing} not answered, {len(differ)} differ'
                    + (': ' + ', '.join(f'{h:#010x} save {saved[kind][h]} live {live[h]}' for h in differ[:5])
                       if differ else ''))
    if 'Bool64bitKey' in answers:
        raw = bytes.fromhex(answers['Bool64bitKey']['records'])
        live_keys = {struct.unpack_from('<Q', raw, o)[0] for o in range(0, len(raw), 16)}
        lost = keys - live_keys
        good &= say(not lost, f'placed objects: {len(keys)} set in the save, {len(live_keys)} live, '
                    f'{len(lost)} of the save\'s not set live')
    return good


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=6545)
    ap.add_argument('--watch', type=float, default=0, help='seconds to follow Link once a second')
    ap.add_argument('--save', help='a folder holding the game\'s saves (or a progress.sav) to compare the game data with')
    a = ap.parse_args()
    try:
        api = Api(a.host, a.port)
    except OSError as e:
        print(f'no tracker API on {a.host}:{a.port} ({e}): is the game running with tracker_api = 1?')
        return 2
    good = True

    print('1. status')
    st, err = api.call('status', fps_window_ms=0)
    good &= say(bool(st) and st.get('powered_on'), f'the game runs: {st or err}')

    print('2. nx2saria.state')
    state, err = api.call('nx2saria.state')
    if not say(bool(state), f'answered: {state or err}'):
        return 1
    heap = int(state['heap']['base'], 16)
    good &= say(heap >= HEAP_POINTER_FLOOR, f'the heap starts at {heap:#x}, '
                f'{"above" if heap >= HEAP_POINTER_FLOOR else "below"} {HEAP_POINTER_FLOOR:#x}')
    say(state.get('player') is not None, f'area {state.get("area")}, Link at {state.get("player")} '
        '(no player before a save is loaded)')

    print('3. main by its MOD0 header')
    low, _ = api.call('search_memory', timeout=3, pattern=MOD0_PATTERN, start='0x08000000', end='0x200000000',
                      regions='all', max_hits=4, budget_ms=1000)
    t0 = time.perf_counter()
    found, _ = api.call('search_memory', timeout=4, pattern=MOD0_PATTERN, regions='all', max_hits=4, budget_ms=1500)
    ms = (time.perf_counter() - t0) * 1000
    hits = [int(h, 16) for h in (found or {}).get('hits', [])]
    main = hits[0] - MOD0_OFFSET if hits else 0
    say(low is not None, f'the search below 8 GiB answers ({len((low or {}).get("hits", []))} hits; main is above it)')
    good &= say(main == int(state['main'], 16), f'main at {main:#x} ({len(hits)} hit(s), {ms:.0f} ms)')
    if not main:
        return 1
    good &= say(api.u32(main) == 1 and api.u32(main + 4) == MOD0_OFFSET, "main's first words are 1 and the MOD0 offset")

    print("4. Link by the tracker's chain and by the port's")
    pos, scene, slot = tracker_player(api, main)
    mine = port_player(api, main)
    if not pos and not mine:
        say(True, 'no player yet: load a save and run again')
    else:
        good &= say(pos is not None, f'tracker chain (slot main+{slot or 0:#x}): {pos}')
        good &= say(mine is not None, f'port chain: {mine}')
        if pos and mine:
            d = math.dist(pos, mine)
            good &= say(d < 0.5, f'the two agree within {d:.3f} m')
        say(bool(scene), f'scene file: {scene or "none"}')

    print('5. the game data')
    gdm = api.u64(main + GAME_DATA_SLOT)
    if not gdm:
        say(True, 'no game data yet: load a save and run again')
    else:
        good &= say(gdm >= HEAP_POINTER_FLOOR and api.u64(gdm) == main + GAME_DATA_VTABLE,
                    f'the tracker slot gives {gdm:#x}, its vtable checks')
        mine = api.u64(api.u64(main + PORT_GAME_DATA_SLOT))
        good &= say(mine == gdm + 0x168, f"the port's manager is {mine:#x} (the tracker's + 0x168: {gdm + 0x168:#x})")
        tokens = int_flag(api, gdm + 0x168 + 2 * 0x60, SKULLTULA_TOKENS)
        good &= say(tokens is not None and tokens[0] == 0x20 and 0 <= tokens[1] <= 100,
                    f'Gold Skulltula tokens: {tokens[1]} (tag {tokens[0]:#x})'
                    if tokens else 'Gold Skulltula tokens: not found')

    print('6. the game data by hash (nx2saria.game_data)')
    if not state.get('game_data') and not api.call('nx2saria.state')[0].get('game_data'):
        say(True, 'no game data yet: load a save and run again')
    else:
        good &= check_game_data(api, a.save, main)

    if a.watch > 0:
        print(f'7. following Link for {a.watch:.0f} s (heading: from his rotation | the way he moves)')
        api.times.clear()
        end = time.time() + a.watch
        while time.time() < end:
            pos, scene, _, d = tracker_player(api, main, direction=True)
            st, _ = api.call('nx2saria.state')
            m = ((st or {}).get('player') or {}).get('rotation')
            rot = f'{(math.degrees(math.atan2(m[2], -m[8])) + 360) % 360:5.1f}' if m and math.hypot(m[2], m[8]) > 0.01 else '  -  '
            move = f'{(math.degrees(math.atan2(d[0], -d[1])) + 360) % 360:5.1f}' if d and math.hypot(*d) > 0.01 else '  -  '
            print(f'  {time.strftime("%H:%M:%S")}  {scene or "-"}  {pos}  heading {rot} | {move}')
            time.sleep(1)
        t = sorted(api.times)
        if t:
            print(f'  {len(t)} reads: median {t[len(t) // 2]:.2f} ms, slowest {t[-1]:.2f} ms')

    print('all checks passed' if good else 'some checks failed')
    return 0 if good else 1


if __name__ == '__main__':
    sys.exit(main())
