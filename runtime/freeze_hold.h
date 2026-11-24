#pragma once
#include <stdint.h>

#define FRZ_SLOW_NS 2000000ull
#define FRZ_BACKOFF_NS 250000000ull
#define FRZ_BACKOFF_MAX_NS 2000000000ull

typedef struct {
    uint64_t hold_until;
    uint64_t backoff;
    uint64_t last_slow;
    uint64_t load_until;
} FrzHold;

static inline void frz_freeze_done(FrzHold *h, uint64_t t1, uint64_t took, uint64_t quiet_ns) {
    if (took < FRZ_SLOW_NS) {
        if (!quiet_ns) h->backoff = 0;
        return;
    }
    if (quiet_ns && h->last_slow && t1 - h->last_slow >= quiet_ns) h->backoff = 0;
    h->backoff = h->backoff ? h->backoff * 2 : FRZ_BACKOFF_NS;
    if (h->backoff > FRZ_BACKOFF_MAX_NS) h->backoff = FRZ_BACKOFF_MAX_NS;
    h->hold_until = t1 + h->backoff;
    h->last_slow = t1;
}

static inline int frz_load_hold(FrzHold *h, uint64_t now, uint64_t load_hold_ns, int loading, uint64_t area_at) {
    if (!load_hold_ns) return 0;
    if (loading) {
        h->load_until = now + load_hold_ns;
        return 1;
    }
    if (area_at && area_at <= now && area_at + load_hold_ns > h->load_until) h->load_until = area_at + load_hold_ns;
    return now < h->load_until;
}

static inline int frz_may_run(FrzHold *h, uint64_t now, uint64_t load_hold_ns, int loading, uint64_t area_at) {
    int load = frz_load_hold(h, now, load_hold_ns, loading, area_at);
    return now >= h->hold_until && !load;
}
