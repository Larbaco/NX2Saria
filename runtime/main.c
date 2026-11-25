#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "lib_origin.h"
#include "log_privacy.h"
#include "log_stall.h"
#include "logq.h"
#include "prof.h"
#include "session_summary.h"
#include "platform/platform.h"
#include "sha256.h"

#define MAX_MODULES 16

unsigned rt_trace;

#define LOG_KEEP 5
#define LOG_CAP (64ull << 20)
#define LOG_END_RESERVE (4ull << 20)
enum { LOG_EARLY, LOG_FILE, LOG_OFF };
static int g_log_state;
static PlatAppend *g_log_file;
static char g_log_path[PLAT_PATH_MAX * 3];
static char g_log_early[32768];
static size_t g_log_early_n;
static uint64_t g_log_bytes;

static PlatMutex g_err_lock = PLAT_MUTEX_INIT;
static volatile int32_t g_log_final;
static volatile int32_t g_log_ending;
static uint64_t g_log_tail;

void rt_log_ending(void) { __atomic_store_n(&g_log_ending, 1, __ATOMIC_RELEASE); }

static void log_sink(const char *p, size_t n) {
    int file = __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE;
    if (file) {
        uint64_t at = __atomic_fetch_add(&g_log_bytes, n, __ATOMIC_RELAXED);
        if (at + n <= LOG_CAP) {
            plat_append_write(g_log_file, p, n);
        } else {
            if (at <= LOG_CAP) {
                static const char cut[] = "[host] log: the file reached 64 MiB, the rest of this run goes to stderr only, "
                                          "but for the lines of its end\n";
                plat_append_write(g_log_file, cut, sizeof cut - 1);
            }
            if (__atomic_load_n(&g_log_ending, __ATOMIC_ACQUIRE)) {
                uint64_t t = __atomic_fetch_add(&g_log_tail, n, __ATOMIC_RELAXED);
                if (!t) {
                    static const char end[] = "[host] log: the end of the run follows; the lines between the cap and it "
                                              "were not written to this file\n";
                    plat_append_write(g_log_file, end, sizeof end - 1);
                }
                if (t + n <= LOG_END_RESERVE) plat_append_write(g_log_file, p, n);
            }
        }
    }
    if (file && __atomic_load_n(&g_log_final, __ATOMIC_ACQUIRE)) {
        if (!plat_mutex_trylock(&g_err_lock)) return;
    } else {
        plat_mutex_lock(&g_err_lock);
    }
    plat_stderr_write(p, n);
    plat_mutex_unlock(&g_err_lock);
}

void rt_log_sync(void) {
    __atomic_store_n(&g_log_final, 1, __ATOMIC_RELEASE);
    logq_sync();
}

static void log_sync_ending(void) {
    rt_log_ending();
    if (log_privacy_on())
        rt_log("log: %llu lines and values had a personal folder written short this run", log_privacy_count());
    rt_log_sync();
}

static void console_closed(const char *what) {
    rt_log("console: %s; the game ends as when its window is closed", what);
    gfx_window_closed();
}

void rt_log_focus_lost(void) {
    logq_nudge();
    plat_exit_note_modules();
}

static void log_put(const char *p, size_t n) {
    if (__atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_EARLY) {
        fwrite(p, 1, n, stderr);
        size_t at = __atomic_fetch_add(&g_log_early_n, n, __ATOMIC_RELAXED);
        if (at + n <= sizeof g_log_early) memcpy(g_log_early + at, p, n);
        return;
    }
    logq_write(p, n);
}

void rt_log_raw(const char *p, size_t n) {
    if (log_privacy_find(p, n) == n) {
        log_put(p, n);
        return;
    }
    char out[2560];
    for (size_t at = 0; at < n;) {
        size_t o = log_privacy_rewrite(p, n, &at, out, sizeof out);
        if (o) log_put(out, o);
    }
}

void rt_vlogf(const char *fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) rt_log_raw(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[host] ");
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
    buf[n++] = '\n';
    rt_log_raw(buf, n);
}

const char *rt_log_path(void) {
    return __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE ? g_log_path : NULL;
}

static void log_open(int game) {
    logq_init(log_sink);
    char dir[PLAT_PATH_MAX * 3], from[PLAT_PATH_MAX * 3 + 16], to[PLAT_PATH_MAX * 3 + 16];
    char *cut = NULL;
    if (game && !plat_stderr_path(dir, sizeof dir) && plat_exe_path(dir, sizeof dir)) {
        cut = plat_last_sep(dir);
        char *f = strrchr(dir, '/');
        if (f > cut) cut = f;
    }
    if (cut && (size_t)(cut - dir) + sizeof "/logs" <= sizeof dir) {
        strcpy(cut, "/logs");
        plat_file_mkdir_p(dir);
        for (int i = LOG_KEEP - 1; i > 0; i--) {
            if (i > 1) snprintf(from, sizeof from, "%s/nx2saria.%d.log", dir, i - 1);
            else snprintf(from, sizeof from, "%s/nx2saria.log", dir);
            snprintf(to, sizeof to, "%s/nx2saria.%d.log", dir, i);
            if (plat_file_exists(from)) plat_file_replace(from, to, 1);
        }
        snprintf(g_log_path, sizeof g_log_path, "%s/nx2saria.log", dir);
        g_log_file = plat_append_open(g_log_path);
    }
    if (!g_log_file) {
        __atomic_store_n(&g_log_state, LOG_OFF, __ATOMIC_RELEASE);
    } else {
        size_t n = __atomic_load_n(&g_log_early_n, __ATOMIC_RELAXED);
        if (n > sizeof g_log_early) n = sizeof g_log_early;
        plat_append_write(g_log_file, g_log_early, n);
        g_log_bytes = n;
        __atomic_store_n(&g_log_state, LOG_FILE, __ATOMIC_RELEASE);
        rt_log("log: %s", g_log_path);
    }
    if (game && g_log_file) {
        plat_exit_note_setup(g_log_file, logq_peek);
        plat_exit_note_modules();
    }
    char zone[16];
    log_stamp(zone, sizeof zone, 0);
    const char *e = cfg_get("NX2SARIA_LOG_ASYNC");
    if (!game || (e && !strcmp(e, "0"))) return;
    plat_exit_hook = log_sync_ending;
    if (logq_async()) rt_log("log: lines are written by a thread of their own (NX2SARIA_LOG_ASYNC)");
    else rt_log("log: no log thread; every line is written by the thread that logs it");
}

static void parse_trace(void) {
    const char *e = cfg_get("NX2SARIA_TRACE");
    if (!e) return;
    static const struct { const char *name; unsigned bit; } names[] = {
        {"svc", RT_TRACE_SVC}, {"host", RT_TRACE_HOSTCALL}, {"ipc", RT_TRACE_IPC}, {"fs", RT_TRACE_FS},
        {"gfx", RT_TRACE_GFX}, {"perf", RT_TRACE_PERF}, {"sched", RT_TRACE_SCHED}, {"audio", RT_TRACE_AUDIO},
        {"present", RT_TRACE_PRESENT}, {"all", ~0u}};
    for (const char *p = e; *p;) {
        size_t n = strcspn(p, ",");
        int known = 0;
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) rt_trace |= names[i].bit, known = 1;
        if (!known && n) rt_log("NX2SARIA_TRACE: unknown item '%.*s' (svc, host, ipc, fs, gfx, perf, sched, audio, present, all)", (int)n, p);
        p += n + (p[n] == ',');
    }
}

void rt_sha256(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }

static RtConfig cfg;
const RtConfig *rt_config(void) { return &cfg; }

volatile int g_fnprof = 0;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)lr, res_fn_hit(addr, x0); }

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("brk #%#x at %s (lr %s)", imm, ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
}

void rt_udf(Cpu *c, uint64_t pc) {
    char at[128], lr[128], d[128];
    rt_logf("udf at %s (lr %s)\n", ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
    for (int i = 0; i < 31; i++) rt_logf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
    rt_logf("  sp  %016llx\n  pc  %016llx\n", (unsigned long long)c->sp, (unsigned long long)c->pc);
    uint64_t fp = c->x[29];
    for (int n = 0; n < 32 && fp && !(fp & 7) && plat_mem_readable(fp, 16); n++) {
        uint64_t next = LD64(fp), link = LD64(fp + 8);
        rt_logf("  guest frame %2d: %s\n", n, ldr_describe(link, d, sizeof d));
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
    rt_fatal("undefined instruction at %s (lr %s)", ldr_describe(pc, at, sizeof at),
             ldr_describe(c->x[30], lr, sizeof lr));
}

static PlatScale tick_scale;

__attribute__((constructor)) static void tick_scale_init(void) { tick_scale = plat_scale(19200000u, plat_counter_hz()); }

uint64_t rt_ticks(void) { return plat_scaled(tick_scale, plat_counter()); }

typedef struct { uintptr_t host; uint64_t guest; } FnRev;
static FnRev *fn_rev;
static uint64_t fn_rev_n;

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const FnRev *)a)->host, y = ((const FnRev *)b)->host;
    return x < y ? -1 : x > y;
}

static uint64_t lifted_function(uintptr_t rip) {
    if (!fn_rev) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        fn_rev = malloc((total ? total : 1) * sizeof *fn_rev);
        if (!fn_rev) return 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++)
                fn_rev[fn_rev_n++] = (FnRev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(fn_rev, fn_rev_n, sizeof *fn_rev, by_host);
    }
    if (!fn_rev_n || rip > fn_rev[fn_rev_n - 1].host) return 0;
    uint64_t lo = 0, hi = fn_rev_n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (fn_rev[mid].host <= rip) lo = mid + 1;
        else hi = mid;
    }
    return lo ? fn_rev[lo - 1].guest : 0;
}

static int readable(uint64_t a, uint64_t n) { return plat_mem_readable(a, n); }

static int foreign_hook(const char *module) {
    char low[160];
    size_t n = 0;
    for (; module && module[n] && n < sizeof low - 1; n++) low[n] = (char)tolower((unsigned char)module[n]);
    low[n] = 0;
    if (strstr(low, "vklayer_khronos")) return 0;
    return strstr(low, "vk_layer") || strstr(low, "vklayer") || strstr(low, "hook") || strstr(low, "overlay") ||
           strstr(low, "reshade") || strstr(low, "obs-vulkan") || strstr(low, "nvspcap");
}

enum { CRASH_NOTE_NONE, CRASH_NOTE_FOREIGN, CRASH_NOTE_RTSS };
static int crash_module_note(const char *module, int on_rtss_thread) {
    return on_rtss_thread ? CRASH_NOTE_RTSS : foreign_hook(module) ? CRASH_NOTE_FOREIGN : CRASH_NOTE_NONE;
}

static volatile int32_t mode_done;
static int mode_rc;

static void crash_handler(const PlatCrash *x) {
    if (!rt_ending_enter(71)) plat_exit(71);
    rt_log_ending();
    if (__atomic_load_n(&mode_done, __ATOMIC_ACQUIRE) && x->module) {
        rt_log("exit: %s (%#x) in %s while the process was ending, after its work was done and saved%s: the run ends "
               "with its own code %d", x->what, x->code, *x->module ? x->module : "code outside every module",
               foreign_hook(x->module) ? " (that library is not part of the game: another program put it there)" : "",
               mode_rc);
        plat_exit(mode_rc);
    }
    vk_pipeline_crash_note();
    rt_log_sync();
    char d[160];
    uintptr_t rip = x->rip;
    rt_logf("FATAL: %s (%#x) at host %p (", x->what, x->code, (void *)rip);
    if (x->module && !*x->module) rt_logf("outside every module)");
    else rt_logf("%s+%#llx)", x->module ? x->module : "nx2saria", (unsigned long long)x->offset);
    if (x->access >= 0)
        rt_logf(", %s address %#llx", x->access == 1 ? "writing" : x->access == 8 ? "executing" : "reading",
                (unsigned long long)x->addr);
    rt_log_raw("\n", 1);
    const char *thread = plat_thread_name();
    int on_rtss = thread && !strcmp(thread, "rtss");
    int module_note = crash_module_note(x->module, on_rtss);
    if (module_note == CRASH_NOTE_FOREIGN)
        rt_logf("  %s is not part of the game: a recording, overlay or capture program put it into the game. Close that "
                "program or turn off its Vulkan capture, then start the game again\n", x->module);
    else if (module_note == CRASH_NOTE_RTSS)
        rt_logf("  the fault is on the thread that hands the frame limit to RivaTuner Statistics Server (rtss_limit.c), "
                "in the game's calls of its library or around them; rtss = 0 in nx2saria.ini keeps the game from calling "
                "it\n");
    const char *note = plat_crash_context_get();
    if (note) rt_logf("  the thread was %s\n", note);
    uint64_t f = x->module ? 0 : lifted_function(rip);
    if (f) rt_logf("  in lifted function f_%llx = %s\n", (unsigned long long)f, ldr_describe(f, d, sizeof d));
    uintptr_t exe = plat_exe_base();
    for (int i = 0; i < x->nframes; i++) {
        uint64_t g = lifted_function(x->frames[i]);
        rt_logf("  host frame %2d: nx2saria+%#llx", i, (unsigned long long)(x->frames[i] - exe));
        if (g) rt_logf(" in lifted function f_%llx", (unsigned long long)g);
        rt_log_raw("\n", 1);
    }
    Cpu *c = kern_cpu();
    if (c) {
        for (int i = 0; i < 31; i++)
            rt_logf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
        rt_logf("  sp  %016llx\n  pc  %016llx  lr %s\n", (unsigned long long)c->sp, (unsigned long long)c->pc,
                ldr_describe(c->x[30], d, sizeof d));
        uint64_t fp = c->x[29];
        for (int n = 0; n < 32 && fp && !(fp & 7) && plat_mem_readable(fp, 16); n++) {
            uint64_t next = LD64(fp), lr = LD64(fp + 8);
            rt_logf("  guest frame %2d: %s\n", n, ldr_describe(lr, d, sizeof d));
            if (next <= fp || next - fp > (64u << 20)) break;
            fp = next;
        }
    } else {
        rt_logf("  not on a guest thread\n");
    }
    char at[16];
    log_stamp(at, sizeof at, 0);
    rt_log("fatal: at %s local time", at);
    rt_fatal_notes();
    kern_free_cores_note();
    fs_save_crash_wait();
    rtss_limit_crash();
    rt_rumble_stop_bounded();
    fflush(stderr);
    prof_sampler_write();
    plat_exit(71);
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

static volatile int host_probe_hits;
static void host_probe(Cpu *c) { host_probe_hits++; c->x[0] = 42; }

#ifndef RT_CPU_LEVEL
#define RT_CPU_LEVEL 3
#endif

static void check_cpu(void) {
    __builtin_cpu_init();
#if RT_CPU_LEVEL >= 3
    int ok = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
    printf("cpu: x86-64-v3 (AVX2, FMA, BMI2) %s\n", ok ? "present" : "MISSING");
    CHECK(ok, "the lifted code is built for x86-64-v3");
#else
    int ok = __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt");
    printf("cpu: x86-64-v2 (SSE4.2, POPCNT) %s\n", ok ? "present" : "MISSING");
    CHECK(ok, "the lifted code is built for x86-64-v2");
#endif
}

static void check_layout(void) {
    const RtLayoutEntry *lay = rt_layout;
    int n = rt_layout_count;
    printf("layout: %d modules\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %-8s base %#llx  size %#10llx  end %#llx\n", lay[i].name, (unsigned long long)lay[i].base,
               (unsigned long long)lay[i].size, (unsigned long long)(lay[i].base + lay[i].size));
        CHECK(lay[i].size > 0, "%s has size 0", lay[i].name);
        CHECK(lay[i].base % 0x10000 == 0, "%s base is not 64 KiB aligned", lay[i].name);
        CHECK(lay[i].base + lay[i].size <= RT_KERNEL_BASE, "%s reaches into the kernel region", lay[i].name);
        if (i) {
            CHECK(lay[i].base > lay[i - 1].base, "%s base is not above %s", lay[i].name, lay[i - 1].name);
            CHECK(lay[i - 1].base + lay[i - 1].size <= lay[i].base, "%s overlaps %s", lay[i - 1].name, lay[i].name);
        }
        uint64_t end;
        int ours = plat_mem_state(lay[i].base, lay[i].base + 1, &end) == PLAT_MEM_RESERVED;
        CHECK(ours, "%s is not inside the reserved module window", lay[i].name);
    }
    const RtGuestLayout *g = mem_layout();
    printf("guest memory: aslr %#llx+%#llx stack %#llx heap %#llx alias %#llx+%#llx kernel %#llx host functions %#llx\n",
           (unsigned long long)g->aslr.base, (unsigned long long)g->aslr.size, (unsigned long long)g->stack.base,
           (unsigned long long)g->heap.base, (unsigned long long)g->alias.base, (unsigned long long)g->alias.size,
           (unsigned long long)g->kernel.base, (unsigned long long)g->hostfn.base);
    CHECK(g->alias.base + g->alias.size <= lay[0].base && g->heap.base + g->heap.size <= g->alias.base,
          "guest regions overlap");
}

static void check_modules(const RtModule *mods, int n) {
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        const RtModule *m = &mods[i];
        const RtLayoutEntry *l = NULL;
        for (int k = 0; k < rt_layout_count; k++)
            if (!strcmp(rt_layout[k].name, m->name)) l = &rt_layout[k];
        CHECK(l && l->base == m->base && l->size == m->size, "%s: linked base/size differ from the layout", m->name);

        uint64_t bad = 0, bad_neg = 0;
        for (uint64_t k = 0; k < m->count; k++) {
            uint64_t a = m->addrs[k];
            if (rt_dispatch_lookup(a) != m->fns[k] && bad++ < 5)
                printf("  FAIL: %s_fn_addrs[%llu] %#llx does not dispatch to its function\n", m->name,
                       (unsigned long long)k, (unsigned long long)a);
            int next_is_start = k + 1 < m->count && m->addrs[k + 1] == a + 4;
            if ((!next_is_start && rt_dispatch_lookup(a + 4)) || rt_dispatch_lookup(a + 2)) bad_neg++;
        }
        if (rt_dispatch_lookup(m->base - 4)) bad_neg++;
        failures += bad > 0;
        CHECK(!bad_neg, "%s: %llu addresses that are not function starts dispatch to a function", m->name,
              (unsigned long long)bad_neg);
        uint64_t lo = m->count ? m->addrs[0] : 0, hi = m->count ? m->addrs[m->count - 1] : 0;
        printf("  %-8s %8llu functions, first %#llx, last %#llx: %s\n", m->name, (unsigned long long)m->count,
               (unsigned long long)lo, (unsigned long long)hi, bad || bad_neg ? "FAIL" : "ok");
        total += m->count;
    }
    printf("  total    %8llu functions\n", (unsigned long long)total);
}

static void bench(const RtModule *mods, int n) {
    uint64_t total = 0, sink = 0;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        for (uint64_t k = 0; k < mods[i].count; k++) sink += (uintptr_t)rt_dispatch_lookup(mods[i].addrs[k]);
        total += mods[i].count;
    }
    double dt = now_s() - t0;
    __asm__ volatile("" ::"r"(sink));
    if (total) printf("lookup: %.1f ns per function start (address order)\n", dt * 1e9 / (double)total);
}

static void check_memory(void) {
    const RtGuestLayout *g = mem_layout();
    RtMemoryInfo q;
    uint64_t heap = 0, v = 0;
    int before = failures;
    CHECK(mem_set_heap_size(0x400000, &heap) == 0 && heap == g->heap.base, "SetHeapSize 4 MiB");
    memset((void *)(uintptr_t)heap, 0xAB, 0x400000);
    mem_query(heap + 0x1000, &q);
    CHECK(q.base == heap && q.size == 0x400000 && q.state == 5 && q.perm == 3, "QueryMemory on the heap");
    CHECK(mem_set_heap_size(0x300000, &heap) != 0, "SetHeapSize accepts a size that is not a 2 MiB multiple");

    uint64_t a = g->alias.base + 0x100000;
    CHECK(mem_map_physical(a, 0x10000) == 0, "MapPhysicalMemory");
    ST64(a + 0x8000, 0x1234);
    CHECK(mem_map_physical(a - 0x4000, 0x20000) == 0 && LD64(a + 0x8000) == 0x1234,
          "MapPhysicalMemory over a mapped range lost its contents");
    mem_query(a, &q);
    CHECK(q.base == a - 0x4000 && q.size == 0x20000 && q.state == 5, "QueryMemory on mapped alias pages");
    mem_info(7, &v);
    CHECK(v == 0x400000 + 0x20000, "GetInfo 7 (used memory) %#llx", (unsigned long long)v);

    uint64_t stack = g->stack.base + 0x100000;
    ST64(a + 8, 0x99);
    CHECK(mem_map(stack, a, 0x4000) == 0 && LD64(stack + 8) == 0x99, "MapMemory alias -> stack did not copy");
    ST64(stack + 0x10, 0x5678);
    mem_query(stack, &q);
    CHECK(q.base == stack && q.size == 0x4000 && q.state == 0xB, "QueryMemory on a moved stack");
    mem_query(a, &q);
    CHECK(q.base == a && q.state == 5 && q.perm == 0, "QueryMemory on a moved stack's source");
    CHECK(mem_unmap_physical(a, 0x4000) != 0, "UnmapPhysicalMemory under a moved stack");
    CHECK(mem_unmap(stack, a, 0x4000) == 0 && LD64(a + 0x10) == 0x5678, "UnmapMemory did not copy the stack back");

    CHECK(mem_unmap_physical(a - 0x4000, 0x20000) == 0, "UnmapPhysicalMemory");
    mem_query(a, &q);
    CHECK(q.state == 0 && q.base == g->alias.base && q.size == g->alias.size, "QueryMemory on the free alias region");
    CHECK(mem_map_physical(a, 0x1000) == 0 && LD64(a + 0x10) == 0 && mem_unmap_physical(a, 0x1000) == 0,
          "remapped physical memory is not zeroed");
    mem_info(6, &v);
    CHECK(mem_map_physical(a, ((v + 0xFFFull) & ~0xFFFull) + 0x1000) == 0x10801,
          "MapPhysicalMemory beyond the pool is not LimitReached");
    mem_query(RT_HOSTFN_BASE + 0x40, &q);
    CHECK(q.state == 0x10 && q.base == RT_HOSTFN_BASE, "QueryMemory on the host-function window");
    mem_query(g->alias.base + g->alias.size + 0x1000, &q);
    CHECK(q.state == 3 && q.base == g->alias.base + g->alias.size, "QueryMemory on host memory");
    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    printf("memory: heap, physical memory, stack move, QueryMemory, GetInfo: %s\n", failures > before ? "FAIL" : "ok");
}

static int selftest(void) {
    static RtModule mods[MAX_MODULES];
    int n = rt_linked_modules(mods, MAX_MODULES);

    check_cpu();
    check_layout();
    uint64_t probe = hostfn_register("<selftest probe>", host_probe);

    double t0 = now_s();
    hostfn_seal();
    double dt = now_s() - t0;
    RtDispatchStats s = rt_dispatch_stats();
    printf("dispatch: %d modules linked, table covers %#llx-%#llx: %llu slots, %.1f MiB reserved, built in %.0f ms\n",
           n, (unsigned long long)s.lo, (unsigned long long)s.hi, (unsigned long long)s.slots,
           (double)s.table_bytes / (1 << 20), dt * 1e3);
    check_modules(mods, n);
    CHECK(s.functions > 0, "no functions linked");
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    uint64_t r = rt_call(c, probe, 1, (uint64_t)7);
    CHECK(rt_dispatch_lookup(probe) && r == 42 && host_probe_hits == 1 && c->x[30] == 0,
          "a registered host function does not dispatch through rt_call");
    CHECK(!rt_dispatch_lookup(probe + 16), "an unregistered host-function address dispatches");
    CHECK(rt_dispatch_lookup(RT_RET_HOST), "the rt_call return sentinel is not registered");
    plat_aligned_free(c);
    bench(mods, n);

    void *k1 = mem_kernel_alloc(0x200, 16), *k2 = mem_kernel_alloc(0x2000, 0x1000);
    CHECK((uintptr_t)k1 - RT_KERNEL_BASE < RT_KERNEL_SIZE && !((uintptr_t)k2 & 0xFFF) && k1 != k2,
          "kernel-region allocation");
    mem_kernel_free(k1, 0x200);
    CHECK(mem_kernel_alloc(0x200, 16) == k1, "a freed kernel-region block is not reused");
    check_memory();
    failures += vk_pipeline_spirv_selftest();

    float used, total, proc;
    plat_mem_info(&used, &total, &proc);
    if (proc >= 0) printf("process: working set %.0f MiB\n", proc);
    printf("selftest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}

static size_t (*volatile probe_strlen)(const char *) = strlen;
static void (*volatile probe_null)(void);

static int probe_crash(int null) {
    if (null) probe_null();
    else printf("%zu\n", probe_strlen((const char *)(uintptr_t)8));
    printf("probe-crash: still running, the crash did not happen\n");
    return 1;
}

#define PROBE_BUSY_THREADS 4
#define PROBE_BUSY_LINES 200
static volatile int32_t probe_busy_lines[PROBE_BUSY_THREADS];

static void probe_busy_main(void *arg) {
    int me = (int)(intptr_t)arg;
    for (int32_t i = 0;; i++) {
        rt_log("probe: busy line %d of thread %d, logged while another thread fails", i, me);
        __atomic_store_n(&probe_busy_lines[me], i + 1, __ATOMIC_RELEASE);
    }
}

static int probe_busy(int fatal) {
    for (int i = 0; i < PROBE_BUSY_THREADS; i++)
        if (!plat_thread_spawn(probe_busy_main, (void *)(intptr_t)i, "probe busy", 0)) {
            printf("probe-busy: thread %d could not be started\n", i);
            return 1;
        }
    for (int i = 0; i < PROBE_BUSY_THREADS; i++)
        while (__atomic_load_n(&probe_busy_lines[i], __ATOMIC_ACQUIRE) < PROBE_BUSY_LINES) plat_sleep_ms(1);
    if (fatal) rt_fatal("probe-fatal-busy: a fatal error while %d threads log", PROBE_BUSY_THREADS);
    printf("%zu\n", probe_strlen((const char *)(uintptr_t)8));
    printf("probe-crash-busy: still running, the crash did not happen\n");
    return 1;
}

static volatile int32_t probe_stuck_held;
static void probe_stuck_main(void *arg) {
    (void)arg;
    plat_mutex_lock(&g_err_lock);
    __atomic_store_n(&probe_stuck_held, 1, __ATOMIC_RELEASE);
    for (;;) plat_sleep_ms(1000);
}

static int probe_crash_stuck(void) {
    if (!plat_thread_spawn(probe_stuck_main, NULL, "probe stuck", 0)) {
        printf("probe-crash-stuck: the thread could not be started\n");
        return 1;
    }
    while (!__atomic_load_n(&probe_stuck_held, __ATOMIC_ACQUIRE)) plat_sleep_ms(1);
    printf("%zu\n", probe_strlen((const char *)(uintptr_t)8));
    printf("probe-crash-stuck: still running, the crash did not happen\n");
    return 1;
}

#define PROBE_EXIT_CODE 3
static void probe_exit_fault(void) { printf("%zu\n", probe_strlen((const char *)(uintptr_t)8)); }

static int probe_crash_exit(void) {
    atexit(probe_exit_fault);
    printf("probe-crash-exit: returning %d; a library then faults while the process ends\n", PROBE_EXIT_CODE);
    return PROBE_EXIT_CODE;
}

static int usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--exefs DIR] [--romfs DIR] [--profile DIR] [--audio-device VALUE] [--monitor VALUE] [--list-audio-devices | --list-monitors | --selftest | --probe-loader | "
                    "--probe-cfg | --probe-dpi | --probe-mods | --probe-kernel | --probe-ipc | --probe-services | --probe-fs | --probe-gfx-abi | "
                    "--probe-nvn | --probe-nvn-tex | --probe-window | --probe-shader | --probe-vkmem | "
                    "--probe-vkexec | --probe-meshfb | --probe-astc | --probe-fsr3 | --probe-fsr3fg | --probe-zstd | --probe-input | --probe-movie | --probe-crash-call | --probe-crash-null | --probe-crash-exit | --probe-crash-busy | --probe-fatal-busy | --probe-crash-stuck | --probe-discord | "
                    "--prepare-shaders | --warm-shaders | --prepare-pipelines | --probe-prewarm | --probe-buildgate | --probe-ffx-cache | --probe-memneed | --probe-devlost | --boot |--seconds S [--shot PNG]]\n", argv0);
    return 2;
}

static char pp_exefs[PLAT_PATH_MAX * 2], pp_romfs[PLAT_PATH_MAX * 2], pp_profile[PLAT_PATH_MAX * 2];

static const char *portable_path(const char *dir, const char *var, const char *sub, const char *must, char *out, size_t cap,
                                 const char *fallback) {
    const char *e = cfg_get(var);
    if (e && *e) {
        int abs_path = e[0] == '\\' || e[0] == '/' || e[1] == ':';
        if (abs_path) snprintf(out, cap, "%s", e);
        else snprintf(out, cap, "%s/%s", dir, e);
        return out;
    }
    char probe[PLAT_PATH_MAX * 3];
    snprintf(out, cap, "%s/%s", dir, sub);
    if (!must) return plat_dir_exists(out) ? out : fallback;
    snprintf(probe, sizeof probe, "%s/%s", out, must);
    return plat_file_exists(probe) ? out : fallback;
}

static void portable_paths(RtConfig *c) {
    char exe[PLAT_PATH_MAX * 2];
    int n = plat_exe_path(exe, sizeof exe);
    char *cut = n && n < sizeof exe ? plat_last_sep(exe) : NULL;
    if (!cut) return;
    *cut = 0;
    c->exefs = portable_path(exe, "NX2SARIA_EXEFS", "exefs", "main.npdm", pp_exefs, sizeof pp_exefs, c->exefs);
    c->romfs = portable_path(exe, "NX2SARIA_ROMFS", "romfs", "romfs.bin", pp_romfs, sizeof pp_romfs, c->romfs);
    c->profile = portable_path(exe, "NX2SARIA_PROFILE_DIR", "profile", NULL, pp_profile, sizeof pp_profile, c->profile);
}

static int main_run(int argc, char **argv);

#ifdef _WIN32
#define SDL_LIBRARY "SDL3.dll"
#else
#define SDL_LIBRARY "libSDL3.so.0"
#endif

static void log_libs(void) {
    char sdl[PLAT_PATH_MAX * 2], exe[PLAT_PATH_MAX * 2], line[2048];
    int built, loaded = input_sdl_version(&built);
    if (!plat_library_path(SDL_LIBRARY, sdl, sizeof sdl)) sdl[0] = 0;
    if (!plat_exe_path(exe, sizeof exe)) exe[0] = 0;
    lib_origin_line(line, sizeof line, loaded, built, sdl, exe, sha256_ni());
    rt_log("libs: %s", line);
}

int main(int argc, char **argv) {
    plat_stack_guarantee();
    mode_rc = main_run(argc, argv);
    __atomic_store_n(&mode_done, 1, __ATOMIC_RELEASE);
    plat_exit_orderly();
    return mode_rc;
}

static int game_run(int argc, char **argv) {
    static const char *const with_value[] = {"--exefs", "--romfs", "--profile", "--audio-device", "--monitor",
                                             "--seconds", "--shot"};
    for (int i = 1; i < argc; i += 2) {
        int option = 0;
        for (size_t k = 0; k < sizeof with_value / sizeof *with_value && !option; k++) option = !strcmp(argv[i], with_value[k]);
        if (!option) return 0;
    }
    return 1;
}

#if defined(__linux__) && defined(__has_feature)
#if __has_feature(thread_sanitizer)
static volatile int tsan_probe_value;
static void tsan_probe_writer(void *arg) { (void)arg; tsan_probe_value = 1; }
static void tsan_probe(void) {
    const char *on = getenv("NX2SARIA_TSAN_PROBE");
    rt_log("tsan: deliberate race probe %s", on && !strcmp(on, "1") ? "on" : "off");
    if (!on || strcmp(on, "1")) return;
    PlatThread *a = plat_thread_new(tsan_probe_writer, NULL, "tsan-probe-a");
    PlatThread *b = plat_thread_new(tsan_probe_writer, NULL, "tsan-probe-b");
    if (a) plat_thread_join(a);
    if (b) plat_thread_join(b);
    if (!a || !b) rt_fatal("tsan: could not start both probe threads");
}
#define TSAN_PROBE() tsan_probe()
#endif
#endif
#ifndef TSAN_PROBE
#define TSAN_PROBE() ((void)0)
#endif

static int main_run(int argc, char **argv) {
    log_privacy_init();
    mem_init(game_run(argc, argv));
    log_privacy_start();
    TSAN_PROBE();
    rt_log("version: NX2Saria %s, game %d.%d.%d", rt_port_version, rt_game_version / 100, rt_game_version / 10 % 10,
           rt_game_version % 10);
    log_libs();
    if (plat_restart_wait(30000) < 0) rt_log("restart: the previous process still runs after 30 s; starting anyway");
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    plat_crash_hook(crash_handler);
    parse_trace();
    prof_start();
    if (cfg_bool("NX2SARIA_TIMER_1MS")) plat_timer_1ms();
    int prepare = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--prepare-shaders") || !strcmp(argv[i], "--warm-shaders") ||
            !strcmp(argv[i], "--prepare-pipelines")) prepare = 1;
    if (!prepare) perf_cpu_start();
    {
        const char *pf = cfg_get("NX2SARIA_POWER_FULL");
        if (!(pf && !strcmp(pf, "0"))) {
            int got = plat_power_full();
            rt_log("power: %s", got == 2 ? "full speed and the 1 ms timer kept when the window is not in front"
                               : got == 1 ? "full speed kept when the window is not in front (no timer control here)"
                                          : "the OS throttling of an unfocused window stays as it is");
        }
    }
    cfg = (RtConfig){.exefs = rt_default_exefs, .romfs = rt_default_romfs, .profile = rt_default_profile,
                     .gpu = cfg_get("NX2SARIA_GPU")};
    portable_paths(&cfg);
    if (cfg.gpu && strcmp(cfg.gpu, "null") && strcmp(cfg.gpu, "vulkan")) {
        rt_logf("NX2SARIA_GPU must be null or vulkan, not '%s'\n", cfg.gpu);
        return 2;
    }
    const char *mode = NULL;
    for (int i = 1; i < argc; i++) {
        const char **path = !strcmp(argv[i], "--exefs") ? &cfg.exefs : !strcmp(argv[i], "--romfs") ? &cfg.romfs
                          : !strcmp(argv[i], "--profile") ? &cfg.profile : NULL;
        if (path) {
            if (++i == argc) return usage(argv[0]);
            *path = argv[i];
        } else if (!strcmp(argv[i], "--audio-device")) {
            if (++i == argc || !*argv[i]) return usage(argv[0]);
            cfg_setenv("NX2SARIA_AUDIO_DEVICE", argv[i]);
        } else if (!strcmp(argv[i], "--monitor")) {
            if (++i == argc || !*argv[i]) return usage(argv[0]);
            cfg_setenv("NX2SARIA_MONITOR", argv[i]);
        } else if (!strcmp(argv[i], "--seconds")) {
            char *end;
            if (++i == argc) return usage(argv[0]);
            double s = strtod(argv[i], &end);
            if (*end || !(s > 0) || s > 1e6) return usage(argv[0]);
            cfg.seconds = s;
        } else if (!strcmp(argv[i], "--shot")) {
            if (++i == argc) return usage(argv[0]);
            cfg.shot = argv[i];
        } else if (!mode && !strncmp(argv[i], "--", 2)) {
            mode = argv[i];
        } else {
            return usage(argv[0]);
        }
    }
    if (cfg.shot && !cfg.seconds) return usage(argv[0]);
    if (cfg.seconds && mode) return usage(argv[0]);
    log_open(!mode);
    if (!mode) {
        if (plat_console_setup(console_closed))
            rt_log("console: QuickEdit off; closing the console ends the game as its window does");
        mem_start_note(0);
        session_summary_start();
        hang_watch_start();
        cfg.boot = cfg.seconds > 0;
        vk_driver_cache_env();
        discord_init();
        tracker_api_init();
        rt_boot();
        return 0;
    }
    if (!strcmp(mode, "--selftest")) return selftest();
    if (!strcmp(mode, "--list-audio-devices")) return list_audio_devices();
    if (!strcmp(mode, "--list-monitors")) return list_monitors();
    if (!strcmp(mode, "--probe-loader")) return probe_loader();
    if (!strcmp(mode, "--probe-crash-call")) return probe_crash(0);
    if (!strcmp(mode, "--probe-crash-null")) return probe_crash(1);
    if (!strcmp(mode, "--probe-crash-exit")) return probe_crash_exit();
    if (!strcmp(mode, "--probe-crash-busy")) return probe_busy(0);
    if (!strcmp(mode, "--probe-fatal-busy")) return probe_busy(1);
    if (!strcmp(mode, "--probe-crash-stuck")) return probe_crash_stuck();
    if (!strcmp(mode, "--probe-cfg")) return probe_cfg();
#ifdef _WIN32
    if (!strcmp(mode, "--probe-dpi")) return probe_dpi();
#endif
    if (!strcmp(mode, "--probe-mods")) return probe_mods();
    if (!strcmp(mode, "--probe-discord")) return probe_discord();
    if (!strcmp(mode, "--probe-kernel")) return probe_kernel();
    if (!strcmp(mode, "--probe-ipc")) return probe_ipc();
    if (!strcmp(mode, "--probe-services")) return probe_services();
    if (!strcmp(mode, "--probe-fs")) return probe_fs();
    if (!strcmp(mode, "--probe-gfx-abi")) return probe_gfx_abi();
    if (!strcmp(mode, "--probe-nvn")) return probe_nvn();
    if (!strcmp(mode, "--probe-nvn-tex")) return probe_nvn_tex();
    if (!strcmp(mode, "--probe-window")) return probe_window();
    if (!strcmp(mode, "--probe-shader")) return probe_shader();
    if (!strcmp(mode, "--probe-vkmem")) return probe_vkmem();
    if (!strcmp(mode, "--probe-vkexec")) return probe_vkexec();
    if (!strcmp(mode, "--probe-meshfb")) return probe_meshfb();
    if (!strcmp(mode, "--probe-astc")) return probe_astc();
    if (!strcmp(mode, "--probe-fsr3")) return probe_fsr3();
    if (!strcmp(mode, "--probe-fsr3fg")) return probe_fsr3fg();
    if (!strcmp(mode, "--probe-zstd")) return probe_zstd();
    if (!strcmp(mode, "--probe-input")) return probe_gamepad();
    if (!strcmp(mode, "--probe-vkdevice")) return probe_vkdevice();
    if (!strcmp(mode, "--probe-devlost")) return vk_sync_probe_devlost();
    if (!strcmp(mode, "--probe-movie")) {
        cfg.gpu = "null";
        return probe_movie();
    }
    if (!strcmp(mode, "--prepare-shaders")) return vk_pipeline_prepare_shaders();
    if (!strcmp(mode, "--warm-shaders")) return vk_pipeline_warm_shaders();
    if (!strcmp(mode, "--prepare-pipelines")) return vk_pipeline_prepare_pipelines();
    if (!strcmp(mode, "--probe-prewarm")) return vk_pipeline_probe_prewarm();
    if (!strcmp(mode, "--probe-buildgate")) return vk_pipeline_probe_buildgate();
    if (!strcmp(mode, "--probe-ffx-cache")) return vk_ffx_probe_cache();
    if (!strcmp(mode, "--probe-memneed")) { mem_start_note(1); return 0; }
    if (!strcmp(mode, "--boot")) { mem_start_note(0); cfg.boot = 1; rt_boot(); return 0; }
    return usage(argv[0]);
}
