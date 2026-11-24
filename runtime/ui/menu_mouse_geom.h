#pragma once
#include <math.h>
#include <stdint.h>

typedef struct { float x0, y0, x1, y1; } MmBox;

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

static inline int mm_units(float m00, float m11, float *ux, float *uy) {
    float x = 1.0f / fabsf(m00), y = 1.0f / fabsf(m11);
    *ux = 1920.0f, *uy = 1080.0f;
    if (!(x >= 100.0f && x <= 20000.0f) || !(y >= 100.0f && y <= 20000.0f)) return 0;
    *ux = x, *uy = y;
    return 1;
}

static inline void mm_to_layout(float px, float py, float ux, float uy, float *x, float *y) {
    *x = (px * 2 - 1) * ux;
    *y = (1 - py * 2) * uy;
}

static inline void mm_to_picture(float x, float y, float ux, float uy, float *px, float *py) {
    *px = (x / ux + 1) * 0.5f;
    *py = (1 - y / uy) * 0.5f;
}

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

static inline int mm_pick_near(const MmBox *box, int n, float x, float y, float margin) {
    int best = -1;
    float bd = 0;
    for (int i = 0; i < n; i++) {
        const MmBox *b = &box[i];
        float dx = x < b->x0 ? b->x0 - x : x > b->x1 ? x - b->x1 : 0;
        float dy = y < b->y0 ? b->y0 - y : y > b->y1 ? y - b->y1 : 0;
        float d = dx > dy ? dx : dy;
        if (d <= margin && (best < 0 || d < bd)) best = i, bd = d;
    }
    return best;
}

static inline int mm_moved(float px, float py, float lx, float ly) {
    return fabsf(px - lx) > 0.00025f || fabsf(py - ly) > 0.0005f;
}

typedef struct { uint32_t seen[2]; } MmClicks;
static inline void mm_clicks_sync(MmClicks *c, uint32_t left, uint32_t right) { c->seen[0] = left, c->seen[1] = right; }
static inline void mm_clicks_take(MmClicks *c, uint32_t left, uint32_t right, int *click_l, int *click_r) {
    *click_l = left != c->seen[0], *click_r = right != c->seen[1];
    mm_clicks_sync(c, left, right);
}

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
static inline int mm_queue_take(volatile int32_t *q) {
    for (int i = 0; i < MM_PRESSES; i++) {
        int32_t n = __atomic_load_n(&q[i], __ATOMIC_SEQ_CST);
        while (n > 0)
            if (__atomic_compare_exchange_n(&q[i], &n, n - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return i;
    }
    return -1;
}

enum { MM_TAB_L = 4, MM_TAB_R = 5 };
static inline int mm_tab_at(float x, float y) {
    if (!(y >= 880.0f && y <= 1080.0f) || !(x >= -1100.0f && x <= 1100.0f)) return -1;
    if (x < -918.0f) return MM_TAB_L;
    if (x > 918.0f) return MM_TAB_R;
    return x < -462.0f ? 0 : x < -1.5f ? 1 : x < 458.0f ? 2 : 3;
}
static inline int mm_pause_page(const char *name) {
    static const char *const pages[4] = {"PaCollectMenu_00", "PaEquipmentMenu_00", "PaItemMenu_00", "PaSystemMenu_00"};
    for (int i = 0; i < 4; i++) {
        const char *a = name, *b = pages[i];
        while (*a && *a == *b) a++, b++;
        if (!*a && !*b) return i;
    }
    return -1;
}
static inline int mm_tab_away(int was, int now, int goal) {
    return was >= 0 && now >= 0 && goal >= 0 && now != was && (now < was) != (goal < was);
}
static inline int mm_tab_step(int cur, int goal) {
    if (cur < 0 || goal < 0 || cur == goal) return -1;
    return goal < cur ? MM_L : MM_R;
}
