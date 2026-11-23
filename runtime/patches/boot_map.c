#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_map.h"
#include "boot_map_core.h"
#include "cfg.h"
#include "debug_tools.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"

enum { BM_OFF, BM_PENDING, BM_DONE };
static int32_t state;
static struct {
    char area[64], start[64];
    int slot, file_slot, mq;
    BmPlace place;
    uint64_t t0_ns;
} bm;
static PlatMutex save_lock = PLAT_MUTEX_INIT;
static BmEdit save_edit[8];
static int save_edit_n;

static void fallback(const char *why) {
    romfs_read_patches_off();
    rt_log("boot map: %s: normal boot", why);
}

static int mq_mode(void) {
    uint64_t n = 0;
    uint8_t *d = fs_save_load("account.sav", &n);
    uint32_t v = 0;
    int on = d && bm_save_find(d, (size_t)n, BM_BOOL, bm_hash("IsLastSelectedMQMode"), &v) && v;
    free(d);
    return on;
}

static int latest_slot(int base) {
    int best = -1;
    uint64_t best_date = 0;
    for (int s = 0; s < 6; s++) {
        char rel[48];
        uint64_t n = 0;
        snprintf(rel, sizeof rel, "slot_%02d/caption.sav", base + s);
        uint8_t *d = fs_save_load(rel, &n);
        uint64_t date = d ? bm_caption_date(d, (size_t)n) : 0;
        free(d);
        snprintf(rel, sizeof rel, "slot_%02d/progress.sav", base + s);
        uint8_t *p = date ? fs_save_load(rel, &n) : NULL;
        if (p && date > best_date) best = s, best_date = date;
        free(p);
    }
    return best;
}

static int arm_graph(const char *rel, int slot, char *why, size_t cap) {
    uint64_t n = 0;
    uint8_t *d = romfs_load(rel, &n);
    BmEdit e[2];
    char no[160] = "";
    int count = !d ? 0 : slot < 0 ? bm_ainb_title_edit(d, (size_t)n, e, no, sizeof no) : 2 * bm_ainb_slot_edits(d, (size_t)n, slot, e, no, sizeof no);
    if (!d) snprintf(no, sizeof no, "it cannot be read");
    free(d);
    if (!count) {
        snprintf(why, cap, "%s is not the 1.1.1 file this setting knows (%s)", rel, no);
        return 0;
    }
    for (int k = 0; k < count; k++)
        if (!romfs_read_patch(rel, e[k].off, e[k].orig, e[k].repl, e[k].n, why, cap)) return 0;
    return 1;
}

void boot_map_init(void) {
    const char *v = cfg_get("NX2SARIA_BOOT_MAP");
    if (!v || !*v) {
        rt_log("boot map: off");
        return;
    }
    char why[320];
    if (!RT_V111) {
        snprintf(why, sizeof why, "boot_map = %s asks for a game version this setting does not know (it is for 1.1.1)", v);
        fallback(why);
        return;
    }
    const char *colon = strchr(v, ':');
    size_t alen = colon ? (size_t)(colon - v) : strlen(v);
    if (!alen || alen >= sizeof bm.area || (colon && strlen(colon + 1) >= sizeof bm.start)) {
        snprintf(why, sizeof why, "boot_map = %s is not <area>[:<start position>]", v);
        fallback(why);
        return;
    }
    char asked[64];
    memcpy(bm.area, v, alen), bm.area[alen] = 0;
    snprintf(asked, sizeof asked, "%s", colon ? colon + 1 : "");
    float rot[3];
    if (!dbg_start_pos(bm.area, asked, bm.start, bm.place.pos, rot, why, sizeof why)) {
        fallback(why);
        return;
    }
    bm.place.area = bm.area;
    bm.place.yaw = bm_yaw(rot);

    const char *sv = cfg_get("NX2SARIA_BOOT_MAP_SLOT");
    bm.mq = mq_mode();
    int latest = !sv || !*sv || !strcmp(sv, "latest");
    char *end = NULL;
    long slot = latest ? latest_slot(bm.mq ? 6 : 0) : strtol(sv, &end, 10);
    if (!latest && (!end || *end || slot < 0 || slot > 5)) {
        snprintf(why, sizeof why, "boot_map_slot = %s is not 0 to 5 or latest", sv);
        fallback(why);
        return;
    }
    if (slot < 0) {
        fallback("no slot has a save (no slot_NN/caption.sav with a date and its progress.sav)");
        return;
    }
    bm.slot = (int)slot, bm.file_slot = bm.slot + (bm.mq ? 6 : 0);
    char rel[48];
    uint64_t n = 0;
    snprintf(rel, sizeof rel, "slot_%02d/progress.sav", bm.file_slot);
    uint8_t *save = fs_save_load(rel, &n);
    if (!save) {
        snprintf(why, sizeof why, "slot %d has no save (%s)", bm.slot, rel);
        fallback(why);
        return;
    }

    const char *av = cfg_get("NX2SARIA_BOOT_MAP_AGE"), *tv = cfg_get("NX2SARIA_BOOT_MAP_TIME");
    int area_age = bm_area_age(bm.area), want = -1;
    if (av && *av && strcmp(av, "save")) {
        want = !strcmp(av, "child") ? 1 : !strcmp(av, "adult") ? 0 : -2;
        if (want == -2) {
            free(save);
            snprintf(why, sizeof why, "boot_map_age = %s is not child, adult or save", av);
            fallback(why);
            return;
        }
    }
    bm.place.child = area_age >= 0 ? area_age : want;
    bm.place.day_seconds = -1;
    if (tv && *tv && strcmp(tv, "save") && (bm.place.day_seconds = bm_day_seconds(tv)) < 0) {
        free(save);
        snprintf(why, sizeof why, "boot_map_time = %s is not HH:MM (24 hours) or save", tv);
        fallback(why);
        return;
    }
    BmEdit trial[8];
    int edits = bm_save_edits(save, (size_t)n, &bm.place, trial, why, sizeof why);
    uint32_t was_child = 1;
    bm_save_find(save, (size_t)n, BM_BOOL, bm_hash("Sequence_IsChildhood"), &was_child);
    free(save);
    if (!edits) {
        char no[160];
        snprintf(no, sizeof no, "%s", why);
        snprintf(why, sizeof why, "the save of slot %d lacks a value this setting changes (%s)", bm.slot, no);
        fallback(why);
        return;
    }
    if (!arm_graph("Sequence/NormalTitle.module.ainb", -1, why, sizeof why) ||
        !arm_graph("Sequence/OnLoadGameSaveData.module.ainb", bm.slot, why, sizeof why)) {
        fallback(why);
        return;
    }
    char age[64], clock[48];
    int child = bm.place.child >= 0 ? bm.place.child : (int)(was_child != 0);
    snprintf(age, sizeof age, "age %s (%s)", child ? "child" : "adult",
             area_age >= 0 ? (want >= 0 && want != area_age ? "the area's, not the one asked for" : "the area's")
                           : want >= 0 ? "asked for" : "the save's");
    if (bm.place.day_seconds >= 0) snprintf(clock, sizeof clock, "time %s", tv);
    else snprintf(clock, sizeof clock, "time from the save");
    rt_log("boot map: on: %s, start position %s%s at %.1f %.1f %.1f (yaw %.2f), slot %d (%s), %s, %s", bm.area,
           bm.start[0] ? bm.start : "(the area's default)", asked[0] ? "" : " (none asked for)", bm.place.pos[0],
           bm.place.pos[1], bm.place.pos[2], bm.place.yaw, bm.slot, latest ? "the latest" : "asked for", age, clock);
    if (bm.mq) rt_log("boot map: Master Quest mode: the game loads slot_%02d", bm.file_slot);
    bm.t0_ns = plat_now_ns();
    __atomic_store_n(&state, BM_PENDING, __ATOMIC_RELEASE);
}

int boot_map_save_open(const char *path, PlatFile *f) {
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != BM_PENDING || !path) return 0;
    char tail[40];
    snprintf(tail, sizeof tail, "/slot_%02d/progress.sav", bm.file_slot);
    size_t pl = strlen(path), tl = strlen(tail);
    if (pl < tl || strcmp(path + pl - tl, tail)) return 0;
    PlatFileId id;
    uint8_t *d = plat_file_id(f, &id) && id.size >= 0x28 && id.size < (64u << 20) ? malloc((size_t)id.size) : NULL;
    int ok = d && plat_file_pread(f, d, (uint32_t)id.size, 0) == (int64_t)id.size;
    char why[160] = "it cannot be read";
    BmEdit e[8];
    int count = ok ? bm_save_edits(d, (size_t)id.size, &bm.place, e, why, sizeof why) : 0;
    free(d);
    plat_mutex_lock(&save_lock);
    int first = count && !save_edit_n;
    if (first) memcpy(save_edit, e, sizeof e), save_edit_n = count;
    else if (count && !bm_edits_same(e, count, save_edit, save_edit_n)) {
        count = 0;
        snprintf(why, sizeof why, "its values are not where the first open of the save found them");
    }
    plat_mutex_unlock(&save_lock);
    static int32_t said_ok, said_no;
    if (first && !__atomic_fetch_add(&said_ok, 1, __ATOMIC_RELAXED))
        rt_log("boot map: %s is read with the boot map's place (%d values changed in what the game reads)", path, count);
    if (!count && !__atomic_fetch_add(&said_no, 1, __ATOMIC_RELAXED))
        rt_log("boot map: %s lacks a value this setting changes now (%s): the game loads it as it is", path, why);
    return count > 0;
}

void boot_map_save_read(uint64_t offset, void *buf, uint64_t n) {
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != BM_PENDING) return;
    plat_mutex_lock(&save_lock);
    bm_edits_apply(save_edit, save_edit_n, offset, buf, n);
    plat_mutex_unlock(&save_lock);
}

void boot_map_tick(void) {
    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != BM_PENDING) return;
    float p[3];
    uint64_t now = plat_now_ns();
    if (!dbg_player_pos(p)) {
        if (now - bm.t0_ns > 300000000000ull) {
            __atomic_store_n(&state, BM_DONE, __ATOMIC_RELEASE);
            romfs_read_patches_off();
            rt_log("boot map: no arrival 300 s after the start (the save or the area did not load; see the lines above): "
                   "the boot map's changes stop");
        }
        return;
    }
    __atomic_store_n(&state, BM_DONE, __ATOMIC_RELEASE);
    romfs_read_patches_off();
    char area[64] = "";
    if (!dbg_area(area)) snprintf(area, sizeof area, "an unknown area");
    float d = sqrtf((p[0] - bm.place.pos[0]) * (p[0] - bm.place.pos[0]) + (p[1] - bm.place.pos[1]) * (p[1] - bm.place.pos[1]) +
                    (p[2] - bm.place.pos[2]) * (p[2] - bm.place.pos[2]));
    rt_log("boot map: arrived in %s (asked %s), slot %d, Link at %.2f %.2f %.2f, %.1f m from the start position, %.1f s after "
           "the start%s", area, bm.area, bm.slot, p[0], p[1], p[2], d, (double)plat_process_uptime_ns() / 1e9,
           strcmp(area, bm.area) ? ": NOT the area asked for" : "");
}
