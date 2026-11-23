#pragma once

#include <stdint.h>

void changebank_hold_start(void);
void changebank_hold_exit_log(void);

#define CB_UNITS 16
#define CB_GAP_NS 500000000ull

typedef struct {
    int in_use;
    uint64_t map;
} CbUnit;

typedef struct {
    uint64_t mgr;
    uint64_t last_ns;
    int held;
    uint64_t held_map;
    int have_cand;
    uint64_t cand_map;
    uint64_t cand_since;
    uint64_t game_last_map;
    uint64_t asked_last_map;
} CbState;

typedef struct {
    uint64_t updates;
    uint64_t flips;
    uint64_t holds;
    uint64_t absorbed;
    uint64_t taken;
    uint64_t held_updates;
    uint64_t resets;
    uint64_t asked;
} CbStats;

static inline uint64_t cb_fnv1a(const char *s, unsigned n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < n && s[i]; i++) h = (h ^ (uint8_t)s[i]) * 0x100000001b3ull;
    return h ? h : 1;
}

static inline void cb_adopt(CbState *s, int unit, const CbUnit *u) {
    s->held = unit;
    s->held_map = unit >= 0 ? u[unit].map : 0;
    s->have_cand = 0;
}

static inline int cb_pick(CbState *s, CbStats *st, uint64_t mgr, const CbUnit *u, int game, int direct, uint64_t now,
                          uint64_t hold_ns) {
    if (game >= CB_UNITS || game < -1) game = -1;
    const uint64_t gmap = game >= 0 ? u[game].map : 0;
    st->updates++;
    int fresh = s->mgr != mgr || !s->last_ns || now - s->last_ns > CB_GAP_NS;
    if (!fresh && gmap != s->game_last_map) st->flips++;
    s->game_last_map = gmap;
    s->last_ns = now;
    if (fresh) {
        if (s->mgr == mgr && s->held >= 0) st->resets++;
        s->mgr = mgr;
        cb_adopt(s, game, u);
        return game;
    }
    if (s->held >= 0 && (!u[s->held].in_use || u[s->held].map != s->held_map)) {
        st->resets++;
        cb_adopt(s, game, u);
        return game;
    }
    if (!hold_ns || direct || s->held < 0 || gmap == s->held_map) {
        if (s->have_cand && gmap == s->held_map) st->absorbed++;
        cb_adopt(s, game, u);
        return game;
    }
    if (!s->have_cand) {
        s->have_cand = 1, s->cand_since = now;
        st->holds++;
    } else if (s->cand_map != gmap) {
        st->holds++;
    }
    s->cand_map = gmap;
    if (now - s->cand_since >= hold_ns) {
        st->taken++;
        cb_adopt(s, game, u);
        return game;
    }
    st->held_updates++;
    return s->held;
}

static inline int cb_step(CbState *s, CbStats *st, uint64_t mgr, const CbUnit *u, int game, int direct, uint64_t now,
                          uint64_t hold_ns) {
    const int same = s->mgr == mgr && s->last_ns && now - s->last_ns <= CB_GAP_NS;
    const int pick = cb_pick(s, st, mgr, u, game, direct, now, hold_ns);
    const uint64_t map = pick >= 0 && pick < CB_UNITS ? u[pick].map : 0;
    if (same && map != s->asked_last_map) st->asked++;
    s->asked_last_map = map;
    return pick;
}
