// Contains code Copyright (c) 2026 Peiikai, Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "prof.h"
#include "vk.h"
#include "vk_mesh_fb.h"
#include "vk_lowlat.h"
#include "vk_sync_hw.h"
#include "platform/platform.h"

#define WEAK __attribute__((weak))

typedef struct { uint64_t fence; void (*fn)(void *); void *arg; } Pending;
typedef struct { VkCommandBuffer cb; uint64_t fence; } CmdSlot;

static PlatMutex start_lock = PLAT_MUTEX_INIT;
static int started;
static PlatThread *completion_thread;

static PlatMutex sub_lock = PLAT_MUTEX_INIT;
static VkCommandPool cmd_pool;
static CmdSlot *slots;
static int nslots;
static int cur_slot;
static uint64_t cur_fence;
#define PART_CBS 254
static VkCommandBuffer part_cbs[PART_CBS];
static int npart_cbs;

static PlatMutex st_lock = PLAT_MUTEX_INIT;
static PlatCond cv_work = PLAT_COND_INIT;
static PlatCond cv_done = PLAT_COND_INIT;
static uint64_t issued, done;
static uint64_t reserved;
#define LIVE_RING 4096
static uint64_t live_of[LIVE_RING];
static uint64_t live_last_of[LIVE_RING], live_last;
static uint64_t res_of[LIVE_RING], res_last;
static uint8_t res_kinds[LIVE_RING];
static uint64_t early_of[LIVE_RING], empty_of[LIVE_RING];
static VkCommandBuffer tail_cb;
static int tail_early, sub_parts;
static volatile uint64_t tail_ns;
static volatile uint64_t tail_t0, tail_val;
static volatile uint64_t tail_n, tail_waits;
static int res_mask = 0x1f;
static int stop;
static Pending *queue;
static int qhead, qn, qcap;
static Pending *rqueue;
static int rqhead, rqn, rqcap;
static PlatMutex res_lock = PLAT_MUTEX_INIT;
static void results_run(uint64_t upto);
static Pending *fqueue;
static int fqhead, fqn, fqcap;

#define HOLD_CAP 16
#define HOLD_NS 500000ull
#define CHAIN_MAX 64
#define SUB_CBS (CHAIN_MAX + PART_CBS + 3)
static struct {
    int n;
    VkSubmitInfo2 si[HOLD_CAP];
    VkCommandBufferSubmitInfo cbi[HOLD_CAP][SUB_CBS];
    VkSemaphoreSubmitInfo sig[HOLD_CAP][2];
    VkLatencySubmissionPresentIdNV lat[HOLD_CAP];
    uint64_t last;
    uint64_t batches, subs;
} H;
static volatile int32_t held_n;
#define HOLD_PAIRS 512
static uint32_t hold_pend[HOLD_PAIRS], hold_pairs[HOLD_PAIRS];
static int nhold_pend, nhold_pairs;
static uint64_t ts_queue_ns[1024];
static volatile uint64_t held_t0;
static volatile uint64_t queued_v;
static int hold_max = -1;

static VkSemaphore exec_tl;
static uint64_t exec_sig;

static volatile uint64_t tl_hw, ex_hw;
static int wb_split = -1;
static int wb_results;

int vk_sync_wb_split(void) {
    if (wb_split < 0) {
        const char *e = cfg_get("NX2SARIA_WB_SPLIT");
        int forced = e && !strcmp(e, "2"), bare = e && !strcmp(e, "3"), shadow = vk_mem_shadow_storage();
        wb_results = !bare;
        wb_split = cfg_bool("NX2SARIA_WB_SPLIT") && (forced || !shadow);
        rt_log("vk: the wait after a frame's first submission %s (NX2SARIA_WB_SPLIT%s)",
               !wb_split ? "is the fence's: it covers the write-backs and the completion thread's side effects"
               : wb_results ? "ends when the GPU has run the submission's own commands and the results the game reads "
                              "are in its memory: the write-backs and the other side effects are left out"
                            : "ends when the GPU has run the submission's own commands: its write-backs and every side "
                              "effect of the completion thread are left out",
               shadow && !forced ? "; shadow storage keeps the fence's wait unless it is 2" : "");
    }
    return wb_split && exec_tl != VK_NULL_HANDLE;
}

static volatile uint64_t exw_n, exw_early, exw_lag_ns, exw_lag_max;
static volatile uint64_t exw_value, exw_t0;
static volatile int32_t cb_late;

int vk_sync_effect_late(void) { return __atomic_load_n(&cb_late, __ATOMIC_RELAXED); }

static volatile uint64_t cb_value;
static volatile uint32_t cb_tid;

uint64_t vk_sync_cb_value(void) {
    return __atomic_load_n(&cb_tid, __ATOMIC_ACQUIRE) == plat_thread_id() ? __atomic_load_n(&cb_value, __ATOMIC_RELAXED) : 0;
}

void vk_sync_exec_stats(uint64_t *waits, uint64_t *early, uint64_t *lag_ns, uint64_t *lag_max_ns) {
    *waits = __atomic_exchange_n(&exw_n, 0, __ATOMIC_RELAXED), *early = __atomic_exchange_n(&exw_early, 0, __ATOMIC_RELAXED);
    *lag_ns = __atomic_exchange_n(&exw_lag_ns, 0, __ATOMIC_RELAXED);
    *lag_max_ns = __atomic_exchange_n(&exw_lag_max, 0, __ATOMIC_RELAXED);
}

#define CBT_N 32
static struct { void (*fn)(void *); uint64_t ns, calls; } cbt[CBT_N];
static int cbt_n;

void vk_sync_cb_stats(char *out, size_t n) {
    int k = 0;
    *out = 0;
    plat_mutex_lock(&st_lock);
    for (int i = 0; i < cbt_n && k < (int)n - 64; i++) {
        if (!cbt[i].calls) continue;
        k += snprintf(out + k, n - (size_t)k, " %+lld %.3f ms %llu calls;",
                      (long long)((intptr_t)cbt[i].fn - (intptr_t)vk_completion_add), cbt[i].ns / 1e6,
                      (unsigned long long)cbt[i].calls);
        cbt[i].ns = cbt[i].calls = 0;
    }
    plat_mutex_unlock(&st_lock);
}

static int lost_at_due(void) {
    static int at = -1;
    static int32_t calls;
    if (at < 0) {
        const char *e = cfg_get("NX2SARIA_DEVICE_LOST_AT");
        at = e && *e ? atoi(e) : 0;
        if (at < 0) at = 0;
        if (at) rt_log("vk: queue hand-over %d will be taken as a lost device (NX2SARIA_DEVICE_LOST_AT)", at);
    }
    return at && ++calls == at;
}

static void hold_flush_locked(VkCtx *c) {
    if (!H.n) return;
    if (vk_device_is_lost()) {
        H.n = 0, H.last = 0;
        __atomic_store_n(&held_n, 0, __ATOMIC_RELEASE);
        return;
    }
    for (int i = 0; i < H.n; i++)
        for (uint32_t k = 0; k < H.si[i].signalSemaphoreInfoCount; k++) {
            const VkSemaphoreSubmitInfo *s = &H.si[i].pSignalSemaphoreInfos[k];
            if (s->semaphore == c->timeline) sync_hw_raise(&tl_hw, s->value);
            else if (s->semaphore == exec_tl) sync_hw_raise(&ex_hw, s->value);
        }
    VkResult r = lost_at_due() ? VK_ERROR_DEVICE_LOST : vkQueueSubmit2(c->gfx_queue, (uint32_t)H.n, H.si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        vk_mem_oom_report("replay submit", r);
        vk_device_lost_check(r, "the submit of the replay's command buffers");
        rt_fatal("vk: vkQueueSubmit2 of %d held submissions up to value %llu failed (%d)", H.n,
                 (unsigned long long)H.last, (int)r);
    }
    if (H.last) __atomic_store_n(&queued_v, H.last, __ATOMIC_RELEASE);
    uint64_t now = plat_now_ns();
    for (int k = 0; k < nhold_pairs; k++) ts_queue_ns[hold_pairs[k]] = now;
    nhold_pairs = 0;
    H.batches++, H.subs += (uint64_t)H.n;
    H.n = 0, H.last = 0;
    __atomic_store_n(&held_n, 0, __ATOMIC_RELEASE);
}

void vk_sync_flush_held(void) {
    if (!H.n) return;
    VkCtx *c = vk_ctx();
    if (c) hold_flush_locked(c);
}

static void queue_submit(VkCtx *c, const VkSubmitInfo2 *si, uint64_t value) {
    if (hold_max < 0) {
        const char *e = cfg_get("NX2SARIA_SUBMIT_BATCH");
        int v = e && *e ? atoi(e) : 4;
        hold_max = v < 1 ? 1 : v > HOLD_CAP ? HOLD_CAP : v;
        rt_log("vk: up to %d submissions handed to the queue together (NX2SARIA_SUBMIT_BATCH)", hold_max);
    }
    uint64_t p0 = perf_begin();
    vk_queue_lock_raw();
    perf_end(PERF_FENCE, p0);
    if (H.n == HOLD_CAP) hold_flush_locked(c);
    for (int k = 0; k < nhold_pend && nhold_pairs < HOLD_PAIRS; k++) hold_pairs[nhold_pairs++] = hold_pend[k];
    nhold_pend = 0;
    int i = H.n++;
    H.si[i] = *si;
    if (si->commandBufferInfoCount > SUB_CBS) rt_fatal("vk: a submission of %u command buffers", si->commandBufferInfoCount);
    if (si->commandBufferInfoCount)
        memcpy(H.cbi[i], si->pCommandBufferInfos, sizeof H.cbi[i][0] * si->commandBufferInfoCount);
    H.si[i].pCommandBufferInfos = H.cbi[i];
    if (si->signalSemaphoreInfoCount) {
        memcpy(H.sig[i], si->pSignalSemaphoreInfos, sizeof H.sig[i][0] * si->signalSemaphoreInfoCount);
        H.si[i].pSignalSemaphoreInfos = H.sig[i];
    }
    if (vk_lowlat_submit_tag(&H.lat[i])) H.si[i].pNext = &H.lat[i];
    if (value) H.last = value;
    uint64_t now = kern_now_ns();
    if (i == 0) __atomic_store_n(&held_t0, now, __ATOMIC_RELEASE);
    __atomic_store_n(&held_n, H.n, __ATOMIC_RELEASE);
    if (H.n >= hold_max || !value || now - __atomic_load_n(&held_t0, __ATOMIC_ACQUIRE) >= HOLD_NS) hold_flush_locked(c);
    vk_queue_unlock();
}

static void hold_release(uint64_t value, int aged, int try_only) {
    if (!__atomic_load_n(&held_n, __ATOMIC_ACQUIRE)) return;
    int due = value > __atomic_load_n(&queued_v, __ATOMIC_ACQUIRE) ||
              (aged && kern_now_ns() - __atomic_load_n(&held_t0, __ATOMIC_ACQUIRE) >= HOLD_NS);
    if (!due) return;
    if (try_only) {
        if (!vk_queue_trylock_raw()) return;
    } else {
        vk_queue_lock_raw();
    }
    vk_sync_flush_held();
    vk_queue_unlock();
}

void vk_sync_release_held(uint64_t value) { hold_release(value, 0, 0); }

#define TS_PAIRS 1024
_Static_assert(TS_PAIRS == sizeof ts_queue_ns / sizeof ts_queue_ns[0], "ts_queue_ns has a slot per pair");
static VkQueryPool ts_pool;
static int ts_state;
static double ts_period;
static uint64_t ts_mask;
static uint32_t ts_next;
static volatile uint8_t ts_busy[TS_PAIRS];
static int ts_cur = -1;
static uint64_t ts_last_end;
static volatile int64_t ts_busy_ns, ts_parts, ts_missed;
static volatile int64_t ts_busy_all;
static uint64_t ts_sub_ns[TS_PAIRS];
typedef struct { uint32_t pair; int mirror; uint64_t fence; VkCommandBuffer cb; } TsRead;
#define TS_UNSENT (2 * SUB_CBS)
static TsRead *ts_unsent[TS_UNSENT];
static int ts_n_unsent;
static volatile int64_t ts_mirror_ns;
static uint64_t ts_last_end_ns;
static volatile int64_t ts_idle_ns, ts_starved_ns, ts_gaps, ts_max_gap_ns, ts_queued_ns;
static volatile int64_t cal_tick, cal_ns, cal_at;
static PFN_vkGetCalibratedTimestampsKHR cal_fn;

static void ts_setup(VkCtx *c) {
    ts_state = -1;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    if (!qp) return;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    uint32_t bits = c->gfx_family < nq ? qp[c->gfx_family].timestampValidBits : 0;
    free(qp);
    float period = c->props.limits.timestampPeriod;
    if (!bits || period <= 0) {
        rt_log("vk: no GPU timestamps on this queue (valid bits %u, period %.3f ns): the perf line shows no GPU time", bits, period);
        return;
    }
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 2 * TS_PAIRS};
    VkResult r = vkCreateQueryPool(c->device, &qi, NULL, &ts_pool);
    if (r != VK_SUCCESS) {
        rt_log("vk: timestamp query pool failed (%d): the perf line shows no GPU time", (int)r);
        return;
    }
    ts_period = period;
    ts_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    ts_state = 1;
}

static int cal_refresh(VkCtx *c) {
    if (!c->has_calibrated) return 0;
    if (!cal_fn) {
        cal_fn = (PFN_vkGetCalibratedTimestampsKHR)vk_dpa(c->device, c->has_calibrated == 2 ? "vkGetCalibratedTimestampsKHR"
                                                                                                        : "vkGetCalibratedTimestampsEXT");
        if (!cal_fn) { c->has_calibrated = 0; return 0; }
    }
    int64_t now = (int64_t)plat_now_ns();
    if (cal_at && now - cal_at < 250000000) return 1;
    VkCalibratedTimestampInfoKHR ci[2] = {{.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR},
#ifdef _WIN32
                                          {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR}};
#else
                                          {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR}};
#endif
    uint64_t v[2], dev;
    if (cal_fn(c->device, 2, ci, v, &dev) != VK_SUCCESS) {
        if (!cal_at) {
            rt_log("vk: calibrated timestamps failed: the perf line shows no GPU idle split");
            c->has_calibrated = 0;
        }
        return cal_at != 0;
    }
    cal_tick = (int64_t)(v[0] & ts_mask), cal_ns = (int64_t)((double)v[1] * 1e9 / (double)plat_counter_hz()), cal_at = now;
    return 1;
}

static int64_t tick_ns(uint64_t t) { return cal_ns + (int64_t)((double)((int64_t)t - cal_tick) * ts_period); }

static void max64(volatile int64_t *m, int64_t v) {
    int64_t o = __atomic_load_n(m, __ATOMIC_RELAXED);
    while (v > o && !__atomic_compare_exchange_n(m, &o, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void ts_read_cb(void *arg);

#define GTL_N 16384
typedef struct { int kind; int64_t call, sub, start, end; } GtlEv;
static GtlEv gtl[GTL_N];
static int gtl_n;
static uint64_t gtl_dropped;
static PlatMutex gtl_lock = PLAT_MUTEX_INIT;
static const char *const gtl_names[GTL_KINDS] = {"replay parts", "mirror prologues", "pre-pass buffers", "upload batches",
                                                 "presents"};

#define PART_RING 2048
static struct { int64_t sub, sn, en; int kind; } part_ring[PART_RING];
static volatile uint64_t part_n;

static void vk_gtl_note_ns(int kind, int64_t call, int64_t sub, int64_t start, int64_t end) {
    if (!(rt_trace & RT_TRACE_PERF) || kind < 0 || kind >= GTL_KINDS || !sub || end < start) return;
    plat_mutex_lock(&gtl_lock);
    if (gtl_n < GTL_N) gtl[gtl_n++] = (GtlEv){kind, call ? call : sub, sub, start, end};
    else gtl_dropped++;
    uint64_t pn = part_n;
    part_ring[pn % PART_RING].sub = sub, part_ring[pn % PART_RING].sn = start;
    part_ring[pn % PART_RING].en = end, part_ring[pn % PART_RING].kind = kind;
    __atomic_store_n(&part_n, pn + 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&gtl_lock);
}

void vk_gtl_note(int kind, uint64_t sub_ns, uint64_t start_tick, uint64_t end_tick) {
    if (!(rt_trace & RT_TRACE_PERF) || ts_state != 1 || !__atomic_load_n(&cal_at, __ATOMIC_ACQUIRE)) return;
    uint64_t s = start_tick & ts_mask, e = end_tick & ts_mask;
    if (e < s) return;
    vk_gtl_note_ns(kind, (int64_t)sub_ns, (int64_t)sub_ns, tick_ns(s), tick_ns(e));
}

#define GW_RING 4096
typedef struct { uint64_t v, res, iss, done; } GwSub;
static GwSub gw[GW_RING];
static PlatMutex gw_lock = PLAT_MUTEX_INIT;
static uint64_t gw_n, gw_ns, gw_rep_ns, gw_gpu_ns, gw_lead_ns, gw_age, gw_before, gw_lat_rep, gw_lat_gpu, gw_lat_n;
static uint64_t gw_max_ns, gw_unknown, gw_rec_n, gw_rec_done, gw_rec_self, gw_need_back, gw_need_n;
static uint64_t gw_need_kind[5], gw_skip_kind[5];

static GwSub *gw_at(uint64_t v) {
    GwSub *e = &gw[v & (GW_RING - 1)];
    if (e->v != v) *e = (GwSub){v, 0, 0, 0};
    return e;
}

static void gw_mark(uint64_t v, int what) {
    if (!(rt_trace & RT_TRACE_PERF) || !v) return;
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&gw_lock);
    GwSub *e = gw_at(v);
    if (what == 0) e->res = now;
    else if (what == 1) e->iss = now;
    else if (!e->done) e->done = now;
    plat_mutex_unlock(&gw_lock);
}

static void gw_wait(uint64_t value, uint64_t t0, uint64_t t1, uint64_t newest, uint64_t sent) {
    if (!(rt_trace & RT_TRACE_PERF) || t1 <= t0 || plat_thread_id() == prof_replay_tid) return;
    uint64_t d = t1 - t0;
    plat_mutex_lock(&gw_lock);
    GwSub e = gw[value & (GW_RING - 1)];
    gw_n++, gw_ns += d;
    if (d > gw_max_ns) gw_max_ns = d;
    if (e.v != value || !e.iss) {
        gw_unknown++, gw_gpu_ns += d;
    } else {
        uint64_t rep = e.iss > t0 ? (e.iss < t1 ? e.iss : t1) - t0 : 0;
        gw_rep_ns += rep, gw_gpu_ns += d - rep;
        if (e.res && e.res <= t0) gw_lead_ns += t0 - e.res;
        if (e.res && e.done && e.iss >= e.res && e.done >= e.iss)
            gw_lat_rep += e.iss - e.res, gw_lat_gpu += e.done - e.iss, gw_lat_n++;
    }
    if (newest > value) gw_age += newest - value;
    if (value > sent + 1) gw_before += value - sent - 1;
    plat_mutex_unlock(&gw_lock);
}

static void gw_log(uint64_t frame, int frames) {
    plat_mutex_lock(&gw_lock);
    uint64_t n = gw_n, ns = gw_ns, rep = gw_rep_ns, gpu = gw_gpu_ns, lead = gw_lead_ns, age = gw_age, before = gw_before,
             lr = gw_lat_rep, lg = gw_lat_gpu, ln = gw_lat_n, mx = gw_max_ns, unk = gw_unknown, rn = gw_rec_n,
             rd = gw_rec_done, rs = gw_rec_self;
    gw_n = gw_ns = gw_rep_ns = gw_gpu_ns = gw_lead_ns = gw_age = gw_before = gw_lat_rep = gw_lat_gpu = gw_lat_n = 0;
    uint64_t nb = gw_need_back, nn = gw_need_n, nk[5], sk[5];
    memcpy(nk, gw_need_kind, sizeof nk), memcpy(sk, gw_skip_kind, sizeof sk);
    memset(gw_need_kind, 0, sizeof gw_need_kind), memset(gw_skip_kind, 0, sizeof gw_skip_kind);
    gw_max_ns = gw_unknown = gw_rec_n = gw_rec_done = gw_rec_self = gw_need_back = gw_need_n = 0;
    plat_mutex_unlock(&gw_lock);
    if (!n) {
        rt_log("perf: frame %llu: waits for GPU work outside the replay: none", (unsigned long long)frame);
        return;
    }
    double f = frames;
    rt_log("perf: frame %llu: waits for GPU work outside the replay: %.1f per frame, %.2f ms per frame (longest %.2f ms): "
           "%.2f ms until the replay had submitted the waited submission (%.2f submissions still before it in the replay "
           "on average), %.2f ms after (GPU, submit batching hold, completion; %llu without a record); the waited one was "
           "%.2f submissions behind the newest and handed to the replay %.2f ms before the wait; per waited submission "
           "%.2f ms from hand-over to submit and %.2f ms from submit to done",
           (unsigned long long)frame, n / f, ns / 1e6 / f, mx / 1e6, rep / 1e6 / f, (double)before / (double)n,
           gpu / 1e6 / f, (unsigned long long)unk, (double)age / (double)n, lead / 1e6 / (double)n,
           ln ? lr / 1e6 / (double)ln : 0.0, ln ? lg / 1e6 / (double)ln : 0.0);
    if (rn) rt_log("perf: frame %llu: submit waits: %llu, %llu of them without the GPU's run of the submission itself (%llu of "
                   "those waited for it anyway: it hands the CPU results), the rest went on to the GPU (the submission's GPU "
                   "read guest memory itself)", (unsigned long long)frame, (unsigned long long)rn, (unsigned long long)rd,
                   (unsigned long long)rs);
    if (nn) rt_log("perf: frame %llu: the submission the submit wait waited for was %.2f submissions before the waited one; "
                   "its kinds: events %llu, counter reports %llu, occlusion reports %llu, unmirrored writes %llu, write-backs "
                   "%llu; the submissions after it (left out by the mask): %llu, %llu, %llu, %llu, %llu",
                   (unsigned long long)frame, (double)nb / (double)nn, (unsigned long long)nk[0], (unsigned long long)nk[1],
                   (unsigned long long)nk[2], (unsigned long long)nk[3], (unsigned long long)nk[4], (unsigned long long)sk[0],
                   (unsigned long long)sk[1], (unsigned long long)sk[2], (unsigned long long)sk[3], (unsigned long long)sk[4]);
}

static int gtl_cmp(const void *a, const void *b) {
    int64_t x = ((const GtlEv *)a)->start, y = ((const GtlEv *)b)->start;
    return x < y ? -1 : x > y;
}

void vk_gtl_log(uint64_t frame, int frames) {
    static GtlEv ev[GTL_N];
    gw_log(frame, frames);
    plat_mutex_lock(&gtl_lock);
    int n = gtl_n;
    memcpy(ev, gtl, sizeof ev[0] * (size_t)n);
    gtl_n = 0;
    uint64_t dropped = gtl_dropped;
    gtl_dropped = 0;
    plat_mutex_unlock(&gtl_lock);
    if (n < 2 || frames < 1) return;
    qsort(ev, (size_t)n, sizeof ev[0], gtl_cmp);
    double busy = 0, starved = 0, queued = 0, q_by[GTL_KINDS] = {0}, lat[GTL_KINDS] = {0}, gpu[GTL_KINDS] = {0};
    double held = 0, held_idle = 0;
    int big[GTL_KINDS] = {0}, nlat[GTL_KINDS] = {0}, cnt[GTL_KINDS] = {0};
    int64_t end = ev[0].start;
    for (int i = 0; i < n; i++) {
        const GtlEv *e = &ev[i];
        cnt[e->kind]++, gpu[e->kind] += (double)(e->end - e->start);
        held += (double)(e->sub - e->call);
        if (e->start > end) {
            int64_t gap = e->start - end, s = e->sub > end ? e->sub - end : 0;
            int64_t h0 = e->call > end ? e->call : end, h1 = e->sub < e->start ? e->sub : e->start;
            if (h1 > h0) held_idle += (double)(h1 - h0);
            if (s > gap) s = gap;
            starved += (double)s, queued += (double)(gap - s), q_by[e->kind] += (double)(gap - s);
            if (gap - s > 500000) big[e->kind]++;
            if (e->sub >= end) lat[e->kind] += (double)(e->start - e->sub), nlat[e->kind]++;
        }
        int64_t from = e->start > end ? e->start : end;
        if (e->end > from) busy += (double)(e->end - from), end = e->end;
    }
    double f = 1e6 * frames;
    char by[512], la[512], gp[512];
    int a = 0, b = 0, g = 0;
    for (int k = 0; k < GTL_KINDS; k++) {
        if (!cnt[k]) continue;
        a += snprintf(by + a, sizeof by - (size_t)a, "%s%s %.2f ms (%.1f gaps over 0.5 ms)", a ? ", " : "", gtl_names[k],
                      q_by[k] / f, (double)big[k] / frames);
        b += snprintf(la + b, sizeof la - (size_t)b, "%s%s %.2f ms (%d)", b ? ", " : "", gtl_names[k],
                      nlat[k] ? lat[k] / nlat[k] / 1e6 : 0.0, nlat[k]);
        g += snprintf(gp + g, sizeof gp - (size_t)g, "%s%s %.1f x %.2f ms", g ? ", " : "", gtl_names[k],
                      (double)cnt[k] / frames, gpu[k] / f);
    }
    rt_log("perf: frame %llu: GPU timeline per frame: busy %.2f ms; idle %.2f ms starved (nothing in the queue; %.2f ms of "
           "it while the next piece was held in the submit batching) and %.2f ms with work in the queue, before %s; held "
           "in the submit batching %.2f ms in all", (unsigned long long)frame, busy / f, starved / f, held_idle / f,
           queued / f, by, held / f);
    rt_log("perf: frame %llu: GPU timeline: pieces per frame and their GPU time: %s; from hand-over to start on an idle "
           "GPU, on average: %s; %d pieces, %llu dropped", (unsigned long long)frame, gp, la, n, (unsigned long long)dropped);
}

static void ts_submitted(const VkCommandBufferSubmitInfo *cbi, uint32_t ncb) {
    uint64_t now = plat_now_ns();
    gfx_tl_note('S', now, cur_fence, 0);
    for (uint32_t k = 0; k <= ncb; k++)
        for (int i = 0; i < ts_n_unsent; i++) {
            TsRead *t = ts_unsent[i];
            if (!t || (k < ncb ? t->cb != cbi[k].commandBuffer : 0)) continue;
            ts_sub_ns[t->pair] = now;
            ts_queue_ns[t->pair] = 0;
            if (nhold_pend < HOLD_PAIRS) hold_pend[nhold_pend++] = t->pair;
            vk_completion_add(t->fence, ts_read_cb, t);
            ts_unsent[i] = NULL;
        }
    ts_n_unsent = 0;
}

static void ts_read_cb(void *arg) {
    TsRead *t = arg;
    VkCtx *c = vk_ctx();
    uint64_t v[2];
    if (vkGetQueryPoolResults(c->device, ts_pool, 2 * t->pair, 2, sizeof v, v, sizeof v[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        uint64_t s = v[0] & ts_mask, e = v[1] & ts_mask;
        if (e >= s) {
            if (cal_refresh(c)) {
                int64_t sn = tick_ns(s), en = tick_ns(e), sub = (int64_t)ts_sub_ns[t->pair];
                if (ts_last_end_ns && sn > ts_last_end_ns) {
                    int64_t gap = sn - ts_last_end_ns, late = sub - ts_last_end_ns;
                    plat_add64(&ts_idle_ns, gap);
                    if (late > 0) plat_add64(&ts_starved_ns, late < gap ? late : gap);
                    if (gap > 500000) plat_inc64(&ts_gaps);
                    max64(&ts_max_gap_ns, gap);
                }
                if (sub && sn > sub) plat_add64(&ts_queued_ns, sn - sub);
                gfx_tl_note('G', (uint64_t)sn, (uint64_t)en, (uint64_t)sub);
                if (en > ts_last_end_ns) ts_last_end_ns = en;
                int64_t q = (int64_t)ts_queue_ns[t->pair];
                vk_gtl_note_ns(t->mirror, sub, q ? q : sub, sn, en);
            }
            uint64_t from = s > ts_last_end ? s : ts_last_end;
            if (e > from) {
                int64_t ns = (int64_t)((double)(e - from) * ts_period);
                plat_add64(&ts_busy_ns, ns);
                if (t->mirror == GTL_MIRROR) plat_add64(&ts_mirror_ns, ns);
                plat_add64(&ts_busy_all, ns);
                perf_gpu_busy((uint64_t)ns);
            }
            if (e > ts_last_end) ts_last_end = e;
            plat_inc64(&ts_parts);
        } else plat_inc64(&ts_missed);
    } else plat_inc64(&ts_missed);
    __atomic_store_n(&ts_busy[t->pair], 0, __ATOMIC_RELEASE);
    free(t);
}

typedef struct { int64_t sub, sn, en; int kind; } GpuPart;
static int part_by_start(const void *a, const void *b) {
    int64_t x = ((const GpuPart *)a)->sn, y = ((const GpuPart *)b)->sn;
    return x < y ? -1 : x > y;
}

void vk_gpu_parts_log(uint64_t frame, uint64_t t0, uint64_t t1, double fence_ms) {
    static uint32_t logged;
    if (logged >= 400) return;
    uint64_t n = __atomic_load_n(&part_n, __ATOMIC_ACQUIRE), from = n > PART_RING / 2 ? n - PART_RING / 2 : 0;
    GpuPart *sel = malloc(sizeof *sel * (PART_RING / 2));
    if (!sel) return;
    int nsel = 0;
    for (uint64_t i = from; i < n; i++) {
        const typeof(part_ring[0]) *p = &part_ring[i % PART_RING];
        if (p->en < (int64_t)t0 || p->en > (int64_t)t1 || p->en < p->sn) continue;
        sel[nsel++] = (GpuPart){p->sub, p->sn, p->en, p->kind};
    }
    qsort(sel, (size_t)nsel, sizeof *sel, part_by_start);
    struct { int64_t ns, q; int kind; } top[6] = {{0}};
    static const char *const kn[GTL_KINDS] = {"part", "mirror copies", "pre-pass", "uploads", "present"};
    uint64_t parts = 0, busy = 0, gap = 0;
    int64_t prev_end = 0;
    for (int i = 0; i < nsel; i++) {
        const GpuPart *p = &sel[i];
        int64_t ns = p->en - p->sn, q = p->sub && p->sn > p->sub ? p->sn - p->sub : 0;
        parts++, busy += (uint64_t)ns;
        if (prev_end && p->sn > prev_end && (uint64_t)(p->sn - prev_end) > gap) gap = (uint64_t)(p->sn - prev_end);
        if (p->en > prev_end) prev_end = p->en;
        for (int k = 0; k < 6; k++)
            if (ns > top[k].ns) {
                memmove(&top[k + 1], &top[k], sizeof top[0] * (size_t)(5 - k));
                top[k].ns = ns, top[k].q = q, top[k].kind = p->kind;
                break;
            }
    }
    free(sel);
    if (!parts) return;
    logged++;
    char line[700];
    int o = snprintf(line, sizeof line, "perf: frame %llu: the GPU's timed parts that ended while the render thread waited "
                     "%.1f ms for fences: %llu parts, %.1f ms of GPU time, the longest gap between two %.1f ms; the longest:",
                     (unsigned long long)frame, fence_ms, (unsigned long long)parts, busy / 1e6, gap / 1e6);
    for (int k = 0; k < 6 && top[k].ns && o < (int)sizeof line - 80; k++)
        o += snprintf(line + o, sizeof line - (size_t)o, "%s %.1f ms (%s, %.1f ms in the queue)", k ? "," : "",
                      top[k].ns / 1e6, top[k].kind >= 0 && top[k].kind < GTL_KINDS ? kn[top[k].kind] : "?", top[k].q / 1e6);
    rt_log("%s", line);
}

#define TL_CAP 16384
#define TL_FRAMES 4
typedef struct { uint64_t t, a, b; uint32_t tid; int kind; } TlEv;
static TlEv *tl_ev;
static volatile int32_t tl_n, tl_rec;
static int tl_on = -1;
static uint64_t tl_from, tl_frames;

void gfx_tl_note(int kind, uint64_t t, uint64_t a, uint64_t b) {
    if (!__atomic_load_n(&tl_rec, __ATOMIC_ACQUIRE)) return;
    int32_t i = plat_inc32(&tl_n) - 1;
    if (i < TL_CAP) tl_ev[i] = (TlEv){t ? t : plat_now_ns(), a, b, plat_thread_id(), kind};
}

static int tl_cmp(const void *x, const void *y) {
    uint64_t a = ((const TlEv *)x)->t, b = ((const TlEv *)y)->t;
    return a < b ? -1 : a > b;
}

static void tl_dump(void) {
    int n = tl_n < TL_CAP ? tl_n : TL_CAP;
    qsort(tl_ev, (size_t)n, sizeof *tl_ev, tl_cmp);
    uint64_t t0 = 0, t1 = UINT64_MAX;
    for (int i = 0; i < n; i++)
        if (tl_ev[i].kind == 'P') {
            if (tl_ev[i].a == tl_from) t0 = tl_ev[i].t;
            if (tl_ev[i].a == tl_from + TL_FRAMES) t1 = tl_ev[i].t;
        }
    rt_log("vk: timeline of frames %llu-%llu (%d events), ms from the first present:", (unsigned long long)tl_from,
           (unsigned long long)(tl_from + TL_FRAMES), n);
    for (int i = 0; i < n; i++) {
        const TlEv *e = &tl_ev[i];
        if (e->t < t0 || e->t > t1) continue;
        double at = (double)(int64_t)(e->t - t0) / 1e6;
        switch (e->kind) {
        case 'P': rt_log("vk: tl %8.3f t%-5u present, frame %llu", at, e->tid, (unsigned long long)e->a); break;
        case 'R': rt_log("vk: tl %8.3f t%-5u replay begins: %llu handles, value %llu", at, e->tid,
                         (unsigned long long)e->a, (unsigned long long)e->b); break;
        case 'r': rt_log("vk: tl %8.3f t%-5u replay ends: %llu commands in the last part", at, e->tid,
                         (unsigned long long)e->a); break;
        case 'W': rt_log("vk: tl %8.3f t%-5u waits for the GPU to run value %llu", at, e->tid, (unsigned long long)e->a); break;
        case 'w': rt_log("vk: tl %8.3f t%-5u wait ends", at, e->tid); break;
        case 'g': rt_log("vk: tl %8.3f t%-5u group of %llu draws in %llu chunks", at, e->tid, (unsigned long long)e->a,
                         (unsigned long long)e->b); break;
        case 'h': rt_log("vk: tl %8.3f t%-5u group recorded and in the part", at, e->tid); break;
        case 'q': rt_log("vk: tl %8.3f t%-5u waits for its group: %s", at, e->tid, (const char *)(uintptr_t)e->a); break;
        case 'u': rt_log("vk: tl %8.3f t%-5u group done, goes on", at, e->tid); break;
        case 'c': rt_log("vk: tl %8.3f t%-5u records chunk %llu (%llu ops)", at, e->tid, (unsigned long long)e->a,
                         (unsigned long long)e->b); break;
        case 'd': rt_log("vk: tl %8.3f t%-5u chunk %llu done", at, e->tid, (unsigned long long)e->a); break;
        case 'S': rt_log("vk: tl %8.3f t%-5u part submitted, value %llu", at, e->tid, (unsigned long long)e->a); break;
        case 'G': rt_log("vk: tl %8.3f gpu    part runs %.3f ms (submitted %.3f ms before)", at,
                         (double)(e->a - e->t) / 1e6, (double)(int64_t)(e->t - e->b) / 1e6); break;
        default: break;
        }
    }
}

void gfx_tl_present(void) {
    if (tl_on < 0) {
        const char *e = cfg_get("NX2SARIA_GPU_TIMELINE");
        tl_from = e && *e ? strtoull(e, NULL, 0) : 0;
        tl_on = tl_from && (tl_ev = calloc(TL_CAP, sizeof *tl_ev)) != NULL;
    }
    if (!tl_on) return;
    uint64_t f = ++tl_frames;
    if (f == tl_from) __atomic_store_n(&tl_rec, 1, __ATOMIC_RELEASE);
    gfx_tl_note('P', 0, f, 0);
    if (f == tl_from + TL_FRAMES + 3) {
        __atomic_store_n(&tl_rec, 0, __ATOMIC_SEQ_CST);
        tl_dump();
        tl_on = 0;
    }
}

static int ts_open(VkCommandBuffer cb) {
    if (ts_state != 1 || !perf_gpu_timing()) {
        if (ts_state || !perf_gpu_timing()) return -1;
        ts_setup(vk_ctx());
        if (ts_state != 1) return -1;
    }
    uint32_t p = ts_next % TS_PAIRS;
    if (__atomic_load_n(&ts_busy[p], __ATOMIC_ACQUIRE)) { plat_inc64(&ts_missed); return -1; }
    ts_next++;
    ts_busy[p] = 1;
    vkCmdResetQueryPool(cb, ts_pool, 2 * p, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts_pool, 2 * p);
    return (int)p;
}

static void ts_close(VkCommandBuffer cb, int pair, uint64_t fence, int mirror) {
    if (pair < 0) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool, 2 * (uint32_t)pair + 1);
    ts_sub_ns[pair] = 0;
    TsRead *t = malloc(sizeof *t);
    if (!t) rt_fatal("vk: out of memory");
    *t = (TsRead){(uint32_t)pair, mirror, fence, cb};
    if (ts_n_unsent < TS_UNSENT) ts_unsent[ts_n_unsent++] = t;
    else vk_completion_add(fence, ts_read_cb, t);
}

static void ts_begin(VkCommandBuffer cb) { ts_cur = ts_open(cb); }

static void ts_end(VkCommandBuffer cb, uint64_t fence) {
    ts_close(cb, ts_cur, fence, 0);
    ts_cur = -1;
}

static void ts_drop(void) {
    if (ts_cur < 0) return;
    __atomic_store_n(&ts_busy[ts_cur], 0, __ATOMIC_RELEASE);
    ts_cur = -1;
}

uint64_t vk_gpu_busy_all(void) { return (uint64_t)__atomic_load_n(&ts_busy_all, __ATOMIC_RELAXED); }

static void gpu_stats(PerfGpu *g) {
    g->have_gpu = ts_state == 1;
    g->gpu_ns = (uint64_t)plat_xchg64(&ts_busy_ns, 0);
    g->gpu_parts = (uint64_t)plat_xchg64(&ts_parts, 0);
    g->gpu_missed = (uint64_t)plat_xchg64(&ts_missed, 0);
    g->have_vram = vk_mem_vram(&g->vram_used, &g->vram_budget, &g->vram_exact);
    g->have_idle = vk_ctx()->has_calibrated != 0 && cal_at != 0;
    g->idle_ns = (uint64_t)plat_xchg64(&ts_idle_ns, 0);
    g->starved_ns = (uint64_t)plat_xchg64(&ts_starved_ns, 0);
    g->queued_ns = (uint64_t)plat_xchg64(&ts_queued_ns, 0);
    g->idle_gaps = (uint64_t)plat_xchg64(&ts_gaps, 0);
    g->max_gap_ns = (uint64_t)plat_xchg64(&ts_max_gap_ns, 0);
    g->mirror_ns = (uint64_t)plat_xchg64(&ts_mirror_ns, 0);
}

WEAK void vk_exec_mesh_flush(VkCommandBuffer cb) { (void)cb; }
WEAK void vk_reg_replay_begin(void) {}
WEAK void vk_reg_replay_end(uint64_t fence) { (void)fence; }

WEAK int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    (void)cb, (void)fence;
    for (int i = 0; i < n; i++) {
        const op_hdr *h = handles[i] ? nvn_op_first(handles[i]) : NULL;
        if (!h || h->op != OP_BEGIN || h->size < sizeof *h || nvn_op_next(h)->op != OP_END)
            rt_fatal("vk: op replay is not linked");
    }
    return 0;
}

WEAK int vk_exec_held(void) { return 0; }

static void timeline_beyond(const char *name, uint64_t raw, volatile uint64_t *hw, const char *where) {
    uint64_t h = __atomic_load_n(hw, __ATOMIC_ACQUIRE);
    if (!sync_hw_beyond(raw, h)) return;
    rt_log("vk: the %s reads %llu, above every value its signals carry (%llu): the device is lost", name,
           (unsigned long long)raw, (unsigned long long)h);
    vk_device_lost_check(VK_ERROR_DEVICE_LOST, where);
}

void vk_sync_signal_note(uint64_t value) { sync_hw_raise(&tl_hw, value); }

void vk_sync_check_timeline(const char *where) {
    VkCtx *c = vk_ctx();
    if (!c || !c->timeline || vk_device_is_lost()) return;
    uint64_t raw = 0;
    VkResult r = vkGetSemaphoreCounterValue(c->device, c->timeline, &raw);
    vk_device_lost_check(r, where);
    if (r == VK_SUCCESS) timeline_beyond("submissions' timeline", raw, &tl_hw, where);
}

void vk_sync_state_text(char *o, size_t n) {
    VkCtx *c = vk_ctx();
    uint64_t tl = 0, ex = 0;
    char a[32] = "nothing (unreadable)", b[32] = "nothing (unreadable)";
    if (c && c->timeline && vkGetSemaphoreCounterValue(c->device, c->timeline, &tl) == VK_SUCCESS)
        snprintf(a, sizeof a, "%llu", (unsigned long long)tl);
    if (c && exec_tl && vkGetSemaphoreCounterValue(c->device, exec_tl, &ex) == VK_SUCCESS)
        snprintf(b, sizeof b, "%llu", (unsigned long long)ex);
    snprintf(o, n,
             "submissions issued up to %llu, their side effects run up to %llu, handed to the queue up to %llu with %d "
             "held back; the submissions' timeline reads %s of %llu signalled at most, the executed timeline %s of %llu; "
             "%d submission(s) held for an event",
             (unsigned long long)__atomic_load_n(&issued, __ATOMIC_ACQUIRE),
             (unsigned long long)__atomic_load_n(&done, __ATOMIC_ACQUIRE),
             (unsigned long long)__atomic_load_n(&queued_v, __ATOMIC_ACQUIRE), (int)__atomic_load_n(&held_n, __ATOMIC_ACQUIRE),
             a, (unsigned long long)__atomic_load_n(&tl_hw, __ATOMIC_ACQUIRE), b,
             (unsigned long long)__atomic_load_n(&ex_hw, __ATOMIC_ACQUIRE), vk_exec_held());
}

void vk_sync_lost_report(void) {
    char t[512];
    vk_sync_state_text(t, sizeof t);
    rt_log("vk: when the device was lost: %s", t);
}

void vk_sync_progress(uint64_t *issued_out, uint64_t *done_out) {
    *issued_out = __atomic_load_n(&issued, __ATOMIC_ACQUIRE);
    *done_out = __atomic_load_n(&done, __ATOMIC_ACQUIRE);
}

static void completion_main(void *arg) {
    VkCtx *c = arg;
    __atomic_store_n(&cb_tid, plat_thread_id(), __ATOMIC_RELEASE);
    plat_mutex_lock(&st_lock);
    for (;;) {
        while (!stop && done == issued && !(qn && queue[qhead].fence <= done) && !(rqn && rqueue[rqhead].fence <= done) &&
               !(fqn && fqueue[fqhead].fence <= done))
            plat_cond_wait(&cv_work, &st_lock);
        if (stop) break;
        uint64_t reached = done;
        if (issued > done) {
            uint64_t want = done + 1;
            plat_mutex_unlock(&st_lock);
            hold_release(0, 1, 1);
            uint64_t tmo = __atomic_load_n(&held_n, __ATOMIC_ACQUIRE) ? HOLD_NS : 100000000ull;
            VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                      .pSemaphores = &c->timeline, .pValues = &want};
            VkResult r = vkWaitSemaphores(c->device, &wi, tmo);
            if (r != VK_SUCCESS && r != VK_TIMEOUT) {
                vk_device_lost_check(r, "the completion thread's wait for a submission");
                rt_fatal("vk: vkWaitSemaphores failed (%d; device lost?)", (int)r);
            }
            r = vkGetSemaphoreCounterValue(c->device, c->timeline, &reached);
            if (r != VK_SUCCESS) {
                vk_device_lost_check(r, "the completion thread's read of the submissions' timeline");
                rt_fatal("vk: the submissions' timeline could not be read (%d; device lost?)", (int)r);
            }
            timeline_beyond("submissions' timeline", reached, &tl_hw, "the completion thread's read of the submissions' "
                                                                      "timeline (a value no submission signals)");
            if (reached > __atomic_load_n(&done, __ATOMIC_ACQUIRE)) vk_device_lost_poll();
            plat_mutex_lock(&st_lock);
            if (reached > issued) reached = issued;
        }
        if (vk_device_is_lost()) break;
        while (fqn && fqueue[fqhead].fence <= reached && !vk_device_is_lost()) {
            Pending p = fqueue[fqhead++];
            fqn--;
            plat_mutex_unlock(&st_lock);
            p.fn(p.arg);
            plat_mutex_lock(&st_lock);
        }
        plat_mutex_unlock(&st_lock);
        results_run(reached);
        plat_mutex_lock(&st_lock);
        if (vk_device_is_lost()) break;
        while (qn && queue[qhead].fence <= reached) {
            Pending p = queue[qhead++];
            qn--;
            plat_mutex_unlock(&st_lock);
            __atomic_store_n(&cb_late, p.fence <= __atomic_load_n(&exw_value, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
            uint64_t c0 = (rt_trace & RT_TRACE_PERF) ? kern_now_ns() : 0;
            __atomic_store_n(&cb_value, p.fence, __ATOMIC_RELAXED);
            p.fn(p.arg);
            __atomic_store_n(&cb_value, 0, __ATOMIC_RELAXED);
            uint64_t c1 = c0 ? kern_now_ns() : 0;
            __atomic_store_n(&cb_late, 0, __ATOMIC_RELAXED);
            plat_mutex_lock(&st_lock);
            if (c0) {
                int k = 0;
                while (k < cbt_n && cbt[k].fn != p.fn) k++;
                if (k == cbt_n && cbt_n < CBT_N) cbt[cbt_n++].fn = p.fn;
                if (k < cbt_n) cbt[k].ns += c1 - c0, cbt[k].calls++;
            }
            if (vk_device_is_lost()) break;
        }
        if (vk_device_is_lost()) break;
        if (reached > done) {
            uint64_t ev = __atomic_load_n(&exw_value, __ATOMIC_ACQUIRE);
            if (ev && reached >= ev) {
                uint64_t lag = kern_now_ns() - __atomic_load_n(&exw_t0, __ATOMIC_RELAXED);
                __atomic_fetch_add(&exw_lag_ns, lag, __ATOMIC_RELAXED);
                if (lag > __atomic_load_n(&exw_lag_max, __ATOMIC_RELAXED)) __atomic_store_n(&exw_lag_max, lag, __ATOMIC_RELAXED);
                __atomic_compare_exchange_n(&exw_value, &ev, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            }
            uint64_t tv = __atomic_load_n(&tail_val, __ATOMIC_ACQUIRE);
            if (tv && reached >= tv) {
                uint64_t d = kern_now_ns() - __atomic_load_n(&tail_t0, __ATOMIC_RELAXED), a = tail_ns;
                if (d < 50000000ull) __atomic_store_n(&tail_ns, a ? (a * 7 + d) / 8 : d, __ATOMIC_RELAXED);
                __atomic_compare_exchange_n(&tail_val, &tv, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            }
            uint64_t was = done;
            __atomic_store_n(&done, reached, __ATOMIC_RELEASE);
            plat_cond_broadcast(&cv_done);
            for (uint64_t v = reached - was > GW_RING ? reached - GW_RING + 1 : was + 1; v <= reached; v++) gw_mark(v, 2);
            plat_mutex_unlock(&st_lock);
            vk_mem_collect_try();
            plat_mutex_lock(&st_lock);
        }
    }
    plat_mutex_unlock(&st_lock);
    return;
}

void vk_sync_start(void) {
    if (__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&start_lock);
    if (!started) {
        if (!vk_init()) rt_fatal("vk: no usable Vulkan device (see the vk: lines above; NX2SARIA_GPU=null runs without a GPU)");
        VkCtx *c = vk_ctx();
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &cmd_pool) != VK_SUCCESS) rt_fatal("vk: submission command pool");
        if (!ts_state && perf_gpu_timing()) ts_setup(c);
        perf_gpu_stats = gpu_stats;
        uint64_t now = 0;
        vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
        sync_hw_raise(&tl_hw, now > issued ? now : issued);
        sync_hw_raise(&ex_hw, now > issued ? now : issued);
        if (issued > now) {
            VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline,
                                        .value = issued};
            if (vkSignalSemaphore(c->device, &si) != VK_SUCCESS) rt_fatal("vk: timeline restart at %llu", (unsigned long long)issued);
            now = issued;
        }
        plat_mutex_lock(&st_lock);
        issued = done = now;
        if (reserved < now) reserved = now;
        stop = 0;
        plat_mutex_unlock(&st_lock);
        VkSemaphoreTypeCreateInfo et = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = now};
        VkSemaphoreCreateInfo ei = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &et};
        if (vkCreateSemaphore(c->device, &ei, NULL, &exec_tl) != VK_SUCCESS) exec_tl = VK_NULL_HANDLE;
        exec_sig = now;
        vk_mem_note_submit(now);
        completion_thread = plat_thread_new(completion_main, c, "vk completion");
        if (!completion_thread) rt_fatal("vk: cannot start the completion thread");
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
        vk_astc_mode();
    }
    plat_mutex_unlock(&start_lock);
}

static int claim_slot(VkCtx *c, uint64_t fence) {
    uint64_t gpu = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &gpu);
    for (int i = 0; i < nslots; i++)
        if (slots[i].fence <= gpu) {
            slots[i].fence = fence;
            return i;
        }
    CmdSlot *n = realloc(slots, sizeof *n * (size_t)(nslots + 1));
    if (!n) rt_fatal("vk: out of memory (command buffers)");
    slots = n;
    CmdSlot *s = &slots[nslots];
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &ai, &s->cb) != VK_SUCCESS) rt_fatal("vk: command buffer allocation");
    s->fence = fence;
    return nslots++;
}

static CmdSlot *take_slot(VkCtx *c, uint64_t fence) {
    cur_slot = claim_slot(c, fence);
    return &slots[cur_slot];
}

static VkCommandPool pre_pool;
static CmdSlot *pre_slots;
static int npre, pre_pair = -1;
static VkCommandBuffer pre_cb;

VkCommandBuffer vk_sync_pre(void) {
    if (pre_cb) return pre_cb;
    VkCtx *c = vk_ctx();
    if (!pre_pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &pre_pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    uint64_t gpu = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &gpu);
    int i = 0;
    while (i < npre && pre_slots[i].fence > gpu) i++;
    if (i == npre) {
        CmdSlot *n = realloc(pre_slots, sizeof *n * (size_t)(npre + 1));
        if (!n) return VK_NULL_HANDLE;
        pre_slots = n;
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pre_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(c->device, &ai, &pre_slots[npre].cb) != VK_SUCCESS) return VK_NULL_HANDLE;
        npre++;
    }
    pre_slots[i].fence = cur_fence;
    VkCommandBuffer cb = pre_slots[i].cb;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    pre_pair = ts_open(cb);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    pre_cb = cb;
    return cb;
}

static VkCommandBuffer chain[CHAIN_MAX];
static int nchain;

static VkCommandBuffer pre_end(uint64_t fence) {
    VkCommandBuffer cb = pre_cb;
    if (!cb) return VK_NULL_HANDLE;
    vk_exec_mesh_flush(cb);
    vk_meshfb_pre_end(cb);
    ts_close(cb, pre_pair, fence, GTL_PRE);
    pre_pair = -1;
    vkEndCommandBuffer(cb);
    pre_cb = VK_NULL_HANDLE;
    return cb;
}

static VkCommandBuffer prologue(VkCtx *c, uint64_t fence) {
    vk_mem_shadow_sync(fence);
    vk_mem_lazy_part_end();
    if (!vk_mem_mirror_pending()) return VK_NULL_HANDLE;
    int i = claim_slot(c, fence);
    VkCommandBuffer cb = slots[i].cb;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    int pair = ts_open(cb);
    vk_mem_mirror_prologue(cb);
    ts_close(cb, pair, fence, 1);
    vkEndCommandBuffer(cb);
    return cb;
}

static uint32_t chain_infos(VkCommandBufferSubmitInfo *cbi, VkCommandBuffer pro, VkCommandBuffer pre, VkCommandBuffer cb) {
    uint32_t n = 0;
#define CHAIN_ADD(x) \
    if (x) cbi[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = (x)}
    CHAIN_ADD(pro);
    for (int k = 0; k < nchain; k++) CHAIN_ADD(chain[k]);
    CHAIN_ADD(pre);
    for (int i = 0; i < npart_cbs; i++) {
        if (!part_cbs[i]) rt_fatal("vk: a replay worker's command buffer is missing from a part");
        CHAIN_ADD(part_cbs[i]);
    }
    CHAIN_ADD(cb);
#undef CHAIN_ADD
    nchain = 0, npart_cbs = 0;
    return n;
}

VkCommandBuffer vk_sync_cut(void) {
    if (nchain + npart_cbs + 2 > CHAIN_MAX) return VK_NULL_HANDLE;
    for (int i = 0; i < npart_cbs; i++)
        if (!part_cbs[i]) return VK_NULL_HANDLE;
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = slots[cur_slot].cb;
    vk_gpu_passes_part(cb, 1);
    ts_end(cb, cur_fence);
    if (!rec_end_cb(cb, 0, 0)) vkEndCommandBuffer(cb);
    VkCommandBuffer pre = pre_end(cur_fence);
    if (pre) chain[nchain++] = pre;
    for (int i = 0; i < npart_cbs; i++) chain[nchain++] = part_cbs[i];
    npart_cbs = 0;
    chain[nchain++] = cb;
    CmdSlot *s = take_slot(c, cur_fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    ts_begin(s->cb);
    rec_begin(s->cb, cur_fence);
    vk_gpu_passes_part(s->cb, 0);
    return s->cb;
}

int vk_sync_append_room(int n) { return npart_cbs + n + 1 <= PART_CBS; }

VkCommandBuffer vk_sync_append_later(int n, int *at) {
    if (!vk_sync_append_room(n)) rt_fatal("vk: a submission part of more than %d command buffers", PART_CBS);
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = slots[cur_slot].cb;
    if (!rec_end_cb(cb, 0, 0)) vkEndCommandBuffer(cb);
    part_cbs[npart_cbs++] = cb;
    *at = npart_cbs;
    for (int i = 0; i < n; i++) part_cbs[npart_cbs++] = VK_NULL_HANDLE;
    CmdSlot *s = take_slot(c, cur_fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    rec_begin(s->cb, cur_fence);
    return s->cb;
}

void vk_sync_append_fill(int at, const VkCommandBuffer *cbs, int n) {
    if (at < 0 || at + n > npart_cbs) rt_fatal("vk: replay worker command buffers past the part's list");
    for (int i = 0; i < n; i++) part_cbs[at + i] = cbs[i];
}

#define SYNC_WORKERS 17
static struct { VkCommandPool pool; CmdSlot *slots; int n; } WP[SYNC_WORKERS];

VkCommandBuffer vk_sync_worker_begin(int w) {
    VkCtx *c = vk_ctx();
    if (w < 0 || w >= SYNC_WORKERS) rt_fatal("vk: replay worker %d of %d", w, SYNC_WORKERS);
    typeof(WP[0]) *p = &WP[w];
    if (!p->pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &p->pool) != VK_SUCCESS) rt_fatal("vk: replay worker command pool");
    }
    uint64_t gpu = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &gpu);
    CmdSlot *s = NULL;
    for (int i = 0; i < p->n && !s; i++)
        if (p->slots[i].fence <= gpu) s = &p->slots[i];
    if (!s) {
        CmdSlot *m = realloc(p->slots, sizeof *m * (size_t)(p->n + 1));
        if (!m) rt_fatal("vk: out of memory (command buffers)");
        p->slots = m, s = &m[p->n++];
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = p->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(c->device, &ai, &s->cb) != VK_SUCCESS) rt_fatal("vk: command buffer allocation");
    }
    s->fence = cur_fence;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    return s->cb;
}

void vk_sync_worker_end(VkCommandBuffer cb) { vkEndCommandBuffer(cb); }

uint64_t vk_sync_rec_fence(void) { return cur_fence; }

static VkCommandBuffer split_part(int exec);

int vk_sync_tail_ok(void) { return !pre_cb && !tail_cb && exec_tl && exec_sig < cur_fence && nchain + 2 <= CHAIN_MAX; }

VkCommandBuffer vk_sync_tail_hold(void) {
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = slots[cur_slot].cb;
    vk_gpu_passes_part(cb, 1);
    ts_end(cb, cur_fence);
    if (!rec_end_cb(cb, 0, 0)) vkEndCommandBuffer(cb);
    tail_cb = cb;
    CmdSlot *s = take_slot(c, cur_fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    ts_begin(s->cb);
    rec_begin(s->cb, cur_fence);
    vk_gpu_passes_part(s->cb, 0);
    return s->cb;
}

VkCommandBuffer vk_sync_tail_release(void) {
    VkCommandBuffer cb = split_part(1);
    chain[nchain++] = tail_cb;
    tail_cb = VK_NULL_HANDLE;
    tail_early = 1;
    return cb;
}

static uint64_t wait_target(uint64_t value, int *early) {
    plat_mutex_lock(&st_lock);
    uint64_t t = value;
    while (t > done && empty_of[t & (LIVE_RING - 1)] == t) t--;
    *early = t > done && early_of[t & (LIVE_RING - 1)] == t;
    plat_mutex_unlock(&st_lock);
    return t;
}

uint64_t vk_sync_tail_pending_ns(uint64_t value) {
    plat_mutex_lock(&st_lock);
    int pending = value > done && early_of[value & (LIVE_RING - 1)] == value;
    plat_mutex_unlock(&st_lock);
    return pending ? __atomic_load_n(&tail_ns, __ATOMIC_RELAXED) : 0;
}

void vk_sync_tail_stats(uint64_t *tails, uint64_t *waits, double *ms) {
    *tails = __atomic_exchange_n(&tail_n, 0, __ATOMIC_RELAXED);
    *waits = __atomic_exchange_n(&tail_waits, 0, __ATOMIC_RELAXED);
    *ms = (double)__atomic_load_n(&tail_ns, __ATOMIC_RELAXED) / 1e6;
}
VkCommandBuffer vk_sync_split(void) { return split_part(0); }
VkCommandBuffer vk_sync_split_exec(void) { return split_part(1); }

static VkCommandBuffer split_part(int exec) {
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = slots[cur_slot].cb;
    vk_gpu_passes_part(cb, 1);
    ts_end(cb, cur_fence);
    if (!rec_end_cb(cb, 0, 0)) vkEndCommandBuffer(cb);
    vk_upload_flush(0);
    VkCommandBuffer pro = prologue(c, cur_fence), pre = pre_end(cur_fence);
    VkCommandBufferSubmitInfo cbi[SUB_CBS];
    uint32_t ncb = chain_infos(cbi, pro, pre, cb);
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = ncb, .pCommandBufferInfos = cbi};
    VkSemaphoreSubmitInfo esig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = exec_tl,
                                  .value = cur_fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    if (exec && exec_tl && exec_sig < cur_fence)
        si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &esig, exec_sig = cur_fence;
    ts_submitted(cbi, ncb);
    queue_submit(c, &si, 0);
    sub_parts++;
    vk_mem_lazy_handed();
    vk_mem_note_submit(cur_fence);
    CmdSlot *s = take_slot(c, cur_fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    vk_crumb(s->cb, VK_CRUMB_SUBMIT, cur_fence);
    ts_begin(s->cb);
    rec_begin(s->cb, cur_fence);
    vk_gpu_passes_part(s->cb, 0);
    return s->cb;
}

uint64_t vk_sync_reserve(void) {
    vk_sync_start();
    plat_mutex_lock(&st_lock);
    if (reserved < issued) reserved = issued;
    uint64_t v = ++reserved;
    plat_mutex_unlock(&st_lock);
    gw_mark(v, 0);
    return v;
}

typedef struct {
    void (*record)(VkCommandBuffer, void *);
    void *arg;
} OwnCommands;

static uint64_t submit(const uint64_t *handles, int n, const OwnCommands *own) {
    vk_sync_start();
    if (vk_device_is_lost()) return __atomic_load_n(&issued, __ATOMIC_ACQUIRE);
    VkCtx *c = vk_ctx();
    plat_mutex_lock(&sub_lock);
    vk_present_frames_owed();
    if (n > 0 && !own) vk_sub_in_frame_publish();
    vk_mem_dirty_submit();
    uint64_t fence = __atomic_load_n(&issued, __ATOMIC_ACQUIRE) + 1;
    cur_fence = fence;
    tail_early = sub_parts = 0;
    CmdSlot *s = take_slot(c, fence);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    static int cpu_on = -1;
    if (cpu_on < 0) cpu_on = cfg_bool("NX2SARIA_REPLAY_TIME");
    uint32_t me = plat_thread_id();
    uint64_t t0 = perf_begin(), cpu0 = cpu_on ? plat_thread_cpu_ns(me) : 0;
    vkBeginCommandBuffer(s->cb, &bi);
    vk_crumb(s->cb, VK_CRUMB_SUBMIT, fence);
    ts_begin(s->cb);
    rec_begin(s->cb, fence);
    vk_gpu_passes_fence(fence);
    vk_gpu_passes_part(s->cb, 0);
    int recorded = 1;
    if (own) own->record(s->cb, own->arg);
    else vk_reg_replay_begin(), recorded = vk_exec_record(s->cb, handles, n, fence);
    s = &slots[cur_slot];
    vk_gpu_passes_part(s->cb, 1);
    if (recorded) ts_end(s->cb, fence);
    else ts_drop();
    if (!rec_end_cb(s->cb, VK_CRUMB_SUBMIT_END, fence)) vkEndCommandBuffer(s->cb);
    perf_replay(t0);
    if (cpu0) perf_replay_cpu(plat_thread_cpu_ns(me) - cpu0);
    vk_upload_flush(0);
    VkCommandBuffer pro = prologue(c, fence), pre = pre_end(fence);
    s = &slots[cur_slot];
    VkCommandBufferSubmitInfo cbi[SUB_CBS];
    int sent = recorded || nchain || npart_cbs;
    uint32_t ncb = chain_infos(cbi, pro, pre, sent ? s->cb : VK_NULL_HANDLE);
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = c->timeline,
                                     .value = fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = exec_tl,
                                     .value = fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    int both = exec_tl && exec_sig < fence;
    if (both) exec_sig = fence;
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = ncb, .pCommandBufferInfos = cbi,
                        .signalSemaphoreInfoCount = both ? 2u : 1u, .pSignalSemaphoreInfos = sig};
    ts_submitted(cbi, ncb);
    queue_submit(c, &si, fence);
    if (!own) vk_reg_replay_end(fence);
    vk_mem_lazy_handed();
    s->fence = sent ? fence : 0;
    vk_mem_note_submit(fence);
    gw_mark(fence, 1);
    int live = own ? 1 : vk_mem_take_live();
    int res = own ? 0x1f : vk_exec_take_results();
    plat_mutex_lock(&st_lock);
    live_of[fence & (LIVE_RING - 1)] = fence << 1 | (uint64_t)(live != 0);
    if (live) live_last = fence;
    live_last_of[fence & (LIVE_RING - 1)] = live_last;
    res_kinds[fence & (LIVE_RING - 1)] = (uint8_t)res;
    early_of[fence & (LIVE_RING - 1)] = tail_early ? fence : 0;
    empty_of[fence & (LIVE_RING - 1)] = !own && !sent && !sub_parts && !res ? fence : 0;
    if (tail_early) __atomic_fetch_add(&tail_n, 1, __ATOMIC_RELAXED);
    if (res & res_mask) res_last = fence;
    res_of[fence & (LIVE_RING - 1)] = res_last;
    __atomic_store_n(&issued, fence, __ATOMIC_RELEASE);
    plat_cond_broadcast(&cv_work);
    plat_cond_broadcast(&cv_done);
    plat_mutex_unlock(&st_lock);
    static unsigned subs;
    if (++subs % 4096 == 0) {
        rec_stats_log();
        if (subs % (4096 * 8) == 4096)
            rt_log("vk: submit batching: %llu submissions in %llu queue calls (%.2f each)", (unsigned long long)H.subs,
                   (unsigned long long)H.batches, H.batches ? (double)H.subs / (double)H.batches : 0.0);
    }
    plat_mutex_unlock(&sub_lock);
    return fence;
}

uint64_t vk_sync_submit(const uint64_t *handles, int n) { return submit(handles, n, NULL); }

uint64_t vk_sync_submit_own(void (*record)(VkCommandBuffer, void *), void *arg) {
    OwnCommands own = {record, arg};
    return submit(NULL, 0, &own);
}

int vk_sync_try_locked(void (*fn)(void)) {
    if (!plat_mutex_trylock(&sub_lock)) return 0;
    fn();
    plat_mutex_unlock(&sub_lock);
    return 1;
}

void vk_sync_locked(void (*fn)(void)) {
    plat_mutex_lock(&sub_lock);
    fn();
    plat_mutex_unlock(&sub_lock);
}

WEAK void vk_present_frames_owed(void) {}
WEAK void vk_sub_in_frame_publish(void) {}

static void pending_push(Pending **q, int *head, int *n, int *cap, Pending p) {
    if (*head + *n == *cap) {
        if (*head) {
            memmove(*q, *q + *head, sizeof **q * (size_t)*n);
            *head = 0;
        } else {
            int c = *cap ? *cap * 2 : 256;
            Pending *m = realloc(*q, sizeof *m * (size_t)c);
            if (!m) rt_fatal("vk: out of memory (completion queue)");
            *q = m, *cap = c;
        }
    }
    (*q)[*head + (*n)++] = p;
}

void vk_completion_add_first(uint64_t fence, void (*fn)(void *), void *arg) {
    plat_mutex_lock(&st_lock);
    pending_push(&fqueue, &fqhead, &fqn, &fqcap, (Pending){fence, fn, arg});
    if (fence <= done) plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
}

void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg) {
    plat_mutex_lock(&st_lock);
    pending_push(&queue, &qhead, &qn, &qcap, (Pending){fence, fn, arg});
    if (fence <= done) plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
}

void vk_completion_add_result(uint64_t fence, void (*fn)(void *), void *arg) {
    plat_mutex_lock(&st_lock);
    pending_push(&rqueue, &rqhead, &rqn, &rqcap, (Pending){fence, fn, arg});
    if (fence <= done) plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
}

static void results_run(uint64_t upto) {
    plat_mutex_lock(&res_lock);
    plat_mutex_lock(&st_lock);
    while (rqn && rqueue[rqhead].fence <= upto && !vk_device_is_lost()) {
        Pending p = rqueue[rqhead++];
        rqn--;
        plat_mutex_unlock(&st_lock);
        __atomic_store_n(&cb_late, p.fence <= __atomic_load_n(&exw_value, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
        p.fn(p.arg);
        __atomic_store_n(&cb_late, 0, __ATOMIC_RELAXED);
        plat_mutex_lock(&st_lock);
    }
    plat_mutex_unlock(&st_lock);
    plat_mutex_unlock(&res_lock);
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns);


int vk_sync_result_mask(void) { return res_mask; }

int vk_sync_recorded_wait(uint64_t value, uint64_t timeout_ns, int (*gpu_wait)(uint64_t, uint64_t)) {
    vk_sync_start();
    static int gpu = -1, res_mode = 1;
    if (gpu < 0) {
        res_mode = rt_knob("SUBMIT_FINISH_RESULTS", "1");
        const char *km = cfg_get("NX2SARIA_SUBMIT_FINISH_KINDS");
        res_mask = (km && *km ? (int)strtol(km, NULL, 0) : 31) & 0x1f;
        rt_log("vk: result kinds the submit wait counts: %#x (NX2SARIA_SUBMIT_FINISH_KINDS: 1 events, 2 counter reports, "
               "4 occlusion reports, 8 GPU writes of unmirrored memory, 16 mirror write-backs)", res_mask);
        rt_log("vk: the wait after a frame's first submission also waits for %s (NX2SARIA_SUBMIT_FINISH_RESULTS=%d)",
               res_mode == 2 ? "every submission before it" : res_mode ? "the newest submission up to it that hands the "
               "CPU results at its completion (events, counter reports, write-backs)" : "nothing more", res_mode);
        gpu = rt_knob("SUBMIT_FINISH_GPU", "1") != 0;
        rt_log("vk: the wait after a frame's first submission ends %s (NX2SARIA_SUBMIT_FINISH_GPU=%d)",
               gpu ? "when the GPU has run it" : "when the replay has submitted it, or the GPU has run it where it reads guest "
               "memory itself", gpu);
    }
    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) >= value) return 1;
    if (gpu) return gpu_wait(value, timeout_ns);
    uint64_t w0 = plat_now_ns(), newest = __atomic_load_n(&reserved, __ATOMIC_ACQUIRE);
    uint64_t sent = __atomic_load_n(&issued, __ATOMIC_ACQUIRE);
    int live = 1, ok = 1;
    plat_mutex_lock(&st_lock);
    if (value > issued && value > reserved) ok = 0;
    while (ok && issued < value && done < value) {
        if (timeout_ns < (1ull << 62)) {
            uint64_t el = plat_now_ns() - w0;
            if (el >= timeout_ns) { ok = 0; break; }
            plat_cond_wait_ns(&cv_done, &st_lock, timeout_ns - el);
        } else {
            plat_cond_wait(&cv_done, &st_lock);
        }
    }
    uint64_t need = 0;
    if (ok) {
        live = done < value && live_of[value & (LIVE_RING - 1)] != (value << 1);
        int known = live_of[value & (LIVE_RING - 1)] >> 1 == value;
        need = known ? res_of[value & (LIVE_RING - 1)] : value;
        if (res_mode == 2) need = value - 1;
        if (!res_mode) need = 0;
        uint64_t lneed = known ? live_last_of[value & (LIVE_RING - 1)] : value;
        if (lneed > need) need = lneed;
        while (!live && need && done < need) {
            if (timeout_ns < (1ull << 62)) {
                uint64_t el = plat_now_ns() - w0;
                if (el >= timeout_ns) { ok = 0; break; }
                plat_cond_wait_ns(&cv_done, &st_lock, timeout_ns - el);
            } else {
                plat_cond_wait(&cv_done, &st_lock);
            }
        }
    }
    plat_mutex_unlock(&st_lock);
    uint64_t w1 = plat_now_ns();
    if (w1 > w0) {
        if (!live) gw_wait(value, w0, w1, newest > sent ? newest : sent, sent);
        plat_mutex_lock(&gw_lock);
        gw_rec_n++;
        if (!live) gw_rec_done++;
        if (!live && need == value) gw_rec_self++;
        if (!live && need) vk_gpu_passes_waited(need);
        if (!live && need && need <= value) {
            gw_need_back += value - need, gw_need_n++;
            int k = res_kinds[need & (LIVE_RING - 1)];
            for (int b = 0; b < VK_RES_KINDS; b++) if (k >> b & 1) gw_need_kind[b]++;
            if (need < value) {
                for (uint64_t v = need + 1; v <= value && v - need < 64; v++)
                    for (int b = 0; b < VK_RES_KINDS; b++) if (res_kinds[v & (LIVE_RING - 1)] >> b & 1) gw_skip_kind[b]++;
            }
        }
        plat_mutex_unlock(&gw_lock);
    }
    if (!ok) return 0;
    return live ? gpu_wait(value, timeout_ns) : 1;
}

int vk_sync_fence_wait(uint64_t value, uint64_t timeout_ns) {
    uint64_t p0 = perf_begin();
    uint64_t newest = __atomic_load_n(&reserved, __ATOMIC_ACQUIRE), sent = __atomic_load_n(&issued, __ATOMIC_ACQUIRE);
    uint64_t w0 = __atomic_load_n(&done, __ATOMIC_ACQUIRE) >= value ? 0 : plat_now_ns();
    int ok = fence_wait_impl(value, timeout_ns);
    if (w0 && ok) gw_wait(value, w0, plat_now_ns(), newest > sent ? newest : sent, sent);
    perf_end(PERF_FENCE, p0);
    perf_gpu_wait(p0);
    return ok;
}

static int fence_wait_impl(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    hold_release(value, 0, 0);
    plat_mutex_lock(&st_lock);
    int ok;
    if (value > issued && value > reserved) {
        ok = 0;
    } else {
        uint64_t t0 = kern_now_ns();
        while (done < value) {
            if (timeout_ns < (1ull << 62)) {
                uint64_t el = kern_now_ns() - t0;
                if (el >= timeout_ns) break;
                plat_cond_wait_ns(&cv_done, &st_lock, timeout_ns - el);
            } else {
                plat_cond_wait(&cv_done, &st_lock);
            }
        }
        ok = done >= value;
    }
    plat_mutex_unlock(&st_lock);
    return ok;
}

int vk_sync_exec_wait(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    if (vk_device_is_lost()) return __atomic_load_n(&done, __ATOMIC_ACQUIRE) >= value;
    if (!vk_sync_wb_split()) return vk_sync_fence_wait(value, timeout_ns);
    if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) >= value) return 1;
    int early = 0;
    uint64_t asked = value, target = wb_results ? wait_target(value, &early) : value;
    if (early) value = target;
    uint64_t p0 = perf_begin();
    hold_release(asked, 0, 0);
    VkCtx *c = vk_ctx();
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &exec_tl, .pValues = &value};
    VkResult r = vkWaitSemaphores(c->device, &wi, timeout_ns);
    if (r != VK_SUCCESS && r != VK_TIMEOUT) {
        vk_device_lost_check(r, "the wait for a submission's commands");
        rt_fatal("vk: the wait for a submission's commands failed (%d; device lost?)", (int)r);
    }
    if (r == VK_SUCCESS) {
        uint64_t raw = 0;
        VkResult q = vkGetSemaphoreCounterValue(c->device, exec_tl, &raw);
        vk_device_lost_check(q, "the read of the executed timeline");
        if (q == VK_SUCCESS)
            timeline_beyond("executed timeline", raw, &ex_hw, "the wait for a submission's commands (a value no part "
                                                              "signals)");
    }
    if (r == VK_SUCCESS) vk_mem_merge_cpu_wait(asked);
    if (r == VK_SUCCESS && wb_results) results_run(value);
    perf_end(PERF_FENCE, p0);
    perf_gpu_wait(p0);
    if (r == VK_SUCCESS && early && __atomic_load_n(&done, __ATOMIC_ACQUIRE) < value) {
        __atomic_fetch_add(&tail_waits, 1, __ATOMIC_RELAXED);
        uint64_t none = 0;
        if (__atomic_load_n(&tail_val, __ATOMIC_ACQUIRE) != value &&
            __atomic_compare_exchange_n(&tail_val, &none, value, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            __atomic_store_n(&tail_t0, kern_now_ns(), __ATOMIC_RELAXED);
    }
    if (r == VK_SUCCESS) {
        __atomic_fetch_add(&exw_n, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&done, __ATOMIC_ACQUIRE) < value) {
            __atomic_fetch_add(&exw_early, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&exw_t0, kern_now_ns(), __ATOMIC_RELAXED);
            __atomic_store_n(&exw_value, value, __ATOMIC_RELEASE);
        }
    }
    return r == VK_SUCCESS;
}

int vk_sync_game_wait(uint64_t value, uint64_t timeout_ns) {
    vk_sync_start();
    if (vk_sync_wb_split() && wb_results && __atomic_load_n(&done, __ATOMIC_ACQUIRE) < value) {
        int early = 0;
        wait_target(value, &early);
        if (early) return vk_sync_exec_wait(value, timeout_ns);
    }
    return vk_sync_fence_wait(value, timeout_ns);
}

uint64_t vk_sync_fence_done(void) { return __atomic_load_n(&done, __ATOMIC_ACQUIRE); }

uint64_t vk_sync_issued(void) { return __atomic_load_n(&issued, __ATOMIC_ACQUIRE); }

void vk_sync_finish(void) {
    vk_sync_start();
    vk_sync_fence_wait(__atomic_load_n(&issued, __ATOMIC_ACQUIRE), UINT64_MAX);
    vk_upload_flush(1);
}

uint32_t vk_completion_flush_idle(void) {
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE) || vk_device_is_lost() ||
        plat_thread_id() == __atomic_load_n(&cb_tid, __ATOMIC_ACQUIRE))
        return 0;
    vk_sync_finish();
    plat_mutex_lock(&st_lock);
    int nf = fqn, nq = qn, left = rqn, k = 0;
    Pending *run = nf + nq ? malloc(sizeof *run * (size_t)(nf + nq)) : NULL;
    if (run) {
        for (int i = 0; i < nf; i++) run[k++] = fqueue[fqhead + i];
        for (int i = 0; i < nq; i++) run[k++] = queue[qhead + i];
        fqhead = fqn = qhead = qn = 0;
    } else {
        nf = nq = 0;
    }
    uint64_t reached = done, newest = 0;
    plat_mutex_unlock(&st_lock);
    uint32_t ahead = 0;
    for (int i = 0; i < k; i++) {
        ahead += run[i].fence > reached;
        if (run[i].fence > newest) newest = run[i].fence;
        run[i].fn(run[i].arg);
    }
    free(run);
    if (k || left)
        rt_log("vk: sync teardown: ran %d callbacks still queued after the last submission finished, on this thread (%d "
               "first, %d ordinary; %u for values past the last one, %llu, the newest %llu); %d result callbacks left",
               k, nf, nq, ahead, (unsigned long long)reached, (unsigned long long)newest, left);
    return (uint32_t)k;
}

void vk_sync_shutdown(void) {
    plat_mutex_lock(&start_lock);
    if (started) {
        vk_sync_finish();
        vk_completion_flush_idle();
        plat_mutex_lock(&st_lock);
        stop = 1;
        plat_cond_broadcast(&cv_work);
        plat_mutex_unlock(&st_lock);
        plat_thread_join(completion_thread);
        completion_thread = NULL;
        plat_mutex_lock(&st_lock);
        if (qn || rqn || fqn) {
            uint64_t lo = UINT64_MAX;
            for (int i = 0; i < qn; i++) lo = queue[qhead + i].fence < lo ? queue[qhead + i].fence : lo;
            for (int i = 0; i < rqn; i++) lo = rqueue[rqhead + i].fence < lo ? rqueue[rqhead + i].fence : lo;
            for (int i = 0; i < fqn; i++) lo = fqueue[fqhead + i].fence < lo ? fqueue[fqhead + i].fence : lo;
            rt_log("vk: sync shutdown: %zu callbacks left in the queues are dropped (%zu, %zu results, %zu first; lowest value "
                   "%llu, done %llu, issued %llu, reserved %llu)", (size_t)(qn + rqn + fqn), (size_t)qn, (size_t)rqn,
                   (size_t)fqn, (unsigned long long)lo, (unsigned long long)done, (unsigned long long)issued,
                   (unsigned long long)reserved);
        }
        qhead = qn = rqhead = rqn = fqhead = fqn = 0;
        plat_mutex_unlock(&st_lock);
        plat_mutex_lock(&sub_lock);
        VkCtx *c = vk_ctx();
        if (c && cmd_pool) vkDestroyCommandPool(c->device, cmd_pool, NULL);
        for (int w = 0; w < SYNC_WORKERS; w++) {
            if (c && WP[w].pool) vkDestroyCommandPool(c->device, WP[w].pool, NULL);
            free(WP[w].slots);
            memset(&WP[w], 0, sizeof WP[w]);
        }
        if (c && pre_pool) vkDestroyCommandPool(c->device, pre_pool, NULL);
        pre_pool = VK_NULL_HANDLE;
        free(pre_slots);
        pre_slots = NULL;
        npre = 0, pre_pair = -1, pre_cb = VK_NULL_HANDLE, nchain = 0;
        if (c && exec_tl) vkDestroySemaphore(c->device, exec_tl, NULL);
        exec_tl = VK_NULL_HANDLE;
        perf_gpu_stats = NULL;
        if (c && ts_pool) vkDestroyQueryPool(c->device, ts_pool, NULL);
        ts_pool = VK_NULL_HANDLE;
        ts_state = 0;
        ts_last_end = 0;
        memset((void *)ts_busy, 0, sizeof ts_busy);
        cmd_pool = VK_NULL_HANDLE;
        free(slots);
        slots = NULL;
        nslots = 0;
        plat_mutex_unlock(&sub_lock);
        __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&start_lock);
}

static volatile int32_t probe_ran[2];
static void probe_lost_cb(void *arg) { plat_xchg32((volatile int32_t *)arg, 1); }
static void probe_lost_record(VkCommandBuffer cb, void *arg) { (void)cb; plat_xchg32((volatile int32_t *)arg, 1); }
static volatile int32_t probe_go;
static void probe_lost_thread(void *arg) {
    static const char *const where[4] = {"probe thread 0", "probe thread 1", "probe thread 2", "probe thread 3"};
    while (!__atomic_load_n(&probe_go, __ATOMIC_ACQUIRE)) {}
    vk_device_lost_check(VK_ERROR_DEVICE_LOST, where[(intptr_t)arg & 3]);
}

static uint64_t probe_lost_advance(VkCtx *c, volatile int32_t *ran) {
    plat_mutex_lock(&st_lock);
    uint64_t v = issued + 1;
    plat_mutex_unlock(&st_lock);
    vk_completion_add(v, probe_lost_cb, (void *)ran);
    sync_hw_raise(&tl_hw, v);
    VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline, .value = v};
    if (vkSignalSemaphore(c->device, &si) != VK_SUCCESS) return 0;
    plat_mutex_lock(&st_lock);
    __atomic_store_n(&issued, v, __ATOMIC_RELEASE);
    plat_cond_broadcast(&cv_work);
    plat_mutex_unlock(&st_lock);
    return v;
}

int vk_sync_probe_devlost(void) {
    if (!vk_init()) {
        printf("probe-devlost: SKIP (no Vulkan device on this machine)\n");
        return 0;
    }
    vk_sync_start();
    VkCtx *c = vk_ctx();
    uint64_t v1 = probe_lost_advance(c, &probe_ran[0]);
    for (int i = 0; i < 200 && !plat_cas32(&probe_ran[0], 1, 1); i++) plat_sleep_ms(10);
    if (!v1 || !plat_cas32(&probe_ran[0], 1, 1) || !vk_sync_fence_wait(v1, 2000000000ull)) {
        printf("probe-devlost: FAILED: a callback did not run on a healthy device (value %llu)\n", (unsigned long long)v1);
        return 1;
    }
    printf("probe-devlost: healthy device: the callback of value %llu ran\n", (unsigned long long)v1);

    plat_xchg32(&vk_lost, 1);
    uint64_t v2 = probe_lost_advance(c, &probe_ran[1]);
    plat_sleep_ms(500);
    int ran = plat_cas32(&probe_ran[1], 1, 1), reached = vk_sync_fence_done() >= v2;
    volatile int32_t recorded = 0;
    uint64_t before = vk_sync_issued(), t0 = plat_now_ms();
    uint64_t got = vk_sync_submit_own(probe_lost_record, (void *)&recorded);
    vk_upload_flush(1);
    int waited = vk_sync_exec_wait(v2, 5000000000ull);
    uint64_t ms = plat_now_ms() - t0;
    int ok = v2 && !ran && !reached && !recorded && got == before && vk_sync_issued() == before && !waited && ms < 1000;
    printf("probe-devlost: device marked lost: callback of value %llu %s, done %s, submit %s (issued %llu -> %llu), "
           "the paths returned in %llu ms\n", (unsigned long long)v2, ran ? "RAN" : "not run", reached ? "ADVANCED" : "stayed",
           recorded ? "RECORDED" : "not recorded", (unsigned long long)before, (unsigned long long)vk_sync_issued(),
           (unsigned long long)ms);
    if (!ok) { printf("probe-devlost: FAILED\n"); return 1; }

    printf("probe-devlost: four threads report the loss now; the process must end with exit code %d\n", RT_LOST_EXIT);
    fflush(stdout);
    plat_xchg32(&vk_lost, 0);
    for (intptr_t i = 0; i < 4; i++) plat_thread_spawn(probe_lost_thread, (void *)i, "probe lost", 0);
    plat_sleep_ms(50);
    __atomic_store_n(&probe_go, 1, __ATOMIC_RELEASE);
    plat_sleep_ms(20000);
    printf("probe-devlost: FAILED: still running 20 s after the loss was reported\n");
    return 1;
}

void vk_sync_sizes(PerfSizes *s) {
    uint64_t slots = (uint64_t)__atomic_load_n(&nslots, __ATOMIC_RELAXED);
    plat_mutex_lock(&st_lock);
    uint64_t q = (uint64_t)qn;
    plat_mutex_unlock(&st_lock);
    perf_size(s, "command buffer slots", slots, 0, PERF_SIZE_NONE);
    perf_size(s, "completion queue", q, 0, PERF_SIZE_NONE);
    perf_size(s, "held submissions", (uint64_t)__atomic_load_n(&held_n, __ATOMIC_RELAXED), 0, PERF_SIZE_NONE);
}
