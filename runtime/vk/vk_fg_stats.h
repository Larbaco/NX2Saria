#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FGS_PERIOD 600
#define FGS_IDLE 3600
#define FGS_LINES 60
#define FGS_EVERY 10
#define FGS_STR_(x) #x
#define FGS_STR(x) FGS_STR_(x)

typedef enum {
    FGS_GENERATED,
    FGS_CUT,
    FGS_NO_INPUTS,
    FGS_NO_HUDLESS,
    FGS_TOO_LARGE,
    FGS_MENU,
    FGS_NO_SCENE,
    FGS_N
} FgsOutcome;

typedef struct {
    uint64_t n[FGS_N];
    uint64_t total[FGS_N];
    uint64_t presents0;
    uint64_t t0_ns;
    double gpu_ms;
    uint64_t gpu_n;
    double gpu_ms_total;
    uint64_t gpu_n_total;
    uint64_t periods;
    uint64_t lines;
    int started;
} FgStats;

static inline int fgs_gameplay(FgsOutcome o) { return o <= FGS_TOO_LARGE; }

static inline FgsOutcome fgs_classify(int menu, int scene, int have, int fits, int hudless, int cut) {
    if (menu) return FGS_MENU;
    if (!scene && !have) return FGS_NO_SCENE;
    if (!have) return FGS_NO_INPUTS;
    if (!fits) return FGS_TOO_LARGE;
    if (!hudless) return FGS_NO_HUDLESS;
    return cut ? FGS_CUT : FGS_GENERATED;
}

static inline uint64_t fgs_play(const uint64_t *n) {
    return n[FGS_GENERATED] + n[FGS_CUT] + n[FGS_NO_INPUTS] + n[FGS_NO_HUDLESS] + n[FGS_TOO_LARGE];
}

static inline uint64_t fgs_all(const uint64_t *n) { return fgs_play(n) + n[FGS_MENU] + n[FGS_NO_SCENE]; }

static inline void fgs_count(FgStats *s, FgsOutcome o, uint64_t presents, uint64_t now_ns) {
    if (!s->started || presents < s->presents0) s->started = 1, s->presents0 = presents, s->t0_ns = now_ns;
    if ((unsigned)o >= FGS_N) return;
    s->n[o]++, s->total[o]++;
}

static inline void fgs_gpu(FgStats *s, double ms) {
    if (!(ms > 0) || ms > 1000) return;
    s->gpu_ms += ms, s->gpu_n++, s->gpu_ms_total += ms, s->gpu_n_total++;
}

static inline int fgs_due(const FgStats *s) {
    return s->started && (fgs_play(s->n) >= FGS_PERIOD || fgs_all(s->n) >= FGS_IDLE);
}

static inline double fgs_pct(uint64_t a, uint64_t b) { return b ? 100.0 * (double)a / (double)b : 0.0; }

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
