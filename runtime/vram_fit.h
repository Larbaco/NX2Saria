#pragma once
#include <stdint.h>
#include <string.h>

#include "mem_budget.h"
#include "preset_pick.h"
#include "vram_warn.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { VRAM_FIT_NONE, VRAM_FIT_MEASURED, VRAM_FIT_PRESET };
enum { VRAM_MODE_OFF, VRAM_MODE_ULTRA_PERFORMANCE, VRAM_MODE_PERFORMANCE, VRAM_MODE_BALANCED, VRAM_MODE_QUALITY,
       VRAM_MODE_FULL };

typedef struct {
    unsigned res_h;
    int mode;
    int framegen;
} VramLoad;

static inline int vram_fit_mode(int taa_on, const char *dlss) {
    static const char *const names[] = {"ultra_performance", "performance", "balanced", "quality", "dlaa"};
    if (!taa_on) return VRAM_MODE_OFF;
    for (int i = 0; i < 5; i++)
        if (dlss && !strcmp(dlss, names[i])) return VRAM_MODE_ULTRA_PERFORMANCE + i;
    return VRAM_MODE_FULL;
}

static inline unsigned vram_fit_heavier(const VramLoad *now, const VramLoad *draft) {
    return (draft->res_h > now->res_h ? VRAM_SUGGEST_RES : 0u) | (draft->mode > now->mode ? VRAM_SUGGEST_MODE : 0u) |
           (draft->framegen && !now->framegen ? VRAM_SUGGEST_FRAMEGEN : 0u);
}

static inline int vram_fit_tier(uint32_t vram_mib) {
    unsigned gib = preset_gib(vram_mib);
    if (!vram_mib) return -1;
    return gib < PRESET_VRAM_LOW_GIB ? PRESET_LOW : gib < PRESET_VRAM_MEDIUM_GIB ? PRESET_MEDIUM : -1;
}

static inline unsigned vram_fit_over_preset(const PresetHw *hw, const VramLoad *draft, unsigned *res_h, int *mode) {
    int tier = vram_fit_tier(hw->have_gpu ? hw->vram_mib : 0);
    *res_h = 0, *mode = VRAM_MODE_OFF;
    if (tier < 0) return 0;
    const char *r = preset_value(tier, hw, "GAME_RES"), *m = preset_value(tier, hw, "DLSS");
    unsigned what = 0;
    if (r && atoi(r) > 0) {
        *res_h = (unsigned)atoi(r);
        if (draft->res_h > *res_h) what |= VRAM_SUGGEST_RES;
    }
    if (m) {
        *mode = vram_fit_mode(1, m);
        if (draft->mode > *mode) what |= VRAM_SUGGEST_MODE;
    }
    return what;
}

static inline int vram_fit_note(int level, int noticed, const VramLoad *now, const VramLoad *draft, const PresetHw *hw,
                                unsigned *what, unsigned *preset_res_h, int *preset_mode) {
    unsigned heavier = vram_fit_heavier(now, draft);
    *preset_res_h = 0, *preset_mode = VRAM_MODE_OFF;
    if (heavier && (level >= MEM_TIGHT || noticed)) {
        *what = heavier;
        return VRAM_FIT_MEASURED;
    }
    unsigned over = heavier ? vram_fit_over_preset(hw, draft, preset_res_h, preset_mode) & heavier : 0;
    *what = over;
    return over ? VRAM_FIT_PRESET : VRAM_FIT_NONE;
}

#ifdef __cplusplus
}
#endif
