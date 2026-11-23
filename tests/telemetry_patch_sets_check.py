"""telemetry/src/patch_sets.ts lists the port's patch sets a session report may name (telemetry/src/validate.ts drops
any other name). It must hold exactly the names of the `set` lines of recomp/patches-v*.txt: a set added there and not
here would be dropped from every report by the server.

Usage: telemetry_patch_sets_check.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    want = set()
    for path in sorted((ROOT / 'recomp').glob('patches-v*.txt')):
        for line in path.read_text(encoding='utf-8').splitlines():
            m = re.match(r'set\s+(\S+)\s', line)
            if m:
                want.add(m.group(1))
    text = (ROOT / 'telemetry' / 'src' / 'patch_sets.ts').read_text(encoding='utf-8')
    listed = re.findall(r'"([^"]+)"', text[text.index('['):])
    bad = []
    if listed != sorted(listed):
        bad.append('the list is not sorted')
    if len(listed) != len(set(listed)):
        bad.append('a name is listed twice')
    bad += [f'missing from patch_sets.ts: {n}' for n in sorted(want - set(listed))]
    bad += [f'in patch_sets.ts and in no recomp/patches-v*.txt: {n}' for n in sorted(set(listed) - want)]
    bad += [f'not a name the server takes ([a-z0-9_], at most 32): {n}' for n in sorted(want)
            if not re.fullmatch(r'[a-z0-9_]{1,32}', n)]
    for b in bad:
        print('telemetry_patch_sets_check:', b)
    print(f'telemetry_patch_sets_check: {len(want)} patch sets, {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
