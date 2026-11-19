// The mouse in Ocarina of Time 1.1.1's own menus (the title and its save files, the pause menu and its pages, Options,
// the save dialogs): while a menu screen has the game's focus and keyboard and mouse are the device used last, the
// pointer is released from the mouse camera and shown; the button under it takes the focus as if the d-pad had moved
// there (the game's highlight, slide and sound), a left click on a button is A on it, a right click is B, and the wheel
// is the d-pad's up and down (a list scrolls a row a notch).
//
// No game code is called. Each interface screen's update calls menu_mouse_screen_host (the set menu_mouse's site,
// recomp/patches-v111.txt, main+0x2d13b0), on the thread that runs the game's interface, between the focus manager's
// read of the pad and its controls' update. For the screen that has the focus there, the screen's buttons are read
// and a focus move is written as the game's focus setter (main+0x86ffc0 and its manager's method main+0x3774f70) writes
// it: the control's reserved entry, the screen's requested entry and its request bit. The controls' update
// (main+0x768990) then moves the focus itself. A and B are pad presses the pad read adds (menu_mouse_poll, from
// input.c input_poll), held 60 ms so the game sees one press whatever its frame rate.
//
// The game's objects (main offsets; docs/menu-mouse.md says how they were found):
//  - the interface system *(main+0x4e982e0) (its vtable main+0x4c73ce0); its screen manager +0x38, whose focus manager
//    is +0x60: +0x22 the players whose input it takes (bit 0 player 1), +0x28 player 1's control;
//  - a control: +0x78 the focused entry, +0x80 the reserved one, +0xb4 bit 3 the reserved move's "no slide" flag;
//  - an entry: +0x18 its button, +0x20 its screen; a screen's entries are a list, head at +0x88, first node +0x90, the
//    entry at the node less the int32 at +0x9c, the next node at the node's +8;
//  - a screen: +0x1a6 its state (2 open), +0x1a5, +0x1aa, +0x1ab the entry test's other bytes (main+0x769d70), +0x38
//    its controller (+0x40 bit 1: takes input), +0x188
//    the requested entry, +0x1ae bit 2 the request, +0x1c0 its 2D projection (4 x 4), +0x28 its layout (the name);
//  - a button: +0x40 bit 4 enabled, +0x58 its root pane (shown), +0x60 the pane its focus frame takes (the box);
//  - a pane (0xb0 bytes, in its layout's array at +0x100): +0x18 its layout, +0x28 its global matrix (3 x 4 by column:
//    +0x28 m00, +0x38 m11, +0x4c, +0x50 the translation), +0x58 its size, +0x74 its alpha (0..1), +0x80 the extra size,
//    +0xa8 its flags (bit 0 shown, bit 3 alpha inherited, bit 10 an extra height or with bit 9 width, bits 11..17 the
//    base position).
//
// NX2SARIA_MENU_CURSOR (live, default 1): 0 leaves the menus to the keys and the pad. NX2SARIA_MENU_CURSOR_LOG=1 logs
// every screen the focus goes to. NX2SARIA_MENU_MOUSE_PATCH=0 removes the site.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "cpu.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_wsys.h"
#include "layout.h"
#include "menu_mouse_geom.h"
#include "platform/platform.h"

#define UI_SYSTEM 0x4e982e0ull          // the interface system's pointer
#define UI_SYSTEM_VT 0x4c73ce0ull       // its vtable
#define MAIN_SIZE 0x4ea0000ull          // main's image, up to the patch page

enum { UI_SCREENS = 0x38, SM_FOCUS = 0x60, FM_PLAYERS = 0x22,
       FM_CONTROL = 0x28 };
enum { CTL_FOCUSED = 0x78, CTL_RESERVED = 0x80, CTL_FLAGS = 0xb4 };
enum { ENT_BUTTON = 0x18, ENT_SCREEN = 0x20 };
enum { SCR_LAYOUT = 0x28, SCR_CONTROLLER = 0x38, SCR_HEAD = 0x88, SCR_FIRST = 0x90, SCR_LINK = 0x9c,
       SCR_REQ_ENTRY = 0x188,
       SCR_HIDDEN = 0x1a5, SCR_STATE = 0x1a6, SCR_BLOCK0 = 0x1aa, SCR_BLOCK1 = 0x1ab, SCR_REQUEST = 0x1ae, SCR_PROJ = 0x1c0,
       SCR_OPEN = 2 };
enum { BTN_FLAGS = 0x40, BTN_ROOT = 0x58, BTN_BOX = 0x60 };
enum { PANE_LAYOUT = 0x18, PANE_MTX = 0x28, PANE_SIZE = 0x58, PANE_ALPHA = 0x74, PANE_EXTRA = 0x80, PANE_FLAGS = 0xa8,
       PANE_STRIDE = 0xb0 };
enum { LYT_PANES = 0x100, LYT_HOST = 0x108, LYT_TREE = 0x110 };

static uint64_t main_lo, main_hi;

static int ok_at(uint64_t p, uint64_t n) {
    return p >= 0x10000 && p < (1ull << 47) && mem_host_readable(p, n);
}
static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static uint16_t rd16(uint64_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
static uint8_t rd8(uint64_t a) { return *(const volatile uint8_t *)(uintptr_t)a; }
static float rdf(uint64_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
// An object of the game: readable, aligned, its vtable in main's image
static int game_object(uint64_t p, uint64_t n) {
    if ((p & 7) || !ok_at(p, n)) return 0;
    uint64_t vt = rd64(p);
    return vt >= main_lo && vt < main_hi;
}

// ---- panes
static int pane_alpha(uint64_t p) {
    float a = rdf(p + PANE_ALPHA) * 255.0f;
    return a >= 1.0f && a < 1e6f;   // (int)a != 0, as the game truncates; a NaN is transparent
}
// The pane's parent: in its layout's tree, or for the layout's first pane the pane that holds the layout (a part)
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
// Shown as the entry test takes it (main+0x769d70): the pane shown and not transparent, every ancestor shown, and one
// whose alpha its children inherit not transparent
static int pane_shown(uint64_t p) {
    if (!ok_at(p, PANE_STRIDE) || !(rd32(p + PANE_FLAGS) & 1) || !pane_alpha(p)) return 0;
    int depth = 0;
    for (uint64_t a = pane_parent(p); a; a = pane_parent(a)) {
        if (!ok_at(a, PANE_STRIDE) || ++depth > 48) return 0;
        uint32_t f = rd32(a + PANE_FLAGS);
        if (!(f & 1) || ((f & 8) && !pane_alpha(a))) return 0;
    }
    return 1;
}
static int pane_box(uint64_t p, MmBox *b) {
    if (!ok_at(p, PANE_STRIDE)) return 0;
    return mm_pane_box(rdf(p + PANE_SIZE), rdf(p + PANE_SIZE + 4), rdf(p + PANE_MTX), rdf(p + PANE_MTX + 0x10),
                       rdf(p + PANE_MTX + 0x24), rdf(p + PANE_MTX + 0x28), rd32(p + PANE_FLAGS), rdf(p + PANE_EXTRA), b);
}

// ---- screens and entries
typedef struct { uint64_t mgr, focus, ctl; } Ui;
static int ui_now(Ui *u) {
    uint64_t sys = rd64(MOD_MAIN_BASE + UI_SYSTEM);
    if (!game_object(sys, UI_SCREENS + 8) || rd64(sys) != MOD_MAIN_BASE + UI_SYSTEM_VT) return 0;
    u->mgr = rd64(sys + UI_SCREENS);
    if (!game_object(u->mgr, SM_FOCUS + 8)) return 0;
    u->focus = rd64(u->mgr + SM_FOCUS);
    if (!game_object(u->focus, FM_CONTROL + 8)) return 0;
    u->ctl = rd64(u->focus + FM_CONTROL);
    return game_object(u->ctl, CTL_FLAGS + 4);
}
// Why no screen is for the mouse: one line each time the reason or the objects change (a field that moved in another
// game build shows here), at most 12 a session, 60 with NX2SARIA_MENU_CURSOR_LOG=1
static void why_none(const Ui *u, const char *why) {
    static char last[200];
    static int lines;
    char line[200];
    uint64_t c2 = ok_at(u->focus + FM_CONTROL + 8, 8) ? rd64(u->focus + FM_CONTROL + 8) : 0, e = rd64(u->ctl + CTL_FOCUSED), sc = ok_at(e, ENT_SCREEN + 8) ? rd64(e + ENT_SCREEN) : 0;
    uint64_t e2 = ok_at(c2, CTL_FOCUSED + 8) ? rd64(c2 + CTL_FOCUSED) : 0;
    snprintf(line, sizeof line, "%s (players %#x, entry %#llx, its screen %#llx state %d, second control's entry %#llx)", why,
             rd8(u->focus + FM_PLAYERS), (unsigned long long)e, (unsigned long long)sc,
             ok_at(sc, SCR_STATE + 1) ? rd8(sc + SCR_STATE) : -1, (unsigned long long)e2);
    if (!strcmp(line, last)) return;
    snprintf(last, sizeof last, "%s", line);
    const char *v = cfg_get("NX2SARIA_MENU_CURSOR_LOG");
    if (lines++ < (v && !strcmp(v, "1") ? 60 : 12)) rt_log("menu_mouse: no menu for the mouse: %s", line);
}
static const char *g_why;   // focus_screen's reason for 0
// The screen part of the game's entry test (main+0x769d70): open, shown, not blocked, its controller taking input
// (+0x40 bit 1). 0 with the reason in g_why.
static int screen_open_why(uint64_t mgr, uint64_t s) {
    if (!game_object(s, SCR_PROJ + 64)) return g_why = "the entry's screen is no object", 0;
    if (rd8(s + SCR_STATE) != SCR_OPEN || (int8_t)rd8(s + SCR_HIDDEN) < 0) return g_why = "the screen is not open", 0;
    if ((rd8(s + SCR_BLOCK0) & 1) || (rd8(s + SCR_BLOCK1) & 1)) return g_why = "the screen is blocked", 0;
    uint64_t ctl = rd64(s + SCR_CONTROLLER);
    if (!ok_at(ctl, 0x48) || !(rd8(ctl + 0x40) & 2)) return g_why = "the screen's controller takes no input", 0;
    (void)mgr;   // (the entry test's level floors, +0x170 against the manager's +0xdc, +0xe0, are not taken: read as u32
                 // they refused the save files' list while it had the focus; the focused entry is the manager's choice)
    return 1;
}
// The screen of the focused entry, 0: none (gameplay)
static uint64_t focus_screen(const Ui *u, uint64_t *entry) {
    if (!(rd8(u->focus + FM_PLAYERS) & 1)) return g_why = "the focus manager takes no input from player 1", 0;
    uint64_t e = rd64(u->ctl + CTL_FOCUSED);
    if (!game_object(e, ENT_SCREEN + 8)) return g_why = "no focused entry", 0;
    uint64_t s = rd64(e + ENT_SCREEN);
    if (!screen_open_why(u->mgr, s)) return 0;
    *entry = e;
    return s;
}
static void screen_name(uint64_t s, char *out, size_t n) {
    snprintf(out, n, "?");
    uint64_t a = rd64(s + SCR_LAYOUT), lyt = ok_at(a, 0x88) ? rd64(a + 0x80) : 0, r = ok_at(lyt, 0x100) ? rd64(lyt + 0xf8) : 0,
             name = ok_at(r, 0x20) ? rd64(r + 0x18) : 0;
    if (!ok_at(name, n)) return;
    memcpy(out, (const void *)(uintptr_t)name, n - 1);
    out[n - 1] = 0;
    for (char *q = out; *q; q++)
        if (*q < 0x20 || *q > 0x7e) { *q = 0; break; }
    if (!out[0]) snprintf(out, n, "?");
}
// A tab click of the pause menu and its outcome (the first 40 lines a session)
static void tab_log(const char *what, int tab, int page) {
    static const char *const names[] = {"Collection", "Equipment", "Items", "System"};
    static int lines;
    if (lines++ < 40)
        rt_log("menu_mouse: pause menu tab %s %s (page %s)", names[tab & 3], what, page >= 0 ? names[page & 3] : "unknown");
}

// The pause menu's page the focused entry is on (0 Collection .. 3 System, menu_mouse_geom.h mm_pause_page), -1 none
// found: the layout that holds the entry's button, or the layout holding that one (a slot is a part inside its page),
// up to the page's own layout, whose name is the page's (PaItemMenu_00, ...)
static int pause_page_of(uint64_t entry) {
    uint64_t btn = rd64(entry + ENT_BUTTON), pane = ok_at(btn, BTN_ROOT + 8) ? rd64(btn + BTN_ROOT) : 0;
    for (int depth = 0; depth < 8 && ok_at(pane, PANE_STRIDE); depth++) {
        uint64_t lyt = rd64(pane + PANE_LAYOUT);
        if (!ok_at(lyt, LYT_TREE + 8)) return -1;
        uint64_t r = rd64(lyt + 0xf8), nm = ok_at(r, 0x20) ? rd64(r + 0x18) : 0;
        char name[32];
        if (ok_at(nm, sizeof name)) {
            memcpy(name, (const void *)(uintptr_t)nm, sizeof name - 1);
            name[sizeof name - 1] = 0;
            int page = mm_pause_page(name);
            if (page >= 0) return page;
        }
        pane = rd64(lyt + LYT_HOST);   // the pane that holds this layout, in the layout above
    }
    return -1;
}

// The layout units from the picture's centre to its edge, from the screen's projection (squeezed on ultrawide)
static int screen_units(uint64_t s, float *ux, float *uy) { return mm_units(rdf(s + SCR_PROJ), rdf(s + SCR_PROJ + 0x14), ux, uy); }

#define MAX_ENTRIES 256
typedef struct { uint64_t entry[MAX_ENTRIES]; MmBox box[MAX_ENTRIES]; int n, listed; } Entries;
static int button_takes_focus(uint64_t btn) {
    return ok_at(btn, BTN_BOX + 8) && (rd32(btn + BTN_FLAGS) & 0x10) && pane_shown(rd64(btn + BTN_ROOT));
}
// The screen's entries that can take the focus, with their boxes
#define NEAR_UNITS 80.0f   // on the pause menu, a pointer this near a box (layout units; 40 pixels at 1080) is on it
static int dump_boxes;   // the next entries_of logs every box (a menu screen's first, NX2SARIA_MENU_CURSOR_LOG=1)
static void entries_of(uint64_t s, Entries *out) {
    out->n = out->listed = 0;
    uint64_t head = s + SCR_HEAD;
    int32_t off = (int32_t)rd32(s + SCR_LINK);
    if (off < 0 || off > 0x100) return;
    int guard = 0;
    for (uint64_t link = rd64(s + SCR_FIRST); link && link != head && guard < 4 * MAX_ENTRIES; link = rd64(link + 8), guard++) {
        if (!ok_at(link, 16)) break;
        out->listed++;
        uint64_t e = link - (uint64_t)off;
        if (!ok_at(e, ENT_SCREEN + 8) || rd64(e + ENT_SCREEN) != s) continue;
        uint64_t btn = rd64(e + ENT_BUTTON);
        MmBox b;
        if (!button_takes_focus(btn) || !pane_box(rd64(btn + BTN_BOX), &b) || out->n >= MAX_ENTRIES) continue;
        if (dump_boxes)   // (NX2SARIA_MENU_CURSOR_LOG=1: each box of a menu as it opens)
            rt_log("menu_mouse: box %d: %.0f..%.0f, %.0f..%.0f", out->n, (double)b.x0, (double)b.x1, (double)b.y0, (double)b.y1);
        out->entry[out->n] = e, out->box[out->n] = b, out->n++;
    }
    dump_boxes = 0;
}
// The focus to entry e of screen s: the writes of the game's focus setter with its flag 0 (a slide, as the d-pad moves).
// (The method's busy guard, its +0x1f bit 5 with the focused button's +0x40 bit 5, is not taken: it held on the save
// files' list the whole time the list waited for input, and the d-pad's moves there do not go through it.)
static void reserve_focus(const Ui *u, uint64_t s, uint64_t e) {
    memcpy((void *)(uintptr_t)(u->ctl + CTL_RESERVED), &e, 8);
    __atomic_fetch_and((uint32_t *)(uintptr_t)(u->ctl + CTL_FLAGS), ~8u, __ATOMIC_SEQ_CST);
    if (rd64(s + SCR_REQ_ENTRY) != e) {
        memcpy((void *)(uintptr_t)(s + SCR_REQ_ENTRY), &e, 8);
        __atomic_fetch_or((uint16_t *)(uintptr_t)(s + SCR_REQUEST), (uint16_t)4, __ATOMIC_SEQ_CST);
    }
}

// ---- the pad presses the pad read adds (menu_mouse_poll): counts the interface thread raises, the pad read spends
static const int press_bit[MM_PRESSES] = {B_A, B_B, B_UP, B_DOWN, B_L, B_R};   // (menu_mouse_geom.h MM_A..MM_R)
static volatile int32_t pending[MM_PRESSES];
// A walk to a clicked pause-menu tab is under way (the interface thread sets it); the pad read sets g_walk_player when
// the player's own pad or keys press L, R, the d-pad or the left stick meanwhile, and the walk stops
static volatile int32_t g_walk, g_walk_player;
static volatile int32_t g_pointer;           // a menu has the pointer: input_cursor_allowed releases the capture
static volatile uint64_t g_active_ms;        // the last interface update that found a menu for the mouse, or a focus
                                             // on a screen that opens or closes (between two menus)
static uint64_t g_menu_ms;                   // interface thread: the last update that found a menu for the mouse
static volatile uint64_t g_hidden_ms;        // when the pad read last hid the pointer
#define IDLE_MS 350                          // no menu that long: the pointer goes back to the camera

// Everything queued goes when its menu does: in play B is the sword and the d-pad the ocarina, Navi and the boots
static void drop_presses(void) { mm_queue_drop(pending); }

int menu_mouse_pointer(void) { return __atomic_load_n(&g_pointer, __ATOMIC_RELAXED); }

static int enabled(void) {
    static int set = -2;
    if (set == -2) set = rt_patch_set("menu_mouse");
    if (!RT_V111 || !rt_patch_enabled(set)) return 0;
    const char *v = cfg_get("NX2SARIA_MENU_CURSOR"), *in = cfg_get("NX2SARIA_INPUT");
    return !(v && !strcmp(v, "0")) && !(in && !strcmp(in, "none"));
}

// ---- the log: one line per menu the mouse was used in, when the focus leaves it (at most LOG_MENUS a session)
typedef struct { unsigned hover, click_a, click_miss, click_b, wheel, hits, misses, tabs; } Stats;
#define LOG_MENUS 48
static void stats_flush(const char *name, Stats *st) {
    static int lines;
    if (!st->hover && !st->click_a && !st->click_miss && !st->click_b && !st->wheel && !st->tabs) return;
    if (lines++ < LOG_MENUS)
        rt_log("menu_mouse: %s: focus moved %u, left clicks %u (A %u, on no button %u, on the tab row %u), right clicks "
               "(B) %u, wheel notches %u; hit test %u on a button, %u off", name, st->hover,
               st->click_a + st->click_miss + st->tabs, st->click_a, st->click_miss, st->tabs, st->click_b, st->wheel,
               st->hits, st->misses);
    memset(st, 0, sizeof *st);
}

// NX2SARIA_MENU_CURSOR_LOG=1: one more of at most n lines of what the pointer does
static int verbose_log(int n) {
    static int lines;
    const char *v = cfg_get("NX2SARIA_MENU_CURSOR_LOG");
    return v && !strcmp(v, "1") && lines++ < n;
}

// The pointer on the picture, 0..1 from its top left (the present's fit, letterbox or pillarbox); 2: the script's
static int picture_pos(float *px, float *py) {
    float nx, ny, r[4];
    int got = input_menu_pointer(&nx, &ny);
    if (!got || !gfx_picture_rect(r) || r[2] <= 0 || r[3] <= 0) return 0;
    *px = (nx - r[0]) / r[2], *py = (ny - r[1]) / r[3];
    return *px >= 0 && *px <= 1 && *py >= 0 && *py <= 1 ? got : 0;
}

static void set_pointer(int on) {
    if (plat_xchg32(&g_pointer, on) != on) ikb_cursor_update();
}

// No menu takes the mouse now: wheel notches and button presses so far are taken as seen, not kept for the next menu
static void no_menu_input(MmClicks *c) {
    input_menu_wheel_take();
    mm_clicks_sync(c, input_menu_mouse_presses(0), input_menu_mouse_presses(1));
}

// Every interface screen's update, before the focus manager's controls take a reserved focus: x19 (and x0) the
// screen. Changes no register.
void menu_mouse_screen_host(Cpu *c) {
    static int inited;
    static uint64_t last_screen;   // the screen the mouse was for at the last look
    static char last_name[48];
    static Stats st;
    static float last_px = -1, last_py = -1;
    static int shown, resync;   // resync: the pointer was just warped, its next position read is the start, no move
    // The pause menu (PlusMenu_00): its page now, and a tab clicked (the page to reach, when asked, when the page last
    // changed, when the last L or R went)
    static int pause, page = -1, tab_goal = -1;
    static uint64_t tab_ms, page_ms, step_ms;
    static MmClicks clicks;     // the button presses already taken
    static uint64_t pend_entry, pend_until, asked, asked_ms;
    static Entries ent;
    uint64_t s = c->x[19];
    if (!inited) {
        inited = 1;
        main_lo = MOD_MAIN_BASE, main_hi = MOD_MAIN_BASE + MAIN_SIZE;
        Ui u;
        rt_log("menu_mouse: the screens' site runs (thread %llu); interface system %s", (unsigned long long)plat_thread_id(),
               ui_now(&u) ? "found" : "not found yet");
    }
    if (shown && !menu_mouse_pointer()) shown = 0;   // the pad read took the pointer away: it comes anew
    static int blocked_logged;
    if (enabled() && input_game_input_blocked()) {   // the settings overlay or a text entry has the input and the mouse
        if (!blocked_logged && last_screen) {
            static int lines;
            if (lines++ < 8) rt_log("menu_mouse: %s: the settings overlay or a text entry is open: the mouse is its until it closes", last_name);
            blocked_logged = 1;
        }
        no_menu_input(&clicks);
        return;
    }
    blocked_logged = 0;
    if (!enabled()) { no_menu_input(&clicks); return; }   // (no notches or clicks kept for later)
    Ui u;
    uint64_t focused = 0, fs = 0;
    if (!ui_now(&u)) { no_menu_input(&clicks); return; }
    if (!(fs = focus_screen(&u, &focused))) {   // no menu has the focus: the last one's line now
        // (a focus still on a screen that opens or closes keeps the pointer for up to 1.5 s: one menu giving way to
        // the next)
        uint64_t t = plat_now_ms();
        if (game_object(rd64(u.ctl + CTL_FOCUSED), ENT_SCREEN + 8) && t - g_menu_ms < 1500)
            __atomic_store_n(&g_active_ms, t, __ATOMIC_RELEASE);
        drop_presses();
        why_none(&u, g_why);
        no_menu_input(&clicks);
        if (last_screen) stats_flush(last_name, &st), last_screen = 0;
        return;
    }
    if (fs != s) return;   // another screen's update: the focused one's comes in this frame too
    uint64_t now = plat_now_ms();
    char name[48];
    if (s != last_screen) {   // the focus went to another screen: the last one's line, this one's start
        if (last_screen) stats_flush(last_name, &st);
        screen_name(s, name, sizeof name);
        entries_of(s, &ent);
        float ux, uy;
        int proj = screen_units(s, &ux, &uy);
        static int lines;
        const char *e = cfg_get("NX2SARIA_MENU_CURSOR_LOG");
        if (lines++ < 24 || (e && !strcmp(e, "1")))
            rt_log("menu_mouse: screen %s: %d of %d entries take the focus, layout %.0f x %.0f units across the "
                   "picture%s", name, ent.n, ent.listed, 2 * (double)ux, 2 * (double)uy,
                   proj ? "" : " (no projection read: 16:9 taken)");
        snprintf(last_name, sizeof last_name, "%s", name);
        pause = !strcmp(name, "PlusMenu_00"), page = -1, tab_goal = -1;
        static int dumps;   // (each box of the first 6 menu screens opened, with NX2SARIA_MENU_CURSOR_LOG=1)
        const char *lg = cfg_get("NX2SARIA_MENU_CURSOR_LOG");
        dump_boxes = lg && !strcmp(lg, "1") && dumps++ < 6;
        last_screen = s, pend_until = 0, asked = 0;
        drop_presses();
    }
    // A menu for the mouse: some entry takes the focus, the focused one among them (a screen of the play, whose focus
    // sits on nothing shown, never takes the pointer from the camera)
    entries_of(s, &ent);   // (the boxes slide with a page: read each frame)
    int has_focused = 0;
    for (int i = 0; i < ent.n && !has_focused; i++) has_focused = ent.entry[i] == focused;
    if (!has_focused) {
        char why[96];
        snprintf(why, sizeof why, "the focused entry is not among the %d of %d that take the focus", ent.n, ent.listed);
        why_none(&u, why);
        no_menu_input(&clicks);
        drop_presses();
        return;
    }
    __atomic_store_n(&g_active_ms, now, __ATOMIC_RELEASE);
    g_menu_ms = now;
    int kbm = input_kbm_last();
    float px = 0, py = 0, ux, uy;
    screen_units(s, &ux, &uy);
    int pos = picture_pos(&px, &py);
    if (!kbm) {   // the gamepad: the menus as on the console, the capture kept
        if (shown) {
            shown = 0, set_pointer(0);
            static int lines;
            if (lines++ < 8) rt_log("menu_mouse: %s: the gamepad was used last: the pointer hidden", last_name);
        }
        last_px = last_py = -1, pend_until = 0;
        drop_presses();
        no_menu_input(&clicks);
        return;
    }
    if (!shown) {   // the pointer comes: on the focused button, so nothing moves until the mouse does
        shown = 1;
        set_pointer(1);
        static int lines;
        if (lines++ < 8) rt_log("menu_mouse: %s: keyboard and mouse used last: the pointer shown", last_name);
        input_menu_wheel_take();
        MmBox b;
        float r[4];
        uint64_t btn = rd64(focused + ENT_BUTTON);
        // (not when it was hidden only a moment ago, between two menus: it stays where the player left it)
        int moment = (int64_t)(now - __atomic_load_n(&g_hidden_ms, __ATOMIC_RELAXED)) < 1500;
        if (pos != 2 && !moment && ok_at(btn, BTN_BOX + 8) && pane_box(rd64(btn + BTN_BOX), &b) && gfx_picture_rect(r)) {
            float fx, fy;
            mm_to_picture((b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, ux, uy, &fx, &fy);
            ikb_pointer_set(r[0] + fx * r[2], r[1] + fy * r[3]);
            resync = 1;   // (the warp may not happen, Wayland's: where the pointer is gets read before any move counts)
        } else last_px = px, last_py = py;
        mm_clicks_sync(&clicks, input_menu_mouse_presses(0), input_menu_mouse_presses(1));   // earlier presses: no clicks
        return;
    }
    // Clicks are presses counted at the window's event, so one shorter than a frame is not lost at a low frame rate;
    // several between two looks are one
    uint32_t pl = input_menu_mouse_presses(0), pr = input_menu_mouse_presses(1);
    static int was_fg = 1;
    int fg = ikb_foreground() || pos == 2;
    if (fg && !was_fg) mm_clicks_sync(&clicks, pl, pr);   // the click that brought the window back is not a menu click
    was_fg = fg;
    int click_l, click_r;
    mm_clicks_take(&clicks, pl, pr, &click_l, &click_r);
    if (click_r) mm_queue(pending, MM_B, 1), st.click_b++;
    // (a walk to a clicked tab stops for any other wish of the player: a right click, the wheel)
    int32_t wheel = input_menu_wheel_take();
    if ((click_r || wheel) && tab_goal >= 0) tab_log("left for the player's input at", tab_goal, page), tab_goal = -1;
    if (plat_xchg32(&g_walk_player, 0) && tab_goal >= 0)
        tab_log("left for the player's d-pad, stick, L or R at", tab_goal, page), tab_goal = -1;
    float tx = 0, ty = 0;   // the pointer in layout units, for the pause menu's tab row
    if (pos) mm_to_layout(px, py, ux, uy, &tx, &ty);
    int on_tabs = pause && pos && mm_tab_at(tx, ty) >= 0;
    if (pause) {   // the page now, and a clicked tab's page reached an L or R at a time, each once the last has landed
        int cur = pause_page_of(focused);
        if (cur != page) {
            // (the page went the other way than the walk: the player's own L or R, keys or pad; the walk stops)
            if (mm_tab_away(page, cur, tab_goal))
                tab_log("left for the player's L or R at", tab_goal, cur), tab_goal = -1;
            page = cur, page_ms = now;
        }
        if (tab_goal >= 0) {
            int step = mm_tab_step(page, tab_goal);
            if (page == tab_goal || now - tab_ms > 4000) {
                tab_log(page == tab_goal ? "reached" : "given up at", tab_goal, page);
                tab_goal = -1;
            } else if (step >= 0 && now - page_ms > 250 && now - step_ms > 400 && !pending[MM_L] && !pending[MM_R]) {
                mm_queue(pending, step, 1), step_ms = now;
            }
        }
    }
    plat_xchg32(&g_walk, tab_goal >= 0);
    if (wheel && on_tabs) {   // over the pause menu's tab row a notch turns the page: up the one before (L), down the next
        mm_queue(pending, wheel > 0 ? MM_L : MM_R, 1);
        st.wheel += (unsigned)(wheel < 0 ? -wheel : wheel);
        wheel = 0;
    }
    if (wheel) {   // a notch is the d-pad's up or down: a list moves a row, as the pad moves it
        int k = wheel < 0 ? -wheel : wheel;
        mm_queue(pending, wheel > 0 ? MM_UP : MM_DOWN, k);
        st.wheel += (unsigned)k;
    }
    if (pend_until) {   // a click on another button: A once the focus is there, the reservation renewed till then
        if (focused == pend_entry) mm_queue(pending, MM_A, 1), pend_until = 0, st.click_a++;
        else if (now > pend_until) pend_until = 0;
        else reserve_focus(&u, s, pend_entry);
        return;
    }
    if (!pos) return;
    if (resync) last_px = px, last_py = py, resync = 0;   // the first position read after a warp: the start
    int moved = mm_moved(px, py, last_px, last_py);
    if (!moved && !click_l) return;
    if (moved) last_px = px, last_py = py;
    float x, y;
    mm_to_layout(px, py, ux, uy, &x, &y);
    int i = mm_pick(ent.box, ent.n, x, y);
    // (the nearest box only on the pause menu, whose pages have the narrow slots: on a prompt's Yes and No a click just
    // outside a button stays outside it)
    if (i < 0 && pause && !on_tabs) i = mm_pick_near(ent.box, ent.n, x, y, NEAR_UNITS);
    if (i < 0 && click_l && on_tabs) {   // the pause menu's tab row: L or R for a sign, else steps to the page clicked
        int tab = mm_tab_at(x, y);
        st.tabs++;
        if (tab == MM_TAB_L || tab == MM_TAB_R) mm_queue(pending, tab == MM_TAB_L ? MM_L : MM_R, 1), tab_goal = -1;
        else if (page < 0) tab_log("clicked, the page now not known for", tab, page), tab_goal = -1;
        else if (tab != page) tab_goal = tab, tab_ms = now, step_ms = 0, tab_log("clicked", tab, page);
        else tab_goal = -1;   // (the page shown: nothing to walk to, a walk under way stops)
        plat_xchg32(&g_walk, tab_goal >= 0), plat_xchg32(&g_walk_player, 0);
        return;
    }
    if (i < 0) {
        if (verbose_log(40)) rt_log("menu_mouse: pointer (%.0f, %.0f): on none of %d entries", (double)x, (double)y, ent.n);
        st.misses++;
        if (click_l) st.click_miss++;
        return;
    }
    st.hits++;
    uint64_t e = ent.entry[i];
    // A hover asks once; asked again after 150 ms if the game has not taken it (a reservation it refused)
    if (e == focused) asked = 0;
    else if (click_l || e != asked || now - asked_ms > 150) {
        reserve_focus(&u, s, e);
        st.hover += e != asked, asked = e, asked_ms = now;
        if (verbose_log(40))
            rt_log("menu_mouse: pointer (%.0f, %.0f): entry %d of %d (%.0f..%.0f, %.0f..%.0f) reserved, the focus on %#llx",
                   (double)x, (double)y, i, ent.n, (double)ent.box[i].x0, (double)ent.box[i].x1, (double)ent.box[i].y0,
                   (double)ent.box[i].y1, (unsigned long long)focused);
    }
    if (click_l) {
        if (e == focused) mm_queue(pending, MM_A, 1), st.click_a++;
        else pend_entry = e, pend_until = now + 300;
    }
}

// Once per pad read: the presses the interface thread asked for, one at a time, each held 60 ms and then 60 ms let go
void menu_mouse_poll(uint64_t *buttons) {
    static int logged;
    // The press being given: its bit and the times it is held to and let go until. The pad is read on more than one
    // thread (a cheat's frame reads it on the render thread): one at a time takes presses, the others add the one held.
    static volatile uint64_t hold_bit, hold_until, gap_until;
    static volatile int32_t busy;
    if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED) && RT_V111) {
        int set = rt_patch_set("menu_mouse");
        rt_log("menu_mouse: %s (NX2SARIA_MENU_CURSOR %s; set menu_mouse %s, the screens' site main+0x2d13b0)",
               enabled() ? "on for 1.1.1" : "off", cfg_value("NX2SARIA_MENU_CURSOR"),
               set < 0 ? "missing from this build" : rt_patch_enabled(set) ? "on" : "off");
    }
    uint64_t active = __atomic_load_n(&g_active_ms, __ATOMIC_ACQUIRE), now = plat_now_ms();
    if (plat_xchg32(&busy, 1)) {   // another thread is taking presses: the one held, if any
        if (now < __atomic_load_n(&hold_until, __ATOMIC_ACQUIRE)) *buttons |= __atomic_load_n(&hold_bit, __ATOMIC_RELAXED);
        return;
    }
    const uint64_t walk_keys = 1ull << B_L | 1ull << B_R | 1ull << B_LEFT | 1ull << B_UP | 1ull << B_RIGHT |
                               1ull << B_DOWN | 1ull << B_SL_LEFT | 1ull << B_SL_UP | 1ull << B_SL_RIGHT | 1ull << B_SL_DOWN;
    if (__atomic_load_n(&g_walk, __ATOMIC_RELAXED) && (*buttons & walk_keys)) plat_xchg32(&g_walk_player, 1);
    // No menu for the mouse for IDLE_MS (gameplay, a movie, the overlay): the pointer goes, the camera takes the mouse
    if (menu_mouse_pointer() && (int64_t)(now - active) > IDLE_MS) {
        static int lines;
        if (lines++ < 8) rt_log("menu_mouse: no menu for %d ms: the pointer hidden, the mouse back to the camera", IDLE_MS);
        __atomic_store_n(&g_hidden_ms, now, __ATOMIC_RELAXED);
        set_pointer(0);
        drop_presses();
    }
    if (now < hold_until) *buttons |= hold_bit;
    else if (now >= gap_until) {
        int k = mm_queue_take(pending);
        if (k >= 0) {
            __atomic_store_n(&hold_bit, 1ull << press_bit[k], __ATOMIC_RELAXED);
            __atomic_store_n(&gap_until, now + 120, __ATOMIC_RELAXED);
            __atomic_store_n(&hold_until, now + 60, __ATOMIC_RELEASE);
            *buttons |= 1ull << press_bit[k];
        }
    }
    plat_xchg32(&busy, 0);
}
