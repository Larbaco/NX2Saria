// Contains code Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_submit_guard.h"
#include "platform/platform.h"

#define CMD_MAGIC MAGIC('C', 'M', 'D', 'B')
#define COPY_MAGIC 0x59504f43444d43ull
#define OP_RESERVE 24
#define MIN_COMMAND_SIZE 512
#define MIN_CONTROL_SIZE 256
#define CTL_PER_RECORDING 64
#define COPY_PAGE_SHIFT 16

static cmd_t *cmd_of(const Cpu *c) { return arg_p(c, 0); }

enum { CB_NO_COMMAND = 1, CB_NO_CONTROL = 2 };
static PlatMutex cb_lock = PLAT_MUTEX_INIT;
static nvn_hmap cb_off = {.klen = 8};

static unsigned cb_disabled(const cmd_t *o) {
    plat_mutex_lock_shared(&cb_lock);
    uintptr_t v = (uintptr_t)nvn_hm_get(&cb_off, &o);
    plat_mutex_unlock_shared(&cb_lock);
    return (unsigned)v & 0xff;
}

static void cb_set_disabled(const cmd_t *o, unsigned kind, int disabled) {
    plat_mutex_lock(&cb_lock);
    unsigned v = (unsigned)(uintptr_t)nvn_hm_get(&cb_off, &o) & 0xff;
    v = disabled ? v | kind : v & ~kind;
    if (v) nvn_hm_put(&cb_off, &o, (void *)(uintptr_t)(v | 0x100));
    else nvn_hm_del(&cb_off, &o);
    plat_mutex_unlock(&cb_lock);
}

static void memory_event(Cpu *c, cmd_t *o, int event, uint64_t min) {
    if (!o->cb || (cb_disabled(o) & (event == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY ? CB_NO_COMMAND
                                                                                                     : CB_NO_CONTROL)))
        return;
    NVN_LOG("command buffer %p: memory callback %p (%s memory, %llu bytes)", (void *)o, (void *)o->cb,
            event == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY ? "command" : "control",
            (unsigned long long)min);
    rt_call(c, (uint64_t)(uintptr_t)o->cb, 4, (uint64_t)(uintptr_t)o, (uint64_t)event, min,
            (uint64_t)(uintptr_t)o->cb_data);
}

static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) {
    op_hdr h = {op, size};
    memcpy(p, &h, sizeof h);
}

#define POOL_SLOTS 16
#define POOL_MAX_BYTES ((size_t)4 << 20)
static PlatMutex pool_lock = PLAT_MUTEX_INIT;
static struct { uint8_t *p; size_t size; } pool[POOL_SLOTS];
static uint32_t npool;

static uint8_t *pool_take(size_t *size) {
    uint8_t *p = NULL;
    plat_mutex_lock(&pool_lock);
    if (npool) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < npool; i++)
            if (pool[i].size > pool[best].size) best = i;
        p = pool[best].p;
        *size = pool[best].size;
        pool[best] = pool[--npool];
    }
    plat_mutex_unlock(&pool_lock);
    return p;
}

static void pool_put(uint8_t *p, size_t size) {
    if (!p) return;
    if (size <= POOL_MAX_BYTES) {
        plat_mutex_lock(&pool_lock);
        if (npool < POOL_SLOTS) {
            pool[npool].p = p;
            pool[npool++].size = size;
            p = NULL;
        } else {
            uint32_t small = 0;
            for (uint32_t i = 1; i < npool; i++)
                if (pool[i].size < pool[small].size) small = i;
            if (pool[small].size < size) {
                uint8_t *drop = pool[small].p;
                pool[small].p = p;
                pool[small].size = size;
                p = drop;
            }
        }
        plat_mutex_unlock(&pool_lock);
    }
    free(p);
}

static void pool_drain(void) {
    plat_mutex_lock(&pool_lock);
    while (npool) free(pool[--npool].p);
    plat_mutex_unlock(&pool_lock);
}

static void *realloc_wait(void *old, size_t n, const char *what) {
    void *p = realloc(old, n);
    if (p) return p;
    pool_drain();
    static uint32_t reports;
    int report = __atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) < 8;
    if (report) {
        double proc, used, limit;
        plat_commit_info(&proc, &used, &limit);
        rt_log("nvn: %s of %zu bytes failed (memory commit: this process %.0f MB, the system %.0f of %.0f MB); waiting "
               "for memory", what, n, proc, used, limit);
    }
    uint64_t t0 = plat_now_ms();
    while (plat_now_ms() - t0 < 30000) {
        plat_sleep_ms(10);
        if ((p = realloc(old, n))) {
            if (report)
                rt_log("nvn: %s of %zu bytes succeeded after %llu ms", what, n,
                       (unsigned long long)(plat_now_ms() - t0));
            return p;
        }
    }
    return NULL;
}

static void cmd_reserve(cmd_t *o, size_t need) {
    if (o->seg && o->cur + need + OP_RESERVE <= o->end) return;
    if (!o->seg) {
        size_t size = 0;
        uint8_t *p = pool_take(&size);
        if (p) {
            o->seg = o->cur = p;
            o->end = p + size;
            if (need + OP_RESERVE <= size) return;
        }
    }
    size_t used = o->seg ? (size_t)(o->cur - o->seg) : 0;
    size_t cap = o->seg ? (size_t)(o->end - o->seg) : 0;
    if (!cap) cap = 64 << 10;
    while (used + need + OP_RESERVE > cap) cap *= 2;
    uint8_t *b = realloc_wait(o->seg, cap, "command recording buffer");
    if (!b)
        rt_fatal("nvn: command recording buffer of %zu bytes (command buffer %p, magic %#x, buffer %p of %zu bytes, %zu "
                 "used, %zu asked)", cap, (void *)o, o->magic, (void *)o->seg, o->seg ? (size_t)(o->end - o->seg) : 0,
                 used, need);
    o->seg = b;
    o->cur = b + used;
    o->end = b + cap;
}

static uint8_t *cmd_alloc(cmd_t *o, uint32_t size) {
    size = (size + 7) & ~7u;
    cmd_reserve(o, size);
    uint8_t *p = o->cur;
    o->cur += size;
    o->cmd_used += size;
    o->hw_used += size;
    o->last_rec = size;
    put_hdr(p, 0, size);
    return p;
}

static void hw_fix(cmd_t *o, size_t bytes) {
    o->hw_used += bytes;
    o->hw_used -= o->last_rec;
    o->last_rec = bytes;
}

static void *rec(cmd_t *o, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(o, sz);
    put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(o, OP, T) ((T *)rec((o), (OP), sizeof(T)))
#define RECN(o, OP, T, ARR, N) ((T *)rec((o), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))

static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) {
    *has = v != NULL;
    if (v) *dst = *(const view_t *)v;
}

typedef struct {
    tex_t t;
    uint64_t last_use;
    uint32_t refs;
    uint8_t queued;
} pin_t;
_Static_assert(offsetof(pin_t, t) == 0, "ops hold pins as tex_t pointers");
typedef struct { pin_t *p; uint64_t at; } pin_due;
static PlatMutex pin_lock = PLAT_MUTEX_INIT;
static nvn_hmap pins = {.klen = 8};
static pin_due *pin_q;
static size_t pin_qh, pin_qn, pin_qcap;
static uint64_t pins_live, pins_freed;
static uint64_t nsubmits;

static void pin_queue(pin_t *p, uint64_t at) {
    if (p->queued) return;
    if (pin_qh && pin_qh * 2 >= pin_qn) {
        memmove(pin_q, pin_q + pin_qh, sizeof *pin_q * (pin_qn - pin_qh));
        pin_qn -= pin_qh, pin_qh = 0;
    }
    if (pin_qn == pin_qcap) {
        pin_qcap = pin_qcap ? pin_qcap * 2 : 4096;
        if (!(pin_q = realloc(pin_q, sizeof *pin_q * pin_qcap))) rt_fatal("nvn: out of memory (texture pin queue)");
    }
    pin_q[pin_qn++] = (pin_due){p, at};
    p->queued = 1;
}

static const tex_t *tex_pin(const NVNtexture *nt) {
    const tex_t *t = (const tex_t *)nt;
    if (!t) return NULL;
    uint64_t serial = t->serial;
    if (!serial) rt_fatal("nvn: a recording references the uninitialized texture object %p", (const void *)t);
    uint64_t now = __atomic_load_n(&nsubmits, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&pin_lock);
    pin_t *r = nvn_hm_get(&pins, &serial);
    if (r) __atomic_store_n(&r->last_use, now, __ATOMIC_RELAXED);
    plat_mutex_unlock_shared(&pin_lock);
    if (r) return &r->t;
    plat_mutex_lock(&pin_lock);
    r = nvn_hm_get(&pins, &serial);
    if (!r) {
        if (!(r = malloc(sizeof *r))) rt_fatal("nvn: texture pin copy");
        *r = (pin_t){.t = *t};
        nvn_hm_put(&pins, &serial, r);
        pins_live++;
        pin_queue(r, now);
    }
    __atomic_store_n(&r->last_use, now, __ATOMIC_RELAXED);
    plat_mutex_unlock(&pin_lock);
    return &r->t;
}

#define PIN_OPS (1ull << OP_SET_RT | 1ull << OP_COPY_B2T | 1ull << OP_COPY_T2B | 1ull << OP_CLEAR_TEX | 1ull << OP_COPY_T2T)
_Static_assert(OP_COUNT_ <= 64, "PIN_OPS holds one bit per op kind");

static int op_pins(uint32_t op, const void *a, const tex_t *out[9]) {
    int k = 0;
    switch (op) {
    case OP_SET_RT: {
        const p_set_rt *r = a;
        int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
        for (int i = 0; i < n; i++)
            if (r->color[i]) out[k++] = r->color[i];
        if (r->depth) out[k++] = r->depth;
        break;
    }
    case OP_COPY_B2T: case OP_COPY_T2B:
        if (((const p_copy_b2t *)a)->tex) out[k++] = ((const p_copy_b2t *)a)->tex;
        break;
    case OP_CLEAR_TEX:
        if (((const p_clear_tex *)a)->tex) out[k++] = ((const p_clear_tex *)a)->tex;
        break;
    case OP_COPY_T2T: {
        const p_copy_t2t *c = a;
        if (c->src) out[k++] = c->src;
        if (c->dst) out[k++] = c->dst;
        break;
    }
    default: break;
    }
    return k;
}

static void pin_ref(const tex_t *t, int delta, uint64_t now) {
    pin_t *p = (pin_t *)(uintptr_t)t;
    if (delta > 0) {
        __atomic_add_fetch(&p->refs, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&p->last_use, now, __ATOMIC_RELAXED);
    if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) return;
    plat_mutex_lock(&pin_lock);
    pin_queue(p, now);
    plat_mutex_unlock(&pin_lock);
}

static uint32_t pin_refs(const uint8_t *copy, int delta) {
    uint64_t now = __atomic_load_n(&nsubmits, __ATOMIC_RELAXED);
    uint32_t n = 0;
    for (const op_hdr *h = nvn_op_first((uintptr_t)copy); h->op != OP_END; h = nvn_op_next(h)) {
        if (h->op >= 64 || !(PIN_OPS >> h->op & 1)) continue;
        const tex_t *p[9];
        int k = op_pins(h->op, nvn_op_payload(h), p);
        for (int i = 0; i < k; i++) pin_ref(p[i], delta, now);
        n += (uint32_t)k;
    }
    return n;
}

void nvn_pins_sweep(uint64_t now, const tex_t *const *keep, int nkeep) {
    if (now < NVN_PIN_AGE) return;
    static int on = -1;
    if (on < 0) {
        const char *s = cfg_get("NX2SARIA_PIN_SWEEP");
        on = !(s && !strcmp(s, "0"));
        if (!on) rt_log("nvn: NX2SARIA_PIN_SWEEP=0: texture pin copies are kept for the run (the heap grows in long sessions)");
    }
    if (!on) return;
    static uint64_t logged;
    plat_mutex_lock(&pin_lock);
    for (int budget = 1024; pin_qh < pin_qn && budget; budget--) {
        pin_due d = pin_q[pin_qh];
        if (d.at > now - NVN_PIN_AGE) break;
        pin_qh++;
        pin_t *p = d.p;
        p->queued = 0;
        if (__atomic_load_n(&p->refs, __ATOMIC_ACQUIRE)) continue;
        uint64_t last = __atomic_load_n(&p->last_use, __ATOMIC_RELAXED);
        int kept = 0;
        for (int i = 0; i < nkeep; i++) kept |= keep[i] == &p->t;
        if (kept || last > now - NVN_PIN_AGE) {
            pin_queue(p, kept ? now : last);
            continue;
        }
        nvn_hm_del(&pins, &p->t.serial);
        free(p);
        pins_live--, pins_freed++;
    }
    if (now / 7200 != logged) {
        logged = now / 7200;
        rt_log("nvn: texture pins at submit %llu: %llu live, %llu waiting, %llu freed so far", (unsigned long long)now,
               (unsigned long long)pins_live, (unsigned long long)(pin_qn - pin_qh), (unsigned long long)pins_freed);
    }
    plat_mutex_unlock(&pin_lock);
}

void nvn_pins_stats(uint64_t *live, uint64_t *freed) {
    plat_mutex_lock_shared(&pin_lock);
    *live = pins_live, *freed = pins_freed;
    plat_mutex_unlock_shared(&pin_lock);
}

typedef struct { uint64_t magic, bytes, made_at, used_at, deferred, used_min; uint32_t pins, busy; } copy_prefix;
_Static_assert(sizeof(copy_prefix) % 8 == 0, "records stay 8-byte aligned");
_Static_assert(sizeof(copy_prefix) == 56, "copy_prefix keeps its size");
typedef struct { uintptr_t lo, hi; uint8_t *copy; } cmd_copy;
typedef struct { cmd_copy *v; uint32_t n, cap; } copy_bucket;
static PlatMutex copy_lock = PLAT_MUTEX_INIT;
static nvn_hmap copy_pages = {.klen = 8};
static nvn_hmap copy_live = {.klen = 8};
static uint64_t ncopies, copy_bytes, copies_made;

#define COPY_AGE_MINS 65536
#define COPY_IDLE_MINS 5
static volatile int64_t age_n[COPY_AGE_MINS], age_b[COPY_AGE_MINS];

static uint64_t copy_minute(void) { return plat_now_ns() / 60000000000ull; }

static void age_add(uint64_t min, const copy_prefix *pf, int64_t sign) {
    plat_add64(&age_n[min % COPY_AGE_MINS], sign);
    plat_add64(&age_b[min % COPY_AGE_MINS], sign * (int64_t)pf->bytes);
}

static void copy_age(copy_prefix *pf, uint64_t min) {
    uint64_t was = __atomic_exchange_n(&pf->used_min, min, __ATOMIC_RELAXED);
    if (was != min) age_add(was, pf, -1), age_add(min, pf, 1);
}
static copy_prefix **zombies;
static uint32_t nzombies, cap_zombies;

static void zombie_add(copy_prefix *pf) {
    if (nzombies == cap_zombies) {
        cap_zombies = cap_zombies ? cap_zombies * 2 : 64;
        zombies = realloc(zombies, sizeof *zombies * cap_zombies);
        if (!zombies) rt_fatal("nvn: out of memory (released recording copies)");
    }
    zombies[nzombies++] = pf;
}

static void copy_free(copy_prefix *pf) {
    if (pf->pins) pin_refs((const uint8_t *)(pf + 1), -1);
    free(pf);
}

static copy_prefix *doom(copy_prefix *list, copy_prefix *pf) {
    pf->made_at = (uint64_t)(uintptr_t)list;
    return pf;
}

static void doom_free(copy_prefix *list) {
    while (list) {
        copy_prefix *next = (copy_prefix *)(uintptr_t)list->made_at;
        copy_free(list);
        list = next;
    }
}

static copy_prefix *zombie_sweep(uint64_t replayed, copy_prefix *doomed) {
    for (uint32_t i = 0; i < nzombies;) {
        if (zombies[i]->used_at <= replayed && !__atomic_load_n(&zombies[i]->busy, __ATOMIC_ACQUIRE)) {
            doomed = doom(doomed, zombies[i]);
            zombies[i] = zombies[--nzombies];
        } else {
            i++;
        }
    }
    return doomed;
}

static copy_prefix *copy_find(uint64_t handle) {
    return nvn_hm_get(&copy_live, &handle);
}

int nvn_handle_live(uint64_t handle) {
    plat_mutex_lock_shared(&copy_lock);
    int live = copy_find(handle) != NULL;
    plat_mutex_unlock_shared(&copy_lock);
    return live;
}

uint64_t nvn_handle_bytes(uint64_t handle) {
    plat_mutex_lock_shared(&copy_lock);
    const copy_prefix *pf = copy_find(handle);
    uint64_t n = pf ? pf->bytes : 0;
    plat_mutex_unlock_shared(&copy_lock);
    return n;
}

static uint64_t copy_touch(uint64_t handle, uint64_t now, const char *what) {
    plat_mutex_lock_shared(&copy_lock);
    copy_prefix *pf = copy_find(handle);
    uint64_t bytes = 0;
    if (pf) __atomic_store_n(&pf->used_at, now, __ATOMIC_RELAXED), bytes = pf->bytes, copy_age(pf, copy_minute());
    plat_mutex_unlock_shared(&copy_lock);
    if (!pf) rt_fatal("nvn: %s: command handle %#llx is not a live recording (released or never recorded)", what,
                      (unsigned long long)handle);
    return bytes;
}

static uint8_t *flatten_recording(const uint8_t *p, size_t n) {
    uint8_t *out = realloc_wait(NULL, sizeof(copy_prefix) + n, "command copy");
    if (!out) rt_fatal("nvn: command copy allocation");
    memcpy(out + sizeof(copy_prefix), p, n);
    uint64_t pins_named = 0;
    for (size_t at = 0;;) {
        op_hdr h;
        memcpy(&h, p + at, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_ || h.op == OP_JUMP || h.op == OP_CALL || at + h.size > n ||
            (!at && h.op != OP_BEGIN))
            rt_fatal("nvn: corrupt recording at %p (op %u size %u)", (const void *)(p + at), h.op, h.size);
        const tex_t *pinned[9];
        int k = PIN_OPS >> h.op & 1 ? op_pins(h.op, out + sizeof(copy_prefix) + at + 8, pinned) : 0;
        for (int i = 0; i < k; i++) pin_ref(pinned[i], 1, 0);
        pins_named += (uint64_t)k;
        at += h.size;
        if (h.op == OP_END) {
            if (at != n) rt_fatal("nvn: recording at %p ends at %zu of %zu bytes", (const void *)p, at, n);
            break;
        }
    }
    if (pins_named > UINT32_MAX) rt_fatal("nvn: recording at %p names %llu texture pins", (const void *)p,
                                          (unsigned long long)pins_named);
    *(copy_prefix *)out = (copy_prefix){.magic = COPY_MAGIC, .bytes = n, .pins = (uint32_t)pins_named};
    return out + sizeof(copy_prefix);
}

#define RETIRE_PERIOD_MS 5000
#define RETIRE_BATCH 4096
#define RETIRE_CHUNK 64
#define FP_UNKNOWN 0ull
#define FP_GONE 2ull
enum { RS_NONE, RS_CHANGED, RS_GONE };
typedef struct { uint64_t handle, lo, made_at, bytes; } retire_new;
typedef struct { uint64_t handle, lo, made_at, bytes, fp; int state; } retire_rec;
static PlatMutex retire_lock = PLAT_MUTEX_INIT;
static retire_new *retire_q;
static uint32_t retire_qn, retire_qcap;
static retire_rec *retire_recs;
static uint32_t retire_nrec, retire_cap, retire_cursor;
static nvn_hmap retire_at = {.klen = 8};
static volatile int64_t retire_changed_n, retire_changed_b, retire_gone_n, retire_gone_b;

static uint64_t ctl_fingerprint(uint64_t lo) {
    uint8_t b[16];
    if (mem_lazy_touches(lo, sizeof b)) return FP_UNKNOWN;
    if (mem_untouched_run(lo, lo + sizeof b) >= lo + sizeof b) {
        memset(b, 0, sizeof b);
    } else {
        if (!mem_read_hold(lo, sizeof b)) return FP_GONE;
        memcpy(b, (const void *)(uintptr_t)lo, sizeof b);
        mem_read_release();
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < sizeof b; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h | 1;
}

static void retire_count(int state, uint64_t bytes, int64_t sign) {
    if (state == RS_CHANGED) plat_add64(&retire_changed_n, sign), plat_add64(&retire_changed_b, sign * (int64_t)bytes);
    if (state == RS_GONE) plat_add64(&retire_gone_n, sign), plat_add64(&retire_gone_b, sign * (int64_t)bytes);
}

static void retire_visit(retire_rec *r, int live, uint64_t used_min, uint64_t min) {
    int state = RS_NONE;
    if (live && r->fp != FP_UNKNOWN && r->fp != FP_GONE && min >= used_min + COPY_IDLE_MINS) {
        uint64_t now = ctl_fingerprint(r->lo);
        state = now == FP_UNKNOWN ? r->state : now == FP_GONE ? RS_GONE : now != r->fp ? RS_CHANGED : RS_NONE;
    }
    if (state != r->state) retire_count(r->state, r->bytes, -1), retire_count(state, r->bytes, 1), r->state = state;
}

static int retire_on(void) {
    static volatile int32_t mode = -1;
    int32_t m = __atomic_load_n(&mode, __ATOMIC_RELAXED);
    if (m < 0) __atomic_store_n(&mode, m = cfg_int("NX2SARIA_COPY_RETIRE"), __ATOMIC_RELAXED);
    return m > 0;
}

static void retire_queue(uint64_t handle, uint64_t lo, uint64_t made_at, uint64_t bytes) {
    plat_mutex_lock(&retire_lock);
    if (retire_qn == retire_qcap) {
        uint32_t cap = retire_qcap ? retire_qcap * 2 : 1024;
        retire_new *q = realloc(retire_q, cap * sizeof *q);
        if (q) retire_q = q, retire_qcap = cap;
    }
    if (retire_qn < retire_qcap) retire_q[retire_qn++] = (retire_new){handle, lo, made_at, bytes};
    plat_mutex_unlock(&retire_lock);
}

static void retire_drop(uint32_t i) {
    retire_rec *r = &retire_recs[i];
    retire_count(r->state, r->bytes, -1);
    nvn_hm_del(&retire_at, &r->handle);
    if (i != --retire_nrec) {
        *r = retire_recs[retire_nrec];
        nvn_hm_put(&retire_at, &r->handle, (void *)(uintptr_t)(i + 1));
    }
}

static void retire_pass(void) {
    plat_mutex_lock(&retire_lock);
    retire_new *q = retire_q;
    uint32_t qn = retire_qn;
    retire_q = NULL, retire_qn = retire_qcap = 0;
    plat_mutex_unlock(&retire_lock);
    for (uint32_t i = 0; i < qn; i++) {
        void *at = nvn_hm_get(&retire_at, &q[i].handle);
        if (at) retire_drop((uint32_t)(uintptr_t)at - 1);
        if (retire_nrec == retire_cap) {
            uint32_t cap = retire_cap ? retire_cap * 2 : 1024;
            retire_rec *v = realloc(retire_recs, cap * sizeof *v);
            if (!v) break;
            retire_recs = v, retire_cap = cap;
        }
        retire_recs[retire_nrec] =
            (retire_rec){q[i].handle, q[i].lo, q[i].made_at, q[i].bytes, ctl_fingerprint(q[i].lo), RS_NONE};
        nvn_hm_put(&retire_at, &q[i].handle, (void *)(uintptr_t)(++retire_nrec));
    }
    free(q);
    uint64_t min = copy_minute();
    uint32_t todo = retire_nrec < RETIRE_BATCH ? retire_nrec : RETIRE_BATCH;
    while (todo && retire_nrec) {
        if (retire_cursor >= retire_nrec) retire_cursor = 0;
        uint32_t c = retire_cursor, n = retire_nrec - c;
        if (n > todo) n = todo;
        if (n > RETIRE_CHUNK) n = RETIRE_CHUNK;
        int live[RETIRE_CHUNK];
        uint64_t used[RETIRE_CHUNK];
        plat_mutex_lock_shared(&copy_lock);
        for (uint32_t k = 0; k < n; k++) {
            const retire_rec *r = &retire_recs[c + k];
            const copy_prefix *pf = copy_find(r->handle);
            live[k] = pf && pf->made_at == r->made_at && pf->bytes == r->bytes;
            used[k] = live[k] ? __atomic_load_n(&pf->used_min, __ATOMIC_RELAXED) : 0;
        }
        plat_mutex_unlock_shared(&copy_lock);
        for (uint32_t k = n; k-- > 0;) {
            retire_rec *r = &retire_recs[c + k];
            retire_visit(r, live[k], used[k], min);
            if (!live[k]) retire_drop(c + k);
        }
        retire_cursor = c + n;
        todo -= n;
    }
}

static void retire_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_NORMAL);
    for (;;) {
        plat_sleep_ms(RETIRE_PERIOD_MS);
        retire_pass();
    }
}

static void copy_add(uintptr_t lo, uintptr_t hi, uint8_t *copy) {
    copy_prefix *pf = (copy_prefix *)copy - 1;
    uint64_t handle = (uintptr_t)copy;
    uintptr_t page = lo >> COPY_PAGE_SHIFT;
    plat_mutex_lock(&copy_lock);
    pf->made_at = nsubmits;
    copy_bucket *b = nvn_hm_get(&copy_pages, &page);
    if (!b) {
        b = calloc(1, sizeof *b);
        if (!b) rt_fatal("nvn: command copy index");
        nvn_hm_put(&copy_pages, &page, b);
    }
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->v = realloc(b->v, b->cap * sizeof *b->v);
        if (!b->v) rt_fatal("nvn: command copy index");
    }
    b->v[b->n++] = (cmd_copy){lo, hi, copy};
    nvn_hm_put(&copy_live, &handle, pf);
    ncopies++;
    copy_bytes += pf->bytes;
    pf->used_min = copy_minute();
    age_add(pf->used_min, pf, 1);
    if (++copies_made % 20000 == 0)
        NVN_LOG("recording copies: %llu made, %llu live, %.1f MiB, submit %llu", (unsigned long long)copies_made,
                (unsigned long long)ncopies, copy_bytes / 1048576.0, (unsigned long long)nsubmits);
    uint64_t made_at = pf->made_at, bytes = pf->bytes;
    plat_mutex_unlock(&copy_lock);
    if (hi && retire_on()) {
        static volatile int32_t started, failed;
        if (!plat_xchg32(&started, 1) && !plat_thread_spawn(retire_main, NULL, "nvn copy sweep", 0)) {
            plat_xchg32(&failed, 1);
            rt_log("nvn: the copy sweep thread did not start: NX2SARIA_COPY_RETIRE counts nothing");
        }
        if (!__atomic_load_n(&failed, __ATOMIC_RELAXED)) retire_queue(handle, lo, made_at, bytes);
    }
}

static void release_copies(uintptr_t lo, uintptr_t hi) {
    if (hi <= lo) return;
    uint64_t replayed = gfx_replayed_submits();
    copy_prefix *doomed = NULL;
    plat_mutex_lock(&copy_lock);
    if (nzombies) doomed = zombie_sweep(replayed, doomed);
    for (uintptr_t pg = lo >> COPY_PAGE_SHIFT; pg <= (hi - 1) >> COPY_PAGE_SHIFT; pg++) {
        copy_bucket *b = nvn_hm_get(&copy_pages, &pg);
        if (!b) continue;
        for (uint32_t i = 0; i < b->n;) {
            cmd_copy *k = &b->v[i];
            if (!(k->lo < hi && lo < k->hi)) { i++; continue; }
            uint64_t handle = (uintptr_t)k->copy;
            copy_prefix *pf = (copy_prefix *)k->copy - 1;
            nvn_hm_del(&copy_live, &handle);
            ncopies--;
            copy_bytes -= pf->bytes;
            age_add(pf->used_min, pf, -1);
            pf->magic = 0;
            if (pf->used_at > replayed || __atomic_load_n(&pf->busy, __ATOMIC_ACQUIRE)) zombie_add(pf);
            else doomed = doom(doomed, pf);
            b->v[i] = b->v[--b->n];
        }
        if (!b->n) {
            nvn_hm_del(&copy_pages, &pg);
            free(b->v);
            free(b);
        }
    }
    plat_mutex_unlock(&copy_lock);
    doom_free(doomed);
}

NVN_API(nvnCommandBufferInitialize) {
    cmd_t *o = cmd_of(c);
    static uint32_t reinits;
    if (o->magic == CMD_MAGIC && o->seg && __atomic_fetch_add(&reinits, 1, __ATOMIC_RELAXED) < 8)
        rt_log("nvn: Initialize over the command buffer %p, which was not finalized; its %zu-byte recording buffer %p "
               "is dropped", (void *)o, (size_t)(o->end - o->seg), (void *)o->seg);
    memset(o, 0, sizeof *o);
    o->magic = CMD_MAGIC;
    o->dev = arg_p(c, 1);
    o->inline_cur = ~0ull;
    o->inline_end = 0;
    cb_set_disabled(o, CB_NO_COMMAND | CB_NO_CONTROL, 0);
    ret_x(c, 1);
}
NVN_API(nvnCommandBufferFinalize) {
    cmd_t *o = cmd_of(c);
    pool_put(o->seg, (size_t)(o->end - o->seg));
    o->seg = o->cur = o->end = NULL;
    o->magic = 0;
    cb_set_disabled(o, CB_NO_COMMAND | CB_NO_CONTROL, 0);
}
NVN_API(nvnCommandBufferSetMemoryCallback) { cmd_of(c)->cb = arg_p(c, 1); }
NVN_API(nvnCommandBufferSetMemoryCallbackData) { cmd_of(c)->cb_data = arg_p(c, 1); }
NVN_API(nvnCommandBufferGetMemoryCallback) { ret_x(c, (uintptr_t)cmd_of(c)->cb); }
NVN_API(nvnCommandBufferGetMemoryCallbackData) { ret_x(c, (uintptr_t)cmd_of(c)->cb_data); }
NVN_API(nvnCommandBufferSetCommandMemoryCallbackEnabled) { cb_set_disabled(cmd_of(c), CB_NO_COMMAND, !nvn_arg_bool(c, 1)); }
NVN_API(nvnCommandBufferSetControlMemoryCallbackEnabled) { cb_set_disabled(cmd_of(c), CB_NO_CONTROL, !nvn_arg_bool(c, 1)); }
NVN_API(nvnCommandBufferAddCommandMemory) {
    cmd_t *o = cmd_of(c);
    const pool_t *p = arg_p(c, 1);
    o->cmd_base = (uintptr_t)(p ? p->mem : NULL) + (uintptr_t)arg_x(c, 2);
    o->game_size = arg_x(c, 3);
    o->cmd_used = 0;
    o->hw_used = 0;
}
NVN_API(nvnCommandBufferAddControlMemory) {
    cmd_t *o = cmd_of(c);
    void *m = arg_p(c, 1);
    size_t n = arg_x(c, 2);
    release_copies((uintptr_t)m, (uintptr_t)m + n);
    o->ctl = m;
    o->ctl_size = n;
    o->ctl_used = 0;
}
static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
NVN_API(nvnCommandBufferGetControlMemorySize) { ret_x(c, cmd_of(c)->ctl_size); }
NVN_API(nvnCommandBufferGetControlMemoryUsed) { const cmd_t *o = cmd_of(c); ret_x(c, min_size(o->ctl_used, o->ctl_size)); }
NVN_API(nvnCommandBufferGetControlMemoryFree) { const cmd_t *o = cmd_of(c); ret_x(c, o->ctl_size - min_size(o->ctl_used, o->ctl_size)); }
NVN_API(nvnCommandBufferGetCommandMemorySize) { ret_x(c, cmd_of(c)->game_size); }
NVN_API(nvnCommandBufferGetCommandMemoryUsed) { const cmd_t *o = cmd_of(c); ret_x(c, min_size(o->hw_used, o->game_size)); }
NVN_API(nvnCommandBufferGetCommandMemoryFree) { const cmd_t *o = cmd_of(c); ret_x(c, o->game_size - min_size(o->hw_used, o->game_size)); }

NVN_API(nvnCommandBufferBeginRecording) {
    cmd_t *o = cmd_of(c);
    if (o->recording) rt_log("nvn: BeginRecording on command buffer %p while it records", (void *)o);
    o->recording = 1;
    o->cur = o->seg;
    o->rec_start = o->cmd_used;
    put_hdr(cmd_alloc(o, 8), OP_BEGIN, 8);
    o->hw_used -= 8;
    o->hw_start = o->hw_used;
    if (!o->game_size) memory_event(c, o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, MIN_COMMAND_SIZE);
}
NVN_API(nvnCommandBufferEndRecording) {
    cmd_t *o = cmd_of(c);
    if (!o->recording || !o->seg) rt_fatal("nvn: EndRecording on command buffer %p, which does not record", (void *)o);
    if (!o->ctl_size) memory_event(c, o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY, MIN_CONTROL_SIZE);
    put_hdr(cmd_alloc(o, 8), OP_END, 8);
    o->hw_used -= 8;
    o->recording = 0;
    uint8_t *copy = flatten_recording(o->seg, (size_t)(o->cur - o->seg));
    uintptr_t lo = (uintptr_t)o->ctl + (o->ctl_used < o->ctl_size ? o->ctl_used : 0);
    uintptr_t hi = o->ctl ? lo + 1 : 0;
    o->ctl_used += CTL_PER_RECORDING;
    copy_add(lo, hi, copy);
    ret_x(c, (uintptr_t)copy);
}
NVN_API(nvnCommandBufferIsRecording) { ret_x(c, cmd_of(c)->recording != 0); }

static void inline_recording(cmd_t *o, uint64_t handle, const char *what) {
    if (!handle) rt_fatal("nvn: %s with a null handle", what);
    uint64_t bytes = copy_touch(handle, __atomic_load_n(&nsubmits, __ATOMIC_RELAXED), what);
    if (bytes <= 16) return;
    size_t body = (size_t)bytes - 16;
    cmd_reserve(o, body);
    memcpy(o->cur, (const uint8_t *)(uintptr_t)handle + 8, body);
    o->cur += body;
    o->cmd_used += body;
    o->hw_used += body;
    o->last_rec = 0;
}
NVN_API(nvnCommandBufferCallCommands) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    for (int i = 0; i < n; i++) inline_recording(o, h[i], "CallCommands");
}
NVN_API(nvnCommandBufferCopyCommands) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    for (int i = 0; i < n; i++) inline_recording(o, h[i], "CopyCommands");
}

NVN_API(nvnCommandBufferSignalEvent) {
    const ev_t *e = arg_p(c, 1);
    *REC(cmd_of(c), OP_SIGNAL_EVENT, p_signal_event) = (p_signal_event){e->val, (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 5)};
}
NVN_API(nvnCommandBufferResetCounter) { REC(cmd_of(c), OP_RESET_COUNTER, p_reset_counter)->type = (uint32_t)arg_x(c, 1); }
NVN_API(nvnCommandBufferReportCounter) {
    *REC(cmd_of(c), OP_REPORT_COUNTER, p_report_counter) = (p_report_counter){(uint32_t)arg_x(c, 1), arg_x(c, 2)};
}
NVN_API(nvnCommandBufferCopyBufferToBuffer) {
    *REC(cmd_of(c), OP_COPY_B2B, p_copy_b2b) = (p_copy_b2b){arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
}
#define PUSH_CC_MAX 1024
NVN_API(nvnCommandBufferPushComputeConstants) {
    uint64_t off = arg_x(c, 1), n = arg_x(c, 3);
    const void *data = arg_p(c, 4);
    static int logged;
    if (arg_x(c, 2) && !logged++) NVN_LOG("PushComputeConstants: second argument %#llx not understood", (unsigned long long)arg_x(c, 2));
    static struct { uint64_t a1, a2, a3; } seen[24];
    static int nseen;
    int known = 0;
    for (int i = 0; i < nseen; i++) known |= seen[i].a1 == off && seen[i].a2 == arg_x(c, 2) && seen[i].a3 == n;
    if (!known && nseen < 24) {
        seen[nseen++] = (typeof(seen[0])){off, arg_x(c, 2), n};
        rt_log("nvn: PushComputeConstants: x1 %#llx x2 %#llx x3 %#llx x4 %p x5 %#llx x6 %#llx", (unsigned long long)off,
                (unsigned long long)arg_x(c, 2), (unsigned long long)n, data, (unsigned long long)arg_x(c, 5),
                (unsigned long long)arg_x(c, 6));
    }
    if (!data || off + n > PUSH_CC_MAX) { NVN_LOG("PushComputeConstants: %#llx bytes at %#llx dropped", (unsigned long long)n, (unsigned long long)off); return; }
    p_push_cc *r = rec(cmd_of(c), OP_PUSH_CC, offsetof(p_push_cc, data) + n);
    r->offset = (uint32_t)off;
    r->size = (uint32_t)n;
    memcpy(r->data, data, n);
}
NVN_API(nvnCommandBufferUpdateUniformBuffer) {
    uint64_t dst = arg_x(c, 1) + arg_x(c, 3), n = arg_x(c, 4);
    const void *data = arg_p(c, 5);
    if (n > (16u << 20)) rt_fatal("nvn: UpdateUniformBuffer of %#llx bytes", (unsigned long long)n);
    p_update_ub *r = rec(cmd_of(c), OP_UPDATE_UB, offsetof(p_update_ub, data) + n);
    r->dst = dst;
    r->size = n;
    memcpy(r->data, data, n);
}

NVN_API(nvnCommandBufferSetRenderTargets) {
    int n = (int)arg_x(c, 1);
    const NVNtexture *const *ct = arg_p(c, 2);
    const NVNtextureView *const *cv = arg_p(c, 3);
    const NVNtexture *dt = arg_p(c, 4);
    const NVNtextureView *dv = arg_p(c, 5);
    if (n < 0 || n > 8) {
        rt_log("nvn: SetRenderTargets with %d colour targets", n);
        n = n < 0 ? 0 : 8;
    }
    p_set_rt *r = REC(cmd_of(c), OP_SET_RT, p_set_rt);
    r->n = n;
    for (int i = 0; i < n; i++) {
        r->color[i] = ct ? tex_pin(ct[i]) : NULL;
        copy_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : NULL);
    }
    r->depth = tex_pin(dt);
    copy_view(&r->dview, &r->has_dview, dv);
}
NVN_API(nvnCommandBufferClearColor) {
    p_clear_color *r = REC(cmd_of(c), OP_CLEAR_COLOR, p_clear_color);
    r->index = (int)arg_x(c, 1);
    r->mask = (int)arg_x(c, 3);
    memcpy(r->rgba, arg_p(c, 2), 16);
}
NVN_API(nvnCommandBufferClearDepthStencil) {
    *REC(cmd_of(c), OP_CLEAR_DS, p_clear_ds) = (p_clear_ds){arg_s(c, 0), nvn_arg_bool(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3)};
}
static void set_viewports(cmd_t *o, int first, int n, const float *v) {
    if (first < 0 || n < 0 || first + n > 16) {
        rt_log("nvn: SetViewports first %d n %d", first, n);
        return;
    }
    p_viewports *r = REC(o, OP_VIEWPORTS, p_viewports);
    r->first = first;
    r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
NVN_API(nvnCommandBufferSetViewports) { set_viewports(cmd_of(c), (int)arg_x(c, 1), (int)arg_x(c, 2), arg_p(c, 3)); }
NVN_API(nvnCommandBufferSetViewport) {
    float v[4] = {(float)(int)arg_x(c, 1), (float)(int)arg_x(c, 2), (float)(int)arg_x(c, 3), (float)(int)arg_x(c, 4)};
    set_viewports(cmd_of(c), 0, 1, v);
}
static void set_scissor(cmd_t *o, int x, int y, int w, int h) { *REC(o, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h}; }
NVN_API(nvnCommandBufferSetScissor) {
    set_scissor(cmd_of(c), (int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 4));
}
NVN_API(nvnCommandBufferSetScissors) {
    int first = (int)arg_x(c, 1), n = (int)arg_x(c, 2);
    const int *r = arg_p(c, 3);
    if (first < 0 || n < 0 || first + n > 16) rt_fatal("nvn: SetScissors first %d n %d", first, n);
    for (int i = 1; i < n; i++)
        if (memcmp(r + 4 * i, r, 16)) {
            static int once;
            if (!once++) rt_log("nvn: SetScissors: differing scissor %d ignored (single-scissor replay)", first + i);
        }
    if (first == 0 && n > 0) set_scissor(cmd_of(c), r[0], r[1], r[2], r[3]);
}
static void set_depth_range(cmd_t *o, float n, float f) { *REC(o, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
NVN_API(nvnCommandBufferSetDepthRange) { set_depth_range(cmd_of(c), arg_s(c, 0), arg_s(c, 1)); }
NVN_API(nvnCommandBufferSetDepthRanges) {
    int first = (int)arg_x(c, 1), n = (int)arg_x(c, 2);
    const float *r = arg_p(c, 3);
    if (first < 0 || n < 0 || first + n > 16) rt_fatal("nvn: SetDepthRanges first %d n %d", first, n);
    for (int i = 1; i < n; i++)
        if (memcmp(r + 2 * i, r, 8)) {
            static int once;
            if (!once++) rt_log("nvn: SetDepthRanges: differing range %d ignored (single-range replay)", first + i);
        }
    if (first == 0 && n > 0) set_depth_range(cmd_of(c), r[0], r[1]);
}
NVN_API(nvnCommandBufferSetDepthClamp) { REC(cmd_of(c), OP_DEPTH_CLAMP, p_flag)->value = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnCommandBufferSetRenderEnable) { REC(cmd_of(c), OP_RENDER_ENABLE, p_flag)->value = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnCommandBufferSetRasterizerDiscard) { REC(cmd_of(c), OP_RASTER_DISCARD, p_flag)->value = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnCommandBufferSetPrimitiveRestart) {
    *REC(cmd_of(c), OP_PRIM_RESTART, p_prim_restart) = (p_prim_restart){nvn_arg_bool(c, 1) != 0, (uint32_t)arg_x(c, 2)};
}
NVN_API(nvnCommandBufferSetRenderEnableConditional) {
    int m = (int)arg_x(c, 1);
    if (m != 0 && m != 1) rt_fatal("nvn: SetRenderEnableConditional: unknown mode %d", m);
    p_render_cond *r = REC(cmd_of(c), OP_RENDER_COND, p_render_cond);
    r->mode = m;
    r->addr = arg_x(c, 2);
}
NVN_API(nvnCommandBufferBindProgram) {
    *REC(cmd_of(c), OP_BIND_PROGRAM, p_bind_program) = (p_bind_program){arg_p(c, 1), (int)arg_x(c, 2)};
}
static void bind_state(const Cpu *c, int kind, size_t n) {
    p_bind_state *r = REC(cmd_of(c), OP_BIND_STATE, p_bind_state);
    r->kind = kind;
    memcpy(r->bytes, arg_p(c, 1), n);
}
NVN_API(nvnCommandBufferBindBlendState) { bind_state(c, 0, sizeof(blend_st)); }
NVN_API(nvnCommandBufferBindColorState) { bind_state(c, 1, sizeof(color_st)); }
NVN_API(nvnCommandBufferBindChannelMaskState) { bind_state(c, 2, sizeof(chmask_st)); }
NVN_API(nvnCommandBufferBindMultisampleState) { bind_state(c, 3, sizeof(ms_st)); }
NVN_API(nvnCommandBufferBindPolygonState) { bind_state(c, 4, sizeof(poly_st)); }
NVN_API(nvnCommandBufferBindDepthStencilState) { bind_state(c, 5, sizeof(ds_st)); }
NVN_API(nvnCommandBufferSetPointSize) {
    p_bind_state *r = REC(cmd_of(c), OP_BIND_STATE, p_bind_state);
    float s = arg_s(c, 0);
    r->kind = 6;
    memcpy(r->bytes, &s, sizeof s);
}
NVN_API(nvnCommandBufferSetLineWidth) {
    p_bind_state *r = REC(cmd_of(c), OP_BIND_STATE, p_bind_state);
    float w = arg_s(c, 0);
    r->kind = 7;
    memcpy(r->bytes, &w, sizeof w);
}
NVN_API(nvnCommandBufferBindVertexAttribState) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    if (n < 0 || n > 16) { rt_log("nvn: BindVertexAttribState n %d", n); return; }
    p_bind_vattr *r = RECN(o, OP_BIND_VATTR, p_bind_vattr, st, n);
    r->first = 0;
    r->n = n;
    memcpy(r->st, arg_p(c, 2), 4 * (size_t)n);
    hw_fix(o, 68);
}
NVN_API(nvnCommandBufferBindVertexStreamState) {
    cmd_t *o = cmd_of(c);
    int n = (int)arg_x(c, 1);
    if (n < 0 || n > 16) { rt_log("nvn: BindVertexStreamState n %d", n); return; }
    p_bind_vstream *r = RECN(o, OP_BIND_VSTREAM, p_bind_vstream, st, n);
    r->first = 0;
    r->n = n;
    memcpy(r->st, arg_p(c, 2), 8 * (size_t)n);
    hw_fix(o, 4 * (16 + (size_t)n));
}
static void bind_vertex_buffers(cmd_t *o, int first, int n, const NVNbufferRange *rg) {
    if (first < 0 || n < 0 || first + n > 16) { rt_log("nvn: BindVertexBuffers first %d n %d", first, n); return; }
    p_bind_vbuf *r = RECN(o, OP_BIND_VBUF, p_bind_vbuf, r, n);
    r->first = first;
    r->n = n;
    memcpy(r->r, rg, sizeof *rg * (size_t)n);
    hw_fix(o, 24 * (size_t)n);
}
NVN_API(nvnCommandBufferBindVertexBuffers) { bind_vertex_buffers(cmd_of(c), (int)arg_x(c, 1), (int)arg_x(c, 2), arg_p(c, 3)); }
NVN_API(nvnCommandBufferBindVertexBuffer) {
    NVNbufferRange r = {arg_x(c, 2), arg_x(c, 3)};
    bind_vertex_buffers(cmd_of(c), (int)arg_x(c, 1), 1, &r);
}
static void bind_buffers(const Cpu *c, uint32_t op) {
    cmd_t *o = cmd_of(c);
    int st = (int)arg_x(c, 1), first = (int)arg_x(c, 2), n = (int)arg_x(c, 3);
    const NVNbufferRange *rg = arg_p(c, 4);
    for (int i = 0; i < n; i++) {
        *REC(o, op, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
        hw_fix(o, 20);
    }
}
static void bind_buffer(const Cpu *c, uint32_t op) {
    cmd_t *o = cmd_of(c);
    *REC(o, op, p_bind_buf) = (p_bind_buf){(int)arg_x(c, 1), (int)arg_x(c, 2), arg_x(c, 3), arg_x(c, 4)};
    hw_fix(o, 20);
}
NVN_API(nvnCommandBufferBindUniformBuffer) { bind_buffer(c, OP_BIND_UBO); }
NVN_API(nvnCommandBufferBindUniformBuffers) { bind_buffers(c, OP_BIND_UBO); }
NVN_API(nvnCommandBufferBindStorageBuffer) { bind_buffer(c, OP_BIND_SSBO); }
NVN_API(nvnCommandBufferBindStorageBuffers) { bind_buffers(c, OP_BIND_SSBO); }
static void bind_handles(cmd_t *o, uint32_t op, int st, int first, int n, const uint64_t *h, int deferred, size_t hw_bytes) {
    if (n < 0 || n > 32) { rt_log("nvn: bind of %d handles", n); return; }
    p_bind_handles *r = RECN(o, op, p_bind_handles, h, n);
    r->stage = st;
    r->first = first;
    r->n = n;
    r->src = deferred ? (uint64_t)(uintptr_t)h : 0;
    memcpy(r->h, h, 8 * (size_t)n);
    hw_fix(o, hw_bytes ? hw_bytes : 16 + 4 * (size_t)n);
}
static void bind_handle_array(const Cpu *c, uint32_t op, int deferred, int wide) {
    int n = (int)arg_x(c, 3);
    bind_handles(cmd_of(c), op, (int)arg_x(c, 1), (int)arg_x(c, 2), n, arg_p(c, 4), deferred,
                 wide && n >= 0 && n <= 32 ? 16 + 8 * (size_t)n : 0);
}
static void bind_one_handle(const Cpu *c, uint32_t op, size_t hw_bytes) {
    uint64_t v = arg_x(c, 3);
    bind_handles(cmd_of(c), op, (int)arg_x(c, 1), (int)arg_x(c, 2), 1, &v, 0, hw_bytes);
}
NVN_API(nvnCommandBufferBindTexture) { bind_one_handle(c, OP_BIND_TEX, 0); }
NVN_API(nvnCommandBufferBindTextures) { bind_handle_array(c, OP_BIND_TEX, 0, 0); }
NVN_API(nvnCommandBufferBindImage) { bind_one_handle(c, OP_BIND_IMG, 24); }
NVN_API(nvnCommandBufferBindImages) { bind_handle_array(c, OP_BIND_IMG, 0, 1); }
NVN_API(nvnCommandBufferBindSeparateTexture) { bind_one_handle(c, OP_BIND_TEX, 24); }
NVN_API(nvnCommandBufferBindSeparateTextures) { bind_handle_array(c, OP_BIND_TEX, 0, 1); }
NVN_API(nvnCommandBufferBindSeparateSampler) { bind_one_handle(c, OP_BIND_SAMP, 24); }
NVN_API(nvnCommandBufferBindSeparateSamplers) { bind_handle_array(c, OP_BIND_SAMP, 0, 1); }
NVN_API(nvnCommandBufferBindTexturesDeferred) { bind_handle_array(c, OP_BIND_TEX, 1, 0); }
NVN_API(nvnCommandBufferBindImagesDeferred) { bind_handle_array(c, OP_BIND_IMG, 1, 1); }
NVN_API(nvnCommandBufferBindSeparateTexturesDeferred) { bind_handle_array(c, OP_BIND_TEX, 1, 1); }
NVN_API(nvnCommandBufferBindSeparateSamplersDeferred) { bind_handle_array(c, OP_BIND_SAMP, 1, 1); }
NVN_API(nvnCommandBufferBindUniformBuffersDeferred) {
    static int once;
    if (!once++) NVN_LOG("BindUniformBuffersDeferred: buffer ranges bind at record time");
    bind_buffers(c, OP_BIND_UBO);
}
NVN_API(nvnCommandBufferBindStorageBuffersDeferred) {
    static int once;
    if (!once++) NVN_LOG("BindStorageBuffersDeferred: buffer ranges bind at record time");
    bind_buffers(c, OP_BIND_SSBO);
}
NVN_API(nvnCommandBufferSetStencilValueMask) {
    *REC(cmd_of(c), OP_STENCIL, p_stencil) = (p_stencil){(int)arg_x(c, 1), (int)arg_x(c, 2), 0, 0, 0};
}
NVN_API(nvnCommandBufferSetStencilMask) {
    *REC(cmd_of(c), OP_STENCIL, p_stencil) = (p_stencil){(int)arg_x(c, 1), 0, (int)arg_x(c, 2), 0, 1};
}
NVN_API(nvnCommandBufferSetStencilRef) {
    *REC(cmd_of(c), OP_STENCIL, p_stencil) = (p_stencil){(int)arg_x(c, 1), 0, 0, (int)arg_x(c, 2), 2};
}
NVN_API(nvnCommandBufferSetPolygonOffsetClamp) {
    *REC(cmd_of(c), OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){arg_s(c, 0), arg_s(c, 1), arg_s(c, 2)};
    static int logged, on = -1;
    if (on < 0) on = cfg_env("NX2SARIA_POLY_OFFSET_LOG") != NULL;
    float f = (float)arg_s(c, 0), u = (float)arg_s(c, 1), cl = (float)arg_s(c, 2);
    if (on && logged < 24 && (f != 0.0f || u != 0.0f)) {
        logged++;
        rt_log("nvn: polygon offset factor %.3f units %.3f clamp %.3f", f, u, cl);
    }
}
NVN_API(nvnCommandBufferDrawArrays) {
    *REC(cmd_of(c), OP_DRAW, p_draw) = (p_draw){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), 0, 1};
}
NVN_API(nvnCommandBufferDrawArraysInstanced) {
    int inst = (int)arg_x(c, 5);
    if (inst <= 0) return;
    *REC(cmd_of(c), OP_DRAW, p_draw) = (p_draw){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 4), inst};
}
NVN_API(nvnCommandBufferDrawElements) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), 0, 0, 1, arg_x(c, 4)};
    hw_fix(o, 48);
}
NVN_API(nvnCommandBufferDrawElementsBaseVertex) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_DRAW_IDX, p_draw_idx) =
        (p_draw_idx){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 5), 0, 1, arg_x(c, 4)};
    hw_fix(o, 44);
}
NVN_API(nvnCommandBufferDrawElementsInstanced) {
    cmd_t *o = cmd_of(c);
    *REC(o, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3),
                                                    (int)arg_x(c, 5), (int)arg_x(c, 6), (int)arg_x(c, 7), arg_x(c, 4)};
    hw_fix(o, 44);
}
NVN_API(nvnCommandBufferDrawElementsIndirect) {
    *REC(cmd_of(c), OP_DRAW_IDX_IND, p_draw_idx_ind) = (p_draw_idx_ind){(int)arg_x(c, 1), (int)arg_x(c, 2), arg_x(c, 3), arg_x(c, 4)};
}
NVN_API(nvnCommandBufferDispatchCompute) {
    *REC(cmd_of(c), OP_DISPATCH, p_dispatch) = (p_dispatch){(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), c->x[30]};
}
NVN_API(nvnCommandBufferDispatchComputeIndirect) {
    *REC(cmd_of(c), OP_DISPATCH_IND, p_dispatch_ind) = (p_dispatch_ind){arg_x(c, 1), c->x[30]};
}
NVN_API(nvnCommandBufferDrawMeshTasks) {
    *REC(cmd_of(c), OP_DRAW_MESH, p_draw_mesh) = (p_draw_mesh){(uint32_t)arg_x(c, 1), (uint32_t)arg_x(c, 2), c->x[30]};
}
NVN_API(nvnCommandBufferDrawMeshTasksIndirect) {
    *REC(cmd_of(c), OP_DRAW_MESH_IND, p_draw_mesh_ind) =
        (p_draw_mesh_ind){arg_x(c, 1), arg_x(c, 3), (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 4), c->x[30]};
}
NVN_API(nvnCommandBufferSetGeometryProcessingMode) {
    static int logged;
    if (!logged++) NVN_LOG("SetGeometryProcessingMode(%llu): taken from the bound program", (unsigned long long)arg_x(c, 1));
}
NVN_API(nvnCommandBufferClearBuffer) {
    *REC(cmd_of(c), OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){arg_x(c, 1), arg_x(c, 2), (uint32_t)arg_x(c, 3), 0};
}
NVN_API(nvnCommandBufferReportValue) {
    *REC(cmd_of(c), OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){arg_x(c, 2), 4, (uint32_t)arg_x(c, 1), 0};
}
NVN_API(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN_API(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
NVN_API(nvnCommandBufferBarrier) { REC(cmd_of(c), OP_BARRIER, p_barrier)->bits = (int)arg_x(c, 1); }
NVN_API(nvnCommandBufferSetTexturePool) { REC(cmd_of(c), OP_SET_TEX_POOL, p_set_pool)->pool = arg_p(c, 1); }
NVN_API(nvnCommandBufferSetSamplerPool) { REC(cmd_of(c), OP_SET_SAMP_POOL, p_set_pool)->pool = arg_p(c, 1); }
NVN_API(nvnCommandBufferCopyBufferToTexture) {
    p_copy_b2t *o = REC(cmd_of(c), OP_COPY_B2T, p_copy_b2t);
    o->src = arg_x(c, 1);
    o->tex = tex_pin(arg_p(c, 2));
    copy_view(&o->view, &o->has_view, arg_p(c, 3));
    o->r = *(const NVNcopyRegion *)arg_p(c, 4);
    o->flags = (int)arg_x(c, 5);
}
NVN_API(nvnCommandBufferCopyTextureToBuffer) {
    p_copy_b2t *o = REC(cmd_of(c), OP_COPY_T2B, p_copy_b2t);
    o->src = arg_x(c, 4);
    o->tex = tex_pin(arg_p(c, 1));
    copy_view(&o->view, &o->has_view, arg_p(c, 2));
    o->r = *(const NVNcopyRegion *)arg_p(c, 3);
    o->flags = (int)arg_x(c, 5);
}
static void clear_texture(const Cpu *c, int is_uint) {
    p_clear_tex *o = REC(cmd_of(c), OP_CLEAR_TEX, p_clear_tex);
    o->tex = tex_pin(arg_p(c, 1));
    copy_view(&o->view, &o->has_view, arg_p(c, 2));
    o->r = *(const NVNcopyRegion *)arg_p(c, 3);
    o->mask = (int)arg_x(c, 5);
    o->is_uint = (uint8_t)is_uint;
    memcpy(o->c.u, arg_p(c, 4), 16);
}
NVN_API(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN_API(nvnCommandBufferClearTextureui) { clear_texture(c, 1); }
NVN_API(nvnCommandBufferCopyTextureToTexture) {
    p_copy_t2t *o = REC(cmd_of(c), OP_COPY_T2T, p_copy_t2t);
    o->src = tex_pin(arg_p(c, 1));
    o->dst = tex_pin(arg_p(c, 4));
    copy_view(&o->sv, &o->has_sv, arg_p(c, 2));
    copy_view(&o->dv, &o->has_dv, arg_p(c, 5));
    o->sr = *(const NVNcopyRegion *)arg_p(c, 3);
    o->dr = *(const NVNcopyRegion *)arg_p(c, 6);
    o->flags = (int)arg_x(c, 7);
}
NVN_API(nvnCommandBufferDownsample) {
    p_copy_t2t *o = REC(cmd_of(c), OP_COPY_T2T, p_copy_t2t);
    o->src = tex_pin(arg_p(c, 1));
    o->dst = tex_pin(arg_p(c, 2));
    if (o->src) o->sr = (NVNcopyRegion){0, 0, 0, (int)o->src->d.width, (int)o->src->d.height, 1};
    if (o->dst) o->dr = (NVNcopyRegion){0, 0, 0, (int)o->dst->d.width, (int)o->dst->d.height, 1};
    o->flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
}
NVN_API(nvnCommandBufferSetCopyRowStride) { REC(cmd_of(c), OP_COPY_STRIDE, p_copy_stride)->stride = (int64_t)arg_x(c, 1); }
NVN_API(nvnCommandBufferSetCopyImageStride) {
    static volatile int32_t logged;
    int64_t stride = (int64_t)arg_x(c, 1);
    if (stride && !plat_xchg32(&logged, 1))
        rt_log("nvn: copy image stride %lld not replayed (slices assumed tightly packed)", (long long)stride);
}
NVN_API(nvnCommandBufferSetBlendColor) { memcpy(REC(cmd_of(c), OP_BLEND_COLOR, p_blend_color)->rgba, arg_p(c, 1), 16); }

NVN_API(nvnCommandBufferWaitSync) {
    static volatile int32_t logged;
    if (!plat_xchg32(&logged, 1)) rt_log("nvn: first CommandBufferWaitSync: recorded as nothing (one replay queue)");
    (void)c;
}
NVN_API(nvnCommandBufferWaitEvent) {
    const ev_t *e = arg_p(c, 1);
    *REC(cmd_of(c), OP_WAIT_EVENT, p_wait_event) = (p_wait_event){e->val, (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 3)};
}
NVN_API(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN_API(nvnCommandBufferSetPatchSize) { (void)c; }

static uint64_t def_reads, def_changed, n_clones, n_sync_clones;

static void note_first_changes(const p_bind_handles *b, const uint64_t *now, uint64_t nth) {
    if (nth < 6)
        NVN_LOG("deferred bind read new handles at submit: stage %d slot %d, %#llx at record -> %#llx", b->stage,
                b->first, (unsigned long long)b->h[0], (unsigned long long)now[0]);
}

static int resolve_deferred(const uint8_t *p) {
    const uint8_t *bad = NULL;
    int nd = nvn_resolve_binds(p, 0, &def_reads, &def_changed, note_first_changes, &bad);
    if (nd >= 0) return nd;
    op_hdr h;
    memcpy(&h, bad, sizeof h);
    if (h.op == OP_CALL && h.size >= 8 && !(h.size & 7)) rt_fatal("nvn: deferred bind resolution: call depth over 16");
    rt_fatal("nvn: corrupt recording at %p while resolving deferred binds (op %u size %u)", (const void *)bad, h.op,
             h.size);
}

void nvn_submit_prepare(const uint64_t *handles, uint64_t *out, int n) {
    uint64_t now = __atomic_add_fetch(&nsubmits, 1, __ATOMIC_RELAXED), min = copy_minute(), k = 0, cloned = 0;
    plat_mutex_lock_shared(&copy_lock);
    for (int i = 0; i < n; i++) {
        copy_prefix *pf = copy_find(handles[i]);
        if (!pf) {
            plat_mutex_unlock_shared(&copy_lock);
            rt_fatal("nvn: QueueSubmitCommands: command handle %#llx is not a live recording (released or never recorded)",
                     (unsigned long long)handles[i]);
        }
        __atomic_store_n(&pf->used_at, now, __ATOMIC_RELAXED);
        copy_age(pf, min);
        out[i] = handles[i];
        if (nvn_copy_enter(&pf->busy) || __atomic_load_n(&pf->deferred, __ATOMIC_RELAXED) == 1 ||
            nvn_copy_again(handles, out, i))
            continue;
        size_t sz = sizeof *pf + pf->bytes;
        copy_prefix *cl = malloc(sz);
        if (!cl) {
            plat_mutex_unlock_shared(&copy_lock);
            rt_fatal("nvn: out of memory (recording clone, %zu bytes)", sz);
        }
        memcpy(cl, pf, sz);
        cl->magic = 0, cl->busy = 0;
        if (cl->pins) pin_refs((const uint8_t *)(cl + 1), 1);
        nvn_copy_leave(&pf->busy);
        out[i] = (uintptr_t)(cl + 1);
        cloned = handles[i], k = __atomic_add_fetch(&n_sync_clones, 1, __ATOMIC_RELAXED);
    }
    plat_mutex_unlock_shared(&copy_lock);
    for (int i = 0; i < n; i++) {
        int nd = resolve_deferred((const uint8_t *)(uintptr_t)out[i]);
        if (out[i] == handles[i])
            __atomic_store_n(&((copy_prefix *)(uintptr_t)out[i] - 1)->deferred, (uint64_t)(nd ? 2 : 1), __ATOMIC_RELAXED);
    }
    if (k && (k <= 4 || !(k & (k - 1))))
        rt_log("nvn: thread %u submits the recording %#llx while another thread's submission of it is replayed: its "
               "deferred binds go into a copy of its own (%llu so far)", plat_thread_id(), (unsigned long long)cloned,
               (unsigned long long)k);
}

void nvn_submit_done(const uint64_t *handles, const uint64_t *out, int n) {
    for (int i = 0; i < n; i++) {
        if (out[i] != handles[i]) copy_free((copy_prefix *)(uintptr_t)out[i] - 1);
        else nvn_copy_leave(&((copy_prefix *)(uintptr_t)handles[i] - 1)->busy);
    }
}

void nvn_submit_prepare_async(const uint64_t *handles, uint64_t *out, int n) {
    uint64_t now = __atomic_add_fetch(&nsubmits, 1, __ATOMIC_RELAXED);
    uint64_t replayed = gfx_replayed_submits();
    copy_prefix *doomed = NULL;
    plat_mutex_lock(&copy_lock);
    if (nzombies) doomed = zombie_sweep(replayed, doomed);
    for (int i = 0; i < n; i++) {
        copy_prefix *pf = copy_find(handles[i]);
        if (!pf) {
            plat_mutex_unlock(&copy_lock);
            rt_fatal("nvn: QueueSubmitCommands: command handle %#llx is not a live recording (released or never recorded)",
                     (unsigned long long)handles[i]);
        }
        out[i] = handles[i];
        if (pf->used_at > replayed && pf->used_at != now && pf->deferred != 1) {
            size_t sz = sizeof *pf + pf->bytes;
            copy_prefix *cl = malloc(sz);
            if (!cl) rt_fatal("nvn: out of memory (recording clone, %zu bytes)", sz);
            memcpy(cl, pf, sz);
            if (cl->pins) pin_refs((const uint8_t *)(cl + 1), 1);
            cl->magic = 0;
            cl->used_at = now;
            zombie_add(cl);
            out[i] = (uintptr_t)(cl + 1);
            __atomic_fetch_add(&n_clones, 1, __ATOMIC_RELAXED);
        } else {
            pf->used_at = now;
        }
        copy_age(pf, copy_minute());
    }
    plat_mutex_unlock(&copy_lock);
    doom_free(doomed);
    for (int i = 0; i < n; i++) {
        int nd = resolve_deferred((const uint8_t *)(uintptr_t)out[i]);
        if (out[i] == handles[i]) ((copy_prefix *)(uintptr_t)out[i] - 1)->deferred = nd ? 2 : 1;
    }
    if (now % 7200 == 0)
        rt_log("nvn: asynchronous replay at submit %llu: %llu recording clones so far, %u copies waiting for the replay",
               (unsigned long long)now, (unsigned long long)__atomic_load_n(&n_clones, __ATOMIC_RELAXED), nzombies);
}

uint64_t nvn_submit_count(void) { return __atomic_load_n(&nsubmits, __ATOMIC_RELAXED); }

const char *nvn_op_name(uint32_t op) {
    static const char *const names[] = {
        "END", "JUMP", "BEGIN", "CALL", "SIGNAL_EVENT", "REPORT_COUNTER", "COPY_B2B", "UPDATE_UB", "SET_RT",
        "CLEAR_COLOR", "CLEAR_DS", "VIEWPORTS", "SCISSOR", "DEPTH_RANGE", "BIND_PROGRAM", "BIND_STATE", "BIND_VATTR",
        "BIND_VSTREAM", "BIND_VBUF", "BIND_UBO", "BIND_SSBO", "BIND_TEX", "BIND_SAMP", "STENCIL", "POLY_OFFSET", "DRAW",
        "DRAW_IDX", "DISPATCH", "BARRIER", "SET_TEX_POOL", "SET_SAMP_POOL", "COPY_B2T", "COPY_STRIDE", "BLEND_COLOR",
        "COPY_T2B", "BIND_IMG", "COPY_T2T", "RESET_COUNTER", "CLEAR_TEX", "DEPTH_CLAMP", "RENDER_ENABLE",
        "RASTER_DISCARD", "RENDER_COND", "PRIM_RESTART", "DRAW_IDX_IND", "DISPATCH_IND", "CLEAR_BUF", "PUSH_CC", "DRAW_MESH", "DRAW_MESH_IND",
        "WAIT_EVENT",
    };
    _Static_assert(sizeof names / sizeof *names == OP_COUNT_, "one name per op");
    return op < OP_COUNT_ ? names[op] : "?";
}

void nvn_copies_stats(uint64_t *n, uint64_t *bytes, uint64_t *idle_n, uint64_t *idle_bytes) {
    uint64_t m = copy_minute();
    int64_t rn = 0, rb = 0;
    plat_mutex_lock_shared(&copy_lock);
    *n = ncopies, *bytes = copy_bytes;
    for (uint64_t k = 0; k < COPY_IDLE_MINS && k <= m; k++)
        rn += __atomic_load_n(&age_n[(m - k) % COPY_AGE_MINS], __ATOMIC_RELAXED),
        rb += __atomic_load_n(&age_b[(m - k) % COPY_AGE_MINS], __ATOMIC_RELAXED);
    plat_mutex_unlock_shared(&copy_lock);
    uint64_t un = rn > 0 ? (uint64_t)rn : 0, ub = rb > 0 ? (uint64_t)rb : 0;
    *idle_n = un < *n ? *n - un : 0;
    *idle_bytes = ub < *bytes ? *bytes - ub : 0;
}

void nvn_cmd_sizes(PerfSizes *s) {
    uint64_t in, ib, idle_n, idle_b;
    nvn_copies_stats(&in, &ib, &idle_n, &idle_b);
    plat_mutex_lock_shared(&copy_lock);
    uint64_t nc = ncopies, cb = copy_bytes, nz = nzombies, zb = 0;
    for (uint32_t i = 0; i < nzombies; i++) zb += zombies[i]->bytes;
    plat_mutex_unlock_shared(&copy_lock);
    plat_mutex_lock(&pin_lock);
    uint64_t pins = pins_live, pq = pin_qn - pin_qh;
    plat_mutex_unlock(&pin_lock);
    plat_mutex_lock(&pool_lock);
    uint64_t np = npool, pb = 0;
    for (uint32_t i = 0; i < npool; i++) pb += pool[i].size;
    plat_mutex_unlock(&pool_lock);
    perf_size(s, "recording copies", nc, 0, cb);
    perf_size(s, "of them unused for 5 minutes", idle_n, 0, idle_b);
    perf_size(s, "recording copy zombies", nz, 0, zb);
    if (retire_on()) {
        int64_t v[4] = {__atomic_load_n(&retire_changed_n, __ATOMIC_RELAXED),
                        __atomic_load_n(&retire_changed_b, __ATOMIC_RELAXED),
                        __atomic_load_n(&retire_gone_n, __ATOMIC_RELAXED),
                        __atomic_load_n(&retire_gone_b, __ATOMIC_RELAXED)};
        for (int i = 0; i < 4; i++) v[i] = v[i] > 0 ? v[i] : 0;
        perf_size(s, "of those idle, control memory changed", (uint64_t)v[0], 0, (uint64_t)v[1]);
        perf_size(s, "control memory gone", (uint64_t)v[2], 0, (uint64_t)v[3]);
    }
    perf_size(s, "texture pins", pins, 0, PERF_SIZE_NONE);
    perf_size(s, "pin sweep queue", pq, 0, PERF_SIZE_NONE);
    perf_size(s, "recording-buffer pool", np, POOL_SLOTS, pb);
}
