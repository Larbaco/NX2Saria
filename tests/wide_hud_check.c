// runtime/wide_hud.c: the HUD's edge elements moved out to the sides of a picture wider than 16:9, and the menus laid
// out over the whole picture (check_menus). The layout files here are made by the test from the format as wide_hud.c's
// header comment describes it, with the panes, values and animation curves the game's 1.1.1 PlayerStatus_00,
// MapMenu_00, NumDisplay_00, LoadingFade_00, PlusMenu_00, MenuOverlay_00, PaShortCutSlideList_00 and PaItemMenu_00
// hold, so the reader is checked against a writer of its own; the archives and zstd dictionaries are made here too.
// No game files. Given romfs.bin and
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
#include "restbl.h"
#include "sarc.h"
#include "wide_hud.h"

// The tables of panes and curves below leave their trailing fields out where they are 0 (a pane's width, a curve's
// slopes), as most of the game's panes and curves have them.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// A shift of the HUD alone (out units), of the loading icon alone (squeeze q), or both.
#define HUD(o) (&(WideHudShift){(o), 1.0f, 0, 0})
#define MENUS(q) (&(WideHudShift){0, (q), 0, 0})
#define BOTH(o, q) (&(WideHudShift){(o), (q), 0, 0})

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
    float w;   // the width, written when not 0 (the record keeps its fill pattern there otherwise)
} PaneDef;
// A curve of one component (0 x, 4 y) of a pane's property (0 scale, 1 translate, 4 alpha): its keys' values and
// slopes (0 when not given).
typedef struct {
    const char *pane;
    int prop, component, nkeys;
    float keys[24], slopes[24];
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
static uint64_t pane_at[256], values_at[16][72];

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
    top = 0x310;   // past the header, whose last slot (the per-material array, plus_records) ends at 0x310
    memcpy(file, "BFFH", 4);
    uint64_t names = alloc(8 * (size_t)L->npanes), records = alloc(8 * (size_t)L->npanes);
    for (int i = 0; i < L->npanes; i++) {
        const PaneDef *d = &L->panes[i];
        uint64_t r = alloc(PANE_REC);
        for (int k = 0; k < PANE_REC; k += 4) w32(r + k, 0xa5a50000u + (uint32_t)(i << 8) + (uint32_t)k);
        wf(r + 0x10, d->sx), wf(r + 0x14, 1), wf(r + 0x18, d->rot), wf(r + 0x1c, d->x), wf(r + 0x20, d->y);
        w32(r + 0x28, (uint32_t)d->parent);
        if (d->w != 0) wf(r + 0x2c, d->w);
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
            for (int k = 0; k < C->nkeys; k++) wf(va + 24 + 8 * k, C->keys[k]), wf(va + 28 + 8 * k, C->slopes[k]);
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

// The records PlusMenu_00 holds past its panes and animations, as the game's 1.1.1 file has them where the backdrop's
// split reads them (wide_hud.c split_layout): each pane's subtree end and kind, a hash beside each name
// (wide_hud_name_hash), the pictures and materials of P_InfoBg_00 and P_MenuBg_00 (a picture record with its lists
// inside its own 0x88 bytes, a material with one texture map right after it), the per-material array at 0x300
// (P_MenuBg_00's entry 7, to see the new ones take it), the four pages' parts and the two captures with their panes'
// indices, and the string pool section reaching the end of the file. Appended after make_layout(L)'s records
// (pane_at[] still its); the file's new size.
static const char *const plus_pictures[2] = {"P_InfoBg_00", "P_MenuBg_00"};
static const char *const plus_parts[4] = {"Pa_Collect_00", "Pa_Equip_00", "Pa_Item_00", "Pa_System_00"};
static const char *const plus_captures[2] = {"C_3DFade_00", "C_3DFade_01"};
static size_t plus_records(const LayoutDef *L, size_t n) {
    top = n;
    int np = L->npanes;
    uint32_t end[256];
    for (int i = 0; i < np; i++) end[i] = (uint32_t)i + 1;
    for (int i = np - 1; i > 0; i--)
        if (end[i] > end[L->panes[i].parent]) end[L->panes[i].parent] = end[i];
    uint64_t hashes = alloc(8 * (size_t)np);
    for (int i = 0; i < np; i++) {
        const char *nm = L->panes[i].name;
        uint32_t kind = !strncmp(nm, "Pa_", 3) ? 6 : !strncmp(nm, "P_", 2) ? 1 : !strncmp(nm, "C_", 2) ? 5
                        : !strncmp(nm, "T_", 2)                           ? 2
                                                                          : 0;
        w32(pane_at[i] + 0x34, end[i]);
        w32(pane_at[i] + 0x38, 1 | kind << 7 | 2u << 11 | 5u << 14 | 2u << 17 | 5u << 20);
        for (int k = 0x3c; k < 0x44; k += 2) w16(pane_at[i] + k, 0xffff);
        w64(hashes + 8 * (uint64_t)i, wide_hud_name_hash(nm));
    }
    w64(0x128, hashes);
    uint64_t lists[2][3];   // the pictures' and the materials' names, records and hashes
    for (int t = 0; t < 2; t++)
        for (int l = 0; l < 3; l++) lists[t][l] = alloc(16);
    for (uint16_t k = 0; k < 2; k++) {
        int pane = pane_index(L, plus_pictures[k]);
        uint64_t name = str(plus_pictures[k]), mat = alloc(0x78), pic = alloc(0x88);
        w64(mat, mat + 0x40), w32(mat + 8, 1), w32(mat + 0xc, 8), w64(mat + 0x40, mat + 0x48);
        wf(mat + 0x48 + 0x10, 1), wf(mat + 0x48 + 0x14, 1), wf(mat + 0x48 + 0x18, 0), wf(mat + 0x48 + 0x1c, 0);
        wf(mat + 0x48 + 0x20, 0);
        static const unsigned ptr[6][2] = {{0x00, 0x18}, {0x20, 0x48}, {0x48, 0x68},
                                           {0x50, 0x70}, {0x58, 0x78}, {0x60, 0x80}};
        for (int j = 0; j < 6; j++) w64(pic + ptr[j][0], pic + ptr[j][1]);
        w16(pic + 0x10, k);
        w16(pane_at[pane] + 0x3c, k), w16(pane_at[pane] + 0x42, k);
        for (int t = 0; t < 2; t++) {
            w64(lists[t][0] + 8 * k, name), w64(lists[t][1] + 8 * k, t ? mat : pic);
            w64(lists[t][2] + 8 * k, wide_hud_name_hash(plus_pictures[k]));
        }
    }
    for (int t = 0; t < 2; t++) {
        uint64_t slot = t ? 0x138 : 0x158;
        w64(slot, lists[t][0]), w64(slot + 8, lists[t][1]), w64(slot + 16, lists[t][2]), w32(slot + 24, 2);
        w32(slot + 28, 8);
    }
    uint64_t extra = alloc(8);
    w32(extra, 0), w32(extra + 4, 7);
    w64(0x300, extra), w32(0x308, 2), w32(0x30c, 4);
    for (int t = 0; t < 2; t++) {   // the parts (their pane at +0x40) and the captures (at +0x04)
        int count = t ? 2 : 4;
        uint64_t slot = t ? 0x1d8 : 0x1b8, names = alloc(8 * (size_t)count), records = alloc(8 * (size_t)count),
                 hash = alloc(8 * (size_t)count);
        for (int k = 0; k < count; k++) {
            const char *nm = t ? plus_captures[k] : plus_parts[k];
            uint64_t r = alloc(0x48);
            w32(r + (t ? 4 : 0x40), (uint32_t)pane_index(L, nm));
            w64(names + 8 * (uint64_t)k, str(nm)), w64(records + 8 * (uint64_t)k, r);
            w64(hash + 8 * (uint64_t)k, wide_hud_name_hash(nm));
        }
        w64(slot, names), w64(slot + 8, records), w64(slot + 16, hash), w32(slot + 24, (uint32_t)count);
        w32(slot + 28, 8);
    }
    top = (top + 7) & ~(size_t)7;
    uint64_t pool = top;
    str("StringPool");
    memcpy(file + 0xa8, "BFSI", 4), w32(0xac, 0xf8), w32(0xb0, (uint32_t)pool - 0xf8), w32(0xb4, 1), w64(0xb8, 0xf8);
    memcpy(file + 0xd0, "BFSI", 4), w32(0xd4, (uint32_t)pool), w32(0xd8, (uint32_t)(top - pool)), w32(0xdc, 1);
    w64(0xe0, pool);
    w32(0x08, (uint32_t)top);
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
    // the pause menu's map and the world map, which the menus over the whole picture lay out wider
    {"BG_00", 0, 0, 0, 1, 0, 3840},
    {"P_NotOpen_00", 5, 0, 0, 1, 0, 4080},
    {"P_MapBase_00", 5, -44, 0, 1, 0, 3928},
    {"P_PicField_00", 5, 1600, -700, 0.75f, 0, 500},   // its own scale: a move in its parent's space all the same
    {"Pa_Guide_01", 5, 1796, -888, 1, 0},
    {"N_FloorTab_00", 5, -1740, -118, 1, 0},
    {"T_MapTitle_00", 5, -1702, 640, 1, 0},
    {"N_WorldMap_00", 4, 0, 0, 1, 0},
    {"N_Vignette_00", 25, 0, 0, 1, 0},
    {"P_Vignette_L_00", 26, -2040, 0, 1, 0, 1200},
    {"P_Vignette_R_00", 26, 2040, 0, 1, 0, 1200},
    {"P_Overlay_00", 4, 0, 0, 1, 0, 3840},
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

static const PaneDef loading_panes[] = {{"RootPane", -1, 0, 0, 1, 0},
                                        {"N_In_00", 0, 0, 0, 1, 0},
                                        {"P_Fade_00", 1, 0, 0, 1, 0},
                                        {"N_IconIn_00", 0, 1778, -936, 1, 0},
                                        {"N_LoadIcon_00", 3, 0, 0, 1, 0}};
static const CurveDef loading_in[] = {{"N_IconIn_00", 4, 0, 2, {0, 1}}, {"N_In_00", 4, 0, 2, {0, 1}}};
static const AnimDef loading_anims[] = {{"NormalIn", loading_in, 2, NULL, 0}};

static const LayoutDef layouts[] = {
    {"PlayerStatus_00.buil", status_panes, sizeof status_panes / sizeof *status_panes, status_anims, 2},
    {"MapMenu_00.buil", map_panes, sizeof map_panes / sizeof *map_panes, map_anims, 3},
    {"NumDisplay_00.buil", num_panes, sizeof num_panes / sizeof *num_panes, num_anims, 1},
    {"LoadingFade_00.buil", loading_panes, sizeof loading_panes / sizeof *loading_panes, loading_anims, 1},
};
enum { NLAYOUTS = 4 };

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

// wide_hud_archive on b[0..n), which the shift does not make larger here: the archive edited in b.
static int archive_same(uint8_t *b, size_t n, const WideHudShift *s, char *why, size_t why_n) {
    uint8_t *p = malloc(n ? n : 1);
    size_t m = n;
    memcpy(p, b, n);
    int r = wide_hud_archive(&p, &m, s, why, why_n);
    CHECK(m == n, "the archive grew from %zu to %zu bytes", n, m);
    if (m == n) memcpy(b, p, n);
    free(p);
    return r;
}

// Each layout's file as make_layout makes it, kept for the archives below.
static uint8_t *made[NLAYOUTS];
static size_t made_n[NLAYOUTS];
static uint64_t made_pane_at[NLAYOUTS][256], made_values_at[NLAYOUTS][16][72];

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
    int r = wide_hud_lay_out(p, made_n[0], "PlayerStatus_00.buil", HUD(out), report, sizeof report, why, sizeof why);
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
    r = wide_hud_lay_out(q, made_n[0], "PlayerStatus_00.buil", HUD(out), report, sizeof report, why, sizeof why);
    CHECK(r == 0 && !memcmp(q, p, made_n[0]) && !report[0], "PlayerStatus_00 again: %d", r);
    r = wide_hud_lay_out(q, made_n[0], "PlayerStatus_00.buil", HUD(1920), report, sizeof report, why, sizeof why);
    CHECK(r == -1 && !memcmp(q, p, made_n[0]) && strstr(why, "N_ScaleL_00 x is -2580"), "PlayerStatus_00 at 1920 over "
          "660: %d (%s)", r, why);
    free(q);
    free(p);

    // MapMenu_00: the minimap's panes and the keys at its place; the pause map's pane and keys stay
    p = malloc(made_n[1]);
    memcpy(p, made[1], made_n[1]);
    r = wide_hud_lay_out(p, made_n[1], "MapMenu_00.buil", HUD(out), report, sizeof report, why, sizeof why);
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
    r = wide_hud_lay_out(p, made_n[2], "NumDisplay_00.buil", HUD(out), report, sizeof report, why, sizeof why);
    CHECK(r == 2 && pane_x(p, 2, "N_NumDisplayFirst_00") == 660 && pane_x(p, 2, "N_NumDisplaySecond_00") == 660 &&
              pane_x(p, 2, "Pa_NumDisplay_00") == 1580 && rf(p, made_values_at[2][0][0] + 24) == 380,
          "NumDisplay_00: %d (%s)", r, why);
    free(p);

    // LoadingFade_00: its icon squeezed into the middle 16:9's corner, for the menus' shift alone
    p = malloc(made_n[3]);
    memcpy(p, made[3], made_n[3]);
    CHECK(wide_hud_lay_out(p, made_n[3], "LoadingFade_00.buil", HUD(660), report, sizeof report, why, sizeof why) == 0 &&
              !memcmp(p, made[3], made_n[3]),
          "LoadingFade_00 changed by the HUD's shift");
    r = wide_hud_lay_out(p, made_n[3], "LoadingFade_00.buil", MENUS(0.75f), report, sizeof report, why, sizeof why);
    CHECK(r == 2 && pane_x(p, 3, "N_IconIn_00") == 1333.5f &&
              rf(p, made_pane_at[3][pane_index(&layouts[3], "N_IconIn_00")] + 0x10) == 0.75f &&
              pane_x(p, 3, "N_LoadIcon_00") == 0 && !strcmp(report, "N_IconIn_00 x 1778 -> 1333.5, scale x 1 -> 0.75"),
          "LoadingFade_00: %d, report %s (%s)", r, report, why);
    CHECK(wide_hud_lay_out(p, made_n[3], "LoadingFade_00.buil", MENUS(0.75f), report, sizeof report, why, sizeof why) ==
              0,
          "LoadingFade_00 again");
    free(p);
    p = malloc(made_n[0]);
    memcpy(p, made[0], made_n[0]);
    CHECK(wide_hud_lay_out(p, made_n[0], "PlayerStatus_00.buil", MENUS(0.75f), report, sizeof report, why, sizeof why) ==
              0 && !memcmp(p, made[0], made_n[0]),
          "PlayerStatus_00 changed by the menus' shift");
    free(p);

    // a member edges mode does not edit, and no distance: nothing
    p = malloc(made_n[0]);
    memcpy(p, made[0], made_n[0]);
    CHECK(wide_hud_lay_out(p, made_n[0], "DoCommand_00.buil", HUD(out), report, sizeof report, why, sizeof why) == 0 &&
              wide_hud_lay_out(p, made_n[0], "PlayerStatus_00.buil", HUD(0), report, sizeof report, why, sizeof why) == 0 &&
              !memcmp(p, made[0], made_n[0]),
          "an unedited member or out 0 changed something");
    // not a BFFH file
    p[0] = 'X';
    CHECK(wide_hud_lay_out(p, made_n[0], "PlayerStatus_00.buil", HUD(out), report, sizeof report, why, sizeof why) == -1,
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
    int r = wide_hud_lay_out(p, made_n[layout], layouts[layout].member, BOTH(660, 0.75f), report, sizeof report, why,
                             sizeof why);
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
static void f_icon_scale(uint8_t *p) { set_pane_f(p, 3, "N_IconIn_00", 0x10, 0.9f); }
static void f_root_x(uint8_t *p) { set_pane_f(p, 3, "RootPane", 0x1c, 10); }
static const CurveDef root_scaled_in[] = {{"RootPane", 0, 0, 2, {0.5f, 1}}};
static const AnimDef root_scaled[] = {{"NormalIn", root_scaled_in, 1, NULL, 0}};
static const CurveDef icon_scale_in[] = {{"N_IconIn_00", 0, 0, 2, {0.5f, 1}}};
static const AnimDef icon_scaled[] = {{"NormalIn", icon_scale_in, 1, NULL, 0}};
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
static CurveDef many_curves[65];
static const AnimDef many_tracks[] = {{"MapType", many_curves, 65, NULL, 0}};

// The layout L as make_layout makes it, refused by the shift s with a why that holds expect, every byte as it was.
static void refused_layout_by(const LayoutDef *L, const WideHudShift *s, const char *what, const char *expect) {
    char why[240] = "";
    size_t n = make_layout(L);
    uint8_t *keep = malloc(n);
    memcpy(keep, file, n);
    int r = wide_hud_lay_out(file, n, L->member, s, NULL, 0, why, sizeof why);
    CHECK(r == -1 && !memcmp(file, keep, n) && strstr(why, expect), "%s: %d, why \"%s\" (expected \"%s\")", what, r, why,
          expect);
    free(keep);
}
static void refused_layout(const LayoutDef *L, const char *what, const char *expect) {
    refused_layout_by(L, BOTH(660, 0.75f), what, expect);
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
    refused(3, "the loading icon's scale not the game's", "N_IconIn_00 scale x is 0.9, not 1", f_icon_scale);
    refused(3, "the loading icon in a parent off the centre", "N_IconIn_00 is inside a pane at x 10", f_root_x);
    {
        LayoutDef F = layouts[3];
        F.anims = icon_scaled, F.nanims = 1;
        refused_layout(&F, "the loading icon's scale keyed", "N_IconIn_00 has its scale x keyed by NormalIn");
        F.anims = root_scaled;
        refused_layout(&F, "the loading icon's parent scaled by an animation",
                       "N_IconIn_00 is inside a pane at x 0 or one an animation moves or scales");
    }
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
        int r = wide_hud_lay_out(p, made_n[1], "MapMenu_00.buil", HUD(660), NULL, 0, why, sizeof why);
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
    for (int i = 0; i < 65; i++) many_curves[i] = (CurveDef){"N_MapIcon_00", 1, 0, 1, {0}};
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
    int r = wide_hud_lay_out(file, n, L.member, HUD(660), NULL, 0, why, sizeof why);
    CHECK(r == -1 && strstr(why, "N_ScaleR_00 has its x keyed by HeartPos"), "keyed by a curve: %d (%s)", r, why);
    L.anims = keyed_const_anims;
    n = make_layout(&L);
    r = wide_hud_lay_out(file, n, L.member, HUD(660), NULL, 0, why, sizeof why);
    CHECK(r == -1 && strstr(why, "N_VRS_00 has its x keyed by IsSubItemVisible"), "keyed by a constant: %d (%s)", r, why);
    // every prefix of each layout: refused, never a read past it (the sanitizers watch)
    for (int i = 0; i < NLAYOUTS; i++)
        for (size_t cut = 0; cut < made_n[i]; cut += 3) {
            uint8_t *p = malloc(cut ? cut : 1);
            memcpy(p, made[i], cut);
            r = wide_hud_lay_out(p, cut, layouts[i].member, BOTH(660, 0.75f), NULL, 0, why, sizeof why);
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
    int r = archive_same(b, n, HUD(660), why, sizeof why);
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
    r = archive_same(c2, n, HUD(660), why, sizeof why);
    CHECK(r == 0 && !memcmp(c2, b, n) && nlog == 0, "archive again: %d, %d lines", r, nlog);
    // one bad copy of one layout: nothing changes in any layout
    memcpy(c2, a, n);
    float bad = 1;
    memcpy(c2 + member_at[5] + made_pane_at[2][pane_index(&layouts[2], "N_NumDisplayFirst_00")] + 0x1c, &bad, 4);
    uint8_t *keep = malloc(CAP);
    memcpy(keep, c2, n);
    r = archive_same(c2, n, HUD(660), why, sizeof why);
    CHECK(r == -1 && !memcmp(c2, keep, n) && strstr(why, "N_NumDisplayFirst_00 x is 1"), "archive with a bad copy: %d (%s)",
          r, why);
    size_t m;
    // an archive without one of the layouts
    const char *names[] = {layouts[0].member, layouts[1].member};
    const uint8_t *bytes[] = {made[0], made[1]};
    size_t sizes[] = {made_n[0], made_n[1]};
    m = make_sarc(c2, CAP, names, bytes, sizes, 2);
    memcpy(keep, c2, m);
    r = archive_same(c2, m, HUD(660), why, sizeof why);
    CHECK(r == -1 && !memcmp(c2, keep, m) && strstr(why, "has no NumDisplay_00.buil"), "archive without NumDisplay_00: "
          "%d (%s)", r, why);
    // the menus' shift too: LoadingFade_00's two copies, one line of its own; the menus' alone: only those
    memcpy(c2, a, n);
    nlog = 0;
    r = archive_same(c2, n, BOTH(660, 0.75f), why, sizeof why);
    CHECK(r == 66 + 4 && pane_x(c2 + member_at[6], 3, "N_IconIn_00") == 1333.5f &&
              pane_x(c2 + member_at[7], 3, "N_IconIn_00") == 1333.5f &&
              log_find("widescreen: menus: LoadingFade_00 (2 copies, 4 fields): N_IconIn_00 x 1778 -> 1333.5, scale x 1 "
                       "-> 0.75") && nlog == 4,
          "archive with both shifts: %d, %d lines (%s)", r, nlog, why);
    memcpy(c2, a, n);
    r = archive_same(c2, n, MENUS(0.75f), why, sizeof why);
    size_t diff = 0;
    for (size_t i = 0; i < n; i++) diff += c2[i] != a[i];
    CHECK(r == 4 && diff <= 16, "archive with the menus' shift: %d, %zu bytes", r, diff);
    {
        const char *nm[] = {layouts[0].member, layouts[1].member, layouts[2].member};
        const uint8_t *by[] = {made[0], made[1], made[2]};
        size_t sz[] = {made_n[0], made_n[1], made_n[2]};
        m = make_sarc(c2, CAP, nm, by, sz, 3);
        r = archive_same(c2, m, MENUS(0.75f), why, sizeof why);
        CHECK(r == -1 && strstr(why, "has no LoadingFade_00.buil"), "menus' shift without LoadingFade_00: %d (%s)", r,
              why);
        CHECK(archive_same(c2, m, HUD(660), why, sizeof why) == 33, "the HUD's shift without LoadingFade_00");
    }
    // more than 16 copies of a layout
    {
        const char *nm[20];
        const uint8_t *by[20];
        size_t sz[20];
        for (int i = 0; i < 17; i++) nm[i] = layouts[0].member, by[i] = made[0], sz[i] = made_n[0];
        nm[17] = layouts[1].member, by[17] = made[1], sz[17] = made_n[1];
        nm[18] = layouts[2].member, by[18] = made[2], sz[18] = made_n[2];
        m = make_sarc(c2, CAP, nm, by, sz, 19);
        r = archive_same(c2, m, HUD(660), why, sizeof why);
        CHECK(r == -1 && strstr(why, "more than 16 times"), "17 copies: %d (%s)", r, why);
    }
    // copies of one layout that share some of their bytes, and two layouts that share bytes
    n = archive(c2, CAP, 2);
    set_range(c2, 1, member_at[0] + 0x100, made_n[0]);
    memcpy(keep, c2, n);
    r = archive_same(c2, n, HUD(660), why, sizeof why);
    CHECK(r == -1 && !memcmp(c2, keep, n) && strstr(why, "two copies of PlayerStatus_00.buil in the layout archive "
          "overlap"), "copies that overlap: %d (%s)", r, why);
    n = archive(c2, CAP, 2);
    set_range(c2, 4, member_at[2], made_n[2]);   // a copy of NumDisplay_00 over MapMenu_00's first
    r = archive_same(c2, n, HUD(660), why, sizeof why);
    CHECK(r == -1 && strstr(why, "NumDisplay_00 and MapMenu_00 share bytes"), "layouts that share bytes: %d (%s)", r,
          why);
    // copies that are the same bytes are moved once
    n = archive(c2, CAP, 2);
    set_range(c2, 1, member_at[0], made_n[0]);
    r = archive_same(c2, n, HUD(660), why, sizeof why);
    CHECK(r == 4 + 2 * 27 + 2 * 2 && pane_x(c2 + member_at[0], 0, "N_ScaleL_00") == -2580, "copies of the same bytes: %d "
          "(%s)", r, why);
    // one copy moved already, the other not: the line says what the other moved
    n = archive(c2, CAP, 2);
    CHECK(wide_hud_lay_out(c2 + member_at[0], made_n[0], layouts[0].member, HUD(660), NULL, 0, why, sizeof why) == 4,
          "moving one copy");
    nlog = 0;
    r = archive_same(c2, n, HUD(660), why, sizeof why);
    CHECK(r == 4 + 2 * 27 + 2 * 2 && log_find("wide hud: PlayerStatus_00 (2 copies, 4 fields): N_ScaleL_00 x -1920 -> "
                                              "-2580"),
          "one copy moved already: %d (%s)", r, nlog ? log_lines[0] : "no line");
    // nothing at out 0
    memcpy(c2, a, n);
    CHECK(archive_same(c2, n, HUD(0), why, sizeof why) == 0 && !memcmp(c2, a, n), "archive at out 0");
    // damaged: a SARC cut short
    CHECK(archive_same(c2, 40, HUD(660), why, sizeof why) == -1, "a damaged archive");
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
    int r = wide_hud_build(z, zn, zdic, zdn, HUD(660), &out, &out_n);
    CHECK(r == 1 && out, "build: %d (%s)", r, last_log());
    CHECK(log_find("wide hud: the layout archive edited (the HUD's edge elements 660 units out), 66 fields"),
          "build: its line: %s", last_log());
    if (r == 1) {
        CHECK(ZSTD_getDictID_fromFrame(out, out_n) == 0x1234 && ZSTD_getFrameContentSize(out, out_n) == n,
              "build: another dictionary or size");
        uint8_t *raw = malloc(n), *expect = malloc(n);
        ZSTD_DCtx *dc = ZSTD_createDCtx();
        size_t got = ZSTD_decompress_usingDict(dc, raw, n, out, out_n, dict, dn);
        ZSTD_freeDCtx(dc);
        char why[240];
        memcpy(expect, a, n);
        archive_same(expect, n, HUD(660), why, sizeof why);
        CHECK(got == n && !memcmp(raw, expect, n), "build: not the archive edited in memory");
        r = wide_hud_build(out, out_n, zdic, zdn, HUD(660), &again, &again_n);
        CHECK(r == 0 && !again && strstr(last_log(), "has these edits already (the HUD's edge elements 660 units out)"),
              "build again: %d (%s)", r, last_log());
        free(raw);
        free(expect);
    }
    CHECK(wide_hud_build(z, zn, zdic, zdn, HUD(0), &again, &again_n) == 0 && !again, "build at out 0");
    CHECK(wide_hud_build(z, zn / 2, zdic, zdn, HUD(660), &again, &again_n) == -1 && strstr(last_log(), "the layout archive is served as it is"),
          "build of a cut frame: %s", last_log());
    free(out);
    free(z);
    free(zdic);
    free(dict);
    free(a);
}

// ---- the menus over the whole picture (NX2SARIA_WIDE_MENUS = full): the operations of group G_FULL in wide_hud.c's
// plan, on layouts with the panes, values and animation curves (slopes too) the game's 1.1.1 PlusMenu_00,
// MenuOverlay_00, PaShortCutSlideList_00 and PaItemMenu_00 hold as far as the edit reads them, MapMenu_00 above with
// its pause map's panes and LoadingFade_00 above. No plan uses OP_MOVE_IN or OP_WIDEN, so they are not reached here.
#define FULL(m) (&(WideHudShift){0, 1.0f, (m), 0})
#define HUD_FULL(o, m) (&(WideHudShift){(o), 1.0f, (m), 0})
#define COUNT(a) (int)(sizeof a / sizeof *a)

static const PaneDef plus_panes[] = {
    {"RootPane", -1, 0, 0, 1, 0},
    {"N_All_00", 0, 0, 0, 1, 0},
    {"C_3DFade_00", 1, 1080, 0, 1, 0, 1680},
    {"C_3DFade_01", 1, -1080, 0, 1, 0, 1680},
    {"N_3DFade_00", 1, 0, 0, 1, 0},
    {"P_3DFade_02", 4, 1080, 0, 1, 0, 1680},
    {"P_3DFade_01", 4, -1080, 0, 1, 0, 1680},
    {"N_3DFade_01", 1, 0, 0, 1, 0},
    {"P_Equipment3DFade_00", 7, 0, 0, 1, 0, 3840},
    {"N_InOut_00", 1, 0, 0, 1, 0},
    {"N_MenuChange_00", 9, 0, 0, 1, 0},
    {"P_InfoBg_00", 10, -1079, -515, 1, 0, 1682},
    {"P_InfoBg_01", 10, 4186, -515, 1, 0, 1600},
    {"P_InfoBg_02", 10, 9660, -515, 1, 0, 1742},
    {"P_InfoBg_03", 10, 10681, -515, 1, 0, 300},
    {"P_MenuBg_00", 10, 4280, 0, 1, 0, 9200},
    {"P_MenuBgAmiibo_00", 10, 2932, 562, 1, 0, 520},
    {"P_MenuBgArrow_00", 10, 7876, 248, 1, 0, 528},
    {"P_SystemBg_00", 10, 11512, 0, 1, 0, 1080},
    {"N_HeaderSh_00", 9, 0, 0, 1, 0},
    {"P_HeaderSh_00", 19, 0, 620, 1, 0, 3840},
    {"N_MenuChange_01", 9, 0, 0, 1, 0},
    {"Pa_Collect_00", 21, 0, 0, 1, 0},
    {"Pa_Equip_00", 21, 4280, 0, 1, 0},
    {"Pa_Item_00", 21, 8560, 0, 1, 0},
    {"Pa_System_00", 21, 12840, 0, 1, 0},
    {"N_DreamInfo_00", 21, 0, 0, 1, 0},
    {"N_Pos_01", 26, 8560, -75, 1, 0},
    {"N_Base_00", 27, 0, 0, 1, 0},
    {"P_MessageBaseSh_00", 28, -89, 0, 1, 0, 2200},
    {"P_MessageBase_00", 28, -89, 0, 1, 0, 2200},
    {"P_MessageBaseDeco_00", 28, -89, 0, 1, 0, 2200},
    {"T_Text_01", 27, 0, 2, 1, 0, 1720},
    {"N_Pos_00", 26, 4195, -75, 1, 0},
    {"N_Base_01", 33, 0, 0, 1, 0},
    {"P_MessageBaseSh_01", 34, -89, 0, 1, 0, 2200},
    {"P_MessageBase_01", 34, -89, 0, 1, 0, 2200},
    {"P_MessageBaseDeco_01", 34, -89, 0, 1, 0, 2200},
    {"T_Text_00", 33, 0, 2, 1, 0, 1720},
    {"P_Black_00", 9, 0, 0, 1, 0, 3840},
};
// The page strip's moves, each a curve of both N_MenuChange panes: three keys (value, slope).
#define PAGE(pane, a, b, c, s0, s1) {pane, 1, 0, 3, {a, b, c}, {s0, s1, 0}}
#define PAGES(a, b, c, s0, s1) {PAGE("N_MenuChange_00", a, b, c, s0, s1), PAGE("N_MenuChange_01", a, b, c, s0, s1)}
static const CurveDef collect_to_equip[] = PAGES(0, -3020, -4280, -560, -196);
static const CurveDef equip_to_item[] = PAGES(-4280, -7300, -8560, -560, -196);
static const CurveDef item_to_system[] = PAGES(-8560, -11580, -12840, -860, -196);
static const CurveDef system_to_item[] = PAGES(-12840, -9820, -8560, 560, 196);
static const CurveDef item_to_equip[] = PAGES(-8560, -5540, -4280, 560, 196);
static const CurveDef equip_to_collect[] = PAGES(-4280, -1260, 0, 560, 196);
static const CurveDef plus_in_after[] = {{"N_InOut_00", 1, 4, 3, {-38, -18, 0}, {5.609396f, 2.502355f, 0}}};
enum { PAGE_ANIMS = 6 };
#define PLUS_ANIMS                                                                                                     \
    {"CollectToEquip", collect_to_equip, 2, NULL, 0}, {"EquipToItem", equip_to_item, 2, NULL, 0},                      \
        {"ItemToSystem", item_to_system, 2, NULL, 0}, {"SystemToItem", system_to_item, 2, NULL, 0},                    \
        {"ItemToEquip", item_to_equip, 2, NULL, 0}, {"EquipToCollect", equip_to_collect, 2, NULL, 0},                  \
        {"InAfter", plus_in_after, 1, NULL, 0}
static const AnimDef plus_anims[] = {PLUS_ANIMS};
static const LayoutDef plus_menu = {"PlusMenu_00.buil", plus_panes, COUNT(plus_panes), plus_anims, COUNT(plus_anims)};
// another animation that holds a value of the strip the plan moves, and one of the strip's curves with a fourth key
static const CurveDef plus_other_keys[] = {{"N_MenuChange_00", 1, 0, 2, {0, -4280}}};
static const AnimDef plus_other[] = {PLUS_ANIMS, {"Other", plus_other_keys, 1, NULL, 0}};
static const CurveDef collect_to_equip4[] = {{"N_MenuChange_00", 1, 0, 4, {0, -3020, -4280, -4280}},
                                             PAGE("N_MenuChange_01", 0, -3020, -4280, -560, -196)};
static const AnimDef plus_four_keys[] = {
    {"CollectToEquip", collect_to_equip4, 2, NULL, 0}, {"EquipToItem", equip_to_item, 2, NULL, 0},
    {"ItemToSystem", item_to_system, 2, NULL, 0},      {"SystemToItem", system_to_item, 2, NULL, 0},
    {"ItemToEquip", item_to_equip, 2, NULL, 0},        {"EquipToCollect", equip_to_collect, 2, NULL, 0}};

static const PaneDef overlay_panes[] = {
    {"RootPane", -1, 0, 0, 1, 0},
    {"N_All_00", 0, 0, 0, 1, 0},
    {"N_HeaderAll_00", 1, 0, 0, 1, 0},
    {"N_InOut_00", 2, 0, 0, 1, 0},
    {"N_HeaderIn_00", 3, 0, 0, 1, 0},
    {"N_Header_00", 4, 0, 1000, 1, 0},
    {"P_MenuHeader_00", 5, 0, 40, 1, 0, 3840},
    {"N_MenuChange_00", 5, 0, 0, 1, 0},
    {"Pa_Category_00", 7, -690, 0, 1, 0},
    {"Pa_Category_01", 7, -230, 0, 1, 0},
    {"Pa_Category_02", 7, 230, 0, 1, 0},
    {"Pa_Category_03", 7, 690, 0, 1, 0},
    {"N_InOut_01", 5, 0, 0, 1, 0},
    {"T_BtnL_00", 12, -990, 2, 1, 0},
    {"T_BtnR_00", 12, 990, 2, 1, 0},
    {"N_Open_00", 12, -520, -8, 1, 0},
    // turned and scaled panes beside the moved ones, none of them an ancestor of one
    {"P_MenuHeaderEffect_01", 15, 0, 0, 1.4f, -1.5708f},
    {"P_MenuHeaderEffect_00", 15, 0, 0, 1.7f, -0.523599f},
    {"N_MenuHeaderGlow_00", 15, 0, 0, 2.4f, -0.349066f},
    {"P_MenuHeaderGlow_01", 18, 0, 0, 1, 0},
    {"P_MenuHeaderGlow_00", 18, 0, 0, 1, 0},
    {"N_MenuChange_01", 12, 0, -1000, 1, 0},
    {"A_Rupee_00", 21, 1652, 986, 1, 0},
    {"P_Icon_00", 22, -256, 3, 1, 0},
    {"T_Rupee_00", 22, 6, 0, 1, 0},
    {"T_Slash_00", 22, 52, -12, 1, 0},
    {"T_RupeeTotal_00", 22, 176, -12, 1, 0},
    {"P_ReplaceIcon_00", 21, 1792, 944, 1, 0},
    {"Pa_HeartGauge_00", 21, -1352, 984, 0.8f, 0},
    {"N_HeartPos_00", 21, -108, 0, 1, 0},
    {"Pa_MpGauge_00", 29, -1580, 956, 0.8f, 0},
    {"N_Footer_00", 0, 0, -1080, 1, 0},
    {"W_GuideBg_01", 31, 2110, -19, 1, 0, 1880},
    {"W_GuideBg_00", 31, 2110, -19, 1, 0, 1880},
    {"A_FirstGuide_00", 31, 340, 62, 1, 0, 3000},
};
// The panes the game's animations key by constants and curves, none of them moved.
static const ConstDef minus_to_plus[] = {{"Pa_Category_00", 1, 0, -690}, {"Pa_Category_01", 1, 0, -230},
                                         {"Pa_Category_02", 1, 0, 230}};
static const ConstDef plus_to_minus[] = {{"Pa_Category_00", 1, 0, -520}, {"Pa_Category_01", 1, 0, 0},
                                         {"Pa_Category_02", 1, 0, 520}};
static const CurveDef open_scales[] = {{"N_MenuHeaderGlow_00", 0, 0, 2, {1, 2.4f}, {0.044747f, 0}},
                                       {"P_MenuHeaderEffect_00", 0, 0, 2, {0.7f, 1.7f}, {0.027248f, 0}},
                                       {"P_MenuHeaderEffect_01", 0, 0, 2, {0.5f, 1.4f}, {0.039358f, 0}}};
static const ConstDef open_at[] = {{"N_Open_00", 1, 0, -520}};
static const CurveDef header_in[] = {{"N_HeaderIn_00", 1, 4, 2, {100, 0}}};
#define OVERLAY_ANIMS                                                                                                  \
    {"MinusToPlus", NULL, 0, minus_to_plus, 3}, {"PlusToMinus", NULL, 0, plus_to_minus, 3},                            \
        {"OpenImportant", open_scales, 3, open_at, 1}, {"HeaderIn", header_in, 1, NULL, 0}
static const AnimDef overlay_anims[] = {OVERLAY_ANIMS};
static const LayoutDef menu_overlay = {"MenuOverlay_00.buil", overlay_panes, COUNT(overlay_panes), overlay_anims,
                                       COUNT(overlay_anims)};
// a moved pane's x keyed by an animation the plan has no key operation for
static const ConstDef rupee_at[] = {{"A_Rupee_00", 1, 0, 1652}};
static const AnimDef overlay_rupee_keyed[] = {OVERLAY_ANIMS, {"RupeeIn", NULL, 0, rupee_at, 1}};

static const PaneDef slide_panes[] = {
    {"RootPane", -1, 0, 0, 1, 0},
    {"N_Cursor_00", 0, 0, 64, 1, 0},
    {"P_CursorEff_00", 1, 0, 0, 1, 0, 700},
    {"P_CursorLine_00", 1, 0, 0, 1, 0, 440},
    {"N_Arrow_00", 0, -220, 64, 1, 0},
    {"P_ArrowSh_00", 4, 8, 0, 1, 0, 220},
    {"P_Arrow_00", 4, 8, 0, 1, 0, 220},
    {"N_Arrow_01", 0, 220, 64, 1, 0},
    {"P_ArrowSh_01", 7, -8, 0, 1, 0, 220},
    {"P_Arrow_01", 7, -8, 0, 1, 0, 220},
    {"P_CursorSh_00", 0, 0, 24, 1, 0, 900},
    {"N_All_00", 0, 0, 0, 1, 0},
    {"N_SlideListAll_00", 11, 0, 0, 1, 0},
    {"Pa_ShortCutScrollBar_00", 12, 0, -502, 1, 0, 336},
    {"T_Text_00", 12, 0, -376, 1, 0, 1700},
    {"S_ScissorHT_00", 12, 0, 256, 1, 0, 3840},
    {"N_SlideList_00", 15, 0, -190, 1, 0, 3840},
};
static const CurveDef slide_loop[] = {{"N_Arrow_00", 1, 0, 3, {-220, -228, -220}},
                                      {"N_Arrow_01", 1, 0, 3, {220, 228, 220}}};
static const AnimDef slide_anims[] = {{"Loop", slide_loop, 2, NULL, 0}};
static const LayoutDef slide_list = {"PaShortCutSlideList_00.buil", slide_panes, COUNT(slide_panes), slide_anims, 1};

static const PaneDef item_panes[] = {
    {"RootPane", -1, 0, 0, 1, 0},          {"N_All_00", 0, 0, 0, 1, 0},
    {"N_Arrow_00", 1, -1850, -140, 1, 0},  {"P_ArrowSh_00", 2, 0, 0, 1, 0, 256},
    {"P_Arrow_00", 2, 0, 0, 1, 0, 256},    {"N_Info_00", 1, 1774, -450, 1, 0},
    {"P_Amiibo_00", 5, -678, 560, 0.83f, 0}, {"N_ItemAll_00", 1, 0, 152, 1, 0},
    {"N_Item_00", 7, -1602, -60, 1, 0}, {"Pa_ItemIcon_00", 8, 20, 396, 1, 0, 380},
};
// N_Arrow_00's x, moved, is keyed by Loop at two values of the plan's map: two keys at -1850, one at -1858.
static const CurveDef item_loop[] = {{"N_Arrow_00", 1, 0, 3, {-1850, -1858, -1850}}};
static const AnimDef item_anims[] = {{"Loop", item_loop, 1, NULL, 0}};
static const LayoutDef item_menu = {"PaItemMenu_00.buil", item_panes, COUNT(item_panes), item_anims, 1};
// Loop with one key at -1850, Loop as a constant, Loop with another animation keying the arrow, and no Loop at all
static const CurveDef item_loop_once[] = {{"N_Arrow_00", 1, 0, 3, {-1850, -1858, -1858}}};
static const AnimDef item_once[] = {{"Loop", item_loop_once, 1, NULL, 0}};
static const ConstDef item_loop_const[] = {{"N_Arrow_00", 1, 0, -1850}};
static const AnimDef item_const[] = {{"Loop", NULL, 0, item_loop_const, 1}};
static const CurveDef item_other_keys[] = {{"N_Arrow_00", 1, 0, 2, {-1850, -1900}}};
static const AnimDef item_other[] = {{"Loop", item_loop, 1, NULL, 0}, {"Other", item_other_keys, 1, NULL, 0}};
static const CurveDef item_alpha[] = {{"N_Arrow_00", 4, 0, 2, {0, 1}}};
static const AnimDef item_no_loop[] = {{"ArrowIn", item_alpha, 1, NULL, 0}};

// MapMenu_00's pause map: one of the panes the menus move keyed by an animation.
static const CurveDef vignette_in[] = {{"P_Vignette_R_00", 1, 0, 2, {2040, 1800}}};
static const AnimDef map_vignette_keyed[] = {{"MenuIn", map_menu_in, 1, NULL, 0}, {"MapType", map_type, 2, NULL, 0},
                                             {"DungeonMiniMapPattern", map_dungeon, 2, NULL, 0},
                                             {"VignetteIn", vignette_in, 1, NULL, 0}};

// A layout as make_layout made it: its bytes and where its panes' records and curves' values are.
typedef struct {
    const LayoutDef *L;
    uint8_t *p;
    size_t n;
    uint64_t pane_at[256], values_at[16][72];
} Made;

static void make(Made *m, const LayoutDef *L) {
    m->L = L;
    m->n = make_layout(L);
    if (!strcmp(L->member, "PlusMenu_00.buil")) m->n = plus_records(L, m->n);
    m->p = malloc(m->n);
    memcpy(m->p, file, m->n);
    memcpy(m->pane_at, pane_at, sizeof pane_at);
    memcpy(m->values_at, values_at, sizeof values_at);
}
static uint8_t *copy_of(const Made *m) {
    uint8_t *p = malloc(m->n);
    memcpy(p, m->p, m->n);
    return p;
}
enum { AT_SX = 0x10, AT_ROT = 0x18, AT_X = 0x1c, AT_W = 0x2c };
static uint64_t field(const Made *m, const char *pane, int off) {
    return m->pane_at[pane_index(m->L, pane)] + (uint64_t)off;
}
// Key k's value (slope 0) or slope (slope 1) of curve c of animation a; its span is at key_at(m, a, c, -3, 0).
static uint64_t key_at(const Made *m, int a, int c, int k, int slope) {
    return m->values_at[a][c] + (uint64_t)(24 + 8 * k + 4 * slope);
}
static void put(uint8_t *p, uint64_t at, float v) { memcpy(p + at, &v, 4); }
static int near(float a, float b) { return fabsf(a - b) < 0.05f; }

// m's file edited by the shift: `fields` changed, and a second edit of the edited file (the reading back the game's
// next start would make of a file served so) changes nothing and reports nothing. The edited copy, malloc'd.
static uint8_t *edited(const Made *m, const WideHudShift *s, int fields, char *report, size_t report_n) {
    char why[240] = "", again[64] = "x";
    uint8_t *p = copy_of(m), *q = malloc(m->n);
    int r = wide_hud_lay_out(p, m->n, m->L->member, s, report, report_n, why, sizeof why);
    CHECK(r == fields, "%s: %d fields, not %d (%s)", m->L->member, r, fields, why);
    memcpy(q, p, m->n);
    r = wide_hud_lay_out(q, m->n, m->L->member, s, again, sizeof again, why, sizeof why);
    CHECK(r == 0 && !memcmp(q, p, m->n) && !again[0], "%s read back: %d, report \"%s\" (%s)", m->L->member, r, again,
          why);
    free(q);
    return p;
}

// p, a copy of m's file with one fault: the shift refuses it with a why that holds expect and leaves every byte.
// Frees p.
static void refuse(const Made *m, uint8_t *p, const WideHudShift *s, const char *what, const char *expect) {
    char why[240] = "";
    uint8_t *keep = malloc(m->n);
    memcpy(keep, p, m->n);
    int r = wide_hud_lay_out(p, m->n, m->L->member, s, NULL, 0, why, sizeof why);
    CHECK(r == -1 && !memcmp(p, keep, m->n) && strstr(why, expect), "%s: %d, why \"%s\" (expected \"%s\")", what, r,
          why, expect);
    free(keep);
    free(p);
}

// Each pane's field (AT_X, AT_W, AT_SX) holds v in p.
typedef struct {
    const char *pane;
    int off;
    float v;
} Want;
static void check_fields(const Made *m, const uint8_t *p, const Want *w, int n) {
    for (int i = 0; i < n; i++) {
        float got = rf(p, field(m, w[i].pane, w[i].off));
        CHECK(near(got, w[i].v), "%s %s at +0x%x: %g, not %g", m->L->member, w[i].pane, w[i].off, (double)got,
              (double)w[i].v);
    }
}

static const WideHudCount *count_of(const WideHudCount *c, int n, const char *layout) {
    for (int i = 0; i < n; i++)
        if (!strcmp(c[i].layout, layout)) return &c[i];
    return NULL;
}

static void check_menu_counts(void) {
    WideHudCount c[32], h[32];
    int n = wide_hud_counts_of(1, c, 32), nh = wide_hud_counts_of(0, h, 32);
    static const struct { const char *layout; int panes, keys; } want[] = {
        {"MapMenu_00", 11, 0},     {"LoadingFade_00", 1, 0}, {"MenuOverlay_00", 8, 0},
        {"PlusMenu_00", 16, 32},   {"PaItemMenu_00", 2, 3},  {"PaShortCutSlideList_00", 2, 0},
        {"PaCollectMenu_00", 2, 3}, {"SystemActionGuide_00", 2, 2},
    };
    CHECK(n == 19 && count_of(c, n, "TitleMenuBG_00") && !count_of(c, n, "PlayerStatus_00") &&
              !count_of(c, n, "NumDisplay_00"),
          "wide_hud_counts_of(1): %d layouts", n);
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
        const WideHudCount *k = count_of(c, n, want[i].layout);
        CHECK(k && k->panes == want[i].panes && k->keys == want[i].keys, "wide_hud_counts_of(1) %s: %d panes, %d keys",
              want[i].layout, k ? k->panes : -1, k ? k->keys : -1);
    }
    // the HUD's counts leave the menus' operations of the same layout out (MapMenu_00)
    CHECK(nh == 3 && count_of(h, nh, "MapMenu_00") && count_of(h, nh, "MapMenu_00")->panes == 5 &&
              count_of(h, nh, "MapMenu_00")->keys == 21 && !count_of(h, nh, "PlusMenu_00"),
          "wide_hud_counts_of(0): %d layouts", nh);
}

static void check_plus_menu(void) {
    char report[4096];
    float u = 660, F = 1 + 2 * u / 4280;   // the pages 4280 units apart, each 2 * u further from the one before
    Made m;
    make(&m, &plus_menu);
    // 17 pane fields; per curve the keys at the plan's values, both nonzero slopes and the span: (5 + 6 * 4 + 5) * 2.
    // P_MenuBg_00 stays: the split draws it in pieces (check_split)
    uint8_t *p = edited(&m, FULL(u), 17 + 68, report, sizeof report);
    static const Want moved[] = {
        {"Pa_Equip_00", AT_X, 5600},        {"Pa_Item_00", AT_X, 11200},        {"Pa_System_00", AT_X, 16800},
        {"N_Pos_00", AT_X, 5515},           {"N_Pos_01", AT_X, 11200},          {"P_InfoBg_00", AT_X, -1409},
        {"P_InfoBg_00", AT_W, 2342},        {"P_InfoBg_01", AT_X, 5506},        {"P_InfoBg_02", AT_X, 12630},
        {"P_InfoBg_02", AT_W, 2402},        {"P_InfoBg_03", AT_X, 13981},       {"P_MenuBg_00", AT_X, 4280},
        {"P_MenuBg_00", AT_W, 9200},        {"P_MenuBgAmiibo_00", AT_X, 3592}, {"P_MenuBgArrow_00", AT_X, 9856},
        {"P_SystemBg_00", AT_X, 14812},     {"P_HeaderSh_00", AT_W, 5160},      {"P_Black_00", AT_W, 5160},
        {"P_Equipment3DFade_00", AT_W, 5160},
        // what the plan leaves: the strip's own panes, the first page, a grown pane's x, a moved pane's scale
        {"N_MenuChange_00", AT_X, 0},       {"N_MenuChange_01", AT_X, 0},       {"Pa_Collect_00", AT_X, 0},
        {"P_HeaderSh_00", AT_X, 0},         {"P_InfoBg_00", AT_SX, 1},          {"C_3DFade_00", AT_W, 1680},
    };
    check_fields(&m, p, moved, COUNT(moved));
    // every key, slope and span of the strip's twelve curves grows by one factor: the motion keeps its shape
    int off = 0;
    for (int a = 0; a < PAGE_ANIMS; a++)
        for (int c = 0; c < 2; c++) {
            const CurveDef *C = &plus_anims[a].curves[c];
            for (int k = 0; k < 3; k++) {
                off += !near(rf(p, key_at(&m, a, c, k, 0)), C->keys[k] * F);
                off += !near(rf(p, key_at(&m, a, c, k, 1)), C->slopes[k] * F);
            }
            off += !near(rf(p, key_at(&m, a, c, -3, 0)), (C->keys[2] - C->keys[0]) * F);
        }
    CHECK(!off, "PlusMenu_00: %d keys, slopes or spans of the page strip not %g times the game's", off, (double)F);
    CHECK(near(rf(p, key_at(&m, 0, 0, 1, 0)), -3951.4f) && near(rf(p, key_at(&m, 0, 0, 1, 1)), -256.449f) &&
              rf(p, key_at(&m, 0, 0, 2, 0)) == -5600 && rf(p, key_at(&m, 0, 0, -3, 0)) == -5600 &&
              rf(p, key_at(&m, 0, 0, 0, 0)) == 0 && rf(p, key_at(&m, 0, 0, 2, 1)) == 0,
          "PlusMenu_00 CollectToEquip: %g (slope %g) %g, span %g", (double)rf(p, key_at(&m, 0, 0, 1, 0)),
          (double)rf(p, key_at(&m, 0, 0, 1, 1)), (double)rf(p, key_at(&m, 0, 0, 2, 0)),
          (double)rf(p, key_at(&m, 0, 0, -3, 0)));
    // the y curve of another pane stays, slopes and all
    CHECK(rf(p, key_at(&m, 6, 0, 0, 0)) == -38 && rf(p, key_at(&m, 6, 0, 0, 1)) == 5.609396f &&
              rf(p, key_at(&m, 6, 0, -3, 0)) == 38,
          "PlusMenu_00 InAfter changed");
    CHECK(!strncmp(report, "Pa_Equip_00 x 4280 -> 5600, Pa_Item_00 x 8560 -> 11200, ", 56) &&
              strstr(report, ", P_InfoBg_00 x -1079 -> -1409, P_InfoBg_00 width 1682 -> 2342, ") &&
              !strstr(report, "P_MenuBg_00 ") &&
              strstr(report, ", CollectToEquip N_MenuChange_00 x 1 key -3020 -> -3951.4, CollectToEquip "
                             "N_MenuChange_00 x 1 key -4280 -> -5600, CollectToEquip N_MenuChange_01 x 1 key ") &&
              strstr(report, ", EquipToCollect N_MenuChange_01 x 1 key -1260 -> -1648.6") &&
              !strstr(report, "InAfter"),
          "PlusMenu_00 report: %s", report);
    // the HUD's shift and the menus in the middle 16:9 leave it; another distance over the edited file is refused
    uint8_t *q = copy_of(&m);
    CHECK(wide_hud_lay_out(q, m.n, m.L->member, HUD(660), report, sizeof report, report, sizeof report) == 0 &&
              wide_hud_lay_out(q, m.n, m.L->member, MENUS(0.75f), report, sizeof report, report, sizeof report) == 0 &&
              !memcmp(q, m.p, m.n),
          "PlusMenu_00 changed by the HUD's shift or the squeeze");
    free(q);
    refuse(&m, p, FULL(1000), "PlusMenu_00 edited at 660, then at 1000", "PlusMenu_00 Pa_Equip_00 x is 5600, not 4280");
    // 32:9 (u 1920): the strip's pages twice as far apart
    p = edited(&m, FULL(1920), 17 + 68, NULL, 0);
    CHECK(rf(p, field(&m, "Pa_System_00", AT_X)) == 24360 && rf(p, key_at(&m, 2, 1, 2, 0)) == -24360 &&
              near(rf(p, key_at(&m, 2, 0, 0, 1)), -860 * (1 + 2 * 1920.0f / 4280)),
          "PlusMenu_00 at 1920: %g %g", (double)rf(p, field(&m, "Pa_System_00", AT_X)),
          (double)rf(p, key_at(&m, 2, 1, 2, 0)));
    free(p);

    // refusals, each leaving the file as it was: a pane's x, a width, a key, a span, a curve's length, another
    // animation at a value the strip moves
    p = copy_of(&m);
    put(p, field(&m, "Pa_Item_00", AT_X), 8000);
    refuse(&m, p, FULL(u), "PlusMenu_00 a page's x", "PlusMenu_00 Pa_Item_00 x is 8000, not 8560");
    p = copy_of(&m);
    put(p, field(&m, "P_InfoBg_02", AT_W), 1700);
    refuse(&m, p, FULL(u), "PlusMenu_00 a width", "PlusMenu_00 P_InfoBg_02 width is 1700, not 1742");
    p = copy_of(&m);
    put(p, key_at(&m, 1, 0, 1, 0), -7301);
    refuse(&m, p, FULL(u), "PlusMenu_00 a key", "PlusMenu_00 EquipToItem N_MenuChange_00 x has 0 keys at -7300, not 1 of 3");
    p = copy_of(&m);
    put(p, key_at(&m, 3, 1, 2, 0), -12840);   // SystemToItem ends where it starts: two keys at -12840
    refuse(&m, p, FULL(u), "PlusMenu_00 a key at another value of the map",
           "PlusMenu_00 SystemToItem N_MenuChange_01 x has 2 keys at -12840, not 1 of 3");
    p = copy_of(&m);
    put(p, key_at(&m, 2, 1, -3, 0), -4000);
    refuse(&m, p, FULL(u), "PlusMenu_00 a span",
           "PlusMenu_00 ItemToSystem N_MenuChange_01 x has span -4000, not its last value less its first, -4280");
    LayoutDef L = plus_menu;
    L.anims = plus_four_keys, L.nanims = COUNT(plus_four_keys);
    refused_layout_by(&L, FULL(u), "PlusMenu_00 a curve of four keys",
                      "PlusMenu_00 CollectToEquip N_MenuChange_00 x is not one curve of 3 keys");
    L.anims = plus_other, L.nanims = COUNT(plus_other);
    refused_layout_by(&L, FULL(u), "PlusMenu_00 another animation at a page's place",
                      "PlusMenu_00 N_MenuChange_00 x is also at -4280 in Other");
    // every prefix of what the edit reads (the panes and animations, before the records the split reads): refused,
    // never a read past it
    size_t reads = make_layout(&plus_menu);
    for (size_t cut = 0; cut < reads; cut += 5) {
        q = malloc(cut ? cut : 1);
        memcpy(q, m.p, cut);
        char why[240];
        int r = wide_hud_lay_out(q, cut, m.L->member, FULL(u), NULL, 0, why, sizeof why);
        free(q);
        CHECK(r == -1, "PlusMenu_00 cut to %zu bytes: %d", cut, r);
        if (r != -1) break;
    }
    free(m.p);
}

// ---- the pause menu's backdrop drawn in pieces (wide_hud.c split_layout), on PlusMenu_00 with the records the game's
// file has for it (plus_records)
static uint64_t r64(const uint8_t *p, uint64_t at) {
    uint64_t v;
    memcpy(&v, p + at, 8);
    return v;
}
static uint32_t r32(const uint8_t *p, uint64_t at) {
    uint32_t v;
    memcpy(&v, p + at, 4);
    return v;
}
static uint16_t r16(const uint8_t *p, uint64_t at) {
    uint16_t v;
    memcpy(&v, p + at, 2);
    return v;
}
// Entry j of the table whose header slots start at slot: its name, its record.
static const char *entry_name_of(const uint8_t *p, uint64_t slot, uint32_t j) {
    return (const char *)p + r64(p, r64(p, slot) + 8 * (uint64_t)j);
}
static uint64_t entry_rec(const uint8_t *p, uint64_t slot, uint32_t j) {
    return r64(p, r64(p, slot + 8) + 8 * (uint64_t)j);
}
// The pane index track t of animation a names.
static unsigned track_pane(const uint8_t *p, int a, int t) {
    uint64_t r = r64(p, r64(p, 0x240) + 8 * (uint64_t)a), body = r64(p, r);
    uint64_t e = r64(p, r64(p, body + 8) + 8 * (uint64_t)t), c = r64(p, e), v = 0;
    for (int i = 5; i >= 0; i--) v = v << 8 | p[c + i];
    return (unsigned)(((v >> 11) & 0xf) | (((v >> 16) & 7) << 4));
}

// The split of p (a copy of m's file with one fault) refused with a why that holds expect, nothing made. Frees p.
static void refuse_split(const Made *m, uint8_t *p, const char *what, const char *expect) {
    char why[300] = "";
    uint8_t *o = NULL;
    size_t on = 0;
    int r = wide_hud_split(p, m->n, m->L->member, FULL(660), &o, &on, NULL, 0, why, sizeof why);
    CHECK(r == -1 && !o && strstr(why, expect), "split, %s: %d, why \"%s\" (expected \"%s\")", what, r, why, expect);
    free(o);
    free(p);
}

static void check_split(void) {
    static const struct {
        const char *name;
        uint64_t hash;
    } hashes[] = {{"RootPane", 0x55a7ab4a9d264968ull},
                  {"N_All_00", 0x7b2f820ba988bfbcull},
                  {"P_MenuBg_00", 0x8fb0748a44554936ull},
                  {"P_Equipment3DFade_00", 0x8a617cf9dbbc0adull}};
    for (size_t i = 0; i < sizeof hashes / sizeof *hashes; i++)
        CHECK(wide_hud_name_hash(hashes[i].name) == hashes[i].hash, "name hash of %s: %#llx", hashes[i].name,
              (unsigned long long)wide_hud_name_hash(hashes[i].name));
    CHECK(wide_hud_name_hash("P_ThisNameIsLongerThanThirtyTwoBytes") == 0, "the hash of a name of 36 bytes");
    Made m;
    make(&m, &plus_menu);
    char report[200] = "", why[300] = "";
    uint8_t *o = NULL;
    size_t on = 0;
    int r = wide_hud_split(m.p, m.n, m.L->member, FULL(660), &o, &on, report, sizeof report, why, sizeof why);
    CHECK(r == 6 && o && on > m.n && !strcmp(report, "P_MenuBg_00 in 7 pieces"), "split: %d, %s (%s)", r, report, why);
    if (!o) {
        free(m.p);
        return;
    }
    // the pieces after P_MenuBg_00, every later pane six further on, its parent and subtree end with it
    uint32_t np = r32(o, 0x130);
    int parent[64], end[64], bad = 0;
    CHECK(np == 46, "split: %u panes", np);
    for (uint32_t j = 0; j < np && j < 64; j++) {
        char want[40];
        int src = j < 16 ? (int)j : j < 22 ? 15 : (int)j - 6;
        if (j >= 16 && j < 22) snprintf(want, sizeof want, "P_MenuBg_00_%u", j - 15);
        else snprintf(want, sizeof want, "%s", plus_panes[src].name);
        bad += strcmp(entry_name_of(o, 0x118, j), want) != 0 ||
               r64(o, r64(o, 0x128) + 8 * (uint64_t)j) != wide_hud_name_hash(want);
        parent[j] = plus_panes[src].parent > 15 ? plus_panes[src].parent + 6 : plus_panes[src].parent;
        end[j] = (int)j + 1;
    }
    for (int j = (int)np - 1; j > 0 && np <= 64; j--)
        if (end[j] > end[parent[j]]) end[parent[j]] = end[j];
    for (uint32_t j = 0; j < np && j < 64; j++)
        bad += (int)r32(o, entry_rec(o, 0x118, j) + 0x28) != parent[j] ||
               (int)r32(o, entry_rec(o, 0x118, j) + 0x34) != end[j];
    CHECK(!bad && end[0] == 46 && end[9] == 46 && end[10] == 25 && end[15] == 16 && end[21] == 22 && parent[27] == 9 &&
              parent[28] == 27,
          "split: %d panes not in place, of their parent or with their subtree's end", bad);
    // the page strip's tracks: N_MenuChange_00 stays 10, N_MenuChange_01 moves from 21 to 27, N_InOut_00 stays 9
    bad = 0;
    for (int a = 0; a < PAGE_ANIMS; a++) bad += track_pane(o, a, 0) != 10 || track_pane(o, a, 1) != 27;
    CHECK(!bad && track_pane(o, PAGE_ANIMS, 0) == 9, "split: %d page tracks renumbered otherwise", bad);
    // the parts' panes past the split, the captures' before it
    bad = 0;
    for (uint32_t k = 0; k < 4; k++) bad += r32(o, entry_rec(o, 0x1b8, k) + 0x40) != 28 + k;
    for (uint32_t k = 0; k < 2; k++) bad += r32(o, entry_rec(o, 0x1d8, k) + 4) != 2 + k;
    CHECK(!bad, "split: %d parts' or captures' panes", bad);
    // each piece where it moves at u 660, with a picture and a material of its own showing its window: the seams the
    // picture's plain stretch from 1880 to 2010 (of -320 to 8880), the others their own part of it
    static const float x[7] = {10, 2190, 4370, 5500, 6630, 8910, 11190}, w[7] = {660, 3700, 660, 1600, 660, 3900, 660};
    static const float uvs[7] = {0.0141304f, 0.4021739f, 0.0141304f, 0.1739130f, 0.0141304f, 0.4239130f, 0.0141304f},
                       uvt[7] = {-0.2538043f, -0.2989130f, -0.2538043f, -0.0108696f, -0.2538043f, 0.2880435f, -0.2538043f};
    bad = 0;
    uint64_t pics[7] = {0};
    for (uint32_t k = 0; k < 7; k++) {
        uint64_t rec = entry_rec(o, 0x118, 15 + k);
        uint16_t pi = r16(o, rec + 0x42), mi = r16(o, rec + 0x3c);
        uint64_t pic = pics[k] = entry_rec(o, 0x158, pi), mat = entry_rec(o, 0x138, mi);
        int own = k ? pi == 1 + k && mi == 1 + k : pi == 1 && mi == 1;
        int ok = own && rf(o, rec + AT_X) == x[k] && rf(o, rec + AT_W) == w[k] && r16(o, pic + 0x10) == mi &&
                 !strcmp(entry_name_of(o, 0x158, pi), entry_name_of(o, 0x118, 15 + k)) &&
                 !strcmp(entry_name_of(o, 0x138, mi), entry_name_of(o, 0x118, 15 + k)) &&
                 r64(o, pic) == pic + 0x18 && r64(o, pic + 0x20) == pic + 0x48 && r64(o, pic + 0x60) == pic + 0x80 &&
                 r64(o, mat) == mat + 0x40 && r32(o, mat + 8) == 1 && r64(o, mat + 0x40) == mat + 0x48 &&
                 fabsf(rf(o, mat + 0x58) - uvs[k]) < 1e-6f && fabsf(rf(o, mat + 0x60) - uvt[k]) < 1e-6f &&
                 rf(o, mat + 0x5c) == 1 && rf(o, mat + 0x64) == 0;
        if (!ok)
            printf("split piece %u: pane x %g width %g, picture %u material %u, uv %g %g\n", k, (double)rf(o, rec + AT_X),
                   (double)rf(o, rec + AT_W), pi, mi, (double)rf(o, mat + 0x58), (double)rf(o, mat + 0x60));
        bad += !ok;
        for (uint32_t j = 0; j < k; j++) bad += pics[j] == pic;
    }
    CHECK(!bad, "split: %d pieces not placed, or without a picture and material of their own", bad);
    // the tables of pictures and materials and the per-material array grown by six, each new entry P_MenuBg_00's 7
    bad = r32(o, 0x170) != 8 || r32(o, 0x150) != 8 || r32(o, 0x308) != 8 || r32(o, 0x30c) != 4;
    for (uint32_t j = 0; j < 8; j++) bad += r32(o, r64(o, 0x300) + 4 * j) != (j ? 7u : 0u);
    CHECK(!bad && !strcmp(entry_name_of(o, 0x158, 0), "P_InfoBg_00") &&
              r64(o, r64(o, 0x168) + 8 * 7) == wide_hud_name_hash("P_MenuBg_00_6"),
          "split: the pictures', materials' or per-material array's entries");
    // the file's size and the string pool's reach its new end
    CHECK(r32(o, 8) == on && r32(o, 0xd4) + r32(o, 0xd8) == on && r64(o, 0xe0) == r32(o, 0xd4),
          "split: the file's size %u, the pool's end %u, not %zu", r32(o, 8), r32(o, 0xd4) + r32(o, 0xd8), on);
    // split so already: nothing to do; the in-place edit of the split file is that of the game's
    uint8_t *again = (uint8_t *)o;
    size_t again_n = 0;
    r = wide_hud_split(o, on, m.L->member, FULL(660), &again, &again_n, report, sizeof report, why, sizeof why);
    CHECK(r == 0 && !again && !report[0], "split again: %d (%s)", r, why);
    uint8_t *q = malloc(on);
    memcpy(q, o, on);
    r = wide_hud_lay_out(q, on, m.L->member, FULL(660), NULL, 0, why, sizeof why);
    CHECK(r == 17 + 68 && rf(q, field(&m, "Pa_Equip_00", AT_X)) == 5600 && rf(q, field(&m, "P_MenuBg_00", AT_X)) == 10,
          "split, then edited in place: %d (%s)", r, why);
    // that file split again: the split finds its pieces; another distance does not
    r = wide_hud_split(q, on, m.L->member, FULL(660), &again, &again_n, NULL, 0, why, sizeof why);
    CHECK(r == 0 && !again, "split, edited, split again: %d (%s)", r, why);
    r = wide_hud_split(q, on, m.L->member, FULL(1000), &again, &again_n, NULL, 0, why, sizeof why);
    CHECK(r == -1 && !again && strstr(why, "PlusMenu_00 has P_MenuBg_00_1 but is not split so (PlusMenu_00 P_MenuBg_00 is "
                                           "at x 10, width 660"),
          "split at 660, then at 1000: %d (%s)", r, why);
    free(q);
    free(o);
    // 32:9: the gaps three times as wide
    r = wide_hud_split(m.p, m.n, m.L->member, FULL(1920), &o, &on, NULL, 0, why, sizeof why);
    CHECK(r == 6 && o && rf(o, entry_rec(o, 0x118, 15) + AT_X) == 640 && rf(o, entry_rec(o, 0x118, 21) + AT_X) == 15600 &&
              rf(o, entry_rec(o, 0x118, 21) + AT_W) == 1920 && rf(o, entry_rec(o, 0x118, 20) + AT_X) == 12690,
          "split at 1920: %d (%s)", r, why);
    free(o);
    // the HUD's shift, the squeeze, another layout: nothing
    o = (uint8_t *)m.p;
    CHECK(wide_hud_split(m.p, m.n, m.L->member, HUD(660), &o, &on, NULL, 0, why, sizeof why) == 0 && !o &&
              wide_hud_split(m.p, m.n, m.L->member, MENUS(0.75f), &o, &on, NULL, 0, why, sizeof why) == 0 && !o &&
              wide_hud_split(m.p, m.n, "MenuOverlay_00.buil", FULL(660), &o, &on, NULL, 0, why, sizeof why) == 0 && !o,
          "split by another shift or of another layout");
    // what the split refuses, each time making nothing
    uint64_t pane = field(&m, "P_MenuBg_00", 0), pic = entry_rec(m.p, 0x158, 1), mat = entry_rec(m.p, 0x138, 1);
    uint8_t *p = copy_of(&m);
    put(p, pane + AT_ROT, 0.5f);
    refuse_split(&m, p, "a turned picture", "PlusMenu_00 P_MenuBg_00 is turned 0.5 or scaled 1");
    p = copy_of(&m);
    put(p, pane + AT_SX, 0.5f);
    refuse_split(&m, p, "a scaled picture", "or scaled 0.5");
    p = copy_of(&m);
    memcpy(p + pane + 0x38, &(uint32_t){1 | 0u << 7 | 2u << 11 | 5u << 14}, 4);
    refuse_split(&m, p, "not a picture", "PlusMenu_00 P_MenuBg_00 is not a picture pane of its own");
    p = copy_of(&m);
    memcpy(p + pane + 0x38, &(uint32_t){1 | 1u << 7 | 1u << 11 | 5u << 14}, 4);
    refuse_split(&m, p, "an origin at the left", "or its origin is not at its centre");
    p = copy_of(&m);
    memcpy(p + field(&m, "P_MenuBgAmiibo_00", 0x28), &(uint32_t){15}, 4);
    refuse_split(&m, p, "a picture with a child", "PlusMenu_00 P_MenuBg_00 has children");
    p = copy_of(&m);
    memcpy(p + mat + 8, &(uint32_t){2}, 4);
    refuse_split(&m, p, "two texture maps", "PlusMenu_00 P_MenuBg_00 is not a plain picture with one texture map");
    p = copy_of(&m);
    memcpy(p + pic + 8, &(uint64_t){pic + 0x18}, 8);
    refuse_split(&m, p, "a picture with a window", "is not a plain picture with one texture map");
    p = copy_of(&m);
    memcpy(p + pic + 0x48, &(uint64_t){pic + 0x88}, 8);
    refuse_split(&m, p, "a picture record pointing past itself", "P_MenuBg_00's picture record is not laid out as expected");
    p = copy_of(&m);
    put(p, mat + 0x48 + 0x20, 0.3f);
    refuse_split(&m, p, "a turned texture map", "P_MenuBg_00's texture map is turned 0.3");
    p = copy_of(&m);
    memcpy(p + pane + 0x3c, &(uint16_t){0}, 2);
    refuse_split(&m, p, "a pane naming another material", "P_MenuBg_00's picture or material is not the one its pane names");
    p = copy_of(&m);
    memcpy(p + r64(m.p, 0x158) + 8, p + r64(m.p, 0x158), 8);   // the picture named P_InfoBg_00
    refuse_split(&m, p, "the picture of another name", "P_MenuBg_00's picture or material is not the one its pane names");
    p = copy_of(&m);
    memcpy(p + 0x308, &(uint32_t){3}, 4);
    refuse_split(&m, p, "a per-material array of another length", "per-material array holds 3 entries of 4 bytes, not 2 of 4");
    p = copy_of(&m);
    memcpy(p + 0xd8, &(uint32_t){r32(m.p, 0xd8) - 1}, 4);
    refuse_split(&m, p, "a string pool short of the end", "PlusMenu_00's string pool section or size does not reach its end");
    p = copy_of(&m);
    memcpy(p + 8, &(uint32_t){(uint32_t)m.n + 8}, 4);
    refuse_split(&m, p, "a file size past the end", "PlusMenu_00's string pool section or size does not reach its end");
    p = copy_of(&m);
    memcpy(p + r64(m.p, 0x1c0) + 8, p + r64(m.p, 0x1c0), 8);
    refuse_split(&m, p, "two parts of one record", "PlusMenu_00 has a damaged or shared part record");
    p = copy_of(&m);
    memcpy(p + r64(m.p, 0x120) + 8 * 16, p + r64(m.p, 0x120) + 8 * 15, 8);
    refuse_split(&m, p, "two panes of one record", "PlusMenu_00's panes 15 and 16 share a record");
    p = copy_of(&m);
    memcpy(p + 0x128, &(uint64_t){0}, 8);   // no list of hashes
    refuse_split(&m, p, "no hashes", "PlusMenu_00 has a damaged table of panes");
    p = copy_of(&m);
    memcpy(p + (uint64_t)(entry_name_of(m.p, 0x118, 16) - (const char *)m.p), "P_MenuBg_00_1", 14);
    refuse_split(&m, p, "a pane of the first piece's name", "PlusMenu_00 has P_MenuBg_00_1 but is not split so");
    // every prefix: refused, never a read past it
    for (size_t cut = 0; cut < m.n; cut += 7) {
        o = NULL;
        r = wide_hud_split(m.p, cut, m.L->member, FULL(660), &o, &on, NULL, 0, why, sizeof why);
        free(o);
        CHECK(r == -1, "split of PlusMenu_00 cut to %zu bytes: %d", cut, r);
        if (r != -1) break;
    }
    free(m.p);
}

static void check_menu_overlay(void) {
    char report[1200];
    Made m;
    make(&m, &menu_overlay);
    uint8_t *p = edited(&m, FULL(660), 8, report, sizeof report);
    static const Want moved[] = {
        {"P_MenuHeader_00", AT_W, 5160},   {"Pa_HeartGauge_00", AT_X, -2012}, {"Pa_MpGauge_00", AT_X, -2240},
        {"A_Rupee_00", AT_X, 2312},        {"P_ReplaceIcon_00", AT_X, 2452},  {"W_GuideBg_00", AT_X, 2770},
        {"W_GuideBg_01", AT_X, 2770},      {"A_FirstGuide_00", AT_X, 1000},
        {"P_MenuHeader_00", AT_X, 0},      {"Pa_Category_00", AT_X, -690},    {"N_Open_00", AT_X, -520},
        {"Pa_HeartGauge_00", AT_SX, 0.8f}, {"W_GuideBg_00", AT_W, 1880},
    };
    check_fields(&m, p, moved, COUNT(moved));
    CHECK(!strcmp(report, "P_MenuHeader_00 width 3840 -> 5160, Pa_HeartGauge_00 x -1352 -> -2012, Pa_MpGauge_00 x "
                          "-1580 -> -2240, A_Rupee_00 x 1652 -> 2312, P_ReplaceIcon_00 x 1792 -> 2452, W_GuideBg_00 x "
                          "2110 -> 2770, W_GuideBg_01 x 2110 -> 2770, A_FirstGuide_00 x 340 -> 1000"),
          "MenuOverlay_00 report: %s", report);
    free(p);
    // a moved pane inside a scaled or a turned pane (an OP_MOVE, not an OP_MOVE_IN), or keyed by an animation of
    // its own, or not at the game's x
    p = copy_of(&m);
    put(p, field(&m, "N_MenuChange_01", AT_SX), 0.8f);
    refuse(&m, p, FULL(660), "MenuOverlay_00 a scaled ancestor",
           "MenuOverlay_00 Pa_HeartGauge_00 is inside a pane scaled 0.8 or turned 0");
    p = copy_of(&m);
    put(p, field(&m, "N_InOut_01", AT_ROT), 0.5f);
    refuse(&m, p, FULL(660), "MenuOverlay_00 a turned ancestor",
           "MenuOverlay_00 Pa_HeartGauge_00 is inside a pane scaled 1 or turned 0.5");
    p = copy_of(&m);
    put(p, field(&m, "A_FirstGuide_00", AT_X), 300);
    refuse(&m, p, FULL(660), "MenuOverlay_00 a pane's x", "MenuOverlay_00 A_FirstGuide_00 x is 300, not 340");
    LayoutDef L = menu_overlay;
    L.anims = overlay_rupee_keyed, L.nanims = COUNT(overlay_rupee_keyed);
    refused_layout_by(&L, FULL(660), "MenuOverlay_00 a moved pane keyed by a constant",
                      "MenuOverlay_00 A_Rupee_00 has its x keyed by RupeeIn");
    free(m.p);
}

static void check_slide_list(void) {
    char report[600];
    Made m;
    make(&m, &slide_list);
    // a scissor and the list it clips grow on both sides; their x, their scale and the arrows' animation stay
    uint8_t *p = edited(&m, FULL(660), 2, report, sizeof report);
    static const Want grown[] = {{"S_ScissorHT_00", AT_W, 5160}, {"N_SlideList_00", AT_W, 5160},
                                 {"S_ScissorHT_00", AT_X, 0},    {"N_SlideList_00", AT_SX, 1},
                                 {"N_Arrow_00", AT_X, -220},     {"T_Text_00", AT_W, 1700}};
    check_fields(&m, p, grown, COUNT(grown));
    CHECK(rf(p, key_at(&m, 0, 0, 1, 0)) == -228 && rf(p, key_at(&m, 0, 1, 1, 0)) == 228,
          "PaShortCutSlideList_00: the arrows' Loop changed");
    CHECK(!strcmp(report, "S_ScissorHT_00 width 3840 -> 5160, N_SlideList_00 width 3840 -> 5160"),
          "PaShortCutSlideList_00 report: %s", report);
    free(p);
    p = copy_of(&m);
    put(p, field(&m, "N_SlideList_00", AT_W), 3000);
    refuse(&m, p, FULL(660), "PaShortCutSlideList_00 a width",
           "PaShortCutSlideList_00 N_SlideList_00 width is 3000, not 3840");
    // OP_GROW looks at the pane's own width alone: a scaled ancestor does not refuse it (the width grows in the
    // pane's own space). What the edit does today, not a property the plan's layouts need: theirs have scale 1.
    p = copy_of(&m);
    put(p, field(&m, "N_SlideListAll_00", AT_SX), 0.5f);
    char why[240] = "";
    CHECK(wide_hud_lay_out(p, m.n, m.L->member, FULL(660), NULL, 0, why, sizeof why) == 2 &&
              rf(p, field(&m, "N_SlideList_00", AT_W)) == 5160,
          "PaShortCutSlideList_00 inside a scaled pane: %s", why);
    free(p);
    free(m.p);
}

static void check_item_menu(void) {
    char report[600];
    Made m;
    make(&m, &item_menu);
    // a moved pane whose x an animation keys, with that animation's keys moved with it: both values of the map
    uint8_t *p = edited(&m, FULL(660), 2 + 3, report, sizeof report);
    CHECK(rf(p, field(&m, "N_Arrow_00", AT_X)) == -2510 && rf(p, field(&m, "N_ItemAll_00", AT_X)) == -660 &&
              rf(p, key_at(&m, 0, 0, 0, 0)) == -2510 && rf(p, key_at(&m, 0, 0, 1, 0)) == -2518 &&
              rf(p, key_at(&m, 0, 0, 2, 0)) == -2510 && rf(p, key_at(&m, 0, 0, -3, 0)) == 0 &&
              rf(p, field(&m, "N_Info_00", AT_X)) == 1774,
          "PaItemMenu_00: arrow %g, keys %g %g %g", (double)rf(p, field(&m, "N_Arrow_00", AT_X)),
          (double)rf(p, key_at(&m, 0, 0, 0, 0)), (double)rf(p, key_at(&m, 0, 0, 1, 0)),
          (double)rf(p, key_at(&m, 0, 0, 2, 0)));
    CHECK(!strcmp(report, "N_ItemAll_00 x 0 -> -660, N_Arrow_00 x -1850 -> -2510, Loop N_Arrow_00 x 2 keys -1850 -> "
                          "-2510, Loop N_Arrow_00 x 1 key -1858 -> -2518"),
          "PaItemMenu_00 report: %s", report);
    free(p);
    p = copy_of(&m);
    put(p, field(&m, "N_ItemAll_00", AT_X), 1);
    refuse(&m, p, FULL(660), "PaItemMenu_00 a pane's x", "PaItemMenu_00 N_ItemAll_00 x is 1, not 0");
    LayoutDef L = item_menu;
    L.anims = item_once;
    refused_layout_by(&L, FULL(660), "PaItemMenu_00 one key at a value of two",
                      "PaItemMenu_00 Loop N_Arrow_00 x has 1 keys at -1850, not 2 of 3");
    L.anims = item_const;
    refused_layout_by(&L, FULL(660), "PaItemMenu_00 the arrow's Loop a constant",
                      "PaItemMenu_00 Loop N_Arrow_00 x is a constant, not a curve of 3 keys");
    L.anims = item_other, L.nanims = 2;
    refused_layout_by(&L, FULL(660), "PaItemMenu_00 the arrow keyed by another animation",
                      "PaItemMenu_00 N_Arrow_00 has its x keyed by Other");
    L.anims = item_no_loop, L.nanims = 1;
    refused_layout_by(&L, FULL(660), "PaItemMenu_00 no Loop", "PaItemMenu_00 Loop does not key N_Arrow_00 x");
    free(m.p);
}

static void check_loading_pin(void) {
    char report[600], why[240] = "";
    Made m;
    make(&m, &layouts[3]);
    // the loading icon stretched with its canvas over the whole picture: its width the game's, as far from the right
    // side of the grid as the game's at that scale
    float q = 1920.0f / (1920 + 660);
    uint8_t *p = edited(&m, FULL(660), 2, report, sizeof report);
    CHECK(near(rf(p, field(&m, "N_IconIn_00", AT_X)), 1920 - 142 * q) &&
              near(rf(p, field(&m, "N_IconIn_00", AT_SX)), q) && rf(p, field(&m, "N_LoadIcon_00", AT_X)) == 0 &&
              !strcmp(report, "N_IconIn_00 x 1778 -> 1814.33, scale x 1 -> 0.744186"),
          "LoadingFade_00 pinned: %g, %g, report %s", (double)rf(p, field(&m, "N_IconIn_00", AT_X)),
          (double)rf(p, field(&m, "N_IconIn_00", AT_SX)), report);
    free(p);
    p = edited(&m, FULL(1920), 2, NULL, 0);
    CHECK(rf(p, field(&m, "N_IconIn_00", AT_X)) == 1849 && rf(p, field(&m, "N_IconIn_00", AT_SX)) == 0.5f,
          "LoadingFade_00 pinned at 1920: %g", (double)rf(p, field(&m, "N_IconIn_00", AT_X)));
    // pinned, then squeezed: refused, as the pinned icon is not at the game's x
    refuse(&m, p, MENUS(0.75f), "LoadingFade_00 pinned, then squeezed", "LoadingFade_00 N_IconIn_00 x is 1849, not 1778");
    // both groups at once (no setting asks for that: wide_menus is one or the other): two writes to one field
    refuse(&m, copy_of(&m), &(WideHudShift){0, 0.75f, 660, 0}, "LoadingFade_00 squeezed and pinned",
           "LoadingFade_00 has two moved values in one place");
    p = copy_of(&m);
    put(p, field(&m, "N_IconIn_00", AT_SX), 0.9f);
    refuse(&m, p, FULL(660), "LoadingFade_00 pinned, its scale not the game's", "N_IconIn_00 scale x is 0.9, not 1");
    LayoutDef L = layouts[3];
    L.anims = icon_scaled, L.nanims = 1;
    refused_layout_by(&L, FULL(660), "LoadingFade_00 pinned, its scale keyed",
                      "LoadingFade_00 N_IconIn_00 has its scale x keyed by NormalIn");
    // the HUD's shift alone leaves it
    p = copy_of(&m);
    CHECK(wide_hud_lay_out(p, m.n, m.L->member, HUD(660), NULL, 0, why, sizeof why) == 0 && !memcmp(p, m.p, m.n),
          "LoadingFade_00 changed by the HUD's shift");
    free(p);
    free(m.p);
}

static void check_map_full(void) {
    char report[1200];
    Made m;
    make(&m, &layouts[1]);
    static const Want full[] = {
        {"P_MapBase_00", AT_W, 5248},    {"P_NotOpen_00", AT_W, 5400},     {"BG_00", AT_W, 5160},
        {"P_Overlay_00", AT_W, 5160},    {"N_FloorTab_00", AT_X, -2400},   {"T_MapTitle_00", AT_X, -2362},
        {"N_ItemIcon_00", AT_X, 2326},   {"P_PicField_00", AT_X, 2260},    {"Pa_Guide_01", AT_X, 2456},
        {"P_Vignette_L_00", AT_X, -2700}, {"P_Vignette_R_00", AT_X, 2700}, {"P_MapBase_00", AT_X, -44},
        {"P_PicField_00", AT_SX, 0.75f}, {"P_Vignette_R_00", AT_W, 1200},
    };
    static const Want game_full[] = {
        {"P_MapBase_00", AT_W, 3928},    {"BG_00", AT_W, 3840},            {"N_ItemIcon_00", AT_X, 1666},
        {"P_Vignette_R_00", AT_X, 2040},
    };
    static const Want hud[] = {{"N_MiniMap_00", AT_X, 660}, {"N_MiniMapCap_00", AT_X, 2130}, {"N_VRS_00", AT_X, 2196}};
    static const Want game_hud[] = {{"N_MiniMap_00", AT_X, 0}, {"N_MiniMapCap_00", AT_X, 1470}, {"N_VRS_00", AT_X, 1536}};
    uint64_t mt = m.values_at[1][0], dg = m.values_at[2][0];
    // both groups: the minimap and its keys for the HUD, the pause and world maps for the menus
    uint8_t *p = edited(&m, HUD_FULL(660, 660), 27 + 11, report, sizeof report);
    check_fields(&m, p, full, COUNT(full));
    check_fields(&m, p, hud, COUNT(hud));
    CHECK(rf(p, mt + 24 + 8 * 4) == 2150 && rf(p, dg + 24 + 8 * 2) == 2196, "MapMenu_00 both: the minimap's keys");
    CHECK(!strncmp(report, "N_MiniMap_00 x 0 -> 660, ", 25) &&
              strstr(report, ", P_MapBase_00 width 3928 -> 5248, P_NotOpen_00 width 4080 -> 5400, BG_00 width 3840 "
                             "-> 5160, N_FloorTab_00 x -1740 -> -2400, ") &&
              strstr(report, "P_Overlay_00 width 3840 -> 5160, MapType N_MapIcon_00 x 2 keys 1490 -> 2150, "),
          "MapMenu_00 both report: %s", report);
    free(p);
    // the menus alone: the minimap's panes and the keys of the HUD's key operations as the game has them
    p = edited(&m, FULL(660), 11, report, sizeof report);
    check_fields(&m, p, full, COUNT(full));
    check_fields(&m, p, game_hud, COUNT(game_hud));
    int at_place = 0;
    for (int k = 0; k < 21; k++) at_place += rf(p, dg + 24 + 8 * k) == 1536;
    CHECK(rf(p, mt + 24 + 8 * 4) == 1490 && rf(p, mt + 24 + 8 * 5) == 1490 && at_place == 19 && rf(p, dg) == 1536 &&
              !strstr(report, "MapType") && !strstr(report, "N_MiniMap_00"),
          "MapMenu_00 menus alone: MapType %g, %d keys at 1536, report %s", (double)rf(p, mt + 24 + 8 * 4), at_place,
          report);
    free(p);
    // the HUD alone: the pause map as the game has it
    p = edited(&m, HUD(660), 27, NULL, 0);
    check_fields(&m, p, hud, COUNT(hud));
    check_fields(&m, p, game_full, COUNT(game_full));
    // the HUD's edit first, then both: the menus' operations find the game's values but the HUD's do not, and
    // lay_out takes a layout whole, so the file stays (wide_hud_archive keeps the menus' part of it, below)
    refuse(&m, p, HUD_FULL(660, 660), "MapMenu_00 moved for the HUD, then both", "MapMenu_00 N_MiniMap_00 x is 660, not 0");
    // a pause map's pane keyed by an animation: refused for the menus, the HUD's edit of the same layout goes ahead
    LayoutDef L = layouts[1];
    L.anims = map_vignette_keyed, L.nanims = COUNT(map_vignette_keyed);
    refused_layout_by(&L, FULL(660), "MapMenu_00 a vignette keyed", "MapMenu_00 P_Vignette_R_00 has its x keyed by VignetteIn");
    size_t n = make_layout(&L);
    char why[240] = "";
    CHECK(wide_hud_lay_out(file, n, L.member, HUD(660), NULL, 0, why, sizeof why) == 27,
          "MapMenu_00 a vignette keyed, the HUD's shift: %s", why);
    p = copy_of(&m);
    put(p, field(&m, "P_NotOpen_00", AT_W), 3840);
    refuse(&m, p, HUD_FULL(660, 660), "MapMenu_00 a width", "MapMenu_00 P_NotOpen_00 width is 3840, not 4080");
    free(m.p);
}

// wide_hud_archive on *b, a buffer of cap bytes the archive may grow in, of *n bytes before and after.
static int archive_grow(uint8_t **b, size_t *n, size_t cap, const WideHudShift *s, char *why, size_t why_n) {
    int r = wide_hud_archive(b, n, s, why, why_n);
    uint8_t *g = realloc(*b, cap > *n ? cap : *n);
    if (g) *b = g;
    return r;
}

// Copy c of the member named `name` in the archive p[0..n): its bytes (*size of them), NULL when there is none.
static uint8_t *member_sized(uint8_t *p, size_t n, const char *name, int c, size_t *size) {
    unsigned next = 0;
    size_t at;
    for (int k = 0; sarc_next(p, n, name, &next, &at, size) == 1; k++)
        if (k == c) return p + at;
    return NULL;
}
static uint8_t *member_of(uint8_t *p, size_t n, const char *name, int c) {
    size_t size;
    return member_sized(p, n, name, c, &size);
}

// MapMenu_00 in the archive p, and the offset of a field of one of its panes.
static uint8_t *map_of(uint8_t *p, size_t n) { return member_of(p, n, "MapMenu_00.buil", 0); }
static uint64_t map_at(const char *pane, int off) { return made_pane_at[1][pane_index(&layouts[1], pane)] + (uint64_t)off; }

// The archive of the four HUD layouts (made[]) and the four menus' layouts above, one copy each, member i from
// bytes[i] (NULL: as made): its size. Every other layout of the redesign is missing from it.
enum { NMENU_MEMBERS = 8 };
static Made menu_made[4];
static size_t menu_archive(uint8_t *out, size_t cap, const uint8_t *const *bytes) {
    const char *nm[NMENU_MEMBERS];
    const uint8_t *by[NMENU_MEMBERS];
    size_t sz[NMENU_MEMBERS];
    for (int i = 0; i < NMENU_MEMBERS; i++) {
        nm[i] = i < 4 ? layouts[i].member : menu_made[i - 4].L->member;
        by[i] = bytes && bytes[i] ? bytes[i] : i < 4 ? made[i] : menu_made[i - 4].p;
        sz[i] = i < 4 ? made_n[i] : menu_made[i - 4].n;
    }
    return make_sarc(out, cap, nm, by, sz, NMENU_MEMBERS);
}

static void check_menus_archive(void) {
    enum { CAP = 1 << 20 };
    static const LayoutDef *const L[4] = {&plus_menu, &menu_overlay, &slide_list, &item_menu};
    for (int i = 0; i < 4; i++) make(&menu_made[i], L[i]);
    uint8_t *a = malloc(CAP), *b = malloc(CAP), *keep = malloc(CAP);
    char why[240] = "";
    const uint8_t *by[NMENU_MEMBERS] = {0};
    // PlayerStatus 4, MapMenu 27 + 11, NumDisplay 2, LoadingFade 2, PlusMenu 85 and its 6 new panes, MenuOverlay 8,
    // the slide list 2, PaItemMenu 5
    enum { ALL = 4 + 38 + 2 + 2 + 85 + 6 + 8 + 2 + 5 };
    size_t n = menu_archive(a, CAP, NULL);
    memcpy(b, a, n);
    nlog = 0;
    int r = archive_grow(&b, &n, CAP, HUD_FULL(660, 660), why, sizeof why);
    int kept = 0, stays = 0;
    for (int i = 0; i < nlog; i++) {
        kept += strstr(log_lines[i], "keeps the game's") != NULL;
        stays += strstr(log_lines[i], "stays as the game lays it out") != NULL;
    }
    CHECK(r == ALL && !kept && stays == 12 && nlog == 12 + 8, "menus archive: %d fields, %d lines (%d kept, %d stay) (%s)",
          r, nlog, kept, stays, why);
    CHECK(log_find("wide hud: PlayerStatus_00 (1 copy, 4 fields): N_ScaleL_00 x -1920 -> -2580") &&
              log_find("wide hud and menus: MapMenu_00 (1 copy, 38 fields): N_MiniMap_00 x 0 -> 660, ") &&
              log_find("wide menus: LoadingFade_00 (1 copy, 2 fields): N_IconIn_00 x 1778 -> 1814.33, scale x 1 -> "
                       "0.744186") &&
              log_find("wide menus: PlusMenu_00 (1 copy, 85 fields, P_MenuBg_00 in 7 pieces, 6 new panes): Pa_Equip_00 x 4280 "
                       "-> 5600, ") &&
              log_find("wide menus: MenuOverlay_00 (1 copy, 8 fields): P_MenuHeader_00 width 3840 -> 5160") &&
              log_find("wide menus: PaShortCutSlideList_00 (1 copy, 2 fields): S_ScissorHT_00 width 3840 -> 5160") &&
              log_find("wide menus: PaItemMenu_00 (1 copy, 5 fields): N_ItemAll_00 x 0 -> -660") &&
              log_find("wide menus: the layout archive has no ChallengeMenu_01.buil; ChallengeMenu_01 stays as the game "
                       "lays it out"),
          "menus archive: the lines (last: %s)", last_log());
    CHECK(rf(member_of(b, n, "PlusMenu_00.buil", 0), field(&menu_made[0], "Pa_System_00", AT_X)) == 16800 &&
              rf(map_of(b, n), map_at("BG_00", AT_W)) == 5160,
          "menus archive: PlusMenu_00 or MapMenu_00 not edited");
    // again: nothing more
    memcpy(keep, b, n);
    CHECK(archive_grow(&b, &n, CAP, HUD_FULL(660, 660), why, sizeof why) == 0 && !memcmp(b, keep, n),
          "menus archive again");

    // a menus' operation of a layout the HUD edits too that does not fit: that layout keeps the game's menu layout,
    // its HUD's edit and every other layout go ahead
    uint8_t *f = malloc(made_n[1]);
    memcpy(f, made[1], made_n[1]);
    put(f, made_pane_at[1][pane_index(&layouts[1], "P_MapBase_00")] + AT_W, 3900);
    by[1] = f;
    n = menu_archive(b, CAP, by);
    nlog = 0;
    r = archive_grow(&b, &n, CAP, HUD_FULL(660, 660), why, sizeof why);
    CHECK(r == ALL - 11 &&
              log_find("wide menus: MapMenu_00 P_MapBase_00 width is 3900, not 3928; MapMenu_00 keeps the game's menu "
                       "layout (the HUD's edit of it goes ahead)") &&
              log_find("wide hud: MapMenu_00 (1 copy, 27 fields): N_MiniMap_00 x 0 -> 660") &&
              rf(map_of(b, n), map_at("N_MiniMap_00", AT_X)) == 660 &&
              rf(map_of(b, n), map_at("BG_00", AT_W)) == 3840 &&
              rf(map_of(b, n), map_at("P_MapBase_00", AT_W)) == 3900 &&
              rf(member_of(b, n, "PlusMenu_00.buil", 0), field(&menu_made[0], "Pa_System_00", AT_X)) == 16800,
          "menus archive, MapMenu_00's menus not fitting: %d (last: %s)", r, last_log());
    // MapMenu_00 moved for the HUD already, not for the menus: the HUD's part is found moved, the menus' part kept
    memcpy(f, made[1], made_n[1]);
    CHECK(wide_hud_lay_out(f, made_n[1], layouts[1].member, HUD(660), NULL, 0, why, sizeof why) == 27, "MapMenu_00 for "
          "the HUD");
    n = menu_archive(b, CAP, by);
    nlog = 0;
    r = archive_grow(&b, &n, CAP, HUD_FULL(660, 660), why, sizeof why);
    CHECK(r == ALL - 38 &&
              log_find("wide menus: MapMenu_00 N_MiniMap_00 x is 660, not 0; MapMenu_00 keeps the game's menu layout "
                       "(the HUD's edit of it goes ahead)") &&
              rf(map_of(b, n), map_at("BG_00", AT_W)) == 3840,
          "menus archive, MapMenu_00 moved for the HUD already: %d (last: %s)", r, last_log());
    by[1] = NULL;
    free(f);

    // a layout of the menus alone that does not fit: it keeps the game's, the line without the HUD's part
    f = copy_of(&menu_made[0]);
    put(f, key_at(&menu_made[0], 1, 0, 1, 0), -7301);
    by[4] = f;
    n = menu_archive(b, CAP, by);
    nlog = 0;
    r = archive_grow(&b, &n, CAP, HUD_FULL(660, 660), why, sizeof why);
    const char *line = log_find("wide menus: PlusMenu_00 EquipToItem");
    CHECK(r == ALL - 85 - 6 && line &&
              !strcmp(line, "wide menus: PlusMenu_00 EquipToItem N_MenuChange_00 x has 0 keys at -7300, not 1 of 3; "
                            "PlusMenu_00 keeps the game's menu layout") &&
              !memcmp(member_of(b, n, "PlusMenu_00.buil", 0), f, menu_made[0].n) && !log_find("wide menus: PlusMenu_00 (") &&
              log_find("wide hud and menus: MapMenu_00 (1 copy, 38 fields)"),
          "menus archive, PlusMenu_00 not fitting: %d (line: %s)", r, line ? line : "none");
    by[4] = NULL;
    free(f);

    // a HUD's layout that does not fit: the whole archive as it is
    f = malloc(made_n[0]);
    memcpy(f, made[0], made_n[0]);
    put(f, made_pane_at[0][pane_index(&layouts[0], "N_ScaleL_00")] + AT_X, -1900);
    by[0] = f;
    n = menu_archive(b, CAP, by);
    memcpy(keep, b, n);
    r = archive_grow(&b, &n, CAP, HUD_FULL(660, 660), why, sizeof why);
    CHECK(r == -1 && !memcmp(b, keep, n) && strstr(why, "PlayerStatus_00 N_ScaleL_00 x is -1900, not -1920"),
          "menus archive, PlayerStatus_00 not fitting: %d (%s)", r, why);
    by[0] = NULL;
    free(f);
    // the HUD's part of MapMenu_00 not fitting: the whole archive as it is too. The menus' line written on the way
    // says the HUD's edit goes ahead, which it does not (wide_hud.c wide_hud_archive logs before it knows).
    f = malloc(made_n[1]);
    memcpy(f, made[1], made_n[1]);
    put(f, made_pane_at[1][pane_index(&layouts[1], "N_MiniMapCap_00")] + AT_X, 1400);
    by[1] = f;
    n = menu_archive(b, CAP, by);
    memcpy(keep, b, n);
    nlog = 0;
    r = archive_grow(&b, &n, CAP, HUD_FULL(660, 660), why, sizeof why);
    // the HUD's part fails too: the archive stays as it is, and no line says the HUD's edit goes ahead
    CHECK(r == -1 && !memcmp(b, keep, n) && strstr(why, "MapMenu_00 N_MiniMapCap_00 x is 1400, not 1470") &&
              !log_find("wide menus: MapMenu_00 N_MiniMapCap_00 x is 1400, not 1470; MapMenu_00 keeps"),
          "menus archive, MapMenu_00's HUD not fitting: %d (%s; last: %s)", r, why, last_log());
    by[1] = NULL;
    free(f);
    // the menus alone over an archive without the HUD's layouts: the menus' layouts there edited
    {
        const char *nm[] = {menu_made[0].L->member, menu_made[2].L->member};
        const uint8_t *bb[] = {menu_made[0].p, menu_made[2].p};
        size_t sz[] = {menu_made[0].n, menu_made[2].n};
        n = make_sarc(b, CAP, nm, bb, sz, 2);
        nlog = 0;
        r = archive_grow(&b, &n, CAP, FULL(660), why, sizeof why);
        CHECK(r == 85 + 6 + 2 && log_find("wide menus: the layout archive has no MapMenu_00.buil; MapMenu_00 stays as the "
                                      "game lays it out"),
              "menus alone without the HUD's layouts: %d (%s)", r, why);
    }
    for (int i = 0; i < 4; i++) free(menu_made[i].p);
    free(keep);
    free(b);
    free(a);
}

// An entry's range in the archive p (entry i of make_sarc's SFAT, the other member first): its start and end.
static void entry_range(const uint8_t *p, int i, size_t *lo, size_t *hi) {
    *lo = r32(p, 12) + (size_t)r32(p, 0x20 + 16 * (size_t)i + 8);
    *hi = r32(p, 12) + (size_t)r32(p, 0x20 + 16 * (size_t)i + 12);
}

// The split in an archive: four copies of PlusMenu_00 and one of MenuOverlay_00 (after make_sarc's other member).
// The archive is laid out again around the grown copies, each in a range of its own at its old alignment; what the
// split refuses, or an archive that may not grow, leaves PlusMenu_00 with the game's menu layout and the archive its
// size.
static void check_split_archive(void) {
    enum { CAP = 1 << 20, COPIES = 4, MEMBERS = COPIES + 1 };
    Made plus, overlay;
    make(&plus, &plus_menu);
    make(&overlay, &menu_overlay);
    const char *nm[MEMBERS];
    const uint8_t *by[MEMBERS];
    size_t sz[MEMBERS];
    for (int i = 0; i < MEMBERS; i++) {
        nm[i] = i < COPIES ? plus.L->member : overlay.L->member;
        by[i] = i < COPIES ? plus.p : overlay.p;
        sz[i] = i < COPIES ? plus.n : overlay.n;
    }
    uint8_t *a = malloc(CAP), *b = malloc(CAP), *keep = malloc(CAP);
    size_t n = make_sarc(a, CAP, nm, by, sz, MEMBERS), bn = n, was[MEMBERS + 1][2];
    for (int i = 0; i <= MEMBERS; i++) entry_range(a, i, &was[i][0], &was[i][1]);
    // what each copy becomes: the game's file edited in place, then split; MenuOverlay_00 edited in place
    char why[300] = "";
    uint8_t *e = copy_of(&plus), *want = NULL, *ov = copy_of(&overlay);
    size_t want_n = 0;
    CHECK(wide_hud_lay_out(e, plus.n, plus.L->member, FULL(660), NULL, 0, why, sizeof why) == 85 &&
              wide_hud_split(e, plus.n, plus.L->member, FULL(660), &want, &want_n, NULL, 0, why, sizeof why) == 6 &&
              wide_hud_lay_out(ov, overlay.n, overlay.L->member, FULL(660), NULL, 0, why, sizeof why) == 8,
          "split archive: the expected copies (%s)", why);
    free(e);
    memcpy(b, a, n);
    nlog = 0;
    int r = archive_grow(&b, &bn, CAP, FULL(660), why, sizeof why);
    CHECK(r == COPIES * (85 + 6) + 8 && bn > n && r32(b, 8) == bn && sarc_count(b, bn) == MEMBERS + 1,
          "split archive: %d, %zu -> %zu bytes (%s)", r, n, bn, why);
    CHECK(log_find("wide menus: PlusMenu_00 (4 copies, 340 fields, P_MenuBg_00 in 7 pieces, 24 new panes): Pa_Equip_00 x "
                   "4280 -> 5600, Pa_Item_00 x 8560 -> 11200, "),
          "split archive: its PlusMenu_00 line (last: %s)", last_log());
    // every entry's bytes, in a range of its own after the one before, at its old start's alignment
    size_t prev = 0;
    int bad = 0;
    for (int i = 0; i <= MEMBERS && r > 0; i++) {
        size_t lo, hi, align = was[i][0] & (~was[i][0] + 1);
        entry_range(b, i, &lo, &hi);
        const uint8_t *expect = !i ? a + was[0][0] : i <= COPIES ? want : ov;
        size_t expect_n = !i ? was[0][1] - was[0][0] : i <= COPIES ? want_n : overlay.n;
        if (lo < prev || hi - lo != expect_n || memcmp(b + lo, expect, expect_n) || lo % (align < 0x2000 ? align : 0x2000)) {
            printf("split archive: entry %d at %zu..%zu (was %zu..%zu)\n", i, lo, hi, was[i][0], was[i][1]);
            bad++;
        }
        prev = hi;
    }
    CHECK(!bad && r32(b, 12) == r32(a, 12), "split archive: %d entries not where or what they should be", bad);
    CHECK(!memcmp(b + 0x14, a + 0x14, 12) && !memcmp(b + 0x20 + 16 * (MEMBERS + 1), a + 0x20 + 16 * (MEMBERS + 1),
                                                     r32(a, 12) - (0x20 + 16 * (MEMBERS + 1))),
          "split archive: SFAT's header or SFNT changed");
    // again: nothing more
    size_t again = bn;
    memcpy(keep, b, bn);
    CHECK(archive_grow(&b, &again, CAP, FULL(660), why, sizeof why) == 0 && again == bn && !memcmp(b, keep, bn),
          "split archive again: %zu bytes (%s)", again, why);
    // the archive as served: the frame records the grown size, the line says so
    {
        uint8_t *dict = malloc(1 << 16), *zdic = malloc(1 << 17), *z = malloc(CAP);
        size_t dn = make_dict(dict, 1 << 16, 0x1234, a, n), zdn = make_zsdic(zdic, 1 << 17, dict, dn);
        ZSTD_CCtx *cc = ZSTD_createCCtx();
        size_t zn = ZSTD_compress_usingDict(cc, z, CAP, a, n, dict, dn, 3);
        ZSTD_freeCCtx(cc);
        void *out = NULL;
        size_t out_n = 0;
        char line[200];
        nlog = 0;
        r = wide_hud_build(z, zn, zdic, zdn, FULL(660), &out, &out_n);
        snprintf(line, sizeof line, "wide hud: the layout archive edited (the menus over the whole picture, 660 units "
                                    "wider on each side), 348 fields and 24 new panes, the archive %zu -> %zu bytes", n, bn);
        CHECK(r == 1 && ZSTD_getFrameContentSize(out, out_n) == bn && log_find(line), "split archive built: %d (%s)", r,
              last_log());
        free(out);
        free(z);
        free(zdic);
        free(dict);
    }
    // two entries of one range: each split copy in a range of its own
    memcpy(b, a, n);
    set_range(b, 1, member_at[0], plus.n);
    bn = n;
    r = archive_grow(&b, &bn, CAP, FULL(660), why, sizeof why);
    size_t lo0, hi0, lo1, hi1;
    entry_range(b, 1, &lo0, &hi0), entry_range(b, 2, &lo1, &hi1);
    CHECK(r == 3 * 85 + COPIES * 6 + 8 && lo1 >= hi0 && hi0 - lo0 == want_n && hi1 - lo1 == want_n &&
              !memcmp(b + lo0, want, want_n) && !memcmp(b + lo1, want, want_n),
          "split archive with two entries of one range: %d (%s)", r, why);
    // one copy the split refuses (two texture maps): PlusMenu_00 keeps the game's menu layout, the archive its size
    memcpy(b, a, n);
    memcpy(b + member_at[2] + entry_rec(plus.p, 0x138, 1) + 8, &(uint32_t){2}, 4);
    memcpy(keep, b, n);
    bn = n;
    nlog = 0;
    r = archive_grow(&b, &bn, CAP, FULL(660), why, sizeof why);
    CHECK(r == 8 && bn == n && !memcmp(b + member_at[0], keep + member_at[0], member_at[4] - member_at[0]) &&
              log_find("wide menus: PlusMenu_00 P_MenuBg_00 is not a plain picture with one texture map; PlusMenu_00 "
                       "keeps the game's menu layout") &&
              log_find("wide menus: MenuOverlay_00 (1 copy, 8 fields)"),
          "split archive, one copy refused: %d, %zu bytes (last: %s)", r, bn, last_log());
    // an archive that may not grow (no room in the resource size table): the same
    memcpy(b, a, n);
    bn = n;
    nlog = 0;
    r = archive_grow(&b, &bn, CAP, &(WideHudShift){0, 1, 660, 1}, why, sizeof why);
    CHECK(r == 8 && bn == n && !memcmp(b + member_at[0], a + member_at[0], member_at[4] - member_at[0]) &&
              log_find("wide menus: the layout archive may not grow for P_MenuBg_00's pieces; PlusMenu_00 keeps the "
                       "game's menu layout"),
          "split archive that may not grow: %d, %zu bytes (last: %s)", r, bn, last_log());
    // the rebuild alone: nothing replaced gives the archive back; ranges that overlap, or a damaged archive, nothing
    const uint8_t *none[MEMBERS + 1] = {0};
    size_t none_n[MEMBERS + 1] = {0}, rn = 0;
    uint8_t *same = sarc_rebuild(a, n, none, none_n, &rn);
    CHECK(same && rn == n && !memcmp(same, a, n), "sarc_rebuild of nothing: %zu bytes", rn);
    free(same);
    memcpy(b, a, n);
    set_range(b, 1, member_at[0] + 0x10, plus.n);
    CHECK(!sarc_rebuild(b, n, none, none_n, &rn) && !sarc_rebuild(a, 40, none, none_n, &rn),
          "sarc_rebuild of ranges that overlap or of a damaged archive");
    free(want);
    free(ov);
    free(keep);
    free(b);
    free(a);
    free(plus.p);
    free(overlay.p);
}

// ---- the resource size table's entry raised for a larger archive (restbl.c restbl_fit, romfs.c restbl_room)
static uint32_t crc32_of(const char *s) {
    uint32_t c = ~0u;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void check_restbl(void) {
    static const char key[] = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc", other[] = "Other/Thing.byml";
    uint8_t raw[22 + 16];
    uint32_t h[2] = {crc32_of(key), crc32_of(other)}, size[2] = {9931664, 100};
    int first = h[0] > h[1];   // the entries sorted by their hash
    memcpy(raw, "RESTBL", 6);
    memcpy(raw + 6, &(uint32_t){1}, 4), memcpy(raw + 10, &(uint32_t){160}, 4);
    memcpy(raw + 14, &(uint32_t){2}, 4), memcpy(raw + 18, &(uint32_t){0}, 4);
    for (int i = 0; i < 2; i++) {
        int k = i ^ first;
        memcpy(raw + 22 + 8 * i, &h[k], 4), memcpy(raw + 26 + 8 * i, &size[k], 4);
    }
    CHECK(h[0] == 0xf79a2c8d, "the archive's key hashes to %#x", h[0]);
    Restbl *t = restbl_parse(raw, sizeof raw);
    uint32_t from = 0, to = 0, got = 0;
    // the game's entry, 4175 bytes over the archive's 9927489, raised by the split's growth: the room kept
    CHECK(t && restbl_fit(t, key, 9927489, 9945921, &from, &to) && from == 9931664 && to == 9950112 &&
              restbl_get(t, key, &got) && got == to && to - 9945921 >= 4175,
          "restbl_fit: %u -> %u", from, to);
    // a mod's merged entry above the game's stays above it
    restbl_raise(t, key, 12000000);
    CHECK(restbl_fit(t, key, 9927489, 9945921, &from, &to) && from == 12000000 && to == 12018432,
          "restbl_fit over a higher entry: %u -> %u", from, to);
    // no entry: the estimate for its type; a type the table never lists: nothing
    CHECK(restbl_fit(t, "UI/Layout2Archive/New.blarc", 0, 100000, &from, &to) && from == 0 && to == 110592 &&
              !restbl_fit(t, "Sound/Thing.bwav", 10, 20, &from, &to),
          "restbl_fit of a new key: %u -> %u", from, to);
    restbl_free(t);
}

static void check_menus(void) {
    check_menu_counts();
    check_plus_menu();
    check_split();
    check_menu_overlay();
    check_slide_list();
    check_item_menu();
    check_loading_pin();
    check_map_full();
    check_menus_archive();
    check_split_archive();
    check_restbl();
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

// The 4-byte words that differ between the members of archive a and those of b (the same entries, b laid out again
// after a member grew), and how many entries differ outside the members named `names` or change size other than
// PlusMenu_00.buil.
static size_t members_changed(const uint8_t *a, size_t an, const uint8_t *b, size_t bn, const char *const *names,
                              int nnames, size_t *outside) {
    size_t changed = 0;
    int count = sarc_count(a, an);
    *outside = count != sarc_count(b, bn);
    unsigned na = 0, nb = 0;
    uint16_t head;
    memcpy(&head, a + 4, 2);
    size_t sfnt = (size_t)head + 12 + 16 * (size_t)(count > 0 ? count : 0) + 8;
    for (int i = 0; i < count && !*outside; i++) {
        size_t aat, asz, bat, bsz;
        if (sarc_next(a, an, NULL, &na, &aat, &asz) != 1 || sarc_next(b, bn, NULL, &nb, &bat, &bsz) != 1) {
            (*outside)++;
            break;
        }
        const char *name = (const char *)a + sfnt + (r32(a, (size_t)head + 12 + 16 * (size_t)i + 4) & 0xffffff) * 4;
        int inside = 0;
        for (int k = 0; k < nnames; k++) inside |= !strcmp(name, names[k]);
        if (asz != bsz) {
            *outside += strcmp(name, "PlusMenu_00.buil") != 0;
            continue;
        }
        size_t words = 0;
        for (size_t j = 0; j + 4 <= asz; j += 4) words += memcmp(a + aat + j, b + bat + j, 4) != 0;
        changed += words;
        *outside += words && !inside;
    }
    return changed;
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    long size = f && !fseek(f, 0, SEEK_END) ? ftell(f) : -1;
    uint8_t *p = size > 0 ? malloc((size_t)size) : NULL;
    int ok = p && !fseek(f, 0, SEEK_SET) && fread(p, 1, (size_t)size, f) == (size_t)size;
    if (f) fclose(f);
    if (!ok) free(p);
    *n = ok ? (size_t)size : 0;
    return ok ? p : NULL;
}

// The float at offset `off` of each copy of member `name` in the archive p: equal to v in all of them.
static int each_copy(const uint8_t *p, size_t n, const char *name, size_t off, float v) {
    unsigned next = 0;
    size_t at, size;
    int copies = 0, ok = 1;
    while (sarc_next(p, n, name, &next, &at, &size) == 1) copies++, ok &= off + 4 <= size && rf(p, at + off) == v;
    return ok && copies == 4;
}

// argv: romfs.bin; the offset and size of the layout archive, of Pack/ZsDic.pack.zs and of the resource size table in
// it; then (argc 10) the files tools/layout_dump.py split_picture made of the archive's first PlusMenu_00.buil at u 640
// and 1920 (tests/wide_hud_check.py).
static void game_archive(int argc, char **argv) {
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
        int made_z = wide_hud_build(a, an, d, dn, HUD(out), &z, &zn);
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
    // the menus' shift with the HUD's (a 3440x1440 picture): the loading icon's x and scale x in its four copies too
    {
        float out = wide_hud_out(3440, 1440), q = (float)(16.0 / 9.0) / ((float)3440 / 1440);
        static const char *const with_icon[] = {"PlayerStatus_00.buil", "MapMenu_00.buil", "NumDisplay_00.buil",
                                                "LoadingFade_00.buil"};
        void *z = NULL;
        size_t zn = 0, on = 0, outside = 0;
        nlog = 0;
        int made_z = wide_hud_build(a, an, d, dn, BOTH(out, q), &z, &zn);
        uint8_t *got = made_z == 1 ? unpack(z, zn, dics, pn, &on) : NULL;
        CHECK(got && on == rn, "game with the menus' shift: build %d (%s)", made_z, last_log());
        if (got && on == rn) {
            size_t changed = words_changed(raw, got, rn, with_icon, 4, &outside);
            CHECK(changed == 4 * (33 + 2) && !outside && each_copy(got, rn, "LoadingFade_00.buil", 0x548, 1778 * q) &&
                      each_copy(got, rn, "LoadingFade_00.buil", 0x53c, 1 * q),
                  "game with the menus' shift: %zu words changed, %zu outside", changed, outside);
            const char *line = log_find("widescreen: menus: LoadingFade_00");
            printf("game 3440x1440 with menus = center: %s\n", line ? line : "(no line)");
            CHECK(line != NULL, "game with the menus' shift: no LoadingFade_00 line");
        }
        free(got);
        free(z);
    }
    // the menus over the whole picture with the HUD's edges (21:9, 32:9 and a 3440x1440 window): every layout of the
    // redesign takes its edit (no "keeps the game's" line), nothing outside them changes, and a second pass on the
    // edited archive finds nothing to do
    static const char *const redesign[] = {
        "PlayerStatus_00.buil", "MapMenu_00.buil", "NumDisplay_00.buil", "LoadingFade_00.buil", "MenuOverlay_00.buil",
        "PlusMenu_00.buil", "PaCollectMenu_00.buil", "PaEquipmentMenu_00.buil", "PaItemMenu_00.buil",
        "PaSystemMenu_00.buil", "ImportantMenu_01.buil", "ChallengeMenu_01.buil", "ShortCut_01.buil",
        "PaShortCutSlideList_00.buil", "SystemOverlay_00.buil", "SystemOption_00.buil", "SystemActionGuide_00.buil",
        "GameOver_00.buil", "MessageLog_01.buil", "StaffRollEnd_00.buil"};
    static const struct { unsigned w, h; } wide[] = {{2560, 1080}, {5120, 1440}, {3440, 1440}};
    size_t full_n = 0;   // the archive's size at 21:9
    for (size_t s = 0; s < sizeof wide / sizeof *wide; s++) {
        float out = wide_hud_out(wide[s].w, wide[s].h);
        WideHudShift full = {out, 1, out, 0};
        void *z = NULL;
        size_t zn = 0, on = 0, outside = 0;
        nlog = 0;
        int made_z = wide_hud_build(a, an, d, dn, &full, &z, &zn);
        uint8_t *got = made_z == 1 ? unpack(z, zn, dics, pn, &on) : NULL;
        CHECK(got && on > rn, "game %ux%u with the menus full: build %d (%s)", wide[s].w, wide[s].h, made_z, last_log());
        if (!s) full_n = on;
        int kept = 0, menus = 0;
        for (int i = 0; i < nlog && i < LOG_LINES; i++) {
            kept += strstr(log_lines[i], "keeps the game's") != NULL || strstr(log_lines[i], "stays as the game") != NULL;
            menus += !strncmp(log_lines[i], "wide menus: ", 12) || !strncmp(log_lines[i], "wide hud and menus: ", 20);
            if (s == 0) printf("game %ux%u with the menus full: %s\n", wide[s].w, wide[s].h, log_lines[i]);
        }
        CHECK(!kept && menus == 18, "game %ux%u with the menus full: %d layouts keep the game's, %d edited", wide[s].w,
              wide[s].h, kept, menus);
        if (got && on > rn) {
            size_t changed = members_changed(raw, rn, got, on, redesign, sizeof redesign / sizeof *redesign, &outside);
            CHECK(changed > 4 * 33 && !outside, "game %ux%u with the menus full: %zu words changed, %zu outside",
                  wide[s].w, wide[s].h, changed, outside);
            char why[240] = "";
            CHECK(archive_same(got, on, &full, why, sizeof why) == 0,
                  "game %ux%u with the menus full: a second pass changes something", wide[s].w, wide[s].h);
            // each copy of PlusMenu_00: the game's edited in place, then split
            int bad = 0;
            for (int c = 0; c < 4; c++) {
                size_t size, got_n, sn = 0;
                uint8_t *game = member_sized(raw, rn, "PlusMenu_00.buil", c, &size), *mine = malloc(size), *split = NULL;
                const uint8_t *served = member_sized(got, on, "PlusMenu_00.buil", c, &got_n);
                memcpy(mine, game, size);
                bad += wide_hud_lay_out(mine, size, "PlusMenu_00.buil", &full, NULL, 0, why, sizeof why) != 85 ||
                       wide_hud_split(mine, size, "PlusMenu_00.buil", &full, &split, &sn, NULL, 0, why, sizeof why) != 6 ||
                       !served || got_n != sn || memcmp(served, split, sn);
                free(split);
                free(mine);
            }
            CHECK(!bad, "game %ux%u with the menus full: %d copies of PlusMenu_00 not edited and split", wide[s].w,
                  wide[s].h, bad);
        }
        free(got);
        free(z);
    }
    // after the action prompt's edit of the same archive (romfs.c serves it first): both edits, nothing else
    void *ap = NULL, *z = NULL;
    size_t apn = 0, zn = 0, on = 0, outside = 0;
    int r = action_prompt_build(a, an, d, dn, 1, &ap, &apn);
    CHECK(r == 1, "game: the action prompt's build %d", r);
    r = r == 1 ? wide_hud_build(ap, apn, d, dn, HUD(660), &z, &zn) : -1;
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
    // the split of the game's PlusMenu_00 at 21:9 and 32:9: byte for byte what tools/layout_dump.py split_picture makes
    if (argc >= 10) {
        size_t size, en = 0, sn = 0;
        const uint8_t *member = member_sized(raw, rn, "PlusMenu_00.buil", 0, &size);
        for (int k = 0; k < 2; k++) {
            float u = k ? 1920 : 640;
            uint8_t *expect = read_file(argv[8 + k], &en), *split = NULL;
            char why[300] = "";
            int made_split = member ? wide_hud_split(member, size, "PlusMenu_00.buil", FULL(u), &split, &sn, NULL, 0,
                                                     why, sizeof why)
                                    : -1;
            int same = expect && split && sn == en && !memcmp(split, expect, en);
            CHECK(made_split == 6 && same, "game: PlusMenu_00 split at u %g: %d, %zu bytes, %s the reference's %zu (%s)",
                  (double)u, made_split, sn, same ? "as" : "not", en, why);
            if (same) printf("game: PlusMenu_00 split at u %g: %zu -> %zu bytes, byte for byte tools/layout_dump.py's\n",
                             (double)u, size, sn);
            free(split);
            free(expect);
        }
    }
    // the resource size table's entry for the archive: the game's leaves 4175 bytes of room; raised for the archive at
    // 21:9 it keeps at least that room
    {
        size_t tn = 0, tr = 0;
        uint8_t *tz = read_part(argv[1], argv[6], argv[7], &tn), *table = tz ? unpack(tz, tn, dics, pn, &tr) : NULL;
        Restbl *t = table ? restbl_parse(table, tr) : NULL;
        static const char key[] = "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc";
        uint32_t entry = 0, from = 0, to = 0;
        CHECK(t && restbl_get(t, key, &entry) && entry == 9931664 && rn == 9927489,
              "game: the resource size table's entry %u for the archive of %zu bytes", entry, rn);
        CHECK(t && full_n > rn && restbl_fit(t, key, rn, full_n, &from, &to) && from == entry && to >= full_n &&
                  to - full_n >= entry - rn,
              "game: the entry raised %u -> %u for %zu -> %zu bytes", from, to, rn, full_n);
        printf("game: resource size table: %s raised %u -> %u for the file's %zu -> %zu bytes\n", key, from, to, rn,
               full_n);
        restbl_free(t);
        free(table);
        free(tz);
    }
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
    check_menus();
    if (argc >= 8) game_archive(argc, argv);
    for (int i = 0; i < NLAYOUTS; i++) free(made[i]);
    if (fails) {
        printf("wide hud: %d check(s) failed\n", fails);
        return 1;
    }
    printf("wide hud: all checks passed%s\n", argc >= 8 ? " (and the game's archive)" : "");
    return 0;
}
