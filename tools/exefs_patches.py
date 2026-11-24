import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cheats import Main, convert, fnv  # noqa: E402
from mod_files import patch_files  # noqa: E402

DESCRIPTION = ('ExeFS patches of the mods folder to code patch sets: IPS, IPS32 and IPSwitch (.pchtxt) files '
               "for the game's main.")

NSO_HEADER = 0x100


def same_build(name, build_id):
    name = name.lower()
    return bool(re.fullmatch(r'[0-9a-f]{1,64}', name)) and name.ljust(64, '0') == build_id.ljust(64, '0')


def parse_ips(data):
    if data[:5] == b'PATCH':
        width, end, at = 3, b'EOF', 5
    elif data[:5] == b'IPS32':
        width, end, at = 4, b'EEOF', 5
    else:
        raise ValueError('not an IPS or IPS32 file')
    writes = {}
    while True:
        if data[at:at + len(end)] == end and at + len(end) >= len(data) - 3:
            return writes
        if at + width + 2 > len(data):
            raise ValueError('the IPS file is cut short')
        off = int.from_bytes(data[at:at + width], 'big')
        size = int.from_bytes(data[at + width:at + width + 2], 'big')
        at += width + 2
        if size:
            body = data[at:at + size]
            at += size
        else:
            run = int.from_bytes(data[at:at + 2], 'big')
            body = data[at + 2:at + 3] * run
            at += 3
        if len(body) != (size or len(body)):
            raise ValueError('the IPS file is cut short')
        for i, b in enumerate(body):
            writes[off - NSO_HEADER + i] = b


def pchtxt_value(token):
    if token.startswith('"'):
        if not token.endswith('"') or len(token) < 2:
            raise ValueError(f'a string without its closing quote: {token}')
        return token[1:-1].encode('utf-8').decode('unicode_escape').encode('latin-1')
    if len(token) % 2 or not re.fullmatch(r'[0-9A-Fa-f]+', token):
        raise ValueError(f'not a hex value: {token}')
    return bytes.fromhex(token)


def parse_pchtxt(text):
    bid, shift, enabled, writes = None, 0, False, {}
    for raw in text.splitlines():
        line = raw.split('//', 1)[0].strip()
        if not line:
            continue
        if line.startswith('@stop'):
            break
        if line.startswith('@nsobid-'):
            bid = line[len('@nsobid-'):].strip()
        elif line.startswith('@enabled'):
            enabled = True
        elif line.startswith('@disabled'):
            enabled = False
        elif line.startswith('@flag'):
            f = line.split()
            if len(f) >= 3 and f[1] == 'offset_shift':
                shift = int(f[2], 0)
        elif line.startswith('@') or not enabled:
            continue
        else:
            m = re.fullmatch(r'([0-9A-Fa-f]+)\s+(".*"|\S+)', line)
            if not m:
                continue
            off = int(m.group(1), 16) + shift - NSO_HEADER
            for i, b in enumerate(pchtxt_value(m.group(2))):
                writes[off + i] = b
    return bid, writes


def word_writes(main, writes):
    words = {}
    for off in sorted(writes):
        if not main.in_code(off - off % 4):
            raise ValueError(f'it writes main\'s data at main+{off:X} (only code patches are supported)')
        a = off - off % 4
        w = words.get(a, main.word(a))
        shift = (off % 4) * 8
        words[a] = (w & ~(0xFF << shift) | writes[off] << shift) & 0xFFFFFFFF
    return words


def builtin_sites(text):
    out = {}
    for line in text.splitlines():
        f = line.split('#', 1)[0].split()
        if len(f) >= 4 and f[0] == 'site' and f[2] == 'main' and not f[1].startswith(('cheat_', 'mod_')):
            out.setdefault(int(f[3], 16), []).append(f[1])
    return out


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--mods', type=Path, required=True, help='the mods folder next to nx2saria.exe')
    ap.add_argument('--patches', type=Path, required=True, help='the patch file so far')
    ap.add_argument('--out', type=Path, required=True, help='the patch file with the mods\' ExeFS patches added')
    ap.add_argument('--status', type=Path, required=True)
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='replace')
    m = Main(args.exefs / 'main')
    raw = (args.exefs / 'main').read_bytes()
    bid = raw[0x40:0x60].hex()
    text = args.patches.read_text()
    builtin = builtin_sites(text)
    files = patch_files(args.mods)
    sets, sites, status, kinds = [], [], [], {}
    for mod, rel, f in files:
        data = f.read_bytes()
        key, content = fnv(f'{mod}/{rel}'.encode()), fnv(data)
        kind, note, over, file_sites = 'code', '', [], {}
        try:
            if f.suffix.lower() == '.ips':
                for_main = same_build(f.stem, bid)
                writes = parse_ips(data) if for_main else {}
            else:
                pid, writes = parse_pchtxt(data.decode('utf-8-sig', errors='replace'))
                for_main = pid is not None and same_build(pid, bid)
            if not for_main:
                kind = 'other'
            else:
                file_sites = convert(m, word_writes(m, writes))
                if not file_sites:
                    kind = 'empty'
        except ValueError as e:
            kind, note, file_sites = 'unsupported', str(e), {}
        if file_sites:
            name = f'mod_{key.lower()}'
            label = f'{mod}/{rel}'.encode('ascii', 'replace').decode()
            sets.append(f'set {name} NX2SARIA_MODPATCH_{key} 0   # {label}')
            for a, repl in sorted(file_sites.items()):
                sites.append(f'site {name} main {a:x} {m.word(a):08x} {" ".join(f"{w:08x}" for w in repl)}')
                over += [s for s in builtin.get(a, []) if s not in over]
        kinds[kind] = kinds.get(kind, 0) + 1
        status.append(f'{key}\t{content}\t{kind}\t{",".join(over) or "-"}\t{note.replace(chr(9), " ")}')
        print(f'exefs: {kind:11} {mod}/{rel}' + (f' ({note})' if note else '')
              + (f' ({len(file_sites)} sites, over {", ".join(over)})' if over else
                 f' ({len(file_sites)} sites)' if file_sites else ''))
    if sets:
        text += ('\n# ExeFS patches of the mods folder (tools/exefs_patches.py)\n' + '\n'.join(sets) + '\n'
                 + '\n'.join(sites) + '\n')
    args.out.write_text(text)
    args.status.parent.mkdir(parents=True, exist_ok=True)
    args.status.write_text('\n'.join(status) + ('\n' if status else ''), encoding='utf-8')
    print(f'exefs: {len(files)} patch files: ' + ', '.join(f'{n} {k}' for k, n in sorted(kinds.items()))
          + f'; {len(sites)} sites')


if __name__ == '__main__':
    main()
