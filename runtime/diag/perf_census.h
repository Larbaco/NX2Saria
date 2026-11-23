#pragma once
#include <stdint.h>

#define CENSUS_WALK_MAX_NS 2000000000ull

static inline uint64_t census_charge(uint64_t now, uint64_t d, uint64_t w0, uint64_t w1, uint64_t *replay,
                                     uint64_t *other, uint64_t *census) {
    if (!w0 || !d || d > now) return 0;
    if (w1 && w1 < w0) w1 = 0;
    if (!w1 && now - w0 > CENSUS_WALK_MAX_NS) return 0;
    uint64_t lo = now - d > w0 ? now - d : w0, hi = w1 && w1 < now ? w1 : now;
    if (hi <= lo) return 0;
    uint64_t ov = hi - lo, room = d > *census ? d - *census : 0;
    if (ov > room) ov = room;
    uint64_t r = ov < *replay ? ov : *replay;
    uint64_t o = ov - r < *other ? ov - r : *other;
    *replay -= r, *other -= o, *census += r + o;
    return r + o;
}
