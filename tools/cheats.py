import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import flags  # noqa: E402
from layout import FIRST_BASE  # noqa: E402
from lifter import Lifter, Unsupported  # noqa: E402
from nso import Nso  # noqa: E402
from mod_files import cheat_files  # noqa: E402

DESCRIPTION = ("Cheat files to code patch sets: the code cheats of the mods' Atmosphere cheat files become "
               'patch sets of the lifted')

BASE = FIRST_BASE
NOP = 0xD503201F
RET = 0xD65F03C0


def fnv(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return f'{h:08X}'


def build_id(main):
    return Path(main).read_bytes()[0x40:0x48].hex().upper()


class Cheat:
    def __init__(self, name, master, line):
        self.name, self.master, self.line, self.words = name, master, line, []
        self.key = self.kind = self.note = ''
        self.buttons = 0
        self.sites = {}


def parse(text, mod):
    out = []
    for no, raw in enumerate(text.splitlines(), 1):
        s = raw.strip()
        if len(s) >= 2 and s[0] in '[{' and s[-1] in ']}':
            out.append(Cheat(s[1:-1].strip(), s[0] == '{', no))
        elif out:
            for w in s.split():
                if len(w) == 8:
                    try:
                        out[-1].words.append(int(w, 16))
                    except ValueError:
                        pass
    seen = {}
    for c in out:
        n = seen[c.name] = seen.get(c.name, 0) + 1
        c.key = fnv((f'{mod}/' + (c.name if n == 1 else f'{c.name}#{n}')).encode())
    return out


def ops(words):
    out, i = [], 0
    while i < len(words):
        w = words[i]
        t = w >> 28
        width = (w >> 24) & 0xF
        if t in (0, 1):
            n = 4 if width == 8 else 3
        elif t in (2, 8):
            n = 1
        elif t == 3:
            n = 2 if (w >> 24) & 0xF == 0 else 1
        elif t in (4, 6):
            n = 3
        elif t in (5, 7):
            n = 2
        elif t == 9:
            n = 1 + ((2 if width == 8 else 1) if (w >> 8) & 0xF else 0)
        elif t == 0xA:
            n = 2 if (w >> 8) & 0xF in (2, 4, 5) else 1
        else:
            t = (w >> 24) & 0xFF if t == 0xC else (w >> 20) & 0xFFF
            n = 1
        if i + n > len(words):
            raise ValueError(f'opcode {w:08X} is cut short')
        out.append((t, words[i:i + n]))
        i += n
    return out


VM_TYPES = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA}


def vm_ok(o):
    t, w = o
    if t not in VM_TYPES:
        return f'opcode {w[0]:08X} is not supported'
    if t == 5 and (w[0] >> 12) & 0xF > 1:
        return f'opcode {w[0]:08X} (load from a register offset) is not supported'
    if t == 0xA and (w[0] >> 8) & 0xF > 5:
        return f'opcode {w[0]:08X} is not supported'
    if t in (0, 1, 5, 6, 7, 9, 0xA) and (w[0] >> 24) & 0xF not in (1, 2, 4, 8):
        return f'opcode {w[0]:08X} has a bad width'
    return None


class Main:
    def __init__(self, path):
        n = Nso(str(path))
        self.image = bytes(n.image)
        self.text = n.text
        self.code_end = n.rodata[0]
        self.lifter = Lifter(BASE, 'c')

    def in_text(self, off):
        return self.text[0] <= off < self.text[0] + self.text[1] and off % 4 == 0

    def in_code(self, off):
        return self.text[0] <= off < self.code_end and off % 4 == 0

    def word(self, off):
        return struct.unpack_from('<I', self.image, off)[0]

    def lift(self, w, off):
        return self.lifter.lift(w, BASE + off)

    def effect(self, w, off):
        ins = self.lift(w, off)
        if ins.kind == 'op':
            return flags.effect(ins.code or '')
        if ins.kind == 'bcond':
            return flags.effect(ins.cond)
        return (False, None)


def code_writes(main, o):
    t, w = o
    if t != 0 or (w[0] >> 20) & 0xF != 0:
        return None
    width = (w[0] >> 24) & 0xF
    off = ((w[0] & 0xFF) << 32) | w[1]
    if not main.in_code(off - off % 4):
        return None
    if width == 4 and off % 4 == 0:
        return {off: w[2]}
    if width == 8 and off % 4 == 0:
        return {off: w[3], off + 4: w[2]}
    raise ValueError(f'a {width}-byte write into the code at main+{off:X}')


def branch(w, off):
    if w & 0x7C000000 == 0x14000000:
        return ('bl' if w >> 31 else 'b', off + ((((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) << 2))
    return None


def pc_relative(w):
    return (w & 0x1F000000 == 0x10000000 or w & 0x3B000000 == 0x18000000 or w & 0x7C000000 == 0x14000000
            or w & 0xFE000000 in (0x54000000, 0x34000000, 0x36000000, 0xB4000000, 0xB6000000)
            or w & 0xFE000000 == 0xD6000000)


def skip_store(body, i):
    w = body[i]
    if w & 0x7E000000 != 0x34000000 or (w >> 5) & 0x7FFFF != 2 or i + 1 >= len(body):
        return False
    s = body[i + 1]
    return s & 0x3B400000 == 0x39000000 and s & 0x1F == w & 0x1F


def cave_body(writes, caves, start, back, call):
    body, a = [], start
    while True:
        if a not in caves:
            raise ValueError(f'the cheat branches to main+{a:X}, which it does not write')
        w = writes[a]
        if call and w == RET:
            return body
        b = branch(w, a)
        if not call and b and b[0] == 'b' and b[1] == back:
            return body
        body.append(w)
        a += 4
        if len(body) > 64:
            raise ValueError('a code cave of more than 64 instructions')


def convert(main, writes):
    try:
        return _convert(main, writes)
    except Unsupported as e:
        raise ValueError(f'an instruction the lifter cannot take: {e}') from None


def _convert(main, writes):
    caves = {a for a in writes if main.word(a) == 0}
    sites = {}
    for a, w in sorted(writes.items()):
        if a in caves or w == main.word(a):
            continue
        if not main.in_text(a):
            raise ValueError(f'main+{a:X} is past the end of the code')
        b = branch(w, a)
        if b and b[1] in caves:
            body = cave_body(writes, caves, b[1], a + 4, b[0] == 'bl')
            out = []
            for i, x in enumerate(body):
                if skip_store(body, i):
                    continue
                if pc_relative(x):
                    raise ValueError(f'the code cave at main+{b[1]:X} has a branch or pc-relative instruction')
                out.append(x)
            repl = out or [NOP]
        else:
            repl = [w]
        orig = main.word(a)
        kind = main.lift(orig, a).kind
        if kind not in ('op', 'bl', 'b', 'bcond'):
            raise ValueError(f'main+{a:X} is a {kind}, which a patch cannot replace')
        last = main.lift(repl[-1], a).kind
        if last not in ('op', 'bl', 'b', 'bcond', 'ret'):
            raise ValueError(f'main+{a:X} gets a {last}')
        mid = next((k for k in (main.lift(x, a).kind for x in repl[:-1]) if k != 'op'), None)
        if mid:
            raise ValueError(f'main+{a:X}: its code cave holds a {mid} before its last instruction')
        want = main.effect(orig, a)[1]
        got = [e[1] for e in (main.effect(x, a) for x in repl) if e[1]]
        if (got[0] if got else None) != want or len(got) > 1:
            if repl == [NOP] and orig & 0x1F000000 == 0x11000000 and orig & 0x20000000 and \
                    orig & 0x1F == (orig >> 5) & 0x1F:
                repl = [orig & ~0x003FFC00]
            else:
                raise ValueError(f'main+{a:X}: the cheat changes the CPU flags differently from the game')
        sites[a] = repl
    return sites


def classify(main, c):
    try:
        os_ = ops(c.words)
    except ValueError as e:
        c.kind, c.note = 'unsupported', str(e)
        return
    os_ = [o for o in os_ if not (o[0] == 2 and len(os_) == 1)]
    if not os_:
        c.kind = 'heading'
        return
    try:
        cw = [code_writes(main, o) for o in os_]
    except ValueError as e:
        c.kind, c.note = 'unsupported', str(e)
        return
    if not any(x is not None for x in cw):
        bad = next((r for r in map(vm_ok, os_) if r), None)
        c.kind, c.note = ('unsupported', bad) if bad else ('data', '')
        return
    try:
        if all(x is not None for x in cw):
            writes = {k: v for x in cw for k, v in x.items()}
            if all(main.word(a) == w for a, w in writes.items()):
                c.kind = 'restore'
                return
            c.sites, c.kind = convert(main, writes), 'master' if c.master else 'code'
            return
        t = [o[0] for o in os_]
        els = next((i for i, o in enumerate(os_) if o[0] == 2 and (o[1][0] >> 24) & 0xF == 1), -1)
        if (t[0] == 8 and t[-1] == 2 and (os_[-1][1][0] >> 24) & 0xF == 0 and els > 1
                and all(x is not None for x in cw[1:els] + cw[els + 1:-1])):
            on = {k: v for x in cw[1:els] for k, v in x.items()}
            off = {k: v for x in cw[els + 1:-1] for k, v in x.items()}
            if set(off) == set(on) and all(main.word(a) == w for a, w in off.items()):
                c.sites, c.kind, c.buttons = convert(main, on), 'key', os_[0][1][0] & 0x0FFFFFFF
                return
        raise ValueError('it mixes instruction writes with other cheat opcodes')
    except ValueError as e:
        c.kind, c.note, c.sites = 'unsupported', str(e), {}


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--mods', type=Path, required=True, help='the mods folder next to nx2saria.exe')
    ap.add_argument('--patches', type=Path, required=True, help='the game version\'s patch file')
    ap.add_argument('--out', type=Path, required=True, help='the patch file with the cheats added')
    ap.add_argument('--status', type=Path, required=True)
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='replace')
    m = Main(args.exefs / 'main')
    text = args.patches.read_text()
    files = cheat_files(args.mods, build_id(args.exefs / 'main'))
    cheats = [c for mod, f in files for c in parse(f.read_text(encoding='utf-8-sig', errors='replace'), mod)]
    sets, sites, status = [], [], []
    for c in cheats:
        classify(m, c)
        if c.sites:
            name = f'cheat_{c.key.lower()}'
            label = c.name.encode('ascii', 'replace').decode()
            sets.append(f'set {name} NX2SARIA_CHEAT_{c.key} {1 if c.kind == "master" else 0}   # {label}')
            for a, repl in sorted(c.sites.items()):
                sites.append(f'site {name} main {a:x} {m.word(a):08x} {" ".join(f"{w:08x}" for w in repl)}')
        content = fnv(' '.join(f'{w:08X}' for w in c.words).encode())
        note = c.note.replace('\t', ' ')
        status.append(f'{c.key}\t{content}\t{c.kind}\t{c.buttons:X}\t{note}')
        print(f'cheats: {c.kind:11} {c.name}' + (f' ({note})' if note else ''))
    if sets:
        text += ('\n# Cheats of the mods folder (tools/cheats.py)\n' + '\n'.join(sets) + '\n' + '\n'.join(sites) + '\n')
    args.out.write_text(text)
    args.status.parent.mkdir(parents=True, exist_ok=True)
    args.status.write_text('\n'.join(status) + ('\n' if status else ''), encoding='utf-8')
    kinds = {}
    for c in cheats:
        kinds[c.kind] = kinds.get(c.kind, 0) + 1
    print(f'cheats: {len(cheats)} in {len(files)} files ({", ".join(mod for mod, _ in files) or "none"}): '
          + ', '.join(f'{n} {k}' for k, n in sorted(kinds.items())) + f'; {len(sites)} sites')


if __name__ == '__main__':
    main()
