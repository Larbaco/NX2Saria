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

#define PT_TAB_STEP 1.25f

static inline int pt_tab_box(const PtBox *before, const PtBox *last, PtBox *out) {
    float c0 = (before->x0 + before->x1) * 0.5f, c1 = (last->x0 + last->x1) * 0.5f, step = c1 - c0;
    float hw = (last->x1 - last->x0) * 0.5f;
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
enum { PT_SYSTEM_INOUT = 2, PT_CATEGORY_BG = 3 };

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

static inline void pt_page_box(PtBox *out) { *out = (PtBox){-1560.0f, -780.0f, 1560.0f, 430.0f}; }

#endif
