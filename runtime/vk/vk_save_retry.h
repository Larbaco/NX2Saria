#pragma once

#include <stdint.h>

#define SAVE_EVERY_MS 20000u
#define SAVE_RETRIES 6

typedef struct {
    uint64_t n;
    uint64_t at_ms;
    uint32_t failed;
} SaveRetry;

static inline int save_due(const SaveRetry *s, uint64_t n, uint64_t now_ms, int pending) {
    if (now_ms - s->at_ms < SAVE_EVERY_MS) return 0;
    if (n != s->n) return 1;
    return (pending || s->failed) && s->failed <= SAVE_RETRIES;
}

static inline int save_tried(SaveRetry *s, uint64_t n, uint64_t now_ms, int saved) {
    if (n != s->n) s->failed = 0;
    s->n = n, s->at_ms = now_ms;
    s->failed = saved ? 0 : s->failed + 1;
    return s->failed == SAVE_RETRIES + 1;
}
