# needs: clang
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

DESCRIPTION = """Dynamic resolution's target and step hold (runtime/pacing/dynres.c): builds dynres.c, cut by its text, with stand-ins for
the settings, the GPU's busy time, the clock and the game's rate under the limit (frame_rate.c frame_step_game_hz),
and checks that

  - the default target is 90 % of the frame time of the game's rate (1000 / the limit: 30, 40, 60, 144 and 240 fps;
    15.0 ms at 60, 22.5 ms at 40), of 16.7 ms while that rate is unknown or unlimited, and the start line names the
    frame time and the margin; a GPU at 98 % of a 60 fps frame steps down, at 96 % it holds; a target that is set wins
    at any rate, exactly, without the margin;
  - a limit changed while it runs moves the target on the next frame, with one log line, also when the menu changed
    it (or dyn_res_target) and dynres_read took the new target first; a refresh rate wandering under 1 % after the
    start line logs nothing, and after dyn_res off and on a move back to the earlier run's last target is logged;
  - each step is held for dyn_res_step 60ths of a second of the clock at 60 and 240 fps (90: 1.5 s), never less than
    1 s (60, 45 and 8 all hold 1 s), down while the GPU is over the target and back up once it is under three quarters
    of it, the first step up no sooner than 3 s after a step down;
  - with an upscaler it steps the upscaler's mode, and its lines name the one that runs (vk_dlaa.c vk_dlaa_fsr3):
    "the upscaler's mode, FSR 3.1" and "FSR 3.1 quality down to performance" at the start, "FSR 3.1 balanced ->
    performance" at a step, DLSS and FSR 4 by their own names, and the name of the one that runs now when FSR 4
    falls back to FSR 3.1 without a settings change; with DLAA or FSR at native size the line says which runs and that
    the mode is not an upscaling one;
  - without an upscaler (widescreen.c wide_dynres_base gives a share) it steps the scene share: from the share in
    effect down by 0.87 a step to the floor, the lower of that share and dyn_res_min, and back up when the GPU is
    light; never above the share, never below the floor, and not at all when the share is at or under dyn_res_min
    (0.4 against 0.5), with one line that says so; a changed scene_share or dyn_res_min starts again at the share;
  - a GPU time at the edge of the target (alternating just over and under 108 %, or landing at the target after a
    step down) does not swing between steps, and where one step changes the GPU time by more than the gap between the
    thresholds (a step up lands over 108 % again) the wait before a step up doubles each time, so ten minutes hold
    a bounded number of steps, on both paths.

No game files, no GPU. Usage: python tests/dynres_check.py [--cc <clang>]
"""

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

HARNESS = r'''
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned ver = 1;
static const char *dlss = "quality", *dyn = "1";
static double set_target_ms;   // NX2SARIA_DYN_RES_TARGET as cfg_double reads it (0: the default)
static int set_step = 60;      // NX2SARIA_DYN_RES_STEP
static double set_min = 0.5;   // NX2SARIA_DYN_RES_MIN
static double wbase = -1;      // widescreen.c wide_dynres_base: -1 an upscaler runs, else the scene share in effect
static double game_hz = 60;
static uint64_t busy = 1, now_ns = 1000000000ull;
static int timing, logs;
static char last_log[256];
static unsigned cfg_version(void) { return ver; }
static const char *cfg_value(const char *k) {
    return !strcmp(k, "NX2SARIA_DLSS") ? dlss : !strcmp(k, "NX2SARIA_DYN_RES") ? dyn : "0";
}
static int cfg_bool(const char *k) { return strcmp(cfg_value(k), "0") != 0; }
static double cfg_double(const char *k) {
    return !strcmp(k, "NX2SARIA_DYN_RES_TARGET") ? set_target_ms : !strcmp(k, "NX2SARIA_DYN_RES_MIN") ? set_min : 0;
}
static double wide_dynres_base(void) { return wbase; }
static int fsr;
static int vk_dlaa_fsr3(void) { return fsr; }
static int fsr4;
static int vk_ffx_fsr4(void) { return fsr4; }
static int cfg_int(const char *k) { return !strcmp(k, "NX2SARIA_DYN_RES_STEP") ? set_step : 0; }
static double frame_step_game_hz(void) { return game_hz; }
static uint64_t vk_gpu_busy_all(void) { return busy; }
static void perf_gpu_timing_want(int on) { timing = on; }
static uint64_t plat_now_ns(void) { return now_ns; }
static void rt_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, a);
    va_end(a);
    logs++;
}
%s
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) <= (b) * 1e-6)

// One presented frame at hz with the GPU busy gpu_ms; returns the step in use (0 quality .. 3 ultra performance).
static int frame(double hz, double gpu_ms) {
    now_ns += (uint64_t)(1e9 / hz);
    busy += (uint64_t)(gpu_ms * 1e6);
    dynres_frame();
    return D.level;
}

static void restart(void) {
    dyn = "0", ver++, frame(60, 1);
    dyn = "1", ver++, frame(60, 1);
}

// Frames at hz with the GPU busy gpu_ms until the step changes (at most limit frames): their count, -1 for none.
static int frames_to_change(double hz, double gpu_ms, int limit) {
    int from = D.level;
    for (int k = 1; k <= limit; k++)
        if (frame(hz, gpu_ms) != from) return k;
    return -1;
}

// The scene share in use on the plain path: dynamic resolution's step, else the share it steps from.
static double share_now(void) {
    double d = dynres_share();
    return d > 0 ? d : wbase;
}

// Seconds of frames at hz with the GPU time model(frame index) in ms: how many times the step changed. The lowest and
// highest share seen go to *lo and *hi on the plain path, the fewest frames a step was held to *held.
static int run(double hz, double secs, double (*model)(int), double *lo, double *hi, int *held) {
    int n = (int)(secs * hz), from = D.level, switches = 0, since = 0;
    if (lo) *lo = share_now(), *hi = share_now();
    if (held) *held = 1 << 30;
    for (int k = 0; k < n; k++) {
        since++;
        if (frame(hz, model(k)) != from) {
            switches++, from = D.level;
            if (held && switches > 1 && since < *held) *held = since;
            since = 0;
        }
        if (lo) {
            double s = share_now();
            if (s < *lo) *lo = s;
            if (s > *hi) *hi = s;
        }
    }
    return switches;
}

// GPU models against the 60 fps target (15 ms: 90 %% of the 16.67 ms frame). The plain path's time follows the scene's
// pixels.
static double T60 = 900.0 / 60, load;
static double heavy(int k) { (void)k; return T60 * 2; }
static double light(int k) { (void)k; return T60 * 0.3; }
static double pixels(int k) { (void)k; double r = share_now() / wbase; return T60 * load * r * r; }
// just over and just under 108 %% of the target, frame by frame, at the share stepped from
static double edge(int k) { double r = share_now() / wbase; return T60 * (k & 1 ? 1.09 : 1.07) * r * r; }
// a step costs more than the gap between the thresholds: 1.1 of the target at the top, 0.6 of that a step lower
static double steep(int k) { (void)k; return T60 * 1.1 * pow(0.6, D.level - D.base); }

static void plain_start(double base, double min) {
    wbase = base, set_min = min, game_hz = 60, set_step = 60;
    restart();
}

int main(void) {
    // The default target follows the game's rate, 10 %% under its frame time.
    static const struct { double hz; const char *line; } rates[] = {
        {60, "target 15.0 ms (the frame limit's 16.7 ms less 10 %%)"},
        {40, "target 22.5 ms (the frame limit's 25.0 ms less 10 %%)"},
        {30, "target 30.0 ms (the frame limit's 33.3 ms less 10 %%)"},
        {144, "(the frame limit's 6.9 ms less 10 %%)"},
        {240, "(the frame limit's 4.2 ms less 10 %%)"},
        {0, "target 15.0 ms (a 60 Hz frame's 16.7 ms less 10 %%, the frame limit being unknown or unlimited)"}};
    for (int i = 0; i < 6; i++) {
        game_hz = rates[i].hz;
        restart();
        double want = 0.9 * (rates[i].hz > 0 ? 1e9 / rates[i].hz : 16.7e6);
        CHECK(NEAR(D.target_ns, want), "at %%.0f fps the target is %%.3f ms, want %%.3f", rates[i].hz,
              D.target_ns / 1e6, want / 1e6);
        CHECK(strstr(last_log, "dynamic resolution on") && strstr(last_log, rates[i].line), "start line: %%s",
              last_log);
    }
    CHECK(timing, "GPU timestamps asked for while it runs");
    // The margin: at 60 fps a GPU at 96 %% of the frame time holds (under 108 %% of 15 ms, 16.2 ms), at 98 %% it steps
    // down before frames are lost.
    game_hz = 60;
    restart();
    int n = frames_to_change(60, 1000.0 / 60 * 0.96, 600);
    CHECK(n < 0 && D.level == 0, "96 %%%% of a 60 fps frame moved the step to %%d", D.level);
    n = frames_to_change(60, 1000.0 / 60 * 0.98, 600);
    CHECK(n > 0 && D.level == 1, "98 %%%% of a 60 fps frame: step %%d after %%d frames", D.level, n);
    // A target that is set wins, exactly: no margin.
    set_target_ms = 10, game_hz = 240;
    restart();
    frame(240, 1);
    CHECK(NEAR(D.target_ns, 10e6) && strstr(last_log, "target 10.0 ms (as set)") && !strstr(last_log, "less"),
          "a set target: %%.3f ms (%%s)", D.target_ns / 1e6, last_log);
    set_target_ms = 15, game_hz = 60;
    restart();
    CHECK(NEAR(D.target_ns, 15e6) && strstr(last_log, "target 15.0 ms (as set)"), "a set 15 ms at 60 fps: %%.3f ms "
          "(%%s)", D.target_ns / 1e6, last_log);
    set_target_ms = 0;
    // A limit changed while it runs: the next frame's target, one line.
    game_hz = 60;
    restart();
    int before = logs;
    game_hz = 144;
    frame(144, 1);
    CHECK(NEAR(D.target_ns, 0.9e9 / 144) && logs == before + 1
          && strstr(last_log, "target 6.25 ms (the frame limit's 6.94 ms less 10 %%)"), "live limit: %%.3f ms, %%d "
          "lines (%%s)", D.target_ns / 1e6, logs - before, last_log);
    before = logs;
    for (int k = 0; k < 100; k++) frame(144, 1);
    CHECK(logs == before, "a steady limit logged %%d lines", logs - before);
    // A limit or a target changed in the menu (a new cfg_version, so dynres_read takes the new target first): one line.
    before = logs;
    game_hz = 120, ver++;
    frame(120, 1);
    CHECK(NEAR(D.target_ns, 0.9e9 / 120) && logs == before + 1
          && strstr(last_log, "7.50 ms (the frame limit's 8.33 ms less 10 %%)"),
          "the menu's limit: %%.3f ms, %%d lines (%%s)", D.target_ns / 1e6, logs - before, last_log);
    before = logs;
    set_target_ms = 20, ver++;
    frame(120, 1);
    CHECK(NEAR(D.target_ns, 20e6) && logs == before + 1 && strstr(last_log, "20.00 ms (as set)"),
          "the menu's target: %%.3f ms, %%d lines (%%s)", D.target_ns / 1e6, logs - before, last_log);
    set_target_ms = 0;
    // A refresh rate wandering under 1 %% after the start line: no line. Off and on again, then back to the target the
    // earlier run logged last: one line, measured from the new start line.
    game_hz = 60;
    restart();
    before = logs;
    game_hz = 59.9;
    frame(59.9, 1);
    CHECK(logs == before, "a wander of 0.17 %%%% after the start logged %%d lines (%%s)", logs - before, last_log);
    game_hz = 144;
    frame(144, 1);
    dyn = "0", ver++, frame(144, 1);
    game_hz = 60;
    dyn = "1", ver++, frame(60, 1);
    before = logs;
    game_hz = 144;
    frame(144, 1);
    CHECK(logs == before + 1 && strstr(last_log, "6.25"), "back to the earlier run's last target: %%d lines (%%s)",
          logs - before, last_log);
    // Each step is held for a time, whatever the frame rate, and never less than 1 s.
    static const struct { double hz; int step; double hold_s; } holds[] = {{60, 60, 1.0}, {240, 45, 1.0},
                                                                           {240, 90, 1.5}, {144, 8, 1.0}};
    for (int i = 0; i < 4; i++) {
        double hz = holds[i].hz;
        game_hz = hz, set_step = holds[i].step;
        restart();
        double target = 900 / hz;   // 90 %% of the frame time
        int want = (int)ceil(holds[i].hold_s * hz);
        int n = frames_to_change(hz, target * 2, 100000);
        CHECK(D.level == 1 && n >= want - 1 && n <= want + 1, "%%.0f fps, step %%d: down after %%d frames, want %%d",
              hz, holds[i].step, n, want);
        n = frames_to_change(hz, target * 2, 100000);
        CHECK(D.level == 2 && n >= want - 1 && n <= want + 1, "%%.0f fps: the next step after %%d frames, want %%d", hz,
              n, want);
        n = frames_to_change(hz, target * 2, want * 3);
        CHECK(n < 0 && D.level == 2, "%%.0f fps: below performance (%%d)", hz, D.level);
        n = frames_to_change(hz, target * 0.5, 100000);   // the average needs a few frames to fall under 75 %%
        CHECK(D.level == 1 && n >= 1 && n <= want + 40, "%%.0f fps: up after %%d frames", hz, n);
        n = frames_to_change(hz, target * 0.5, 100000);
        CHECK(D.level == 0 && n >= want - 1 && n <= want + 1, "%%.0f fps: back to quality after %%d frames, want %%d",
              hz, n, want);
        n = frames_to_change(hz, target * 0.9, want * 3);
        CHECK(n < 0, "%%.0f fps: a GPU between three quarters of the target and 8 %%%% over it moved the step", hz);
    }
    set_step = 60;
    // The mode path: the first step up waits 3 s after a step down, however light the GPU.
    game_hz = 60;
    restart();
    frames_to_change(60, T60 * 2, 100000);
    n = frames_to_change(60, T60 * 0.3, 100000);
    CHECK(D.level == 0 && n >= 180, "the mode path: up %%d frames after a step down, want 180 (3 s) or more", n);

    // ---- The plain path: no upscaler runs, widescreen.c gives the scene share in effect.
    dlss = "dlaa";   // any DLSS setting: it is not read on this path
    plain_start(0.75, 0.5);
    static const double want75[] = {0.75, 0.6525, 0.567675, 0.5};
    CHECK(D.on && D.plain && D.low == 3, "0.75 down to 0.5: on %%d, plain %%d, %%d steps", D.on, D.plain, D.low);
    for (int i = 0; i < 4 && i <= D.low; i++)
        CHECK(NEAR(D.share[i], want75[i]), "step %%d is %%.4f, want %%.4f", i, D.share[i], want75[i]);
    CHECK(strstr(last_log, "dynamic resolution on (the scene share, no upscaler runs)")
          && strstr(last_log, "scene share 0.75 down to 0.50") && strstr(last_log, "1.00 s apart"), "start line: %%s",
          last_log);
    CHECK(timing && dynres_override() == NULL && dynres_share() == 0, "at the start: the share in effect");
    double lo, hi;
    int held;
    int sw = run(60, 20, heavy, &lo, &hi, &held);
    CHECK(sw == 3 && NEAR(lo, 0.5) && NEAR(hi, 0.75) && NEAR(dynres_share(), 0.5) && held >= 59,
          "heavy: %%d steps, %%.3f..%%.3f, now %%.3f, held %%d frames", sw, lo, hi, dynres_share(), held);
    CHECK(strstr(last_log, "scene share 0.57 -> 0.50"), "the step line: %%s", last_log);
    sw = run(60, 30, light, &lo, &hi, &held);
    CHECK(sw == 3 && NEAR(lo, 0.5) && NEAR(hi, 0.75) && dynres_share() == 0 && held >= 59,
          "light: %%d steps, %%.3f..%%.3f, now %%.3f, held %%d frames", sw, lo, hi, dynres_share(), held);
    CHECK(strstr(last_log, "scene share 0.65 -> 0.75"), "the step line up: %%s", last_log);
    // The first step up comes 3 s after the last step down, however light the GPU.
    frames_to_change(60, T60 * 2, 100000);
    n = frames_to_change(60, T60 * 0.3, 100000);
    CHECK(n >= 180 && dynres_share() == 0, "up %%d frames after a step down, want 180 (3 s) or more", n);
    // dyn_res_step 8 still holds each step 1 s, and 90 says its longer hold.
    set_step = 90, ver++;
    frame(60, T60);
    CHECK(strstr(last_log, "steps at least 1.50 s apart"), "dyn_res_step 90: %%s", last_log);
    set_step = 8, ver++;
    frame(60, T60);
    CHECK(strstr(last_log, "steps at least 1.00 s apart"), "dyn_res_step 8: %%s", last_log);
    sw = run(60, 10, heavy, NULL, NULL, &held);
    CHECK(sw == 3 && held >= 59, "dyn_res_step 8: %%d steps, held %%d frames", sw, held);
    set_step = 60, ver++;

    // The floor is the lower of the share and dyn_res_min.
    plain_start(1.0, 0.33);
    CHECK(D.on && D.low == 8 && NEAR(D.share[D.low], 0.33), "1 down to 0.33: %%d steps to %%.3f", D.low,
          D.share[D.low]);
    for (int i = 1; i <= D.low; i++)
        CHECK(D.share[i] < D.share[i - 1] && D.share[i] >= 0.33 && (i == D.low || NEAR(D.share[i], D.share[i - 1] * 0.87)),
              "step %%d: %%.4f after %%.4f", i, D.share[i], D.share[i - 1]);
    sw = run(60, 30, heavy, &lo, &hi, NULL);
    CHECK(sw == 8 && NEAR(lo, 0.33) && NEAR(hi, 1.0), "1 down to 0.33 under load: %%d steps, %%.3f..%%.3f", sw, lo, hi);
    plain_start(0.67, 0.6);
    CHECK(D.on && D.low == 1 && NEAR(D.share[1], 0.6), "0.67 down to 0.6: %%d steps to %%.3f", D.low, D.share[D.low]);
    // A share at or under dyn_res_min (the Low preset's 0.4 on an integrated GPU, against 0.5): no room, one line.
    plain_start(0.4, 0.5);
    CHECK(!D.on && !timing && strstr(last_log, "has no room: the scene share 0.40 is at or under dyn_res_min 0.50"),
          "0.4 against 0.5: on %%d (%%s)", D.on, last_log);
    before = logs;
    sw = run(60, 10, heavy, &lo, &hi, NULL);
    ver++, frame(60, T60), ver++, frame(60, T60);
    CHECK(sw == 0 && NEAR(lo, 0.4) && NEAR(hi, 0.4) && dynres_share() == 0 && logs == before,
          "0.4 against 0.5 under load: %%d steps, %%.3f..%%.3f, %%d more lines", sw, lo, hi, logs - before);
    plain_start(0.5, 0.5);
    CHECK(!D.on && strstr(last_log, "has no room"), "0.5 against 0.5: on %%d (%%s)", D.on, last_log);
    plain_start(0, 0.5);
    CHECK(!D.on && strstr(last_log, "no scene share to step"), "a share of 0: on %%d (%%s)", D.on, last_log);

    // A changed scene_share or dyn_res_min starts again at the share in effect.
    plain_start(0.75, 0.5);
    run(60, 5, heavy, NULL, NULL, NULL);
    CHECK(D.level >= 2, "heavy for 5 s: step %%d", D.level);
    wbase = 0.67, ver++;
    frame(60, T60 * 2);
    CHECK(D.on && D.level == 0 && NEAR(D.share[0], 0.67) && dynres_share() == 0
          && strstr(last_log, "scene share 0.67 down to 0.50"), "scene_share 0.67: step %%d (%%s)", D.level, last_log);
    run(60, 3, heavy, NULL, NULL, NULL);
    CHECK(D.level >= 1, "heavy again: step %%d", D.level);
    set_min = 0.4, ver++;
    frame(60, T60 * 2);
    CHECK(D.level == 0 && strstr(last_log, "scene share 0.67 down to 0.40"), "dyn_res_min 0.4: step %%d (%%s)",
          D.level, last_log);
    run(60, 3, heavy, NULL, NULL, NULL);
    wbase = 0.75;   // the share in effect moved without a settings change seen first (the heap's limit, the path)
    frame(60, T60 * 2);
    CHECK(D.level == 0 && strstr(last_log, "scene share 0.75 down to 0.40"), "a moved share: step %%d (%%s)", D.level,
          last_log);
    // An upscaler that runs takes the mode path, which the share leaves alone.
    wbase = -1, dlss = "quality", ver++;
    frame(60, T60);
    CHECK(D.on && !D.plain && dynres_share() == 0 && strstr(last_log, "(the upscaler's mode, DLSS)")
          && strstr(last_log, "DLSS quality down to performance"), "an upscaler: %%s", last_log);
    run(60, 5, heavy, NULL, NULL, NULL);
    CHECK(dynres_share() == 0 && dynres_override() && !strcmp(dynres_override(), "performance"),
          "an upscaler under load: share %%.3f, mode %%s", dynres_share(), dynres_override());
    CHECK(strstr(last_log, "DLSS balanced -> performance"), "DLSS's step line: %%s", last_log);
    fsr = 1;
    restart();
    CHECK(D.on && !D.plain && strstr(last_log, "(the upscaler's mode, FSR 3.1)")
          && strstr(last_log, "FSR 3.1 quality down to performance") && !strstr(last_log, "DLSS"),
          "FSR 3.1's start line: %%s", last_log);
    run(60, 5, heavy, NULL, NULL, NULL);
    CHECK(dynres_override() && !strcmp(dynres_override(), "performance")
          && strstr(last_log, "FSR 3.1 balanced -> performance") && !strstr(last_log, "DLSS"),
          "FSR 3.1's step line: %%s", last_log);
    fsr4 = 1;
    restart();
    CHECK(strstr(last_log, "(the upscaler's mode, FSR 4)") && strstr(last_log, "FSR 4 quality down to performance"),
          "FSR 4's start line: %%s", last_log);
    run(60, 2, heavy, NULL, NULL, NULL);
    CHECK(strstr(last_log, "FSR 4 quality -> balanced"), "FSR 4's step line: %%s", last_log);
    fsr4 = 0;
    run(60, 3, heavy, NULL, NULL, NULL);
    CHECK(strstr(last_log, "FSR 3.1 balanced -> performance") && !strstr(last_log, "FSR 4"),
          "FSR 4 fell back to FSR 3.1 without a settings change: %%s", last_log);
    dlss = "dlaa", ver++;
    frame(60, T60);
    CHECK(!D.on && dynres_override() == NULL
          && strstr(last_log, "needs an upscaling mode (quality, balanced, performance or ultra performance) while "
                    "FSR 3.1 runs; the mode (dlss) is dlaa"), "FSR 3.1 at native size: %%s", last_log);
    fsr = 0, dlss = "quality", ver++;
    frame(60, T60);
    dlss = "dlaa", ver++;
    frame(60, T60);
    CHECK(!D.on && dynres_override() == NULL && strstr(last_log, "while DLSS runs; the mode (dlss) is dlaa"),
          "DLAA: %%s", last_log);
    dlss = "quality";

    // A GPU time at the edge of the target does not swing between steps.
    plain_start(0.75, 0.5);
    sw = run(60, 600, edge, &lo, &hi, NULL);
    CHECK(sw <= 1 && lo >= 0.65, "just over and under 108 %%%% for 10 min: %%d steps, down to %%.3f", sw, lo);
    plain_start(0.75, 0.5);
    load = 1.32;   // a step down lands at the target
    sw = run(60, 600, pixels, &lo, &hi, NULL);
    CHECK(sw == 1 && NEAR(lo, 0.6525), "at the target after a step down, 10 min: %%d steps, down to %%.3f", sw, lo);
    plain_start(0.75, 0.5);
    load = 0.9;   // between the thresholds
    sw = run(60, 600, pixels, NULL, NULL, NULL);
    CHECK(sw == 0, "between the thresholds, 10 min: %%d steps", sw);
    // One step changes the GPU time by more than the gap: down at 1.1, up at 0.66, over again after the step up.
    // Without the growing wait that is a pair of steps every 4 s, 300 in ten minutes.
    plain_start(0.75, 0.5);
    sw = run(60, 600, steep, NULL, NULL, &held);
    CHECK(sw <= 40 && sw >= 2 && D.up_wait_ns == 48000000000ull, "steep, plain path, 10 min: %%d steps, wait %%.0f s",
          sw, D.up_wait_ns / 1e9);
    CHECK(run(60, 120, steep, NULL, NULL, NULL) <= 6, "steep, plain path: the last two minutes keep few steps");
    wbase = -1, dlss = "quality", game_hz = 60;
    restart();
    sw = run(60, 600, steep, NULL, NULL, NULL);
    CHECK(!D.plain && sw <= 40 && sw >= 2, "steep, mode path, 10 min: %%d steps", sw);
    // A step up that holds 5 s brings the wait back to 3 s.
    run(60, 120, light, NULL, NULL, NULL);
    CHECK(D.level == D.base && D.up_wait_ns == 3000000000ull, "light after steep: step %%d, wait %%.0f s", D.level,
          D.up_wait_ns / 1e9);
    printf(fails ? "dynres: %%d failures\n" : "dynres: ok\n", fails);
    return fails != 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=DESCRIPTION, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    src = (ROOT / 'runtime' / 'pacing' / 'dynres.c').read_text(encoding='utf-8')
    body = src[src.index('#define DYN_LEVELS'):]
    with tempfile.TemporaryDirectory(prefix='nx2navi-dynres-') as tmp:
        c = Path(tmp) / 'check.c'
        c.write_text(HARNESS % body, encoding='utf-8')
        exe = Path(tmp) / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-Wall', '-Wextra', '-Wno-unused-function', str(c), '-o', str(exe),
                               *([] if win else ['-lm'])], env=env)
        return subprocess.call([str(exe)])


if __name__ == '__main__':
    sys.exit(main())
