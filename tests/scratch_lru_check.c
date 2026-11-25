#include <stdio.h>
#include <string.h>

#include "vk_scratch_lru.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define N 8
static ScrEntry e[N];

static int request(uint32_t w, uint32_t h, uint32_t f, uint64_t frame, int *was_hit, int *was_evict) {
    int hit, evict, i = scr_pick(e, N, w, h, f, frame, &hit, &evict);
    if (!hit) e[i] = (ScrEntry){w, h, f, frame, 1};
    if (was_hit) *was_hit = hit;
    if (was_evict) *was_evict = evict;
    return i;
}

int main(void) {
    int hit, evict;
    for (int k = 0; k < N; k++) {
        int i = request(1920 + (uint32_t)k, 1080, 37, (uint64_t)k + 1, &hit, &evict);
        CHECK(i == k && !hit && !evict, "size %d took entry %d (hit %d, evict %d)", k, i, hit, evict);
    }
    int i = request(1923, 1080, 37, 100, &hit, &evict);
    CHECK(i == 3 && hit && !evict && e[3].used == 100, "the match was entry %d (hit %d, evict %d, used %llu)", i, hit,
          evict, (unsigned long long)e[3].used);
    i = request(1923, 1080, 43, 101, &hit, &evict);
    CHECK(!hit && evict, "another format matched (hit %d, evict %d)", hit, evict);
    CHECK(i == 0, "the other format evicted entry %d, not the oldest (0)", i);
    i = request(5120, 2142, 37, 102, &hit, &evict);
    CHECK(i == 1 && evict, "the next new size evicted entry %d (evict %d), not entry 1", i, evict);
    for (int k = 0; k < 4; k++) request(800 + (uint32_t)k, 600, 37, 103 + (uint64_t)k, &hit, &evict);
    CHECK(e[3].live && e[3].w == 1923 && e[3].f == 37, "entry 3, used at frame 100, was evicted before older ones");
    int a = request(2560, 1440, 37, 200, &hit, &evict);
    int b = request(2560, 1440, 43, 200, &hit, &evict);
    CHECK(a != b && e[a].live && e[a].w == 2560 && e[a].f == 37, "the second image of a frame took the first's entry");
    CHECK(request(2560, 1440, 37, 201, &hit, NULL) == a && hit, "the first image of the pair was not kept");
    e[5].live = 0;
    i = request(640, 360, 37, 300, &hit, &evict);
    CHECK(i == 5 && !hit && !evict, "a free entry was passed over (entry %d, evict %d)", i, evict);
    if (fails) return printf("scratch_lru_check: %d checks failed\n", fails), 1;
    printf("scratch_lru_check: all checks passed\n");
    return 0;
}
