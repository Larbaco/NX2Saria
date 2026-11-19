// Whether a setting does anything on the running game build (cfg.h cfg_supported). The menu hides a row whose key it
// answers 0 for (ui/option_rows.cpp), and the start-up log names the keys a player has set that are inert.
// The answers follow the generated patch sets of recomp/patches-v*.txt (rt_patch_set is -1 for a set the build lacks)
// wherever a setting needs one; the few that depend on the game itself check rt_game_version.
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
    if (!strcasecmp(name, "FOV")) return has("fov");   // resolution.c res_fov_live
    // resolution.c decide reads both, and only res_init's patch of the output and scene sets uses them
    if (!strcasecmp(name, "OUTPUT_RES") || !strcasecmp(name, "RENDER_SCALE")) return res_adjustable();
    if (!strcasecmp(name, "SHADOW_RES")) return has("shadow");       // resolution.c shadow_init
    if (!strcasecmp(name, "SHADOW_RANGE")) return has("cascade");    // resolution.c res_shadow_range
    if (!strcasecmp(name, "REALTIME")) return has("realtime");       // the set's switch (patches.c), frame_rate.c
    if (!strcasecmp(name, "CLOTH_REALTIME")) return has("cloth");    // the set's switch (patches.c)
    // the switch of three sets (recomp/patches-v*.txt): bounce, fade and cursor
    if (!strcasecmp(name, "HIGHFPS_FIX")) return has("bounce") || has("fade") || has("cursor");
    if (!strcasecmp(name, "HORSE_JUMP_FIX")) return has("ohorse");
    if (!strcasecmp(name, "GOHMA_TURN_FIX")) return has("ogohma");
    if (!strcasecmp(name, "FROZEN_ZORA_FIX")) return has("ozora");
    if (!strcasecmp(name, "BATTLE_MUSIC_FIX")) return has("obgm");
    if (!strcasecmp(name, "VOICE_LANGUAGE_FIX")) return has("voice_language");
    if (!strcasecmp(name, "BALLOON_ANCHOR_FIX")) return has("wide_balloon");
    // menu_cursor.c enabled() on 1.4.3, menu_mouse.c (its screens' site, set menu_mouse) on 1.1.1
    if (!strcasecmp(name, "MENU_CURSOR")) return RT_V143 || has("menu_mouse");
    // the squeeze of the UI into the middle 16:9 (set wide_hud) is what both place their elements against
    if (!strcasecmp(name, "WIDE_HUD") || !strcasecmp(name, "WIDE_MENUS")) return has("wide_hud");
    // STICK_SENS has no entry: romfs.c overlay_init serves the player camera's pack with its speeds multiplied
    // (camera_overlay, camera_speed.c cam_pack_build) on every game version, 1.1.1 included, and cam_speed_tick follows
    // a change during the run. Whether the pack was found is known only once the RomFS is up (romfs.c fs_init), which
    // on 1.1.1 is after boot.c cfg_log_unsupported first asks, and the log says so then ("camera speed: ...").
    return 1;
}

// One line per key the player set (nx2navi.ini or the environment) that this game build ignores.
static void note_unsupported(const char *key, const char *value, int from_env, void *ctx) {
    (void)value, (void)from_env, (void)ctx;
    if (!cfg_supported(key)) rt_log("cfg: %s has no effect on this game version", key);
}

void cfg_log_unsupported(void) { cfg_each_active(note_unsupported, NULL); }
