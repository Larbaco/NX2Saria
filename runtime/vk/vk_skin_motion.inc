#include "skin_pair.h"

#define SKIN_BANKS 5
#define SKIN_SLOTS 4096
#define SKIN_FILL 2048
#define SKIN_NEAR_UNITS 1.0f

enum { SKIN_NOT_READY = SKIN_PAIR_WHYS,
       SKIN_BANKS_OTHER,
       SKIN_STORAGE,
       SKIN_SNAPSHOT,
       SKIN_TWIN,
       SKIN_VIEW,
       SKIN_SHARED,
       SKIN_FAR,
       SKIN_FULL,
       SKIN_UNUSED,
       SKIN_WHYS };

typedef struct { uint8_t stride, bank; } SkinPick;

typedef struct {
    uint64_t addr;
    uint32_t size, at;
    VkDescriptorBufferInfo was;
    uint8_t *was_map;
    int32_t last;
    int32_t sharp_with, sharp;
} SkinBlock;

typedef struct {
    int32_t block[SKIN_BANKS];
    uint8_t palette, stride;
    uint32_t zw, zh;
    VkImage depth;
    uint64_t cam;
    uint64_t mesh, twin;
    int32_t draw;
} SkinDraw;

typedef struct {
    uint64_t frame;
    VkImage scene_depth;
    SkinDraw *d;
    SkinEntry *e;
    int n, ne, cap_d, cap_e;
    SkinBlock *b;
    int nb, cap_b;
    uint8_t *bytes;
    uint32_t used, size;
    uint16_t block_at[SKIN_SLOTS];
    uint64_t seen[SKIN_SLOTS];
    uint64_t lit[SKIN_SLOTS];
    int nseen;
} SkinFrame;

static struct {
    SkinFrame f[2];
    int cur;
    int resolved;
    uint32_t pending;
    uint64_t seen, own, guarded, why[SKIN_WHYS];
} SK;

static int skin_enabled(void) {
    static int on = -1;
    if (on < 0) on = RT_V111 && cfg_bool("NX2SARIA_SKIN_MOTION");
    return on;
}

static void skin_left(int why) { __atomic_add_fetch(&SK.why[why], 1, __ATOMIC_RELAXED); }

static int skin_has(const uint64_t *set, uint64_t v) {
    v |= 1;
    for (uint32_t i = (uint32_t)(v >> 40) & (SKIN_SLOTS - 1); set[i]; i = (i + 1) & (SKIN_SLOTS - 1))
        if (set[i] == v) return 1;
    return 0;
}
static void skin_put(uint64_t *set, uint64_t v) {
    v |= 1;
    uint32_t i = (uint32_t)(v >> 40) & (SKIN_SLOTS - 1);
    while (set[i] && set[i] != v) i = (i + 1) & (SKIN_SLOTS - 1);
    set[i] = v;
}

static int skin_joint_format(uint32_t fmt) {
    switch (fmt) {
    case NVN_FORMAT_R8UI: case NVN_FORMAT_RG8UI: case NVN_FORMAT_RGB8UI: case NVN_FORMAT_RGBA8UI:
    case NVN_FORMAT_R16UI: case NVN_FORMAT_RG16UI: case NVN_FORMAT_RGB16UI: case NVN_FORMAT_RGBA16UI:
    case NVN_FORMAT_R32UI: case NVN_FORMAT_RG32UI: case NVN_FORMAT_RGB32UI: case NVN_FORMAT_RGBA32UI: return 1;
    default: return 0;
    }
}

static int skin_pick(VkExec *ex, SkinPick *out) {
    out->stride = 0;
    if (!skin_enabled() || !(vk_dlaa_active() || vk_dlaa_fg_only())) return 0;
    const DrawCtx *cx = ex->dc;
    int slot, storage;
    uint32_t joints;
    int stride = gfx_shader_palette(cx->B.st.vs, &slot, &storage, &joints);
    if (!stride || joints >> 16) return 0;
    for (int l = 0; l < 16; l++) {
        va_st a;
        memcpy(&a, &cx->B.st.vattr[l], sizeof a);
        if ((joints >> l & 1) && !skin_joint_format(a.fmt)) return 0;
    }
    __atomic_add_fetch(&SK.seen, 1, __ATOMIC_RELAXED);
    if (storage || slot < 3 || slot >= 3 + SKIN_BANKS) {
        skin_left(SKIN_BANKS_OTHER);
        return 0;
    }
    out->stride = (uint8_t)stride, out->bank = (uint8_t)(slot - 3);
    return 1;
}

static int skin_pick_left(const SkinPick *s, int why) {
    if (s->stride) skin_left(why);
    return 0;
}

static void skin_begin(uint64_t frame) {
    SK.why[SKIN_UNUSED] += SK.pending;
    SK.pending = 0, SK.resolved = 0;
    if (!skin_enabled()) return;
    SkinFrame *f = &SK.f[SK.cur];
    f->frame = frame, f->scene_depth = VK_NULL_HANDLE;
    f->n = f->ne = f->nb = f->nseen = 0, f->used = 0;
    memset(f->block_at, 0, sizeof f->block_at);
    memset(f->seen, 0, sizeof f->seen);
}

static int skin_block(VkExec *ex, SkinFrame *f, const p_bind_buf *u) {
    VkDescriptorBufferInfo snap;
    if (!u->addr || !u->size || u->size > LIM.ubo_max || u->size > POSE_BYTES ||
        !vk_usnap_find(u->addr, u->size, LIM.ubo_align, &snap))
        return -1;
    uint32_t h = (uint32_t)(skin_mix(u->addr, u->size) >> 40) & (SKIN_SLOTS - 1);
    for (; f->block_at[h]; h = (h + 1) & (SKIN_SLOTS - 1)) {
        const SkinBlock *b = &f->b[f->block_at[h] - 1];
        if (b->addr == u->addr && b->size == u->size) return f->block_at[h] - 1;
    }
    if (f->nb == SKIN_FILL) return -2;
    uint32_t size = (uint32_t)u->size;
    if (f->used + size > f->size) {
        uint32_t want = f->size ? f->size * 2 : 1u << 20;
        while (want < f->used + size) want *= 2;
        uint8_t *bytes = realloc(f->bytes, want);
        if (!bytes) return -2;
        f->bytes = bytes, f->size = want;
    }
    f->b = mo_grow(f->b, &f->cap_b, f->nb + 1, sizeof *f->b);
    SkinBlock *b = &f->b[f->nb];
    *b = (SkinBlock){.addr = u->addr, .size = size, .at = f->used, .last = -1, .sharp_with = -1};
    memcpy(f->bytes + b->at, vk_usnap_map() + snap.offset, size);
    f->used += size;
    b->was.range = size;
    b->was_map = scratch(ex, size, &b->was.buffer, &b->was.offset);
    f->block_at[h] = (uint16_t)(f->nb + 1);
    return f->nb++;
}

static int skin_sharp(SkinFrame *f, const SkinFrame *last, int c, int l, uint32_t stride) {
    SkinBlock *b = &f->b[c];
    const SkinBlock *o = &last->b[l];
    if (b->sharp_with == l) return b->sharp;
    b->sharp_with = l, b->sharp = 0;
    if (b->size != o->size) return 0;
    const uint8_t *now = f->bytes + b->at, *then = last->bytes + o->at;
    for (uint32_t at = 0; at + stride <= b->size && !b->sharp; at += stride)
        if (memcmp(now + at, then + at, 48)) b->sharp = pose_matrix_sharp(now + at, then + at);
    return b->sharp;
}

static int skin_try(VkExec *ex, const p_draw_idx *d, uint64_t frame, const GfxShader *vs, uint32_t banks,
                    const SkinPick *pick, SkinDraw *sd, SkinEntry *se, uint64_t *listed, int *guard) {
    const DrawCtx *cx = ex->dc;
    SkinFrame *f = &SK.f[SK.cur];
    const SkinFrame *last = &SK.f[SK.cur ^ 1];
    int consecutive = last->frame + 1 == frame;
    const GfxShaderRes *res;
    *guard = 0;
    if (SK.resolved || f->frame != frame) return skin_pick_left(pick, SKIN_UNUSED);
    if (!(banks >> 3 & 1) || !(banks >> (pick->bank + 3) & 1) || (banks & ~0xf8u)) return skin_pick_left(pick, SKIN_BANKS_OTHER);
    if (gfx_shader_resources(vs, GFX_RES_SSBO, &res) || gfx_shader_resources(vs, GFX_RES_TEX, &res) ||
        gfx_shader_resources(vs, GFX_RES_IMG, &res))
        return skin_pick_left(pick, SKIN_STORAGE);
    uint32_t inputs = gfx_shader_inputs(cx->B.st.vs, NULL), streams = 0;
    for (int i = 0; i < 16; i++) {
        va_st a;
        memcpy(&a, &cx->B.st.vattr[i], sizeof a);
        if ((inputs >> i & 1) && a.fmt && a.stream < 16) streams |= 1u << a.stream;
    }
    uint64_t mesh = skin_mix(skin_mix(d->ibuf, (uint64_t)(uint32_t)d->count << 32 | (uint32_t)d->base_vertex), (uint64_t)d->itype);
    for (int s = 0; s < 16; s++)
        if (streams >> s & 1) mesh = skin_mix(skin_mix(mesh, ex->vbuf[s].address), ex->vbuf[s].size ^ (uint64_t)s << 56);
    const p_bind_buf *pal = &ex->ubo[NVN_STAGE_VS][pick->bank];
    const VkExecTarget *z = &ex->depth;
    uint64_t twin = skin_mix(mesh, pal->addr), cam = ex->ubo[NVN_STAGE_VS][0].addr;
    uint64_t here = skin_mix(skin_mix(twin, cam), (uint64_t)(uintptr_t)z->image);
    if (skin_has(f->seen, here)) return skin_pick_left(pick, SKIN_TWIN);
    if (consecutive && cx->B.st.ds.dw && last->scene_depth && z->image != last->scene_depth && skin_has(last->lit, mesh))
        return skin_pick_left(pick, SKIN_TWIN);
    if (f->nseen == SKIN_FILL) return skin_pick_left(pick, SKIN_FULL);
    for (int i = 0; i < SKIN_BANKS; i++) {
        sd->block[i] = -1;
        if (!(banks >> (i + 3) & 1)) continue;
        sd->block[i] = skin_block(ex, f, &ex->ubo[NVN_STAGE_VS][i]);
        if (sd->block[i] < 0) return skin_pick_left(pick, sd->block[i] == -1 ? SKIN_SNAPSHOT : SKIN_FULL);
    }
    sd->palette = pick->bank, sd->stride = pick->stride;
    sd->zw = z->width, sd->zh = z->height, sd->depth = z->image;
    sd->cam = cam, sd->mesh = mesh, sd->twin = twin;
    *se = (SkinEntry){.key = skin_mix(skin_mix(mesh, (uint64_t)(uintptr_t)cx->B.st.vs), (uint64_t)(uintptr_t)cx->B.st.fs),
                      .order = vk_exec_stream_pos() << 24 | (uint64_t)f->n, .palette = pal->addr,
                      .bytes = (uint32_t)pal->size};
    *listed = here;
    int at = consecutive && !pose_raw() ? skin_find(last->e, last->ne, se->key) : -1;
    for (uint32_t k = 0; at >= 0 && k < last->e[at].of && !*guard; k++) {
        const SkinDraw *ld = &last->d[last->e[at + (int)k].tag];
        if (ld->palette == sd->palette) *guard = skin_sharp(f, last, sd->block[sd->palette], ld->block[ld->palette], sd->stride);
    }
    return 1;
}

static void skin_commit(const SkinDraw *sd, const SkinEntry *se, uint64_t listed, int draw) {
    SkinFrame *f = &SK.f[SK.cur];
    f->d = mo_grow(f->d, &f->cap_d, f->n + 1, sizeof *f->d);
    f->e = mo_grow(f->e, &f->cap_e, f->n + 1, sizeof *f->e);
    f->d[f->n] = *sd, f->d[f->n].draw = draw;
    f->e[f->n] = *se, f->e[f->n].tag = (uint32_t)f->n;
    f->n++;
    skin_put(f->seen, listed), f->nseen++;
    SK.pending++;
}

static void skin_refuse(const SkinDraw *sd, int why) {
    MQ.d[sd->draw].skin = -1;
    SK.why[why]++, SK.pending--;
}

static const SkinFrame *skin_sort_frame;
static int skin_twin_cmp(const void *a, const void *b) {
    const SkinEntry *x = a, *y = b;
    const SkinDraw *dx = &skin_sort_frame->d[x->tag], *dy = &skin_sort_frame->d[y->tag];
    if (dx->twin != dy->twin) return dx->twin < dy->twin ? -1 : 1;
    int ox = dx->depth != skin_sort_frame->scene_depth, oy = dy->depth != skin_sort_frame->scene_depth;
    if (ox != oy) return ox - oy;
    return x->order < y->order ? -1 : x->order > y->order;
}

static void skin_resolve(VkExec *ex, uint64_t frame, uint64_t cam, VkImage depth, uint32_t w, uint32_t h) {
    SkinFrame *f = &SK.f[SK.cur];
    const SkinFrame *last = &SK.f[SK.cur ^ 1];
    if (!skin_enabled() || SK.resolved || MQ.frame != frame || MQ.fence != ex->fence || f->frame != frame) return;
    SK.resolved = 1;
    f->scene_depth = depth;
    int ne = 0;
    for (int i = 0; i < f->n; i++) {
        const SkinDraw *sd = &f->d[i];
        if (sd->cam != cam || sd->zw != w || sd->zh != h) skin_refuse(sd, SKIN_VIEW);
        else f->e[ne++] = f->e[i];
    }
    skin_sort_frame = f;
    qsort(f->e, (size_t)ne, sizeof *f->e, skin_twin_cmp);
    int kept = 0;
    for (int i = 0; i < ne; i++) {
        if (kept && f->d[f->e[kept - 1].tag].twin == f->d[f->e[i].tag].twin) skin_refuse(&f->d[f->e[i].tag], SKIN_TWIN);
        else f->e[kept++] = f->e[i];
    }
    f->ne = ne = kept;
    skin_number(f->e, ne);
    skin_pair(f->e, ne, last->e, last->ne, last->frame + 1 == frame);
    for (int i = 0; i < ne; i++) {
        SkinEntry *e = &f->e[i];
        const SkinDraw *sd = &f->d[e->tag];
        if (e->why != SKIN_PAIRED) continue;
        const SkinDraw *ld = &last->d[last->e[e->prev].tag];
        if (ld->palette != sd->palette || ld->stride != sd->stride) e->why = SKIN_SIZE;
        for (int k = 0; k < SKIN_BANKS && e->why == SKIN_PAIRED; k++)
            if ((sd->block[k] < 0) != (ld->block[k] < 0) ||
                (sd->block[k] >= 0 && f->b[sd->block[k]].size != last->b[ld->block[k]].size))
                e->why = SKIN_SIZE;
        if (e->why == SKIN_PAIRED && e->of > 1 &&
            !skin_near(f->bytes + f->b[sd->block[sd->palette]].at, last->bytes + last->b[ld->block[ld->palette]].at,
                       e->bytes, sd->stride, SKIN_NEAR_UNITS))
            e->why = SKIN_FAR;
        for (int k = 0; k < SKIN_BANKS && e->why == SKIN_PAIRED; k++) {
            if (sd->block[k] < 0) continue;
            SkinBlock *b = &f->b[sd->block[k]];
            b->last = b->last == -1 || b->last == ld->block[k] ? ld->block[k] : -2;
        }
    }
    memset(f->lit, 0, sizeof f->lit);
    for (int i = 0; i < ne; i++) {
        SkinEntry *e = &f->e[i];
        const SkinDraw *sd = &f->d[e->tag];
        if (sd->depth == depth) skin_put(f->lit, sd->mesh);
        for (int k = 0; k < SKIN_BANKS && e->why == SKIN_PAIRED; k++)
            if (sd->block[k] >= 0 && f->b[sd->block[k]].last == -2) e->why = SKIN_SHARED;
        if (e->why != SKIN_PAIRED) skin_refuse(sd, e->why);
    }
    for (int i = 0; i < f->nb; i++) {
        const SkinBlock *b = &f->b[i];
        if (b->last >= 0) memcpy(b->was_map, last->bytes + last->b[b->last].at, b->size);
    }
    SK.cur ^= 1;
}

static void skin_recorded(int guarded) {
    SK.own++, SK.guarded += guarded != 0, SK.pending--;
}

static int skin_status(char *out, size_t cap) {
    out[0] = 0;
    if (!skin_enabled()) return 0;
    const uint64_t *y = SK.why;
    return snprintf(out, cap, "; skinned draws seen %llu, drawn with their own motion %llu (checked against last frame's "
                    "depth %llu), left to the camera: carried by another draw of the mesh %llu, not drawn last frame %llu, "
                    "drawn another number of times %llu, palette of another size %llu, palette at another address %llu, "
                    "too far from last frame's %llu, shared block disagrees %llu, another view %llu, shader or pipeline "
                    "not ready %llu, position reads other blocks %llu, storage in the vertex shader %llu, block not in the "
                    "snapshot %llu, tables full %llu, list not recorded %llu",
                    (unsigned long long)__atomic_load_n(&SK.seen, __ATOMIC_RELAXED), (unsigned long long)SK.own,
                    (unsigned long long)SK.guarded, (unsigned long long)y[SKIN_TWIN], (unsigned long long)y[SKIN_NEW],
                    (unsigned long long)y[SKIN_COUNT], (unsigned long long)y[SKIN_SIZE], (unsigned long long)y[SKIN_ADDRESS],
                    (unsigned long long)y[SKIN_FAR], (unsigned long long)y[SKIN_SHARED], (unsigned long long)y[SKIN_VIEW],
                    (unsigned long long)y[SKIN_NOT_READY], (unsigned long long)y[SKIN_BANKS_OTHER],
                    (unsigned long long)y[SKIN_STORAGE], (unsigned long long)y[SKIN_SNAPSHOT], (unsigned long long)y[SKIN_FULL],
                    (unsigned long long)y[SKIN_UNUSED]);
}

static void skin_shutdown(void) {
    for (int i = 0; i < 2; i++) free(SK.f[i].d), free(SK.f[i].e), free(SK.f[i].b), free(SK.f[i].bytes);
    memset(&SK, 0, sizeof SK);
}
