#pragma once
#include <stdint.h>

#include "platform/platform.h"

typedef struct {
    const void *t;
    int crop[4], has_crop;
    uint64_t at_ns, queued, frame, since;
    uint32_t tid;
} PhJob;

enum { PH_AFTER, PH_CAPPED, PH_NEXT, PH_WHY_N };
#define PH_NONE (-1)

typedef struct {
    PlatMutex lock;
    int32_t on;
    PhJob job;
    uint64_t waited, out[PH_WHY_N];
    uint64_t lock_waits, lock_wait_ns, lock_wait_max;
    void (*on_wait)(uint64_t t0);
} PhState;

typedef void (*PhPresent)(const PhJob *j, int why, void *arg);
typedef int (*PhWhy)(const PhJob *j, void *arg);

static inline int ph_armed(const PhState *s) { return __atomic_load_n(&s->on, __ATOMIC_ACQUIRE); }

static inline void ph_lock(PhState *s) {
    if (plat_mutex_trylock(&s->lock)) return;
    uint64_t t0 = plat_now_ns();
    plat_mutex_lock(&s->lock);
    uint64_t d = plat_now_ns() - t0;
    __atomic_add_fetch(&s->lock_waits, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->lock_wait_ns, d, __ATOMIC_RELAXED);
    if (d > s->lock_wait_max) __atomic_store_n(&s->lock_wait_max, d, __ATOMIC_RELAXED);
    if (s->on_wait) s->on_wait(t0);
}

static inline int ph_flush_try(PhState *s, PhWhy why_now, PhPresent present, void *arg) {
    if (!ph_armed(s) || !plat_mutex_trylock(&s->lock)) return 0;
    int why = s->on ? why_now(&s->job, arg) : PH_NONE;
    if (why != PH_NONE) {
        __atomic_store_n(&s->on, 0, __ATOMIC_RELAXED);
        s->out[why]++;
        present(&s->job, why, arg);
    }
    plat_mutex_unlock(&s->lock);
    return why != PH_NONE;
}

static inline int ph_flush_next(PhState *s, PhPresent present, void *arg) {
    if (!ph_armed(s)) return 0;
    ph_lock(s);
    int out = s->on;
    if (out) {
        __atomic_store_n(&s->on, 0, __ATOMIC_RELAXED);
        s->out[PH_NEXT]++;
        present(&s->job, PH_NEXT, arg);
    }
    plat_mutex_unlock(&s->lock);
    return out;
}

static inline void ph_present(PhState *s, const PhJob *j, int (*hold_now)(void *arg), PhPresent present, void *arg) {
    ph_lock(s);
    if (s->on) {
        __atomic_store_n(&s->on, 0, __ATOMIC_RELAXED);
        s->out[PH_NEXT]++;
        present(&s->job, PH_NEXT, arg);
    }
    if (hold_now && hold_now(arg)) {
        s->job = *j;
        s->waited++;
        __atomic_store_n(&s->on, 1, __ATOMIC_RELEASE);
    } else {
        present(j, PH_NONE, arg);
    }
    plat_mutex_unlock(&s->lock);
}
