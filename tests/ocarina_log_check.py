# needs: clang
"""Build runtime/pacing/frame_rate.c's ocarina log (ocarina_log_init, ocarina_watch, ocarina_end, cut out of the file) with
stand-ins for the patch sets, the patch page and the log, and play it the page words the sets orecord (+264) and ostaff
(+776) write: a stretch of ocarina use gives one line after two quiet seconds, with its presents, the presents the
recorder clock moved on and the notes the staff moved (rises of its signal), and nothing comes while neither word
changes. Without the sets the log watches nothing and says so at the start.

No GPU, no game files, any Python.

Usage: ocarina_log_check.py
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

HARNESS = r'''
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int sets_on = 1;   // the sets orecord (1) and ostaff (2) exist and are on
static int rt_patch_set(const char *name) { return !sets_on ? -1 : !strcmp(name, "orecord") ? 1 : !strcmp(name, "ostaff") ? 2 : -1; }
static int rt_patch_enabled(int set) { return set > 0; }
static uint32_t page[1024];
static void *rt_patch_page(void) { return page; }
static int lines;
static char last[512];
static void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last, sizeof last, fmt, ap);
    va_end(ap);
    lines++;
}

%s

static int fails;
static void expect(const char *what, int ok) {
    if (!ok) printf("FAIL %%s (last line: %%s)\n", what, last), fails++;
}

int main(void) {
    ocarina_log_init();
    expect("the start line names both sets on", lines == 1 && strstr(last, "set orecord on") && strstr(last, "set ostaff on"));
    uint64_t t = 1000000000ull, dt = 7000000ull;   // 7 ms a present, about 143 fps
    for (int i = 0; i < 100; i++) ocarina_watch(t += dt);   // the ocarina put away: no word changes
    expect("no line while nothing changes", lines == 1);
    // a played-back song: the clock's fraction changes every present, the staff signal rises three times
    static const int signal[] = {0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0};
    for (int i = 0; i < 13; i++) {
        float f = 0.1f * (float)(i + 1);
        memcpy(&page[264 / 4], &f, 4);
        page[776 / 4] = (uint32_t)signal[i];
        ocarina_watch(t += dt);
    }
    expect("no line before the quiet", lines == 1);
    for (int i = 0; i < 280; i++) ocarina_watch(t += dt);   // 1.96 s quiet
    expect("no line within two seconds", lines == 1);
    for (int i = 0; i < 20; i++) ocarina_watch(t += dt);
    expect("one line after two quiet seconds", lines == 2);
    expect("13 presents", strstr(last, "13 presents") != NULL);
    expect("the clock moved on all 13", strstr(last, "fraction on 13 of them") != NULL);
    expect("three notes", strstr(last, "moved 3 notes") != NULL);
    expect("about 143 fps", strstr(last, "(143 fps)") != NULL);
    for (int i = 0; i < 400; i++) ocarina_watch(t += dt);
    expect("no second line", lines == 2);
    if (fails) return 1;
    printf("ocarina_log_check: all checks passed\n");
    return 0;
}
'''


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def cut():
    """frame_rate.c from the ocarina log's first #define to the end of ocarina_watch."""
    src = (ROOT / 'runtime' / 'pacing' / 'frame_rate.c').read_text(encoding='utf-8')
    m = re.search(r'#define OCARINA_QUIET_NS .*?\nstatic void ocarina_watch\(uint64_t now_ns\) \{\n.*?\n\}\n', src, re.S)
    assert m, 'runtime/pacing/frame_rate.c: the ocarina log (OCARINA_QUIET_NS ... ocarina_watch) was not found'
    return m.group(0)


def main():
    work = ROOT / 'build' / 'test' / 'ocarina_log'
    work.mkdir(parents=True, exist_ok=True)
    c = work / 'ocarina_log_check.c'
    c.write_text('// Made by tests/ocarina_log_check.py from runtime/pacing/frame_rate.c.\n' + HARNESS % cut(), encoding='utf-8')
    exe = work / ('ocarina_log_check.exe' if os.name == 'nt' else 'ocarina_log_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function', str(c), '-o', str(exe)],
                          env=env)
    return subprocess.run([str(exe)], env=env).returncode


if __name__ == '__main__':
    sys.exit(main())
