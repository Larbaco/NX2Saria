#ifndef PAUSE_TAB_CORE_H
#define PAUSE_TAB_CORE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct { float x0, y0, x1, y1; } PtBox;

typedef struct { int was_down, held; } PtPress;

static inline int pt_press(PtPress *s, int armed, int down, int *opened) {
    *opened = 0;
    if (!down) {
        s->was_down = s->held = 0;
        return 0;
    }
    if (!s->was_down && armed) s->held = 1, *opened = 1;
    s->was_down = 1;
    return s->held;
}

#define PT_TAB_STEP 1.075f
#define PT_TAB_HALF 0.325f

static inline int pt_tab_box(const PtBox *before, const PtBox *last, PtBox *out) {
    float c0 = (before->x0 + before->x1) * 0.5f, c1 = (last->x0 + last->x1) * 0.5f, step = c1 - c0;
    float hw = (last->x1 - last->x0) * PT_TAB_HALF;
    if (!(step > 1.0f) || !(hw > 1.0f) || !isfinite(step) || !isfinite(hw)) return 0;
    float c = c1 + step * PT_TAB_STEP;
    *out = (PtBox){c - hw, last->y0, c + hw, last->y1};
    return 1;
}

static inline int pt_settled(float x, float last_x, float units) {
    return fabsf(x) < units && fabsf(x - last_x) < 0.01f;
}

typedef struct { uint16_t index, parent; } PtLink;

static const PtLink PT_SYSTEM_TREE[] = {{1, 0}, {2, 1}, {3, 2}, {4, 2}, {8, 2}, {12, 2}};
static const PtLink PT_CATEGORY_TREE[] = {{1, 0}, {2, 1}, {3, 2}, {4, 3}, {5, 3}, {6, 1}};
static const PtLink PT_OVERLAY_TREE[] = {{7, 5}, {8, 7}, {9, 7}, {10, 7}, {11, 7}, {12, 5}, {13, 12}, {14, 12}};
enum { PT_SYSTEM_INOUT = 2, PT_CATEGORY_BG = 3, PT_OVERLAY_FIRST_CATEGORY = 8, PT_OVERLAY_L = 13, PT_OVERLAY_R = 14 };

static inline int pt_tree_ok(const uint16_t *parents, int n, const PtLink *want, int wn) {
    for (int i = 0; i < wn; i++)
        if (want[i].index >= n || parents[want[i].index] != want[i].parent) return 0;
    return 1;
}

enum { PT_NAME_PRODUCT, PT_NAME_SETTINGS };

static inline int pt_name_kind(const char *v, int *known) {
    *known = 1;
    if (!v || !*v || !strcmp(v, "product")) return PT_NAME_PRODUCT;
    if (!strcmp(v, "settings")) return PT_NAME_SETTINGS;
    *known = 0;
    return PT_NAME_PRODUCT;
}

static inline void pt_header_boxes(PtBox *before, PtBox *last) {
    *before = (PtBox){30.0f, 888.0f, 430.0f, 1112.0f};
    *last = (PtBox){490.0f, 888.0f, 890.0f, 1112.0f};
}

enum { PT_TAB_SETTINGS = 6 };
enum { PT_MOVED = 6, PT_AS_GAME = 0, PT_AS_FIVE = 1, PT_AS_OTHER = 2 };
static const float PT_GAME_X[PT_MOVED] = {-690.0f, -230.0f, 230.0f, 690.0f, -990.0f, 990.0f};
static const float PT_FIVE_X[PT_MOVED] = {-840.0f, -420.0f, 0.0f, 420.0f, -1100.0f, 1100.0f};
#define PT_FIVE_TAB_X 840.0f
#define PT_FIVE_TAB_HALF 170.0f

static inline int pt_spacing(const float x[PT_MOVED]) {
    int game = 1, five = 1;
    for (int i = 0; i < PT_MOVED; i++) {
        game &= fabsf(x[i] - PT_GAME_X[i]) < 0.5f;
        five &= fabsf(x[i] - PT_FIVE_X[i]) < 0.5f;
    }
    return game ? PT_AS_GAME : five ? PT_AS_FIVE : PT_AS_OTHER;
}

static const float PT_GAME_Y[PT_MOVED] = {0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 2.0f};

static inline int pt_find_trans(const float *panes[PT_MOVED], int words, int skip_from, int skip_to) {
    int found = -1;
    for (int k = 0; k + 1 < words; k++) {
        if (k >= skip_from && k < skip_to) continue;
        int all = 1;
        for (int i = 0; i < PT_MOVED && all; i++)
            all = fabsf(panes[i][k] - PT_GAME_X[i]) < 0.01f && fabsf(panes[i][k + 1] - PT_GAME_Y[i]) < 0.01f;
        if (!all) continue;
        if (found >= 0) return -2;
        found = k;
    }
    return found;
}

static inline float pt_alpha_clamp(float a) { return !(a > 0.0f) ? 0.0f : a > 1.0f ? 1.0f : a; }

static inline float pt_chain_alpha(const float *alpha, const uint32_t *flags, int n) {
    if (n <= 0 || !(flags[0] & 1u)) return 0.0f;
    float a = pt_alpha_clamp(alpha[0]);
    for (int i = 1; i < n; i++) {
        if (!(flags[i] & 1u)) return 0.0f;
        if (flags[i] & 8u) a *= pt_alpha_clamp(alpha[i]);
    }
    return a;
}

static inline void pt_five_box(const PtBox *last, PtBox *out) {
    *out = (PtBox){PT_FIVE_TAB_X - PT_FIVE_TAB_HALF, last->y0, PT_FIVE_TAB_X + PT_FIVE_TAB_HALF, last->y1};
}

static inline int pt_five_tab_at(float x, float y) {
    if (!(y >= 880.0f && y <= 1080.0f) || !(x >= -1160.0f && x <= 1160.0f)) return -1;
    if (x < -1040.0f) return 4;
    if (x > 1040.0f) return 5;
    return x < -630.0f ? 0 : x < -210.0f ? 1 : x < 210.0f ? 2 : x < 630.0f ? 3 : PT_TAB_SETTINGS;
}

static inline void pt_page_box(PtBox *out, float units_x, int wide_full) {
    float u = wide_full && units_x > 1920.0f ? units_x - 1920.0f : 0.0f;
    *out = (PtBox){-1560.0f - u, -880.0f, 1560.0f + u, 410.0f};
}

#endif
