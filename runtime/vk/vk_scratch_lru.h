#pragma once
#include <stdint.h>

typedef struct {
    uint32_t w, h, f;
    uint64_t used;
    int live;
} ScrEntry;

static inline int scr_pick(ScrEntry *e, int n, uint32_t w, uint32_t h, uint32_t f, uint64_t now, int *hit, int *evict) {
    int free_at = -1, lru = -1;
    *hit = *evict = 0;
    for (int i = 0; i < n; i++) {
        if (!e[i].live) {
            if (free_at < 0) free_at = i;
            continue;
        }
        if (e[i].w == w && e[i].h == h && e[i].f == f) {
            e[i].used = now, *hit = 1;
            return i;
        }
        if (lru < 0 || e[i].used < e[lru].used) lru = i;
    }
    if (free_at >= 0) return free_at;
    *evict = 1;
    return lru;
}
