#include <stdio.h>

#include "freeze_hold.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MS 1000000ull
#define S 1000000000ull

int main(void) {
    {
        FrzHold h = {0};
        uint64_t t = 10 * S;
        frz_freeze_done(&h, t, 5 * MS, 5 * S);
        CHECK(h.backoff == 250 * MS && h.hold_until == t + 250 * MS, "first slow: %llu", (unsigned long long)h.backoff);
        frz_freeze_done(&h, t + 260 * MS, 100000, 5 * S);
        CHECK(h.backoff == 250 * MS, "a fast freeze keeps the backoff: %llu", (unsigned long long)h.backoff);
        frz_freeze_done(&h, t + 300 * MS, 4 * MS, 5 * S);
        CHECK(h.backoff == 500 * MS, "second slow doubles: %llu", (unsigned long long)h.backoff);
        frz_freeze_done(&h, t + 900 * MS, 4 * MS, 5 * S);
        frz_freeze_done(&h, t + 2 * S, 4 * MS, 5 * S);
        frz_freeze_done(&h, t + 4 * S, 4 * MS, 5 * S);
        CHECK(h.backoff == FRZ_BACKOFF_MAX_NS, "capped at 2 s: %llu", (unsigned long long)h.backoff);
        frz_freeze_done(&h, t + 10 * S, 3 * MS, 5 * S);
        CHECK(h.backoff == 250 * MS, "after the quiet time it starts over: %llu", (unsigned long long)h.backoff);
    }
    {
        FrzHold h = {0};
        frz_freeze_done(&h, 1 * S, 5 * MS, 0);
        frz_freeze_done(&h, 1 * S + 260 * MS, 100000, 0);
        frz_freeze_done(&h, 1 * S + 300 * MS, 5 * MS, 0);
        CHECK(h.backoff == 250 * MS, "quiet 0: a fast freeze between restarts it: %llu", (unsigned long long)h.backoff);
    }
    {
        FrzHold h = {0};
        frz_freeze_done(&h, 1 * S, 5 * MS, 5 * S);
        CHECK(!frz_may_run(&h, 1 * S + 100 * MS, 3 * S, 0, 0), "held by the backoff");
        CHECK(frz_may_run(&h, 1 * S + 250 * MS, 3 * S, 0, 0), "runs after the backoff");
    }
    {
        FrzHold h = {0};
        CHECK(frz_may_run(&h, 100 * S, 3 * S, 0, 0), "no load: runs");
        CHECK(!frz_may_run(&h, 100 * S, 3 * S, 1, 0), "the pool loads: held");
        CHECK(!frz_may_run(&h, 101 * S, 3 * S, 1, 0), "still loading: held");
        CHECK(!frz_may_run(&h, 103 * S, 3 * S, 0, 0), "2 s after the load ended: held");
        CHECK(frz_may_run(&h, 104 * S + 1, 3 * S, 0, 0), "3 s after: runs");
        CHECK(!frz_may_run(&h, 200 * S, 3 * S, 0, 199 * S), "area loaded 1 s ago: held");
        CHECK(frz_may_run(&h, 204 * S, 3 * S, 0, 200 * S), "area loaded 4 s ago: runs");
        FrzHold z = {0};
        CHECK(frz_may_run(&z, 300 * S, 0, 1, 299 * S), "load hold 0: runs while loading");
    }
    if (fails) return 1;
    printf("freeze_hold_check: all checks passed\n");
    return 0;
}
