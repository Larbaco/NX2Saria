// The neighbouring map the game preloads, held for a while before another one is taken (Ocarina of Time 1.1.1, the
// patch set changebank_hold, recomp/patches-v111.txt). The rule is changebank_hold.h's; this file reads the guest and
// logs.
//
// The manager ("OptimizeChangeBank", vtable main+0x4ba1a28, made by main+0xd12c00) updates each frame through its
// vtable's +0x30, main+0x3299880 (x19 the manager). Its units are 16 of 0x128 bytes from +0x408: +0 in use (1), +2 asked
// for directly, +4..+0xc the place, +0x10 the map's path (a pointer the constructor sets to the unit's own buffer at
// +0x1c, +0x18 that buffer's size, 0x100), +0x120 the load radius, +0x124 the priority. The update's loop
// (main+0x3299c54..0x3299d34) leaves the best unit of those whose radius Link is in in w15 (by the heading while Link
// moves, else the nearest) and the direct one in w17 (-1 each for none), and main+0x3299d90..0x3299d9c take
// w21 = w15 < 0 && w17 >= 0 ? w17 : w15 (cmp w15, #0; fmov s0, #4.0; ccmn w17, #1, #4, lt; csel w21, w17, w15, gt).
// From there the update asks for unit w21's map (main+0x329a540, which does nothing when that map is the one asked
// for, manager +0x1e8, or loaded, +0x70, and otherwise starts it over the one loading) and lets go of every other unit
// it had asked for (main+0x329a400, unit +1 cleared).
//
// The site is the loop's add x10, x10, #1 at main+0x3299d28, which every pass of the loop goes through. It keeps the add
// and then calls the page's +0xe30 (changebank_hold_host), which returns at once until x10 is 16 (the last pass) and
// then may put the held unit in w15. The cmp w15, #0, ccmn and csel after the loop then take it: with w15 0 or more the
// ccmn sets Z and the csel takes w15, and with w15 -1 the update's choice is w17 or -1 as before. The host changes w15
// only when the game's choice is w15 or none, never for a direct request (w15 -1, w17 0 or more), so the csel's choice
// is the new w15. No flags are live at the site: the cmp x10 two words on writes them before anything reads them, and a
// patched call may not keep flags live across it (recomp/flags.py counts a call as no definition). x10 and x15 are
// caller-saved, so the lifted code reads them from the Cpu again after the call (recomp/regs.py); x30 is the update's
// own, saved in its prologue and read again only in its epilogue; nothing else changes.
//
// Every update the host checks the manager before it reads it: the vtable, every unit's in-use byte (0 or 1) and its
// path pointer and buffer size as the constructor set them. A manager that does not match gets the game's own choice
// at that update (counted, and logged once), and so does any update whose w15 or w17 is not a unit or -1.
//
// Switches, read once at the start: NX2SARIA_CHANGEBANK_HOLD=0 turns the set off (the game's own code at the site).
// NX2SARIA_CHANGEBANK_HOLD_MS (default 2000): how long the game's choice must have left the held map before it is
// taken; 0 (or a negative number) gives the game's choice at every update, with the counts still kept, so the log
// shows how often the game's own choice flips.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "changebank_hold.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#define SITE 0x3299d28ull         // add x10, x10, #1: the end of the loop over the units
#define SITE_WORD 0x9100054au
#define SITE_RET (SITE + 4)       // the bl's return address, after the kept add
#define VTABLE 0x4ba1a28ull
#define UNIT0 0x408
#define UNIT_SIZE 0x128
#define UNIT_PATH 0x10
#define UNIT_PATH_SIZE 0x18
#define UNIT_PATH_BUF 0x1c
#define PATH_BUF_SIZE 0x100
#define MGR_SPAN (UNIT0 + CB_UNITS * UNIT_SIZE)
#define HOST_SLOT 0xe30           // patch page slot (runtime/patches.c lists the others)
#define DEFAULT_HOLD_MS 2000
#define LOG_EVERY_NS 60000000000ull
#define EVENT_BUDGET 8            // "held ... for N ms" lines a session

static int set_index = -1;
static uint64_t hold_ns;
static PlatMutex lock = PLAT_MUTEX_INIT;
static CbState st_ = {.held = -1};
static CbStats stats, logged;    // under lock; logged: what the last periodic line said
static uint64_t mismatched, odd_regs, last_log_ns;
static int events;
static char held_name[64], cand_name[64];   // the held map and the last one held back, for the event lines

static uint64_t ld64(uint64_t a) { return *(const uint64_t *)(uintptr_t)a; }
static uint32_t ld32(uint64_t a) { return *(const uint32_t *)(uintptr_t)a; }
static uint8_t ld8(uint64_t a) { return *(const uint8_t *)(uintptr_t)a; }

// The units as changebank_hold.h takes them, or 0 when the manager is not laid out as the constructor makes it
static int read_units(uint64_t mgr, CbUnit *u, const char **why) {
    if (!mem_readable(mgr, MGR_SPAN)) return *why = "the manager is not readable", 0;
    if (ld64(mgr) != MOD_MAIN_BASE + VTABLE) return *why = "the manager's vtable is not main+0x4ba1a28", 0;
    for (int k = 0; k < CB_UNITS; k++) {
        const uint64_t unit = mgr + UNIT0 + (uint64_t)k * UNIT_SIZE;
        const uint8_t use = ld8(unit);
        if (use > 1) return *why = "a unit's in-use byte is not 0 or 1", 0;
        if (ld64(unit + UNIT_PATH) != unit + UNIT_PATH_BUF || ld32(unit + UNIT_PATH_SIZE) != PATH_BUF_SIZE)
            return *why = "a unit's path is not its own 0x100-byte buffer", 0;
        u[k].in_use = use == 1;
        u[k].map = 0;
        const char *p = (const char *)(uintptr_t)(unit + UNIT_PATH_BUF);
        if (use == 1 && p[0]) u[k].map = cb_fnv1a(p, PATH_BUF_SIZE);
    }
    return 1;
}

// The map's file name, from the last '/' of its path, cut to the buffer
static void map_name(uint64_t mgr, int unit, char *out, size_t n) {
    if (unit < 0) {
        snprintf(out, n, "(none)");
        return;
    }
    const char *p = (const char *)(uintptr_t)(mgr + UNIT0 + (uint64_t)unit * UNIT_SIZE + UNIT_PATH_BUF);
    size_t len = strnlen(p, PATH_BUF_SIZE - 1);
    const char *s = p;
    for (size_t i = 0; i < len; i++)
        if (p[i] == '/') s = p + i + 1;
    snprintf(out, n, "%.*s", (int)(len - (size_t)(s - p)), s);
}

static void log_window(uint64_t now) {
    const CbStats d = {
        stats.updates - logged.updates, stats.flips - logged.flips, stats.holds - logged.holds,
        stats.absorbed - logged.absorbed, stats.taken - logged.taken, stats.held_updates - logged.held_updates,
        stats.resets - logged.resets, stats.asked - logged.asked,
    };
    const double secs = (double)(now - last_log_ns) / 1e9;
    logged = stats;
    last_log_ns = now;
    if (!d.flips && !d.holds) return;
    rt_log("changebank hold: last %.0f s, %llu updates: the game's choice of map changed %llu times, the map asked for "
           "%llu times (each a request for another map); %llu choices held back, %llu back to the held map within the hold (a "
           "cancelled load and its reload avoided), %llu taken after %llu ms; %llu updates went on with the held map",
           secs, (unsigned long long)d.updates, (unsigned long long)d.flips, (unsigned long long)d.asked,
           (unsigned long long)d.holds, (unsigned long long)d.absorbed, (unsigned long long)d.taken,
           (unsigned long long)(hold_ns / 1000000), (unsigned long long)d.held_updates);
}

// The page's +0xe30, after main+0x3299d28's add at each pass of the loop over the units: x10 the passes done, and after
// the last one (16) w15 and w17 what the update chooses from; x19 the manager
static void changebank_hold_host(Cpu *c) {
    if (c->x[10] != CB_UNITS || c->x[30] - MOD_MAIN_BASE != SITE_RET) return;   // (a pass before the last one)
    const int32_t w15 = (int32_t)c->x[15], w17 = (int32_t)c->x[17];
    const int direct = w15 < 0 && w17 >= 0, game = direct ? w17 : w15;
    const uint64_t mgr = c->x[19], now = plat_now_ns();
    plat_mutex_lock(&lock);
    if (w15 < -1 || w15 >= CB_UNITS || w17 < -1 || w17 >= CB_UNITS || c->x[15] >> 32 || c->x[17] >> 32) {
        if (!odd_regs++)
            rt_log("changebank hold: w15 %#llx and w17 %#llx at main+0x3299d2c are not units of the manager or -1: this "
                   "update and any like it keep the game's choice", (unsigned long long)c->x[15],
                   (unsigned long long)c->x[17]);
        plat_mutex_unlock(&lock);
        return;
    }
    CbUnit u[CB_UNITS];
    const char *why = NULL;
    if (!read_units(mgr, u, &why)) {
        if (!mismatched++)
            rt_log("changebank hold: the manager at %#llx does not look as 1.1.1's constructor makes it (%s): its updates "
                   "keep the game's choice", (unsigned long long)mgr, why);
        plat_mutex_unlock(&lock);
        return;
    }
    const int held_before = st_.held, had_cand = st_.have_cand;
    const uint64_t absorbed = stats.absorbed, taken = stats.taken, since = st_.cand_since;
    const int pick = cb_step(&st_, &stats, mgr, u, game, direct, now, hold_ns);
    if (st_.have_cand && !had_cand) map_name(mgr, st_.held, held_name, sizeof held_name);
    if (st_.have_cand || stats.taken != taken) map_name(mgr, game, cand_name, sizeof cand_name);
    if ((stats.absorbed != absorbed || stats.taken != taken) && events < EVENT_BUDGET && held_before >= 0) {
        events++;
        if (stats.absorbed != absorbed)
            rt_log("changebank hold: kept %s preloading while the game chose %s for %llu ms, then it chose %s again%s",
                   held_name, cand_name, (unsigned long long)((now - since) / 1000000), held_name,
                   events == EVENT_BUDGET ? " (the last such line; the minute lines count them)" : "");
        else
            rt_log("changebank hold: the game's choice left %s for the whole %llu ms hold: %s is taken in its place%s",
                   held_name, (unsigned long long)(hold_ns / 1000000), cand_name,
                   events == EVENT_BUDGET ? " (the last such line; the minute lines count them)" : "");
    }
    if (!last_log_ns) last_log_ns = now;
    else if (now - last_log_ns >= LOG_EVERY_NS) log_window(now);
    plat_mutex_unlock(&lock);
    if (pick != game) c->x[15] = (uint32_t)pick;   // (never for a direct request: cb_step gives it back)
}

void changebank_hold_start(void) {
    set_index = rt_patch_set("changebank_hold");
    if (set_index < 0) {
        rt_log("changebank hold: not in this game version (the patch set changebank_hold is 1.1.1's)");
        return;
    }
    const uint64_t a = MOD_MAIN_BASE + SITE;
    if (mem_readable(a, 4) && *(const uint32_t *)(uintptr_t)a != SITE_WORD && rt_patch_enabled(set_index)) {
        rt_patch_switch(set_index, 0);
        rt_log("changebank hold: main+%#llx holds %08x where 1.1.1 has %08x: the set changebank_hold stays off and the "
               "game runs its own code there", (unsigned long long)SITE, *(const uint32_t *)(uintptr_t)a, SITE_WORD);
    }
    const char *v = cfg_get("NX2SARIA_CHANGEBANK_HOLD_MS");
    long ms = DEFAULT_HOLD_MS;
    if (v && *v) {
        char *end;
        long l = strtol(v, &end, 10);
        if (end != v && !*end) {
            ms = l < 0 ? 0 : l > 60000 ? 60000 : l;
            if (l > 60000) rt_log("changebank hold: NX2SARIA_CHANGEBANK_HOLD_MS = %ld is over 60000: 60000 is used", l);
        } else rt_log("changebank hold: NX2SARIA_CHANGEBANK_HOLD_MS = \"%s\" is not a number of milliseconds: %d is used",
                    v, DEFAULT_HOLD_MS);
    }
    hold_ns = (uint64_t)ms * 1000000;
    if (!rt_patch_enabled(set_index))
        rt_log("changebank hold: off (NX2SARIA_CHANGEBANK_HOLD=0): the game preloads whichever neighbouring map it "
               "chooses at each update, as shipped");
    else if (!hold_ns)
        rt_log("changebank hold: on, hold 0 ms (NX2SARIA_CHANGEBANK_HOLD_MS = 0): the game's own choice of the "
               "neighbouring map to preload at every update, counted for the log");
    else
        rt_log("changebank hold: on, %ld ms: the neighbouring map the game preloads changes only after its choice has "
               "left it for %ld ms, so walking between two maps' load radii no longer cancels a load and starts "
               "another (patch set changebank_hold; NX2SARIA_CHANGEBANK_HOLD=0 turns it off, "
               "NX2SARIA_CHANGEBANK_HOLD_MS sets the time)", ms, ms);
}

void changebank_hold_exit_log(void) {
    if (set_index < 0) return;
    plat_mutex_lock(&lock);
    rt_log("changebank hold: this session (the set %s at the end, hold %llu ms) %llu updates: the game's choice of map "
           "changed %llu times, the map asked for %llu times; %llu choices held back, %llu back to the held map within the hold, %llu taken after "
           "the hold, %llu updates with the held map, %llu holds started again; %llu updates with a manager that did "
           "not match, %llu with registers that were not units",
           rt_patch_enabled(set_index) ? "on" : "off", (unsigned long long)(hold_ns / 1000000),
           (unsigned long long)stats.updates, (unsigned long long)stats.flips, (unsigned long long)stats.asked,
           (unsigned long long)stats.holds,
           (unsigned long long)stats.absorbed, (unsigned long long)stats.taken,
           (unsigned long long)stats.held_updates, (unsigned long long)stats.resets, (unsigned long long)mismatched,
           (unsigned long long)odd_regs);
    plat_mutex_unlock(&lock);
}

#ifdef RT_PATCH_PAGE
__attribute__((constructor)) static void changebank_hold_register(void) {
    if (!RT_V111) return;   // the set's site is in 1.1.1's main only
    rt_register_host(RT_PATCH_PAGE + HOST_SLOT, changebank_hold_host);
}
#endif
