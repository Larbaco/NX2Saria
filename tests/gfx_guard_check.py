# needs: clang
"""Build and run tests/gfx_guard_check.c: runtime/gfx/gfx_guard.h, the checks of the presentation path that must not trust
what they are given: the game's crop cut to the image without int overflow (runtime/gfx/gfx_present.c present_image),
the swapchain image list taken whole or not at all (create_swapchain), and how often a repeating failure is logged.
Where the compiler has it, the harness is built with the signed overflow sanitizer, so a sum that overflows traps.

Then a scan of runtime/gfx/gfx_present.c and runtime/vk/vk_texture.c: no swapchain image query, semaphore, fence, command
pool, command buffer or sampler create there may drop its result (a call that starts a statement), and a failed
vkCreateSampler may not end the game (rt_fatal right after it). No game files, no GPU.

Usage: gfx_guard_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
try:
    from common import MSYS_BIN, compiler  # noqa: E402
except ImportError:   # a Python without the differential tests' packages: the same compiler choice without them
    MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

    def compiler():
        if os.environ.get('CC'):
            return os.environ['CC']
        if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
            return str(MSYS_BIN / 'clang.exe')
        return shutil.which('clang') or 'clang'

CHECKED_CALLS = ('vkGetSwapchainImagesKHR', 'vkCreateSemaphore', 'vkCreateFence', 'vkCreateCommandPool',
                 'vkAllocateCommandBuffers', 'vkCreateSampler')
SCANNED = ('gfx_present.c', 'vk_texture.c')


def strip_comments(src):
    """src with comments and string literals blanked, line breaks kept, so positions still name their lines."""
    out, k, n = [], 0, len(src)
    while k < n:
        if src.startswith('//', k):
            e = src.find('\n', k)
            e = n if e < 0 else e
            out.append(' ' * (e - k))
            k = e
        elif src.startswith('/*', k):
            e = src.find('*/', k + 2)
            e = n if e < 0 else e + 2
            out.append(re.sub(r'[^\n]', ' ', src[k:e]))
            k = e
        elif src[k] in '"\'':
            q, e = src[k], k + 1
            while e < n and src[e] != q:
                e += 2 if src[e] == '\\' else 1
            out.append(' ' * (e + 1 - k))
            k = e + 1
        else:
            out.append(src[k])
            k += 1
    return ''.join(out)


def statement_level(src, at):
    """1 when the expression at src[at] starts a statement or follows a comma operator outside any parentheses: what it
    returns is dropped. Walks back over whitespace to the nearest significant character, then over a comma's left side
    to the statement's start, keeping count of brackets."""
    k = at - 1
    while k >= 0 and src[k].isspace():
        k -= 1
    if k < 0 or src[k] in ';{}' or src[k] == ')' or src[max(0, k - 3):k + 1] == 'else':
        return True
    if src[k] != ',':
        return False
    depth = 0
    while k >= 0:
        c = src[k]
        if c in ')]':
            depth += 1
        elif c in '([':
            if depth == 0:
                return False   # inside an argument list or a condition
            depth -= 1
        elif depth == 0 and c in ';{}':
            return True
        k -= 1
    return True


def scan(root):
    """Problems as 'file:line: text'; one per call that drops its result, and one per fatal sampler failure."""
    problems = []
    call = re.compile(r'\b(' + '|'.join(CHECKED_CALLS) + r')\s*\(')
    for name in SCANNED:
        src = strip_comments((root / 'runtime' / name).read_text(encoding='utf-8', errors='replace'))
        for m in call.finditer(src):
            line = src.count('\n', 0, m.start(1)) + 1
            if statement_level(src, m.start(1)):
                problems.append(f'runtime/{name}:{line}: the result of {m.group(1)} is dropped')
            if m.group(1) == 'vkCreateSampler':   # the call's statement and the one after it may not end the game
                e = src.find(';', m.end())
                e = src.find(';', e + 1) if e >= 0 else -1
                if 'rt_fatal' in src[m.end():e if e >= 0 else len(src)]:
                    problems.append(f'runtime/{name}:{line}: a failed vkCreateSampler ends the game')
    return problems


def self_test():
    """The scan catches each kind of fault in a made-up file and passes its fixed form."""
    import tempfile
    bad = ('void f(void) {\n    vkCreateSemaphore(d, &i, NULL, &s);\n    for (;;) vkCreateFence(d, &i, NULL, &f);\n'
           '    if (x) y(); else vkGetSwapchainImagesKHR(d, sc, &n, img);\n    a = 1, vkCreateCommandPool(d, &p, NULL, &q);\n'
           '    VkResult r = vkCreateSampler(d, &si, NULL, &s);\n    if (r != VK_SUCCESS) rt_fatal("x");\n'
           '    if (vkCreateSampler(d, &si, NULL, &s) != VK_SUCCESS) { rt_fatal("y"); }\n}\n')
    good = ('void f(void) {\n    // vkCreateSemaphore(d, &i, NULL, &s); in a comment\n'
            '    VkResult r = vkCreateSemaphore(d, &i, NULL, &s);\n    if ((r = vkCreateFence(d, &i, NULL, &f))) g();\n'
            '    if (vkCreateSampler(d, &si, NULL, &s) != VK_SUCCESS) s = 0;\n    h("vkCreateFence(");\n'
            '    k(1, vkCreateSemaphore(d, &i, NULL, &s));\n    ok = ok && vkCreateFence(d, &i, NULL, &f) == 0;\n}\n')
    with tempfile.TemporaryDirectory() as t:
        (Path(t) / 'runtime').mkdir()
        for name in SCANNED:
            (Path(t) / 'runtime' / name).write_text(bad if name == SCANNED[0] else good)
        found = scan(Path(t))
    want = 6   # the semaphore, the fence, the image query, the pool after a comma, two fatal samplers; the good file nothing
    ok = len(found) == want and all(SCANNED[0] in p for p in found)
    if not ok:
        print('gfx_guard_check: FAIL: the scan\'s self-test found %d problems, expected %d:\n  %s'
              % (len(found), want, '\n  '.join(found)))
    return ok


def main():
    work = ROOT / 'build' / 'test' / 'gfx_guard'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('gfx_guard_check.exe' if os.name == 'nt' else 'gfx_guard_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    base = [compiler(), '-O1', '-Wall', '-Werror', '-I', str(ROOT / 'runtime'), str(ROOT / 'tests' / 'gfx_guard_check.c'),
            '-o', str(exe)]
    san = ['-fsanitize=signed-integer-overflow', '-fno-sanitize-recover=all']
    if subprocess.run(base + san, env=env, capture_output=True).returncode != 0:
        print('gfx_guard_check: no signed overflow sanitizer here; built without it')
        subprocess.check_call(base, env=env)
    rc = subprocess.run([str(exe)], env=env).returncode
    if not self_test():
        rc = 1
    problems = scan(ROOT)
    for p in problems:
        print('gfx_guard_check: FAIL: ' + p)
    print('gfx_guard_check: the scan of %s found %d problems' % (', '.join(SCANNED), len(problems)))
    return 1 if rc or problems or log_lines() else 0


LOG_LINES = ['"gfx: %s failed \\(%d\\), %u times: %s; the swapchain is made again', '"gfx: vkCreateSampler failed \\(%d\\), %u times: %s"', '"gfx: %s failed \\(%d\\), %d times: the %s presenter', '"gfx: presentation checks on \\(no setting\\)', '"gfx: the game\\\'s crop %d,%d %dx%d %s the %dx%d image: .*%u times"']   # the log's lines a run is checked by (the formats in the runtime's source)


def log_lines():
    """0 when every log line this change leaves is in the runtime's source as its format string."""
    src = ''.join((ROOT / 'runtime' / f).read_text(encoding='utf-8', errors='replace') for f in ['gfx_present.c', 'vk_texture.c'])
    src = re.sub(r'"\s*\n\s*"', '', src)   # adjacent string literals as one
    missing = [p for p in LOG_LINES if not re.search(p, src)]
    for p in missing:
        print('gfx_guard_check: FAIL: no log line matching %s in %s' % (p, ', '.join(['gfx_present.c', 'vk_texture.c'])))
    return 1 if missing else 0


if __name__ == '__main__':
    sys.exit(main())
