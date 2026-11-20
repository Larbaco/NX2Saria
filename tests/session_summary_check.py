"""Build and run tests/session_summary_check.c: runtime/session_summary_fmt.c, the JSON text of logs/session-summary.json
(runtime/session_summary.c writes it at the game's end). Each sample summary (all fields, all null, hostile strings and
numbers past the limits, a device-lost end) must parse as JSON with exactly the summary's keys, each value of its type
and within its limits: strings printable and capped, settings and display values in [A-Za-z0-9_.:x+-] or "other",
vendor and device as 0x and 4 hex digits, numbers with at most one decimal. The C side checks the value rules (a setting
outside its list, ranges, the OS name, the frame-time 1% low). Also checks statically that the two ends call the
writer where they should (runtime/gfx_present.c rt_exit_steps, runtime/dispatch.c fatal_end) and that the hard crash
(runtime/main.c crash_handler) writes none. No game files.

Usage: session_summary_check.py
"""
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / 'runtime'
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'

# The summary's keys and the types of their leaves: str, int, float (an int or a number with one decimal), bool,
# 'hex' (0x and 4 hex digits) and 'token' (a display or settings value). None in a tuple: the leaf may be null.
SCHEMA = {
    'schema': 'str', 'version': 'str', 'game_version': 'str',
    'os': {'family': 'family', 'name': ('text', 64), 'wine': 'bool'},
    'cpu': {'name': ('text', 64), 'threads': ('int', 1024)},
    'ram_mb': ('int', 4194304),
    'gpu': {'name': ('text', 64), 'vendor': 'hex', 'device': 'hex', 'driver': ('text', 64), 'vram_mb': ('int', 4194304)},
    'display': {'output': 'token', 'render_scale': 'token', 'fullscreen': 'bool', 'game_aspect': 'token',
                'picture': 'token'},
    'settings': {'preset': 'token', 'upscaler': 'token', 'taa': 'token', 'dlss': 'token', 'framegen': 'token',
                 'fps_limit': ('int', None), 'vk_present': 'token'},
    'session': {'duration_s': ('int', 30 * 24 * 3600), 'frames': ('int', None), 'end': 'end'},
    'frametime': {'fps_mean': ('dec', 2000), 'fps_low_1pct': ('dec', 2000), 'worst_ms': ('dec', 600000),
                  'over_25ms': ('int', None), 'over_100ms': ('int', None)},
    'startup': {'shader_prewarm_ms': ('int', None), 'shaders_translated': ('int', None),
                'pipelines_built': ('int', None), 'pipeline_build_ms': ('int', None)},
    'fatal': ('text', 300),
}
NOT_NULL = {'schema', 'version', 'game_version', 'session.end'}
TOKEN = re.compile(r'[A-Za-z0-9_.:x+-]{1,32}')


def check_leaf(path, kind, v, raw):
    """Problems with one leaf, as a list of strings."""
    if v is None:
        return [] if path not in NOT_NULL else [f'{path} is null']
    kind, cap = kind if isinstance(kind, tuple) else (kind, None)
    if kind == 'str':
        return [] if isinstance(v, str) and v else [f'{path}: {v!r} is not a non-empty string']
    if kind == 'family':
        return [] if v in ('windows', 'linux') else [f'{path}: {v!r}']
    if kind == 'end':
        return [] if v in ('normal', 'fatal', 'device_lost', 'crash') else [f'{path}: {v!r}']
    if kind == 'bool':
        return [] if isinstance(v, bool) else [f'{path}: {v!r} is not a bool']
    if kind == 'hex':
        return [] if isinstance(v, str) and re.fullmatch(r'0x[0-9a-f]{4}', v) else [f'{path}: {v!r}']
    if kind == 'token':
        return [] if isinstance(v, str) and TOKEN.fullmatch(v) else [f'{path}: {v!r} is not a token']
    if kind == 'text':
        bad = []
        if not isinstance(v, str):
            return [f'{path}: {v!r} is not a string']
        if len(v) > cap:
            bad.append(f'{path}: {len(v)} characters, over {cap}')
        if any(ord(c) < 0x20 or 0x7f <= ord(c) < 0xa0 or 0xd800 <= ord(c) < 0xe000 for c in v):
            bad.append(f'{path}: a control character or surrogate in {v!r}')
        if v != v.strip():
            bad.append(f'{path}: spaces at an end of {v!r}')
        return bad
    if kind == 'int':
        if isinstance(v, bool) or not isinstance(v, int) or v < 0:
            return [f'{path}: {v!r} is not a non-negative int']
        return [f'{path}: {v} over {cap}'] if cap and v > cap else []
    if kind == 'dec':
        if isinstance(v, bool) or not isinstance(v, (int, float)) or v < 0:
            return [f'{path}: {v!r} is not a non-negative number']
        if cap and v > cap:
            return [f'{path}: {v} over {cap}']
        m = re.search(r'"' + re.escape(path.split('.')[-1]) + r'": ([0-9.]+)', raw)
        return [] if m and re.fullmatch(r'\d+(\.\d)?', m.group(1)) else [f'{path}: {m and m.group(1)} has more than 1 decimal']
    return [f'{path}: unknown kind {kind}']


def check_obj(prefix, schema, obj, raw):
    if not isinstance(obj, dict):
        return [f'{prefix or "summary"}: not an object']
    bad = []
    if set(obj) != set(schema):
        bad.append(f'{prefix or "summary"}: keys {sorted(obj)}, want {sorted(schema)}')
    for k, kind in schema.items():
        path = f'{prefix}.{k}' if prefix else k
        if k not in obj:
            continue
        if isinstance(kind, dict):
            # never a null group: the server refuses one (telemetry/src/validate.ts); its leaves are null instead
            bad += check_obj(path, kind, obj[k], raw)
        else:
            bad += check_leaf(path, kind, obj[k], raw)
    return bad


def expect(name, s):
    """What each case must hold beyond the schema."""
    bad = []
    if name == 'full':
        want = {'version': '0.19.0-rc.2', 'game_version': '1.1.1', 'schema': 'nx2saria.session/1'}
        bad += [f'full: {k} = {s[k]!r}' for k, v in want.items() if s[k] != v]
        if s['frametime'] != {'fps_mean': 59.9, 'fps_low_1pct': 52.1, 'worst_ms': 88.4, 'over_25ms': 14, 'over_100ms': 0}:
            bad.append(f'full: frametime {s["frametime"]}')
        if s['gpu']['vendor'] != '0x10de' or s['gpu']['device'] != '0x2786' or s['settings']['preset'] != 'high':
            bad.append(f'full: gpu {s["gpu"]}, preset {s["settings"]["preset"]!r}')
        if s['display'] != {'output': '2560x1440', 'render_scale': 'game', 'fullscreen': True, 'game_aspect': '16:10',
                            'picture': '2304x1440'}:
            bad.append(f'full: display {s["display"]}')
    elif name == 'all_null':
        leaves = [v for k, o in s.items() if isinstance(o, dict) for v in o.values()] + [s['ram_mb'], s['fatal']]
        if s['session']['end'] != 'crash' or sum(v is not None for v in leaves) != 1 or not all(v is None for v in s['frametime'].values()):
            bad.append(f'all_null: something besides session.end is set: {s}')
    elif name == 'hostile':
        if s['os']['name'] != 'Ubuntu "24.04" \\ LTS':
            bad.append(f'hostile: os.name {s["os"]["name"]!r}')
        if s['cpu']['name'] != 'Café “CPU”    🎮':
            bad.append(f'hostile: cpu.name {s["cpu"]["name"]!r}')
        if s['gpu']['name'] != 'N' * 64 or s['gpu']['driver'] != 'padded':
            bad.append(f'hostile: gpu {s["gpu"]}')
        if [s['display'][k] for k in ('output', 'render_scale', 'game_aspect')] != ['other'] * 3:
            bad.append(f'hostile: display {s["display"]}')
        if len(s['fatal']) != 300:
            bad.append(f'hostile: fatal is {len(s["fatal"])} characters, want 300')
        if (s['cpu']['threads'], s['ram_mb'], s['gpu']['vram_mb'], s['session']['duration_s']) != (1024, 4194304, 4194304, 2592000):
            bad.append('hostile: numbers past the limits are not held at them')
        if s['gpu']['vendor'] is not None or s['gpu']['device'] != '0x0001':
            bad.append(f'hostile: vendor {s["gpu"]["vendor"]!r} device {s["gpu"]["device"]!r}')
        if s['frametime']['fps_mean'] != 2000 or s['frametime']['fps_low_1pct'] is not None or s['frametime']['worst_ms'] != 600000:
            bad.append(f'hostile: frametime {s["frametime"]}')
    elif name == 'device_lost':
        if s['session']['end'] != 'device_lost' or not all(v is None for v in s['frametime'].values()) or not s['fatal'].startswith('vk: the GPU'):
            bad.append(f'device_lost: {s["session"]}, {s["frametime"]}, {s["fatal"]!r}')
    return bad


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return re.sub(r'//[^\n]*', '', source[start:end])


def static_checks():
    bad = 0
    gp = function((RT / 'gfx_present.c').read_text(encoding='utf-8'), 'void rt_exit_steps(unsigned what)')
    a, b = gp.find('prof_write('), gp.find('session_summary_exit(')
    ok = 0 <= a < b
    print(f'static: gfx_present.c rt_exit_steps writes the summary after prof_write: {"ok" if ok else "FAIL"}')
    bad += not ok
    fe = function((RT / 'dispatch.c').read_text(encoding='utf-8'),
                  'static void fatal_end(int code, const char *fmt, va_list ap) {')
    a, b, c = fe.find('va_copy('), fe.find('perf_summary_flush_try('), fe.find('session_summary_fatal(code')
    ok = 0 <= a and 0 <= b < c
    print(f'static: dispatch.c fatal_end copies the va_list and writes the summary after perf_summary_flush_try: '
          f'{"ok" if ok else "FAIL"}')
    bad += not ok
    ch = function((RT / 'main.c').read_text(encoding='utf-8'), 'static void crash_handler(const PlatCrash *x)')
    ok = 'session_summary' not in ch
    print(f'static: main.c crash_handler writes no summary: {"ok" if ok else "FAIL"}')
    bad += not ok
    return bad


def main():
    bad = static_checks()
    work = ROOT / 'build' / 'test' / 'session_summary'
    work.mkdir(parents=True, exist_ok=True)
    for f in work.glob('*.json'):
        f.unlink()
    exe = work / ('session_summary_check.exe' if os.name == 'nt' else 'session_summary_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(RT),
                           str(ROOT / 'tests' / 'session_summary_check.c'), str(RT / 'session_summary_fmt.c'),
                           '-o', str(exe), '-lm'], env=env)
    bad += subprocess.run([str(exe), str(work)], env=env).returncode != 0
    for name in ('full', 'all_null', 'hostile', 'device_lost'):
        raw = (work / f'{name}.json').read_text(encoding='utf-8')   # strict: invalid UTF-8 fails here
        try:
            s = json.loads(raw)
        except ValueError as e:
            print(f'{name}: not JSON: {e}: FAIL\n{raw}')
            bad += 1
            continue
        problems = check_obj('', SCHEMA, s, raw) + expect(name, s)
        print(f'{name}: {len(raw.encode())} bytes, {"ok" if not problems else "FAIL"}')
        for p in problems:
            print(f'  {p}')
        bad += bool(problems)
    print(f'session_summary_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
