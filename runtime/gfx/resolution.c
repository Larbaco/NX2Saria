// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
//
#ifndef _WIN32
#include <dirent.h>
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define BASE_W 1920
#define BASE_H 1080
#define SCENE_W 1600
#define SCENE_H 900
#define DTA_KIB 113664u
#define SCENE_MAX 4096
#define SHADOW_BASE 960

static struct {
    int decided;
    unsigned ow, oh, sw, sh;
    int scene_set;
    int heapdef_ok;
    int on;
    unsigned shadow;
    double dlss_k;
    double base_k;
    unsigned hw, hh;
    int live;
} R;

static int dlss_dll(void) {
#ifdef _WIN32
    char exe[1024], path[1024 + 32];
    if (!plat_exe_path(exe, sizeof exe)) return 1;
    char *last = plat_last_sep(exe);
    if (!last) return 1;
    snprintf(path, sizeof path, "%.*s/nvngx_dlss.dll", (int)(last - exe), exe);
    return plat_file_exists(path);
#else
    char exe[1024];
    if (!plat_exe_path(exe, sizeof exe)) return 1;
    char *last = plat_last_sep(exe);
    if (!last) return 1;
    *last = 0;
    DIR *d = opendir(exe);
    if (!d) return 1;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));)
        found = !strncmp(e->d_name, "libnvidia-ngx-dlss.so.", 22);
    closedir(d);
    return found;
#endif
}

static void scene_for(double k, unsigned *w, unsigned *h) {
    double sw = R.ow * k, sh = R.oh * k;
    double fit = fmin(1.0, fmin(SCENE_MAX / sw, SCENE_MAX / sh));
    *w = (unsigned)(sw * fit + 0.5);
    *h = (unsigned)(sh * fit + 0.5);
}

static double dlss_share(const char *m) {
    static const struct { const char *name; double k; } dlss[] = {
        {"quality", 2.0 / 3.0}, {"balanced", 0.58}, {"performance", 0.5}, {"ultra_performance", 1.0 / 3.0}};
    static int dll = -1;
    if (!cfg_bool("NX2SARIA_DLAA")) return 0;
    for (int i = 0; i < (int)(sizeof dlss / sizeof dlss[0]); i++)
        if (!strcmp(m, dlss[i].name)) {
            if (dll < 0 && !(dll = dlss_dll()) && res_adjustable()) {
#ifdef _WIN32
                rt_log("resolution: DLSS %s needs nvngx_dlss.dll next to nx2saria.exe, which is missing: the scene follows "
                       "render_scale", m);
#else
                rt_log("resolution: DLSS %s needs libnvidia-ngx-dlss.so.<version> beside the game executable, which is missing: the "
                       "scene follows render_scale", m);
#endif
            }
            return dll ? dlss[i].k : 0;
        }
    return 0;
}

static void decide(void) {
    if (R.decided) return;
    R.decided = 1;
    R.ow = BASE_W;
    R.oh = BASE_H;
    const char *o = cfg_value("NX2SARIA_OUTPUT_RES");
    unsigned w = 0, h = 0;
    char tail;
    if (sscanf(o, "%ux%u%c", &w, &h, &tail) == 2 && w >= 1280 && h >= 720 && w <= 7680 && h <= 4320) {
        R.ow = w & ~1u;
        R.oh = h & ~1u;
    } else {
        rt_log("resolution: NX2SARIA_OUTPUT_RES=%s is not <width>x<height> in 1280x720..7680x4320; 1920x1080", o);
    }
    const char *s = cfg_value("NX2SARIA_RENDER_SCALE");
    double k = 5.0 / 6.0;
    if (strcmp(s, "game")) {
        char *end;
        double pct = strtod(s, &end);
        if (end != s && !*end && pct >= 25 && pct <= 400) {
            k = pct / 100.0;
            R.scene_set = 1;
        } else {
            rt_log("resolution: NX2SARIA_RENDER_SCALE=%s is not game or a percentage 25..400; game", s);
        }
    }
    R.base_k = k;
    R.live = RT_V143 && cfg_bool("NX2SARIA_DLAA") && dlss_dll();
    const char *m = cfg_value("NX2SARIA_DLSS");
    R.dlss_k = dlss_share(m);
    if (R.dlss_k) rt_log("resolution: DLSS %s: the scene at %.0f %% of the output (render_scale ignored)", m, R.dlss_k * 100);
    double sk = R.dlss_k ? R.dlss_k : k;
    if (R.live || R.dlss_k || R.ow * sk > SCENE_MAX || R.oh * sk > SCENE_MAX) R.scene_set = 1;
    scene_for(sk, &R.sw, &R.sh);
    if (R.live) scene_for(fmax(k, 2.0 / 3.0), &R.hw, &R.hh);
    else R.hw = R.sw, R.hh = R.sh;
    const char *sr = cfg_value("NX2SARIA_SHADOW_RES");
    if (strcmp(sr, "game")) {
        unsigned n = (unsigned)atoi(sr);
        if (n >= 256 && n <= 4096 && !(n & 7)) R.shadow = n;
        else rt_log("resolution: NX2SARIA_SHADOW_RES=%s is not game or a size 256..4096 (a multiple of 8); game", sr);
    }
}

static uint64_t shadow_kib(void) {
    return R.shadow > SHADOW_BASE ? ((uint64_t)R.shadow * R.shadow - SHADOW_BASE * SHADOW_BASE) * 8 * 3 / 1024 : 0;
}

int res_adjustable(void) { return rt_patch_set("output") >= 0 && rt_patch_set("scene") >= 0; }

static int bigger(void) {
    decide();
    return res_adjustable() && (R.ow != BASE_W || R.oh != BASE_H || R.scene_set || shadow_kib());
}

uint64_t res_heap_extra_kib(void) {
    if (!res_adjustable()) return wide_heap_extra_kib();
    if (!bigger()) return 0;
    double grow = fmax(0.0, (double)R.ow * R.oh / (BASE_W * BASE_H) - 1.0) +
                  fmax(0.0, (double)R.hw * R.hh / (SCENE_W * SCENE_H) - 1.0);
    uint64_t kib = grow > 0 ? (uint64_t)ceil(DTA_KIB * (1.0 + grow) / 1024.0) * 1024 - DTA_KIB : 0;
    return kib + ((shadow_kib() + 1023) & ~1023ull);
}

uint64_t res_heap_extra_bytes(void) { return ((res_heap_extra_kib() << 10) + 0x1fffff) & ~0x1fffffull; }

typedef struct { uint8_t *p; size_t n; uint32_t keys; } Byml;

static int u32_at(const Byml *b, size_t off, uint32_t *v) {
    if (off + 4 > b->n) return 0;
    memcpy(v, b->p + off, 4);
    return 1;
}

static const char *key_name(const Byml *b, uint32_t k) {
    uint32_t head, at;
    if (!u32_at(b, b->keys, &head) || (head & 0xff) != 0xc2 || k >= head >> 8 || !u32_at(b, b->keys + 4 + 4 * k, &at) ||
        (size_t)b->keys + at >= b->n || !memchr(b->p + (size_t)b->keys + at, 0, b->n - ((size_t)b->keys + at)))
        return NULL;
    return (const char *)b->p + (size_t)b->keys + at;
}

static size_t field(const Byml *b, uint32_t node, const char *name, uint8_t type) {
    uint32_t head;
    if (!u32_at(b, node, &head) || (head & 0xff) != 0xc1) return 0;
    for (uint32_t i = 0; i < head >> 8; i++) {
        uint32_t e;
        if (!u32_at(b, node + 4 + 8 * (size_t)i, &e)) return 0;
        const char *k = key_name(b, e & 0xffffff);
        if (k && !strcmp(k, name)) return e >> 24 == type ? node + 8 + 8 * (size_t)i : 0;
    }
    return 0;
}

static size_t heap_size_field(const Byml *b, uint32_t heaps, const char *heap) {
    size_t at = field(b, heaps, heap, 0xc1);
    uint32_t node;
    return at && u32_at(b, at, &node) ? field(b, node, "Size", 0xd1) : 0;
}

int res_heapdef_build(const void *in, size_t n, void **out, size_t *out_n) {
    uint64_t extra = res_heap_extra_kib();
    *out = NULL;
    if (!extra) return 0;
    if (ZSTD_getDictID_fromFrame(in, n)) {
        rt_log("resolution: the HeapDef table needs a zstd dictionary; the game keeps its own resolution");
        return 0;
    }
    unsigned long long raw = ZSTD_getFrameContentSize(in, n);
    if (raw == ZSTD_CONTENTSIZE_UNKNOWN || raw == ZSTD_CONTENTSIZE_ERROR || raw > (64u << 20)) {
        rt_log("resolution: the HeapDef table is not a zstd frame with a size; the game keeps its own resolution");
        return 0;
    }
    Byml b = {malloc((size_t)raw), (size_t)raw, 0};
    if (!b.p) return 0;
    size_t got = ZSTD_decompress(b.p, b.n, in, n);
    uint32_t root, heaps = 0;
    uint16_t ver = 0;
    if (!ZSTD_isError(got) && got == b.n && b.n >= 16) memcpy(&ver, b.p + 2, 2);
    size_t heaps_at = 0, dta_at = 0, gfx_at = 0;
    if (ver >= 2 && ver <= 7 && !memcmp(b.p, "YB", 2) && u32_at(&b, 4, &b.keys) && u32_at(&b, 12, &root) &&
        (heaps_at = field(&b, root, "Heaps", 0xc1)) && u32_at(&b, heaps_at, &heaps)) {
        dta_at = heap_size_field(&b, heaps, "DynamicTextureAllocatorMem");
        gfx_at = heap_size_field(&b, heaps, "Graphics");
    }
    if (!dta_at || !gfx_at) {
        rt_log("resolution: the HeapDef table has no DynamicTextureAllocatorMem / Graphics sizes; the game keeps its own "
               "resolution");
        free(b.p);
        return 0;
    }
    int32_t dta, gfx;
    memcpy(&dta, b.p + dta_at, 4);
    memcpy(&gfx, b.p + gfx_at, 4);
    int32_t dta2 = dta + (int32_t)extra, gfx2 = gfx + (int32_t)extra;
    memcpy(b.p + dta_at, &dta2, 4);
    memcpy(b.p + gfx_at, &gfx2, 4);
    size_t cap = ZSTD_compressBound(b.n);
    void *z = malloc(cap);
    size_t zn = z ? ZSTD_compress(z, cap, b.p, b.n, 9) : 0;
    free(b.p);
    if (!z || ZSTD_isError(zn)) {
        free(z);
        return 0;
    }
    *out = z;
    *out_n = zn;
    rt_log("resolution: HeapDef DynamicTextureAllocatorMem %d -> %d KiB, Graphics %d -> %d KiB", dta, dta2, gfx, gfx2);
    return 1;
}

void res_heapdef_served(void) { R.heapdef_ok = 1; }
int res_heapdef_ok(void) { return R.heapdef_ok; }

static void shadow_init(uint8_t *page) {
    if (!R.shadow) {
        rt_log("resolution: sun shadow map sized by the game (NX2SARIA_SHADOW_RES=game)");
        return;
    }
    int set = rt_patch_set("shadow");
    if (set < 0) {
        rt_log("resolution: the generated C has no shadow patch set: the game sizes its shadow map");
        return;
    }
    int32_t n = (int32_t)R.shadow;
    memcpy(page + 92, &n, 4);
    rt_patch_switch(set, 1);
    rt_log("resolution: sun shadow map %ux%u (NX2SARIA_SHADOW_RES)", R.shadow, R.shadow);
}

static uint64_t pack(unsigned w, unsigned h) { return (uint64_t)h << 32 | w; }

static void res_shadow_range(void) {
    int pct = cfg_int("NX2SARIA_SHADOW_RANGE"), set = rt_patch_set("cascade");
    if (pct == 100) return;
    if (set < 0) {
        rt_log("resolution: the generated C has no cascade patch set; shadow_range stays 100");
        return;
    }
    float k = (float)(pct < 50 ? 50 : pct > 400 ? 400 : pct) / 100.0f, d[4] = {20 * k, 40 * k, 80 * k, 160 * k};
    memcpy((uint8_t *)rt_patch_page() + 64, d, sizeof d);
    rt_patch_switch(set, 1);
    rt_log("resolution: shadow cascades end at %g, %g, %g and %g m", d[0], d[1], d[2], d[3]);
}

void res_init(void) {
    res_fov_live();
    res_shadow_range();
    if (!res_adjustable()) {
        rt_log("resolution: the game's own (this build has no output and scene patch sets; output_res and render_scale "
               "do nothing)");
        return;
    }
    if (!bigger()) {
        rt_log("resolution: the game's own (output 1920x1080, scene 1600x900)");
        shadow_init(rt_patch_page());
        return;
    }
    int output = rt_patch_set("output"), scene = rt_patch_set("scene");
    fs_init();
    if (res_heap_extra_kib() && !R.heapdef_ok) {
        rt_log("resolution: no grown HeapDef table; the game keeps its own resolution");
        return;
    }
    uint8_t *page = rt_patch_page();
    double k = RT_V143 ? (double)R.ow / BASE_W : 1.0;
    int scene_on = R.scene_set || (!RT_V143 && (R.ow != BASE_W || R.oh != BASE_H));
    float sc[2] = {(float)(R.sw / k), (float)(R.sh / k)};
    uint64_t out = pack(R.ow, R.oh), none = 0;
    memcpy(page + 16, sc, sizeof sc);
    memcpy(page + 32, &out, 8);
    memcpy(page + 40, &none, 8);
    memcpy(page + 48, &out, 8);
    rt_patch_switch(output, R.ow != BASE_W || R.oh != BASE_H);
    rt_patch_switch(scene, scene_on);
    R.on = 1;
    if (R.live) g_fnprof = 1;
    double ratio = (double)R.ow / R.oh, game = (double)BASE_W / BASE_H;
    int aspect = rt_patch_set("aspect");
    if (!res_is_16x9(R.ow, R.oh)) {
        if (aspect < 0) {
            rt_log("resolution: output %ux%u is not 16:9 and this game version has no camera aspect patch; the view "
                   "is stretched", R.ow, R.oh);
        } else {
            float r = (float)ratio;
            memcpy(page + 56, &r, sizeof r);
            rt_patch_switch(aspect, 1);
            rt_log("resolution: camera aspect ratio %.4f (16:9 is %.4f)", ratio, game);
        }
    } else {
        rt_patch_switch(aspect, 0);
    }
    shadow_init(page);
    rt_log("resolution: output %ux%u, scene %ux%u%s; texture heap +%llu KiB", R.ow, R.oh, R.sw, R.sh,
           scene_on ? "" : " (the game's own 5/6 of the output)", (unsigned long long)res_heap_extra_kib());
}

double wide_dlss_share(void);

float res_dlss_lod_bias(void) {
    decide();
    double k = res_adjustable() ? R.dlss_k : wide_dlss_share();
    return k > 0 && k < 1 ? (float)log2(k) : 0.0f;
}

int res_is_16x9(unsigned w, unsigned h) {
    double rel = ((double)w / h) / ((double)BASE_W / BASE_H);
    return rel >= 0.99 && rel <= 1.01;
}

int res_wants_wide_overlay(const unsigned out[2]) {
    return !res_is_16x9(out[0], out[1]) && rt_patch_set("aspect") >= 0;
}

#define FOV_MIN 70
#define FOV_MAX 140
static _Atomic float fov_factor = 1.0f;

float res_fov_factor(void) { return atomic_load_explicit(&fov_factor, memory_order_relaxed); }

void res_fov_live(void) {
    static int set = -2, shown = -1;
    if (set == -2) set = rt_patch_set("fov");
    int pct = cfg_int("NX2SARIA_FOV");
    pct = pct < FOV_MIN ? FOV_MIN : pct > FOV_MAX ? FOV_MAX : pct;
    if (pct == shown) return;
    shown = pct;
    if (set < 0) {
        if (pct != 100) rt_log("fov: unsupported, requested %d%%", pct);
        return;
    }
    static int allowed = -1;
    if (allowed < 0) allowed = !RT_V111 || (rt_patch_enabled(set) && rt_patch_enabled(rt_patch_set("fov_menu")));
    if (!allowed) {
        rt_patch_switch(set, 0);
        rt_log("fov: off, requested %d%%, patch disabled or replaced by an ExeFS mod", pct);
        return;
    }
    float k = pct / 100.0f;
    if (RT_V111) atomic_store_explicit(&fov_factor, k, memory_order_relaxed);
    else memcpy((uint8_t *)rt_patch_page() + 60, &k, sizeof k);
    rt_patch_switch(set, pct != 100);
    rt_log("fov: %s, %d%%%s", pct == 100 ? "off" : "on", pct,
           RT_V111 && pct != 100 ? " (excludes non-player cameras, pause/map/system menus, active events and first-person aiming)" : "");
}

void res_wanted(unsigned out[2]) {
    decide();
    out[0] = R.ow, out[1] = R.oh;
}

#define SET_RENDER_SIZE 0x7000585bd0ull
static uint64_t renderer;
static volatile int size_pending;

void res_fn_hit(uint64_t addr, uint64_t x0) {
    if (addr != SET_RENDER_SIZE) return;
    renderer = x0;
    g_fnprof = 0;
    rt_log("resolution: renderer at %#llx; the scene size can change while the game runs", (unsigned long long)x0);
}

void res_present(Cpu *c) {
    static unsigned waited;
    if (g_fnprof && !renderer && ++waited == 1800) {
        g_fnprof = 0;
        rt_log("resolution: the game's render size setup was not seen; DLSS modes change at the next start");
    }
    if (!size_pending || !renderer) return;
    size_pending = 0;
    Cpu keep = *c;
    rt_call(c, SET_RENDER_SIZE, 2, (uint64_t)renderer, (uint64_t)1);
    memcpy(c->x, keep.x, sizeof c->x);
    memcpy(c->v, keep.v, sizeof c->v);
    c->nzcv = keep.nzcv, c->fpsr = keep.fpsr;
    rt_log("resolution: the game set the scene size %ux%u", R.sw, R.sh);
}

int res_dlss_mode(const char *m) {
    decide();
    double k = dlss_share(m);
    if (k == R.dlss_k || !R.on) return 0;
    if (!RT_V143 || !renderer) {
        rt_log("resolution: DLSS %s takes effect at the next start (the game set its render size already)", m);
        return 0;
    }
    R.dlss_k = k;
    unsigned w, h;
    scene_for(k ? k : R.base_k, &w, &h);
    if (w == R.sw && h == R.sh) return 0;
    double s = RT_V143 ? (double)R.ow / BASE_W : 1.0;
    float sc[2] = {(float)(w / s), (float)(h / s)};
    memcpy((uint8_t *)rt_patch_page() + 16, sc, sizeof sc);
    R.sw = w, R.sh = h;
    size_pending = 1;
    rt_log("resolution: DLSS %s: scene %ux%u", m, w, h);
    return 1;
}

void res_sizes(unsigned out[2], unsigned scene[2]) {
    decide();
    out[0] = R.on ? R.ow : BASE_W, out[1] = R.on ? R.oh : BASE_H;
    scene[0] = R.on ? R.sw : SCENE_W, scene[1] = R.on ? R.sh : SCENE_H;
}
