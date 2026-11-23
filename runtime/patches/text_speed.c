#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "text_speed.h"

enum { SPEED_NORMAL, SPEED_FAST, SPEED_INSTANT };
static const char *const speed_names[] = {"normal", "fast", "instant"};

#define PAGE_FACTOR 0x208
#define PAGE_INSTANT 0x20c

static int setting_speed(void) {
    const char *v = cfg_value("NX2SARIA_TEXT_SPEED");
    for (int i = 0; i < (int)(sizeof speed_names / sizeof *speed_names); i++)
        if (v && !strcmp(v, speed_names[i])) return i;
    return SPEED_NORMAL;
}

static _Atomic int allowed = -1;
static int speed_set(void) { return rt_patch_set("text_speed"); }
int text_speed_available(void) {
    int a = atomic_load(&allowed);
    if (a < 0) {
        a = rt_patch_enabled(speed_set()) && rt_patch_page() != NULL;
        atomic_store(&allowed, a);
    }
    return a;
}

void text_speed_poll(void) {
    if (!text_speed_available()) return;
    int s = setting_speed(), set = speed_set();
    uint8_t *page = rt_patch_page();
    float factor = s == SPEED_FAST ? TEXT_SPEED_FAST_FACTOR - 1.0f : 0.0f;
    uint32_t bits;
    memcpy(&bits, &factor, sizeof bits);
    __atomic_store_n((uint32_t *)(page + PAGE_FACTOR), bits, __ATOMIC_RELAXED);
    __atomic_store_n(page + PAGE_INSTANT, (uint8_t)(s == SPEED_INSTANT), __ATOMIC_RELAXED);
    static int logged = -1;
    if (s != logged) {
        logged = s;
        rt_log("text speed: %s", speed_names[s]);
    }
    if (rt_patch_enabled(set) != (s != SPEED_NORMAL)) rt_patch_switch(set, s != SPEED_NORMAL);
}
