#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRAM_WARN_WINDOW 30
#define VRAM_WARN_HOLD 10
#define VRAM_WARN_GAP_MS 600000
#define VRAM_WARN_MAX 3
#define VRAM_WARN_SHOW_MS 12000
#define VRAM_WARN_LINES 24

enum { VRAM_WARN_NONE, VRAM_WARN_DUE, VRAM_WARN_SPENT, VRAM_WARN_OVER };
enum { VRAM_SUGGEST_RES = 1, VRAM_SUGGEST_MODE = 2, VRAM_SUGGEST_FRAMEGEN = 4, VRAM_SUGGEST_PRESET = 8 };

typedef struct {
    uint32_t hist;
    int active;
    int due;
    int told;
    int shown;
    uint32_t critical;
    uint64_t since_ms;
    uint64_t shown_ms;
} VramWarn;

static inline int vram_warn_count(const VramWarn *w) { return __builtin_popcount(w->hist); }

static inline int vram_warn_step(VramWarn *w, uint64_t now_ms, int critical) {
    w->hist = (w->hist << 1 | (critical ? 1u : 0u)) & ((1u << VRAM_WARN_WINDOW) - 1);
    int n = vram_warn_count(w);
    if (!w->active) {
        if (n < VRAM_WARN_HOLD) return VRAM_WARN_NONE;
        w->active = 1, w->since_ms = now_ms, w->critical = (uint32_t)n;
    } else if (!n) {
        w->active = w->due = w->told = 0;
        return VRAM_WARN_OVER;
    } else if (critical) {
        w->critical++;
    }
    if (w->due) return VRAM_WARN_NONE;
    if (w->shown >= VRAM_WARN_MAX) {
        if (w->told) return VRAM_WARN_NONE;
        w->told = 1;
        return VRAM_WARN_SPENT;
    }
    if (w->shown && now_ms - w->shown_ms < VRAM_WARN_GAP_MS) return VRAM_WARN_NONE;
    w->due = 1;
    return VRAM_WARN_DUE;
}

static inline void vram_warn_mark(VramWarn *w, uint64_t now_ms) {
    w->due = 0;
    w->told = 1;
    w->shown++;
    w->shown_ms = now_ms;
}

static inline unsigned vram_warn_suggest(int res_above, int full_mode, int framegen) {
    unsigned what = (res_above ? VRAM_SUGGEST_RES : 0u) | (full_mode ? VRAM_SUGGEST_MODE : 0u) |
                    (framegen ? VRAM_SUGGEST_FRAMEGEN : 0u);
    return what ? what : VRAM_SUGGEST_PRESET;
}

void vram_warn_start(void);
void vram_warn_sample(int critical, uint64_t used_mb, uint64_t limit_mb, uint32_t free_pm);
int vram_warn_due(void);
void vram_warn_waits(const char *why);
void vram_warn_shown(const char *suggested);

#ifdef __cplusplus
}
#endif
