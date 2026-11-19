// The voice resource's update with the guest option calls stubbed. Built by tests/voice_language_check.py.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "layout.h"

void voice_language_host(Cpu *c);

#define MANAGER 0x123400ull
#define OPTION_KEY 0xa2dcdc9dull

static uint32_t option;
static int found, written, calls, reports;
static char last_log[160];

uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    va_list args;
    va_start(args, n);
    assert(va_arg(args, uint64_t) == MANAGER);
    uint64_t value;
    if (fn == MOD_MAIN_BASE + 0xa7d1c0) {
        assert(n == 2 && (calls++ & 1) == 0);
        assert(va_arg(args, uint64_t) == OPTION_KEY);
        value = (uint64_t)written;
    } else {
        assert(fn == MOD_MAIN_BASE + 0x7e73d0);
        assert(n == 3 && (calls++ & 1) == 1);
        uint32_t *out = (void *)(uintptr_t)va_arg(args, uint64_t);
        assert(va_arg(args, uint64_t) == OPTION_KEY);
        if (found) *out = option;
        value = (uint64_t)found;
    }
    va_end(args);
    c->x[0] = value;
    c->x[1] = 0;
    return value;
}

void rt_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, args);
    va_end(args);
    reports++;
}

static void update(int has_option, int has_write, uint32_t saved_value, int take_branch, int expected_reports) {
    Cpu c = {0};
    c.x[0] = MANAGER;
    c.x[1] = OPTION_KEY;
    found = has_option;
    written = has_write;
    option = saved_value;
    voice_language_host(&c);
    assert(c.x[0] == (uint64_t)take_branch);
    assert(reports == expected_reports);
}

int main(void) {
    update(0, 0, 0, 0, 0);   // the saved option is not ready
    update(1, 0, 0, 1, 1);   // a successful read of zero still has a value
    assert(strstr(last_log, "refresh 1 for initial option 0x00000000"));
    update(1, 0, 0, 0, 1);
    update(1, 1, 3, 1, 1);   // the stock write triggers the refresh and advances our observation
    update(1, 0, 3, 0, 1);
    update(1, 0, 5, 1, 2);   // a new saved value without a write notification
    assert(strstr(last_log, "refresh 2 for option 0x00000003 -> 0x00000005"));
    update(0, 1, 0, 1, 2);   // an unavailable lookup leaves the stock write result intact
    update(1, 0, 5, 0, 2);   // a temporarily missing value does not repeat the refresh
    for (unsigned i = 6; i < 16; ++i) update(1, 0, i, 1, i < 12 ? (int)i - 3 : 8);
    assert(calls == 36);
    puts("voice_language_check: delayed option, saved value changes, stock writes and log limit pass");
    return 0;
}
