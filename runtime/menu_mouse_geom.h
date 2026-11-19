// The arithmetic and the bookkeeping of the mouse in Ocarina of Time 1.1.1's menus (menu_mouse.c), kept free of the
// game's memory and of the window so tests/menu_mouse_check.c can run them: a button's box from its pane's fields, the
// pointer in the menus' layout units, the window position of a point of the layout, the box a point falls in, which
// button presses are new clicks, and the pad presses queued for the game.
//
// The layout's units: 3840 x 2160, the origin at the screen's centre, y up. The UI's projection maps them to the
// picture; on a picture wider than 16:9 the runtime squeezes its first row (widescreen.c, set wide_hud) by 16:9 over
// the picture's ratio, so the layout is drawn in the picture's middle 16:9 and the units reach ±1920 / squeeze across
// it. A screen the runtime keeps unsqueezed (the menus' backgrounds and the fades) has squeeze 1. Panes moved or widened
// by NX2SARIA_WIDE_HUD / WIDE_MENUS (wide_hud.c) carry their new place in their matrices, so the box needs no change.
#pragma once
#include <math.h>
#include <stdint.h>

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

// ---- clicks: the window counts each button's presses as they come (a click shorter than a frame is not lost); a
// click is a count that moved since the last look. Wherever no menu takes the mouse the counts are taken as seen, so a
// click given between two menus (on a fade, on a screen that does not take the focus) is not a click of the next one.
typedef struct { uint32_t seen[2]; } MmClicks;
static inline void mm_clicks_sync(MmClicks *c, uint32_t left, uint32_t right) { c->seen[0] = left, c->seen[1] = right; }
static inline void mm_clicks_take(MmClicks *c, uint32_t left, uint32_t right, int *click_l, int *click_r) {
    *click_l = left != c->seen[0], *click_r = right != c->seen[1];
    mm_clicks_sync(c, left, right);
}

// ---- the pad presses queued for the game (the interface thread adds, the pad read takes, one at a time). A and B
// wait one at most: several quick clicks are one, so a B that closes a menu has no second B behind it to reach the
// play (where B is the sword). The wheel's up and down wait three at most, and a turn the other way drops the rest.
// L and R (the pause menu's pages) wait one at most, like A and B. Everything queued is dropped when its menu goes.
enum { MM_A, MM_B, MM_UP, MM_DOWN, MM_L, MM_R, MM_PRESSES };
static inline void mm_queue(volatile int32_t *q, int kind, int n) {
    if (kind == MM_A || kind == MM_B || kind == MM_L || kind == MM_R) {
        __atomic_store_n(&q[kind], 1, __ATOMIC_SEQ_CST);
        return;
    }
    __atomic_store_n(&q[kind == MM_UP ? MM_DOWN : MM_UP], 0, __ATOMIC_SEQ_CST);
    int32_t v = __atomic_load_n(&q[kind], __ATOMIC_SEQ_CST) + n;
    __atomic_store_n(&q[kind], v > 3 ? 3 : v < 0 ? 0 : v, __ATOMIC_SEQ_CST);
}
static inline void mm_queue_drop(volatile int32_t *q) {
    for (int i = 0; i < MM_PRESSES; i++) __atomic_store_n(&q[i], 0, __ATOMIC_SEQ_CST);
}
// The next press to give the game (A, B, up, down, L, R in that order), -1 none; safe against a drop on another thread
static inline int mm_queue_take(volatile int32_t *q) {
    for (int i = 0; i < MM_PRESSES; i++) {
        int32_t n = __atomic_load_n(&q[i], __ATOMIC_SEQ_CST);
        while (n > 0)
            if (__atomic_compare_exchange_n(&q[i], &n, n - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return i;
    }
    return -1;
}

// ---- the pause menu's tab row (PlusMenu_00): the pages' names Collection, Equipment, Items and System (Menu) and the L
// and R signs across its top are no buttons of the screen's list, so the hit test finds none there. The game turns its
// pages with L and R, one page a press. Their places in layout units (origin at the centre, y up), measured from the
// menu as drawn: the names centred at x -693, -232, 229 and 688, the signs at -989 and 989, the row from y 880 to 1080.
// The wide menus' edits do not move them. The boxes meet halfway between the centres.
enum { MM_TAB_L = 4, MM_TAB_R = 5 };
// The tab under (x, y): the page 0..3, MM_TAB_L or MM_TAB_R for a sign, -1 off the row
static inline int mm_tab_at(float x, float y) {
    if (!(y >= 880.0f && y <= 1080.0f) || !(x >= -1100.0f && x <= 1100.0f)) return -1;
    if (x < -918.0f) return MM_TAB_L;
    if (x > 918.0f) return MM_TAB_R;
    return x < -462.0f ? 0 : x < -1.5f ? 1 : x < 458.0f ? 2 : 3;
}
// The page a pause menu page's layout name stands for (0 Collection, 1 Equipment, 2 Items, 3 System), -1 another
static inline int mm_pause_page(const char *name) {
    static const char *const pages[4] = {"PaCollectMenu_00", "PaEquipmentMenu_00", "PaItemMenu_00", "PaSystemMenu_00"};
    for (int i = 0; i < 4; i++) {
        const char *a = name, *b = pages[i];
        while (*a && *a == *b) a++, b++;
        if (!*a && !*b) return i;
    }
    return -1;
}
// The press that brings page cur one nearer to page goal: MM_L, MM_R, or -1 when it is there (or either is unknown).
// The pages do not wrap past the ends.
static inline int mm_tab_step(int cur, int goal) {
    if (cur < 0 || goal < 0 || cur == goal) return -1;
    return goal < cur ? MM_L : MM_R;
}
