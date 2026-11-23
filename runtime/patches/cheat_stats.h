#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { CHEAT_WR_UNCHANGED, CHEAT_WR_WRITTEN, CHEAT_WR_REFUSED, CHEAT_WR_KINDS };
#define CHEAT_STATS_EVERY_NS 60000000000ull

typedef struct {
    uint64_t frames, n[CHEAT_WR_KINDS];
    uint64_t said_acts, said_ns;
    int said;
} CheatStats;

static inline const char *cheat_s(long long n) { return n == 1 ? "" : "s"; }

static inline uint64_t cheat_stats_get(const uint64_t *v) { return __atomic_load_n(v, __ATOMIC_RELAXED); }

static inline void cheat_stats_frame(CheatStats *s, const uint32_t add[CHEAT_WR_KINDS]) {
    __atomic_fetch_add(&s->frames, 1, __ATOMIC_RELAXED);
    for (int k = 0; k < CHEAT_WR_KINDS; k++)
        if (add[k]) __atomic_fetch_add(&s->n[k], add[k], __ATOMIC_RELAXED);
}

static inline int cheat_stats_due(CheatStats *s, uint64_t now_ns) {
    uint64_t acts = cheat_stats_get(&s->n[CHEAT_WR_WRITTEN]) + cheat_stats_get(&s->n[CHEAT_WR_REFUSED]);
    if (acts == s->said_acts) return 0;
    if (s->said && now_ns - s->said_ns < CHEAT_STATS_EVERY_NS) return 0;
    s->said = 1, s->said_acts = acts, s->said_ns = now_ns;
    return 1;
}

static inline const char *cheat_stats_line(const CheatStats *s, const char *mod, const char *name, const char *when,
                                           char *buf, size_t size) {
    unsigned long long w = cheat_stats_get(&s->n[CHEAT_WR_WRITTEN]), f = cheat_stats_get(&s->frames);
    snprintf(buf, size, "%s/%s: %llu write%s in %llu frame%s, %llu unchanged, %llu refused %s", mod, name, w, cheat_s((long long)w),
             f, cheat_s((long long)f), (unsigned long long)cheat_stats_get(&s->n[CHEAT_WR_UNCHANGED]),
             (unsigned long long)cheat_stats_get(&s->n[CHEAT_WR_REFUSED]), when);
    return buf;
}
