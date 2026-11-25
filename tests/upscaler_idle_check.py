import re
import sys
from pathlib import Path

WAIT_ALLOWED = {
    'vk/vk_dlaa.c': {'vk_upscaler_device_idle'},
    'vk/vk_ffx.cpp': {'vk_ffx_probe_cache'},
    'vk/vk_ffx4.cpp': set(),
    'vk/vk_fsr3.c': set(),
}
IDLE_ALLOWED = {'vk_upscaler_settle', 'vk_upscaler_gpu_wait'}
LET_GO = {
    'vk/vk_dlaa.c': ['free_size', 'free_snap', 'stale_sizes', 'release_kept', 'hudless_from', 'pb_images', 'retire_idle',
                  'evaluate'],
    'vk/vk_ffx.cpp': ['context_for'],
    'vk/vk_ffx4.cpp': ['context_for'],
    'vk/vk_fsr3.c': ['ctx_for'],
}
REFUSED = {
    'vk/vk_dlaa.c': ['evaluate'],
    'vk/vk_ffx.cpp': ['context_for'],
    'vk/vk_ffx4.cpp': ['context_for'],
    'vk/vk_fsr3.c': ['ctx_for'],
}
FREE_NOW = {
    'vk/vk_dlaa.c': ['destroy_size', 'destroy_snap', 'hl_destroy', 'pb_destroy', 'feat_release', 'N.release',
                  'vkDestroyImageView', 'vmaDestroyImage'],
    'vk/vk_ffx.cpp': ['A.destroy'],
    'vk/vk_ffx4.cpp': ['A.destroy'],
    'vk/vk_fsr3.c': ['free_ctx', 'vkDestroyImageView', 'vmaDestroyImage'],
}
NO_FREE_NOW = {
    'vk/vk_dlaa.c': ['free_size', 'free_snap', 'stale_sizes', 'release_kept', 'retire_idle', 'evaluate', 'retire_size',
                  'retire_feat', 'retire_snap', 'retire_hl', 'retire_pb'],
    'vk/vk_ffx.cpp': ['context_for', 'trim', 'retire_up', 'vk_ffx_trim_idle', 'vk_ffx_release_idle'],
    'vk/vk_ffx4.cpp': ['context_for', 'vk_ffx4_trim', 'retire_up', 'vk_ffx4_release_idle'],
    'vk/vk_fsr3.c': ['ctx_for', 'trim', 'retire_ctx', 'vk_fsr3_release_idle', 'vk_fsr3_upscale'],
}
NO_FREE_BEFORE_MAKE = {'vk/vk_dlaa.c': ['hudless_from', 'pb_images', 'snap_for']}
TAKE_ALL = {
    'vk/vk_dlaa.c': {'vk_dlaa_release': False, 'dead_put': True},
    'vk/vk_ffx.cpp': {'vk_ffx_release': False, 'retire_up': True},
    'vk/vk_ffx4.cpp': {'vk_ffx4_release': False, 'retire_up': True},
    'vk/vk_fsr3.c': {'vk_fsr3_release': False, 'retire_ctx': True},
}
FUNC = re.compile(r'^(?:extern "[^"\n]*" )?(?:static )?(?:inline )?[A-Za-z_][\w \*]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{',
                  re.M)


def blank_comments(src):
    out, i, n = list(src), 0, len(src)
    while i < n:
        if src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = ' '
            i = j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != '\n':
                    out[k] = ' '
            i = j
        elif src[i] in '"\'':
            q, j = src[i], i + 1
            while j < n and src[j] != q and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            for k in range(i + 1, min(j, n)):
                out[k] = ' '
            i = j + 1
        else:
            i += 1
    return ''.join(out)


def functions(code):
    found = []
    for m in FUNC.finditer(code):
        depth, k = 0, m.end() - 1
        while k < len(code):
            if code[k] == '{':
                depth += 1
            elif code[k] == '}':
                depth -= 1
                if depth == 0:
                    break
            k += 1
        found.append((m.group(1), m.end() - 1, k))
    return found


def enclosing(funcs, pos):
    for name, a, b in funcs:
        if a <= pos <= b:
            return name
    return None


def calls(code, funcs, callee):
    out = []
    for m in re.finditer(r'\b%s\s*\(' % re.escape(callee), code):
        fn = enclosing(funcs, m.start())
        if fn is None:
            continue
        out.append((code.count('\n', 0, m.start()) + 1, fn))
    return out


def call_offsets(code, callee, a, b):
    return [m.start() for m in re.compile(r'\b%s\s*\(' % re.escape(callee)).finditer(code, a, b)]


def body(code, funcs, name):
    for n, a, b in funcs:
        if n == name:
            return code[a:b + 1]
    return None


def main():
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    rt = root / 'runtime'
    fails = []
    sources = {}
    for f in WAIT_ALLOWED:
        code = blank_comments((rt / f).read_text(encoding='utf-8'))
        sources[f] = (code, functions(code))
    for f, allowed in WAIT_ALLOWED.items():
        code, funcs = sources[f]
        for line, fn in calls(code, funcs, 'vkDeviceWaitIdle'):
            if fn not in allowed:
                fails.append(f'{f}:{line}: vkDeviceWaitIdle in {fn}, allowed only in {sorted(allowed) or "none"}')
        for line, fn in calls(code, funcs, 'vk_upscaler_device_idle'):
            if f != 'vk/vk_dlaa.c' or fn not in IDLE_ALLOWED:
                fails.append(f'{f}:{line}: vk_upscaler_device_idle in {fn}, allowed only in vk_dlaa.c '
                             f'{sorted(IDLE_ALLOWED)}')
    nwaits = len(calls(*sources['vk/vk_dlaa.c'], 'vkDeviceWaitIdle'))
    if nwaits != 1:
        fails.append(f'vk_dlaa.c: {nwaits} calls of vkDeviceWaitIdle, expected the one in vk_upscaler_device_idle')
    for f, names in LET_GO.items():
        code, funcs = sources[f]
        for name in names:
            b = body(code, funcs, name)
            if b is None:
                fails.append(f'{f}: no function {name}')
            elif not re.search(r'\bvk_upscaler_let_go\s*\(', b):
                fails.append(f'{f} {name}: lets something go without vk_upscaler_let_go')
    for f, names in REFUSED.items():
        code, funcs = sources[f]
        for name in names:
            b = body(code, funcs, name)
            if b is None:
                fails.append(f'{f}: no function {name}')
            elif not re.search(r'\bvk_upscaler_gpu_wait\s*\(', b):
                fails.append(f'{f} {name}: no retry of a refused creation after vk_upscaler_gpu_wait')
    for f, names in NO_FREE_NOW.items():
        code, funcs = sources[f]
        defined = {n for n, _, _ in funcs}
        fails += [f'{f}: no function {name}' for name in names if name not in defined]
        for callee in FREE_NOW[f]:
            for line, fn in calls(code, funcs, callee):
                if fn in names:
                    fails.append(f'{f}:{line}: {fn} calls {callee}, which frees at once what the GPU may still use')
    for f, names in NO_FREE_BEFORE_MAKE.items():
        code, funcs = sources[f]
        for name in names:
            spans = [(a, b) for n, a, b in funcs if n == name]
            if not spans:
                fails.append(f'{f}: no function {name}')
            for a, b in spans:
                made = call_offsets(code, 'image2d', a, b)
                if not made:
                    fails.append(f'{f} {name}: no image2d call')
                    continue
                for callee in FREE_NOW[f]:
                    for at in call_offsets(code, callee, a, made[0]):
                        fails.append(f'{f}:{code.count(chr(10), 0, at) + 1}: {name} calls {callee} before it makes '
                                     f'its images, on what the GPU may still use')
    for f, allowed in TAKE_ALL.items():
        code, funcs = sources[f]
        for name, a, b in funcs:
            for at in call_offsets(code, 'upd_take_all', a, b):
                line = code.count('\n', 0, at) + 1
                if name not in allowed:
                    fails.append(f'{f}:{line}: upd_take_all in {name}, allowed only in {sorted(allowed)}')
                elif allowed[name] and not call_offsets(code, 'vk_upscaler_settle', a, at):
                    fails.append(f'{f}:{line}: upd_take_all in {name} before vk_upscaler_settle')

    key = 'UPSCALER_IDLE_WAIT'
    cfg = (rt / 'cfg' / 'cfg.c').read_text(encoding='utf-8-sig')
    if not re.search(r'\{\s*"%s"\s*,\s*"display"\s*\}' % key, cfg):
        fails.append(f'cfg.c: {key} has no sections[] entry in "display"')
    ini = (root / 'nx2saria.ini.example').read_text(encoding='utf-8-sig')
    if not re.search(r'^#\s*%s\s*=' % key.lower(), ini, re.M):
        fails.append(f'nx2saria.ini.example: no {key.lower()} line')

    host = (rt / 'host.h').read_text(encoding='utf-8')
    m = re.search(r'enum\s*\{\s*(PERF_M_FILE[^}]*)\}', host)
    kinds = [k.strip() for k in m.group(1).split(',') if k.strip() and k.strip() != 'PERF_M_N'] if m else []
    perf = (rt / 'diag' / 'perf.c').read_text(encoding='utf-8')
    m = re.search(r'mark_name\[PERF_M_N\]\s*=\s*\{(.*?)\};', perf, re.S)
    names = re.findall(r'"[^"]*"', m.group(1)) if m else []
    if not kinds or len(names) != len(kinds):
        fails.append(f'perf.c: mark_name has {len(names)} names for {len(kinds)} PERF_M kinds in host.h')

    code, funcs = sources['vk/vk_dlaa.c']
    b = body(code, funcs, 'pb_images')
    if b is None:
        fails.append('vk_dlaa.c: no function pb_images')
    else:
        clears = [mm.end() for mm in re.finditer(r'\b(?:pb_destroy|retire_pb)\s*\(\s*\)|memset\s*\(\s*&PB\b', b)]
        sets = [mm.start() for mm in re.finditer(r'\bPB\.used\s*=', b)]
        last_clear = max(clears) if clears else -1
        if not sets or max(sets) < last_clear:
            fails.append('vk_dlaa.c pb_images: PB.used is not noted after the last call that clears PB')

    for f in fails:
        print('FAIL', f)
    print('upscaler_idle_check:', 'FAILED' if fails else
          f'passed (vkDeviceWaitIdle once in vk_dlaa.c, {sum(len(v) for v in LET_GO.values())} places let go, '
          f'{len(kinds)} perf mark kinds named)')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
