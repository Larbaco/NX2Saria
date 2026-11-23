#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

static int has(const char *set) { return rt_patch_set(set) >= 0; }

int cfg_supported(const char *key) {
    char k[64];
    snprintf(k, sizeof k, "%s", key);
    const char *name = k;
    if (!strncasecmp(name, "NX2SARIA_", 9)) name += 9;
    else if (!strncasecmp(name, "NX2NAVI_", 8)) name += 8;
    if (!strcasecmp(name, "FOV")) return has("fov");
    if (!strcasecmp(name, "OUTPUT_RES") || !strcasecmp(name, "RENDER_SCALE")) return res_adjustable();
    if (!strcasecmp(name, "SHADOW_RES")) return has("shadow");
    if (!strcasecmp(name, "SHADOW_RANGE")) return has("cascade");
    if (!strcasecmp(name, "REALTIME")) return has("realtime");
    if (!strcasecmp(name, "CLOTH_REALTIME")) return has("cloth");
    if (!strcasecmp(name, "HIGHFPS_FIX")) return has("bounce") || has("fade") || has("cursor");
    if (!strcasecmp(name, "HORSE_JUMP_FIX")) return has("ohorse");
    if (!strcasecmp(name, "GOHMA_TURN_FIX")) return has("ogohma");
    if (!strcasecmp(name, "FROZEN_ZORA_FIX")) return has("ozora");
    if (!strcasecmp(name, "BATTLE_MUSIC_FIX")) return has("obgm");
    if (!strcasecmp(name, "VOICE_LANGUAGE_FIX")) return has("voice_language");
    if (!strcasecmp(name, "BALLOON_ANCHOR_FIX")) return has("wide_balloon");
    if (!strcasecmp(name, "MENU_CURSOR")) return RT_V143 || has("menu_mouse");
    if (!strcasecmp(name, "WIDE_HUD") || !strcasecmp(name, "WIDE_MENUS")) return has("wide_hud");
    return 1;
}

static void note_unsupported(const char *key, const char *value, int from_env, void *ctx) {
    (void)value, (void)from_env, (void)ctx;
    if (!cfg_supported(key)) rt_log("cfg: %s has no effect on this game version", key);
}

void cfg_log_unsupported(void) { cfg_each_active(note_unsupported, NULL); }
