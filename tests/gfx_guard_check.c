#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "gfx_guard.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int crop_model(const int *crop, int w, int h, long long out[4]) {
    out[0] = 0, out[1] = 0, out[2] = w, out[3] = h;
    if (!crop || crop[2] <= 0 || crop[3] <= 0) return GFX_CROP_WHOLE;
    long long cx = crop[0] < 0 ? 0 : crop[0], cy = crop[1] < 0 ? 0 : crop[1], cw = crop[2], ch = crop[3];
    if (cx >= w || cy >= h) return GFX_CROP_OUTSIDE;
    int cut = crop[0] < 0 || crop[1] < 0;
    if (cx + cw > w) cw = w - cx, cut = 1;
    if (cy + ch > h) ch = h - cy, cut = 1;
    out[0] = cx, out[1] = cy, out[2] = cw, out[3] = ch;
    return cut ? GFX_CROP_CUT : GFX_CROP_KEPT;
}

static void crop_case(int x, int y, int cw, int ch, int w, int h) {
    int crop[4] = {x, y, cw, ch}, out[4];
    long long ref[4];
    int how = gfx_crop_clip(crop, w, h, out), want = crop_model(crop, w, h, ref);
    CHECK(how == want && out[0] == ref[0] && out[1] == ref[1] && out[2] == ref[2] && out[3] == ref[3],
          "crop %d,%d %dx%d on %dx%d: %d -> %d,%d %dx%d, expected %d -> %lld,%lld %lldx%lld", x, y, cw, ch, w, h, how,
          out[0], out[1], out[2], out[3], want, ref[0], ref[1], ref[2], ref[3]);
    CHECK(out[0] >= 0 && out[1] >= 0 && out[2] > 0 && out[3] > 0 && out[2] <= w - out[0] && out[3] <= h - out[1],
          "crop %d,%d %dx%d on %dx%d presents %d,%d %dx%d", x, y, cw, ch, w, h, out[0], out[1], out[2], out[3]);
    CHECK(gfx_rect_inside(out, w, h), "the presented rectangle is not inside by gfx_rect_inside");
}

int main(void) {
    int out[4];
    CHECK(gfx_crop_clip(NULL, 1920, 1080, out) == GFX_CROP_WHOLE && out[2] == 1920 && out[3] == 1080, "no crop");
    int zero[4] = {-5, -5, 0, 720};
    CHECK(gfx_crop_clip(zero, 1920, 1080, out) == GFX_CROP_WHOLE && out[0] == 0 && out[2] == 1920, "a 0-wide crop");
    int game[4] = {0, 0, 1280, 720};
    CHECK(gfx_crop_clip(game, 1920, 1080, out) == GFX_CROP_KEPT && out[2] == 1280 && out[3] == 720, "the game's crop");
    int past[4] = {100, 50, 1900, 1100};
    CHECK(gfx_crop_clip(past, 1920, 1080, out) == GFX_CROP_CUT && out[2] == 1820 && out[3] == 1030, "cut at the edge");
    int neg[4] = {-10, -20, 100, 100};
    CHECK(gfx_crop_clip(neg, 1920, 1080, out) == GFX_CROP_CUT && out[0] == 0 && out[1] == 0 && out[2] == 100,
          "a negative origin");
    int out_x[4] = {1920, 0, 10, 10};
    CHECK(gfx_crop_clip(out_x, 1920, 1080, out) == GFX_CROP_OUTSIDE && out[2] == 1920 && out[3] == 1080, "outside");
    crop_case(INT_MAX, 0, INT_MAX, 10, 1920, 1080);
    crop_case(1, 1, INT_MAX, INT_MAX, 1920, 1080);
    crop_case(1919, 1079, INT_MAX, INT_MAX, 1920, 1080);
    crop_case(INT_MIN, INT_MIN, INT_MAX, INT_MAX, 1920, 1080);
    crop_case(INT_MAX - 5, INT_MAX - 5, 6, 6, INT_MAX, INT_MAX);
    crop_case(0, 0, INT_MAX, 1, 1, 1);
    for (int x = -3; x <= 9; x++)
        for (int cw = -1; cw <= 9; cw++) crop_case(x, x - 1, cw, cw + 1, 7, 5);
    srand(12345);
    const int edge[] = {INT_MIN, INT_MIN + 1, -1, 0, 1, 719, 720, 1279, 1280, 1919, 1920, INT_MAX - 1, INT_MAX};
    for (int i = 0; i < 200000; i++) {
        int v[4];
        for (int k = 0; k < 4; k++)
            v[k] = rand() & 1 ? edge[rand() % (int)(sizeof edge / sizeof *edge)] : (int)((unsigned)rand() * 2654435761u);
        crop_case(v[0], v[1], v[2], v[3], i & 1 ? 1920 : 1280, i & 1 ? 1080 : 720);
    }

    int big[4] = {10, 10, INT_MAX, 5};
    CHECK(!gfx_rect_inside(big, 1920, 1080), "an extent near INT_MAX inside");
    int negr[4] = {-1, 0, 10, 10};
    CHECK(!gfx_rect_inside(negr, 1920, 1080), "a negative origin inside");
    int full[4] = {0, 0, 1920, 1080};
    CHECK(gfx_rect_inside(full, 1920, 1080), "the whole image not inside");

    CHECK(gfx_sc_images_ok(0, 3, 0, 3, 16), "3 images refused");
    CHECK(gfx_sc_images_ok(0, 16, 0, 16, 16), "16 images (the arrays' size) refused");
    CHECK(!gfx_sc_images_ok(0, 17, 0, 17, 16), "17 images for 16 slots accepted");
    CHECK(!gfx_sc_images_ok(0, 3, 5, 3, 16), "a VK_INCOMPLETE list accepted");
    CHECK(!gfx_sc_images_ok(0, 3, 0, 2, 16), "fewer images than counted accepted");
    CHECK(!gfx_sc_images_ok(-1, 0, 0, 0, 16), "a failed count accepted");
    CHECK(!gfx_sc_images_ok(0, 0, 0, 0, 16), "no images accepted");
    CHECK(!gfx_sc_images_ok(0, 3, -4, 3, 16), "a failed list (device lost) accepted");

    int lines = 0;
    for (uint32_t n = 1; n <= 100000; n++) lines += gfx_fail_log_due(n);
    CHECK(lines == 8 + 13, "%d lines for 100000 failures, expected 21 (1-8, then 16 ... 65536)", lines);
    CHECK(!gfx_fail_log_due(0) && gfx_fail_log_due(1) && gfx_fail_log_due(8) && !gfx_fail_log_due(9) &&
          gfx_fail_log_due(16) && !gfx_fail_log_due(24) && gfx_fail_log_due(1u << 31), "the log's counts");

    printf("gfx_guard_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
