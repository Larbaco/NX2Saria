#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/menu_skin.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "cheat_stats.h"
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

static void count_is(const char *lang, UiTextId id, int n, const char *want) {
    char got[200], what[400];
    ui_text_count(lang, id, n, got, sizeof got);
    snprintf(what, sizeof what, "%s, %d: \"%s\"", lang, n, want);
    if (strcmp(got, want)) snprintf(what, sizeof what, "%s, %d: \"%s\", expected \"%s\"", lang, n, got, want);
    expect(!strcmp(got, want), what);
}

static void plurals(void) {
    static const struct { const char *lang; int n; const char *cheats, *files, *plain; } want[] = {
        {"en", 0, "0 code cheats", "0 ExeFS patch files", "0 cheats"},
        {"en", 1, "1 code cheat", "1 ExeFS patch file", "1 cheat"},
        {"en", 2, "2 code cheats", "2 ExeFS patch files", "2 cheats"},
        {"en", 5, "5 code cheats", "5 ExeFS patch files", "5 cheats"},
        {"pt-BR", 0, "0 cheats de código", "0 arquivos de patch ExeFS", "0 cheats"},
        {"pt-BR", 1, "1 cheat de código", "1 arquivo de patch ExeFS", "1 cheat"},
        {"pt-BR", 2, "2 cheats de código", "2 arquivos de patch ExeFS", "2 cheats"},
        {"pt-BR", 5, "5 cheats de código", "5 arquivos de patch ExeFS", "5 cheats"},
        {"zh-Hans", 0, "0 个修改代码的金手指", "0 个 ExeFS 补丁文件", "0 个金手指"},
        {"zh-Hans", 1, "1 个修改代码的金手指", "1 个 ExeFS 补丁文件", "1 个金手指"},
        {"zh-Hans", 2, "2 个修改代码的金手指", "2 个 ExeFS 补丁文件", "2 个金手指"},
        {"zh-Hans", 5, "5 个修改代码的金手指", "5 个 ExeFS 补丁文件", "5 个金手指"},
        {"ru", 0, "0 читов, меняющих код", "0 файлов патчей ExeFS", "0 читов"},
        {"ru", 1, "1 чит, меняющий код", "1 файл патчей ExeFS", "1 чит"},
        {"ru", 2, "2 чита, меняющих код", "2 файла патчей ExeFS", "2 чита"},
        {"ru", 5, "5 читов, меняющих код", "5 файлов патчей ExeFS", "5 читов"},
        {"ru", 11, "11 читов, меняющих код", "11 файлов патчей ExeFS", "11 читов"},
        {"ru", 21, "21 чит, меняющий код", "21 файл патчей ExeFS", "21 чит"},
        {"ru", 22, "22 чита, меняющих код", "22 файла патчей ExeFS", "22 чита"},
        {"de", 1, "1 code cheat", "1 ExeFS patch file", "1 cheat"},
        {"de", 2, "2 code cheats", "2 ExeFS patch files", "2 cheats"},
    };
    for (const auto &w : want) {
        count_is(w.lang, UI_MENU_COUNT_CODE_CHEATS, w.n, w.cheats);
        count_is(w.lang, UI_MENU_COUNT_EXEFS_FILES, w.n, w.files);
        count_is(w.lang, UI_MENU_COUNT_CHEATS, w.n, w.plain);
    }
    char a[200], b[200], line[512];
    snprintf(line, sizeof line, ui_text_in("en", UI_MENU_CHEATS_UNBUILT), ui_text_count("en", UI_MENU_COUNT_CODE_CHEATS, 1, a, sizeof a),
             ui_text_count("en", UI_MENU_COUNT_EXEFS_FILES, 2, b, sizeof b));
    expect(!strcmp(line, "1 code cheat and 2 ExeFS patch files are not built into the game: Play from the launcher "
                         "builds them in."), line);
    snprintf(line, sizeof line, ui_text_in("zh-Hans", UI_MENU_CHEATS_UNBUILT),
             ui_text_count("zh-Hans", UI_MENU_COUNT_CODE_CHEATS, 3, a, sizeof a),
             ui_text_count("zh-Hans", UI_MENU_COUNT_EXEFS_FILES, 1, b, sizeof b));
    expect(!strcmp(line, "有 3 个修改代码的金手指和 1 个 ExeFS 补丁文件尚未构建进游戏：在启动器中点击 Play 会把它们构建进去。"), line);
    snprintf(line, sizeof line, ui_text_in("ru", UI_MENU_CHEATS_MOD), "Mod", ui_text_count("ru", UI_MENU_COUNT_CHEATS, 3, a, sizeof a));
    expect(!strcmp(line, "Mod — 3 чита"), line);
}

static void stats(void) {
    const uint64_t s = 1000000000ull, t0 = 5 * s;
    CheatStats st = {};
    char line[400];
    const uint32_t none[CHEAT_WR_KINDS] = {0, 0, 0}, held[CHEAT_WR_KINDS] = {4, 0, 0}, one[CHEAT_WR_KINDS] = {0, 1, 0};
    const uint32_t mixed[CHEAT_WR_KINDS] = {1, 2, 3}, refused[CHEAT_WR_KINDS] = {0, 0, 1};
    expect(!strcmp(cheat_stats_line(&st, "Mod", "Hearts", "this session", line, sizeof line),
                   "Mod/Hearts: 0 writes in 0 frames, 0 unchanged, 0 refused this session"), "a cheat that never ran: its end line");
    cheat_stats_frame(&st, none);
    cheat_stats_frame(&st, held);
    expect(!cheat_stats_due(&st, t0), "frames without a write or a refusal: no line");
    cheat_stats_frame(&st, one);
    expect(cheat_stats_due(&st, t0), "the first write: a line at once");
    expect(!strcmp(cheat_stats_line(&st, "Mod", "Hearts", "so far", line, sizeof line),
                   "Mod/Hearts: 1 write in 3 frames, 4 unchanged, 0 refused so far"), line);
    expect(!cheat_stats_due(&st, t0 + s), "nothing new: no second line");
    for (int f = 0; f < 600; f++) cheat_stats_frame(&st, mixed);
    expect(!cheat_stats_due(&st, t0 + 59 * s), "still writing 59 s after the last line: no line");
    expect(cheat_stats_due(&st, t0 + 60 * s), "still writing a minute after the last line: a line");
    expect(!strcmp(cheat_stats_line(&st, "Mod", "Hearts", "so far", line, sizeof line),
                   "Mod/Hearts: 1201 writes in 603 frames, 604 unchanged, 1800 refused so far"), line);
    for (int f = 0; f < 10; f++) cheat_stats_frame(&st, held);
    expect(!cheat_stats_due(&st, t0 + 200 * s), "a minute on with nothing written since: no line");
    cheat_stats_frame(&st, refused);
    expect(cheat_stats_due(&st, t0 + 200 * s) && !cheat_stats_due(&st, t0 + 200 * s), "a refusal counts as acting: one line");
    cheat_stats_frame(&st, one);
    expect(!cheat_stats_due(&st, t0 + 259 * s) && cheat_stats_due(&st, t0 + 261 * s), "the minute counts from the last line");
    CheatStats single = {};
    cheat_stats_frame(&single, refused);
    expect(!strcmp(cheat_stats_line(&single, "Mod", "Bad", "this session", line, sizeof line),
                   "Mod/Bad: 0 writes in 1 frame, 0 unchanged, 1 refused this session"), line);
    expect(!strcmp(cheat_s(1), "") && !strcmp(cheat_s(0), "s") && !strcmp(cheat_s(2), "s"), "cheat_s: 1 alone is singular");
}

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

    plurals();
    stats();

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
