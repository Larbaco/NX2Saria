#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"

static int enabled;
static int adjustments;
static char last_log[160];

static int rt_patch_set(const char *name) {
    assert(!strcmp(name, "wide_balloon"));
    return 0;
}

static int rt_patch_enabled(int set) {
    assert(set == 0);
    return enabled;
}

static void rt_patch_switch(int set, int on) {
    assert(set == 0);
    enabled = on;
}

static void rt_log(const char *fmt, ...) {
    if (strstr(fmt, "anchor x")) adjustments++;
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, args);
    va_end(args);
}

#include "wide_balloon.inc"

static void check_anchor(float scale, float x) {
    Cpu c = {0};
    uint32_t bits;
    memcpy(&bits, &x, sizeof bits);
    c.v[0] = bits;
    c.v[1] = 0x42400000;
    c.x[8] = 0x500000;
    Cpu want = c;
    float expected = x / scale;
    memcpy(&bits, &expected, sizeof bits);
    want.v[0] = bits;
    wide_balloon_host(&c);
    assert(!memcmp(&c, &want, sizeof c));
    bits = (uint32_t)c.v[0];
    float shown;
    memcpy(&shown, &bits, sizeof shown);
    assert(fabsf(shown * scale - x) < 0.001f);
}

int main(void) {
    enabled = 1;
    wide_balloon_init(1.0f);
    assert(!enabled && strstr(last_log, "off, UI horizontal scale 1.0000"));

    wide_balloon_init(0.75f);
    assert(!enabled && strstr(last_log, "patch disabled or replaced"));

    enabled = 1;
    wide_balloon_init(0.75f);
    assert(enabled && strstr(last_log, "on, UI horizontal scale 0.7500"));
    check_anchor(0.75f, 480.0f);
    assert(adjustments == 1 && strstr(last_log, "anchor x 480.00 -> 640.00"));
    check_anchor(0.75f, -720.0f);

    wide_balloon_init(0.5f);
    check_anchor(0.5f, 0.0f);
    check_anchor(0.5f, 960.0f);
    assert(adjustments == 1);

    puts("wide_balloon_check: 16:9 stays native, 21:9 and 32:9 restore the anchor, and logging is bounded");
    return 0;
}
