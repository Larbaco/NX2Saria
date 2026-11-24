import argparse
import hashlib
import re
import sys
from pathlib import Path

DESCRIPTION = 'Check that two lifts hold the same game code, however recomp/gen.py grouped the functions into chunks.'

DEF = re.compile(r'void (f_[0-9a-f]+)\(Cpu \*c\) \{\n')
FALLBACK = re.compile(r'static void fb_([0-9a-f]+)\(Cpu \*c\);\n')
PROTO = re.compile(r'void (f_[0-9a-f]+)\(Cpu \*c\);\n')
PREAMBLE_OK = re.compile(r'(#include .*|//.*|extern uint8_t rt_patch_on\[\];|)\n')
CALL = re.compile(r'\b(f_[0-9a-f]+)\(c\)')
ADDR = re.compile(r'0x([0-9a-f]+)ull,?\n')
TOP_FILES = ('layout.json', 'layout.h', 'patches.h')


def chunk_re(mod):
    return re.compile(re.escape(mod) + r'_(\d{4,})\.c')


def split_chunk(path):
    defs, protos, problems = [], set(), []
    name = h = calls = None
    pending = None
    blank = 0

    def close(separator):
        if blank > separator:
            h.update(b'\n' * (blank - separator))
        defs.append((name, h.hexdigest(), calls))

    with open(path, 'rb') as f:
        for n, raw in enumerate(f, 1):
            line = raw.decode('utf-8', errors='surrogateescape')
            m = DEF.fullmatch(line)
            if m:
                if name:
                    close(1)
                name, h, calls, blank = m.group(1), hashlib.sha256(), set(), 0
                if pending is not None:
                    if pending[0] != name[2:]:
                        problems.append(f'{path.name}:{pending[2]}: fallback declaration fb_{pending[0]} before '
                                        f'{name}')
                    h.update(pending[1])
                    pending = None
                h.update(raw)
                continue
            fb = FALLBACK.fullmatch(line)
            if fb:
                if pending is not None:
                    problems.append(f'{path.name}:{pending[2]}: fallback declaration fb_{pending[0]} not followed '
                                    f'by f_{pending[0]}')
                pending = (fb.group(1), raw, n)
                continue
            if pending is not None:
                problems.append(f'{path.name}:{pending[2]}: fallback declaration fb_{pending[0]} not followed by '
                                f'f_{pending[0]}')
                if name:
                    h.update(pending[1])
                pending = None
            if name is None:
                p = PROTO.fullmatch(line)
                if p:
                    protos.add(p.group(1))
                elif not PREAMBLE_OK.fullmatch(line):
                    problems.append(f'{path.name}:{n}: unexpected line before the first definition: {line[:80]!r}')
                continue
            if raw == b'\n':
                blank += 1
                continue
            if blank:
                h.update(b'\n' * blank)
                blank = 0
            calls.update(CALL.findall(line))
            h.update(raw)
    if pending is not None:
        problems.append(f'{path.name}:{pending[2]}: fallback declaration fb_{pending[0]} at the end')
    if name:
        close(0)
    return defs, protos, problems


def table_addrs(path):
    addrs, inside = [], False
    with open(path, encoding='utf-8', errors='surrogateescape', newline='') as f:
        for line in f:
            if not inside:
                inside = line.startswith('const uint64_t ') and '_fn_addrs[] = {' in line
                continue
            m = ADDR.fullmatch(line)
            if not m:
                return addrs
            addrs.append(f'f_{int(m.group(1), 16):x}')
    return addrs if inside else None


def scan_module(moddir, mod):
    pat = chunk_re(mod)
    chunks = sorted((p for p in moddir.iterdir() if pat.fullmatch(p.name)), key=lambda p: int(pat.fullmatch(p.name)[1]))
    defs, where, problems = {}, {}, []
    for p in chunks:
        found, protos, bad = split_chunk(p)
        problems += bad
        defined = set()
        for name, digest, calls in found:
            where.setdefault(name, []).append(p.name)
            defs[name] = digest
            missing = calls - protos - defined
            if missing:
                problems.append(f'{p.name}: {name} calls {", ".join(sorted(missing)[:5])} with no declaration')
            defined.add(name)
    table = moddir / f'{mod}_table.c'
    addrs = table_addrs(table) if table.is_file() else None
    if addrs is None:
        problems.append(f'{table.name}: missing, or holds no {mod}_fn_addrs[]')
    else:
        listed = set(addrs)
        if len(listed) != len(addrs):
            problems.append(f'{table.name}: {len(addrs) - len(listed)} addresses listed more than once')
        for label, names in (('in the table but not defined', listed - set(defs)),
                             ('defined but not in the table', set(defs) - listed)):
            if names:
                problems.append(f'{len(names)} functions {label}: {", ".join(sorted(names)[:5])}')
    dups = {n: w for n, w in where.items() if len(w) > 1}
    return defs, dups, len(chunks), problems


def modules(lift):
    out = set()
    for p in lift.iterdir():
        if p.is_dir() and ((p / f'{p.name}_table.c').is_file()
                           or any(chunk_re(p.name).fullmatch(q.name) for q in p.iterdir())):
            out.add(p.name)
    return out


def same_file(a, b):
    if not a.is_file() or not b.is_file() or a.stat().st_size != b.stat().st_size:
        return False
    with open(a, 'rb') as fa, open(b, 'rb') as fb:
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


def compare(old, new, out=None):
    out = out or sys.stdout
    problems = 0
    for name in TOP_FILES:
        if (old / name).exists() or (new / name).exists():
            if not same_file(old / name, new / name):
                print(f'{name}: differs', file=out)
                problems += 1
    mods_old, mods_new = modules(old), modules(new)
    for mod in sorted(mods_old ^ mods_new):
        print(f'{mod}: only in {"old" if mod in mods_old else "new"}', file=out)
        problems += 1
    for mod in sorted(mods_old & mods_new):
        a, dup_a, n_a, bad_a = scan_module(old / mod, mod)
        b, dup_b, n_b, bad_b = scan_module(new / mod, mod)
        same = sum(1 for f, d in a.items() if b.get(f) == d)
        differ = sorted(f for f, d in a.items() if f in b and b[f] != d)
        missing = sorted(set(a) - set(b))
        extra = sorted(set(b) - set(a))
        table = same_file(old / mod / f'{mod}_table.c', new / mod / f'{mod}_table.c')
        print(f'{mod}: {len(a)} functions, {same} identical, {len(differ)} differing, {len(missing)} missing, '
              f'{len(extra)} extra, defined more than once {len(dup_a)} old / {len(dup_b)} new, other problems '
              f'{len(bad_a)} old / {len(bad_b)} new, table {"identical" if table else "differs"}, '
              f'chunks {n_a} -> {n_b}', file=out)
        for label, names in (('differing', differ), ('missing', missing), ('extra', extra)):
            if names:
                print(f'  {label}: {", ".join(names[:10])}{" ..." if len(names) > 10 else ""}', file=out)
        for label, dups in (('old', dup_a), ('new', dup_b)):
            for f, w in list(dups.items())[:10]:
                print(f'  {label}: {f} defined in {", ".join(w)}', file=out)
        for label, bad in (('old', bad_a), ('new', bad_b)):
            for u in bad[:10]:
                print(f'  {label}: {u}', file=out)
            if len(bad) > 10:
                print(f'  {label}: ... {len(bad) - 10} more', file=out)
        problems += len(differ) + len(missing) + len(extra) + len(dup_a) + len(dup_b) + len(bad_a) + len(bad_b)
        problems += not table
    print('same game code' if not problems else f'{problems} differences', file=out)
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('old', type=Path, help='lift folder (layout.json, <mod>/<mod>_NNNN.c, ...)')
    ap.add_argument('new', type=Path, help='lift folder to compare with it')
    args = ap.parse_args(argv)
    for p in (args.old, args.new):
        if not p.is_dir():
            ap.error(f'not a folder: {p}')
    return 1 if compare(args.old, args.new) else 0


if __name__ == '__main__':
    sys.exit(main())
