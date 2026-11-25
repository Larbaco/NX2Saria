#pragma once
#include <stdint.h>

static inline void sync_hw_raise(volatile uint64_t *hw, uint64_t v) {
    uint64_t o = __atomic_load_n(hw, __ATOMIC_RELAXED);
    while (v > o && !__atomic_compare_exchange_n(hw, &o, v, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
}

static inline int sync_hw_beyond(uint64_t raw, uint64_t hw) { return raw > hw; }
