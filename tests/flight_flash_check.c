#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "flight_flash.h"

#define W 640
#define H 360
#define DIFF 24
#define MIN_CELLS 3

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint8_t px[W * H * 4]; } Pic;
typedef struct { uint8_t c[FLASH_CELLS]; } Cells;

static void fill(Pic *p, int x0, int y0, int x1, int y1, int v) {
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++) {
            uint8_t *q = &p->px[4 * (y * W + x)];
            q[0] = q[1] = q[2] = (uint8_t)v, q[3] = 255;
        }
}

static void scene(Pic *p, int dx, int dy) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int u = x + dx, v = y + dy;
            int g = 60 + ((u / 40 * 37 + v / 40 * 91) % 7) * 20 + ((u / 5 + v / 5) & 1) * 10;
            uint8_t *q = &p->px[4 * (y * W + x)];
            q[0] = q[1] = q[2] = (uint8_t)g, q[3] = 255;
        }
}

static Cells cells_of(const Pic *p) {
    Cells c;
    flash_cells(p->px, W, H, c.c);
    return c;
}

static int test3(const Pic *a, const Pic *b, const Pic *c) {
    Cells ca = cells_of(a), cb = cells_of(b), cc = cells_of(c);
    return flash_test(ca.c, cb.c, cc.c, DIFF, MIN_CELLS);
}

static Pic A, B, C;

int main(int argc, char **argv) {
    memset(&A, 0, sizeof A);
    fill(&A, 0, 0, W, H, 40);
    fill(&A, 80, 40, 160, 120, 200);
    fill(&A, 600, 320, 640, 360, 120);
    Cells ca = cells_of(&A);
    int wrong = 0;
    for (int y = 0; y < FLASH_CELLS_Y; y++)
        for (int x = 0; x < FLASH_CELLS_X; x++) {
            int want = x >= 2 && x <= 3 && y >= 1 && y <= 2 ? 200 : x == 15 && y == 8 ? 120 : 40;
            wrong += ca.c[y * FLASH_CELLS_X + x] != want;
        }
    CHECK(!wrong, "%d cells do not hold their rectangle's level", wrong);
    for (int i = 0; i < W * H; i++) A.px[4 * i] = 255, A.px[4 * i + 2] = 0;
    Cells rb = cells_of(&A);
    for (int i = 0; i < W * H; i++) A.px[4 * i] = 0, A.px[4 * i + 2] = 255;
    Cells br = cells_of(&A);
    CHECK(!memcmp(rb.c, br.c, FLASH_CELLS), "the means differ with the order of the first and the third byte");

    scene(&A, 0, 0);
    B = A;
    fill(&B, 200, 120, 320, 200, 255);
    int n = test3(&A, &B, &A);
    CHECK(n == 6, "a white patch of 6 cells for one frame counts %d cells", n);
    fill(&B, 200, 120, 320, 200, 0);
    n = test3(&A, &B, &A);
    CHECK(n == 6, "a black patch of 6 cells for one frame counts %d cells", n);
    fill(&B, 0, 0, W, H, 255);
    n = test3(&A, &B, &A);
    CHECK(n == FLASH_CELLS, "a white frame counts %d cells", n);
    scene(&C, 1, 0);
    B = A;
    fill(&B, 200, 120, 320, 200, 255);
    n = test3(&A, &B, &C);
    CHECK(n == 6, "the patch between two frames a pixel apart counts %d cells", n);

    B = A;
    fill(&B, 200, 120, 280, 160, 255);
    CHECK(!test3(&A, &B, &A), "a patch of 2 cells is a flash with %d as the least", MIN_CELLS);
    Cells flat, mid;
    memset(flat.c, 100, FLASH_CELLS);
    mid = flat;
    for (int i = 0; i < 5; i++) mid.c[40 + i] = 100 + DIFF;
    CHECK(!flash_test(flat.c, mid.c, flat.c, DIFF, MIN_CELLS), "cells that differ by the threshold count");
    for (int i = 0; i < 5; i++) mid.c[40 + i] = 100 + DIFF + 1;
    CHECK(flash_test(flat.c, mid.c, flat.c, DIFF, MIN_CELLS) == 5, "cells one past the threshold do not count");
    B = A;
    fill(&B, 200, 120, 320, 200, 255);
    CHECK(!test3(&A, &B, &B), "a patch that stays is a flash");
    CHECK(!test3(&B, &B, &A), "a patch that goes is a flash");
    CHECK(!test3(&A, &A, &A), "three frames alike are a flash");

    scene(&C, 333, 222);
    for (int i = 0; i < W * H * 4; i++) C.px[i] = (uint8_t)(255 - C.px[i]);
    CHECK(!test3(&A, &C, &C), "a cut before the frame is a flash");
    CHECK(!test3(&A, &A, &C), "a cut after the frame is a flash");
    scene(&B, 77, 140);
    CHECK(!test3(&A, &B, &C), "three unrelated pictures are a flash");
    fill(&A, 0, 0, W, H, 200), fill(&B, 0, 0, W, H, 120), fill(&C, 0, 0, W, H, 40);
    CHECK(!test3(&A, &B, &C), "a fade is a flash");

    for (int v = 1; v <= 64; v *= 2) {
        scene(&A, 0, 0), scene(&B, v, 0), scene(&C, 2 * v, 0);
        n = test3(&A, &B, &C);
        CHECK(!n, "a pan of %d pixels a frame is a flash of %d cells", v, n);
        scene(&B, v, v), scene(&C, 2 * v, 2 * v);
        n = test3(&A, &B, &C);
        CHECK(!n, "a diagonal pan of %d pixels a frame is a flash of %d cells", v, n);
    }
    int hits = 0;
    for (int v = 1; v <= 80; v++)
        for (int x = 160; x < 160 + 40; x++) {
            for (int k = 0; k < 3; k++) {
                Pic *p = k == 0 ? &A : k == 1 ? &B : &C;
                fill(p, 0, 0, W, H, 50);
                fill(p, x + (k - 1) * v, 80, x + (k - 1) * v + 40, 280, 230);
            }
            hits += test3(&A, &B, &C) != 0;
        }
    CHECK(!hits, "a bar crossing the picture is a flash at %d of 3200 speeds and places", hits);
    hits = 0;
    for (int v = 1; v <= 60; v++)
        for (int y = 120; y < 120 + 40; y++) {
            for (int k = 0; k < 3; k++) {
                Pic *p = k == 0 ? &A : k == 1 ? &B : &C;
                fill(p, 0, 0, W, H, 50);
                fill(p, 120, y + (k - 1) * v, 440, y + (k - 1) * v + 40, 230);
            }
            hits += test3(&A, &B, &C) != 0;
        }
    CHECK(!hits, "a bar moving down the picture is a flash at %d of 2400 speeds and places", hits);
    fill(&A, 0, 0, W, H, 50);
    B = A;
    fill(&B, 160, 80, 200, 280, 230);
    n = test3(&A, &B, &A);
    CHECK(n == 5, "the bar shown for one frame counts %d cells", n);
    hits = 0;
    for (int v = 1; v < 40; v++)
        for (int ph = 0; ph < 40; ph++) {
            for (int k = 0; k < 3; k++) {
                Pic *p = k == 0 ? &A : k == 1 ? &B : &C;
                int o = ph + (k - 1) * v;
                fill(p, 0, 0, W, H, 50);
                fill(p, 120 + o, 80 + o, 160 + o, 120 + o, 230);
                fill(p, 280 + o, 160 + o, 320 + o, 200 + o, 230);
                fill(p, 440 + o, 240 + o, 480 + o, 280 + o, 230);
            }
            hits += test3(&A, &B, &C) != 0;
        }
    CHECK(!hits, "three squares moving diagonally are a flash at %d of 1560 speeds and places", hits);

    Cells p0, c0, n0;
    memset(p0.c, 100, FLASH_CELLS);
    c0 = n0 = p0;
    for (int i = 0; i < 3; i++) c0.c[4 * FLASH_CELLS_X + 5 + i] = 160;
    CHECK(flash_test(p0.c, c0.c, n0.c, DIFF, MIN_CELLS) == 3, "three cells 60 brighter for one frame are no flash");
    n0.c[3 * FLASH_CELLS_X + 4] = 115;
    CHECK(flash_test(p0.c, c0.c, n0.c, DIFF, MIN_CELLS) == 3, "a cell beside 15 apart, a quarter of the flash, hides it");
    n0.c[3 * FLASH_CELLS_X + 4] = 116;
    CHECK(!flash_test(p0.c, c0.c, n0.c, DIFF, MIN_CELLS), "a cell beside 16 apart does not hide a flash of 60");
    CHECK(flash_test(p0.c, c0.c, n0.c, DIFF, 2) == 2, "the two cells further from it do not count");
    n0 = p0;
    n0.c[8 * FLASH_CELLS_X + 15] = 100 + DIFF;
    CHECK(flash_test(p0.c, c0.c, n0.c, DIFF, MIN_CELLS) == 3, "a far cell %d apart keeps the frame from being judged", DIFF);
    n0.c[8 * FLASH_CELLS_X + 15] = 100 + DIFF + 1;
    CHECK(!flash_test(p0.c, c0.c, n0.c, DIFF, MIN_CELLS), "a far cell %d apart does not", DIFF + 1);
    n0 = p0;
    for (int i = 0; i < FLASH_MOVING; i++) n0.c[6 * FLASH_CELLS_X + i] = 100 + DIFF / 4 + 1;
    CHECK(flash_test(p0.c, c0.c, n0.c, DIFF, MIN_CELLS) == 3, "%d cells a little apart keep the frame from being judged", FLASH_MOVING);
    n0.c[6 * FLASH_CELLS_X + FLASH_MOVING] = 100 + DIFF / 4 + 1;
    CHECK(!flash_test(p0.c, c0.c, n0.c, DIFF, MIN_CELLS), "%d cells a little apart do not", FLASH_MOVING + 1);
    for (int i = 0; i < FLASH_CELLS; i++) p0.c[i] = 40, c0.c[i] = 100, n0.c[i] = 160;
    CHECK(!flash_test(p0.c, c0.c, n0.c, 4, 1) && !flash_test(n0.c, c0.c, p0.c, 4, 1), "a frame between its neighbours is a flash");

    scene(&A, 0, 0);
    B = A;
    fill(&B, 200, 120, 320, 200, 255);
    Cells steady = cells_of(&A), flash = cells_of(&B);
    static FlashTrack t;
    int r[40] = {0};
    for (uint64_t f = 1; f <= 9; f++) r[f] = flash_note(&t, f, f == 5 ? flash.c : steady.c, DIFF, MIN_CELLS);
    wrong = 0;
    for (int f = 1; f <= 9; f++) wrong += (r[f] != 0) != (f == 6);
    CHECK(!wrong && r[6] == 6, "a flash at frame 5 is reported at %d other frames, with %d cells at frame 6", wrong, r[6]);
    wrong = 0;
    for (uint64_t f = 10; f <= 18; f++)
        if (f != 13) wrong += flash_note(&t, f, f == 12 ? flash.c : steady.c, DIFF, MIN_CELLS) != 0;
    CHECK(!wrong, "a flash whose next frame has no thumbnail is reported %d times", wrong);
    wrong = 0;
    for (uint64_t f = 20; f <= 28; f++)
        if (f != 22) wrong += flash_note(&t, f, f == 23 ? flash.c : steady.c, DIFF, MIN_CELLS) != 0;
    CHECK(!wrong, "a flash whose frame before has no thumbnail is reported %d times", wrong);
    wrong = 0;
    for (uint64_t f = 30; f <= 38; f++)
        if (f != 33) wrong += flash_note(&t, f, steady.c, DIFF, MIN_CELLS) != 0;
    CHECK(!wrong, "steady frames with one missing are reported %d times", wrong);
    wrong = 0;
    for (uint64_t f = 40; f <= 60; f += 2) wrong += flash_note(&t, f, f % 4 ? flash.c : steady.c, DIFF, MIN_CELLS) != 0;
    CHECK(!wrong, "frames two apart are judged %d times", wrong);
    memset(&t, 0, sizeof t);
    CHECK(!flash_note(&t, 0, flash.c, DIFF, MIN_CELLS) && !flash_note(&t, 1, steady.c, DIFF, MIN_CELLS) &&
              !flash_note(&t, 2, flash.c, DIFF, MIN_CELLS),
          "the first two frames are judged");
    CHECK(flash_note(&t, 3, steady.c, DIFF, MIN_CELLS) == 6, "a flash at frame 2 is not found at frame 3");

    if (argc > 1 && !strcmp(argv[1], "time")) {
        enum { N = 2000 };
        scene(&A, 0, 0);
        unsigned sum = 0;
        clock_t t0 = clock();
        for (int i = 0; i < N; i++) {
            A.px[(i * 4099) % (W * H * 4)] ^= 1;
            Cells c = cells_of(&A);
            sum += c.c[i % FLASH_CELLS];
        }
        clock_t t1 = clock();
        for (int i = 0; i < N * 100; i++) {
            flash.c[i % FLASH_CELLS] ^= 1;
            sum += (unsigned)flash_test(steady.c, flash.c, steady.c, DIFF, MIN_CELLS);
        }
        clock_t t2 = clock();
        printf("flight_flash_check: flash_cells %.1f us a picture, flash_test %.2f us a frame (sum %u)\n",
               (double)(t1 - t0) * 1e6 / CLOCKS_PER_SEC / N, (double)(t2 - t1) * 1e6 / CLOCKS_PER_SEC / (N * 100), sum);
    }
    printf(fails ? "flight_flash_check: %d failed\n" : "flight_flash_check: ok\n", fails);
    return fails != 0;
}
