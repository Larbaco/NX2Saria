// The amiibo daily limit for Ocarina of Time 1.1.1 (the patch set amiibo_daily, recomp/patches-v111.txt). A figure that
// gives random items (docs/amiibo.md names them) gives them once a day; after that the game says "You cannot use this
// amiibo anymore today" (SimpleInfo_00 CannotUseAmiiboToday). The rule is the field EnableDailyLimit of each drop table
// in the game's amiibo table: the two mask figures' tables set it false, the others leave it out, and a table without
// it keeps the limit. The set's one site makes the game read false for every table it matches a figure with.
//
// Setting amiibo_daily_limit (live): 1, the game's rule (the set off), or 0 (the set on). The game reads the field each
// time it reads a figure, so a change applies to the next figure.
#include <stdatomic.h>

#include "amiibo_daily.h"
#include "cfg.h"
#include "host.h"

static int daily_set(void) { return rt_patch_set("amiibo_daily"); }

// Whether the set may run at all is read once, after the start: NX2NAVI_AMIIBO_DAILY_PATCH=0, or a mod whose code
// patches the same site (mods.c mods_exefs_switch), leaves it off then, and switching it on later must not undo that.
static _Atomic int allowed = -1;
int amiibo_daily_available(void) {
    int a = atomic_load(&allowed);
    if (a < 0) {
        a = rt_patch_enabled(daily_set());
        atomic_store(&allowed, a);
    }
    return a;
}

void amiibo_daily_poll(void) {
    if (!amiibo_daily_available()) return;
    const int lifted = !cfg_bool("NX2NAVI_AMIIBO_DAILY_LIMIT"), set = daily_set();
    if (rt_patch_enabled(set) != lifted) rt_patch_switch(set, lifted);
    static int logged = -1;
    if (lifted != logged) {
        logged = lifted;
        rt_log("amiibo: daily limit %s", lifted ? "off: every tap gives items" : "on, as in the game");
    }
}
