#include "vk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "hdr_encode_spv.h"
#include "hdr_fullscreen_spv.h"
#include "hdr_tonemap_spv.h"
#include "hdr_ui_spv.h"
#include "host.h"
#include "nvn_api.h"
#include "vk_exec.h"
#include "vk_fg.h"
#include "platform/platform.h"

#define SLOTS 8

static int g_armed = -1, g_native = -1;

static void hdr_start(void) {
    if (g_armed >= 0) return;
    g_native = !strcmp(cfg_value("NX2SARIA_HDR"), "native");
    g_armed = cfg_bool("NX2SARIA_HDR") || cfg_bool("NX2SARIA_HDR_LIVE");
}

int vk_hdr_armed(void) {
    hdr_start();
    return g_armed;
}

int vk_hdr_native(void) { return vk_hdr_armed() && g_native; }

int vk_hdr_wanted(void) {
    static unsigned seen = ~0u;
    static int on;
    if (!vk_hdr_armed()) return 0;
    if (g_native) return 1;
    unsigned v = cfg_version();
    if (__atomic_load_n(&seen, __ATOMIC_ACQUIRE) != v) {
        __atomic_store_n(&on, cfg_bool("NX2SARIA_HDR") != 0, __ATOMIC_RELEASE);
        __atomic_store_n(&seen, v, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&on, __ATOMIC_ACQUIRE);
}

int vk_hdr_restart_for(const char *value) {
    hdr_start();
    int native = !strcmp(value, "native"), on = native || !strcmp(value, "1");
    return native != g_native || (!g_armed && on);
}
int vk_hdr_needs_restart(void) { return vk_hdr_restart_for(cfg_value("NX2SARIA_HDR")); }

void vk_hdr_off(void) { g_native = 0, g_armed = 0; }

#define HDR_GAMMA_22 1.0f

typedef struct {
    float peak, game, ui;
    float gamma;
    float tm[20];
} Nits;
static struct { unsigned seen; int cur; Nits snap[2]; PlatMutex lock; } N = {.seen = ~0u, .lock = PLAT_MUTEX_INIT};

static float clampf(float v, float lo, float hi) { return fminf(fmaxf(v, lo), hi); }

static Nits nits(void) {
    unsigned v = cfg_version();
    if (__atomic_load_n(&N.seen, __ATOMIC_ACQUIRE) != v) {
        plat_mutex_lock(&N.lock);
        if (N.seen != v) {
            Nits *n = &N.snap[!N.cur];
            n->peak = clampf((float)cfg_double("NX2SARIA_HDR_PEAK_NITS"), 80.0f, 10000.0f);
            n->game = clampf((float)cfg_double("NX2SARIA_HDR_GAME_NITS"), 48.0f, 500.0f);
            n->ui = clampf((float)cfg_double("NX2SARIA_HDR_UI_NITS"), 48.0f, 500.0f);
            const char *tm = cfg_value("NX2SARIA_HDR_TONEMAP");
            float mapper = !strcmp(tm, "vanilla") ? 0.0f : !strcmp(tm, "renodrt") ? 3.0f : 9.0f;
            const char *g = cfg_value("NX2SARIA_HDR_GAMMA");
            n->gamma = vk_hdr_native() ? HDR_GAMMA_22
                       : !strcmp(g, "off") || !strcmp(g, "0") ? 0.0f : !strcmp(g, "2.4") ? 2.0f : HDR_GAMMA_22;
            float pct[10];
            static const char *const keys[10] = {"NX2SARIA_HDR_HIGHLIGHTS", "NX2SARIA_HDR_SHADOWS", "NX2SARIA_HDR_CONTRAST",
                                                 "NX2SARIA_HDR_SATURATION", "NX2SARIA_HDR_HIGHLIGHT_SAT", "NX2SARIA_HDR_BLOWOUT",
                                                 "NX2SARIA_HDR_FLARE", "NX2SARIA_HDR_HUE", "NX2SARIA_HDR_TONE_SLOPE",
                                                 "NX2SARIA_HDR_HUE_CLIP"};
            for (int i = 0; i < 10; i++) pct[i] = clampf((float)cfg_double(keys[i]), 0.0f, 100.0f);
            float *t = n->tm;
            t[0] = n->peak, t[1] = n->game, t[2] = n->ui;
            t[3] = mapper;
            t[4] = clampf((float)cfg_double("NX2SARIA_HDR_EXPOSURE"), 0.0f, 200.0f) / 100.0f;
            t[5] = pct[0] / 50.0f, t[6] = pct[1] / 50.0f, t[7] = pct[2] / 50.0f;
            t[8] = pct[3] / 50.0f, t[9] = pct[4] / 50.0f, t[10] = pct[5] / 100.0f, t[11] = pct[6] / 50.0f;
            t[12] = pct[7] / 100.0f;
            t[13] = clampf((float)cfg_double("NX2SARIA_HDR_GRADE"), 0.0f, 100.0f) / 100.0f;
            t[14] = n->gamma, t[15] = pct[8] / 50.0f;
            t[16] = pct[9] / 100.0f;
            t[17] = !strcmp(cfg_value("NX2SARIA_HDR_MIDGRAY"), "render") ? 0.0f : 1.0f;
            t[18] = cfg_bool("NX2SARIA_HDR_MATCH_SLOPE") ? 1.0f : 0.0f, t[19] = 0;
            __atomic_store_n(&N.cur, !N.cur, __ATOMIC_RELEASE);
            __atomic_store_n(&N.seen, v, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&N.lock);
    }
    return N.snap[__atomic_load_n(&N.cur, __ATOMIC_ACQUIRE)];
}

void vk_hdr_support(float out[20]) {
    Nits n = nits();
    memcpy(out, n.tm, sizeof n.tm);
    if (!vk_hdr_active()) out[3] = -1.0f;
}

enum { SPV_OP_NAME = 5, SPV_OP_EXT_INST_IMPORT = 11, SPV_OP_EXT_INST = 12, SPV_OP_MEMORY_MODEL = 14,
       SPV_OP_ENTRY_POINT = 15, SPV_OP_CAPABILITY = 17, SPV_OP_TYPE_BOOL = 20, SPV_OP_TYPE_INT = 21,
       SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23, SPV_OP_TYPE_MATRIX = 24, SPV_OP_TYPE_STRUCT = 30,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_TYPE_FUNCTION = 33, SPV_OP_CONSTANT_TRUE = 41, SPV_OP_CONSTANT_FALSE = 42,
       SPV_OP_CONSTANT = 43, SPV_OP_CONSTANT_COMPOSITE = 44, SPV_OP_CONSTANT_NULL = 46, SPV_OP_FUNCTION = 54,
       SPV_OP_FUNCTION_PARAMETER = 55, SPV_OP_FUNCTION_END = 56, SPV_OP_FUNCTION_CALL = 57, SPV_OP_VARIABLE = 59,
       SPV_OP_LOAD = 61, SPV_OP_STORE = 62, SPV_OP_ACCESS_CHAIN = 65, SPV_OP_DECORATE = 71, SPV_OP_MEMBER_DECORATE = 72,
       SPV_OP_VECTOR_SHUFFLE = 79, SPV_OP_COMPOSITE_CONSTRUCT = 80, SPV_OP_COMPOSITE_EXTRACT = 81,
       SPV_OP_COMPOSITE_INSERT = 82, SPV_OP_FADD = 129, SPV_OP_FMUL = 133, SPV_OP_FDIV = 136, SPV_OP_LOOP_MERGE = 246,
       SPV_OP_SELECTION_MERGE = 247, SPV_OP_LABEL = 248, SPV_OP_BRANCH_CONDITIONAL = 250, SPV_OP_SWITCH = 251,
       SPV_OP_TYPE_VOID = 19, SPV_OP_TYPE_FORWARD_POINTER = 39, SPV_OP_CONSTANT_SAMPLER = 45, SPV_OP_COPY_MEMORY = 63,
       SPV_OP_IMAGE_WRITE = 99, SPV_OP_ATOMIC_STORE = 228, SPV_OP_BRANCH = 249, SPV_OP_KILL = 252, SPV_OP_RETURN = 253,
       SPV_OP_RETURN_VALUE = 254, SPV_OP_UNREACHABLE = 255, SPV_OP_LINE = 8, SPV_OP_NO_LINE = 317, SPV_DEC_OFFSET = 35,
       SPV_SC_INPUT = 1, SPV_SC_UNIFORM = 2, SPV_SC_OUTPUT = 3,
       SPV_SC_FUNCTION = 7, SPV_CAP_SHADER = 1, GLSL_FMA = 50, GLSL_FMAX = 40, GLSL_FCLAMP = 43, GLSL_NMAX = 80,
       GLSL_NCLAMP = 81 };

static int name_is(const uint32_t *a, uint32_t len, const char *s) {
    size_t n = strlen(s);
    return (len - 2) * 4 >= n + 1 && !memcmp(a + 1, s, n + 1);
}

static int is_clamp(uint32_t inst) { return inst == GLSL_FCLAMP || inst == GLSL_NCLAMP; }

static int fsr_unclamp(uint32_t *w, uint64_t *words) {
    uint64_t n = *words;
    uint32_t bound = w[3], glsl = 0, float_type = 0, zero = 0, one = 0;
    int magic = 0, tex = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_EXT_INST_IMPORT && len >= 3 && name_is(a, len, "GLSL.std.450")) glsl = a[0];
        else if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) float_type = a[0];
        else if (op == SPV_OP_CONSTANT && len == 4) {
            magic |= (a[2] == 0x7ef07ebbu) | (a[2] == 0x7ef19fffu) << 1;
            if (a[0] == float_type && float_type && a[2] == 0) zero = a[1];
            if (a[0] == float_type && float_type && a[2] == 0x3f800000u) one = a[1];
        } else if (op == SPV_OP_NAME && len >= 3 && name_is(a, len, "fp_t_tcb_8")) tex = 1;
        i += len;
    }
    if (magic != 3 || !tex) return 0;
    if (!glsl || !zero || !one || !bound) return -1;
    uint8_t *out = calloc(bound, 1);
    if (!out) return 0;
    for (uint64_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_VARIABLE && len >= 4 && a[2] == SPV_SC_OUTPUT && a[1] < bound) out[a[1]] = 1;
        else if (op == SPV_OP_ACCESS_CHAIN && len >= 4 && a[2] < bound && out[a[2]] == 1 && a[1] < bound) out[a[1]] = 1;
        else if (op == SPV_OP_STORE && len >= 3 && a[0] < bound && out[a[0]] == 1 && a[1] < bound) out[a[1]] = 2;
    }
    int changed = 0;
    uint64_t k = 5;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_EXT_INST && len == 8 && a[2] == glsl && is_clamp(a[3]) && a[1] < bound && out[a[1]] == 2 &&
            a[5] == zero && a[6] == one) {
            uint32_t ins[7] = {7u << 16 | SPV_OP_EXT_INST, a[0], a[1], a[2], a[3] == GLSL_NCLAMP ? GLSL_NMAX : GLSL_FMAX,
                               a[4], zero};
            memmove(w + k, ins, sizeof ins);
            k += 7, changed++;
        } else {
            memmove(w + k, w + i, len * sizeof *w);
            k += len;
        }
        i += len;
    }
    free(out);
    *words = k;
    return changed ? changed : -1;
}

static int spv_literal(uint32_t op, uint32_t j) {
    switch (op) {
    case SPV_OP_FUNCTION: case SPV_OP_VARIABLE: return j == 3;
    case SPV_OP_LOAD: return j >= 4;
    case SPV_OP_STORE: return j >= 3;
    case SPV_OP_COMPOSITE_EXTRACT: return j >= 4;
    case SPV_OP_COMPOSITE_INSERT: case SPV_OP_VECTOR_SHUFFLE: return j >= 5;
    case SPV_OP_EXT_INST: return j == 4;
    case SPV_OP_SELECTION_MERGE: return j == 2;
    case SPV_OP_LOOP_MERGE: return j >= 3;
    case SPV_OP_BRANCH_CONDITIONAL: return j >= 4;
    case SPV_OP_CONSTANT: return j >= 3;
    case SPV_OP_TYPE_INT: case SPV_OP_TYPE_FLOAT: return j >= 2;
    case SPV_OP_TYPE_VECTOR: case SPV_OP_TYPE_MATRIX: return j == 3;
    case SPV_OP_TYPE_POINTER: return j == 2;
    case SPV_OP_SWITCH: case SPV_OP_LINE: case SPV_OP_NO_LINE: case SPV_OP_COPY_MEMORY: return -1;
    default: return 0;
    }
}

typedef struct { uint32_t *w; uint64_t n, cap; int bad; } SpvOut;

static void put(SpvOut *o, const uint32_t *ins, uint32_t len) {
    if (o->n + len > o->cap) {
        uint64_t cap = (o->n + len) * 2;
        uint32_t *w = realloc(o->w, cap * sizeof *w);
        if (!w) { o->bad = 1; return; }
        o->w = w, o->cap = cap;
    }
    memcpy(o->w + o->n, ins, len * sizeof *ins);
    o->n += len;
}
#define PUT(o, ...) do { const uint32_t ins_[] = {__VA_ARGS__}; const uint32_t n_ = sizeof ins_ / sizeof *ins_; \
        uint32_t h_[1] = {n_ << 16 | ins_[0]}; put(o, h_, 1); put(o, ins_ + 1, n_ - 1); } while (0)

static int put_mapped(SpvOut *o, const uint32_t *ins, const uint32_t *map, uint32_t lbound) {
    uint32_t op = ins[0] & 0xffff, len = ins[0] >> 16, tmp[64];
    if (len > 64) return 0;
    tmp[0] = ins[0];
    for (uint32_t j = 1; j < len; j++) {
        int lit = spv_literal(op, j);
        if (lit < 0) return 0;
        if (lit) { tmp[j] = ins[j]; continue; }
        if (ins[j] >= lbound || !map[ins[j]]) return 0;
        tmp[j] = map[ins[j]];
    }
    put(o, tmp, len);
    return 1;
}

static int is_type_op(uint32_t op) { return op >= SPV_OP_TYPE_VOID && op <= SPV_OP_TYPE_FORWARD_POINTER; }
static int is_const_op(uint32_t op) {
    return op >= SPV_OP_CONSTANT_TRUE && op <= SPV_OP_CONSTANT_NULL && op != SPV_OP_CONSTANT_SAMPLER;
}

typedef struct { const uint32_t *w, *def, *sval; const uint8_t *cls, *scnt; uint32_t bound, glsl; } SpvView;

static uint32_t spv_stored(const SpvView *v, uint32_t id) {
    if (id >= v->bound || !v->def[id]) return id;
    const uint32_t *l = v->w + v->def[id];
    if ((l[0] & 0xffff) != SPV_OP_LOAD || l[3] >= v->bound || v->cls[l[3]] != 3 || v->scnt[l[3]] != 1) return id;
    return v->sval[l[3]];
}

static int spv_uniform_load(const SpvView *v, uint32_t id) {
    if (id >= v->bound || !v->def[id]) return 0;
    const uint32_t *l = v->w + v->def[id];
    if ((l[0] & 0xffff) != SPV_OP_LOAD || l[3] >= v->bound || !v->def[l[3]]) return 0;
    const uint32_t *c = v->w + v->def[l[3]];
    return (c[0] & 0xffff) == SPV_OP_ACCESS_CHAIN && c[3] < v->bound && v->cls[c[3]] == 2;
}

static int spv_same_source(const SpvView *v, uint32_t a, uint32_t b) {
    if (a == b) return 1;
    if (a >= v->bound || b >= v->bound || !v->def[a] || !v->def[b]) return 0;
    const uint32_t *la = v->w + v->def[a], *lb = v->w + v->def[b];
    if ((la[0] & 0xffff) != SPV_OP_LOAD || (lb[0] & 0xffff) != SPV_OP_LOAD) return 0;
    if (la[3] == lb[3]) return 1;
    if (la[3] >= v->bound || lb[3] >= v->bound || !v->def[la[3]] || !v->def[lb[3]]) return 0;
    const uint32_t *ca = v->w + v->def[la[3]], *cb = v->w + v->def[lb[3]];
    return (ca[0] & 0xffff) == SPV_OP_ACCESS_CHAIN && ca[0] == cb[0] && !memcmp(ca + 3, cb + 3, ((ca[0] >> 16) - 3) * 4);
}

static int spv_common_factor(const SpvView *v, const uint32_t val[3], uint32_t rest[3], uint32_t *factor) {
    const uint32_t *m[3];
    for (int c = 0; c < 3; c++) {
        if (val[c] >= v->bound || !v->def[val[c]]) return 0;
        m[c] = v->w + v->def[val[c]];
        if ((m[c][0] & 0xffff) != SPV_OP_FMUL) return 0;
    }
    for (int pos = 4; pos >= 3; pos--) {
        uint32_t f = m[0][pos];
        int ok = 1;
        rest[0] = m[0][7 - pos];
        for (int c = 1; c < 3 && ok; c++) {
            if (spv_same_source(v, f, m[c][4])) rest[c] = m[c][3];
            else if (spv_same_source(v, f, m[c][3])) rest[c] = m[c][4];
            else ok = 0;
        }
        if (ok) { *factor = f; return 1; }
    }
    return 0;
}

static int spv_common_fma(const SpvView *v, const uint32_t val[3], uint32_t a[3], uint32_t t[3], uint32_t *factor) {
    const uint32_t *m[3];
    for (int c = 0; c < 3; c++) {
        if (val[c] >= v->bound || !v->def[val[c]]) return 0;
        m[c] = v->w + v->def[val[c]];
        if ((m[c][0] & 0xffff) != SPV_OP_EXT_INST || (m[c][0] >> 16) != 8 || m[c][3] != v->glsl || m[c][4] != GLSL_FMA) return 0;
        t[c] = m[c][7];
    }
    for (int pos = 6; pos >= 5; pos--) {
        uint32_t f = m[0][pos];
        int ok = 1;
        a[0] = m[0][11 - pos];
        for (int c = 1; c < 3 && ok; c++) {
            if (spv_same_source(v, f, m[c][6])) a[c] = m[c][5];
            else if (spv_same_source(v, f, m[c][5])) a[c] = m[c][6];
            else ok = 0;
        }
        if (ok) { *factor = f; return 1; }
    }
    return 0;
}

static int spv_exposure(const SpvView *v, const uint32_t x[3], uint32_t exposed[3], uint32_t add[3], uint32_t *gain) {
    uint32_t val[3], inner[3], rest[3], a[3], t[3], f;
    for (int c = 0; c < 3; c++) val[c] = spv_stored(v, x[c]), exposed[c] = x[c], add[c] = 0;
    if (spv_common_factor(v, val, rest, gain)) {
        if (spv_uniform_load(v, *gain)) return 1;
        for (int c = 0; c < 3; c++) inner[c] = spv_stored(v, rest[c]);
        if (spv_common_factor(v, inner, rest, &f) && spv_uniform_load(v, f)) {
            memcpy(exposed, inner, sizeof inner);
            *gain = f;
        }
        return 1;
    }
    if (!spv_common_fma(v, val, a, t, gain)) return 0;
    if (spv_uniform_load(v, *gain)) return 1;
    for (int c = 0; c < 3; c++) inner[c] = spv_stored(v, a[c]);
    if (!spv_common_factor(v, inner, rest, &f) || !spv_uniform_load(v, f)) return 0;
    memcpy(exposed, inner, sizeof inner);
    memcpy(add, t, sizeof t);
    *gain = f;
    return 1;
}

static int spv_top_level(const uint32_t *w, uint64_t first_label, uint64_t end, uint64_t pos) {
    enum { DEPTH = 64 };
    uint32_t merge[DEPTH];
    int depth = 0;
    for (uint64_t i = first_label; i < end; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        if (op == SPV_OP_LABEL)
            while (depth && merge[depth - 1] == w[i + 1]) depth--;
        if (i == pos) return !depth;
        if (op == SPV_OP_SELECTION_MERGE || op == SPV_OP_LOOP_MERGE) {
            if (depth == DEPTH) return 0;
            merge[depth++] = w[i + 1];
        }
    }
    return 0;
}

enum { HDR_SETS = 5, HDR_PARAMS = HDR_SETS + 2 };
static int tonemap_splice(uint32_t **spirv, uint64_t *words) {
    uint32_t *w = *spirv;
    uint64_t n = *words;
    uint32_t bound = w[3];
    if (bound < 8 || bound > (1u << 22)) return 0;
    const float f251 = 2.51f, f003 = 0.03f, fone = 1.0f;
    uint32_t b251, b003, bone;
    memcpy(&b251, &f251, 4), memcpy(&b003, &f003, 4), memcpy(&bone, &fone, 4);
    uint32_t glsl = 0, ft = 0, c0 = 0, c1 = 0, c251 = 0, c003 = 0, sb = 0, sb_ptr = 0, st = 0, st_len = 0, st_v4 = 0,
             it = 0, ptr_v4u = 0, last_offset = 0;
    uint64_t first_fn = 0, first_label = 0, st_at = 0, dec_at = 0;
    int nfn = 0;
    uint32_t *def = calloc(bound, sizeof *def);
    int32_t *cval = calloc(bound, sizeof *cval);
    uint8_t *cls = calloc(bound, 1);
    uint32_t *sval = calloc(bound, sizeof *sval);
    uint8_t *scnt = calloc(bound, 1);
    if (!def || !cval || !cls || !sval || !scnt) goto fail;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto fail;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_EXT_INST_IMPORT && len >= 3 && name_is(a, len, "GLSL.std.450")) glsl = a[0];
        else if (op == SPV_OP_TYPE_FLOAT && len == 3 && a[1] == 32) ft = a[0];
        else if (op == SPV_OP_TYPE_INT && len == 4 && a[1] == 32 && (!it || a[2] == 1)) it = a[0];
        else if (op == SPV_OP_CONSTANT && len == 4 && a[0] == ft && ft) {
            if (a[2] == 0) c0 = a[1];
            else if (a[2] == bone) c1 = a[1];
            else if (a[2] == b251) c251 = a[1];
            else if (a[2] == b003) c003 = a[1];
        } else if (op == SPV_OP_CONSTANT && len == 4 && a[1] < bound && a[2] < 0x10000) cval[a[1]] = (int32_t)a[2] + 1;
        else if (op == SPV_OP_NAME && len >= 3 && name_is(a, len, "support_buffer")) sb = a[0];
        else if (op == SPV_OP_MEMBER_DECORATE && len == 5 && a[1] == 10 && a[2] == SPV_DEC_OFFSET && a[3] == 1424)
            last_offset = a[0], dec_at = i;
        else if (op == SPV_OP_FUNCTION) { nfn++; if (!first_fn) first_fn = i; }
        else if (op == SPV_OP_LABEL && !first_label) first_label = i;
        if (op == SPV_OP_VARIABLE && len >= 4 && a[1] < bound) {
            if (a[1] == sb && sb) sb_ptr = a[0];
            cls[a[1]] = a[2] == SPV_SC_OUTPUT ? 1 : a[2] == SPV_SC_UNIFORM ? 2 : a[2] == SPV_SC_FUNCTION ? 3 : 0;
        }
        if (op == SPV_OP_STORE && len >= 3 && a[0] < bound) sval[a[0]] = a[1], scnt[a[0]] += scnt[a[0]] < 2;
        if (op == SPV_OP_VARIABLE && len >= 4 && a[1] < bound) def[a[1]] = (uint32_t)i;
        else if (first_fn && len >= 3 && a[1] < bound && !def[a[1]] && op != SPV_OP_STORE && op != SPV_OP_LABEL &&
                 op != SPV_OP_SELECTION_MERGE && op != SPV_OP_LOOP_MERGE && op != SPV_OP_BRANCH_CONDITIONAL &&
                 op != SPV_OP_SWITCH && op != SPV_OP_COPY_MEMORY && op != SPV_OP_IMAGE_WRITE &&
                 op != SPV_OP_ATOMIC_STORE && op != SPV_OP_RETURN_VALUE)
            def[a[1]] = (uint32_t)i;
        i += len;
    }
    for (uint64_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == SPV_OP_TYPE_POINTER && len == 4 && w[i + 1] == sb_ptr && sb_ptr) st = w[i + 3];
    }
    for (uint64_t i = 5; i < n && st; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == SPV_OP_TYPE_STRUCT && w[i + 1] == st) st_len = len, st_at = i, st_v4 = w[i + len - 1];
    }
    for (uint64_t i = 5; i < n && st_v4; i += w[i] >> 16)
        if ((w[i] & 0xffff) == SPV_OP_TYPE_POINTER && (w[i] >> 16) == 4 && w[i + 2] == SPV_SC_UNIFORM && w[i + 3] == st_v4)
            ptr_v4u = w[i + 1];
    if (!glsl || !ft || !it || !c0 || !c1 || !c251 || !c003 || !sb || st_len != 13 || last_offset != st || nfn != 1 ||
        !first_label || first_label < first_fn)
        goto fail;
    enum { MAX_SITES = 8 };
    uint64_t site[MAX_SITES];
    uint32_t site_x[MAX_SITES], site_var[MAX_SITES];
    int nsite = 0, bad_x = 0, other_x = 0;
    const SpvView view = {w, def, sval, cls, scnt, bound, glsl};
    for (uint64_t i = first_fn; i < n && nsite < MAX_SITES; i += w[i] >> 16) {
        const uint32_t *a = w + i + 1;
        if ((w[i] & 0xffff) != SPV_OP_EXT_INST || (w[i] >> 16) != 8 || a[2] != glsl || !is_clamp(a[3]) ||
            a[5] != c0 || a[6] != c1 || a[4] >= bound || !def[a[4]])
            continue;
        const uint32_t *m = w + def[a[4]];
        if ((m[0] & 0xffff) != SPV_OP_FMUL || m[3] >= bound || !def[m[3]]) continue;
        const uint32_t *num = w + def[m[3]];
        if ((num[0] & 0xffff) != SPV_OP_FMUL || num[4] >= bound || !def[num[4]]) continue;
        const uint32_t *f = w + def[num[4]];
        if ((f[0] & 0xffff) != SPV_OP_EXT_INST || (f[0] >> 16) != 8 || f[4] != GLSL_FMA || f[6] != c251 || f[7] != c003)
            continue;
        if (num[3] >= bound) {
            bad_x++;
            continue;
        }
        if (f[5] != num[3] && !spv_same_source(&view, f[5], num[3]) &&
            spv_stored(&view, f[5]) != spv_stored(&view, num[3])) {
            other_x++;
            continue;
        }
        site[nsite] = i, site_x[nsite] = num[3], site_var[nsite] = 0;
        for (uint64_t j = i; j < n; j += w[j] >> 16)
            if ((w[j] & 0xffff) == SPV_OP_STORE && (w[j] >> 16) >= 3 && w[j + 2] == a[1]) { site_var[nsite] = w[j + 1]; break; }
        if (site_var[nsite]) nsite++;
    }
    if (bad_x || other_x) {
        static volatile int32_t refused;
        if (plat_inc32(&refused) <= 4)
            rt_log("hdr: %d clamp(s) of a fitted ACES curve not taken: %d with an x that is no id of the module, %d whose "
                   "Fma does not take the x its FMul does; %d taken, %s", bad_x + other_x, bad_x, other_x, nsite,
                   nsite < 3 ? "too few: the shader keeps its own tonemap" : "enough for the splice");
    }
    if (nsite < 3) goto fail;
    uint64_t chain_at[3] = {0}, store_at[3] = {0};
    int chan_site[3] = {-1, -1, -1};
    for (uint64_t i = first_fn; i < n; i += w[i] >> 16) {
        const uint32_t *a = w + i + 1;
        if ((w[i] & 0xffff) != SPV_OP_STORE || (w[i] >> 16) < 3 || a[0] >= bound || !def[a[0]]) continue;
        const uint32_t *ch = w + def[a[0]];
        if ((ch[0] & 0xffff) != SPV_OP_ACCESS_CHAIN || (ch[0] >> 16) != 5 || ch[3] >= bound || cls[ch[3]] != 1 ||
            ch[4] >= bound || cval[ch[4]] < 1 || cval[ch[4]] > 3)
            continue;
        int c = cval[ch[4]] - 1;
        if (store_at[c]) goto fail;
        chain_at[c] = def[a[0]], store_at[c] = i;
        for (uint64_t j = chain_at[c]; j < i; j += w[j] >> 16)
            if ((w[j] & 0xffff) == SPV_OP_LOAD)
                for (int s = 0; s < nsite; s++)
                    if (w[j + 3] == site_var[s]) chan_site[c] = s;
    }
    uint64_t at = ~0ull, lo = ~0ull, hi = 0;
    for (int c = 0; c < 3; c++) {
        if (chan_site[c] < 0 || !store_at[c]) goto fail;
        for (int d = 0; d < c; d++)
            if (chan_site[d] == chan_site[c]) goto fail;
        int s = chan_site[c];
        at = chain_at[c] < at ? chain_at[c] : at;
        uint64_t xd = def[site_x[s]] ? def[site_x[s]] : site[s];
        lo = xd < lo ? xd : lo, lo = site[s] < lo ? site[s] : lo;
        hi = store_at[c] > hi ? store_at[c] : hi;
    }
    for (int c = 0; c < 3; c++)
        if (site[chan_site[c]] >= at || def[site_x[chan_site[c]]] >= at) goto fail;
    for (uint64_t i = lo; i < hi; i += w[i] >> 16)
        if ((w[i] & 0xffff) == SPV_OP_LABEL) goto fail;
    uint32_t exposed[3], add[3], gain = 0;
    uint64_t at_block = first_label;
    for (uint64_t i = first_label + (w[first_label] >> 16); i < at; i += w[i] >> 16)
        if ((w[i] & 0xffff) == SPV_OP_LABEL) at_block = i;
    const uint32_t xs[3] = {site_x[chan_site[0]], site_x[chan_site[1]], site_x[chan_site[2]]};
    int exposure_found = spv_exposure(&view, xs, exposed, add, &gain);
    for (int c = 0; c < 7 && exposure_found; c++) {
        uint32_t idc = c < 3 ? exposed[c] : c < 6 ? add[c - 3] : gain;
        if (c >= 3 && c < 6 && !idc) continue;
        uint64_t d = idc < bound ? def[idc] : 0;
        if (!d || d >= at || !(d > at_block || spv_top_level(w, first_label, at, d))) exposure_found = 0;
    }
    if (!exposure_found) {
        for (int c = 0; c < 3; c++) exposed[c] = site_x[chan_site[c]], add[c] = 0;
        gain = c1;
    }

    const uint32_t *L = hdr_tonemap_spv;
    const uint64_t ln = sizeof hdr_tonemap_spv / 4;
    const uint32_t lbound = L[3];
    uint32_t *map = calloc(lbound, sizeof *map), id = bound;
    SpvOut g = {0}, f = {0}, o = {0};
    uint32_t entry = 0, fn = 0, fn_type = 0;
    if (!map) goto fail;
    for (uint64_t i = 5; i < ln; i += L[i] >> 16) {
        uint32_t op = L[i] & 0xffff, len = L[i] >> 16;
        if (op == SPV_OP_ENTRY_POINT) entry = L[i + 2];
        else if (op == SPV_OP_NAME && len >= 3 && !memcmp(L + i + 2, "nx_tonemap(", 11)) fn = L[i + 1];
    }
    for (uint64_t i = 5; i < ln;) {
        uint32_t op = L[i] & 0xffff, len = L[i] >> 16;
        const uint32_t *a = L + i + 1;
        if (op == SPV_OP_FUNCTION) break;
        if (op == SPV_OP_CAPABILITY && a[0] != SPV_CAP_SHADER) goto lib_fail;
        if (op == SPV_OP_EXT_INST_IMPORT) {
            if (!name_is(a, len, "GLSL.std.450")) goto lib_fail;
            map[a[0]] = glsl;
        } else if (is_type_op(op)) {
            uint32_t tmp[64];
            if (len > 64) goto lib_fail;
            tmp[0] = L[i], tmp[1] = 0;
            for (uint32_t j = 2; j < len; j++) {
                int lit = spv_literal(op, j);
                if (!lit && (a[j - 1] >= lbound || !map[a[j - 1]])) goto lib_fail;
                tmp[j] = lit ? a[j - 1] : map[a[j - 1]];
            }
            uint32_t same = 0;
            for (uint64_t k = 5; k < first_fn && op != SPV_OP_TYPE_STRUCT && !same; k += w[k] >> 16)
                if (w[k] == L[i] && !memcmp(w + k + 2, tmp + 2, (len - 2) * 4)) same = w[k + 1];
            if (same) {
                map[a[0]] = same;
            } else {
                map[a[0]] = tmp[1] = id++;
                put(&g, tmp, len);
            }
        } else if (is_const_op(op)) {
            map[a[1]] = id++;
            if (!put_mapped(&g, L + i, map, lbound)) goto lib_fail;
        } else if (op == SPV_OP_VARIABLE && a[2] != SPV_SC_INPUT && a[2] != SPV_SC_OUTPUT) {
            goto lib_fail;
        }
        i += len;
    }
    for (uint64_t i = 5; i < ln; i += L[i] >> 16)
        if ((L[i] & 0xffff) == SPV_OP_FUNCTION && L[i + 2] == fn) fn_type = L[i + 4];
    if (!entry || !fn || !fn_type || !map[fn_type]) goto lib_fail;
    const uint32_t *ftype = NULL;
    for (uint64_t i = 5; i < ln && !ftype; i += L[i] >> 16)
        if ((L[i] & 0xffff) == SPV_OP_TYPE_FUNCTION && L[i + 1] == fn_type && (L[i] >> 16) == 3 + HDR_PARAMS) ftype = L + i;
    if (!ftype) goto lib_fail;
    uint32_t param_ptr[HDR_PARAMS], v3 = map[ftype[2]], v4 = 0, ptr_types_ok = 1;
    for (int p = 0; p < HDR_PARAMS; p++) {
        param_ptr[p] = map[ftype[3 + p]];
        uint32_t pointee = 0;
        for (uint64_t i = 5; i < ln; i += L[i] >> 16)
            if ((L[i] & 0xffff) == SPV_OP_TYPE_POINTER && L[i + 1] == ftype[3 + p] && L[i + 2] == SPV_SC_FUNCTION)
                pointee = map[L[i + 3]];
        if (!pointee || (p == 0 && pointee != v3) || (p > 0 && v4 && pointee != v4)) ptr_types_ok = 0;
        if (p > 0) v4 = pointee;
    }
    if (!ptr_types_ok || v4 != st_v4) goto lib_fail;
    uint64_t lib_fn = 0;
    for (uint64_t i = 5; i < ln && !lib_fn; i += L[i] >> 16)
        if ((L[i] & 0xffff) == SPV_OP_FUNCTION) lib_fn = i;
    for (uint64_t i = lib_fn; i < ln && lib_fn; i += L[i] >> 16) {
        uint32_t op = L[i] & 0xffff, len = L[i] >> 16, r;
        switch (op) {
        case SPV_OP_STORE: case SPV_OP_SELECTION_MERGE: case SPV_OP_LOOP_MERGE: case SPV_OP_BRANCH_CONDITIONAL:
        case SPV_OP_BRANCH: case SPV_OP_RETURN: case SPV_OP_RETURN_VALUE: case SPV_OP_FUNCTION_END:
        case SPV_OP_KILL: case SPV_OP_UNREACHABLE:
            continue;
        case SPV_OP_LABEL: r = len >= 2 ? L[i + 1] : 0; break;
        default: r = len >= 3 ? L[i + 2] : 0; break;
        }
        if (!r || r >= lbound) goto lib_fail;
        if (!map[r]) map[r] = id++;
    }
    int in_main = 0;
    for (uint64_t i = lib_fn; i < ln && lib_fn; i += L[i] >> 16) {
        uint32_t op = L[i] & 0xffff;
        if (op == SPV_OP_FUNCTION) in_main = L[i + 2] == entry;
        if (!in_main && !put_mapped(&f, L + i, map, lbound)) goto lib_fail;
        if (op == SPV_OP_FUNCTION_END) in_main = 0;
    }
    if (!lib_fn || !map[fn] || g.bad || f.bad) goto lib_fail;

    uint32_t idx[HDR_SETS], ptr_v4 = ptr_v4u ? ptr_v4u : id++, var[HDR_PARAMS], pv[HDR_SETS], pl[HDR_SETS], xv = id++,
             sv = id++, res = id++, e[3], sum[3];
    for (int m = 0; m < HDR_SETS; m++) idx[m] = id++, pv[m] = id++, pl[m] = id++;
    for (int p = 0; p < HDR_PARAMS; p++) var[p] = id++;
    for (int c = 0; c < 3; c++) e[c] = id++, sum[c] = id++;
    put(&o, w, 5);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == first_fn) {
            put(&o, g.w, (uint32_t)g.n);
            if (!ptr_v4u) PUT(&o, SPV_OP_TYPE_POINTER, ptr_v4, SPV_SC_UNIFORM, st_v4);
            for (int m = 0; m < HDR_SETS; m++) PUT(&o, SPV_OP_CONSTANT, it, idx[m], 11 + m);
        }
        if (i == st_at) {
            put(&o, w + i, len);
            o.w[o.n - len] = (len + HDR_SETS) << 16 | op;
            for (int m = 0; m < HDR_SETS; m++) put(&o, &st_v4, 1);
        } else if (i == dec_at) {
            put(&o, w + i, len);
            for (int m = 0; m < HDR_SETS; m++) PUT(&o, SPV_OP_MEMBER_DECORATE, st, 11 + m, SPV_DEC_OFFSET, 1440 + 16 * m);
        } else if (i == first_label) {
            put(&o, w + i, len);
            for (int p = 0; p < HDR_PARAMS; p++) PUT(&o, SPV_OP_VARIABLE, param_ptr[p], var[p], SPV_SC_FUNCTION);
        } else if (i == store_at[0] || i == store_at[1] || i == store_at[2]) {
            int c = i == store_at[0] ? 0 : i == store_at[1] ? 1 : 2;
            put(&o, w + i, len);
            o.w[o.n - len + 2] = e[c];
        } else {
            if (i == at) {
                for (int m = 0; m < HDR_SETS; m++) {
                    PUT(&o, SPV_OP_ACCESS_CHAIN, ptr_v4, pv[m], sb, idx[m]);
                    PUT(&o, SPV_OP_LOAD, st_v4, pl[m], pv[m]);
                }
                PUT(&o, SPV_OP_COMPOSITE_CONSTRUCT, v3, xv, site_x[chan_site[0]], site_x[chan_site[1]],
                    site_x[chan_site[2]]);
                for (int c = 0; c < 3; c++)
                    if (add[c]) {
                        PUT(&o, SPV_OP_FADD, ft, sum[c], exposed[c], add[c]);
                        exposed[c] = sum[c];
                    }
                PUT(&o, SPV_OP_COMPOSITE_CONSTRUCT, st_v4, sv, exposed[0], exposed[1], exposed[2], gain);
                PUT(&o, SPV_OP_STORE, var[0], xv);
                PUT(&o, SPV_OP_STORE, var[1], sv);
                for (int m = 0; m < HDR_SETS; m++) PUT(&o, SPV_OP_STORE, var[m + 2], pl[m]);
                uint32_t call[4 + HDR_PARAMS] = {(4u + HDR_PARAMS) << 16 | SPV_OP_FUNCTION_CALL, v3, res, map[fn]};
                memcpy(call + 4, var, sizeof var);
                put(&o, call, 4 + HDR_PARAMS);
                for (int c = 0; c < 3; c++) PUT(&o, SPV_OP_COMPOSITE_EXTRACT, ft, e[c], res, c);
            }
            put(&o, w + i, len);
        }
        i += len;
    }
    put(&o, f.w, (uint32_t)f.n);
    if (o.bad) goto lib_fail;
    o.w[3] = id;
    free(map), free(g.w), free(f.w), free(def), free(cval), free(cls), free(sval), free(scnt);
    free(*spirv);
    *spirv = o.w;
    *words = o.n;
    return exposure_found ? 1 : 2;
lib_fail:
    free(map), free(g.w), free(f.w), free(o.w);
fail:
    free(def), free(cval), free(cls), free(sval), free(scnt);
    return 0;
}
#undef PUT

void vk_hdr_shader_swap(uint32_t **spirv, uint64_t *words) {
    if (!vk_hdr_armed() || vk_hdr_native() || !*spirv || *words < 5) return;
    int spliced = tonemap_splice(spirv, words);
    if (spliced) {
        static volatile int32_t count;
        rt_log("hdr: the game's tonemap maps its picture onto the display's range (%d shaders so far)%s",
               plat_inc32(&count), spliced == 2 ? "; its exposure was not found, so the tone mapper takes it as 1" : "");
    }
    int unclamped = fsr_unclamp(*spirv, words);
    if (unclamped > 0) {
        static volatile int32_t fsrs;
        rt_log("hdr: the game's FSR pass keeps values above 1 (%d output clamps; %d so far)", unclamped,
               plat_inc32(&fsrs));
    } else if (unclamped < 0) {
        static volatile int32_t missed;
        if (plat_inc32(&missed) == 1)
            rt_log("hdr: the game's FSR pass was found but no clamp of its output; it still clips HDR highlights");
    }
}


int vk_hdr_track;
static volatile int32_t g_active;
static PlatMutex rec_lock = PLAT_MUTEX_INIT;
static struct { VkImage win, src; VkFormat fmt; uint64_t pos; } recs[4];
static int nrecs;

void vk_hdr_set_active(int on) { plat_xchg32(&g_active, on); }
int vk_hdr_active(void) { return plat_cas32(&g_active, 0, 0); }

static int is_float(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT ||
           f == VK_FORMAT_R32G32B32A32_SFLOAT;
}

void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info) {
    const VkExecTarget *t = &ex->color[0];
    if (!t->image) return;
    int known = 0;
    for (int i = 0; i < 4; i++)
        known |= __atomic_load_n(&recs[i].win, __ATOMIC_RELAXED) == t->image;
    if (!known) return;
    uint64_t pos = vk_exec_stream_pos();
    VkImage src = VK_NULL_HANDLE;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    int ntex = 0, slot = -1;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *bd = &info->bindings[i];
        if (bd->kind != GFX_RES_TEX || bd->stage != GFX_STAGE_FS || !bd->res) continue;
        ntex += bd->count ? bd->count : 1;
        if ((bd->res->type & 0x7ff) == 3 && !bd->res->separate) slot = bd->res->slot;
    }
    if (ntex == 1 && slot >= 0 && slot < 128 && ex->tex[1][slot]) {
        const VkTexImage *s = vk_texture_by_id(NVN_HANDLE_TEX(ex->tex[1][slot]), NULL, NULL);
        if (s && is_float(s->format) && s->width == t->width && s->height == t->height && s->samples == 1)
            src = s->image, fmt = s->format;
    }
    plat_mutex_lock(&rec_lock);
    for (int i = 0; i < nrecs; i++)
        if (recs[i].win == t->image && pos >= recs[i].pos) recs[i].src = src, recs[i].fmt = fmt, recs[i].pos = pos;
    plat_mutex_unlock(&rec_lock);
}

void vk_hdr_present_source(const VkTexImage *win, VkImage *image, VkFormat *format) {
    if (!vk_hdr_wanted() || vk_hdr_native()) return;
    vk_hdr_track = 1;
    plat_mutex_lock(&rec_lock);
    int r = -1;
    for (int i = 0; i < nrecs; i++)
        if (recs[i].win == win->image) r = i;
    if (r < 0 && nrecs < 4) r = nrecs++, recs[r].win = win->image, recs[r].src = VK_NULL_HANDLE, recs[r].pos = 0;
    if (r >= 0 && recs[r].src && vk_hdr_active()) *image = recs[r].src, *format = recs[r].fmt;
    plat_mutex_unlock(&rec_lock);
    static int logged;
    if (!logged && *image != win->image) logged = 1, rt_log("hdr: the present reads the game's float picture");
}

void vk_hdr_image_gone(VkImage image) {
    if (!vk_hdr_track) return;
    plat_mutex_lock(&rec_lock);
    for (int i = 0; i < nrecs; i++) {
        if (recs[i].src == image) recs[i].src = VK_NULL_HANDLE;
        if (recs[i].win == image) recs[i] = recs[--nrecs], recs[nrecs] = (typeof(recs[0])){0}, i--;
    }
    plat_mutex_unlock(&rec_lock);
}

int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out) {
    if (!vk_hdr_wanted()) return 0;
    static int logged;
    if (!logged) {
        char b[1024];
        int o = 0;
        for (uint32_t i = 0; i < n && o < (int)sizeof b - 32; i++)
            o += snprintf(b + o, sizeof b - (size_t)o, " %d/%d", (int)f[i].format, (int)f[i].colorSpace);
        rt_log("hdr: surface formats (VkFormat/VkColorSpaceKHR):%s", b);
    }
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (f[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                f[i].format == (pass ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
                *out = f[i];
                if (!logged) rt_log("hdr: HDR10 swapchain (format %d)", (int)f[i].format);
                logged = 1;
                return 1;
            }
    if (!logged)
        rt_log("hdr: the display offers no HDR10 format, so the picture stays SDR (Windows: turn on \"Use HDR\" in the "
               "display settings; Linux: a Wayland compositor with colour management and HDR on)");
    logged = 1;
    return 0;
}

void vk_hdr_metadata(VkSwapchainKHR sc, int new_sc) {
    static unsigned sent = ~0u;
    VkCtx *c = vk_ctx();
    if (!c->has_hdr_metadata || (!new_sc && cfg_version() == sent)) return;
    sent = cfg_version();
    PFN_vkSetHdrMetadataEXT set = vk_fg_hdr_metadata_fn();
    if (!set) set = (PFN_vkSetHdrMetadataEXT)vk_dpa(c->device, "vkSetHdrMetadataEXT");
    if (!set) return;
    Nits n = nits();
    VkHdrMetadataEXT m = {.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                          .displayPrimaryRed = {0.708f, 0.292f}, .displayPrimaryGreen = {0.170f, 0.797f},
                          .displayPrimaryBlue = {0.131f, 0.046f}, .whitePoint = {0.3127f, 0.3290f},
                          .maxLuminance = n.peak, .minLuminance = 0.0f, .maxContentLightLevel = n.peak,
                          .maxFrameAverageLightLevel = n.game};
    set(c->device, 1, &sc, &m);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

typedef struct { VkImage img; VmaAllocation alloc; VkImageView view; uint32_t w, h; } Img;

static void img_free(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->alloc);
    *m = (Img){0};
}

static int img_ensure(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    if (m->img && m->w == w && m->h == h) return 1;
    if (m->img) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        img_free(c, m);
    }
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &m->img, &m->alloc, NULL) != VK_SUCCESS) {
        m->img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &m->view) != VK_SUCCESS) {
        m->view = VK_NULL_HANDLE;
        img_free(c, m);
        return 0;
    }
    m->w = w, m->h = h;
    return 1;
}

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkSampler sampler;
    VkDescriptorSetLayout dsl_enc, dsl_ui;
    VkPipelineLayout enc_layout, ui_layout;
    VkPipeline enc;
    VkPipeline ui;
    VkFormat ui_fmt;
    Img out;
    Img menu;
    VkImageView src_view[SLOTS];
} H;

static VkShaderModule module(VkCtx *c, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static VkDescriptorSetLayout set_layout(VkCtx *c, const VkDescriptorSetLayoutBinding *b, uint32_t n) {
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                          .bindingCount = n, .pBindings = b};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    return vkCreateDescriptorSetLayout(c->device, &ci, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static VkPipelineLayout pipe_layout(VkCtx *c, VkDescriptorSetLayout *dsl, VkShaderStageFlags st, uint32_t pc) {
    VkPushConstantRange pr = {st, 0, pc};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkPipelineLayout l = VK_NULL_HANDLE;
    return vkCreatePipelineLayout(c->device, &li, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static int init(VkCtx *c) {
    H.tried = 1;
    H.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding enc[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutBinding ui = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (!H.push || vkCreateSampler(c->device, &si, NULL, &H.sampler) != VK_SUCCESS ||
        !(H.dsl_enc = set_layout(c, enc, 2)) || !(H.dsl_ui = set_layout(c, &ui, 1)) ||
        !(H.enc_layout = pipe_layout(c, &H.dsl_enc, VK_SHADER_STAGE_COMPUTE_BIT, 32)) ||
        !(H.ui_layout = pipe_layout(c, &H.dsl_ui, VK_SHADER_STAGE_FRAGMENT_BIT, 16))) {
        rt_log("hdr: the present passes could not be created; the picture goes out unconverted");
        return 0;
    }
    VkShaderModule m = module(c, hdr_encode_spv, sizeof hdr_encode_spv);
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = H.enc_layout};
    VkResult r = m ? vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &H.enc) : VK_ERROR_UNKNOWN;
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("hdr: the present passes could not be created; the picture goes out unconverted");
        return 0;
    }
    return H.ok = 1;
}

int vk_hdr_native_picture(VkFormat f) { return vk_hdr_native() && f == VK_FORMAT_A2B10G10R10_UNORM_PACK32; }

VkImage vk_hdr_encode(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]) {
    VkCtx *c = vk_ctx();
    if (vk_hdr_native_picture(fmt)) {
        static int told;
        if (!told++) rt_log("hdr: native: the game's own HDR10 picture goes to the swapchain as it is");
        return VK_NULL_HANDLE;
    }
    if (!H.tried) init(c);
    if (!H.ok || !img_ensure(c, &H.out, (uint32_t)crop[2], (uint32_t)crop[3], VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return VK_NULL_HANDLE;
    slot %= SLOTS;
    if (H.src_view[slot]) vkDestroyImageView(c->device, H.src_view[slot], NULL);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = src,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &H.src_view[slot]) != VK_SUCCESS) {
        H.src_view[slot] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    Nits n = nits();
    int decode = fmt == VK_FORMAT_R8G8B8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_UNORM;
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, CS,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(cb, H.out.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, ALL, 0, CS,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkDescriptorImageInfo ii = {H.sampler, H.src_view[slot], VK_IMAGE_LAYOUT_GENERAL},
                          oi = {VK_NULL_HANDLE, H.out.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.enc);
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.enc_layout, 0, 2, w);
    struct { int32_t offset[2], size[2]; float p[4]; } pc = {{crop[0], crop[1]}, {crop[2], crop[3]},
                                                            {n.ui, n.peak, decode ? 1.0f : 0.0f, n.gamma}};
    vkCmdPushConstants(cb, H.enc_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, ((uint32_t)crop[2] + 7) / 8, ((uint32_t)crop[3] + 7) / 8, 1);
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, 0, ALL, 0);
    barrier(cb, H.out.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL,
            VK_ACCESS_2_MEMORY_READ_BIT);
    return H.out.img;
}

VkImageView vk_hdr_ui_target(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.tried) init(c);
    if (!H.ok || !img_ensure(c, &H.menu, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return VK_NULL_HANDLE;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return H.menu.view;
}

static int ui_pipeline(VkCtx *c, VkFormat fmt) {
    if (H.ui && H.ui_fmt == fmt) return 1;
    if (H.ui) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        vkDestroyPipeline(c->device, H.ui, NULL);
        H.ui = VK_NULL_HANDLE;
    }
    VkShaderModule vs = module(c, hdr_fullscreen_spv, sizeof hdr_fullscreen_spv),
                   fs = module(c, hdr_ui_spv, sizeof hdr_ui_spv);
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs,
         .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                       .layout = H.ui_layout};
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gi, NULL, &H.ui) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(c->device, vs, NULL);
    if (fs) vkDestroyShaderModule(c->device, fs, NULL);
    if (r != VK_SUCCESS) {
        H.ui = VK_NULL_HANDLE;
        rt_log("hdr: the menu pass could not be created; the menu is not shown");
        return 0;
    }
    H.ui_fmt = fmt;
    return 1;
}

void vk_hdr_ui_compose(VkCommandBuffer cb, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h) {
    VkCtx *c = vk_ctx();
    if (!H.ok || !H.menu.img || !ui_pipeline(c, fmt)) return;
    barrier(cb, H.menu.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.ui);
    VkViewport v = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D s = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
    VkDescriptorImageInfo ii = {H.sampler, H.menu.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_push(H.push, cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.ui_layout, 0, 1, &wr);
    Nits n = nits();
    float pc[4] = {n.ui, n.gamma, 0, 0};
    vkCmdPushConstants(cb, H.ui_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

void vk_hdr_images_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || (!H.out.img && !H.menu.img)) return;
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    for (int i = 0; i < SLOTS; i++)
        if (H.src_view[i]) vkDestroyImageView(c->device, H.src_view[i], NULL), H.src_view[i] = VK_NULL_HANDLE;
    img_free(c, &H.out);
    img_free(c, &H.menu);
    rt_log("vk_hdr: HDR is off: the encode's images are freed");
}

void vk_hdr_release(void) {
    VkCtx *c = vk_ctx();
    for (int i = 0; i < SLOTS; i++)
        if (H.src_view[i]) vkDestroyImageView(c->device, H.src_view[i], NULL), H.src_view[i] = VK_NULL_HANDLE;
    img_free(c, &H.out);
    img_free(c, &H.menu);
    if (H.enc) vkDestroyPipeline(c->device, H.enc, NULL);
    if (H.ui) vkDestroyPipeline(c->device, H.ui, NULL);
    if (H.enc_layout) vkDestroyPipelineLayout(c->device, H.enc_layout, NULL);
    if (H.ui_layout) vkDestroyPipelineLayout(c->device, H.ui_layout, NULL);
    if (H.dsl_enc) vkDestroyDescriptorSetLayout(c->device, H.dsl_enc, NULL);
    if (H.dsl_ui) vkDestroyDescriptorSetLayout(c->device, H.dsl_ui, NULL);
    if (H.sampler) vkDestroySampler(c->device, H.sampler, NULL);
    memset(&H, 0, sizeof H);
}

static float pq_nits(float e) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = powf(e, 1.0f / m2);
    return 10000.0f * powf(fmaxf(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

void vk_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r) {
    float ui = nits().ui;
    static const float m[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};
    for (size_t i = 0; i < px; i++) {
        uint32_t v;
        memcpy(&v, in + 4 * i, 4);
        float c[3] = {(float)(v & 1023), (float)(v >> 10 & 1023), (float)(v >> 20 & 1023)};
        if (a2r) { float t = c[0]; c[0] = c[2]; c[2] = t; }
        for (int k = 0; k < 3; k++) c[k] = pq_nits(c[k] / 1023.0f) / ui;
        for (int k = 0; k < 3; k++) {
            float l = fminf(fmaxf(m[3 * k] * c[0] + m[3 * k + 1] * c[1] + m[3 * k + 2] * c[2], 0.0f), 1.0f);
            out[4 * i + k] = (uint8_t)(powf(l, 1.0f / 2.2f) * 255.0f + 0.5f);
        }
        out[4 * i + 3] = 255;
    }
}

void vk_hdr_luminance_stats(const uint8_t *in, int stride, const int rect[4], int a2r, float out[7]) {
    enum { LO = -24, HI = 14, PER = 64, BINS = (HI - LO) * PER };
    static uint32_t hist[BINS + 1];
    static float lut[1024];
    for (int i = 0; i < 1024; i++) lut[i] = pq_nits((float)i / 1023.0f);
    memset(hist, 0, sizeof hist);
    float lo = 1e30f, hi = 0.0f;
    uint64_t total = 0, dark = 0;
    for (int y = rect[1]; y < rect[1] + rect[3]; y++)
        for (int x = rect[0]; x < rect[0] + rect[2]; x++) {
            uint32_t v;
            memcpy(&v, in + 4 * ((size_t)y * (size_t)stride + (size_t)x), 4);
            float r = lut[v & 1023], g = lut[v >> 10 & 1023], b = lut[v >> 20 & 1023];
            if (a2r) { float t = r; r = b; b = t; }
            float l = 0.2627002120f * r + 0.6779980715f * g + 0.0593017165f * b;
            lo = fminf(lo, l), hi = fmaxf(hi, l);
            dark += l < 0.01f;
            int bin = l > 0.0f ? (int)floorf((log2f(l) - LO) * PER) + 1 : 0;
            hist[bin < 0 ? 0 : bin > BINS ? BINS : bin]++;
            total++;
        }
    static const double q[4] = {0.001, 0.01, 0.5, 0.99};
    for (int k = 0; k < 4; k++) {
        uint64_t want = (uint64_t)(q[k] * (double)total), seen = 0;
        int bin = 0;
        while (bin < BINS && seen + hist[bin] <= want) seen += hist[bin++];
        out[1 + k] = bin == 0 ? 0.0f : exp2f((float)LO + (float)bin / PER);
    }
    out[0] = total ? lo : 0.0f, out[5] = hi, out[6] = total ? (float)((double)dark / (double)total) : 0.0f;
}
