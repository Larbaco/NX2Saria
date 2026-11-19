// The menus' black bars (runtime/wide_bars.c) against stand-in interface screens: the screens the set wide_bars' site
// hands over, the share of the width each present blacks out for each state of the menu backgrounds and the loading
// fade, a movie, the log lines, a screen destroyed and another set up in its place, a full table, and the registers the
// host function leaves. Built by tests/wide_bars_check.py, which writes the stand-in layout.h; "no-set" runs a start
// whose generated C lacks the set.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t test_main_base = 0x7000000000ull;   // layout.h's MOD_MAIN_BASE: the vtables are numbers compared, never read

#include "wide_bars.c"

#define MENU_BG (0x4be4fb0ull)
#define LOADING_FADE (0x4be7b30ull)
#define EVENT_FADE (0x4be61c8ull)
#define SCREEN_BASE (0x4bec998ull)   // what a destroyed screen's vtable becomes (its destructor's base class)
#define UNMAPPED 0x1000ull

static int have_set = 1, set_on;
int rt_patch_set(const char *name) { return have_set && !strcmp(name, "wide_bars") ? 7 : -1; }
void rt_patch_switch(int set, int on) { assert(set == 7); set_on = on != 0; }
int mem_readable(uint64_t a, uint64_t n) { (void)n; return a && a != UNMAPPED; }
static int movie;
int ov_movie_live(void) { return movie; }

static int log_lines, full_lines;   // full_lines: the lines about a screen left out of a full table
static char last_log[512];
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    log_lines++;
    full_lines += strstr(last_log, "is not watched") != NULL;
}

typedef struct { _Alignas(16) uint8_t b[0x2f0]; } Screen;
static void make(Screen *s, uint64_t vt, int state) {
    memset(s, 0, sizeof *s);
    uint64_t v = test_main_base + vt;
    memcpy(s->b, &v, 8);
    s->b[0x1a6] = (uint8_t)state;
}
static void set_vt(Screen *s, uint64_t vt) {
    uint64_t v = test_main_base + vt;
    memcpy(s->b, &v, 8);
}

// The site: the screen in x19, x8 just loaded from it; every register the same afterwards.
static void setup(uint64_t screen) {
    Cpu c, before;
    memset(&c, 0x5a, sizeof c);
    c.x[19] = screen;
    c.x[8] = screen && screen != UNMAPPED ? *(uint64_t *)(uintptr_t)screen : 0;
    before = c;
    wide_bars_screen_host(&c);
    assert(!memcmp(&c, &before, sizeof c));
}

static int watched(void) {
    int n = 0;
    for (int i = 0; i < BARS_SCREENS; i++) n += screens[i] != 0;
    return n;
}

#define MARGIN 0.125
#define expect(want, log_has) expect_at(__LINE__, want, log_has)
static void expect_at(int line, double want, const char *log_has) {
    int lines = log_lines;
    double got = wide_menu_sides();
    if (got != want) {
        fprintf(stderr, "wide_bars_check: line %d: sides %g, expected %g (last log: %s)\n", line, got, want, last_log);
        exit(1);
    }
    if (log_has && (log_lines == lines || !strstr(last_log, log_has))) {
        fprintf(stderr, "wide_bars_check: line %d: no log line with \"%s\" (last: %s)\n", line, log_has, last_log);
        exit(1);
    }
}

static int run_no_set(void) {
    have_set = 0;
    Screen menu;
    make(&menu, MENU_BG, 0);
    wide_bars_init(MARGIN);
    assert(!set_on && strstr(last_log, "no set wide_bars"));
    expect(0, NULL);
    movie = 1;
    expect(MARGIN, "a movie plays");
    movie = 0;
    expect(0, "the movie ended");
    puts("wide_bars_check: without the set: only a movie blacks the sides");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "no-set")) return run_no_set();

    static Screen menu, system_bg, loading, event, other, spare[BARS_SCREENS + 2];
    make(&menu, MENU_BG, 0);
    make(&system_bg, MENU_BG, 0);
    make(&loading, LOADING_FADE, 0);
    make(&event, EVENT_FADE, 2);
    make(&other, 0x4b4dfc0ull, 2);

    // before wide_init: nothing to black out, whatever is up
    setup((uintptr_t)&menu);
    setup((uintptr_t)&loading);
    assert(watched() == 1 && !wide_menu_open());
    menu.b[0x1a6] = 2;
    assert(wide_menu_open());
    expect(0, NULL);
    menu.b[0x1a6] = 0;
    assert(!wide_menu_open());

    wide_bars_init(MARGIN);
    assert(set_on);

    // the screens the game sets up: the two menu backgrounds and the loading fade are watched, the others not
    setup((uintptr_t)&menu);
    setup((uintptr_t)&system_bg);
    setup((uintptr_t)&loading);
    setup((uintptr_t)&event);
    setup((uintptr_t)&other);
    setup(0);
    setup(UNMAPPED);
    assert(watched() == 3);
    setup((uintptr_t)&menu);   // set up again: still once
    assert(watched() == 3);
    assert(strstr(last_log, "watching the loading fade"));

    expect(0, NULL);   // play: every watched screen closed
    expect(0, NULL);

    // the pause menu: black from its opening to its close
    menu.b[0x1a6] = 1;
    expect(MARGIN, "black from present");
    assert(strstr(last_log, "pause or system menu"));
    menu.b[0x1a6] = 2;
    expect(MARGIN, NULL);
    menu.b[0x1a6] = 3;
    expect(MARGIN, NULL);
    menu.b[0x1a6] = 0;
    expect(0, "shown again");
    assert(strstr(last_log, "after 3 presents black") && strstr(last_log, "background screen closed"));

    // the system menu's background, the other screen of the class
    system_bg.b[0x1a6] = 2;
    expect(MARGIN, "pause or system menu");
    system_bg.b[0x1a6] = 0;
    expect(0, "shown again");

    // the loading fade: only while it is open, not while it fades in or out
    loading.b[0x1a6] = 1;
    expect(0, NULL);
    loading.b[0x1a6] = 2;
    expect(MARGIN, "a loading screen is up");
    loading.b[0x1a6] = 3;
    expect(0, "the loading fade closed");
    loading.b[0x1a6] = 0;
    expect(0, NULL);

    // the event fade and other screens never black the sides
    event.b[0x1a6] = 1;
    other.b[0x1a6] = 2;
    expect(0, NULL);

    // a movie, also under a menu; the sides come back once both are gone
    movie = 1;
    expect(MARGIN, "a movie plays");
    menu.b[0x1a6] = 2;
    expect(MARGIN, NULL);
    movie = 0;
    expect(MARGIN, NULL);
    menu.b[0x1a6] = 0;
    expect(0, "the menu's background screen closed");

    // a screen the game destroys: its vtable becomes the base class's, its state no longer counts and its entry frees
    menu.b[0x1a6] = 2;
    set_vt(&menu, SCREEN_BASE);
    expect(0, "is gone");
    assert(watched() == 2);
    // another menu background set up at the same address is watched again
    make(&menu, MENU_BG, 1);
    setup((uintptr_t)&menu);
    assert(watched() == 3);
    expect(MARGIN, "pause or system menu");
    menu.b[0x1a6] = 0;
    expect(0, "shown again");

    // a full table: one line, and the screens already watched still count
    for (int i = 0; i < BARS_SCREENS + 2; i++) {
        make(&spare[i], i & 1 ? LOADING_FADE : MENU_BG, 0);
        setup((uintptr_t)&spare[i]);
    }
    assert(watched() == BARS_SCREENS);
    assert(full_lines == 1);
    system_bg.b[0x1a6] = 2;
    expect(MARGIN, "pause or system menu");
    system_bg.b[0x1a6] = 0;
    expect(0, "shown again");

    // a destroyed screen's slot goes to the next screen set up
    set_vt(&loading, SCREEN_BASE);
    Screen late;
    make(&late, LOADING_FADE, 2);
    setup((uintptr_t)&late);
    assert(watched() == BARS_SCREENS);
    expect(MARGIN, "a loading screen is up");
    late.b[0x1a6] = 0;
    expect(0, "shown again");

    // the lines about the sides stop after BARS_TOLD
    for (int i = 0; i < BARS_TOLD; i++) {
        system_bg.b[0x1a6] = 2;
        wide_menu_sides();
        system_bg.b[0x1a6] = 0;
        wide_menu_sides();
    }
    int lines = log_lines;
    system_bg.b[0x1a6] = 2;
    expect(MARGIN, NULL);
    system_bg.b[0x1a6] = 0;
    expect(0, NULL);
    assert(log_lines == lines);

    puts("wide_bars_check: black sides under the pause and system menus (opening to closed), the open loading fade "
         "and movies, never for other screens; destroyed screens dropped, a full table logged once, registers kept");
    return 0;
}
