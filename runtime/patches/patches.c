#include "cfg.h"
#include "platform/platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#endif
#ifndef RT_PATCH_COUNT
#define RT_PATCH_COUNT 0
#endif

uint8_t rt_patch_on[RT_PATCH_COUNT > 0 ? RT_PATCH_COUNT : 1];

#ifdef RT_PATCH_PAGE
#include "debug_tools.h"
__attribute__((constructor)) static void freecam_register(void) { rt_register_host(RT_PATCH_PAGE + 0xf70, dbg_freecam_host); }
__attribute__((constructor)) static void fov_register(void) {
    if (RT_V111) rt_register_host(RT_PATCH_PAGE + 0xeb0, dbg_fov_host);
}
__attribute__((constructor)) static void wide_lens_register(void) {
    if (RT_V111) rt_register_host(RT_PATCH_PAGE + 0xe40, wide_lens_host);
}
__attribute__((constructor)) static void day_register(void) {
    if (!RT_V111) return;
    rt_register_host(RT_PATCH_PAGE + 0xf60, frame_rate_day_host);
    rt_register_host(RT_PATCH_PAGE + 0xf50, frame_rate_water_host);
    rt_register_host(RT_PATCH_PAGE + 0xf20, frame_rate_sim_host);
    rt_register_host(RT_PATCH_PAGE + 0xf10, frame_rate_sound_gate_host);
    rt_register_host(RT_PATCH_PAGE + 0xf00, frame_rate_sound_step_host);
    rt_register_host(RT_PATCH_PAGE + 0xea0, frame_rate_horse_jump_host);
    rt_register_host(RT_PATCH_PAGE + 0xe80, frame_rate_gohma_turn_host);
    rt_register_host(RT_PATCH_PAGE + 0xe90, animation_loop_host);
    rt_register_host(RT_PATCH_PAGE + 0xe70, battle_music_flags_host);
    rt_register_host(RT_PATCH_PAGE + 0xe60, voice_language_host);
    rt_register_host(RT_PATCH_PAGE + 0xe50, wide_balloon_host);
}
__attribute__((constructor)) static void wide_bars_register(void) {
    if (RT_V111) rt_register_host(RT_PATCH_PAGE + 0xef0, wide_bars_screen_host);
}
__attribute__((constructor)) static void menu_mouse_register(void) {
    if (RT_V111) rt_register_host(RT_PATCH_PAGE + 0xee0, menu_mouse_screen_host);
}
#endif

#if RT_PATCH_COUNT
static const struct { const char *name, *env; int on; } sets[] = {
#define X(i, n, e, d) {#n, e, d},
    RT_PATCH_SETS(X)
#undef X
};
static void *page;
#endif

void rt_patches_init(void) {
#if RT_PATCH_COUNT
    mem_commit_checked(RT_PATCH_PAGE, 0x1000, "patch page");
    page = (void *)(uintptr_t)RT_PATCH_PAGE;
    char list[1024];
    size_t n = 0;
    list[0] = 0;
    for (int i = 0; i < RT_PATCH_COUNT; i++) {
        const char *e = cfg_get(sets[i].env);
        rt_patch_on[i] = (uint8_t)(cfg_meta(sets[i].env) ? cfg_bool(sets[i].env) : e && *e ? strcmp(e, "0") != 0 : sets[i].on);
        n += (size_t)snprintf(list + n, sizeof list - n, "%s%s %s", i ? ", " : "", sets[i].name,
                              rt_patch_on[i] ? "on" : "off");
        if (n >= sizeof list) n = sizeof list - 1;
    }
    rt_log("patches: %s (page %#llx)", list, RT_PATCH_PAGE);
#endif
}

int rt_patch_set(const char *name) {
#if RT_PATCH_COUNT
    for (int i = 0; i < RT_PATCH_COUNT; i++)
        if (!strcmp(sets[i].name, name)) return i;
#endif
    (void)name;
    return -1;
}

int rt_patches_on(const char **names, int cap) {
#if RT_PATCH_COUNT
    if (!page) return -1;
    int n = 0;
    for (int i = 0; i < RT_PATCH_COUNT && n < cap; i++)
        if (__atomic_load_n(&rt_patch_on[i], __ATOMIC_RELAXED) && strncmp(sets[i].name, "mod_", 4)) names[n++] = sets[i].name;
    return n;
#else
    (void)names, (void)cap;
    return -1;
#endif
}

int rt_patch_enabled(int set) { return set >= 0 && set < RT_PATCH_COUNT && rt_patch_on[set]; }

void rt_patch_disable(int set) {
    if (set >= 0 && set < RT_PATCH_COUNT) rt_patch_on[set] = 0;
}

void rt_patch_switch(int set, int on) {
    if (set >= 0 && set < RT_PATCH_COUNT) __atomic_store_n(&rt_patch_on[set], (uint8_t)(on != 0), __ATOMIC_RELAXED);
}

void *rt_patch_page(void) {
#if RT_PATCH_COUNT
    return page;
#else
    return NULL;
#endif
}
