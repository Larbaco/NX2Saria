#include <stdio.h>

#include "vk_mem_heap.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int session(int census, int reports, int play_at, double ms, int *before) {
    int slow = 0, in_play_n = 0;
    *before = 0;
    for (int r = 1; r <= reports; r++) {
        int in_play = r * 3600 >= play_at;
        if (!mem_heap_summary_due(census, in_play, slow)) continue;
        if (in_play) in_play_n++;
        else ++*before;
        if (ms > 2.0 && !census) slow = 1;
    }
    return in_play_n;
}

int main(void) {
    CHECK(mem_heap_summary_due(0, 0, 0), "before play the summary is not asked for");
    CHECK(!mem_heap_summary_due(0, 1, 0), "the summary is asked for in play");
    CHECK(!mem_heap_summary_due(0, 0, 1) && !mem_heap_summary_due(0, 1, 1), "a slow summary is asked for again");
    CHECK(mem_heap_summary_due(1, 1, 0) && mem_heap_summary_due(1, 1, 1) && mem_heap_summary_due(1, 0, 1),
          "the census switch does not ask at every report");

    int before;
    CHECK(session(0, 6, 2400, 70.0, &before) == 0 && before == 0, "a session that plays before its first report asks");
    CHECK(session(0, 10, 9000, 0.4, &before) == 0 && before == 2, "a title screen of two reports: %d before play", before);
    CHECK(session(0, 10, 20000, 30.0, &before) == 0 && before == 1, "a slow summary before play: %d taken", before);
    CHECK(session(1, 6, 2400, 70.0, &before) == 6 && before == 0, "the census switch: not every report in play asks");
    CHECK(session(1, 6, 9000, 70.0, &before) == 4 && before == 2, "the census switch across a title screen");

    if (fails) printf("%d checks failed\n", fails);
    else printf("mem_heap_check: ok\n");
    return fails != 0;
}
