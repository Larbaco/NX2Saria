// The play gate of the background pipeline builds (vk_pipeline.c play_rest): whether they rest now, and the counts the
// log gives. Own header so tests/play_gate_check.c checks it without Vulkan.
//
// A background build runs the driver's shader compiler for tens to hundreds of milliseconds, and below-normal or idle
// priority does not keep it out of the frame: the game's own calls into the driver wait behind the compile, and the
// compiler takes memory the frame needs (docs/performance.md, "Background builds in play", measures what it costs
// here). So builds made ahead of their first use (the low queue of the pool, the library warm) rest while the frame
// shown is a heavy one, and go on in light ones: the title, menus, pause and map screens, loading, a frame not shown
// for a while. A pipeline a draw is waiting for never rests.
#pragma once

#include <stdint.h>

#define PLAY_GATE_SHOWN_MS 500.0   // a frame shown longer ago than this is not a frame in play (a load, a hang)

// 1 when background builds rest now. draws_min: the setting (0: they never rest); draws_last: the draws of the last
// frame shown, at shown_ms (below 0: none shown yet); light: the moment is a light one whatever the frame was (an area
// load, the boot screen waiting for the warm, a preparation).
static inline int play_gate_rest(int draws_min, uint32_t draws_last, double now_ms, double shown_ms, int light) {
    return draws_min > 0 && !light && shown_ms >= 0 && draws_last >= (uint32_t)draws_min &&
           now_ms - shown_ms < PLAY_GATE_SHOWN_MS;
}

// What the log's line counts. The caller serializes the calls.
typedef struct {
    uint64_t frames;     // frames shown while they were heavy and at least one builder rested
    uint64_t deferred;   // builds held back: each counted once, at the first rest that found it queued
    uint64_t rests;      // rests begun (a builder that found work and a heavy frame)
    double rest_ms;      // the builders' time at rest, summed over them
    int resting;         // builders at rest now
} PlayGateCount;

// A frame was shown; rest: play_gate_rest for it.
static inline void play_gate_frame(PlayGateCount *c, int rest) {
    if (rest && c->resting > 0) c->frames++;
}

// A builder begins a rest with held builds newly held back (those no earlier rest counted).
static inline void play_gate_rest_begin(PlayGateCount *c, uint64_t held) {
    c->resting++;
    c->rests++;
    c->deferred += held;
}

// More builds were queued while the builders rest.
static inline void play_gate_held(PlayGateCount *c, uint64_t held) { c->deferred += held; }

// A builder's rest of ms ended.
static inline void play_gate_rest_end(PlayGateCount *c, double ms) {
    if (c->resting > 0) c->resting--;
    if (ms > 0) c->rest_ms += ms;
}

// 1 when the counts moved since *last, which then takes them: the log's line comes only when there is something new.
static inline int play_gate_changed(const PlayGateCount *c, PlayGateCount *last) {
    if (c->frames == last->frames && c->deferred == last->deferred && c->rests == last->rests) return 0;
    *last = *c;
    return 1;
}
