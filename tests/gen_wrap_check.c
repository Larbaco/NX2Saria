#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RT_ADDRESS_LIMIT (1ull << 39)
typedef struct { int x; } PlatMutex;
#define PLAT_MUTEX_INIT {0}
static void plat_mutex_lock(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static void plat_mutex_lock_shared(PlatMutex *m) { (void)m; }
static void plat_mutex_unlock_shared(PlatMutex *m) { (void)m; }
static uint64_t qpc(void) { return 0; }
static int rt_trace;
#define RT_TRACE_PERF 1
static void plat_add64(volatile int64_t *p, int64_t v) { *p += v; }
static int64_t plat_xchg64(volatile int64_t *p, int64_t v) { int64_t o = *p; *p = v; return o; }
static struct { uint64_t frames; } gfx_counters;
static uint64_t fake_now, rt_guest_start_ns;
static uint64_t kern_now_ns(void) { return fake_now; }
static void grow(void **arr, int *cap, size_t elem) {
    int n = *cap ? *cap * 2 : 64;
    *arr = realloc(*arr, (size_t)n * elem);
    if (!*arr) exit(70);
    *cap = n;
}
#define SB_PAGE 4096ull

static uint64_t ww_pend[64];
static int nww_pend;
static void cpu_write(uint64_t page) { ww_pend[nww_pend++] = page & ~0xfffull; }
static uint64_t ww_take(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, int reset) {
    uint64_t n = 0;
    for (int i = 0; i < nww_pend && n < max;) {
        if (ww_pend[i] >= addr && ww_pend[i] < addr + size) {
            if (pages) pages[n] = ww_pend[i];
            n++;
            if (reset) {
                ww_pend[i] = ww_pend[--nww_pend];
                continue;
            }
        }
        i++;
    }
    return n;
}
uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return ww_take(addr, size, pages, max, 1);
}
uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return ww_take(addr, size, pages, max, 0);
}
int mem_frozen(uint64_t a) { (void)a; return 0; }
int mem_watched(uint64_t addr, uint64_t size) { (void)addr, (void)size; return 1; }
int nvn_gpu_only(uint64_t a) { (void)a; return 0; }
static const char *cfg_seed;
static const char *cfg_get(const char *k) { return !strcmp(k, "NX2SARIA_GEN_SEED") ? cfg_seed : NULL; }
typedef struct { uintptr_t lo, hi, base; void *buf; } pidx_ent;
typedef struct { int n; } pidx;
static pidx pool_index;
static PlatMutex pool_lock;
static const pidx_ent *pidx_find(const pidx *x, uintptr_t a, uint64_t l) { (void)x, (void)a, (void)l; return NULL; }
enum { POOL_IMPORT = 1 };

static char gen_lines[16][512];
static int n_gen_lines;
static void rt_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strncmp(line, "mem: page generations", 21) && strncmp(line, "mem: NX2SARIA_GEN_SEED", 22)) return;
    if (n_gen_lines < 16) memcpy(gen_lines[n_gen_lines], line, sizeof line);
    n_gen_lines++;
}
static void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(70);
}

#include "gen_section.inc"

typedef __typeof__(gen_now) gen_t;

static int checks, failed;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        checks++;                                                                                                     \
        if (!(cond)) failed++, printf("FAIL line %d: ", __LINE__), printf(__VA_ARGS__), printf("\n");                 \
    } while (0)

#define TEX 0x100000000ull
#define OTHER 0x100200000ull
#define SPIN 0x200000000ull
#define LONE 0x100400000ull

static uint64_t last_now;
static void seen(gen_t now, const char *what) {
    CHECK((uint64_t)now > last_now, "%s: generation %llu after %llu: not newer", what, (unsigned long long)now,
          (unsigned long long)last_now);
    last_now = now;
}

int main(int argc, char **argv) {
    int from_cfg = argc > 4 && !strcmp(argv[1], "--cfg");
    const char *want_line = NULL;
    uint64_t seed;
    if (from_cfg) {
        cfg_seed = strcmp(argv[2], "(unset)") ? argv[2] : NULL;
        seed = strtoull(argv[3], NULL, 0);
        want_line = strcmp(argv[4], "-") ? argv[4] : NULL;
    } else {
        seed = argc > 1 ? strtoull(argv[1], NULL, 0) : (1ull << 32) - 8;
    }
    uint64_t edge = seed + 8;
    gen_t w_tex, w_oth, now, d;
    rt_guest_start_ns = 1000000000ull;
    fake_now = rt_guest_start_ns + 3600ull * 1000000000ull;
    gfx_counters.frames = 216000;
    if (!from_cfg) {
        gen_now = (gen_t)seed;
        CHECK((uint64_t)gen_now == seed, "the seed %#llx does not fit the generations (%u bits)",
              (unsigned long long)seed, (unsigned)(8 * sizeof(gen_t)));
    }
    last_now = (uint64_t)gen_now;

    if (!failed) {
        vk_guest_written(TEX, 0x4000);
        CHECK((uint64_t)gen_now == seed + 1, "%s%s: the first generation is %llu, not %llu",
              from_cfg ? "NX2SARIA_GEN_SEED=" : "seeded directly", from_cfg ? (cfg_seed ? cfg_seed : "(unset)") : "",
              (unsigned long long)gen_now, (unsigned long long)(seed + 1));
    }
    if (failed) {
        printf("gen_wrap_check: seed %#llx: %d of %d checks failed\n", (unsigned long long)seed, failed, checks);
        return 1;
    }
    vk_guest_written(OTHER, 0x4000);
    CHECK(vk_mem_changed(TEX, 0x4000, 0, &w_tex) == 1, "TEX: its load is not seen");
    seen(w_tex, "TEX first check");
    CHECK(vk_mem_changed(OTHER, 0x4000, 0, &w_oth) == 1, "OTHER: its load is not seen");
    seen(w_oth, "OTHER first check");
    uint64_t pre = (uint64_t)gen_now;

    while ((uint64_t)gen_now + 1 < edge) {
        vk_mem_changed(SPIN, 0x1000, 0, &d);
        seen(d, "spin");
    }
    vk_guest_written(LONE, 0x1000);
    gen_t *ls = gen_slot(LONE, 0);
    CHECK(ls && (uint64_t)*ls == edge, "the page written at the boundary is stamped %llu, not %llu",
          ls ? (unsigned long long)*ls : 0ull, (unsigned long long)edge);
    CHECK(vk_mem_changed(LONE, 0x1000, 0, &now) == 1, "the page written at the boundary reads as never written");
    seen(now, "LONE");
    CHECK(vk_mem_changed(LONE, 0x1000, (gen_t)(edge - 1), &now) == 1, "the boundary's write is older than edge - 1");
    seen(now, "LONE again");
    for (int i = 0; i < 6; i++) {
        vk_mem_changed(SPIN, 0x1000, 0, &d);
        seen(d, "spin past");
    }

    cpu_write(TEX);
    CHECK(vk_mem_changed(TEX, 0x4000, w_tex, &now) == 1, "TEX: a CPU write after the boundary is missed");
    seen(now, "TEX after a CPU write");
    w_tex = now;
    vk_guest_written(TEX + 0x1000, 4);
    CHECK(vk_mem_changed(TEX, 0x4000, w_tex, &now) == 1, "TEX: a host write after the boundary is missed");
    seen(now, "TEX after a host write");
    w_tex = now;
    CHECK(vk_mem_changed(TEX, 0x4000, w_tex, &now) == 0, "TEX: no write since its check, but it reads as written");
    seen(now, "TEX unwritten");
    w_tex = now;

    CHECK(vk_mem_changed(OTHER, 0x4000, w_oth, &now) == 0,
          "OTHER: unwritten since before the boundary, reads as written");
    seen(now, "OTHER 1");
    w_oth = now;
    CHECK(vk_mem_changed(OTHER, 0x4000, w_oth, &now) == 0, "OTHER: unwritten, reads as written at the next check too");
    seen(now, "OTHER 2");
    w_oth = now;

    uint32_t ep = 7;
    CHECK(vk_mem_changed_snap(OTHER, 0x4000, w_oth, ep, &now) == 0, "OTHER via the snapshot: reads as written");
    seen(now, "OTHER snap");
    CHECK(vk_mem_unchanged_why_locked(OTHER, 0x4000, w_oth, ep) == 0, "OTHER's region check answers %d, not 0",
          vk_mem_unchanged_why_locked(OTHER, 0x4000, w_oth, ep));
    cpu_write(OTHER + 0x2000);
    ep = 8;
    CHECK(vk_mem_changed_snap(OTHER, 0x4000, w_oth, ep, &now) == 1, "OTHER via the snapshot: a CPU write is missed");
    seen(now, "OTHER snap written");
    CHECK(vk_mem_unchanged_why_locked(OTHER, 0x4000, w_oth, ep) == 3,
          "OTHER's region check answers %d, not 3 (written)",
          vk_mem_unchanged_why_locked(OTHER, 0x4000, w_oth, ep));
    CHECK(vk_mem_unchanged_why_locked(OTHER, 0x4000, now, ep) == 0, "OTHER's region check after its check answers %d",
          vk_mem_unchanged_why_locked(OTHER, 0x4000, now, ep));

    gen_t *gs = gen_slot(TEX + 0x1000, 0), *cs = slot_in(cgen_chunk, TEX + 0x1000, 0);
    CHECK(gs && cs && *gs == *cs && (uint64_t)*cs > edge, "TEX page 1: texture stamp %llu, CPU-side stamp %llu",
          gs ? (unsigned long long)*gs : 0ull, cs ? (unsigned long long)*cs : 0ull);
    CHECK((uint64_t)creg_max[TEX >> REG_SHIFT] > edge && reg_max[TEX >> REG_SHIFT] == creg_max[TEX >> REG_SHIFT],
          "TEX's region: newest stamps %llu / %llu", (unsigned long long)reg_max[TEX >> REG_SHIFT],
          (unsigned long long)creg_max[TEX >> REG_SHIFT]);

    PoolRec p = {.kind = POOL_IMPORT, .import_lo = TEX, .import_hi = TEX + 0x4000};
    p.mgen = (__typeof__(p.mgen))pre, p.sgen = (__typeof__(p.sgen))pre;
    CHECK((uint64_t)p.mgen == pre && (uint64_t)p.sgen == pre, "PoolRec's mgen and sgen do not hold %llu",
          (unsigned long long)pre);
    CHECK(!lazy_clean(TEX, TEX + 0x4000, &p), "lazy_clean: the mirror lacks a CPU write after the boundary, yet clean");
    CHECK(cpu_wrote_since(TEX + 0x1000, p.sgen), "cpu_wrote_since: a CPU write after the boundary is missed");
    CHECK(page_dirty(TEX + 0x1000, p.mgen), "page_dirty: a CPU write after the boundary is missed");
    vk_mem_changed_snap(TEX, 0x4000, w_tex, ep, &now);
    seen(now, "TEX snap");
    CHECK(!shadow_pool_clean(&p, ep, 1), "shadow_pool_clean (CPU-side tables): a write after the boundary is missed");
    CHECK(!shadow_pool_clean(&p, ep, 0), "shadow_pool_clean: a write after the boundary is missed");
    p.mgen = (__typeof__(p.mgen))gen_now, p.sgen = (__typeof__(p.sgen))gen_now;
    CHECK(lazy_clean(TEX, TEX + 0x4000, &p), "lazy_clean: synced after every write, yet not clean");
    CHECK(!cpu_wrote_since(TEX + 0x1000, p.sgen), "cpu_wrote_since: nothing written since the sync, yet written");
    CHECK(!page_dirty(TEX + 0x1000, p.mgen), "page_dirty: nothing written since the sync, yet dirty");
    CHECK(shadow_pool_clean(&p, ep, 1) && shadow_pool_clean(&p, ep, 0), "shadow_pool_clean: synced, yet not clean");

    int reached = seed < (1ull << 32) && (uint64_t)gen_now >= (1ull << 32), n_reached = 0, n_seed = 0;
    for (int i = 0; i < n_gen_lines && i < 16; i++) {
        if (strstr(gen_lines[i], "reached 4294967296")) {
            n_reached++;
            CHECK(strstr(gen_lines[i], "reached 4294967296 at frame 216000 after 1.0 h of play (19884 a frame)"),
                  "the line at 2^32: %s", gen_lines[i]);
        }
        if (strstr(gen_lines[i], "NX2SARIA_GEN_SEED")) {
            n_seed++;
            CHECK(want_line && strstr(gen_lines[i], want_line), "the seed's line is not \"%s\": %s",
                  want_line ? want_line : "(none)", gen_lines[i]);
        }
        CHECK(!strstr(gen_lines[i], "run out") && !strstr(gen_lines[i], "ran out at"), "a line of running out: %s",
              gen_lines[i]);
        printf("  %s\n", gen_lines[i]);
    }
    CHECK(n_reached == reached, "%d lines at 2^32, not %d", n_reached, reached);
    CHECK(n_seed == (want_line != NULL), "%d lines of the seed, not %d", n_seed, want_line != NULL);
    CHECK(n_gen_lines == n_reached + n_seed, "%d page generation lines, not %d", n_gen_lines, n_reached + n_seed);

    printf("gen_wrap_check: seed %#llx, %u-bit generations, gen_now %#llx: %d of %d checks failed\n",
           (unsigned long long)seed, (unsigned)(8 * sizeof(gen_t)), (unsigned long long)gen_now, failed, checks);
    return failed ? 1 : 0;
}
