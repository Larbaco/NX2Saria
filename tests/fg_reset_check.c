#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "vk_fg_reset.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static FgrRing ring;
static volatile int stop;

static void *writer(void *arg) {
    (void)arg;
    while (!stop) fgr_note(&ring, 3, 1), fgr_note(&ring, 3 + FGR_SLOTS, 0);
    return NULL;
}

int main(void) {
    CHECK(fgr_cut(&ring, 0) && fgr_cut(&ring, 1) && fgr_cut(&ring, 77), "an id never noted is a cut");
    CHECK(fgr_cut(&ring, FGR_MAX_ID + 1) && fgr_cut(&ring, UINT64_MAX), "an id past the range is a cut");
    for (uint64_t f = 1; f <= 60; f++) fgr_note(&ring, f, f == 40);
    int wrong = 0;
    for (uint64_t f = 60 - FGR_SLOTS + 1; f <= 60; f++) wrong += fgr_cut(&ring, f) != (f == 40);
    CHECK(!wrong, "%d of the last %d frames answered wrong", wrong, FGR_SLOTS);
    CHECK(fgr_cut(&ring, 60 - FGR_SLOTS), "a frame the ring no longer holds is a cut");
    CHECK(fgr_cut(&ring, 61), "a frame not noted yet is a cut");
    memset(&ring, 0, sizeof ring);
    pthread_t t;
    pthread_create(&t, NULL, writer, NULL);
    int paired = 0;
    for (int i = 0; i < 2000000; i++) paired += !fgr_cut(&ring, 3);
    stop = 1;
    pthread_join(t, NULL);
    CHECK(!paired, "frame 3 was answered with the other frame's value %d times", paired);
    printf(fails ? "fg_reset_check: %d failed\n" : "fg_reset_check: ok\n", fails);
    return fails != 0;
}
