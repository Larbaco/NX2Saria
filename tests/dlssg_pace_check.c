#include <stdio.h>
#include <string.h>

#include "dlssg_pace.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MS 1000000ull
static const uint64_t P60 = 16666667ull;

static int run(DlssgPace *p, uint64_t *t, int n, uint64_t d, uint64_t period, int mult, int allowed, uint32_t *applied,
               uint32_t *want, int *last_found) {
    int held = 0;
    for (int i = 0; i < n; i++) {
        *t += d;
        int found;
        held += dlssg_pace_step(p, *t, period, mult, *t - d + 1 * MS, allowed, *applied, want, &found);
        if (found != DLSSG_PACE_SAME && last_found) *last_found = found;
        *applied = *want;
    }
    return held;
}

int main(void) {
    uint32_t want = 0, applied = 0;
    int found = 0;
    uint64_t t = 1000 * MS;

    DlssgPace p;
    memset(&p, 0, sizeof p);
    CHECK(!dlssg_pace_step(&p, t, P60, 2, 0, 1, 0, &want, &found) && want == 0, "no host present yet: pacer, no limit");

    CHECK(!dlssg_pace_step(&p, t, P60, 2, t - 5 * MS, 1, 0, &want, &found) && want == 16667,
          "first frame: limit asked (%u), not yet applied", want);
    CHECK(dlssg_pace_step(&p, t + P60, P60, 2, t, 1, 16667, &want, &found) && want == 16667, "applied: Reflex holds");
    CHECK(!dlssg_pace_step(&p, t + 2 * P60, P60, 2, t, 1, 20000, &want, &found), "another limit applied: the pacer holds");

    CHECK(!dlssg_pace_step(&p, t, 0, 2, t - MS, 1, 16667, &want, &found) && want == 0, "movie: pacer");
    CHECK(!dlssg_pace_step(&p, t, P60, 2, t - MS, 0, 16667, &want, &found) && want == 0, "not allowed: pacer");
    CHECK(!dlssg_pace_step(&p, t, P60, 2, t - 200 * MS, 1, 16667, &want, &found) && want == 0, "idle 200 ms: pacer");
    CHECK(dlssg_pace_step(&p, t, P60, 2, t - 199 * MS, 1, 16667, &want, &found), "idle 199 ms: Reflex");

    memset(&p, 0, sizeof p), applied = 0, found = 0;
    int held = run(&p, &t, 600, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(held == 599 && !p.bad && !p.shown && found == 0 && want == 16667, "held: %d of 600, bad %d shown %d", held, p.bad, p.shown);

    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 300, 2 * P60, P60, 1, 1, &applied, &want, &found);
    CHECK(!p.bad && !p.shown && found == 0, "1x, slow GPU: no verdict");

    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 30, 5 * MS, P60, 2, 1, &applied, &want, &found);
    run(&p, &t, 30, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(!p.bad && found == 0, "30 of 60 too soon: Reflex keeps it");
    run(&p, &t, 31, 5 * MS, P60, 2, 1, &applied, &want, &found);
    run(&p, &t, 29, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(p.bad && found == DLSSG_PACE_FAST, "31 of 60 too soon: the pacer for the run");
    held = run(&p, &t, 120, P60, P60, 2, 1, &applied, &want, NULL);
    CHECK(held == 0 && want == 0, "after that, the pacer, whatever the frames do (%d held)", held);

    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 60, 2 * P60, P60, 2, 1, &applied, &want, &found);
    CHECK(p.shown && !p.bad && found == DLSSG_PACE_SHOWN && want == 8333, "2x, two periods apart: shown units (%u)", want);
    found = 0;
    held = run(&p, &t, 300, P60, P60, 2, 1, &applied, &want, &found);
    CHECK(!p.bad && found == 0 && held == 300, "shown units hold the drawn period");
    run(&p, &t, 2, P60, P60, 4, 1, &applied, &want, NULL);
    CHECK(want == 4167, "4x shown units: %u", want);
    run(&p, &t, 2, P60, P60, 1, 1, &applied, &want, NULL);
    CHECK(want == 16667, "1x shown units: %u", want);

    memset(&p, 0, sizeof p), applied = 0, found = 0;
    p.shown = 1;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 60, P60 / 2, P60, 2, 1, &applied, &want, &found);
    CHECK(p.bad && found == DLSSG_PACE_FAST, "shown units, frames at twice the limit: the pacer");

    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 3, 1, &applied, &want, NULL);
    run(&p, &t, 60, 3 * P60, P60, 3, 1, &applied, &want, &found);
    CHECK(p.shown && found == DLSSG_PACE_SHOWN, "3x, three periods apart: shown units");
    run(&p, &t, 61, 3 * P60, P60, 3, 1, &applied, &want, &found);
    CHECK(p.bad && found == DLSSG_PACE_SLOW, "again: the pacer for the run");

    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 120, 10 * P60, P60, 2, 1, &applied, &want, &found);
    CHECK(!p.bad && !p.shown && found == 0, "hitches: no verdict");

    memset(&p, 0, sizeof p), applied = 0, found = 0;
    run(&p, &t, 2, P60, P60, 2, 1, &applied, &want, NULL);
    run(&p, &t, 40, 5 * MS, P60, 2, 1, &applied, &want, &found);
    run(&p, &t, 40, 5 * MS, P60 / 2, 2, 1, &applied, &want, &found);
    CHECK(!p.bad, "window restarted at the period's change");
    run(&p, &t, 40, 2 * MS, P60 / 2, 3, 1, &applied, &want, &found);
    CHECK(!p.bad, "window restarted at the multiplier's change");

    memset(&p, 0, sizeof p), found = 0;
    for (int i = 0; i < 200; i++) {
        t += 5 * MS;
        dlssg_pace_step(&p, t, P60, 2, t - MS, 1, 0, &want, &found);
    }
    CHECK(!p.bad && found == 0, "unapplied: no window");

    memset(&p, 0, sizeof p);
    dlssg_pace_step(&p, t, 1000000000ull / 30, 1, t - MS, 1, 0, &want, &found);
    CHECK(want == 33333, "30 fps: %u", want);
    dlssg_pace_step(&p, t, 3000000000000000ull, 1, t - MS, 1, 0, &want, &found);
    CHECK(want == 0, "a period no limit can hold: %u", want);
    CHECK(dlssg_pace_step(&p, t, P60, 2, t + MS, 1, 16667, &want, &found) && want == 16667, "a host present stamped after the game present read the clock: Reflex");

    printf(fails ? "%d checks failed\n" : "all checks passed\n", fails);
    return fails != 0;
}
