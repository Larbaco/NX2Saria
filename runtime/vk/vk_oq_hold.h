#pragma once
#include <stdint.h>

#define OQH_N 1024

typedef struct {
    uint64_t addr[OQH_N];
    uint64_t count[OQH_N];
} OqHold;

static inline uint32_t oqh_slot(uint64_t addr) {
    uint64_t h = (addr >> 4) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 54) & (OQH_N - 1);
}

static inline void oqh_note(OqHold *t, uint64_t addr, uint64_t count) {
    if (!addr) return;
    uint32_t i = oqh_slot(addr);
    t->addr[i] = addr, t->count[i] = count;
}

static inline uint64_t oqh_lost(const OqHold *t, uint64_t addr, uint64_t counted) {
    uint64_t c = counted ? counted : 1;
    if (!addr) return c;
    uint32_t i = oqh_slot(addr);
    if (t->addr[i] == addr && t->count[i] > c) c = t->count[i];
    return c;
}
