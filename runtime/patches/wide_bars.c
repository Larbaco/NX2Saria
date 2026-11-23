#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include "host.h"
#include "layout.h"

#define VT_MENU_BG 0x4be4fb0ull
#define VT_LOADING_FADE 0x4be7b30ull
#define VT_EVENT_FADE 0x4be61c8ull
#define SCREEN_STATE 0x1a6
#define BARS_SCREENS 16
#define BARS_TOLD 24
#define SCREENS_TOLD 48

enum { KIND_NONE, KIND_MENU, KIND_LOADING, KIND_EVENT_FADE };
enum { WHY_NONE, WHY_MOVIE, WHY_MENU, WHY_LOADING, WHY_EVENT_FADE };

static int bars_on;
static double bars_x_margin, bars_y_margin;
static uint64_t screens[BARS_SCREENS];

static uint64_t ld64(uint64_t a) { return a && mem_readable(a, 8) ? *(const volatile uint64_t *)(uintptr_t)a : 0; }
static int ld8(uint64_t a) { return a && mem_readable(a, 1) ? *(const volatile uint8_t *)(uintptr_t)a : -1; }

static int kind_of(uint64_t screen) {
    uint64_t vt = ld64(screen);
    return vt == MOD_MAIN_BASE + VT_MENU_BG        ? KIND_MENU
           : vt == MOD_MAIN_BASE + VT_LOADING_FADE ? KIND_LOADING
           : vt == MOD_MAIN_BASE + VT_EVENT_FADE ? KIND_EVENT_FADE
                                                 : KIND_NONE;
}

static const char *const kind_names[] = {"", "pause or system menu background (MenuBG_00, SystemBG_00)",
                                         "loading fade (LoadingFade_00)", "event fade"};
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

void wide_bars_screen_host(Cpu *c) {
    uint64_t s = c->x[19];
    int kind = kind_of(s);
    if (!kind || (!bars_on && kind != KIND_MENU)) return;
    for (int i = 0; i < BARS_SCREENS; i++)
        if (__atomic_load_n(&screens[i], __ATOMIC_RELAXED) == s) return;
    for (int i = 0; i < BARS_SCREENS; i++) {
        uint64_t old = __atomic_load_n(&screens[i], __ATOMIC_RELAXED);
        if (old && kind_of(old)) continue;
        if (__atomic_compare_exchange_n(&screens[i], &old, s, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            if (bars_on)
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

int wide_menu_open(void) {
    for (int i = 0; i < BARS_SCREENS; i++) {
        uint64_t s = __atomic_load_n(&screens[i], __ATOMIC_ACQUIRE);
        int state = kind_of(s) == KIND_MENU ? ld8(s + SCREEN_STATE) : -1;
        if (state >= 1 && state <= 3) return 1;
    }
    return 0;
}

void wide_bars_init(double x_margin, double y_margin) {
    int set = rt_patch_set("wide_bars");
    bars_x_margin = x_margin, bars_y_margin = y_margin;
    if (set < 0) {
        rt_log("widescreen: centered screens: the generated C has no set wide_bars (gen.py --patches "
               "recomp/patches-v111.txt): only a movie can black its margins");
    } else {
        rt_patch_switch(set, 1);
    }
    bars_on = 1;
}

void wide_menu_bars(double *x, double *y) {
    *x = *y = 0;
    if (!bars_on) return;
    static uint64_t presents, since;
    static uint64_t seen_screen[BARS_SCREENS];
    static int seen_state[BARS_SCREENS], was, told;
    uint64_t n = ++presents;
    int menu = 0, loading = 0, event_fade = 0;
    for (int i = 0; i < BARS_SCREENS; i++) {
        uint64_t s = __atomic_load_n(&screens[i], __ATOMIC_ACQUIRE);
        int kind = s ? kind_of(s) : KIND_NONE, st = kind ? ld8(s + SCREEN_STATE) : -1;
        if (s != seen_screen[i]) seen_screen[i] = s, seen_state[i] = kind ? st : -1;
        if (!s) continue;
        if (!kind) {
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
        int loading_active = st == 2 || (st >= 1 && st <= 3 &&
                                           (bars_y_margin > 0 || (bars_x_margin == 0 && bars_y_margin == 0)));
        if (kind == KIND_LOADING && loading_active) loading = 1;
        if (kind == KIND_EVENT_FADE && st >= 1 && st <= 3) event_fade = 1;
    }
    int why = ov_movie_live() ? WHY_MOVIE : menu ? WHY_MENU : event_fade ? WHY_EVENT_FADE : loading ? WHY_LOADING : WHY_NONE;
    if ((why != WHY_NONE) != (was != WHY_NONE)) {
        if (told < BARS_TOLD) {
            told++;
            static const char *const up[] = {"", "a movie plays", "a pause or system menu is up (its background on)",
                                             "a loading screen is up (the loading fade open)", "an event fade is active"};
            static const char *const down[] = {"", "the movie ended", "the menu's background screen closed",
                                               "the loading fade closed", "the event fade ended"};
            int margins = bars_x_margin > 0 || bars_y_margin > 0;
            if (why && margins)
                rt_log("widescreen: centered screens: picture margins black from present %llu: %s", (unsigned long long)n,
                       up[why]);
            else if (why)
                rt_log("widescreen: full-height screens: active from present %llu; no margins cleared: %s",
                       (unsigned long long)n, up[why]);
            else if (margins)
                rt_log("widescreen: centered screens: picture margins shown again at present %llu, after %llu presents black: "
                       "%s", (unsigned long long)n, (unsigned long long)(n - since), down[was]);
            else
                rt_log("widescreen: full-height screens: ended at present %llu after %llu presents: %s",
                       (unsigned long long)n, (unsigned long long)(n - since), down[was]);
            if (told == BARS_TOLD) rt_log("widescreen: centered screens: further margin changes are not logged");
        }
        since = n;
    }
    was = why;
    if (why) *x = bars_x_margin, *y = bars_y_margin;
}
