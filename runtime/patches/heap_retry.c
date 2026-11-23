#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "heap_retry.h"
#include "host.h"
#include "layout.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

enum { HEAP_NAME = 0x20, HEAP_START = 0x28, HEAP_SIZE = 0x30, HEAP_PARENT = 0x38, VT_TRY_ALLOC = 0x30 };
enum { MAX_CLIMB = 8, LOG_BUDGET = 32, LOG_EVERY = 64 };
#define CPU_RET_C8 0x436914ull
#define CPU_RET_A8 0x436db8ull
#define CPU_CALL_C8 0x43690cull
#define CPU_CALL_A8 0x436da0ull
#define GPU_RET_A 0x436f1cull
#define GPU_RET_B 0x437098ull
#define GPU_ALLOC 0x502050ull
#define REQUEST_SLOT 0x38
#define HOST_CPU 0xed0
#define HOST_GPU 0xec0

enum { SITE_C8, SITE_A8, SITE_GPU, NSITES };
static const char *const site_names[NSITES] = {"model buffer", "model buffer", "GPU memory"};
static const char *const site_places[NSITES] = {"main+0x43690c", "main+0x436da0", "main+0x436f18 and main+0x437094"};

static int set_index = -1;
static int shipped;
static _Atomic long test_left[2];
static _Atomic long moved_n[NSITES], moved_bytes[NSITES];
static _Atomic long full_n, test_n, kept_n;
static _Atomic int told;

static uint64_t ld64(uint64_t a) { return a && mem_readable(a, 8) ? *(const uint64_t *)(uintptr_t)a : 0; }
static uint64_t own64(uint64_t a) { return *(const uint64_t *)(uintptr_t)a; }

static int test_refuse(int gpu) {
    long n = atomic_load(&test_left[gpu]);
    while (n > 0)
        if (atomic_compare_exchange_weak(&test_left[gpu], &n, n - 1)) return 1;
    return 0;
}

static int heap_usable(uint64_t h) { return h && mem_readable(h, HEAP_PARENT + 8) && ld64(ld64(h) + VT_TRY_ALLOC); }

static uint64_t parent_of(uint64_t h) {
    uint64_t p = ld64(h + HEAP_PARENT);
    if (!p || p == h || !heap_usable(p)) return 0;
    uint64_t hs = ld64(h + HEAP_START), hn = ld64(h + HEAP_SIZE), ps = ld64(p + HEAP_START), pn = ld64(p + HEAP_SIZE);
    if (hs < ps || hs - ps > pn || hn > pn - (hs - ps)) {
        static _Atomic int said;
        if (!atomic_exchange(&said, 1))
            rt_log("heap retry: heap %#llx names %#llx as its parent, whose memory (%#llx, %#llx bytes) does not hold its "
                   "own (%#llx, %#llx bytes): the climb stops there", (unsigned long long)h, (unsigned long long)p,
                   (unsigned long long)ps, (unsigned long long)pn, (unsigned long long)hs, (unsigned long long)hn);
        return 0;
    }
    return p;
}

static void describe(uint64_t h, char *out, size_t n) {
    char name[64];
    uint64_t s = ld64(h + HEAP_NAME);
    size_t i = 0;
    for (; s && i < sizeof name - 1 && mem_readable(s + i, 1); i++) {
        char ch = *(const char *)(uintptr_t)(s + i);
        if (!ch) break;
        if (ch < 32 || ch > 126) { i = 0; break; }
        name[i] = ch;
    }
    if (!i) name[i++] = '?';
    name[i] = 0;
    snprintf(out, n, "%#llx \"%s\" (%llu KB)", (unsigned long long)h, name,
             (unsigned long long)(ld64(h + HEAP_SIZE) >> 10));
}

static int may_tell(void) { return atomic_fetch_add(&told, 1) < LOG_BUDGET; }

static void note_moved(int site, uint64_t at, uint64_t heap, uint64_t from, uint64_t bytes, int up, int test) {
    long n = atomic_fetch_add(&moved_n[site], 1) + 1;
    long b = atomic_fetch_add(&moved_bytes[site], (long)bytes) + (long)bytes;
    if (may_tell()) {
        char was[160], got[160];
        describe(heap, was, sizeof was);
        if (from) describe(from, got, sizeof got);
        else snprintf(got, sizeof got, "the game's default heap");
        rt_log("heap retry: %s at main+%#llx, %llu bytes: heap %s refused it%s; given by %s, %d level%s up; "
               "so far %ld blocks, %ld KB from other heaps at this site", site_names[site], (unsigned long long)at,
               (unsigned long long)bytes, was, test ? " (test: NX2SARIA_HEAP_RETRY_TEST_FAIL)" : "", got, up,
               up == 1 ? "" : "s", n, b >> 10);
    } else if (n % LOG_EVERY == 0) {
        rt_log("heap retry: %s at %s: %ld blocks, %ld KB from other heaps so far", site_names[site], site_places[site],
               n, b >> 10);
    }
}

static void note_kept(int site, uint64_t at, uint64_t heap, uint64_t bytes) {
    atomic_fetch_add(&kept_n, 1);
    if (!may_tell()) return;
    char was[160];
    describe(heap, was, sizeof was);
    rt_log("heap retry: %s at main+%#llx, %llu bytes: heap %s refused it; not tried again "
           "(NX2SARIA_HEAP_RETRY_TEST_SHIPPED=1): the game goes on as shipped", site_names[site],
           (unsigned long long)at, (unsigned long long)bytes, was);
}

static void count_refusal(int test) { atomic_fetch_add(test ? &test_n : &full_n, 1); }

__attribute__((noreturn)) static void give_up(int site, uint64_t at, uint64_t heap, uint64_t bytes, int up,
                                               const char *then) {
    char was[160];
    describe(heap, was, sizeof was);
    rt_fatal("heap retry: the game ran out of its own memory while loading a model: no heap had room for the %s at "
             "main+%#llx, %llu bytes (heap %s and %d other heap%s tried), so the game cannot go on (%s). This memory is "
             "the game's own, of a fixed size: closing other programs or lowering graphics settings does not change "
             "it. Start the game again and load the save; if it stops at the same place again, report it with "
             "logs/nx2saria.log and the save", site_names[site], (unsigned long long)at, (unsigned long long)bytes,
             was, up, up == 1 ? "" : "s", then);
}

static void heap_retry_cpu(Cpu *c) {
    const uint64_t at = c->x[30] - MOD_MAIN_BASE;
    const int site = at == CPU_RET_C8 ? SITE_C8 : at == CPU_RET_A8 ? SITE_A8 : -1;
    if (site < 0) return;
    int test = 0;
    if (c->x[0]) {
        if (!test_refuse(0)) return;
        test = 1;
    }
    const uint64_t bytes = c->x[site == SITE_C8 ? 21 : 22], call = site == SITE_C8 ? CPU_CALL_C8 : CPU_CALL_A8;
    const uint64_t req = own64(c->sp + REQUEST_SLOT), heap = req ? own64(req + 8) : 0;
    count_refusal(test);
    c->x[0] = 0;
    if (shipped) {
        note_kept(site, call, heap, bytes);
        return;
    }
    uint64_t got = 0, h = heap;
    int up = 0;
    while (!got && up < MAX_CLIMB && (h = parent_of(h)) != 0) {
        up++;
        got = rt_call(c, ld64(ld64(h) + VT_TRY_ALLOC), 3, h, bytes, (uint64_t)8);
    }
    if (!got)
        give_up(site, call, heap, bytes, up, "the material block's buffer would be null, and the game would write to guest "
                                       "address 0x8 a moment later");
    c->x[0] = got;
    note_moved(site, call, heap, h, bytes, up, test);
}

static void heap_retry_gpu(Cpu *c) {
    const uint64_t block = c->x[0], req = c->x[1], one = c->x[2], at = c->x[30] - MOD_MAIN_BASE;
    const uint64_t fn = MOD_MAIN_BASE + GPU_ALLOC;
    const uint64_t heap = req ? own64(req + 8) : 0, bytes = req ? own64(req) * one : 0;
    int test = 0;
    uint64_t r = 0;
    if (heap && bytes && test_refuse(1)) test = 1;
    else r = rt_call(c, fn, 3, block, req, one);
    if ((r & 1) || !heap || (at != GPU_RET_A && at != GPU_RET_B)) {
        c->x[0] = r;
        return;
    }
    count_refusal(test);
    if (shipped) {
        note_kept(SITE_GPU, at - 4, heap, bytes);
        c->x[0] = 0;
        return;
    }
    uint64_t *slot = (uint64_t *)(uintptr_t)(req + 8), h = heap;
    int up = 0;
    while (!(r & 1) && up < MAX_CLIMB && (h = parent_of(h)) != 0) {
        up++;
        *slot = h;
        r = rt_call(c, fn, 3, block, req, one);
    }
    if (!(r & 1)) {
        h = 0, up++;
        *slot = 0;
        r = rt_call(c, fn, 3, block, req, one);
    }
    *slot = heap;
    if (!(r & 1))
        give_up(SITE_GPU, at - 4, heap, bytes, up, "the material block would have no memory pool, and the game would write "
                                           "through a null pointer");
    c->x[0] = r;
    note_moved(SITE_GPU, at - 4, heap, h, bytes, up, test);
}

static const struct { uint32_t off, word; } site_words[] = {
    {0x436910, 0xa90d577f}, {0x436db4, 0xa90b5b7f}, {0x436f18, 0x94032c4e}, {0x437094, 0x94032bef},
};

void heap_retry_start(void) {
    set_index = rt_patch_set("heap_retry");
    if (set_index < 0) {
        rt_log("heap retry: not in this game version (the patch set heap_retry is 1.1.1's)");
        return;
    }
    for (size_t i = 0; i < sizeof site_words / sizeof *site_words; i++) {
        uint64_t a = MOD_MAIN_BASE + site_words[i].off;
        if (!mem_readable(a, 4)) continue;
        uint32_t w = *(const uint32_t *)(uintptr_t)a;
        if (w != site_words[i].word && rt_patch_enabled(set_index)) {
            rt_patch_switch(set_index, 0);
            rt_log("heap retry: main+%#x holds %08x where 1.1.1 has %08x: the set heap_retry stays off and the game "
                   "runs its own code there", (unsigned)site_words[i].off, w, (unsigned)site_words[i].word);
        }
    }
    const char *f = cfg_get("NX2SARIA_HEAP_RETRY_TEST_FAIL"), *comma = f ? strchr(f, ',') : NULL;
    long cpu = f && *f ? atol(f) : 0, gpu = comma ? atol(comma + 1) : 0;
    const char *s = cfg_get("NX2SARIA_HEAP_RETRY_TEST_SHIPPED");
    shipped = s && *s && strcmp(s, "0") != 0;
    atomic_store(&test_left[0], cpu > 0 ? cpu : 0);
    atomic_store(&test_left[1], gpu > 0 ? gpu : 0);
    char test[160] = "";
    if (cpu > 0 || gpu > 0 || shipped)
        snprintf(test, sizeof test, "; test: the first %ld CPU buffers and %ld GPU memories refused%s", cpu > 0 ? cpu : 0,
                 gpu > 0 ? gpu : 0, shipped ? ", then the game goes on as shipped" : "");
    if (rt_patch_enabled(set_index))
        rt_log("heap retry: on: a model's buffer or GPU memory that its full heap refuses is taken from the heap's "
               "parents (patch set heap_retry; NX2SARIA_HEAP_RETRY=0 turns it off)%s", test);
    else
        rt_log("heap retry: off (NX2SARIA_HEAP_RETRY=0): a full model heap crashes the game as shipped%s", test);
}

void heap_retry_exit_log(void) {
    if (set_index < 0) return;
    rt_log("heap retry: this session (the set %s at the end) %ld model buffers (%ld KB) and %ld GPU memories (%ld KB) taken from other heaps; "
           "%ld refusals by a full heap, %ld by the test switch, %ld left as shipped",
           rt_patch_enabled(set_index) ? "on" : "off",
           atomic_load(&moved_n[SITE_C8]) + atomic_load(&moved_n[SITE_A8]),
           (atomic_load(&moved_bytes[SITE_C8]) + atomic_load(&moved_bytes[SITE_A8])) >> 10,
           atomic_load(&moved_n[SITE_GPU]), atomic_load(&moved_bytes[SITE_GPU]) >> 10, atomic_load(&full_n),
           atomic_load(&test_n), atomic_load(&kept_n));
}

#ifdef RT_PATCH_PAGE
__attribute__((constructor)) static void heap_retry_register(void) {
    if (!RT_V111) return;
    rt_register_host(RT_PATCH_PAGE + HOST_CPU, heap_retry_cpu);
    rt_register_host(RT_PATCH_PAGE + HOST_GPU, heap_retry_gpu);
}
#endif
