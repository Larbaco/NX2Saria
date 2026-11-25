#pragma once
#include <stdint.h>
#include <stdio.h>

#include "platform/platform.h"

enum { NGX_AT_REPLAY, NGX_AT_PRESENT, NGX_AT_FRAME, NGX_AT_SETUP, NGX_AT_N };

typedef struct {
    PlatMutex m;
    uint64_t calls[NGX_AT_N], waits[NGX_AT_N], wait_max[NGX_AT_N];
    uint64_t hold_max;
    int hold_max_at;
} NgxLock;

static inline uint64_t ngx_lock_take(NgxLock *l, int at) {
    if (!plat_mutex_trylock(&l->m)) {
        const uint64_t t0 = plat_now_ns();
        plat_mutex_lock(&l->m);
        const uint64_t d = plat_now_ns() - t0;
        l->waits[at]++;
        if (d > l->wait_max[at]) l->wait_max[at] = d;
    }
    l->calls[at]++;
    return plat_now_ns();
}

static inline void ngx_lock_give(NgxLock *l, int at, uint64_t from) {
    const uint64_t d = plat_now_ns() - from;
    if (d > l->hold_max) l->hold_max = d, l->hold_max_at = at;
    plat_mutex_unlock(&l->m);
}

#define NGX_CALL(lock, at, expr)                                                                                       \
    __extension__({                                                                                                    \
        const uint64_t ngx_from_ = ngx_lock_take((lock), (at));                                                        \
        __typeof__(expr) ngx_r_ = (expr);                                                                              \
        ngx_lock_give((lock), (at), ngx_from_);                                                                        \
        ngx_r_;                                                                                                        \
    })

static inline const char *ngx_at_name(int at) {
    static const char *const n[NGX_AT_N] = {"replay", "present", "frame bookkeeping", "setup"};
    return at >= 0 && at < NGX_AT_N ? n[at] : "?";
}

static inline int ngx_lock_line(NgxLock *l, char *out, size_t size) {
    plat_mutex_lock(&l->m);
    const NgxLock s = *l;
    plat_mutex_unlock(&l->m);
    uint64_t calls = 0, waits = 0, others = 0, others_max = 0;
    for (int i = 0; i < NGX_AT_N; i++) {
        calls += s.calls[i], waits += s.waits[i];
        if (i != NGX_AT_PRESENT) {
            others += s.waits[i];
            if (s.wait_max[i] > others_max) others_max = s.wait_max[i];
        }
    }
    return snprintf(out, size,
                    "NGX lock: %llu calls (replay %llu, present %llu, frame bookkeeping %llu, setup %llu), %llu waited "
                    "for another thread: the present thread %llu times (longest %.2f ms), the others %llu (longest "
                    "%.2f ms); longest hold %.2f ms (%s)",
                    (unsigned long long)calls, (unsigned long long)s.calls[NGX_AT_REPLAY],
                    (unsigned long long)s.calls[NGX_AT_PRESENT], (unsigned long long)s.calls[NGX_AT_FRAME],
                    (unsigned long long)s.calls[NGX_AT_SETUP], (unsigned long long)waits,
                    (unsigned long long)s.waits[NGX_AT_PRESENT], s.wait_max[NGX_AT_PRESENT] / 1e6,
                    (unsigned long long)others, others_max / 1e6, s.hold_max / 1e6, ngx_at_name(s.hold_max_at));
}
