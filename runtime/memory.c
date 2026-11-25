// Contains code Copyright (c) 2026 MacNativeGames contributors, Copyright (c) 2026 Peiikai (MIT; see
// THIRD_PARTY_NOTICES.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "log_stall.h"
#include "mem_need.h"
#include "platform/platform.h"

#define PAGE 0x1000ull
#define GiB (1ull << 30)
#define LOW_SEARCH_FROM 0x100000000ull
#define HIGH_SEARCH_FROM 0x1000000000ull
#define LOW_SIZE (14 * GiB)
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define HEAP_CAP (12 * GiB)
#define ALIAS_SIZE (64 * GiB)
#define ALIAS_ALIGN (4 * GiB)
#define ALIAS_PAGES (ALIAS_SIZE / PAGE)
#define DEFAULT_POOL rt_app_pool_bytes
#define SYSTEM_RESOURCE_BUDGET (32ull << 20)
#define MAX_STACK_MAPS 4096
#define REEXEC_LIMIT 20

enum { ST_FREE = 0, ST_CODE = 3, ST_CODEDATA = 4, ST_NORMAL = 5, ST_STACK = 0xB, ST_THREADLOCAL = 0xC,
       ST_INACCESSIBLE = 0x10 };
enum { PERM_NONE = 0, PERM_R = 1, PERM_RW = 3, PERM_RX = 5 };
enum { ATTR_PERMISSION_LOCKED = 16 };
#define R_OK 0u
#define R_INVALID_ADDRESS 0xcc01u
#define R_INVALID_SIZE 0xca01u
#define R_INVALID_NEW_PERM 0xd801u
#define R_INVALID_STATE 0xd401u
#define R_INVALID_REGION 0xdc01u
#define R_LIMIT_REACHED 0x10801u

static RtGuestLayout L;
static uint64_t mods_lo, mods_hi;
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t heap_size, alias_bytes;
static int heap_lazy;
static uint64_t heap_lazy_faults, heap_lazy_bytes;
static uint64_t heap_lazy_ns, heap_lazy_max_ns;
static uint8_t *alias_pages;
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[MAX_STACK_MAPS];
static unsigned nstack_maps;
static uint8_t *module_perm[MOD_COUNT];

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int reserve_at(uint64_t at, uint64_t size, int watch) { return plat_mem_reserve(at, size, watch); }

static void commit_failed(uint64_t a, uint64_t n, const char *what, uint32_t error) __attribute__((noreturn));
static void commit_failed(uint64_t a, uint64_t n, const char *what, uint32_t error) {
    char reason[320];
    double proc, used, limit;
    plat_mem_commit_error(error, reason, sizeof reason);
    plat_commit_info(&proc, &used, &limit);
    rt_fatal("memory: commit %s %#llx+%#llx failed (%llu bytes, %s, commit %.0f of %.0f MB in use)", what,
             (unsigned long long)a, (unsigned long long)n, (unsigned long long)n, reason, used, limit);
}

void mem_commit_checked(uint64_t addr, size_t size, const char *what) {
    uint32_t error = 0;
    if (!plat_mem_commit(addr, size, &error)) commit_failed(addr, size, what, error);
}

#define FRZ_SHIFT 21
#define FRZ_SIZE (1ull << FRZ_SHIFT)
#define FRZ_HOLD_MS 5000u
static volatile uint8_t frz[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint32_t thaw_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
#define FRZ_QUICK_MS 60000u
#define FRZ_HOT_MIN_MS 4000u
#define FRZ_HOT_MAX_MS 600000u
static uint32_t frz_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint32_t frz_hot_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint32_t frz_hot_until[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static volatile int64_t frz_hot_holds;
static volatile int32_t nfrz;
static PlatMutex frz_lock = PLAT_MUTEX_INIT;
static volatile int64_t frz_count[4];
#define FRZ_THAWING 2
static int frz_async = 1;
typedef struct { uint8_t n; uint16_t run[16][2]; } FrzRuns;
static FrzRuns *frz_runs[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static PlatMutex vm_lock = PLAT_MUTEX_INIT;
static volatile int64_t frz_async_n;
static volatile int64_t frz_protect_fails;

#define PRB_HITS 64
static volatile uint8_t prb[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static volatile int32_t nprb, prb_on;
static struct { uint64_t addr, ip; uint32_t tid; uint8_t write; } prb_hit[PRB_HITS];
static int prb_nhit;
static uint64_t prb_arms, prb_ends[2];

static int watched(uint64_t addr, uint64_t size) {
    const RtGuestLayout *g = &L;
    return (addr >= g->aslr.base && size <= LOW_SIZE && addr - g->aslr.base <= LOW_SIZE - size) ||
           (addr >= g->alias.base && size <= ALIAS_SIZE && addr - g->alias.base <= ALIAS_SIZE - size);
}

static void unprobe_locked(uint64_t r) {
    if (!prb[r]) return;
    int prot = frz[r] == 1 ? PLAT_PROT_READ : PLAT_PROT_RW;
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, b;
    for (; a < end; a = b)
        if (plat_mem_state(a, end, &b) == PLAT_PROT_NONE && !plat_mem_protect(a, b - a, prot))
            rt_fatal("memory: ending the probe of %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)(b - a));
    __atomic_store_n(&prb[r], 0, __ATOMIC_RELEASE);
    plat_dec32(&nprb);
}

typedef struct { uint64_t lo, hi, h0, h1; int64_t delta; uint8_t stale; } LzSeg;
static PlatMutex lz_lock = PLAT_MUTEX_INIT;
static LzSeg *lzs;
static int lz_n, lz_cap;
static volatile uint8_t lzr[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint32_t lz_hot[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint8_t lz_back[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static volatile int32_t lz_on;
static int lz_want;
static void (*lz_fetch)(const MemLazyCopy *c, int n);
static int (*lz_clean)(uint64_t lo, uint64_t hi, void *ctx);
static uint64_t lz_unclean;
enum { LZ_NO_FROZEN, LZ_NO_HOT, LZ_NO_THAW, LZ_NO_PAGES, LZ_NO_EDGE, LZ_NO_N };
static uint64_t lz_no[LZ_NO_N], lz_no_small, lz_arm_calls;
#define LZ_CHUNK_SHIFT 16
#define LZ_CHUNK (1ull << LZ_CHUNK_SHIFT)
#define LZ_HOTS 1024
#define LZ_HEAVY_N 4
#define LZ_HEAVY_MS 1000
#define LZ_HEAVY_KEEP_MS 60000
typedef struct { uint64_t key; uint32_t until; uint8_t back; } LzHot;
static LzHot lz_hots[LZ_HOTS];
static int lz_nhots;
static uint32_t lz_touch_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint8_t lz_touch_n[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static uint32_t lz_heavy_ms[RT_ADDRESS_LIMIT >> FRZ_SHIFT];
static int lz_chunk;
static uint64_t lz_pieces, lz_piece_bytes, lz_heavy;
static uint64_t lz_foreign;
#define LZ_NOTES 8
static struct { uint64_t addr, lo, hi, out; uint32_t tid; uint8_t write, stale, hold; } lz_note[LZ_NOTES];
static int lz_nnote;
static uint64_t lz_faults[2], lz_other, lz_gaps, lz_lock_wait_max_ns;
static uint64_t lz_fetches, lz_fetch_bytes, lz_fetch_ns, lz_fetch_max_ns, lz_joins, lz_join_max_ns;
static int lz_fly_most;
static volatile uint32_t lz_win_tid;
static volatile uint64_t lz_fetch_total;
static volatile uint32_t lz_fetch_ms;

#define LZ_FLY 64
#define LZ_BATCH 16
typedef struct { uint64_t lo, hi, h0, bytes; uint32_t seq, tid; uint8_t used; } LzFly;
static LzFly lz_fly[LZ_FLY];
static volatile int32_t lz_nfly;
static PlatCond lz_fly_cv = PLAT_COND_INIT;
static int64_t lz_arm_r = -1;
static int lz_unlocked = 1;

static void lazy_flags_locked(uint64_t lo, uint64_t hi) {
    for (uint64_t r = lo >> FRZ_SHIFT; r <= (hi - 1) >> FRZ_SHIFT; r++) {
        uint64_t b0 = r << FRZ_SHIFT, b1 = b0 + FRZ_SIZE;
        int any = (int64_t)r == lz_arm_r;
        for (int j = 0; j < lz_n && !any; j++) any = lzs[j].lo < b1 && lzs[j].hi > b0;
        for (int k = 0; k < LZ_FLY && lz_nfly && !any; k++)
            any = lz_fly[k].used && lz_fly[k].lo < b1 && lz_fly[k].hi > b0;
        __atomic_store_n(&lzr[r], (uint8_t)any, __ATOMIC_RELEASE);
    }
}

static int lazy_fly_find_locked(uint64_t lo, uint64_t hi, uint64_t h0) {
    for (int k = 0; k < LZ_FLY && lz_nfly; k++)
        if (lz_fly[k].used && lz_fly[k].lo < hi && lz_fly[k].hi > lo && (!h0 || lz_fly[k].h0 == h0)) return k;
    return -1;
}

static uint64_t lazy_fly_wait_entry_locked(int k) {
    uint32_t seq = lz_fly[k].seq;
    uint64_t t0 = plat_now_ns();
    while (lz_fly[k].seq == seq) plat_cond_wait_ns(&lz_fly_cv, &lz_lock, 100000000ull);
    return plat_now_ns() - t0;
}

static int lazy_fly_join_locked(uint64_t a) {
    int k = lazy_fly_find_locked(a, a + 1, 0);
    if (k < 0) return 0;
    uint64_t ns = lazy_fly_wait_entry_locked(k);
    lz_joins++;
    if (ns > lz_join_max_ns) lz_join_max_ns = ns;
    return 1;
}

static void lazy_fly_wait_ranges_locked(const uint64_t (*rg)[3], int nr) {
    for (int j = 0; j < nr; j++)
        for (int k; (k = lazy_fly_find_locked(rg[j][0], rg[j][1], rg[j][2])) >= 0;) lazy_fly_wait_entry_locked(k);
}

static void lazy_fly_room_locked(void) {
    while (lz_nfly >= LZ_FLY) plat_cond_wait_ns(&lz_fly_cv, &lz_lock, 100000000ull);
}

static int lazy_fly_start_locked(LzSeg s) {
    int k = 0;
    while (lz_fly[k].used) k++;
    LzFly *f = &lz_fly[k];
    f->lo = s.lo, f->hi = s.hi, f->h0 = s.h0, f->bytes = s.stale ? s.hi - s.lo : 0, f->tid = plat_thread_id();
    f->used = 1;
    lz_nfly++;
    if (lz_nfly > lz_fly_most) lz_fly_most = lz_nfly;
    return k;
}

static void lazy_fly_run(const int *k, const LzSeg *s, int n) {
    MemLazyCopy cp[LZ_BATCH];
    int nc = 0;
    uint64_t bytes = 0, ns = 0;
    for (int i = 0; i < n; i++)
        if (s[i].stale && lz_fetch) {
            cp[nc++] = (MemLazyCopy){s[i].h0, s[i].h1, (uint64_t)((int64_t)s[i].lo + s[i].delta), s[i].hi - s[i].lo};
            bytes += s[i].hi - s[i].lo;
        }
    if (nc) {
        uint64_t t0 = plat_now_ns();
        lz_fetch(cp, nc);
        ns = plat_now_ns() - t0;
        __atomic_store_n(&lz_fetch_ms, (uint32_t)plat_now_ms() | 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&lz_fetch_total, (uint64_t)nc, __ATOMIC_RELAXED);
    }
    for (int i = 0; i < n; i++)
        for (uint64_t a = s[i].lo, b; a < s[i].hi; a = b)
            if (plat_mem_state(a, s[i].hi, &b) == PLAT_PROT_NONE && !plat_mem_protect(a, b - a, PLAT_PROT_RW))
                rt_fatal("memory: giving back the lazily written pages %#llx+%#llx failed", (unsigned long long)a,
                         (unsigned long long)(b - a));
    plat_mutex_lock(&lz_lock);
    for (int i = 0; i < n; i++) lz_fly[k[i]].used = 0, lz_fly[k[i]].seq++;
    lz_nfly -= n;
    for (int i = 0; i < n; i++) lazy_flags_locked(s[i].lo, s[i].hi);
    lz_fetches += (uint64_t)nc, lz_fetch_bytes += bytes, lz_fetch_ns += ns;
    if (ns > lz_fetch_max_ns) lz_fetch_max_ns = ns;
    plat_cond_broadcast(&lz_fly_cv);
    plat_mutex_unlock(&lz_lock);
}

static LzSeg lazy_take_locked(int i) {
    LzSeg s = lzs[i];
    lzs[i] = lzs[--lz_n];
    return s;
}

static void lazy_hold_locked(uint64_t lo, uint64_t hi) {
    uint32_t now = (uint32_t)plat_now_ms();
    for (uint64_t r = lo >> FRZ_SHIFT; r <= (hi - 1) >> FRZ_SHIFT; r++) {
        int again = lz_hot[r] && (int32_t)(now - lz_hot[r]) < 60000;
        uint8_t b = again ? (uint8_t)(lz_back[r] < 32 ? lz_back[r] * 2 : 32) : 1;
        lz_back[r] = b ? b : 1;
        lz_hot[r] = (now + (uint32_t)lz_back[r] * 1000u) | 1;
    }
}

static LzHot *lazy_hot_locked(uint64_t c, int make) {
    uint32_t h = (uint32_t)((c * 0x9e3779b97f4a7c15ull) >> 40);
    LzHot *take = NULL;
    for (int k = 0; k < 8; k++) {
        LzHot *e = &lz_hots[(h + (uint32_t)k) & (LZ_HOTS - 1)];
        if (e->key == c + 1) return e;
        if (!take || (take->key && (!e->key || (int32_t)(e->until - take->until) < 0))) take = e;
    }
    if (!make) return NULL;
    *take = (LzHot){c + 1, 0, 0};
    lz_nhots++;
    return take;
}

static int lazy_chunk_held_locked(uint64_t c, uint32_t now) {
    if (!lz_nhots) return 0;
    const LzHot *e = lazy_hot_locked(c, 0);
    return e && e->until && (int32_t)(e->until - now) > 0;
}

static void lazy_push_locked(LzSeg s) {
    if (lz_n == lz_cap) {
        lz_cap = lz_cap ? lz_cap * 2 : 256;
        lzs = realloc(lzs, sizeof *lzs * (size_t)lz_cap);
        if (!lzs) rt_fatal("memory: out of memory for the lazily written ranges");
    }
    lzs[lz_n++] = s;
}

typedef struct { LzSeg s; uint64_t c0, c1; } LzPiece;

static LzPiece lazy_piece_take_locked(int i, uint64_t a) {
    LzSeg s = lazy_take_locked(i);
    uint64_t c0 = a & ~(LZ_CHUNK - 1), c1 = c0 + LZ_CHUNK;
    if (c0 < s.lo) c0 = s.lo;
    if (c1 > s.hi) c1 = s.hi;
    if (c0 > s.lo) lazy_push_locked((LzSeg){s.lo, c0, s.h0, s.h1, s.delta, s.stale});
    if (c1 < s.hi) lazy_push_locked((LzSeg){c1, s.hi, s.h0, s.h1, s.delta, s.stale});
    return (LzPiece){s, c0, c1};
}

static void lazy_piece_hold_locked(uint64_t a) {
    uint32_t now = (uint32_t)plat_now_ms();
    LzHot *e = lazy_hot_locked(a >> LZ_CHUNK_SHIFT, 1);
    int again = e->until && (int32_t)(now - e->until) < 60000;
    e->back = again ? (uint8_t)(e->back < 32 ? e->back * 2 : 32) : 1;
    if (!e->back) e->back = 1;
    e->until = (now + (uint32_t)e->back * 1000u) | 1;
}

static int lazy_take_batch_locked(const uint64_t (*rg)[3], int nr, int *k, LzSeg *s) {
    int n = 0, max = lz_unlocked ? LZ_BATCH : 1;
    for (int i = lz_n; i-- > 0 && n < max;) {
        int hit = 0;
        for (int j = 0; j < nr && !hit; j++)
            hit = lzs[i].lo < rg[j][1] && lzs[i].hi > rg[j][0] && (!rg[j][2] || lzs[i].h0 == rg[j][2]);
        if (!hit) continue;
        if (lz_nfly >= LZ_FLY) return n ? n : -1;
        s[n] = lazy_take_locked(i);
        k[n] = lazy_fly_start_locked(s[n]);
        n++;
    }
    return n;
}

static void lazy_drop_region_locked(uint64_t r) {
    uint64_t rg[1][3] = {{r << FRZ_SHIFT, (r + 1) << FRZ_SHIFT, 0}};
    for (;;) {
        int k[LZ_BATCH];
        LzSeg s[LZ_BATCH];
        plat_mutex_lock(&lz_lock);
        int n = lazy_take_batch_locked((const uint64_t (*)[3])rg, 1, k, s);
        if (n <= 0) {
            if (n < 0) lazy_fly_room_locked();
            else lazy_fly_wait_ranges_locked((const uint64_t (*)[3])rg, 1);
            plat_mutex_unlock(&lz_lock);
            if (n < 0) continue;
            return;
        }
        plat_mutex_unlock(&lz_lock);
        lz_other += (uint64_t)n;
        lazy_fly_run(k, s, n);
    }
}

static int lazy_covered_locked(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < lz_n; i++)
        if (lzs[i].lo <= lo && lzs[i].hi >= hi) return 1;
    return 0;
}

static int lazy_protect_locked(uint64_t x, uint64_t e, void *ctx) {
    uint64_t run[16][2];
    int k = 0;
    for (uint64_t a = x, b; a < e; a = b) {
        int st = plat_mem_state(a, e, &b);
        if (st == PLAT_PROT_RW) {
            if (k == 16) return 0;
            run[k][0] = a, run[k][1] = b, k++;
        } else if (st != PLAT_PROT_NONE || !lazy_covered_locked(a, b)) return 0;
    }
    for (int i = 0; i < k; i++)
        if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_NONE)) {
            while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
            return 0;
        }
    if (lz_clean && !lz_clean(x, e, ctx)) {
        for (int i = 0; i < k; i++) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
        lz_unclean++;
        return 0;
    }
    return 1;
}

uint64_t mem_lazy_arm(uint64_t a, uint64_t n, uint64_t h0, uint64_t h1, uint64_t off, void *ctx) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED) || a >= RT_ADDRESS_LIMIT || !watched(a, n)) return 0;
    uint64_t lo = (a + PAGE - 1) & ~(PAGE - 1), hi = (a + n) & ~(PAGE - 1), armed = 0;
    int64_t delta = (int64_t)off - (int64_t)a;
    plat_mutex_lock(&frz_lock);
    lz_arm_calls++;
    if (hi <= lo) {
        lz_no[LZ_NO_EDGE] += n, lz_no_small++;
        plat_mutex_unlock(&frz_lock);
        return 0;
    }
    lz_no[LZ_NO_EDGE] += n - (hi - lo);
    if (lazy_covered_locked(lo, hi)) {
        plat_mutex_unlock(&frz_lock);
        return 0;
    }
    uint32_t now = (uint32_t)plat_now_ms();
    for (uint64_t x = lo, e; x < hi; x = e) {
        uint64_t r = x >> FRZ_SHIFT;
        e = (r + 1) << FRZ_SHIFT;
        if (e > hi) e = hi;
        if (lz_nhots) {
            uint64_t c = x >> LZ_CHUNK_SHIFT, y = x;
            while (y < e && lazy_chunk_held_locked(c, now)) y = ++c << LZ_CHUNK_SHIFT;
            if (y > x) {
                if (y > e) y = e;
                lz_no[LZ_NO_HOT] += y - x;
                e = y;
                continue;
            }
            for (c++; c << LZ_CHUNK_SHIFT < e; c++)
                if (lazy_chunk_held_locked(c, now)) {
                    e = c << LZ_CHUNK_SHIFT;
                    break;
                }
        }
        int no = frz[r] ? LZ_NO_FROZEN : prb[r] ? LZ_NO_PAGES : lz_hot[r] && (int32_t)(lz_hot[r] - now) > 0 ? LZ_NO_HOT :
                 thaw_ms[r] && (int32_t)(now - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS ? LZ_NO_THAW : -1;
        if (no < 0) {
            plat_mutex_lock(&lz_lock);
            int busy = lazy_fly_find_locked(x, e, 0) >= 0;
            if (!busy) lz_arm_r = (int64_t)r, __atomic_store_n(&lzr[r], 1, __ATOMIC_RELEASE);
            plat_mutex_unlock(&lz_lock);
            if (busy || !lazy_protect_locked(x, e, ctx)) {
                no = LZ_NO_PAGES;
                if (!busy) {
                    plat_mutex_lock(&lz_lock);
                    lz_arm_r = -1;
                    lazy_flags_locked(x, e);
                    plat_mutex_unlock(&lz_lock);
                }
            }
        }
        if (no >= 0) {
            lz_no[no] += e - x;
            continue;
        }
        uint64_t slo = x, shi = e;
        uint8_t stale = 0;
        plat_mutex_lock(&lz_lock);
        for (int j = 0; j < lz_n; j++)
            if (lzs[j].h0 == h0 && lzs[j].delta == delta && lzs[j].lo <= shi && lzs[j].hi >= slo) {
                if (lzs[j].lo < slo) slo = lzs[j].lo;
                if (lzs[j].hi > shi) shi = lzs[j].hi;
                stale |= lzs[j].stale;
                lzs[j] = lzs[--lz_n];
                j = -1;
            }
        lazy_push_locked((LzSeg){slo, shi, h0, h1, delta, stale});
        lz_arm_r = -1;
        plat_mutex_unlock(&lz_lock);
        armed += e - x;
    }
    plat_mutex_unlock(&frz_lock);
    return armed;
}

static int lazy_foreign_locked(uint64_t a, uint64_t n, uint64_t h0, int64_t delta) {
    int i = lz_n;
    while (i-- > 0 && !(lzs[i].lo < a + n && lzs[i].hi > a && (lzs[i].h0 != h0 || lzs[i].delta != delta))) {}
    return i;
}

int mem_lazy_skip(uint64_t a, uint64_t n, uint64_t h0, int64_t delta, uint64_t out[][2], int max) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED) || !n || a >= RT_ADDRESS_LIMIT || a + n > RT_ADDRESS_LIMIT) return 0;
    int any = 0, k = 0;
    for (uint64_t r = a >> FRZ_SHIFT; r <= (a + n - 1) >> FRZ_SHIFT && !any; r++) any = lzr[r];
    if (!any) return 0;
    plat_mutex_lock(&lz_lock);
    int foreign = lazy_foreign_locked(a, n, h0, delta) >= 0;
    if (foreign) {
        plat_mutex_unlock(&lz_lock);
        plat_mutex_lock(&frz_lock);
        for (;;) {
            plat_mutex_lock(&lz_lock);
            int i = lazy_foreign_locked(a, n, h0, delta), f = -1;
            if (i >= 0 && lz_nfly >= LZ_FLY) {
                lazy_fly_room_locked();
                i = lazy_foreign_locked(a, n, h0, delta);
            }
            LzSeg s;
            if (i >= 0) {
                lzs[i].stale = 0;
                lz_foreign++;
                s = lazy_take_locked(i);
                f = lazy_fly_start_locked(s);
            }
            plat_mutex_unlock(&lz_lock);
            if (i < 0) break;
            lz_other++;
            lazy_fly_run(&f, &s, 1);
        }
        plat_mutex_lock(&lz_lock);
    }
    for (int i = 0; i < lz_n && k < max; i++) {
        uint64_t lo = lzs[i].lo > a ? lzs[i].lo : a, hi = lzs[i].hi < a + n ? lzs[i].hi : a + n;
        if (lo >= hi) continue;
        lzs[i].stale = 1;
        int at = k++;
        for (; at > 0 && out[at - 1][0] > lo; at--) out[at][0] = out[at - 1][0], out[at][1] = out[at - 1][1];
        out[at][0] = lo, out[at][1] = hi;
    }
    plat_mutex_unlock(&lz_lock);
    if (foreign) plat_mutex_unlock(&frz_lock);
    return k;
}

int mem_lazy_touches(uint64_t a, uint64_t n) {
    if (!n) return 0;
    if (a >= RT_ADDRESS_LIMIT || a + n > RT_ADDRESS_LIMIT || __atomic_load_n(&prb_on, __ATOMIC_RELAXED)) return 1;
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return 0;
    int any = 0;
    for (uint64_t r = a >> FRZ_SHIFT; r <= (a + n - 1) >> FRZ_SHIFT && !any; r++) any = __atomic_load_n(&lzr[r], __ATOMIC_ACQUIRE);
    if (!any) return 0;
    int hit = 0;
    plat_mutex_lock(&lz_lock);
    for (int i = 0; i < lz_n && !hit; i++) hit = lzs[i].lo < a + n && lzs[i].hi > a;
    if (!hit) hit = lazy_fly_find_locked(a, a + n, 0) >= 0;
    plat_mutex_unlock(&lz_lock);
    return hit;
}

int mem_lazy_armed(uint64_t a, uint64_t n) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return 0;
    plat_mutex_lock(&lz_lock);
    int ok = lazy_covered_locked(a, a + n);
    plat_mutex_unlock(&lz_lock);
    return ok;
}

int mem_copy_readable(uint8_t *const *dst, const uint64_t (*src)[2], int n, uint8_t *ok) {
    int got = 0;
    plat_mutex_lock(&frz_lock);
    uint64_t heap = L.heap.base, top = heap + __atomic_load_n(&heap_size, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        uint64_t a = src[i][0], len = src[i][1];
        ok[i] = 0;
        if (!len || a >= RT_ADDRESS_LIMIT || a + len > RT_ADDRESS_LIMIT) continue;
        int lazy = heap_lazy && a < top && a + len > heap;
        int quick = !lazy, can = 1;
        for (uint64_t r = a >> FRZ_SHIFT; quick && r <= (a + len - 1) >> FRZ_SHIFT; r++)
            quick = !__atomic_load_n(&lzr[r], __ATOMIC_RELAXED) && !__atomic_load_n(&prb[r], __ATOMIC_RELAXED);
        for (uint64_t x = a, e; !quick && can && x < a + len; x = e) {
            int st = plat_mem_state(x, a + len, &e);
            int untouched = lazy && st == PLAT_MEM_RESERVED && x >= heap && e <= top;
            can = (st == PLAT_PROT_RW || st == PLAT_PROT_READ || untouched) && e > x;
        }
        if (!can) continue;
        if (!lazy) {
            memcpy(dst[i], (const void *)(uintptr_t)a, len);
        } else {
            for (uint64_t x = a, e; x < a + len; x = e) {
                if (plat_mem_state(x, a + len, &e) == PLAT_MEM_RESERVED) memset(dst[i] + (x - a), 0, e - x);
                else memcpy(dst[i] + (x - a), (const void *)(uintptr_t)x, e - x);
            }
        }
        ok[i] = 1, got++;
    }
    plat_mutex_unlock(&frz_lock);
    return got;
}

void mem_lazy_release(const uint64_t (*rg)[3], int nr) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED) || nr <= 0) return;
    for (;;) {
        int k[LZ_BATCH];
        LzSeg s[LZ_BATCH];
        plat_mutex_lock(&frz_lock);
        plat_mutex_lock(&lz_lock);
        int n = lazy_take_batch_locked(rg, nr, k, s);
        if (n <= 0) {
            plat_mutex_unlock(&frz_lock);
            if (n < 0) lazy_fly_room_locked();
            else lazy_fly_wait_ranges_locked(rg, nr);
            plat_mutex_unlock(&lz_lock);
            if (n < 0) continue;
            return;
        }
        plat_mutex_unlock(&lz_lock);
        lz_other += (uint64_t)n;
        if (lz_unlocked) plat_mutex_unlock(&frz_lock);
        lazy_fly_run(k, s, n);
        if (!lz_unlocked) plat_mutex_unlock(&frz_lock);
    }
}

static void lazy_release_regions(uint64_t a, uint64_t n) {
    if (!lz_unlocked || !n || a >= RT_ADDRESS_LIMIT || !__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return;
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    int any = 0;
    for (uint64_t r = r0; r <= r1 && !any; r++) any = __atomic_load_n(&lzr[r], __ATOMIC_ACQUIRE);
    if (!any) return;
    plat_mutex_lock(&frz_lock);
    uint32_t now = (uint32_t)plat_now_ms() | 1;
    for (uint64_t r = r0; r <= r1; r++) thaw_ms[r] = now;
    plat_mutex_unlock(&frz_lock);
    uint64_t rg[1][3] = {{r0 << FRZ_SHIFT, (r1 + 1) << FRZ_SHIFT, 0}};
    mem_lazy_release((const uint64_t (*)[3])rg, 1);
}

int mem_lazy_enable(void (*fetch)(const MemLazyCopy *c, int n), int (*clean)(uint64_t lo, uint64_t hi, void *ctx)) {
    if (!lz_want) return 0;
    lz_fetch = fetch, lz_clean = clean;
    __atomic_store_n(&lz_on, 1, __ATOMIC_RELEASE);
    return 1;
}

void mem_lazy_window(int open) {
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return;
    if (!open) {
        __atomic_store_n(&lz_win_tid, 0, __ATOMIC_RELEASE);
        return;
    }
    plat_mutex_lock(&lz_lock);
    __atomic_store_n(&lz_win_tid, plat_thread_id(), __ATOMIC_RELEASE);
    plat_mutex_unlock(&lz_lock);
}

static int lazy_fault(uint64_t a, int write) {
    if (a >= RT_ADDRESS_LIMIT || !__atomic_load_n(&lzr[a >> FRZ_SHIFT], __ATOMIC_ACQUIRE)) return 0;
    uint32_t me = plat_thread_id();
    if (__atomic_load_n(&lz_nfly, __ATOMIC_ACQUIRE)) {
        plat_mutex_lock(&lz_lock);
        int joined = lazy_fly_join_locked(a);
        plat_mutex_unlock(&lz_lock);
        if (joined) return 1;
    }
    for (int tries = 0;;) {
        uint64_t w0 = plat_now_ns();
        plat_mutex_lock(&frz_lock);
        uint64_t waited = plat_now_ns() - w0;
        if (waited > lz_lock_wait_max_ns) lz_lock_wait_max_ns = waited;
        plat_mutex_lock(&lz_lock);
        int i = 0;
        while (i < lz_n && !(a >= lzs[i].lo && a < lzs[i].hi)) i++;
        if (i == lz_n) {
            if (lazy_fly_find_locked(a, a + 1, 0) >= 0) {
                plat_mutex_unlock(&frz_lock);
                lazy_fly_join_locked(a);
                plat_mutex_unlock(&lz_lock);
                return 1;
            }
            plat_mutex_unlock(&lz_lock);
            uint64_t pg = a & ~(PAGE - 1), end;
            int st = plat_mem_state(pg, pg + PAGE, &end);
            plat_mutex_unlock(&frz_lock);
            return write ? st == PLAT_PROT_RW : st >= PLAT_PROT_READ;
        }
        uint32_t w = __atomic_load_n(&lz_win_tid, __ATOMIC_ACQUIRE);
        if (w && w != me && tries < 400) {
            tries++;
            plat_mutex_unlock(&lz_lock);
            plat_mutex_unlock(&frz_lock);
            plat_wait_ns(50000);
            continue;
        }
        if (lz_nfly >= LZ_FLY) {
            plat_mutex_unlock(&frz_lock);
            lazy_fly_room_locked();
            plat_mutex_unlock(&lz_lock);
            continue;
        }
        if (w) lz_gaps++;
        int k = lz_nnote < LZ_NOTES ? lz_nnote++ : -1;
        if (k >= 0)
            lz_note[k].addr = a, lz_note[k].lo = lzs[i].lo, lz_note[k].hi = lzs[i].hi, lz_note[k].tid = me,
            lz_note[k].write = write != 0, lz_note[k].stale = lzs[i].stale;
        uint64_t r = a >> FRZ_SHIFT, out = lzs[i].hi - lzs[i].lo;
        uint32_t now = (uint32_t)plat_now_ms();
        int reader = lz_heavy_ms[r] && (int32_t)(now - lz_heavy_ms[r]) < LZ_HEAVY_KEEP_MS;
        if (lz_chunk && !reader) {
            if (lz_touch_ms[r] && (int32_t)(now - lz_touch_ms[r]) < LZ_HEAVY_MS) {
                if (++lz_touch_n[r] > LZ_HEAVY_N) reader = 1, lz_heavy++;
            } else {
                lz_touch_ms[r] = now | 1, lz_touch_n[r] = 1;
            }
        }
        if (reader) lz_heavy_ms[r] = now | 1;
        LzSeg s;
        if (lz_chunk && !reader) {
            LzPiece p = lazy_piece_take_locked(i, a);
            s = (LzSeg){p.c0, p.c1, p.s.h0, p.s.h1, p.s.delta, p.s.stale};
            out = p.c1 - p.c0;
            lazy_piece_hold_locked(a);
            lz_pieces++, lz_piece_bytes += out;
            if (k >= 0) lz_note[k].hold = lazy_hot_locked(a >> LZ_CHUNK_SHIFT, 1)->back;
        } else {
            s = lazy_take_locked(i);
            lazy_hold_locked(s.lo, s.hi);
            if (k >= 0) lz_note[k].hold = lz_back[r];
        }
        lz_faults[write != 0]++;
        if (k >= 0) lz_note[k].out = out;
        int f = lazy_fly_start_locked(s);
        plat_mutex_unlock(&lz_lock);
        if (lz_unlocked) plat_mutex_unlock(&frz_lock);
        lazy_fly_run(&f, &s, 1);
        if (!lz_unlocked) plat_mutex_unlock(&frz_lock);
        return 1;
    }
}

int mem_lazy_pick(uint64_t *addr, int max) {
    static int next;
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return 0;
    int k = 0;
    plat_mutex_lock(&frz_lock);
    for (; k < max && k < lz_n; k++) addr[k] = lzs[(next + k) % lz_n].lo;
    next += k;
    plat_mutex_unlock(&frz_lock);
    return k;
}

void mem_lazy_note(char *out, size_t n) {
    *out = 0;
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return;
    uint32_t tid = 0, me = plat_thread_id(), last = __atomic_load_n(&lz_fetch_ms, __ATOMIC_RELAXED);
    uint64_t bytes = 0;
    for (int f = 0; f < LZ_FLY && __atomic_load_n(&lz_nfly, __ATOMIC_RELAXED) && tid != me; f++) {
        uint32_t t = lz_fly[f].tid;
        uint64_t b = lz_fly[f].bytes;
        if (lz_fly[f].used && b && (!tid || t == me)) tid = t, bytes = b;
    }
    int k = snprintf(out, n, "%d ranges armed, %llu fetches so far", lz_n,
                     (unsigned long long)__atomic_load_n(&lz_fetch_total, __ATOMIC_RELAXED));
    if (k < 0 || (size_t)k >= n) return;
    if (tid)
        snprintf(out + k, n - (size_t)k, ", one of %llu bytes in progress on thread %u%s", (unsigned long long)bytes,
                 tid, tid == me ? " (this one)" : "");
    else if (last)
        snprintf(out + k, n - (size_t)k, ", the last ended %u ms ago", (uint32_t)plat_now_ms() - last);
}

void mem_lazy_report(uint64_t frame, uint64_t skipped) {
    static int64_t async_said = -1;
    int64_t an = __atomic_load_n(&frz_async_n, __ATOMIC_RELAXED), af = __atomic_load_n(&frz_protect_fails, __ATOMIC_RELAXED);
    if ((rt_trace & RT_TRACE_PERF) && an != async_said) {
        async_said = an;
        rt_log("mem: thaws outside the freeze lock so far %lld (NX2SARIA_FREEZE_ASYNC), page protections that failed there "
               "%lld", (long long)an, (long long)af);
    }
    if (!__atomic_load_n(&lz_on, __ATOMIC_RELAXED)) return;
    plat_mutex_lock(&frz_lock);
    uint64_t bytes = 0, fr = lz_faults[0], fw = lz_faults[1], other = lz_other, wmax = lz_lock_wait_max_ns,
             gaps = lz_gaps, unclean = lz_unclean, no[LZ_NO_N];
    int n = lz_n;
    for (int i = 0; i < lz_n; i++) bytes += lzs[i].hi - lzs[i].lo;
    lz_faults[0] = lz_faults[1] = lz_other = lz_lock_wait_max_ns = lz_gaps = lz_unclean = 0;
    plat_mutex_lock(&lz_lock);
    uint64_t nf = lz_fetches, fb = lz_fetch_bytes, fns = lz_fetch_ns, fmax = lz_fetch_max_ns, joins = lz_joins,
             jmax = lz_join_max_ns;
    int most = lz_fly_most;
    lz_fetches = lz_fetch_bytes = lz_fetch_ns = lz_fetch_max_ns = lz_joins = lz_join_max_ns = 0;
    lz_fly_most = lz_nfly;
    plat_mutex_unlock(&lz_lock);
    uint64_t small = lz_no_small, calls = lz_arm_calls;
    int nnote = lz_nnote;
    __typeof__(lz_note) note;
    memcpy(note, lz_note, sizeof note);
    lz_nnote = 0;
    memcpy(no, lz_no, sizeof no);
    memset(lz_no, 0, sizeof lz_no);
    lz_no_small = lz_arm_calls = 0;
    uint64_t pieces = lz_pieces, piece_bytes = lz_piece_bytes, heavy = lz_heavy, foreign = lz_foreign;
    lz_pieces = lz_piece_bytes = lz_heavy = 0;
    plat_mutex_unlock(&frz_lock);
    static uint64_t foreign_said;
    if (foreign != foreign_said) {
        foreign_said = foreign;
        rt_log("mem: lazy write-back: %llu armed ranges named a mirror that is no longer the pool's, given back without "
               "a fetch so far", (unsigned long long)foreign);
    }
    rt_log("mem: lazy write-back at frame %llu: %d ranges armed, %.1f MiB; since the last line %.1f MiB left in the "
           "mirror, %llu read and %llu write faults (%llu inside a part's window), %llu other releases, %llu fetches of "
           "%.1f MiB in %.2f ms, the longest %.2f ms, faults waited up to %.2f ms for the freeze lock, %llu arms refused "
           "(a CPU write not in the mirror yet)", (unsigned long long)frame, n, bytes / 1048576.0, skipped / 1048576.0,
           (unsigned long long)fr, (unsigned long long)fw, (unsigned long long)gaps, (unsigned long long)other,
           (unsigned long long)nf, fb / 1048576.0, fns / 1e6, fmax / 1e6, wmax / 1e6, (unsigned long long)unclean);
    if (joins)
        rt_log("mem: lazy write-back at frame %llu: %llu touches waited for a range another thread was fetching, the "
               "longest %.2f ms; at most %d ranges in flight at once", (unsigned long long)frame,
               (unsigned long long)joins, jmax / 1e6, most);
    for (int i = 0; i < nnote && (rt_trace & RT_TRACE_PERF); i++)
        rt_log("mem: lazy write-back: thread %u %s %#llx, %llu KiB into an armed range of %.2f MiB at %#llx; %llu KiB of "
               "it given back%s and held for %u s", note[i].tid, note[i].write ? "wrote" : "read",
               (unsigned long long)note[i].addr, (unsigned long long)((note[i].addr - note[i].lo) >> 10),
               (note[i].hi - note[i].lo) / 1048576.0, (unsigned long long)note[i].lo, (unsigned long long)(note[i].out >> 10),
               note[i].stale ? " (fetched)" : " (its bytes were current)", note[i].hold);
    if (pieces || heavy)
        rt_log("mem: lazy write-back at frame %llu: %llu touches gave back a piece only (%.2f MiB in all), %llu regions "
               "were touched too often in a second and give back whole ranges for now",
               (unsigned long long)frame, (unsigned long long)pieces, piece_bytes / 1048576.0, (unsigned long long)heavy);
    if (no[LZ_NO_FROZEN] + no[LZ_NO_HOT] + no[LZ_NO_THAW] + no[LZ_NO_PAGES] + no[LZ_NO_EDGE])
        rt_log("mem: lazy write-back at frame %llu: written back and not armed since the last line: %.1f MiB in frozen "
               "regions, %.1f MiB in regions the CPU touched lately, %.1f MiB in regions held after a thaw, %.1f MiB in "
               "regions with pages that are not read-write or with a CPU write not in the mirror, %.2f MiB in the "
               "partial pages at the ends of ranges; %llu ranges written back, %llu of them without a whole page",
               (unsigned long long)frame, no[LZ_NO_FROZEN] / 1048576.0, no[LZ_NO_HOT] / 1048576.0,
               no[LZ_NO_THAW] / 1048576.0, no[LZ_NO_PAGES] / 1048576.0, no[LZ_NO_EDGE] / 1048576.0,
               (unsigned long long)calls, (unsigned long long)small);
}

static void thaw_pages_vm(uint64_t r, int strict);

static void thaw_pages(uint64_t r, int strict) {
    plat_mutex_lock(&vm_lock);
    thaw_pages_vm(r, strict);
    plat_mutex_unlock(&vm_lock);
}

static void thaw_pages_vm(uint64_t r, int strict) {
    uint64_t base = r << FRZ_SHIFT;
    const FrzRuns *k = frz_runs[r];
    if (k && k->n) {
        for (int i = 0; i < k->n; i++) {
            uint64_t a = base + ((uint64_t)k->run[i][0] << 12), n = (uint64_t)(k->run[i][1] - k->run[i][0]) << 12;
            if (!plat_mem_protect(a, n, PLAT_PROT_RW)) {
                if (strict)
                    rt_fatal("memory: thawing %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)n);
                plat_inc64(&frz_protect_fails);
            }
        }
        return;
    }
    uint64_t a = base, end = a + FRZ_SIZE, b;
    for (; a < end; a = b)
        if (plat_mem_state(a, end, &b) == PLAT_PROT_READ && !plat_mem_protect(a, b - a, PLAT_PROT_RW)) {
            if (strict) rt_fatal("memory: thawing %#llx+%#llx failed", (unsigned long long)a, (unsigned long long)(b - a));
            plat_inc64(&frz_protect_fails);
        }
}

static int thaw_begin_locked(uint64_t r, int why) {
    if (why == 2) thaw_ms[r] = (uint32_t)plat_now_ms() | 1;
    if (lzr[r]) lazy_drop_region_locked(r);
    if (prb[r]) unprobe_locked(r);
    return frz[r] == 1;
}

static void thaw_end_locked(uint64_t r, int why);

static void thaw_locked(uint64_t r, int why) {
    if (!thaw_begin_locked(r, why) && frz[r] != FRZ_THAWING) return;
    thaw_pages(r, 1);
    __atomic_store_n(&frz[r], FRZ_THAWING, __ATOMIC_RELEASE);
    thaw_end_locked(r, why);
}

static void thaw_end_locked(uint64_t r, int why) {
    if (frz[r] != FRZ_THAWING) return;
    __atomic_store_n(&frz[r], 0, __ATOMIC_RELEASE);
    plat_dec32(&nfrz);
    plat_inc64(&frz_count[why]);
    if (why == 1) {
        uint32_t now = (uint32_t)plat_now_ms();
        if (frz_ms[r] && now - frz_ms[r] < FRZ_QUICK_MS) {
            uint32_t h = frz_hot_ms[r] ? frz_hot_ms[r] * 2 : FRZ_HOT_MIN_MS;
            frz_hot_ms[r] = h > FRZ_HOT_MAX_MS ? FRZ_HOT_MAX_MS : h;
            frz_hot_until[r] = (now + frz_hot_ms[r]) | 1;
            plat_inc64(&frz_hot_holds);
        } else {
            frz_hot_ms[r] = 0;
        }
    }
}

static void thaw_range_locked(uint64_t a, uint64_t n, int why) {
    if (!n || a >= RT_ADDRESS_LIMIT) return;
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1; r++) thaw_locked(r, why);
}

static void thaw_wait_locked(uint64_t a, uint64_t n) {
    if (!n || a >= RT_ADDRESS_LIMIT) return;
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1; r++)
        while (__atomic_load_n(&frz[r], __ATOMIC_ACQUIRE) == FRZ_THAWING) {
            plat_mutex_unlock(&frz_lock);
            plat_wait_ns(20000);
            plat_mutex_lock(&frz_lock);
        }
}

static uint64_t heap_lazy_commit_locked(uint64_t a, uint64_t n) {
    uint64_t lo = a > L.heap.base ? a : L.heap.base, top = L.heap.base + __atomic_load_n(&heap_size, __ATOMIC_ACQUIRE);
    uint64_t end = a + n < a ? UINT64_MAX : a + n, hi = end < top ? end : top, got = 0;
    if (lo >= hi) return 0;
    lo &= ~(PAGE - 1), hi = align_up(hi, PAGE);
    int thawed = 0;
    for (uint64_t x = lo, y; x < hi; x = y) {
        if (plat_mem_state(x, hi, &y) != PLAT_MEM_RESERVED) continue;
        if (!thawed) thaw_range_locked(lo, hi - lo, 3), thawed = 1;
        uint32_t error;
        if (!plat_mem_commit(x, y - x, &error)) commit_failed(x, y - x, "heap pages on first use", error);
        got += y - x;
    }
    heap_lazy_bytes += got;
    return got;
}

void mem_kernel_access(uint64_t a, uint64_t n) {
    if (!heap_lazy || !n) return;
    plat_mutex_lock(&frz_lock);
    heap_lazy_commit_locked(a, n);
    plat_mutex_unlock(&frz_lock);
}

uint64_t mem_untouched_run(uint64_t a, uint64_t end) {
    if (!heap_lazy || a < L.heap.base || a >= L.heap.base + L.heap.size) return 0;
    uint64_t y, x = a & ~(PAGE - 1);
    return plat_mem_state(x, end, &y) == PLAT_MEM_RESERVED && y > a ? y : 0;
}

int mem_host_readable(uint64_t a, uint64_t n) {
    mem_kernel_access(a, n);
    return plat_mem_readable(a, n);
}

static int heap_lazy_fault(uint64_t a) {
    uint64_t heap = L.heap.base;
    if (a < heap || a - heap >= __atomic_load_n(&heap_size, __ATOMIC_ACQUIRE)) return 0;
    uint64_t t0 = plat_now_ns();
    plat_mutex_lock(&frz_lock);
    uint64_t got = heap_lazy_commit_locked(a & ~0xFFFFull, 0x10000);
    if (got) {
        uint64_t ns = plat_now_ns() - t0;
        heap_lazy_faults++, heap_lazy_ns += ns;
        if (ns > heap_lazy_max_ns) heap_lazy_max_ns = ns;
    }
    plat_mutex_unlock(&frz_lock);
    return got != 0;
}

void mem_thaw(uint64_t a, uint64_t n) {
    lazy_release_regions(a, n);
    plat_mutex_lock(&frz_lock);
    if (heap_lazy || !frz_async || !n || a >= RT_ADDRESS_LIMIT) {
        if (heap_lazy) {
            thaw_wait_locked(a, n);
            heap_lazy_commit_locked(a, n);
        }
        thaw_range_locked(a, n, 2);
        plat_mutex_unlock(&frz_lock);
        return;
    }
    enum { MANY = 64 };
    uint64_t r0 = a >> FRZ_SHIFT, r1 = (a + n - 1 < RT_ADDRESS_LIMIT ? a + n - 1 : RT_ADDRESS_LIMIT - 1) >> FRZ_SHIFT;
    for (uint64_t r = r0; r <= r1;) {
        uint64_t mine[MANY], theirs[MANY];
        int nm = 0, nt = 0;
        for (; r <= r1 && nm < MANY && nt < MANY; r++) {
            if (thaw_begin_locked(r, 2)) {
                __atomic_store_n(&frz[r], FRZ_THAWING, __ATOMIC_RELEASE);
                mine[nm++] = r;
            } else if (frz[r] == FRZ_THAWING) {
                theirs[nt++] = r;
            }
        }
        if (!nm && !nt) continue;
        plat_mutex_unlock(&frz_lock);
        for (int i = 0; i < nm; i++) thaw_pages(mine[i], 0);
        for (int i = 0; i < nt; i++) thaw_pages(theirs[i], 0);
        plat_add64(&frz_async_n, nm);
        plat_mutex_lock(&frz_lock);
        for (int i = 0; i < nm; i++) thaw_end_locked(mine[i], 2);
    }
    plat_mutex_unlock(&frz_lock);
}

static int freeze_locked(uint64_t r) {
    uint64_t a = r << FRZ_SHIFT, end = a + FRZ_SIZE, run[16][2];
    int n = 0, ok = 1;
    plat_mutex_lock(&vm_lock);
    for (uint64_t x = a, y; x < end && ok; x = y) {
        int st = plat_mem_state(x, end, &y);
        if (st >= PLAT_PROT_NONE) {
            if (st != PLAT_PROT_RW || n == 16) ok = 0;
            else run[n][0] = x, run[n][1] = y, n++;
        } else if (st != PLAT_MEM_RESERVED) ok = 0;
    }
    for (int i = 0; ok && i < n; i++)
        if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_READ)) {
            while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_RW);
            ok = 0;
        }
    plat_mutex_unlock(&vm_lock);
    if (!ok) return 0;
    if (n && !frz_runs[r]) frz_runs[r] = calloc(1, sizeof *frz_runs[r]);
    if (frz_runs[r]) {
        frz_runs[r]->n = (uint8_t)n;
        for (int i = 0; i < n; i++)
            frz_runs[r]->run[i][0] = (uint16_t)((run[i][0] - a) >> 12), frz_runs[r]->run[i][1] = (uint16_t)((run[i][1] - a) >> 12);
    }
    return n > 0;
}

int mem_freeze(uint64_t a) {
    if ((a & (FRZ_SIZE - 1)) || !watched(a, FRZ_SIZE)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    int ok = 0;
    plat_mutex_lock(&frz_lock);
    uint32_t now = (uint32_t)plat_now_ms();
    if (frz[r]) ok = 1;
    else if (__atomic_load_n(&lzr[r], __ATOMIC_ACQUIRE)) ok = 0;
    else if (thaw_ms[r] && (int32_t)(now - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) ok = 0;
    else if (frz_hot_until[r] && (int32_t)(frz_hot_until[r] - now) > 0) ok = 0;
    else if (freeze_locked(r)) {
        frz_ms[r] = now | 1;
        __atomic_store_n(&frz[r], 1, __ATOMIC_RELEASE);
        plat_inc32(&nfrz);
        plat_inc64(&frz_count[0]);
        ok = 1;
    }
    plat_mutex_unlock(&frz_lock);
    return ok;
}

int mem_frozen(uint64_t a) { return a < RT_ADDRESS_LIMIT && __atomic_load_n(&frz[a >> FRZ_SHIFT], __ATOMIC_ACQUIRE); }

int mem_probe(uint64_t a, uint64_t n) {
    if (!__atomic_load_n(&prb_on, __ATOMIC_RELAXED) || n < FRZ_SIZE || a >= RT_ADDRESS_LIMIT || !watched(a, n)) return 0;
    uint64_t r0 = (a + FRZ_SIZE - 1) >> FRZ_SHIFT, r1 = (a + n) >> FRZ_SHIFT;
    int armed = 0;
    plat_mutex_lock(&frz_lock);
    for (uint64_t r = r0; r < r1; r++) {
        if (prb[r]) continue;
        if (thaw_ms[r] && (int32_t)((uint32_t)plat_now_ms() - thaw_ms[r]) < (int32_t)FRZ_HOLD_MS) continue;
        int want = frz[r] ? PLAT_PROT_READ : PLAT_PROT_RW, k = 0, ok = 1;
        uint64_t run[16][2];
        for (uint64_t x = r << FRZ_SHIFT, end = x + FRZ_SIZE, y; x < end && ok; x = y) {
            int st = plat_mem_state(x, end, &y);
            if (st >= PLAT_PROT_NONE) {
                if (st != want || k == 16) ok = 0;
                else run[k][0] = x, run[k][1] = y, k++;
            } else if (st != PLAT_MEM_RESERVED) ok = 0;
        }
        if (!ok || !k) continue;
        for (int i = 0; i < k && ok; i++)
            if (!plat_mem_protect(run[i][0], run[i][1] - run[i][0], PLAT_PROT_NONE)) {
                while (i--) plat_mem_protect(run[i][0], run[i][1] - run[i][0], want);
                ok = 0;
            }
        if (!ok) continue;
        __atomic_store_n(&prb[r], 1, __ATOMIC_RELEASE);
        plat_inc32(&nprb);
        prb_arms++, armed++;
    }
    plat_mutex_unlock(&frz_lock);
    return armed;
}

int mem_probing(void) { return __atomic_load_n(&prb_on, __ATOMIC_RELAXED) || __atomic_load_n(&lz_on, __ATOMIC_RELAXED); }

static int probe_fault(uint64_t a, int write, uint64_t ip) {
    if (a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    plat_mutex_lock(&frz_lock);
    int was = prb[r];
    if (was) {
        unprobe_locked(r);
        prb_ends[write != 0]++;
        if (prb_nhit < PRB_HITS) prb_hit[prb_nhit++] = (typeof(prb_hit[0])){a, ip, plat_thread_id(), (uint8_t)(write != 0)};
    }
    plat_mutex_unlock(&frz_lock);
    if (was) return 1;
    if (write) return 0;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) >= PLAT_PROT_READ;
}

static int access_fault(uint64_t a, int write, uint64_t ip) {
    if (heap_lazy && heap_lazy_fault(a)) return 1;
    if (__atomic_load_n(&lz_on, __ATOMIC_RELAXED) && lazy_fault(a, write)) return 1;
    if (__atomic_load_n(&prb_on, __ATOMIC_RELAXED)) return probe_fault(a, write, ip);
    if (write || a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) >= PLAT_PROT_READ;
}

void mem_probe_report(uint64_t frame) {
    if (!__atomic_load_n(&prb_on, __ATOMIC_RELAXED)) return;
    typeof(prb_hit[0]) hit[PRB_HITS];
    plat_mutex_lock(&frz_lock);
    uint64_t arms = prb_arms, reads = prb_ends[0], writes = prb_ends[1];
    int n = prb_nhit;
    memcpy(hit, prb_hit, sizeof hit[0] * (size_t)n);
    prb_arms = prb_ends[0] = prb_ends[1] = 0, prb_nhit = 0;
    plat_mutex_unlock(&frz_lock);
    rt_log("mem: write-back probe at frame %llu: %llu regions of 2 MiB armed, probes ended by %llu reads and %llu writes "
           "since the last line; %d probed now", (unsigned long long)frame, (unsigned long long)arms,
           (unsigned long long)reads, (unsigned long long)writes, (int)__atomic_load_n(&nprb, __ATOMIC_RELAXED));
    uintptr_t exe = plat_exe_base();
    for (int i = 0, lines = 0; i < n && lines < 12; i++) {
        if (!hit[i].ip) continue;
        int same = 1;
        for (int j = i + 1; j < n; j++)
            if (hit[j].ip == hit[i].ip && hit[j].write == hit[i].write) same++, hit[j].ip = 0;
        rt_log("mem:   %d x a %s at %#llx.. from %s%#llx (thread %u)", same, hit[i].write ? "write" : "read",
               (unsigned long long)hit[i].addr, hit[i].ip >= exe && hit[i].ip - exe < (1ull << 32) ? "nx2navi+" : "",
               (unsigned long long)(hit[i].ip >= exe && hit[i].ip - exe < (1ull << 32) ? hit[i].ip - exe : hit[i].ip),
               hit[i].tid);
        lines++;
    }
}

void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws,
                      uint64_t *hot_holds) {
    *frozen = (uint64_t)__atomic_load_n(&nfrz, __ATOMIC_RELAXED);
    *hot_holds = (uint64_t)plat_xchg64(&frz_hot_holds, 0);
    *freezes = (uint64_t)plat_xchg64(&frz_count[0], 0);
    *fault_thaws = (uint64_t)plat_xchg64(&frz_count[1], 0);
    *other_thaws = (uint64_t)plat_xchg64(&frz_count[2], 0) + (uint64_t)plat_xchg64(&frz_count[3], 0);
}

static int thaw_fault(uint64_t a) {
    if (a >= RT_ADDRESS_LIMIT || !watched(a, 1)) return 0;
    uint64_t r = a >> FRZ_SHIFT;
    plat_mutex_lock(&frz_lock);
    int was = frz[r];
    if (was == FRZ_THAWING) {
        while (frz[r] == FRZ_THAWING) {
            plat_mutex_unlock(&frz_lock);
            plat_wait_ns(20000);
            plat_mutex_lock(&frz_lock);
        }
        plat_mutex_unlock(&frz_lock);
        return 1;
    }
    if (frz_async && was == 1) {
        thaw_begin_locked(r, 1);
        __atomic_store_n(&frz[r], FRZ_THAWING, __ATOMIC_RELEASE);
        plat_mutex_unlock(&frz_lock);
        thaw_pages(r, 0);
        plat_inc64(&frz_async_n);
        plat_mutex_lock(&frz_lock);
        thaw_end_locked(r, 1);
        plat_mutex_unlock(&frz_lock);
        return 1;
    }
    thaw_locked(r, 1);
    plat_mutex_unlock(&frz_lock);
    if (was) return 1;
    uint64_t end;
    return plat_mem_state(a & ~(PAGE - 1), (a & ~(PAGE - 1)) + PAGE, &end) == PLAT_PROT_RW;
}

static void commit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_wait_locked(a, n);
    thaw_range_locked(a, n, 3);
    uint32_t error = 0;
    int ok = !n || plat_mem_commit(a, n, &error);
    plat_mutex_unlock(&frz_lock);
    if (!ok) commit_failed(a, n, what, error);
}

static void decommit(uint64_t a, uint64_t n, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_wait_locked(a, n);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_decommit(a, n);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: decommit %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

static void protect(uint64_t a, uint64_t n, int prot, const char *what) {
    plat_mutex_lock(&frz_lock);
    thaw_wait_locked(a, n);
    thaw_range_locked(a, n, 3);
    int ok = !n || plat_mem_protect(a, n, prot);
    plat_mutex_unlock(&frz_lock);
    if (!ok) rt_fatal("memory: protect %s %#llx+%#llx failed", what, (unsigned long long)a, (unsigned long long)n);
}

static void reexec_or_fail(const char *what) {
    const char *e = cfg_get("NX2SARIA_REEXEC");
    int n = e ? atoi(e) : 0;
    if (n >= REEXEC_LIMIT) rt_fatal("memory: %s is not free after %d attempts", what, n);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    cfg_setenv("NX2SARIA_REEXEC", buf);
    plat_reexec();
}

void mem_init(int game) {
    if (L.pool) return;
    mods_lo = MOD_MAIN_BASE;
    mods_hi = align_up(rt_layout[rt_layout_count - 1].base + rt_layout[rt_layout_count - 1].size, 0x10000);
    if (mods_hi > RT_KERNEL_BASE) rt_fatal("memory: modules end at %#llx, inside the kernel region",
                                           (unsigned long long)mods_hi);
    if (!reserve_at(mods_lo, mods_hi - mods_lo, 0)) reexec_or_fail("the module window");
    if (!reserve_at(RT_KERNEL_BASE, RT_KERNEL_SIZE, 0)) reexec_or_fail("the kernel region");
    if (!reserve_at(RT_HOSTFN_BASE, RT_HOSTFN_SIZE, 0)) reexec_or_fail("the host-function window");

    cfg_init();

    uint64_t low = 0, alias = 0;
    if (tracker_api_on())
        for (uint64_t at = HIGH_SEARCH_FROM; at + LOW_SIZE <= mods_lo && !low; at += GiB)
            if (reserve_at(at, LOW_SIZE, 1)) low = at;
    for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= mods_lo && !low; at += GiB)
        if (reserve_at(at, LOW_SIZE, 1)) low = at;
    if (!low) reexec_or_fail("a 14 GiB stack and heap block below 2^39");
    for (uint64_t at = align_up(low + LOW_SIZE, ALIAS_ALIGN); at + ALIAS_SIZE <= mods_lo && !alias; at += ALIAS_ALIGN)
        if (reserve_at(at, ALIAS_SIZE, 1)) alias = at;
    if (!alias) reexec_or_fail("a 64 GiB alias region below 2^39");
    cfg_start(game);

    plat_mem_fault_hook(thaw_fault);
    alias_pages = calloc(ALIAS_PAGES, 1);
    if (!alias_pages) rt_fatal("memory: cannot allocate the alias page map");
    if (cfg_bool("NX2SARIA_WB_PROBE")) {
        plat_mem_access_hook(access_fault);
        __atomic_store_n(&prb_on, 1, __ATOMIC_RELEASE);
    } else if (cfg_bool("NX2SARIA_WB_LAZY")) {
        plat_mem_access_hook(access_fault);
        lz_want = 1;
        lz_chunk = cfg_bool("NX2SARIA_WB_LAZY_CHUNK");
        lz_unlocked = cfg_bool("NX2SARIA_WB_LAZY_UNLOCKED");
    }
    const char *fa = cfg_get("NX2SARIA_FREEZE_ASYNC");
    frz_async = !fa || !*fa || atoi(fa) != 0;
    const char *pool = cfg_get("NX2SARIA_APP_POOL");
    L = (RtGuestLayout){
        .aslr = {low, RT_ADDRESS_LIMIT - low},
        .stack = {low + STACK_OFF, STACK_SIZE},
        .heap = {low + HEAP_OFF, HEAP_CAP},
        .alias = {alias, ALIAS_SIZE},
        .kernel = {RT_KERNEL_BASE, RT_KERNEL_SIZE},
        .hostfn = {RT_HOSTFN_BASE, RT_HOSTFN_SIZE},
        .pool = pool ? strtoull(pool, NULL, 0) : DEFAULT_POOL + res_heap_extra_bytes(),
    };
    if (!L.pool) rt_fatal("memory: NX2SARIA_APP_POOL is 0");
    rt_log("mem: commit failure diagnostics enabled (host error, byte count and commit used/limit)");
#ifdef _WIN32
    if (cfg_bool("NX2SARIA_HEAP_LAZY")) {
        plat_mem_access_hook(access_fault);
        heap_lazy = 1;
        rt_log("mem: the heap is committed on first use (NX2SARIA_HEAP_LAZY)");
    }
#endif
}

const RtGuestLayout *mem_layout(void) { return &L; }

int mem_start_note(int probe) {
    double proc, used, limit, need = strtod(cfg_value("NX2SARIA_MEM_NEED_MB"), NULL);
    plat_commit_info(&proc, &used, &limit);
    int is_short = mem_need_short(proc, used, limit, need);
    if (probe)
        printf("probe-memneed: system commit %.0f of %.0f MB in use, this process %.0f MB, the game needs %.0f MB "
               "(NX2SARIA_MEM_NEED_MB): %s\n", used, limit, proc, need,
               need <= 0 ? "not checked" : limit < 0 ? "no limit known" : is_short ? "short" : "enough");
    if (!is_short) return 0;
    char note[400] = "";
    plat_commit_note(note, sizeof note);
    rt_log("mem: WARNING: this machine has %.0f MB of memory left for the game and the game takes about %.0f MB in play "
           "(system commit: %.0f of %.0f MB in use; the limit is RAM plus page file%s%s). Short of memory the game "
           "stutters, loses textures or stops with an out-of-memory or lost-device error. Closing other programs or a "
           "larger page file gives it room (mem_need_mb = 0 in nx2saria.ini (or an existing nx2navi.ini) turns this warning off)",
           mem_need_room(proc, used, limit), need, used, limit, note[0] ? "; " : "", note);
    return 1;
}

static uint64_t written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !size) return 0;
    if (!watched(addr, size)) return UINT64_MAX;
    plat_mutex_lock(&vm_lock);
    uint64_t n = plat_mem_written(addr, size, pages, max, reset);
    plat_mutex_unlock(&vm_lock);
    return n;
}

uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 1);
}

uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 0);
}

int mem_watched(uint64_t addr, uint64_t size) { return size && watched(addr, size); }

static uint64_t image_bytes(void) {
    uint64_t n = 0;
    for (int i = 0; i < ldr_count(); i++) n += ldr_module(i)->size;
    return align_up(n, PAGE);
}

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }
static int inside(uint64_t a, uint64_t n, RtRange r) { return a >= r.base && n <= r.size && a - r.base <= r.size - n; }

static uint32_t map_gen;
uint32_t mem_map_gen(void) { return __atomic_load_n(&map_gen, __ATOMIC_ACQUIRE); }

uint64_t mem_heap_extra_max_kib(void) {
    return HEAP_CAP > DEFAULT_POOL ? ((HEAP_CAP - DEFAULT_POOL) & ~0x1fffffull) >> 10 : 0;
}

uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr) {
    uint64_t t0 = perf_mark_begin();
    plat_mutex_lock(&lock);
    if (size > HEAP_CAP || size % 0x200000 || size + image_bytes() > L.pool) {
        plat_mutex_unlock(&lock);
        return R_INVALID_STATE;
    }
    uint64_t was = heap_size;
    if (size > was && !heap_lazy) commit(L.heap.base + was, size - was, "heap");
    __atomic_store_n(&heap_size, size, __ATOMIC_RELEASE);
    if (size < was) decommit(L.heap.base + size, was - size, "heap");
    uint64_t moved = size > was ? size - was : was - size;
    __atomic_store_n(&map_gen, map_gen + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    perf_mark_end(PERF_M_GMEM, t0, moved);
    if (addr) *addr = L.heap.base;
    return R_OK;
}

static int module_index(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a - m->base < m->size) return i;
    }
    return -1;
}

static uint8_t *perm_table(int i) {
    if (!module_perm[i]) {
        const RtLoaded *m = ldr_module(i);
        size_t n = (m->size - m->data_off) / PAGE;
        module_perm[i] = malloc(n);
        if (!module_perm[i]) rt_fatal("memory: module permission table");
        memset(module_perm[i], PERM_RW, n);
    }
    return module_perm[i];
}

static unsigned module_page_perm(int i, uint64_t off) {
    const RtLoaded *m = ldr_module(i);
    if (off < m->data_off) return off < align_up(m->text_end, PAGE) ? PERM_RX : PERM_R;
    return module_perm[i] ? module_perm[i][(off - m->data_off) / PAGE] : PERM_RW;
}

static int module_data(uint64_t a, uint64_t n) {
    int i = module_index(a);
    if (i < 0) return -1;
    const RtLoaded *m = ldr_module(i);
    uint64_t off = a - m->base;
    return off >= m->data_off && n <= m->size - off ? i : -1;
}

uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++)
        if (t[p] & ATTR_PERMISSION_LOCKED) {
            plat_mutex_unlock(&lock);
            return R_INVALID_STATE;
        }
    memset(t, (int)perm, n / PAGE);
    protect(a, n, perm == PERM_RW ? PLAT_PROT_RW : perm == PERM_R ? PLAT_PROT_READ : PLAT_PROT_NONE, "module data");
    __atomic_store_n(&map_gen, map_gen + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    rt_log("%s data permission +%#llx size %#llx perm %u", m->name, (unsigned long long)(a - m->base),
           (unsigned long long)n, perm);
    return R_OK;
}

uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (mask != ATTR_PERMISSION_LOCKED || at != ATTR_PERMISSION_LOCKED) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++) t[p] |= ATTR_PERMISSION_LOCKED;
    plat_mutex_unlock(&lock);
    return R_OK;
}

static int alias_all_mapped(uint64_t a, uint64_t n) {
    uint64_t first = (a - L.alias.base) / PAGE;
    for (uint64_t i = 0; i < n / PAGE; i++)
        if (!alias_pages[first + i]) return 0;
    return 1;
}

static uint32_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t n) {
    if (!n || ((dst | src | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(dst, n, L.stack)) return R_INVALID_REGION;
    int in_heap = inside(src, n, (RtRange){L.heap.base, heap_size});
    int in_alias = inside(src, n, L.alias);
    if (!in_heap && !in_alias) return R_INVALID_REGION;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (in_alias && !alias_all_mapped(src, n)) {
        r = R_INVALID_REGION;
        goto out;
    }
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++)
        if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == n) found = i;
    if (unmap) {
        if (found == nstack_maps) {
            r = R_INVALID_STATE;
            goto out;
        }
        protect(src, n, PLAT_PROT_RW, "moved stack source");
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, n);
        decommit(dst, n, "moved stack");
        stack_maps[found] = stack_maps[--nstack_maps];
    } else {
        if (nstack_maps == MAX_STACK_MAPS) {
            r = R_INVALID_STATE;
            goto out;
        }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, n, stack_maps[i].dst, stack_maps[i].size) ||
                overlaps(src, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        commit(dst, n, "moved stack");
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
        protect(src, n, PLAT_PROT_NONE, "moved stack source");
        stack_maps[nstack_maps++] = (StackMap){dst, src, n};
    }
out:
    __atomic_store_n(&map_gen, map_gen + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    return r;
}

uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(0, dst, src, n); }
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(1, dst, src, n); }

static void alias_runs(uint64_t first, uint64_t count, uint8_t want, int do_commit) {
    uint64_t i = 0;
    while (i < count) {
        if (alias_pages[first + i] != want) { i++; continue; }
        uint64_t j = i;
        while (j < count && alias_pages[first + j] == want) j++;
        uint64_t a = L.alias.base + (first + i) * PAGE, n = (j - i) * PAGE;
        if (do_commit) commit(a, n, "physical memory");
        else decommit(a, n, "physical memory");
        memset(alias_pages + first + i, !want, j - i);
        i = j;
    }
}

static uint32_t alias_map(int unmap, uint64_t a, uint64_t n) {
    if (!n) return R_INVALID_ADDRESS;
    n = (n + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    if ((a | n) & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!inside(a, n, L.alias)) return R_INVALID_REGION;
    uint64_t first = (a - L.alias.base) / PAGE, count = n / PAGE, t0 = perf_mark_begin();
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(a, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        uint64_t released = 0;
        for (uint64_t i = 0; i < count; i++) released += alias_pages[first + i];
        alias_runs(first, count, 1, 0);
        alias_bytes -= released * PAGE;
    } else {
        uint64_t fresh = 0;
        for (uint64_t i = 0; i < count; i++) fresh += !alias_pages[first + i];
        if (fresh * PAGE + alias_bytes + heap_size + image_bytes() > L.pool) {
            r = R_LIMIT_REACHED;
            goto out;
        }
        alias_runs(first, count, 0, 1);
        alias_bytes += fresh * PAGE;
    }
out:
    __atomic_store_n(&map_gen, map_gen + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&lock);
    perf_mark_end(PERF_M_GMEM, t0, n);
    if (r) {
        static uint32_t told[2];
        int due = log_event(unmap ? LS_UNMAP_PHYS_FAILED : LS_MAP_PHYS_FAILED);
        int other = __atomic_exchange_n(&told[unmap != 0], r, __ATOMIC_RELAXED) != r;
        if (due || other)
            rt_log("%sMapPhysicalMemory %#llx+%#llx failed %#x (alias %#llx used, heap %#llx)%s", unmap ? "Un" : "",
                   (unsigned long long)a, (unsigned long long)n, r, (unsigned long long)alias_bytes,
                   (unsigned long long)heap_size, due ? "; the later ones are counted in the frametime events line" : "");
    }
    return r;
}

uint32_t mem_map_physical(uint64_t a, uint64_t n) { return alias_map(0, a, n); }
uint32_t mem_unmap_physical(uint64_t a, uint64_t n) { return alias_map(1, a, n); }

static void taken_gap(uint64_t a, uint64_t *base, uint64_t *size) {
    uint64_t lo = 0, hi = RT_ADDRESS_LIMIT;
    RtRange known[3 + MOD_COUNT + 2];
    int n = 0;
    known[n++] = (RtRange){L.aslr.base, LOW_SIZE};
    known[n++] = L.alias;
    known[n++] = L.kernel;
    known[n++] = L.hostfn;
    for (int i = 0; i < ldr_count(); i++) known[n++] = (RtRange){ldr_module(i)->base, ldr_module(i)->size};
    for (int i = 0; i < n; i++) {
        uint64_t e = known[i].base + known[i].size;
        if (e <= a && e > lo) lo = e;
        if (known[i].base > a && known[i].base < hi) hi = known[i].base;
    }
    *base = lo;
    *size = hi - lo;
}

static uint64_t run_end(const uint8_t *p, uint64_t i, uint64_t n, uint8_t v) {
    while (i < n && (i & 7)) { if (p[i] != v) return i; i++; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i + 8 <= n && *(const uint64_t *)(p + i) == w) i += 8;
    while (i < n && p[i] == v) i++;
    return i;
}

static uint64_t run_start(const uint8_t *p, uint64_t i, uint8_t v) {
    while (i > 0 && (i & 7)) { if (p[i - 1] != v) return i; i--; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i >= 8 && *(const uint64_t *)(p + i - 8) == w) i -= 8;
    while (i > 0 && p[i - 1] == v) i--;
    return i;
}

static void query_locked(uint64_t a, RtMemoryInfo *out);

static int readable_fast(uint64_t a, uint64_t n) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("READABLE_FAST", "1");
    if (!RT_KNOB_ON(mode)) return -1;
    uint64_t end = a + n, low = L.aslr.base;
    if (!n || end < a || end > RT_ADDRESS_LIMIT || a < low) return -1;
    for (unsigned i = 0; i < nstack_maps; i++) {
        const StackMap *m = &stack_maps[i];
        if ((a < m->dst + m->size && end > m->dst) || (a < m->src + m->size && end > m->src)) return -1;
    }
    uint64_t heap = L.heap.base;
    if (a >= heap && end <= heap + heap_size && end <= low + LOW_SIZE) return 1;
    if (a < low + LOW_SIZE) return -1;
    if (a - L.alias.base < L.alias.size && end - L.alias.base <= L.alias.size) {
        uint64_t p0 = (a - L.alias.base) / PAGE, p1 = (end - 1 - L.alias.base) / PAGE;
        return memchr(alias_pages + p0, 0, (size_t)(p1 - p0 + 1)) == NULL;
    }
    return -1;
}

static int readable_locked(uint64_t a, uint64_t n) {
    int fast = readable_fast(a, n);
    if (fast >= 0) return fast;
    for (uint64_t end = a + n; a < end;) {
        RtMemoryInfo r;
        query_locked(a, &r);
        if (r.state == ST_FREE || r.state == ST_INACCESSIBLE || !(r.perm & PERM_R) || r.base + r.size <= a) return 0;
        a = r.base + r.size;
    }
    return 1;
}

int mem_readable(uint64_t a, uint64_t n) {
    plat_mutex_lock_shared(&lock);
    int ok = readable_locked(a, n);
    plat_mutex_unlock_shared(&lock);
    return ok;
}

int mem_read_hold(uint64_t a, uint64_t n) {
    plat_mutex_lock_shared(&lock);
    if (readable_locked(a, n)) return 1;
    plat_mutex_unlock_shared(&lock);
    return 0;
}

void mem_read_release(void) { plat_mutex_unlock_shared(&lock); }

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    plat_mutex_lock_shared(&lock);
    query_locked(a, out);
    plat_mutex_unlock_shared(&lock);
    return R_OK;
}

static void query_locked(uint64_t a, RtMemoryInfo *out) {
    RtMemoryInfo r = {.base = a & ~(PAGE - 1), .size = PAGE};
    uint64_t low = L.aslr.base;
    int mi;
    if (a >= RT_ADDRESS_LIMIT) {
        r.base = RT_ADDRESS_LIMIT;
        r.size = 0 - RT_ADDRESS_LIMIT;
        r.state = ST_INACCESSIBLE;
    } else if (a < low) {
        r.base = 0;
        r.size = low;
        r.state = ST_FREE;
    } else if (a < low + LOW_SIZE) {
        uint64_t heap = L.heap.base;
        if (a >= heap && a < heap + heap_size) {
            r = (RtMemoryInfo){.base = heap, .size = heap_size, .state = ST_NORMAL, .perm = PERM_RW};
        } else if (a < heap) {
            r = (RtMemoryInfo){.base = low, .size = heap - low, .state = ST_FREE};
        } else {
            r = (RtMemoryInfo){.base = heap + heap_size, .size = low + LOW_SIZE - heap - heap_size, .state = ST_FREE};
        }
    } else if (a - L.alias.base < L.alias.size) {
        uint64_t p = (a - L.alias.base) / PAGE;
        uint8_t v = alias_pages[p];
        uint64_t s = run_start(alias_pages, p, v), e = run_end(alias_pages, p, ALIAS_PAGES, v);
        r = (RtMemoryInfo){.base = L.alias.base + s * PAGE, .size = (e - s) * PAGE, .state = v ? ST_NORMAL : ST_FREE,
                           .perm = v ? PERM_RW : PERM_NONE};
    } else if ((mi = module_index(a)) >= 0) {
        const RtLoaded *m = ldr_module(mi);
        uint64_t off = (a - m->base) & ~(PAGE - 1), lo = off, hi = off + PAGE;
        unsigned perm = module_page_perm(mi, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && module_page_perm(mi, lo - PAGE) == perm) lo -= PAGE;
        while (hi < end && module_page_perm(mi, hi) == perm) hi += PAGE;
        r = (RtMemoryInfo){.base = m->base + lo, .size = hi - lo, .state = data ? ST_CODEDATA : ST_CODE,
                           .perm = perm & 7, .attr = perm & ATTR_PERMISSION_LOCKED};
    } else if (a - L.kernel.base < L.kernel.size) {
        r = (RtMemoryInfo){.base = L.kernel.base, .size = L.kernel.size, .state = ST_THREADLOCAL, .perm = PERM_RW};
    } else if (a - L.hostfn.base < L.hostfn.size) {
        r = (RtMemoryInfo){.base = L.hostfn.base, .size = L.hostfn.size, .state = ST_INACCESSIBLE};
    } else {
        taken_gap(a, &r.base, &r.size);
        r.state = ST_CODE;
        r.perm = PERM_RX;
    }
    for (unsigned i = 0; i < nstack_maps; i++) {
        const StackMap *m = &stack_maps[i];
        uint64_t end = m->dst + m->size, send = m->src + m->size;
        if (a >= m->dst && a < end) {
            r = (RtMemoryInfo){.base = m->dst, .size = m->size, .state = ST_STACK, .perm = PERM_RW};
            break;
        }
        if (a >= m->src && a < send) {
            r = (RtMemoryInfo){.base = m->src, .size = m->size, .state = ST_NORMAL, .perm = PERM_NONE, .attr = 4};
            break;
        }
        uint64_t lo = r.base, hi = r.base + r.size;
        uint64_t b = r.state == ST_FREE ? m->dst : r.state == ST_NORMAL ? m->src : 0;
        uint64_t e = b + m->size;
        if (!b) continue;
        if (e <= a && e > lo) lo = e;
        if (b > a && b < hi) hi = b;
        r.base = lo;
        r.size = hi - lo;
    }
    if (out) *out = r;
}

int mem_info(uint32_t id, uint64_t *out) {
    uint64_t v;
    switch (id) {
    case 2: v = L.alias.base; break;
    case 3: v = L.alias.size; break;
    case 4: v = L.heap.base; break;
    case 5: v = L.heap.size; break;
    case 6: case 21: v = L.pool; break;
    case 7: case 22:
        plat_mutex_lock_shared(&lock);
        v = image_bytes() + heap_size + alias_bytes;
        plat_mutex_unlock_shared(&lock);
        break;
    case 12: v = L.aslr.base; break;
    case 13: v = L.aslr.size; break;
    case 14: v = L.stack.base; break;
    case 15: v = L.stack.size; break;
    case 16: v = SYSTEM_RESOURCE_BUDGET; break;
    case 17: v = sizeof stack_maps + ALIAS_PAGES; break;
    case 28: v = 0; break;
    default: return 0;
    }
    *out = v;
    return 1;
}

#define KCLASSES 29
#define KMIN_SHIFT 6
#define KCOMMIT 0x10000ull
typedef struct KFree { struct KFree *next; } KFree;
static KFree *kfree_list[KCLASSES];
static uint64_t kbump, kcommitted;
static PlatMutex klock = PLAT_MUTEX_INIT;

static int kclass(uint64_t size) {
    int s = KMIN_SHIFT;
    while ((1ull << s) < size) s++;
    return s - KMIN_SHIFT;
}

void *mem_kernel_alloc(uint64_t size, uint64_t align) {
    if (!L.pool) rt_fatal("mem_kernel_alloc before mem_init");
    if (align > KCOMMIT || (align & (align - 1))) rt_fatal("mem_kernel_alloc: alignment %#llx", (unsigned long long)align);
    uint64_t want = size > align ? size : align;
    int k = kclass(want ? want : 1);
    if (k >= KCLASSES) rt_fatal("mem_kernel_alloc: %#llx bytes", (unsigned long long)size);
    uint64_t bytes = 1ull << (k + KMIN_SHIFT), al = bytes < KCOMMIT ? bytes : KCOMMIT;
    plat_mutex_lock(&klock);
    KFree *f = kfree_list[k];
    void *p;
    if (f) {
        kfree_list[k] = f->next;
        p = f;
    } else {
        uint64_t a = align_up(kbump ? kbump : RT_KERNEL_BASE, al);
        if (a + bytes > RT_KERNEL_BASE + RT_KERNEL_SIZE) rt_fatal("kernel region exhausted (%#llx bytes)",
                                                                  (unsigned long long)bytes);
        uint64_t need = align_up(a + bytes, KCOMMIT);
        if (!kcommitted) kcommitted = RT_KERNEL_BASE;
        if (need > kcommitted) {
            commit(kcommitted, need - kcommitted, "kernel region");
            kcommitted = need;
        }
        kbump = a + bytes;
        p = (void *)(uintptr_t)a;
    }
    plat_mutex_unlock(&klock);
    memset(p, 0, size ? size : 1);
    return p;
}

void mem_kernel_free(void *p, uint64_t size) {
    if (!p) return;
    uint64_t a = (uintptr_t)p;
    if (a - RT_KERNEL_BASE >= RT_KERNEL_SIZE) rt_fatal("mem_kernel_free(%p): not in the kernel region", p);
    int k = kclass(size ? size : 1);
    plat_mutex_lock(&klock);
    KFree *f = p;
    f->next = kfree_list[k];
    kfree_list[k] = f;
    plat_mutex_unlock(&klock);
}

void mem_lazy_heap_report(uint64_t frame) {
    static uint64_t faults_prv, bytes_prv;
    if (!heap_lazy) return;
    plat_mutex_lock(&frz_lock);
    uint64_t faults = heap_lazy_faults, bytes = heap_lazy_bytes, ns = heap_lazy_ns, max = heap_lazy_max_ns;
    heap_lazy_ns = heap_lazy_max_ns = 0;
    plat_mutex_unlock(&frz_lock);
    if (faults == faults_prv && bytes == bytes_prv) return;
    rt_log("mem: lazy heap at frame %llu: %llu faults and %.1f MiB committed since the last line, %.2f ms in the faults "
           "(longest %.3f ms); %.0f of %.0f MiB committed", (unsigned long long)frame,
           (unsigned long long)(faults - faults_prv), (bytes - bytes_prv) / 1048576.0, ns / 1e6, max / 1e6,
           bytes / 1048576.0, __atomic_load_n(&heap_size, __ATOMIC_ACQUIRE) / 1048576.0);
    faults_prv = faults, bytes_prv = bytes;
}

int mem_guest_ranges(const char **name, RtRange *r, int max) {
    const RtGuestLayout *g = mem_layout();
    const struct { const char *name; RtRange r; } reg[] = {
        {"stack", g->stack}, {"heap", g->heap}, {"alias", g->alias}, {"kernel", g->kernel}, {"hostfn", g->hostfn},
    };
    int n = 0;
    for (unsigned i = 0; i < sizeof reg / sizeof *reg && n < max; i++)
        if (reg[i].r.size) name[n] = reg[i].name, r[n++] = reg[i].r;
    for (int i = 0; i < ldr_count() && n < max; i++) {
        const RtLoaded *m = ldr_module(i);
        if (m && m->size) name[n] = "modules", r[n++] = (RtRange){m->base, m->size};
    }
    return n;
}

void mem_guest_census_log(uint64_t frame, const char *const *name, const uint64_t *committed, int n) {
    char line[400];
    int o = snprintf(line, sizeof line, "mem: guest regions at frame %llu (MiB committed):", (unsigned long long)frame);
    uint64_t all = 0;
    for (int i = 0; i < n && o < (int)sizeof line - 40; i++) {
        int first = 1;
        for (int k = 0; k < i && first; k++) first = strcmp(name[k], name[i]) != 0;
        if (!first) continue;
        uint64_t sum = 0;
        for (int k = i; k < n; k++)
            if (!strcmp(name[k], name[i])) sum += committed[k];
        all += sum;
        o += snprintf(line + o, sizeof line - o, " %s %.0f", name[i], sum / 1048576.0);
    }
    rt_log("%s; all %.0f", line, all / 1048576.0);
}

void mem_guest_census(uint64_t frame) {
    const char *name[16];
    RtRange r[16];
    uint64_t committed[16];
    int n = mem_guest_ranges(name, r, 16);
    for (int i = 0; i < n; i++) {
        uint64_t x = r[i].base, end = r[i].base + r[i].size, b = 0;
        committed[i] = 0;
        while (x < end) {
            int st = plat_mem_state(x, end, &b);
            if (b <= x) break;
            if (st >= 0) committed[i] += b - x;
            x = b;
        }
    }
    mem_guest_census_log(frame, name, committed, n);
    mem_lazy_heap_census(frame);
}

void mem_lazy_heap_census(uint64_t frame) {
    if (heap_lazy)
        rt_log("mem: lazy heap at frame %llu: %.0f of %.0f MiB committed, by %llu faults and the rest by kernel "
               "accesses and pointer checks", (unsigned long long)frame, heap_lazy_bytes / 1048576.0,
               __atomic_load_n(&heap_size, __ATOMIC_ACQUIRE) / 1048576.0, (unsigned long long)heap_lazy_faults);
}

void mem_guest_usage(uint64_t *heap, uint64_t *heap_committed, uint64_t *phys_maps) {
    *heap = __atomic_load_n(&heap_size, __ATOMIC_RELAXED);
    *heap_committed = heap_lazy ? __atomic_load_n(&heap_lazy_bytes, __ATOMIC_RELAXED) : *heap;
    *phys_maps = __atomic_load_n(&alias_bytes, __ATOMIC_RELAXED);
}

void mem_sizes(PerfSizes *s) {
    plat_mutex_lock_shared(&lock);
    uint64_t heap = heap_size, alias = alias_bytes, stacks = nstack_maps;
    plat_mutex_unlock_shared(&lock);
    plat_mutex_lock_shared(&klock);
    uint64_t kused = kbump ? kbump - RT_KERNEL_BASE : 0, kcom = kcommitted ? kcommitted - RT_KERNEL_BASE : 0;
    plat_mutex_unlock_shared(&klock);
    plat_mutex_lock_shared(&frz_lock);
    uint64_t lz = (uint64_t)lz_n, lzc = (uint64_t)lz_cap;
    plat_mutex_unlock_shared(&frz_lock);
    perf_size(s, "guest heap", PERF_SIZE_NONE, 0, heap);
    perf_size(s, "guest physical maps", PERF_SIZE_NONE, 0, alias);
    perf_size(s, "kernel region used", PERF_SIZE_NONE, 0, kused);
    perf_size(s, "kernel region committed", PERF_SIZE_NONE, 0, kcom);
    perf_size(s, "moved stacks", stacks, sizeof stack_maps / sizeof *stack_maps, PERF_SIZE_NONE);
    perf_size(s, "lazy write-back ranges", lz, 0, PERF_SIZE_NONE);
    perf_size(s, "their array's room", lzc, 0, PERF_SIZE_NONE);
}
