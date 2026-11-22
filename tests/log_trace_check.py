# needs: clang
"""The log lines of the pipeline cache stamp, the play gate of the background builds and the presentation and sampler
failures, as the runtime's own code writes them: each function is cut from its source file by its text, built with
stand-ins for what it calls (real files in a scratch folder for the stamp, a clock the harness moves for the play
gate, a vkCreateSampler that fails on request), and run through a session's events. What its rt_log calls write goes
to build/test/log_trace/nx2saria-sample.log, and each line a player's log is read by must be there with the numbers
the events give:

- runtime/vk/vk_pipeline.c cache_stamp_check over the starts of an update (no stamp, the same translator, another one the
  carry serves, another one it does not while a file is held open and then free, the starts after) and a preparation
  under yet another translator: "vk: pipeline cache stamp <id> none|matches|differs: <action>, <MB>", the files
  deleted or kept as the line says, and the line's note that the run's exit writes pipeline-cache.bin on exactly the
  starts in play without that file (which saves write it: tests/cache_stamp_check.c);
- the play gate ("the play gate" to vk_pipeline_probe_buildgate): the start's line, then frames shown while builders
  rest, and "vk: background builds paused in play: N frames, M builds deferred" with those counts; with the setting
  at 0 the start's "off" line and no other;
- runtime/gfx/gfx_present.c swapchain_unusable failing 20 times: "gfx: vkGetSwapchainImagesKHR failed (5), N times: ..."
  for N = 1 to 8 and 16 only;
- runtime/vk/vk_texture.c make_sampler: a failed vkCreateSampler that the plain retry makes, and one it does not, each
  "gfx: vkCreateSampler failed (-1), N times: ..." with what stood in.

No game files, no GPU (Vulkan headers only). Usage: log_trace_check.py [--cc <clang>]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
import runtime_include

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


def source(name):
    return (ROOT / 'runtime' / name).read_text(encoding='utf-8', errors='replace')


def function(src, signature):
    """The function that starts with signature, through the closing brace at the start of a line."""
    at = src.index(signature)
    end = src.index('\n}\n', at) + 3
    return src[at:end]


def between(src, start, end):
    at = src.index(start)
    return src[at:src.index(end, at)]


COMMON = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE *logf_;
static void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(logf_, "[host] ");
    vfprintf(logf_, fmt, ap);
    fprintf(logf_, "\n");
    fflush(logf_);
    va_end(ap);
}
'''

# ---- the pipeline cache stamp
STAMP = COMMON + r'''
#include "vk_cache_stamp.h"
#define PLAT_PATH_MAX 260
static struct { char cache_path[PLAT_PATH_MAX * 2]; char play_path[PLAT_PATH_MAX * 2 + 8]; } P;
static int prep_mode, stamp_on = 1, carried, del_fail;
static uint8_t cur_id[32];
static int cfg_bool(const char *k) { (void)k; return stamp_on; }
static const uint8_t *gfx_shader_identity(void) { return cur_id; }
static int gfx_shader_carries(const uint8_t id[32]) { (void)id; return carried; }
static uint8_t *plat_file_read(const char *p, uint64_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len + 1);
    *n = fread(b, 1, (size_t)len, f);
    fclose(f);
    return b;
}
static int64_t plat_file_size(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fclose(f);
    return len;
}
static void plat_file_delete(const char *p) { if (!del_fail) remove(p); }   // del_fail: a file held open elsewhere
static int plat_file_exists(const char *p) { return plat_file_size(p) >= 0; }
static int plat_file_write_atomic(const char *p, const void *d, size_t n) {
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    fwrite(d, 1, n, f);
    fclose(f);
    return 1;
}
%s
static void put(const char *p, long mb) {
    FILE *f = fopen(p, "wb");
    if (mb) { fseek(f, mb * 1048576 - 1, SEEK_SET); fputc(0, f); }
    fclose(f);
}
int main(int argc, char **argv) {
    logf_ = fopen(argv[2], "a");
    char play[600];
    snprintf(P.cache_path, sizeof P.cache_path, "%%s/pipeline-cache.bin", argv[1]);
    snprintf(play, sizeof play, "%%s/pipeline-cache-play.bin", argv[1]);
    for (int i = 0; i < 32; i++) cur_id[i] = (uint8_t)(0xa0 + i);
    put(P.cache_path, 600), put(play, 50);
    cache_stamp_check();                       // 1: no stamp yet: read, stamped
    cache_stamp_check();                       // 2: the same translator
    cur_id[0] = 0x11;                          // 3: an update; the carry still serves the earlier translator
    carried = 1;
    cache_stamp_check();
    carried = 0;                               // 4: the carry pass is done, but a file is held open: read, kept
    del_fail = 1;
    cache_stamp_check();
    printf("held %%d %%d\n", plat_file_exists(P.cache_path), plat_file_exists(play));
    del_fail = 0;                              // 5: the files go; this run writes pipeline-cache.bin at its exit
    cache_stamp_check();
    printf("drop %%d %%d\n", plat_file_exists(P.cache_path), plat_file_exists(play));
    cache_stamp_check();                       // 6: that run ended before its exit: matches, the file still due
    put(P.cache_path, 30);                     // 7: an exit wrote it: matches, nothing due
    cache_stamp_check();
    cur_id[0] = 0x22;                          // 8: a preparation under yet another translator: deleted, and the
    prep_mode = 1;                             //    preparation writes the file itself
    cache_stamp_check();
    prep_mode = 0;
    stamp_on = 0;
    cache_stamp_check();                       // 9: the setting off
    return 0;
}
'''

# ---- the play gate
GATE = COMMON + r'''
#include "vk_play_gate.h"
typedef int PlatMutex;
#define PLAT_MUTEX_INIT 0
static void plat_mutex_lock(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static uint64_t clock_ns = 1000000000ull;
static uint64_t plat_now_ns(void) { return clock_ns; }
static double now_ms(void) { return (double)plat_now_ns() / 1e6; }
static void plat_sleep_ms(int ms) { clock_ns += (uint64_t)ms * 1000000ull; }
static int setting = 600, prep_mode, loading;
static int cfg_int(const char *k) { (void)k; return setting; }
static int vk_exec_pool_loading(void) { return loading; }
typedef struct Job { struct Job *next, *prev; uint8_t rested; } Job;
static struct { Job *low; } A;
static int bf_waiting(void) { return 0; }
%s
int main(int argc, char **argv) {
    logf_ = fopen(argv[1], "a");
    setting = atoi(argv[2]);
    pg_draws_min();                            // the start's line
    Job jobs[5] = {{0}};
    for (int i = 0; i < 4; i++) jobs[i].next = &jobs[i + 1];
    A.low = &jobs[0];                          // five builds queued
    for (int f = 0; f < 3; f++) clock_ns += 16000000ull, vk_pipeline_frame_note(40);   // a menu: light
    printf("menu rest %%d\n", play_rest());
    clock_ns += 16000000ull, vk_pipeline_frame_note(2850);                             // play: heavy
    int rest = play_rest();
    printf("play rest %%d\n", rest);
    if (rest) pg_rest_begin(low_held()), pg_rest_begin(low_held());                    // two workers rest
    for (int f = 0; f < 119; f++) clock_ns += 16000000ull, vk_pipeline_frame_note(2850);
    pg_rest_end(1920.0), pg_rest_end(1920.0);
    clock_ns += 16000000ull, vk_pipeline_frame_note(40);                               // the pause screen
    printf("pause rest %%d\n", play_rest());
    pg_log();                                  // a save
    pg_log();                                  // the next save, nothing new: no line
    loading = 1;
    clock_ns += 16000000ull, vk_pipeline_frame_note(2850);
    printf("load rest %%d\n", play_rest());
    return 0;
}
'''

# ---- swapchain_unusable
SWAP = COMMON + r'''
#include <vulkan/vulkan.h>
#include "gfx_guard.h"
typedef struct { VkDevice device; } VkCtx;
static VkSwapchainKHR g_sc;
static volatile int32_t g_dirty;
static int32_t plat_inc32(volatile int32_t *p) { return ++*p; }
static int32_t plat_xchg32(volatile int32_t *p, int32_t v) { int32_t o = *p; *p = v; return o; }
static void vk_lowlat_swapchain(VkSwapchainKHR sc, int made) { (void)sc, (void)made; }
static int vk_fg_active(void) { return 0; }
static void vk_fg_destroy_swapchain(VkSwapchainKHR sc) { (void)sc; }
static int dropped, unlocked;
static void drop_swapchain_locked(VkCtx *c) { (void)c; dropped++; }
static void vk_queue_unlock(void) { unlocked++; }
%s
int main(int argc, char **argv) {
    logf_ = fopen(argv[1], "a");
    VkCtx c = {0};
    static volatile int32_t n;
    int zero = 0;
    for (int i = 0; i < 20; i++)
        zero += swapchain_unusable(&c, &n, "vkGetSwapchainImagesKHR", VK_INCOMPLETE, "the image list, 3 of 4 images");
    printf("%%d %%d %%d %%d\n", zero, dropped, unlocked, (int)g_dirty);
    return 0;
}
'''

# ---- make_sampler
SAMP = COMMON + r'''
#include <math.h>
#include <vulkan/vulkan.h>
#include "nvn_obj.h"
#include "gfx_guard.h"
typedef struct { VkDevice device; VkPhysicalDeviceProperties props; } VkCtx;
static struct { int init, mirror_clamp_edge, minmax, border_no_format; float aniso; } sf = {1, 1, 1, 1, 16};
static void sampler_features(VkCtx *c) { (void)c; }
static VkCompareOp compare_op(uint32_t f) { (void)f; return VK_COMPARE_OP_LESS_OR_EQUAL; }
static void compare_sampler_note(const samp_par *d) { (void)d; }
static int shadow_scale(void) { return 1; }
static int shadow_pcf(void) { return 0; }
static int32_t plat_inc32(volatile int32_t *p) { return ++*p; }
static int fail_all, calls, plain_calls;
static VkResult vkCreateSampler_(VkDevice d, const VkSamplerCreateInfo *si, const VkAllocationCallbacks *a, VkSampler *s) {
    (void)d, (void)a;
    calls++;
    int plain = !si->pNext && !si->anisotropyEnable;
    plain_calls += plain;
    if (fail_all || !plain) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *s = (VkSampler)(uintptr_t)0x5a;
    return VK_SUCCESS;
}
#define vkCreateSampler vkCreateSampler_
%s
int main(int argc, char **argv) {
    logf_ = fopen(argv[1], "a");
    VkCtx c = {0};
    c.props.limits.maxSamplerLodBias = 16, c.props.limits.maxSamplerAnisotropy = 16;
    samp_par d;
    memset(&d, 0, sizeof d);
    d.ws = d.wt = d.wr = NVN_WRAP_MODE_CLAMP_TO_BORDER;
    d.border[0] = 0.25f, d.border[3] = 1;   // a custom border colour
    d.aniso = 8, d.lod_max = 1000;
    VkSampler a = make_sampler(&c, &d);     // the plain retry makes it
    fail_all = 1;
    VkSampler b = make_sampler(&c, &d);     // nothing makes it
    printf("%%d %%d %%d %%d\n", a != VK_NULL_HANDLE, b == VK_NULL_HANDLE, calls, plain_calls);
    return 0;
}
'''


def build(cc, name, text, work, inc):
    c = work / (name + '.c')
    c.write_text(text)
    exe = work / (name + ('.exe' if os.name == 'nt' else ''))
    r = subprocess.run([cc, '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
                        '-Wno-unused-parameter'] + inc + [str(c), '-o', str(exe), '-lm'],
                       capture_output=True, text=True, errors='replace')
    if r.returncode:
        print(f'log_trace_check: FAIL: {name} does not build:\n{r.stderr[-3000:]}')
        return None
    return exe


def run(exe, args):
    r = subprocess.run([str(exe)] + [str(a) for a in args], capture_output=True, text=True, errors='replace')
    if r.returncode:
        print(f'log_trace_check: FAIL: {exe.name} ended with {r.returncode}:\n{r.stdout}{r.stderr}')
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', help="the C compiler (default: the build's clang)")
    args = ap.parse_args()
    cc = args.cc or compiler()
    work = ROOT / 'build' / 'test' / 'log_trace'
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    log = work / 'nx2saria-sample.log'
    inc = [*runtime_include.flags()]
    vk = ROOT / 'toolchain' / 'vulkan-headers' / 'include'   # build_game.py's pinned headers, when fetched
    if vk.is_dir():
        inc = ['-I', str(vk)] + inc

    pipe, present, tex = source('vk/vk_pipeline.c'), source('gfx/gfx_present.c'), source('vk/vk_texture.c')
    fails = 0
    out = {}
    progs = {
        'stamp': STAMP % function(pipe, 'static void cache_stamp_check(void) {'),
        'gate': GATE % between(pipe, '// ---- the play gate', '// --probe-buildgate'),
        'swap': SWAP % function(present, 'static int swapchain_unusable('),
        'samp': SAMP % function(tex, 'static VkSampler make_sampler(VkCtx *c, const samp_par *d) {'),
    }
    exes = {k: build(cc, k, v, work, inc) for k, v in progs.items()}
    if not all(exes.values()):
        return 1
    with tempfile.TemporaryDirectory() as d:
        out['stamp'] = run(exes['stamp'], [d, log])
    out['gate'] = run(exes['gate'], [log, 600])
    out['gate_off'] = run(exes['gate'], [log, 0])
    out['swap'] = run(exes['swap'], [log])
    out['samp'] = run(exes['samp'], [log])
    text = log.read_text()
    lines = text.splitlines()

    def want(cond, what):
        nonlocal fails
        if not cond:
            fails += 1
            print('log_trace_check: FAIL: ' + what)

    # The stamp: none, matches, differs kept for the carry, differs read while a file is held open, differs deleted
    # (650 MB, both files), matches with the file still due (its run ended before its exit), matches once written, a
    # preparation's deletion (it writes the file itself), off. Only a start in play without pipeline-cache.bin says
    # that its exit writes it.
    st = [ln for ln in lines if 'pipeline cache stamp' in ln]
    due = "pipeline-cache.bin is written at this run's exit; "
    want(len(st) == 9, f'{len(st)} stamp lines, expected 9')
    if len(st) == 9:
        want(re.search(r'\] vk: pipeline cache stamp a0a1a2a3 none: read, stamped for the next start, 650\.0 MB', st[0]),
             'start 1: ' + st[0])
        want(re.search(r'\] vk: pipeline cache stamp a0a1a2a3 matches: read, 650\.0 MB', st[1]), 'start 2: ' + st[1])
        want(re.search(r"\] vk: pipeline cache stamp 11a1a2a3 differs: read for the carried shaders, stamp kept, "
                       r"650\.0 MB \(the files' translator a0a1a2a3; ", st[2]), 'start 3: ' + st[2])
        want(re.search(r"\] vk: pipeline cache stamp 11a1a2a3 differs: read \(not deleted: a file is in use; read, the "
                       r"stamp kept\), 650\.0 MB \(the files' translator a0a1a2a3; ", st[3]), 'start 4: ' + st[3])
        want(re.search(r"\] vk: pipeline cache stamp 11a1a2a3 differs: deleted, starting empty, 650\.0 MB \(the files' "
                       r"translator a0a1a2a3; " + re.escape(due), st[4]), 'start 5: ' + st[4])
        want(re.search(r'\] vk: pipeline cache stamp 11a1a2a3 matches: read, 0\.0 MB \(' + re.escape(due), st[5]),
             'start 6: ' + st[5])
        want(re.search(r'\] vk: pipeline cache stamp 11a1a2a3 matches: read, 30\.0 MB', st[6]), 'start 7: ' + st[6])
        want(re.search(r"\] vk: pipeline cache stamp 22a1a2a3 differs: deleted, starting empty, 30\.0 MB \(the files' "
                       r"translator 11a1a2a3; ", st[7]), 'start 8, a preparation: ' + st[7])
        want('vk: pipeline cache stamp off (NX2SARIA_PIPELINE_CACHE_STAMP 0)' in st[8], 'start 9: ' + st[8])
        for i in (0, 1, 2, 3, 6, 7):
            want(due not in st[i], f'start {i + 1} says its exit writes pipeline-cache.bin: ' + st[i])
    # The files kept while one is held open (start 4), gone at start 5.
    want(out['stamp'].split() == 'held 1 1 drop 0 0'.split(), 'the files: ' + ' '.join(out['stamp'].split()))

    # The play gate: on at 600, a rest only in play; of the 120 heavy frames the first came before the builders began
    # their rest, so 119 count; five builds held back, each once though two builders rested.
    want('menu rest 0\nplay rest 1\npause rest 0\nload rest 0' in out['gate'], 'when it rests: ' + out['gate'])
    want(any('] vk: background builds rest in play: while the frame shown has 600 draws or more' in ln for ln in lines),
         'no start line with 600')
    paused = [ln for ln in lines if 'background builds paused in play' in ln]
    want(len(paused) == 1 and re.search(r'\] vk: background builds paused in play: 119 frames, 5 builds deferred '
                                        r'\(2 rests, 3\.8 s of builder time at rest; NX2SARIA_BG_BUILD_PLAY_DRAWS 600\)',
                                        paused[0] if paused else ''),
         'the paused line: %s' % paused)
    want(any('] vk: background builds rest in play: off (NX2SARIA_BG_BUILD_PLAY_DRAWS 0)' in ln for ln in lines),
         'no off line at 0')
    want('play rest 0' in out['gate_off'], 'the gate at 0 rested')

    # swapchain_unusable: 20 failures, logged at 1-8 and 16.
    sw = [int(m.group(1)) for m in re.finditer(r'\] gfx: vkGetSwapchainImagesKHR failed \(5\), (\d+) times: the image '
                                               r'list, 3 of 4 images; the swapchain is made again', text)]
    want(sw == [1, 2, 3, 4, 5, 6, 7, 8, 16], f'swapchain lines at counts {sw}')
    want(out['swap'].strip() == '0 20 20 1', 'swapchain_unusable: returns, drops, unlocks, dirty: ' + out['swap'].strip())

    # make_sampler: the plain retry made the first; the second fell back. Four calls, two of them plain.
    sm = [ln for ln in lines if 'vkCreateSampler failed' in ln]
    want(len(sm) == 2, f'{len(sm)} sampler lines, expected 2')
    if len(sm) == 2:
        want('] gfx: vkCreateSampler failed (-1), 1 times: made without its custom border colour, reduction and '
             'anisotropy' in sm[0], sm[0])
        want('] gfx: vkCreateSampler failed (-1), 2 times: the default sampler in its place, made again in 250 ms'
             in sm[1], sm[1])
    want(out['samp'].strip() == '1 1 4 2', 'make_sampler: made, fell back, calls, plain calls: ' + out['samp'].strip())

    print(f'log_trace_check: {len(lines)} lines in {log.relative_to(ROOT)}')
    print('log_trace_check: %s' % ('FAILED' if fails else 'passed'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
