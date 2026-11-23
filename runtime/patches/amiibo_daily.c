#include <stdatomic.h>

#include "amiibo_daily.h"
#include "cfg.h"
#include "host.h"

static int daily_set(void) { return rt_patch_set("amiibo_daily"); }

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
    const int lifted = !cfg_bool("NX2SARIA_AMIIBO_DAILY_LIMIT"), set = daily_set();
    if (rt_patch_enabled(set) != lifted) rt_patch_switch(set, lifted);
    static int logged = -1;
    if (lifted != logged) {
        logged = lifted;
        rt_log("amiibo: daily limit %s", lifted ? "off: every tap gives items" : "on, as in the game");
    }
}
