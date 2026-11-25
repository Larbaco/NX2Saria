#include <assert.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t test_main_base;
const int rt_game_version = 111;

#include "sprint.c"

static const char *button = "off", *mode = "toggle";
static GuestFn link_fn, stance_fn;
const char *cfg_value(const char *key) {
    if (!strcmp(key, "NX2SARIA_SPRINT_BUTTON")) return button;
    if (!strcmp(key, "NX2SARIA_SPRINT_MODE")) return mode;
    return "";
}
static int set_on = 1;
int rt_patch_set(const char *name) { return strcmp(name, "sprint") ? -1 : 3; }
int rt_patch_enabled(int set) { return set == 3 && set_on; }
void rt_patch_switch(int set, int on) { assert(set == 3); set_on = on != 0; }
void rt_register_host(uint64_t addr, GuestFn fn) {
    if (addr == RT_PATCH_PAGE + 0xf40) link_fn = fn;
    else if (addr == RT_PATCH_PAGE + 0xf30) stance_fn = fn;
    else abort();
}

static float step = 1.0f;
float frame_step_frames(void) { return step; }
int mem_readable(uint64_t a, uint64_t n) { (void)n; return a != 0; }
static int log_lines;
static char last_log[256];
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    log_lines++;
}

static uint64_t link[8], owner[0x50], a[0x40], b[0x40], state_obj[0x20], other[8];
static uint64_t manager[0x90], controlled[8];

static void set_state(int32_t id) { memcpy((char *)state_obj + 0x28, &id, 4); }

static uint64_t poll_buttons, flags_x10, flags_w8;
static uint64_t frame(uint64_t buttons, int32_t state, uint64_t *who) {
    set_state(state);
    sprint_pad(&buttons);
    poll_buttons = buttons;
    Cpu c;
    memset(&c, 0, sizeof c);
    c.x[19] = (uintptr_t)who;
    c.x[10] = 0x40;
    if (set_on) link_fn(&c);
    flags_x10 = c.x[10];
    c.x[8] = 0x01;
    if (set_on) stance_fn(&c);
    flags_w8 = c.x[8];
    return buttons;
}
static int dashing(void) { return (flags_x10 & 0x10) && (flags_w8 & 0x10) && (flags_x10 & 0x40) && (flags_w8 & 1); }

enum { WALK = 1, JUMP = 13, SLIDE = 35, WAIT_ID = 40, ROLL = 99, ATTACK = 112, CUTSCENE = 0 };
#define L3 (1u << 4)
#define BB (1u << 1)
#define LL (1u << 6)
#define ZL (1u << 8)

static void reset(const char *btn, const char *md) {
    button = btn, mode = md;
    for (int i = 0; i < 12; i++) frame(0, CUTSCENE, link);
    assert(!dashing());
}

int main(int argc, char **argv) {
    char *img = calloc(1, 0x4c80000);
    assert(img);
    test_main_base = (uintptr_t)img;
    assert(link_fn && stance_fn);
    if (argc > 1 && !strcmp(argv[1], "kept-off")) {
        set_on = 0, button = "l3";
        assert(!sprint_available());
        uint64_t bt = L3;
        for (int i = 0; i < 20; i++) bt = L3, sprint_pad(&bt);
        assert(!set_on && bt == L3);
        puts("sprint_check kept-off: ok");
        return 0;
    }
    assert(sprint_available());
    link[0] = test_main_base + 0x4b4dfc0, link[3] = (uintptr_t)owner;
    other[0] = test_main_base + 0x4b4dfc0, other[3] = (uintptr_t)a;
    owner[0x270 / 8] = (uintptr_t)a, a[0x188 / 8] = (uintptr_t)b, b[0x1c0 / 8] = (uintptr_t)state_obj;
    uint64_t *slot = (uint64_t *)(img + 0x4c7e0d0);
    uint64_t mgr_ptr = (uintptr_t)manager;
    *slot = (uintptr_t)&mgr_ptr;
    manager[0] = test_main_base + 0x4bdfe68, manager[0x470 / 8] = (uintptr_t)controlled;
    controlled[0] = test_main_base + 0x4b49698, controlled[3] = (uintptr_t)owner;

    reset("off", "toggle");
    assert(frame(L3, WALK, link) == L3 && !dashing() && !set_on);
    button = "l3";
    frame(0, WALK, link);
    assert(set_on);

    reset("l3", "toggle");
    assert(frame(L3, WALK, link) == 0 && dashing());
    assert(frame(L3, WALK, link) == 0 && dashing());
    assert(frame(0, WALK, link) == 0 && dashing());
    assert(frame(0, JUMP, link) == 0 && dashing());
    assert(frame(0, SLIDE, link) == 0 && dashing());
    assert(frame(0, ROLL, link) == 0 && dashing());
    assert(frame(0, ROLL, link) == 0 && dashing());
    assert(frame(0, WALK, link) == 0 && dashing());
    frame(L3, WALK, link);
    assert(!dashing());
    frame(0, WALK, link);
    assert(!dashing());

    frame(L3, WALK, link), frame(0, WALK, link);
    for (int i = 0; i < 4; i++) assert((frame(0, WAIT_ID, link), dashing()));
    frame(0, WAIT_ID, link);
    assert(!dashing());
    frame(L3, WAIT_ID, link), frame(0, WAIT_ID, link);
    assert(dashing());
    for (int i = 0; i < 60; i++) frame(0, WAIT_ID, link);
    assert(dashing());
    frame(0, WALK, link);
    assert(dashing());
    frame(L3, WALK, link);
    assert(!dashing());
    frame(L3, WAIT_ID, link), frame(0, WAIT_ID, link);
    for (int i = 0; i < 90; i++) frame(0, WAIT_ID, link);
    assert(!dashing());

    const int32_t enders[] = {ATTACK, CUTSCENE};
    for (int k = 0; k < 2; k++) {
        frame(L3, WALK, link), frame(0, WALK, link);
        assert(dashing());
        frame(0, enders[k], link);
        assert(!dashing());
        frame(0, WALK, link);
        assert(!dashing());
    }
    frame(L3, WALK, link), frame(0, WALK, link);
    assert(frame(ZL, WALK, link) == ZL && !dashing());

    frame(L3, ATTACK, link);
    assert(!dashing());

    frame(0, WALK, link), frame(L3, WALK, link);
    assert(dashing());
    frame(0, WALK, other);
    assert(!(flags_x10 & 0x10) && !(flags_w8 & 0x10));
    frame(0, WALK, link);
    assert(dashing());
    frame(L3, WALK, link);

    {
        uint64_t got = 0;
        sprint_pad(&got);
        for (int i = 0; i < 9; i++) {
            uint64_t bt = L3;
            sprint_pad(&bt);
            got = bt;
        }
        assert(got == L3);
        frame(0, WALK, link);
        assert(dashing());
        frame(L3, WALK, link);
        assert(!dashing());
    }

    reset("b", "toggle");
    assert(frame(BB, WAIT_ID, link) == BB && !dashing());
    frame(0, WALK, link);
    assert(frame(BB, WALK, link) == 0 && dashing());
    assert(frame(0, WALK, link) == 0 && dashing());
    frame(BB, WALK, link);
    assert(!dashing() && poll_buttons == 0);
    frame(0, ATTACK, link);
    assert(frame(BB, ATTACK, link) == BB && !dashing());

    reset("l3", "hold");
    assert(frame(L3, WALK, link) == 0 && dashing());
    assert(frame(L3, WAIT_ID, link) == 0 && dashing());
    assert(frame(0, WALK, link) == 0 && !dashing());
    assert(frame(LL, WALK, link) == LL && !dashing());

    reset("r3", "toggle");
    assert(frame(1u << 5, WALK, link) == (1u << 5) && !dashing() && !set_on);

    reset("b", "hold");
    frame(0, WALK, link);
    assert(frame(BB, WALK, link) == 0 && dashing());
    assert((frame(BB, JUMP, link), dashing()));
    assert(frame(0, WALK, link) == 0 && !dashing());
    frame(0, WAIT_ID, link);
    assert(frame(BB, WAIT_ID, link) == BB && !dashing());
    assert(frame(BB, WALK, link) == BB && !dashing());
    assert(frame(BB, WALK, link) == BB && !dashing());
    frame(0, WALK, link);
    assert(frame(BB, WALK, link) == 0 && dashing());
    assert(frame(BB, WAIT_ID, link) == 0);
    frame(0, WAIT_ID, link);

    reset("l3", "toggle");
    frame(0, WALK, link);
    {
        uint64_t bt = 0;
        for (int i = 0; i < 10; i++) bt = 0, sprint_pad(&bt);
        bt = L3, sprint_pad(&bt);
        assert(bt == L3);
        bt = 0, sprint_pad(&bt);
    }
    frame(0, WALK, link);
    assert(!dashing());

    reset("l3", "toggle");
    {
        uint64_t event[0x10] = {0}, ev_ptr = (uintptr_t)event;
        *(uint64_t *)(img + 0x4c7e090) = (uintptr_t)&ev_ptr;
        frame(0, WALK, link);
        event[0x68 / 8] = 1;
        int lines = log_lines;
        assert(frame(L3, WAIT_ID, link) == L3 && !dashing());
        assert(log_lines == lines + 1 && strstr(last_log, "while an event runs"));
        assert(frame(L3, WAIT_ID, link) == L3 && !dashing());
        assert(frame(0, WAIT_ID, link) == 0 && !dashing());
        event[0x68 / 8] = 0;
        frame(0, WALK, link);
        assert(frame(L3, WALK, link) == 0 && dashing());
        frame(0, WALK, link), frame(L3, WALK, link), frame(0, WALK, link);
        *(uint64_t *)(img + 0x4c7e090) = 0;
    }

    reset("l3", "toggle");
    step = 0.25f;
    frame(L3, WALK, link), frame(0, WALK, link);
    for (int i = 0; i < 4; i++) frame(0, WAIT_ID, link);
    assert(dashing());
    for (int i = 0; i < 16; i++) frame(0, WAIT_ID, link);
    assert(!dashing());
    step = 1.0f;

    reset("l3", "toggle");
    frame(L3, WALK, link), frame(0, WALK, link);
    assert(dashing());
    button = "off", frame(0, WALK, link);
    button = "l3", frame(0, WALK, link);
    assert(!dashing());

    puts("sprint_check: ok");
    free(img);
    return 0;
}
