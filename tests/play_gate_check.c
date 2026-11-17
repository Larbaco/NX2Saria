// runtime/vk_play_gate.h: when the background pipeline builds rest in play (runtime/vk_pipeline.c play_rest) and what
// the log's line counts. Built and run by tests/play_gate_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>

#include "vk_play_gate.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const int min = 600;
    const double now = 100000;
    // The game in play: a frame of 2,850 draws shown 16 ms ago. Builds rest.
    CHECK(play_gate_rest(min, 2850, now, now - 16, 0), "a heavy frame in play: no rest");
    // At the threshold they rest, one draw under it they go on: a menu, the pause screen, the title.
    CHECK(play_gate_rest(min, 600, now, now - 16, 0), "600 draws: no rest");
    CHECK(!play_gate_rest(min, 599, now, now - 16, 0), "599 draws: a rest");
    CHECK(!play_gate_rest(min, 40, now, now - 16, 0), "a menu's 40 draws: a rest");
    // No frame shown for PLAY_GATE_SHOWN_MS: a load or a hang, not play. Builds go on.
    CHECK(play_gate_rest(min, 2850, now, now - 499, 0), "shown 499 ms ago: no rest");
    CHECK(!play_gate_rest(min, 2850, now, now - 500, 0), "shown 500 ms ago: a rest");
    CHECK(!play_gate_rest(min, 2850, now, -1, 0), "no frame shown yet: a rest");
    // A light moment whatever the frame: an area load, the boot screen waiting for the warm, a preparation.
    CHECK(!play_gate_rest(min, 2850, now, now - 16, 1), "a light moment: a rest");
    // The setting at 0: they build in every frame, as before.
    CHECK(!play_gate_rest(0, 100000, now, now - 1, 0), "the gate off: a rest");
    // A setting of 1: every frame shown in play is heavy.
    CHECK(play_gate_rest(1, 1, now, now - 1, 0), "a setting of 1 and one draw: no rest");

    // The counts: a session where two builders rest over three heavy frames with 5 queued builds, a third queued during
    // the rest, then a light frame lets them go on.
    PlayGateCount c = {0}, logged = {0};
    CHECK(!play_gate_changed(&c, &logged), "nothing counted, yet the line is due");
    play_gate_frame(&c, 1);   // heavy, but nobody rests yet: not counted
    CHECK(c.frames == 0, "a heavy frame with no builder at rest counted");
    play_gate_rest_begin(&c, 5);   // the first builder finds the queue's 5 builds
    play_gate_rest_begin(&c, 0);   // the second finds them counted already
    play_gate_held(&c, 1);          // a build queued during the rest
    for (int i = 0; i < 3; i++) play_gate_frame(&c, 1);
    play_gate_frame(&c, 0);        // a light frame: not counted, and the builders go on
    play_gate_rest_end(&c, 240.0);
    play_gate_rest_end(&c, 250.0);
    CHECK(c.frames == 3 && c.deferred == 6 && c.rests == 2 && c.resting == 0 && c.rest_ms == 490.0,
          "frames %llu deferred %llu rests %llu resting %d ms %.0f", (unsigned long long)c.frames,
          (unsigned long long)c.deferred, (unsigned long long)c.rests, c.resting, c.rest_ms);
    // The line comes when the counts moved, and once for them.
    CHECK(play_gate_changed(&c, &logged), "the counts moved, no line");
    CHECK(!play_gate_changed(&c, &logged), "the same counts logged twice");
    play_gate_frame(&c, 1);   // heavy, nobody at rest: nothing new
    CHECK(!play_gate_changed(&c, &logged), "a line with nothing new");
    // More ends than begins never leave a negative count of builders at rest, which would count no frame again.
    play_gate_rest_end(&c, 0);
    CHECK(c.resting == 0, "resting %d after an extra end", c.resting);

    printf("play_gate_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
