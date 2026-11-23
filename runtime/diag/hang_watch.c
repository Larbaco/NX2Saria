#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "logq.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define HW_REPORTS 3

typedef struct {
    uint64_t now_ns, frames, issued, done;
    int judge_present;
} HwBeat;

typedef struct {
    uint64_t limit_ns;
    uint64_t frames, frames_at;
    uint64_t done, done_at;
    int present_told, gpu_told;
    int reports;
} HangWatch;

enum { HW_NONE, HW_PRESENT, HW_GPU };

static int hw_step(HangWatch *w, const HwBeat *b) {
    if (b->frames != w->frames || !b->judge_present) {
        w->frames = b->frames;
        w->frames_at = b->now_ns;
        w->present_told = 0;
    }
    if (b->done != w->done || b->issued <= b->done) {
        w->done = b->done;
        w->done_at = b->now_ns;
        w->gpu_told = 0;
    }
    if (w->reports >= HW_REPORTS) return HW_NONE;
    int gpu = !w->gpu_told && b->issued > b->done && b->now_ns - w->done_at >= w->limit_ns;
    int present = !w->present_told && b->judge_present && b->now_ns - w->frames_at >= w->limit_ns;
    if (!gpu && !present) return HW_NONE;
    w->reports++;
    w->present_told = 1;
    if (gpu) w->gpu_told = 1;
    return gpu ? HW_GPU : HW_PRESENT;
}

static void hw_read(HwBeat *b) {
    b->now_ns = plat_now_ns();
    b->frames = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    vk_sync_progress(&b->issued, &b->done);
    b->judge_present = __atomic_load_n(&rt_guest_start_ns, __ATOMIC_ACQUIRE) && gfx_window_in_front() &&
                       !rt_exit_started();
}

static void hw_report(int kind, const HwBeat *b, const HangWatch *w) {
    int64_t world = frame_step_world_us();
    uint64_t since = b->now_ns - (kind == HW_GPU ? w->done_at : w->frames_at);
    rt_log("hang watch: %s for %.0f s (frames shown %llu; GPU submissions issued up to %llu, done up to %llu; window "
           "%s; world clock %.1f s); report %d of at most %d, the game goes on",
           kind == HW_GPU ? "GPU submissions not done" : "no frame shown with the window in front", since / 1e9,
           (unsigned long long)b->frames, (unsigned long long)b->issued, (unsigned long long)b->done,
           gfx_window_in_front() ? "in front" : "not in front", world >= 0 ? world / 1e6 : -1.0, w->reports, HW_REPORTS);
    GfxLockHolders h;
    gfx_lock_holders(&h);
    rt_log("hang watch: the present lock is held by thread %u (depth %u), the queue lock by thread %u (depth %u), the "
           "window start lock by thread %u (0: free)", h.present, h.present_depth, h.queue, h.queue_depth, h.start);
    kern_wait_report();
    logq_flush();
}

static void hang_watch_main(void *arg) {
    HangWatch w = {.limit_ns = (uint64_t)(uintptr_t)arg * 1000000000ull};
    HwBeat b;
    hw_read(&b);
    w.frames = b.frames, w.frames_at = b.now_ns, w.done = b.done, w.done_at = b.now_ns;
    while (w.reports < HW_REPORTS) {
        plat_sleep_ms(1000);
        hw_read(&b);
        int kind = hw_step(&w, &b);
        if (kind) hw_report(kind, &b, &w);
    }
}

void hang_watch_start(void) {
    int s = cfg_int("NX2SARIA_HANG_WATCH_S");
    if (s <= 0) return;
    if (!plat_thread_spawn(hang_watch_main, (void *)(uintptr_t)s, "hang watch", 0))
        rt_log("hang watch: its thread did not start");
}
