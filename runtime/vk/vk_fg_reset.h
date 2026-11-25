#pragma once
#include <stdint.h>

#define FGR_SLOTS 32

typedef struct {
    uint64_t slot[FGR_SLOTS];
} FgrRing;

#define FGR_MAX_ID (UINT64_MAX >> 1)

static inline void fgr_note(FgrRing *r, uint64_t id, int cut) {
    if (id && id <= FGR_MAX_ID) __atomic_store_n(&r->slot[id % FGR_SLOTS], id << 1 | (cut != 0), __ATOMIC_RELEASE);
}

static inline int fgr_cut(const FgrRing *r, uint64_t id) {
    if (!id || id > FGR_MAX_ID) return 1;
    uint64_t v = __atomic_load_n(&r->slot[id % FGR_SLOTS], __ATOMIC_ACQUIRE);
    return v >> 1 != id || (v & 1);
}
