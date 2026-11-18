// The arithmetic of the mouse in Ocarina of Time 1.1.1's menus (menu_mouse.c), kept free of the game's memory and of
// the window so tests/menu_mouse_check.c can run it: a button's box from its pane's fields, the pointer in the menus'
// layout units, the window position of a point of the layout, and the box a point falls in.
//
// The layout's units: 3840 x 2160, the origin at the screen's centre, y up. The UI's projection maps them to the
// picture; on a picture wider than 16:9 the runtime squeezes its first row (widescreen.c, set wide_hud) by 16:9 over
// the picture's ratio, so the layout is drawn in the picture's middle 16:9 and the units reach ±1920 / squeeze across
// it. A screen the runtime keeps unsqueezed (the menus' backgrounds and the fades) has squeeze 1. Panes moved or widened
// by NX2SARIA_WIDE_HUD / WIDE_MENUS (wide_hud.c) carry their new place in their matrices, so the box needs no change.
#pragma once
#include <math.h>

typedef struct { float x0, y0, x1, y1; } MmBox;

// A pane's box in layout units from the fields menu_mouse.c reads (as the game's focus frame takes it, main+0x769650):
// its size w, h, the scales m00 and m11 and the translation tx, ty of its global matrix, its flags word (+0xa8: bits
// 11..17 the base position, bit 10 an extra height of `extra` or, with bit 9, an extra width: main+0x769930). Returns 0
// for a box with no area or a value that is not a number.
static inline int mm_pane_box(float w, float h, float m00, float m11, float tx, float ty, unsigned flags, float extra,
                              MmBox *out) {
    if (flags & (1u << 10)) {
        if (flags & (1u << 9)) w += extra;
        else h += extra;
    }
    float hw = fabsf(w * m00) * 0.5f, hh = fabsf(h * m11) * 0.5f;
    if (!(hw > 0.5f && hh > 0.5f) || !isfinite(hw) || !isfinite(hh) || !isfinite(tx) || !isfinite(ty)) return 0;
    unsigned base = flags >> 11 & 0x7f, bh = base % 3, bv = base / 3 % 3;
    float cx = tx + (bh == 0 ? hw : bh == 2 ? -hw : 0), cy = ty + (bv == 0 ? -hh : bv == 2 ? hh : 0);
    *out = (MmBox){cx - hw, cy - hh, cx + hw, cy + hh};
    return 1;
}

// The layout units from the picture's centre to its edge, across (*ux) and down (*uy), from the first two diagonal
// entries of a screen's 2D projection (the canvas's 4 x 4 at the screen's +0x1c0): 2 / 3840 times the squeeze across
// and 2 / 2160 down for the game's 3840 x 2160 layouts, so 1920 / squeeze and 1080; another layout size reads as its
// own. Returns 0 (and the 16:9 3840 x 2160 values) when the matrix is no such projection.
static inline int mm_units(float m00, float m11, float *ux, float *uy) {
    float x = 1.0f / fabsf(m00), y = 1.0f / fabsf(m11);
    *ux = 1920.0f, *uy = 1080.0f;
    if (!(x >= 100.0f && x <= 20000.0f) || !(y >= 100.0f && y <= 20000.0f)) return 0;
    *ux = x, *uy = y;
    return 1;
}

// The pointer at (px, py), fractions of the picture from its top left, in layout units
static inline void mm_to_layout(float px, float py, float ux, float uy, float *x, float *y) {
    *x = (px * 2 - 1) * ux;
    *y = (1 - py * 2) * uy;
}

// The inverse: a point of the layout as fractions of the picture
static inline void mm_to_picture(float x, float y, float ux, float uy, float *px, float *py) {
    *px = (x / ux + 1) * 0.5f;
    *py = (1 - y / uy) * 0.5f;
}

// The box (x, y) is in; the smallest when boxes overlap (a slot inside a panel). -1: none.
static inline int mm_pick(const MmBox *box, int n, float x, float y) {
    int best = -1;
    float area = 0;
    for (int i = 0; i < n; i++) {
        const MmBox *b = &box[i];
        if (x < b->x0 || x > b->x1 || y < b->y0 || y > b->y1) continue;
        float a = (b->x1 - b->x0) * (b->y1 - b->y0);
        if (best < 0 || a < area) best = i, area = a;
    }
    return best;
}

// The pointer has moved since the last look by more than a pixel of a 4K picture
static inline int mm_moved(float px, float py, float lx, float ly) {
    return fabsf(px - lx) > 0.00025f || fabsf(py - ly) > 0.0005f;
}
