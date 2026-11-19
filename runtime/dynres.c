// Dynamic resolution from the GPU's own timestamps: the scene's resolution follows how long the GPU takes per frame,
// so a heavy view renders fewer pixels and keeps the frame time, and a light one goes back to the player's choice. The
// HUD, the menus and the text are drawn at the picture's size, so only the 3D scene changes.
//
// NX2SARIA_DYN_RES=1 turns it on (off by default: it changes the picture's sharpness; the quality presets never turn it
// on). It steps one of two things, as widescreen.c wide_dynres_base says:
//   - while DLSS or FSR 3.1 runs (widescreen.c upscaler_runs), the upscaler's mode: quality, balanced, performance and
//     ultra performance, 2/3, 0.58, 1/2 and 1/3 of the output on each axis. NX2SARIA_DLSS is the sharpest step it
//     uses; the lowest is performance, or ultra performance when that is the player's own mode. It acts only while
//     NX2SARIA_DLSS is one of those upscaling modes. The step goes to vk_dlaa.c dlss_mode and to widescreen.c
//     mode_share as an override (dynres_override), which apply it as they apply the menu's choice.
//   - without one (the stand-in's edge-adaptive or linear filter enlarges the scene), the scene's share of the picture:
//     from the share in effect (NX2SARIA_SCENE_SHARE as the player or the quality preset sets it, 0.75 unset, within
//     the heap the start made) down to a floor, each step DYN_SHARE_STEP (0.87) of the one above on each axis, about
//     three quarters of the pixels; a step that would land within 4 % of the floor is the floor. The floor is the
//     lower of that share and NX2SARIA_DYN_RES_MIN (0.33 to 1, default 0.5), so it never starts below the floor and
//     never goes above the player's share; a share at or under dyn_res_min leaves it no room, and the log says so
//     once. The step goes to widescreen.c scene_share as a share (dynres_share). A changed scene_share or dyn_res_min
//     starts again at the new share.
// The player's settings themselves never change.
//
// While it runs it asks for GPU timestamps (perf_gpu_timing_want; vk_sync.c times every submission part) and reads
// their running total (vk_gpu_busy_all) once per presented frame. It averages the busy time and moves one step at a
// time: down when the average is over the target by 8 % (DYN_DOWN), up when it is under 75 % of it (DYN_UP). The gap
// between the two is wider than one step: a step up takes about 1 / 0.757 more pixels, so a GPU under 75 % of the
// target goes to about 99 % of it at the step above, under the 108 % that would send it down again.
//
// A size that changes every few frames is worse to look at than a steady one, so steps are held, by the clock:
//   - each step for NX2SARIA_DYN_RES_STEP 60ths of a second (default 45), never less than DYN_HOLD_MIN_NS, 1 s;
//   - after a step down, no step up for DYN_UP_WAIT_NS, 3 s, so a GPU time at the edge of the target does not climb
//     back the moment it dips;
//   - a step up that a step down undoes within DYN_UP_PROVEN_NS (5 s) doubles that wait, up to DYN_UP_WAIT_MAX_NS
//     (48 s): where one step's cost is larger than the gap between the two thresholds, the steps would otherwise swing
//     between the same two sizes every four seconds. A step up held 5 s brings the wait back to 3 s.
// The target is NX2SARIA_DYN_RES_TARGET milliseconds when that is set (1 to 100), else the frame time of the limit the
// game draws at (frame_rate.c frame_step_game_hz: 1000 / the limit, the monitor's rate for display, the drawn frames
// with frame generation), and 16.7 ms, a 60 Hz frame, while that rate is unknown or unlimited. A fixed 16.7 ms target
// let the GPU take up to 18 ms a frame at full size under a 120 or 144 fps limit, and a hold of 45 presented frames
// lasted 0.19 s at 240 fps.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define DYN_LEVELS 4
#define DYN_TARGET_MS 16.7   // the target while the game's rate is unknown or unlimited: a 60 Hz frame
static const char *const DYN_MODE[DYN_LEVELS] = {"quality", "balanced", "performance", "ultra_performance"};

#define DYN_SHARES 12          // the most scene-share steps: 1 down to 0.33 takes 9
#define DYN_SHARE_STEP 0.87    // a scene-share step's share of the one above, on each axis (0.757 of the pixels)
#define DYN_SHARE_SNAP 1.04    // a step that would land within 4 % above the floor is the floor
#define DYN_MIN_LO 0.33        // NX2SARIA_DYN_RES_MIN's range
#define DYN_MIN_HI 1.0
#define DYN_DOWN 1.08          // a step down while the GPU's average is over the target by 8 %
#define DYN_UP 0.75            // a step up while it is under 75 % of the target
#define DYN_HOLD_MIN_NS 1000000000ull        // the least time between two steps, whatever dyn_res_step says
#define DYN_UP_WAIT_NS 3000000000ull         // the least time from a step down to a step up
#define DYN_UP_WAIT_MAX_NS 48000000000ull    // the most that wait doubles to
#define DYN_UP_PROVEN_NS 5000000000ull       // a step up undone sooner doubles the wait; one held this long resets it
#define DYN_STEP_LINES 200     // step lines a run, then one line that says the rest are not logged
#define DYN_STATE_LINES 100    // on, off and why-not lines a run

static struct {
    int want;             // NX2SARIA_DYN_RES
    int on;               // dynamic resolution is acting now
    int plain;            // the path it reads now: 1 the scene share (no upscaler runs), 0 the upscaler's mode
    int base, low;        // the sharpest step used and the lowest (DYN_MODE indexes, or share[] indexes: 0 and the floor)
    int level;            // the step in use
    double share[DYN_SHARES];   // the scene-share path's steps: share[0] the player's share, share[low] the floor
    double from;          // widescreen.c wide_dynres_base as last read: the share stepped from, -1 the upscaler's mode
    double min;           // NX2SARIA_DYN_RES_MIN as last read, within its range
    uint64_t held_from;   // plat_now_ns when the step in use was taken
    uint64_t hold_ns;     // how long to hold a step
    uint64_t up_from;     // no step up before this plat_now_ns (a step down was taken DYN_UP_WAIT_NS or more before)
    uint64_t up_wait_ns;  // the wait after a step down before a step up: DYN_UP_WAIT_NS, doubled by undone steps up
    uint64_t upped_at;    // when the last step up was taken, until it is held DYN_UP_PROVEN_NS; 0 for none
    double avg_ns;        // moving average of the GPU's busy time per frame
    double target_ms;     // NX2SARIA_DYN_RES_TARGET; under 1: the frame time of the game's rate
    double target_ns;     // the target in use
    double logged_ns;     // the target the log last named (the start line's, or dynres_frame's last line)
    uint64_t last_busy;   // vk_gpu_busy_all at the previous presented frame
    unsigned cfg_seen;    // cfg_version the settings were read at
    char base_mode[32];
    char why[320];        // the last line that said why it does not act, so each reason is said once
} D = {.cfg_seen = ~0u, .from = -2};

// The scene share in use, for widescreen.c scene_share, which runs on the game's thread in its present while
// dynres_frame runs on the submitting or presenting thread: one value, the double's bits, stored whole after a step or
// a (re)start, so a reader never sees a share half-way through a restart. 0 while not stepping the scene share.
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

// The scene-share path's steps from the share in effect down to the floor (the lower of it and dyn_res_min) into out;
// the floor's index, 0 when there is no room.
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

// A step as the log names it: its value alone ("0.75", "quality"), or with what it is ("scene share 0.75").
static const char *step_value(int level, char *buf, size_t n) {
    if (D.plain) snprintf(buf, n, "%.2f", D.share[level]);
    else snprintf(buf, n, "%s", DYN_MODE[level]);
    return buf;
}

static const char *step_name(int level, char *buf, size_t n) {
    char v[32];
    snprintf(buf, n, "%s %s", D.plain ? "scene share" : "DLSS", step_value(level, v, sizeof v));
    return buf;
}

static void state_log(const char *line) {
    static int said;
    if (said < DYN_STATE_LINES) said++, rt_log("%s", line);
}

// The target in ns: the setting's, else the frame time of the rate the game draws at under the limit.
static double target_now_ns(void) {
    if (D.target_ms >= 1) return D.target_ms * 1e6;
    double hz = frame_step_game_hz();
    return (hz > 0 ? 1000.0 / hz : DYN_TARGET_MS) * 1e6;
}

// The settings, again whenever one changed (the menu switches them live) or the share in effect or the path moved.
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
    char line[320], a[48], b[48];
    if (D.on && !on) {   // a run that goes on with other settings logs its new start line below instead
        snprintf(line, sizeof line, "resolution: dynamic resolution off: %s as set", step_name(D.base, a, sizeof a));
        state_log(line);
    }
    D.plain = plain, D.from = from, D.min = min;
    snprintf(D.base_mode, sizeof D.base_mode, "%s", m);
    if (on && (!D.on || moved)) {   // (re)start at the player's step
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
                 "resolution: dynamic resolution on (%s): target %.1f ms (%s), steps at least %.2f s apart and %.0f s "
                 "from a step down to a step up, %s down to %s",
                 plain ? "the scene share, no upscaler runs" : "the upscaler's mode", D.target_ns / 1e6,
                 D.target_ms >= 1 ? "as set" : "the frame limit's", hold / 1e9, DYN_UP_WAIT_NS / 1e9,
                 step_name(D.base, a, sizeof a), step_value(D.low, b, sizeof b));
        state_log(line);
        D.why[0] = 0;
    } else if (on && hold != D.hold_ns) {
        D.hold_ns = hold;
        snprintf(line, sizeof line, "resolution: dynamic resolution: steps at least %.2f s apart", hold / 1e9);
        state_log(line);
    }
    if (D.want && !on) {   // why it does not act, once for each reason
        if (!plain)
            snprintf(line, sizeof line, "resolution: dynamic resolution needs a DLSS upscaling mode (quality, balanced, "
                     "performance or ultra performance) while DLSS or FSR runs; DLSS is %s", m);
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

// Once per presented frame, before vk_dlaa.c reads the mode.
void dynres_frame(void) {
    if (cfg_version() != D.cfg_seen || (D.want && wide_dynres_base() != D.from)) dynres_read();
    if (!D.on) return;
    // The limit, the monitor's rate and frame generation move the target without a dyn_res setting changing, so it is
    // read every frame. A move of more than 1 % from the target the log last named is logged, whether it came from the
    // frame, from the menu (dynres_read takes a changed limit or dyn_res_target already) or since the start line: a
    // measured refresh rate wanders a little. At most 20 lines a run.
    D.target_ns = target_now_ns();
    if (fabs(D.target_ns - D.logged_ns) > D.logged_ns * 0.01) {
        static int said;
        if (said < 20) {
            said++, D.logged_ns = D.target_ns;
            rt_log("resolution: dynamic resolution: target %.2f ms (%s)", D.target_ns / 1e6,
                   D.target_ms >= 1 ? "as set" : "the frame limit's");
        }
    }
    uint64_t now = vk_gpu_busy_all(), d = now - D.last_busy;
    D.last_busy = now;
    if (!d || d > 1000000000ull) return;   // no timing this frame yet, or a stall that says nothing about the scene
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
    if (to > D.level) {   // down: hold off the next step up, longer when this undoes a step up just taken
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
