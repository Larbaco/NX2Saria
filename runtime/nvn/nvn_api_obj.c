// Contains code Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "prof.h"
#include "platform/platform.h"

NVN_API(nvnTexturePoolInitialize) {
    *(dpool_t *)arg_p(c, 0) = (dpool_t){arg_p(c, 1), (int64_t)arg_x(c, 2), (int32_t)arg_x(c, 3)};
    NVN_LOG("TexturePool n=%d", (int)arg_x(c, 3));
    ret_x(c, 1);
}
NVN_API(nvnTexturePoolFinalize) { (void)c; }
NVN_API(nvnTexturePoolRegisterTexture) {
    gfx_backend()->texture_register((int)arg_x(c, 1), arg_p(c, 2), arg_p(c, 3));
}
NVN_API(nvnSamplerPoolInitialize) {
    *(dpool_t *)arg_p(c, 0) = (dpool_t){arg_p(c, 1), (int64_t)arg_x(c, 2), (int32_t)arg_x(c, 3)};
    NVN_LOG("SamplerPool n=%d", (int)arg_x(c, 3));
    ret_x(c, 1);
}
NVN_API(nvnSamplerPoolFinalize) { (void)c; }
NVN_API(nvnSamplerPoolRegisterSampler) {
    gfx_backend()->sampler_register((int)arg_x(c, 1), &((const samp_t *)arg_p(c, 2))->p);
}

#define TB(c) ((tex_builder_t *)arg_p(c, 0))
NVN_API(nvnTextureBuilderSetDefaults) {
    tex_builder_t *b = TB(c);
    NVNdevice *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->d.swizzle_r = 2;
    b->d.swizzle_g = 3;
    b->d.swizzle_b = 4;
    b->d.swizzle_a = 5;
    b->d.target = NVN_TEXTURE_TARGET_2D;
    b->d.width = b->d.height = b->d.depth = b->d.levels = 1;
}
NVN_API(nvnTextureBuilderSetDevice) { TB(c)->dev = arg_p(c, 1); }
NVN_API(nvnTextureBuilderSetFlags) { TB(c)->d.flags = (int)arg_x(c, 1); }
NVN_API(nvnTextureBuilderSetTarget) { TB(c)->d.target = (int)arg_x(c, 1); }
NVN_API(nvnTextureBuilderSetWidth) { TB(c)->d.width = (uint32_t)arg_x(c, 1); }
NVN_API(nvnTextureBuilderSetHeight) { TB(c)->d.height = (uint32_t)arg_x(c, 1); }
NVN_API(nvnTextureBuilderSetDepth) { TB(c)->d.depth = (uint32_t)arg_x(c, 1); }
NVN_API(nvnTextureBuilderSetSize1D) {
    tex_builder_t *b = TB(c);
    b->d.width = (uint32_t)arg_x(c, 1);
    b->d.height = b->d.depth = 1;
}
NVN_API(nvnTextureBuilderSetSize2D) {
    tex_builder_t *b = TB(c);
    b->d.width = (uint32_t)arg_x(c, 1);
    b->d.height = (uint32_t)arg_x(c, 2);
    b->d.depth = 1;
    static int diag = -1, n;
    if (diag < 0) diag = cfg_get("NX2SARIA_LOG_RT_SIZES") != NULL;
    if (diag && n < 200 && b->d.width * 9 == b->d.height * 16 && b->d.width >= 320) {
        char at[160], up[160];
        n++;
        rt_log("nvn: SetSize2D %ux%u from %s, caller %s", b->d.width, b->d.height, ldr_describe(c->x[30], at, sizeof at),
               ldr_describe(c->x[29] ? *(uint64_t *)(uintptr_t)(c->x[29] + 8) : 0, up, sizeof up));
    }
}
NVN_API(nvnTextureBuilderSetSize3D) {
    tex_builder_t *b = TB(c);
    b->d.width = (uint32_t)arg_x(c, 1);
    b->d.height = (uint32_t)arg_x(c, 2);
    b->d.depth = (uint32_t)arg_x(c, 3);
}
NVN_API(nvnTextureBuilderSetLevels) { TB(c)->d.levels = (uint32_t)arg_x(c, 1); }
NVN_API(nvnTextureBuilderSetFormat) { TB(c)->d.format = (int)arg_x(c, 1); }
NVN_API(nvnTextureBuilderSetDepthStencilMode) {
    if ((uint32_t)arg_x(c, 1) != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH)
        rt_fatal("nvn: stencil-plane texture sampling is not implemented");
}
NVN_API(nvnTextureBuilderSetSwizzle) {
    uint32_t s[4] = {(uint32_t)arg_x(c, 1), (uint32_t)arg_x(c, 2), (uint32_t)arg_x(c, 3), (uint32_t)arg_x(c, 4)};
    for (int i = 0; i < 4; i++)
        if (s[i] > 5) rt_fatal("nvn: invalid texture swizzle %u", s[i]);
    nvn_texdesc *d = &TB(c)->d;
    d->swizzle_r = s[0];
    d->swizzle_g = s[1];
    d->swizzle_b = s[2];
    d->swizzle_a = s[3];
}
NVN_API(nvnTextureBuilderSetSamples) { TB(c)->d.samples = (int)arg_x(c, 1); }
NVN_API(nvnTextureBuilderSetStride) { TB(c)->d.stride = (uint32_t)arg_x(c, 1); }
NVN_API(nvnTextureBuilderSetStorage) {
    tex_builder_t *b = TB(c);
    b->pool = arg_p(c, 1);
    b->off = (int64_t)arg_x(c, 2);
}
NVN_API(nvnTextureBuilderSetPackagedTextureLayout) {
    tex_builder_t *b = TB(c);
    const NVNpackagedTextureLayout *l = arg_p(c, 1);
    b->has_layout = l != NULL;
    if (l) memcpy(b->layout, l->layout, 8);
    b->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NVN_API(nvnTextureBuilderSetPackagedTextureData) { TB(c)->pkg_data = arg_p(c, 1); }
NVN_API(nvnTextureBuilderGetPackagedTextureData) { ret_x(c, (uintptr_t)TB(c)->pkg_data); }
static uint64_t storage_class(const nvn_texdesc *d) { return d->target == NVN_TEXTURE_TARGET_BUFFER ? 0 : 1; }
NVN_API(nvnTextureBuilderGetStorageClass) { ret_x(c, storage_class(&TB(c)->d)); }
NVN_API(nvnTextureBuilderGetFlags) { ret_x(c, (uint32_t)TB(c)->d.flags); }
NVN_API(nvnTextureBuilderGetStorageSize) {
    if (nvn_no_object(c, "nvnTextureBuilderGetStorageSize")) return;
    nvn_texlayout L;
    nvn_tex_layout(&TB(c)->d, &L);
    if (!L.bpb) rt_log("nvn: TextureBuilderGetStorageSize: unknown format %#x", TB(c)->d.format);
    ret_x(c, L.size);
}
NVN_API(nvnTextureBuilderGetStorageAlignment) {
    if (nvn_no_object(c, "nvnTextureBuilderGetStorageAlignment")) return;
    nvn_texlayout L;
    nvn_tex_layout(&TB(c)->d, &L);
    ret_x(c, L.align);
}

static nvn_hmap live = {.klen = 8};
static PlatMutex live_lock = PLAT_MUTEX_INIT;

static void live_put(const tex_t *o) {
    if (!o->mem) return;
    uint64_t k = (uintptr_t)o->mem;
    tex_t *copy = malloc(sizeof *copy);
    if (!copy) rt_fatal("nvn: out of memory for the live texture table");
    *copy = *o;
    plat_mutex_lock(&live_lock);
    tex_t *old = nvn_hm_put(&live, &k, copy);
    plat_mutex_unlock(&live_lock);
    free(old);
}

static void live_del(const tex_t *o) {
    if (!o->mem) return;
    uint64_t k = (uintptr_t)o->mem;
    plat_mutex_lock(&live_lock);
    tex_t *cur = nvn_hm_get(&live, &k);
    if (cur && cur->serial == o->serial) nvn_hm_del(&live, &k);
    else cur = NULL;
    plat_mutex_unlock(&live_lock);
    free(cur);
}

int nvn_tex_live_at(const void *mem, tex_t *out) {
    if (!mem) return 0;
    uint64_t k = (uintptr_t)mem;
    plat_mutex_lock_shared(&live_lock);
    const tex_t *cur = nvn_hm_get(&live, &k);
    if (cur) *out = *cur;
    plat_mutex_unlock_shared(&live_lock);
    return cur != NULL;
}

static void tex_release(tex_t *o) {
    live_del(o);
    gfx_backend()->texture_release(o);
}

NVN_API(nvnTextureInitialize) {
    tex_t *o = arg_p(c, 0);
    const tex_builder_t *s = arg_p(c, 1);
    if (cfg_get("NX2SARIA_LOG_TEX_FLAGS")) {
        static uint64_t seen[256]; static int n;
        uint64_t key = (uint64_t)(uint32_t)s->d.flags << 32 | (uint32_t)s->d.format;
        int known = 0;
        for (int i = 0; i < n; i++) known |= seen[i] == key;
        if (!known && n < 256) { seen[n++] = key; rt_log("nvn: texture flags %#x format %#x %ux%u", s->d.flags, s->d.format, s->d.width, s->d.height); }
    }
    if (o->magic == NVN_TEX_MAGIC && o->serial) {
        NVN_LOG("texture %p initialized again without Finalize (serial %llu released)", (void *)o,
                (unsigned long long)o->serial);
        tex_release(o);
    }
    memset(o, 0, sizeof *o);
    o->magic = NVN_TEX_MAGIC;
    o->d = s->d;
    o->pool = s->pool;
    o->pool_flags = s->pool ? (uint16_t)s->pool->flags : 0;
    o->off = s->off;
    o->has_layout = s->has_layout;
    memcpy(o->layout, s->layout, 8);
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    if (s->pkg_data) {
        if (!o->mem || nvn_gpu_only((uintptr_t)o->mem))
            rt_fatal("nvn: packaged texture data without CPU-visible storage");
        uint64_t room = s->pool->span > s->pool->size ? s->pool->span : s->pool->size;
        if (s->off < 0 || (uint64_t)s->off > room || o->L.size > room - (uint64_t)s->off)
            rt_fatal("nvn: packaged texture data of %#llx bytes at %#llx does not fit its pool (%#llx bytes)",
                     (unsigned long long)o->L.size, (unsigned long long)s->off, (unsigned long long)room);
        memcpy(o->mem, s->pkg_data, (size_t)o->L.size);
        nvn_guest_written((uintptr_t)o->mem, o->L.size);
    }
    o->serial = nvn_new_serial();
    live_put(o);
    NVN_LOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void *)o, o->d.width, o->d.height,
            o->d.depth, o->d.levels, o->d.format, o->d.target, o->d.flags, (unsigned long long)o->L.size);
    ret_x(c, 1);
}
NVN_API(nvnTextureFinalize) {
    tex_t *o = arg_p(c, 0);
    if (o->magic == NVN_TEX_MAGIC && o->serial) tex_release(o);
    o->magic = 0;
}
#define CTX(c) ((const tex_t *)arg_p(c, 0))
NVN_API(nvnTextureGetStorageClass) { ret_x(c, storage_class(&CTX(c)->d)); }
NVN_API(nvnTextureGetFlags) { ret_x(c, (uint32_t)CTX(c)->d.flags); }
NVN_API(nvnTextureGetWidth) { ret_x(c, CTX(c)->d.width); }
NVN_API(nvnTextureGetHeight) { ret_x(c, CTX(c)->d.height); }
NVN_API(nvnTextureGetDepth) { ret_x(c, CTX(c)->d.depth); }
NVN_API(nvnTextureGetStorageSize) { ret_x(c, (uint32_t)(int)CTX(c)->L.size); }
NVN_API(nvnTextureCompare) {
    const tex_t *x = arg_p(c, 0), *y = arg_p(c, 1);
    ret_x(c, !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem);
}
NVN_API(nvnTextureGetRawStorageClass) { ret_x(c, 1); }

#define VW(c) ((view_t *)arg_p(c, 0))
NVN_API(nvnTextureViewSetDefaults) { memset(arg_p(c, 0), 0, sizeof(view_t)); }
NVN_API(nvnTextureViewSetLevels) {
    uint32_t base = (uint32_t)arg_x(c, 1), n = (uint32_t)arg_x(c, 2);
    if (base > 255 || n > 255) rt_fatal("nvn: texture view levels %d/%d", (int)base, (int)n);
    view_t *v = VW(c);
    v->set_levels = 1;
    v->base_level = (uint8_t)base;
    v->nlevels = (uint8_t)n;
}
NVN_API(nvnTextureViewSetLayers) {
    uint32_t first = (uint32_t)arg_x(c, 1), n = (uint32_t)arg_x(c, 2);
    if (first > 65535 || n > 65535) rt_fatal("nvn: texture view layers %d/%d", (int)first, (int)n);
    view_t *v = VW(c);
    v->set_layers = 1;
    v->min_layer = (uint16_t)first;
    v->nlayers = (uint16_t)n;
}
NVN_API(nvnTextureViewSetFormat) {
    uint32_t f = (uint32_t)arg_x(c, 1);
    if (f > 65535) rt_fatal("nvn: texture view format %d", (int)f);
    view_t *v = VW(c);
    v->set_format = 1;
    v->format = (uint16_t)f;
}
NVN_API(nvnTextureViewSetTarget) {
    uint32_t t = (uint32_t)arg_x(c, 1);
    if (t > 255) rt_fatal("nvn: texture view target %d", (int)t);
    view_t *v = VW(c);
    v->set_target = 1;
    v->target = (uint8_t)t;
}
NVN_API(nvnTextureViewGetLayers) {
    const view_t *v = arg_p(c, 0);
    *(int *)arg_p(c, 1) = v->min_layer;
    *(int *)arg_p(c, 2) = v->nlayers;
    ret_x(c, v->set_layers);
}

uint64_t nvn_tex_view_offset(const tex_t *t, const view_t *v) {
    if (!v) return 0;
    uint64_t off = v->set_levels ? nvn_tex_level_offset(&t->d, v->base_level) : 0;
    if (v->set_layers) off += (uint64_t)v->min_layer * t->L.layer_size;
    return off;
}
NVN_API(nvnTextureGetViewOffset) { ret_x(c, nvn_tex_view_offset(arg_p(c, 0), arg_p(c, 1))); }

static void texel_strides(const tex_t *o, const NVNcopyRegion *r, uint64_t *row, uint64_t *img, const char *what) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    if (!bpb) rt_fatal("nvn: %s: unknown format %#x", what, o->d.format);
    uint64_t wb = (uint64_t)(((uint32_t)r->width + bw - 1) / bw) * bpb, hb = ((uint32_t)r->height + bh - 1) / bh;
    if (!*row) *row = wb;
    if (!*img) *img = *row * hb;
    if (*row < wb || *img < *row * hb) rt_fatal("nvn: %s: stride smaller than the region", what);
}
static void read_texels(Cpu *c, int strided) {
    const tex_t *o = arg_p(c, 0);
    const NVNcopyRegion *r = arg_p(c, 2);
    uint64_t row = strided ? arg_x(c, 4) : 0, img = strided ? arg_x(c, 5) : 0;
    texel_strides(o, r, &row, &img, "ReadTexels");
    NVN_BLOCKING();
    gfx_backend()->read_texels(o, arg_p(c, 1), r, arg_p(c, 3), row, img);
}
static void write_texels(Cpu *c, int strided) {
    const tex_t *o = arg_p(c, 0);
    const view_t *v = arg_p(c, 1);
    const NVNcopyRegion *r = arg_p(c, 2);
    uint64_t row = strided ? arg_x(c, 4) : 0, img = strided ? arg_x(c, 5) : 0;
    texel_strides(o, r, &row, &img, "WriteTexels");
    if (v && v->set_format && v->format != o->d.format) rt_fatal("nvn: WriteTexels: format-reinterpreting view not supported");
    NVN_BLOCKING();
    gfx_backend()->write_texels(o, v, r, arg_p(c, 3), row, img);
}
NVN_API(nvnTextureReadTexels) { read_texels(c, 0); }
NVN_API(nvnTextureReadTexelsStrided) { read_texels(c, 1); }
NVN_API(nvnTextureWriteTexels) { write_texels(c, 0); }
NVN_API(nvnTextureWriteTexelsStrided) { write_texels(c, 1); }
NVN_API(nvnTextureFlushTexels) {
    const tex_t *t = arg_p(c, 0);
    if (t && t->mem && !nvn_gpu_only((uintptr_t)t->mem)) nvn_guest_written((uintptr_t)t->mem, t->L.size);
}
NVN_API(nvnTextureInvalidateTexels) { (void)c; }

#define SB(c) (&((samp_builder_t *)arg_p(c, 0))->p)
NVN_API(nvnSamplerBuilderSetDefaults) {
    samp_builder_t *b = arg_p(c, 0);
    NVNdevice *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    samp_par *o = &b->p;
    o->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR;
    o->magf = NVN_MAG_FILTER_LINEAR;
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT;
    o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = -1000.f;
    o->lod_max = 1000.f;
    o->aniso = 1.f;
}
NVN_API(nvnSamplerBuilderSetDevice) { ((samp_builder_t *)arg_p(c, 0))->dev = arg_p(c, 1); }
NVN_API(nvnSamplerBuilderSetMinMagFilter) {
    samp_par *p = SB(c);
    p->minf = (uint8_t)arg_x(c, 1);
    p->magf = (uint8_t)arg_x(c, 2);
}
NVN_API(nvnSamplerBuilderSetWrapMode) {
    samp_par *p = SB(c);
    p->ws = (uint8_t)arg_x(c, 1);
    p->wt = (uint8_t)arg_x(c, 2);
    p->wr = (uint8_t)arg_x(c, 3);
}
NVN_API(nvnSamplerBuilderSetLodClamp) {
    samp_par *p = SB(c);
    p->lod_min = arg_s(c, 0);
    p->lod_max = arg_s(c, 1);
}
NVN_API(nvnSamplerBuilderSetLodBias) { SB(c)->lod_bias = arg_s(c, 0); }
NVN_API(nvnSamplerBuilderSetCompare) {
    samp_par *p = SB(c);
    p->cmp_mode = (uint8_t)arg_x(c, 1);
    p->cmp_func = (uint8_t)arg_x(c, 2);
}
NVN_API(nvnSamplerBuilderSetBorderColor) { memcpy(SB(c)->border, arg_p(c, 1), 16); }
NVN_API(nvnSamplerBuilderSetMaxAnisotropy) { SB(c)->aniso = arg_s(c, 0); }
NVN_API(nvnSamplerBuilderSetLodSnap) { SB(c)->lod_snap = arg_s(c, 0); }
NVN_API(nvnSamplerBuilderGetMinMagFilter) {
    const samp_par *p = SB(c);
    *(uint32_t *)arg_p(c, 1) = p->minf;
    *(uint32_t *)arg_p(c, 2) = p->magf;
}
NVN_API(nvnSamplerBuilderGetWrapMode) {
    const samp_par *p = SB(c);
    *(uint32_t *)arg_p(c, 1) = p->ws;
    *(uint32_t *)arg_p(c, 2) = p->wt;
    *(uint32_t *)arg_p(c, 3) = p->wr;
}
NVN_API(nvnSamplerBuilderSetReductionFilter) {
    uint32_t r = (uint32_t)arg_x(c, 1);
    if (r > 2) rt_fatal("nvn: invalid sampler reduction %u", r);
    SB(c)->reduction = (uint8_t)r;
}
NVN_API(nvnSamplerBuilderGetReductionFilter) { ret_x(c, SB(c)->reduction); }
NVN_API(nvnSamplerGetReductionFilter) { ret_x(c, ((const samp_t *)arg_p(c, 0))->p.reduction); }
NVN_API(nvnSamplerInitialize) {
    samp_t *o = arg_p(c, 0);
    o->magic = NVN_SAMP_MAGIC;
    o->p = ((const samp_builder_t *)arg_p(c, 1))->p;
    ret_x(c, 1);
}
NVN_API(nvnSamplerFinalize) { ((samp_t *)arg_p(c, 0))->magic = 0; }

NVN_API(nvnProgramInitialize) {
    prog_t *o = arg_p(c, 0);
    memset(o, 0, sizeof *o);
    o->magic = NVN_PROG_MAGIC;
    ret_x(c, 1);
}
static void prog_release(prog_t *o) {
    if (o->serial) gfx_backend()->program_release(o);
    o->serial = 0;
}
NVN_API(nvnProgramFinalize) {
    prog_t *o = arg_p(c, 0);
    prog_release(o);
    o->magic = 0;
}
#define PROG_MADE_N 16384
static uint64_t prog_made_serial[PROG_MADE_N], prog_made_at[PROG_MADE_N];
uint64_t nvn_prog_made_ns(uint64_t serial) {
    uint32_t i = (uint32_t)serial & (PROG_MADE_N - 1);
    return __atomic_load_n(&prog_made_serial[i], __ATOMIC_RELAXED) == serial ? __atomic_load_n(&prog_made_at[i], __ATOMIC_RELAXED) : 0;
}

static int prog_set_shaders(prog_t *o, int n, const NVNshaderData *sd) {
    if (n < 0 || n > 6 || (n && !sd)) {
        rt_log("nvn: ProgramSetShaders with %d stages at %p", n, (const void *)sd);
        return 0;
    }
    prog_release(o);
    o->nstage = (uint32_t)n;
    if (n) memcpy(o->sd, sd, (size_t)n * sizeof *sd);
    o->serial = nvn_new_serial();
    static int lead = -1;
    if (lead < 0) lead = cfg_get("NX2SARIA_PROG_LEAD") != NULL && strcmp(cfg_get("NX2SARIA_PROG_LEAD"), "0") != 0;
    if (lead) {
        uint32_t i = (uint32_t)o->serial & (PROG_MADE_N - 1);
        __atomic_store_n(&prog_made_at[i], plat_now_ns(), __ATOMIC_RELAXED);
        __atomic_store_n(&prog_made_serial[i], o->serial, __ATOMIC_RELAXED);
    }
    NVN_LOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)o, n, n ? (unsigned long long)sd[0].data : 0ull);
    return 1;
}
static void shader_dump(const prog_t *o, int n, const NVNshaderData *sd);
NVN_API(nvnProgramSetShaders) {
    NVN_BLOCKING();
    int ok = prog_set_shaders(arg_p(c, 0), (int)arg_x(c, 1), arg_p(c, 2));
    shader_dump(arg_p(c, 0), (int)arg_x(c, 1), arg_p(c, 2));
    ret_x(c, ok);
}
typedef struct { NVNbufferAddress data; const void *control; uint64_t reserved; } shader_data_ext;
_Static_assert(sizeof(shader_data_ext) == 0x18, "NVNshaderDataExt stride");
NVN_API(nvnProgramSetShadersExt) {
    NVN_BLOCKING();
    prog_t *o = arg_p(c, 0);
    int n = (int)arg_x(c, 1);
    const shader_data_ext *sd = arg_p(c, 2);
    if (n < 0 || n > 6 || (n && !sd)) {
        rt_log("nvn: ProgramSetShadersExt with %d stages at %p", n, (const void *)sd);
        ret_x(c, 0);
        return;
    }
    NVNshaderData plain[6];
    for (int i = 0; i < n; i++) {
        if (sd[i].reserved)
            rt_fatal("nvn: ProgramSetShadersExt: unknown non-zero field %#llx in stage %d", (unsigned long long)sd[i].reserved, i);
        plain[i] = (NVNshaderData){sd[i].data, sd[i].control};
    }
    int ok = prog_set_shaders(o, n, plain);
    shader_dump(o, n, plain);
    ret_x(c, ok);
}

typedef struct { uint32_t magic; int32_t n; NVNdevice *dev; const NVNshaderData *sd; } prog_builder_t;
#define NVN_PROG_BUILDER_MAGIC 0x50524742u

static void shader_dump(const prog_t *o, int n, const NVNshaderData *sd) {
    for (int i = 0; sd && i < n && i < 8; i++) {
        static uint32_t seen;
        const uint8_t *ctl = (const uint8_t *)sd[i].control;
        uint32_t magic, st;
        if (!ctl) continue;
        memcpy(&magic, ctl, 4), memcpy(&st, ctl + 0x54, 4);
        st >>= 16;
        if (magic == 0x98761234u && st < 32 && !(__atomic_fetch_or(&seen, 1u << st, __ATOMIC_RELAXED) >> st & 1))
            rt_log("nvn: first NVN2 program with stage %u (%d stages in the program)", st, n);
    }
    const char *dir = cfg_get("NX2SARIA_SHADER_DUMP");
    if (!dir || !*dir) return;
    static volatile int32_t count;
    int k = __atomic_fetch_add(&count, 1, __ATOMIC_RELAXED);
    if (k >= 20000) return;
    if (k == 0) {
        const uint64_t *raw = (const uint64_t *)sd;
        rt_log("nvn: shader dump: first program %p, %d stages, raw shader data %#llx %#llx %#llx %#llx %#llx %#llx",
               (const void *)o, n, (unsigned long long)raw[0], (unsigned long long)raw[1], (unsigned long long)raw[2],
               (unsigned long long)raw[3], (unsigned long long)raw[4], (unsigned long long)raw[5]);
    }
    for (int i = 0; i < n; i++) {
        char path[1100];
        snprintf(path, sizeof path, "%s/p%04llu_s%d.ctl", dir, (unsigned long long)o->serial, i);
        if (sd[i].control) plat_file_write(path, sd[i].control, 0x880);
        const pool_t *p = nvn_pool_find(sd[i].data, 1);
        if (p && p->mem && !nvn_gpu_only(sd[i].data)) {
            uint64_t end = (uint64_t)(uintptr_t)p->mem + p->size, len = end - sd[i].data;
            if (len > 0x40000) len = 0x40000;
            snprintf(path, sizeof path, "%s/p%04llu_s%d.code", dir, (unsigned long long)o->serial, i);
            plat_file_write(path, (const void *)(uintptr_t)sd[i].data, (size_t)len);
        }
    }
}

NVN_API(nvnProgramBuilderSetDefaults) {
    *(prog_builder_t *)arg_p(c, 0) = (prog_builder_t){NVN_PROG_BUILDER_MAGIC, 0, NULL, NULL};
}
NVN_API(nvnProgramBuilderSetDevice) { ((prog_builder_t *)arg_p(c, 0))->dev = arg_p(c, 1); }
NVN_API(nvnProgramBuilderSetShaders) {
    prog_builder_t *b = arg_p(c, 0);
    b->n = (int32_t)arg_x(c, 1);
    b->sd = arg_p(c, 2);
}
NVN_API(nvnProgramInitializeFromBuilder) {
    NVN_BLOCKING();
    prog_t *o = arg_p(c, 0);
    const prog_builder_t *b = arg_p(c, 1);
    if (!o || !b || b->magic != NVN_PROG_BUILDER_MAGIC) {
        rt_log("nvn: ProgramInitializeFromBuilder with builder %p (not set up by SetDefaults)", (const void *)b);
        ret_x(c, 0);
        return;
    }
    const char *mode = cfg_get("NX2SARIA_NVN2_PROGRAMS");
    if (mode && !strcmp(mode, "off")) {
        ret_x(c, 0);
        return;
    }
    memset(o, 0, sizeof *o);
    o->magic = NVN_PROG_MAGIC;
    int ok = prog_set_shaders(o, b->n, b->sd);
    shader_dump(o, b->n, b->sd);
    ret_x(c, ok);
}

#define ST(T, c) ((T *)arg_p(c, 0))
NVN_API(nvnBlendStateSetDefaults) {
    *ST(blend_st, c) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO,
                                  NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
}
NVN_API(nvnBlendStateSetBlendTarget) { ST(blend_st, c)->target = (uint32_t)arg_x(c, 1); }
NVN_API(nvnBlendStateSetBlendFunc) {
    blend_st *o = ST(blend_st, c);
    o->src_rgb = (uint32_t)arg_x(c, 1);
    o->dst_rgb = (uint32_t)arg_x(c, 2);
    o->src_a = (uint32_t)arg_x(c, 3);
    o->dst_a = (uint32_t)arg_x(c, 4);
}
NVN_API(nvnBlendStateSetBlendEquation) {
    blend_st *o = ST(blend_st, c);
    o->eq_rgb = (uint32_t)arg_x(c, 1);
    o->eq_a = (uint32_t)arg_x(c, 2);
}
NVN_API(nvnColorStateSetDefaults) { *ST(color_st, c) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN_API(nvnColorStateSetBlendEnable) {
    color_st *o = ST(color_st, c);
    int i = (int)arg_x(c, 1);
    if (i < 0 || i > 7) return;
    o->blend_en = nvn_arg_bool(c, 2) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN_API(nvnChannelMaskStateSetDefaults) { ST(chmask_st, c)->mask = 0xffffffffu; }
NVN_API(nvnChannelMaskStateSetChannelMask) {
    int i = (int)arg_x(c, 1);
    if (i < 0 || i > 7) return;
    uint32_t m = (nvn_arg_bool(c, 2) ? 1u : 0) | (nvn_arg_bool(c, 3) ? 2u : 0) | (nvn_arg_bool(c, 4) ? 4u : 0) |
                 (nvn_arg_bool(c, 5) ? 8u : 0);
    chmask_st *o = ST(chmask_st, c);
    o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN_API(nvnMultisampleStateSetDefaults) {
    memset(arg_p(c, 0), 0, sizeof(ms_st));
    ST(ms_st, c)->enable = 1;
}
NVN_API(nvnMultisampleStateSetMultisampleEnable) { ST(ms_st, c)->enable = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnMultisampleStateSetSamples) { ST(ms_st, c)->samples = (uint8_t)arg_x(c, 1); }
NVN_API(nvnMultisampleStateSetAlphaToCoverageEnable) { ST(ms_st, c)->a2c = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnMultisampleStateSetSampleLocationsEnable) { ST(ms_st, c)->sl_en = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnMultisampleStateSetSampleLocations) { (void)c; }
NVN_API(nvnPolygonStateSetDefaults) {
    *ST(poly_st, c) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
}
NVN_API(nvnPolygonStateSetCullFace) { ST(poly_st, c)->cull = (uint32_t)arg_x(c, 1); }
NVN_API(nvnPolygonStateSetFrontFace) { ST(poly_st, c)->front = (uint32_t)arg_x(c, 1); }
NVN_API(nvnPolygonStateSetPolygonMode) { ST(poly_st, c)->mode = (uint32_t)arg_x(c, 1); }
NVN_API(nvnPolygonStateSetPolygonOffsetEnables) { ST(poly_st, c)->offset_en = (uint32_t)arg_x(c, 1); }
NVN_API(nvnDepthStencilStateSetDefaults) {
    *ST(ds_st, c) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN_API(nvnDepthStencilStateSetDepthTestEnable) { ST(ds_st, c)->dt = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnDepthStencilStateSetDepthWriteEnable) { ST(ds_st, c)->dw = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnDepthStencilStateSetDepthFunc) { ST(ds_st, c)->df = (uint32_t)arg_x(c, 1); }
NVN_API(nvnDepthStencilStateSetStencilTestEnable) { ST(ds_st, c)->st = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnDepthStencilStateSetStencilFunc) {
    ds_st *o = ST(ds_st, c);
    uint32_t face = (uint32_t)arg_x(c, 1), fn = (uint32_t)arg_x(c, 2);
    if (face & NVN_FACE_FRONT) o->sf_f = fn;
    if (face & NVN_FACE_BACK) o->sf_b = fn;
}
NVN_API(nvnDepthStencilStateSetStencilOp) {
    ds_st *o = ST(ds_st, c);
    uint32_t face = (uint32_t)arg_x(c, 1), fail = (uint32_t)arg_x(c, 2), zfail = (uint32_t)arg_x(c, 3),
             zpass = (uint32_t)arg_x(c, 4);
    if (face & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (face & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN_API(nvnVertexAttribStateSetDefaults) { *ST(va_st, c) = (va_st){0, 0, 0}; }
NVN_API(nvnVertexAttribStateSetFormat) {
    int64_t off = (int64_t)arg_x(c, 2);
    if (off < 0 || off >= (1 << 19)) rt_fatal("nvn: vertex attribute offset %lld outside the packed range", (long long)off);
    va_st *o = ST(va_st, c);
    o->fmt = (uint32_t)arg_x(c, 1);
    o->offset = (uint32_t)off;
}
NVN_API(nvnVertexAttribStateSetStreamIndex) { ST(va_st, c)->stream = (uint32_t)arg_x(c, 1); }
NVN_API(nvnVertexStreamStateSetDefaults) { *ST(vs_st, c) = (vs_st){0, 0}; }
NVN_API(nvnVertexStreamStateSetStride) { ST(vs_st, c)->stride = (uint32_t)arg_x(c, 1); }
NVN_API(nvnVertexStreamStateSetDivisor) { ST(vs_st, c)->divisor = (uint32_t)arg_x(c, 1); }

#define WB(c) ((win_builder_t *)arg_p(c, 0))
#define WN(c) ((win_t *)arg_p(c, 0))
NVN_API(nvnWindowBuilderSetDefaults) {
    win_builder_t *b = WB(c);
    NVNdevice *dev = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = dev;
    b->interval = 1;
}
NVN_API(nvnWindowBuilderSetDevice) { WB(c)->dev = arg_p(c, 1); }
NVN_API(nvnWindowBuilderSetNativeWindow) { WB(c)->native = arg_p(c, 1); }
NVN_API(nvnWindowBuilderSetTextures) {
    win_builder_t *b = WB(c);
    int n = (int)arg_x(c, 1);
    NVNtexture *const *t = arg_p(c, 2);
    if (n < 0 || n > 4 || (n && !t)) rt_fatal("nvn: WindowBuilderSetTextures with %d textures at %p", n, (const void *)t);
    b->ntex = n;
    for (int i = 0; i < n; i++) b->tex[i] = t[i];
}
NVN_API(nvnWindowBuilderSetNumActiveTextures) {
    int n = (int)arg_x(c, 1);
    if (n < 2 || n > 4) rt_fatal("nvn: unsupported active texture count %d", n);
    WB(c)->active = n;
}
NVN_API(nvnWindowBuilderGetNumActiveTextures) {
    const win_builder_t *b = WB(c);
    ret_x(c, (uint32_t)(b->active ? b->active : b->ntex));
}
NVN_API(nvnWindowSetNumActiveTextures) {
    win_t *w = WN(c);
    int n = (int)arg_x(c, 1);
    if (n < 2 || n > w->ntex) rt_fatal("nvn: invalid active window texture count %d", n);
    w->active = n;
}
NVN_API(nvnWindowGetNumActiveTextures) {
    const win_t *w = WN(c);
    ret_x(c, (uint32_t)(w->active ? w->active : w->ntex));
}
NVN_API(nvnWindowBuilderSetPresentInterval) { WB(c)->interval = (int)arg_x(c, 1); }
NVN_API(nvnWindowInitialize) {
    win_t *o = arg_p(c, 0);
    const win_builder_t *s = arg_p(c, 1);
    memset(o, 0, sizeof *o);
    o->magic = NVN_WIN_MAGIC;
    o->native = s->native;
    o->ntex = s->ntex;
    o->interval = s->interval;
    o->active = s->active ? s->active : s->ntex;
    if (o->ntex < 2 || o->ntex > 4 || o->active < 2 || o->active > o->ntex) {
        rt_log("nvn: WindowInitialize with %d textures, %d active", o->ntex, o->active);
        ret_x(c, 0);
        return;
    }
    memcpy(o->tex, s->tex, sizeof o->tex);
    const tex_t *t0 = (const tex_t *)o->tex[0];
    NVN_LOG("WindowInitialize %d textures, interval %d, first %ux%u format %#x", o->ntex, o->interval,
            t0 ? t0->d.width : 0, t0 ? t0->d.height : 0, t0 ? t0->d.format : 0);
    ret_x(c, 1);
}
NVN_API(nvnWindowFinalize) { (void)c; }
NVN_API(nvnWindowSetPresentInterval) {
    win_t *o = WN(c);
    int v = (int)arg_x(c, 1);
    if (o->interval != v) NVN_LOG("present interval %d -> %d", o->interval, v);
    o->interval = v;
}
NVN_API(nvnWindowSetCrop) {
    win_t *o = WN(c);
    int v[4] = {(int)arg_x(c, 1), (int)arg_x(c, 2), (int)arg_x(c, 3), (int)arg_x(c, 4)};
    if (memcmp(o->crop, v, sizeof v)) NVN_LOG("WindowCrop %d,%d %dx%d", v[0], v[1], v[2], v[3]);
    memcpy(o->crop, v, sizeof v);
}

#define VSYNC_HZ 60ull
static uint64_t present_period(const win_t *o) {
    if (frame_step_realtime() || frame_free_running()) return frame_step_cap_ns();
    static int pace_hz = -1;
    if (pace_hz < 0) pace_hz = cfg_get("NX2SARIA_PACE_HZ") ? atoi(cfg_get("NX2SARIA_PACE_HZ")) : 0;
    if (pace_hz > 0) return 1000000000ull / (uint64_t)pace_hz;
    return (o->interval > 0 ? (uint64_t)o->interval : 1ull) * 1000000000ull / VSYNC_HZ;
}

#define MOVIE_CATCHUP_NS 1000000000ull
static uint64_t pace(uint64_t *target, uint64_t now, uint64_t period) {
    if (!period || !*target) { *target = now; return 0; }
    uint64_t next = *target + period;
    if (now <= next) { *target = next; return next - now; }
    uint64_t keep = frame_step_movie_hz() > 0 ? MOVIE_CATCHUP_NS : period;
    *target = now - next < keep ? next : now;
    return 0;
}

static void sleep_until(uint64_t deadline) {
    for (;;) {
        uint64_t now = kern_now_ns();
        if (now >= deadline) return;
        uint64_t d = deadline - now;
        if (d > PLAT_WAIT_LATE_NS + 100000) plat_wait_ns(d - PLAT_WAIT_LATE_NS - 100000);
        else plat_yield();
    }
}

static struct { const win_t *w; uint64_t fence[4]; } img_fence[4];
static uint64_t last_present_fence;

static uint64_t *img_fence_of(const win_t *o, int idx) {
    if (idx < 0 || idx >= 4) return NULL;
    for (int i = 0; i < 4; i++) {
        if (img_fence[i].w == o) return &img_fence[i].fence[idx];
        if (!img_fence[i].w) return img_fence[i].w = o, &img_fence[i].fence[idx];
    }
    return NULL;
}

NVN_API(nvnWindowAcquireTexture) {
    win_t *o = arg_p(c, 0);
    sync_t *s = arg_p(c, 1);
    int *out = arg_p(c, 2);
    NVN_BLOCKING();
    int idx = o->active ? o->next++ % o->active : 0;
    *out = idx;
    static volatile int32_t named;
    if (prof_render_tid && !named && !plat_xchg32(&named, 1)) {
        uint32_t tid = plat_thread_id();
        rt_log("nvn: the game acquires its window images on thread %u and presents on thread %u%s", tid,
               prof_render_tid, tid == prof_render_tid ? "" : ": the acquire's waits count for the next present");
    }
    uint64_t at = idx < 4 ? o->avail[idx] : 0;
    if (at) {
        uint64_t p0 = perf_begin();
        sleep_until(at);
        perf_end_any(PERF_PACE, p0);
    }
    static int acq_wait = -1;
    if (acq_wait < 0) {
        const char *e = cfg_get("NX2SARIA_ACQUIRE_WAIT");
        acq_wait = e && (*e == '0' || *e == '2') && !e[1] ? *e - '0' : 1;
    }
    uint64_t *fe = img_fence_of(o, idx), want = fe ? *fe : 0;
    if (acq_wait == 2) want = last_present_fence;
    if (acq_wait && want) {
        const GfxBackend *b = gfx_backend();
        if (b->fence_done() < want) b->fence_wait(want, UINT64_MAX);
    }
    if (gfx_backend()->frame_start) gfx_backend()->frame_start();
    if (s) {
        s->fenced = 3;
        s->fence = 0;
    }
    ret_x(c, NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS);
}

NVN_API(nvnQueuePresentTexture) {
    win_t *o = arg_p(c, 1);
    int idx = (int)arg_x(c, 2);
    movie_tick(c);
    res_present(c);
    wide_present();
    NVN_BLOCKING();
    if (o->magic != NVN_WIN_MAGIC || idx < 0 || idx >= o->ntex || idx >= 4 || !o->tex[idx])
        rt_fatal("nvn: QueuePresentTexture of image %d on window %p", idx, (void *)o);
    nvn_call_note(1, (uintptr_t)arg_p(c, 0), 1);
    o->presents++;
    __atomic_store_n(&nvn_submits_in_frame, 0, __ATOMIC_RELAXED);
    const tex_t *t = (const tex_t *)o->tex[idx];
    static int hitch_every = -1, hitch_ms;
    if (hitch_every < 0) {
        const char *e = cfg_get("NX2SARIA_TEST_HITCH");
        int every = 0, ms = 0;
        hitch_every = e && sscanf(e, "%d:%d", &every, &ms) == 2 && every > 0 && ms > 0 ? every : 0;
        hitch_ms = ms;
        if (hitch_every) rt_log("nvn: a %d ms late frame every %d presents (NX2SARIA_TEST_HITCH)", hitch_ms, hitch_every);
    }
    if (hitch_every && o->presents % (uint64_t)hitch_every == 0) plat_wait_ns((uint64_t)hitch_ms * 1000000);
    uint64_t period = present_period(o), now = kern_now_ns();
    if (gfx_backend()->frame_paced) {
        int movie = frame_step_movie_hz() > 0;
        uint64_t held = gfx_backend()->frame_paced(movie ? 0 : period);
        if (!movie) period = held;
    }
    uint64_t slot = now + pace(&o->pace, now, period);
    gfx_backend()->present(t, o->crop, slot);
    o->ticket[idx] = o->presents;
    o->avail[idx] = slot + period;
    uint64_t *fe = img_fence_of(o, idx);
    if (fe) *fe = nvn_fence_last();
    last_present_fence = nvn_fence_last();
    frame_step_present(slot, o->interval);
    rt_frame_presented(t, o->presents);
    nvn_call_note(1, (uintptr_t)arg_p(c, 0), 0);
}
