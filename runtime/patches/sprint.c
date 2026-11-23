#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "sprint.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

enum { BTN_OFF, BTN_B, BTN_L3 };
static const char *const button_names[] = {"off", "b", "l3"};
static const uint64_t button_bits[] = {0, 1u << 1, 1u << 4};
#define BUTTON_ZL (1u << 8)

#define DASH_INPUT 0x10u
#define LINK_VTABLE 0x4b4dfc0ull
#define GAMEPLAY_POLLS 8
#define WAIT_STOP_FRAMES 4.0
#define WAIT_START_FRAMES 90.0

enum { MOVE, KEEP, WAIT, STOP };
static const struct { int32_t id; uint8_t kind; } states[] = {
    {1, MOVE},
    {13, MOVE},
    {32, MOVE},
    {33, MOVE},
    {68, MOVE},
    {253, KEEP},
    {35, KEEP},
    {36, KEEP},
    {99, KEEP},
    {40, WAIT},
};

static _Atomic uint64_t pressed;
static _Atomic int held;
static _Atomic int zl_held;
static _Atomic int polls = GAMEPLAY_POLLS;
static _Atomic int moving;
static _Atomic int active;
static _Atomic int latched;
static int moved;
static double wait_frames;
static int last_button = -1;

static int setting_button(void) {
    const char *v = cfg_value("NX2SARIA_SPRINT_BUTTON");
    for (int i = 0; i < (int)(sizeof button_names / sizeof *button_names); i++)
        if (v && !strcmp(v, button_names[i])) return i;
    return BTN_OFF;
}

static int setting_hold(void) {
    const char *v = cfg_value("NX2SARIA_SPRINT_MODE");
    return v && !strcmp(v, "hold");
}

static uint64_t ld64(uint64_t a) { return a && mem_readable(a, 8) ? *(const uint64_t *)(uintptr_t)a : 0; }
static int32_t ld32(uint64_t a) { return a && mem_readable(a, 4) ? *(const int32_t *)(uintptr_t)a : 0; }

static int is_link(uint64_t link) {
    if (!link || ld64(link) != MOD_MAIN_BASE + LINK_VTABLE) return 0;
    uint64_t slot = ld64(MOD_MAIN_BASE + 0x4c7e0d0), mgr = slot ? ld64(slot) : 0;
    if (!mgr || ld64(mgr) != MOD_MAIN_BASE + 0x4bdfe68) return 1;
    uint64_t ctl = ld64(mgr + 0x470);
    if (!ctl || ld64(ctl) != MOD_MAIN_BASE + 0x4b49698) return 1;
    return ld64(ctl + 0x18) == ld64(link + 0x18);
}

#define EVENT_MGR 0x4c7e090ull
static int event_running(void) {
    uint64_t slot = ld64(MOD_MAIN_BASE + EVENT_MGR), mgr = slot ? ld64(slot) : 0;
    return mgr && ld64(mgr + 0x68);
}

static int state_kind(uint64_t link) {
    uint64_t p = ld64(link + 0x18);
    if (p) p = ld64(p + 0x270);
    if (p) p = ld64(p + 0x188);
    if (p) p = ld64(p + 0x1c0);
    if (!p) return STOP;
    int32_t id = ld32(p + 0x28);
    if (id == 0x11a) id = ld32(p + 0x4c);
    for (size_t i = 0; i < sizeof states / sizeof *states; i++)
        if (states[i].id == id) return states[i].kind;
    return STOP;
}

static _Atomic int allowed = -1;
static int sprint_set(void) { return rt_patch_set("sprint"); }
int sprint_available(void) {
    int a = atomic_load(&allowed);
    if (a < 0) {
        a = rt_patch_enabled(sprint_set());
        atomic_store(&allowed, a);
    }
    return a;
}

void sprint_pad(uint64_t *buttons) {
    if (!sprint_available()) return;
    int b = setting_button(), set = sprint_set();
    if (rt_patch_enabled(set) != (b != BTN_OFF)) rt_patch_switch(set, b != BTN_OFF);
    if (b == BTN_OFF) {
        atomic_store(&active, 0), atomic_store(&pressed, 0), atomic_store(&latched, 0);
        return;
    }
    static uint64_t last;
    uint64_t bit = button_bits[b], now = *buttons;
    int n = atomic_load(&polls), gameplay = n < GAMEPLAY_POLLS && !event_running();
    if (n <= GAMEPLAY_POLLS) atomic_store(&polls, n + 1);
    if (now & ~last & bit) {
        int mine = gameplay && (b != BTN_B || atomic_load(&moving));
        static int told;
        if (n < GAMEPLAY_POLLS && !gameplay && told < 16) {
            told++;
            rt_log("sprint: the sprint button was pressed while an event runs (a conversation, a cutscene): the game's "
                   "press, no sprint (L3 hides a conversation's text box)");
        }
        atomic_store(&latched, mine);
        if (mine) atomic_fetch_or(&pressed, bit);
    } else if (!(now & bit)) {
        atomic_store(&latched, 0);
    }
    last = now;
    atomic_store(&held, (now & bit) != 0);
    atomic_store(&zl_held, (now & BUTTON_ZL) != 0);
    if (atomic_load(&latched) && gameplay) *buttons &= ~bit;
}

int sprint_step(int kind, int button, int hold) {
    if (button != last_button) last_button = button, atomic_store(&active, 0), atomic_store(&pressed, 0);
    int press = (atomic_exchange(&pressed, 0) & button_bits[button]) != 0, on = atomic_load(&active);
    if (atomic_load(&zl_held)) kind = STOP;
    atomic_store(&moving, kind == MOVE);
    if (button == BTN_OFF) on = 0;
    else if (hold) on = atomic_load(&held) && atomic_load(&latched) && kind != STOP;
    else {
        if (press) {
            if (on) on = 0;
            else if (kind != STOP && (button != BTN_B || kind == MOVE)) on = 1, moved = kind == MOVE, wait_frames = 0;
        } else if (on) {
            if (kind == STOP) on = 0;
            else if (kind == WAIT) {
                wait_frames += frame_step_frames();
                if (wait_frames > (moved ? WAIT_STOP_FRAMES : WAIT_START_FRAMES)) on = 0;
            } else {
                wait_frames = 0;
                moved |= kind == MOVE;
            }
        }
    }
    atomic_store(&active, on);
    return on;
}

static void sprint_link(Cpu *c) {
    if (!is_link(c->x[19])) return;
    atomic_store(&polls, 0);
    if (sprint_step(state_kind(c->x[19]), setting_button(), setting_hold())) c->x[10] |= DASH_INPUT;
}

static void sprint_stance(Cpu *c) {
    if (atomic_load(&active) && is_link(c->x[19])) c->x[8] |= DASH_INPUT;
}

#ifdef RT_PATCH_PAGE
__attribute__((constructor)) static void sprint_register(void) {
    if (!RT_V111) return;
    rt_register_host(RT_PATCH_PAGE + 0xf40, sprint_link);
    rt_register_host(RT_PATCH_PAGE + 0xf30, sprint_stance);
}
#endif
