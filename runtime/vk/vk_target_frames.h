#pragma once

#include <stdint.h>

#define TFR_SETS 1024
#define TFR_WAYS 8
#define TFR_RECENT 4
#define TFR_STEADY 8

typedef struct { uint64_t image; uint32_t level, layer; uint64_t owner; } TfrTarget;
typedef struct { uint64_t image; uint32_t level, layer, n; uint64_t last, prev, owner; } TargetFrames;
static TargetFrames tfr[TFR_SETS * TFR_WAYS];

static inline TargetFrames *tfr_find(const TfrTarget *t, int make) {
    uint64_t h = (t->image ^ (uint64_t)t->level << 56 ^ (uint64_t)t->layer << 40) * 0x9E3779B97F4A7C15ull;
    TargetFrames *set = &tfr[(h >> 54) * TFR_WAYS], *oldest = set;
    for (int i = 0; i < TFR_WAYS; i++) {
        if (set[i].image == t->image && set[i].level == t->level && set[i].layer == t->layer) return &set[i];
        if (set[i].last < oldest->last) oldest = &set[i];
    }
    if (!make) return NULL;
    *oldest = (TargetFrames){.image = t->image, .level = t->level, .layer = t->layer};
    return oldest;
}

static inline void tfr_note(const TfrTarget *t, uint64_t stamp) {
    TargetFrames *e = tfr_find(t, 1);
    if (e->owner != t->owner) e->owner = t->owner, e->n = 0;
    if (e->last != stamp) e->prev = e->last, e->last = stamp, e->n += e->n < UINT32_MAX;
}

static inline uint64_t tfr_before(const TfrTarget *t, uint64_t stamp) {
    const TargetFrames *e = tfr_find(t, 0);
    return !e ? 0 : e->last == stamp ? e->prev : e->last;
}

static inline int tfr_recent(const TfrTarget *t, uint64_t stamp) {
    uint64_t before = tfr_before(t, stamp);
    return before && stamp - before <= TFR_RECENT;
}

static inline int tfr_first(const TfrTarget *t, uint64_t stamp) { return !tfr_before(t, stamp); }

static inline int tfr_steady(const TfrTarget *t, uint64_t stamp) {
    const TargetFrames *e = tfr_find(t, 0);
    return e && e->owner == t->owner && e->n - (e->last == stamp) >= TFR_STEADY;
}

static inline int tfr_pass_recent(const TfrTarget *c, int n_color, const TfrTarget *d, uint64_t stamp, int any,
                                  int steady, int *by_steady) {
    int n = 0, color[2] = {0, 0}, depth[2] = {0, 0}, color_first = 0, all[2] = {1, 1};
    for (int i = 0; i <= n_color; i++) {
        const TfrTarget *t = i < n_color ? &c[i] : d;
        if (!t || !t->image) continue;
        n++;
        int recent = tfr_recent(t, stamp), kept = recent || (steady && tfr_steady(t, stamp));
        if (recent) *(i < n_color ? &color[0] : &depth[0]) = 1;
        else all[0] = 0;
        if (kept) *(i < n_color ? &color[1] : &depth[1]) = 1;
        else all[1] = 0;
        if (i < n_color && tfr_first(t, stamp)) color_first = 1;
    }
    int r[2];
    for (int k = 0; k < 2; k++) r[k] = any ? color[k] || (depth[k] && !color_first) : n > 0 && all[k];
    if (by_steady) *by_steady = r[1] && !r[0];
    return r[1];
}
