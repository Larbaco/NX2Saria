#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_ffx.h"
#include "platform/platform.h"

#define DYN_LEVELS 4
#define DYN_TARGET_MS 16.7
#define DYN_MARGIN 0.9
static const char *const DYN_MODE[DYN_LEVELS] = {"quality", "balanced", "performance", "ultra_performance"};

#define DYN_SHARES 12
#define DYN_SHARE_STEP 0.87
#define DYN_SHARE_SNAP 1.04
#define DYN_MIN_LO 0.33
#define DYN_MIN_HI 1.0
#define DYN_DOWN 1.08
#define DYN_UP 0.75
#define DYN_HOLD_MIN_NS 1000000000ull
#define DYN_UP_WAIT_NS 3000000000ull
#define DYN_UP_WAIT_MAX_NS 48000000000ull
#define DYN_UP_PROVEN_NS 5000000000ull
#define DYN_STEP_LINES 200
#define DYN_STATE_LINES 100

static struct {
    int want;
    int on;
    int plain;
    int base, low;
    int level;
    double share[DYN_SHARES];
    double from;
    double min;
    uint64_t held_from;
    uint64_t hold_ns;
    uint64_t up_from;
    uint64_t up_wait_ns;
    uint64_t upped_at;
    double avg_ns;
    double target_ms;
    double target_ns;
    double logged_ns;
    uint64_t last_busy;
    unsigned cfg_seen;
    char base_mode[32];
    const char *up;
    char why[320];
} D = {.cfg_seen = ~0u, .from = -2, .up = "DLSS"};

static uint64_t share_bits;

static void share_publish(void) {
    double s = D.on && D.plain && D.level != D.base ? D.share[D.level] : 0;
    uint64_t bits;
    memcpy(&bits, &s, sizeof bits);
    __atomic_store_n(&share_bits, bits, __ATOMIC_RELEASE);
}

static int mode_index(const char *m) {
    for (int i = 0; i < DYN_LEVELS; i++)
        if (m && !strcmp(m, DYN_MODE[i])) return i;
    return -1;
}

static int share_steps(double from, double min, double *out) {
    double floor = from < min ? from : min;
    int n = 0;
    out[0] = from;
    while (out[n] > floor + 0.001 && n < DYN_SHARES - 1) {
        double s = out[n] * DYN_SHARE_STEP;
        out[++n] = s < floor * DYN_SHARE_SNAP ? floor : s;
    }
    return n;
}

static const char *step_value(int level, char *buf, size_t n) {
    if (D.plain) snprintf(buf, n, "%.2f", D.share[level]);
    else snprintf(buf, n, "%s", DYN_MODE[level]);
    return buf;
}

static const char *upscaler_name(void) { return !vk_dlaa_fsr3() ? "DLSS" : vk_ffx_fsr4() ? "FSR 4" : "FSR 3.1"; }

static const char *step_name(int level, char *buf, size_t n) {
    char v[32];
    snprintf(buf, n, "%s %s", D.plain ? "scene share" : D.up, step_value(level, v, sizeof v));
    return buf;
}

static void state_log(const char *line) {
    static int said;
    if (said < DYN_STATE_LINES) said++, rt_log("%s", line);
}

static double target_now_ns(void) {
    if (D.target_ms >= 1) return D.target_ms * 1e6;
    double hz = frame_step_game_hz();
    return (hz > 0 ? 1000.0 / hz : DYN_TARGET_MS) * DYN_MARGIN * 1e6;
}

static const char *target_why(int digits, char *buf, size_t n) {
    double hz = frame_step_game_hz(), pct = (1 - DYN_MARGIN) * 100;
    if (D.target_ms >= 1) snprintf(buf, n, "as set");
    else if (hz > 0) snprintf(buf, n, "the frame limit's %.*f ms less %.0f %%", digits, 1000.0 / hz, pct);
    else snprintf(buf, n, "a 60 Hz frame's %.*f ms less %.0f %%, the frame limit being unknown or unlimited", digits,
                  DYN_TARGET_MS, pct);
    return buf;
}

static void dynres_read(void) {
    D.cfg_seen = cfg_version();
    const char *m = cfg_value("NX2SARIA_DLSS");
    D.want = cfg_bool("NX2SARIA_DYN_RES");
    double from = wide_dynres_base(), min = cfg_double("NX2SARIA_DYN_RES_MIN");
    min = min < DYN_MIN_LO ? DYN_MIN_LO : min > DYN_MIN_HI ? DYN_MIN_HI : min;
    int plain = from >= 0, base = plain ? 0 : mode_index(m);
    double steps[DYN_SHARES] = {0};
    int floor_at = plain && from > 0 ? share_steps(from, min, steps) : 0;
    D.target_ms = cfg_double("NX2SARIA_DYN_RES_TARGET");
    D.target_ns = target_now_ns();
    int step = cfg_int("NX2SARIA_DYN_RES_STEP");
    uint64_t hold = step > 0 ? (uint64_t)step * 1000000000ull / 60 : 0;
    if (hold < DYN_HOLD_MIN_NS) hold = DYN_HOLD_MIN_NS;
    int on = D.want && (plain ? floor_at > 0 : base >= 0);
    int moved = plain != D.plain || (plain ? from != D.from || min != D.min : strcmp(m, D.base_mode) != 0);
    char line[320], a[48], b[48], why[96];
    if (D.on && !on) {
        snprintf(line, sizeof line, "resolution: dynamic resolution off: %s as set", step_name(D.base, a, sizeof a));
        state_log(line);
    }
    D.plain = plain, D.from = from, D.min = min, D.up = upscaler_name();
    snprintf(D.base_mode, sizeof D.base_mode, "%s", m);
    if (on && (!D.on || moved)) {
        if (plain) {
            memcpy(D.share, steps, sizeof steps);
            D.base = D.level = 0, D.low = floor_at;
        } else {
            D.base = D.level = base;
            D.low = base > 2 ? base : 2;
        }
        D.hold_ns = hold;
        D.held_from = plat_now_ns(), D.avg_ns = 0, D.last_busy = vk_gpu_busy_all();
        D.up_from = 0, D.up_wait_ns = DYN_UP_WAIT_NS, D.upped_at = 0;
        D.logged_ns = D.target_ns;
        snprintf(line, sizeof line,
                 "resolution: dynamic resolution on (%s%s): target %.1f ms (%s), steps at least %.2f s apart and "
                 "%.0f s from a step down to a step up, %s down to %s",
                 plain ? "the scene share, no upscaler runs" : "the upscaler's mode, ", plain ? "" : D.up,
                 D.target_ns / 1e6,
                 target_why(1, why, sizeof why), hold / 1e9, DYN_UP_WAIT_NS / 1e9,
                 step_name(D.base, a, sizeof a), step_value(D.low, b, sizeof b));
        state_log(line);
        D.why[0] = 0;
    } else if (on && hold != D.hold_ns) {
        D.hold_ns = hold;
        snprintf(line, sizeof line, "resolution: dynamic resolution: steps at least %.2f s apart", hold / 1e9);
        state_log(line);
    }
    if (D.want && !on) {
        if (!plain)
            snprintf(line, sizeof line, "resolution: dynamic resolution needs an upscaling mode (quality, balanced, "
                     "performance or ultra performance) while %s runs; the mode (dlss) is %s", D.up, m);
        else if (from <= 0)
            snprintf(line, sizeof line, "resolution: dynamic resolution has no scene share to step: scene_share is 0 "
                     "(the game's own dynamic resolution) or this game version cannot set it");
        else
            snprintf(line, sizeof line, "resolution: dynamic resolution has no room: the scene share %.2f is at or "
                     "under dyn_res_min %.2f, the lowest it may go; the scene stays at %.2f", from, min, from);
        if (strcmp(line, D.why)) snprintf(D.why, sizeof D.why, "%s", line), state_log(line);
    } else if (!D.want) {
        D.why[0] = 0;
    }
    D.on = on;
    share_publish();
    perf_gpu_timing_want(on);
}

const char *dynres_override(void) { return D.on && !D.plain && D.level != D.base ? DYN_MODE[D.level] : NULL; }

double dynres_share(void) {
    uint64_t bits = __atomic_load_n(&share_bits, __ATOMIC_ACQUIRE);
    double s;
    memcpy(&s, &bits, sizeof s);
    return s;
}

void dynres_frame(void) {
    if (cfg_version() != D.cfg_seen || (D.want && wide_dynres_base() != D.from)) dynres_read();
    if (!D.on) return;
    if (!D.plain) D.up = upscaler_name();
    D.target_ns = target_now_ns();
    if (fabs(D.target_ns - D.logged_ns) > D.logged_ns * 0.01) {
        static int said;
        if (said < 20) {
            char why[96];
            said++, D.logged_ns = D.target_ns;
            rt_log("resolution: dynamic resolution: target %.2f ms (%s)", D.target_ns / 1e6,
                   target_why(2, why, sizeof why));
        }
    }
    uint64_t now = vk_gpu_busy_all(), d = now - D.last_busy;
    D.last_busy = now;
    if (!d || d > 1000000000ull) return;
    D.avg_ns = D.avg_ns > 0 ? D.avg_ns * 0.9 + (double)d * 0.1 : (double)d;
    char a[48], b[48];
    static int frames, said;
    if (++frames % 600 == 0 && said < 12) {
        said++;
        rt_log("resolution: dynamic resolution: GPU %.2f ms a frame, %s, target %.2f ms", D.avg_ns / 1e6,
               step_name(D.level, a, sizeof a), D.target_ns / 1e6);
    }
    uint64_t t = plat_now_ns();
    if (t - D.held_from < D.hold_ns) return;
    int to = D.level;
    if (D.avg_ns > D.target_ns * DYN_DOWN && D.level < D.low) to = D.level + 1;
    else if (D.avg_ns < D.target_ns * DYN_UP && D.level > D.base && t >= D.up_from) to = D.level - 1;
    if (to == D.level) {
        if (D.upped_at && t - D.upped_at >= DYN_UP_PROVEN_NS) D.upped_at = 0, D.up_wait_ns = DYN_UP_WAIT_NS;
        return;
    }
    int longer = 0;
    if (to > D.level) {
        if (D.upped_at && t - D.upped_at < DYN_UP_PROVEN_NS && D.up_wait_ns < DYN_UP_WAIT_MAX_NS)
            D.up_wait_ns *= 2, longer = 1;
        D.upped_at = 0, D.up_from = t + D.up_wait_ns;
    } else {
        D.upped_at = t;
    }
    static int steps;
    if (steps < DYN_STEP_LINES) {
        char wait[64] = "";
        if (longer) snprintf(wait, sizeof wait, " (the next step up waits %.0f s)", D.up_wait_ns / 1e9);
        rt_log("resolution: dynamic resolution: GPU %.2f ms a frame against %.2f ms: %s -> %s%s", D.avg_ns / 1e6,
               D.target_ns / 1e6, step_name(D.level, a, sizeof a), step_value(to, b, sizeof b),
               wait);
    } else if (steps == DYN_STEP_LINES) {
        rt_log("resolution: dynamic resolution: %d steps logged; the later ones are not", DYN_STEP_LINES);
    }
    steps++;
    D.level = to, D.held_from = t;
    share_publish();
}
