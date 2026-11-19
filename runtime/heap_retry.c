// Model heaps that run full, in Ocarina of Time 1.1.1 (the patch set heap_retry, recomp/patches-v111.txt).
//
// The game's "OptimizeChangeBank" thread rebuilds a model's material block in main+0x4366e0 (x0 the block, x1 a
// request kept at its [sp + 0x38], whose +8 names the heap for the block's CPU parts). That memory comes from small
// heaps of the model's own, children of larger ones, and none of these allocations is checked:
// - two CPU buffers from the heap's tryAlloc (vtable +0x30, the size in x21 and x22, alignment 8), called at
//   main+0x43690c and main+0x436da0. Each result is the base of a bump allocator at block +0xc8 and +0xa8 (+0xd0 and
//   +0xb0 the bytes used, +0xd8 and +0xb8 the size). A null base makes the bump allocator hand out guest address 0,
//   and the next store, in main+0x42cb30 (called at main+0x436d44), writes guest address 0x8;
// - the GPU memory, main+0x502050 called at main+0x436f18 and main+0x437094 (x0 the memory block, x1 a request: +0
//   the count, +8 the heap, +0x20 the name of what it is for; x2 the size of one). A failure with a heap named goes on
//   with no memory pool. main+0x502050 leaves nothing allocated when it fails, so it can be called again.
// A null result at these four sites is now tried again in the heap's parents up to the root, and the GPU memory last in
// the game's default heap (the one main+0x502050 takes for a request without a heap). The first heap with room gives
// the block. A block goes back where it came from: the material block's destructor (main+0x91f940) frees +0xa8 and
// +0xc8 through main+0x1fdea0, the game's free by address, which looks for the heap whose range holds the block (it
// reads +0x28 and +0x30 below); the GPU memory's release (main+0x5020f4) goes through the same function. This was read
// from the code, not traced in a run. When no heap has room, the game ends with a fatal error that says so, instead of
// the write to guest address 0x8.
//
// A heap here: +0 its vtable, +0x20 its name, +0x28 its memory, +0x30 the memory's size, +0x38 its parent; vtable
// +0x30 tryAlloc(heap, size, alignment). A parent is used only when the child's memory lies inside the parent's, so a
// different layout ends the climb instead of calling through a wrong pointer.
//
// Sites: main+0x436910 and main+0x436db4 keep their store of the size and then call the page's +0xed0
// (heap_retry_cpu), which mends x0 before the function's own store of the base at the next word. main+0x436f18 and
// main+0x437094 call the page's +0xec0 (heap_retry_gpu) in place of main+0x502050, and it calls the game's.
//
// Switches, read once at the start: NX2SARIA_HEAP_RETRY=0 turns the set off (the game as shipped).
// NX2SARIA_HEAP_RETRY_TEST_FAIL=<cpu>[,<gpu>] is a test: the first <cpu> CPU buffers and <gpu> GPU memories at these
// sites are refused, as a full heap would refuse them, so the retry runs. A refused CPU buffer stays allocated in the
// model's heap: the test makes that heap fuller and never frees memory behind the game's back.
// NX2SARIA_HEAP_RETRY_TEST_SHIPPED=1, with it: a refused allocation goes on as the shipped game does, which reproduces
// the crash.
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
// main offsets: the return addresses of the set's calls, and the game's GPU memory allocation
#define CPU_RET_C8 0x436914ull   // after main+0x436910: the buffer at block +0xc8, its size in x21
#define CPU_RET_A8 0x436db8ull   // after main+0x436db4: the buffer at block +0xa8, its size in x22
#define CPU_CALL_C8 0x43690cull  // the game's tryAlloc calls those sites follow
#define CPU_CALL_A8 0x436da0ull
#define GPU_RET_A 0x436f1cull    // after main+0x436f18
#define GPU_RET_B 0x437098ull    // after main+0x437094
#define GPU_ALLOC 0x502050ull
#define REQUEST_SLOT 0x38        // main+0x4366e0 keeps its x1 at [sp + 0x38]
#define HOST_CPU 0xed0           // patch page slots (runtime/patches.c lists the others)
#define HOST_GPU 0xec0

enum { SITE_C8, SITE_A8, SITE_GPU, NSITES };
static const char *const site_names[NSITES] = {"model buffer", "model buffer", "GPU memory"};
static const char *const site_places[NSITES] = {"main+0x43690c", "main+0x436da0", "main+0x436f18 and main+0x437094"};

static int set_index = -1;      // the set heap_retry in this build, -1 when it has none
static int shipped;             // NX2SARIA_HEAP_RETRY_TEST_SHIPPED
static _Atomic long test_left[2];   // NX2SARIA_HEAP_RETRY_TEST_FAIL: refusals left, CPU buffers and GPU memory
static _Atomic long moved_n[NSITES], moved_bytes[NSITES];   // blocks a parent heap gave, per site
static _Atomic long full_n, test_n, kept_n;   // refusals by a full heap, by the test; refusals that went on as shipped
static _Atomic int told;        // retry lines logged

static uint64_t ld64(uint64_t a) { return a && mem_readable(a, 8) ? *(const uint64_t *)(uintptr_t)a : 0; }
// A word the game's own code reads at the site or right after it: the function's stack slot and its request (main+0x436918
// and main+0x43691c load both again; main+0x502050 loads the request's +0 and +8). They are read as they are, without
// mem_readable, which answered 0 for the game thread's stack in a bench run (the stack the game hands CreateThread).
static uint64_t own64(uint64_t a) { return *(const uint64_t *)(uintptr_t)a; }

// 1 while the test switch still refuses this kind (0 the CPU buffers, 1 the GPU memory); the count stops at 0
static int test_refuse(int gpu) {
    long n = atomic_load(&test_left[gpu]);
    while (n > 0)
        if (atomic_compare_exchange_weak(&test_left[gpu], &n, n - 1)) return 1;
    return 0;
}

// A heap the climb may call: readable, with a tryAlloc in its vtable
static int heap_usable(uint64_t h) { return h && mem_readable(h, HEAP_PARENT + 8) && ld64(ld64(h) + VT_TRY_ALLOC); }

// h's parent, or 0 at the root or when the parent's memory does not hold h's
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

// "0x... "name" (N KB)": a guest C string of printable characters, else "?"
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

// A block a parent heap (or with from 0, the game's default heap) gave in place of the full one
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

// A refusal that goes on as shipped (NX2SARIA_HEAP_RETRY_TEST_SHIPPED)
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

// The page's +0xed0, right after the tryAlloc at main+0x43690c or main+0x436da0: x0 its result, which the function
// stores as the buffer's base at the next word. A null one is tried in the heap's parents.
static void heap_retry_cpu(Cpu *c) {
    const uint64_t at = c->x[30] - MOD_MAIN_BASE;
    const int site = at == CPU_RET_C8 ? SITE_C8 : at == CPU_RET_A8 ? SITE_A8 : -1;
    if (site < 0) return;   // (only the set's two sites call the slot)
    int test = 0;
    if (c->x[0]) {
        if (!test_refuse(0)) return;
        test = 1;   // the block stays in the model's heap, as allocated
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

// The page's +0xec0, in place of the call of main+0x502050 at main+0x436f18 or main+0x437094: the game's call, and
// when it fails with a heap named, the same call with each of that heap's parents and then the game's default heap.
static void heap_retry_gpu(Cpu *c) {
    const uint64_t block = c->x[0], req = c->x[1], one = c->x[2], at = c->x[30] - MOD_MAIN_BASE;
    const uint64_t fn = MOD_MAIN_BASE + GPU_ALLOC;
    const uint64_t heap = req ? own64(req + 8) : 0, bytes = req ? own64(req) * one : 0;
    int test = 0;
    uint64_t r = 0;
    if (heap && bytes && test_refuse(1)) test = 1;
    else r = rt_call(c, fn, 3, block, req, one);
    // (a request without a heap already used the default one: the function's own check after the call handles that)
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

// The four sites' words as 1.1.1's main has them (recomp/patches-v111.txt). The generator checked them in the
// executable it lifted; the image loaded now is checked too, so a main that differs keeps the game's own code.
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
        if (!mem_readable(a, 4)) continue;   // (code the runtime keeps unmapped: the generator's check stands)
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
    if (!RT_V111) return;   // the set's sites are in 1.1.1's main only
    rt_register_host(RT_PATCH_PAGE + HOST_CPU, heap_retry_cpu);
    rt_register_host(RT_PATCH_PAGE + HOST_GPU, heap_retry_gpu);
}
#endif
