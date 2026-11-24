// Contains code Copyright (c) 2026 MacNativeGames contributors, Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT;
// see THIRD_PARTY_NOTICES.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "kernel.h"
#include "prof.h"
#include "platform/platform.h"

#define R_OK 0u
#define R_TIMEOUT KERN_R_TIMEOUT
#define R_CANCELLED KERN_R_CANCELLED
#define R_BAD_HANDLE KERN_R_BAD_HANDLE
#define R_BAD_STATE KERN_R_BAD_STATE
#define R_INVALID_ENUM KERN_R_INVALID_ENUM
#define R_INVALID_ADDRESS 0xCC01u
#define WAITERS_BIT KERN_WAITERS_BIT
#define H_CUR_THREAD KERN_H_CURRENT_THREAD

#define MAX_H 4096
#define NCORE 6
#define CORE_MASK_ALL ((1ull << NCORE) - 1)
#define HOST_STACK (256ull << 20)
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400
#define TLS_SIZE 0x2000
#define MAIN_STACK (1ull << 20)
#define MAIN_PRIO 44
#define MAIN_CORE 0
#define FIBER_MAX 1024

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION = KERN_SESSION, O_TMEM = KERN_TMEM };
_Static_assert(O_EVENT_W < O_SESSION, "handle kinds");
enum { W_NONE = KERN_WAIT_NONE, W_LOCK = KERN_WAIT_LOCK, W_CONDVAR = KERN_WAIT_CONDVAR, W_ADDRESS = KERN_WAIT_ADDRESS,
       W_SYNC = KERN_WAIT_SYNC };

enum { S_OFF, S_RUN, S_CORE, S_HOST, S_SLEEP, S_SYNC, S_CV, S_LOCK, S_ADDR, S_N };
static const char *const s_name[S_N] = {"off", "run", "core", "host", "sleep", "sync", "cv", "lock", "addr"};

typedef struct Event { int signaled, refs; uint64_t serial; } Event;
typedef struct Fiber Fiber;

struct RtThread {
    int preempt;
    int host_depth;
    uint32_t handle;
    uint64_t id;
    Cpu *cpu;
    Cpu *cur;
    uint64_t entry, arg, stack_top;
    int prio, core;
    uint64_t mask;
    int started, exited, closed, cancel, is_main;
    PlatCond cv;
    int wkind, woke;
    uint64_t waddr, wkey;
    uint32_t wtag, wres;
    uint64_t wseq;
    RtThread *next;
    RtThread *all;
    int cur_core;
    uint64_t cseq;
    RtThread *cnext;
    uint8_t *tlr, *tls;
    PlatJmp exit_jmp;
    PlatFiber *base_fiber;
    Fiber *fiber;
    Fiber *leaving;
    int exit_pending;
    uint32_t tid;
    int st;
    uint64_t st_since, st_ns[S_N], nblk, npre, pre_since, cpu_last;
    uint64_t wobj;
    const uint32_t *whs;
    int wn;
    uint32_t waker;
    uint64_t blk_since;
    int blk_told;
    uint64_t wake_at, grant_at;
    uint64_t lw_ns, lw_n, lw_max, lg_ns, lg_n, lg_max, sl_n, sl_req, sl_act;
    uint64_t sp_n, sp_ho, sp_site;
    RtThread *target;
    PlatIntr *hthr;
    uint64_t ap_rip;
    uint64_t ap_last;
    int ap_pending;
    uint64_t dg_since;
    int dg_cls;
    uint64_t dg_kick;
    uint64_t dg_host_end;
    uint64_t dg_pc, dg_lr;
    int dg_host_kick;
    uint32_t dg_svc;
    struct DgRel { uint64_t kick, host_end, pc, lr; int host, pre, prio; uint32_t h; } dg_rel;
};

static int core_current(const RtThread *t) { return __atomic_load_n(&t->cur_core, __ATOMIC_RELAXED); }
static int core_current_unlocked(const RtThread *t) { return __atomic_load_n(&t->cur_core, __ATOMIC_ACQUIRE); }

static PlatMutex G = PLAT_MUTEX_INIT;
static struct { int type; void *obj; } g_h[MAX_H];
static _Thread_local RtThread *tcur;

__attribute__((noinline)) static RtThread *tcur_now(void) { return tcur; }
_Static_assert(offsetof(RtThread, preempt) == 0, "host.h kern_preempt_pending reads the first int");
_Static_assert(offsetof(RtThread, host_depth) == sizeof(int), "host.h kern_host_depth reads the second int");
static RtThread *g_all, *g_wait, *g_main;
static uint64_t g_next_id = 0x51, g_wseq, g_created;
static uint64_t g_ev_serial;
static uint64_t g_main_stack;

static void lock(void) { plat_mutex_lock(&G); }
static void unlock(void) { plat_mutex_unlock(&G); }

uint64_t kern_now_ns(void) { return plat_now_ns(); }

typedef struct { uint64_t ts, obj; uint32_t a, b; uint16_t kind, st; uint32_t pad; } CritEv;
enum { CRIT_STATE, CRIT_WAKE, CRIT_FRAME };
#define CRIT_N (1u << 22)
static CritEv *g_crit;
static uint32_t g_crit_n;
static int g_crit_on;
static uint64_t g_crit_lo, g_crit_hi;
static int64_t g_crit_wlo, g_crit_whi;

static void crit_add(int kind, uint32_t a, uint32_t b, int st, uint64_t obj, uint64_t ts) {
    if (g_crit_n < CRIT_N) g_crit[g_crit_n++] = (CritEv){ts, obj, a, b, (uint16_t)kind, (uint16_t)st, 0};
}

#define RW_N 32
static RtThread *g_render;
typedef struct { int st; uint32_t waker; uint64_t obj, ns; } WaitRec;
static WaitRec g_rw[RW_N], g_mw[RW_N];
static uint64_t g_pre_req, g_pre_n, g_pre_ns, g_pre_max, g_pre_stale;
static uint64_t g_sync_calls, g_sync_blocks;
static uint64_t g_sync_woken, g_sync_spared;

static void wait_add(WaitRec *w, int st, uint64_t obj, uint32_t waker, uint64_t ns) {
    for (int i = 0; i < RW_N; i++)
        if (!w[i].ns || (w[i].st == st && w[i].obj == obj && w[i].waker == waker)) {
            w[i].st = st;
            w[i].obj = obj;
            w[i].waker = waker;
            w[i].ns += ns;
            return;
        }
}

static void acct(RtThread *t, int st) {
    if (!(rt_trace & RT_TRACE_SCHED)) return;
    uint64_t now = kern_now_ns();
    if (t->st_since) {
        t->st_ns[t->st] += now - t->st_since;
        if (t->st >= S_HOST && (t == g_render || t->is_main)) {
            uint32_t waker = t->st == S_HOST || t->st == S_SLEEP ? 0 : t->waker;
            wait_add(t == g_render ? g_rw : g_mw, t->st, t->wobj, waker, now - t->st_since);
        }
    }
    if (g_crit_on) crit_add(CRIT_STATE, t->handle, t->waker, st, t->wobj, now);
    t->st = st;
    t->st_since = now;
}

static int st_of(int kind) {
    return kind == W_LOCK ? S_LOCK : kind == W_CONDVAR ? S_CV : kind == W_ADDRESS ? S_ADDR : S_SYNC;
}

static uint32_t h_new(int type, void *obj) {
    for (int i = 1; i < MAX_H; i++)
        if (!g_h[i].type) {
            g_h[i].type = type;
            g_h[i].obj = obj;
            return 0x1000 + i;
        }
    rt_fatal("kernel: handle table full (%d handles)", MAX_H);
}

static void *h_peek(uint32_t h, int type) {
    if (h == H_CUR_THREAD && type == O_THREAD) return tcur;
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}

static void *h_get(uint32_t h, int type) {
    void *o = h_peek(h, type);
    if (!o) rt_log("kernel: invalid handle %#x (want kind %d)", h, type);
    return o;
}

static RtThread *cur(uint32_t svc) {
    if (!tcur) rt_fatal("kernel: svc %#x on a host thread (no guest thread)", svc);
    return tcur;
}

static void wait_unlink(RtThread *t) {
    for (RtThread **p = &g_wait; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    t->next = NULL;
}

static RtThread *pick(int kind, uint64_t a) {
    RtThread *b = NULL;
    for (RtThread *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == W_CONDVAR ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}

static void wake(RtThread *t, uint32_t res) {
    wait_unlink(t);
    t->wkind = W_NONE;
    t->woke = 1;
    t->wres = res;
    t->waker = tcur ? tcur->handle : 0;
    if (rt_trace & RT_TRACE_SCHED) t->wake_at = kern_now_ns();
    if (g_crit_on) crit_add(CRIT_WAKE, t->handle, t->waker, 0, 0, t->wake_at);
    plat_cond_signal(&t->cv);
}

static int sync_watches(const RtThread *w, const void *obj) {
    for (int i = 0; i < w->wn; i++) {
        unsigned k = w->whs[i] - 0x1000;
        if (k > 0 && k < MAX_H && g_h[k].obj == obj && (g_h[k].type == O_EVENT_R || g_h[k].type == O_THREAD)) return 1;
    }
    return 0;
}

static void wake_sync_waiters(const void *obj) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("SYNC_WAKE_ALL", "0");
    int all = RT_KNOB_ON(mode);
    for (RtThread *w = g_wait; w;) {
        RtThread *n = w->next;
        if (w->wkind == W_SYNC) {
            if (all || sync_watches(w, obj)) g_sync_woken++, wake(w, (uint32_t)-1);
            else g_sync_spared++;
        }
        w = n;
    }
}

static uint32_t block_inner(RtThread *t, int kind, int64_t ns) {
    t->wkind = kind;
    t->woke = 0;
    t->wseq = ++g_wseq;
    t->next = g_wait;
    g_wait = t;
    if (ns == 0) {
        wait_unlink(t);
        t->wkind = W_NONE;
        return R_TIMEOUT;
    }
    uint64_t now = ns < 0 ? 0 : kern_now_ns();
    int forever = ns < 0 || (uint64_t)ns > UINT64_MAX - now;
    uint64_t deadline = forever ? 0 : now + (uint64_t)ns;
    while (!t->woke) {
        if (forever) {
            plat_cond_wait(&t->cv, &G);
            continue;
        }
        now = kern_now_ns();
        if (now >= deadline) {
            wait_unlink(t);
            t->wkind = W_NONE;
            return R_TIMEOUT;
        }
        uint64_t ms = (deadline - now + 999999) / 1000000;
        plat_cond_wait_ms(&t->cv, &G, ms > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)ms);
    }
    if (t->wake_at) {
        uint64_t d = kern_now_ns() - t->wake_at;
        t->lw_ns += d;
        t->lw_n++;
        if (d > t->lw_max) t->lw_max = d;
        t->wake_at = 0;
    }
    return t->wres;
}

static RtThread *g_core_owner[NCORE], *g_core_wait;
static int g_core_waiting;
static int g_cores_on = -1, g_held_max;
static int g_active_cores = NCORE;

static void diag_init(void);
static void ap_init(void);

static int cores_enabled(void) {
    if (g_cores_on < 0) {
        g_cores_on = !cfg_bool("NX2SARIA_FREE_CORES");
        const char *n = cfg_get("NX2SARIA_CORES");
        if (n && atoi(n) >= 1 && atoi(n) <= NCORE) {
            g_active_cores = atoi(n);
            rt_log("kernel: %d virtual cores run threads (NX2SARIA_CORES); higher cores fold onto them", g_active_cores);
        }
        if (!g_cores_on)
            rt_log("kernel: WARNING: free_cores = 1 (NX2SARIA_FREE_CORES) turns the virtual core gate off. This is an "
                   "experimental mode and the game crashes or hangs in it, usually at start. Set free_cores = 0 in "
                   "nx2saria.ini (or an existing nx2navi.ini) to play");
        diag_init();
        ap_init();
    }
    return g_cores_on;
}

void kern_free_cores_note(void) {
    if (!g_cores_on)
        rt_log("fatal: free_cores = 1 was on: the game crashes in this experimental mode; set free_cores = 0 in "
               "nx2saria.ini (or an existing nx2navi.ini) before reporting this crash");
}

static int core_allowed(const RtThread *t, int c) {
    if (c >= g_active_cores) return 0;
    for (int j = c; j < NCORE; j += g_active_cores)
        if ((t->mask >> j) & 1) return 1;
    return 0;
}

static int core_seen(const RtThread *t, int c) {
    if (g_active_cores >= NCORE) return c;
    if (t->core >= 0 && t->core < NCORE && t->core % g_active_cores == c && ((t->mask >> t->core) & 1)) return t->core;
    for (int j = c; j < NCORE; j += g_active_cores)
        if ((t->mask >> j) & 1) return j;
    return c;
}

// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
#define AP_FIRST_NS 250000ull
#define AP_GAP_NS 1000000ull
static int g_ap = -1, g_ap_int;
static int g_ap_log;
#define AP_LOG_N 40
static uintptr_t g_gen_lo, g_gen_hi;
static uint64_t g_ap_sent, g_ap_taken, g_ap_host, g_ap_pc, g_ap_deferred, g_ap_fail, g_ap_spin_ok;
void kern_ap_stub_ymm(void), kern_ap_stub_xmm(void);
static void (*ap_stub)(void) = kern_ap_stub_xmm;

static int cpu_keeps_ymm(void) {
    unsigned a = 1, b, c = 0, d;
    __asm__("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if ((c & 0x18000000u) != 0x18000000u) return 0;
    unsigned lo, hi;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;
}

static void ap_init(void) {
    if (g_ap >= 0) return;
    const char *e = cfg_value("NX2SARIA_ASYNC_PREEMPT");
    g_ap = strcmp(e, "0") != 0;
    g_ap_int = g_ap && strcmp(e, "2") != 0;
    if (g_ap) {
        RtModule m[16];
        int n = rt_linked_modules(m, 16);
        uintptr_t lo = UINTPTR_MAX, hi = 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < m[i].count; k++) {
                uintptr_t f = (uintptr_t)m[i].fns[k];
                if (f < lo) lo = f;
                if (f > hi) hi = f;
            }
        ap_stub = cpu_keeps_ymm() ? kern_ap_stub_ymm : kern_ap_stub_xmm;
        const uintptr_t host[] = {(uintptr_t)kern_ap_stub_ymm, (uintptr_t)kern_ap_stub_xmm, (uintptr_t)kern_checkpoint,
                                  (uintptr_t)rt_svc, (uintptr_t)rt_dispatch, (uintptr_t)rt_call};
        int bad = !n || lo >= hi;
        for (unsigned i = 0; i < sizeof host / sizeof *host; i++) bad |= host[i] >= lo && host[i] <= hi;
        if (bad) {
            rt_log("kernel: asynchronous preemption off: the lifted code range %#llx-%#llx holds host code",
                   (unsigned long long)lo, (unsigned long long)hi);
            g_ap = 0;
        } else {
            g_gen_lo = lo;
            g_gen_hi = hi;
            kern_host_track = 1;
        }
    }
    rt_log("kernel: asynchronous preemption of virtual cores %s, priority inheritance and preemption disable %s; lifted "
           "code exe+%#llx-exe+%#llx; an interrupted core's %s registers are kept", g_ap_int ? "on" : "off",
           g_ap ? "on (NX2SARIA_ASYNC_PREEMPT=0 turns all off)" : "off",
           (unsigned long long)(g_gen_lo ? g_gen_lo - plat_exe_base() : 0),
           (unsigned long long)(g_gen_hi ? g_gen_hi - plat_exe_base() : 0), ap_stub == kern_ap_stub_ymm ? "YMM" : "XMM");
}

static int eff_prio(const RtThread *o) {
    int p = o->prio;
    for (const RtThread *w = g_wait; w; w = w->next)
        if (w->wkind == W_LOCK && w->prio < p && w->waddr &&
            (__atomic_load_n((const uint32_t *)(uintptr_t)w->waddr, __ATOMIC_RELAXED) & ~WAITERS_BIT) == o->handle)
            p = w->prio;
    return p;
}

static int prio_of(const RtThread *t) { return g_ap > 0 ? eff_prio(t) : t->prio; }

static volatile uint16_t *ap_tlr(const RtThread *t) { return (volatile uint16_t *)(t->tlr + 0x100); }
static int ap_disabled(const RtThread *t) { return g_ap > 0 && ap_tlr(t)[0]; }

#define AP_NAME "kern_ap_stub_ymm"
#define AP_VMOV "vmovdqa"
#define AP_V "%ymm"
#define AP_VZEROUPPER "    vzeroupper\n"
#include "kernel_ap_stub.h"
#undef AP_NAME
#undef AP_VMOV
#undef AP_V
#undef AP_VZEROUPPER
#define AP_NAME "kern_ap_stub_xmm"
#define AP_VMOV "movdqa"
#define AP_V "%xmm"
#define AP_VZEROUPPER ""
#include "kernel_ap_stub.h"

enum { AP_NONE, AP_RETRY, AP_SENT };

static int ap_try(RtThread *w) {
    RtThread *o = w->target;
    if (!o || core_current(o) < 0 || !o->preempt) return AP_NONE;
    if (!o->hthr || o->ap_pending) return AP_RETRY;
    volatile uint16_t *tl = ap_tlr(o);
    if (__atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0) {
        g_ap_host++;
        return AP_RETRY;
    }
    if (tl[0]) {
        tl[1] = 1;
        g_ap_deferred++;
        return AP_RETRY;
    }
    uint64_t now = kern_now_ns();
    if (o->ap_last && now - o->ap_last < AP_GAP_NS) return AP_RETRY;
    o->ap_last = now;
    uint64_t rip, rsp;
    if (!plat_intr_stop(o->hthr, &rip, &rsp)) {
        g_ap_fail++;
        return AP_RETRY;
    }
    int r = AP_RETRY;
    uint64_t to = 0;
    if (rip < g_gen_lo || rip > g_gen_hi)
        g_ap_pc++;
    else if (__atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0)
        g_ap_host++;
    else if (tl[0]) {
        tl[1] = 1;
        g_ap_deferred++;
    } else {
        o->ap_rip = rip;
        o->ap_pending = 1;
        to = (uint64_t)(uintptr_t)ap_stub;
    }
    if (!plat_intr_resume(o->hthr, to)) {
        o->ap_pending = 0;
        g_ap_fail++;
    } else if (to) {
        g_ap_sent++;
        r = AP_SENT;
    }
    if (r == AP_SENT && g_ap_log < AP_LOG_N) {
        g_ap_log++;
        rt_log("kernel: interrupt %#x p%d at exe+%#llx rsp %#llx for %#x p%d", o->handle, o->prio,
               (unsigned long long)(o->ap_rip - plat_exe_base()), (unsigned long long)rsp, w->handle, w->prio);
    }
    return r;
}

enum { DG_GAME, DG_HOST, DG_EQ, DG_NOCORE, DG_N };
static const char *const dg_name[DG_N] = {"game", "host", "eq", "nocore"};
#define DG_NB 8
static const uint64_t dg_edge[DG_NB - 1] = {50000, 250000, 1000000, 2000000, 4000000, 8000000, 16000000};
#define DG_LONG_LINES 400
#define DG_SAMPLES 600
#define DG_OWN 32
int kern_host_track;
static int g_diag = -1;
typedef struct {
    uint64_t ns[DG_N], n[DG_N], hist[DG_N][DG_NB];
    struct { uint32_t h; int prio, cls; uint64_t ns; } own[DG_OWN];
    uint64_t long_frames, long_game_frames;
} DgWindow;
static DgWindow g_dgw;
static uint64_t g_dgf[DG_N], g_dgfr[DG_N];
static uint64_t g_dg_samples, g_dg_lines, g_sync36, g_sync36_flag;

static void diag_init(void) {
    if (g_diag >= 0) return;
    const char *e = cfg_get("NX2SARIA_CORE_DIAG");
    g_diag = e && *e && strcmp(e, "0");
    if (g_diag) {
        kern_host_track = 1;
        rt_log("core-diag: on (NX2SARIA_CORE_DIAG)");
    }
}

int kern_core_diag_on(void) { return g_diag > 0; }

void kern_host_left(void *thread) {
    RtThread *t = thread;
    if (g_diag > 0 && t->dg_kick && t->dg_host_kick && !t->dg_host_end) t->dg_host_end = kern_now_ns();
}

static int dg_in_host(const RtThread *o) { return __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0; }

static void dg_add(const RtThread *w, int cls, uint64_t ns, uint32_t oh, int oprio) {
    if (!ns) return;
    g_dgw.ns[cls] += ns;
    g_dgf[cls] += ns;
    if (w == g_render) g_dgfr[cls] += ns;
    if (!oh) return;
    int i = 0;
    for (; i < DG_OWN && g_dgw.own[i].ns; i++)
        if (g_dgw.own[i].h == oh && g_dgw.own[i].cls == cls) break;
    if (i == DG_OWN) return;
    g_dgw.own[i].h = oh;
    g_dgw.own[i].prio = oprio;
    g_dgw.own[i].cls = cls;
    g_dgw.own[i].ns += ns;
}

static int dg_class0(const RtThread *t) {
    if (t->target) return dg_in_host(t->target) ? DG_HOST : DG_GAME;
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i] && g_core_owner[i]->prio > t->prio) return DG_NOCORE;
    return DG_EQ;
}

static void dg_done(RtThread *t) {
    uint64_t now = kern_now_ns(), t0 = t->dg_since, w = now - t0;
    t->dg_since = 0;
    if (!t0) return;
    const struct DgRel *r = &t->dg_rel;
    uint64_t part[DG_N] = {0};
    uint32_t oh = t->target ? t->target->handle : r->h;
    int oprio = t->target ? t->target->prio : r->prio;
    if (r->pre && r->kick) {
        uint64_t k = r->kick > t0 ? (r->kick < now ? r->kick : now) : t0;
        part[t->dg_cls] += k - t0;
        if (r->host) {
            uint64_t he = r->host_end > k && r->host_end < now ? r->host_end : now;
            part[DG_HOST] += he - k;
            part[DG_GAME] += now - he;
        } else
            part[DG_GAME] += now - k;
        oh = r->h;
        oprio = r->prio;
    } else
        part[t->dg_cls] += w;
    int top = 0;
    for (int c = 0; c < DG_N; c++) {
        dg_add(t, c, part[c], oh, oprio);
        if (part[c] > part[top]) top = c;
    }
    int b = 0;
    while (b < DG_NB - 1 && w >= dg_edge[b]) b++;
    g_dgw.n[top]++;
    g_dgw.hist[top][b]++;
    if ((part[DG_GAME] >= 2000000 || part[DG_HOST] >= 2000000) && g_dg_lines < DG_LONG_LINES) {
        g_dg_lines++;
        char a[128], l[128];
        rt_log("core-diag: wait %.2f ms (game %.2f host %.2f): %#x p%d mask %llx; released by %#x p%d%s at %s lr %s",
               w / 1e6, part[DG_GAME] / 1e6, part[DG_HOST] / 1e6, t->handle, t->prio, (unsigned long long)t->mask, r->h,
               r->prio, r->pre ? " (preempted)" : "", r->pc ? ldr_describe(r->pc, a, sizeof a) : "-",
               r->lr ? ldr_describe(r->lr, l, sizeof l) : "-");
    }
    t->target = NULL;
    memset(&t->dg_rel, 0, sizeof t->dg_rel);
}

static void dg_sample(RtThread *t) {
    RtThread *o = t->target;
    if (!o || core_current(o) < 0 || !o->hthr || g_dg_samples >= DG_SAMPLES) return;
    uint64_t rip, rsp;
    if (!plat_intr_stop(o->hthr, &rip, &rsp)) return;
    int depth = __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED);
    plat_intr_resume(o->hthr, 0);
    g_dg_samples++;
    char mn[256] = "?";
    uintptr_t m = 0;
    plat_module_of((uintptr_t)rip, mn, sizeof mn, &m);
    char lr[128];
    rt_log("core-diag: sample owner %#x p%d depth %d rip %s+%#llx, last svc %#x, guest lr %s (waiter %#x p%d, %s, "
           "%.2f ms)", o->handle, o->prio, depth, mn, (unsigned long long)(rip - m), o->dg_svc,
           o->cur ? ldr_describe(o->cur->x[30], lr, sizeof lr) : "-", t->handle, t->prio,
           t->dg_cls == DG_HOST ? "host" : "game", (kern_now_ns() - t->dg_since) / 1e6);
}


void kern_core_diag_frame(uint64_t frame, uint64_t interval_ns) {
    if (g_ap > 0 && frame && frame % 600 == 0) {
        lock();
        uint64_t v[8] = {g_ap_sent, g_ap_taken, g_ap_host, g_ap_pc, g_ap_deferred, g_ap_fail, g_ap_spin_ok, g_sync36};
        unlock();
        rt_log("kernel: frame %llu: asynchronous preemption: %llu interrupted, %llu cores given up there; refused: "
               "%llu in a host call, %llu outside game code, %llu preemption disabled, %llu failed; %llu waits ended "
               "in the first spin; SVC 0x36 %llu",
               (unsigned long long)frame, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
               (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6],
               (unsigned long long)v[7]);
    }
    if (g_diag <= 0) return;
    DgWindow w;
    uint64_t f[DG_N], fr[DG_N], s36, s36f;
    lock();
    g_render = tcur;
    memcpy(f, g_dgf, sizeof f);
    memcpy(fr, g_dgfr, sizeof fr);
    memset(g_dgf, 0, sizeof g_dgf);
    memset(g_dgfr, 0, sizeof g_dgfr);
    int lng = interval_ns > 25000000ull;
    if (lng) {
        g_dgw.long_frames++;
        if (f[DG_GAME] > 1000000) g_dgw.long_game_frames++;
    }
    int rep = frame && frame % 60 == 0;
    if (rep) {
        w = g_dgw;
        memset(&g_dgw, 0, sizeof g_dgw);
    }
    s36 = g_sync36;
    s36f = g_sync36_flag;
    unlock();
    if (lng)
        rt_log("core-diag: frame %llu: %.1f ms; core wait game %.2f host %.2f eq %.2f nocore %.2f ms; render thread game "
               "%.2f host %.2f eq %.2f nocore %.2f ms",
               (unsigned long long)frame, interval_ns / 1e6, f[0] / 1e6, f[1] / 1e6, f[2] / 1e6, f[3] / 1e6,
               fr[0] / 1e6, fr[1] / 1e6, fr[2] / 1e6, fr[3] / 1e6);
    if (!rep) return;
    char s[1024];
    int o = 0;
    for (int c = 0; c < DG_N; c++) {
        o += snprintf(s + o, sizeof s - o, "%s %.3f ms/frame %llu waits [", dg_name[c], w.ns[c] / 60e6,
                      (unsigned long long)w.n[c]);
        for (int b = 0; b < DG_NB; b++)
            o += snprintf(s + o, sizeof s - o, "%s%llu", b ? " " : "", (unsigned long long)w.hist[c][b]);
        o += snprintf(s + o, sizeof s - o, "]%s", c < DG_N - 1 ? "; " : "");
    }
    rt_log("core-diag: frames %llu-%llu: %s; long frames %llu (game wait > 1 ms in %llu); SVC 0x36 %llu (flag set %llu)",
           (unsigned long long)frame - 59, (unsigned long long)frame, s, (unsigned long long)w.long_frames,
           (unsigned long long)w.long_game_frames, (unsigned long long)s36, (unsigned long long)s36f);
    o = 0;
    for (int k = 0; k < 6; k++) {
        int bi = -1;
        for (int i = 0; i < DG_OWN; i++)
            if (w.own[i].ns && (bi < 0 || w.own[i].ns > w.own[bi].ns)) bi = i;
        if (bi < 0) break;
        o += snprintf(s + o, sizeof s - o, " %#x p%d %s %.3f;", w.own[bi].h, w.own[bi].prio, dg_name[w.own[bi].cls],
                      w.own[bi].ns / 60e6);
        w.own[bi].ns = 0;
    }
    if (o) rt_log("core-diag: owners (ms/frame):%s", s);
}

static void core_count(void) {
    int n = 0;
    for (int i = 0; i < NCORE; i++) n += g_core_owner[i] != NULL;
    if (n > g_held_max) g_held_max = n;
}

static struct DgRel g_dg_rel;

static void core_grant(int c) {
    RtThread *b = NULL, **bp = NULL;
    int bpri = 0;
    for (RtThread **p = &g_core_wait; *p; p = &(*p)->cnext) {
        if (!core_allowed(*p, c)) continue;
        int pr = prio_of(*p);
        if (!b || pr < bpri || (pr == bpri && (*p)->cseq < b->cseq)) {
            b = *p;
            bp = p;
            bpri = pr;
        }
    }
    if (!b) return;
    *bp = b->cnext;
    __atomic_store_n(&g_core_waiting, g_core_wait != NULL, __ATOMIC_RELEASE);
    b->cnext = NULL;
    __atomic_store_n(&b->cur_core, c, __ATOMIC_RELEASE);
    g_core_owner[c] = b;
    core_count();
    if (g_diag > 0) b->dg_rel = g_dg_rel;
    if (rt_trace & RT_TRACE_SCHED) b->grant_at = kern_now_ns();
    plat_cond_signal(&b->cv);
}

static void core_put(RtThread *t) {
    int c = core_current(t);
    if (c < 0) return;
    if (t->preempt && t->pre_since) {
        uint64_t d = kern_now_ns() - t->pre_since;
        g_pre_n++;
        g_pre_ns += d;
        if (d > g_pre_max) g_pre_max = d;
        t->npre++;
    }
    t->pre_since = 0;
    if (g_diag > 0) {
        g_dg_rel = (struct DgRel){.kick = t->dg_kick, .host_end = t->dg_host_end, .pc = t->dg_pc, .lr = t->dg_lr,
                                  .host = t->dg_host_kick, .pre = t->preempt, .prio = t->prio, .h = t->handle};
        t->dg_kick = t->dg_host_end = t->dg_pc = t->dg_lr = 0;
        t->dg_host_kick = 0;
    }
    __atomic_store_n(&t->cur_core, -1, __ATOMIC_RELAXED);
    t->preempt = 0;
    g_core_owner[c] = NULL;
    for (RtThread *w = g_core_wait; w; w = w->cnext)
        if (w->target == t) w->target = NULL;
    core_grant(c);
}

static int core_better_waiter(const RtThread *t, int c) {
    int tp = prio_of(t);
    for (const RtThread *w = g_core_wait; w; w = w->cnext)
        if (core_allowed(w, c) && prio_of(w) < tp) return 1;
    return 0;
}

static void core_kick(void) {
    RtThread *w[64];
    int wp[64], op[NCORE], n = 0, claimed = 0;
    for (int i = 0; i < NCORE; i++) op[i] = g_core_owner[i] ? prio_of(g_core_owner[i]) : 0;
    for (RtThread *p = g_core_wait; p && n < 64; p = p->cnext) {
        int i = n++, pp = prio_of(p);
        for (; i > 0 && (wp[i - 1] > pp || (wp[i - 1] == pp && w[i - 1]->cseq > p->cseq)); i--) {
            w[i] = w[i - 1];
            wp[i] = wp[i - 1];
        }
        w[i] = p;
        wp[i] = pp;
    }
    for (int k = 0; k < n && claimed != (1 << NCORE) - 1; k++) {
        int v = -1;
        for (int i = 0; i < NCORE; i++) {
            RtThread *o = g_core_owner[i];
            if ((claimed >> i & 1) || !core_allowed(w[k], i) || !o || op[i] <= wp[k]) continue;
            RtThread *b = v < 0 ? NULL : g_core_owner[v];
            if (!b || op[i] > op[v] || (op[i] == op[v] && o->preempt && !b->preempt)) v = i;
        }
        if (v < 0) continue;
        claimed |= 1 << v;
        RtThread *o = g_core_owner[v];
        w[k]->target = o;
        if (o->preempt) continue;
        if (rt_trace & RT_TRACE_SCHED) {
            o->pre_since = kern_now_ns();
            g_pre_req++;
        }
        if (g_diag > 0) {
            o->dg_kick = kern_now_ns();
            o->dg_host_kick = dg_in_host(o);
            o->dg_host_end = 0;
        }
        __atomic_store_n(&o->preempt, 1, __ATOMIC_RELEASE);
    }
}

static void core_take(RtThread *t) {
    if (core_current(t) >= 0) return;
    if (!cores_enabled()) {
        acct(t, S_RUN);
        return;
    }
    if (!(t->mask & ((1u << NCORE) - 1)))
        rt_fatal("kernel: thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++)
        if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    if (c >= 0) {
        __atomic_store_n(&t->cur_core, c, __ATOMIC_RELAXED);
        g_core_owner[c] = t;
        core_count();
        acct(t, S_RUN);
        return;
    }
    acct(t, S_CORE);
    t->cseq = ++g_wseq;
    t->cnext = g_core_wait;
    g_core_wait = t;
    __atomic_store_n(&g_core_waiting, 1, __ATOMIC_RELEASE);
    t->target = NULL;
    if (g_diag > 0) {
        t->dg_since = kern_now_ns();
        memset(&t->dg_rel, 0, sizeof t->dg_rel);
    }
    core_kick();
    if (g_diag > 0) t->dg_cls = dg_class0(t);
    if (g_ap_int && t->target && core_current(t) < 0) {
        uint64_t end = kern_now_ns() + AP_FIRST_NS;
        unlock();
        while (__atomic_load_n(&t->cur_core, __ATOMIC_ACQUIRE) < 0 && kern_now_ns() < end) plat_pause();
        lock();
        if (core_current(t) >= 0) g_ap_spin_ok++;
    }
    int sampled = 0;
    while (core_current(t) < 0) {
        uint32_t ms = 0;
        if (g_ap_int && ap_try(t) != AP_NONE) ms = 1;
        int sample = g_diag > 0 && !sampled && (t->dg_cls == DG_GAME || t->dg_cls == DG_HOST);
        if (sample && !ms) ms = 2;
        if (ms) plat_cond_wait_ms(&t->cv, &G, ms);
        else plat_cond_wait(&t->cv, &G);
        if (sample && core_current(t) < 0 && kern_now_ns() - t->dg_since >= 2000000) {
            sampled = 1;
            dg_sample(t);
        }
    }
    if (g_diag > 0) dg_done(t);
    if (t->grant_at) {
        uint64_t d = kern_now_ns() - t->grant_at;
        t->lg_ns += d;
        t->lg_n++;
        if (d > t->lg_max) t->lg_max = d;
        t->grant_at = 0;
    }
    t->target = NULL;
    acct(t, S_RUN);
}

static uint32_t block(RtThread *t, int kind, int64_t ns) {
    t->waker = 0;
    if (ns == 0) return block_inner(t, kind, ns);
    core_put(t);
    acct(t, st_of(kind));
    t->nblk++;
    t->blk_since = kern_now_ns(), t->blk_told = 0;
    uint32_t r = block_inner(t, kind, ns);
    t->blk_since = 0;
    core_take(t);
    return r;
}

#define WAIT_WATCH_LINES 24

static void wait_watch_holder(char *out, size_t cap, const RtThread *h) {
    if (!h) { snprintf(out, cap, "no such thread"); return; }
    if (h->exited) { snprintf(out, cap, "thread %#x, which has ended", h->handle); return; }
    if (!h->blk_since) { snprintf(out, cap, "thread %#x (priority %d), not waiting", h->handle, h->prio); return; }
    static const char *const kinds[] = {"nothing", "a lock", "a condition variable", "an address", "a handle"};
    int k = h->wkind >= 0 && h->wkind <= W_SYNC ? h->wkind : 0;
    snprintf(out, cap, "thread %#x (priority %d), itself waiting %.1f s for %s (%#llx)", h->handle, h->prio,
             (kern_now_ns() - h->blk_since) / 1e9, kinds[k], (unsigned long long)h->wobj);
}

#define WAIT_LINE_CAP 384
static int wait_pass(uint64_t limit, int max, int mark, char *out, size_t cap) {
    int lines = 0;
    size_t used = 0;
    uint64_t now = kern_now_ns();
    out[0] = 0;
    for (RtThread *t = g_all; t && lines < max; t = t->all) {
        if (!t->blk_since || (mark && t->blk_told) || now - t->blk_since < limit) continue;
        char who[160], line[WAIT_LINE_CAP];
        int n;
        if (t->wkind == W_LOCK) {
            wait_watch_holder(who, sizeof who, h_peek((uint32_t)t->wobj, O_THREAD));
            n = snprintf(line, sizeof line, "kernel: thread %#x (priority %d) has waited %.0f s for the lock at %#llx held "
                         "by %s\n", t->handle, t->prio, (now - t->blk_since) / 1e9, (unsigned long long)t->waddr, who);
        } else if (t->wkind == W_SYNC && t->whs) {
            const RtThread *end = NULL;
            for (int i = 0; i < t->wn && !end; i++) end = h_peek(t->whs[i], O_THREAD);
            if (!end) continue;
            wait_watch_holder(who, sizeof who, end);
            n = snprintf(line, sizeof line, "kernel: thread %#x (priority %d) has waited %.0f s for the end of %s\n",
                         t->handle, t->prio, (now - t->blk_since) / 1e9, who);
        } else {
            continue;
        }
        if (n <= 0 || (size_t)n >= sizeof line || used + (size_t)n >= cap) break;
        memcpy(out + used, line, (size_t)n + 1);
        used += (size_t)n;
        if (mark) t->blk_told = 1;
        lines++;
    }
    return lines;
}

static void wait_lines_log(const char *p) {
    for (const char *e; *p && (e = strchr(p, '\n')); p = e + 1) rt_log("%.*s", (int)(e - p), p);
}

static void wait_watch(void *arg) {
    (void)arg;
    uint64_t limit = (uint64_t)atoi(cfg_value("NX2SARIA_WAIT_WATCH_S")) * 1000000000ull;
    if (!limit) return;
    int lines = 0;
    while (lines < WAIT_WATCH_LINES) {
        plat_wait_ns(2000000000ull);
        char out[WAIT_WATCH_LINES * WAIT_LINE_CAP];
        lock();
        lines += wait_pass(limit, WAIT_WATCH_LINES - lines, 1, out, sizeof out);
        unlock();
        wait_lines_log(out);
    }
    rt_log("kernel: %d long waits logged: no more", lines);
}

void kern_wait_report(void) {
    int got = 0;
    for (int i = 0; i < 10 && !(got = plat_mutex_trylock(&G)); i++) plat_sleep_ms(10);
    if (!got) {
        rt_log("kernel: the thread lock stayed taken for 100 ms; the guest threads' waits are not listed");
        return;
    }
    char out[16 * WAIT_LINE_CAP];
    int lines = wait_pass(0, 16, 0, out, sizeof out);
    unlock();
    wait_lines_log(out);
    if (!lines) rt_log("kernel: no guest thread waits for a lock or for another thread's end");
}

int kern_core_away(void) {
    RtThread *t = tcur;
    if (!t || core_current_unlocked(t) < 0) return 0;
    lock();
    core_put(t);
    acct(t, S_HOST);
    if (rt_trace & RT_TRACE_SCHED)
        t->wobj = (uintptr_t)__builtin_return_address(0) - plat_exe_base();
    unlock();
    return 1;
}

void kern_core_back(int *away) {
    RtThread *t = tcur;
    if (!*away || !t) return;
    lock();
    core_take(t);
    unlock();
    *away = 0;
}

// Contains code Copyright (c) 2026 MacNativeGames contributors, Copyright (c) 2026 seedds (MIT; see
// THIRD_PARTY_NOTICES.md).
#define SPIN_GAP_NS 20000ull
static int g_spin_on = -1;
static uint64_t g_spin_handoff_ns;
static _Thread_local uint64_t sw_head, sw_t0, sw_last;

void rt_spin_gate(Cpu *c, uint64_t head) {
    (void)c;
    plat_pause();
    RtThread *t = tcur;
    if (!t) return;
    if (g_spin_on < 0) {
        const char *e = cfg_get("NX2SARIA_SPIN_HANDOFF_US");
        g_spin_handoff_ns = (uint64_t)(e && *e ? atoll(e) : 50) * 1000;
        const char *spin = cfg_get("NX2SARIA_SPIN");
        g_spin_on = !(spin && *spin && !cfg_bool("NX2SARIA_SPIN"));
    }
    t->sp_n++;
    t->sp_site = head;
    uint64_t now = kern_now_ns();
    if (head != sw_head || now - sw_last > SPIN_GAP_NS) {
        sw_head = head;
        sw_t0 = now;
    }
    sw_last = now;
    if (!g_spin_on || core_current_unlocked(t) < 0 || now - sw_t0 < g_spin_handoff_ns ||
        !__atomic_load_n(&g_core_waiting, __ATOMIC_ACQUIRE))
        return;
    t->sp_ho++;
    int away = kern_core_away();
    plat_yield();
    kern_core_back(&away);
    sw_last = kern_now_ns();
}

void kern_checkpoint(void) {
    RtThread *t = tcur;
    if (!t || !__atomic_load_n(&t->preempt, __ATOMIC_RELAXED)) return;
    lock();
    if (g_diag > 0 && t->cur) {
        t->dg_pc = t->cur->pc;
        t->dg_lr = t->cur->x[30];
    }
    int core = core_current(t);
    int better = core >= 0 && core_better_waiter(t, core);
    if (better && ap_disabled(t)) {
        ap_tlr(t)[1] = 1;
        t->preempt = 0;
        g_ap_deferred++;
    } else if (better) {
        core_put(t);
        core_take(t);
    } else {
        t->preempt = 0;
        t->pre_since = 0;
        t->dg_kick = t->dg_host_end = 0;
        t->dg_host_kick = 0;
        g_pre_stale++;
    }
    unlock();
}

uint64_t kern_ap_rip(void) { return tcur->ap_rip; }

void kern_ap_yield(void) {
    RtThread *t = tcur;
    lock();
    t->ap_pending = 0;
    int core = core_current(t);
    if (t->preempt && core >= 0 && !ap_disabled(t) && core_better_waiter(t, core)) {
        if (g_diag > 0) {
            t->dg_pc = t->cur->pc;
            t->dg_lr = t->cur->x[30];
        }
        g_ap_taken++;
        core_put(t);
        core_take(t);
    }
    unlock();
}

static void ap_sync(RtThread *t) {
    lock();
    int core = core_current(t);
    if (g_ap > 0 && core >= 0 && core_better_waiter(t, core)) {
        core_put(t);
        core_take(t);
    }
    unlock();
}

void kern_core_stats(int *held_now, int *held_max) {
    lock();
    int n = 0;
    for (int i = 0; i < NCORE; i++) n += g_core_owner[i] != NULL;
    *held_now = n;
    *held_max = g_held_max;
    unlock();
}

int kern_cores(void) { return g_active_cores; }

void kern_core_stats_reset(void) {
    lock();
    g_held_max = 0;
    core_count();
    unlock();
}

#define REP_N 8
static void crit_frame(RtThread *self, uint64_t frame) {
    static int parsed;
    if (!parsed) {
        parsed = 1;
        const char *e = cfg_get("NX2SARIA_CRIT");
        unsigned long long lo, hi;
        double wl, wh;
        if (e && e[0] == 'w' && sscanf(e + 1, "%lf-%lf", &wl, &wh) == 2 && wl >= 0 && wl < wh &&
            (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_wlo = (int64_t)(wl * 1e6), g_crit_whi = (int64_t)(wh * 1e6);
            g_crit_lo = g_crit_hi = UINT64_MAX;
            rt_log("crit: recording world seconds %g-%g", wl, wh);
        } else if (e && sscanf(e, "%llu-%llu", &lo, &hi) == 2 && lo < hi && (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_lo = lo;
            g_crit_hi = hi;
            rt_log("crit: recording frames %llu-%llu", lo, hi);
        }
    }
    if (g_crit && g_crit_whi && g_crit_hi == UINT64_MAX) {
        int64_t w = frame_step_world_us();
        if (g_crit_lo == UINT64_MAX) {
            if (w < g_crit_wlo) return;
            g_crit_lo = frame;
        }
        if (w > g_crit_whi) g_crit_hi = frame;
    }
    if (!g_crit || frame < g_crit_lo || frame > g_crit_hi) return;
    g_crit_on = 1;
    crit_add(CRIT_FRAME, self ? self->handle : 0, 0, 0, frame, kern_now_ns());
    if (frame < g_crit_hi) return;
    g_crit_on = 0;
    const char *path = cfg_get("NX2SARIA_CRIT_OUT");
    FILE *f = fopen(path && *path ? path : "crit.bin", "wb");
    if (!f) {
        rt_log("crit: cannot write %s", path && *path ? path : "crit.bin");
        return;
    }
    uint32_t nt = 0;
    for (RtThread *t = g_all; t; t = t->all) nt++;
    fwrite("CRIT", 1, 4, f);
    fwrite(&nt, 4, 1, f);
    fwrite(&g_crit_n, 4, 1, f);
    for (RtThread *t = g_all; t; t = t->all) {
        uint32_t row[4] = {t->handle, (uint32_t)t->prio, (uint32_t)(t->is_main | (t == g_render) << 1), t->tid};
        fwrite(row, 4, 4, f);
    }
    fwrite(g_crit, sizeof *g_crit, g_crit_n, f);
    fclose(f);
    rt_log("crit: %u events, %u threads, frames %llu-%llu%s", g_crit_n, nt, (unsigned long long)g_crit_lo,
           (unsigned long long)g_crit_hi, g_crit_n >= CRIT_N ? " (buffer full: the last frames are incomplete)" : "");
}

void kern_sched_report(uint64_t frame) {
    static uint64_t last;
    typedef struct {
        uint32_t h;
        int prio, render;
        uint64_t mask, entry, ns[S_N], nblk, npre, cpu, lw_ns, lw_n, lw_max, lg_ns, lg_n, lg_max, sl_n, sl_req, sl_act;
        uint64_t sp_n, sp_ho, sp_site;
    } Row;
    Row rows[REP_N + 1];
    WaitRec rw[2][4] = {0};
    int nr = 0;
    RtThread *self = tcur;
    lock();
    if (g_render != self) {
        g_render = self;
        memset(g_rw, 0, sizeof g_rw);
    }
    crit_frame(self, frame);
    if (frame % 120) {
        unlock();
        return;
    }
    uint64_t now = kern_now_ns(), span = last ? now - last : 0;
    last = now;
    RtThread *top[REP_N + 1] = {0};
    uint64_t busy[REP_N + 1] = {0}, all_run = 0, all_core = 0;
    for (RtThread *t = g_all; t; t = t->all) {
        acct(t, t->st);
        uint64_t b = t->st_ns[S_RUN] + t->st_ns[S_CORE] + t->st_ns[S_HOST];
        all_run += t->st_ns[S_RUN];
        all_core += t->st_ns[S_CORE];
        if (t == self) continue;
        for (int i = 0; i < REP_N; i++)
            if (!top[i] || b > busy[i]) {
                memmove(top + i + 1, top + i, (REP_N - 1 - i) * sizeof *top);
                memmove(busy + i + 1, busy + i, (REP_N - 1 - i) * sizeof *busy);
                top[i] = t;
                busy[i] = b;
                break;
            }
    }
    if (self) {
        memmove(top + 1, top, REP_N * sizeof *top);
        top[0] = self;
    }
    for (int i = 0; i <= REP_N && top[i]; i++) {
        RtThread *t = top[i];
        Row *r = &rows[nr++];
        r->h = t->handle;
        r->prio = t->prio;
        r->render = t == self;
        r->mask = t->mask;
        r->entry = t->entry;
        memcpy(r->ns, t->st_ns, sizeof r->ns);
        r->nblk = t->nblk;
        r->npre = t->npre;
        r->lw_ns = t->lw_ns, r->lw_n = t->lw_n, r->lw_max = t->lw_max;
        r->lg_ns = t->lg_ns, r->lg_n = t->lg_n, r->lg_max = t->lg_max;
        r->sl_n = t->sl_n, r->sl_req = t->sl_req, r->sl_act = t->sl_act;
        r->sp_n = t->sp_n, r->sp_ho = t->sp_ho, r->sp_site = t->sp_site;
        r->cpu = 0;
        uint64_t c = t->tid ? plat_thread_cpu_ns(t->tid) : 0;
        if (c) {
            r->cpu = t->cpu_last ? c - t->cpu_last : 0;
            t->cpu_last = c;
        }
    }
    for (int k = 0; k < 2; k++) {
        const WaitRec *src = k ? g_mw : g_rw;
        for (int i = 0; i < RW_N && src[i].ns; i++)
            for (int j = 0; j < 4; j++)
                if (src[i].ns > rw[k][j].ns) {
                    memmove(rw[k] + j + 1, rw[k] + j, (3 - j) * sizeof **rw);
                    rw[k][j] = src[i];
                    break;
                }
    }
    uint64_t pre_stale = g_pre_stale, pre_req = g_pre_req, pre_n = g_pre_n, pre_ns = g_pre_ns, pre_max = g_pre_max;
    uint64_t sc = g_sync_calls, sb = g_sync_blocks, sw = g_sync_woken, ss = g_sync_spared;
    for (RtThread *t = g_all; t; t = t->all) {
        memset(t->st_ns, 0, sizeof t->st_ns);
        t->nblk = t->npre = 0;
        t->lw_ns = t->lw_n = t->lw_max = t->lg_ns = t->lg_n = t->lg_max = t->sl_n = t->sl_req = t->sl_act = 0;
        t->sp_n = t->sp_ho = 0;
    }
    memset(g_rw, 0, sizeof g_rw);
    memset(g_mw, 0, sizeof g_mw);
    g_pre_req = g_pre_n = g_pre_ns = g_pre_max = g_pre_stale = 0;
    g_sync_calls = g_sync_blocks = g_sync_woken = g_sync_spared = 0;
    unlock();
    if (!span) return;
    rt_log("sched: frame %llu: %.2f s, cores busy %.2f of %d, core wait %.2f thread-s, preempt req %llu released %llu "
           "avg %.2f ms max %.2f ms, WaitSynchronization %llu calls %llu blocks (signals woke %llu, left %llu asleep), "
           "stale preempt %llu%s",
           (unsigned long long)frame, span / 1e9, (double)all_run / span, NCORE, all_core / 1e9,
           (unsigned long long)pre_req, (unsigned long long)pre_n, pre_n ? pre_ns / 1e6 / pre_n : 0.0, pre_max / 1e6,
           (unsigned long long)sc, (unsigned long long)sb, (unsigned long long)sw, (unsigned long long)ss,
           (unsigned long long)pre_stale, self ? "" : " (render thread is not a guest thread)");
    for (int i = 0; i < nr; i++) {
        Row *r = &rows[i];
        char d[128], s[256];
        int o = 0;
        for (int k = S_RUN; k < S_N; k++)
            o += snprintf(s + o, sizeof s - o, " %s %.0f%%", s_name[k], 100.0 * r->ns[k] / span);
        ldr_describe(r->entry, d, sizeof d);
        rt_log("sched: %s%#x p%d m%llx%s cpu %.0f%% blocks %llu preempted %llu (%s)", r->render ? "render " : "",
               r->h, r->prio, (unsigned long long)r->mask, s, 100.0 * r->cpu / span, (unsigned long long)r->nblk,
               (unsigned long long)r->npre, r->entry ? d : "main");
        rt_log("sched:   %#x wake to run avg %.1f us max %.0f us (%llu), core grant to run avg %.1f us max %.0f us "
               "(%llu), sleep %llu asked avg %.1f us took avg %.1f us", r->h, r->lw_n ? r->lw_ns / 1e3 / r->lw_n : 0.0,
               r->lw_max / 1e3, (unsigned long long)r->lw_n, r->lg_n ? r->lg_ns / 1e3 / r->lg_n : 0.0, r->lg_max / 1e3,
               (unsigned long long)r->lg_n, (unsigned long long)r->sl_n, r->sl_n ? r->sl_req / 1e3 / r->sl_n : 0.0,
               r->sl_n ? r->sl_act / 1e3 / r->sl_n : 0.0);
        if (r->sp_n) {
            ldr_describe(r->sp_site, d, sizeof d);
            rt_log("sched:   %#x spin wait passes %llu, core handoffs %llu, last at %s", r->h,
                   (unsigned long long)r->sp_n, (unsigned long long)r->sp_ho, d);
        }
    }
    for (int k = 0; k < 2; k++)
        for (int j = 0; j < 4 && rw[k][j].ns; j++) {
            char by[32] = "";
            if (rw[k][j].waker) snprintf(by, sizeof by, ", woken by %#x", rw[k][j].waker);
            rt_log("sched: %s wait %d: %s on %s%#llx%s: %.1f%%", k ? "main" : "render", j + 1, s_name[rw[k][j].st],
                   rw[k][j].st == S_HOST ? "exe+" : "", (unsigned long long)rw[k][j].obj, by, 100.0 * rw[k][j].ns / span);
        }
}

static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void lock_handoff(uint64_t addr) {
    RtThread *w = pick(W_LOCK, addr);
    int more = 0;
    if (!w) {
        st32(addr, 0);
        return;
    }
    for (RtThread *t = g_wait; t; t = t->next)
        if (t != w && t->wkind == W_LOCK && t->waddr == addr) {
            more = 1;
            break;
        }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}

static void lock_acquire_for(RtThread *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) {
            if (cas32(addr, &v, tag)) {
                wake(t, 0);
                return;
            }
            continue;
        }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    acct(t, S_LOCK);
    t->wobj = v & ~WAITERS_BIT;
    t->wkind = W_LOCK;
    t->waddr = addr;
    t->wtag = tag;
    t->wseq = ++g_wseq;
}

static RtThread *thr_new(int prio, int core, uint64_t mask) {
    RtThread *t = calloc(1, sizeof *t);
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    if (!t || !c) rt_fatal("kernel: thread allocation");
    memset(c, 0, sizeof *c);
    plat_cond_init(&t->cv);
    t->prio = prio;
    t->core = core;
    t->mask = mask;
    __atomic_store_n(&t->cur_core, -1, __ATOMIC_RELAXED);
    t->tlr = mem_kernel_alloc(TLR_SIZE + THREAD_OBJ_SIZE, TLR_SIZE);
    *(uint64_t *)(t->tlr + 0x1F8) = (uint64_t)(uintptr_t)(t->tlr + TLR_SIZE);
    t->tls = mem_kernel_alloc(TLS_SIZE, 16);
    *(uint64_t *)(t->tlr + 0x1F0) = (uint64_t)(uintptr_t)t->tls;
    c->tpidrro = (uint64_t)(uintptr_t)t->tlr;
    c->tpidr = (uint64_t)(uintptr_t)t->tls;
    c->excl_addr = ~0ull;
    c->thread = t;
    t->cpu = t->cur = c;
    return t;
}

static void thr_link(RtThread *t) {
    t->handle = h_new(O_THREAD, t);
    t->id = g_next_id++;
    t->all = g_all;
    g_all = t;
    g_created++;
    if (t->tlr) *(uint32_t *)(t->tlr + 0x110) = t->handle;
}

static void thr_free(RtThread *t) {
    for (RtThread **p = &g_all; *p; p = &(*p)->all)
        if (*p == t) {
            *p = t->all;
            break;
        }
    mem_kernel_free(t->tlr, TLR_SIZE + THREAD_OBJ_SIZE);
    mem_kernel_free(t->tls, TLS_SIZE);
    plat_aligned_free(t->cpu);
    free(t);
}

static void thread_finish(RtThread *t) {
    if (t->base_fiber) plat_fiber_unself();
    t->base_fiber = NULL;
    tcur = NULL;
    lock();
    core_put(t);
    acct(t, S_OFF);
    if (t->hthr) plat_intr_free(t->hthr);
    t->hthr = NULL;
    t->exited = 1;
    wake_sync_waiters(t);
    if (t->closed) thr_free(t);
    unlock();
}

static __attribute__((noreturn)) void exit_now(RtThread *t) { plat_jmp_go(&t->exit_jmp); }

static PlatIntr *ap_open_self(void) {
    if (!cores_enabled() || (!g_ap_int && g_diag <= 0)) return NULL;
    PlatIntr *h = plat_intr_self();
    if (!h) rt_log("kernel: no interruption handle for this thread: it is not preempted asynchronously");
    return h;
}

static void thread_main(void *p) {
    RtThread *t = p;
    tcur = t;
    t->tid = plat_thread_id();
    PlatIntr *self = ap_open_self();
    lock();
    t->hthr = self;
    while (!t->started) plat_cond_wait(&t->cv, &G);
    core_take(t);
    unlock();
    Cpu *c = t->cpu;
    c->sp = t->stack_top & ~15ull;
    if (!plat_jmp_save(&t->exit_jmp)) {
        rt_call(c, t->entry, 1, t->arg);
        char d[128];
        rt_fatal("guest thread %#x: entry %s returned (Horizon threads end with ExitThread)", t->handle,
                 ldr_describe(t->entry, d, sizeof d));
    }
    thread_finish(tcur);
    return;
}

uint64_t kern_threads_created(void) {
    lock();
    uint64_t n = g_created;
    unlock();
    return n;
}

Cpu *kern_cpu(void) { return tcur ? tcur->cur : NULL; }

void kern_seed_tls(void) {
    lock();
    if (tcur && tcur->tlr && tcur->tls) {
        *(uint64_t *)(tcur->tlr + 0x1F0) = (uint64_t)(uintptr_t)tcur->tls;
        if (tcur->cur) tcur->cur->tpidr = (uint64_t)(uintptr_t)tcur->tls;
    }
    unlock();
}

uint32_t kern_main_thread(Cpu **cpu) {
    if (tcur) {
        if (!tcur->is_main) rt_fatal("kern_main_thread on guest thread %#x, not the main thread", tcur->handle);
        *cpu = tcur->cpu;
        return tcur->handle;
    }
    if (g_main) rt_fatal("kern_main_thread: the main thread already runs on another host thread");
    const RtGuestLayout *L = mem_layout();
    uint64_t stack = L->stack.base + L->stack.size - MAIN_STACK;
    mem_commit_checked(stack, MAIN_STACK, "main thread stack");
    RtThread *t = thr_new(MAIN_PRIO, MAIN_CORE, CORE_MASK_ALL);
    t->started = 1;
    t->is_main = 1;
    t->stack_top = stack + MAIN_STACK;
    t->cpu->sp = t->stack_top;
    t->tid = plat_thread_id();
    perf_main_thread();
    PlatIntr *self = ap_open_self();
    lock();
    t->hthr = self;
    thr_link(t);
    g_main = t;
    g_main_stack = stack;
    core_take(t);
    unlock();
    tcur = t;
    *cpu = t->cpu;
    return t->handle;
}

void kern_main_stack(uint64_t *base, uint64_t *size) {
    uint64_t b = __atomic_load_n(&g_main_stack, __ATOMIC_ACQUIRE);
    *base = b;
    *size = b ? MAIN_STACK : 0;
}

static void guest_main(void *p) {
    void (*fn)(Cpu *) = (void (*)(Cpu *))p;
    Cpu *c;
    kern_main_thread(&c);
    if (!plat_jmp_save(&tcur->exit_jmp)) fn(c);
    thread_finish(tcur);
    return;
}

void kern_run_on_guest_main(void (*fn)(Cpu *c)) {
    static int watching;
    if (!watching++) {
        PlatThread *w = plat_thread_new(wait_watch, NULL, "wait watch");
        if (w) plat_thread_detach(w);
    }
    PlatThread *h = plat_thread_new_ex(guest_main, (void *)fn, "guest main", HOST_STACK);
    if (!h) rt_fatal("kernel: cannot start the guest main thread");
    plat_thread_join(h);
}

#define SLEEP_SPIN_NS (2 * PLAT_WAIT_LATE_NS)
static void sleep_ns(RtThread *t, int64_t ns) {
    (void)t;
    if (ns <= SLEEP_SPIN_NS) {
        uint64_t end = kern_now_ns() + (uint64_t)ns;
        while (kern_now_ns() < end) plat_pause();
        return;
    }
    plat_wait_ns((uint64_t)ns);
}

static int sig_obj(uint32_t h, int *bad) {
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) {
        *bad = 1;
        return 0;
    }
    switch (g_h[i].type) {
    case O_THREAD: return ((RtThread *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((Event *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}

int kern_thread_state(uint32_t h) {
    lock();
    RtThread *t = h_peek(h, O_THREAD);
    int r = !t ? -1 : t->exited ? KERN_THREAD_EXITED : t->wkind;
    unlock();
    return r;
}

static uint32_t handle_close(uint32_t h) {
    unsigned i = h - 0x1000;
    int ok = 0, kind = 0;
    void *ipc = NULL;
    lock();
    if (i > 0 && i < MAX_H) {
        int type = g_h[i].type;
        void *o = g_h[i].obj;
        if (type == O_EVENT_R || type == O_EVENT_W) {
            Event *e = o;
            if (--e->refs == 0) free(e);
            ok = 1;
        } else if (type == O_SESSION || type == O_TMEM) {
            kind = type;
            ipc = o;
            ok = 1;
        } else if (type == O_THREAD) {
            RtThread *t = o;
            t->closed = 1;
            if (t->exited) thr_free(t);
            ok = 1;
        }
        if (ok) {
            g_h[i].type = O_FREE;
            g_h[i].obj = NULL;
        }
    }
    unlock();
    if (ipc) ipc_handle_closed(kind, ipc);
    return ok ? R_OK : R_BAD_HANDLE;
}

void kern_handle_close(uint32_t h) {
    if (handle_close(h)) rt_fatal("kern_handle_close(%#x): bad handle", h);
}

volatile int kern_test_cv_old_order;
void (*volatile kern_test_cv_hook)(void);

static void cv_hook(void) {
    void (*h)(void) = kern_test_cv_hook;
    if (h) h();
}

static int kern_svc_inner(Cpu *c, uint32_t imm) {
    uint64_t *x = c->x;
    if (g_diag > 0 && tcur) tcur->dg_svc = imm;
    switch (imm) {
    case 0x08: {
        int prio = (int)x[4], core = (int)x[5];
        if (!x[1] || !x[3] || prio < 28 || prio > 59 || core < -3 || core >= NCORE) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        RtThread *t = thr_new(prio, core >= 0 ? core : MAIN_CORE, core >= 0 ? 1ull << core : CORE_MASK_ALL);
        t->entry = x[1];
        t->arg = x[2];
        t->stack_top = x[3] & ~15ull;
        lock();
        thr_link(t);
        unlock();
        if (!plat_thread_spawn(thread_main, t, "guest", HOST_STACK)) rt_fatal("kernel: cannot start a guest thread");
        if (rt_trace & RT_TRACE_SVC) {
            char d[128];
            rt_log("CreateThread %#x entry %s stack %#llx prio %d core %d", t->handle,
                   ldr_describe(t->entry, d, sizeof d), (unsigned long long)t->stack_top, prio, core);
        }
        x[0] = R_OK;
        x[1] = t->handle;
        return 1;
    }
    case 0x09: {
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->started = 1;
            plat_cond_signal(&t->cv);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x0A: {
        RtThread *t = cur(imm);
        if (t->fiber) {
            t->exit_pending = 1;
            t->leaving = t->fiber;
            t->fiber = NULL;
            t->cur = t->cpu;
            plat_fiber_switch(t->base_fiber);
            rt_fatal("kernel: ExitThread on a fiber came back");
        }
        exit_now(t);
    }
    case 0x0B: {
        int64_t ns = (int64_t)x[0];
        RtThread *t = cur(imm);
        if (ns > 0) {
            lock();
            core_put(t);
            acct(t, S_SLEEP);
            t->wobj = (uint64_t)ns;
            t->nblk++;
            unlock();
            uint64_t t0 = kern_now_ns();
            sleep_ns(t, ns);
            uint64_t took = kern_now_ns() - t0;
            lock();
            t->sl_n++;
            t->sl_req += (uint64_t)ns;
            t->sl_act += took;
            core_take(t);
            unlock();
            return 1;
        }
        if (!__atomic_load_n(&g_core_waiting, __ATOMIC_ACQUIRE)) {
            plat_yield();
            return 1;
        }
        lock();
        int give = 0, tp = -1, core = core_current(t);
        for (RtThread *w = g_core_wait; w && !give; w = w->cnext)
            if (core >= 0 && core_allowed(w, core)) {
                if (tp < 0) tp = prio_of(t);
                give = prio_of(w) <= tp;
            }
        if (give) {
            core_put(t);
            core_take(t);
        }
        unlock();
        if (!give) plat_yield();
        return 1;
    }
    case 0x0C: {
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = (uint64_t)t->prio;
        unlock();
        return 1;
    }
    case 0x0D: {
        if ((uint32_t)x[1] < 28 || (uint32_t)x[1] > 59) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) t->prio = (int)x[1];
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x0E: {
        lock();
        RtThread *t = h_get((uint32_t)x[2], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) {
            x[1] = (uint64_t)(uint32_t)t->core;
            x[2] = t->mask;
        }
        unlock();
        return 1;
    }
    case 0x0F: {
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            int core = (int)x[1];
            uint64_t mask = x[2] & CORE_MASK_ALL;
            if (core >= NCORE || (x[2] & ~CORE_MASK_ALL)) {
                static int logged;
                if (logged++ < 4)
                    rt_log("kernel: SetThreadCoreMask(%#x, core %d, mask %#llx) clamped to cores 0-%d", t->handle, core,
                           (unsigned long long)x[2], NCORE - 1);
                if (core >= NCORE) core = mask ? __builtin_ctzll(mask) : MAIN_CORE;
            }
            if (core >= 0) t->core = core;
            else if (core == -1) t->core = MAIN_CORE;
            if (mask) t->mask = mask;
            int current_core = core_current(t);
            if (t == tcur && current_core >= 0 && !core_allowed(t, current_core)) {
                core_put(t);
                core_take(t);
            }
        }
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x10: {
        RtThread *t = cur(imm);
        int core = core_current(t);
        x[0] = (uint64_t)(uint32_t)(core >= 0 ? core_seen(t, core) : t->core);
        return 1;
    }
    case 0x36: {
        RtThread *t = cur(imm);
        volatile uint16_t *flag = (volatile uint16_t *)(t->tlr + 0x102);
        __atomic_fetch_add(&g_sync36, 1, __ATOMIC_RELAXED);
        if (*flag) __atomic_fetch_add(&g_sync36_flag, 1, __ATOMIC_RELAXED);
        *flag = 0;
        ap_sync(t);
        x[0] = R_OK;
        return 1;
    }
    case 0x25: {
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = t->id;
        unlock();
        return 1;
    }

    case 0x1A: {
        RtThread *t = cur(imm);
        uint64_t addr = x[1];
        uint32_t r = R_OK;
        lock();
        if (ld32(addr) == ((uint32_t)x[0] | WAITERS_BIT)) {
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            t->wobj = (uint32_t)x[0];
            r = block(t, W_LOCK, -1);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1B:
        lock();
        lock_handoff(x[0]);
        unlock();
        x[0] = R_OK;
        return 1;
    case 0x1C: {
        RtThread *t = cur(imm);
        uint64_t addr = x[0], key = x[1];
        lock();
        if (!kern_test_cv_old_order) {
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            lock_handoff(addr);
            cv_hook();
        } else {
            lock_handoff(addr);
            cv_hook();
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
        }
        t->wobj = key;
        uint32_t r = block(t, W_CONDVAR, (int64_t)x[3]);
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1D: {
        uint64_t key = x[0];
        int cnt = (int)x[1], n = 0;
        lock();
        for (RtThread *w; (cnt <= 0 || n < cnt) && (w = pick(W_CONDVAR, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0;
        for (RtThread *w = g_wait; w && !left; w = w->next) left = w->wkind == W_CONDVAR && w->wkey == key;
        if (!left) st32(key, 0);
        unlock();
        x[0] = R_OK;
        return 1;
    }

    case 0x34: {
        RtThread *t = cur(imm);
        uint64_t addr = x[0];
        int type = (int)x[1];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 3 || (type == 3 && (addr & 7))) {
            x[0] = type == 3 && (addr & 7) ? R_INVALID_ADDRESS : R_INVALID_ENUM;
            return 1;
        }
        lock();
        uint32_t r, v = ld32(addr);
        int ok = type == 3 ? __atomic_load_n((const uint64_t *)(uintptr_t)addr, __ATOMIC_SEQ_CST) == x[2]
               : type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1)
            while (!cas32(addr, &v, v - 1))
                if (!((int32_t)v < val)) {
                    ok = 0;
                    break;
                }
        if (!ok) r = R_BAD_STATE;
        else {
            t->waddr = addr;
            t->wobj = addr;
            r = block(t, W_ADDRESS, (int64_t)x[3]);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x35: {
        uint64_t addr = x[0];
        int type = (int)x[1], cnt = (int)x[3];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 2) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        lock();
        int nw = 0;
        for (RtThread *w = g_wait; w; w = w->next) nw += w->wkind == W_ADDRESS && w->waddr == addr;
        uint32_t r = R_OK;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r)
            for (int n = 0; cnt <= 0 || n < cnt; n++) {
                RtThread *w = pick(W_ADDRESS, addr);
                if (!w) break;
                wake(w, 0);
            }
        unlock();
        x[0] = r;
        return 1;
    }

    case 0x45: {
        Event *e = calloc(1, sizeof *e);
        if (!e) rt_fatal("kernel: event allocation");
        e->refs = 2;
        lock();
        e->serial = ++g_ev_serial;
        uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e);
        unlock();
        x[0] = R_OK;
        x[1] = hw;
        x[2] = hr;
        return 1;
    }
    case 0x11: case 0x12: case 0x17: {
        uint32_t h = (uint32_t)x[0];
        uint32_t r = R_OK;
        lock();
        Event *e = imm == 0x11 ? h_peek(h, O_EVENT_W) : imm == 0x17 ? h_peek(h, O_EVENT_R)
                 : h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R);
        if (!e) {
            r = imm == 0x17 && h_peek(h, O_THREAD) ? R_BAD_STATE : R_BAD_HANDLE;
            if (r == R_BAD_HANDLE) rt_log("kernel: event svc %#x: invalid handle %#x", imm, h);
        } else if (imm == 0x11) {
            e->signaled = 1;
            wake_sync_waiters(e);
        } else if (imm == 0x17 && !e->signaled) {
            r = R_BAD_STATE;
        } else {
            e->signaled = 0;
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x18: {
        RtThread *t = cur(imm);
        const uint32_t *hs = (const uint32_t *)(uintptr_t)x[1];
        int n = (int)x[2];
        int64_t ns = (int64_t)x[3];
        if (n < 0 || n > 64 || (!hs && n)) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        uint64_t start = kern_now_ns();
        uint32_t r;
        int idx = -1;
        lock();
        g_sync_calls++;
        uint32_t woke = 0;
        for (;;) {
            if (woke == R_CANCELLED) {
                t->cancel = 0;
                r = R_CANCELLED;
                break;
            }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++)
                if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) {
                static int logged;
                if (rt_trace && logged++ < 4)
                    rt_logf("kernel: WaitSynchronizationN on a handle that is not waitable: %#x\n", hs[0]);
                r = R_BAD_HANDLE;
                break;
            }
            if (idx >= 0) {
                r = R_OK;
                break;
            }
            if (ns == 0) {
                r = R_TIMEOUT;
                break;
            }
            if (t->cancel) {
                t->cancel = 0;
                r = R_CANCELLED;
                break;
            }
            int64_t left = ns;
            if (ns > 0) {
                uint64_t spent = kern_now_ns() - start;
                if (spent >= (uint64_t)ns) {
                    r = R_TIMEOUT;
                    break;
                }
                left = ns - (int64_t)spent;
            }
            {
                static int logged;
                if (rt_trace && logged++ < 3) {
                    char d[96];
                    rt_logf("kernel: first blocking wait: %d handle(s) %#x %#x lr %s\n", n,
                            n > 0 ? hs[0] : 0, n > 1 ? hs[1] : 0,
                            ldr_describe(c->x[30], d, sizeof d));
                }
            }
            t->wobj = n ? hs[0] | (uint64_t)n << 32 : 0;
            g_sync_blocks++;
            t->whs = hs, t->wn = n;
            woke = block(t, W_SYNC, left);
            t->whs = NULL, t->wn = 0;
            if (woke == R_TIMEOUT) {
                r = R_TIMEOUT;
                break;
            }
        }
        unlock();
        x[0] = r;
        x[1] = idx >= 0 ? (uint64_t)idx : 0;
        return 1;
    }
    case 0x19: {
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->cancel = 1;
            if (t->wkind == W_SYNC) wake(t, R_CANCELLED);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x16:
        x[0] = handle_close((uint32_t)x[0]);
        return 1;
    }
    return 0;
}

uint32_t kern_handle_new(int kind, void *obj) {
    if (kind != KERN_SESSION && kind != KERN_TMEM) rt_fatal("kern_handle_new: kind %d", kind);
    lock();
    uint32_t h = h_new(kind, obj);
    unlock();
    return h;
}

void *kern_handle_get(uint32_t h, int kind) {
    lock();
    void *o = h_peek(h, kind);
    unlock();
    return o;
}

void *kern_handle_hold(uint32_t h, int kind, void (*hold)(void *obj)) {
    lock();
    void *o = h_peek(h, kind);
    if (o) hold(o);
    unlock();
    return o;
}

int kern_handle_copy(uint32_t h, int kind, void *out, size_t n) {
    lock();
    const void *o = h_peek(h, kind);
    if (o) memcpy(out, o, n);
    unlock();
    return o != NULL;
}

uint32_t kern_event_new(void) {
    Event *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("kernel: event allocation");
    e->refs = 1;
    lock();
    e->serial = ++g_ev_serial;
    uint32_t hw = h_new(O_EVENT_W, e);
    unlock();
    return hw;
}

uint32_t kern_event_readable(uint32_t hw) {
    lock();
    Event *e = h_peek(hw, O_EVENT_W);
    if (!e) rt_fatal("kern_event_readable: %#x is not a writable event handle", hw);
    e->refs++;
    uint32_t hr = h_new(O_EVENT_R, e);
    unlock();
    return hr;
}

void kern_event_signal(uint32_t hw) {
    lock();
    Event *e = h_peek(hw, O_EVENT_W);
    if (!e) rt_fatal("kern_event_signal: %#x is not a writable event handle", hw);
    e->signaled = 1;
    wake_sync_waiters(e);
    unlock();
}

void kern_event_clear(uint32_t hw) {
    lock();
    Event *e = h_peek(hw, O_EVENT_W);
    if (!e) rt_fatal("kern_event_clear: %#x is not a writable event handle", hw);
    e->signaled = 0;
    unlock();
}

#define SE_STATE 0x28
static struct { uint64_t ev, serial; uint32_t wh; uint8_t guest; } g_se[512];
static int g_nse;
static PlatMutex g_se_lock = PLAT_MUTEX_INIT;

static int se_live(uint64_t ev) {
    return __atomic_load_n((const uint8_t *)(uintptr_t)(ev + SE_STATE), __ATOMIC_RELAXED) != 0;
}

static uint32_t se_find(uint64_t ev, uint64_t *serial) {
    plat_mutex_lock_shared(&g_se_lock);
    uint32_t wh = 0;
    for (int i = 0; i < g_nse; i++)
        if (g_se[i].ev == ev) {
            wh = g_se[i].wh;
            if (serial) *serial = g_se[i].serial;
            break;
        }
    plat_mutex_unlock_shared(&g_se_lock);
    return wh;
}

void kern_bind_system_event(Cpu *c, uint64_t ev, int autoclear) {
    if (se_find(ev, NULL) && se_live(ev)) return;
    if (!tcur || !c) rt_fatal("kern_bind_system_event(%#llx) on a host thread", (unsigned long long)ev);
    static uint64_t create, getw;
    if (!create) {
        create = ldr_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        getw = ldr_symbol("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
        if (!create || !getw) rt_fatal("kern_bind_system_event: the SDK's system event functions are not loaded");
    }
    uint32_t r = (uint32_t)rt_call(c, create, 3, ev, (uint64_t)(autoclear ? 1 : 0), (uint64_t)1);
    if (r) rt_fatal("kern_bind_system_event: CreateSystemEvent(%#llx) failed %#x", (unsigned long long)ev, r);
    uint32_t wh = (uint32_t)rt_call(c, getw, 1, ev);
    lock();
    Event *e = h_peek(wh, O_EVENT_W);
    uint64_t serial = e ? e->serial : 0;
    unlock();
    uint8_t guest = (uint8_t)mem_readable(ev + SE_STATE, 1);
    plat_mutex_lock(&g_se_lock);
    int i = 0;
    while (i < g_nse && g_se[i].ev != ev) i++;
    if (i == g_nse && g_nse == (int)(sizeof g_se / sizeof *g_se)) {
        i = 0;
        while (i < g_nse && (!g_se[i].guest || (mem_readable(g_se[i].ev + SE_STATE, 1) && se_live(g_se[i].ev)))) i++;
        if (i == g_nse) rt_fatal("kern_bind_system_event: table full (%d live system events)", g_nse);
        g_se[i].ev = ev;
    } else if (i == g_nse) {
        g_se[g_nse++].ev = ev;
    }
    g_se[i].wh = wh;
    g_se[i].serial = serial;
    g_se[i].guest = guest;
    plat_mutex_unlock(&g_se_lock);
}

static void system_event_set(uint64_t ev, int set) {
    static int told;
    int tell = 0;
    lock();
    uint64_t serial = 0;
    uint32_t wh = se_find(ev, &serial);
    Event *e = wh ? h_peek(wh, O_EVENT_W) : NULL;
    if (e && e->serial == serial) {
        e->signaled = set;
        if (set) wake_sync_waiters(e);
    } else if (wh && !told) {
        told = tell = 1;
    }
    unlock();
    if (tell)
        rt_log("kernel: system event %#llx: its kept handle %#x no longer names its event; %s skipped (logged once)",
               (unsigned long long)ev, wh, set ? "signal" : "clear");
}

void kern_signal_system_event(uint64_t ev) {
    uint32_t wh = se_find(ev, NULL);
    if (wh && !se_live(ev)) return;
    if (!wh) kern_bind_system_event(kern_cpu(), ev, 0);
    system_event_set(ev, 1);
}

void kern_clear_system_event(uint64_t ev) {
    if (se_find(ev, NULL) && se_live(ev)) system_event_set(ev, 0);
}

void kern_unbind_system_event(Cpu *c, uint64_t ev) {
    if (!se_find(ev, NULL)) return;
    if (c && tcur && se_live(ev)) {
        static uint64_t destroy;
        uint64_t fn = __atomic_load_n(&destroy, __ATOMIC_RELAXED);
        if (!fn) {
            fn = ldr_symbol("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
            if (!fn)
                rt_log("kernel: the SDK's DestroySystemEvent is not loaded: unbound system events keep their handles");
            __atomic_store_n(&destroy, fn = fn ? fn : 1, __ATOMIC_RELAXED);
        }
        if (fn > 1) rt_call(c, fn, 1, ev);
    }
    plat_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; i++)
        if (g_se[i].ev == ev) {
            g_se[i] = g_se[--g_nse];
            break;
        }
    plat_mutex_unlock(&g_se_lock);
}

int kern_system_event_live(uint64_t ev) { return mem_readable(ev + SE_STATE, 1) && se_live(ev); }

enum { F_NEW, F_STARTED, F_DONE };
struct Fiber {
    uint64_t addr, func, arg, stack, size;
    Cpu *cpu;
    PlatFiber *host;
    int state;
    volatile int running;
};
static Fiber *g_fibers[FIBER_MAX];
static PlatMutex g_fiber_lock = PLAT_MUTEX_INIT;

static Fiber *fiber_find(uint64_t addr) {
    Fiber *f = NULL;
    plat_mutex_lock_shared(&g_fiber_lock);
    for (int i = 0; i < FIBER_MAX && !f; i++)
        if (g_fibers[i] && g_fibers[i]->addr == addr) f = g_fibers[i];
    plat_mutex_unlock_shared(&g_fiber_lock);
    return f;
}

static void fiber_switch_to(RtThread *t, const Cpu *from, Fiber *to);

__attribute__((noinline)) static void fiber_resumed(void) {
    RtThread *t = tcur_now();
    rt_fpcr_sync(t->cur);
    if (t->leaving) {
        __atomic_store_n(&t->leaving->running, 0, __ATOMIC_RELEASE);
        t->leaving = NULL;
    }
    if (t->exit_pending && !t->fiber) exit_now(t);
}

static void fiber_main(void *p) {
    Fiber *f = p;
    fiber_resumed();
    Cpu *c = f->cpu;
    tcur_now()->host_depth = 0;
    uint64_t next = rt_call(c, f->func, 1, f->arg);
    f->state = F_DONE;
    Fiber *n = NULL;
    if (next && !(n = fiber_find(next)))
        rt_fatal("fiber %#llx returned %#llx, which is not an initialized fiber", (unsigned long long)f->addr,
                 (unsigned long long)next);
    fiber_switch_to(tcur_now(), c, n);
    rt_fatal("fiber %#llx resumed after it finished", (unsigned long long)f->addr);
}

static void fiber_switch_to(RtThread *t, const Cpu *from, Fiber *to) {
    if (!t->base_fiber) {
        t->base_fiber = plat_fiber_self();
        if (!t->base_fiber) rt_fatal("kernel: cannot make the thread a fiber");
    }
    Cpu *dst = to ? to->cpu : t->cpu;
    if (to) {
        if (to->state == F_DONE) rt_fatal("SwitchToFiber(%#llx): the fiber has finished", (unsigned long long)to->addr);
        int idle = 0;
        if (!__atomic_compare_exchange_n(&to->running, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            rt_fatal("SwitchToFiber(%#llx): the fiber is running", (unsigned long long)to->addr);
        if (to->state == F_NEW) {
            dst->sp = (to->stack + to->size) & ~15ull;
            to->state = F_STARTED;
        }
    }
    dst->tpidr = from->tpidr;
    dst->tpidrro = from->tpidrro;
    dst->fpcr = from->fpcr;
    dst->fpsr = from->fpsr;
    dst->excl_addr = ~0ull;
    dst->thread = t;
    t->leaving = t->fiber;
    t->fiber = to;
    t->cur = dst;
    int depth = t->host_depth;
    plat_fiber_switch(to ? to->host : t->base_fiber);
    tcur_now()->host_depth = depth;
    fiber_resumed();
}

void kern_fiber_initialize(Cpu *c) {
    uint64_t addr = c->x[0];
    Fiber *f = fiber_find(addr);
    if (f && f->running) rt_fatal("InitializeFiber(%#llx): the fiber is running", (unsigned long long)addr);
    if (!f) {
        f = calloc(1, sizeof *f);
        Cpu *fc = plat_aligned_alloc(sizeof(Cpu), 64);
        if (!f || !fc) rt_fatal("kernel: fiber allocation");
        f->cpu = fc;
        plat_mutex_lock(&g_fiber_lock);
        int i = 0;
        while (i < FIBER_MAX && g_fibers[i]) i++;
        if (i == FIBER_MAX) rt_fatal("kernel: more than %d fibers", FIBER_MAX);
        g_fibers[i] = f;
        plat_mutex_unlock(&g_fiber_lock);
    } else if (f->host) {
        plat_fiber_free(f->host);
    }
    memset(f->cpu, 0, sizeof *f->cpu);
    f->cpu->excl_addr = ~0ull;
    f->addr = addr;
    f->func = c->x[1];
    f->arg = c->x[2];
    f->stack = c->x[3];
    f->size = c->x[4];
    f->state = F_NEW;
    f->running = 0;
    f->host = plat_fiber_new(HOST_STACK, fiber_main, f);
    {
        static int logged;
        if (logged++ < 6)
            rt_log("kernel: InitializeFiber %#llx (stack %#llx+%#llx) on thread %#x", (unsigned long long)addr,
                   (unsigned long long)f->stack, (unsigned long long)f->size, tcur ? tcur->handle : 0);
    }
    if (!f->host) rt_fatal("kernel: cannot create a host fiber");
}

void kern_fiber_finalize(Cpu *c) {
    uint64_t addr = c->x[0];
    plat_mutex_lock(&g_fiber_lock);
    Fiber *f = NULL;
    for (int i = 0; i < FIBER_MAX && !f; i++)
        if (g_fibers[i] && g_fibers[i]->addr == addr) {
            f = g_fibers[i];
            g_fibers[i] = NULL;
        }
    plat_mutex_unlock(&g_fiber_lock);
    if (!f) rt_fatal("FinalizeFiber(%#llx): not an initialized fiber", (unsigned long long)addr);
    if (f->running) rt_fatal("FinalizeFiber(%#llx): the fiber is running", (unsigned long long)addr);
    plat_fiber_free(f->host);
    plat_aligned_free(f->cpu);
    free(f);
}

void kern_fiber_switch(Cpu *c) {
    RtThread *t = tcur;
    if (!t) rt_fatal("SwitchToFiber on a host thread");
    if (!c->x[0]) {
        static int logged;
        if (logged++ < 3)
            rt_log("kernel: SwitchToFiber(nullptr) on thread %#x (on fiber %#llx)", t->handle,
                   (unsigned long long)(t->fiber ? t->fiber->addr : 0));
        if (t->fiber) fiber_switch_to(t, c, NULL);
        return;
    }
    Fiber *to = fiber_find(c->x[0]);
    if (!to) rt_fatal("SwitchToFiber(%#llx): not an initialized fiber", (unsigned long long)c->x[0]);
    if (to == t->fiber) return;
    fiber_switch_to(t, c, to);
}

void kern_fiber_current(Cpu *c) {
    RtThread *t = tcur;
    c->x[0] = t && t->fiber ? t->fiber->addr : 0;
}

HOST_OVERRIDE(KERN_SYM_INITIALIZE_FIBER, kern_fiber_initialize);
HOST_OVERRIDE(KERN_SYM_FINALIZE_FIBER, kern_fiber_finalize);
HOST_OVERRIDE(KERN_SYM_SWITCH_TO_FIBER, kern_fiber_switch);
HOST_OVERRIDE(KERN_SYM_GET_CURRENT_FIBER, kern_fiber_current);

#define SW_N 256
#define SW_DEPTH 4
static struct { uint64_t key, lr[SW_DEPTH], ns, n; uint32_t imm; } svw[SW_N];
static uint64_t svw_t0;

static void svc_wait_note(const Cpu *c, uint32_t imm, uint64_t ns) {
    uint64_t lr[SW_DEPTH] = {c->x[30]}, fp = c->x[29], key = imm * 0x9E3779B97F4A7C15ull ^ c->x[30];
    for (int d = 1; d < SW_DEPTH && fp && !(fp & 7) && mem_readable(fp, 16); d++) {
        uint64_t next = *(const volatile uint64_t *)(uintptr_t)fp, ret = *(const volatile uint64_t *)(uintptr_t)(fp + 8);
        lr[d] = ret, key = (key ^ ret) * 0x100000001B3ull;
        if (next <= fp) break;
        fp = next;
    }
    if (!key) key = 1;
    for (uint32_t k = 0; k < 32; k++) {
        typeof(svw[0]) *e = &svw[(key + k) % SW_N];
        if (e->key && e->key != key) continue;
        if (!e->key) e->key = key, e->imm = imm, memcpy(e->lr, lr, sizeof lr);
        e->ns += ns, e->n++;
        break;
    }
    uint64_t now = kern_now_ns();
    if (!svw_t0) svw_t0 = now;
    if (now - svw_t0 < 10000000000ull) return;
    double secs = (now - svw_t0) / 1e9;
    svw_t0 = now;
    for (int top = 0; top < 6; top++) {
        int best = -1;
        for (int i = 0; i < SW_N; i++)
            if (svw[i].key && svw[i].ns && (best < 0 || svw[i].ns > svw[best].ns)) best = i;
        if (best < 0) break;
        char d[SW_DEPTH][96];
        for (int k = 0; k < SW_DEPTH; k++) {
            if (svw[best].lr[k]) ldr_describe(svw[best].lr[k], d[k], sizeof d[k]);
            else d[k][0] = 0;
        }
        rt_log("kernel: main thread in service %#x: %.1f ms/s over %llu calls (%.2f ms each) from %s < %s < %s < %s",
               svw[best].imm, svw[best].ns / 1e6 / secs, (unsigned long long)svw[best].n, svw[best].ns / 1e6 / (double)svw[best].n,
               d[0], d[1], d[2], d[3]);
        svw[best].ns = 0;
    }
    memset(svw, 0, sizeof svw);
}

int kern_svc(Cpu *c, uint32_t imm) {
    static int waits = -1;
    if (waits < 0) {
        const char *v = cfg_get("NX2SARIA_SVC_WAITS");
        waits = v && *v && cfg_bool("NX2SARIA_SVC_WAITS");
    }
    if (waits && plat_thread_id() == prof_main_tid) {
        uint64_t w0 = kern_now_ns();
        int r = kern_svc_inner(c, imm);
        svc_wait_note(c, imm, kern_now_ns() - w0);
        return r;
    }
    static double lim = -1;
    if (lim < 0) lim = strtod(cfg_value("NX2SARIA_SVC_SLOW_MS"), NULL);
    if (lim <= 0) return kern_svc_inner(c, imm);
    uint64_t t0 = plat_now_ns();
    int r = kern_svc_inner(c, imm);
    double ms = (plat_now_ns() - t0) / 1e6;
    if (ms >= lim) {
        uint32_t tid = plat_thread_id();
        rt_log("kernel: service %#x blocked %.1f ms on thread %u%s", imm, ms, tid,
               tid == prof_replay_tid ? " (the replay thread)" : "");
    }
    return r;
}

void kern_sizes(PerfSizes *s) {
    uint64_t threads = 0, handles = 0, fibers = 0, ses;
    plat_mutex_lock_shared(&G);
    for (RtThread *t = g_all; t; t = t->all) threads++;
    for (int i = 0; i < MAX_H; i++) handles += g_h[i].type != 0;
    plat_mutex_unlock_shared(&G);
    plat_mutex_lock_shared(&g_se_lock);
    ses = (uint64_t)g_nse;
    plat_mutex_unlock_shared(&g_se_lock);
    plat_mutex_lock_shared(&g_fiber_lock);
    for (int i = 0; i < FIBER_MAX; i++) fibers += g_fibers[i] != NULL;
    plat_mutex_unlock_shared(&g_fiber_lock);
    perf_size(s, "guest threads", threads, 0, PERF_SIZE_NONE);
    perf_size(s, "handles", handles, MAX_H, PERF_SIZE_NONE);
    perf_size(s, "process handles", plat_handle_count(), 0, PERF_SIZE_NONE);
    perf_size(s, "system events", ses, sizeof g_se / sizeof *g_se, PERF_SIZE_NONE);
    perf_size(s, "fibers", fibers, FIBER_MAX, PERF_SIZE_NONE);
}
