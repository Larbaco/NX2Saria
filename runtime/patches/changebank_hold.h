// The neighbouring map the game preloads, held (changebank_hold.c, the patch set changebank_hold of 1.1.1): the rule,
// apart from the guest so that tests/changebank_hold_check.c runs it.
//
// The game's "OptimizeChangeBank" manager preloads one neighbouring map at a time. Each update chooses one of its 16
// units: among those whose load radius Link is in, the one he moves towards, else the nearest; with none, a unit the
// game asked for directly. The update then asks for the chosen unit's map and lets go of every other unit it had
// asked for, and asking for a map other than the one loading or loaded throws that load away and starts the new one.
// Where two radii overlap (the Market's square lies in Hyrule Field's and Hyrule Castle's), walking or turning flips the
// choice, and every flip is a cancelled load and a whole new one.
//
// cb_step takes the game's choice of this update and returns the one the update goes on with:
// - the game's own when there is nothing held yet (the first choice), when the game's came from a direct request, when
//   it names the map already held (another unit of the same map takes the hold over: no load is cancelled), when the
//   held unit is no longer in use or holds another map now, after a gap in the updates (a loading screen, the manager of
//   another area) and with a hold of 0;
// - otherwise the held unit, until the game's choice has named other maps than the held one (or no map) for the whole
//   hold, counted from the first update that left it: then the game's choice of that update is taken.
// A candidate that goes back to the held map before the hold is over is a pair of loads the game did not throw away and
// make again ("absorbed").
#pragma once

#include <stdint.h>

// After rt_patches_init and mods_exefs_switch (boot.c rt_boot): reads NX2SARIA_CHANGEBANK_HOLD_MS and logs whether the
// set is on ("changebank hold: ..."). At the orderly end (gfx_present.c rt_exit_steps): the session's totals, one line.
void changebank_hold_start(void);
void changebank_hold_exit_log(void);

#define CB_UNITS 16
#define CB_GAP_NS 500000000ull   // updates further apart than this start the hold again (a loading screen, a pause)

typedef struct {
    int in_use;                  // the unit's +0 is 1
    uint64_t map;                // FNV-1a of its map's path, 0 when it names none
} CbUnit;

typedef struct {
    uint64_t mgr;                // the manager the state is for
    uint64_t last_ns;            // the last update seen
    int held;                    // the unit the update goes on with, -1 none
    uint64_t held_map;
    int have_cand;               // the game's choice has left the held map and is being held back
    uint64_t cand_map;           // the map it named at the last update (0: no map)
    uint64_t cand_since;         // the first update that left the held map
    uint64_t game_last_map;      // the game's own choice's map at the last update, for the flip count
    uint64_t asked_last_map;     // the map of the choice returned at the last update, for the asked count
} CbState;

typedef struct {
    uint64_t updates;            // updates seen (the normal mode's choice)
    uint64_t flips;              // the game's own choice named another map than at the update before (not across a gap)
    uint64_t holds;              // choices of another map held back (each map named in turn while away, once)
    uint64_t absorbed;           // times the choice came back to the held map before the hold was over
    uint64_t taken;              // times the hold was over and the game's choice was taken
    uint64_t held_updates;       // updates that went on with the held unit instead of the game's choice
    uint64_t resets;             // holds started again (a gap, another manager, the held unit gone)
    uint64_t asked;              // the choice returned named another map than at the update before: each one a request
                                 // for another map, which starts a load unless that map is loaded already (with a hold
                                 // of 0, the same as flips)
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

// The choice the update goes on with. game: the game's (-1 none, else 0..15); direct: it came from a direct request.
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
    // The hold counts from the first update whose choice left the held map, whichever other maps it names since: a
    // choice that flips between two other maps is taken after the hold all the same.
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

// cb_pick, and the asked count: the returned choice's map against the last update's of the same manager
static inline int cb_step(CbState *s, CbStats *st, uint64_t mgr, const CbUnit *u, int game, int direct, uint64_t now,
                          uint64_t hold_ns) {
    const int same = s->mgr == mgr && s->last_ns && now - s->last_ns <= CB_GAP_NS;
    const int pick = cb_pick(s, st, mgr, u, game, direct, now, hold_ns);
    const uint64_t map = pick >= 0 && pick < CB_UNITS ? u[pick].map : 0;
    if (same && map != s->asked_last_map) st->asked++;
    s->asked_last_map = map;
    return pick;
}
