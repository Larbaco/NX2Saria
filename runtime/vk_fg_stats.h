// What happened to each of the game's frames under frame generation, by the state the game was in, for the log line
// both FSR 3.1 paths write (vk_fg.cpp and vk_fg4.cpp through AMD's libraries on Windows, vk_fsr3fg.c through the
// runtime's passes elsewhere). One counter a frame, in one of three states:
// - gameplay: the game drew a scene for the frame (vk_dlaa.c armed it). Generated, or why not: a history that starts
//   at this frame (a camera cut, a new size, the first frame after a pause), no depth and motion vectors for it, no
//   picture without the HUD, or a scene larger than the generation context;
// - menu: the settings menu was open (generation pauses), whether a scene was drawn behind it or not;
// - no scene: the game drew no scene (the title, a movie, a load, a fade to black, the file menu), so there was
//   nothing to generate from.
// The counters before this were one total from the start of the run, and the frames of the title, the opening movie
// and the file menu, about 4600 at 60 fps, came first: a log read at 21600 frames said 13719 generated, 63%, while the
// same log's gameplay periods generated 600 of 600. The settings menu's frames were counted as frames without depth and
// motion, since the inputs were tested before the menu.
//
// A line is due after FGS_PERIOD gameplay frames, or after FGS_IDLE frames of any kind (a long movie or menu still
// reports), and gives that period's counts, the presents the screen got in it and its time (presents over seconds is
// the frame rate the performance overlay shows with frame generation, gfx_present.c output_rate), the GPU time a
// generated frame took, and the run's totals. The first FGS_LINES lines are all written, then one period in
// FGS_EVERY, each with the totals, so a long session neither floods the log nor loses a count.
// Own header so tests/fg_stats_check.c checks it without a GPU.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FGS_PERIOD 600   // gameplay frames a line
#define FGS_IDLE 3600    // frames of any kind a line, when gameplay is rare
#define FGS_LINES 60     // lines written before only one period in FGS_EVERY is
#define FGS_EVERY 10
#define FGS_STR_(x) #x
#define FGS_STR(x) FGS_STR_(x)

typedef enum {
    FGS_GENERATED,    // gameplay: prepared with generation on, from this frame's inputs
    FGS_CUT,          // gameplay: prepared, but the history starts here (no frame between it and the one before)
    FGS_NO_INPUTS,    // gameplay: the scene was drawn, but no depth and motion vectors reached the present
    FGS_NO_HUDLESS,   // gameplay: no picture without the HUD
    FGS_TOO_LARGE,    // gameplay: the scene is larger than the generation context
    FGS_MENU,         // the settings menu was open
    FGS_NO_SCENE,     // the game drew no scene
    FGS_N
} FgsOutcome;

typedef struct {
    uint64_t n[FGS_N];       // this period
    uint64_t total[FGS_N];   // the run
    uint64_t presents0;      // the screen's presents when the period started (FgsLine.presents is the difference)
    uint64_t t0_ns;          // when the period started
    double gpu_ms;           // GPU time of the generated frames timed in this period, and how many
    uint64_t gpu_n;
    double gpu_ms_total;
    uint64_t gpu_n_total;
    uint64_t periods;        // periods ended
    uint64_t lines;          // lines written
    int started;
} FgStats;

static inline int fgs_gameplay(FgsOutcome o) { return o <= FGS_TOO_LARGE; }

// The frame's outcome, in this order: the menu first (whatever the frame had behind it), then whether the game drew a
// scene, then why a scene's frame was not generated. scene: the game drew a scene for this frame; have: its depth and
// motion vectors reached the present; fits: they fit the context; hudless: the picture without the HUD did; cut: the
// history starts at this frame.
static inline FgsOutcome fgs_classify(int menu, int scene, int have, int fits, int hudless, int cut) {
    if (menu) return FGS_MENU;
    if (!scene && !have) return FGS_NO_SCENE;   // inputs without the scene flag: the scene was drawn (counted as such)
    if (!have) return FGS_NO_INPUTS;
    if (!fits) return FGS_TOO_LARGE;
    if (!hudless) return FGS_NO_HUDLESS;
    return cut ? FGS_CUT : FGS_GENERATED;
}

static inline uint64_t fgs_play(const uint64_t *n) {
    return n[FGS_GENERATED] + n[FGS_CUT] + n[FGS_NO_INPUTS] + n[FGS_NO_HUDLESS] + n[FGS_TOO_LARGE];
}

static inline uint64_t fgs_all(const uint64_t *n) { return fgs_play(n) + n[FGS_MENU] + n[FGS_NO_SCENE]; }

// presents: the screen's presents so far (generated frames included), now_ns: the time.
static inline void fgs_count(FgStats *s, FgsOutcome o, uint64_t presents, uint64_t now_ns) {
    // first frame, or a new swapchain whose present count went back to 0: the period's presents and time start here
    if (!s->started || presents < s->presents0) s->started = 1, s->presents0 = presents, s->t0_ns = now_ns;
    if ((unsigned)o >= FGS_N) return;
    s->n[o]++, s->total[o]++;
}

// The GPU time one generated frame took (its preparation and its generation).
static inline void fgs_gpu(FgStats *s, double ms) {
    if (!(ms > 0) || ms > 1000) return;
    s->gpu_ms += ms, s->gpu_n++, s->gpu_ms_total += ms, s->gpu_n_total++;
}

static inline int fgs_due(const FgStats *s) {
    return s->started && (fgs_play(s->n) >= FGS_PERIOD || fgs_all(s->n) >= FGS_IDLE);
}

static inline double fgs_pct(uint64_t a, uint64_t b) { return b ? 100.0 * (double)a / (double)b : 0.0; }

// When a line is due: writes it into out (name: the log's prefix, "fg", "fg4", "fsr3fg") and starts the next period.
// Returns 1 when the line is to be logged, 0 when it is due but this period is one of those skipped, -1 when none is due.
static inline int fgs_line(FgStats *s, const char *name, uint64_t presents, uint64_t now_ns, char *out, size_t size) {
    if (!fgs_due(s)) return -1;
    const uint64_t *n = s->n, *t = s->total;
    uint64_t play = fgs_play(n), all = fgs_all(n), tplay = fgs_play(t);
    uint64_t shown = presents >= s->presents0 ? presents - s->presents0 : 0;
    double secs = now_ns > s->t0_ns ? (double)(now_ns - s->t0_ns) / 1e9 : 0;
    int write = s->lines < FGS_LINES || s->periods % FGS_EVERY == 0;
    if (write && out && size) {
        char gpu[64] = "no generated frame timed";
        if (s->gpu_n) snprintf(gpu, sizeof gpu, "%.2f ms a generated frame (%llu timed)", s->gpu_ms / (double)s->gpu_n,
                                (unsigned long long)s->gpu_n);
        snprintf(out, size,
                 "%s: %llu gameplay frames: %llu generated (%.1f%%), %llu history starts, %llu without depth and motion, "
                 "%llu without the picture without the HUD, %llu larger than the context; besides them %llu with the menu "
                 "open, %llu without a scene (title, movie, load); %llu presents in %.1f s (%.1f a second on screen, %.1f "
                 "rendered); GPU %s; the run: %llu gameplay frames, %llu generated (%.1f%%), %llu menu, %llu without a "
                 "scene%s",
                 name, (unsigned long long)play, (unsigned long long)n[FGS_GENERATED], fgs_pct(n[FGS_GENERATED], play),
                 (unsigned long long)n[FGS_CUT], (unsigned long long)n[FGS_NO_INPUTS], (unsigned long long)n[FGS_NO_HUDLESS],
                 (unsigned long long)n[FGS_TOO_LARGE], (unsigned long long)n[FGS_MENU], (unsigned long long)n[FGS_NO_SCENE],
                 (unsigned long long)shown, secs, secs > 0 ? (double)shown / secs : 0.0, secs > 0 ? (double)all / secs : 0.0,
                 gpu, (unsigned long long)tplay, (unsigned long long)t[FGS_GENERATED], fgs_pct(t[FGS_GENERATED], tplay),
                 (unsigned long long)t[FGS_MENU], (unsigned long long)t[FGS_NO_SCENE],
                 s->lines + 1 == FGS_LINES ? "; from here one line in " FGS_STR(FGS_EVERY) " periods" : "");
    }
    s->periods++;
    if (write) s->lines++;
    memset(s->n, 0, sizeof s->n);
    s->gpu_ms = 0, s->gpu_n = 0;
    s->presents0 = presents, s->t0_ns = now_ns;
    return write;
}
