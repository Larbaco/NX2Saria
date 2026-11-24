#pragma once

#include <stdint.h>

enum {
    GFX_CROP_WHOLE = 0,
    GFX_CROP_KEPT = 1,
    GFX_CROP_CUT = 2,
    GFX_CROP_OUTSIDE = 3,
};

static inline int gfx_crop_clip(const int *crop, int w, int h, int out[4]) {
    out[0] = 0, out[1] = 0, out[2] = w, out[3] = h;
    if (!crop || crop[2] <= 0 || crop[3] <= 0) return GFX_CROP_WHOLE;
    int cx = crop[0] < 0 ? 0 : crop[0], cy = crop[1] < 0 ? 0 : crop[1], cw = crop[2], ch = crop[3];
    if (cx >= w || cy >= h) return GFX_CROP_OUTSIDE;
    int cut = crop[0] < 0 || crop[1] < 0;
    if (cw > w - cx) cw = w - cx, cut = 1;
    if (ch > h - cy) ch = h - cy, cut = 1;
    out[0] = cx, out[1] = cy, out[2] = cw, out[3] = ch;
    return cut ? GFX_CROP_CUT : GFX_CROP_KEPT;
}

static inline int gfx_rect_inside(const int r[4], int w, int h) {
    return r[0] >= 0 && r[1] >= 0 && r[2] > 0 && r[3] > 0 && r[0] < w && r[1] < h && r[2] <= w - r[0] &&
           r[3] <= h - r[1];
}

static inline int gfx_sc_images_ok(int count_r, uint32_t count, int list_r, uint32_t got, uint32_t cap) {
    return count_r == 0 && count > 0 && count <= cap && list_r == 0 && got == count;
}

static inline int gfx_fail_log_due(uint32_t n) { return n && (n <= 8 || !(n & (n - 1))); }
