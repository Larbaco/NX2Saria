#pragma once
#include <stdint.h>

#define EVO_PEEK_TRIES 3

typedef struct { volatile uint32_t *val; uint32_t mode, value; } EvoWait;

typedef struct {
    uint32_t has;
    uint32_t has_at;
    EvoWait w;
    uint64_t since;
} EvoPeekCopy;

typedef struct {
    uint32_t seq;
    uint32_t has, has_at, mode, value;
    uintptr_t val;
    uint64_t since;
} EvoPeek;

static inline void evo_peek_publish(EvoPeek *p, const EvoPeekCopy *v) {
    uint32_t s = __atomic_load_n(&p->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&p->seq, s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&p->has, v->has, __ATOMIC_RELAXED);
    __atomic_store_n(&p->has_at, v->has_at, __ATOMIC_RELAXED);
    __atomic_store_n(&p->mode, v->w.mode, __ATOMIC_RELAXED);
    __atomic_store_n(&p->value, v->w.value, __ATOMIC_RELAXED);
    __atomic_store_n(&p->val, (uintptr_t)v->w.val, __ATOMIC_RELAXED);
    __atomic_store_n(&p->since, v->since, __ATOMIC_RELAXED);
    __atomic_store_n(&p->seq, s + 2, __ATOMIC_RELEASE);
}

static inline int evo_peek_read(const EvoPeek *p, EvoPeekCopy *out) {
    for (int k = 0; k < EVO_PEEK_TRIES; k++) {
        uint32_t s0 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) continue;
        out->has = __atomic_load_n(&p->has, __ATOMIC_RELAXED);
        out->has_at = __atomic_load_n(&p->has_at, __ATOMIC_RELAXED);
        out->w.mode = __atomic_load_n(&p->mode, __ATOMIC_RELAXED);
        out->w.value = __atomic_load_n(&p->value, __ATOMIC_RELAXED);
        out->w.val = (volatile uint32_t *)__atomic_load_n(&p->val, __ATOMIC_RELAXED);
        out->since = __atomic_load_n(&p->since, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&p->seq, __ATOMIC_RELAXED) == s0) return 1;
    }
    return 0;
}

static inline int evo_peek_ready(const EvoPeekCopy *c, int (*met)(const void *wait), uint64_t submit_seq, uint64_t max) {
    return c->has && (!c->has_at || met(&c->w) || submit_seq - c->since > max);
}
