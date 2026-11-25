#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlssg.h"
#include "vk_fg.h"
#include "vk_lowlat.h"

enum { LL_NONE, LL_NV, LL_AMD };
#define RING 64
#define HOLD_IDLE_NS 200000000ull
#define SLEEP_MAX_NS 500000000ull
#define SLEEP_FAILS 3
#define REPORTS 16
#define PACE_WINDOW 60
#define MODE_UNSET 0xffffffffu
#define TIMINGS_REV 2u

#define LOAD(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)

static struct {
    int kind;
    int why;
    int present_ids;
    uint32_t rev;
    int no_report_said;
    VkDevice dev;
    PFN_vkSetLatencySleepModeNV set_mode;
    PFN_vkLatencySleepNV sleep;
    PFN_vkSetLatencyMarkerNV marker;
    PFN_vkGetLatencyTimingsNV timings;
    PFN_vkAntiLagUpdateAMD antilag;
    VkSemaphore sem;
    PlatMutex m;
    VkSwapchainKHR sc;
    uint32_t mode;
    uint64_t sleep_value;
    int attached;
    int on;
    int said;
    int failed;
    int fg_block;
    int pace_bad;
    int cfg_read, level;
    unsigned cfg_seen;
    uint32_t interval_us;
    uint32_t fails;
    uint64_t game;
    uint64_t started;
    uint64_t input_id;
    uint64_t submit_id;
    uint64_t cur;
    uint64_t hosted_ns;
    uint64_t ring[RING];
    uint64_t staged[RING];
    uint64_t n_sleep, sleep_ns, n_input, n_unheld, n_timeout;
} L = {.why = VK_LOWLAT_NO_DRIVER, .mode = MODE_UNSET};

static int level_now(void) {
    unsigned v = cfg_version();
    if (!LOAD(&L.cfg_read) || v != LOAD(&L.cfg_seen)) {
        int n = cfg_int("NX2SARIA_LOW_LATENCY");
        STORE(&L.level, n < 0 ? 0 : n > 2 ? 2 : n);
        STORE(&L.cfg_seen, v);
        STORE(&L.cfg_read, 1);
    }
    return LOAD(&L.level);
}

static int level_eff(void) { return LOAD(&L.failed) ? 0 : level_now(); }

static int frame_on(uint64_t id) { return id && LOAD(&L.ring[id % RING]) == id; }

static void say(int on, const char *why) {
    static int32_t lines;
    if (__atomic_exchange_n(&L.said, on, __ATOMIC_ACQ_REL) == on || plat_inc32(&lines) > 40) return;
    if (on)
        rt_log("lowlat: low latency on%s: the driver holds each frame's start, and the frame limit with it",
               L.kind == LL_NV && level_now() == 2 ? " with boost" : "");
    else rt_log("lowlat: low latency off (%s): the game's own limiter paces the frames", why);
}

static void give_up(const char *what, int r) {
    if (__atomic_exchange_n(&L.failed, 1, __ATOMIC_ACQ_REL)) return;
    rt_log("lowlat: %s (%d): low latency is off for the rest of the run", what, r);
}

static uint32_t mode_want(void) {
    int lv = LOAD(&L.fg_block) ? 0 : level_eff();
    return lv ? (uint32_t)lv | LOAD(&L.interval_us) << 2 : 0;
}

static void nv_mode_locked(uint32_t want) {
    if (want == L.mode) return;
    VkLatencySleepModeInfoNV mi = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
                                   .lowLatencyMode = (want & 3) != 0, .lowLatencyBoost = (want & 3) == 2,
                                   .minimumIntervalUs = want >> 2};
    VkResult r = L.set_mode(L.dev, L.sc, &mi);
    if (r != VK_SUCCESS) {
        give_up("the driver refused the low-latency sleep mode", (int)r);
        want = 0;
    }
    STORE(&L.mode, want);
    STORE(&L.on, (want & 3) != 0);
    say((want & 3) != 0, LOAD(&L.failed) ? "the driver refused it" : LOAD(&L.fg_block) ? "frame generation presents the frames"
                                                                                       : "the setting");
}

static void nv_marker(VkLatencyMarkerNV marker, uint64_t id) {
    VkSetLatencyMarkerInfoNV mi = {.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = id, .marker = marker};
    plat_mutex_lock(&L.m);
    if (L.sc) L.marker(L.dev, L.sc, &mi);
    plat_mutex_unlock(&L.m);
}

static int nv_sleep(void) {
    VkLatencySleepInfoNV si = {.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV, .signalSemaphore = L.sem};
    VkResult r = VK_NOT_READY;
    plat_mutex_lock(&L.m);
    if (L.sc && L.mode != MODE_UNSET && (L.mode & 3)) {
        si.value = ++L.sleep_value;
        r = L.sleep(L.dev, L.sc, &si);
    }
    plat_mutex_unlock(&L.m);
    if (r == VK_NOT_READY) return 0;
    if (r != VK_SUCCESS) {
        vk_device_lost_check(r, "the low-latency sleep");
        give_up("the driver refused the low-latency sleep", (int)r);
        return 0;
    }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &L.sem,
                              .pValues = &si.value};
    r = vkWaitSemaphores(L.dev, &wi, SLEEP_MAX_NS);
    vk_device_lost_check(r, "the wait for the low-latency sleep");
    if (r == VK_SUCCESS) {
        STORE(&L.fails, 0);
        return 1;
    }
    __atomic_add_fetch(&L.n_timeout, 1, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&L.fails, 1, __ATOMIC_ACQ_REL) >= SLEEP_FAILS)
        give_up("the driver did not end its low-latency sleep three times in a row", (int)r);
    return 0;
}

static void amd_stage(VkAntiLagStageAMD stage, uint64_t id) {
    uint32_t us = LOAD(&L.interval_us);
    VkAntiLagPresentationInfoAMD pi = {.sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD, .stage = stage,
                                       .frameIndex = id};
    VkAntiLagDataAMD d = {.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD, .mode = VK_ANTI_LAG_MODE_ON_AMD,
                          .maxFPS = us ? (1000000u + us / 2) / us : 0, .pPresentationInfo = &pi};
    plat_mutex_lock_shared(&L.m);
    if (LOAD(&L.on)) L.antilag(L.dev, &d);
    plat_mutex_unlock_shared(&L.m);
}

static int refresh(void) {
    if (L.kind == LL_AMD) {
        int want = level_eff() > 0, was = __atomic_exchange_n(&L.on, want, __ATOMIC_ACQ_REL);
        if (was && !want) {
            VkAntiLagDataAMD d = {.sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD, .mode = VK_ANTI_LAG_MODE_OFF_AMD};
            plat_mutex_lock(&L.m);
            if (!LOAD(&L.on)) L.antilag(L.dev, &d);
            plat_mutex_unlock(&L.m);
        }
        say(want, LOAD(&L.failed) ? "the driver refused it" : "the setting");
        return want;
    }
    if (!LOAD(&L.attached)) return 0;
    uint32_t want = mode_want();
    if (want != LOAD(&L.mode)) {
        plat_mutex_lock(&L.m);
        if (L.sc) nv_mode_locked(want);
        plat_mutex_unlock(&L.m);
    }
    return LOAD(&L.on);
}

static void frame_begin(uint64_t id, int at_input) {
    uint64_t t0 = kern_now_ns(), hosted = LOAD(&L.hosted_ns);
    STORE(&L.ring[id % RING], id);
    if (hosted && t0 - hosted < HOLD_IDLE_NS) {
        int away = kern_core_away();
        uint64_t p0 = perf_begin();
        if (L.kind == LL_NV) nv_sleep();
        else STORE(&L.staged[id % RING], id), amd_stage(VK_ANTI_LAG_STAGE_INPUT_AMD, id);
        perf_end_any(PERF_PACE, p0);
        kern_core_back(&away);
        __atomic_add_fetch(&L.sleep_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&L.n_sleep, 1, __ATOMIC_RELAXED);
        if (at_input) __atomic_add_fetch(&L.n_input, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&L.n_unheld, 1, __ATOMIC_RELAXED);
    }
    if (L.kind == LL_NV) nv_marker(VK_LATENCY_MARKER_SIMULATION_START_NV, id);
}

void vk_lowlat_frame_start(int at_input) {
    if (L.kind == LL_NONE) return;
    if (!refresh() || vk_device_is_lost()) return;
    uint64_t id = LOAD(&L.game) + 1, prev = LOAD(&L.started);
    if (prev != id && __atomic_compare_exchange_n(&L.started, &prev, id, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        frame_begin(id, at_input);
    if (!at_input || L.kind != LL_NV) return;
    prev = LOAD(&L.input_id);
    if (prev != id && __atomic_compare_exchange_n(&L.input_id, &prev, id, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) && frame_on(id))
        nv_marker(VK_LATENCY_MARKER_INPUT_SAMPLE_NV, id);
}

void vk_lowlat_submit_start(void) {
    if (L.kind != LL_NV || !LOAD(&L.on)) return;
    uint64_t id = LOAD(&L.game) + 1, prev = LOAD(&L.submit_id);
    if (prev == id || !frame_on(id)) return;
    if (!__atomic_compare_exchange_n(&L.submit_id, &prev, id, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    nv_marker(VK_LATENCY_MARKER_SIMULATION_END_NV, id);
    nv_marker(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, id);
}

void vk_lowlat_game_present(void) { __atomic_add_fetch(&L.game, 1, __ATOMIC_RELEASE); }

void vk_lowlat_submitted(uint64_t frame) {
    if (L.kind == LL_NV && frame_on(frame)) nv_marker(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, frame);
}

int vk_lowlat_pace(uint64_t period_ns) {
    static uint64_t last_ns, last_period;
    static int window, fast;
    if (!LOAD(&L.on)) return 0;
    uint64_t now = kern_now_ns(), hosted = LOAD(&L.hosted_ns);
    if (last_period && last_period == period_ns && !LOAD(&L.pace_bad)) {
        if (now - last_ns < period_ns / 4 * 3) fast++;
        if (++window >= PACE_WINDOW) {
            if (fast > PACE_WINDOW / 2) {
                STORE(&L.pace_bad, 1);
                rt_log("lowlat: the driver's sleep does not hold the frame limit (%d of %d frames came sooner than three "
                       "quarters of %.2f ms): the game's own limiter holds it for the rest of the run", fast, PACE_WINDOW,
                       (double)period_ns / 1e6);
            }
            window = fast = 0;
        }
    }
    uint64_t us = period_ns && !LOAD(&L.pace_bad) && hosted && now - hosted < HOLD_IDLE_NS ? (period_ns + 500) / 1000 : 0;
    if (us > 0x3fffffffu) us = 0x3fffffffu;
    STORE(&L.interval_us, (uint32_t)us);
    last_ns = now, last_period = us ? period_ns : 0;
    return us != 0;
}

static void tag_fill(VkLatencySubmissionPresentIdNV *tag, uint64_t id) {
    *tag = (VkLatencySubmissionPresentIdNV){.sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV, .presentID = id};
}

int vk_lowlat_submit_tag(VkLatencySubmissionPresentIdNV *tag) {
    if (L.kind != LL_NV || !LOAD(&L.on)) return 0;
    uint64_t id = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    if (!frame_on(id)) return 0;
    tag_fill(tag, id);
    return 1;
}

void vk_lowlat_present_frame(uint64_t frame) { STORE(&L.cur, frame); }

uint64_t vk_lowlat_blit_frame(VkLatencySubmissionPresentIdNV *tag, int *tagged) {
    *tagged = 0;
    if (L.kind == LL_NONE) return 0;
    uint64_t id = __atomic_exchange_n(&L.cur, 0, __ATOMIC_ACQ_REL);
    if (!frame_on(id)) return 0;
    if (L.kind == LL_NV) tag_fill(tag, id), *tagged = 1;
    return id;
}

const void *vk_lowlat_present_begin(uint64_t frame, VkLowlatPresent *p) {
    p->id = 0;
    if (L.kind == LL_NONE || !frame_on(frame)) return NULL;
    p->id = frame;
    if (L.kind == LL_AMD) {
        if (LOAD(&L.on) && LOAD(&L.staged[frame % RING]) == frame) amd_stage(VK_ANTI_LAG_STAGE_PRESENT_AMD, frame);
        return NULL;
    }
    VkSetLatencyMarkerInfoNV mi = {.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, .presentID = frame,
                                   .marker = VK_LATENCY_MARKER_PRESENT_START_NV};
    plat_mutex_lock(&L.m);
    int have = L.sc != VK_NULL_HANDLE;
    if (have) L.marker(L.dev, L.sc, &mi);
    plat_mutex_unlock(&L.m);
    if (!have || !L.present_ids) return NULL;
    p->info = (VkPresentIdKHR){.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &p->id};
    return &p->info;
}

static void nv_report(char *out, size_t cap) {
    static VkLatencyTimingsFrameReportNV rep[REPORTS];
    memset(rep, 0, sizeof rep);
    for (int i = 0; i < REPORTS; i++) rep[i].sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;
    VkGetLatencyMarkerInfoNV gi = {.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV, .timingCount = REPORTS, .pTimings = rep};
    plat_mutex_lock(&L.m);
    if (L.sc) L.timings(L.dev, L.sc, &gi);
    else gi.timingCount = 0;
    plat_mutex_unlock(&L.m);
    uint32_t n = gi.timingCount > REPORTS ? REPORTS : gi.timingCount, used = 0;
    double total = 0, sim = 0, sub = 0, pres = 0, drv = 0, osq = 0, gpu = 0;
#define SPAN(a, b) ((b) > (a) ? (double)((b) - (a)) / 1000.0 : 0.0)
    for (uint32_t i = 0; i < n; i++) {
        const VkLatencyTimingsFrameReportNV *t = &rep[i];
        uint64_t from = t->inputSampleTimeUs ? t->inputSampleTimeUs : t->simStartTimeUs;
        if (!from || t->gpuRenderEndTimeUs <= from) continue;
        used++;
        total += SPAN(from, t->gpuRenderEndTimeUs), sim += SPAN(t->simStartTimeUs, t->simEndTimeUs);
        sub += SPAN(t->renderSubmitStartTimeUs, t->renderSubmitEndTimeUs), pres += SPAN(t->presentStartTimeUs, t->presentEndTimeUs);
        drv += SPAN(t->driverStartTimeUs, t->driverEndTimeUs), osq += SPAN(t->osRenderQueueStartTimeUs, t->osRenderQueueEndTimeUs);
        gpu += SPAN(t->gpuRenderStartTimeUs, t->gpuRenderEndTimeUs);
    }
#undef SPAN
    if (!used) {
        snprintf(out, cap, "; the driver reported no frame timings (%u reports)", n);
        return;
    }
    snprintf(out, cap, "; the driver's report over %u frames: pad read to GPU done %.2f ms (simulation %.2f, render "
             "submit %.2f, present call %.2f, driver %.2f, OS queue %.2f, GPU %.2f)", used, total / used, sim / used,
             sub / used, pres / used, drv / used, osq / used, gpu / used);
}

static void stats(uint64_t frame) {
    static uint64_t shown, next = 600;
    if (++shown < next) return;
    next = shown + (rt_trace & RT_TRACE_PERF ? 600 : 3600);
    uint64_t n = __atomic_exchange_n(&L.n_sleep, 0, __ATOMIC_RELAXED), ns = __atomic_exchange_n(&L.sleep_ns, 0, __ATOMIC_RELAXED);
    uint64_t in = __atomic_exchange_n(&L.n_input, 0, __ATOMIC_RELAXED), un = __atomic_exchange_n(&L.n_unheld, 0, __ATOMIC_RELAXED);
    uint64_t late = __atomic_exchange_n(&L.n_timeout, 0, __ATOMIC_RELAXED);
    uint32_t us = LOAD(&L.interval_us);
    char limit[96], drv[320] = "";
    if (us) snprintf(limit, sizeof limit, "the driver holds frames to %.3f ms", us / 1000.0);
    else snprintf(limit, sizeof limit, "the game's own limiter holds the frame limit");
    if (L.kind == LL_NV && L.timings) nv_report(drv, sizeof drv);
    else if (L.kind == LL_NV && !L.no_report_said++)
        snprintf(drv, sizeof drv, "; no report from the driver: revision %u of VK_NV_low_latency2 gives its frame timings "
                 "through another form of the call, which is not made (said once)", L.rev);
    rt_log("lowlat: frame %llu: %s; %llu frame starts held %.2f ms on average, %llu at the pad read and %llu at the "
           "image acquire; %llu not held, %llu sleeps not ended in time%s", (unsigned long long)frame, limit,
           (unsigned long long)n, n ? (double)ns / (double)n / 1e6 : 0.0, (unsigned long long)in,
           (unsigned long long)(n - in), (unsigned long long)un, (unsigned long long)late, drv);
}

void vk_lowlat_present_end(const VkLowlatPresent *p) {
    if (!p->id) return;
    if (L.kind == LL_NV) nv_marker(VK_LATENCY_MARKER_PRESENT_END_NV, p->id);
    STORE(&L.hosted_ns, kern_now_ns());
    stats(p->id);
}

int vk_lowlat_swapchain_wanted(void) { return L.kind == LL_NV && level_eff() > 0; }

void vk_lowlat_swapchain(VkSwapchainKHR sc, int made) {
    if (L.kind != LL_NV) return;
    plat_mutex_lock(&L.m);
    L.sc = sc;
    STORE(&L.mode, MODE_UNSET);
    STORE(&L.on, 0);
    if (sc) nv_mode_locked(mode_want());
    STORE(&L.attached, sc != VK_NULL_HANDLE);
    plat_mutex_unlock(&L.m);
    if (made && !sc && level_eff() > 0) say(0, "a frame generation's swapchain is in the window swapchain's place");
}

void vk_lowlat_swapchain_refused(int result) { give_up("no swapchain could be made with the latency mode", result); }

void vk_lowlat_fg_presents(int on) {
    if (L.kind == LL_NV) STORE(&L.fg_block, on != 0);
}

int vk_lowlat_on(void) { return LOAD(&L.on); }

int vk_lowlat_status(void) {
    if (L.kind == LL_NONE) return L.why;
    if (LOAD(&L.failed)) return VK_LOWLAT_FAILED;
    if (L.kind == LL_AMD) return VK_LOWLAT_AMD;
    return vk_fg_active() || LOAD(&L.fg_block) ? VK_LOWLAT_FG : VK_LOWLAT_NV;
}

void vk_lowlat_init(VkCtx *c) {
    int level = level_now();
    L.dev = c->device;
    L.kind = LL_NONE, L.why = VK_LOWLAT_NO_DRIVER, L.present_ids = 0, L.rev = 0, L.no_report_said = 0;
    STORE(&L.failed, 0);
    STORE(&L.pace_bad, 0);
    if (vk_dlssg_dpa()) {
        L.why = VK_LOWLAT_DLSSG;
        rt_log("lowlat: DLSS Frame Generation's interposer owns low latency in this run (NX2SARIA_REFLEX); the direct "
               "path is off (NX2SARIA_LOW_LATENCY=%d)", level);
        return;
    }
    if (c->has_low_latency2) {
        L.set_mode = (PFN_vkSetLatencySleepModeNV)vk_dpa(c->device, "vkSetLatencySleepModeNV");
        L.sleep = (PFN_vkLatencySleepNV)vk_dpa(c->device, "vkLatencySleepNV");
        L.marker = (PFN_vkSetLatencyMarkerNV)vk_dpa(c->device, "vkSetLatencyMarkerNV");
        L.rev = c->low_latency2_rev;
        L.timings = L.rev >= TIMINGS_REV ? (PFN_vkGetLatencyTimingsNV)vk_dpa(c->device, "vkGetLatencyTimingsNV") : NULL;
        VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
        if (L.set_mode && L.sleep && L.marker && (L.timings || L.rev < TIMINGS_REV) &&
            vkCreateSemaphore(c->device, &si, NULL, &L.sem) == VK_SUCCESS) {
            L.present_ids = c->has_present_id;
            L.kind = LL_NV;
            rt_log("lowlat: NVIDIA's low latency through VK_NV_low_latency2 revision %u (presents %s the frame's number): %s "
                   "(NX2SARIA_LOW_LATENCY=%d)%s", L.rev, L.present_ids ? "carry" : "do not carry",
                   level == 2 ? "on with boost" : level ? "on" : "off", level,
                   L.timings ? "" : "; the driver's frame timings are not asked for: this revision gives them through "
                                    "another form of the call, so the frame lines come without the driver's report");
            return;
        }
        rt_log("lowlat: VK_NV_low_latency2 is enabled but its entry points or the sleep's semaphore are missing");
    } else if (c->has_anti_lag) {
        L.antilag = (PFN_vkAntiLagUpdateAMD)vk_dpa(c->device, "vkAntiLagUpdateAMD");
        if (L.antilag) {
            L.kind = LL_AMD;
            rt_log("lowlat: AMD's Anti-Lag through VK_AMD_anti_lag: %s (NX2SARIA_LOW_LATENCY=%d)", level ? "on" : "off", level);
            return;
        }
        rt_log("lowlat: VK_AMD_anti_lag is enabled but its entry point is missing");
    }
    if (c->lists_low_latency) {
        L.why = VK_LOWLAT_RESTART;
        rt_log("lowlat: the driver has a low-latency mode; this run started with NX2SARIA_LOW_LATENCY off and made its "
               "device without it, so the setting takes effect at the next start");
        return;
    }
    rt_log("lowlat: this driver has neither VK_NV_low_latency2 nor VK_AMD_anti_lag: no low-latency mode%s",
           level ? " (NX2SARIA_LOW_LATENCY does nothing here)" : "");
}

void vk_lowlat_release(void) {
    L.kind = LL_NONE;
    STORE(&L.on, 0);
    STORE(&L.attached, 0);
    L.sc = VK_NULL_HANDLE;
    if (L.sem) vkDestroySemaphore(L.dev, L.sem, NULL);
    L.sem = VK_NULL_HANDLE;
}
