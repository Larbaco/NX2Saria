// The model heaps' retry (runtime/heap_retry.c) against stand-in heaps: the set heap_retry's two host functions called
// as the lifted sites call them, with a chain of heaps (a model's full heap, its parents, the root) whose tryAlloc and
// the game's GPU memory allocation are stand-ins that give memory while a heap has room. Checked: the block a parent
// gives, the registers the functions leave, the request's heap put back, a heap whose parent does not hold it, the
// fatal error when no heap has room, the test switches, the log lines and their budget, and the start and end lines.
// Built by tests/heap_retry_check.py, which writes the stand-in layout.h and patches.h. Modes: "main", "test" (the
// test refusals), "shipped" (the refusals left as shipped), "off" (the set switched off) and "no-set" (a build without
// the set).
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t test_main_base = 0x7000000000ull;   // layout.h's MOD_MAIN_BASE: only compared and added, never read
const int rt_game_version = 111;

#include "heap_retry.c"

#define TRY_ALLOC 0x5550000ull   // the stand-in tryAlloc's address (the heaps' vtable +0x30)
#define UNMAPPED 0x1000ull

// ---- the runtime the file calls
static int have_set = 1, set_on = 1;
int rt_patch_set(const char *name) { return have_set && !strcmp(name, "heap_retry") ? 5 : -1; }
int rt_patch_enabled(int set) { return set == 5 && set_on; }
int mem_readable(uint64_t a, uint64_t n) { (void)n; return a > UNMAPPED; }
static const char *env_fail, *env_shipped;
const char *cfg_get(const char *name) {
    if (!strcmp(name, "NX2SARIA_HEAP_RETRY_TEST_FAIL")) return env_fail;
    if (!strcmp(name, "NX2SARIA_HEAP_RETRY_TEST_SHIPPED")) return env_shipped;
    return NULL;
}
static GuestFn cpu_fn, gpu_fn;
void rt_register_host(uint64_t addr, GuestFn fn) {
    if (addr == RT_PATCH_PAGE + 0xed0) cpu_fn = fn;
    else if (addr == RT_PATCH_PAGE + 0xec0) gpu_fn = fn;
    else abort();
}
static int log_lines, retry_lines;
static char logs[64][512], last_log[512];
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    if (log_lines < 64) memcpy(logs[log_lines], last_log, sizeof last_log);
    log_lines++;
    retry_lines += strstr(last_log, "refused it;") != NULL;
}
static jmp_buf fatal_jmp;
static char fatal_msg[512];
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fatal_msg, sizeof fatal_msg, fmt, ap);
    va_end(ap);
    longjmp(fatal_jmp, 1);
}

// ---- stand-in heaps: +0 vtable, +0x20 name, +0x28 memory, +0x30 its size, +0x38 parent; room: what tryAlloc may give
typedef struct { _Alignas(16) uint64_t w[8]; uint64_t room, given, calls; } Heap;
static uint64_t vtable[8] = {0, 0, 0, 0, 0, 0, TRY_ALLOC, 0};
static Heap root, mid, model, other, *default_heap = &root;
static void make(Heap *h, const char *name, uint64_t start, uint64_t size, Heap *parent, uint64_t room) {
    memset(h, 0, sizeof *h);
    h->w[0] = (uintptr_t)vtable, h->w[4] = (uintptr_t)name, h->w[5] = start, h->w[6] = size;
    h->w[7] = (uintptr_t)parent, h->room = room;
}
static void heaps(uint64_t model_room, uint64_t mid_room, uint64_t root_room) {
    make(&root, "RootHeap", 0x100000000ull, 0x40000000ull, NULL, root_room);   // 1 GiB
    make(&mid, "ResourceHeap", 0x110000000ull, 0x1000000ull, &root, mid_room);   // 16 MiB inside it
    make(&model, "Model/Npc_Test.bfres", 0x110800000ull, 0x10000ull, &mid, model_room);   // 64 KiB inside that
    make(&other, "Elsewhere", 0x900000000ull, 0x10000ull, NULL, 0);
}
static uint64_t take(Heap *h, uint64_t bytes) {
    h->calls++;
    if (h->room < bytes) return 0;
    h->room -= bytes;
    h->given++;
    return h->w[5] + 0x100 * h->given;   // an address inside the heap's memory
}
static Heap *as_heap(uint64_t a) { return (Heap *)(uintptr_t)a; }

// the game's GPU memory allocation: the request's +8 heap (the default heap for none), count * size of one bytes;
// returns 1 on success, 0 leaving nothing allocated
static int gpu_calls;
static uint64_t gpu_alloc(uint64_t req, uint64_t one) {
    gpu_calls++;
    uint64_t *r = (uint64_t *)(uintptr_t)req;
    if (!r[0]) return 1;
    Heap *h = r[1] ? as_heap(r[1]) : default_heap;
    return take(h, r[0] * one) ? 1 : 0;
}
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    uint64_t a[8] = {0};
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint64_t);
    va_end(ap);
    uint64_t r;
    if (fn == TRY_ALLOC) {
        assert(n == 3 && a[2] == 8);
        r = take(as_heap(a[0]), a[1]);
    } else if (fn == test_main_base + 0x502050) {
        assert(n == 3);
        r = gpu_alloc(a[1], a[2]);
    } else {
        abort();
    }
    for (int i = 0; i < 19; i++) c->x[i] = 0xdead0000ull + i;   // a call gives up the caller-saved registers
    c->x[0] = r;
    return r;
}

// ---- the sites
static uint64_t frame[0x60], request[4];   // main+0x4366e0's stack (its x1 at +0x38) and that x1 (+8 the heap)
static Cpu cpu_site(uint64_t ret, uint64_t x0, uint64_t size, Heap *heap) {
    Cpu c;
    memset(&c, 0x5a, sizeof c);
    request[1] = (uintptr_t)heap;
    frame[0x38 / 8] = (uintptr_t)request;
    c.sp = (uintptr_t)frame;
    c.x[0] = x0;
    c.x[ret == CPU_RET_C8 ? 21 : 22] = size;
    c.x[30] = test_main_base + ret;
    return c;
}
static uint64_t call_cpu(uint64_t ret, uint64_t x0, uint64_t size, Heap *heap) {
    Cpu c = cpu_site(ret, x0, size, heap), before = c;
    cpu_fn(&c);
    for (int i = 19; i < 31; i++) assert(c.x[i] == before.x[i]);   // callee-saved registers and x30 kept
    assert(c.sp == before.sp);
    return c.x[0];
}
static uint64_t gpu_req[6];   // +0 count, +8 heap, +0x20 what it is for
static uint64_t call_gpu(uint64_t ret, uint64_t count, Heap *heap) {
    Cpu c;
    memset(&c, 0x5a, sizeof c);
    gpu_req[0] = count, gpu_req[1] = (uintptr_t)heap, gpu_req[4] = (uintptr_t)"MatBlock";
    c.x[0] = 0x123456000ull, c.x[1] = (uintptr_t)gpu_req, c.x[2] = 1, c.x[30] = test_main_base + ret;
    Cpu before = c;
    gpu_fn(&c);
    assert(gpu_req[1] == (uintptr_t)heap);   // the request's heap put back
    for (int i = 19; i < 31; i++) assert(c.x[i] == before.x[i]);
    return c.x[0];
}
static int fatal_of(void (*fn)(void)) {
    fatal_msg[0] = 0;
    if (setjmp(fatal_jmp)) return 1;
    fn();
    return 0;
}

static void no_room_cpu(void) { call_cpu(CPU_RET_C8, 0, 0x4000, &model); }
static void no_room_gpu(void) { call_gpu(GPU_RET_A, 0x20000, &model); }
static void foreign_parent(void) { call_cpu(CPU_RET_A8, 0, 0x800, &model); }

static void run_main(void) {
    heap_retry_start();
    assert(cpu_fn && gpu_fn);
    assert(log_lines == 1 && strstr(last_log, "heap retry: on:") && !strstr(last_log, "test:"));

    // a block the model's heap gave: nothing changes, nothing is logged
    heaps(0x10000, 0, 0);
    assert(call_cpu(CPU_RET_C8, 0x110800100ull, 0x4198, &model) == 0x110800100ull);
    assert(log_lines == 1 && mid.calls == 0);

    // the model's heap full, its parent full, the root has room: the root's block, two levels up
    heaps(0, 0, 1 << 20);
    uint64_t got = call_cpu(CPU_RET_C8, 0, 0x4198, &model);
    assert(got == root.w[5] + 0x100 && root.given == 1 && mid.calls == 1 && model.calls == 0);
    assert(strstr(last_log, "heap retry: model buffer at main+0x43690c, 16792 bytes"));
    assert(strstr(last_log, "\"Model/Npc_Test.bfres\" (64 KB) refused it;") && strstr(last_log, "\"RootHeap\""));
    assert(strstr(last_log, "2 levels up") && !strstr(last_log, "test:"));

    // the other CPU site: its size in x22, the parent has room
    heaps(0, 1 << 20, 0);
    got = call_cpu(CPU_RET_A8, 0, 0x800, &model);
    assert(got == mid.w[5] + 0x100 && strstr(last_log, "main+0x436da0") && strstr(last_log, "1 level up"));

    // a return address that is not a site: left alone
    assert(call_cpu(0x400000, 0, 0x800, &model) == 0 && mid.calls == 1);

    // GPU memory: the game's call fails in the model's heap, its parent gives it
    heaps(0, 1 << 20, 0);
    gpu_calls = 0;
    assert(call_gpu(GPU_RET_A, 0x20000, &model) == 1 && gpu_calls == 2 && mid.given == 1);
    assert(strstr(last_log, "GPU memory at main+0x436f18, 131072 bytes") && strstr(last_log, "\"ResourceHeap\""));
    // no parent has room: the game's default heap gives it
    heaps(0, 0, 0);
    make(&other, "DefaultHeap", 0x900000000ull, 0x1000000ull, NULL, 1 << 20);
    default_heap = &other;
    gpu_calls = 0;
    assert(call_gpu(GPU_RET_B, 0x8000, &model) == 1 && gpu_calls == 4 && other.given == 1);
    assert(strstr(last_log, "main+0x437094") && strstr(last_log, "the game's default heap"));
    default_heap = &root;
    // success at once, a request without a heap, a zero count: one call each, nothing logged
    heaps(1 << 20, 0, 0);
    int before = log_lines;
    gpu_calls = 0;
    assert(call_gpu(GPU_RET_A, 0x100, &model) == 1 && gpu_calls == 1);
    heaps(0, 0, 0);
    assert(call_gpu(GPU_RET_A, 0x100, NULL) == 0 && gpu_calls == 2);   // the game's own check after the call
    assert(call_gpu(GPU_RET_A, 0, &model) == 1 && gpu_calls == 3 && log_lines == before);

    // no heap has room: a fatal error naming the heap, in place of the write to guest address 0x8
    heaps(0, 0, 0);
    assert(fatal_of(no_room_cpu));
    assert(strstr(fatal_msg, "no heap had room for the model buffer at main+0x43690c, 16384 bytes"));
    assert(strstr(fatal_msg, "2 other heaps tried") && strstr(fatal_msg, "guest address 0x8"));
    default_heap = &other;
    make(&other, "DefaultHeap", 0x900000000ull, 0x1000000ull, NULL, 0);
    assert(fatal_of(no_room_gpu) && strstr(fatal_msg, "GPU memory at main+0x436f18") && strstr(fatal_msg, "no memory pool"));
    assert(gpu_req[1] == (uintptr_t)&model);   // put back before the fatal error too
    default_heap = &root;

    // a parent whose memory does not hold the child's: the climb stops there (one line), then the fatal error
    heaps(0, 1 << 20, 1 << 20);
    model.w[7] = (uintptr_t)&other;
    make(&other, "Elsewhere", 0x900000000ull, 0x10000ull, NULL, 1 << 20);
    before = log_lines;
    assert(fatal_of(foreign_parent) && other.calls == 0 && strstr(fatal_msg, "0 other heaps tried"));
    assert(log_lines == before + 1 && strstr(last_log, "does not hold its own"));
    assert(fatal_of(foreign_parent) && log_lines == before + 1);   // said once
    // an unreadable parent pointer ends the climb as the root does
    heaps(0, 0, 0);
    model.w[7] = UNMAPPED;
    assert(fatal_of(no_room_cpu) && strstr(fatal_msg, "0 other heaps tried"));

    // the log budget: 32 retry lines in all, then a total every 64th block of a site
    heaps(0, 1 << 30, 0);
    for (int i = 0; i < 200; i++) assert(call_cpu(CPU_RET_C8, 0, 0x10, &model));
    assert(retry_lines == 32);   // 4 retries before, 28 of these
    assert(strstr(last_log, "model buffer at main+0x43690c: 192 blocks"));   // the 64th, 128th and 192nd of the site

    heap_retry_exit_log();
    assert(strstr(last_log, "heap retry: this session (the set on at the end) 202 model buffers") && strstr(last_log, "and 2 GPU memories"));
    assert(strstr(last_log, "209 refusals by a full heap, 0 by the test switch, 0 left as shipped"));
    puts("heap_retry_check: main: parents give the blocks, the registers and the request are kept, no room ends in a "
         "fatal error, the log keeps its budget");
}

static void run_test(void) {
    env_fail = "1,1";
    heap_retry_start();
    assert(strstr(last_log, "heap retry: on:") && strstr(last_log, "test: the first 1 CPU buffers and 1 GPU memories"));
    heaps(1 << 20, 1 << 20, 0);
    // the model's heap gave the block, the test refuses it: the parent gives another (the first stays allocated)
    uint64_t got = call_cpu(CPU_RET_C8, 0x110800100ull, 0x100, &model);
    assert(got == mid.w[5] + 0x100 && strstr(last_log, "(test: NX2SARIA_HEAP_RETRY_TEST_FAIL)"));
    // the refusals used up: the next block is the game's
    assert(call_cpu(CPU_RET_C8, 0x110800200ull, 0x100, &model) == 0x110800200ull);
    // the GPU memory's refusal: the game's call is not made for the model's heap, the parent's is
    gpu_calls = 0;
    assert(call_gpu(GPU_RET_A, 0x100, &model) == 1 && gpu_calls == 1 && model.calls == 0 && mid.given == 2);
    gpu_calls = 0;
    assert(call_gpu(GPU_RET_A, 0x100, &model) == 1 && gpu_calls == 1 && model.given == 1);
    heap_retry_exit_log();
    assert(strstr(last_log, "at the end) 1 model buffers") && strstr(last_log, "0 refusals by a full heap, 2 by the test switch"));
    puts("heap_retry_check: test: NX2SARIA_HEAP_RETRY_TEST_FAIL refuses the first allocations and the retry runs");
}

static void run_shipped(void) {
    env_fail = "1,1", env_shipped = "1";
    heap_retry_start();
    assert(strstr(last_log, "then the game goes on as shipped"));
    heaps(1 << 20, 1 << 20, 0);
    assert(call_cpu(CPU_RET_C8, 0x110800100ull, 0x100, &model) == 0 && mid.calls == 0);
    assert(strstr(last_log, "not tried again (NX2SARIA_HEAP_RETRY_TEST_SHIPPED=1)"));
    assert(call_gpu(GPU_RET_B, 0x100, &model) == 0 && mid.calls == 0);
    heap_retry_exit_log();
    assert(strstr(last_log, "at the end) 0 model buffers") && strstr(last_log, "2 left as shipped"));
    puts("heap_retry_check: shipped: a refused allocation goes on null, as the shipped game does");
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "main";
    if (!strcmp(mode, "no-set")) have_set = 0;
    if (!strcmp(mode, "off")) set_on = 0;
    heap_retry_register();
    if (!strcmp(mode, "main")) run_main();
    else if (!strcmp(mode, "test")) run_test();
    else if (!strcmp(mode, "shipped")) run_shipped();
    else if (!strcmp(mode, "off")) {
        heap_retry_start();
        assert(log_lines == 1 && strstr(last_log, "heap retry: off (NX2SARIA_HEAP_RETRY=0)"));
        puts("heap_retry_check: off: the start line says so");
    } else if (!strcmp(mode, "no-set")) {
        heap_retry_start();
        heap_retry_exit_log();
        assert(log_lines == 1 && strstr(logs[0], "not in this game version"));
        puts("heap_retry_check: no-set: one line, no totals");
    } else {
        return 2;
    }
    return 0;
}
