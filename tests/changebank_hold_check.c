// The neighbouring map's hold (runtime/changebank_hold.c and changebank_hold.h) without the game: the rule over
// stand-in units, then the host function called as the lifted site calls it, over a stand-in manager laid out as
// 1.1.1's constructor (main+0xd12c00) lays it out. Checked: the first choice, a flip and its way back, a choice taken
// after the hold, two units of one map, a direct request, a gap in the updates, a held unit let go, a hold of 0, a
// choice that leaves for two other maps in turn, no map at all; then x21 as the host leaves it, a manager that does not
// match, registers that are not units, the start lines and the session line. Built by tests/changebank_hold_check.py,
// which writes the stand-in layout.h and patches.h.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t test_main_base;   // layout.h's MOD_MAIN_BASE
const int rt_game_version = 111;

#include "changebank_hold.c"

#define MS 1000000ull

// ---- the runtime the file calls
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
static uint32_t site_word[1] = {0x9100054a};   // main+0x3299d28 as 1.1.1 has it
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

// ---- the rule alone
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
    // the first choice is the game's
    assert(cb_step(&s, &st, M, U, 0, 0, t, H) == 0 && s.held == 0 && s.held_map == FIELD);
    // the castle chosen for 300 ms, then the field again: held all along, and absorbed
    for (int i = 0; i < 18; i++) assert(cb_step(&s, &st, M, U, 1, 0, t += 16 * MS, H) == 0);
    assert(st.holds == 1 && st.held_updates == 18 && st.flips == 1);
    assert(cb_step(&s, &st, M, U, 0, 0, t += 16 * MS, H) == 0 && st.absorbed == 1 && !s.have_cand && st.flips == 2);
    assert(st.asked == 0);   // the field asked for all along: no load cancelled
    // the castle for the whole hold: taken at 2000 ms, not before
    const uint64_t left = t + 16 * MS;
    for (t = left; t - left < H; t += 16 * MS) assert(cb_step(&s, &st, M, U, 1, 0, t, H) == 0);
    assert(cb_step(&s, &st, M, U, 1, 0, t, H) == 1 && st.taken == 1 && s.held == 1 && s.held_map == CASTLE);
    assert(st.asked == 1);   // one new load: the castle, once
    // two units of one map: the game's choice moves from unit 2 to unit 3, both the valley's: no hold, no load lost
    s = (CbState){.held = -1}, st = (CbStats){0};
    assert(cb_step(&s, &st, M, U, 2, 0, t += 16 * MS, H) == 2);
    assert(cb_step(&s, &st, M, U, 3, 0, t += 16 * MS, H) == 3 && s.held == 3 && st.holds == 0 && st.flips == 0);
    assert(cb_step(&s, &st, M, U, 2, 0, t += 16 * MS, H) == 2 && st.holds == 0);
    // a direct request is the game's at once
    assert(cb_step(&s, &st, M, U, 4, 1, t += 16 * MS, H) == 4 && s.held == 4 && st.holds == 0);
    // a gap over half a second (a loading screen): the choice after it is the game's
    assert(cb_step(&s, &st, M, U, 0, 0, t += 16 * MS, H) == 4 && st.holds == 1);
    assert(cb_step(&s, &st, M, U, 0, 0, t += 600 * MS, H) == 0 && s.held == 0 && st.resets == 1);
    // another manager (another area): the game's
    assert(cb_step(&s, &st, M + 0x2000, U, 1, 0, t += 16 * MS, H) == 1 && s.mgr == M + 0x2000);
    // the held unit let go by the game (no longer in use): the game's choice at once
    assert(cb_step(&s, &st, M + 0x2000, U, 0, 0, t += 16 * MS, H) == 1);
    unit(1, 0, CASTLE);
    assert(cb_step(&s, &st, M + 0x2000, U, 0, 0, t += 16 * MS, H) == 0 && s.held == 0 && st.resets == 2);
    unit(1, 1, CASTLE);
    // ...or its unit holding another map now
    assert(cb_step(&s, &st, M + 0x2000, U, 1, 0, t += 16 * MS, H) == 0);
    unit(0, 1, LAKE);
    assert(cb_step(&s, &st, M + 0x2000, U, 1, 0, t += 16 * MS, H) == 1 && st.resets == 3);
    unit(0, 1, FIELD);
    // a hold of 0: always the game's, the flips counted
    s = (CbState){.held = -1}, st = (CbStats){0};
    int want[] = {0, 1, 0, 1, 1, 0};
    for (int i = 0; i < 6; i++) assert(cb_step(&s, &st, M, U, want[i], 0, t += 16 * MS, 0) == want[i]);
    assert(st.flips == 4 && st.holds == 0 && st.held_updates == 0 && st.asked == 4);
    // away for two other maps in turn: the hold counts from the first update that left, so it ends all the same
    s = (CbState){.held = -1}, st = (CbStats){0};
    assert(cb_step(&s, &st, M, U, 0, 0, t += 16 * MS, H) == 0);
    const uint64_t away = t + 16 * MS;
    int k = 0;
    for (t = away; t - away < H; t += 16 * MS, k ^= 1) assert(cb_step(&s, &st, M, U, k ? 2 : 1, 0, t, H) == 0);
    assert(cb_step(&s, &st, M, U, 2, 0, t, H) == 2 && st.taken == 1 && st.holds > 2);
    // no map at all (Link left every radius): the held map stays for the hold, then none
    s = (CbState){.held = -1}, st = (CbStats){0};
    assert(cb_step(&s, &st, M, U, 0, 0, t += 16 * MS, H) == 0);
    assert(cb_step(&s, &st, M, U, -1, 0, t += 16 * MS, H) == 0);
    assert(cb_step(&s, &st, M, U, -1, 0, t += H, H) == -1 && s.held == -1);
    // nothing held and nothing chosen
    assert(cb_step(&s, &st, M, U, -1, 0, t += 16 * MS, H) == -1);
    // the hash: a path's bytes up to its end, never 0
    assert(cb_fnv1a("Map/HyruleField", 0x100) != cb_fnv1a("Map/HyruleCastle", 0x100));
    assert(cb_fnv1a("", 0x100) != 0 && cb_fnv1a("ab", 1) == cb_fnv1a("a", 0x100));
    puts("changebank_hold_check: rule: first choice, flip and back, taken after the hold, one map in two units, direct "
         "request, gap, another manager, held unit let go, hold 0, two other maps in turn, no map: ok");
}

// ---- the host over a stand-in manager
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
// One update as the lifted code runs it: the loop's 16 passes through the site (the add, then the host), the last with
// the loop's w15 and w17; then, after the loop, cmp w15, #0, ccmn w17, #1, #4, lt and csel w21, w17, w15, gt
// (main+0x3299d90..0x3299d9c) on the registers as the host left them. Returns w21.
static int update(int w15, int w17) {
    Cpu c;
    memset(&c, 0, sizeof c);
    c.x[30] = test_main_base + SITE_RET, c.x[19] = M_ADDR;
    for (int pass = 0; pass < CB_UNITS; pass++) {
        c.x[15] = pass < CB_UNITS - 1 ? 0x77 : (uint32_t)w15;   // mid-loop values the host must leave alone
        c.x[17] = (uint32_t)w17;
        c.x[10] = c.x[10] + 1;   // add x10, x10, #1
        host_fn(&c);
        if (pass < CB_UNITS - 1) assert(c.x[15] == 0x77);
    }
    clock_ns += 16 * MS;
    assert(c.x[15] >> 32 == 0 && c.x[17] == (uint32_t)w17 && c.x[10] == CB_UNITS);
    const int lt = (int32_t)c.x[15] < 0;                // cmp w15, #0
    const int gt = lt ? (int32_t)c.x[17] + 1 > 0 : 0;   // ccmn w17, #1, #4, lt: Z (not gt) when not lt
    return gt ? (int32_t)c.x[17] : (int32_t)c.x[15];   // csel w21, w17, w15, gt
}

static void host_checks(void) {
    lay_out();
    env_ms = NULL;
    changebank_hold_start();
    assert(strstr(last_log, "changebank hold: on, 2000 ms") && hold_ns == 2000 * MS);
    assert(update(0, -1) == 0);
    for (int i = 0; i < 10; i++) assert(update(1, -1) == 0);   // the castle held back
    assert(update(0, -1) == 0 && stats.absorbed == 1);
    assert(strstr(last_log, "kept HyruleField_Child preloading while the game chose HyruleCastle_Child for 160 ms"));
    for (int i = 0; i < 125; i++) assert(update(1, -1) == 0);   // 2000 ms of it
    assert(update(1, -1) == 1 && stats.taken == 1);
    assert(strstr(last_log, "the game's choice left HyruleField_Child for the whole 2000 ms hold: HyruleCastle_Child is "
                            "taken in its place"));
    assert(update(-1, 1) == 1);   // a direct request
    // the held map while the game names none (w15 and w17 -1), and while it asks for a unit directly
    assert(update(0, -1) == 1 && stats.absorbed == 1);   // the castle held, the field chosen: held back
    assert(update(-1, -1) == 1);   // no map: the castle stays
    assert(update(-1, 0) == 0);    // a direct request for the field: the game's at once
    // a call from another place of the slot changes nothing
    Cpu c;
    memset(&c, 0, sizeof c);
    c.x[30] = test_main_base + 0x1234, c.x[15] = 5, c.x[10] = CB_UNITS;
    host_fn(&c);
    assert(c.x[15] == 5);
    // registers that are not units: kept, logged once
    memset(&c, 0, sizeof c);
    c.x[30] = test_main_base + SITE_RET, c.x[19] = M_ADDR, c.x[15] = 16, c.x[17] = (uint32_t)-1, c.x[10] = CB_UNITS;
    host_fn(&c);
    c.x[15] = 0x100000001ull;
    host_fn(&c);
    assert(c.x[15] == 0x100000001ull && odd_regs == 2 && strstr(all_logs, "w15 0x10 and w17 0xffffffff at main+0x3299d2c"));
    // a manager that does not match: its updates keep the game's choice; one line
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
    // the minute line, once a minute and only when the choice moved
    clock_ns += 61000 * MS;
    lines = log_lines;
    assert(update(0, -1) == 0);   // after the gap: the game's
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
    site_word[0] = 0xd503201f;   // a main that differs at the site: the set goes off
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
