#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/menu_skin.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "hw_detect.h"
#include "menu_sound.h"
#include "platform/platform.h"
}

struct PerfSizes;
#include "preset_check_stubs.h"

extern "C" {
void menu_sound_play(MenuSound id) { (void)id; }
void session_summary_header(const char *at) { (void)at; }
int vk_dlaa_needs_restart(void) { return 0; }
int vk_hdr_restart_for(const char *value) { (void)value; return 0; }
int vk_dlssg_wanted(void) { return 0; }
int vk_ngxfg_wanted(void) { return 0; }
int menu_lowlat_restart_for(const char *value) { (void)value; return 0; }
const char *ov_language(void) { return "en-US"; }
const char *plat_ui_language(void) { return "en"; }
}

void skin_button(ImDrawList *, ImVec2, ImVec2, float, float, bool) {}
float skin_cursor_reach(float s) { return 4 * s; }
void skin_cursor_glow(ImDrawList *, ImVec2, ImVec2, float, float) {}
void skin_cursor_frame(ImDrawList *, ImVec2, ImVec2, float, float) {}
void skin_filigree(ImDrawList *, float, float, float, float, float) {}
float skin_filigree_height(float s) { return 8 * s; }
void skin_panel(ImDrawList *, ImVec2, ImVec2, float, float) {}

static int tier_of(const char *name) {
    for (int t = 0; t < 4; t++)
        if (!strcmp(name, cfg_preset_name(t))) return t;
    return -1;
}

static void report(void) {
    printf("got: staged %d\n", opt_preset_tier());
    printf("got: in_use %s\n", cfg_preset_name(cfg_preset_in_use()));
    printf("got: hw_changed %d\n", cfg_preset_hw_changed());
    for (int t = 0; t < 4; t++) {
        char names[32][64];
        const int n = opt_preset_handback(t, names, 32);
        printf("got: handback %s", cfg_preset_name(t));
        for (int i = 0; i < n; i++) printf("%s%s", i ? "," : " ", names[i] + sizeof "NX2SARIA_" - 1);
        printf("\n");
    }
    int nk = 0;
    const char *const *keys = cfg_preset_keys(&nk);
    for (int i = 0; i < nk; i++) {
        char name[80];
        snprintf(name, sizeof name, "NX2SARIA_%s", keys[i]);
        const char *g = opt_get(name), *d = opt_default(name);
        printf("got: value %s %s\n", keys[i], opt_value(name));
        printf("got: get %s %s\n", keys[i], g ? g : "(unset)");
        printf("got: default %s %s\n", keys[i], d ? d : "(none)");
        printf("got: is_set %s %d\n", keys[i], opt_is_set(name) ? 1 : 0);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) return 2;
    snprintf(exe_path, sizeof exe_path, "%s/nx2saria", argv[1]);
    cfg_init();
    cfg_start(1);
    report();
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "stage=", 6)) {
            const int t = tier_of(a + 6);
            opt_apply("NX2SARIA_PRESET", t >= 0 ? cfg_preset_name(t) : "");
            char names[32][64];
            const int n = opt_preset_handback(t >= 0 ? t : cfg_preset_recommended(nullptr), names, 32);
            for (int k = 0; k < n; k++) opt_apply(names[k], "");
        } else if (!strncmp(a, "set=", 4)) {
            char name[160];
            snprintf(name, sizeof name, "NX2SARIA_%s", a + 4);
            char *eq = strchr(name, '=');
            if (eq) *eq = 0, opt_apply(name, eq + 1);
        } else if (!strcmp(a, "apply")) {
            opt_draft_apply(false);
        } else if (!strcmp(a, "discard")) {
            opt_draft_discard();
        } else {
            printf("FAIL: unknown action %s\n", a);
            return 2;
        }
        printf("--- after %s\n", a);
        report();
    }
    return 0;
}
