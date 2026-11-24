#pragma once
#include <stdint.h>

#define MFB_HOLD_SLOTS 1024
#define MFB_HOLD_PRESENTS 2
#define MFB_HOLD_BITS 24
#define MFB_HOLD_MASK ((1ull << MFB_HOLD_BITS) - 1)

static inline uint32_t mfb_hold_index(uint64_t list) { return (uint32_t)((list >> 2) * 0x9e3779b1u) >> 22; }

static inline void mfb_hold_mark(volatile uint64_t *tab, uint64_t list, int waits, uint64_t present) {
    volatile uint64_t *slot = &tab[mfb_hold_index(list)];
    if (waits) __atomic_store_n(slot, list << MFB_HOLD_BITS | (present & MFB_HOLD_MASK), __ATOMIC_RELAXED);
    else if (__atomic_load_n(slot, __ATOMIC_RELAXED) >> MFB_HOLD_BITS == list) __atomic_store_n(slot, 0, __ATOMIC_RELAXED);
}

static inline int mfb_hold_test(const volatile uint64_t *tab, uint64_t list, uint64_t present) {
    uint64_t w = __atomic_load_n(&tab[mfb_hold_index(list)], __ATOMIC_RELAXED);
    if (!w || w >> MFB_HOLD_BITS != list) return 0;
    return ((present - w) & MFB_HOLD_MASK) <= MFB_HOLD_PRESENTS;
}
