#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t test_main_base;
const int rt_game_version = 111;

#include "changebank_hold.c"

#define MS 1000000ull

static int have_set = 1, set_on = 1;
int rt_patch_set(const char *name) { return have_set && !strcmp(name, "changebank_hold") ? 7 : -1; }
int rt_patch_enabled(int set) { return set == 7 && set_on; }
void rt_patch_switch(int set, int on) { assert(set == 7); set_on = on != 0; }
static const char *env_ms;
const char *cfg_get(const char *name) { return !strcmp(name, "NX2SARIA_CHANGEBANK_HOLD_MS") ? env_ms : NULL; }
static GuestFn host_fn;
void rt_register_host(uint64_t addr, GuestFn fn) {
    assert(addr == RT_PATCH_PAGE + 0xe30);
    host_fn = fn;
}
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static uint64_t clock_ns = 1000 * MS;
uint64_t plat_now_ns(void) { return clock_ns; }
static uint32_t site_word[1] = {0x9100054a};
static _Alignas(16) uint8_t mgr_mem[0x2000];
int mem_readable(uint64_t a, uint64_t n) {
    const uint64_t m = (uintptr_t)mgr_mem, s = (uintptr_t)site_word;
    return (a >= m && a + n <= m + sizeof mgr_mem) || (a >= s && a + n <= s + sizeof site_word);
}
static int log_lines;
static char last_log[1024], all_logs[16384];
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof last_log, fmt, ap);
    va_end(ap);
    log_lines++;
    if (strlen(all_logs) + strlen(last_log) + 2 < sizeof all_logs) strcat(strcat(all_logs, last_log), "\n");
}

static CbUnit U[CB_UNITS];
static void unit(int k, int use, uint64_t map) { U[k].in_use = use, U[k].map = map; }
enum { FIELD = 11, CASTLE = 22, VALLEY = 33, LAKE = 44 };

static void rule_checks(void) {
    CbState s = {.held = -1};
    CbStats st = {0};
    const uint64_t M = 0x1000, H = 2000 * MS;
    uint64_t t = 1000 * MS;
    memset(U, 0, sizeof U);
    unit(0, 1, FIELD), unit(1, 1, CASTLE), unit(2, 1, VALLEY), unit(3, 1, VALLEY), unit(4, 1, LAKE);
    assert(cb_step(&s, &st, M, U, 0, 0, t, H) == 0 && s.held == 0 && s.held_map == FIELD);
    for (int i = 0; i < 18; i++) assert(cb_step(&s, &st, M, U, 1, 0, t += 16 * MS, H) == 0);
    assert(st.holds == 1 && st.held_updates == 18 && st.flips == 1);
    assert(cb_step(&s, &st, M, U, 0, 0, t += 16 * MS, H) == 0 && st.absorbed == 1 && !s.have_cand && st.flips == 2);
    assert(st.asked == 0);
    const uint64_t left = t + 16 * MS;
    for (t = left; t - left < H; t += 16 * MS) assert(cb_step(&s, &st, M, U, 1, 0, t, H) == 0);
    assert(cb_step(&s, &st, M, U, 1, 0, t, H) == 1 && st.taken == 1 && s.held == 1 && s.held_map == CASTLE);
    assert(st.asked == 1);
    s = (CbState){.held = -1}, st = (CbStats){0};
    assert(cb_step(&s, &st, M, U, 2, 0, t += 16 * MS, H) == 2);
    assert(cb_step(&s, &st, M, U, 3, 0, t += 16 * MS, H) == 3 && s.held == 3 && st.holds == 0 && st.flips == 0);
    assert(cb_step(&s, &st, M, U, 2, 0, t += 16 * MS, H) == 2 && st.holds == 0);
    assert(cb_step(&s, &st, M, U, 4, 1, t += 16 * MS, H) == 4 && s.held == 4 && st.holds == 0);
    assert(cb_step(&s, &st, M, U, 0, 0, t += 16 * MS, H) == 4 && st.holds == 1);
    assert(cb_step(&s, &st, M, U, 0, 0, t += 600 * MS, H) == 0 && s.held == 0 && st.resets == 1);
    assert(cb_step(&s, &st, M + 0x2000, U, 1, 0, t += 16 * MS, H) == 1 && s.mgr == M + 0x2000);
    assert(cb_step(&s, &st, M + 0x2000, U, 0, 0, t += 16 * MS, H) == 1);
    unit(1, 0, CASTLE);
    assert(cb_step(&s, &st, M + 0x2000, U, 0, 0, t += 16 * MS, H) == 0 && s.held == 0 && st.resets == 2);
    unit(1, 1, CASTLE);
    assert(cb_step(&s, &st, M + 0x2000, U, 1, 0, t += 16 * MS, H) == 0);
    unit(0, 1, LAKE);
    assert(cb_step(&s, &st, M + 0x2000, U, 1, 0, t += 16 * MS, H) == 1 && st.resets == 3);
    unit(0, 1, FIELD);
    s = (CbState){.held = -1}, st = (CbStats){0};
    int want[] = {0, 1, 0, 1, 1, 0};
    for (int i = 0; i < 6; i++) assert(cb_step(&s, &st, M, U, want[i], 0, t += 16 * MS, 0) == want[i]);
    assert(st.flips == 4 && st.holds == 0 && st.held_updates == 0 && st.asked == 4);
    s = (CbState){.held = -1}, st = (CbStats){0};
    assert(cb_step(&s, &st, M, U, 0, 0, t += 16 * MS, H) == 0);
    const uint64_t away = t + 16 * MS;
    int k = 0;
    for (t = away; t - away < H; t += 16 * MS, k ^= 1) assert(cb_step(&s, &st, M, U, k ? 2 : 1, 0, t, H) == 0);
    assert(cb_step(&s, &st, M, U, 2, 0, t, H) == 2 && st.taken == 1 && st.holds > 2);
    s = (CbState){.held = -1}, st = (CbStats){0};
    assert(cb_step(&s, &st, M, U, 0, 0, t += 16 * MS, H) == 0);
    assert(cb_step(&s, &st, M, U, -1, 0, t += 16 * MS, H) == 0);
    assert(cb_step(&s, &st, M, U, -1, 0, t += H, H) == -1 && s.held == -1);
    assert(cb_step(&s, &st, M, U, -1, 0, t += 16 * MS, H) == -1);
    assert(cb_fnv1a("Map/HyruleField", 0x100) != cb_fnv1a("Map/HyruleCastle", 0x100));
    assert(cb_fnv1a("", 0x100) != 0 && cb_fnv1a("ab", 1) == cb_fnv1a("a", 0x100));
    puts("changebank_hold_check: rule: first choice, flip and back, taken after the hold, one map in two units, direct "
         "request, gap, another manager, held unit let go, hold 0, two other maps in turn, no map: ok");
}

static uint64_t M_ADDR;
static void path(int k, const char *p) {
    uint8_t *u = mgr_mem + UNIT0 + k * UNIT_SIZE;
    memset(u + UNIT_PATH_BUF, 0, PATH_BUF_SIZE);
    strcpy((char *)u + UNIT_PATH_BUF, p);
}
static void lay_out(void) {
    memset(mgr_mem, 0, sizeof mgr_mem);
    M_ADDR = (uintptr_t)mgr_mem;
    *(uint64_t *)mgr_mem = test_main_base + VTABLE;
    for (int k = 0; k < CB_UNITS; k++) {
        uint8_t *u = mgr_mem + UNIT0 + k * UNIT_SIZE;
        *(uint64_t *)(u + UNIT_PATH) = M_ADDR + UNIT0 + (uint64_t)k * UNIT_SIZE + UNIT_PATH_BUF;
        *(uint32_t *)(u + UNIT_PATH_SIZE) = PATH_BUF_SIZE;
    }
    mgr_mem[UNIT0 + 0 * UNIT_SIZE] = 1, path(0, "Map/Field/HyruleField_Child");
    mgr_mem[UNIT0 + 1 * UNIT_SIZE] = 1, path(1, "Map/Castle/HyruleCastle_Child");
}
static int update(int w15, int w17) {
    Cpu c;
    memset(&c, 0, sizeof c);
    c.x[30] = test_main_base + SITE_RET, c.x[19] = M_ADDR;
    for (int pass = 0; pass < CB_UNITS; pass++) {
        c.x[15] = pass < CB_UNITS - 1 ? 0x77 : (uint32_t)w15;
        c.x[17] = (uint32_t)w17;
        c.x[10] = c.x[10] + 1;
        host_fn(&c);
        if (pass < CB_UNITS - 1) assert(c.x[15] == 0x77);
    }
    clock_ns += 16 * MS;
    assert(c.x[15] >> 32 == 0 && c.x[17] == (uint32_t)w17 && c.x[10] == CB_UNITS);
    const int lt = (int32_t)c.x[15] < 0;
    const int gt = lt ? (int32_t)c.x[17] + 1 > 0 : 0;
    return gt ? (int32_t)c.x[17] : (int32_t)c.x[15];
}

static void host_checks(void) {
    lay_out();
    env_ms = NULL;
    changebank_hold_start();
    assert(strstr(last_log, "changebank hold: on, 2000 ms") && hold_ns == 2000 * MS);
    assert(update(0, -1) == 0);
    for (int i = 0; i < 10; i++) assert(update(1, -1) == 0);
    assert(update(0, -1) == 0 && stats.absorbed == 1);
    assert(strstr(last_log, "kept HyruleField_Child preloading while the game chose HyruleCastle_Child for 160 ms"));
    for (int i = 0; i < 125; i++) assert(update(1, -1) == 0);
    assert(update(1, -1) == 1 && stats.taken == 1);
    assert(strstr(last_log, "the game's choice left HyruleField_Child for the whole 2000 ms hold: HyruleCastle_Child is "
                            "taken in its place"));
    assert(update(-1, 1) == 1);
    assert(update(0, -1) == 1 && stats.absorbed == 1);
    assert(update(-1, -1) == 1);
    assert(update(-1, 0) == 0);
    Cpu c;
    memset(&c, 0, sizeof c);
    c.x[30] = test_main_base + 0x1234, c.x[15] = 5, c.x[10] = CB_UNITS;
    host_fn(&c);
    assert(c.x[15] == 5);
    memset(&c, 0, sizeof c);
    c.x[30] = test_main_base + SITE_RET, c.x[19] = M_ADDR, c.x[15] = 16, c.x[17] = (uint32_t)-1, c.x[10] = CB_UNITS;
    host_fn(&c);
    c.x[15] = 0x100000001ull;
    host_fn(&c);
    assert(c.x[15] == 0x100000001ull && odd_regs == 2 && strstr(all_logs, "w15 0x10 and w17 0xffffffff at main+0x3299d2c"));
    const uint64_t updates = stats.updates;
    int lines = log_lines;
    *(uint64_t *)mgr_mem = test_main_base + VTABLE + 8;
    assert(update(0, -1) == 0 && update(1, -1) == 1 && mismatched == 2 && stats.updates == updates);
    assert(log_lines == lines + 1 && strstr(last_log, "vtable is not main+0x4ba1a28"));
    *(uint64_t *)mgr_mem = test_main_base + VTABLE;
    *(uint64_t *)(mgr_mem + UNIT0 + 3 * UNIT_SIZE + UNIT_PATH) += 1;
    assert(update(0, -1) == 0 && mismatched == 3);
    lay_out();
    mgr_mem[UNIT0 + 5 * UNIT_SIZE] = 2;
    assert(update(0, -1) == 0 && mismatched == 4);
    lay_out();
    clock_ns += 61000 * MS;
    lines = log_lines;
    assert(update(0, -1) == 0);
    assert(log_lines == lines + 1 && strstr(last_log, "changebank hold: last 6") && strstr(last_log, " s, "));
    changebank_hold_exit_log();
    assert(strstr(last_log, "changebank hold: this session (the set on at the end, hold 2000 ms)") &&
           strstr(last_log, "4 updates with a manager that did not match, 2 with registers that were not units"));
    puts("changebank_hold_check: host: w15 held and taken at the loop's last pass, through the cmp, ccmn and csel, no map, direct requests, another "
         "caller, registers that are not units, managers that do not match, the minute and session lines: ok");
}

static void start_checks(void) {
    env_ms = "0";
    changebank_hold_start();
    assert(hold_ns == 0 && strstr(last_log, "on, hold 0 ms"));
    env_ms = "-5";
    changebank_hold_start();
    assert(hold_ns == 0);
    env_ms = "750";
    changebank_hold_start();
    assert(hold_ns == 750 * MS && strstr(last_log, "on, 750 ms"));
    env_ms = "12x";
    changebank_hold_start();
    assert(hold_ns == 2000 * MS && strstr(all_logs, "\"12x\" is not a number of milliseconds: 2000 is used"));
    env_ms = NULL;
    set_on = 0;
    changebank_hold_start();
    assert(strstr(last_log, "changebank hold: off (NX2SARIA_CHANGEBANK_HOLD=0)"));
    set_on = 1;
    site_word[0] = 0xd503201f;
    changebank_hold_start();
    assert(!set_on && strstr(all_logs, "main+0x3299d28 holds d503201f where 1.1.1 has 9100054a"));
    site_word[0] = 0x9100054a, set_on = 1;
    have_set = 0;
    changebank_hold_start();
    assert(strstr(last_log, "not in this game version"));
    have_set = 1;
    puts("changebank_hold_check: start: default, 0, negative, a number, not a number, off, another main, no set: ok");
}

int main(void) {
    test_main_base = (uintptr_t)site_word - SITE;
    rule_checks();
    assert(host_fn);
    host_checks();
    start_checks();
    return 0;
}
