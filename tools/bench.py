import argparse
import bisect
import csv
import datetime
import json
import math
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tomllib
from pathlib import Path

DESCRIPTION = 'Benchmark scenes: the same place, route and camera every run, every frame timed, a summary per scene.'

ROOT = Path(__file__).resolve().parent.parent
RUN_GAME = ROOT / 'tools' / 'run_game.py'
SCENES = ROOT / 'tools' / 'bench' / 'scenes.toml'

SLOW_RE = re.compile(r'perf: slow frame (\d+): ([\d.]+) ms; blocked pipeline ([\d.]+) xlate ([\d.]+) texture ([\d.]+) '
                     r'astc ([\d.]+) fence ([\d.]+) pacing ([\d.]+) drain ([\d.]+)(?: handoff [\d.]+)?; replay ([\d.]+)')
SEG_START_RE = re.compile(r"segment: '([^']*)' starts at frame (\d+), world ([\d.]+|n/a) s")
SEG_END_RE = re.compile(r"segment: '([^']*)' frames (\d+)-(\d+), world \S+ to \S+ s: (\d+) frames in ([\d.]+) s, "
                        r"mean ([\d.]+) ms \(([\d.]+) fps\), 1% low ([\d.]+) fps")
WIN_RE = re.compile(r'perf: frame (\d+): gpu avg ([\d.]+|n/a)(?: ms)?.*?; bound: (\w+)')
WIN_REPLAY_RE = re.compile(r'perf: frame (\d+): interval avg .*?; replay avg ([\d.]+) ms')
FIELDS = ('frame', 'ms', 'pipeline', 'xlate', 'texture', 'astc', 'fence', 'pacing', 'drain', 'replay')
BOUNDS = ('cpu', 'gpu', 'stall', 'pace')


def bound_of(f, gpu_ms=None):
    ms = f['ms']
    if ms <= 0:
        return 'cpu'
    if f['fence'] >= 0.5 * ms or (gpu_ms is not None and gpu_ms >= 0.8 * ms):
        return 'gpu'
    if f['pipeline'] + f['xlate'] + f['texture'] >= 0.25 * ms:
        return 'stall'
    if f['pacing'] >= 0.15 * ms:
        return 'pace'
    return 'cpu'


def parse_log(text):
    frames, starts, ends, windows, replay_win = {}, {}, {}, [], {}
    for line in text.splitlines():
        if m := SLOW_RE.search(line):
            f = dict(zip(FIELDS, (int(m[1]), *map(float, m.groups()[1:]))))
            frames[f['frame']] = f
        elif m := SEG_START_RE.search(line):
            starts[m[1]] = int(m[2])
        elif m := SEG_END_RE.search(line):
            ends[m[1]] = {'f0': int(m[2]), 'f1': int(m[3]), 'frames': int(m[4]), 'mean_ms': float(m[6]),
                          'fps': float(m[7]), 'low1_fps': float(m[8])}
        elif m := WIN_RE.search(line):
            windows.append({'frame': int(m[1]), 'gpu_ms': None if m[2] == 'n/a' else float(m[2]), 'bound': m[3]})
        elif m := WIN_REPLAY_RE.search(line):
            replay_win[int(m[1])] = float(m[2])
    for w in windows:
        w['replay_ms'] = replay_win.get(w['frame'])
    mark_bounds(frames, windows)
    return frames, starts, ends, windows


def mark_bounds(frames, windows):
    ends = sorted(w['frame'] for w in windows)
    gpu = {w['frame']: w['gpu_ms'] for w in windows}
    for k, f in frames.items():
        i = bisect.bisect_left(ends, k)
        f['bound'] = bound_of(f, gpu[ends[i]] if i < len(ends) and ends[i] - 60 < k else None)


def measured(frames, starts, windows, seg='measure', end='end'):
    if seg not in starts or end not in starts:
        return [], []
    f0, f1 = starts[seg], starts[end]
    return ([frames[k] for k in sorted(frames) if f0 < k <= f1],
            [w for w in windows if f0 + 60 <= w['frame'] <= f1])


def percentile(sorted_vals, q):
    if not sorted_vals:
        return math.nan
    pos = (len(sorted_vals) - 1) * q
    lo = math.floor(pos)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def stats(ms_list):
    if not ms_list:
        return {'frames': 0}
    s = sorted(ms_list)
    k = max(1, math.ceil(len(s) / 100))
    worst = s[-k:]
    return {'frames': len(s), 'avg_fps': 1000 * len(s) / sum(s), 'low1_fps': 1000 / statistics.fmean(worst),
            'p99_ms': percentile(s, 0.99), 'max_ms': s[-1], 'over33': sum(v > 1000 / 30 for v in s)}


def run_summary(frames, wins):
    r = stats([f['ms'] for f in frames])
    if frames:
        n = len(frames)
        for b in BOUNDS:
            r[f'{b}_pct'] = 100 * sum(f['bound'] == b for f in frames) / n
        r['replay_ms'] = statistics.fmean(f['replay'] for f in frames)
        r['fence_ms'] = statistics.fmean(f['fence'] for f in frames)
    gpu = [w['gpu_ms'] for w in wins if w['gpu_ms'] is not None]
    r['gpu_ms'] = statistics.fmean(gpu) if gpu else math.nan
    verdicts = [w['bound'] for w in wins]
    r['win_bound'] = max(set(verdicts), key=verdicts.count) if verdicts else ''
    return r


def spread(vals):
    vals = [v for v in vals if v == v]
    if not vals:
        return (math.nan,) * 4
    m = statistics.fmean(vals)
    return m, min(vals), max(vals), 100 * (max(vals) - min(vals)) / m if m else math.nan


def mangohud_frames(path):
    rows = path.read_text(errors='replace').splitlines()
    for i, line in enumerate(rows):
        cols = line.split(',')
        if 'frametime' in cols and 'fps' in cols:
            ft = cols.index('frametime')
            out = []
            for r in rows[i + 1:]:
                c = r.split(',')
                try:
                    out.append(float(c[ft]))
                except (ValueError, IndexError):
                    pass
            if out and statistics.median(out) > 1000:
                out = [v / 1000 for v in out]
            return out
    return []


def mangohud_window(mh, game_frames_all, f0, f1):
    after = sum(f['ms'] for k, f in game_frames_all.items() if k > f1) / 1000
    length = sum(f['ms'] for k, f in game_frames_all.items() if f0 < k <= f1) / 1000
    t_end = sum(mh) / 1000 - after
    out, t, eps = [], 0.0, 0.0005
    for v in mh:
        t += v / 1000
        if t_end - length + eps < t <= t_end + eps:
            out.append(v)
    return out


def load_scenes(path, only):
    scenes = tomllib.loads(Path(path).read_text(encoding='utf-8'))['scene']
    if only:
        names = only.split(',')
        scenes = [s for s in scenes if s['name'] in names]
    return scenes


def make_script(scene, title_ticks=6000):
    lines = [f'# bench scene {scene["name"]}', '# title screen to the first save slot: A every 90 ticks']
    lines += [f'{t} A 4' for t in range(240, title_ticks, 90)]
    warm, length = int(scene['warmup_s'] * 1000), int(scene['measure_s'] * 1000)
    for t, stick, x, y, hold in scene.get('moves', []):
        lines.append(f'@{warm + int(t * 1000)} {stick} {x} {y} {int(hold * 1000)}')
    lines.append(f'@{warm} MARK measure')
    lines.append(f'@{warm + length} MARK end')
    return '\n'.join(lines) + '\n'


def boot_map_of(scene):
    for k, (_, step) in enumerate(scene.get('dbg', [])):
        parts = step.split()
        if parts[0] == 'warp' and len(parts) in (2, 3):
            rest = [s for j, s in enumerate(scene['dbg']) if j != k]
            return ':'.join(parts[1:]), {**scene, 'dbg': rest}
    return None, scene


def dbg_test(scene):
    parts = [f'{t}:{cmd}' for t, cmd in scene.get('dbg', [])]
    return ';'.join(parts)


def machine():
    info = {'os': platform.platform()}
    try:
        info['cpu'] = next(l.split(':', 1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines()
                           if l.startswith('model name'))
    except (OSError, StopIteration):
        info['cpu'] = platform.processor()
    try:
        out = subprocess.run(['vulkaninfo', '--summary'], capture_output=True, text=True, timeout=30).stdout
        for key in ('deviceName', 'driverInfo'):
            if m := re.search(rf'{key}\s*=\s*(.+)', out):
                info[key] = m[1].strip()
    except (OSError, subprocess.TimeoutExpired):
        pass
    return info


def git(*args):
    try:
        return subprocess.run(['git', '-C', str(ROOT), *args], capture_output=True, text=True).stdout.strip()
    except OSError:
        return ''


def game_version(log_text):
    m = re.search(r'NX2(?:Saria|navi) (\d+\.\d+\.\d+)', log_text)
    return m[1] if m else ''


def recorded_build_commit(game):
    game = Path(game)
    try:
        lines = (game / 'build-commits.txt').read_text(encoding='utf-8').splitlines()
        if not lines or not (game / 'ocarina').is_file():
            return None
        fields = lines[-1].split()
        if len(fields) != 4:
            return None
        commit, name, stamp, result = fields
        if (not re.fullmatch(r'(?:[0-9a-f]{40}|[0-9a-f]{64})', commit) or not commit.strip('0') or
                name != game.name or result != 'rc=0'):
            return None
        built = datetime.datetime.fromisoformat(stamp)
        if built.tzinfo is None:
            return None
        if (game / 'ocarina').stat().st_mtime > built.timestamp() + 1:
            return None
        return commit
    except (OSError, UnicodeError, ValueError, OverflowError):
        return None


def cmd_run(a):
    game = Path(a.game).expanduser().resolve()
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M')
    out = (Path(a.out) if a.out else ROOT / 'build' / 'bench' / f'{stamp}-{a.label}').resolve()
    out.mkdir(parents=True, exist_ok=True)
    scenes = load_scenes(a.scenes, a.only)
    meta = {'label': a.label, 'game': str(game), 'started': stamp, 'build_commit': recorded_build_commit(game),
            'head': git('rev-parse', 'HEAD'),
            'origin_main': git('rev-parse', 'origin/main'), 'machine': machine(), 'set': a.set,
            'runs': a.runs, 'scenes': [s['name'] for s in scenes], 'shot': a.shot, 'boot_map': a.boot_map}
    for name in ('nx2saria.ini', 'nx2navi.ini'):
        if (game / name).is_file():
            shutil.copyfile(game / name, out / name)
            break
    (out / 'meta.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
    for run in range(1, a.runs + 1):
        for s in scenes:
            tag = f'{s["name"]}-{run}'
            script = out / f'{s["name"]}.input.txt'
            boot_map = None
            if a.boot_map:
                boot_map, s = boot_map_of(s)
                if not boot_map:
                    sys.exit(f'bench: --boot-map: the scene {s["name"]} has no warp step to boot to')
            script.write_text(make_script(s, title_ticks=0 if boot_map else 6000), encoding='utf-8')
            seconds = s['warmup_s'] + s['measure_s'] + 2
            env = ['NX2SARIA_TRACE=perf', 'NX2SARIA_PERF_SLOW_MS=1', 'NX2SARIA_FPS_LIMIT=unlimited',
                   f'NX2SARIA_DBG_TEST={dbg_test(s)}', *(e.replace('{tag}', tag) for e in a.set)]
            if a.mangohud:
                mh_dir = out / f'mangohud-{tag}'
                mh_dir.mkdir(exist_ok=True)
                env += ['MANGOHUD=1', f'MANGOHUD_CONFIG=fps_only,autostart_log=1,log_interval=0,'
                                      f'output_folder={mh_dir}']
            cmd = [sys.executable, str(RUN_GAME), str(game), '--save', a.save, '--script', str(script),
                   *(['--boot-map', boot_map] if boot_map else []),
                   '--seconds', str(seconds), '--wall', str(seconds + a.boot_wall), '--tag', f'bench-{tag}',
                   '--log', str(out / f'{tag}.log'), '--quiet']
            if a.shot:
                cmd += ['--shot', str(out / f'{tag}.png')]
            for e in env:
                cmd += ['--env', e]
            print(f'bench: {tag} ...', flush=True)
            subprocess.run(cmd, check=False)
            write_run_csv(out, tag)
    summarize(out)


def write_run_csv(out, tag):
    log = out / f'{tag}.log'
    if not log.is_file():
        return
    frames, starts, _, windows = parse_log(log.read_text(errors='replace'))
    sel, _ = measured(frames, starts, windows)
    with open(out / f'{tag}.csv', 'w', newline='') as fh:
        w = csv.writer(fh)
        w.writerow([*FIELDS, 'bound'])
        for f in sel:
            w.writerow([f[k] for k in FIELDS] + [f['bound']])


def summarize(out):
    out = Path(out)
    meta = json.loads((out / 'meta.json').read_text()) if (out / 'meta.json').is_file() else {}
    per_scene, rows, version = {}, [], ''
    for log in sorted(out.glob('*-[0-9]*.log')):
        tag = log.stem
        scene, run = tag.rsplit('-', 1)
        text = log.read_text(errors='replace')
        version = version or game_version(text)
        frames, starts, ends, windows = parse_log(text)
        sel, wins = measured(frames, starts, windows)
        r = run_summary(sel, wins)
        r.update(scene=scene, run=int(run), game_fps=ends.get('measure', {}).get('fps', math.nan),
                 game_low1=ends.get('measure', {}).get('low1_fps', math.nan),
                 game_frames=ends.get('measure', {}).get('frames', 0),
                 ok='measure' in starts and 'end' in starts)
        mh = sorted((out / f'mangohud-{tag}').glob('*.csv')) if (out / f'mangohud-{tag}').is_dir() else []
        mh = [p for p in mh if not p.name.endswith('_summary.csv')]
        if mh and r['ok']:
            m = mangohud_window(mangohud_frames(mh[-1]), frames, starts['measure'], starts['end'])
            ms = stats(m)
            r['mh_avg_fps'], r['mh_p99_ms'] = ms.get('avg_fps', math.nan), ms.get('p99_ms', math.nan)
        rows.append(r)
        per_scene.setdefault(scene, []).append(r)
    keys = ['scene', 'run', 'ok', 'frames', 'game_frames', 'avg_fps', 'low1_fps', 'p99_ms', 'max_ms', 'over33',
            'cpu_pct', 'gpu_pct', 'stall_pct', 'pace_pct', 'replay_ms', 'fence_ms', 'gpu_ms', 'win_bound', 'game_fps',
            'game_low1', 'mh_avg_fps', 'mh_p99_ms']
    with open(out / 'summary.csv', 'w', newline='') as fh:
        w = csv.DictWriter(fh, fieldnames=keys, extrasaction='ignore')
        w.writeheader()
        for r in rows:
            w.writerow({k: (round(v, 2) if isinstance(v, float) else v) for k, v in r.items()})
    md = [f'# Benchmark {meta.get("label", "")} ({meta.get("started", "")})', '',
          f'- Game: {meta.get("game", "")}, NX2Saria {version or "?"}',
          f'- Build commit: {(meta.get("build_commit") or "unknown")[:10]} (build-commits.txt record)',
          f'- Checkout: HEAD {meta.get("head", "")[:10] or "unknown"}, '
          f'origin/main {meta.get("origin_main", "")[:10] or "unknown"}',
          f'- Machine: {meta.get("machine", {})}',
          '- Mode: ' + ('--shot (pipelines built on the render thread; not comparable with play mode)'
                        if meta.get('shot', True) else 'play (pipelines built in the background)'),
          f'- Extra settings: {meta.get("set") or "none"} '
          '(plus NX2SARIA_FPS_LIMIT=unlimited; the rest from the copied settings file)', '',
          '| Scene | Runs | Avg fps (min-max) | 1% low fps (min-max) | p99 ms | Spread avg | CPU % | GPU % | Stall % '
          '| GPU ms | Replay ms | Game says | MangoHud avg fps |',
          '|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    f = lambda v: '-' if v != v else f'{v:.1f}'
    for scene, rs in per_scene.items():
        good = [r for r in rs if r['ok'] and r['frames']]
        if not good:
            md.append(f'| {scene} | 0 of {len(rs)} | failed | | | | | | | | | | |')
            continue
        avg, low, p99 = (spread([r[k] for r in good]) for k in ('avg_fps', 'low1_fps', 'p99_ms'))
        mean = lambda k: statistics.fmean(r.get(k, math.nan) for r in good)
        md.append(f'| {scene} | {len(good)} of {len(rs)} | {f(avg[0])} ({f(avg[1])}-{f(avg[2])}) | '
                  f'{f(low[0])} ({f(low[1])}-{f(low[2])}) | {f(p99[0])} | {f(avg[3])}% | {f(mean("cpu_pct"))} | '
                  f'{f(mean("gpu_pct"))} | {f(mean("stall_pct"))} | {f(mean("gpu_ms"))} | {f(mean("replay_ms"))} | '
                  f'{f(mean("game_fps"))} / {f(mean("game_low1"))} | {f(mean("mh_avg_fps"))} |')
    md += ['', 'Avg fps: frames / time over the measured stretch. 1% low: the mean of the slowest 1% of frames, as fps. '
           'CPU/GPU/Stall: share of frames by what set their length (tools/bench.py bound_of). GPU ms: the GPU\'s '
           'own busy time per frame from timestamps (60-frame windows). Game says: the runtime\'s own segment mean fps '
           '/ 1% low (its 1% low is the 99th percentile bin, so it reads a little higher).']
    (out / 'summary.md').write_text('\n'.join(md) + '\n', encoding='utf-8')
    print('\n'.join(md))


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('game')
    r.add_argument('--save', required=True)
    r.add_argument('--scenes', default=str(SCENES))
    r.add_argument('--only')
    r.add_argument('--runs', type=int, default=3)
    r.add_argument('--out')
    r.add_argument('--label', default='bench')
    r.add_argument('--set', action='append', default=[], metavar='KEY=VALUE',
                   help='extra environment, repeatable; {tag} becomes the run\'s scene and number')
    r.add_argument('--boot-map', action='store_true',
                   help='start each scene in its area without the title screen (its first warp step becomes the boot map)')
    r.add_argument('--mangohud', action='store_true')
    r.add_argument('--shot', action='store_true',
                   help='save a PNG of each run\'s last frame; the runtime then builds pipelines on the render thread '
                        '(runtime/boot.c), so first visits stall longer than in play and the numbers are not '
                        'comparable with a run without it')
    r.add_argument('--boot-wall', type=float, default=240, help='wall seconds allowed for boot and save load')
    s = sub.add_parser('summary')
    s.add_argument('out')
    a = ap.parse_args()
    if a.cmd == 'run':
        cmd_run(a)
    else:
        summarize(a.out)


if __name__ == '__main__':
    main()
