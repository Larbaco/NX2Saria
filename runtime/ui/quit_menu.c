#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "cpu.h"
#include "fs.h"
#include "host.h"
#include "input_map.h"
#include "layout.h"
#include "platform/platform.h"
#include "quit_menu.h"
#include "quit_menu_core.h"
#include "ui/menu.h"

#define UI_SYSTEM 0x4e982e0ull
#define UI_SYSTEM_VT 0x4c73ce0ull
#define MAIN_SIZE 0x4ea0000ull

enum { UI_SCREENS = 0x38, SM_FOCUS = 0x60, FM_CONTROL = 0x28 };
enum { CTL_FOCUSED = 0x78, CTL_RESERVED = 0x80, CTL_FLAGS = 0xb4 };
enum { ENT_BUTTON = 0x18, ENT_SCREEN = 0x20 };
enum { SCR_LAYOUT = 0x28, SCR_HEAD = 0x88, SCR_FIRST = 0x90, SCR_LINK = 0x9c, SCR_STATE = 0x1a6, SCR_PROJ = 0x1c0,
       SCR_OPEN = 2 };
enum { BTN_FLAGS = 0x40, BTN_ROOT = 0x58, BTN_ENABLED = 0x10 };
enum { PANE_LAYOUT = 0x18, PANE_MTX = 0x28, PANE_ALPHA = 0x74, PANE_FLAGS = 0xa8, PANE_STRIDE = 0xb0, PANE_SHOWN = 1 };
enum { LYT_PANES = 0x100, LYT_HOST = 0x108, LYT_TREE = 0x110 };
enum { TITLE_COUNT = 0x2c8, TITLE_BUTTONS = 0x2d0, TITLE_MENU = 0x2e8, TITLE_ENTRIES = 4, TITLE_SPARE = 3, TITLE_PART_DEPTH = 3 };
enum { WINDOW_KIND_TITLE = 2, WINDOW_ENTRIES = 3, WINDOW_SPARE = 2 };
enum { AT_NONE, AT_TITLE, AT_PAUSE };
#define ARMED_MS 250

static uint64_t main_lo, main_hi;
static volatile int32_t g_texts, g_title, g_pause, g_quit;
static volatile uint64_t g_title_ms, g_pause_ms, g_window, g_chosen_ms;

static int ok_at(uint64_t p, uint64_t n) { return p >= 0x10000 && p < (1ull << 47) && mem_host_readable(p, n); }
static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static uint8_t rd8(uint64_t a) { return *(const volatile uint8_t *)(uintptr_t)a; }
static float rdf(uint64_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static int game_object(uint64_t p, uint64_t n) {
    if ((p & 7) || !ok_at(p, n)) return 0;
    uint64_t vt = rd64(p);
    return vt >= main_lo && vt < main_hi;
}

static int test_flag(const char *name) {
    const char *v = cfg_get("NX2SARIA_QUIT_MENU_TEST");
    return v && strstr(v, name) != NULL;
}

int quit_menu_wanted(void) {
    static int set = -2;
    if (set == -2) set = rt_patch_set("quit_menu");
    return RT_V111 && rt_patch_enabled(set);
}

typedef struct { uint32_t at, word; } Site;
static const Site title_sites[] = {{0x2d13b0, 0xaa0003f3}, {0x3405bfc, 0x2a1303e3}, {0x3406964, 0xd0004fe2},
                                   {0x3406968, 0x91023842}};
static const Site pause_sites[] = {{0x2d13b0, 0xaa0003f3}, {0x3405bfc, 0x2a1303e3}, {0x3405b04, 0x1e201000},
                                   {0x3405b38, 0x52800021}, {0x3405b3c, 0x52800022}};

static int sites_ok(const Site *sites, size_t n, const char *off_flag, char *why, size_t why_n) {
    if (test_flag(off_flag)) return snprintf(why, why_n, "NX2SARIA_QUIT_MENU_TEST=%s", off_flag), 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t a = MOD_MAIN_BASE + sites[i].at;
        if (!ok_at(a, 4)) return snprintf(why, why_n, "main+%#x cannot be read", sites[i].at), 0;
        if (rd32(a) != sites[i].word)
            return snprintf(why, why_n, "main+%#x holds %08x, not the %08x its site expects", sites[i].at, rd32(a),
                            sites[i].word), 0;
    }
    return 1;
}

static int title_on(void) { return quit_menu_wanted() && __atomic_load_n(&g_title, __ATOMIC_RELAXED); }
static int pause_on(void) { return quit_menu_wanted() && __atomic_load_n(&g_pause, __ATOMIC_RELAXED); }
static int enabled(void) { return title_on() || pause_on(); }

void quit_menu_texts(int made, int archives, const char *language, double ms, const char *why) {
    int set = rt_patch_set("quit_menu");
    if (!RT_V111) return;
    if (!quit_menu_wanted()) {
        rt_log("quit_menu: off (set quit_menu %s, NX2SARIA_QUIT_MENU): the game's menus have no Quit Game",
               set < 0 ? "missing from this build" : "off");
        return;
    }
    if (!(archives > 0 && made == archives)) {
        rt_log("quit_menu: off: the labels are in %d of %d message archives for the language %s (%s); the game's menus "
               "stay as they are", made, archives, language, why && *why ? why : "no message archive in the RomFS");
        return;
    }
    __atomic_store_n(&g_texts, 1, __ATOMIC_RELEASE);
    char title_why[120] = "", pause_why[120] = "";
    int title = sites_ok(title_sites, sizeof title_sites / sizeof *title_sites, "notitle", title_why, sizeof title_why);
    int pause = sites_ok(pause_sites, sizeof pause_sites / sizeof *pause_sites, "nopause", pause_why, sizeof pause_why);
    __atomic_store_n(&g_title, title, __ATOMIC_RELEASE);
    __atomic_store_n(&g_pause, pause, __ATOMIC_RELEASE);
    rt_log("quit_menu: %s (NX2SARIA_QUIT_MENU): title screen %s%s%s%s, pause menu's Title Screen confirmation %s%s%s%s; "
           "the game's own labels in %d message archive(s) for the language %s (%.0f ms)", title || pause ? "on" : "off",
           title ? "on" : "off", title ? "" : " (", title_why, title ? "" : ")", pause ? "on" : "off", pause ? "" : " (",
           pause_why, pause ? "" : ")", archives, language, ms);
}

static int screen_is(uint64_t s, const char *want) {
    uint64_t a = rd64(s + SCR_LAYOUT), lyt = ok_at(a, 0x88) ? rd64(a + 0x80) : 0, r = ok_at(lyt, 0x100) ? rd64(lyt + 0xf8) : 0,
             name = ok_at(r, 0x20) ? rd64(r + 0x18) : 0;
    size_t len = strlen(want) + 1;
    return ok_at(name, len) && !memcmp((const void *)(uintptr_t)name, want, len);
}

static uint64_t focused_entry(void) {
    uint64_t sys = rd64(MOD_MAIN_BASE + UI_SYSTEM);
    if (!game_object(sys, UI_SCREENS + 8) || rd64(sys) != MOD_MAIN_BASE + UI_SYSTEM_VT) return 0;
    uint64_t mgr = rd64(sys + UI_SCREENS);
    if (!game_object(mgr, SM_FOCUS + 8)) return 0;
    uint64_t focus = rd64(mgr + SM_FOCUS);
    if (!game_object(focus, FM_CONTROL + 8)) return 0;
    uint64_t ctl = rd64(focus + FM_CONTROL);
    if (!game_object(ctl, CTL_FLAGS + 4)) return 0;
    uint64_t e = rd64(ctl + CTL_FOCUSED);
    return game_object(e, ENT_SCREEN + 8) ? e : 0;
}

static int entries_of(uint64_t s, uint64_t *out, int max) {
    uint64_t head = s + SCR_HEAD;
    int32_t off = (int32_t)rd32(s + SCR_LINK);
    int n = 0, guard = 0;
    if (off < 0 || off > 0x100) return 0;
    for (uint64_t link = rd64(s + SCR_FIRST); link && link != head && guard < 64; link = rd64(link + 8), guard++) {
        if (!ok_at(link, 16)) break;
        uint64_t e = link - (uint64_t)off;
        if (!ok_at(e, ENT_SCREEN + 8) || rd64(e + ENT_SCREEN) != s) continue;
        if (n < max) out[n] = e;
        n++;
    }
    return n;
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
    uint16_t up;
    memcpy(&up, (const void *)(uintptr_t)(tree + idx * 8 + 2), 2);
    return up == 0xffff ? 0 : arr + up * (uint64_t)PANE_STRIDE;
}

static int pane_alpha(uint64_t p) {
    float a = rdf(p + PANE_ALPHA) * 255.0f;
    return a >= 1.0f && a < 1e6f;
}

static int hidden_at(uint64_t p, uint64_t *where) {
    int depth = 0;
    *where = 0;
    if (!ok_at(p, PANE_STRIDE)) return -1;
    if (!(rd32(p + PANE_FLAGS) & PANE_SHOWN) || !pane_alpha(p)) return *where = p, 0;
    for (uint64_t a = pane_parent(p); a; a = pane_parent(a)) {
        if (!ok_at(a, PANE_STRIDE) || ++depth > 48) return -1;
        uint32_t f = rd32(a + PANE_FLAGS);
        if (!(f & PANE_SHOWN) || ((f & 8) && !pane_alpha(a))) return *where = a, depth;
    }
    return -2;
}

static void describe(uint64_t s, char *out, size_t cap) {
    uint64_t e[8], focused = focused_entry();
    int n = entries_of(s, e, 8), at = snprintf(out, cap, "state %d, menu %d, %d entries:", rd8(s + SCR_STATE),
                                               rd8(s + TITLE_MENU), n);
    for (int i = 0; i < n && i < 8 && at > 0 && at < (int)cap; i++) {
        uint64_t btn = rd64(e[i] + ENT_BUTTON), root = ok_at(btn, BTN_ROOT + 8) ? rd64(btn + BTN_ROOT) : 0, where = 0;
        int depth = hidden_at(root, &where);
        at += snprintf(out + at, cap - at, " [%d]%s flags %#x y %.0f", i, e[i] == focused ? "*" : "",
                       ok_at(btn, BTN_FLAGS + 4) ? rd32(btn + BTN_FLAGS) : 0,
                       ok_at(root, PANE_STRIDE) ? (double)rdf(root + PANE_MTX + 0x28) : 0.0);
        if (depth == -2) at += snprintf(out + at, cap - at, " shown");
        else if (at < (int)cap)
            at += snprintf(out + at, cap - at, " hidden at ancestor %d (flags %#x alpha %.2f)", depth,
                           where ? rd32(where + PANE_FLAGS) : 0, where ? (double)rdf(where + PANE_ALPHA) : 0.0);
    }
}

static void trace(const char *what, uint64_t s) {
    static char last[2][900];
    static int lines;
    char now[900];
    int slot = what[0] == 't';
    if (!test_flag("log")) return;
    describe(s, now, sizeof now);
    if (!strcmp(now, last[slot])) return;
    snprintf(last[slot], sizeof last[slot], "%s", now);
    if (lines++ < 120) rt_log("quit_menu: %s: %s", what, now);
}

static void title_tick(uint64_t s) {
    static int reported;
    uint64_t e[TITLE_ENTRIES];
    int n = entries_of(s, e, TITLE_ENTRIES);
    uint32_t count = ok_at(s, TITLE_MENU + 1) ? rd32(s + TITLE_COUNT) : 0;
    trace("title", s);
    if (n != TITLE_ENTRIES || count != TITLE_ENTRIES) {
        if (!reported++)
            rt_log("quit_menu: title: the screen has %d entries and %u buttons, not %d: no Quit Game there", n, count,
                   TITLE_ENTRIES);
        __atomic_store_n(&g_title_ms, 0, __ATOMIC_RELEASE);
        return;
    }
    uint64_t btn = rd64(e[TITLE_SPARE] + ENT_BUTTON), root = ok_at(btn, BTN_ROOT + 8) ? rd64(btn + BTN_ROOT) : 0;
    if (!ok_at(root, PANE_STRIDE)) return;
    uint64_t where = 0;
    int depth = hidden_at(root, &where);
    if (depth > 0 && depth <= TITLE_PART_DEPTH) {
        static int lines;
        uint32_t was = rd32(where + PANE_FLAGS), now = was | PANE_SHOWN;
        if (now != was) {
            memcpy((void *)(uintptr_t)(where + PANE_FLAGS), &now, 4);
            if (lines++ < 4)
                rt_log("quit_menu: title: the fourth button's pane shown (flags %#x -> %#x, %d above the button's own)",
                       was, now, depth);
        }
    }
    int on = rd8(s + SCR_STATE) == SCR_OPEN && focused_entry() == e[TITLE_SPARE];
    __atomic_store_n(&g_title_ms, on ? plat_now_ms() : 0, __ATOMIC_RELEASE);
}

static void window_tick(uint64_t s) {
    static int open;
    if (s != __atomic_load_n(&g_window, __ATOMIC_ACQUIRE)) return;
    uint64_t e[WINDOW_ENTRIES];
    int n = entries_of(s, e, WINDOW_ENTRIES), state = rd8(s + SCR_STATE);
    trace("pause", s);
    if (state == SCR_OPEN) open = 1;
    if (open && state != SCR_OPEN) {
        static int lines;
        open = 0;
        __atomic_store_n(&g_window, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_pause_ms, 0, __ATOMIC_RELEASE);
        if (!__atomic_load_n(&g_quit, __ATOMIC_RELAXED) && lines++ < 16)
            rt_log("quit_menu: pause: the confirmation closed without Quit Game (Title Screen or Cancel)");
        return;
    }
    int on = n == WINDOW_ENTRIES && state == SCR_OPEN && focused_entry() == e[WINDOW_SPARE];
    __atomic_store_n(&g_pause_ms, on ? plat_now_ms() : 0, __ATOMIC_RELEASE);
}

void quit_menu_screen_host(Cpu *c) {
    uint64_t s = c->x[19];
    if (!enabled()) return;
    if (!main_hi) main_lo = MOD_MAIN_BASE, main_hi = MOD_MAIN_BASE + MAIN_SIZE;
    if (!game_object(s, SCR_PROJ + 64)) return;
    if (title_on() && screen_is(s, "Title_00")) title_tick(s);
    else if (pause_on() && screen_is(s, "SystemWindow_00")) window_tick(s);
}

void quit_menu_window_count_host(Cpu *c) {
    float buttons = 2.0f;
    uint64_t s = c->x[19];
    if (pause_on() && (uint32_t)c->x[20] == WINDOW_KIND_TITLE) {
        static int lines;
        buttons = 3.0f;
        __atomic_store_n(&g_window, s, __ATOMIC_RELEASE);
        if (lines++ < 16)
            rt_log("quit_menu: pause: the Title Screen confirmation opens with three buttons (Title Screen, Cancel, "
                   "Quit Game)");
    } else if (__atomic_load_n(&g_window, __ATOMIC_ACQUIRE) == s) {
        __atomic_store_n(&g_window, 0, __ATOMIC_RELEASE);
    }
    uint32_t bits;
    memcpy(&bits, &buttons, 4);
    c->v[0] = bits;
}

void quit_menu_window_label_host(Cpu *c) {
    int three = pause_on() && c->x[21] == __atomic_load_n(&g_window, __ATOMIC_ACQUIRE);
    uint32_t label = (uint32_t)c->x[19];
    if (label == QM_LABEL_QUIT && !__atomic_load_n(&g_texts, __ATOMIC_ACQUIRE)) label = 1;
    c->x[3] = qm_window_label(three, (uint32_t)c->x[20], label);
}

static int armed(uint64_t now) {
    uint64_t t = __atomic_load_n(&g_title_ms, __ATOMIC_ACQUIRE), p = __atomic_load_n(&g_pause_ms, __ATOMIC_ACQUIRE);
    if (p && now - p < ARMED_MS) return AT_PAUSE;
    if (t && now - t < ARMED_MS) return AT_TITLE;
    return AT_NONE;
}

void quit_menu_poll(uint64_t *buttons) {
    static QmPress press;
    static volatile int32_t busy, held;
    static int said;
    const uint64_t a = 1ull << B_A;
    if (!enabled()) return;
    uint64_t now = plat_now_ms();
    if (plat_xchg32(&busy, 1)) {
        if (__atomic_load_n(&held, __ATOMIC_RELAXED)) *buttons &= ~a;
        return;
    }
    int at = armed(now), chosen = 0, pass = test_flag("pass");
    int swallow = qm_press(&press, at != AT_NONE, (*buttons & a) != 0, &chosen);
    __atomic_store_n(&held, swallow && !pass, __ATOMIC_RELAXED);
    if (swallow && !pass) *buttons &= ~a;
    if (chosen && pass) {
        rt_log("quit_menu: %s: Quit Game chosen; NX2SARIA_QUIT_MENU_TEST=pass gives the press to the game instead",
               at == AT_TITLE ? "title" : "pause");
    } else if (chosen && !plat_xchg32(&g_quit, 1)) {
        __atomic_store_n(&g_chosen_ms, now, __ATOMIC_RELEASE);
        rt_log("quit_menu: %s: Quit Game chosen%s", at == AT_TITLE ? "title" : "pause",
               fs_save_busy() ? "; the game is saving, it closes once the save is written" : "; closing the game");
    }
    if (__atomic_load_n(&g_quit, __ATOMIC_RELAXED) && !said && !fs_save_busy()) {
        said = 1;
        uint64_t waited = now - __atomic_load_n(&g_chosen_ms, __ATOMIC_ACQUIRE);
        if (waited > 50) rt_log("quit_menu: the save is written (%llu ms): closing the game", (unsigned long long)waited);
        gfx_window_request_close();
    }
    plat_xchg32(&busy, 0);
}
