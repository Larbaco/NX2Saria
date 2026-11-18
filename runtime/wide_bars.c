// The menus with black bars (NX2SARIA_WIDE_MENUS = center, Ocarina of Time 1.1.1): the present clears the picture's
// sides outside its middle 16:9 to black while a pause or system menu, a loading screen or a movie is up, and never in
// play (gfx_present.c present_image asks wide_menu_sides at each present).
//
// The signal is the game's own state of the screens behind those menus. Every interface screen of the game is an
// object whose byte +0x1a6 says where it is in its life: 0 closed, 1 opening (main+0x2d62e0 sets it when the screen is
// asked to open), 2 open (vtable +0x240, main+0x2cda10), 3 closing (vtable +0x248, main+0x2d8af0) and 0 again once it
// has closed (vtable +0x258, main+0xc60a10). Two classes of screen are watched, by their vtables, the ones the
// widescreen keep list names (widescreen.c wide_init):
// - main+0x4be4fb0, the background of the pause and system menus (MenuBG_00, SystemBG_00): a capture of the picture
//   the menu opens over, redrawn under it. Its screen is on (states 1 to 3) for as long as such a menu is up: the sides
//   go black when the menu starts to open and come back once it has closed. The game keeps drawing 3D under the pause
//   menu (the item's model on the items page, Link on the equipment page), so whether a 3D scene is drawn does not
//   tell a menu from play; this does.
// - main+0x4be7b30, the loading fade (LoadingFade_00), which covers the whole picture while an area loads: only while
//   it is open (state 2). Then the fade covers the sides anyway; opening and closing are the fade itself, during which
//   black sides would show before the rest of the picture is dark.
// A movie (ov_movie2.c ov_movie_live) blacks the sides by itself: the movie sits in the middle 16:9 (set wide_movie).
//
// The screens' addresses come from the patch set wide_bars (recomp/patches-v111.txt): right after the screen's setup
// (main+0x773b90, vtable +0x80) has set up its 2D canvas, its site calls the patch page's +0xef0 with the screen in x19
// (wide_bars_screen_host, registered by patches.c). The setup runs when the game makes a screen, which lives on from
// there, so the table below is short and a screen set up again is found in it. A screen the game has destroyed no
// longer holds a watched vtable (a destructor writes its base class's, or the memory is reused or unmapped): the
// present notices and frees its entry. The set is switched on only with center on a picture wider than 16:9
// (wide_bars_init), so the default, full, runs the game's setup as it is and nothing here.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "host.h"
#include "layout.h"

#define VT_MENU_BG 0x4be4fb0ull       // main offsets of the two watched vtables
#define VT_LOADING_FADE 0x4be7b30ull
#define SCREEN_STATE 0x1a6            // the screen's state byte (above)
#define BARS_SCREENS 16               // screens watched at once (the game makes a few of each)
#define BARS_TOLD 24                  // lines a run about the sides (black, shown again)
#define SCREENS_TOLD 48               // lines a run about the watched screens (found, state changes, gone)

enum { KIND_NONE, KIND_MENU, KIND_LOADING };
enum { WHY_NONE, WHY_MOVIE, WHY_MENU, WHY_LOADING };

static int bars_on;              // wide_bars_init: center on a picture wider than 16:9, with the set on
static double bars_margin;       // the share of the picture's width on each side of its middle 16:9
static uint64_t screens[BARS_SCREENS];         // watched screens' guest addresses, 0 free

static uint64_t ld64(uint64_t a) { return a && mem_readable(a, 8) ? *(const volatile uint64_t *)(uintptr_t)a : 0; }
static int ld8(uint64_t a) { return a && mem_readable(a, 1) ? *(const volatile uint8_t *)(uintptr_t)a : -1; }

static int kind_of(uint64_t screen) {
    uint64_t vt = ld64(screen);
    return vt == MOD_MAIN_BASE + VT_MENU_BG        ? KIND_MENU
           : vt == MOD_MAIN_BASE + VT_LOADING_FADE ? KIND_LOADING
                                                   : KIND_NONE;
}

static const char *const kind_names[] = {"", "pause or system menu background (MenuBG_00, SystemBG_00)",
                                         "loading fade (LoadingFade_00)"};
static const char *const state_names[] = {"closed", "opening", "open", "closing"};
static const char *state_name(int s) { return s >= 0 && s < 4 ? state_names[s] : "unknown"; }

static int screens_told;
static void screen_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void screen_log(const char *fmt, ...) {
    int n = __atomic_fetch_add(&screens_told, 1, __ATOMIC_RELAXED);
    if (n >= SCREENS_TOLD) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("widescreen: menus: %s%s", line, n == SCREENS_TOLD - 1 ? " (further screen lines are not logged)" : "");
}

// main+0x774068 (set wide_bars), after the screen's setup has set up its canvas: x19 is the screen. Every register is
// left as it is (the site's next instruction reads x8, which the site loaded before the call).
void wide_bars_screen_host(Cpu *c) {
    uint64_t s = c->x[19];
    int kind = kind_of(s);
    if (!kind) return;
    for (int i = 0; i < BARS_SCREENS; i++)
        if (__atomic_load_n(&screens[i], __ATOMIC_RELAXED) == s) return;   // set up again: already watched
    for (int i = 0; i < BARS_SCREENS; i++) {
        uint64_t old = __atomic_load_n(&screens[i], __ATOMIC_RELAXED);
        if (old && kind_of(old)) continue;   // a live screen (one destroyed since is taken over here)
        if (__atomic_compare_exchange_n(&screens[i], &old, s, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            screen_log("watching the %s screen at %#llx (state %s)", kind_names[kind], (unsigned long long)s,
                       state_name(ld8(s + SCREEN_STATE)));
            return;
        }
    }
    static int full;
    if (!__atomic_exchange_n(&full, 1, __ATOMIC_RELAXED))
        rt_log("widescreen: menus: more than %d menu background and loading fade screens; the %s screen at %#llx is "
               "not watched, and its menu may show the picture's sides", BARS_SCREENS, kind_names[kind],
               (unsigned long long)s);
}

// wide_init: the setting is center and the picture is wider than 16:9 by margin of its width on each side.
void wide_bars_init(double margin) {
    int set = rt_patch_set("wide_bars");
    bars_margin = margin;
    if (set < 0) {
        rt_log("widescreen: menus center: the generated C has no set wide_bars (gen.py --patches "
               "recomp/patches-v111.txt): the picture's sides go black only while a movie plays");
    } else {
        rt_patch_switch(set, 1);
    }
    bars_on = 1;
}

// Each present, from the present's thread: the share of the picture's width to black out on each side, 0 for none.
double wide_menu_sides(void) {
    if (!bars_on) return 0;
    static uint64_t presents, since;
    static uint64_t seen_screen[BARS_SCREENS];
    static int seen_state[BARS_SCREENS], was, told;
    uint64_t n = ++presents;
    int menu = 0, loading = 0;
    for (int i = 0; i < BARS_SCREENS; i++) {
        uint64_t s = __atomic_load_n(&screens[i], __ATOMIC_ACQUIRE);
        int kind = s ? kind_of(s) : KIND_NONE, st = kind ? ld8(s + SCREEN_STATE) : -1;
        if (s != seen_screen[i]) seen_screen[i] = s, seen_state[i] = kind ? st : -1;   // newly watched: logged there
        if (!s) continue;
        if (!kind) {   // destroyed: its memory is no longer a watched screen
            screen_log("the screen at %#llx is gone (present %llu)", (unsigned long long)s, (unsigned long long)n);
            if (__atomic_compare_exchange_n(&screens[i], &s, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                seen_screen[i] = 0;
            continue;
        }
        if (st != seen_state[i]) {
            screen_log("the %s screen at %#llx: %s -> %s at present %llu", kind_names[kind], (unsigned long long)s,
                       state_name(seen_state[i]), state_name(st), (unsigned long long)n);
            seen_state[i] = st;
        }
        if (kind == KIND_MENU && st >= 1 && st <= 3) menu = 1;
        if (kind == KIND_LOADING && st == 2) loading = 1;
    }
    int why = ov_movie_live() ? WHY_MOVIE : menu ? WHY_MENU : loading ? WHY_LOADING : WHY_NONE;
    if ((why != WHY_NONE) != (was != WHY_NONE)) {
        if (told < BARS_TOLD) {
            told++;
            static const char *const up[] = {"", "a movie plays", "a pause or system menu is up (its background on)",
                                             "a loading screen is up (the loading fade open)"};
            static const char *const down[] = {"", "the movie ended", "the menu's background screen closed",
                                               "the loading fade closed"};
            if (why)
                rt_log("widescreen: menus: the picture's sides black from present %llu: %s", (unsigned long long)n,
                       up[why]);
            else
                rt_log("widescreen: menus: the picture's sides shown again at present %llu, after %llu presents black: "
                       "%s", (unsigned long long)n, (unsigned long long)(n - since), down[was]);
            if (told == BARS_TOLD) rt_log("widescreen: menus: further changes of the sides are not logged");
        }
        since = n;
    }
    was = why;
    return why ? bars_margin : 0;
}
