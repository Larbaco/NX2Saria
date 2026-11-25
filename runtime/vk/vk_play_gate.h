#pragma once

#include <stdint.h>

#define PLAY_GATE_SHOWN_MS 500.0

static inline int play_gate_rest(int draws_min, uint32_t draws_last, double now_ms, double shown_ms, int light) {
    return draws_min > 0 && !light && shown_ms >= 0 && draws_last >= (uint32_t)draws_min &&
           now_ms - shown_ms < PLAY_GATE_SHOWN_MS;
}

typedef struct {
    uint64_t frames;
    uint64_t deferred;
    uint64_t rests;
    double rest_ms;
    int resting;
} PlayGateCount;

static inline void play_gate_frame(PlayGateCount *c, int rest) {
    if (rest && c->resting > 0) c->frames++;
}

static inline void play_gate_rest_begin(PlayGateCount *c, uint64_t held) {
    c->resting++;
    c->rests++;
    c->deferred += held;
}

static inline void play_gate_held(PlayGateCount *c, uint64_t held) { c->deferred += held; }

static inline void play_gate_rest_end(PlayGateCount *c, double ms) {
    if (c->resting > 0) c->resting--;
    if (ms > 0) c->rest_ms += ms;
}

static inline int play_gate_changed(const PlayGateCount *c, PlayGateCount *last) {
    if (c->frames == last->frames && c->deferred == last->deferred && c->rests == last->rests) return 0;
    *last = *c;
    return 1;
}
