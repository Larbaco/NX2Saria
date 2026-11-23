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

static int g_cheats;
static int cheats_count(void) { return g_cheats; }
#include "menu_tabs_cut.h"

static int fails;
static void expect(bool ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    fails += !ok;
}
static bool is(const char *got, const char *want) { return want ? got && !strcmp(got, want) : !got; }

static const char *const A = "NX2SARIA_CHEAT_0123ABCD", *const B = "NX2SARIA_CHEAT_0000BEEF";

static void toggle_both(void) {
    opt_change(A, "1", "Moon jump", "Off", "On");
    opt_change(B, "0", "Infinite rupees", "On", "Off");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) return 2;
    snprintf(exe_path, sizeof exe_path, "%s/nx2saria", argv[1]);
    cfg_init();
    cfg_start(1);

    g_cheats = 0;
    expect(tabs_shown() == TAB_CHEATS, "no cheats: the tabs shown stop before Cheats");
    g_cheats = 3;
    expect(tabs_shown() == TAB_CHEATS + 1 && tabs_shown() == TAB_COUNT, "cheats: Cheats is the last tab shown");

    expect(is(cfg_get(A), NULL) && is(cfg_get(B), "1"), "start: A unset, B on in the settings file");

    opt_change(A, "1", "Moon jump", "Off", "On");
    expect(is(cfg_get(A), NULL), "toggle: the setting is not written before Apply");
    expect(is(opt_get(A), "1"), "toggle: the row reads the draft's value");
    OptDraftItem it;
    expect(opt_draft_count() == 1 && opt_draft_item(0, &it) && !strcmp(it.label, "Moon jump") &&
               !strcmp(it.from, "Off") && !strcmp(it.to, "On") && !it.restart && !it.display,
           "toggle: one draft line, label, from and to, no restart");

    opt_change(A, "", "Moon jump", "On", "Off");
    expect(opt_draft_any() == 0 && is(opt_get(A), NULL), "toggle back: the change leaves the draft");

    toggle_both();
    expect(opt_draft_count() == 2 && is(cfg_get(B), "1") && is(opt_get(B), "0"), "two toggles: two lines, B still on");
    opt_change(B, "1", "Infinite rupees", "Off", "On");
    expect(opt_draft_count() == 1 && opt_draft_any() == 1, "B back on: its change leaves the draft");
    opt_change(B, "0", "Infinite rupees", "On", "Off");
    opt_draft_discard();
    expect(opt_draft_any() == 0 && is(cfg_get(A), NULL) && is(cfg_get(B), "1"), "Discard: nothing changed");

    toggle_both();
    const unsigned before = cfg_version();
    expect(opt_draft_apply(true), "Apply: saved");
    expect(is(cfg_get(A), "1") && is(cfg_get(B), "0") && cfg_version() != before, "Apply: both settings written");
    expect(opt_draft_any() == 0 && opt_display_pending() == 0, "Apply: the draft is empty, nothing waits for Keep");
    return fails != 0;
}
