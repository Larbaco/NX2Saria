#include <stdio.h>

#include "cfg.h"
#include "host.h"
#include "mem_budget.h"
#include "platform/platform.h"
#include "vram_warn.h"

#define SAMPLE_S(n) ((int)((n) * (MEM_BUDGET_PERIOD_MS / 1000)))

static struct {
    PlatMutex lock;
    VramWarn w;
    int on;
    volatile int32_t due;
    uint64_t used_mb, limit_mb, peak_mb;
    uint32_t free_pm;
    VramWait wait;
    int lines;
} V = {.lock = PLAT_MUTEX_INIT};

static const char *const wait_names[VRAM_WAIT_REASONS] = {
    "", "the settings menu is open", "a movie plays", "the game is not in play yet (title screen or file menu)",
    "the game's pause or system menu is open", "a loading screen is up", "another notice is up"};

static const char *line_take(void) {
    if (V.lines >= VRAM_WARN_LINES) return NULL;
    return ++V.lines == VRAM_WARN_LINES ? "; the last such line of this session" : "";
}

void vram_warn_start(void) {
    V.on = cfg_bool("NX2SARIA_VRAM_WARNING");
    if (V.on)
        rt_log("vram_warn: on (vram_warning = 1): a notice on screen once the video memory was critical for %d of the "
               "last %d s, at most %d a session, %d s apart; vram_warning = 0 turns it off",
               SAMPLE_S(VRAM_WARN_HOLD), SAMPLE_S(VRAM_WARN_WINDOW), VRAM_WARN_MAX, VRAM_WARN_GAP_MS / 1000);
    else rt_log("vram_warn: off (vram_warning = 0): no notice on screen when the video memory runs out");
}

void vram_warn_sample(int critical, uint64_t used_mb, uint64_t limit_mb, uint32_t free_pm) {
    if (!V.on) return;
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&V.lock);
    if (critical) {
        V.used_mb = used_mb, V.limit_mb = limit_mb, V.free_pm = free_pm;
        if (!V.w.active || used_mb > V.peak_mb) V.peak_mb = used_mb;
    }
    int ev = vram_warn_step(&V.w, now, critical);
    __atomic_store_n(&V.due, V.w.due, __ATOMIC_RELEASE);
    const char *last;
    if (ev == VRAM_WARN_OVER) {
        V.wait = (VramWait){0};
        if ((last = line_take()))
            rt_log("vram_warn: video memory not critical for %d s (now %llu of %llu MiB in use): the stretch lasted "
                   "%llu s, %d s of them critical, at most %llu of %llu MiB in use; %d of %d notices shown this "
                   "session%s",
                   SAMPLE_S(VRAM_WARN_WINDOW), (unsigned long long)used_mb, (unsigned long long)limit_mb,
                   (unsigned long long)((now - V.w.since_ms) / 1000), SAMPLE_S(V.w.critical),
                   (unsigned long long)V.peak_mb, (unsigned long long)V.limit_mb, V.w.shown, VRAM_WARN_MAX, last);
    } else if (ev == VRAM_WARN_SPENT && (last = line_take())) {
        rt_log("vram_warn: video memory critical for %d of the last %d s again (%llu of %llu MiB in use, %.1f%% free): "
               "no notice, the session's %d were shown%s",
               SAMPLE_S(vram_warn_count(&V.w)), SAMPLE_S(VRAM_WARN_WINDOW), (unsigned long long)V.used_mb,
               (unsigned long long)V.limit_mb, V.free_pm / 10.0, VRAM_WARN_MAX, last);
    }
    plat_mutex_unlock(&V.lock);
}

int vram_warn_due(void) {
    if (!__atomic_load_n(&V.due, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock(&V.lock);
    int due = V.w.due;
    plat_mutex_unlock(&V.lock);
    return due;
}

void vram_warn_waits(int reason) {
    if (reason <= VRAM_WAIT_NONE || reason >= VRAM_WAIT_REASONS) return;
    plat_mutex_lock(&V.lock);
    const char *last;
    if (V.w.due && vram_warn_wait_step(&V.wait, reason) && (last = line_take())) {
        rt_log("vram_warn: notice %d of %d due (video memory critical for %d of the last %d s, %llu of %llu MiB in "
               "use), waits: %s%s",
               V.w.shown + 1, VRAM_WARN_MAX, SAMPLE_S(vram_warn_count(&V.w)), SAMPLE_S(VRAM_WARN_WINDOW),
               (unsigned long long)V.used_mb, (unsigned long long)V.limit_mb, wait_names[reason], last);
    }
    plat_mutex_unlock(&V.lock);
}

void vram_warn_shown(const char *suggested) {
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&V.lock);
    if (V.w.due) {
        vram_warn_mark(&V.w, now);
        V.wait = (VramWait){0};
        __atomic_store_n(&V.due, 0, __ATOMIC_RELEASE);
        rt_log("vram_warn: notice %d of %d shown: video memory critical for %d of the last %d s (%llu of %llu MiB in "
               "use, %.1f%% free); suggested: %s",
               V.w.shown, VRAM_WARN_MAX, SAMPLE_S(vram_warn_count(&V.w)), SAMPLE_S(VRAM_WARN_WINDOW),
               (unsigned long long)V.used_mb, (unsigned long long)V.limit_mb, V.free_pm / 10.0, suggested);
    }
    plat_mutex_unlock(&V.lock);
}

int vram_warn_shown_count(void) {
    plat_mutex_lock(&V.lock);
    int n = V.w.shown;
    plat_mutex_unlock(&V.lock);
    return n;
}
