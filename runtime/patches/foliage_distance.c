#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "foliage_distance.h"
#include "host.h"
#include "platform/platform.h"

static void list_floats(char *out, size_t cap, const float *v) {
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < FOLIAGE_FLOATS && n < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s%g", i ? ", " : "", (double)v[i]);
}

static int write_main(uint64_t a, const void *from, size_t n) {
    uint64_t page = a & ~0xFFFull, end;
    if (((a + n - 1) & ~0xFFFull) != page) return 0;
    int prot = plat_mem_state(page, page + 0x1000, &end);
    if (prot != PLAT_PROT_READ && prot != PLAT_PROT_RW) return 0;
    if (prot == PLAT_PROT_READ && !plat_mem_protect(page, 0x1000, PLAT_PROT_RW)) return 0;
    memcpy((void *)(uintptr_t)a, from, n);
    if (prot == PLAT_PROT_READ) plat_mem_protect(page, 0x1000, PLAT_PROT_READ);
    return 1;
}

void foliage_distance_init(void) {
    const char *v = cfg_value("NX2SARIA_FOLIAGE_DISTANCE");
    float factor;
    int fit = foliage_factor(v, &factor);
    char why[160] = "";
    if (fit < 0) snprintf(why, sizeof why, "; foliage_distance = %s is not a number: 1 is used", v);
    else if (fit > 0)
        snprintf(why, sizeof why, "; foliage_distance = %s is outside %g to %g: %g is used", v, (double)FOLIAGE_MIN,
                 (double)FOLIAGE_MAX, (double)factor);
    if (!RT_V111) {
        rt_log("foliage: distance factor %g not applied: the foliage's distances are known for game 1.1.1 only (this "
               "is %d.%d.%d)%s",
               (double)factor, rt_game_version / 100, rt_game_version / 10 % 10, rt_game_version % 10, why);
        return;
    }
    const RtLoaded *m = ldr_count() ? ldr_module(0) : NULL;
    if (!m || m->size < FOLIAGE_LOD_OFF + sizeof foliage_game) {
        rt_log("foliage: distance factor %g not applied: main is not loaded%s", (double)factor, why);
        return;
    }
    uint64_t at = m->base + FOLIAGE_LOD_OFF;
    float game[FOLIAGE_FLOATS], now[FOLIAGE_FLOATS];
    char was[160], to[160];
    memcpy(game, (const void *)(uintptr_t)at, sizeof game);
    list_floats(was, sizeof was, game);
    if (memcmp(game, foliage_game, sizeof game)) {
        rt_log("foliage: distance factor %g not applied: main+%#x holds %s, not the game's 1.1.1 distances (an ExeFS "
               "mod changed them?)%s",
               (double)factor, FOLIAGE_LOD_OFF, was, why);
        return;
    }
    int capped = foliage_scale(factor, game, now);
    if (factor == 1.0f) {
        rt_log("foliage: distance factor 1 (the game's: %s m; now: the same)%s", was, why);
        return;
    }
    if (!write_main(at, now, sizeof now)) {
        rt_log("foliage: distance factor %g not applied: main+%#x could not be written%s", (double)factor,
               FOLIAGE_LOD_OFF, why);
        return;
    }
    list_floats(to, sizeof to, now);
    rt_log("foliage: distance factor %g (the game's: %s m; now: %s m%s)%s", (double)factor, was, to,
           capped ? "; ornaments at most 75 m and low trees' leaves at most 70 m" : "", why);
}
