#pragma once
#include <stdint.h>

#define DLSSG_HOLD_IDLE_NS 200000000ull
#define DLSSG_PACE_WINDOW 60

enum { DLSSG_PACE_SAME, DLSSG_PACE_FAST, DLSSG_PACE_SHOWN, DLSSG_PACE_SLOW };

typedef struct {
    uint64_t last_ns;
    uint64_t last_period;
    int last_mult;
    int window, fast, slow;
    int shown;
    int bad;
} DlssgPace;

static inline int dlssg_pace_step(DlssgPace *p, uint64_t now, uint64_t period_ns, int mult, uint64_t hosted_ns,
                                  int allowed, uint32_t applied_us, uint32_t *want_us, int *found) {
    *found = DLSSG_PACE_SAME;
    if (mult < 1) mult = 1;
    if (!p->bad && p->last_ns && now >= p->last_ns && p->last_period == period_ns && p->last_mult == mult) {
        uint64_t d = now - p->last_ns, m = (uint64_t)mult;
        if (d < period_ns / 4 * 3) p->fast++;
        else if (m > 1 && d * 2 > period_ns * (2 * m - 1) && d * 2 < period_ns * (2 * m + 1)) p->slow++;
        if (++p->window >= DLSSG_PACE_WINDOW) {
            if (p->fast > DLSSG_PACE_WINDOW / 2) p->bad = 1, *found = DLSSG_PACE_FAST;
            else if (p->slow > DLSSG_PACE_WINDOW / 2 && p->shown) p->bad = 1, *found = DLSSG_PACE_SLOW;
            else if (p->slow > DLSSG_PACE_WINDOW / 2) p->shown = 1, *found = DLSSG_PACE_SHOWN;
            p->window = p->fast = p->slow = 0;
        }
    } else {
        p->window = p->fast = p->slow = 0;
    }
    uint64_t us = 0;
    int live = hosted_ns && (hosted_ns >= now || now - hosted_ns < DLSSG_HOLD_IDLE_NS);
    if (allowed && period_ns && !p->bad && live) {
        uint64_t per = p->shown ? period_ns / (uint64_t)mult : period_ns;
        us = (per + 500) / 1000;
        if (!us) us = 1;
        if (us > 0x7fffffffu) us = 0;
    }
    *want_us = (uint32_t)us;
    int held = us && applied_us == (uint32_t)us;
    p->last_ns = held ? now : 0;
    p->last_period = held ? period_ns : 0;
    p->last_mult = mult;
    return held;
}
