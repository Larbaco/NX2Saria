#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "cpu.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "layout.h"
#include "menu_mouse_geom.h"
#include "pause_tab.h"
#include "pause_tab_core.h"
#include "platform/platform.h"

#define UI_SYSTEM 0x4e982e0ull
#define UI_SYSTEM_VT 0x4c73ce0ull
#define MAIN_SIZE 0x4ea0000ull

enum { UI_SCREENS = 0x38, SM_FOCUS = 0x60, FM_PLAYERS = 0x22, FM_CONTROL = 0x28 };
enum { CTL_FOCUSED = 0x78, CTL_FLAGS = 0xb4 };
enum { ENT_BUTTON = 0x18, ENT_SCREEN = 0x20 };
enum { SCR_LAYOUT = 0x28, SCR_CONTROLLER = 0x38, SCR_HIDDEN = 0x1a5, SCR_STATE = 0x1a6, SCR_BLOCK0 = 0x1aa,
       SCR_BLOCK1 = 0x1ab, SCR_PROJ = 0x1c0, SCR_OPEN = 2 };
enum { BTN_ROOT = 0x58 };
enum { PANE_LAYOUT = 0x18, PANE_MTX = 0x28, PANE_SIZE = 0x58, PANE_ALPHA = 0x74, PANE_EXTRA = 0x80, PANE_FLAGS = 0xa8,
       PANE_STRIDE = 0xb0, PANE_SHOWN = 1 };
enum { LYT_NAME = 0xf8, LYT_PANES = 0x100, LYT_HOST = 0x108, LYT_TREE = 0x110 };
enum { HDR_COUNT = 0x328, HDR_CATS = 0x330, CAT_LAYOUT = 0x20, HDR_CATEGORIES = 4 };
enum { TREE_READ = 16 };
#define ARMED_MS 250
#define SEEN_MS 1500

static uint64_t main_lo, main_hi;

static int ok_at(uint64_t p, uint64_t n) { return p >= 0x10000 && p < (1ull << 47) && mem_host_readable(p, n); }
static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static uint16_t rd16(uint64_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
static uint8_t rd8(uint64_t a) { return *(const volatile uint8_t *)(uintptr_t)a; }
static float rdf(uint64_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static void wr32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static int game_object(uint64_t p, uint64_t n) {
    if ((p & 7) || !ok_at(p, n)) return 0;
    uint64_t vt = rd64(p);
    return vt >= main_lo && vt < main_hi;
}

int pause_tab_test(const char *word) {
    const char *v = cfg_get("NX2SARIA_PAUSE_SETTINGS_TAB_TEST");
    return v && strstr(v, word) != NULL;
}

static int sites_on(void) {
    static int mouse = -2, quit = -2;
    if (mouse == -2) mouse = rt_patch_set("menu_mouse"), quit = rt_patch_set("quit_menu");
    return rt_patch_enabled(mouse) || rt_patch_enabled(quit);
}

static int wanted(void) {
    const char *v = cfg_get("NX2SARIA_PAUSE_SETTINGS_TAB");
    return v && *v && strcmp(v, "0") != 0;
}

int pause_tab_on(void) {
    static volatile int32_t on = -1;
    int v = __atomic_load_n(&on, __ATOMIC_ACQUIRE);
    if (v < 0) v = RT_V111 && wanted() && sites_on(), __atomic_store_n(&on, v, __ATOMIC_RELEASE);
    return v;
}

int pause_tab_name_kind(void) {
    int known;
    return pt_name_kind(cfg_get("NX2SARIA_PAUSE_SETTINGS_TAB_NAME"), &known);
}

static void start_line(void) {
    int known;
    const char *name = cfg_get("NX2SARIA_PAUSE_SETTINGS_TAB_NAME");
    int kind = pt_name_kind(name, &known);
    if (!wanted()) {
        rt_log("pause_tab: off (NX2SARIA_PAUSE_SETTINGS_TAB %s): the pause menu has the game's four tabs",
               cfg_get("NX2SARIA_PAUSE_SETTINGS_TAB") ? "0" : "unset");
        return;
    }
    if (!RT_V111) {
        rt_log("pause_tab: off: game version %d is not 1.1.1, whose pause menu this tab reads", rt_game_version);
        return;
    }
    if (!sites_on()) {
        rt_log("pause_tab: off: the screens' site main+0x2d13b0 runs with the set menu_mouse or quit_menu, and both "
               "are off");
        return;
    }
    rt_log("pause_tab: on (NX2SARIA_PAUSE_SETTINGS_TAB): a fifth tab, %s%s, after the pause menu's System tab; R on "
           "that tab opens it, L, B or Plus closes it; its page: the Picture, Sound, Controls and Game rows",
           kind == PT_NAME_SETTINGS ? "the menu language's \"Settings\"" : "\"NX2Saria\"",
           known ? "" : " (NX2SARIA_PAUSE_SETTINGS_TAB_NAME unknown: product taken)");
}

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static PauseTabView g_view;
static uint64_t g_header_ms, g_page_ms;
static volatile uint64_t g_armed_ms;
static volatile int32_t g_req, g_open;
static uint64_t g_open_ms;

static uint64_t layout_of(uint64_t s) {
    uint64_t a = rd64(s + SCR_LAYOUT);
    return ok_at(a, 0x88) ? rd64(a + 0x80) : 0;
}

static int layout_named(uint64_t lyt, const char *want) {
    if (!ok_at(lyt, LYT_TREE + 8)) return 0;
    uint64_t r = rd64(lyt + LYT_NAME), name = ok_at(r, 0x20) ? rd64(r + 0x18) : 0;
    size_t len = strlen(want) + 1;
    return ok_at(name, len) && !memcmp((const void *)(uintptr_t)name, want, len);
}

static uint64_t pane_at(uint64_t lyt, int idx) {
    uint64_t arr = rd64(lyt + LYT_PANES), p = arr + (uint64_t)idx * PANE_STRIDE;
    return ok_at(p, PANE_STRIDE) && rd64(p + PANE_LAYOUT) == lyt ? p : 0;
}

static int tree_ok(uint64_t lyt, const PtLink *want, int wn) {
    uint64_t tree = rd64(lyt + LYT_TREE);
    uint16_t parents[TREE_READ];
    if (!ok_at(tree, TREE_READ * 8)) return 0;
    for (int i = 0; i < TREE_READ; i++) parents[i] = rd16(tree + (uint64_t)i * 8 + 2);
    for (int i = 0; i < wn; i++)
        if (!pane_at(lyt, want[i].index)) return 0;
    return pt_tree_ok(parents, TREE_READ, want, wn);
}

static int pane_alpha(uint64_t p) {
    float a = rdf(p + PANE_ALPHA) * 255.0f;
    return a >= 1.0f && a < 1e6f;
}

static uint64_t pane_parent(uint64_t p) {
    uint64_t lyt = rd64(p + PANE_LAYOUT);
    if (!ok_at(lyt, LYT_TREE + 8)) return 0;
    uint64_t arr = rd64(lyt + LYT_PANES);
    if (p < arr || (p - arr) % PANE_STRIDE) return 0;
    uint64_t idx = (p - arr) / PANE_STRIDE;
    if (!idx) return rd64(lyt + LYT_HOST);
    uint64_t tree = rd64(lyt + LYT_TREE);
    if (!ok_at(tree + idx * 8, 8)) return 0;
    uint16_t up = rd16(tree + idx * 8 + 2);
    return up == 0xffff ? 0 : arr + up * (uint64_t)PANE_STRIDE;
}

static int pane_shown(uint64_t p) {
    if (!ok_at(p, PANE_STRIDE) || !(rd32(p + PANE_FLAGS) & PANE_SHOWN) || !pane_alpha(p)) return 0;
    int depth = 0;
    for (uint64_t a = pane_parent(p); a; a = pane_parent(a)) {
        if (!ok_at(a, PANE_STRIDE) || ++depth > 48) return 0;
        uint32_t f = rd32(a + PANE_FLAGS);
        if (!(f & PANE_SHOWN) || ((f & 8) && !pane_alpha(a))) return 0;
    }
    return 1;
}

static float pane_chain_alpha(uint64_t p) {
    float alpha[49];
    uint32_t flags[49];
    int n = 0;
    for (uint64_t a = p; a && n < 49; a = pane_parent(a)) {
        if (!ok_at(a, PANE_STRIDE)) return 0.0f;
        alpha[n] = rdf(a + PANE_ALPHA), flags[n] = rd32(a + PANE_FLAGS), n++;
    }
    return pt_chain_alpha(alpha, flags, n);
}

static int pane_box(uint64_t p, PtBox *b) {
    MmBox m;
    if (!ok_at(p, PANE_STRIDE) ||
        !mm_pane_box(rdf(p + PANE_SIZE), rdf(p + PANE_SIZE + 4), rdf(p + PANE_MTX), rdf(p + PANE_MTX + 0x10),
                     rdf(p + PANE_MTX + 0x24), rdf(p + PANE_MTX + 0x28), rd32(p + PANE_FLAGS), rdf(p + PANE_EXTRA), &m))
        return 0;
    *b = (PtBox){m.x0, m.y0, m.x1, m.y1};
    return 1;
}

static void to_picture(const PtBox *b, uint64_t s, float out[4]) {
    float ux, uy, x0, y0, x1, y1;
    mm_units(rdf(s + SCR_PROJ), rdf(s + SCR_PROJ + 0x14), &ux, &uy);
    mm_to_picture(b->x0, b->y1, ux, uy, &x0, &y0);
    mm_to_picture(b->x1, b->y0, ux, uy, &x1, &y1);
    out[0] = x0, out[1] = y0, out[2] = x1, out[3] = y1;
}

static void trace(const char *fmt, const char *what) {
    static char last[160];
    static int lines;
    if (!pause_tab_test("log") || !strcmp(last, what)) return;
    snprintf(last, sizeof last, "%s", what);
    if (lines++ < 120) rt_log(fmt, what);
}

static int screen_takes_input(uint64_t s) {
    if (rd8(s + SCR_STATE) != SCR_OPEN || (int8_t)rd8(s + SCR_HIDDEN) < 0) return 0;
    if ((rd8(s + SCR_BLOCK0) & 1) || (rd8(s + SCR_BLOCK1) & 1)) return 0;
    uint64_t ctl = rd64(s + SCR_CONTROLLER);
    return ok_at(ctl, 0x48) && (rd8(ctl + 0x40) & 2);
}

static uint64_t focused_entry(void) {
    uint64_t sys = rd64(MOD_MAIN_BASE + UI_SYSTEM);
    if (!game_object(sys, UI_SCREENS + 8) || rd64(sys) != MOD_MAIN_BASE + UI_SYSTEM_VT) return 0;
    uint64_t mgr = rd64(sys + UI_SCREENS);
    if (!game_object(mgr, SM_FOCUS + 8)) return 0;
    uint64_t focus = rd64(mgr + SM_FOCUS);
    if (!game_object(focus, FM_CONTROL + 8) || !(rd8(focus + FM_PLAYERS) & 1)) return 0;
    uint64_t ctl = rd64(focus + FM_CONTROL);
    if (!game_object(ctl, CTL_FLAGS + 4)) return 0;
    uint64_t e = rd64(ctl + CTL_FOCUSED);
    return game_object(e, ENT_SCREEN + 8) ? e : 0;
}

static uint64_t system_layout_of(uint64_t entry, uint64_t plus) {
    uint64_t btn = rd64(entry + ENT_BUTTON), pane = ok_at(btn, BTN_ROOT + 8) ? rd64(btn + BTN_ROOT) : 0;
    for (int depth = 0; depth < 8 && ok_at(pane, PANE_STRIDE); depth++) {
        uint64_t lyt = rd64(pane + PANE_LAYOUT);
        if (!ok_at(lyt, LYT_TREE + 8) || lyt == plus) return 0;
        if (layout_named(lyt, "PaSystemMenu_00")) return lyt;
        pane = rd64(lyt + LYT_HOST);
    }
    return 0;
}

static uint64_t g_sys_lyt, g_inout_hidden;
static float g_last_x = 1e9f;

static void drop_system(void) {
    uint64_t p = g_inout_hidden;
    if (p && ok_at(p, PANE_STRIDE) && rd64(p + PANE_LAYOUT) == g_sys_lyt && layout_named(g_sys_lyt, "PaSystemMenu_00") &&
        pane_at(g_sys_lyt, PT_SYSTEM_INOUT) == p) {
        uint32_t f = rd32(p + PANE_FLAGS);
        wr32(p + PANE_FLAGS, f | PANE_SHOWN);
        rt_log("pause_tab: the System tab's page shown again as the tab let go of it (N_InOut_00 flags %#x -> %#x)", f,
               f | PANE_SHOWN);
    }
    g_inout_hidden = 0, g_sys_lyt = 0;
}

static uint64_t g_header_read_ms;

static void static_header(uint64_t s) {
    PtBox before, last, tab;
    float out[4] = {0, 0, 0, 0};
    int shown = rd8(s + SCR_STATE) == SCR_OPEN && (int8_t)rd8(s + SCR_HIDDEN) >= 0;
    pt_header_boxes(&before, &last);
    if (shown && pt_tab_box(&before, &last, &tab)) to_picture(&tab, s, out);
    else shown = 0;
    plat_mutex_lock(&g_lock);
    memcpy(g_view.tab, out, sizeof out);
    g_view.alpha = shown ? 1.0f : 0.0f;
    g_header_ms = shown ? plat_now_ms() : g_header_ms;
    plat_mutex_unlock(&g_lock);
}

static void plus_tick(uint64_t s) {
    uint64_t now = plat_now_ms(), plus = layout_of(s), e = focused_entry(), sys = 0;
    const char *why = NULL;
    if (!ok_at(plus, LYT_TREE + 8)) why = "the pause menu's layout cannot be read";
    else if (!e || rd64(e + ENT_SCREEN) != s) why = "the focus is not on the pause menu";
    else if (!screen_takes_input(s)) why = "the pause menu takes no input";
    else if (!(sys = system_layout_of(e, plus))) why = "the focus is not on the System tab";
    int open = __atomic_load_n(&g_open, __ATOMIC_ACQUIRE);
    if (sys && sys != g_sys_lyt) drop_system(), g_sys_lyt = sys;
    uint64_t host = ok_at(g_sys_lyt, LYT_TREE + 8) ? rd64(g_sys_lyt + LYT_HOST) : 0;
    if (host && (!ok_at(host, PANE_STRIDE) || rd64(host + PANE_LAYOUT) != plus)) drop_system(), host = 0;
    int shape = g_sys_lyt && tree_ok(g_sys_lyt, PT_SYSTEM_TREE, (int)(sizeof PT_SYSTEM_TREE / sizeof *PT_SYSTEM_TREE));
    if (!why && !shape) why = "the System tab's layout is not the one this tab knows";
    float x = host ? rdf(host + PANE_MTX + 0x24) : 1e9f;
    if (!why && !pt_settled(x, g_last_x, 2.0f)) why = "the tabs are moving";
    g_last_x = x;
    if (!why) __atomic_store_n(&g_armed_ms, now, __ATOMIC_RELEASE);
    trace("pause_tab: R not taken: %s", why ? why : "armed: R opens the tab");
    uint64_t inout = shape ? pane_at(g_sys_lyt, PT_SYSTEM_INOUT) : 0;
    if (open && inout) {
        uint32_t f = rd32(inout + PANE_FLAGS);
        if (!g_inout_hidden && (f & PANE_SHOWN)) {
            wr32(inout + PANE_FLAGS, f & ~(uint32_t)PANE_SHOWN);
            g_inout_hidden = inout;
            rt_log("pause_tab: the System tab's page hidden (N_InOut_00 flags %#x -> %#x)", f, f & ~(uint32_t)PANE_SHOWN);
        }
    } else if (!open && g_inout_hidden && g_inout_hidden == inout) {
        uint32_t f = rd32(inout + PANE_FLAGS);
        wr32(inout + PANE_FLAGS, f | PANE_SHOWN);
        g_inout_hidden = 0;
        rt_log("pause_tab: the System tab's page shown again (N_InOut_00 flags %#x -> %#x)", f, f | PANE_SHOWN);
    }
    PtBox page;
    float out[4], ux, uy;
    mm_units(rdf(s + SCR_PROJ), rdf(s + SCR_PROJ + 0x14), &ux, &uy);
    pt_page_box(&page, ux, strcmp(cfg_value("NX2SARIA_WIDE_MENUS"), "center") != 0);
    to_picture(&page, s, out);
    plat_mutex_lock(&g_lock);
    memcpy(g_view.page, out, sizeof out);
    g_page_ms = shape && host ? now : g_page_ms;
    plat_mutex_unlock(&g_lock);
    if (now - g_header_read_ms > 200) static_header(s);
}

static uint64_t g_bg_hidden;

static uint64_t category_layout(uint64_t cat) {
    uint64_t holder = game_object(cat, CAT_LAYOUT + 8) ? rd64(cat + CAT_LAYOUT) : 0;
    return ok_at(holder, 0x88) ? rd64(holder + 0x80) : 0;
}

static volatile int32_t g_five;
static int g_five_failed, g_five_pending, g_trans = -1;
enum { FIVE_NOT_SEEN, FIVE_TOOK, FIVE_PUT_BACK, FIVE_ELSEWHERE, FIVE_NO_OFFSET, FIVE_SHAPE };
static volatile int32_t g_five_state;
static uint64_t g_five_ms;

int pause_tab_five(void) { return __atomic_load_n(&g_five, __ATOMIC_ACQUIRE); }

static int moved_panes(uint64_t s, uint64_t cats, uint64_t p[PT_MOVED]) {
    uint64_t lyt = layout_of(s);
    if (!tree_ok(lyt, PT_OVERLAY_TREE, (int)(sizeof PT_OVERLAY_TREE / sizeof *PT_OVERLAY_TREE))) return 0;
    for (int i = 0; i < HDR_CATEGORIES; i++) {
        uint64_t cl = category_layout(rd64(cats + (uint64_t)i * 8));
        p[i] = ok_at(cl, LYT_TREE + 8) ? rd64(cl + LYT_HOST) : 0;
        if (!p[i] || p[i] != pane_at(lyt, PT_OVERLAY_FIRST_CATEGORY + i)) return 0;
    }
    p[4] = pane_at(lyt, PT_OVERLAY_L), p[5] = pane_at(lyt, PT_OVERLAY_R);
    return p[4] && p[5];
}

static void place(const uint64_t p[PT_MOVED], const float x[PT_MOVED]) {
    for (int i = 0; i < PT_MOVED; i++) memcpy((void *)(uintptr_t)(p[i] + (uint64_t)g_trans), &x[i], 4);
}

static int find_trans(const uint64_t p[PT_MOVED]) {
    float words[PT_MOVED][PANE_STRIDE / 4];
    const float *rows[PT_MOVED];
    for (int i = 0; i < PT_MOVED; i++) memcpy(words[i], (const void *)(uintptr_t)p[i], PANE_STRIDE), rows[i] = words[i];
    int k = pt_find_trans(rows, PANE_STRIDE / 4, PANE_MTX / 4, PANE_SIZE / 4);
    if (k >= 0) rt_log("pause_tab: the header panes' own position found at +%#x (the game's x and y in all six)", k * 4);
    else {
        __atomic_store_n(&g_five_state, FIVE_NO_OFFSET, __ATOMIC_RELAXED);
        rt_log("pause_tab: the header panes' own position %s: the four categories keep their places and the tab stands "
               "after the R icon", k == -2 ? "is at more than one place" : "was not found");
    }
    return k >= 0 ? k * 4 : -1;
}

static void respace(uint64_t s, uint64_t cats, int want) {
    static int said;
    uint64_t p[PT_MOVED];
    if (g_five_failed) return;
    if (!moved_panes(s, cats, p)) {
        g_five_failed = 1;
        __atomic_store_n(&g_five_state, FIVE_SHAPE, __ATOMIC_RELAXED);
        rt_log("pause_tab: the header's panes are not the ones this tab knows: the four categories keep their places "
               "and the tab stands after the R icon");
        return;
    }
    if (g_trans < 0) {
        if (!want) return;
        if ((g_trans = find_trans(p)) < 0) {
            g_five_failed = 1;
            return;
        }
    }
    float x[PT_MOVED], y[PT_MOVED];
    for (int i = 0; i < PT_MOVED; i++) x[i] = rdf(p[i] + (uint64_t)g_trans), y[i] = rdf(p[i] + (uint64_t)g_trans + 4);
    int as = pt_spacing(x), ys = fabsf(y[3]) < 0.5f && fabsf(y[5] - 2.0f) < 0.5f;
    uint64_t now = plat_now_ms();
    if ((as == PT_AS_OTHER || !ys) && !want) return;
    if (as == PT_AS_OTHER || !ys) {
        g_five_failed = 1;
        if (__atomic_load_n(&g_five, __ATOMIC_ACQUIRE)) place(p, PT_GAME_X);
        __atomic_store_n(&g_five, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_five_state, FIVE_ELSEWHERE, __ATOMIC_RELAXED);
        rt_log("pause_tab: the header's panes are at x %.1f %.1f %.1f %.1f, L %.1f, R %.1f (y %.1f, %.1f), not where the "
               "game or this tab puts them: they are left alone and the tab stands after the R icon",
               (double)x[0], (double)x[1], (double)x[2], (double)x[3], (double)x[4], (double)x[5], (double)y[3],
               (double)y[5]);
        return;
    }
    if (want && as == PT_AS_GAME) {
        place(p, PT_FIVE_X);
        g_five_pending = 1, g_five_ms = now;
        if (said++ < 8) rt_log("pause_tab: the header's categories moved to make room for a fifth (x -840, -420, 0, 420; L and R at -1100 and 1100)");
    } else if (!want && as == PT_AS_FIVE) {
        place(p, PT_GAME_X);
        g_five_pending = 0;
        __atomic_store_n(&g_five, 0, __ATOMIC_RELEASE);
        if (said++ < 8) rt_log("pause_tab: the header's categories back where the game puts them (the Minus menu's header)");
    }
    if (g_five_pending && now - g_five_ms > 150) {
        PtBox b;
        g_five_pending = 0;
        float cx = pane_box(p[3], &b) ? (b.x0 + b.x1) * 0.5f : 1e9f;
        if (fabsf(cx - PT_FIVE_X[3]) < 3.0f && !pause_tab_test("nofive")) {
            __atomic_store_n(&g_five, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_five_state, FIVE_TOOK, __ATOMIC_RELAXED);
            rt_log("pause_tab: the header's new places took effect (the System category's centre at x %.1f)", (double)cx);
        } else {
            place(p, PT_GAME_X);
            g_five_failed = 1;
            __atomic_store_n(&g_five_state, FIVE_PUT_BACK, __ATOMIC_RELAXED);
            float f[10];
            memcpy(f, (const void *)(uintptr_t)(p[3] + PANE_SIZE), sizeof f);
            rt_log("pause_tab: the header's new places did not take effect%s (the System category's centre at x %.1f, "
                   "not %.0f; its pane from +0x58: %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f): put back, and "
                   "the tab stands after the R icon", pause_tab_test("nofive") ? " (NX2SARIA_PAUSE_SETTINGS_TAB_TEST=nofive)" : "",
                   (double)cx, (double)PT_FIVE_X[3], (double)f[0], (double)f[1],
                   (double)f[2], (double)f[3], (double)f[4], (double)f[5], (double)f[6], (double)f[7], (double)f[8],
                   (double)f[9]);
        }
    }
}

static int header_tick(uint64_t s) {
    static int reported;
    uint64_t now = plat_now_ms();
    if (!ok_at(s, HDR_CATS + 8)) return 0;
    uint32_t n = rd32(s + HDR_COUNT);
    uint64_t cats = rd64(s + HDR_CATS);
    if (n != HDR_CATEGORIES || !ok_at(cats, HDR_CATEGORIES * 8)) {
        if (!reported++)
            rt_log("pause_tab: the pause menu's header has %u categories, not %d: the tab stands where the game's layout "
                   "puts them", n, HDR_CATEGORIES);
        return 0;
    }
    uint64_t host[2] = {0, 0}, last_lyt = 0;
    PtBox box[2];
    for (int i = 0; i < 2; i++) {
        uint64_t lyt = category_layout(rd64(cats + (uint64_t)(2 + i) * 8));
        host[i] = ok_at(lyt, LYT_TREE + 8) ? rd64(lyt + LYT_HOST) : 0;
        if (!pane_box(host[i], &box[i])) {
            static int lines;
            if (lines++ < 4)
                rt_log("pause_tab: the header's category %d has no box (layout %#llx, its part pane %#llx): the tab stands "
                       "where the game's layout puts it", 2 + i, (unsigned long long)lyt, (unsigned long long)host[i]);
            return 0;
        }
        if (i) last_lyt = lyt;
    }
    PtBox tab;
    respace(s, cats, (rd32(host[1] + PANE_FLAGS) & PANE_SHOWN) != 0);
    int shown = pane_shown(host[1]) && (pause_tab_five() ? (pt_five_box(&box[1], &tab), 1) : pt_tab_box(&box[0], &box[1], &tab));
    float out[4] = {0, 0, 0, 0};
    if (shown) to_picture(&tab, s, out);
    int open = __atomic_load_n(&g_open, __ATOMIC_ACQUIRE);
    int shape = tree_ok(last_lyt, PT_CATEGORY_TREE, (int)(sizeof PT_CATEGORY_TREE / sizeof *PT_CATEGORY_TREE));
    uint64_t bg = shape ? pane_at(last_lyt, PT_CATEGORY_BG) : 0;
    if (open && bg && !g_bg_hidden) {
        uint32_t f = rd32(bg + PANE_FLAGS);
        if (f & PANE_SHOWN) wr32(bg + PANE_FLAGS, f & ~(uint32_t)PANE_SHOWN), g_bg_hidden = bg;
    } else if (!open && g_bg_hidden) {
        if (g_bg_hidden == bg) wr32(bg + PANE_FLAGS, rd32(bg + PANE_FLAGS) | PANE_SHOWN);
        else rt_log("pause_tab: the header changed while the tab was open: the System category's plate left as it is");
        g_bg_hidden = 0;
    }
    static int once;
    if (shown && !once++)
        rt_log("pause_tab: header: the System category at x %.0f..%.0f, the tab at %.0f..%.0f (layout units); its plate "
               "%s", (double)box[1].x0, (double)box[1].x1, (double)tab.x0, (double)tab.x1,
               shape ? "found" : "not found: it stays while the tab is open");
    plat_mutex_lock(&g_lock);
    memcpy(g_view.tab, out, sizeof out);
    uint64_t text = shape ? pane_at(last_lyt, PT_CATEGORY_TEXT) : 0;
    g_view.alpha = shown ? pane_chain_alpha(text ? text : host[1]) : 0.0f;
    g_header_ms = shown ? now : g_header_ms;
    plat_mutex_unlock(&g_lock);
    return 1;
}

static int layout_is(uint64_t s, const char *want) { return layout_named(layout_of(s), want); }

static void screens_seen(uint64_t s) {
    static uint64_t seen[48];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i] == s) return;
    if (n >= 48) return;
    seen[n++] = s;
    uint64_t lyt = layout_of(s), r = ok_at(lyt, LYT_NAME + 8) ? rd64(lyt + LYT_NAME) : 0,
             name = ok_at(r, 0x20) ? rd64(r + 0x18) : 0;
    char text[40] = "?";
    if (ok_at(name, sizeof text)) {
        memcpy(text, (const void *)(uintptr_t)name, sizeof text - 1);
        text[sizeof text - 1] = 0;
    }
    rt_log("pause_tab: the screens' site runs for %s (%#llx)", text, (unsigned long long)s);
}

void pause_tab_screen_host(Cpu *c) {
    uint64_t s = c->x[19];
    if (!pause_tab_on()) return;
    if (!main_hi) main_lo = MOD_MAIN_BASE, main_hi = MOD_MAIN_BASE + MAIN_SIZE;
    if (!game_object(s, SCR_PROJ + 64)) return;
    if (pause_tab_test("screens")) screens_seen(s);
    if (layout_is(s, "PlusMenu_00")) plus_tick(s);
    else if (layout_is(s, "MenuOverlay_00") && header_tick(s)) g_header_read_ms = plat_now_ms();
}

int pause_tab_view(PauseTabView *v) {
    if (!pause_tab_on()) return 0;
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&g_lock);
    *v = g_view;
    v->header = now - g_header_ms < 100;
    v->page_ok = now - g_page_ms < SEEN_MS;
    plat_mutex_unlock(&g_lock);
    return v->header;
}

int pause_tab_request_take(void) { return plat_xchg32(&g_req, 0); }

static volatile uint64_t g_click_ms;

void pause_tab_click_wanted(void) { __atomic_store_n(&g_click_ms, plat_now_ms(), __ATOMIC_RELEASE); }

static volatile int32_t g_opened_r, g_opened_click, g_applies, g_changes, g_more_times;
static const char *const CLOSE_WHY[] = {"L", "B", "Plus", "Apply", "Discard", "More settings"};
enum { CLOSE_KINDS = sizeof CLOSE_WHY / sizeof *CLOSE_WHY };
static volatile int32_t g_closed[CLOSE_KINDS + 1];

void pause_tab_set_open(int open, const char *why) {
    static int lines;
    int was = plat_xchg32(&g_open, open);
    if (!open) plat_xchg32(&g_req, 0);
    if (was == open) return;
    if (!open) {
        int k = 0;
        while (k < CLOSE_KINDS && strcmp(why ? why : "", CLOSE_WHY[k])) k++;
        __atomic_add_fetch(&g_closed[k], 1, __ATOMIC_RELAXED);
    }
    uint64_t now = plat_now_ms();
    if (open) g_open_ms = now;
    if (lines++ < 64) {
        if (open) rt_log("pause_tab: the tab opened");
        else rt_log("pause_tab: the tab closed (%s) after %llu ms", why ? why : "?", (unsigned long long)(now - g_open_ms));
    }
}

void pause_tab_applied(int changes, const char *list) {
    static int lines;
    __atomic_add_fetch(&g_applies, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_changes, changes, __ATOMIC_RELAXED);
    if (lines++ < 32) rt_log("pause_tab: Apply from the tab: %d change(s): %s", changes, list && *list ? list : "none");
}

void pause_tab_poll(uint64_t *buttons) {
    static PtPress press;
    static volatile int32_t busy, held, said;
    const uint64_t r = 1ull << B_R;
    if (!plat_xchg32(&said, 1)) start_line();
    if (!pause_tab_on()) return;
    if (plat_xchg32(&busy, 1)) {
        if (__atomic_load_n(&held, __ATOMIC_RELAXED)) *buttons &= ~r;
        return;
    }
    uint64_t now = plat_now_ms(), armed_at = __atomic_load_n(&g_armed_ms, __ATOMIC_ACQUIRE);
    PauseTabView v;
    int armed = armed_at && now - armed_at < ARMED_MS && !__atomic_load_n(&g_open, __ATOMIC_ACQUIRE) &&
                !__atomic_load_n(&g_req, __ATOMIC_ACQUIRE) && pause_tab_view(&v);
    static uint32_t clicks_seen;
    static uint64_t polled;
    uint32_t clicks = input_menu_mouse_presses(0);
    int click = clicks != clicks_seen && now - polled < 100;
    clicks_seen = clicks, polled = now;
    float nx, ny, pic[4];
    if (click && armed && input_menu_pointer(&nx, &ny) && gfx_picture_rect(pic) && pic[2] > 0 && pic[3] > 0) {
        float px = (nx - pic[0]) / pic[2], py = (ny - pic[1]) / pic[3];
        click = px >= v.tab[0] && px <= v.tab[2] && py >= v.tab[1] && py <= v.tab[3];
    } else click = 0;
    uint64_t wanted_at = __atomic_load_n(&g_click_ms, __ATOMIC_ACQUIRE);
    if (wanted_at && now - wanted_at > 4000) __atomic_store_n(&g_click_ms, 0, __ATOMIC_RELEASE), wanted_at = 0;
    if (armed && wanted_at) {
        __atomic_store_n(&g_click_ms, 0, __ATOMIC_RELEASE);
        click = 1;
    }
    int opened = 0, swallow = pt_press(&press, armed, (*buttons & r) != 0, &opened);
    if (click && !opened) {
        static int lines;
        if (lines++ < 32) rt_log("pause_tab: a click on the tab: the tab opens");
        __atomic_add_fetch(&g_opened_click, 1, __ATOMIC_RELAXED);
        plat_xchg32(&g_req, 1);
    }
    __atomic_store_n(&held, swallow, __ATOMIC_RELAXED);
    if (swallow) *buttons &= ~r;
    if (opened) {
        static int lines;
        if (lines++ < 32) rt_log("pause_tab: R on the System tab: the tab opens");
        __atomic_add_fetch(&g_opened_r, 1, __ATOMIC_RELAXED);
        plat_xchg32(&g_req, 1);
    }
    plat_xchg32(&busy, 0);
}

void pause_tab_more(int open, uint64_t ms) {
    static int lines;
    if (lines++ >= 32) return;
    if (open) {
        __atomic_add_fetch(&g_more_times, 1, __ATOMIC_RELAXED);
        rt_log("pause_tab: more settings: the settings menu opens on its Controls tab over the paused game; the tab gives "
               "the System tab's page back under it");
        return;
    }
    uint64_t seen = plat_now_ms() - g_page_ms;
    rt_log("pause_tab: more settings: the settings menu closed after %llu ms; %s", (unsigned long long)ms,
           g_page_ms && seen < SEEN_MS ? "the pause menu is still open under it, the game still paused"
                                       : "the pause menu is not seen any more (the game closed it meanwhile)");
}

void pause_tab_exit_log(void) {
    static const char *const header[] = {
        "never shown", "in five places", "put back (the new places did not take)",
        "left alone (its panes not where the game puts them)", "left alone (the panes' own position not found)",
        "left alone (its panes not the ones this tab knows)"};
    if (!pause_tab_on()) return;
    int r = __atomic_load_n(&g_opened_r, __ATOMIC_RELAXED), c = __atomic_load_n(&g_opened_click, __ATOMIC_RELAXED);
    int st = __atomic_load_n(&g_five_state, __ATOMIC_RELAXED);
    if (st < 0 || st > FIVE_SHAPE) st = FIVE_NOT_SEEN;
    char closed[200];
    int at = 0;
    for (int k = 0; k <= CLOSE_KINDS && at < (int)sizeof closed; k++)
        at += snprintf(closed + at, sizeof closed - (size_t)at, "%s%s %d", k ? ", " : "", k < CLOSE_KINDS ? CLOSE_WHY[k] : "other",
                       __atomic_load_n(&g_closed[k], __ATOMIC_RELAXED));
    rt_log("pause_tab: session: the tab opened %d time(s) (R %d, a click %d); closed by %s; %d Apply with %d change(s); "
           "More settings %d; the header %s", r + c, r, c, closed, __atomic_load_n(&g_applies, __ATOMIC_RELAXED),
           __atomic_load_n(&g_changes, __ATOMIC_RELAXED), __atomic_load_n(&g_more_times, __ATOMIC_RELAXED), header[st]);
}
