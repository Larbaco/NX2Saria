#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "action_prompt.h"
#include "host.h"
#include "platform/platform.h"
#include "sarc.h"

#define MEMBER "DoCommand_00.buil"
#define MAX_RAW (64u << 20)
enum { PANE_TABLE = 0x118, MATERIAL_TABLE = 0x138, BLOCKS = 0x228, BLOCK_COUNT = 0x230 };
enum { PANE_SCALE_X = 0x10, PANE_SCALE_Y = 0x14, PANE_X = 0x1c, PANE_Y = 0x20, PANE_FLAGS = 0x38, PANE_NEEDS = 0x3c };
enum { MATERIAL_BLOCK = 0x14, MATERIAL_NEEDS = 0x18, BINDING_SIZE = 16, BINDING_NAME = 0x08, TEXT_RIGHT = 0x1000 };

enum { EAST, SOUTH, NORTH, WEST };
enum { F_X, F_Y, F_SCALE_X, F_SCALE_Y, F_RIGHT, F_CROSS };
static const unsigned field_at[] = {
    [F_X] = PANE_X, [F_Y] = PANE_Y, [F_SCALE_X] = PANE_SCALE_X, [F_SCALE_Y] = PANE_SCALE_Y, [F_RIGHT] = PANE_FLAGS};
static const char *const field_name[] = {"x", "y", "scale x", "scale y", "text origin", "cross picture"};

static const char buttons[4] = {'A', 'B', 'X', 'Y'};
static const int direction[2][4] = {{EAST, SOUTH, NORTH, WEST},
                                    {SOUTH, EAST, WEST, NORTH}};
static const float place[4][2] = {[EAST] = {80, 0}, [SOUTH] = {0, -80}, [NORTH] = {0, 80}, [WEST] = {-80, 0}};
static const struct { int picture; float sx, sy; } cross[4] = {
    [EAST] = {0, 1, 1}, [SOUTH] = {1, 1, 1}, [NORTH] = {1, 1, -1}, [WEST] = {0, -1, 1}};
static const struct { const char *pane; int button; } at_button[] = {
    {"P_B_00", 1},    {"P_X_00", 2},    {"P_Y_00", 3},    {"P_A_00", 0},    {"P_X_02", 2},    {"P_Y_02", 3},
    {"P_A_02", 0},    {"P_B_03", 1},    {"P_Y_04", 3},    {"P_A_01", 0},    {"P_B_02", 1},    {"P_X_03", 2},
    {"N_BtnA_00", 0}, {"N_BtnB_00", 1}, {"N_BtnX_00", 2}, {"N_BtnY_00", 3}};
enum { AT_BUTTON = sizeof at_button / sizeof *at_button };

typedef struct {
    char pane[24];
    int field;
    float value;
} Field;
enum { MAX_FIELDS = 2 * AT_BUTTON + 4 * 3 + 2 * 6 };

static int put(Field *f, int k, const char *pane, int field, float value) {
    if (k >= MAX_FIELDS) return k + 1;
    snprintf(f[k].pane, sizeof f[k].pane, "%s", pane);
    f[k].field = field, f[k].value = value;
    return k + 1;
}

static int diamond(int label, Field *f) {
    int k = 0;
    char name[24];
    for (size_t i = 0; i < AT_BUTTON; i++) {
        const float *at = place[direction[label][at_button[i].button]];
        k = put(f, k, at_button[i].pane, F_X, at[0]);
        k = put(f, k, at_button[i].pane, F_Y, at[1]);
    }
    for (int b = 0; b < 4; b++) {
        int d = direction[label][b];
        snprintf(name, sizeof name, "P_Base%c_00", buttons[b]);
        k = put(f, k, name, F_CROSS, (float)cross[d].picture);
        k = put(f, k, name, F_SCALE_X, cross[d].sx);
        k = put(f, k, name, F_SCALE_Y, cross[d].sy);
        if (b < 2) continue;
        int left = d == WEST;
        snprintf(name, sizeof name, "W_Base%c_00", buttons[b]);
        k = put(f, k, name, F_SCALE_X, left ? -1.0f : 1.0f);
        k = put(f, k, name, F_X, left ? 80.0f : -80.0f);
        snprintf(name, sizeof name, "W_Base%cDecide_00", buttons[b]);
        k = put(f, k, name, F_SCALE_X, left ? -1.0f : 1.0f);
        k = put(f, k, name, F_X, left ? 80.0f : -80.0f);
        snprintf(name, sizeof name, "T_Guide%c_00", buttons[b]);
        k = put(f, k, name, F_X, left ? -68.0f : 64.0f);
        k = put(f, k, name, F_RIGHT, (float)left);
    }
    return k;
}

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

static const char *str_at(const uint8_t *p, size_t n, uint64_t off) {
    return off < n && memchr(p + off, 0, n - off) ? (const char *)p + off : NULL;
}

static uint64_t table_find(const uint8_t *p, size_t n, size_t slot, const char *name, size_t size) {
    uint64_t names, records;
    uint32_t count;
    if (!u64_at(p, n, slot, &names) || !u64_at(p, n, slot + 8, &records) || !u32_at(p, n, slot + 24, &count) ||
        names > n || records > n || (n - names) / 8 < count || (n - records) / 8 < count)
        return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t s, r;
        const char *t;
        if (!u64_at(p, n, names + 8 * (uint64_t)i, &s) || !(t = str_at(p, n, s)) || strcmp(t, name)) continue;
        return u64_at(p, n, records + 8 * (uint64_t)i, &r) && r && r <= n && n - r >= size ? r : 0;
    }
    return 0;
}

static int cross_names(const uint8_t *p, size_t n, const char *pane, uint64_t at[2]) {
    uint64_t m = table_find(p, n, MATERIAL_TABLE, pane, MATERIAL_NEEDS), blocks, block, bindings;
    uint32_t index, count, bound;
    if (!m || !u32_at(p, n, m + MATERIAL_BLOCK, &index) || !u64_at(p, n, BLOCKS, &blocks) ||
        !u32_at(p, n, BLOCK_COUNT, &count) || index >= count || !u64_at(p, n, blocks + 8 * (uint64_t)index, &block) ||
        !u64_at(p, n, block, &bindings) || !u32_at(p, n, block + 8, &bound) || bound < 2)
        return 0;
    for (int k = 0; k < 2; k++) {
        uint64_t b, s;
        if (!u64_at(p, n, bindings + 8 * (uint64_t)k, &b) || b > n || n - b < BINDING_SIZE ||
            !u64_at(p, n, b + BINDING_NAME, &s))
            return 0;
        at[k] = b + BINDING_NAME;
    }
    return 1;
}

static int cross_picture(const uint8_t *p, size_t n, const uint64_t at[2]) {
    static const char *const names[2][2] = {{"DoCommandCrossBase_A_00", "DoCommandCrossBaseGrd_A_00"},
                                            {"DoCommandCrossBase_B_00", "DoCommandCrossBaseGrd_B_00"}};
    uint64_t s[2];
    const char *t[2];
    for (int k = 0; k < 2; k++)
        if (!u64_at(p, n, at[k], &s[k]) || !(t[k] = str_at(p, n, s[k]))) return -1;
    for (int c = 0; c < 2; c++)
        if (!strcmp(t[0], names[c][0]) && !strcmp(t[1], names[c][1])) return c;
    return -1;
}

typedef struct {
    uint64_t at[2];
} Where;

static int field_find(const uint8_t *p, size_t n, const Field *f, Where *w) {
    if (f->field == F_CROSS) return cross_names(p, n, f->pane, w->at);
    uint64_t r = table_find(p, n, PANE_TABLE, f->pane, PANE_NEEDS);
    w->at[0] = r + field_at[f->field], w->at[1] = 0;
    return r != 0;
}

static int field_get(const uint8_t *p, size_t n, const Field *f, const Where *w, float *v) {
    if (f->field == F_CROSS) {
        int c = cross_picture(p, n, w->at);
        *v = (float)c;
        return c >= 0;
    }
    if (f->field == F_RIGHT) {
        uint32_t flags;
        memcpy(&flags, p + w->at[0], 4);
        *v = (flags & TEXT_RIGHT) ? 1.0f : 0.0f;
        return 1;
    }
    memcpy(v, p + w->at[0], 4);
    return 1;
}

static int same(float a, float b) { return fabsf(a - b) < 1e-3f; }

int action_prompt_lay_out(uint8_t *p, size_t n, int label, char *why, size_t why_n) {
    label = !!label;
    Field from[MAX_FIELDS], to[MAX_FIELDS];
    int count = diamond(!label, from);
    if (count != MAX_FIELDS || diamond(label, to) != MAX_FIELDS) {
        snprintf(why, why_n, "the diamond has %d fields, not %d", count, MAX_FIELDS);
        return -1;
    }
    if (n < 4 || memcmp(p, "BFFH", 4)) {
        snprintf(why, why_n, "%s is not a BFFH layout", MEMBER);
        return -1;
    }
    Where where[MAX_FIELDS];
    float now[MAX_FIELDS];
    int done = 1;
    for (int k = 0; k < count; k++) {
        if (!field_find(p, n, &from[k], &where[k]) || !field_get(p, n, &from[k], &where[k], &now[k])) {
            snprintf(why, why_n, "%s has no %s %s", MEMBER, from[k].pane, field_name[from[k].field]);
            return -1;
        }
        done &= same(now[k], to[k].value);
    }
    if (done) return 0;
    for (int k = 0; k < count; k++)
        if (!same(now[k], from[k].value)) {
            snprintf(why, why_n, "%s %s %s is %g, not %g", MEMBER, from[k].pane, field_name[from[k].field],
                     (double)now[k], (double)from[k].value);
            return -1;
        }
    uint64_t names[2][2];
    for (int k = 0; k < count; k++)
        if (from[k].field == F_CROSS)
            for (int j = 0; j < 2; j++) memcpy(&names[(int)now[k]][j], p + where[k].at[j], 8);
    uint8_t undo[MAX_FIELDS][2][8];
    int changed = 0;
    for (int k = 0; k < count; k++) {
        int words = to[k].field == F_CROSS ? 2 : 1, size = to[k].field == F_CROSS ? 8 : 4;
        for (int j = 0; j < words; j++) memcpy(undo[k][j], p + where[k].at[j], size);
        if (same(now[k], to[k].value)) continue;
        changed++;
        if (to[k].field == F_CROSS) {
            for (int j = 0; j < 2; j++) memcpy(p + where[k].at[j], &names[(int)to[k].value][j], 8);
        } else if (to[k].field == F_RIGHT) {
            uint32_t flags;
            memcpy(&flags, p + where[k].at[0], 4);
            flags = to[k].value != 0 ? flags | TEXT_RIGHT : flags & ~(uint32_t)TEXT_RIGHT;
            memcpy(p + where[k].at[0], &flags, 4);
        } else {
            memcpy(p + where[k].at[0], &to[k].value, 4);
        }
    }
    for (int k = 0; k < count; k++) {
        Where w;
        float v;
        if (field_find(p, n, &to[k], &w) && !memcmp(&w, &where[k], sizeof w) && field_get(p, n, &to[k], &w, &v) &&
            same(v, to[k].value))
            continue;
        for (int i = count - 1; i >= 0; i--) {
            int words = to[i].field == F_CROSS ? 2 : 1, size = to[i].field == F_CROSS ? 8 : 4;
            for (int j = words - 1; j >= 0; j--) memcpy(p + where[i].at[j], undo[i][j], size);
        }
        snprintf(why, why_n, "%s %s %s overlaps another field or the records that lead to it", MEMBER, to[k].pane,
                 field_name[to[k].field]);
        return -1;
    }
    return changed;
}

int action_prompt_archive(uint8_t *sarc, size_t n, int label, char *why, size_t why_n) {
    enum { MAX_COPIES = 16 };
    size_t at[MAX_COPIES], size[MAX_COPIES], a, s;
    unsigned next = 0;
    int copies = 0, total = 0, found;
    while ((found = sarc_next(sarc, n, MEMBER, &next, &a, &s)) == 1) {
        if (copies == MAX_COPIES) {
            snprintf(why, why_n, "the layout archive holds %s more than %d times", MEMBER, MAX_COPIES);
            return -1;
        }
        at[copies] = a, size[copies++] = s;
    }
    if (found < 0) {
        snprintf(why, why_n, "the layout archive is damaged");
        return -1;
    }
    if (!copies) {
        snprintf(why, why_n, "the layout archive has no %s", MEMBER);
        return -1;
    }
    for (int c = 0; c < copies; c++)
        for (int d = 0; d < c; d++)
            if (!(at[c] == at[d] && size[c] == size[d]) && at[c] < at[d] + size[d] && at[d] < at[c] + size[c]) {
                snprintf(why, why_n, "two copies of %s in the layout archive overlap", MEMBER);
                return -1;
            }
    for (int c = 0; c < copies; c++) {
        uint8_t *trial = malloc(size[c] ? size[c] : 1);
        if (!trial) {
            snprintf(why, why_n, "out of memory");
            return -1;
        }
        memcpy(trial, sarc + at[c], size[c]);
        int r = action_prompt_lay_out(trial, size[c], label, why, why_n);
        free(trial);
        if (r < 0) return -1;
    }
    for (int c = 0; c < copies; c++) {
        int r = action_prompt_lay_out(sarc + at[c], size[c], label, why, why_n);
        if (r < 0) return -1;
        total += r;
    }
    return total;
}

int action_prompt_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, int label, void **out,
                        size_t *out_n) {
    uint64_t t0 = plat_now_ns();
    label = !!label;
    *out = NULL;
    int result = -1;
    uint8_t *dics = NULL, *raw = NULL;
    void *z = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_CDict *cd = NULL;
    char why[200] = "";
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
    int changed = action_prompt_archive(raw, (size_t)rn, label, why, sizeof why);
    if (changed <= 0) {
        result = changed;
        goto done;
    }
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
    *out = z, *out_n = zn, z = NULL;
    result = 1;
    rt_log("action prompt: face buttons laid out %s, %d fields (%.0f ms)", label ? "by label (A bottom)" : "by position",
           changed, (double)(plat_now_ns() - t0) / 1e6);
done:
    if (result == 0)
        rt_log("action prompt: the layout archive has the face buttons %s already", label ? "by label" : "by position");
    if (result < 0) rt_log("action prompt: %s; the diamond keeps the Switch places", why);
    free(z);
    free(raw);
    free(dics);
    ZSTD_freeDDict(dd);
    ZSTD_freeCDict(cd);
    return result;
}
