import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
while not (ROOT / 'tools' / 'shader_census.py').is_file() and ROOT != ROOT.parent:
    ROOT = ROOT.parent
sys.path.insert(0, str(ROOT / 'tools'))
import pipeline_list  # noqa: E402
import shader_list  # noqa: E402
from shader_census import inventory  # noqa: E402

DESCRIPTION = 'Prepare the default variant of every game shader and the variants of the shared shader list for a'
STAGES = {'vs': 0, 'ps': 4, 'cs': 5, 'ts': 6, 'ms': 7}
TEX = [None, '1d', '1darray', 'buffer', '2d', '2darray', '2dms', '2dmsarray', '3d', 'cube', 'cubearray']
FMT = [None, 'R8Unorm', 'R8Snorm', 'R8Uint', 'R8Sint', 'R16Float', 'R16Unorm', 'R16Snorm', 'R16Uint', 'R16Sint',
       'R32Float', 'R32Uint', 'R32Sint', 'R8G8Unorm', 'R8G8Snorm', 'R8G8Uint', 'R8G8Sint', 'R16G16Float', 'R16G16Unorm',
       'R16G16Snorm', 'R16G16Uint', 'R16G16Sint', 'R32G32Float', 'R32G32Uint', 'R32G32Sint', 'R8G8B8A8Unorm',
       'R8G8B8A8Snorm', 'R8G8B8A8Uint', 'R8G8B8A8Sint', 'R16G16B16A16Float', 'R16G16B16A16Unorm', 'R16G16B16A16Snorm',
       'R16G16B16A16Uint', 'R16G16B16A16Sint', 'R32G32B32A32Float', 'R32G32B32A32Uint', 'R32G32B32A32Sint',
       'R10G10B10A2Unorm', 'R10G10B10A2Uint', 'R11G11B10Float']
TYPES = {'float': 0, 'sint': 1, 'uint': 2}
DEFAULT = {'tex': {str(slot): '2d' for slot in range(32)}}
CONTROL = 0x880
ANS_REC = 4 + 32 + 32 + 147 + 1
DEFAULT_FLAG = 0x80
SLOW_UNUSED = (('/agl_ndsr.pck', 16 << 10), ('/agl_ndsr_vr.pck', 16 << 10))


def slow_unused(s):
    return any(s['source'].endswith(suffix) and len(s['code']) > limit for suffix, limit in SLOW_UNUSED)
JOURNAL = 'shader-prepare-journal.bin'
RETRIES = 3


BUNDLED_LIST = ROOT / 'tools' / 'data' / 'shader_list.tsv'


BUNDLED_PIPELINES = ROOT / 'tools' / 'data' / 'pipeline_list.bin'
if not BUNDLED_PIPELINES.is_file():
    BUNDLED_PIPELINES = ROOT / 'data' / 'pipeline_list.bin'
SEED_KEYS = 'seed-pipeline-keys.bin'
LIST_PRIORITY = 1


def read_pipeline_list(path):
    p = Path(path) if path else BUNDLED_PIPELINES
    if not p.is_file():
        if path:
            sys.exit(f'prepare_shaders: no pipeline list at {p}')
        return None
    got = pipeline_list.unpack(p.read_bytes())
    if got is None:
        sys.exit(f'prepare_shaders: {p} is not a pipeline list (tools/pipeline_list.py merge writes one)')
    return got


def list_variants(answers_recs, have, seen):
    out, absent = [], 0
    for a in answers_recs:
        h, packed = a[4:36], a[68:68 + 147]
        if h not in have:
            absent += 1
            continue
        if (h, packed) in seen:
            continue
        seen.add((h, packed))
        out.append(a[:36] + bytes(32) + packed + bytes([LIST_PRIORITY]))
    return out, absent


def read_list(path):
    if path:
        return Path(path).read_text(encoding='utf-8')
    if BUNDLED_LIST.is_file():
        return BUNDLED_LIST.read_text(encoding='utf-8')
    return shader_list.history(ROOT / 'refs' / 'MacNativeGames')


def pipeline_variants(key):
    if ':' in key:
        return key.split(':', 2)[:2]
    return [n.decode('ascii', 'replace') for n in bytes.fromhex(key).split(b'\0')[:2]]


def parse_list(text):
    variants, prio = [], {}

    def warm(key, flags):
        p = 0 if 't' in flags else 1
        prio[key] = min(prio.get(key, 2), p)

    for line in text.splitlines():
        f = line.split('\t')
        if f[0] == 'V' and len(f) == 3:
            variants.append((f[1], json.loads(f[2])))
        elif f[0] == 'S' and len(f) == 4:
            warm(f[1][:64], f[3])
        elif f[0] == 'C' and len(f) == 3:
            warm(f[1][:64], f[2])
        elif f[0] in ('R', 'B') and len(f) >= 3:
            for n in pipeline_variants(f[1]):
                if len(n) >= 64:
                    warm(n[:64], f[-1])
    return variants, prio


def answers(deps, stage, control):
    tex, img, fmt, attr, fout = bytearray(32), bytearray(32), bytearray(32), bytearray(32), bytearray(8)
    for slot, t in deps.get('tex', {}).items():
        t = t.removesuffix('_depth')
        tex[int(slot)] = TEX.index('2d' if t == '*' else t)
    for slot, o in deps.get('img', {}).items():
        img[int(slot)] = TEX.index(o['type'])
        fmt[int(slot)] = FMT.index(o['fmt']) if o.get('fmt') in FMT else 0
    for loc, t in deps.get('attr', {}).items():
        attr[int(loc)] = TYPES[t]
    for idx, t in deps.get('fout', {}).items():
        fout[int(idx)] = TYPES[t]
    local = [0, 0, 0]
    if stage not in (STAGES['vs'], STAGES['ps']):
        local = struct.unpack_from('<III', control, 0x58 if nvn2(control) else 0x748)
        local = [(v or 1) & 0xffff for v in local]
    packed = bytes(tex + img + fmt + attr + fout) + struct.pack('<HHHIB', *local, 0, 0)
    assert len(packed) == 147
    return packed


def nvn2(control):
    return control[:4] == bytes.fromhex('34127698') and not any(control[0xe0:0x880])


def bounds(control, code):
    if nvn2(control):
        size, c1_start, c1_end = struct.unpack_from('<III', control, 0x38)[0], *struct.unpack_from('<II', control, 0x40)
        code_size = c1_end if c1_start >= size and c1_end > c1_start and c1_end - size < 1 << 20 else size
        if c1_start >= size and c1_end >= c1_start and c1_end <= min(code_size, len(code)):
            return c1_start, c1_end
        return min(size, len(code)), min(size, len(code))
    return struct.unpack_from('<II', control, 0x700)


def code_hash(stage, control, code, length, c1):
    return hashlib.sha256(struct.pack('<I', stage) + control + struct.pack('<Q', length) + code[:length] +
                          struct.pack('<I', len(c1)) + c1).digest()


def driver_crashed(out):
    p = out / JOURNAL
    data = p.read_bytes() if p.is_file() else b''
    return any(any(data[i:i + 32]) for i in range(0, len(data), 64))


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    ap.add_argument('build', type=Path, help='build directory holding nx2saria.exe (build\\v121)')
    ap.add_argument('--version', default='v121', help='build/extracted/<version>')
    ap.add_argument('--romfs', type=Path, help='RomFS directory (default build/extracted/<version>/romfs)')
    ap.add_argument('--exefs', type=Path, help='ExeFS directory (default build/extracted/<version>/exefs)')
    ap.add_argument('--list', help='a shader_list.tsv instead of tools/data/shader_list.tsv or refs/MacNativeGames')
    ap.add_argument('--no-run', action='store_true', help='write the seed files only')
    ap.add_argument('--no-list', action='store_true', help='default variants only, without a shared shader list')
    ap.add_argument('--pipeline-list', help='a pipeline list instead of tools/data/pipeline_list.bin (tools/pipeline_list.py)')
    ap.add_argument('--no-pipeline-list', action='store_true', help='no shared pipeline list, also when one is bundled')
    ap.add_argument('--all-shaders', action='store_true',
                    help='also the default variants SLOW_UNUSED leaves out (an hour more of GPU driver builds)')
    ap.add_argument('--exe', default='nx2saria.exe' if os.name == 'nt' else 'nx2saria',
                    help='the game executable in the build directory that runs --prepare-shaders')
    a = ap.parse_args()
    t0 = time.monotonic()
    variants, prio = parse_list('' if a.no_list else read_list(a.list))
    extracted = ROOT / 'build' / 'extracted' / a.version
    listed = len(variants)
    shaders, _ = inventory(a.romfs or extracted / 'romfs', a.exefs or extracted / 'exefs', everything=False)
    if len(shaders) < max(64, listed // 20):
        print(f'only {len(shaders)} shaders found of the {listed} listed variants: reading every RomFS file instead',
              flush=True)
        shaders, _ = inventory(a.romfs or extracted / 'romfs', a.exefs or extracted / 'exefs')
    t1 = time.monotonic()
    progs, recs, seen, skipped = {}, [], set(), {'not in this version': 0, 'stage': 0}
    left_out = 0 if a.all_shaders else sum(1 for s in shaders.values() if slow_unused(s))
    variants += [(key, DEFAULT) for key in sorted(shaders) if a.all_shaders or not slow_unused(shaders[key])]
    for key, deps in variants:
        s = shaders.get(key)
        if not s:
            skipped['not in this version'] += 1
            continue
        stage = STAGES.get(s['stage'])
        if stage is None:
            skipped['stage'] += 1
            continue
        control, code = s['control'][:CONTROL], s['code']
        c1_off, length = bounds(control, code)
        c1 = code[c1_off:length]
        if key not in progs:
            h = code_hash(stage, control, code, length, c1)
            progs[key] = (h, struct.pack('<III', stage, length, len(c1)) + h + control + code[:length] + c1)
        h = progs[key][0]
        default = deps is DEFAULT
        packed = answers({} if default and stage == STAGES['cs'] else deps, stage, control)
        if (h, packed) in seen:
            continue
        seen.add((h, packed))
        rec = struct.pack('<I', stage) + h + bytes(32) + packed + bytes([prio.get(key, 2) | (DEFAULT_FLAG if default else 0)])
        assert len(rec) == ANS_REC
        recs.append((prio.get(key, 2), rec))
    out = a.build.resolve()
    plist = None if a.no_pipeline_list else read_pipeline_list(a.pipeline_list)
    (out / SEED_KEYS).unlink(missing_ok=True)
    if plist:
        extra, absent = list_variants(plist[0], {h for h, _ in progs.values()}, seen)
        recs += [(LIST_PRIORITY, r) for r in extra]
        (out / SEED_KEYS).write_bytes(plist[1])
        n_keys = len(pipeline_list.read_keys(plist[1])[3])
        print(f'pipeline list: {n_keys} pipelines; of its {len(plist[0])} shader variants (the ones they name and the '
              f'compute variants), {len(extra)} join the seed, {absent} are of shaders {a.version} does not have, the '
              'others are in it already', flush=True)
    recs.sort(key=lambda r: r[0])
    with open(out / 'seed-programs.bin', 'wb') as f:
        f.write(b'NX2NSPRG' + struct.pack('<II', 1, 0))
        for _, body in progs.values():
            f.write(body)
    with open(out / 'seed-answers.bin', 'wb') as f:
        f.write(b'NX2NSANS' + struct.pack('<II', 1, ANS_REC))
        for _, rec in recs:
            f.write(rec)
    by = [sum(1 for p, _ in recs if p == i) for i in range(3)]
    print(f'{listed} listed variants and {len(shaders)} default ones: {len(recs)} of {len(progs)} shaders in {a.version} '
          f'(priority t {by[0]}, warm {by[1]}, rest {by[2]}), skipped {skipped}, {left_out} slow unused default variants '
          f'left out (SLOW_UNUSED); inventory {t1 - t0:.1f} s, '
          f'seed written in {time.monotonic() - t0:.1f} s', flush=True)
    if a.no_run:
        return 0
    exe = out / a.exe
    r = subprocess.run([str(exe), '--prepare-shaders'], cwd=str(out))
    for _ in range(RETRIES):
        if not r.returncode or not driver_crashed(out):
            break
        print(f'prepare_shaders: the graphics driver crashed while building shaders (rc={r.returncode}); building them '
              'again without the variants it was building (nx2saria.exe --warm-shaders)', flush=True)
        r = subprocess.run([str(exe), '--warm-shaders'], cwd=str(out))
    print(f'{a.exe} --prepare-shaders: rc={r.returncode}; total {time.monotonic() - t0:.1f} s')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
