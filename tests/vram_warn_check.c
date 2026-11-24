#include <stdio.h>

#include "mem_budget.h"
#include "vram_warn.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PERIOD MEM_BUDGET_PERIOD_MS
#define WINDOW_MS ((uint64_t)VRAM_WARN_WINDOW * PERIOD)

static int level = MEM_ROOMY;
static uint64_t now;

static int feed(VramWarn *w, uint64_t used_mb, uint64_t limit_mb) {
    level = mem_level_next(level, mem_room(used_mb, limit_mb));
    int ev = vram_warn_step(w, now, level == MEM_CRITICAL);
    now += PERIOD;
    return ev;
}

static int run(VramWarn *w, int samples, uint64_t used_mb, uint64_t limit_mb, int want, int *first) {
    int seen = 0;
    for (int i = 0; i < samples; i++)
        if (feed(w, used_mb, limit_mb) == want && !seen++ && first) *first = i;
    return seen;
}

static void fresh(VramWarn *w) {
    *w = (VramWarn){0};
    level = MEM_ROOMY;
    now = 0;
}

int main(void) {
    VramWarn w;
    int first = -1, seen;

    fresh(&w);
    seen = run(&w, 60, 5406, 5249, VRAM_WARN_DUE, &first);
    CHECK(seen == 1 && first == VRAM_WARN_HOLD - 1, "5406 of 5249 MiB in use for two minutes: due once, at sample %d (%d at %d)",
          VRAM_WARN_HOLD, seen, first);
    CHECK(w.due == 1 && w.shown == 0, "due stays up until the screen takes it (%d, shown %d)", w.due, w.shown);
    uint64_t shown_at = now;
    vram_warn_mark(&w, shown_at);
    CHECK(w.due == 0 && w.shown == 1, "shown: no longer due, one counted");
    int inside = (int)(VRAM_WARN_GAP_MS / PERIOD) - 1;
    int due = 0, spent = 0;
    for (int i = 0; i < inside; i++) {
        int ev = feed(&w, 5406, 5249);
        due += ev == VRAM_WARN_DUE, spent += ev == VRAM_WARN_SPENT;
    }
    CHECK(!due && !spent, "still critical inside the gap: not due again, not reported as spent (%d, %d)", due, spent);
    seen = run(&w, 4, 5406, 5249, VRAM_WARN_DUE, NULL);
    CHECK(seen == 1 && now - shown_at >= VRAM_WARN_GAP_MS, "still critical once the gap has passed: due again (%d)", seen);

    fresh(&w);
    int any = 0;
    for (int i = 0; i < VRAM_WARN_HOLD - 1; i++) any += feed(&w, 5300, 5249) != VRAM_WARN_NONE;
    for (int i = 0; i < 2 * VRAM_WARN_WINDOW; i++) any += feed(&w, 3000, 5249) != VRAM_WARN_NONE;
    CHECK(!any && !w.active, "one peak a sample short of the hold: nothing, not even its end (%d)", any);
    for (int i = 0; i < VRAM_WARN_HOLD - 1; i++) any += feed(&w, 5300, 5249) != VRAM_WARN_NONE;
    CHECK(!any, "a second such peak more than a window later: nothing (%d)", any);
    for (int i = 0; i < 4; i++) any += feed(&w, 3000, 5249) != VRAM_WARN_NONE;
    seen = run(&w, VRAM_WARN_HOLD - 1, 5300, 5249, VRAM_WARN_DUE, &first);
    CHECK(!any && seen == 1 && first == 0, "a third peak right after it: the window holds both, due at its first sample (%d at %d)",
          seen, first);

    fresh(&w);
    int evs[4] = {0};
    for (int i = 0; i < 300; i++) evs[feed(&w, i % 7 < 3 ? 9060 : 8040, 9323)]++;
    CHECK(evs[VRAM_WARN_DUE] == 1 && !evs[VRAM_WARN_OVER] && !evs[VRAM_WARN_SPENT],
          "ten minutes of 6 s critical, 8 s tight in turn (9060 and 8040 of 9323 MiB): due once and never over (%d, %d, %d)",
          evs[VRAM_WARN_DUE], evs[VRAM_WARN_OVER], evs[VRAM_WARN_SPENT]);
    CHECK(w.critical == 300 / 7 * 3 + 3, "the stretch counts its critical samples (%u)", w.critical);

    fresh(&w);
    int odd = 0;
    for (int i = 0; i < VRAM_WARN_WINDOW; i++) {
        int ev = feed(&w, i % 2 ? 4725 : 5249, 5249);
        odd += ev != VRAM_WARN_NONE && ev != VRAM_WARN_DUE;
    }
    CHECK(!odd && w.due && vram_warn_count(&w) == VRAM_WARN_WINDOW,
          "hovering between 0%% and 10%% free stays critical by the level's hysteresis: every sample counts (%d)",
          vram_warn_count(&w));
    seen = run(&w, VRAM_WARN_WINDOW - 1, 4500, 5249, VRAM_WARN_OVER, NULL);
    CHECK(!seen && w.due, "14%% free for less than a window: still due");
    CHECK(feed(&w, 4500, 5249) == VRAM_WARN_OVER && !w.due && !w.active, "a whole window without a critical sample: over, no longer due");
    CHECK(feed(&w, 4500, 5249) == VRAM_WARN_NONE, "over is said once");
    CHECK(w.shown == 0, "a notice the screen never took is not counted");

    fresh(&w);
    for (int i = 0; i < VRAM_WARN_MAX; i++) {
        seen = run(&w, (int)(VRAM_WARN_GAP_MS / PERIOD) + VRAM_WARN_HOLD, 6000, 5249, VRAM_WARN_DUE, NULL);
        CHECK(seen == 1, "notice %d of the session comes due (%d)", i + 1, seen);
        vram_warn_mark(&w, now);
    }
    due = spent = 0;
    for (int i = 0; i < 4 * (int)(VRAM_WARN_GAP_MS / PERIOD); i++) {
        int ev = feed(&w, 6000, 5249);
        due += ev == VRAM_WARN_DUE, spent += ev == VRAM_WARN_SPENT;
    }
    CHECK(!due && w.shown == VRAM_WARN_MAX, "after the session's last notice: never due again (%d)", due);
    CHECK(!spent, "the stretch whose notice was the last is not reported as spent (%d)", spent);
    seen = run(&w, VRAM_WARN_WINDOW, 3000, 5249, VRAM_WARN_OVER, NULL);
    CHECK(seen == 1, "that stretch ends (%d)", seen);
    seen = run(&w, 3 * VRAM_WARN_WINDOW, 6000, 5249, VRAM_WARN_SPENT, &first);
    CHECK(seen == 1 && first == VRAM_WARN_HOLD - 1 && !w.due, "a later stretch: spent, once, at the hold (%d at %d)", seen, first);

    CHECK(vram_warn_suggest(1, 1, 1) == (VRAM_SUGGEST_RES | VRAM_SUGGEST_MODE | VRAM_SUGGEST_FRAMEGEN), "all three in use");
    CHECK(vram_warn_suggest(0, 1, 1) == (VRAM_SUGGEST_MODE | VRAM_SUGGEST_FRAMEGEN), "full-size mode and frame generation");
    CHECK(vram_warn_suggest(1, 0, 0) == VRAM_SUGGEST_RES, "the picture larger than the window alone");
    CHECK(vram_warn_suggest(0, 0, 0) == VRAM_SUGGEST_PRESET, "none of the three: the preset");
    VramWait wait = {0};
    CHECK(vram_warn_wait_step(&wait, VRAM_WAIT_MOVIE) == 1 && vram_warn_wait_step(&wait, VRAM_WAIT_MOVIE) == 0,
          "a reason to wait is said once while it lasts");
    CHECK(vram_warn_wait_step(&wait, VRAM_WAIT_TITLE) == 1 && vram_warn_wait_step(&wait, VRAM_WAIT_LOADING) == 1,
          "each change of the reason is said: a movie, then the title, then a loading screen");
    CHECK(vram_warn_wait_step(&wait, VRAM_WAIT_NONE) == 0 && vram_warn_wait_step(&wait, VRAM_WAIT_LOADING) == 1,
          "no reason says nothing, and the same reason after it counts as a change");
    CHECK(wait.lines == VRAM_WARN_WAIT_LINES && vram_warn_wait_step(&wait, VRAM_WAIT_PAUSE) == 0 &&
          vram_warn_wait_step(&wait, VRAM_WAIT_SETTINGS) == 0, "after %d lines a notice's later reasons are not said", VRAM_WARN_WAIT_LINES);
    wait = (VramWait){0};
    CHECK(vram_warn_wait_step(&wait, VRAM_WAIT_PAUSE) == 1, "the next notice starts over");

    CHECK(WINDOW_MS == 60000 && VRAM_WARN_HOLD * PERIOD == 20000, "the window is a minute and the hold 20 s of it");

    if (fails) return printf("vram_warn_check: %d checks failed\n", fails), 1;
    printf("vram_warn_check: ok\n");
    return 0;
}
