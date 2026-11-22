// The camera's clearance on pictures that are not 16:9 (runtime/patches/camera_clearance.c) without the game: the host called
// as the two lifted sites call it, with the page's ratios set as widescreen.c sets them. Checked: a wide picture's
// ratio made 16:9 in the load's own register, other values and 16:9 pictures left alone, a call from elsewhere, the
// start lines and the session lines. Built by tests/camera_clearance_check.py, which writes the stand-in layout.h and
// patches.h.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint64_t test_main_base;
const int rt_game_version = 111;

#include "camera_clearance.c"

static int have_set = 1, set_on = 1;
int rt_patch_set(const char *name) { return have_set && !strcmp(name, "camera_clearance") ? 9 : -1; }
int rt_patch_enabled(int set) { return set == 9 && set_on; }
void rt_patch_switch(int set, int on) { assert(set == 9); set_on = on != 0; }
static uint8_t page[4096];
void *rt_patch_page(void) { return page; }
static uint32_t words[2] = {0xbd451e6a, 0xbd451e6e};   // main+0x71a844 and main+0x71b624 as 1.1.1 has them
int mem_readable(uint64_t a, uint64_t n) {
    return a >= (uintptr_t)words && a + n <= (uintptr_t)words + sizeof words;
}
static GuestFn host_fn;
void rt_register_host(uint64_t addr, GuestFn fn) {
    assert(addr == RT_PATCH_PAGE + 0xe00);
    host_fn = fn;
}
static char last_log[512], all_logs[8192];
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    if (strlen(all_logs) + strlen(last_log) + 2 < sizeof all_logs) strcat(strcat(all_logs, last_log), "\n");
}

static void set_page(float picture) {
    float base = 16.0f / 9.0f;
    memcpy(page + 112, &base, 4);
    memcpy(page + 116, &picture, 4);
}
// the site's load (the value in the register's low lane, the rest zero) and then the host; returns the register's
// low lane as a float, and checks the upper lanes stay zero
static float load(uint64_t site, int reg, float value) {
    Cpu c;
    memset(&c, 0, sizeof c);
    uint32_t bits;
    memcpy(&bits, &value, 4);
    c.v[reg] = bits;
    c.x[30] = test_main_base + site + 4;
    host_fn(&c);
    assert(c.v[reg] >> 32 == 0);
    bits = (uint32_t)c.v[reg];
    float out;
    memcpy(&out, &bits, 4);
    return out;
}

int main(void) {
    test_main_base = (uintptr_t)words - 0x71a844;   // so the start check reads the two words above
    // (the second site is 0xde0 bytes after the first; the start check reads words[1] there only through a stand-in)
    assert(host_fn);
    const float base = 16.0f / 9.0f, uw = 2560.0f / 1080.0f, wide = 32.0f / 9.0f;
    camera_clearance_start();
    assert(strstr(last_log, "camera clearance: on:"));
    // 21:9: the picture's ratio becomes 16:9 at both sites, in the load's own register
    set_page(uw);
    assert(load(SITE_A, 10, uw) == base);
    assert(strstr(last_log, "the first ratio at main+0x71a844 is 2.37037 (the picture's 2.37037") &&
           strstr(last_log, "made 16:9 for the clearance"));
    assert(load(SITE_B, 14, uw) == base);
    assert(load(SITE_A, 10, uw * 1.0005f) == base);   // within 0.1%
    assert(load(SITE_A, 10, uw * 1.005f) == uw * 1.005f);   // another value stays
    assert(load(SITE_A, 10, base) == base);
    assert(load(SITE_A, 10, 1.0f) == 1.0f);   // a square view stays
    // 32:9 too
    set_page(wide);
    assert(load(SITE_B, 14, wide) == base);
    // a 16:9 picture: nothing changes, even a ratio that matches it
    set_page(base);
    assert(load(SITE_A, 10, base) == base);
    // a call from elsewhere changes nothing
    Cpu c;
    memset(&c, 0, sizeof c);
    c.v[10] = 0x40000000u;
    c.x[30] = test_main_base + 0x1234;
    host_fn(&c);
    assert(c.v[10] == 0x40000000u);
    camera_clearance_exit_log();
    assert(strstr(all_logs, "this session at main+0x71a844: 2 loads of 16:9, 2 of the picture's ratio (2 made 16:9), 2 of "
                            "other ratios"));
    assert(strstr(all_logs, "this session at main+0x71b624: 0 loads of 16:9, 2 of the picture's ratio (2 made 16:9), 0 of "
                            "other ratios"));
    // the start: off, another main, no set
    set_on = 0;
    camera_clearance_start();
    assert(strstr(last_log, "camera clearance: off (NX2SARIA_CAMERA_CLEARANCE_FIX=0)"));
    set_on = 1;
    words[0] = 0xd503201f;
    camera_clearance_start();
    assert(!set_on && strstr(all_logs, "main+0x71a844 holds d503201f where 1.1.1 has bd451e6a"));
    words[0] = 0xbd451e6a, set_on = 1;
    have_set = 0;
    int before = (int)strlen(all_logs);
    camera_clearance_start();
    camera_clearance_exit_log();
    assert((int)strlen(all_logs) == before);   // no set: no lines
    puts("camera_clearance_check: host: a wide picture's ratio made 16:9 at both sites, other values, 16:9 pictures and "
         "other callers left alone, the start and session lines: ok");
    return 0;
}
