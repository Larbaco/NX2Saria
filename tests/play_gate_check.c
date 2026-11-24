#include <stdio.h>

#include "vk_play_gate.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const int min = 600;
    const double now = 100000;
    CHECK(play_gate_rest(min, 2850, now, now - 16, 0), "a heavy frame in play: no rest");
    CHECK(play_gate_rest(min, 600, now, now - 16, 0), "600 draws: no rest");
    CHECK(!play_gate_rest(min, 599, now, now - 16, 0), "599 draws: a rest");
    CHECK(!play_gate_rest(min, 40, now, now - 16, 0), "a menu's 40 draws: a rest");
    CHECK(play_gate_rest(min, 2850, now, now - 499, 0), "shown 499 ms ago: no rest");
    CHECK(!play_gate_rest(min, 2850, now, now - 500, 0), "shown 500 ms ago: a rest");
    CHECK(!play_gate_rest(min, 2850, now, -1, 0), "no frame shown yet: a rest");
    CHECK(!play_gate_rest(min, 2850, now, now - 16, 1), "a light moment: a rest");
    CHECK(!play_gate_rest(0, 100000, now, now - 1, 0), "the gate off: a rest");
    CHECK(play_gate_rest(1, 1, now, now - 1, 0), "a setting of 1 and one draw: no rest");

    PlayGateCount c = {0}, logged = {0};
    CHECK(!play_gate_changed(&c, &logged), "nothing counted, yet the line is due");
    play_gate_frame(&c, 1);
    CHECK(c.frames == 0, "a heavy frame with no builder at rest counted");
    play_gate_rest_begin(&c, 5);
    play_gate_rest_begin(&c, 0);
    play_gate_held(&c, 1);
    for (int i = 0; i < 3; i++) play_gate_frame(&c, 1);
    play_gate_frame(&c, 0);
    play_gate_rest_end(&c, 240.0);
    play_gate_rest_end(&c, 250.0);
    CHECK(c.frames == 3 && c.deferred == 6 && c.rests == 2 && c.resting == 0 && c.rest_ms == 490.0,
          "frames %llu deferred %llu rests %llu resting %d ms %.0f", (unsigned long long)c.frames,
          (unsigned long long)c.deferred, (unsigned long long)c.rests, c.resting, c.rest_ms);
    CHECK(play_gate_changed(&c, &logged), "the counts moved, no line");
    CHECK(!play_gate_changed(&c, &logged), "the same counts logged twice");
    play_gate_frame(&c, 1);
    CHECK(!play_gate_changed(&c, &logged), "a line with nothing new");
    play_gate_rest_end(&c, 0);
    CHECK(c.resting == 0, "resting %d after an extra end", c.resting);

    printf("play_gate_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
