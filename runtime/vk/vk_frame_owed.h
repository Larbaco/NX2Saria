#pragma once
#include <stdint.h>

typedef struct {
    uint64_t owed;
    uint64_t done;
} FrameOwed;

typedef void (*FrameEnd)(uint64_t frame, void *arg);

static inline int fo_waiting(FrameOwed *o) {
    return __atomic_load_n(&o->owed, __ATOMIC_ACQUIRE) != __atomic_load_n(&o->done, __ATOMIC_RELAXED);
}

static inline uint64_t fo_run(FrameOwed *o, FrameEnd end, void *arg) {
    uint64_t owed = __atomic_load_n(&o->owed, __ATOMIC_ACQUIRE), d = __atomic_load_n(&o->done, __ATOMIC_RELAXED), n = 0;
    while (d < owed) end(++d, arg), __atomic_store_n(&o->done, d, __ATOMIC_RELEASE), n++;
    return n;
}

static inline int fo_ran(FrameOwed *o, uint64_t frame) { return __atomic_load_n(&o->done, __ATOMIC_ACQUIRE) >= frame; }

static inline int fo_present(FrameOwed *o, uint64_t frame, int (*try_run)(void *), void *arg) {
    uint64_t was = __atomic_load_n(&o->owed, __ATOMIC_RELAXED);
    while (was < frame && !__atomic_compare_exchange_n(&o->owed, &was, frame, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
    return try_run(arg);
}

static inline int fo_close(FrameOwed *o, uint64_t frame, void (*run_locked)(void *), void *arg) {
    if (fo_ran(o, frame)) return 0;
    run_locked(arg);
    return 1;
}
