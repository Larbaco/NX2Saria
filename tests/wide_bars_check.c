#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t test_main_base = 0x7000000000ull;

#include "wide_bars.c"

#define MENU_BG (0x4be4fb0ull)
#define LOADING_FADE (0x4be7b30ull)
#define EVENT_FADE (0x4be61c8ull)
#define SCREEN_BASE (0x4bec998ull)
#define UNMAPPED 0x1000ull

static int have_set = 1, set_on;
int rt_patch_set(const char *name) { return have_set && !strcmp(name, "wide_bars") ? 7 : -1; }
void rt_patch_switch(int set, int on) { assert(set == 7); set_on = on != 0; }
int mem_readable(uint64_t a, uint64_t n) { (void)n; return a && a != UNMAPPED; }
static int movie;
int ov_movie_live(void) { return movie; }

static int log_lines, full_lines;
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
#define TOP_MARGIN 0.05
#define expect(want, log_has) expect_at(__LINE__, want, 0, log_has)
static void expect_at(int line, double want_x, double want_y, const char *log_has) {
    int lines = log_lines;
    double got_x, got_y;
    wide_menu_bars(&got_x, &got_y);
    if (got_x != want_x || got_y != want_y) {
        fprintf(stderr, "wide_bars_check: line %d: margins %g/%g, expected %g/%g (last log: %s)\n", line, got_x,
                got_y, want_x, want_y, last_log);
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
    wide_bars_init(MARGIN, 0);
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
    make(&event, EVENT_FADE, 0);
    make(&other, 0x4b4dfc0ull, 2);

    setup((uintptr_t)&menu);
    setup((uintptr_t)&loading);
    setup((uintptr_t)&event);
    assert(watched() == 2 && !wide_menu_open() && !wide_loading_open());
    menu.b[0x1a6] = 2;
    assert(wide_menu_open() && !wide_loading_open());
    expect(0, NULL);
    menu.b[0x1a6] = 0;
    assert(!wide_menu_open());
    loading.b[0x1a6] = 1;
    assert(wide_loading_open() && !wide_menu_open());
    expect(0, NULL);
    loading.b[0x1a6] = 0;
    assert(!wide_loading_open());

    wide_bars_init(MARGIN, 0);
    assert(set_on);

    setup((uintptr_t)&menu);
    setup((uintptr_t)&system_bg);
    setup((uintptr_t)&loading);
    setup((uintptr_t)&event);
    setup((uintptr_t)&other);
    setup(0);
    setup(UNMAPPED);
    assert(watched() == 4);
    setup((uintptr_t)&menu);
    assert(watched() == 4);
    assert(strstr(last_log, "watching the event fade"));

    expect(0, NULL);
    expect(0, NULL);

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

    system_bg.b[0x1a6] = 2;
    expect(MARGIN, "pause or system menu");
    system_bg.b[0x1a6] = 0;
    expect(0, "shown again");

    loading.b[0x1a6] = 1;
    expect(0, NULL);
    loading.b[0x1a6] = 2;
    expect(MARGIN, "a loading screen is up");
    loading.b[0x1a6] = 3;
    expect(0, NULL);
    loading.b[0x1a6] = 0;
    expect(0, NULL);

    event.b[0x1a6] = 1;
    other.b[0x1a6] = 2;
    expect(MARGIN, "event fade is active");
    event.b[0x1a6] = 0;
    expect(0, NULL);

    movie = 1;
    expect(MARGIN, "a movie plays");
    menu.b[0x1a6] = 2;
    expect(MARGIN, NULL);
    movie = 0;
    expect(MARGIN, NULL);
    menu.b[0x1a6] = 0;
    expect(0, "the menu's background screen closed");

    menu.b[0x1a6] = 2;
    set_vt(&menu, SCREEN_BASE);
    expect(0, "is gone");
    assert(watched() == 3);
    make(&menu, MENU_BG, 1);
    setup((uintptr_t)&menu);
    assert(watched() == 4);
    expect(MARGIN, "pause or system menu");
    menu.b[0x1a6] = 0;
    expect(0, "shown again");

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

    set_vt(&loading, SCREEN_BASE);
    Screen late;
    make(&late, LOADING_FADE, 2);
    setup((uintptr_t)&late);
    assert(watched() == BARS_SCREENS);
    expect(MARGIN, "a loading screen is up");
    late.b[0x1a6] = 0;
    expect(0, "shown again");

    wide_bars_init(0, 0);
    late.b[0x1a6] = 1;
    expect_at(__LINE__, 0, 0, "no margins cleared");
    late.b[0x1a6] = 3;
    expect_at(__LINE__, 0, 0, NULL);
    late.b[0x1a6] = 0;
    expect(0, "full-height screens: ended");
    event.b[0x1a6] = 2;
    expect_at(__LINE__, 0, 0, "no margins cleared");
    event.b[0x1a6] = 0;
    expect(0, NULL);
    movie = 1;
    expect_at(__LINE__, 0, 0, "no margins cleared");
    movie = 0;
    expect(0, NULL);

    wide_bars_init(MARGIN, 0);
    for (int i = 0; i < BARS_TOLD; i++) {
        system_bg.b[0x1a6] = 2;
        wide_menu_bars(&(double){0}, &(double){0});
        system_bg.b[0x1a6] = 0;
        wide_menu_bars(&(double){0}, &(double){0});
    }
    int lines = log_lines;
    system_bg.b[0x1a6] = 2;
    expect(MARGIN, NULL);
    system_bg.b[0x1a6] = 0;
    expect(0, NULL);
    assert(log_lines == lines);

    puts("wide_bars_check: side and top/bottom margins follow centered screens, fades and movies; destroyed screens "
         "dropped, a full table logged once, registers kept");
    return 0;
}
