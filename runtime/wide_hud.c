// The HUD's corner elements at the edges of a picture wider than 16:9 (NX2SARIA_WIDE_HUD = edges; docs/wide-hud.md).
//
// With the widescreen sets on (widescreen.c), the game's UI keeps its 16:9 shape in the middle of the picture: the 2D
// canvas setup scales the full-screen projection of the 3840x2160 layout grid by 16:9 over the picture's ratio (set
// wide_hud of recomp/patches-v111.txt). The HUD's corners are then the corners of that middle 16:9. Edges mode moves the
// elements that sit at a side out by wide_hud_out units of the grid, so that after the squeeze they sit at the same side
// of the whole picture; everything else (the action prompt, messages, markers that follow the 3D world, menus) stays
// where the game puts it. The edit is made to the shared interface layouts the game reads once at start
// (romfs.c wide_hud_overlay serves the archive with it), so a change of mode or of the picture's shape shows after a
// restart, as both settings are read at start.
//
// What moves, in the game's 1.1.1 layouts (each pane named; its x is the game's value, checked before the edit):
// - PlayerStatus_00: N_ScaleL_00 (-1920: the hearts, the magic meter, the D-pad guide and the Stone of Agony hang on
//   it) to the left; N_ScaleR_00 (1920: the item slot and its page dots), N_SubItemBaseFishing_00 (the two sub-item
//   slots beside it) and N_VRS_00 (an empty pane over the item slot) to the right.
// - MapMenu_00: the minimap, to the right. The layout is also the pause menu's map, whose panes stay: only the
//   minimap's own panes move, N_MiniMap_00 (the field and dungeon maps), N_EventEndIn_01 (frame, floor label),
//   N_MiniMapCap_00 (the maps' captures), N_VRS_00 and Pa_KeySmall_00 (the small and boss key counter, a tab over the
//   minimap). The map icons are shared by both maps: the game's animations put their layer at the minimap's place for
//   the minimap (MapType: N_MapIcon_00 x 1490; DungeonMiniMapPattern: N_PatternDungeonMini_01 x 1536) and at 0 for the
//   pause map, so only the keys at the minimap's place move.
// - NumDisplay_00: the count that slides in from the right when rupees or an item's count change (two rows), to the
//   right, where it stays next to the item slot.
//
// The layout files are BFFH: little-endian records that point at each other by 64-bit offsets from the file's start.
// - The header has a group of slots per table: +0x00 the offset of the list of name offsets, +0x08 that of the list
//   of record offsets, +0x18 the count. Panes at 0x118 (in the order of the records: parents before children),
//   animations at 0x238.
// - A pane record: scale x at +0x10, rotation at +0x18, translate x at +0x1c (in its parent's space, the grid's centre
//   at 0), the parent's index at +0x28 (-1 for the root).
// - An animation record starts with the offset of its body: +0x08 the list of its tracks and +0x10 their count; +0x18
//   the list of its curves, +0x20 their count, +0x28 one byte a curve for its component (0 x, 4 y); +0x38 the constants'
//   floats, +0x40 their count, +0x48 one component byte a constant.
// - A track: the offset of a 6-byte target code, +0x08 its first curve (0xffff none), +0x0a its first constant, +0x0c
//   the curves' count, +0x0d the constants'. The code: bits 8-10 7 for a pane's own property, the pane's index in bits
//   11-14 and 16-18 (the high part), bit 22 and byte 3 the property (1 and 1: translate).
// - A curve: the offsets of its frames and of its values. The frames: their count at +0x10. The values: +0x00 the
//   last value less the first (its span), +0x08 the offset of the floats, +0x10 their count, (value, slope) a key.
//   The track's 7 bits of pane index name at most 128 panes; the game's largest layout here has 102.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "host.h"
#include "platform/platform.h"
#include "sarc.h"
#include "wide_hud.h"

#define MAX_RAW (64u << 20)
enum { PANE_TABLE = 0x118, ANIM_TABLE = 0x238 };
enum { PANE_SCALE_X = 0x10, PANE_ROTATE = 0x18, PANE_X = 0x1c, PANE_PARENT = 0x28, PANE_NEEDS = 0x2c };
enum { ANIM_TRACKS = 0x08, ANIM_CURVES = 0x18, ANIM_COMPONENTS = 0x28, ANIM_CONSTANTS = 0x38, ANIM_CONST_COMPONENTS = 0x48 };
enum { NO_INDEX = 0xffff, PANE_KIND = 7, COMPONENT_X = 0, MAX_PANES = 128 };

// ---- the plan
// A pane moved out to a side (-1 left, 1 right); x: its translate x in the game's file.
typedef struct {
    const char *pane;
    float x;
    int side;
} Move;
// The keys of one animation's curve of a pane's translate x at the minimap's place; at_place of keys in all.
typedef struct {
    const char *anim, *pane;
    float place;
    int side, at_place, keys;
} KeyMove;
typedef struct {
    const char *layout;
    const Move *moves;
    int nmoves;
    const KeyMove *keys;
    int nkeys;
} Plan;

static const Move player_status[] = {
    {"N_ScaleL_00", -1920, -1}, {"N_ScaleR_00", 1920, 1}, {"N_SubItemBaseFishing_00", 1270, 1}, {"N_VRS_00", 1626, 1}};
static const Move map_menu[] = {{"N_MiniMap_00", 0, 1}, {"N_EventEndIn_01", 0, 1}, {"N_MiniMapCap_00", 1470, 1},
                                {"N_VRS_00", 1536, 1},   {"Pa_KeySmall_00", 0, 1}};
static const KeyMove map_menu_keys[] = {{"MapType", "N_MapIcon_00", 1490, 1, 2, 7},
                                        {"DungeonMiniMapPattern", "N_PatternDungeonMini_01", 1536, 1, 19, 21}};
static const Move num_display[] = {{"N_NumDisplayFirst_00", 0, 1}, {"N_NumDisplaySecond_00", 0, 1}};
#define N(a) (int)(sizeof a / sizeof *a)
static const Plan plans[] = {
    {"PlayerStatus_00", player_status, N(player_status), NULL, 0},
    {"MapMenu_00", map_menu, N(map_menu), map_menu_keys, N(map_menu_keys)},
    {"NumDisplay_00", num_display, N(num_display), NULL, 0},
};
enum { NPLANS = sizeof plans / sizeof *plans };
// The most fields one layout's edit writes: its panes, its keys and each curve's span of values.
enum { MAX_EDITS = 64 };

float wide_hud_out(unsigned w, unsigned h) {
    if (!w || !h) return 0;
    double r = (double)w / h, wide = 16.0 / 9.0;
    return r < wide * 1.01 ? 0 : (float)(1080.0 * r - 1920.0);
}

int wide_hud_counts(WideHudCount *out, int max) {
    int k = 0;
    for (int i = 0; i < NPLANS && k < max; i++) {
        int keys = 0;
        for (int j = 0; j < plans[i].nkeys; j++) keys += plans[i].keys[j].at_place;
        out[k++] = (WideHudCount){plans[i].layout, plans[i].nmoves, keys};
    }
    return k;
}

// ---- reading the layout, every offset checked against its size
static int u32_at(const uint8_t *p, size_t n, uint64_t off, uint32_t *v) {
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

static int u64_at(const uint8_t *p, size_t n, uint64_t off, uint64_t *v) {
    if (off > n || n - off < 8) return 0;
    memcpy(v, p + off, 8);
    return 1;
}

static int f32_at(const uint8_t *p, size_t n, uint64_t off, float *v) {
    if (off > n || n - off < 4) return 0;
    memcpy(v, p + off, 4);
    return 1;
}

static const char *str_at(const uint8_t *p, size_t n, uint64_t off) {
    return off < n && memchr(p + off, 0, n - off) ? (const char *)p + off : NULL;
}

// The lists of a table whose header slots start at slot; 0 when they do not fit the file.
static int table(const uint8_t *p, size_t n, size_t slot, uint64_t *names, uint64_t *records, uint32_t *count) {
    return u64_at(p, n, slot, names) && u64_at(p, n, slot + 8, records) && u32_at(p, n, slot + 24, count) &&
           *names <= n && *records <= n && (n - *names) / 8 >= *count && (n - *records) / 8 >= *count;
}

// The index of the entry `name` of a table, and its record (at least size bytes); -1 when it is not there.
static int table_find(const uint8_t *p, size_t n, size_t slot, const char *name, size_t size, uint64_t *rec) {
    uint64_t names, records;
    uint32_t count;
    if (!table(p, n, slot, &names, &records, &count)) return -1;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t s, r;
        const char *t;
        if (!u64_at(p, n, names + 8 * (uint64_t)i, &s) || !(t = str_at(p, n, s)) || strcmp(t, name)) continue;
        if (!u64_at(p, n, records + 8 * (uint64_t)i, &r) || !r || r > n || n - r < size) return -1;
        *rec = r;
        return (int)i;
    }
    return -1;
}

// The game stores some zeros as -2.8e-14.
static int same(float a, float b) { return fabsf(a - b) < 1e-3f; }

// Each ancestor of pane i has scale x 1 and no rotation, so that a move in the pane's parent's space is the same move
// in the grid's: 1, or 0 with why.
static int plain_ancestors(const uint8_t *p, size_t n, const char *layout, const char *pane, uint64_t rec, char *why,
                           size_t why_n) {
    uint64_t names, records, r = rec;
    uint32_t count;
    if (!table(p, n, PANE_TABLE, &names, &records, &count)) return 0;
    for (uint32_t step = 0; step <= count; step++) {
        uint32_t parent;
        if (!u32_at(p, n, r + PANE_PARENT, &parent)) break;
        if (parent == 0xffffffffu) return 1;
        float sx, rot;
        if (parent >= count || !u64_at(p, n, records + 8 * (uint64_t)parent, &r) || r > n || n - r < PANE_NEEDS ||
            !f32_at(p, n, r + PANE_SCALE_X, &sx) || !f32_at(p, n, r + PANE_ROTATE, &rot))
            break;
        if (!same(sx, 1) || !same(rot, 0)) {
            snprintf(why, why_n, "%s %s is inside a pane scaled %g or turned %g", layout, pane, (double)sx, (double)rot);
            return 0;
        }
    }
    snprintf(why, why_n, "%s %s has a damaged chain of parents", layout, pane);
    return 0;
}

// The translate x tracks of pane `index` in the layout's animations: the curves' offsets (their animation's name in
// anim[]) and the constants' value offsets. Returns how many there are, -1 when the animations do not fit the file.
enum { MAX_TRACKS = 16 };
typedef struct {
    const char *anim;
    uint64_t curve;      // 0 for a constant
    uint64_t constant;   // the offset of a constant's float
} Track;

static int x_tracks(const uint8_t *p, size_t n, uint32_t index, Track *out, int max) {
    uint64_t names, records;
    uint32_t count;
    int found = 0;
    if (!table(p, n, ANIM_TABLE, &names, &records, &count)) return -1;
    for (uint32_t a = 0; a < count; a++) {
        uint64_t s, r, body, tracks, curves, comps, consts, ccomps;
        uint32_t ntracks, ncurves, ncomps, nconsts, nccomps;
        const char *name;
        if (!u64_at(p, n, names + 8 * (uint64_t)a, &s) || !(name = str_at(p, n, s)) ||
            !u64_at(p, n, records + 8 * (uint64_t)a, &r) || !u64_at(p, n, r, &body) ||
            !u64_at(p, n, body + ANIM_TRACKS, &tracks) || !u32_at(p, n, body + ANIM_TRACKS + 8, &ntracks) ||
            !u64_at(p, n, body + ANIM_CURVES, &curves) || !u32_at(p, n, body + ANIM_CURVES + 8, &ncurves) ||
            !u64_at(p, n, body + ANIM_COMPONENTS, &comps) || !u32_at(p, n, body + ANIM_COMPONENTS + 8, &ncomps) ||
            !u64_at(p, n, body + ANIM_CONSTANTS, &consts) || !u32_at(p, n, body + ANIM_CONSTANTS + 8, &nconsts) ||
            !u64_at(p, n, body + ANIM_CONST_COMPONENTS, &ccomps) ||
            !u32_at(p, n, body + ANIM_CONST_COMPONENTS + 8, &nccomps))
            return -1;
        for (uint32_t t = 0; t < ntracks; t++) {
            uint64_t e, code_at;
            uint8_t code[6], head[6];
            if (!u64_at(p, n, tracks + 8 * (uint64_t)t, &e) || !u64_at(p, n, e, &code_at) || code_at > n ||
                n - code_at < 6 || e > n || n - e < 14)
                return -1;
            memcpy(code, p + code_at, 6);
            memcpy(head, p + e + 8, 6);   // first curve, first constant (16 bits each), the two counts (8 bits)
            uint64_t v = 0;
            for (int i = 5; i >= 0; i--) v = v << 8 | code[i];
            uint32_t kind = (v >> 8) & 7, pane = ((v >> 11) & 0xf) | (((v >> 16) & 7) << 4);
            if (kind != PANE_KIND || pane != index || !((v >> 22) & 1) || code[3] != 1) continue;   // its translate
            unsigned first = head[0] | head[1] << 8, cfirst = head[2] | head[3] << 8, nc = head[4], nk = head[5];
            for (unsigned k = first; first != NO_INDEX && k < first + nc; k++) {
                uint64_t c;
                if (k >= ncurves || k >= ncomps || comps > n || n - comps <= k ||
                    !u64_at(p, n, curves + 8 * (uint64_t)k, &c))
                    return -1;
                if (p[comps + k] != COMPONENT_X) continue;
                if (found < max) out[found] = (Track){name, c, 0};
                found++;
            }
            for (unsigned k = cfirst; cfirst != NO_INDEX && k < cfirst + nk; k++) {
                if (k >= nconsts || k >= nccomps || ccomps > n || n - ccomps <= k || consts > n ||
                    (n - consts) / 4 <= k)
                    return -1;
                if (p[ccomps + k] != COMPONENT_X) continue;
                if (found < max) out[found] = (Track){name, 0, consts + 4 * (uint64_t)k};
                found++;
            }
        }
    }
    return found;
}

// 1 when the track (a curve's keys, or a constant) holds the value v; -1 when it cannot be read.
static int track_holds(const uint8_t *p, size_t n, const Track *t, float v) {
    float f;
    if (!t->curve) return f32_at(p, n, t->constant, &f) ? same(f, v) : -1;
    uint64_t values, data;
    uint32_t count;
    if (!u64_at(p, n, t->curve + 8, &values) || !u64_at(p, n, values + 8, &data) ||
        !u32_at(p, n, values + 16, &count) || data > n || (n - data) / 4 < count)
        return -1;
    for (uint32_t j = 0; j < count; j += 2) {   // (value, slope) pairs: the values
        memcpy(&f, p + data + 4 * (uint64_t)j, 4);
        if (same(f, v)) return 1;
    }
    return 0;
}

// ---- the edit: every float it writes, found and checked first
typedef struct {
    uint64_t at;
    float from, to;
} Edit;

static int add(Edit *e, int k, uint64_t at, float from, float to) {
    if (k < MAX_EDITS) e[k] = (Edit){at, from, to};
    return k + 1;
}

// Appends to report[0..report_n) as snprintf would, never past it.
static void say(char *report, size_t report_n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *report, size_t report_n, const char *fmt, ...) {
    if (!report || !report_n) return;
    size_t at = strlen(report);
    if (at + 1 >= report_n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(report + at, report_n - at, fmt, ap);
    va_end(ap);
}

// The edits of `plan` on p[0..n): from the game's values to the moved ones (moved = 0), or what reading the moved file
// back finds (moved = 1: from and to both the moved values). Every field read must hold its from value. Returns the
// count, -1 with why.
static int gather(const uint8_t *p, size_t n, const Plan *plan, float out, int moved, Edit *e, char *report,
                  size_t report_n, char *why, size_t why_n) {
    int k = 0;
    const char *L = plan->layout;
    uint64_t names, records;
    uint32_t npanes;
    if (!table(p, n, PANE_TABLE, &names, &records, &npanes) || npanes > MAX_PANES) {
        snprintf(why, why_n, "%s has %u panes, more than the %d its animations can name", L, npanes, MAX_PANES);
        return -1;
    }
    for (int i = 0; i < plan->nmoves; i++) {
        const Move *m = &plan->moves[i];
        uint64_t rec;
        int index = table_find(p, n, PANE_TABLE, m->pane, PANE_NEEDS, &rec);
        float x, to = m->x + m->side * out, from = moved ? to : m->x;
        Track tr[MAX_TRACKS];
        if (index < 0 || !f32_at(p, n, rec + PANE_X, &x)) {
            snprintf(why, why_n, "%s has no pane %s", L, m->pane);
            return -1;
        }
        if (!same(x, from)) {
            snprintf(why, why_n, "%s %s x is %g, not %g", L, m->pane, (double)x, (double)from);
            return -1;
        }
        int keyed = x_tracks(p, n, (uint32_t)index, tr, MAX_TRACKS);
        if (keyed < 0) {
            snprintf(why, why_n, "%s's animations are damaged", L);
            return -1;
        }
        if (keyed > 0) {
            snprintf(why, why_n, "%s %s has its x keyed by %s", L, m->pane, tr[0].anim);
            return -1;
        }
        if (!plain_ancestors(p, n, L, m->pane, rec, why, why_n)) return -1;
        k = add(e, k, rec + PANE_X, from, to);
        say(report, report_n, "%s%s x %g -> %g", i ? ", " : "", m->pane, (double)m->x, (double)to);
    }
    for (int i = 0; i < plan->nkeys; i++) {
        const KeyMove *km = &plan->keys[i];
        uint64_t rec;
        int index = table_find(p, n, PANE_TABLE, km->pane, PANE_NEEDS, &rec);
        Track tr[MAX_TRACKS];
        int nt = index < 0 ? -1 : x_tracks(p, n, (uint32_t)index, tr, MAX_TRACKS), found = 0;
        if (nt < 0 || nt > MAX_TRACKS) {
            snprintf(why, why_n, index < 0 ? "%s has no pane %s" : "%s's animations of %s cannot be read", L, km->pane);
            return -1;
        }
        if (!plain_ancestors(p, n, L, km->pane, rec, why, why_n)) return -1;
        float to = km->place + km->side * out, from = moved ? to : km->place;
        for (int t = 0; t < nt; t++) {
            if (!tr[t].curve || strcmp(tr[t].anim, km->anim)) {
                // another animation that puts the icons at the minimap's place would keep them at the old one
                int holds = track_holds(p, n, &tr[t], from);
                if (holds < 0) snprintf(why, why_n, "%s's animations of %s cannot be read", L, km->pane);
                if (holds > 0) snprintf(why, why_n, "%s %s x is also at %g in %s", L, km->pane, (double)from, tr[t].anim);
                if (holds) return -1;
                continue;
            }
            uint64_t values, data;
            uint32_t count, nframes;
            uint64_t frames;
            float span;
            if (found++ || !u64_at(p, n, tr[t].curve, &frames) || !u64_at(p, n, tr[t].curve + 8, &values) ||
                !u32_at(p, n, frames + 16, &nframes) || !f32_at(p, n, values, &span) ||
                !u64_at(p, n, values + 8, &data) || !u32_at(p, n, values + 16, &count) || data > n ||
                (n - data) / 4 < count || count != 2 * nframes || (int)nframes != km->keys) {
                snprintf(why, why_n, "%s %s %s x is not one curve of %d keys", L, km->anim, km->pane, km->keys);
                return -1;
            }
            float v0 = 0, v1 = 0, first = 0, last = 0;
            int at_place = 0;
            for (uint32_t j = 0; j < nframes; j++) {
                float v;
                memcpy(&v, p + data + 8 * (uint64_t)j, 4);
                float nv = same(v, from) ? to : v;
                if (same(v, from)) at_place++, k = add(e, k, data + 8 * (uint64_t)j, from, to);
                if (!j) v0 = v, first = nv;
                v1 = v, last = nv;
            }
            if (at_place != km->at_place || !same(span, v1 - v0)) {
                snprintf(why, why_n, "%s %s %s x has %d keys at %g of %d, span %g", L, km->anim, km->pane, at_place,
                         (double)from, km->keys, (double)span);
                return -1;
            }
            k = add(e, k, values, span, last - first);   // also where it stays, so that both readings match
        }
        if (!found) {
            snprintf(why, why_n, "%s %s does not key %s x", L, km->anim, km->pane);
            return -1;
        }
        say(report, report_n, ", %s %s x %d keys %g -> %g", km->anim, km->pane, km->at_place, (double)km->place,
            (double)to);
    }
    if (k > MAX_EDITS) {
        snprintf(why, why_n, "%s needs %d edits, more than %d", L, k, MAX_EDITS);
        return -1;
    }
    for (int i = 0; i < k; i++)
        for (int j = 0; j < i; j++)
            if (e[i].at < e[j].at + 4 && e[j].at < e[i].at + 4) {
                snprintf(why, why_n, "%s has two moved values in one place", L);
                return -1;
            }
    return k;
}

static const Plan *plan_of(const char *member) {
    for (int i = 0; i < NPLANS; i++) {
        size_t len = strlen(plans[i].layout);
        if (!strncmp(member, plans[i].layout, len) && !strcmp(member + len, ".buil")) return &plans[i];
    }
    return NULL;
}

int wide_hud_lay_out(uint8_t *p, size_t n, const char *member, float out, char *report, size_t report_n, char *why,
                     size_t why_n) {
    const Plan *plan = plan_of(member);
    if (report && report_n) report[0] = 0;
    if (!plan || !(out > 0)) return 0;
    if (n < 4 || memcmp(p, "BFFH", 4)) {
        snprintf(why, why_n, "%s is not a BFFH layout", member);
        return -1;
    }
    Edit e[MAX_EDITS], back[MAX_EDITS];
    int k = gather(p, n, plan, out, 0, e, report, report_n, why, why_n);
    if (k < 0) {
        char why2[200];
        // moved already (by an earlier start's file a mod serves, say): nothing to change
        if (gather(p, n, plan, out, 1, back, NULL, 0, why2, sizeof why2) >= 0) {
            if (report && report_n) report[0] = 0;
            return 0;
        }
        return -1;
    }
    uint8_t undo[MAX_EDITS][4];
    int changed = 0;
    for (int i = 0; i < k; i++) {
        memcpy(undo[i], p + e[i].at, 4);
        if (same(e[i].from, e[i].to)) continue;
        memcpy(p + e[i].at, &e[i].to, 4);
        changed++;
    }
    // Read the layout again as the game will: each field where it was, with its new value. A file whose fields lie
    // on the records that lead to them reads otherwise; it gets its bytes back, last written first.
    char why2[200];
    int kb = gather(p, n, plan, out, 1, back, NULL, 0, why2, sizeof why2);
    int ok = kb == k;
    for (int i = 0; ok && i < k; i++) ok = back[i].at == e[i].at;
    if (!ok) {
        for (int i = k - 1; i >= 0; i--) memcpy(p + e[i].at, undo[i], 4);
        snprintf(why, why_n, "%s reads otherwise after the edit (%s)", member, kb < 0 ? why2 : "another field moved");
        return -1;
    }
    return changed;
}

// Each layout's copies in the archive (the game's holds each layout four times, the same bytes at four offsets).
enum { MAX_COPIES = 16 };
typedef struct {
    size_t at[MAX_COPIES], size[MAX_COPIES];
    int copies;
} Copies;

static int copies_of(const uint8_t *sarc, size_t n, const char *member, Copies *c, char *why, size_t why_n) {
    unsigned next = 0;
    size_t a, s;
    int found;
    c->copies = 0;
    while ((found = sarc_next(sarc, n, member, &next, &a, &s)) == 1) {
        if (c->copies == MAX_COPIES) {
            snprintf(why, why_n, "the layout archive holds %s more than %d times", member, MAX_COPIES);
            return 0;
        }
        c->at[c->copies] = a, c->size[c->copies++] = s;
    }
    if (found < 0) {
        snprintf(why, why_n, "the layout archive is damaged");
        return 0;
    }
    if (!c->copies) {
        snprintf(why, why_n, "the layout archive has no %s", member);
        return 0;
    }
    for (int i = 0; i < c->copies; i++)
        for (int j = 0; j < i; j++)
            if (!(c->at[i] == c->at[j] && c->size[i] == c->size[j]) && c->at[i] < c->at[j] + c->size[j] &&
                c->at[j] < c->at[i] + c->size[i]) {
                snprintf(why, why_n, "two copies of %s in the layout archive overlap", member);
                return 0;
            }
    return 1;
}

int wide_hud_archive(uint8_t *sarc, size_t n, float out, char *why, size_t why_n) {
    if (!(out > 0)) return 0;
    Copies c[NPLANS];
    char member[64], report[NPLANS][600], trial_report[600];
    for (int i = 0; i < NPLANS; i++) {
        snprintf(member, sizeof member, "%s.buil", plans[i].layout);
        if (!copies_of(sarc, n, member, &c[i], why, why_n)) return -1;
    }
    // Layouts that share bytes with another layout cannot be edited alone.
    for (int i = 0; i < NPLANS; i++)
        for (int a = 0; a < c[i].copies; a++)
            for (int j = 0; j < i; j++)
                for (int b = 0; b < c[j].copies; b++)
                    if (c[i].at[a] < c[j].at[b] + c[j].size[b] && c[j].at[b] < c[i].at[a] + c[i].size[a]) {
                        snprintf(why, why_n, "%s and %s share bytes in the layout archive", plans[i].layout,
                                 plans[j].layout);
                        return -1;
                    }
    // Every copy of every layout is tried first, each on a copy of its own: either all of them change or none. The
    // line a layout gets says what a copy that changed moved.
    for (int i = 0; i < NPLANS; i++) {
        snprintf(member, sizeof member, "%s.buil", plans[i].layout);
        report[i][0] = 0;
        for (int j = 0; j < c[i].copies; j++) {
            uint8_t *trial = malloc(c[i].size[j] ? c[i].size[j] : 1);
            if (!trial) {
                snprintf(why, why_n, "out of memory");
                return -1;
            }
            memcpy(trial, sarc + c[i].at[j], c[i].size[j]);
            int r = wide_hud_lay_out(trial, c[i].size[j], member, out, trial_report, sizeof trial_report, why, why_n);
            free(trial);
            if (r < 0) return -1;
            if (r > 0) memcpy(report[i], trial_report, sizeof trial_report);
        }
    }
    int total = 0;
    for (int i = 0; i < NPLANS; i++) {
        int fields = 0;
        snprintf(member, sizeof member, "%s.buil", plans[i].layout);
        for (int j = 0; j < c[i].copies; j++) {
            // copies that share their bytes are laid out once: the second finds them moved and changes nothing
            int r = wide_hud_lay_out(sarc + c[i].at[j], c[i].size[j], member, out, NULL, 0, why, why_n);
            if (r < 0) return -1;   // not after a trial of the same bytes; kept so that a change here cannot hide one
            fields += r;
        }
        total += fields;
        if (fields)
            rt_log("wide hud: %s (%d cop%s, %d fields): %s%s", plans[i].layout, c[i].copies,
                   c[i].copies == 1 ? "y" : "ies", fields, report[i],
                   strlen(report[i]) + 1 >= sizeof report[i] ? " ..." : "");
    }
    return total;
}

int wide_hud_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, float out, void **out_z,
                   size_t *out_n) {
    uint64_t t0 = plat_now_ns();
    *out_z = NULL;
    if (!(out > 0)) return 0;
    int result = -1;
    uint8_t *dics = NULL, *raw = NULL;
    void *z = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_CDict *cd = NULL;
    char why[240] = "";
    // The frame names its dictionary by id (0: none); Pack/ZsDic.pack.zs holds them.
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    size_t dn = 0, dic_at = 0, dic_n = 0;
    if (id && (!(dics = zsdic_unpack(zsdic, zsdic_n, MAX_RAW, &dn)) || !zsdic_find(dics, dn, id, &dic_at, &dic_n))) {
        snprintf(why, sizeof why, "no dictionary %u in Pack/ZsDic.pack.zs", id);
        goto done;
    }
    if (id && (!(dd = ZSTD_createDDict(dics + dic_at, dic_n)) || !(cd = ZSTD_createCDict(dics + dic_at, dic_n, 3)))) {
        snprintf(why, sizeof why, "dictionary %u of Pack/ZsDic.pack.zs cannot be loaded", id);
        goto done;
    }
    unsigned long long rn = ZSTD_getFrameContentSize(in, n);
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || !rn || rn > MAX_RAW) {
        snprintf(why, sizeof why, "the layout archive is not a zstd frame with a size");
        goto done;
    }
    if (!(raw = malloc((size_t)rn))) {
        snprintf(why, sizeof why, "out of memory");
        goto done;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t got = !dctx ? 0 : dd ? ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, in, n, dd)
                                : ZSTD_decompressDCtx(dctx, raw, (size_t)rn, in, n);
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(got) || got != rn) {
        snprintf(why, sizeof why, "the layout archive does not decompress");
        goto done;
    }
    int changed = wide_hud_archive(raw, (size_t)rn, out, why, sizeof why);
    if (changed <= 0) {
        result = changed;
        goto done;
    }
    // Level 3, as the action prompt's edit of the same archive: about 15 ms for its 10 MB.
    size_t cap = ZSTD_compressBound((size_t)rn), zn = 0;
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (cctx && (z = malloc(cap)))
        zn = cd ? ZSTD_compress_usingCDict(cctx, z, cap, raw, (size_t)rn, cd)
                : ZSTD_compressCCtx(cctx, z, cap, raw, (size_t)rn, 3);
    ZSTD_freeCCtx(cctx);
    if (!z || !zn || ZSTD_isError(zn)) {
        snprintf(why, sizeof why, "the layout archive cannot be compressed again");
        goto done;
    }
    *out_z = z, *out_n = zn, z = NULL;
    result = 1;
    rt_log("wide hud: the HUD's edge elements moved %g units out, %d fields (%.0f ms)", (double)out, changed,
           (double)(plat_now_ns() - t0) / 1e6);
done:
    if (result == 0) rt_log("wide hud: the layout archive has the HUD's edge elements moved %g units out already", (double)out);
    if (result < 0) rt_log("wide hud: %s; the layout archive is served as it is (the game's own keeps the HUD in the "
                           "middle 16:9)", why);
    free(z);
    free(raw);
    free(dics);
    ZSTD_freeDDict(dd);
    ZSTD_freeCDict(cd);
    return result;
}
