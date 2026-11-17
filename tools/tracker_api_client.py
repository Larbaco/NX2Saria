#!/usr/bin/env python3
"""Asks a running game for what a map tracker reads through the tracker API (tracker_api = 1, runtime/tracker_api.c),
the way a tracker does, and says whether each step works on this build. Standard library only; it reads, never writes.

What it checks, in order:
1. status: the game answers, its title, build and versions.
2. nx2saria.state: the area and Link's position as the port itself reads them, and where the heap is: a tracker takes
   heap pointers below 0x1000000000 for none, so with the heap below that it finds no game data.
3. main by its MOD0 header (search_memory), first in the range a tracker tries first, then everywhere, and the header's
   two words a tracker checks.
4. Link's position by the tracker's chain (a .bss slot of main, the player object, its 37th part) and by the port's own
   chain (runtime/debug_tools.c player), which have to agree; the scene's file name from the player object.
5. The game-data manager by the tracker's slot, the same object as the port's own (debug_tools.c gmd), and the Gold
   Skulltula token count from its Int flags.
--watch N then repeats the position and scene once a second for N seconds and reports the answer times.

The offsets are facts of game version 1.1.1's main (build e0f3ccfa); docs/ocarina-atlas.md lists where each comes
from.

Usage: python tools/tracker_api_client.py [--host 127.0.0.1] [--port 6545] [--watch SECONDS]
"""
import argparse
import json
import math
import socket
import struct
import sys
import time

MOD0_PATTERN = '4d4f44305467030124fb0f016c342501'   # "MOD0" and its three offsets in 1.1.1's main
MOD0_OFFSET = 0x3c4705c
HEAP_POINTER_FLOOR = 0x1000000000
# a tracker's slots and checks in main (1.1.1)
PLAYER_SLOTS = (0x4e6c7a8, 0x4e6c938)
PLAYER_OBJECT_VTABLE = 0x4ba27c8
GAME_DATA_SLOT = 0x4e90ce8
GAME_DATA_VTABLE = 0x4c58270
SKULLTULA_TOKENS = 0x04735978   # the Int flag of the token count; its entry's tag is 0x20
# the port's own (runtime/debug_tools.c)
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


def tracker_player(api, main):
    """The tracker's chain: [main+slot]+0x10 the player object, its 37th part at +0x1c x, y, z."""
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
        return pos, scene, slot
    return None, '', None


def port_player(api, main):
    """runtime/debug_tools.c player: [[main+0x4e371d0]+0x480], named "Player", its position at +0x31c."""
    manager = api.u64(main + PORT_PLAYER_MANAGER)
    actor = api.u64(manager + 0x480) if manager else 0
    name = api.u64(actor + 0x20) if actor else 0
    if not name or text(api.read(name, 7)) != 'Player':
        return None
    return floats(api.read(actor + 0x31c, 12))


def int_flag(api, section, h):
    """An Int flag of the game data by its hash: the section's {hash, index} table probed from hash % size."""
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=6545)
    ap.add_argument('--watch', type=float, default=0, help='seconds to follow Link once a second')
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

    if a.watch > 0:
        print(f'6. following Link for {a.watch:.0f} s')
        api.times.clear()
        end = time.time() + a.watch
        while time.time() < end:
            pos, scene, _ = tracker_player(api, main)
            print(f'  {time.strftime("%H:%M:%S")}  {scene or "-"}  {pos}')
            time.sleep(1)
        t = sorted(api.times)
        if t:
            print(f'  {len(t)} reads: median {t[len(t) // 2]:.2f} ms, slowest {t[-1]:.2f} ms')

    print('all checks passed' if good else 'some checks failed')
    return 0 if good else 1


if __name__ == '__main__':
    sys.exit(main())
