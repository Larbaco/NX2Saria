// runtime/wide_hud.c: the HUD's edge elements moved out to the sides of a picture wider than 16:9. The layout files here
// are made by the test from the format as wide_hud.c's header comment describes it, with the panes, values and
// animation curves the game's 1.1.1 PlayerStatus_00, MapMenu_00 and NumDisplay_00 hold, so the reader is checked
// against a writer of its own; the archives and zstd dictionaries are made here too. No game files. Given romfs.bin and
// the offsets and sizes of the layout archive and Pack/ZsDic.pack.zs in it, it also checks the game's own archive
// (game_archive), alone and after the action prompt's edit of the same archive. Built and run by
// tests/wide_hud_check.py; prints each failed check and exits 1 on any.
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zdict.h>
#include <zstd.h>

#include "action_prompt.h"
#include "sarc.h"
#include "wide_hud.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

// ---- what wide_hud.c and action_prompt.c reach in the runtime: every log line is kept
enum { LOG_LINES = 64 };
static char log_lines[LOG_LINES][1024];
static int nlog;
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_lines[nlog % LOG_LINES], sizeof log_lines[0], fmt, ap);
    va_end(ap);
    nlog++;
}
uint64_t plat_now_ns(void) { return (uint64_t)((double)clock() / CLOCKS_PER_SEC * 1e9); }
static const char *last_log(void) { return nlog ? log_lines[(nlog - 1) % LOG_LINES] : ""; }
// The kept line that starts with prefix; NULL when none does.
static const char *log_find(const char *prefix) {
    for (int i = nlog - 1; i >= 0 && i >= nlog - LOG_LINES; i--)
        if (!strncmp(log_lines[i % LOG_LINES], prefix, strlen(prefix))) return log_lines[i % LOG_LINES];
    return NULL;
}

// ---- a BFFH layout
enum { SIZE = 0x10000, PANE_REC = 0x44 };
static uint8_t file[SIZE];
static size_t top;   // the next free byte

static uint64_t alloc(size_t n) {
    top = (top + 7) & ~(size_t)7;
    uint64_t at = top;
    top += n;
    if (top > SIZE) {
        printf("the test's layout is too small\n");
        exit(2);
    }
    return at;
}
static void w16(uint64_t at, uint16_t v) { memcpy(file + at, &v, 2); }
static void w32(uint64_t at, uint32_t v) { memcpy(file + at, &v, 4); }
static void w64(uint64_t at, uint64_t v) { memcpy(file + at, &v, 8); }
static void wf(uint64_t at, float v) { memcpy(file + at, &v, 4); }
static float rf(const uint8_t *p, uint64_t at) {
    float v;
    memcpy(&v, p + at, 4);
    return v;
}
static uint64_t str(const char *s) {
    uint64_t at = alloc(strlen(s) + 1);
    memcpy(file + at, s, strlen(s) + 1);
    return at;
}

#define Z (-2.842171e-14f)   // the game stores some zeros so
typedef struct {
    const char *name;
    int parent;   // index in the same table, -1 for the root
    float x, y, sx, rot;
} PaneDef;
// A curve of one component (0 x, 4 y) of a pane's property (1 translate, 4 alpha): its keys' values.
typedef struct {
    const char *pane;
    int prop, component, nkeys;
    float keys[24];
} CurveDef;
// A constant of a component of a pane's property.
typedef struct {
    const char *pane;
    int prop, component;
    float value;
} ConstDef;
typedef struct {
    const char *name;
    const CurveDef *curves;
    int ncurves;
    const ConstDef *consts;
    int nconsts;
} AnimDef;
typedef struct {
    const char *member;
    const PaneDef *panes;
    int npanes;
    const AnimDef *anims;
    int nanims;
} LayoutDef;

// Where make_layout put things in the last file it made: each pane's record, and each curve's values block.
static uint64_t pane_at[256], values_at[16][32];

static int pane_index(const LayoutDef *L, const char *name) {
    for (int i = 0; i < L->npanes; i++)
        if (!strcmp(L->panes[i].name, name)) return i;
    printf("no pane %s in the test's %s\n", name, L->member);
    exit(2);
}

// The 6-byte target code of a pane's own property, as the game's files have it.
static uint64_t target_code(int index, int prop) {
    uint64_t v = 0x7f | 7u << 8 | (uint64_t)(index & 0xf) << 11 | (uint64_t)((index >> 4) & 7) << 16 | 4u << 19 |
                 1u << 22 | (uint64_t)prop << 24;
    uint64_t at = alloc(8);
    for (int i = 0; i < 6; i++) file[at + i] = (uint8_t)(v >> (8 * i));
    return at;
}

static size_t make_layout(const LayoutDef *L) {
    memset(file, 0, sizeof file);
    top = 0x300;
    memcpy(file, "BFFH", 4);
    uint64_t names = alloc(8 * (size_t)L->npanes), records = alloc(8 * (size_t)L->npanes);
    for (int i = 0; i < L->npanes; i++) {
        const PaneDef *d = &L->panes[i];
        uint64_t r = alloc(PANE_REC);
        for (int k = 0; k < PANE_REC; k += 4) w32(r + k, 0xa5a50000u + (uint32_t)(i << 8) + (uint32_t)k);
        wf(r + 0x10, d->sx), wf(r + 0x14, 1), wf(r + 0x18, d->rot), wf(r + 0x1c, d->x), wf(r + 0x20, d->y);
        w32(r + 0x28, (uint32_t)d->parent);
        w64(names + 8 * i, str(d->name)), w64(records + 8 * i, r);
        pane_at[i] = r;
    }
    w64(0x118, names), w64(0x120, records), w32(0x130, (uint32_t)L->npanes), w32(0x134, 8);
    uint64_t anames = alloc(8 * (size_t)(L->nanims + 1)), arecords = alloc(8 * (size_t)(L->nanims + 1));
    for (int a = 0; a < L->nanims; a++) {
        const AnimDef *A = &L->anims[a];
        uint64_t r = alloc(0x30), body = alloc(0x58);
        w64(r, body), w32(r + 0x28, 30);
        // one track per (pane, property) in the order the curves and then the constants name them
        int ntracks = A->ncurves + A->nconsts;
        uint64_t tracks = alloc(8 * (size_t)(ntracks + 1)), curves = alloc(8 * (size_t)(A->ncurves + 1)),
                 comps = alloc((size_t)A->ncurves + 1), consts = alloc(4 * (size_t)(A->nconsts + 1)),
                 ccomps = alloc((size_t)A->nconsts + 1);
        for (int c = 0; c < A->ncurves; c++) {
            const CurveDef *C = &A->curves[c];
            uint64_t e = alloc(16), cv = alloc(16), fr = alloc(24 + 4 * (size_t)C->nkeys),
                     va = alloc(24 + 8 * (size_t)C->nkeys);
            w64(e, target_code(pane_index(L, C->pane), C->prop));
            w16(e + 8, (uint16_t)c), w16(e + 10, 0xffff), file[e + 12] = 1, file[e + 13] = 0, w16(e + 14, 1);
            w64(cv, fr), w64(cv + 8, va);
            wf(fr, 0), wf(fr + 4, (float)(C->nkeys - 1)), w64(fr + 8, fr + 24), w32(fr + 16, (uint32_t)C->nkeys),
                w32(fr + 20, 4);
            for (int k = 0; k < C->nkeys; k++) wf(fr + 24 + 4 * k, (float)k);
            wf(va, C->keys[C->nkeys - 1] - C->keys[0]), wf(va + 4, 0), w64(va + 8, va + 24),
                w32(va + 16, 2 * (uint32_t)C->nkeys), w32(va + 20, 4);
            for (int k = 0; k < C->nkeys; k++) wf(va + 24 + 8 * k, C->keys[k]), wf(va + 28 + 8 * k, 0);
            values_at[a][c] = va;
            w64(tracks + 8 * c, e), w64(curves + 8 * c, cv), file[comps + c] = (uint8_t)C->component;
        }
        for (int c = 0; c < A->nconsts; c++) {
            const ConstDef *K = &A->consts[c];
            uint64_t e = alloc(16);
            w64(e, target_code(pane_index(L, K->pane), K->prop));
            w16(e + 8, 0xffff), w16(e + 10, (uint16_t)c), file[e + 12] = 0, file[e + 13] = 1, w16(e + 14, 1);
            wf(consts + 4 * c, K->value), file[ccomps + c] = (uint8_t)K->component;
            w64(tracks + 8 * (A->ncurves + c), e);
        }
        w64(body + 0x08, tracks), w32(body + 0x10, (uint32_t)ntracks);
        w64(body + 0x18, curves), w32(body + 0x20, (uint32_t)A->ncurves);
        w64(body + 0x28, comps), w32(body + 0x30, (uint32_t)A->ncurves);
        w64(body + 0x38, consts), w32(body + 0x40, (uint32_t)A->nconsts);
        w64(body + 0x48, ccomps), w32(body + 0x50, (uint32_t)A->nconsts);
        w64(anames + 8 * a, str(A->name)), w64(arecords + 8 * a, r);
    }
    w64(0x238, anames), w64(0x240, arecords), w32(0x250, (uint32_t)L->nanims), w32(0x254, 8);
    return top;
}

// ---- the game's three layouts, as far as the edit reads them
static const PaneDef status_panes[] = {
    {"RootPane", -1, 0, 0, 1, 0},          {"N_All_00", 0, 0, 2, 1, 0},
    {"N_ScaleL_00", 1, -1920, 1078, 1, 0}, {"Pa_HeartGauge_00", 2, 780, -96, 1, 0},
    {"N_ScaleL_01", 2, 1926, -1078, 1, 0}, {"N_HeartPos_00", 4, Z, 0, 1, 0},
    {"N_ScaleR_00", 1, 1920, 1078, 1, 0},  {"Pa_SelectItem_00", 6, -294, -256, 0.85f, 0},
    {"N_SubItemBaseFishing_00", 1, 1270, 884, 1, 0}, {"N_VRS_00", 0, 1626, 822, 1, 0},
};
static const CurveDef status_heart[] = {{"N_HeartPos_00", 1, 4, 2, {0, -120}}, {"N_ScaleL_00", 4, 0, 2, {0, 1}}};
static const ConstDef status_sub[] = {{"N_SubItemBaseFishing_00", 1, 4, 884}};
static const AnimDef status_anims[] = {{"HeartPos", status_heart, 2, NULL, 0}, {"IsSubItemVisible", NULL, 0, status_sub, 1}};

static const PaneDef map_panes[] = {
    {"RootPane", -1, 0, 0, 1, 0},
    {"N_InOut_00", 0, 0, 0, 1, 0},
    {"N_EventEndIn_00", 1, 0, 0, 1, 0},
    {"N_MenuIn_00", 2, 0, 0, 1, 0},
    {"N_InChallenge_00", 3, 0, 0, 1, 0},
    {"N_LocalMap_00", 4, 0, 0, 1, 0},
    {"N_ItemIcon_00", 5, 1666, 300, 1, 0},
    {"N_MiniMap_00", 4, Z, 0, 1, 0},
    {"N_Field_01", 7, 1490, -644, 1, 0},
    {"N_Dungeon_01", 7, 1536, -690, 1, 0},
    {"N_Common_00", 4, 0, 0, 1, 0},
    {"N_MapIcon_00", 10, 0, 0, 1, 0},
    {"N_PatternDungeonMini_01", 11, 0, 0, 1, 0},
    {"N_EventEndIn_01", 1, 0, 0, 1, 0},
    {"P_MiniMapFrame_00", 13, 1536, -690, 1, 0},
    {"N_MiniMapCap_00", 0, 1470, -544, 1, 0},
    {"N_VRS_00", 0, 1536, -690, 1, 0},
    {"Pa_KeySmall_00", 0, 0, 0, 1, 0},
};
static const CurveDef map_type[] = {{"N_MapIcon_00", 1, 0, 7, {0, 0, 0, 0, 1490, 1490, 0}},
                                    {"N_MapIcon_00", 1, 4, 7, {0, 0, 0, 0, -644, -644, 0}}};
#define K19 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536, 1536
static const CurveDef map_dungeon[] = {{"N_PatternDungeonMini_01", 1, 0, 21, {0, 0, K19}},
                                       {"N_PatternDungeonMini_01", 1, 4, 3, {0, 0, -690}}};
static const CurveDef map_menu_in[] = {{"N_LocalMap_00", 1, 0, 2, {100, 0}}};
static const AnimDef map_anims[] = {{"MenuIn", map_menu_in, 1, NULL, 0}, {"MapType", map_type, 2, NULL, 0},
                                    {"DungeonMiniMapPattern", map_dungeon, 2, NULL, 0}};

static const PaneDef num_panes[] = {{"RootPane", -1, 0, 0, 1, 0},
                                    {"N_NumDisplayFirst_00", 0, 0, 0, 1, 0},
                                    {"N_NumDisplaySecond_00", 0, 0, -188, 1, 0},
                                    {"Pa_NumDisplay_00", 1, 1580, 900, 1, 0}};
static const CurveDef num_in[] = {{"Pa_NumDisplay_00", 1, 0, 2, {380, 0}}};
static const AnimDef num_anims[] = {{"In", num_in, 1, NULL, 0}};

static const LayoutDef layouts[] = {
    {"PlayerStatus_00.buil", status_panes, sizeof status_panes / sizeof *status_panes, status_anims, 2},
    {"MapMenu_00.buil", map_panes, sizeof map_panes / sizeof *map_panes, map_anims, 3},
    {"NumDisplay_00.buil", num_panes, sizeof num_panes / sizeof *num_panes, num_anims, 1},
};
enum { NLAYOUTS = 3 };

// ---- a SARC: one other member, then members[i] (bytes[i], sizes[i]) in order
enum { MAX_MEMBERS = 32 };
static size_t member_at[MAX_MEMBERS];   // where each member starts in the last archive made (after the other one)

static size_t make_sarc(uint8_t *out, size_t cap, const char *const *names, const uint8_t *const *bytes,
                        const size_t *sizes, int count) {
    static const char other[] = "Other_00.buil";
    static const uint8_t other_data[64] = {1, 2, 3, 4, 5, 6, 7, 8};
    int all = 1 + count;
    size_t sfat = 0x14, sfnt = sfat + 0xc + 16 * (size_t)all, name_len[MAX_MEMBERS], names_len = 0;
    size_t other_len = (sizeof other + 3) & ~(size_t)3;
    for (int i = 0; i < count; i++) name_len[i] = (strlen(names[i]) + 1 + 3) & ~(size_t)3, names_len += name_len[i];
    size_t data = (sfnt + 8 + other_len + names_len + 0xff) & ~(size_t)0xff;
    size_t total = (data + sizeof other_data + 0xff) & ~(size_t)0xff;
    for (int i = 0; i < count; i++) member_at[i] = total, total = (total + sizes[i] + 0xff) & ~(size_t)0xff;
    if (total > cap) return 0;
    memset(out, 0, total);
    memcpy(out, "SARC", 4);
    uint16_t hl = 0x14, bom = 0xfeff, v = 0x100, fl = 0xc, cnt = (uint16_t)all, eight = 8;
    uint32_t t32 = (uint32_t)total, d32 = (uint32_t)data, key = 0x65;
    memcpy(out + 4, &hl, 2), memcpy(out + 6, &bom, 2);
    memcpy(out + 8, &t32, 4), memcpy(out + 12, &d32, 4), memcpy(out + 16, &v, 2);
    memcpy(out + sfat, "SFAT", 4), memcpy(out + sfat + 4, &fl, 2), memcpy(out + sfat + 6, &cnt, 2);
    memcpy(out + sfat + 8, &key, 4);
    memcpy(out + sfnt, "SFNT", 4), memcpy(out + sfnt + 4, &eight, 2);
    size_t nm = sfnt + 8;
    memcpy(out + nm, other, sizeof other);
    uint32_t node[4] = {1, 0x01000000u, 0, (uint32_t)sizeof other_data};
    memcpy(out + sfat + 0xc, node, sizeof node);
    memcpy(out + data, other_data, sizeof other_data);
    size_t s = nm + other_len;
    for (int i = 0; i < count; i++) {
        memcpy(out + s, names[i], strlen(names[i]) + 1);
        uint32_t e[4] = {2 + (uint32_t)i, 0x01000000u | (uint32_t)((s - nm) / 4), (uint32_t)(member_at[i] - data),
                         (uint32_t)(member_at[i] - data + sizes[i])};
        memcpy(out + sfat + 0xc + 16 * (size_t)(i + 1), e, sizeof e);
        memcpy(out + member_at[i], bytes[i], sizes[i]);
        s += name_len[i];
    }
    return total;
}

// Points member c of an archive make_sarc made (its SFAT entry c + 1) at s[at..at + size).
static void set_range(uint8_t *s, int c, size_t at, size_t size) {
    uint32_t data, lo, hi;
    memcpy(&data, s + 12, 4);
    lo = (uint32_t)(at - data), hi = (uint32_t)(at + size - data);
    memcpy(s + 0x20 + 16 * (size_t)(c + 1) + 8, &lo, 4);
    memcpy(s + 0x20 + 16 * (size_t)(c + 1) + 12, &hi, 4);
}

// Each layout's file as make_layout makes it, kept for the archives below.
static uint8_t *made[NLAYOUTS];
static size_t made_n[NLAYOUTS];
static uint64_t made_pane_at[NLAYOUTS][256], made_values_at[NLAYOUTS][16][32];

static void make_all(void) {
    for (int i = 0; i < NLAYOUTS; i++) {
        made_n[i] = make_layout(&layouts[i]);
        free(made[i]);
        made[i] = malloc(made_n[i]);
        memcpy(made[i], file, made_n[i]);
        memcpy(made_pane_at[i], pane_at, sizeof pane_at);
        memcpy(made_values_at[i], values_at, sizeof values_at);
    }
}

// The archive of the three layouts, `copies` copies of each (the game has four), into out; its size.
static size_t archive(uint8_t *out, size_t cap, int copies) {
    const char *names[MAX_MEMBERS];
    const uint8_t *bytes[MAX_MEMBERS];
    size_t sizes[MAX_MEMBERS];
    int k = 0;
    for (int i = 0; i < NLAYOUTS; i++)
        for (int c = 0; c < copies; c++) names[k] = layouts[i].member, bytes[k] = made[i], sizes[k++] = made_n[i];
    return make_sarc(out, cap, names, bytes, sizes, k);
}

static float pane_x(const uint8_t *member, int layout, const char *pane) {
    return rf(member, made_pane_at[layout][pane_index(&layouts[layout], pane)] + 0x1c);
}

// ---- the checks on files the test makes
static void check_out(void) {
    static const struct { unsigned w, h; float out; } cases[] = {
        {2560, 1440, 0}, {1920, 1080, 0}, {1920, 1200, 0}, {1280, 800, 0}, {3440, 1440, 660}, {2560, 1080, 640},
        {5120, 1440, 1920}, {3840, 1080, 1920}, {3840, 1600, 672}, {2580, 1440, 0},   // 2580: within 1% of 16:9
        {0, 0, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        float o = wide_hud_out(cases[i].w, cases[i].h);
        CHECK(fabsf(o - cases[i].out) < 0.01f, "wide_hud_out(%u, %u) = %g, not %g", cases[i].w, cases[i].h, (double)o,
              (double)cases[i].out);
    }
    WideHudCount c[8];
    int n = wide_hud_counts(c, 8);
    CHECK(n == 3 && !strcmp(c[0].layout, "PlayerStatus_00") && c[0].panes == 4 && c[0].keys == 0 &&
              !strcmp(c[1].layout, "MapMenu_00") && c[1].panes == 5 && c[1].keys == 21 &&
              !strcmp(c[2].layout, "NumDisplay_00") && c[2].panes == 2 && c[2].keys == 0,
          "wide_hud_counts: %d layouts", n);
}

static void check_layouts(void) {
    char why[240], report[600];
    float out = 660;
    // PlayerStatus_00: the four panes out to their sides, nothing else
    uint8_t *p = malloc(made_n[0]);
    memcpy(p, made[0], made_n[0]);
    int r = wide_hud_lay_out(p, made_n[0], "PlayerStatus_00.buil", out, report, sizeof report, why, sizeof why);
    CHECK(r == 4, "PlayerStatus_00: %d fields (%s)", r, why);
    CHECK(pane_x(p, 0, "N_ScaleL_00") == -2580 && pane_x(p, 0, "N_ScaleR_00") == 2580 &&
              pane_x(p, 0, "N_SubItemBaseFishing_00") == 1930 && pane_x(p, 0, "N_VRS_00") == 2286,
          "PlayerStatus_00: moved to %g %g %g %g", (double)pane_x(p, 0, "N_ScaleL_00"), (double)pane_x(p, 0, "N_ScaleR_00"),
          (double)pane_x(p, 0, "N_SubItemBaseFishing_00"), (double)pane_x(p, 0, "N_VRS_00"));
    CHECK(!strcmp(report, "N_ScaleL_00 x -1920 -> -2580, N_ScaleR_00 x 1920 -> 2580, N_SubItemBaseFishing_00 x 1270 -> "
                          "1930, N_VRS_00 x 1626 -> 2286"),
          "PlayerStatus_00 report: %s", report);
    size_t diff = 0;
    for (size_t i = 0; i < made_n[0]; i++) diff += p[i] != made[0][i];
    CHECK(diff <= 16, "PlayerStatus_00: %zu bytes changed", diff);
    // moved already: nothing more; by another distance: refused, unchanged
    uint8_t *q = malloc(made_n[0]);
    memcpy(q, p, made_n[0]);
    r = wide_hud_lay_out(q, made_n[0], "PlayerStatus_00.buil", out, report, sizeof report, why, sizeof why);
    CHECK(r == 0 && !memcmp(q, p, made_n[0]) && !report[0], "PlayerStatus_00 again: %d", r);
    r = wide_hud_lay_out(q, made_n[0], "PlayerStatus_00.buil", 1920, report, sizeof report, why, sizeof why);
    CHECK(r == -1 && !memcmp(q, p, made_n[0]) && strstr(why, "N_ScaleL_00 x is -2580"), "PlayerStatus_00 at 1920 over "
          "660: %d (%s)", r, why);
    free(q);
    free(p);

    // MapMenu_00: the minimap's panes and the keys at its place; the pause map's pane and keys stay
    p = malloc(made_n[1]);
    memcpy(p, made[1], made_n[1]);
    r = wide_hud_lay_out(p, made_n[1], "MapMenu_00.buil", out, report, sizeof report, why, sizeof why);
    CHECK(r == 5 + 2 + 19 + 1, "MapMenu_00: %d fields (%s)", r, why);
    CHECK(pane_x(p, 1, "N_MiniMap_00") == 660 && pane_x(p, 1, "N_EventEndIn_01") == 660 &&
              pane_x(p, 1, "N_MiniMapCap_00") == 2130 && pane_x(p, 1, "N_VRS_00") == 2196 &&
              pane_x(p, 1, "Pa_KeySmall_00") == 660,
          "MapMenu_00: the minimap's panes");
    CHECK(pane_x(p, 1, "N_ItemIcon_00") == 1666 && pane_x(p, 1, "N_Field_01") == 1490 &&
              pane_x(p, 1, "P_MiniMapFrame_00") == 1536 && pane_x(p, 1, "N_LocalMap_00") == 0,
          "MapMenu_00: panes that move with their parent or stay with the pause map changed");
    uint64_t mt = made_values_at[1][1][0], dg = made_values_at[1][2][0], dgy = made_values_at[1][2][1],
             mi = made_values_at[1][0][0];
    CHECK(rf(p, mt + 24 + 8 * 4) == 2150 && rf(p, mt + 24 + 8 * 5) == 2150 && rf(p, mt + 24) == 0 &&
              rf(p, mt + 24 + 8 * 6) == 0 && rf(p, mt) == 0,
          "MapMenu_00 MapType keys %g %g, span %g", (double)rf(p, mt + 24 + 8 * 4), (double)rf(p, mt + 24),
          (double)rf(p, mt));
    int at_place = 0;
    for (int k = 0; k < 21; k++) at_place += rf(p, dg + 24 + 8 * k) == 2196;
    CHECK(at_place == 19 && rf(p, dg + 24) == 0 && rf(p, dg) == 2196 && rf(p, dgy) == -690,
          "MapMenu_00 DungeonMiniMapPattern: %d keys moved, span %g", at_place, (double)rf(p, dg));
    CHECK(rf(p, mi + 24) == 100 && rf(p, mi) == -100, "MapMenu_00 MenuIn: the pause map's keys changed");
    CHECK(strstr(report, ", MapType N_MapIcon_00 x 2 keys 1490 -> 2150, DungeonMiniMapPattern N_PatternDungeonMini_01 "
                         "x 19 keys 1536 -> 2196") != NULL,
          "MapMenu_00 report: %s", report);
    free(p);

    // NumDisplay_00: both rows to the right; the part's own slide keys stay
    p = malloc(made_n[2]);
    memcpy(p, made[2], made_n[2]);
    r = wide_hud_lay_out(p, made_n[2], "NumDisplay_00.buil", out, report, sizeof report, why, sizeof why);
    CHECK(r == 2 && pane_x(p, 2, "N_NumDisplayFirst_00") == 660 && pane_x(p, 2, "N_NumDisplaySecond_00") == 660 &&
              pane_x(p, 2, "Pa_NumDisplay_00") == 1580 && rf(p, made_values_at[2][0][0] + 24) == 380,
          "NumDisplay_00: %d (%s)", r, why);
    free(p);

    // a member edges mode does not edit, and no distance: nothing
    p = malloc(made_n[0]);
    memcpy(p, made[0], made_n[0]);
    CHECK(wide_hud_lay_out(p, made_n[0], "DoCommand_00.buil", out, report, sizeof report, why, sizeof why) == 0 &&
              wide_hud_lay_out(p, made_n[0], "PlayerStatus_00.buil", 0, report, sizeof report, why, sizeof why) == 0 &&
              !memcmp(p, made[0], made_n[0]),
          "an unedited member or out 0 changed something");
    // not a BFFH file
    p[0] = 'X';
    CHECK(wide_hud_lay_out(p, made_n[0], "PlayerStatus_00.buil", out, report, sizeof report, why, sizeof why) == -1,
          "a file without BFFH");
    free(p);
}

// Each fault on a copy of the layout's file: refused, the file as it was.
static void refused(int layout, const char *what, const char *expect, void (*fault)(uint8_t *p)) {
    char why[240] = "", report[600];
    uint8_t *p = malloc(made_n[layout]), *keep = malloc(made_n[layout]);
    memcpy(p, made[layout], made_n[layout]);
    fault(p);
    memcpy(keep, p, made_n[layout]);
    int r = wide_hud_lay_out(p, made_n[layout], layouts[layout].member, 660, report, sizeof report, why, sizeof why);
    CHECK(r == -1 && !memcmp(p, keep, made_n[layout]) && strstr(why, expect), "%s: %d, why \"%s\" (expected \"%s\")",
          what, r, why, expect);
    free(p);
    free(keep);
}
static void set_pane_f(uint8_t *p, int layout, const char *pane, int off, float v) {
    memcpy(p + made_pane_at[layout][pane_index(&layouts[layout], pane)] + off, &v, 4);
}
static void f_x(uint8_t *p) { set_pane_f(p, 0, "N_ScaleL_00", 0x1c, -1900); }
static void f_scale(uint8_t *p) { set_pane_f(p, 0, "N_All_00", 0x10, 0.5f); }
static void f_turn(uint8_t *p) { set_pane_f(p, 1, "N_InChallenge_00", 0x18, 3.14159f); }
static void f_keys(uint8_t *p) { float v = 1490; memcpy(p + made_values_at[1][1][0] + 24 + 8 * 3, &v, 4); }
static void f_span(uint8_t *p) { float v = 7; memcpy(p + made_values_at[1][2][0], &v, 4); }
static void f_parent(uint8_t *p) {
    uint32_t v = 99;
    memcpy(p + made_pane_at[2][pane_index(&layouts[2], "N_NumDisplaySecond_00")] + 0x28, &v, 4);
}
static void f_name(uint8_t *p) {   // the pane's name no longer the one the edit looks for
    uint64_t names, s;
    memcpy(&names, p + 0x118, 8);
    memcpy(&s, p + names + 8 * (uint64_t)pane_index(&layouts[2], "N_NumDisplayFirst_00"), 8);
    p[s] = 'M';
}

// An animation of PlayerStatus_00 that keys N_ScaleR_00's x: made as a fourth curve of HeartPos.
static const CurveDef keyed_heart[] = {{"N_HeartPos_00", 1, 4, 2, {0, -120}}, {"N_ScaleL_00", 4, 0, 2, {0, 1}},
                                       {"N_ScaleR_00", 1, 0, 2, {1920, 2000}}};
static const AnimDef keyed_anims[] = {{"HeartPos", keyed_heart, 3, NULL, 0}};
static const ConstDef keyed_const[] = {{"N_VRS_00", 1, 0, 1626}};
static const AnimDef keyed_const_anims[] = {{"IsSubItemVisible", NULL, 0, keyed_const, 1}};

static void f_common_scale(uint8_t *p) { set_pane_f(p, 1, "N_Common_00", 0x10, 0.5f); }
// Points pane `a`'s record offset in the pane table at `at` (another record, or a place inside one).
static void set_record(uint8_t *p, int layout, const char *a, uint64_t at) {
    uint64_t records;
    memcpy(&records, p + 0x120, 8);
    memcpy(p + records + 8 * (uint64_t)pane_index(&layouts[layout], a), &at, 8);
}
// N_EventEndIn_01 and N_MiniMap_00 on one record: two moves of one value.
static void f_shared(uint8_t *p) {
    set_record(p, 1, "N_EventEndIn_01", made_pane_at[1][pane_index(&layouts[1], "N_MiniMap_00")]);
}

// Layouts whose animations differ from the game's in one way each (MapMenu_00 with another set of animations).
static const CurveDef field_mini[] = {{"N_MapIcon_00", 1, 0, 2, {0, 1490}}};
static const AnimDef also_at_place[] = {{"MenuIn", map_menu_in, 1, NULL, 0}, {"MapType", map_type, 2, NULL, 0},
                                        {"DungeonMiniMapPattern", map_dungeon, 2, NULL, 0},
                                        {"FieldMiniMapPattern", field_mini, 1, NULL, 0}};
static const CurveDef map_type_twice[] = {{"N_MapIcon_00", 1, 0, 7, {0, 0, 0, 0, 1490, 1490, 0}},
                                          {"N_MapIcon_00", 1, 0, 7, {0, 0, 0, 0, 1490, 1490, 0}}};
static const AnimDef two_curves[] = {{"MapType", map_type_twice, 2, NULL, 0},
                                     {"DungeonMiniMapPattern", map_dungeon, 2, NULL, 0}};
static const CurveDef map_type_six[] = {{"N_MapIcon_00", 1, 0, 6, {0, 0, 0, 1490, 1490, 0}}};
static const AnimDef six_keys[] = {{"MapType", map_type_six, 1, NULL, 0},
                                   {"DungeonMiniMapPattern", map_dungeon, 2, NULL, 0}};
static CurveDef many_curves[17];
static const AnimDef many_tracks[] = {{"MapType", many_curves, 17, NULL, 0}};

static void refused_layout(const LayoutDef *L, const char *what, const char *expect) {
    char why[240] = "";
    size_t n = make_layout(L);
    uint8_t *keep = malloc(n);
    memcpy(keep, file, n);
    int r = wide_hud_lay_out(file, n, L->member, 660, NULL, 0, why, sizeof why);
    CHECK(r == -1 && !memcmp(file, keep, n) && strstr(why, expect), "%s: %d, why \"%s\" (expected \"%s\")", what, r, why,
          expect);
    free(keep);
}

static void check_refusals(void) {
    refused(0, "a pane's x not the game's", "N_ScaleL_00 x is -1900, not -1920", f_x);
    refused(0, "a scaled parent", "inside a pane scaled 0.5", f_scale);
    refused(1, "a turned parent", "turned 3.14159", f_turn);
    refused(1, "a third key at the minimap's place", "MapType N_MapIcon_00 x has 3 keys at 1490", f_keys);
    refused(1, "a curve's span not its last value less its first", "span 7", f_span);
    refused(2, "a broken chain of parents", "damaged chain of parents", f_parent);
    refused(2, "a pane renamed", "has no pane N_NumDisplayFirst_00", f_name);
    refused(1, "a scaled parent of a keyed pane", "N_MapIcon_00 is inside a pane scaled 0.5", f_common_scale);
    refused(1, "two moved panes on one record", "two moved values in one place", f_shared);
    // a moved value that lies on what leads to another field: N_MiniMapCap_00's record laid over N_MiniMap_00's so
    // that its parent index is N_MiniMap_00's x (0, the root) and its x N_MiniMap_00's scale; moving N_MiniMap_00 makes
    // that parent index a float's bits, which the reading after the write finds, and every byte goes back
    {
        char why[240] = "";
        uint8_t *p = malloc(made_n[1]), *keep = malloc(made_n[1]);
        memcpy(p, made[1], made_n[1]);
        uint64_t mini = made_pane_at[1][pane_index(&layouts[1], "N_MiniMap_00")];
        set_record(p, 1, "N_MiniMapCap_00", mini - 0xc);
        float x = 1470, zero = 0;
        memcpy(p + mini + 0x10, &x, 4);
        memcpy(p + mini + 0x1c, &zero, 4);   // a zero whose bits are 0: the root's index
        memcpy(keep, p, made_n[1]);
        int r = wide_hud_lay_out(p, made_n[1], "MapMenu_00.buil", 660, NULL, 0, why, sizeof why);
        CHECK(r == -1 && !memcmp(p, keep, made_n[1]) && strstr(why, "reads otherwise after the edit"),
              "a field over another's parent index: %d (%s)", r, why);
        free(p);
        free(keep);
    }
    LayoutDef M = layouts[1];
    M.anims = also_at_place, M.nanims = 4;
    refused_layout(&M, "another animation at the minimap's place", "N_MapIcon_00 x is also at 1490 in FieldMiniMapPattern");
    M.anims = two_curves, M.nanims = 2;
    refused_layout(&M, "two curves of one pane's x in an animation", "MapType N_MapIcon_00 x is not one curve of 7 keys");
    M.anims = six_keys, M.nanims = 2;
    refused_layout(&M, "a curve of another length", "MapType N_MapIcon_00 x is not one curve of 7 keys");
    for (int i = 0; i < 17; i++) many_curves[i] = (CurveDef){"N_MapIcon_00", 1, 0, 1, {0}};
    M.anims = many_tracks, M.nanims = 1;
    refused_layout(&M, "more tracks of a pane's x than the edit reads", "animations of N_MapIcon_00 cannot be read");
    // more panes than a track's index can name
    {
        static PaneDef big[130];
        for (int i = 0; i < 130; i++) {
            static char names[130][16];
            snprintf(names[i], sizeof names[i], "N_Pad_%03d", i);
            big[i] = (PaneDef){names[i], i ? 0 : -1, 0, 0, 1, 0};
        }
        for (int k = 0; k < layouts[2].npanes; k++) big[k] = layouts[2].panes[k];   // NumDisplay_00's own first
        LayoutDef B = {"NumDisplay_00.buil", big, 130, NULL, 0};
        refused_layout(&B, "a layout of 130 panes", "has 130 panes, more than the 128");
    }
    // a pane's x keyed by an animation, by a curve and by a constant
    LayoutDef L = layouts[0];
    char why[240] = "";
    L.anims = keyed_anims, L.nanims = 1;
    size_t n = make_layout(&L);
    int r = wide_hud_lay_out(file, n, L.member, 660, NULL, 0, why, sizeof why);
    CHECK(r == -1 && strstr(why, "N_ScaleR_00 has its x keyed by HeartPos"), "keyed by a curve: %d (%s)", r, why);
    L.anims = keyed_const_anims;
    n = make_layout(&L);
    r = wide_hud_lay_out(file, n, L.member, 660, NULL, 0, why, sizeof why);
    CHECK(r == -1 && strstr(why, "N_VRS_00 has its x keyed by IsSubItemVisible"), "keyed by a constant: %d (%s)", r, why);
    // every prefix of each layout: refused, never a read past it (the sanitizers watch)
    for (int i = 0; i < NLAYOUTS; i++)
        for (size_t cut = 0; cut < made_n[i]; cut += 3) {
            uint8_t *p = malloc(cut ? cut : 1);
            memcpy(p, made[i], cut);
            r = wide_hud_lay_out(p, cut, layouts[i].member, 660, NULL, 0, why, sizeof why);
            CHECK(r == -1, "%s cut to %zu bytes: %d", layouts[i].member, cut, r);
            free(p);
            if (r != -1) break;
        }
    make_all();   // make_layout above wrote over the shared buffer; the kept files stay the reference
}

static void check_archive(void) {
    enum { CAP = 1 << 20 };
    uint8_t *a = malloc(CAP), *b = malloc(CAP);
    char why[240] = "";
    size_t n = archive(a, CAP, 2);
    memcpy(b, a, n);
    nlog = 0;
    int r = wide_hud_archive(b, n, 660, why, sizeof why);
    CHECK(r == 2 * (4 + 27 + 2), "archive: %d fields (%s)", r, why);
    for (int c = 0; c < 2; c++) {
        CHECK(pane_x(b + member_at[c], 0, "N_ScaleL_00") == -2580, "archive: PlayerStatus_00 copy %d", c);
        CHECK(pane_x(b + member_at[2 + c], 1, "N_MiniMap_00") == 660, "archive: MapMenu_00 copy %d", c);
        CHECK(pane_x(b + member_at[4 + c], 2, "N_NumDisplaySecond_00") == 660, "archive: NumDisplay_00 copy %d", c);
    }
    const char *line = log_find("wide hud: PlayerStatus_00 (2 copies, 8 fields): N_ScaleL_00 x -1920 -> -2580, ");
    CHECK(line != NULL, "archive: the PlayerStatus_00 line (last: %s)", last_log());
    CHECK(log_find("wide hud: MapMenu_00 (2 copies, 54 fields): N_MiniMap_00 x 0 -> 660, N_EventEndIn_01 x 0 -> 660") &&
              log_find("wide hud: NumDisplay_00 (2 copies, 4 fields): N_NumDisplayFirst_00 x 0 -> 660"),
          "archive: the MapMenu_00 and NumDisplay_00 lines");
    CHECK(nlog == 3, "archive: %d lines, not one a layout", nlog);
    for (int i = 0; i < nlog; i++) CHECK(strlen(log_lines[i]) < 600, "archive: a line of %zu characters", strlen(log_lines[i]));
    // again: nothing more, no line
    nlog = 0;
    uint8_t *c2 = malloc(CAP);
    memcpy(c2, b, n);
    r = wide_hud_archive(c2, n, 660, why, sizeof why);
    CHECK(r == 0 && !memcmp(c2, b, n) && nlog == 0, "archive again: %d, %d lines", r, nlog);
    // one bad copy of one layout: nothing changes in any layout
    memcpy(c2, a, n);
    float bad = 1;
    memcpy(c2 + member_at[5] + made_pane_at[2][pane_index(&layouts[2], "N_NumDisplayFirst_00")] + 0x1c, &bad, 4);
    uint8_t *keep = malloc(CAP);
    memcpy(keep, c2, n);
    r = wide_hud_archive(c2, n, 660, why, sizeof why);
    CHECK(r == -1 && !memcmp(c2, keep, n) && strstr(why, "N_NumDisplayFirst_00 x is 1"), "archive with a bad copy: %d (%s)",
          r, why);
    size_t m;
    // an archive without one of the layouts
    const char *names[] = {layouts[0].member, layouts[1].member};
    const uint8_t *bytes[] = {made[0], made[1]};
    size_t sizes[] = {made_n[0], made_n[1]};
    m = make_sarc(c2, CAP, names, bytes, sizes, 2);
    memcpy(keep, c2, m);
    r = wide_hud_archive(c2, m, 660, why, sizeof why);
    CHECK(r == -1 && !memcmp(c2, keep, m) && strstr(why, "has no NumDisplay_00.buil"), "archive without NumDisplay_00: "
          "%d (%s)", r, why);
    // more than 16 copies of a layout
    {
        const char *nm[20];
        const uint8_t *by[20];
        size_t sz[20];
        for (int i = 0; i < 17; i++) nm[i] = layouts[0].member, by[i] = made[0], sz[i] = made_n[0];
        nm[17] = layouts[1].member, by[17] = made[1], sz[17] = made_n[1];
        nm[18] = layouts[2].member, by[18] = made[2], sz[18] = made_n[2];
        m = make_sarc(c2, CAP, nm, by, sz, 19);
        r = wide_hud_archive(c2, m, 660, why, sizeof why);
        CHECK(r == -1 && strstr(why, "more than 16 times"), "17 copies: %d (%s)", r, why);
    }
    // copies of one layout that share some of their bytes, and two layouts that share bytes
    n = archive(c2, CAP, 2);
    set_range(c2, 1, member_at[0] + 0x100, made_n[0]);
    memcpy(keep, c2, n);
    r = wide_hud_archive(c2, n, 660, why, sizeof why);
    CHECK(r == -1 && !memcmp(c2, keep, n) && strstr(why, "two copies of PlayerStatus_00.buil in the layout archive "
          "overlap"), "copies that overlap: %d (%s)", r, why);
    n = archive(c2, CAP, 2);
    set_range(c2, 4, member_at[2], made_n[2]);   // a copy of NumDisplay_00 over MapMenu_00's first
    r = wide_hud_archive(c2, n, 660, why, sizeof why);
    CHECK(r == -1 && strstr(why, "NumDisplay_00 and MapMenu_00 share bytes"), "layouts that share bytes: %d (%s)", r,
          why);
    // copies that are the same bytes are moved once
    n = archive(c2, CAP, 2);
    set_range(c2, 1, member_at[0], made_n[0]);
    r = wide_hud_archive(c2, n, 660, why, sizeof why);
    CHECK(r == 4 + 2 * 27 + 2 * 2 && pane_x(c2 + member_at[0], 0, "N_ScaleL_00") == -2580, "copies of the same bytes: %d "
          "(%s)", r, why);
    // one copy moved already, the other not: the line says what the other moved
    n = archive(c2, CAP, 2);
    CHECK(wide_hud_lay_out(c2 + member_at[0], made_n[0], layouts[0].member, 660, NULL, 0, why, sizeof why) == 4,
          "moving one copy");
    nlog = 0;
    r = wide_hud_archive(c2, n, 660, why, sizeof why);
    CHECK(r == 4 + 2 * 27 + 2 * 2 && log_find("wide hud: PlayerStatus_00 (2 copies, 4 fields): N_ScaleL_00 x -1920 -> "
                                              "-2580"),
          "one copy moved already: %d (%s)", r, nlog ? log_lines[0] : "no line");
    // nothing at out 0
    memcpy(c2, a, n);
    CHECK(wide_hud_archive(c2, n, 0, why, sizeof why) == 0 && !memcmp(c2, a, n), "archive at out 0");
    // damaged: a SARC cut short
    CHECK(wide_hud_archive(c2, 40, 660, why, sizeof why) == -1, "a damaged archive");
    free(keep);
    free(c2);
    free(b);
    free(a);
}

// A zstd dictionary with the id `id`, its content and samples taken from src[0..n); its size, 0 when it cannot be made.
static size_t make_dict(uint8_t *out, size_t cap, unsigned id, const uint8_t *src, size_t n) {
    enum { SAMPLES = 32 };
    size_t sizes[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) sizes[i] = n / SAMPLES;
    ZDICT_params_t params = {3, 0, id};
    size_t r = ZDICT_finalizeDictionary(out, cap, src, n < 2048 ? n : 2048, src, sizes, SAMPLES, params);
    return ZDICT_isError(r) ? 0 : r;
}

// Pack/ZsDic.pack.zs as the game's: a SARC of the dictionaries, compressed without one.
static size_t make_zsdic(uint8_t *out, size_t cap, const uint8_t *dict, size_t dn) {
    static uint8_t sarc[1 << 16];
    const char *names[] = {"zs.zsdic"};
    const uint8_t *bytes[] = {dict};
    size_t sizes[] = {dn};
    size_t sn = make_sarc(sarc, sizeof sarc, names, bytes, sizes, 1);
    size_t zn = ZSTD_compress(out, cap, sarc, sn, 3);
    return ZSTD_isError(zn) ? 0 : zn;
}

static void check_build(void) {
    enum { CAP = 1 << 20 };
    uint8_t *a = malloc(CAP), *dict = malloc(1 << 16), *zdic = malloc(1 << 17), *z = malloc(CAP);
    size_t n = archive(a, CAP, 2), dn = make_dict(dict, 1 << 16, 0x1234, a, n);
    CHECK(dn > 0, "the test's dictionary");
    size_t zdn = make_zsdic(zdic, 1 << 17, dict, dn);
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    size_t zn = ZSTD_compress_usingDict(cc, z, CAP, a, n, dict, dn, 3);
    ZSTD_freeCCtx(cc);
    CHECK(!ZSTD_isError(zn) && ZSTD_getDictID_fromFrame(z, zn) == 0x1234, "the test's archive frame");
    void *out = NULL, *again = NULL;
    size_t out_n = 0, again_n = 0;
    nlog = 0;
    int r = wide_hud_build(z, zn, zdic, zdn, 660, &out, &out_n);
    CHECK(r == 1 && out, "build: %d (%s)", r, last_log());
    CHECK(log_find("wide hud: the HUD's edge elements moved 660 units out, 66 fields"), "build: its line: %s",
          last_log());
    if (r == 1) {
        CHECK(ZSTD_getDictID_fromFrame(out, out_n) == 0x1234 && ZSTD_getFrameContentSize(out, out_n) == n,
              "build: another dictionary or size");
        uint8_t *raw = malloc(n), *expect = malloc(n);
        ZSTD_DCtx *dc = ZSTD_createDCtx();
        size_t got = ZSTD_decompress_usingDict(dc, raw, n, out, out_n, dict, dn);
        ZSTD_freeDCtx(dc);
        char why[240];
        memcpy(expect, a, n);
        wide_hud_archive(expect, n, 660, why, sizeof why);
        CHECK(got == n && !memcmp(raw, expect, n), "build: not the archive edited in memory");
        r = wide_hud_build(out, out_n, zdic, zdn, 660, &again, &again_n);
        CHECK(r == 0 && !again && strstr(last_log(), "moved 660 units out already"), "build again: %d (%s)", r,
              last_log());
        free(raw);
        free(expect);
    }
    CHECK(wide_hud_build(z, zn, zdic, zdn, 0, &again, &again_n) == 0 && !again, "build at out 0");
    CHECK(wide_hud_build(z, zn / 2, zdic, zdn, 660, &again, &again_n) == -1 && strstr(last_log(), "the layout archive is served as it is"),
          "build of a cut frame: %s", last_log());
    free(out);
    free(z);
    free(zdic);
    free(dict);
    free(a);
}

// ---- the game's own archive, read from romfs.bin
static uint8_t *read_part(const char *path, const char *off, const char *size, size_t *n) {
    long long at = strtoll(off, NULL, 10);
    *n = (size_t)strtoull(size, NULL, 10);
    FILE *f = fopen(path, "rb");
    uint8_t *p = f && *n ? malloc(*n) : NULL;
#ifdef _WIN32
    int ok = p && !_fseeki64(f, at, SEEK_SET) && fread(p, 1, *n, f) == *n;
#else
    int ok = p && !fseeko(f, (off_t)at, SEEK_SET) && fread(p, 1, *n, f) == *n;
#endif
    if (f) fclose(f);
    if (!ok) free(p);
    return ok ? p : NULL;
}

static uint8_t *unpack(const void *z, size_t zn, const uint8_t *dics, size_t dn, size_t *n) {
    unsigned id = ZSTD_getDictID_fromFrame(z, zn);
    unsigned long long rn = ZSTD_getFrameContentSize(z, zn);
    size_t at = 0, size = 0;
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || (id && !zsdic_find(dics, dn, id, &at, &size)))
        return NULL;
    uint8_t *raw = malloc((size_t)rn);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    size_t got = !raw || !d ? 0
                 : id       ? ZSTD_decompress_usingDict(d, raw, (size_t)rn, z, zn, dics + at, size)
                            : ZSTD_decompressDCtx(d, raw, (size_t)rn, z, zn);
    ZSTD_freeDCtx(d);
    if (ZSTD_isError(got) || got != rn) {
        free(raw);
        return NULL;
    }
    *n = (size_t)rn;
    return raw;
}

// The 4-byte words that differ between a and b (n bytes each), and how many of them lie outside the members named
// `layouts` of the archive a.
static size_t words_changed(const uint8_t *a, const uint8_t *b, size_t n, const char *const *names, int nnames,
                            size_t *outside) {
    size_t changed = 0;
    *outside = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        if (!memcmp(a + i, b + i, 4)) continue;
        changed++;
        int inside = 0;
        for (int k = 0; k < nnames && !inside; k++) {
            unsigned next = 0;
            size_t at, size;
            while (sarc_next(a, n, names[k], &next, &at, &size) == 1)
                if (i >= at && i < at + size) inside = 1;
        }
        *outside += !inside;
    }
    return changed;
}

// The float at offset `off` of each copy of member `name` in the archive p: equal to v in all of them.
static int each_copy(const uint8_t *p, size_t n, const char *name, size_t off, float v) {
    unsigned next = 0;
    size_t at, size;
    int copies = 0, ok = 1;
    while (sarc_next(p, n, name, &next, &at, &size) == 1) copies++, ok &= off + 4 <= size && rf(p, at + off) == v;
    return ok && copies == 4;
}

static void game_archive(char **argv) {
    size_t an, dn, pn = 0, rn = 0;
    uint8_t *a = read_part(argv[1], argv[2], argv[3], &an), *d = read_part(argv[1], argv[4], argv[5], &dn);
    uint8_t *dics = d ? zsdic_unpack(d, dn, 64u << 20, &pn) : NULL;
    uint8_t *raw = a && dics ? unpack(a, an, dics, pn, &rn) : NULL;
    CHECK(raw != NULL, "game: cannot read and unpack the layout archive from %s", argv[1]);
    if (!raw) {
        free(dics), free(a), free(d);
        return;
    }
    static const char *const edited[] = {"PlayerStatus_00.buil", "MapMenu_00.buil", "NumDisplay_00.buil"};
    // the shapes the settings give (21:9 at 1080 and 1440 lines, 32:9) and windows (3440x1440, 2880x1200, and a
    // 3440x1080 one wider than 3:1): moved, and only the expected words
    static const struct { unsigned w, h; } shapes[] = {{2560, 1080}, {3440, 1440}, {3416, 1440}, {5120, 1440},
                                                       {2880, 1200}, {3440, 1080}};
    for (size_t s = 0; s < sizeof shapes / sizeof *shapes; s++) {
        float out = wide_hud_out(shapes[s].w, shapes[s].h);
        void *z = NULL;
        size_t zn = 0, on = 0, outside = 0;
        nlog = 0;
        int made_z = wide_hud_build(a, an, d, dn, out, &z, &zn);
        CHECK(made_z == 1, "game %ux%u: build %d (%s)", shapes[s].w, shapes[s].h, made_z, last_log());
        uint8_t *got = made_z == 1 ? unpack(z, zn, dics, pn, &on) : NULL;
        CHECK(got && on == rn, "game %ux%u: the built archive does not unpack", shapes[s].w, shapes[s].h);
        if (got && on == rn) {
            size_t changed = words_changed(raw, got, rn, edited, 3, &outside);
            // four copies of 4 + 27 + 2 fields; MapType's span stays 0
            CHECK(changed == 4 * (4 + 5 + 2 + 19 + 1 + 2) && !outside, "game %ux%u: %zu words changed, %zu outside the "
                  "three layouts", shapes[s].w, shapes[s].h, changed, outside);
            CHECK(each_copy(got, rn, "PlayerStatus_00.buil", 0x740, -1920 - out) &&
                      each_copy(got, rn, "PlayerStatus_00.buil", 0x960, 1920 + out) &&
                      each_copy(got, rn, "MapMenu_00.buil", 0x1da8, 0 + out) &&
                      each_copy(got, rn, "MapMenu_00.buil", 0xb0d0, 1490 + out) &&
                      each_copy(got, rn, "MapMenu_00.buil", 0xfc68, 1536 + out) &&
                      each_copy(got, rn, "MapMenu_00.buil", 0xfc90, 1536 + out) &&
                      each_copy(got, rn, "NumDisplay_00.buil", 0x4c0, 0 + out),
                  "game %ux%u: a moved value is not where the dump of 1.1.1 has it", shapes[s].w, shapes[s].h);
        }
        if (s == 1 || s == 2)
            for (int i = 0; i < nlog && i < LOG_LINES; i++) printf("game %ux%u: %s\n", shapes[s].w, shapes[s].h, log_lines[i]);
        free(got);
        free(z);
    }
    // after the action prompt's edit of the same archive (romfs.c serves it first): both edits, nothing else
    void *ap = NULL, *z = NULL;
    size_t apn = 0, zn = 0, on = 0, outside = 0;
    int r = action_prompt_build(a, an, d, dn, 1, &ap, &apn);
    CHECK(r == 1, "game: the action prompt's build %d", r);
    r = r == 1 ? wide_hud_build(ap, apn, d, dn, 660, &z, &zn) : -1;
    CHECK(r == 1, "game: the HUD's build on the action prompt's archive %d (%s)", r, last_log());
    uint8_t *got = r == 1 ? unpack(z, zn, dics, pn, &on) : NULL;
    if (got && on == rn) {
        static const char *const both[] = {"PlayerStatus_00.buil", "MapMenu_00.buil", "NumDisplay_00.buil",
                                           "DoCommand_00.buil"};
        size_t changed = words_changed(raw, got, rn, both, 4, &outside);
        CHECK(!outside && changed > 4 * 33 && each_copy(got, rn, "PlayerStatus_00.buil", 0x740, -2580),
              "game: with the action prompt, %zu words changed, %zu outside", changed, outside);
    } else {
        CHECK(0, "game: the archive with both edits does not unpack");
    }
    free(got);
    free(z);
    free(ap);
    free(raw);
    free(dics);
    free(a);
    free(d);
}

int main(int argc, char **argv) {
    make_all();
    check_out();
    check_layouts();
    check_refusals();
    check_archive();
    check_build();
    if (argc == 6) game_archive(argv);
    for (int i = 0; i < NLAYOUTS; i++) free(made[i]);
    if (fails) {
        printf("wide hud: %d check(s) failed\n", fails);
        return 1;
    }
    printf("wide hud: all checks passed%s\n", argc == 6 ? " (and the game's archive)" : "");
    return 0;
}
