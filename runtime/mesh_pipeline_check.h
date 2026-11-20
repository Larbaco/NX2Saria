// Report a mesh draw's cache-key check without changing the draw. -1 means the cache entry was not found.
#pragma once
#include <stdint.h>

static inline void mesh_pipeline_check_report(int match) {
    static uint32_t logged;
    if (match <= 0 && __atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 8)
        rt_log("vk_exec: mesh emulation pipeline key check: %s", match ? "entry not found" : "pipeline differs from draw");
}
