// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
//
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "gfx_guard.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "par_jobs.h"
#include "tex.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_tex_compact.h"
#include "vk_tex_detail.h"
#include "vk_tex_release.h"
#include "prof.h"
#include "platform/platform.h"

typedef struct {
    uint64_t addr;
    nvn_texdesc d;
    uint8_t has_layout, layout[8], pad[7];
} TexKey;

typedef struct { uint32_t flags, type, format, w, h, d, levels, layers, samples, usage; } ImgKey;

typedef struct Tex {
    VkTexImage pub;
    TexKey key;
    tex_t t;
    TexVkFormat f;
    VmaAllocation alloc;
    uint64_t bytes;
    uint64_t blk_mem, blk_size;
    uint8_t blk_type, blk_own;
    VmaAllocation bound_to;
    uint64_t wgen;
    uint32_t checked;
    uint32_t use_round;
    uint64_t use_frame;
    uint64_t made_frame;
    uint64_t samp_frame;
    uint8_t retiring;
    uint64_t retire_takes;
    uint64_t retire_frame;
    uint64_t retire_due;
    int uploaded, released;
    uint64_t up_retry;
    uint64_t uid;
    uint64_t rel_frame;
    uint32_t back_gap;
    uint32_t n_up;
    int last_ch;
    struct Tex *next_at;
    uint8_t gpu_dirty;
    uint64_t gpu_gen;
    uint64_t gpu_seq;
    uint64_t gpu_fence;
    uint64_t run_frame;
    uint8_t run;
    uint64_t wr_fence;
    uint8_t listed;
    uint8_t *snap;
    uint32_t wt_n;
    uint64_t chash;
    uint8_t chash_ok;
    uint8_t chash_miss;
    struct Tex *rel_prev, *rel_next;
    ImgKey ik;
    uint8_t reusable;
    uint8_t reused;
    uint8_t ru_on;
    uint8_t superseded;
    uint32_t rel_epoch;
    struct Tex *ru_prev, *ru_next;
    uint64_t gw_frame;
    uint64_t scan_frame;
    uint64_t ow_frame;
    uint8_t overwritten;
    uint64_t kept_frame;
    uint8_t in_gwin;
    int pidx;
    int ph;
    uint8_t had;
    uint8_t og_start;
    uint8_t og_drop;
    uint8_t og_reimg;
    uint8_t bc1_try;
    uint8_t said;
    uint8_t sfill;
    uint32_t og_backs;
    uint64_t og_frame;
    uint64_t og_drop_frame;
    uint64_t og_back_frame;
    VkTexImage *flat3d;
    VmaAllocation flat3d_alloc;
    uint32_t flat3d_up, flat3d_layer;
    uint64_t flat3d_wr;
    uint64_t used_frame;
    uint8_t in_rt;
    uint8_t compacting;
    uint8_t clobbered;
} Tex;

typedef struct {
    struct Tex *ce;
    uint32_t cgen;
    uint32_t cepoch;
    view_t cview;
    uint8_t chas_view, used, has_view, finalized;
    uint8_t cheld;
    uint8_t cwant;
    uint8_t fresh;
    uint64_t khash;
    view_t v;
    tex_t t;
} TexSlot;
typedef struct { samp_par p; VkSampler s, sp, sc, sd[2]; uint8_t used; } SampSlot;

static PlatRMutex tex_lock;
static PlatOnce tex_once = PLAT_ONCE_INIT;
static nvn_hmap by_key = {.klen = sizeof(TexKey)};
static nvn_hmap by_addr = {.klen = 8};
static TexSlot *tslots;
static int ntslots;
static SampSlot *sslots;
static int nsslots;
static nvn_hmap samplers = {.klen = sizeof(samp_par)};
static int nreleased;
static Tex *rel_head, *rel_tail;
static uint64_t n_uploads, n_reuploads;
static uint64_t ru_n, ru_bytes, ru_ns, ru_wt_n, ru_wt_bytes, ru_wt_ns;
static uint64_t ru_same_n, ru_same_bytes;
static uint64_t big_same_n, big_same_bytes, big_diff_n, big_hash_n, big_hash_bytes;
static int tex_alias = -1;
static uint64_t n_alias_copied, n_alias_upload, n_alias_uninit, n_alias_kept, n_alias_released;
static uint64_t n_dinit_inline, n_dinit_inpass, n_dinit_upload;
static uint32_t submit_epoch = 1;
#define RATE_N 8
#define RATE_SAY 16
#define RATE_SAY_UNDER 240u
static uint32_t tex_rate = TRL_RATE_60;
#define PRESENTS(n) trl_presents((uint64_t)(n), tex_rate)

#define PLAY_GAP_MAX_NS 1000000000ull
static TrlClock pclock;
static uint64_t pclock_t0;
static uint64_t play_ns;
static uint64_t play_last;

static uint32_t now_sec(void) {
    if (!pclock_t0) return 0;
    uint64_t gap = kern_now_ns() - play_last;
    return (uint32_t)((play_ns + (gap < PLAY_GAP_MAX_NS ? gap : PLAY_GAP_MAX_NS)) / 1000000000ull);
}

static uint32_t secs_since(uint64_t frame) { return trl_clock_age(&pclock, frame, now_sec()); }

static void rate_tick(void) {
    static uint64_t t0, f0;
    static uint32_t ring[RATE_N], at;
    uint64_t t = kern_now_ns(), f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (play_last) play_ns += t - play_last < PLAY_GAP_MAX_NS ? t - play_last : PLAY_GAP_MAX_NS;
    play_last = t;
    if (!t0) t0 = t, f0 = f, pclock_t0 = t, trl_clock_tick(&pclock, f, 0);
    if (t - t0 < 1000000000ull) return;
    trl_clock_tick(&pclock, f, (uint32_t)(play_ns / 1000000000ull));
    static int low = -1, said;
    if (low < 0) low = rt_knob("TEX_RATE_LOW", "1") != 0;
    ring[at++ % RATE_N] = trl_rate_sample(f - f0, t - t0, low);
    t0 = t, f0 = f;
    uint32_t m = trl_rate(ring, RATE_N);
    static int slow;
    if ((m < RATE_SAY_UNDER) != slow && said < RATE_SAY) {
        said++, slow = m < RATE_SAY_UNDER;
        if (slow)
            rt_log("vk: the game presents %u frames a second, fewer than 60, for %d seconds: the textures' waits count "
                   "their seconds in these (2 s are %llu presents, not 120; NX2SARIA_TEX_RATE_LOW=0: 120)",
                   m * 60 >> 8, RATE_N, (unsigned long long)trl_presents(120, m));
        else rt_log("vk: the game presents 60 frames a second or more again: the textures' waits count 60 or more");
    }
    tex_rate = m;
}
static PlatMutex owned_lock = PLAT_MUTEX_INIT;
static Tex **owned;
static int nowned, cowned;
static uint64_t max_owned;
static uint64_t gpu_writes;
static uint32_t tex_gen = 1;
static uint32_t slots_gen = 1;
static uint32_t samp_gen = 1;
static uint64_t samp_retry_at;
#define SAMP_RETRY_NS 250000000ull
static uint64_t n_checks;
#define AGEN_N 65536
static uint32_t agen[AGEN_N];
static uint64_t *sinfo;
static uint32_t reg_serial;
static uint64_t n_same_regs;

static uint32_t agen_idx(uint64_t addr) { return (uint32_t)(addr * 0x9E3779B97F4A7C15ull >> 48); }

#define DIRTY_N (1u << 16)
#define DIRTY_BUCKET 0x80000000u
static uint32_t dirty[DIRTY_N];
static uint64_t ndirty;
static int32_t bucket_head[AGEN_N];
static int32_t *slot_next, *slot_prev;
static uint16_t *slot_bucket;

static void dirty_push(uint32_t x) { dirty[ndirty++ % DIRTY_N] = x; }

static uint64_t n_bumps, n_site[4], n_undone;
static uint32_t bump_count[AGEN_N];
static uint32_t agen_top[AGEN_N];
static uint32_t bepoch[AGEN_N];
static uint64_t bsig[AGEN_N];
#define GS_N 8192
static struct { uint64_t sig; uint32_t b, epoch, gen, valid; } gs[GS_N];
static int in_lookup;

static uint32_t gs_at(uint32_t b) {
    return (uint32_t)(((bsig[b] ^ ((uint64_t)b << 32 | bepoch[b])) * 0x9E3779B97F4A7C15ull) >> 51);
}

static void tex_gen_bump(uint64_t addr) {
    n_bumps++;
    bump_count[agen_idx(addr)]++;
    if (!++tex_gen) tex_gen = 1;
    slots_gen++;
    uint32_t b = agen_idx(addr);
    bepoch[b]++;
    __atomic_store_n(&agen[b], ++agen_top[b], __ATOMIC_RELEASE);
    gs[gs_at(b)] = (typeof(gs[0])){bsig[b], b, bepoch[b], agen[b], 1};
    dirty_push(DIRTY_BUCKET | b);
}

static void tex_gen_flip(Tex *e, int plain) {
    static int on = -1;
    static uint64_t uids;
    if (on < 0) on = rt_knob("TEX_GEN_UNDO", "1") == 1;
    if (!e->uid) {
        uint64_t z = ++uids * 0x9E3779B97F4A7C15ull;
        z = (z ^ z >> 30) * 0xBF58476D1CE4E5B9ull, z = (z ^ z >> 27) * 0x94D049BB133111EBull;
        e->uid = (z ^ z >> 31) | 1;
    }
    uint32_t b = agen_idx(e->key.addr);
    if (in_lookup) {
        typeof(gs[0]) *m0 = &gs[gs_at(b)];
        if (m0->valid && m0->b == b && m0->epoch == bepoch[b] && m0->sig == bsig[b]) m0->valid = 0;
    }
    bsig[b] ^= e->uid;
    if (!on || plain) {
        tex_gen_bump(e->key.addr);
        return;
    }
    if (!++tex_gen) tex_gen = 1;
    slots_gen++;
    typeof(gs[0]) *m = &gs[gs_at(b)];
    if (m->valid && m->b == b && m->epoch == bepoch[b] && m->sig == bsig[b]) {
        n_undone++;
        __atomic_store_n(&agen[b], m->gen, __ATOMIC_RELEASE);
    } else {
        n_bumps++;
        bump_count[b]++;
        __atomic_store_n(&agen[b], ++agen_top[b], __ATOMIC_RELEASE);
        *m = (typeof(*m)){bsig[b], b, bepoch[b], agen[b], 1};
    }
    dirty_push(DIRTY_BUCKET | b);
}

typedef struct { VkSlotGroup pub; int32_t gnext, gprev, tail; uint32_t bucket; } SlotGroup;
static SlotGroup *grp;
static int ngrp, cgrp, grp_free = -1, ngrp_live;
static uint64_t n_grouped;
static int32_t bucket_ghead[AGEN_N];
static int32_t *slot_grp;
typedef struct { uint64_t kh, b; } GrpKey;
static nvn_hmap grp_by = {.klen = sizeof(GrpKey)};

static int grp_get(uint64_t kh, uint32_t b) {
    GrpKey k = {kh, b};
    void *v = nvn_hm_get(&grp_by, &k);
    if (v) return (int)((intptr_t)v - 1);
    int g;
    if (grp_free >= 0) {
        g = grp_free, grp_free = grp[g].gnext;
    } else {
        if (ngrp == cgrp) {
            int n = cgrp ? cgrp * 2 : 1024;
            SlotGroup *p = realloc(grp, sizeof *p * (size_t)n);
            if (!p) rt_fatal("vk: out of memory for texture slot groups");
            grp = p, cgrp = n;
        }
        g = ngrp++;
    }
    SlotGroup *G = &grp[g];
    memset(G, 0, sizeof *G);
    G->pub.kh = kh, G->pub.head = G->tail = -1, G->bucket = b;
    G->gprev = -1, G->gnext = bucket_ghead[b];
    if (G->gnext >= 0) grp[G->gnext].gprev = g;
    bucket_ghead[b] = g;
    nvn_hm_put(&grp_by, &k, (void *)(intptr_t)(g + 1));
    ngrp_live++;
    return g;
}

static void grp_drop(int g) {
    SlotGroup *G = &grp[g];
    GrpKey k = {G->pub.kh, G->bucket};
    nvn_hm_del(&grp_by, &k);
    if (G->gprev >= 0) grp[G->gprev].gnext = G->gnext;
    else bucket_ghead[G->bucket] = G->gnext;
    if (G->gnext >= 0) grp[G->gnext].gprev = G->gprev;
    G->pub.kh = 0, G->pub.head = G->tail = -1, G->pub.n = 0, G->pub.stamp = 0;
    G->gnext = grp_free, grp_free = g;
    ngrp_live--;
}

static void slot_place(int id, uint32_t b, uint64_t kh) {
    if (slot_prev[id] != -2) {
        int32_t g = slot_grp[id];
        int32_t *head = g >= 0 ? &grp[g].pub.head : &bucket_head[slot_bucket[id]];
        if (slot_prev[id] >= 0) slot_next[slot_prev[id]] = slot_next[id];
        else *head = slot_next[id];
        if (slot_next[id] >= 0) slot_prev[slot_next[id]] = slot_prev[id];
        else if (g >= 0) grp[g].tail = slot_prev[id];
        slot_grp[id] = -1;
        if (g >= 0) {
            n_grouped--;
            if (!--grp[g].pub.n) grp_drop(g);
        }
    }
    slot_bucket[id] = (uint16_t)b;
    if (kh) {
        int g = grp_get(kh, b);
        SlotGroup *G = &grp[g];
        slot_grp[id] = g;
        G->pub.n++, n_grouped++;
        slot_next[id] = -1;
        slot_prev[id] = G->tail;
        if (G->tail >= 0) slot_next[G->tail] = id;
        else G->pub.head = id;
        G->tail = id;
        return;
    }
    slot_prev[id] = -1;
    slot_next[id] = bucket_head[b];
    if (bucket_head[b] >= 0) slot_prev[bucket_head[b]] = id;
    bucket_head[b] = id;
}

VkSlotGroup *vk_texture_group_locked(int g) { return &grp[g].pub; }
int vk_texture_group_next_locked(int id) { return slot_next[id]; }
void vk_texture_groups_unstamp_locked(void) {
    for (int g = 0; g < ngrp; g++) grp[g].pub.stamp = 0;
}

uint64_t vk_texture_dirty_pos_locked(void) { return ndirty; }

int vk_texture_dirty_slots_locked(uint64_t *pos, void (*fn)(int id, void *arg), void (*gfn)(int g, void *arg), void *arg) {
    if (!slot_next || ndirty - *pos > DIRTY_N) {
        *pos = ndirty;
        return 0;
    }
    static uint32_t seen[AGEN_N], round;
    for (uint64_t q = *pos; q < ndirty;) {
        uint64_t end = ndirty;
        if (!++round) memset(seen, 0, sizeof seen), round = 1;
        for (; q < end; q++) {
            uint32_t x = dirty[q % DIRTY_N];
            if (x & DIRTY_BUCKET) {
                uint32_t b = x & ~DIRTY_BUCKET;
                if (seen[b] == round) continue;
                seen[b] = round;
                for (int32_t id = bucket_head[b]; id >= 0; id = slot_next[id]) fn(id, arg);
                for (int32_t g = bucket_ghead[b]; g >= 0; g = grp[g].gnext) {
                    if (gfn) gfn(g, arg);
                    else for (int32_t id = grp[g].pub.head; id >= 0; id = slot_next[id]) fn(id, arg);
                }
            } else {
                fn((int)x, arg);
            }
        }
    }
    *pos = ndirty;
    return 1;
}

static volatile int32_t tex_ready;
static void tex_init(void *p) {
    (void)p;
    __atomic_store_n(&tex_ready, 1, __ATOMIC_RELEASE);
    return;
}
static uint64_t hold_t0;
static void *hold_site;
#define TEX_SPINS 2048
__attribute__((noinline)) static void lock(void) {
    if (!__atomic_load_n(&tex_ready, __ATOMIC_ACQUIRE)) plat_once(&tex_once, tex_init, NULL);
    int got = 0;
    if (__atomic_load_n(&tex_lock.owner, __ATOMIC_RELAXED) != plat_thread_id() && vk_exec_shared())
        for (int i = 0; i < TEX_SPINS && !got; i++) {
            if (!__atomic_load_n(&tex_lock.owner, __ATOMIC_RELAXED)) got = plat_rmutex_trylock(&tex_lock);
            if (!got) plat_pause();
        }
    if (!got) plat_rmutex_lock_at(&tex_lock, __builtin_return_address(0));
    if (tex_lock.depth == 1 && perf_lock_holds_on()) hold_t0 = plat_now_ns(), hold_site = __builtin_return_address(0);
}
static void unlock(void) {
    if (tex_lock.depth == 1 && hold_t0) {
        uint64_t d = plat_now_ns() - hold_t0;
        hold_t0 = 0;
        if (d > 1000000) perf_lock_hold(hold_site, d);
    }
    plat_rmutex_unlock(&tex_lock);
}

static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t mip(uint32_t v, unsigned lv) { return umax(v >> lv, 1); }

static void make_key(const tex_t *t, TexKey *k) {
    memset(k, 0, sizeof *k);
    k->addr = (uintptr_t)t->mem;
    k->d = t->d;
    k->has_layout = t->has_layout;
    memcpy(k->layout, t->layout, 8);
}

static int same_but_swizzle(const TexKey *a, const TexKey *b) {
    TexKey x = *a, y = *b;
    x.d.swizzle_r = x.d.swizzle_g = x.d.swizzle_b = x.d.swizzle_a = 0;
    y.d.swizzle_r = y.d.swizzle_g = y.d.swizzle_b = y.d.swizzle_a = 0;
    return !memcmp(&x, &y, sizeof x);
}

static uint64_t n_follow_refused;
static int follow_size_ok(const Tex *p, const TexKey *k) {
    int bw1 = 1, bh1 = 1, bw2 = 1, bh2 = 1;
    nvn_fmt_bpb(p->key.d.format, &bw1, &bh1);
    nvn_fmt_bpb(k->d.format, &bw2, &bh2);
    if (bw1 > 1 || bh1 > 1 || bw2 > 1 || bh2 > 1) return 1;
    if (p->key.d.width == k->d.width && p->key.d.height == k->d.height && p->key.d.depth == k->d.depth) return 1;
    if (n_follow_refused++ < 8)
        rt_log("vk: a texture slot at %#llx (%ux%u format %#x) did not follow a %ux%u texture at the same storage: "
               "its own image instead", (unsigned long long)k->addr, k->d.width, k->d.height, k->d.format,
               p->key.d.width, p->key.d.height);
    return 0;
}

static const char *mirror_refused(const tex_t *t) {
    if (!t->mem) return "no storage";
    if (nvn_gpu_only((uintptr_t)t->mem)) return "storage in the GPU-only address window";
    if (t->has_layout) return NULL;
    if (!(t->pool_flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED)))
        return "a pool the CPU has no access to";
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL |
                      NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL))
        return "texture flags (one of 0x1, 0x8, 0x80, 0x100, 0x400)";
    if (t->d.samples > 1) return "multisample";
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) return "a buffer texture";
    if (t->d.format >= 0x31 && t->d.format <= 0x36) return "a depth or stencil format";
    return NULL;
}

static int tex_mirrored(const tex_t *t) { return !mirror_refused(t); }

static void log_limited(volatile int32_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_limited(volatile int32_t *n, const char *fmt, ...) {
    if (plat_inc32(n) > 16) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("vk: %s", buf);
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

static VkFormatFeatureFlags features_of(VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, f, &p);
    return p.optimalTilingFeatures;
}

static VkImageAspectFlags aspect_of(const TexVkFormat *f) {
    VkImageAspectFlags a = 0;
    if (f->depth) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (f->stencil) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a ? a : VK_IMAGE_ASPECT_COLOR_BIT;
}

static int format_compressed(VkFormat f) {
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && f <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
}

static nvn_hmap reuse = {.klen = sizeof(ImgKey)};
static int tex_reuse = -1;
static uint64_t n_reused, n_reuse_new;
static uint64_t n_restarts;

#define OVERWRITE_FRAMES 2
#define OVERWRITTEN_KEEP PRESENTS(600)
static struct Tex **gwin;
static int ngwin, cgwin;
static uint64_t gwin_span;
static uint64_t n_overwritten, n_over_drops;

static int gwin_lower(uint64_t a) {
    int lo = 0, hi = ngwin;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (gwin[m]->key.addr < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static void gwin_add(Tex *e) {
    if (ngwin == cgwin) {
        cgwin = cgwin ? 2 * cgwin : 256;
        if (!(gwin = realloc(gwin, sizeof *gwin * (size_t)cgwin))) rt_fatal("vk: out of memory for the render target list");
    }
    int i = gwin_lower(e->key.addr);
    memmove(gwin + i + 1, gwin + i, sizeof *gwin * (size_t)(ngwin - i));
    gwin[i] = e, ngwin++;
    if (e->t.L.size > gwin_span) gwin_span = e->t.L.size;
    e->in_gwin = 1;
}

static void gwin_remove(Tex *e) {
    if (!e->in_gwin) return;
    for (int i = gwin_lower(e->key.addr); i < ngwin && gwin[i]->key.addr == e->key.addr; i++)
        if (gwin[i] == e) {
            memmove(gwin + i, gwin + i + 1, sizeof *gwin * (size_t)(ngwin - i - 1));
            ngwin--;
            break;
        }
    e->in_gwin = 0;
}

static int tex_reuse_on(void) {
    if (tex_reuse < 0) {
        tex_reuse = cfg_bool("NX2SARIA_TEX_REUSE");
        rt_log("vk: texture image reuse %s (NX2SARIA_TEX_REUSE)", tex_reuse ? "on" : "off");
    }
    return tex_reuse;
}

static void destroy_tex(Tex *e);
static int used_lately(const struct Tex *e, uint64_t now, uint64_t within);
static int sampled_lately(const struct Tex *e, uint64_t now, uint64_t within);
static int marks_tell(void);
#define REUSE_UNUSED 16
#define REUSE_UNUSED_FULL 8
static int over_share;
#define REUSE_LOOK 1024
static uint64_t n_reuse_young, n_reuse_used, n_reuse_slot;

static uint64_t n_reuse_named;
static int slot_names(const Tex *e) {
    static int off = -1;
    if (off < 0) off = cfg_bool("NX2SARIA_TEX_REUSE_NAMED");
    if (off || !sinfo) return 0;
    for (int id = bucket_head[agen_idx(e->key.addr)]; id >= 0; id = slot_next[id]) {
        const TexSlot *s = &tslots[id];
        if (s->finalized || (uintptr_t)s->t.mem != e->key.addr) continue;
        TexKey sk;
        make_key(&s->t, &sk);
        if (memcmp(&sk, &e->key, sizeof sk)) continue;
        if (!n_reuse_named++ || !(n_reuse_named & (n_reuse_named - 1)))
            rt_log("vk: a released texture keeps its image, a registered slot still names it: %#llx fmt %#x %ux%u, slot "
                   "%d (%llu so far; NX2SARIA_TEX_REUSE_NAMED=1 lets such an image go to another texture)",
                   (unsigned long long)e->key.addr, e->t.d.format, e->t.d.width, e->t.d.height, id,
                   (unsigned long long)n_reuse_named);
        return 1;
    }
    return 0;
}

enum { DROP_NONE, DROP_MAX, DROP_CAP, DROP_COMMIT, DROP_VRAM, DROP_IDLE_VRAM, DROP_IDLE_COMMIT, DROP_DISUSE,
       DROP_OVERWRITTEN, DROP_REPLACED, DROP_REUSED, DROP_RANGE, DROP_EXACT, DROP_SHUTDOWN, DROP_COMPACT,
       DROP_AGE, DROP_HARD, DROP_N };
static void drop_tex(Tex *e, int why);
static void reuse_dropped(const Tex *e, int why);
static void reuse_made(const Tex *e);
static void reuse_taken_back(const Tex *e);
static int compact_on(void);
static void compact_step(void);
static void residency_step(void);
static void compact_forget(Tex *e);
enum { START_NEW, START_UPLOAD_LIVE, START_COPY_LIVE, START_COPY_RELEASED, START_EMPTY_LIVE, START_EMPTY_RELEASED,
       START_COPY_GONE, START_FILL, START_N };
enum { HAD_COPY = 1, HAD_TEXELS = 2, HAD_FILL = 4 };

#define SPLIT_FILL_MAX ((uint64_t)32 << 20)
static int split_mode = -1;
static uint64_t n_split_copy, n_split_fill, n_split_bytes, n_split_ns, n_split_refill, n_split_reused, n_split_big;
static uint64_t split_total[2];

static int split_fill_mode(void) {
    if (split_mode < 0) {
        int v = cfg_int("NX2SARIA_TEX_SPLIT_FILL");
        split_mode = v == 0 ? 0 : v == 2 ? 2 : 1;
        rt_log("vk: a texture the CPU does not write, made at storage the CPU writes, %s (NX2SARIA_TEX_SPLIT_FILL=%d)",
               !split_mode ? "starts empty" : split_mode == 2 ? "starts with the storage's bytes, once"
                                                             : "starts with the storage's bytes and follows them", split_mode);
    }
    return split_mode;
}

void vk_texture_split_fill_set(int mode) { split_mode = mode; }
void vk_texture_split_counts(uint64_t *copies, uint64_t *fills) { *copies = split_total[0], *fills = split_total[1]; }

static int split_wants(const tex_t *t, const TexVkFormat *f) {
    if (!split_fill_mode() || !t->mem || nvn_gpu_only((uintptr_t)t->mem) || tex_mirrored(t)) return 0;
    if (!(t->pool_flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (f->depth || f->stencil || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    int bw, bh;
    return nvn_fmt_bpb(t->d.format, &bw, &bh) != 0;
}

static int from_storage(const Tex *e) { return e->pub.mirrored || e->sfill == 1 || (e->sfill == 2 && !e->uploaded); }

static int pool_fits(VkCtx *c, const Tex *e, const VkMemoryRequirements *req, uint64_t guest_size, VkMemSpan *sp) {
    static int oversize_alias = -1;
    static volatile int32_t n_oversize;
    if (oversize_alias < 0) oversize_alias = rt_knob("TEX_OVERSIZE_ALIAS", "0") != 0;
    if (req->size > guest_size) {
        int ds = e->f.depth || e->f.stencil;
        if (ds || !oversize_alias) {
            if (!ds)
                log_limited(&n_oversize, "texture %#llx (%ux%ux%u, type %u, vk format %u) needs %llu bytes, its "
                            "storage %llu: memory of its own, so the excess lies over no other texture",
                            (unsigned long long)e->pub.addr, e->ik.w, e->ik.h, e->ik.d, e->ik.type, e->ik.format,
                            (unsigned long long)req->size, (unsigned long long)guest_size);
            return 0;
        }
    }
    if (!req->size) return 0;
    if (!vk_mem_lookup(e->pub.addr, req->size, sp) || !sp->alloc || sp->alloc_offset % req->alignment) return 0;
    VmaAllocationInfo info;
    vmaGetAllocationInfo(c->vma, sp->alloc, &info);
    return req->memoryTypeBits >> info.memoryType & 1;
}

static nvn_hmap img_reqs = {.klen = sizeof(ImgKey)};
static uint64_t n_bind_skips;
#define DEDICATED_MIN ((VkDeviceSize)32 << 20)
#define RES_DEDICATED_MIN ((VkDeviceSize)1 << 20)
static int res_dedicated(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2SARIA_TEX_RESIDENCY");
    return on;
}

static int bind_pool_memory(VkCtx *c, Tex *e, VkImage img, uint64_t guest_size) {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c->device, img, &req);
    if (!nvn_hm_get(&img_reqs, &e->ik)) {
        VkMemoryRequirements *q = malloc(sizeof *q);
        if (!q) rt_fatal("vk: out of memory");
        *q = req;
        nvn_hm_put(&img_reqs, &e->ik, q);
    }
    VkMemSpan sp;
    if (!pool_fits(c, e, &req, guest_size, &sp)) return 0;
    if (vmaBindImageMemory2(c->vma, sp.alloc, sp.alloc_offset, img, NULL) != VK_SUCCESS) return 0;
    e->bound_to = sp.alloc;
    return 1;
}

static void img_key_of(const VkImageCreateInfo *ci, ImgKey *k) {
    memset(k, 0, sizeof *k);
    k->flags = ci->flags, k->type = ci->imageType, k->format = ci->format, k->w = ci->extent.width;
    k->h = ci->extent.height, k->d = ci->extent.depth, k->levels = ci->mipLevels, k->layers = ci->arrayLayers;
    k->samples = ci->samples, k->usage = ci->usage;
}

static int block_views_ok(const VkImageCreateInfo *ci) {
    VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                           .format = ci->format, .type = ci->imageType, .tiling = ci->tiling,
                                           .usage = ci->usage, .flags = ci->flags};
    VkImageFormatProperties2 p = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if (vkGetPhysicalDeviceImageFormatProperties2(vk_ctx()->phys, &fi, &p) != VK_SUCCESS) return 0;
    const VkImageFormatProperties *q = &p.imageFormatProperties;
    return ci->extent.width <= q->maxExtent.width && ci->extent.height <= q->maxExtent.height &&
           ci->extent.depth <= q->maxExtent.depth && ci->mipLevels <= q->maxMipLevels &&
           ci->arrayLayers <= q->maxArrayLayers;
}

static VkDeviceSize image_size(const VkImageCreateInfo *ci) {
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = ci};
    VkMemoryRequirements2 r = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(vk_ctx()->device, &q, &r);
    return r.memoryRequirements.size;
}

static void init_image(Tex *e, VkCommandBuffer cb) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = e->reused ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
                                .srcAccessMask = e->reused ? VK_ACCESS_2_MEMORY_WRITE_BIT : 0,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image, .subresourceRange = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    if (e->f.depth || e->f.stencil) {
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    } else if (vk_fix_zero() && !e->bound_to && !e->pub.mirrored && !format_compressed(e->f.vk)) {
        VkClearColorValue zero = {0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    }
}

static int depth_init_inline(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("DEPTH_INIT_INLINE", "1") != 0;
    return on;
}

static void init_new(Tex *e) {
    int ds = !e->reused && (e->f.depth || e->f.stencil) && depth_init_inline();
    if (e->reused || ds) {
        int reopen;
        VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
        if (cb) {
            init_image(e, cb);
            vk_exec_inline_end(reopen);
            if (ds) n_dinit_inline++, n_dinit_inpass += reopen != 0;
            return;
        }
    }
    if (ds) n_dinit_upload++;
    init_image(e, vk_upload_begin());
    vk_upload_end();
}

static int shadow_scale(void) {
    static int scale = -1;
    if (scale < 0) {
        int s = cfg_int("NX2SARIA_SHADOW_SCALE");
        s = s < 1 ? 1 : s > 4 ? 4 : s;
        rt_log("vk: shadow map scale %d", s);
        scale = s;
    }
    return scale;
}

static int shadow_scale_of(const nvn_texdesc *d, const TexVkFormat *f, uint32_t samples, int mirrored) {
    int scale = shadow_scale();
    if (scale == 1 || mirrored || samples > 1 || d->format != NVN_FORMAT_DEPTH16 || !f->depth) return 1;
    if (d->target != NVN_TEXTURE_TARGET_2D_ARRAY || d->width != d->height || d->width < 256 || d->depth < 2 || d->depth > 4)
        return 1;
    uint32_t most = vk_ctx()->props.limits.maxImageDimension2D;
    return d->width * (uint32_t)scale <= most ? scale : 1;
}

static int shadow_pcf(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_SHADOW_PCF");
        rt_log("vk: depth-compare samplers: %s (NX2SARIA_SHADOW_PCF=%d)",
               on ? "linear, each compare blends the 2x2 texels around it" : "the game's filters", on);
    }
    return on;
}

static void compare_sampler_note(const samp_par *d) {
    static uint32_t seen[8];
    static int n;
    uint32_t kind = (uint32_t)d->minf | (uint32_t)d->magf << 8 | (uint32_t)d->cmp_func << 16 | 1u << 24;
    for (int i = 0; i < n; i++)
        if (seen[i] == kind) return;
    if (n == 8) return;
    seen[n++] = kind;
    rt_log("vk: depth-compare sampler: min filter %u, mag filter %u (0 nearest, 1 linear, more: mipmapped), function %u",
           (unsigned)d->minf, (unsigned)d->magf, (unsigned)d->cmp_func);
}

#define ASTC_OPAQUE_DECODES 4096
static uint64_t n_bc1_made, n_bc1_made_bytes, n_bc1_widened, n_bc1_checks, n_bc1_check_ns, n_bc1_check_bytes;

static int astc_opaque(const tex_t *t, uint64_t addr, const TexVkFormat *f) {
    uint64_t t0 = kern_now_ns(), n = t->L.size / 16;
    if (!t->mem || !n || !mem_read_hold(addr, t->L.size)) return 0;
    uint32_t decoded = 0;
    int ok = astc_blocks_opaque_mt((const uint8_t *)t->mem, n, f->astc_bw, f->astc_bh, f->astc_srgb != 0,
                                   ASTC_OPAQUE_DECODES, &decoded, NULL);
    mem_read_release();
    n_bc1_checks++, n_bc1_check_ns += kern_now_ns() - t0, n_bc1_check_bytes += t->L.size;
    return ok;
}

#define DETAIL_MIN 256
#define DETAIL_LOW_MIN 1024
#define EXACT_MAX 65536
static nvn_hmap exact = {.klen = sizeof(TexKey)};
static uint64_t n_exact;

static int alias_has_gpu_content(const struct Tex *src);

static int64_t detail_budget_mb = -1;

static int detail_shared(void) {
    const VkCtx *c = vk_ctx();
    return c && c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
}

int vk_texture_detail_auto(void) {
    int64_t mb = __atomic_load_n(&detail_budget_mb, __ATOMIC_RELAXED);
    if (mb < 0 && !vk_ctx()) return 0;
    if (mb < 0) {
        uint64_t used = 0, budget = 0;
        int exact_budget = 0;
        mb = vk_mem_vram(&used, &budget, &exact_budget) ? (int64_t)(budget >> 20) : 0;
        __atomic_store_n(&detail_budget_mb, mb, __ATOMIC_RELAXED);
    }
    return tex_detail_auto(mb, detail_shared(), vk_astc_mode() != VK_ASTC_NATIVE && !vk_astc_bc());
}

static const char *const detail_names[3] = {"full", "reduced", "low"};
static int detail_chosen = -1;

const char *vk_texture_detail_name(void) {
    int most = __atomic_load_n(&detail_chosen, __ATOMIC_RELAXED);
    return most < 0 ? "not chosen yet (no texture made)" : detail_names[most];
}

static int detail_most(void) {
    static int most = -1;
    if (most >= 0) return most;
    const char *v = cfg_value("NX2SARIA_TEXTURE_DETAIL");
    most = !strcmp(v, "full") ? 0 : !strcmp(v, "reduced") ? 1 : !strcmp(v, "low") ? 2 : -1;
    if (most >= 0) {
        rt_log("vk: texture detail: %s (texture_detail)", v);
    } else {
        most = vk_texture_detail_auto();
        if (!detail_budget_mb)
            rt_log("vk: texture detail: full (texture_detail = %s; the device reports no video memory budget)", v);
        else
            rt_log("vk: texture detail: %s, chosen for a video memory budget of %lld MiB (texture_detail = %s; full from "
                   "%d MiB, low under %d MiB%s%s)", detail_names[most],(long long)detail_budget_mb, v, DETAIL_FULL_MB, DETAIL_LOW_MB,
                   detail_shared() && detail_budget_mb >= DETAIL_FULL_MB
                       ? "; not full on an integrated GPU, whose budget is a share of the system's memory" : "",
                   vk_astc_mode() != VK_ASTC_NATIVE && !vk_astc_bc() && detail_budget_mb < DETAIL_RGBA8_MB
                       ? "; one step lower because ASTC textures stay uncompressed here" : "");
    }
    __atomic_store_n(&detail_chosen, most, __ATOMIC_RELAXED);
    return most;
}

static uint32_t detail_skip(const tex_t *t, const TexKey *k, const TexVkFormat *f, uint32_t w, uint32_t h, uint32_t levels,
                            uint32_t samples) {
    int most = detail_most();
    const nvn_texdesc *d = &t->d;
    if (!most || !f->astc_bw || samples > 1 || levels < 2 || w < DETAIL_MIN || h < DETAIL_MIN) return 0;
    if ((d->target != NVN_TEXTURE_TARGET_2D && d->target != NVN_TEXTURE_TARGET_2D_ARRAY) || nvn_tex_is_linear(d)) return 0;
    if (!tex_mirrored(t) || (exact.n && nvn_hm_get(&exact, k))) return 0;
    for (const struct Tex *p = nvn_hm_get(&by_addr, &k->addr); p; p = p->next_at)
        if (!p->released && alias_has_gpu_content(p)) return 0;
    return most >= 2 && levels >= 4 && w >= DETAIL_LOW_MIN && h >= DETAIL_LOW_MIN ? 2 : 1;
}

static uint64_t placed[VK_MAX_MEMORY_HEAPS], n_spilled;

static void note_placement(VkCtx *c, VmaAllocation a, int step) {
    VmaAllocationInfo info;
    vmaGetAllocationInfo(c->vma, a, &info);
    uint32_t heap = c->mem_props.memoryTypes[info.memoryType].heapIndex;
    if (heap >= VK_MAX_MEMORY_HEAPS) return;
    uint64_t n = ++placed[heap];
    int first_heap = 1;
    for (uint32_t h = 0; h < heap; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) first_heap = 0;
    if (step == 0 && first_heap && (c->mem_props.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) return;
    if (step) n_spilled++;
    if (n <= 4 || !(n & (n - 1)))
        rt_log("vk: texture memory: %llu images in heap %u (%s)%s", (unsigned long long)n, heap,
               c->mem_props.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "video memory" : "system memory",
               step == 2 ? ", past the budget" : "");
}

static VkResult image_alloc(VkCtx *c, const VkImageCreateInfo *ci, VmaAllocationCreateFlags own, VkImage *img,
                            VmaAllocation *a) {
    static const struct { VkMemoryPropertyFlags req; VmaAllocationCreateFlags flags; } step[2] = {
        {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT},
        {0, 0}};
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (int i = 0; i < 2; i++) {
        int target = (ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_STORAGE_BIT)) != 0;
        VmaAllocationCreateInfo ai = {.flags = step[i].flags | own, .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                      .requiredFlags = step[i].req, .priority = target ? 1.0f : 0.3f};
        r = vmaCreateImage(c->vma, ci, &ai, img, a, NULL);
        if (r == VK_SUCCESS) {
            note_placement(c, *a, i ? 2 : 0);
            return r;
        }
    }
    return r;
}

enum { SUBALLOC_MAX = 32 << 20 };
static VkImage suballoc_image(VkCtx *c, const VkImageCreateInfo *ci, VmaAllocation *alloc) {
    VkImage img;
    if (vkCreateImage(c->device, ci, NULL, &img) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryDedicatedRequirements dr = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 mr2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr};
    VkImageMemoryRequirementsInfo2 ri = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = img};
    vkGetImageMemoryRequirements2(c->device, &ri, &mr2);
    VkMemoryRequirements mr = mr2.memoryRequirements;
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, .priority = 0.3f};
    int ok = !dr.requiresDedicatedAllocation && mr.size <= SUBALLOC_MAX && !(res_dedicated() && mr.size >= RES_DEDICATED_MIN) &&
             vmaAllocateMemory(c->vma, &mr, &ai, alloc, NULL) == VK_SUCCESS;
    if (ok) note_placement(c, *alloc, 0);
    if (ok && vmaBindImageMemory(c->vma, *alloc, img) != VK_SUCCESS) {
        vmaFreeMemory(c->vma, *alloc);
        ok = 0;
    }
    if (!ok) {
        vkDestroyImage(c->device, img, NULL);
        img = VK_NULL_HANDLE, *alloc = NULL;
    }
    return img;
}

static VkResult vma_image(VkCtx *c, const VkImageCreateInfo *ci, const ImgKey *ik, VkImage *img, VmaAllocation *alloc) {
    const VkMemoryRequirements *known = nvn_hm_get(&img_reqs, ik);
    VkDeviceSize size = known ? known->size : image_size(ci);
    VmaAllocationCreateFlags own = size >= DEDICATED_MIN || (res_dedicated() && size >= RES_DEDICATED_MIN)
                                       ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
    return image_alloc(c, ci, own, img, alloc);
}

static void own_memory(VkCtx *c, Tex *e) {
    VmaAllocationInfo2 own;
    vmaGetAllocationInfo2(c->vma, e->alloc, &own);
    e->bytes = own.allocationInfo.size;
    e->blk_mem = (uint64_t)(uintptr_t)own.allocationInfo.deviceMemory, e->blk_size = own.blockSize;
    e->blk_type = (uint8_t)own.allocationInfo.memoryType, e->blk_own = own.dedicatedMemory != 0;
}

static Tex **rts;
static int nrts, crts;
static uint64_t max_rt;
static nvn_hmap private_keys = {.klen = sizeof(TexKey)};
static int clobber_fix = -1;
static volatile int32_t n_unaliased;
static uint64_t n_unaliased_period, n_unalias_kept;

static int rts_lower(uint64_t a) {
    int lo = 0, hi = nrts;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (rts[m]->pub.addr < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static void rt_add(Tex *e) {
    if (e->in_rt) return;
    if (nrts == crts) {
        crts = crts ? 2 * crts : 256;
        if (!(rts = realloc(rts, sizeof *rts * (size_t)crts))) rt_fatal("vk: out of memory for the render target list");
    }
    int i = rts_lower(e->pub.addr);
    memmove(rts + i + 1, rts + i, sizeof *rts * (size_t)(nrts - i));
    rts[i] = e;
    nrts++;
    if (e->t.L.size > max_rt) max_rt = e->t.L.size;
    e->in_rt = 1;
}

static void rt_remove(Tex *e) {
    if (!e->in_rt) return;
    for (int i = rts_lower(e->pub.addr); i < nrts && rts[i]->pub.addr == e->pub.addr; i++)
        if (rts[i] == e) {
            memmove(rts + i, rts + i + 1, sizeof *rts * (size_t)(nrts - i - 1));
            nrts--;
            break;
        }
    e->in_rt = 0;
}

static int alias_compatible(const Tex *e, const Tex *src, const char **why);

static void mark_clobbered(Tex *e) {
    if (clobber_fix < 0) {
        clobber_fix = cfg_bool("NX2SARIA_TEX_CLOBBER");
        rt_log("vk: a texture read after another image drew over its pool memory %s (NX2SARIA_TEX_CLOBBER, Fix "
               "overwritten textures on the Screen tab)", clobber_fix ? "gets memory of its own" : "stays in pool memory");
    }
    e->clobbered = 0;
    if (!clobber_fix || !e->bound_to) return;
    rt_add(e);
    uint64_t lo = e->pub.addr, hi = lo + e->t.L.size;
    for (int i = rts_lower(lo > max_rt ? lo - max_rt : 0); i < nrts && rts[i]->pub.addr < hi; i++) {
        Tex *o = rts[i];
        if (o == e || o->clobbered || o->bound_to != e->bound_to || o->pub.addr + o->t.L.size <= lo) continue;
        const char *why;
        if (o->pub.image == e->pub.image || (alias_compatible(o, e, &why) && o->pub.width == e->pub.width &&
                                             o->pub.height == e->pub.height && o->pub.depth == e->pub.depth))
            continue;
        o->clobbered = 1;
    }
}

static Tex *create_tex(const tex_t *t, const TexKey *k, int defer_init) {
    static volatile int32_t n_unmapped, n_unsupported, n_failed, n_unbound, n_thick;
    VkCtx *c = vk_ctx();
    const nvn_texdesc *d = &t->d;
    if (!c || d->target == NVN_TEXTURE_TARGET_BUFFER) return NULL;
    TexVkFormat f;
    if (!tex_vk_format(d->format, &f)) {
        log_limited(&n_unmapped, "texture format %#x has no Vulkan format: %s", d->format, f.reason);
        return NULL;
    }
    if (!tex_vk_format_supported(&f)) {
        log_limited(&n_unsupported, "texture format %#x: VkFormat %d lacks features %#x", d->format, (int)f.vk, (unsigned)f.need);
        return NULL;
    }
    int bc1_try = f.astc_bc == 1 && tex_mirrored(t) && !nvn_gpu_only(k->addr) && vk_astc_bc1();
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    uint32_t w = umax(d->width, 1), h = umax(d->height, 1), depth = 1, layers = 1;
    VkImageType type = VK_IMAGE_TYPE_2D;
    switch (d->target) {
    case NVN_TEXTURE_TARGET_1D: type = VK_IMAGE_TYPE_1D; h = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: type = VK_IMAGE_TYPE_1D; h = 1; layers = L.layers; break;
    case NVN_TEXTURE_TARGET_3D: type = VK_IMAGE_TYPE_3D; depth = umax(d->depth, 1); break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
    case NVN_TEXTURE_TARGET_CUBEMAP: case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: layers = L.layers; break;
    default: break;
    }
    int scale = shadow_scale_of(d, &f, d->samples > 1 ? (uint32_t)d->samples : 1, tex_mirrored(t));
    w *= (uint32_t)scale, h *= (uint32_t)scale;
    uint32_t levels = umax(d->levels, 1), most = 1;
    for (uint32_t s = umax(umax(w, h), depth); s > 1; s >>= 1) most++;
    if (levels > most) levels = most;
    uint32_t samples = d->samples > 1 ? (uint32_t)d->samples : 1;
    if (samples > 1) levels = 1;
    uint32_t skip = detail_skip(t, k, &f, w, h, levels, samples);
    if (skip) w = mip(w, skip), h = mip(h, skip), levels -= skip;
    VkFormatFeatureFlags ff = features_of(f.vk);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    int ds = f.depth || f.stencil;
    if (ds && (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!ds && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((d->flags & NVN_TEXTURE_FLAGS_IMAGE) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples == 1)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (rt_nvn_api_version[0] >= 200 && !ds && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples == 1)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (f.vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && samples == 1 &&
        (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && w >= 640 && vk_contact_strength())
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = ds ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && w == h && samples == 1 &&
        (d->target == NVN_TEXTURE_TARGET_CUBEMAP || d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY))
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = f.vk,
                            .extent = {w, h, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    int bw, bh, block_views = 0;
    static int bv3d = -1;
    if (bv3d < 0) {
        bv3d = rt_knob("BLOCK_VIEWS_3D", "0");
        rt_log("vk: 3D BC textures %s (NX2SARIA_BLOCK_VIEWS_3D=%d)", bv3d ? "get block views (renderable)" : "have no block views",
               bv3d);
    }
    if (!ds && samples == 1 && f.vk == f.ideal && nvn_fmt_bpb(d->format, &bw, &bh) && bw == 4 && bh == 4 &&
        (type != VK_IMAGE_TYPE_3D || bv3d)) {
        VkImageCreateInfo bc = ci;
        bc.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        bc.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (type == VK_IMAGE_TYPE_3D) bc.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        VkImageUsageFlags hidden = 0;
        int ok = block_views_ok(&bc);
        if (ok && type == VK_IMAGE_TYPE_3D && !image_size(&bc)) {
            bc.usage |= VK_IMAGE_USAGE_STORAGE_BIT, hidden = VK_IMAGE_USAGE_STORAGE_BIT;
            ok = block_views_ok(&bc) && image_size(&bc);
            log_limited(&n_thick, "texture fmt %#x %ux%ux%u: the device cannot lay out a 3D image with block views; %s",
                        d->format, w, h, depth, ok ? "made with storage usage (a thick layout)" : "made without them");
        }
        if (ok) ci = bc, usage = bc.usage & ~hidden, flags = bc.flags, block_views = 1;
    }
    int fb_usage = c->feedback == VK_FEEDBACK_LAYOUT && !block_views &&
                   (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    if (fb_usage) ci.usage = usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    Tex *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("vk: out of memory for a texture");
    e->key = *k;
    e->t = *t;
    e->f = f;
    e->pub.addr = k->addr;
    img_key_of(&ci, &e->ik);
    VkImage img = VK_NULL_HANDLE;
    if (scale == 1 && nvn_gpu_only(k->addr) && !nvn_hm_get(&private_keys, k)) {
        const VkMemoryRequirements *q = nvn_hm_get(&img_reqs, &e->ik);
        VkMemSpan sp;
        if (q && !pool_fits(c, e, q, L.size, &sp)) {
            n_bind_skips++;
            log_limited(&n_unbound, "texture %#llx (%ux%u fmt %#x) cannot alias its pool memory; own memory instead",
                        (unsigned long long)k->addr, w, h, d->format);
        } else if (vkCreateImage(c->device, &ci, NULL, &img) == VK_SUCCESS && !bind_pool_memory(c, e, img, L.size)) {
            vkDestroyImage(c->device, img, NULL);
            img = VK_NULL_HANDLE;
            log_limited(&n_unbound, "texture %#llx (%ux%u fmt %#x) cannot alias its pool memory; own memory instead",
                        (unsigned long long)k->addr, w, h, d->format);
        }
    }
    if (!img && tex_mirrored(t) && samples == 1) img = suballoc_image(c, &ci, &e->alloc);
    int gpu_only = !tex_mirrored(t);
    int to_fill = gpu_only && split_wants(t, &f) && L.size <= SPLIT_FILL_MAX;
    for (const Tex *p = to_fill ? nvn_hm_get(&by_addr, &k->addr) : NULL; p; p = p->next_at)
        if (alias_has_gpu_content(p)) { to_fill = 0; break; }
    if (!img && gpu_only && !to_fill && tex_reuse_on()) {
        uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        Tex *a = NULL, *kept = NULL;
        uint64_t best = UINT64_MAX, best_kept = UINT64_MAX;
        int n = 0;
        int marks = marks_tell();
        for (Tex *q = nvn_hm_get(&reuse, &e->ik); q && n < REUSE_LOOK; q = q->ru_next, n++) {
            if (q->key.addr == k->addr) continue;
            if (q->rel_epoch == submit_epoch || now - q->rel_frame < 2) { n_reuse_young++; continue; }
            int over = q->overwritten && marks;
            if (over ? sampled_lately(q, now, over_share ? REUSE_UNUSED_FULL : REUSE_UNUSED)
                     : used_lately(q, now, REUSE_UNUSED)) {
                n_reuse_used++;
                continue;
            }
            uint64_t dist = q->key.addr > k->addr ? q->key.addr - k->addr : k->addr - q->key.addr;
            if (q->overwritten) {
                if (dist >= best) continue;
                if (!over && slot_names(q)) { n_reuse_slot++; continue; }
                best = dist, a = q;
            } else if (dist < best_kept) {
                if (slot_names(q)) { n_reuse_slot++; continue; }
                best_kept = dist, kept = q;
            }
        }
        if (!a) a = kept;
        if (a) {
            img = a->pub.image, e->alloc = a->alloc;
            vk_exec_image_gone(img);
            a->pub.image = VK_NULL_HANDLE, a->alloc = NULL;
            drop_tex(a, DROP_REUSED);
            e->reused = 1, e->og_reimg = 1;
            n_reused++;
        }
    }
    if (!img) {
        if (gpu_only) n_reuse_new++;
        VkResult r = vma_image(c, &ci, &e->ik, &img, &e->alloc);
        if (r != VK_SUCCESS) {
            log_limited(&n_failed, "image creation failed (%d): nvn fmt %#x %ux%ux%u target %d levels %u layers %u", (int)r,
                        d->format, w, h, depth, d->target, levels, layers);
            free(e);
            return NULL;
        }
    }
    if (e->alloc) own_memory(c, e);
    e->bc1_try = (uint8_t)bc1_try;
    e->reusable = e->alloc && gpu_only;
    e->gw_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (e->reusable && nvn_gpu_only(k->addr)) gwin_add(e);
    if (!e->reused) {
        static volatile int32_t n_stale;
        int stale = vk_exec_image_gone(img);
        if (stale)
            log_limited(&n_stale, "new image %#llx had %d cached views of a destroyed image with its handle; dropped",
                        (unsigned long long)(uintptr_t)img, stale);
    }
    VkTexImage *p = &e->pub;
    p->image = img;
    p->format = f.vk;
    p->type = type;
    p->usage = usage;
    p->aspect = aspect_of(&f);
    p->width = w, p->height = h, p->depth = depth, p->levels = levels, p->layers = layers, p->samples = samples;
    p->view_type = type == VK_IMAGE_TYPE_1D ? (d->target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || d->target == NVN_TEXTURE_TARGET_2D_ARRAY || d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY
                     ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    p->alpha_one = f.alpha_one;
    memcpy(p->chan, f.chan, sizeof p->chan);
    p->swz[0] = (uint8_t)d->swizzle_r, p->swz[1] = (uint8_t)d->swizzle_g;
    p->swz[2] = (uint8_t)d->swizzle_b, p->swz[3] = (uint8_t)d->swizzle_a;
    if (!(p->swz[0] | p->swz[1] | p->swz[2] | p->swz[3]))
        p->swz[0] = NVN_TEXTURE_SWIZZLE_R, p->swz[1] = NVN_TEXTURE_SWIZZLE_G, p->swz[2] = NVN_TEXTURE_SWIZZLE_B,
        p->swz[3] = NVN_TEXTURE_SWIZZLE_A;
    p->mirrored = (uint8_t)tex_mirrored(t);
    p->bound = e->bound_to != NULL;
    p->block_views = (uint8_t)block_views;
    p->fb_usage = (uint8_t)fb_usage;
    p->scale = (uint8_t)scale;
    p->skip = (uint8_t)skip;

    if (!defer_init) init_new(e);
    return e;
}

static int owned_lower(uint64_t a) {
    int lo = 0, hi = nowned;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (owned[m]->pub.addr < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static void own_add(Tex *e) {
    if (e->listed) return;
    plat_mutex_lock(&owned_lock);
    if (nowned == cowned) {
        cowned = cowned ? 2 * cowned : 256;
        if (!(owned = realloc(owned, sizeof *owned * (size_t)cowned))) rt_fatal("vk: out of memory for the owned-texture list");
    }
    int i = owned_lower(e->pub.addr);
    memmove(owned + i + 1, owned + i, sizeof *owned * (size_t)(nowned - i));
    owned[i] = e;
    __atomic_store_n(&nowned, nowned + 1, __ATOMIC_RELEASE);
    if (e->t.L.size > max_owned) max_owned = e->t.L.size;
    e->listed = 1;
    plat_mutex_unlock(&owned_lock);
}

static void own_remove(Tex *e) {
    if (!e->listed) return;
    plat_mutex_lock(&owned_lock);
    for (int i = owned_lower(e->pub.addr); i < nowned && owned[i]->pub.addr == e->pub.addr; i++)
        if (owned[i] == e) {
            memmove(owned + i, owned + i + 1, sizeof *owned * (size_t)(nowned - i - 1));
            __atomic_store_n(&nowned, nowned - 1, __ATOMIC_RELEASE);
            break;
        }
    e->listed = 0;
    plat_mutex_unlock(&owned_lock);
}

static uint64_t rel_bytes;

static void ru_link(Tex *e) {
    Tex *head = nvn_hm_get(&reuse, &e->ik);
    e->ru_prev = NULL, e->ru_next = head;
    if (head) head->ru_prev = e;
    nvn_hm_put(&reuse, &e->ik, e);
    e->ru_on = 1;
}

static void ru_unlink(Tex *e) {
    if (!e->ru_on) return;
    if (e->ru_prev) e->ru_prev->ru_next = e->ru_next;
    else if (e->ru_next) nvn_hm_put(&reuse, &e->ik, e->ru_next);
    else nvn_hm_del(&reuse, &e->ik);
    if (e->ru_next) e->ru_next->ru_prev = e->ru_prev;
    e->ru_prev = e->ru_next = NULL;
    e->ru_on = 0;
}

static void rel_append(Tex *e) {
    e->rel_next = NULL;
    e->rel_prev = rel_tail;
    if (rel_tail) rel_tail->rel_next = e;
    else rel_head = e;
    rel_tail = e;
    rel_bytes += e->bytes;
    if (e->reusable && tex_reuse_on()) ru_link(e);
}

static Tex *sweep_at;
static Tex *aged_at;
static Tex *press_at;
static void rel_unlink(Tex *e) {
    if (sweep_at == e) sweep_at = e->rel_next;
    if (aged_at == e) aged_at = e->rel_next;
    if (press_at == e) press_at = e->rel_next;
    ru_unlink(e);
    rel_bytes -= e->bytes;
    if (e->rel_prev) e->rel_prev->rel_next = e->rel_next;
    else rel_head = e->rel_next;
    if (e->rel_next) e->rel_next->rel_prev = e->rel_prev;
    else rel_tail = e->rel_prev;
    e->rel_prev = e->rel_next = NULL;
}

typedef struct { Tex *e; uint64_t addr, size, since; uint8_t ok, rel; } PEnt;
static PEnt *pent;
static int npent, cpent, *pq, cpq;
static uint64_t n_pool_checks, n_pool_skips, n_pool_rel;
static uint64_t n_pool_miss[5], n_presnap_q;
static void make_fresh_snap(Tex *e);

static void pool_unlist(Tex *e) {
    if (!e->pidx) return;
    int i = e->pidx - 1;
    pent[i] = pent[--npent];
    pent[i].e->pidx = i + 1;
    e->pidx = 0;
}

#define POOL_TOUCH_DIV 64
typedef struct { Tex *e; uint32_t refs; int next; uint8_t gown; } PRef;
static PRef *pref;
static int npref, cpref, pref_free;
static void touch(Tex *e);

static uint32_t pool_ref(Tex *e) {
    if (e->ph) return (uint32_t)e->ph;
    int i;
    if (pref_free) {
        i = pref_free - 1, pref_free = pref[i].next;
    } else {
        if (npref == cpref) {
            int n = cpref ? cpref * 2 : 1024;
            PRef *q = realloc(pref, sizeof *q * (size_t)n);
            if (!q) rt_fatal("vk: out of memory for the texture pool's entry counts");
            pref = q, cpref = n;
        }
        i = npref++;
    }
    pref[i] = (PRef){e, 0, 0, e->pub.gpu_owned};
    return (uint32_t)(e->ph = i + 1);
}

void vk_texture_pool_hold_locked(uint32_t old, uint32_t ent) {
    if (ent) pref[ent - 1].refs++;
    if (old && !--pref[old - 1].refs && !pref[old - 1].e) pref[old - 1].next = pref_free, pref_free = (int)old;
}

static void pool_forget(Tex *e) {
    if (!e->ph) return;
    PRef *q = &pref[e->ph - 1];
    q->e = NULL;
    if (!q->refs) q->next = pref_free, pref_free = e->ph;
    e->ph = 0;
}

static void pool_touch(void) {
    static int at;
    for (int i = 0; i < npref; i++)
        if (pref[i].gown && pref[i].refs && pref[i].e) touch(pref[i].e);
    for (int k = (npref + POOL_TOUCH_DIV - 1) / POOL_TOUCH_DIV; k-- > 0;) {
        if (at >= npref) at = 0;
        const PRef *q = &pref[at++];
        if (!q->gown && q->refs && q->e) touch(q->e);
    }
}

static void pool_gpu_owned(Tex *e) {
    pool_unlist(e);
    if (e->ph) pref[e->ph - 1].gown = 1;
}

int vk_texture_pool_note_locked(int id, uint32_t *ent) {
    *ent = 0;
    if (id < 0 || id >= ntslots || !tslots[id].used) return 0;
    if (tslots[id].ce && tslots[id].cgen != agen[sinfo[id] & 0xffff]) vk_texture_by_id_locked(id, NULL, NULL);
    Tex *e = tslots[id].ce;
    if (!e) return 0;
    if (!e->pidx && from_storage(e) && !e->pub.gpu_owned) {
        if (npent == cpent) {
            int n = cpent ? cpent * 2 : 1024;
            PEnt *p = realloc(pent, sizeof *p * (size_t)n);
            int *q = realloc(pq, sizeof *q * (size_t)n);
            if (!p || !q) rt_fatal("vk: out of memory for the texture pool's entry list");
            pent = p, pq = q, cpent = cpq = n;
        }
        pent[npent++] = (PEnt){e, e->pub.addr, e->t.L.size, 0, 0, (uint8_t)e->released};
        e->pidx = npent;
    }
    *ent = pool_ref(e);
    return 1;
}

static int pool_presnap(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("POOL_PRESNAP", "1");
    return RT_KNOB_ON(mode);
}
static int pool_presnap_runs(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("POOL_PRESNAP_RUNS", "1");
    return RT_KNOB_ON(mode);
}
static void pool_made(uint64_t t0);

static int pool_by_list;

int vk_texture_pool_fresh_locked(void) {
    int nq = 0, ps = pool_presnap();
    uint64_t rel0 = n_pool_rel;
    pool_by_list = 1;
    if (ps) vk_mem_gen_lock();
    else vk_mem_gen_lock_shared();
    int by_range = ps;
    if (ps && pool_presnap_runs()) {
        for (int i = 0; i < npent; i++)
            if (pent[i].ok && !pent[i].rel) vk_mem_snap_note_locked(pent[i].addr, pent[i].size, submit_epoch);
        n_presnap_q += vk_mem_snap_noted_locked(submit_epoch, &by_range);
    }
    for (int i = 0; i < npent; i++) {
        const PEnt *p = &pent[i];
        if (p->rel) {
            n_pool_rel++;
            continue;
        }
        if (p->ok) {
            if (by_range) n_presnap_q += vk_mem_snap_range_locked(p->addr, p->size, submit_epoch);
            int why = vk_mem_unchanged_why_locked(p->addr, p->size, p->since, submit_epoch);
            if (!why) continue;
            n_pool_miss[why]++;
        } else {
            n_pool_miss[0]++;
        }
        pq[nq++] = i;
    }
    if (ps) vk_mem_gen_unlock();
    else vk_mem_gen_unlock_shared();
    n_pool_skips += (uint64_t)(npent - nq);
    n_pool_skips -= n_pool_rel - rel0;
    for (int k = nq; k-- > 0;) {
        int i = pq[k];
        PEnt *p = &pent[i];
        Tex *e = p->e;
        if (e->pub.gpu_owned) {
            pool_unlist(e);
            continue;
        }
        if (e->released) continue;
        if (!e->uploaded || e->checked != submit_epoch) {
            uint64_t m0 = e->uploaded ? 0 : kern_now_ns();
            make_fresh_snap(e);
            if (m0) pool_made(m0);
            e->checked = submit_epoch;
            n_pool_checks++;
        }
        p->since = e->wgen, p->ok = e->uploaded;
    }
    pool_touch();
    return npent;
}

static int resolve_stale;

static uint64_t n_fin_own, n_fin_other, n_fin_other_hm, n_stale, n_stale_hm, n_stale_kept;
static int terrain_heightmap(const tex_t *t) {
    return t->d.format == NVN_FORMAT_R32F && t->d.width == 260 && t->d.height == 260;
}

int vk_texture_slot_retry_locked(int id) {
    if (id < 0 || id >= ntslots || !tslots[id].used || !tslots[id].t.mem) return -1;
    resolve_stale = 0;
    if (vk_texture_by_id_locked(id, NULL, NULL)) return 1;
    return resolve_stale ? -1 : 0;
}

static void retire_remove(Tex *e);

#define FLAT3D_MAX (1024u * 1024u)
static volatile int32_t n_flat3d;

const VkTexImage *vk_texture_flat3d_locked(const VkTexImage *img, uint32_t layer) {
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_FLAT3D", "1") == 1;
    Tex *e = (Tex *)img;
    if (!on || img->gone || !img->image || img->type != VK_IMAGE_TYPE_2D || img->samples > 1 ||
        img->aspect != VK_IMAGE_ASPECT_COLOR_BIT || img->block_views || format_compressed(img->format) ||
        (uint64_t)img->width * img->height > FLAT3D_MAX || layer >= img->layers || img->levels > 16)
        return NULL;
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!e->flat3d) {
        VkTexImage *f = malloc(sizeof *f);
        if (!f) return NULL;
        *f = *img;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_3D,
                                .format = img->format, .extent = {img->width, img->height, 1}, .mipLevels = img->levels,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &f->image, &e->flat3d_alloc, NULL) != VK_SUCCESS) {
            free(f);
            return NULL;
        }
        f->type = VK_IMAGE_TYPE_3D, f->view_type = VK_IMAGE_VIEW_TYPE_3D, f->depth = 1, f->layers = 1;
        f->usage = ci.usage, f->bound = 0, f->block_views = 0, f->fb_usage = 0, f->fb_learned = 0, f->gone = 0;
        e->flat3d = f;
        e->flat3d_up = ~0u;
        cb = vk_upload_begin();
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                    .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                    .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = f->image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
        log_limited(&n_flat3d, "texture %#llx (%ux%u, %u levels, format %d) is sampled as a 3D texture: a one-slice 3D "
                    "copy of it is made", (unsigned long long)img->addr, img->width, img->height, img->levels,
                    (int)img->format);
    }
    if (e->flat3d_up != e->n_up || e->flat3d_wr != e->wr_fence || e->flat3d_layer != layer) {
        if (!cb) cb = vk_upload_begin();
        VkImageCopy rg[16];
        for (uint32_t l = 0; l < img->levels; l++) {
            uint32_t w = img->width >> l, h = img->height >> l;
            rg[l] = (VkImageCopy){.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1},
                                  .extent = {w ? w : 1, h ? h : 1, 1}};
        }
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, e->flat3d->image, VK_IMAGE_LAYOUT_GENERAL, img->levels, rg);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
        e->flat3d_up = e->n_up, e->flat3d_wr = e->wr_fence, e->flat3d_layer = layer;
    }
    if (cb) vk_upload_end();
    return e->flat3d;
}
#define DROPS_N 4096
typedef struct { uint64_t kh, addr, frame; uint32_t format, flags, w, h; uint8_t why, mirrored, released; } DropRec;
static DropRec drops[DROPS_N];
static uint64_t ndrops;
static uint64_t n_drop_by[DROP_N];
static const char *const drop_names[DROP_N] = {
    "", "past RELEASED_MAX released entries", "past the released entries' size cap", "under commit pressure",
    "under VRAM pressure", "idle, under VRAM pressure", "idle, under commit pressure", "unsampled (disuse)",
    "overwritten at another address", "replaced by another descriptor at its address", "its image went to another texture",
    "its storage went", "made whole again (texture detail)", "shutdown", "released, in a block a compaction empties",
    "released and unused for longer than the hold", "past RELEASED_HARD released entries"};
static const char *const start_names[START_N] = {
    "started on its own", "filled from storage over a live entry", "content copied from a live entry",
    "content copied from a released entry", "started empty over a live entry", "started empty over a released entry",
    "content copied from the released CPU-written entry it replaced (split fill)", "filled from its storage (split fill)"};
const char *vk_texture_start_name(int start) { return start >= 0 && start < START_N ? start_names[start] : "?"; }
const char *vk_texture_drop_name(int why) { return why >= 0 && why < DROP_N ? drop_names[why] : "?"; }

static void flight_ev(const Tex *e, int kind, uint32_t detail, uint32_t us) {
    if (!flight_on) return;
    uint32_t fl = (e->f.depth ? FLIGHT_TF_DEPTH : 0) | (e->f.stencil ? FLIGHT_TF_STENCIL : 0) |
                  (e->pub.mirrored ? FLIGHT_TF_MIRRORED : 0) | (e->bound_to ? FLIGHT_TF_BOUND : 0) |
                  (e->f.astc_bw ? FLIGHT_TF_ASTC : 0) | (e->pub.gpu_owned ? FLIGHT_TF_GPU : 0);
    flight_tex(kind, e->key.addr, (uint32_t)e->t.d.format, e->pub.width, e->pub.height, e->pub.layers, fl, detail, us);
}

static uint64_t frames_now(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); }

static void flight_bytes(const Tex *e) {
    if (!flight_on || !e->pub.mirrored || e->t.L.size < 64 * 64) return;
    if (!mem_read_hold(e->pub.addr, e->t.L.size)) return;
    const uint8_t *b = (const uint8_t *)(uintptr_t)e->pub.addr;
    uint64_t step = (e->t.L.size - 64) / 63;
    uint32_t h = 0x811c9dc5u, zero = 0;
    for (int i = 0; i < 64; i++) {
        const uint8_t *p = b + (uint64_t)i * step;
        uint8_t any = 0;
        for (int k = 0; k < 64; k++) h = (h ^ p[k]) * 0x01000193u, any |= p[k];
        zero += !any;
    }
    mem_read_release();
    flight_ev(e, FLIGHT_TEX_BYTES, h, zero);
}

static uint64_t key_hash(const TexKey *k) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

#define CB_SLOTS 8192
#define CB_WITHIN PRESENTS(600)
#define CB_PERIOD PRESENTS(240)
#define CB_MIN_DROPS 64
#define CB_HOLD_MIN PRESENTS(600)
#define CB_HOLD_MAX PRESENTS(7200)
static struct { uint64_t kh, frame; } cb_slot[CB_SLOTS];
static uint64_t cb_drops, cb_backs, cb_t0, cb_hold, cb_hold_until;
static uint64_t n_commit_back, n_commit_held;
static int commit_critical, cb_logs;

static uint64_t cb_key(const Tex *e) { return key_hash(&e->key) ^ (e->key.addr * 0x9e3779b97f4a7c15ull); }

static void commit_drop_note(const Tex *e) {
    uint64_t kh = cb_key(e);
    cb_slot[kh & (CB_SLOTS - 1)].kh = kh, cb_slot[kh & (CB_SLOTS - 1)].frame = frames_now() | 1;
    cb_drops++;
}

static void commit_back_note(const Tex *e) {
    uint64_t kh = cb_key(e), now = frames_now();
    typeof(cb_slot[0]) *s = &cb_slot[kh & (CB_SLOTS - 1)];
    if (s->kh != kh || !s->frame || now - s->frame > CB_WITHIN) return;
    s->frame = 0;
    cb_backs++, n_commit_back++;
}

static int commit_drops_held(void) {
    uint64_t now = frames_now();
    if (now - cb_t0 >= CB_PERIOD) {
        if (cb_drops >= CB_MIN_DROPS && cb_backs * 2 >= cb_drops) {
            cb_hold = cb_hold ? (cb_hold * 2 < CB_HOLD_MAX ? cb_hold * 2 : CB_HOLD_MAX) : CB_HOLD_MIN;
            cb_hold_until = now + cb_hold;
            if (cb_logs < 12) {
                cb_logs++;
                rt_log("vk: commit low, but %llu of the %llu textures dropped for it were made again within %llu presents: "
                       "the commit drops pause for %llu presents", (unsigned long long)cb_backs,
                       (unsigned long long)cb_drops, (unsigned long long)CB_WITHIN, (unsigned long long)cb_hold);
            }
        } else if (cb_drops >= CB_MIN_DROPS) {
            cb_hold = 0;
        }
        cb_t0 = now, cb_drops = cb_backs = 0;
    }
    if (commit_critical || now >= cb_hold_until) return 0;
    n_commit_held++;
    return 1;
}

static int wb_wanted(const Tex *e);
static void wb_cfg_init(void);
static int storage_rewritten(const Tex *e, uint64_t since, uint64_t *now);
static void write_back_n(Tex **v, int n);
static uint64_t n_drop_wb;

static void drop_tex(Tex *e, int why) {
    flight_ev(e, FLIGHT_TEX_DROP, (uint32_t)why, 0);
    if (why == DROP_COMMIT || why == DROP_IDLE_COMMIT) commit_drop_note(e);
    if (e->gpu_dirty && why != DROP_OVERWRITTEN && why != DROP_SHUTDOWN) {
        uint64_t now;
        wb_cfg_init();
        if (storage_rewritten(e, e->gpu_gen, &now) == 1) e->gpu_dirty = 0;
        else if (wb_wanted(e)) write_back_n(&e, 1), n_drop_wb++;
    }
    n_drop_by[why]++;
    reuse_dropped(e, why);
    if (rt_trace & RT_TRACE_PERF)
        drops[ndrops++ % DROPS_N] = (DropRec){key_hash(&e->key), e->key.addr, frames_now(), (uint32_t)e->t.d.format,
                                              (uint32_t)e->t.d.flags, e->t.d.width, e->t.d.height, (uint8_t)why,
                                              e->pub.mirrored, e->released != 0};
    destroy_tex(e);
}

static void origin_made(Tex *e) {
    e->og_frame = frames_now();
    commit_back_note(e);
    if (!(rt_trace & RT_TRACE_PERF)) return;
    uint64_t kh = key_hash(&e->key), n = ndrops < DROPS_N ? ndrops : DROPS_N;
    for (uint64_t i = 1; i <= n; i++) {
        const DropRec *r = &drops[(ndrops - i) % DROPS_N];
        if (r->kh != kh || r->addr != e->key.addr) continue;
        e->og_drop = r->why, e->og_drop_frame = r->frame;
        return;
    }
}

static int desc_text(char *out, size_t n, const Tex *e) {
    const nvn_texdesc *d = &e->t.d;
    return snprintf(out, n, "fmt %#x (vk %d) %ux%ux%u, %u levels, %u layers, target %d, samples %d, flags %#x, pool flags %#x",
                    d->format, (int)e->f.vk, d->width, d->height, d->depth, e->pub.levels + e->pub.skip, e->pub.layers,
                    d->target, d->samples, (unsigned)d->flags, (unsigned)e->t.pool_flags);
}

static int origin_text(char *out, size_t n, const Tex *e) {
    int k = snprintf(out, n, "made at frame %llu", (unsigned long long)e->og_frame);
    if (e->og_drop && k < (int)n)
        k += snprintf(out + k, n - (size_t)k, " again after a drop at frame %llu (%s)", (unsigned long long)e->og_drop_frame,
                      drop_names[e->og_drop]);
    if (k < (int)n) k += snprintf(out + k, n - (size_t)k, "; %s", start_names[e->og_start]);
    if (e->og_reimg && k < (int)n) k += snprintf(out + k, n - (size_t)k, "; with a released entry's image");
    if (e->og_backs && k < (int)n)
        k += snprintf(out + k, n - (size_t)k, "; taken back %u times, last at frame %llu", e->og_backs,
                      (unsigned long long)e->og_back_frame);
    return k < (int)n ? k : (int)n - 1;
}

static int no_content(const Tex *e) {
    if (e->pub.gpu_owned) return 0;
    if (e->pub.mirrored) return !e->uploaded;
    return !e->had && !(e->sfill && e->uploaded) && e->key.addr && !nvn_gpu_only(e->key.addr);
}

#define SAID_N 4096
typedef struct { uint64_t addr[SAID_N]; uint16_t n[SAID_N]; } SaidAt;
static int said_at(SaidAt *s, uint64_t addr, unsigned most) {
    uint32_t i = (uint32_t)(addr * 0x9E3779B97F4A7C15ull >> 52);
    for (int probe = 0; probe < 64; probe++, i = (i + 1) % SAID_N) {
        if (s->addr[i] && s->addr[i] != addr) continue;
        s->addr[i] = addr;
        return s->n[i] < most ? (s->n[i]++, 1) : 0;
    }
    return 1;
}

static int others_at(char *out, size_t n, const Tex *e, const Tex *skip) {
    int k = 0, any = 0;
    out[0] = 0;
    for (const Tex *p = nvn_hm_get(&by_addr, &e->key.addr); p && k < (int)n - 1; p = p->next_at) {
        if (p == e || p == skip) continue;
        k += snprintf(out + k, n - (size_t)k, "%s[%s%s%s%s: fmt %#x flags %#x %ux%u, %s, %s]", any++ ? " " : "",
                      p->released ? "released" : "live", p->superseded ? ", superseded" : "", p->retiring ? ", retiring" : "",
                      p->pub.gpu_owned ? ", GPU-written" : "", p->t.d.format, (unsigned)p->t.d.flags, p->t.d.width,
                      p->t.d.height, p->pub.mirrored ? (p->uploaded ? "CPU-written, uploaded" : "CPU-written, not uploaded")
                                                     : (no_content(p) ? "not CPU-written, no content" : "not CPU-written"),
                      start_names[p->og_start]);
    }
    uint64_t have = ndrops < DROPS_N ? ndrops : DROPS_N;
    int listed = 0;
    for (uint64_t i = 1; i <= have && listed < 4 && k < (int)n - 1; i++) {
        const DropRec *r = &drops[(ndrops - i) % DROPS_N];
        if (r->addr != e->key.addr) continue;
        k += snprintf(out + k, n - (size_t)k, "%s[dropped at frame %llu (%s): fmt %#x flags %#x %ux%u, %s%s]", any++ ? " " : "",
                      (unsigned long long)r->frame, drop_names[r->why], r->format, r->flags, r->w, r->h,
                      r->mirrored ? "CPU-written" : "not CPU-written", r->released ? ", released then" : ", live then");
        listed++;
    }
    if (!any) k = snprintf(out, n, "none");
    return k < (int)n ? k : (int)n - 1;
}

#define LOG_NEW_MAX 400
#define GONE_N 4096
static struct { uint64_t h, frame; void *ra; } gone[GONE_N];
static uint32_t gone_n;
static long long log_new_at = -2;
static int log_new_n;

static void log_new(const Tex *e, const TexKey *k) {
    if (log_new_at == -2) {
        const char *v = cfg_get("NX2SARIA_LOG_TEX_NEW");
        log_new_at = v && *v ? atoll(v) : -1;
    }
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (log_new_at < 0 || now < (uint64_t)log_new_at || log_new_n >= LOG_NEW_MAX) return;
    log_new_n++;
    int live = 0, rel = 0;
    for (const Tex *p = nvn_hm_get(&by_addr, &k->addr); p; p = p->next_at)
        if (p != e) p->released ? rel++ : live++;
    uint64_t h = key_hash(k);
    char was[160] = "never destroyed before";
    for (uint32_t i = 0; i < GONE_N && i < gone_n; i++) {
        uint32_t j = (gone_n - 1 - i) & (GONE_N - 1);
        if (gone[j].h == h) {
            snprintf(was, sizeof was, "the same key destroyed %llu frames ago from %p (destroy_tex at %p)",
                     (unsigned long long)(now - gone[j].frame), gone[j].ra, (void *)destroy_tex);
            break;
        }
    }
    rt_log("vk: new texture frame %llu: %#llx %ux%ux%u nvn fmt %#x target %d levels %u%s%s; at its address %d live, %d "
           "released; %s", (unsigned long long)now, (unsigned long long)k->addr, e->pub.width, e->pub.height,
           e->pub.depth, k->d.format, k->d.target, e->pub.levels, e->pub.mirrored ? ", mirrored" : "",
           e->alloc ? ", own memory" : "", live, rel, was);
}

__attribute__((noinline)) static void destroy_tex(Tex *e) {
    if (log_new_at >= 0) {
        uint32_t j = gone_n++ & (GONE_N - 1);
        gone[j].h = key_hash(&e->key), gone[j].frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        gone[j].ra = __builtin_return_address(0);
    }
    if (e->retiring) retire_remove(e);
    if (e->compacting) compact_forget(e);
    pool_unlist(e);
    pool_forget(e);
    own_remove(e);
    gwin_remove(e);
    rt_remove(e);
    n_site[0]++;
    tex_gen_bump(e->key.addr);
    nvn_hm_del(&by_key, &e->key);
    Tex *head = nvn_hm_get(&by_addr, &e->key.addr);
    if (head == e) {
        if (e->next_at) nvn_hm_put(&by_addr, &e->key.addr, e->next_at);
        else nvn_hm_del(&by_addr, &e->key.addr);
    } else {
        for (Tex *p = head; p; p = p->next_at)
            if (p->next_at == e) { p->next_at = e->next_at; break; }
    }
    if (e->released) nreleased--, rel_unlink(e);
    if (e->pub.image) vk_exec_image_gone(e->pub.image);
    vk_defer_image(e->pub.image, e->alloc);
    if (e->flat3d) {
        vk_exec_image_gone(e->flat3d->image);
        vk_defer_image(e->flat3d->image, e->flat3d_alloc);
        e->flat3d->gone = 1;
        vk_defer_free(e->flat3d);
    }
    free(e->snap);
    e->pub.gone = 1;
    vk_defer_free(e);
}

#define RELEASED_MAX 2048
#define RELEASED_MAX_TOP 8192
#define RELEASED_HARD 16384
#define SWEEP_LOOK 64
#define SWEEP_UNSAMPLED PRESENTS(120)
static int comes_back_intact(const Tex *e);

static uint64_t stale_after(const Tex *e);
static void sweep_hard(void);

static int released_max(void) {
    static int max;
    if (max) return max;
    const char *s = cfg_get("NX2SARIA_TEX_RELEASED_MAX");
    if (s && *s) {
        long long n = atoll(s);
        max = n < 1 ? 1 : n > RELEASED_HARD ? RELEASED_HARD : (int)n;
    } else {
        uint64_t vused = 0, vbudget = 0;
        int exact = 0;
        if (!vk_mem_vram(&vused, &vbudget, &exact) || !vbudget) return RELEASED_MAX;
        long long n = (long long)(vbudget >> 21);
        max = n < RELEASED_MAX ? RELEASED_MAX : n > RELEASED_MAX_TOP ? RELEASED_MAX_TOP : (int)n;
    }
    rt_log("vk: released textures: the sweep keeps up to %d entries (NX2SARIA_TEX_RELEASED_MAX)", max);
    return max;
}

static void sweep_released(void) {
    int lim = released_max();
    if (nreleased <= lim) return;
    {
        uint64_t now = frames_now();
        const uint64_t stale = PRESENTS(120);
        Tex *e = sweep_at ? sweep_at : rel_head;
        for (int n = 0; n < SWEEP_LOOK && nreleased > lim; n++) {
            if (!e || now - e->rel_frame < stale) { e = rel_head; break; }
            Tex *next = e->rel_next;
            if ((comes_back_intact(e) || e->overwritten) && now - e->rel_frame >= stale_after(e) &&
                !used_lately(e, now, stale))
                drop_tex(e, DROP_MAX);
            e = next;
        }
        sweep_at = e;
    }
    if (nreleased > RELEASED_HARD) sweep_hard();
}

#define HARD_SLACK 256
#define HARD_BATCH 1024
static uint64_t n_hard_by[TRL_HC_N];
#define HARD_LOG_NS 2000000ull
static uint64_t n_hard_calls, ns_hard, ns_hard_max;

static int hard_order(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_HARD_ORDER", "1") == 1;
    return on;
}

static int e_hard_class(const Tex *e, uint64_t now, uint64_t stale) {
    int intact = comes_back_intact(e);
    return trl_hard_class(e->overwritten, now - e->samp_frame < SWEEP_UNSAMPLED, intact,
                          intact && now - e->rel_frame >= stale_after(e) && !used_lately(e, now, stale));
}

static void hard_lru_drop(Tex *lru) {
    static volatile int32_t n_all_used;
    log_limited(&n_all_used, "all %d released textures were sampled or drawn into lately: the one longest ago goes "
                "(%#llx fmt %#x %ux%u, last in frame %llu)", nreleased, (unsigned long long)lru->key.addr,
                lru->t.d.format, lru->t.d.width, lru->t.d.height, (unsigned long long)lru->samp_frame);
    n_hard_by[TRL_HC_LATELY]++;
    drop_tex(lru, DROP_HARD);
}

static void sweep_hard(void) {
    static Tex **walk;
    static uint8_t *cls;
    static int *out, cap;
    const uint64_t stale = PRESENTS(120);
    const uint64_t t0 = plat_now_ns(), drops0 = n_drop_by[DROP_HARD], wb0 = n_drop_wb;
    uint64_t walked = 0;
    while (nreleased > RELEASED_HARD && rel_head) {
        uint64_t now = frames_now();
        Tex *lru = rel_head;
        if (nreleased > cap && hard_order()) {
            int want = nreleased + HARD_BATCH;
            Tex **w = realloc(walk, (size_t)want * sizeof *w);
            if (w) walk = w;
            uint8_t *c = w ? realloc(cls, (size_t)want) : NULL;
            if (c) cls = c;
            int *o = c ? realloc(out, (size_t)want * sizeof *o) : NULL;
            if (o) out = o, cap = want;
        }
        if (!hard_order() || nreleased > cap) {
            Tex *gone = NULL;
            for (Tex *e = rel_head; e && !gone; e = e->rel_next) {
                walked++;
                if (e->overwritten || now - e->samp_frame >= SWEEP_UNSAMPLED) gone = e;
                else if (e->samp_frame < lru->samp_frame) lru = e;
            }
            if (!gone) hard_lru_drop(lru);
            else n_hard_by[e_hard_class(gone, now, stale)]++, drop_tex(gone, DROP_HARD);
            continue;
        }
        int n = 0;
        for (Tex *e = rel_head; e && n < cap; e = e->rel_next) {
            walk[n] = e, cls[n] = (uint8_t)e_hard_class(e, now, stale), n++;
            if (e->samp_frame < lru->samp_frame) lru = e;
        }
        walked += (uint64_t)n;
        int need = nreleased - (RELEASED_HARD - HARD_SLACK);
        int k = trl_hard_pick(cls, n, need < HARD_BATCH ? need : HARD_BATCH, out);
        static Tex *wbv[HARD_BATCH];
        int m = 0;
        wb_cfg_init();
        for (int i = 0; i < k; i++) {
            Tex *e = walk[out[i]];
            uint64_t t;
            if (!e->gpu_dirty) continue;
            if (storage_rewritten(e, e->gpu_gen, &t) == 1) e->gpu_dirty = 0;
            else if (wb_wanted(e)) wbv[m++] = e;
        }
        if (m) write_back_n(wbv, m), n_drop_wb += (uint64_t)m;
        for (int i = 0; i < k; i++) n_hard_by[cls[out[i]]]++, drop_tex(walk[out[i]], DROP_HARD);
        if (!k) hard_lru_drop(lru);
    }
    uint64_t d = plat_now_ns() - t0;
    n_hard_calls++, ns_hard += d;
    if (d > ns_hard_max) ns_hard_max = d;
    if (d >= HARD_LOG_NS) {
        static volatile int32_t n_slow;
        log_limited(&n_slow, "released textures past the hard count: a batch walked %llu entries and dropped %llu "
                    "(%llu written back) in %.2f ms", (unsigned long long)walked,
                    (unsigned long long)(n_drop_by[DROP_HARD] - drops0), (unsigned long long)(n_drop_wb - wb0),
                    d / 1e6);
    }
}

#define VRAM_PRESSURE_DEFAULT 80
#define PRESSURE_STEP 32
#define EVICT_LOOK 1024
static int vram_pressure = -1;
static uint64_t n_pressure_drops;

static int comes_back_intact(const Tex *e) { return e->pub.mirrored && !e->pub.gpu_owned && !e->gpu_dirty; }

#define IDLE_ROUNDS 240
static uint32_t look_round = 1;
static uint32_t round_checks;
static uint32_t pool_slots;
static int pool_ctx;
static void touch(Tex *e) {
    e->use_round = look_round;
    e->use_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!pool_ctx) e->samp_frame = e->use_frame;
}

void vk_texture_pool_ctx(int on) { pool_ctx = on; }

static int idle(const Tex *e, uint64_t now);

#define COMMIT_LOW_KEEP ((uint64_t)256 << 20)
#define PROC_COMMIT_MIN_MB 24576
static uint64_t released_cap;
static long long commit_free_mb = -2;
static long long proc_commit_mb;
static int commit_low, commit_logs;
static int sys_low, proc_high;
static uint64_t n_cap_drops, n_commit_drops, n_commit_past;
static double commit_free_now = -1, commit_low_mark;

static uint64_t released_cap_bytes(void) {
    if (!released_cap) {
        const char *s = cfg_get("NX2SARIA_TEX_RELEASED_MB");
        long long mb;
        const char *by = "";
        if (s && *s) mb = atoll(s);
        else {
            uint64_t vused = 0, vbudget = 0;
            int exact = 0;
            int known = vk_mem_vram(&vused, &vbudget, &exact) && vbudget;
            mb = trl_cap_mb(known ? vbudget >> 20 : 0);
            by = !known ? ", the default without the card's budget"
               : mb == TRL_CAP_MAX_MB ? ", the most the rule by the card gives"
               : mb == TRL_CAP_MIN_MB ? ", the least the rule by the card gives" : ", a thirty-second of the video memory budget";
        }
        released_cap = mb > 0 ? (uint64_t)mb << 20 : UINT64_MAX;
        if (mb > 0) rt_log("vk: released textures keep at most %lld MiB of their own memory%s (NX2SARIA_TEX_RELEASED_MB)", mb,
                           by);
        else rt_log("vk: released textures: no size limit");
    }
    return released_cap;
}

static int commit_is_low(void) {
    if (commit_free_mb == -2) {
        const char *s = cfg_get("NX2SARIA_TEX_COMMIT_FREE_MB");
        commit_free_mb = s && *s ? atoll(s) : -1;
        if (commit_free_mb < -1) commit_free_mb = -1;
        s = cfg_get("NX2SARIA_TEX_PROC_COMMIT_MB");
        proc_commit_mb = s && *s ? atoll(s) : -1;
        if (proc_commit_mb < 0) {
            float ram_used, ram_total, ram_proc;
            plat_mem_info(&ram_used, &ram_total, &ram_proc);
            proc_commit_mb = ram_total > PROC_COMMIT_MIN_MB ? (long long)ram_total : PROC_COMMIT_MIN_MB;
        }
        if (!plat_commit_enforced()) commit_free_mb = proc_commit_mb = 0;
        char note[2048];
        if (plat_commit_note(note, sizeof note)) rt_log("platform: %s", note);
        if (proc_commit_mb)
            rt_log("vk: this process's commit%s is logged when it passes %lld MB (NX2SARIA_TEX_PROC_COMMIT_MB; 0 = off); "
                   "textures give way only when the system's commit is low",
                   plat_commit_counts_vram() ? ", less the video memory in use on a discrete card," : "", proc_commit_mb);
    }
    commit_free_now = -1;
    if (!commit_free_mb && !proc_commit_mb) return 0;
    double proc, used, limit;
    plat_commit_info(&proc, &used, &limit);
    int low = 0;
    commit_critical = 0;
    if (commit_free_mb && used >= 0 && limit > 0) {
        double want = commit_free_mb > 0 ? (double)commit_free_mb : limit / 10 > 4096 ? limit / 10 : 4096;
        low = limit - used < want;
        commit_free_now = limit - used, commit_low_mark = want;
        commit_critical = limit - used < want / 2;
    }
    double dev = 0;
    VkCtx *c = vk_ctx();
    uint64_t dev_used, dev_budget;
    int exact;
    if (proc_commit_mb && plat_commit_counts_vram() && c &&
        c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && vk_mem_vram(&dev_used, &dev_budget, &exact))
        dev = dev_used / 1048576.0 < proc ? dev_used / 1048576.0 : proc;
    int high = proc_commit_mb &&
               proc - dev > (double)(proc_high ? proc_commit_mb - proc_commit_mb / 32 : proc_commit_mb);
    if (high != proc_high && commit_logs < 40) {
        commit_logs++;
        rt_log("vk: this process's commit %s: %.0f MB less %.0f MB of video memory, the bound is %lld MB (system: %.0f "
               "of %.0f MB in use); released textures kept %d, %.0f MiB of own memory%s", high ? "high" : "no longer high",
               proc, dev, proc_commit_mb, used, limit, nreleased, rel_bytes / 1048576.0,
               high ? ": the host's own memory, which dropping textures does not lower; textures give way only when the "
                      "system's commit is low" : "");
    }
    proc_high = high;
    if (low != sys_low && commit_logs < 40) {
        commit_logs++;
        rt_log("vk: system commit %s: %.0f of %.0f MB in use (this process %.0f MB); released textures kept %d, %.0f MiB "
               "of own memory%s", low ? "low" : "no longer low", used, limit, proc, nreleased, rel_bytes / 1048576.0,
               low ? ": dropping the stale ones" : "");
        static int hinted;
        if (low && !hinted && plat_page_file_managed() == 1) {
            hinted = 1;
            rt_log("vk: the page file is sized by Windows, which grows it only slowly: a fixed page file whose initial "
                   "size is at least 32 GB keeps the system's commit from running low (System > About > Advanced system "
                   "settings > Performance Settings > Advanced > Virtual memory > Change: untick the automatic "
                   "management, Custom size with an initial size of 32768 MB or more, Set, then restart Windows)");
        }
    }
    sys_low = low;
    return commit_low = low;
}

int vk_texture_commit_low(void) { return __atomic_load_n(&commit_low, __ATOMIC_RELAXED); }

#define STALE_FRAMES PRESENTS(120)
#define GPU_STALE_FRAMES PRESENTS(1800)
#define GAP_TIMES 8
static int idle(const Tex *e, uint64_t now) {
    return look_round - e->use_round >= IDLE_ROUNDS && now - e->use_frame >= STALE_FRAMES &&
           now - e->samp_frame >= STALE_FRAMES;
}

static int keep_taken(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_KEEP_TAKEN", "1") == 1;
    return on;
}

static uint64_t stale_after(const Tex *e) {
    uint64_t gap = (uint64_t)e->back_gap * GAP_TIMES;
    if (e->overwritten) return STALE_FRAMES;
    if (comes_back_intact(e)) return keep_taken() && gap > STALE_FRAMES ? gap : STALE_FRAMES;
    return gap > GPU_STALE_FRAMES ? gap : GPU_STALE_FRAMES;
}

static int drop_stale(uint64_t keep, uint64_t *count, int why) {
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int dropped = 0;
    const uint64_t stale = STALE_FRAMES;
    for (int any = 0; any < 2; any++)
        for (Tex *e = rel_head, *next; e && now - e->rel_frame >= stale; e = next) {
            if (dropped == PRESSURE_STEP || rel_bytes <= keep) return dropped;
            next = e->rel_next;
            if (!e->bytes || (!any && !comes_back_intact(e) && !e->overwritten)) continue;
            if (now - e->rel_frame < stale_after(e) || (!e->overwritten && !idle(e, now))) continue;
            drop_tex(e, why);
            dropped++, (*count)++;
        }
    return dropped;
}

static int used_lately(const Tex *e, uint64_t now, uint64_t within) {
    return now - e->use_frame < within || now - e->samp_frame < within;
}

static int sampled_lately(const Tex *e, uint64_t now, uint64_t within) { return now - e->samp_frame < within; }

#define OVERWRITTEN_FULL (REUSE_UNUSED_FULL * 4)
static uint64_t n_over_full;
static void drop_overwritten(int full) {
    static int used_too = -1;
    static uint64_t n_kept;
    if (used_too < 0) used_too = !cfg_bool("NX2SARIA_TEX_OVERWRITTEN_USED");
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int dropped = 0, marks = marks_tell();
    const uint64_t keep = full ? OVERWRITTEN_FULL : OVERWRITTEN_KEEP;
    for (Tex *e = rel_head, *next; e && now - e->rel_frame >= keep && dropped < PRESSURE_STEP; e = next) {
        next = e->rel_next;
        if (!e->overwritten || now - e->ow_frame < keep) continue;
        if (!used_too && (marks ? sampled_lately(e, now, keep) : used_lately(e, now, keep))) {
            if (e->kept_frame != now + 1 && (!n_kept++ || !(n_kept & (n_kept - 1))))
                rt_log("vk: an overwritten texture stays, it is still used: %#llx fmt %#x %ux%u, last used %llu presents "
                       "ago (%llu so far; NX2SARIA_TEX_OVERWRITTEN_USED=0 drops such a texture)",
                       (unsigned long long)e->key.addr, e->t.d.format, e->t.d.width, e->t.d.height,
                       (unsigned long long)(now - (marks || e->samp_frame > e->use_frame ? e->samp_frame
                                                                                                 : e->use_frame)),
                       (unsigned long long)n_kept);
            e->kept_frame = now + 1;
            e->ow_frame = now;
            continue;
        }
        drop_tex(e, DROP_OVERWRITTEN);
        dropped++, n_over_drops++, n_over_full += full != 0;
    }
}

#define AGED_LOOK 256
static int rel_hold = -1;
static uint64_t n_age_drops, n_age_bytes;

static uint32_t released_hold(void) {
    if (rel_hold < 0) {
        const char *s = cfg_get("NX2SARIA_TEX_RELEASED_HOLD");
        long long v = s && *s ? atoll(s) : TRL_HOLD_S;
        rel_hold = v < 0 ? 0 : v > TRL_HOLD_MAX_S ? TRL_HOLD_MAX_S : (int)v;
        if (rel_hold)
            rt_log("vk: released textures go once unused for %d s, %d s for content only the GPU made "
                   "(NX2SARIA_TEX_RELEASED_HOLD); young ones past the size bound stay while the card and the system "
                   "have room, up to %d times the bound", rel_hold, (int)trl_hold((uint32_t)rel_hold, TRL_GPU),
                   TRL_ROOM_TIMES);
        else
            rt_log("vk: released textures stay until the size bound or a pressure takes them "
                   "(NX2SARIA_TEX_RELEASED_HOLD=0)");
    }
    return (uint32_t)rel_hold;
}

static void drop_aged(void) {
    uint32_t hold = released_hold();
    if (!hold) return;
    uint64_t now = frames_now();
    Tex *e = aged_at ? aged_at : rel_head;
    int dropped = 0;
    for (int n = 0; n < AGED_LOOK && dropped < PRESSURE_STEP; n++) {
        if (!e || secs_since(e->rel_frame) < hold) {
            e = NULL;
            break;
        }
        Tex *next = e->rel_next;
        int cls = comes_back_intact(e) || e->overwritten ? TRL_STORAGE : TRL_GPU;
        uint64_t last = e->samp_frame > e->rel_frame ? e->samp_frame : e->rel_frame;
        if (e->bytes && !e->gpu_dirty && trl_aged(hold, cls, secs_since(last)) &&
            now - e->rel_frame >= stale_after(e) && idle(e, now)) {
            n_age_drops++, n_age_bytes += e->bytes, dropped++;
            drop_tex(e, DROP_AGE);
        }
        e = next;
    }
    aged_at = e;
}

#define EVICT_STEP 64
#define EVICT_EMPTY_WAIT 16
#define ROOM_CHECKS 64
static int tex_evict = -1;
static uint64_t evict_wait;
static uint64_t idle_empty_wait;
static uint64_t n_evict, n_evict_bytes;
static uint64_t n_evict_empty, ns_evict_empty;
static int evict_logs;
static volatile int64_t room_wanted;
static uint64_t room_level;
static uint32_t room_checks;

void vk_texture_want_room(uint64_t bytes) {
    int64_t cur = __atomic_load_n(&room_wanted, __ATOMIC_RELAXED);
    while ((int64_t)bytes > cur && !__atomic_compare_exchange_n(&room_wanted, &cur, (int64_t)bytes, 0, __ATOMIC_RELAXED,
                                                                 __ATOMIC_RELAXED)) {}
}

static uint64_t evict_idle(uint64_t want, uint64_t used, uint64_t budget, int why) {
    if (vk_sync_fence_done() < evict_wait || vk_sync_issued() < idle_empty_wait) return 0;
    uint64_t t0 = kern_now_ns();
    Tex *v[EVICT_STEP];
    int nv = 0;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (!e || e->released || e->retiring || !e->bytes || !comes_back_intact(e) || !idle(e, now)) continue;
        uint32_t age = look_round - e->use_round;
        int at = nv;
        while (at > 0 && look_round - v[at - 1]->use_round < age) at--;
        if (at == EVICT_STEP) continue;
        if (nv < EVICT_STEP) nv++;
        memmove(v + at + 1, v + at, sizeof *v * (size_t)(nv - 1 - at));
        v[at] = e;
    }
    if (!nv) {
        idle_empty_wait = vk_sync_issued() + EVICT_EMPTY_WAIT;
        n_evict_empty++, ns_evict_empty += kern_now_ns() - t0;
        return 0;
    }
    uint64_t freed = 0;
    int n = 0;
    for (; n < nv && freed < want; n++) freed += v[n]->bytes, drop_tex(v[n], why);
    if (!n) return 0;
    n_evict += (uint64_t)n, n_evict_bytes += freed;
    evict_wait = vk_sync_issued() + 2;
    if (evict_logs < 8 || ((rt_trace & RT_TRACE_PERF) && evict_logs < 64)) {
        evict_logs++;
        rt_log("vk: VRAM %.0f of %.0f MiB in use: %d idle textures evicted (%.1f MiB), least recently used first; they "
               "are uploaded again when the game uses them", used / 1048576.0, budget / 1048576.0, n, freed / 1048576.0);
    }
    return freed;
}

#define DISUSE_SECONDS 120
#define DISUSE_FRAMES PRESENTS(DISUSE_SECONDS * 60)
#define HOLDS_MAX 65536
static nvn_hmap holds = {.klen = sizeof(TexKey)};
static uint32_t marks_reads;
static int marks_tell(void) {
    return vk_pool_marks_enabled() && !__atomic_load_n(&vk_pool_marks_missed, __ATOMIC_RELAXED) && marks_reads >= 2;
}
static uint64_t marks_peak;
static uint32_t marks_live;
static int resolve_held;
static uint64_t n_disuse, n_disuse_bytes, n_disuse_back;
static uint64_t n_disuse_shared, n_disuse_diverted;
static uint64_t n_retire_cancel, n_retire_freed;
static uint64_t n_retire_stalled;
#define RETIRE_PATIENCE PRESENTS(5 * 60)
static int disuse_logs;
static Tex **retiring;
static int nretiring, cretiring;
static uint64_t retiring_bytes;

static void retire_cancel(Tex *e) {
    e->retiring = 0;
    retiring_bytes -= e->bytes;
    if (holds.n) nvn_hm_del(&holds, &e->key);
    n_retire_cancel++, n_disuse_back++;
    e->use_round = look_round;
    e->use_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    tex_gen_bump(e->key.addr);
}

static void retire_remove(Tex *e) {
    for (int i = 0; i < nretiring; i++)
        if (retiring[i] == e) { retiring[i] = retiring[--nretiring]; break; }
    if (e->retiring) e->retiring = 0, retiring_bytes -= e->bytes;
}

static void retire_sweep(void) {
    if (!nretiring) return;
    static int keep = -1;
    if (keep < 0) keep = rt_knob("TEX_DISUSE", "1") == 2;
    uint64_t takes = vk_pool_takes(), done = vk_sync_fence_done();
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int k = 0;
    for (int i = 0; i < nretiring; i++) {
        Tex *e = retiring[i];
        if (!e->retiring) continue;
        if (!e->retire_due && takes - e->retire_takes > VK_POOL_SETS) e->retire_due = vk_sync_issued();
        if (!e->retire_due && now - e->retire_frame > RETIRE_PATIENCE) {
            retire_cancel(e);
            e->samp_frame = now;
            n_retire_stalled++;
            continue;
        }
        if (!keep && e->retire_due && done >= e->retire_due) {
            e->retiring = 0, retiring_bytes -= e->bytes;
            nvn_hm_put(&holds, &e->key, (void *)(uintptr_t)1);
            n_retire_freed++;
            drop_tex(e, DROP_DISUSE);
            continue;
        }
        retiring[k++] = e;
    }
    nretiring = k;
    static uint64_t stalled_logged;
    static int stalled_logs;
    if (n_retire_stalled != stalled_logged && stalled_logs < 8) {
        stalled_logs++;
        rt_log("vk: %llu textures evicted as unsampled were taken back: the texture pool had not been brought up to "
               "date completely in %d s, so they could not be destroyed and their slots showed stand-ins",
               (unsigned long long)(n_retire_stalled - stalled_logged), 5);
        stalled_logged = n_retire_stalled;
    }
}

static int held_out(const TexKey *k) {
    if (!holds.n || !nvn_hm_get(&holds, k)) return 0;
    if (pool_ctx) return 1;
    nvn_hm_del(&holds, k);
    n_disuse_back++;
    return 0;
}

static int slot_names_other(int32_t id, const Tex *e) {
    const TexSlot *s = &tslots[id];
    if (!s->used || s->finalized || (uintptr_t)s->t.mem != e->key.addr) return 0;
    TexKey sk;
    make_key(&s->t, &sk);
    return memcmp(&sk, &e->key, sizeof sk) != 0;
}

static int shared_address(const Tex *e) {
    if (!sinfo || !slot_next) return 0;
    uint32_t b = agen_idx(e->key.addr);
    for (int32_t id = bucket_head[b]; id >= 0; id = slot_next[id])
        if (slot_names_other(id, e)) return 1;
    for (int32_t g = bucket_ghead[b]; g >= 0; g = grp[g].gnext)
        for (int32_t id = grp[g].pub.head; id >= 0; id = slot_next[id])
            if (slot_names_other(id, e)) return 1;
    return 0;
}

static int disuse_evict_on(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("TEX_DISUSE_EVICT", "0") == 1;
        rt_log("vk: textures no shader sampled for a while %s under pressure (NX2SARIA_TEX_DISUSE_EVICT)",
               on ? "are retired" : "stay");
    }
    return on;
}

static uint64_t evict_disused(uint64_t want) {
    if (!disuse_evict_on()) return 0;
    if (!want || !vk_pool_marks_enabled() || __atomic_load_n(&vk_pool_marks_missed, __ATOMIC_RELAXED) || marks_reads < 2 ||
        marks_live < 2 || vk_sync_fence_done() < evict_wait)
        return 0;
    if (retiring_bytes >= want) return retiring_bytes;
    want -= retiring_bytes;
    Tex *v[EVICT_STEP];
    int nv = 0;
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), disuse = DISUSE_FRAMES;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (!e || e->released || e->retiring || !e->bytes || !comes_back_intact(e) || now - e->samp_frame < disuse)
            continue;
        int at = nv;
        while (at > 0 && v[at - 1]->samp_frame > e->samp_frame) at--;
        if (at == EVICT_STEP) continue;
        if (nv < EVICT_STEP) nv++;
        memmove(v + at + 1, v + at, sizeof *v * (size_t)(nv - 1 - at));
        v[at] = e;
    }
    int kept = 0;
    for (int i = 0; i < nv; i++) {
        if (shared_address(v[i])) v[i]->samp_frame = now, n_disuse_shared++;
        else v[kept++] = v[i];
    }
    nv = kept;
    if (!nv) return 0;
    if (holds.n + (uint32_t)nv > HOLDS_MAX) {
        nvn_hm_clear(&holds, NULL);
        for (int id = 0; id < ntslots; id++) {
            if (!tslots[id].used || !tslots[id].cheld) continue;
            TexKey k;
            make_key(&tslots[id].t, &k);
            nvn_hm_put(&holds, &k, (void *)(uintptr_t)1);
        }
        rt_log("vk: holds of textures evicted by disuse past %d: kept the %u that registered slots still name", HOLDS_MAX,
               holds.n);
    }
    uint64_t freed = 0;
    int n = 0;
    uint64_t takes = vk_pool_takes();
    for (; n < nv && freed < want; n++) {
        Tex *e = v[n];
        if (nretiring == cretiring) {
            int c = cretiring ? cretiring * 2 : 256;
            Tex **r = realloc(retiring, sizeof *r * (size_t)c);
            if (!r) break;
            retiring = r, cretiring = c;
        }
        retiring[nretiring++] = e;
        e->retiring = 1, e->retire_takes = takes, e->retire_due = 0, e->retire_frame = now;
        pool_unlist(e);
        retiring_bytes += e->bytes;
        freed += e->bytes;
        tex_gen_bump(e->key.addr);
    }
    n_disuse += (uint64_t)n, n_disuse_bytes += freed;
    evict_wait = vk_sync_issued() + 2;
    if (disuse_logs < 8 || ((rt_trace & RT_TRACE_PERF) && disuse_logs < 64)) {
        disuse_logs++;
        rt_log("vk: %d textures no shader sampled for %d s or more evicted (%.1f MiB), least recently sampled first; their "
               "images go once the texture pool sets no longer name them, and one sampled again is made again within a "
               "few frames", n, DISUSE_SECONDS, freed / 1048576.0);
    }
    return retiring_bytes;
}

static void slot_note(int32_t id, const Tex *e, int *n, int *n_own, int *n_show, int *n_other, char *own, char *show,
                      char *other, size_t own_cap, size_t other_cap) {
    const TexSlot *s = &tslots[id];
    if (!s->used || (uintptr_t)s->t.mem != e->key.addr) return;
    ++*n;
    TexKey sk;
    make_key(&s->t, &sk);
    size_t at;
    if (!memcmp(&sk, &e->key, sizeof sk)) {
        if (++*n_own <= 8 && (at = strlen(own)) < own_cap - 1)
            snprintf(own + at, own_cap - at, " %d%s", (int)id, s->finalized ? "f" : "");
    } else if (++*n_other <= 8 && (at = strlen(other)) < other_cap - 1) {
        snprintf(other + at, other_cap - at, " %d (fmt %#x flags %#x%s)", (int)id, s->t.d.format, (unsigned)s->t.d.flags,
                 s->finalized ? ", finalized" : "");
    }
    if (s->ce == e && s->cgen == agen[sinfo[id] & 0xffff] && ++*n_show <= 8 && (at = strlen(show)) < own_cap - 1)
        snprintf(show + at, own_cap - at, " %d", (int)id);
}

static void slots_text(char *out, size_t n, const Tex *e) {
    char own[112] = "", show[112] = "", other[360] = "";
    int total = 0, n_own = 0, n_show = 0, n_other = 0;
    uint32_t b = agen_idx(e->key.addr);
    for (int32_t id = bucket_head[b]; id >= 0; id = slot_next[id])
        slot_note(id, e, &total, &n_own, &n_show, &n_other, own, show, other, sizeof own, sizeof other);
    for (int32_t g = bucket_ghead[b]; g >= 0; g = grp[g].gnext)
        for (int32_t id = grp[g].pub.head; id >= 0; id = slot_next[id])
            slot_note(id, e, &total, &n_own, &n_show, &n_other, own, show, other, sizeof own, sizeof other);
    snprintf(out, n, "%d registered; %d of this descriptor:%s; %d showing this entry:%s; %d of other descriptors:%s", total,
             n_own, n_own ? own : " none", n_show, n_show ? show : " none", n_other, n_other ? other : " none");
}

static uint64_t n_mark_empty, n_mark_none;
#define MARK_LINES 6
#define MARK_LINES_ALL 20000
static void marks_check(Tex *e, uint32_t id, uint64_t now, int followed) {
    if (!no_content(e)) return;
    if (e->pub.mirrored && now - e->og_frame < 4) return;
    n_mark_empty++;
    if (e->said) return;
    e->said = 1;
    static SaidAt said;
    static uint32_t total;
    if (total >= MARK_LINES_ALL || !said_at(&said, e->key.addr, MARK_LINES)) return;
    total++;
    char d[224], og[320], state[160], slots[640], others[640];
    desc_text(d, sizeof d, e);
    origin_text(og, sizeof og, e);
    slots_text(slots, sizeof slots, e);
    others_at(others, sizeof others, e, NULL);
    if (e->pub.mirrored)
        snprintf(state, sizeof state, "CPU-written, not uploaded%s",
                 e->up_retry ? " (an upload that had no memory waits for its next try)" : "");
    else
        snprintf(state, sizeof state, "not CPU-written (%s), never drawn into, copied into or filled", mirror_refused(&e->t));
    rt_log("perf: frame %llu: sampled without content: texture %#llx (%llu bytes) [%s]: %s; %s; marked slot %u%s; slots "
           "at the address: %s; other descriptors there: %s", (unsigned long long)now, (unsigned long long)e->key.addr,
           (unsigned long long)e->t.L.size, d, state, og, id, followed ? " (of another descriptor, credited to this entry)" : "",
           slots, others);
}

void vk_pool_marks_read(const uint32_t *marks, uint32_t n) {
    lock();
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int trace = (rt_trace & RT_TRACE_PERF) != 0;
    int over = n == VK_POOL_TEX && marks[VK_POOL_MARK_OVERFLOW] != 0;
    if (over) {
        static uint64_t overflows;
        if (!(overflows++ % 1000))
            rt_log("vk: texture pool marks: a shader noted more slots than its ring holds; this read counts every "
                   "slot as sampled (%llu such reads)", (unsigned long long)overflows);
    }
    if (n > (uint32_t)ntslots) n = (uint32_t)ntslots;
    uint32_t marked = 0;
    for (uint32_t id = 0; id < n; id++) {
        if (!marks[id] && !(over && tslots[id].used)) continue;
        if (!tslots[id].used) {
            if (flight_on) flight_tex(FLIGHT_TEX_UNREG, 0, 0, 0, 0, 0, 0, 0, id);
            continue;
        }
        marked++;
        TexSlot *s = &tslots[id];
        if (s->ce && s->cgen == agen[sinfo[id] & 0xffff]) {
            if (s->ce->retiring) retire_cancel(s->ce);
            s->ce->samp_frame = now;
            if (trace) marks_check(s->ce, id, now, 0);
            continue;
        }
        if (trace && !s->ce && s->cgen == agen[sinfo[id] & 0xffff]) n_mark_none++;
        TexKey k;
        make_key(&s->t, &k);
        Tex *e = nvn_hm_get(&by_key, &k);
        if (e && !e->released) {
            if (e->retiring) retire_cancel(e);
            e->samp_frame = now;
            if (trace) marks_check(e, id, now, 0);
            continue;
        }
        if (e) e->samp_frame = now;
        int followed = 0;
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr); p; p = p->next_at) {
            if (p->released || p->key.d.target != k.d.target) continue;
            if (p->retiring) retire_cancel(p);
            p->samp_frame = now;
            followed = 1;
            if (trace) marks_check(p, id, now, 1);
        }
        if (flight_on && !followed)
            flight_tex(FLIGHT_TEX_BLANK, k.addr, (uint32_t)s->t.d.format, s->t.d.width, s->t.d.height, 0,
                       tex_mirrored(&s->t) ? FLIGHT_TF_MIRRORED : 0,
                       (s->finalized ? 1u : 0) | (holds.n && nvn_hm_get(&holds, &k) ? 2u : 0), id);
        if (holds.n && nvn_hm_del(&holds, &k)) {
            n_disuse_back++;
            n_disuse_diverted += followed != 0;
            s->cheld = 0;
            s->cwant = 1;
            tex_gen_bump(k.addr);
        }
    }
    marks_reads++;
    marks_peak -= marks_peak >> 14;
    if ((uint64_t)marked << 8 > marks_peak) marks_peak = (uint64_t)marked << 8;
    if (marked >= 16 && ((uint64_t)marked << 10) >= marks_peak) marks_live += marks_live < 1000;
    else marks_live = 0;
    unlock();
}

#define COMMIT_EVICT_STEP ((uint64_t)256 << 20)
static int commit_evict_on = -1;
static void commit_evict(void) {
    if (commit_evict_on < 0) commit_evict_on = rt_knob("TEX_COMMIT_EVICT", "1") == 1;
    if (!commit_evict_on || !tex_evict) return;
    if (rel_bytes > COMMIT_LOW_KEEP) {
        static int logs;
        if (!n_commit_past++ && logs < 4) {
            logs++;
            rt_log("vk: commit low: the %d released textures keep %.0f MiB the game still takes back or looks up; "
                   "live textures nothing sampled go without waiting for them", nreleased, rel_bytes / 1048576.0);
        }
    }
    uint64_t used = 0, budget = 0;
    vk_mem_vram_live(&used, &budget);
    uint64_t freed = evict_idle(COMMIT_EVICT_STEP, used, budget, DROP_IDLE_COMMIT);
    if (freed < COMMIT_EVICT_STEP) evict_disused(COMMIT_EVICT_STEP - freed);
}

#define REL_CENSUS_SUBS 3600
#define REL_CENSUS_LINES 24
#define REL_CENSUS_ROOM_NS 60000000000ull
#define REL_CENSUS_ROOM_LINES 60
static void rel_census(uint64_t cap, int room) {
    static uint32_t last, lines, room_lines;
    static uint64_t room_at;
    if (room) {
        uint64_t t = plat_now_ns();
        if (room_lines >= REL_CENSUS_ROOM_LINES || (room_at && t - room_at < REL_CENSUS_ROOM_NS)) return;
        room_at = t, room_lines++;
    } else {
        if (lines >= REL_CENSUS_LINES || (last && submit_epoch - last < REL_CENSUS_SUBS)) return;
        last = submit_epoch ? submit_epoch : 1, lines++;
    }
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    enum { YOUNG, SLOT, SLOT_OVER, SAMPLED, LOOKED, NEXT, KINDS };
    uint64_t by[KINDS] = {0}, gpu = 0;
    int n[KINDS] = {0}, ngpu = 0;
    enum { Y_NEW, Y_GPU, Y_GAP, Y_REACHED, YKINDS };
    uint64_t yby[YKINDS] = {0}, gap_max = 0;
    int yn[YKINDS] = {0};
    for (const Tex *e = rel_head; e; e = e->rel_next) {
        if (!e->bytes) continue;
        int shown = e->ph && pref[e->ph - 1].refs;
        int k = now - e->rel_frame < stale_after(e) ? YOUNG
              : now - e->samp_frame < STALE_FRAMES ? SAMPLED
              : shown ? (e->overwritten ? SLOT_OVER : SLOT)
              : !e->overwritten && !idle(e, now) ? LOOKED : NEXT;
        by[k] += e->bytes, n[k]++;
        if (!comes_back_intact(e)) gpu += e->bytes, ngpu++;
        if (k == YOUNG) {
            int y = now - e->rel_frame < STALE_FRAMES ? Y_NEW
                  : !comes_back_intact(e) && !e->overwritten ? Y_GPU : idle(e, now) ? Y_GAP : Y_REACHED;
            yby[y] += e->bytes, yn[y]++;
            if (y >= Y_GAP && stale_after(e) > gap_max) gap_max = stale_after(e);
        }
    }
    const double M = 1048576.0;
    rt_log("vk: released textures over their bound%s: %d entries, %.0f MiB of %.0f MiB (%d of them, %.0f MiB, hold content "
           "only the GPU made): %d, %.0f MiB, released too lately to go; %d, %.0f MiB, sampled or drawn into lately; %d, "
           "%.0f MiB, shown by a texture pool slot and not sampled, and %d more, %.0f MiB, that another target was drawn "
           "over; %d, %.0f MiB, reached by another lookup; %d, %.0f MiB, next to go",
           room ? " (kept past it while the card has room)" : "", nreleased, rel_bytes / M, cap / M,
           ngpu, gpu / M, n[YOUNG], by[YOUNG] / M, n[SAMPLED], by[SAMPLED] / M, n[SLOT], by[SLOT] / M, n[SLOT_OVER],
           by[SLOT_OVER] / M, n[LOOKED], by[LOOKED] / M, n[NEXT], by[NEXT] / M);
    if (n[YOUNG])
        rt_log("vk: the ones released too lately wait: %d, %.0f MiB, the first %llu presents after a release (2 s at the "
               "%u presents a second the waits count); %d, %.0f MiB, with content only the GPU made, %llu presents or their own "
               "gap; %d, %.0f MiB, intact, for 8 times their longest gap before a take-back (%llu presents at most) with "
               "nothing reaching them: the VRAM pressure takes these; %d, %.0f MiB, intact, for their gap and still "
               "reached",
               yn[Y_NEW], yby[Y_NEW] / M, (unsigned long long)STALE_FRAMES, tex_rate * 60 >> 8, yn[Y_GPU], yby[Y_GPU] / M,
               (unsigned long long)GPU_STALE_FRAMES, yn[Y_GAP], yby[Y_GAP] / M, (unsigned long long)gap_max, yn[Y_REACHED],
               yby[Y_REACHED] / M);
}

#define PRESSURE_HARD_STEP (PRESSURE_STEP * 8)
#define PRESSURE_HARD_PCT 95
#define HARD_UNSAMPLED PRESENTS(30)

static void sweep_pressure(void) {
    if (vram_pressure < 0) {
        const char *s = cfg_get("NX2SARIA_TEX_VRAM_PRESSURE");
        vram_pressure = s && *s ? atoi(s) : VRAM_PRESSURE_DEFAULT;
        if (vram_pressure < 0 || vram_pressure > 100) vram_pressure = VRAM_PRESSURE_DEFAULT;
        tex_evict = rt_knob("TEX_EVICT", "1");
        rt_log("vk: textures under VRAM pressure: %s%d%%%s", vram_pressure ? "dropped above " : "off", vram_pressure,
               !vram_pressure ? "" : tex_evict ? ", released ones first, then idle live ones (NX2SARIA_TEX_EVICT=0: released only)"
                                               : ", released ones only (NX2SARIA_TEX_EVICT=0)");
        if (vram_pressure > 87)
            rt_log("vk: NX2SARIA_TEX_VRAM_PRESSURE=%d is above the seven eighths of the budget past which a new VRAM "
                   "mirror is refused: textures make room only once a mirror was refused", vram_pressure);
    }
    retire_sweep();
    if (submit_epoch & 7) return;
    uint64_t used, budget;
    int exact, dropped = 0;
    uint64_t wanted = tex_evict ? (uint64_t)plat_xchg64(&room_wanted, 0) : 0;
    int known = vram_pressure && (rel_head || tex_evict || wanted) &&
                (tex_evict ? vk_mem_vram_live(&used, &budget) : vk_mem_vram(&used, &budget, &exact));
    uint64_t over = 0;
    if (known) {
        uint64_t limit = budget / 100 * (uint64_t)vram_pressure;
        if (used > limit) over = used - limit + budget / 50;
        if (wanted) {
            uint64_t level = used > wanted ? used - wanted : 0;
            if (!room_checks || level < room_level) room_level = level;
            room_checks = ROOM_CHECKS;
        }
        if (room_checks && used <= room_level) room_checks = 0;
        if (room_checks) {
            room_checks--;
            if (used - room_level > over) over = used - room_level;
        }
    }
    if (over) {
        uint64_t freed = 0, now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        int hard = known && used > budget / 100 * PRESSURE_HARD_PCT && vk_pool_marks_enabled() &&
                   !__atomic_load_n(&vk_pool_marks_missed, __ATOMIC_RELAXED);
        static int hard_told;
        if (hard && !hard_told++)
            rt_log("vk: at %d%% of the video memory budget: released textures no shader sampled for %llu presents go, "
                   "however recently released, lookups or not", PRESSURE_HARD_PCT, (unsigned long long)HARD_UNSAMPLED);
        Tex *e = press_at ? press_at : rel_head;
        for (int n = 0; n < EVICT_LOOK && dropped < (hard ? PRESSURE_HARD_STEP : PRESSURE_STEP); n++) {
            if (!e || (!hard && now - e->rel_frame < STALE_FRAMES)) {
                e = NULL;
                break;
            }
            Tex *next = e->rel_next;
            if (comes_back_intact(e) && (hard ? now - e->samp_frame >= HARD_UNSAMPLED : idle(e, now))) {
                freed += e->bytes;
                drop_tex(e, DROP_VRAM);
                dropped++, n_pressure_drops++;
            }
            e = next;
            if (hard && freed >= over) break;
        }
        press_at = e;
        if (tex_evict && freed < over) freed += evict_idle(over - freed, used, budget, DROP_IDLE_VRAM);
        if (tex_evict && freed < over) evict_disused(over - freed);
    }
    int low = commit_is_low(), more = 0;
    int held = low && commit_drops_held();
    if (rel_head) {
        uint64_t cap = released_cap_bytes(), keep = cap, vused = 0, vbudget = 0;
        int vexact, vknown = vk_mem_vram(&vused, &vbudget, &vexact);
        over_share = vknown && vram_pressure && vused > vbudget / 100 * (uint64_t)vram_pressure;
        drop_overwritten(over_share);
        drop_aged();
        if (rel_bytes > cap && released_hold() && vknown)
            keep = trl_keep(released_hold(), cap,
                            trl_room(vused, vbudget, vram_pressure ? vram_pressure : VRAM_PRESSURE_DEFAULT,
                                     commit_free_now, commit_low_mark));
        if (rel_bytes > keep) drop_stale(keep, &n_cap_drops, DROP_CAP);
        if (keep != UINT64_MAX && rel_bytes > keep + keep / 4) rel_census(keep, 0);
        else if (keep > cap && rel_bytes > cap) rel_census(cap, 1);
        if (rel_bytes > COMMIT_LOW_KEEP && low && !held)
            more = drop_stale(COMMIT_LOW_KEEP, &n_commit_drops, DROP_COMMIT) == PRESSURE_STEP;
    }
    if (low && !more && !held) commit_evict();
}

static volatile int64_t ts_ns[VK_TS_N], ts_n[VK_TS_N], ts_bytes[VK_TS_N];
static int ts_log_ms = -1;

static int ts_thread(void) {
    uint32_t tid = plat_thread_id();
    return tid == prof_render_tid || tid == prof_replay_tid;
}

void vk_texstat_time(int kind, uint64_t t0) {
    if (!t0 || !ts_thread()) return;
    plat_add64(&ts_ns[kind], (int64_t)(kern_now_ns() - t0));
    plat_inc64(&ts_n[kind]);
}

void vk_texstat_add(int kind, uint64_t n, uint64_t bytes) {
    if (!ts_thread()) return;
    plat_add64(&ts_n[kind], (int64_t)n);
    plat_add64(&ts_bytes[kind], (int64_t)bytes);
}

static void compact_frame_time(uint64_t d);

void vk_texstat_frame(uint64_t frame, uint64_t d) {
    if (frame % 30 == 15) vk_mem_tex_pools(frame);
    if (compact_on()) compact_frame_time(d);
    vk_astc_scratch_trim();
    if (ts_log_ms < 0) {
        ts_log_ms = cfg_int("NX2SARIA_TEXUP_LOG_MS");
        if (ts_log_ms < 0) ts_log_ms = 0;
    }
    int64_t ns[VK_TS_N], n[VK_TS_N], b[VK_TS_N];
    for (int k = 0; k < VK_TS_N; k++)
        ns[k] = plat_xchg64(&ts_ns[k], 0), n[k] = plat_xchg64(&ts_n[k], 0), b[k] = plat_xchg64(&ts_bytes[k], 0);
    double work = (ns[VK_TS_CREATE] + ns[VK_TS_UPLOAD] + ns[VK_TS_FLUSH]) / 1e6;
    static int32_t n_lines;
    if (!ts_log_ms || work < ts_log_ms || ++n_lines > 400) return;
    rt_log("texup: frame %llu: %.1f ms frame, texture work %.1f ms; created %lld in %.1f ms; uploads %lld (%.1f MiB) in "
           "%.1f ms: staging %.1f ms (%lld new buffers, %.1f MiB), storage copy %.1f ms, ASTC record %.1f ms (%lld "
           "scratch buffers made in %.1f ms); upload flush %lld in %.1f ms; new device memory of 32 MiB or more %lld "
           "(%.0f MiB)",
           (unsigned long long)frame, d / 1e6, work, (long long)n[VK_TS_CREATE], ns[VK_TS_CREATE] / 1e6,
           (long long)n[VK_TS_UPLOAD], b[VK_TS_UPLOAD] / 1048576.0, ns[VK_TS_UPLOAD] / 1e6, ns[VK_TS_STAGE] / 1e6,
           (long long)n[VK_TS_STG_NEW], b[VK_TS_STG_NEW] / 1048576.0, ns[VK_TS_COPY] / 1e6, ns[VK_TS_ASTC] / 1e6,
           (long long)n[VK_TS_SCRATCH], ns[VK_TS_SCRATCH] / 1e6, (long long)n[VK_TS_FLUSH], ns[VK_TS_FLUSH] / 1e6,
           (long long)n[VK_TS_DEVMEM], b[VK_TS_DEVMEM] / 1048576.0);
}

static uint32_t texel_alpha_one(int format, uint32_t *elem) {
    switch (format) {
    case 0x19: *elem = 1; return 0xFF;
    case 0x1A: *elem = 1; return 0x7F;
    case 0x1B: case 0x1C: *elem = 1; return 1;
    case 0x1D: *elem = 2; return 0x3C00;
    case 0x1E: *elem = 2; return 0xFFFF;
    case 0x1F: *elem = 2; return 0x7FFF;
    case 0x20: case 0x21: *elem = 2; return 1;
    case 0x22: *elem = 4; return 0x3F800000;
    default: *elem = 4; return 1;
    }
}

static void expand_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e, one = texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 3 * e, dst += 4 * e) {
        memcpy(dst, src, 3 * e);
        memcpy(dst + 3 * e, &one, e);
    }
}

static void shrink_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e;
    texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 4 * e, dst += 3 * e) memcpy(dst, src, 3 * e);
}

static void astc_check(const Tex *e, const uint8_t *raw, const VkAstcRegion *ar, int n, const uint8_t *gpu) {
    static volatile int32_t n_bad;
    static uint64_t n_tex, n_texels, n_diff_tex;
    uint32_t bw = e->f.astc_bw, bh = e->f.astc_bh;
    uint64_t diff = 0, first = UINT64_MAX;
    uint32_t want = 0, got = 0;
    int first_k = 0;
    for (int k = 0; k < n; k++) {
        const VkAstcRegion *r = &ar[k];
        size_t sb = (size_t)((r->w + bw - 1) / bw) * ((r->h + bh - 1) / bh) * 16, st = (size_t)r->w * r->h * 4;
        uint8_t *cpu = malloc(st);
        for (uint32_t z = 0; z < r->slices; z++, gpu += st) {
            astc_decode_ldr(raw + r->src + z * sb, r->w, r->h, bw, bh, e->f.astc_srgb, cpu);
            if (!memcmp(cpu, gpu, st)) continue;
            for (size_t i = 0; i < st / 4; i++)
                if (memcmp(cpu + 4 * i, gpu + 4 * i, 4)) {
                    if (first == UINT64_MAX) {
                        first = i, first_k = k;
                        memcpy(&want, cpu + 4 * i, 4), memcpy(&got, gpu + 4 * i, 4);
                    }
                    diff++;
                }
        }
        free(cpu);
        n_texels += (uint64_t)r->w * r->h * r->slices;
    }
    n_tex++;
    if (diff) {
        n_diff_tex++;
        log_limited(&n_bad, "ASTC check: texture %#llx fmt %#x %ux%u: %llu texels differ, first region %d texel %llu "
                    "cpu %08x gpu %08x", (unsigned long long)e->pub.addr, e->t.d.format, bw, bh,
                    (unsigned long long)diff, first_k, (unsigned long long)first, want, got);
    }
    if (n_tex == 1 || !(n_tex & 63) || diff)
        rt_log("vk: ASTC check: %llu textures, %llu texels compared, %llu textures differ", (unsigned long long)n_tex,
               (unsigned long long)n_texels, (unsigned long long)n_diff_tex);
}

#define UPLOAD_PART (32ull << 20)

enum { CHASH_MAX = 64 << 10, CHASH_BIG_MAX = 8 << 20, CHASH_MISSES = 2 };
static uint64_t content_hash(const void *p, uint64_t n) {
    const uint8_t *b = p;
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n, i = 0;
    if (n >= 512) {
        uint64_t c[8];
        for (int k = 0; k < 8; k++) c[k] = h ^ (0xc2b2ae3d27d4eb4full * (uint64_t)(k + 1));
        for (; i + 64 <= n; i += 64) {
            uint64_t w[8];
            memcpy(w, b + i, 64);
            for (int k = 0; k < 8; k++) {
                c[k] = (c[k] ^ w[k]) * 0xbf58476d1ce4e5b9ull;
                c[k] ^= c[k] >> 29;
            }
        }
        for (int k = 0; k < 8; k++) {
            h = (h ^ c[k]) * 0x94d049bb133111ebull;
            h ^= h >> 29;
        }
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0xbf58476d1ce4e5b9ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 0x94d049bb133111ebull;
    return h ^ (h >> 31);
}

static int astc_bl_regions(const Tex *e, uint32_t levels, uint32_t layers, int bw, int bh, VkAstcRegion *ar) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_ASTC_GPU_DESWIZZLE");
        if (!on) rt_log("vk: ASTC blocks deswizzled on the CPU (NX2SARIA_ASTC_GPU_DESWIZZLE=0)");
    }
    const nvn_texdesc *d = &e->t.d;
    if (!on || nvn_tex_is_linear(d)) return 0;
    uint32_t k = 0, sk = e->pub.skip;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) {
            uint32_t row_bytes, rows, slices, bpb;
            uint64_t base;
            unsigned th, td;
            if (nvn_deswizzle_geom(d, lv + sk, ly, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || (base & 15) ||
                th > 5 || td > 5)
                return 0;
            ar[k] = (VkAstcRegion){.src = base, .w = mip(d->width, lv + sk), .h = mip(d->height, lv + sk), .slices = slices,
                                   .level = lv, .layer = ly, .bl = 1, .th = (uint8_t)th, .td = (uint8_t)td};
            if (base + vk_astc_region_bytes(&ar[k], (uint32_t)bw, (uint32_t)bh) > e->t.L.size) return 0;
            k++;
        }
    return 1;
}

static int astc_arena(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_ASTC_ARENA");
        rt_log("vk: ASTC blocks for the GPU decoder stage in %s (NX2SARIA_ASTC_ARENA=%d)",
               on ? "the staging arena" : "a buffer per texture", on);
    }
    return on;
}

#define UP_RETRY_FRAMES 30
static uint64_t n_up_retry;
uint64_t vk_texture_upload_retries(void) { return __atomic_load_n(&n_up_retry, __ATOMIC_RELAXED); }

static int up_wait(const Tex *e) {
    return e->up_retry && __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) < e->up_retry;
}

static int astc_reformat(Tex *e, VkCommandBuffer cb, int bc) {
    static volatile int32_t n_failed;
    VkCtx *c = vk_ctx();
    const ImgKey *k = &e->ik;
    int s = e->f.astc_srgb;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = k->flags,
                            .imageType = (VkImageType)k->type,
                            .format = bc == 2 ? (s ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK)
                                              : (s ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK),
                            .extent = {k->w, k->h, k->d}, .mipLevels = k->levels, .arrayLayers = k->layers,
                            .samples = (VkSampleCountFlagBits)k->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = k->usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    ImgKey nk;
    img_key_of(&ci, &nk);
    VmaAllocation a = NULL;
    VkImage img = suballoc_image(c, &ci, &a);
    if (!img && vma_image(c, &ci, &nk, &img, &a) != VK_SUCCESS) {
        log_limited(&n_failed, "ASTC texture %#llx (%ux%u): no %s image could be made", (unsigned long long)e->pub.addr,
                    e->t.d.width, e->t.d.height, bc == 2 ? "BC1" : "BC3");
        return 0;
    }
    vk_exec_image_gone(e->pub.image);
    vk_defer_image(e->pub.image, e->alloc);
    vk_exec_image_gone(img);
    int relink = e->ru_on;
    if (relink) ru_unlink(e);
    if (e->released) rel_bytes -= e->bytes;
    if (e->retiring) retiring_bytes -= e->bytes;
    e->pub.image = img, e->alloc = a, e->reused = 0;
    own_memory(c, e);
    if (e->released) rel_bytes += e->bytes;
    if (e->retiring) retiring_bytes += e->bytes;
    e->pub.format = e->f.vk = ci.format;
    e->f.astc_bc = (uint8_t)bc;
    e->ik = nk;
    if (relink) ru_link(e);
    if (cb) {
        init_image(e, cb);
    } else {
        init_image(e, vk_upload_begin());
        vk_upload_end();
    }
    tex_gen_bump(e->key.addr);
    if (bc == 2) n_bc1_made++, n_bc1_made_bytes += e->bytes;
    else n_bc1_widened++;
    return 1;
}

static uint64_t n_bc1_up_checks, n_bc1_up_bytes;
static int astc_copy_opaque(const Tex *e, uint8_t *dst, const uint8_t *src, uint64_t size, uint32_t *decoded) {
    uint64_t nb = size / 16;
    int ok = astc_blocks_opaque_mt(src, nb, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb != 0, ASTC_OPAQUE_DECODES,
                                   decoded, dst);
    if (size % 16) memcpy(dst + 16 * nb, src + 16 * nb, size % 16);
    n_bc1_up_checks++, n_bc1_up_bytes += size;
    return ok;
}

static int chash_wanted(const Tex *e) {
    static int mode = -1;
    if (e->t.L.size <= CHASH_MAX) return 1;
    if (mode < 0) mode = rt_knob("TEX_SAME_BIG", "1");
    return RT_KNOB_ON(mode) && e->t.L.size <= CHASH_BIG_MAX && e->chash_miss < CHASH_MISSES && !vk_exec_pool_loading();
}

static volatile int64_t tmp_now, tmp_peak;
static void *tmp_alloc(size_t n) {
    void *p = malloc(n);
    if (!p) return NULL;
    int64_t now = __atomic_add_fetch(&tmp_now, (int64_t)n, __ATOMIC_RELAXED);
    int64_t pk = __atomic_load_n(&tmp_peak, __ATOMIC_RELAXED);
    while (now > pk && !__atomic_compare_exchange_n(&tmp_peak, &pk, now, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return p;
}
static void tmp_free(void *p, size_t n) {
    if (!p) return;
    free(p);
    __atomic_sub_fetch(&tmp_now, (int64_t)n, __ATOMIC_RELAXED);
}

static void upload(Tex *e) {
    static volatile int32_t n_bad, n_undecoded, n_split;
    int no_mem = 0;
    const tex_t *t = &e->t;
    const nvn_texdesc *d = &t->d;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb || !t->mem) return;
    if (!mem_readable(e->pub.addr, e->t.L.size)) {
        static volatile int32_t n_unmapped;
        log_limited(&n_unmapped, "texture %#llx (%llu bytes): storage not mapped; not uploaded",
                    (unsigned long long)e->pub.addr, (unsigned long long)e->t.L.size);
        e->uploaded = 1;
        return;
    }
    e->chash_ok = (uint8_t)chash_wanted(e);
    if (e->chash_ok) {
        if (mem_read_hold(e->pub.addr, e->t.L.size)) {
            e->chash = content_hash((const void *)(uintptr_t)e->pub.addr, e->t.L.size);
            mem_read_release();
            if (e->t.L.size > CHASH_MAX) big_hash_n++, big_hash_bytes += e->t.L.size;
        } else {
            e->chash_ok = 0;
        }
    }
    static __thread char ctx[160];
    snprintf(ctx, sizeof ctx, "texture upload %#llx (%llu bytes, fmt %#x %ux%u)", (unsigned long long)e->pub.addr,
             (unsigned long long)e->t.L.size, d->format, d->width, d->height);
    uint64_t p0 = perf_begin(), u0 = kern_now_ns();
    int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D, one_d = e->pub.type == VK_IMAGE_TYPE_1D;
    uint32_t levels = linear ? 1 : e->pub.levels, layers = linear ? 1 : e->pub.layers;
    int astc = e->f.astc_bw != 0, rgb = e->f.expand_rgb;
    int agpu = astc && vk_astc_gpu(), acheck = agpu && vk_astc_mode() == VK_ASTC_CHECK;
    uint32_t n = levels * layers, k = 0, sk = linear ? 0 : e->pub.skip;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkAstcRegion *ar = agpu ? calloc(n, sizeof *ar) : NULL;
    VkDeviceSize total = 0, decoded = 0;
    uint64_t src_bytes = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) {
            uint32_t w = mip(d->width, lv + sk), h = one_d ? 1 : mip(d->height, lv + sk), sl = is3d ? mip(d->depth, lv + sk) : 1;
            uint64_t texels = (uint64_t)w * h * sl, raw = (uint64_t)(w + bw - 1) / bw * bpb * ((h + bh - 1) / bh) * sl;
            uint64_t out = agpu ? raw : astc ? texels * 4 : rgb ? texels * (uint64_t)bpb / 3 * 4 : raw;
            rg[k++] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {e->pub.aspect, lv, ly, 1},
                                          .imageExtent = {w, h, sl}};
            total += (out + 15) & ~15ull;
            decoded += texels * 4;
            src_bytes += raw;
        }
    if (!sk) src_bytes = t->L.size;
    VkBuffer sb, rbuf = VK_NULL_HANDLE;
    VmaAllocation ralloc = NULL;
    uint8_t *rmap = NULL, *raw_copy = NULL;
    int checked = 0, parts = 1;
    int gdsw = agpu && !acheck && !one_d && astc_bl_regions(e, levels, layers, bw, bh, ar);
    int bc1 = e->f.astc_bc == 2, try1 = e->bc1_try && e->f.astc_bc == 1 && !e->n_up, opq = 1;
    uint32_t bc1_dec = 0;
    e->bc1_try = 0;
    if ((bc1 || try1) && !gdsw) {
        opq = astc_opaque(t, e->pub.addr, &e->f);
        if (bc1 && !opq && !astc_reformat(e, VK_NULL_HANDLE, 1)) {
            e->uploaded = 0;
            e->up_retry = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + UP_RETRY_FRAMES;
            __atomic_add_fetch(&n_up_retry, 1, __ATOMIC_RELAXED);
            free(ar), free(rg);
            return;
        }
        if (try1 && opq) astc_reformat(e, VK_NULL_HANDLE, 2);
        bc1 = try1 = 0;
    }
    int check = bc1 || try1;
    for (uint32_t k0 = 0, k1; k0 < n; k0 = k1) {
        VkDeviceSize base = rg[k0].bufferOffset, size;
        for (k1 = k0 + 1; k1 < n && (agpu || (k1 + 1 < n ? rg[k1 + 1].bufferOffset : total) - base <= UPLOAD_PART);) k1++;
        size = (k1 < n ? rg[k1].bufferOffset : total) - base;
        if (gdsw) size = t->L.size;
        if (gdsw && sk) {
            size = 0;
            for (k = 0; k < n; k++) size += vk_astc_region_bytes(&ar[k], (uint32_t)bw, (uint32_t)bh);
            src_bytes = size;
        }
        if (k0) parts++;
        VkCommandBuffer cb = vk_upload_begin();
        VkDeviceSize soff = 0;
        uint64_t s0 = kern_now_ns();
        uint8_t *stg = agpu && (acheck || !astc_arena()) ? vk_staging(size, &sb, NULL) : vk_staging_at(size, &sb, &soff);
        vk_texstat_time(VK_TS_STAGE, s0);
        uint64_t c0 = kern_now_ns();
        if (acheck) rmap = vk_staging(decoded, &rbuf, &ralloc);
        uint8_t **tmps = calloc(k1 - k0, sizeof *tmps);
        int held = mem_read_hold(e->pub.addr, e->t.L.size);
        if (held) plat_crash_context(ctx);
        else {
            static volatile int32_t n_gone;
            log_limited(&n_gone, "texture %#llx (%llu bytes): storage released during the upload; uploaded as zeros",
                        (unsigned long long)e->pub.addr, (unsigned long long)e->t.L.size);
            e->chash_ok = 0;
        }
        if (gdsw && !sk) {
            if (held && check) opq = astc_copy_opaque(e, stg, t->mem, size, &bc1_dec);
            else if (held) par_memcpy(stg, t->mem, size);
            else memset(stg, 0, size);
        } else if (gdsw) {
            VkDeviceSize at = 0;
            for (k = 0; k < n; k++) {
                VkDeviceSize nb = vk_astc_region_bytes(&ar[k], (uint32_t)bw, (uint32_t)bh);
                if (held && check) opq &= astc_copy_opaque(e, stg + at, t->mem + ar[k].src, nb, &bc1_dec);
                else if (held) par_memcpy(stg + at, t->mem + ar[k].src, nb);
                else memset(stg + at, 0, nb);
                ar[k].src = at, at += nb;
            }
        }
        for (k = k0; k < k1 && !gdsw; k++) {
            uint32_t lv = rg[k].imageSubresource.mipLevel, ly = rg[k].imageSubresource.baseArrayLayer;
            uint32_t w = rg[k].imageExtent.width, h = rg[k].imageExtent.height, sl = rg[k].imageExtent.depth;
            uint32_t rb = (w + bw - 1) / bw * (uint32_t)bpb, rows = (h + bh - 1) / bh;
            size_t raw = (size_t)rb * rows * sl;
            rg[k].bufferOffset -= base;
            uint8_t *dst = stg + rg[k].bufferOffset, *tmp = (astc && !agpu) || rgb ? tmp_alloc(raw) : NULL;
            tmps[k - k0] = tmp;
            if (!held) {
                memset(tmp ? tmp : dst, 0, raw);
            } else if (nvn_deswizzle_mt(d, lv + sk, ly, t->mem, tmp ? tmp : dst, rb)) {
                log_limited(&n_bad, "texture %#llx fmt %#x target %d: level %u layer %u has no CPU layout; uploaded as zeros",
                            (unsigned long long)e->pub.addr, d->format, d->target, lv + sk, ly);
                memset(tmp ? tmp : dst, 0, raw);
            }
        }
        if (held) {
            plat_crash_context(NULL);
            mem_read_release();
        }
        for (k = k0; k < k1 && !gdsw; k++) {
            uint32_t lv = rg[k].imageSubresource.mipLevel, ly = rg[k].imageSubresource.baseArrayLayer;
            uint32_t w = rg[k].imageExtent.width, h = rg[k].imageExtent.height, sl = rg[k].imageExtent.depth;
            uint32_t rb = (w + bw - 1) / bw * (uint32_t)bpb, rows = (h + bh - 1) / bh;
            uint8_t *dst = stg + rg[k].bufferOffset, *tmp = tmps[k - k0];
            if (agpu) {
                ar[k] = (VkAstcRegion){.src = rg[k].bufferOffset, .w = w, .h = h, .slices = sl, .level = lv, .layer = ly};
            } else if (astc) {
                uint64_t a0 = perf_begin();
                for (uint32_t z = 0; z < sl; z++)
                    astc_decode_ldr_mt(tmp + (size_t)z * rb * rows, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb,
                                       dst + (size_t)z * w * h * 4);
                perf_end(PERF_ASTC, a0);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else if (rgb) {
                expand_rgb(d->format, tmp, dst, (size_t)w * h * sl);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else {
                rg[k].bufferRowLength = rb / (uint32_t)bpb * (uint32_t)bw, rg[k].bufferImageHeight = rows * (uint32_t)bh;
            }
            tmp_free(tmp, (size_t)rb * rows * sl);
        }
        free(tmps);
        vk_texstat_time(VK_TS_COPY, c0);
        if (acheck) {
            raw_copy = malloc(size);
            memcpy(raw_copy, stg, size);
        }
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (agpu) {
            for (k = 0; k < n; k++) ar[k].src += soff;
            uint64_t a0 = perf_begin();
            int widen_failed = 0;
            if (bc1 && !opq) widen_failed = !astc_reformat(e, cb, 1);
            else if (try1 && opq) astc_reformat(e, cb, 2);
            checked = widen_failed ? -1
                    : vk_astc_record(cb, sb, size, ar, (int)n, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->f.astc_bc,
                                     e->pub.image, e->pub.aspect, rbuf);
            perf_end(PERF_ASTC, a0);
            vk_texstat_time(VK_TS_ASTC, a0);
            if (checked <= 0) {
                log_limited(&n_undecoded, "ASTC texture %#llx (fmt %#x %ux%u) not decoded on the GPU: left as it was%s",
                            (unsigned long long)e->pub.addr, d->format, d->width, d->height,
                            checked < 0 ? " and tried again later (no memory for the decode)" : "");
                e->chash_ok = 0;
                no_mem |= checked < 0;
            }
        } else {
            for (k = k0; k < k1; k++) rg[k].bufferOffset += soff;
            vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, k1 - k0, rg + k0);
        }
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
        vk_upload_end();
    }
    if (parts > 1)
        log_limited(&n_split, "texture %#llx fmt %#x %ux%u, %u levels, %u layers: %.1f MiB staged in %d parts",
                    (unsigned long long)e->pub.addr, d->format, d->width, d->height, levels, layers, total / 1048576.0,
                    parts);
    if (acheck) {
        vk_upload_flush(1);
        if (checked > 0) {
            vmaInvalidateAllocation(vk_ctx()->vma, ralloc, 0, VK_WHOLE_SIZE);
            astc_check(e, raw_copy, ar, (int)n, rmap);
        }
        vmaDestroyBuffer(vk_ctx()->vma, rbuf, ralloc);
    }
    free(raw_copy);
    free(ar);
    free(rg);
    uint64_t dt = kern_now_ns() - u0;
    if (e->uploaded) {
        n_reuploads++, perf_count(PERF_CNT_TEX_REUP);
        n_split_refill += e->sfill != 0;
        ru_n++, ru_bytes += src_bytes, ru_ns += dt;
        if (e->wt_n) {
            ru_wt_n++, ru_wt_bytes += src_bytes, ru_wt_ns += dt;
            if (rt_trace & RT_TRACE_PERF)
                rt_log("perf: frame %llu: re-upload after %u WriteTexels: texture %#llx fmt %#x %ux%u, %.1f KiB, %.3f ms",
                       (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), e->wt_n,
                       (unsigned long long)e->pub.addr, d->format, d->width, d->height, src_bytes / 1024.0, dt / 1e6);
        }
    } else {
        n_uploads++, perf_count(PERF_CNT_TEX_UP);
    }
    flight_ev(e, e->uploaded ? FLIGHT_TEX_REUP : FLIGHT_TEX_UP, e->wt_n, (uint32_t)(dt / 1000));
    flight_bytes(e);
    e->wt_n = 0;
    e->uploaded = 1;
    e->n_up++;

    if (no_mem) {
        e->uploaded = 0;
        e->up_retry = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + UP_RETRY_FRAMES;
        __atomic_add_fetch(&n_up_retry, 1, __ATOMIC_RELAXED);
    } else {
        e->up_retry = 0;
    }
    perf_end(PERF_TEX, p0);
    vk_texstat_time(VK_TS_UPLOAD, u0);
    vk_texstat_add(VK_TS_UPLOAD, 0, src_bytes);
}

static int fills_no_page(const Tex *e) {
    return ((e->pub.addr + e->t.L.size) & ~0xfffull) <= ((e->pub.addr + 0xfff) & ~0xfffull);
}

enum { SNAP_MAX = 4096 };
static int wb_exact = -1;
static volatile int32_t n_kept;

static int storage_rewritten(const Tex *e, uint64_t since, uint64_t *now) {
    uint64_t lo = (e->pub.addr + 0xfff) & ~0xfffull, hi = (e->pub.addr + e->t.L.size) & ~0xfffull;
    if (hi <= lo) lo = e->pub.addr, hi = e->pub.addr + e->t.L.size;
    int ch = vk_mem_changed(lo, hi - lo, since, now);
    if (ch != 1 || !since || !e->snap || memcmp(e->snap, (const void *)(uintptr_t)e->pub.addr, e->t.L.size)) return ch == 1;
    log_limited(&n_kept, "write-back of %#llx kept: fmt %#x %ux%u, %llu bytes; a neighbour wrote its page",
                (unsigned long long)e->pub.addr, e->t.d.format, e->t.d.width, e->t.d.height,
                (unsigned long long)e->t.L.size);
    return 0;
}

static int same_content(Tex *e) {
    static int on = -1;
    if (on < 0) { const char *v = cfg_get("NX2SARIA_TEX_SAME"); on = !(v && !strcmp(v, "0")); }
    if (!on || !e->chash_ok || e->wt_n) return 0;
    if (!mem_read_hold(e->pub.addr, e->t.L.size)) return 0;
    uint64_t h = content_hash((const void *)(uintptr_t)e->pub.addr, e->t.L.size);
    mem_read_release();
    int big = e->t.L.size > CHASH_MAX;
    if (h != e->chash) {
        if (big) big_diff_n++, e->chash_miss += e->chash_miss < 255;
        return 0;
    }
    ru_same_n++, ru_same_bytes += e->t.L.size;
    if (big) big_same_n++, big_same_bytes += e->t.L.size, e->chash_miss = 0;
    return 1;
}

static void make_fresh(Tex *e) {
    if (!from_storage(e) || e->pub.gpu_owned || (e->sfill && e->released)) return;
    uint64_t size = e->t.L.size;
    if (!e->uploaded) {
        if (up_wait(e)) return;
        vk_mem_changed(e->pub.addr, size, 0, &e->wgen);
        upload(e);
        return;
    }
    uint64_t now;
    int ch = vk_mem_changed(e->pub.addr, size, e->wgen, &now);
    e->wgen = now;
    e->last_ch = ch;
    if (ch && !same_content(e)) upload(e);
}

static void make_fresh_snap(Tex *e) {
    if (!from_storage(e) || e->pub.gpu_owned || (e->sfill && e->released)) return;
    if (!e->uploaded && up_wait(e)) return;
    uint64_t now;
    n_checks++;
    int ch = vk_mem_changed_snap(e->pub.addr, e->t.L.size, e->uploaded ? e->wgen : 0, submit_epoch, &now);
    e->wgen = now;
    if (!e->uploaded) { upload(e); return; }
    e->last_ch = ch;
    if (ch && !same_content(e)) upload(e);
}

static int img_block(const Tex *e, int *bw, int *bh) {
    if (e->f.astc_bc) { *bw = *bh = 4; return e->f.astc_bc == 2 ? 8 : 16; }
    if (e->f.astc_bw) { *bw = *bh = 1; return 4; }
    int b = nvn_fmt_bpb(e->t.d.format, bw, bh);
    if (e->f.expand_rgb) { *bw = *bh = 1; return b / 3 * 4; }
    return e->f.vk == e->f.ideal ? b : 0;
}

static int alias_compatible(const Tex *e, const Tex *src, const char **why) {
    const VkTexImage *p = &e->pub, *q = &src->pub;
    if (e->f.depth || e->f.stencil || src->f.depth || src->f.stencil || p->samples > 1 || q->samples > 1)
        return *why = "depth, stencil or multisample", 0;
    if (p->type != q->type) return *why = "another image type", 0;
    if (p->skip || q->skip) return *why = "made without its top levels (texture detail)", 0;
    if (nvn_tex_is_linear(&e->t.d) != nvn_tex_is_linear(&src->t.d)) return *why = "linear and block-linear", 0;
    int ew, eh, sw, sh, eb = img_block(e, &ew, &eh), sb = img_block(src, &sw, &sh);
    if (!eb || eb != sb || ew != sw || eh != sh) return *why = "another block size", 0;
    if (ew == 1 && eh == 1) return 1;
    if (p->width != q->width || p->height != q->height || p->depth != q->depth || e->t.L.size != src->t.L.size)
        return *why = "compressed, another shape", 0;
    return 1;
}

static int alias_has_gpu_content(const Tex *src) {
    if (!src->pub.gpu_owned) return 0;
    if (!src->pub.mirrored) return 1;
    uint64_t now;
    return src->gpu_dirty && !storage_rewritten(src, src->gpu_gen, &now);
}

static int alias_copy(Tex *e, const Tex *src) {
    int reopen;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
    if (!cb) return 0;
    int bw, bh, bpb = img_block(e, &bw, &bh);
    uint32_t levels = e->pub.levels < src->pub.levels ? e->pub.levels : src->pub.levels;
    uint32_t layers = e->pub.layers < src->pub.layers ? e->pub.layers : src->pub.layers, n = levels * layers, k = 0;
    int partial = src->pub.width < e->pub.width || src->pub.height < e->pub.height || src->pub.depth < e->pub.depth ||
                  levels < e->pub.levels || layers < e->pub.layers;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for an alias copy");
    VkDeviceSize total = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = umin(mip(e->pub.width, lv), mip(src->pub.width, lv));
            uint32_t h = umin(mip(e->pub.height, lv), mip(src->pub.height, lv));
            uint32_t z = umin(mip(e->pub.depth, lv), mip(src->pub.depth, lv));
            rg[k] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1},
                                        .imageExtent = {w, h, z}};
            total += ((VkDeviceSize)(w + bw - 1) / bw * ((h + bh - 1) / bh) * z * bpb + 15) & ~15ull;
        }
    VkBuffer buf = vk_exec_inline_buffer(total);
    if (!buf) {
        free(rg);
        vk_exec_inline_end(reopen);
        return 0;
    }
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, src->pub.image, VK_IMAGE_LAYOUT_GENERAL, buf, n, rg);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    if (partial && vk_fix_zero() && !e->bound_to && !e->pub.mirrored && !format_compressed(e->f.vk)) {
        VkClearColorValue zero = {0};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    vkCmdCopyBufferToImage(cb, buf, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_exec_inline_end(reopen);
    free(rg);
    return 1;
}

#define FAILED_HOLD 32
static struct {
    TexKey k;
    uint64_t until_ns;
} failed_hold[FAILED_HOLD];
static unsigned failed_next;

static int create_held(const TexKey *k) {
    uint64_t now = plat_now_ns();
    for (int i = 0; i < FAILED_HOLD; i++)
        if (failed_hold[i].until_ns > now && !memcmp(&failed_hold[i].k, k, sizeof *k)) return 1;
    return 0;
}

static void create_failed(const TexKey *k) {
    unsigned i = failed_next++ % FAILED_HOLD;
    failed_hold[i].k = *k, failed_hold[i].until_ns = plat_now_ns() + 2000000000ull;
}

static uint64_t n_cpu_empty[3];
#define START_LINES 8
#define START_LINES_ALL 20000
static void start_line(const Tex *e, const Tex *over, const char *gone, uint64_t gone_rel, int flag) {
    static SaidAt said[2];
    static uint32_t total[2];
    int kind = over ? 0 : 1;
    if (total[kind] >= START_LINES_ALL || !said_at(&said[kind], e->key.addr, START_LINES)) return;
    total[kind]++;
    char d[224], o[224], og[320];
    desc_text(d, sizeof d, e);
    if (over) {
        const char *why = mirror_refused(&over->t);
        desc_text(o, sizeof o, over);
        origin_text(og, sizeof og, over);
        rt_log("perf: frame %llu: texture %#llx filled from storage over a live entry%s: new [%s]; live [%s], %s%s%s, %s",
               (unsigned long long)frames_now(), (unsigned long long)e->key.addr, flag ? " (taken back)" : "", d, o,
               why ? "not CPU-written: " : "CPU-written", why ? why : "", no_content(over) ? ", no content" : "", og);
    } else {
        static const char *const how[4] = {"started empty", "got another entry's content", "got the destroyed entry's content",
                                           "was filled from its storage"};
        rt_log("perf: frame %llu: texture %#llx, not CPU-written (%s), %s after the released CPU-written entry at its "
               "address was destroyed: new [%s]; destroyed [%s], released at frame %llu",
               (unsigned long long)frames_now(), (unsigned long long)e->key.addr, mirror_refused(&e->t), how[flag & 3], d,
               gone, (unsigned long long)gone_rel);
    }
}

static int split_fill(Tex *e) {
    if (e->t.L.size > SPLIT_FILL_MAX) return n_split_big++, 0;
    if (e->reused) return n_split_reused++, 0;
    if (!mem_readable(e->pub.addr, e->t.L.size)) return 0;
    uint64_t t0 = kern_now_ns();
    e->sfill = (uint8_t)split_mode;
    make_fresh(e);
    if (!e->uploaded) return 0;
    n_split_fill++, split_total[1]++, n_split_bytes += e->t.L.size, n_split_ns += kern_now_ns() - t0;
    e->had |= HAD_FILL;
    return 1;
}

static int depth_alias_log(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("DEPTH_ALIAS_LOG", "0");
    return on;
}

static void start_at(Tex *e, const TexKey *k, int taken_back) {
    int alias = 0;
    int trace = (rt_trace & RT_TRACE_PERF) != 0;
    int gone = 0, rel_other = 0;
    char gone_desc[224] = "";
    uint64_t gone_rel = 0;
    Tex *src = NULL, *rsrc = NULL;
    int fill = !taken_back && tex_alias && split_wants(&e->t, &e->f), msrc_gpu = 0;
    uint64_t msrc_gen = 0;
    Tex *msrc = NULL;
    const char *why = "no live texture with GPU content";
    const Tex *other = NULL;
    int newer = 0;
    int live_content = 0;
    for (Tex *p = nvn_hm_get(&by_addr, &k->addr), *nx; p; p = nx) {
        nx = p->next_at;
        if (p == e) continue;
        if (taken_back && p->use_frame >= e->rel_frame && alias_has_gpu_content(p)) newer = 1;
        if (p->released) {
            if (p->pub.mirrored) {
                if (trace) {
                    int n = desc_text(gone_desc, sizeof gone_desc, p);
                    if (n > 0 && n < (int)sizeof gone_desc)
                        snprintf(gone_desc + n, sizeof gone_desc - (size_t)n, ", %s", p->pub.gpu_owned ? "GPU-written"
                                 : p->uploaded ? "uploaded" : "not uploaded");
                    gone_rel = p->rel_frame;
                }
                gone++;
                const char *no;
                uint64_t gen = 0;
                int gpu = alias_has_gpu_content(p);
                if (fill && !msrc && (gpu || (p->uploaded && !p->pub.gpu_owned &&
                                              !vk_mem_changed(p->pub.addr, p->t.L.size, p->wgen, &gen))) &&
                    alias_compatible(e, p, &no)) {
                    msrc = p, msrc_gpu = gpu, msrc_gen = gen;
                    continue;
                }
                drop_tex(p, DROP_REPLACED);
                continue;
            }
            p->superseded = 1, rel_other = 1;
            const char *unused;
            if (tex_alias && alias_has_gpu_content(p) && alias_compatible(e, p, &unused) &&
                (!rsrc || p->use_frame > rsrc->use_frame))
                rsrc = p;
            continue;
        }
        alias = 1;
        other = p;
        if (alias_has_gpu_content(p)) live_content = 1;
        if (!tex_alias || (src && !(same_but_swizzle(&p->key, k) && !same_but_swizzle(&src->key, k)))) continue;
        if (!alias_compatible(e, p, &why)) continue;
        if (!alias_has_gpu_content(p)) { why = "the live texture holds no GPU content"; continue; }
        src = p;
    }
    int from_released = !src && rsrc && !live_content;
    if (from_released) src = rsrc;
    if (alias) perf_count(PERF_CNT_TEX_ALIAS);
    if (alias && (e->f.depth || e->f.stencil) && depth_alias_log()) {
        static int n_dlog;
        if (n_dlog++ < 20000)
            rt_log("vk: depth alias: frame %llu %#llx %ux%ux%u fmt %#x%s%s over %ux%ux%u fmt %#x%s%s%s: %s",
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), (unsigned long long)k->addr,
                   e->pub.width, e->pub.height, e->pub.layers, e->t.d.format, e->bound_to ? " bound" : " own memory",
                   taken_back ? " (taken back)" : "", other->pub.width, other->pub.height, other->pub.layers,
                   other->t.d.format, other->f.depth ? " depth" : "", other->bound_to ? " bound" : " own memory",
                   e->bound_to && e->bound_to == other->bound_to ? ", same allocation" : "",
                   taken_back && !newer && !e->pub.mirrored ? "kept its own"
                   : e->reused || taken_back ? "cleared at the replay's position (reused)"
                   : depth_init_inline() ? "cleared at the replay's position" : "cleared in the upload batch");
    }
    if (taken_back && !newer && !e->pub.mirrored) {
        n_alias_kept++;
        return;
    }
    if (!tex_alias) {
        if (taken_back) init_new(e);
        return;
    }
    if (src || live_content) fill = 0;
    int copied = src && alias_copy(e, src), filled = 0;
    if (fill && msrc && alias_copy(e, msrc)) {
        copied = filled = 1;
        n_split_copy++, split_total[0]++;
        e->og_start = START_COPY_GONE;
        e->had |= HAD_COPY;
        if (!msrc_gpu && e->t.L.size <= SPLIT_FILL_MAX) {
            e->sfill = (uint8_t)split_mode, e->uploaded = 1, e->wgen = msrc_gen;
            e->chash_ok = 0;
        }
    }
    if (msrc) drop_tex(msrc, DROP_REPLACED);
    if (filled) {
    } else if (copied) {
        n_alias_copied++;
        if (from_released) n_alias_released++;
        e->og_start = from_released ? START_COPY_RELEASED : START_COPY_LIVE;
        e->had |= HAD_COPY;
        if (e->pub.mirrored) {
            vk_mem_changed(e->pub.addr, e->t.L.size, 0, &e->wgen);
            e->uploaded = 1;
            pool_gpu_owned(e);
            e->pub.gpu_owned = 1;
        }
        e->chash_ok = 0;
    } else {
        init_new(e);
        if (fill) filled = split_fill(e);
        e->og_start = filled ? START_FILL : alias ? (e->pub.mirrored ? START_UPLOAD_LIVE : START_EMPTY_LIVE)
                    : (gone || rel_other) && !e->pub.mirrored ? START_EMPTY_RELEASED : START_NEW;
        if (!filled && !e->pub.mirrored && !nvn_gpu_only(k->addr)) n_cpu_empty[alias ? 2 : gone || rel_other ? 1 : 0]++;
        if (alias) {
            if (e->pub.mirrored) {
                n_alias_upload++;
                if (trace) start_line(e, other, NULL, 0, taken_back);
            } else if (!filled) {
                n_alias_uninit++;
                static int logged;
                int depth = e->f.depth || e->f.stencil || other->f.depth || other->f.stencil;
                if (logged < 24 && other && !depth) {
                    logged++;
                    rt_log("vk: texture %#llx (%ux%u fmt %#x%s) starts empty over a live one (%ux%u fmt %#x): %s; frame %llu",
                           (unsigned long long)k->addr, e->pub.width, e->pub.height, e->t.d.format,
                           taken_back ? ", taken back" : "", other->pub.width, other->pub.height, other->t.d.format,
                           src ? "no replay recording to copy in" : why,
                           (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
                }
            }
        }
    }
    if (trace && gone && !e->pub.mirrored && !nvn_gpu_only(k->addr))
        start_line(e, NULL, gone_desc, gone_rel, e->og_start == START_COPY_GONE ? 2 : filled ? 3 : copied);
}

static _Thread_local int tex_deferred;
static volatile uint64_t n_deferred[3];
void vk_texture_deferred_stats(uint64_t out[3]) {
    for (int i = 0; i < 3; i++) out[i] = __atomic_exchange_n(&n_deferred[i], 0, __ATOMIC_RELAXED);
}
int vk_texture_deferred(void) { return tex_deferred; }
int vk_texture_deferred_take(void) {
    int d = tex_deferred;
    tex_deferred = 0;
    return d;
}

static int lookup_quiet(const Tex *e, const TexKey *k) {
    if (e && e->retiring) return pool_ctx;
    if (e) return !e->superseded && vk_exec_behind();
    if (holds.n && nvn_hm_get(&holds, k)) return pool_ctx;
    return create_held(k);
}

static Tex *entry_of(const tex_t *t) {
    TexKey k;
    make_key(t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    if ((!e || e->retiring || e->released) && vk_exec_turn_due() && !lookup_quiet(e, &k)) {
        __atomic_add_fetch(&n_deferred[!e ? 0 : e->retiring ? 1 : 2], 1, __ATOMIC_RELAXED);
        tex_deferred = 1;
        return NULL;
    }
    if (e && e->retiring) {
        if (pool_ctx) return NULL;
        retire_cancel(e);
    }
    if (!e && private_keys.n)
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr); p; p = p->next_at)
            if (!p->released && !p->bound_to && nvn_hm_get(&private_keys, &p->key) && same_but_swizzle(&p->key, &k)) {
                touch(p);
                return p;
            }
    if (!e) {
        if (held_out(&k) || create_held(&k)) return NULL;
        uint64_t c0 = kern_now_ns();
        if (tex_alias < 0) {
            tex_alias = cfg_bool("NX2SARIA_TEX_ALIAS");
            rt_log("vk: texture aliases: %s", tex_alias ? "a new image takes the live image's content" : "a new image starts empty");
        }
        if (!(e = create_tex(t, &k, tex_alias))) return create_failed(&k), NULL;
        origin_made(e);
        reuse_made(e);
        perf_count(PERF_CNT_TEX_NEW);
        start_at(e, &k, 0);
        flight_ev(e, FLIGHT_TEX_NEW, e->og_start, 0);
        e->next_at = nvn_hm_get(&by_addr, &k.addr);
        nvn_hm_put(&by_addr, &k.addr, e);
        nvn_hm_put(&by_key, &k, e);
        n_site[1]++;
        tex_gen_bump(k.addr);
        log_new(e, &k);
        vk_texstat_time(VK_TS_CREATE, c0);
        e->samp_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        e->made_frame = e->samp_frame;
    }
    if (e->released) {
        flight_ev(e, FLIGHT_TEX_TAKEBACK, 0, 0);
        e->samp_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        e->made_frame = e->samp_frame;
        uint64_t gap = e->samp_frame - e->rel_frame;
        if (gap > e->back_gap) e->back_gap = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        reuse_taken_back(e);
        e->released = 0, nreleased--, rel_unlink(e), n_site[2]++, tex_gen_flip(e, e->superseded || e->overwritten);
        if (e->pidx) pent[e->pidx - 1].rel = 0;
        e->overwritten = 0;
        e->og_backs++, e->og_back_frame = e->samp_frame;
        if (e->superseded) {
            e->superseded = 0, e->reused = 1;
            n_restarts++;
            start_at(e, &k, 1);
        }
    }
    e->t = *t;
    touch(e);
    return e;
}

void vk_texture_debug(const VkTexImage *img, uint32_t *uploads, int *last_change, int *pages_written) {
    lock();
    const Tex *e = (const Tex *)img;
    uint64_t now;
    *uploads = e->n_up;
    *last_change = e->last_ch;
    *pages_written = e->pub.mirrored && e->uploaded ? vk_mem_changed(e->pub.addr, e->t.L.size, e->wgen, &now) : 0;
    unlock();
}

const VkTexImage *vk_texture_get(const tex_t *t) {
    if (!t || !t->mem) return NULL;
    lock();
    Tex *e = entry_of(t);
    if (e) make_fresh(e);
    unlock();
    return e ? &e->pub : NULL;
}

enum { WB_SMALL_DEFAULT = 64 << 10, WB_RUN = 16 };
static int small_pending;
static uint64_t wb_limit = WB_SMALL_DEFAULT;
static int wb_trace;
static int wb_cfg_read;

static void wb_cfg_init(void) {
    if (wb_cfg_read) return;
    int v = cfg_int("NX2SARIA_WB_FENCE_LIMIT");
    wb_limit = v < 0 ? 0 : (uint64_t)v;
    wb_trace = (rt_trace & RT_TRACE_GFX) != 0;
    wb_cfg_read = 1;
    if (wb_limit != WB_SMALL_DEFAULT) rt_log("vk: fence write-back limit %llu bytes", (unsigned long long)wb_limit);
}

static int wb_wanted(const Tex *e) { return e->t.L.size <= wb_limit || e->run < WB_RUN; }

static void mark_released(Tex *e) {
    flight_ev(e, FLIGHT_TEX_RELEASE, 0, 0);
    e->released = 1;
    if (e->pidx) pent[e->pidx - 1].rel = 1;
    e->rel_epoch = submit_epoch;
    e->rel_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    nreleased++;
    rel_append(e);
    n_site[3]++;
    tex_gen_flip(e, 0);
}

static void overwrite_others(const Tex *e, uint64_t fr) {
    uint64_t lo = e->key.addr, hi = lo + e->t.L.size;
    int released = 0;
    for (int i = gwin_lower(lo > gwin_span ? lo - gwin_span : 0); i < ngwin && gwin[i]->key.addr < hi; i++) {
        Tex *q = gwin[i];
        uint64_t qlo = q->key.addr, qhi = qlo + q->t.L.size;
        if (q->overwritten || qlo == lo || qhi <= lo || (lo >= qlo && hi <= qhi)) continue;
        if (fr - q->gw_frame < OVERWRITE_FRAMES) continue;
        if (!q->pub.gpu_owned) continue;
        q->overwritten = 1, q->ow_frame = fr;
        n_overwritten++;
        if (!q->released) mark_released(q), released = 1;
    }
    if (released) sweep_released();
}

void vk_texture_gpu_written(const tex_t *t) {
    if (!t || !t->mem) return;
    vk_exec_frame_written(t);
    lock();
    Tex *e = entry_of(t);
    if (e) {
        make_fresh(e);
        uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        e->gw_frame = now;
        if (e->scan_frame != now + 1 && nvn_gpu_only(e->key.addr)) {
            e->scan_frame = now + 1;
            overwrite_others(e, now);
        }
        pool_gpu_owned(e);
        e->pub.gpu_owned = 1;
        mark_clobbered(e);
        e->chash_ok = 0;
        if (e->pub.skip) {
            static volatile int32_t n_skip_written;
            log_limited(&n_skip_written, "texture %#llx (fmt %#x %ux%u), made without its top %u levels, was written by "
                        "the GPU: its levels are not the game's", (unsigned long long)e->pub.addr, e->t.d.format,
                        e->t.d.width, e->t.d.height, e->pub.skip);
        }
        uint64_t f = vk_exec_recording_fence();
        if (f > e->wr_fence) e->wr_fence = f;
        if (e->pub.mirrored) {
            e->gpu_dirty = 1;
            storage_rewritten(e, 0, &e->gpu_gen);
            if (wb_exact < 0) wb_exact = cfg_bool("NX2SARIA_WB_EXACT");
            if (wb_exact && !e->snap && e->t.L.size <= SNAP_MAX && fills_no_page(e) && !(e->snap = malloc(SNAP_MAX)))
                rt_fatal("vk: out of memory for a texture's storage copy");
            if (e->snap) memcpy(e->snap, (const void *)(uintptr_t)e->pub.addr, e->t.L.size);
            e->gpu_seq = ++gpu_writes;
            e->gpu_fence = vk_exec_recording_fence();
            uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
            if (fr != e->run_frame || !e->run) {
                e->run = fr == e->run_frame + 1 && e->run ? (uint8_t)(e->run < WB_RUN ? e->run + 1 : WB_RUN) : 1;
                e->run_frame = fr;
            }
            wb_cfg_init();
            if (wb_wanted(e)) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
            own_add(e);
        }
    }
    unlock();
}

void vk_texture_release(const tex_t *t) {
    if (!t || !t->mem) return;
    TexKey k;
    make_key(t, &k);
    lock();
    Tex *e = nvn_hm_get(&by_key, &k);
    int changed = 0;
    if (sinfo)
        for (int id = bucket_head[agen_idx(k.addr)]; id >= 0; id = slot_next[id]) {
            TexSlot *s = &tslots[id];
            if (s->finalized || (uintptr_t)s->t.mem != k.addr) continue;
            TexKey sk;
            make_key(&s->t, &sk);
            if (memcmp(&sk, &k, sizeof k)) continue;
            if (s->t.serial != t->serial) {
                n_fin_other++, n_fin_other_hm += terrain_heightmap(&s->t);
                continue;
            }
            s->finalized = 1, s->ce = NULL, changed = 1;
            n_fin_own++;
        }
    if (e && !e->released) {
        mark_released(e);
        changed = 0;
        sweep_released();
    }
    if (changed) tex_gen_bump(k.addr);
    unlock();
}

void vk_texture_range_gone(uint64_t lo, uint64_t size, VmaAllocation alloc) {
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (e && ((e->key.addr >= lo && e->key.addr - lo < size) || (alloc && e->bound_to == alloc))) list[n++] = e;
    }
    for (int i = 0; i < n; i++) drop_tex(list[i], DROP_RANGE);
    free(list);
    unlock();
}

void vk_texture_register(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) rt_fatal("vk: texture slot %d out of range", id);
    if (id >= VK_POOL_TEX) {
        static int said;
        if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
            rt_log("vk: texture slot %d registered, past the %d the shaders index (VK_POOL_TEX): a shader that samples it "
                   "reads slot %d instead (%d for a translation carried over from the translator before)", id,
                   VK_POOL_TEX, VK_POOL_TEX - 1, VK_POOL_TEX_CARRIED - 1);
    } else if (id >= VK_POOL_TEX_CARRIED) {
        static int said;
        uint32_t n;
        if (!__atomic_load_n(&said, __ATOMIC_RELAXED) && (n = gfx_shader_pool_short_seen()) &&
            !__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
            rt_log("vk: texture slot %d registered while %u shaders in use index the texture pool with the earlier "
                   "length (%d, gfx_shader.c pool_short_note): they read slot %d in its place", id, n,
                   VK_POOL_TEX_CARRIED, VK_POOL_TEX_CARRIED - 1);
    }
    lock();
    if (id >= ntslots) {
        int n = ntslots ? ntslots : 1024;
        while (n <= id) n *= 2;
        TexSlot *s = realloc(tslots, sizeof *s * (size_t)n);
        if (!s) rt_fatal("vk: out of memory for texture slots");
        memset(s + ntslots, 0, sizeof *s * (size_t)(n - ntslots));
        tslots = s, ntslots = n;
    }
    if (!sinfo) {
        uint64_t *p = calloc(1 << 20, sizeof *p);
        slot_next = malloc(sizeof *slot_next << 20), slot_prev = malloc(sizeof *slot_prev << 20);
        slot_grp = malloc(sizeof *slot_grp << 20);
        slot_bucket = calloc(1 << 20, sizeof *slot_bucket);
        if (!p || !slot_next || !slot_prev || !slot_grp || !slot_bucket) rt_fatal("vk: out of memory for texture slot tokens");
        for (int i = 0; i < (1 << 20); i++) slot_prev[i] = -2, slot_next[i] = -1, slot_grp[i] = -1;
        for (int i = 0; i < AGEN_N; i++) bucket_head[i] = -1, bucket_ghead[i] = -1;
        __atomic_store_n(&sinfo, p, __ATOMIC_RELEASE);
    }
    TexSlot *s = &tslots[id];
    int current = s->used && !s->finalized && s->cgen == agen[sinfo[id] & 0xffff];
    if (current && ((s->ce && !s->ce->released) || s->cheld) && !memcmp(&s->t, t, sizeof *t) &&
        s->has_view == (v != NULL) && (!v || !memcmp(&s->v, v, sizeof *v))) {
        n_same_regs++;
        unlock();
        return;
    }
    if (flight_on && tex_mirrored(t))
        flight_tex(FLIGHT_TEX_REG, (uint64_t)(uintptr_t)t->mem, (uint32_t)t->d.format, t->d.width, t->d.height, 0,
                   FLIGHT_TF_MIRRORED,
                   !s->used ? 0 : s->finalized ? 3 : (uintptr_t)s->t.mem == (uintptr_t)t->mem ? 1 : 2, (uint32_t)id);
    s->t = *t;
    if (!s->used && id < VK_POOL_TEX) pool_slots++;
    s->used = 1;
    s->finalized = 0;
    s->ce = NULL;
    s->cheld = 0;
    s->fresh = 1;
    slots_gen++;
    if (!++reg_serial) reg_serial = 1;
    __atomic_store_n(&sinfo[id], (uint64_t)reg_serial << 16 | agen_idx((uintptr_t)t->mem), __ATOMIC_RELEASE);
    dirty_push((uint32_t)id);
    s->has_view = v != NULL;
    if (v) s->v = *v;
    else memset(&s->v, 0, sizeof s->v);
    s->khash = 0;
    if (!tex_mirrored(t)) {
        TexKey k;
        make_key(t, &k);
        uint64_t h = 0xcbf29ce484222325ull;
        const uint8_t *b = (const uint8_t *)&k;
        for (size_t i = 0; i < sizeof k; i++) h = (h ^ b[i]) * 0x100000001b3ull;
        b = (const uint8_t *)&s->v;
        for (size_t i = 0; i < sizeof s->v; i++) h = (h ^ b[i]) * 0x100000001b3ull;
        s->khash = (h ^ s->has_view) | 1;
    }
    slot_place(id, agen_idx((uintptr_t)t->mem), s->khash);
    unlock();
}

uint64_t vk_texture_slot_khash_locked(int id) { return id >= 0 && id < ntslots && tslots[id].used ? tslots[id].khash : 0; }
uint32_t vk_texture_entry_gen(void) { return tex_gen; }

static int create_budget = -1, create_deferred;
#define WANT_PER_FRAME 32
static int want_take(void) {
    static uint64_t frame = ~0ull;
    static int left;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = WANT_PER_FRAME;
    return left > 0 ? left-- : 0;
}

void vk_texture_create_budget_locked(int n) { create_budget = n, create_deferred = 0; }

#define SHOWN_PER_FRAME 64
static int pool_shown;
void vk_texture_pool_shown_locked(int shown) { pool_shown = shown; }
static int shown_take(const TexSlot *s) {
    static int on = -1;
    static uint64_t frame = ~0ull;
    static int left;
    if (on < 0) on = cfg_bool("NX2SARIA_POOL_SHOWN_FIRST");
    if (!on || !(pool_shown || s->fresh)) return 0;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = SHOWN_PER_FRAME;
    return left > 0 ? left-- : 0;
}

static uint64_t pool_make_ns;
uint64_t vk_texture_pool_make_ns_take(void) { return __atomic_exchange_n(&pool_make_ns, 0, __ATOMIC_RELAXED); }
static void pool_made(uint64_t t0) { __atomic_fetch_add(&pool_make_ns, kern_now_ns() - t0, __ATOMIC_RELAXED); }
int vk_texture_create_deferred_locked(void) { int d = create_deferred; create_deferred = 0; return d; }

static int stale_kept(const Tex *own) {
    static int on = -1;
    if (on < 0) on = rt_knob("TEX_STALE_KEPT", "1") == 1;
    return on && own && !own->superseded && !own->overwritten && (own->uploaded || own->pub.gpu_owned);
}

#define DATA_PER_FRAME 24
#define DATA_FIRST_BYTES (4u << 20)
static int data_take(const TexSlot *s) {
    static int on = -1;
    static uint64_t frame = ~0ull;
    static int left;
    if (on < 0) on = cfg_bool("NX2SARIA_POOL_DATA_FIRST");
    if (!on || s->t.L.size > DATA_FIRST_BYTES) return 0;
    int bw = 1, bh = 1;
    if (!nvn_fmt_bpb(s->t.d.format, &bw, &bh) || bw > 1 || bh > 1) return 0;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = DATA_PER_FRAME;
    return left > 0 ? left-- : 0;
}

static Tex *slot_resolve(TexSlot *s, view_t *view, int *has_view) {
    *view = s->v;
    *has_view = s->has_view;
    TexKey k;
    make_key(&s->t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    int stale = 0;
    if (!e || e->released || s->finalized) {
        Tex *own = e, *any = NULL;
        e = NULL;
        int cpu_slot = tex_mirrored(&s->t);
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr); p; p = p->next_at) {
            if (p->released) continue;
            if (p->key.d.target != k.d.target) continue;
            if (same_but_swizzle(&p->key, &k) && (!cpu_slot || !s->finalized || alias_has_gpu_content(p))) { e = p; break; }
            if (!any && (!cpu_slot || alias_has_gpu_content(p)) && follow_size_ok(p, &k)) any = p;
        }
        if (!e && any) e = any;
        else if (!e && !cpu_slot) e = own && own->superseded ? NULL : own;
        else if (!e && cpu_slot && s->finalized && stale_kept(own)) e = own, n_stale_kept++;
        else if (!e && cpu_slot && s->finalized) stale = 1, n_stale++, n_stale_hm += terrain_heightmap(&s->t);
    }
    resolve_stale = stale;
    if (!e && s->t.mem && !stale) {
        if (pool_ctx && holds.n && nvn_hm_get(&holds, &k)) resolve_held = 1;
        else if (create_budget < 0) e = entry_of(&s->t);
        else if (!mem_readable((uint64_t)(uintptr_t)s->t.mem, s->t.L.size)) e = NULL;
        else if (!create_budget && !(s->cwant && want_take()) && !shown_take(s) && !data_take(s)) create_deferred = 1;
        else {
            uint64_t m0 = kern_now_ns();
            e = entry_of(&s->t);
            pool_made(m0);
            if (create_budget > 0) create_budget--;
        }
        if (e) s->cwant = 0, s->fresh = 0;
    }
    if (e && e->retiring) {
        if (pool_ctx) e = NULL, resolve_held = 1;
        else retire_cancel(e);
    }
    if (e && !(s->has_view && s->v.set_swizzle)) {
        const nvn_texdesc *d = &s->t.d;
        uint8_t sw[4] = {(uint8_t)d->swizzle_r, (uint8_t)d->swizzle_g, (uint8_t)d->swizzle_b, (uint8_t)d->swizzle_a};
        if (!(sw[0] | sw[1] | sw[2] | sw[3]))
            sw[0] = NVN_TEXTURE_SWIZZLE_R, sw[1] = NVN_TEXTURE_SWIZZLE_G, sw[2] = NVN_TEXTURE_SWIZZLE_B,
            sw[3] = NVN_TEXTURE_SWIZZLE_A;
        if (memcmp(sw, e->pub.swz, 4)) {
            if (!s->has_view) memset(view, 0, sizeof *view);
            memcpy(view->swz, sw, 4);
            view->set_swizzle = 1;
            *has_view = 1;
        }
    }
    return e;
}

uint32_t vk_texture_slot_why_locked(int id, uint64_t *addr, uint32_t *format, uint32_t *w, uint32_t *h) {
    *addr = 0, *format = *w = *h = 0;
    if (id < 0 || id >= ntslots || !tslots[id].used) return 0;
    const TexSlot *s = &tslots[id];
    TexKey k;
    make_key(&s->t, &k);
    const Tex *own = nvn_hm_get(&by_key, &k);
    *addr = k.addr, *format = (uint32_t)s->t.d.format, *w = s->t.d.width, *h = s->t.d.height;
    const Tex *ce = s->ce && s->cgen == agen[sinfo[id] & 0xffff] ? s->ce : NULL;
    return (s->finalized ? 1u : 0) | (own ? 2u : 0) | (own && own->released ? 4u : 0) | (own && own->superseded ? 8u : 0) |
           (own && own->overwritten ? 16u : 0) | (own && own->uploaded ? 32u : 0) |
           (s->t.mem && mem_readable((uint64_t)(uintptr_t)s->t.mem, s->t.L.size) ? 64u : 0) |
           (holds.n && nvn_hm_get(&holds, &k) ? 128u : 0) | (tex_mirrored(&s->t) ? 256u : 0) |
           (own && own->pub.gpu_owned ? 512u : 0) | (own && own->retiring ? 1024u : 0) |
           (ce ? 2048u : 0) | (ce && ce->pub.type == VK_IMAGE_TYPE_3D ? 4096u : 0) | (ce && ce != own ? 8192u : 0);
}

void vk_texture_lock(void) { lock(); }
void vk_texture_unlock(void) { unlock(); }
uint32_t vk_texture_slots_gen(void) { return slots_gen; }
uint64_t vk_texture_same_regs(void) { return __atomic_load_n(&n_same_regs, __ATOMIC_RELAXED); }

void vk_texture_bucket_stats(char *out, size_t n) {
    lock();
    static uint32_t occ[AGEN_N];
    memset(occ, 0, sizeof occ);
    for (int i = 0; i < ntslots && sinfo; i++)
        if (tslots[i].used) occ[sinfo[i] & 0xffff]++;
    int top[4] = {-1, -1, -1, -1};
    for (int b = 0; b < AGEN_N; b++)
        for (int k = 0; k < 4; k++)
            if (top[k] < 0 || occ[b] > occ[top[k]]) {
                for (int j = 3; j > k; j--) top[j] = top[j - 1];
                top[k] = b;
                break;
            }
    uint64_t hot = 0;
    for (int b = 0; b < AGEN_N; b++) hot += (uint64_t)bump_count[b] * occ[b];
    snprintf(out, n, "%llu new generations (destroy %llu, create %llu, take-back %llu, release %llu; %llu releases and "
             "take-backs went back to a generation their state had), %llu slot invalidations; fullest buckets %u/%u/%u/%u slots, bumped %u/%u/%u/%u",
             (unsigned long long)n_bumps, (unsigned long long)n_site[0], (unsigned long long)n_site[1],
             (unsigned long long)n_site[2], (unsigned long long)n_site[3], (unsigned long long)n_undone,
             (unsigned long long)hot, occ[top[0]], occ[top[1]], occ[top[2]], occ[top[3]],
             bump_count[top[0]], bump_count[top[1]], bump_count[top[2]], bump_count[top[3]]);
    size_t used = strlen(out);
    if (used < n)
        snprintf(out + used, n - used, "; pool freshness check: %llu entries checked, %llu passed by their regions (missed: "
                 "%llu not uploaded, %llu a region without a snapshot yet, %llu untracked, %llu written since, %llu "
                 "nothing to check; %llu snapshot queries of NX2SARIA_POOL_PRESNAP; %llu released entries passed by)",
                 (unsigned long long)n_pool_checks, (unsigned long long)n_pool_skips,
                 (unsigned long long)n_pool_miss[0], (unsigned long long)n_pool_miss[1],
                 (unsigned long long)n_pool_miss[2], (unsigned long long)n_pool_miss[3],
                 (unsigned long long)n_pool_miss[4], (unsigned long long)n_presnap_q, (unsigned long long)n_pool_rel);
    used = strlen(out);
    if (used < n && big_hash_n + big_same_n + big_diff_n)
        snprintf(out + used, n - used, "; textures over 64 KiB: %llu uploads hashed (%.1f MiB), after a write to their "
                 "pages %llu left as they were (%.1f MiB), %llu uploaded again",
                 (unsigned long long)big_hash_n, big_hash_bytes / 1048576.0, (unsigned long long)big_same_n,
                 big_same_bytes / 1048576.0, (unsigned long long)big_diff_n);
    big_hash_n = big_hash_bytes = big_same_n = big_same_bytes = big_diff_n = 0;
    used = strlen(out);
    if (used < n)
        snprintf(out + used, n - used, "; %d slot groups holding %llu slots", ngrp_live, (unsigned long long)n_grouped);
    int held = 0, gown = 0;
    for (int i = 0; i < npref; i++)
        if (pref[i].refs && pref[i].e) held++, gown += pref[i].gown;
    used = strlen(out);
    if (used < n)
        snprintf(out + used, n - used, "; pool slots hold %d entries, %d of them GPU-owned (stamped in each submission "
                 "checked by list)", held, gown);
    n_bumps = n_undone = n_pool_checks = n_pool_skips = n_presnap_q = n_pool_rel = 0;
    memset(n_pool_miss, 0, sizeof n_pool_miss);
    memset(n_site, 0, sizeof n_site);
    memset(bump_count, 0, sizeof bump_count);
    unlock();
}

uint64_t vk_texture_slot_token(int id) {
    const uint64_t *si = __atomic_load_n(&sinfo, __ATOMIC_ACQUIRE);
    if (!si || id < 0 || id >= (1 << 20)) return 0;
    uint64_t v = __atomic_load_n(&si[id], __ATOMIC_ACQUIRE);
    return v ? (v >> 16) << 32 | __atomic_load_n(&agen[v & 0xffff], __ATOMIC_ACQUIRE) : 0;
}
uint32_t vk_sampler_gen(void) {
    uint64_t at = __atomic_load_n(&samp_retry_at, __ATOMIC_RELAXED);
    if (__builtin_expect(at != 0, 0) && plat_now_ns() >= at &&
        __atomic_compare_exchange_n(&samp_retry_at, &at, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        __atomic_add_fetch(&samp_gen, 1, __ATOMIC_RELAXED);
    return __atomic_load_n(&samp_gen, __ATOMIC_RELAXED);
}
int vk_texture_slot_count_locked(void) { return ntslots; }
int vk_sampler_slot_count_locked(void) { return nsslots; }

static int unalias(Tex *e) {
    e->clobbered = 0;
    Tex *sib[16];
    int n = 0;
    for (Tex *p = nvn_hm_get(&by_addr, &e->key.addr); p; p = p->next_at) {
        if (p == e || p->released) continue;
        if (!same_but_swizzle(&p->key, &e->key) || n == 16) {
            n_unalias_kept++;
            return 0;
        }
        sib[n++] = p;
    }
    if (n_unaliased++ < 32)
        rt_log("vk: texture %#llx (%ux%u nvn fmt %#x) was read after another image drew over its memory: it gets memory "
               "of its own", (unsigned long long)e->pub.addr, e->pub.width, e->pub.height, e->key.d.format);
    n_unaliased_period++;
    nvn_hm_put(&private_keys, &e->key, (void *)1);
    for (int i = 0; i < n; i++) destroy_tex(sib[i]);
    destroy_tex(e);
    return 1;
}

const VkTexImage *vk_texture_by_id(int id, view_t *view, int *has_view) {
    lock();
    const VkTexImage *r = vk_texture_by_id_locked(id, view, has_view);
    unlock();
    return r;
}

const VkTexImage *vk_texture_by_id_locked(int id, view_t *view, int *has_view) {
    if (id < 0 || id >= ntslots || !tslots[id].used) return NULL;
    TexSlot *s = &tslots[id];
again:;
    uint32_t g = agen[sinfo[id] & 0xffff];
    if ((!s->ce || s->cgen != g) && !(s->cheld && s->cgen == g && pool_ctx)) {
        view_t v;
        int hv;
        resolve_held = 0;
        in_lookup = 1;
        s->ce = slot_resolve(s, &v, &hv);
        in_lookup = 0;
        s->cview = v, s->chas_view = (uint8_t)hv;
        s->cgen = g;
        s->cepoch = 0;
        s->cheld = (uint8_t)(!s->ce && resolve_held);
    }
    Tex *e = s->ce;
    if (e && !pool_ctx) touch(e);
    if (e && e->clobbered && unalias(e)) {
        s->ce = NULL;
        goto again;
    }
    if ((e || s->cheld) && s->cepoch != submit_epoch) {
        round_checks++;
        if (e && !e->released && (!e->uploaded || e->checked != submit_epoch)) {
            uint64_t m0 = pool_ctx && !e->uploaded ? kern_now_ns() : 0;
            make_fresh_snap(e);
            if (m0) pool_made(m0);
            e->checked = submit_epoch;
            touch(e);
        }
        s->cepoch = submit_epoch;
    }
    if (view) *view = has_view ? s->cview : s->v;
    if (has_view) *has_view = view ? s->chas_view : s->has_view;
    return e ? &e->pub : NULL;
}

const VkTexImage *vk_texture_exact_by_id_locked(int id, view_t *view, int *has_view) {
    if (id < 0 || id >= ntslots || !tslots[id].used) return NULL;
    TexSlot *s = &tslots[id];
    Tex *e = s->ce && s->cgen == agen[sinfo[id] & 0xffff] ? s->ce : NULL;
    if (e && e->pub.skip) {
        if (exact.n >= EXACT_MAX) nvn_hm_clear(&exact, NULL);
        nvn_hm_put(&exact, &e->key, (void *)(uintptr_t)1);
        n_exact++;
        static volatile int32_t n_logged;
        log_limited(&n_logged, "texture detail: texture %#llx (fmt %#x %ux%u, %u levels) is read by texel position or its "
                    "size is asked for: made whole again (%llu so far)", (unsigned long long)e->pub.addr, e->t.d.format,
                    e->t.d.width, e->t.d.height, e->pub.levels + e->pub.skip, (unsigned long long)n_exact);
        drop_tex(e, DROP_EXACT);
    }
    return vk_texture_by_id_locked(id, view, has_view);
}

int vk_texture_slot_swz(int id, uint8_t swz[4]) {
    lock();
    int ok = id >= 0 && id < ntslots && tslots[id].used;
    if (ok) {
        const nvn_texdesc *d = &tslots[id].t.d;
        swz[0] = (uint8_t)d->swizzle_r, swz[1] = (uint8_t)d->swizzle_g, swz[2] = (uint8_t)d->swizzle_b;
        swz[3] = (uint8_t)d->swizzle_a;
    }
    unlock();
    return ok;
}

uint32_t vk_texture_epoch(void) { return __atomic_load_n(&submit_epoch, __ATOMIC_RELAXED); }

void vk_texture_new_submit(void) {
    lock();
    if (pool_by_list || (round_checks && (uint64_t)round_checks * 2 >= pool_slots)) look_round++;
    round_checks = 0, pool_by_list = 0;
    if (!++submit_epoch) submit_epoch = 1;
    rate_tick();
    residency_step();
    sweep_pressure();
    if (compact_on()) compact_step();
    static uint64_t log_at;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) / 60;
    if ((rt_trace & RT_TRACE_PERF) && f != log_at && tex_alias > 0) {
        log_at = f;
        rt_log("perf: frame %llu: texture aliases: content copied %llu (%llu from a released texture), filled from storage "
               "%llu, left uninitialised %llu; taken back with their own content %llu; read after an overwrite: given own "
               "memory %llu, kept in pool memory %llu",
               (unsigned long long)f * 60, (unsigned long long)n_alias_copied, (unsigned long long)n_alias_released,
               (unsigned long long)n_alias_upload, (unsigned long long)n_alias_uninit, (unsigned long long)n_alias_kept,
               (unsigned long long)n_unaliased_period, (unsigned long long)n_unalias_kept);
        n_alias_copied = n_alias_upload = n_alias_uninit = n_alias_kept = n_alias_released = 0;
        if (n_dinit_inline || n_dinit_upload)
            rt_log("perf: frame %llu: new depth images started at the replay's position %llu (%llu ended an open pass), "
                   "in the upload batch %llu", (unsigned long long)f * 60, (unsigned long long)n_dinit_inline,
                   (unsigned long long)n_dinit_inpass, (unsigned long long)n_dinit_upload);
        n_dinit_inline = n_dinit_inpass = n_dinit_upload = 0;
        if (n_disuse_shared || n_disuse_diverted)
            rt_log("perf: frame %llu: unsampled textures kept because another descriptor shares their address %llu; "
                   "holds dropped by a mark that another entry at the address had taken %llu",
                   (unsigned long long)f * 60, (unsigned long long)n_disuse_shared, (unsigned long long)n_disuse_diverted);
        n_disuse_shared = n_disuse_diverted = 0;
        n_unaliased_period = n_unalias_kept = 0;
        rt_log("perf: frame %llu: texture re-uploads %llu (%.1f MiB, %.2f ms), after WriteTexels %llu (%.1f MiB, %.2f ms); "
               "skipped with unchanged bytes %llu (%.1f MiB)",
               (unsigned long long)f * 60, (unsigned long long)ru_n, ru_bytes / 1048576.0, ru_ns / 1e6,
               (unsigned long long)ru_wt_n, ru_wt_bytes / 1048576.0, ru_wt_ns / 1e6, (unsigned long long)ru_same_n,
               ru_same_bytes / 1048576.0);
        ru_n = ru_bytes = ru_ns = ru_wt_n = ru_wt_bytes = ru_wt_ns = ru_same_n = ru_same_bytes = 0;
        uint64_t ck, calls, untr;
        vk_mem_snap_stats(&ck, &calls, &untr);
        rt_log("perf: frame %llu: binding storage checks %llu (texture %llu), write-watch snapshot queries %llu, untracked %llu",
               (unsigned long long)f * 60, (unsigned long long)ck, (unsigned long long)n_checks, (unsigned long long)calls,
               (unsigned long long)untr);
        n_checks = 0;
        rt_log("perf: frame %llu: released textures kept %d (%.0f MiB of own memory), dropped past the size limit %llu, "
               "under VRAM pressure %llu, under commit pressure %llu (made again soon after %llu, checks pausing those drops %llu; "
               "%llu checks went on to live textures with the released ones still above their bound); idle live textures evicted %llu (%.1f MiB; "
               "%llu walks found none, %.2f ms); unsampled ones evicted %llu (%.1f MiB), retiring %d (%.1f MiB), freed %llu, taken back while retiring "
               "%llu, back %llu, held %u (marks read %u); images reused %llu, made %llu (released ones of their "
               "parameters passed by: %llu released too lately, %llu drawn with or sampled lately, %llu named by a "
               "slot), pool bindings not tried %llu, "
               "superseded taken back %llu; render targets overwritten at another address %llu, dropped unused %llu "
               "(%llu of them early, with the card past its pressure share)",
               (unsigned long long)f * 60, nreleased, rel_bytes / 1048576.0, (unsigned long long)n_cap_drops,
               (unsigned long long)n_pressure_drops, (unsigned long long)n_commit_drops,
               (unsigned long long)n_commit_back, (unsigned long long)n_commit_held,
               (unsigned long long)n_commit_past, (unsigned long long)n_evict, n_evict_bytes / 1048576.0,
               (unsigned long long)n_evict_empty, ns_evict_empty / 1e6,
               (unsigned long long)n_disuse, n_disuse_bytes / 1048576.0, nretiring, retiring_bytes / 1048576.0,
               (unsigned long long)n_retire_freed, (unsigned long long)n_retire_cancel,
               (unsigned long long)n_disuse_back, holds.n, marks_reads, (unsigned long long)n_reused,
               (unsigned long long)n_reuse_new, (unsigned long long)n_reuse_young, (unsigned long long)n_reuse_used,
               (unsigned long long)n_reuse_slot, (unsigned long long)n_bind_skips, (unsigned long long)n_restarts,
               (unsigned long long)n_overwritten, (unsigned long long)n_over_drops, (unsigned long long)n_over_full);
        n_cap_drops = n_pressure_drops = n_commit_drops = n_commit_past = n_evict = n_evict_bytes = 0;
        n_evict_empty = ns_evict_empty = 0;
        n_commit_back = n_commit_held = 0;
        n_disuse = n_disuse_bytes = n_disuse_back = n_retire_freed = n_retire_cancel = 0;
        n_reused = n_reuse_new = n_bind_skips = n_restarts = n_overwritten = n_over_drops = 0;
        n_reuse_young = n_reuse_used = n_reuse_slot = n_over_full = 0;
        rt_log("perf: frame %llu: texture origins: released entries dropped past RELEASED_MAX %llu, past RELEASED_HARD %llu "
               "(overwritten %llu, intact past their gap %llu, intact %llu, made by the GPU %llu, all sampled lately %llu), "
               "replaced by another "
               "descriptor at their address %llu; entries the CPU does not write made without content at an address it "
               "can see: on their own %llu, over a released entry %llu, over a live one %llu; marked slots that met an "
               "entry without content %llu, that show no entry %llu; GPU-drawn entries written back before their drop %llu"
               "; hard-count batches %llu, %.1f ms in all, the longest %.2f ms",
               (unsigned long long)f * 60, (unsigned long long)n_drop_by[DROP_MAX], (unsigned long long)n_drop_by[DROP_HARD],
               (unsigned long long)n_hard_by[TRL_HC_OVER], (unsigned long long)n_hard_by[TRL_HC_INTACT_OLD],
               (unsigned long long)n_hard_by[TRL_HC_INTACT], (unsigned long long)n_hard_by[TRL_HC_GPU],
               (unsigned long long)n_hard_by[TRL_HC_LATELY], (unsigned long long)n_drop_by[DROP_REPLACED],
               (unsigned long long)n_cpu_empty[0], (unsigned long long)n_cpu_empty[1], (unsigned long long)n_cpu_empty[2],
               (unsigned long long)n_mark_empty, (unsigned long long)n_mark_none, (unsigned long long)n_drop_wb,
               (unsigned long long)n_hard_calls, ns_hard / 1e6, ns_hard_max / 1e6);
        memset(n_drop_by, 0, sizeof n_drop_by), memset(n_cpu_empty, 0, sizeof n_cpu_empty), memset(n_hard_by, 0, sizeof n_hard_by);
        n_mark_empty = n_mark_none = n_drop_wb = 0;
        n_hard_calls = ns_hard = ns_hard_max = 0;
        rt_log("perf: frame %llu: texture finalizes: slots marked %llu; slots of another live texture with the same "
               "parameters at the storage, left current %llu (terrain heightmaps %llu); lookups the stale rule gave the "
               "stand-in %llu (terrain heightmaps %llu), their own kept image %llu (NX2SARIA_TEX_STALE_KEPT)",
               (unsigned long long)f * 60, (unsigned long long)n_fin_own, (unsigned long long)n_fin_other, (unsigned long long)n_fin_other_hm,
               (unsigned long long)n_stale, (unsigned long long)n_stale_hm, (unsigned long long)n_stale_kept);
        n_fin_own = n_fin_other = n_fin_other_hm = n_stale = n_stale_hm = n_stale_kept = 0;
        if (split_mode > 0)
            rt_log("perf: frame %llu: split fill: entries the CPU does not write started with their storage's bytes: "
                   "copied from the released entry they replaced %llu, filled from storage %llu (%.1f MiB, %.2f ms), "
                   "filled again after a write %llu; left empty: image taken from a released entry %llu, storage over "
                   "%llu MiB %llu", (unsigned long long)f * 60, (unsigned long long)n_split_copy,
                   (unsigned long long)n_split_fill, n_split_bytes / 1048576.0, n_split_ns / 1e6,
                   (unsigned long long)n_split_refill, (unsigned long long)n_split_reused,
                   (unsigned long long)(SPLIT_FILL_MAX >> 20), (unsigned long long)n_split_big);
        n_split_copy = n_split_fill = n_split_bytes = n_split_ns = n_split_refill = n_split_reused = n_split_big = 0;
    }
    unlock();
}

typedef struct { unsigned level, layer0; int nz, is3d, bw, bh, bpb; size_t wb, hb, row, img, x0, y0; } Region;

static int region_of(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img, Region *g) {
    g->bpb = nvn_fmt_bpb(t->d.format, &g->bw, &g->bh);
    if (!g->bpb || r->width <= 0 || r->height <= 0 || r->depth < 0 || r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 ||
        r->xoffset % g->bw || r->yoffset % g->bh)
        return 0;
    g->level = v && v->set_levels ? v->base_level : 0;
    g->layer0 = v && v->set_layers ? v->min_layer : 0;
    g->nz = r->depth ? r->depth : 1;
    g->is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    g->wb = (size_t)((r->width + g->bw - 1) / g->bw) * (size_t)g->bpb;
    g->hb = (size_t)(r->height + g->bh - 1) / (size_t)g->bh;
    g->row = row ? row : g->wb;
    g->img = img ? img : g->row * g->hb;
    g->x0 = (size_t)(r->xoffset / g->bw) * (size_t)g->bpb;
    g->y0 = (size_t)(r->yoffset / g->bh);
    return 1;
}

static int storage_texels(const tex_t *t, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    for (int z = 0; z < g->nz; z++) {
        unsigned layer = g->is3d ? 0 : g->layer0 + (unsigned)r->zoffset + (unsigned)z;
        uint32_t slice = g->is3d ? (uint32_t)(r->zoffset + z) : 0;
        if (nvn_texels_box(&t->d, g->level, layer, (uint32_t)g->x0, (uint32_t)g->y0, slice, (uint32_t)g->wb,
                           (uint32_t)g->hb, t->mem, mem + (size_t)z * g->img, g->row, !write))
            return 0;
    }
    return 1;
}

static int read_aspect(const Tex *e, VkImageAspectFlags *a) {
    if (!e->f.depth && !e->f.stencil) { *a = VK_IMAGE_ASPECT_COLOR_BIT; return 1; }
    switch (e->t.d.format) {
    case 0x31: *a = VK_IMAGE_ASPECT_STENCIL_BIT; return 1;
    case 0x32: case 0x34: *a = VK_IMAGE_ASPECT_DEPTH_BIT; return 1;
    default: return 0;
    }
}

typedef struct {
    VkBuffer sb;
    VmaAllocation sa;
    const uint8_t *stg;
    size_t slice, ib;
    uint32_t w;
} TexRead;

static int texels_read_record(Tex *e, const Region *g, const NVNcopyRegion *r, TexRead *t) {
    static volatile int32_t n_astc, n_ds;
    VkImageAspectFlags aspect;
    if (e->f.astc_bw) {
        log_limited(&n_astc, "ReadTexels of ASTC texture %#llx: the image holds decoded RGBA8; zeros returned",
                    (unsigned long long)e->pub.addr);
        return 0;
    }
    if (!read_aspect(e, &aspect)) {
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    if (g->level < e->pub.skip || g->level - e->pub.skip >= e->pub.levels) return 0;
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height, il = g->level - e->pub.skip;
    t->w = w;
    t->ib = e->f.expand_rgb ? (size_t)g->bpb / 3 * 4 : 0;
    t->slice = t->ib ? (size_t)w * h * t->ib : g->wb * g->hb;
    t->stg = vk_staging(t->slice * (size_t)g->nz, &t->sb, &t->sa);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for a texture read");
    for (int z = 0; z < g->nz; z++)
        rg[z] = (VkBufferImageCopy){.bufferOffset = t->slice * (size_t)z,
                                    .imageSubresource = {aspect, il, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_begin();
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, t->sb, (uint32_t)g->nz, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    free(rg);
    return 1;
}

static void texels_read_take(const Tex *e, const Region *g, const TexRead *t, uint8_t *mem) {
    VkCtx *c = vk_ctx();
    vmaInvalidateAllocation(c->vma, t->sa, 0, VK_WHOLE_SIZE);
    for (int z = 0; z < g->nz; z++) {
        const uint8_t *s = t->stg + t->slice * (size_t)z;
        uint8_t *m = mem + (size_t)z * g->img;
        for (size_t y = 0; y < g->hb; y++) {
            if (t->ib) shrink_rgb(e->t.d.format, s + y * t->w * t->ib, m + y * g->row, t->w);
            else memcpy(m + y * g->row, s + y * g->wb, g->wb);
        }
    }
    vmaDestroyBuffer(c->vma, t->sb, t->sa);
}

static int image_texels(Tex *e, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    static volatile int32_t n_astc, n_ds;
    VkCtx *c = vk_ctx();
    VkImageAspectFlags aspect;
    if (!write) {
        TexRead t;
        if (!texels_read_record(e, g, r, &t)) return 0;
        vk_upload_flush(1);
        texels_read_take(e, g, &t, mem);
        return 1;
    }
    if (!read_aspect(e, &aspect)) {
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    if (g->level < e->pub.skip) return 1;
    uint32_t il = g->level - e->pub.skip;
    if (il >= e->pub.levels) return 0;
    e->had |= HAD_TEXELS, e->sfill = 0;
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height;
    int rgb = e->f.expand_rgb, astc = e->f.astc_bw != 0, agpu = astc && vk_astc_gpu();
    size_t texels = (size_t)w * h, ib = agpu ? 0 : astc ? 4 : rgb ? (size_t)g->bpb / 3 * 4 : 0;
    size_t slice = ib ? texels * ib : g->wb * g->hb;
    VkBuffer sb;
    VmaAllocation sa;
    uint8_t *stg = vk_staging(slice * (size_t)g->nz, &sb, &sa);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    for (int z = 0; z < g->nz; z++) {
        uint8_t *s = stg + slice * (size_t)z;
        const uint8_t *m = mem + (size_t)z * g->img;
        if (agpu) {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        } else if (astc) {
            uint8_t *tight = tmp_alloc(g->wb * g->hb);
            for (size_t y = 0; y < g->hb; y++) memcpy(tight + y * g->wb, m + y * g->row, g->wb);
            uint64_t a0 = perf_begin();
            astc_decode_ldr_mt(tight, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, s);
            perf_end(PERF_ASTC, a0);
            tmp_free(tight, g->wb * g->hb);
        } else if (rgb) {
            for (size_t y = 0; y < g->hb; y++) expand_rgb(e->t.d.format, m + y * g->row, s + y * w * ib, w);
        } else {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        }
        rg[z] = (VkBufferImageCopy){.bufferOffset = slice * (size_t)z,
                                    .imageSubresource = {aspect, il, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    }
    VkCommandBuffer cb = vk_upload_begin();
    vmaFlushAllocation(c->vma, sa, 0, VK_WHOLE_SIZE);
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int done = 0;
    uint32_t mw = e->t.d.width >> g->level, mh = e->t.d.height >> g->level;
    int bc_cut = e->f.astc_bc && (((w & 3) && (uint32_t)r->xoffset + w != (mw ? mw : 1)) ||
                                  ((h & 3) && (uint32_t)r->yoffset + h != (mh ? mh : 1)));
    if (agpu && !bc_cut) {
        VkAstcRegion *ar = calloc((size_t)g->nz, sizeof *ar);
        for (int z = 0; z < g->nz; z++)
            ar[z] = (VkAstcRegion){.src = slice * (size_t)z, .w = w, .h = h, .slices = 1, .level = rg[z].imageSubresource.mipLevel,
                                   .layer = rg[z].imageSubresource.baseArrayLayer, .offset = rg[z].imageOffset};
        uint64_t a0 = perf_begin();
        done = vk_astc_record(cb, sb, slice * (size_t)g->nz, ar, g->nz, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->f.astc_bc,
                              e->pub.image, aspect, VK_NULL_HANDLE);
        perf_end(PERF_ASTC, a0);
        free(ar);
    }
    if (!agpu) vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)g->nz, rg);
    else if (done <= 0) log_limited(&n_astc, "WriteTexels of ASTC texture %#llx not decoded on the GPU: left as it was",
                                (unsigned long long)e->pub.addr);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    vk_upload_flush(1);
    free(rg);
    vmaDestroyBuffer(c->vma, sb, sa);
    return 1;
}

static void zero_rows(const Region *g, uint8_t *dst) {
    for (int z = 0; z < g->nz; z++)
        for (size_t y = 0; y < g->hb; y++) memset(dst + (size_t)z * g->img + y * g->row, 0, g->wb);
}

int vk_texture_read_image(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    Region g;
    if (!t || !region_of(t, v, r, row, img, &g)) return 0;
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
    return ok;
}

void vk_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g)) {
        log_limited(&n_bad, "ReadTexels: invalid region %d,%d,%d %dx%dx%d (format %#x)", r->xoffset, r->yoffset,
                    r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e && e->pub.mirrored && !e->pub.gpu_owned) ok = storage_texels(t, &g, r, dst, 0);
    else if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
}

void vk_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g) || (v && v->set_format && v->format != t->d.format)) {
        log_limited(&n_bad, "WriteTexels: invalid region %d,%d,%d %dx%dx%d or view format (format %#x)", r->xoffset,
                    r->yoffset, r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    if (e && e->pub.mirrored) {
        if (!storage_texels(t, &g, r, (uint8_t *)src, 1))
            log_limited(&n_bad, "WriteTexels: region outside level %u of texture %#llx", g.level, (unsigned long long)e->pub.addr);
        vk_guest_written((uintptr_t)t->mem, t->L.size);
        if (e->pub.gpu_owned) image_texels(e, &g, r, (uint8_t *)src, 1);
        else if (e->uploaded) e->wt_n++;
    } else if (e) {
        image_texels(e, &g, r, (uint8_t *)src, 1);
    }
    unlock();
}

typedef struct {
    Tex *e;
    Region g;
    NVNcopyRegion r;
    TexRead t;
} WbPart;

static void write_back_n(Tex **v, int n) {
    int np = 0, cap = 0;
    WbPart *p = NULL;
    for (int i = 0; i < n; i++) {
        Tex *e = v[i];
        const nvn_texdesc *d = &e->t.d;
        int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D;
        uint32_t levels = linear ? 1 : e->pub.levels + e->pub.skip, layers = linear || is3d ? 1 : e->pub.layers;
        for (uint32_t lv = linear ? 0 : e->pub.skip; lv < levels; lv++)
            for (uint32_t ly = 0; ly < layers; ly++) {
                if (np == cap) {
                    cap = cap ? cap * 2 : 16;
                    if (!(p = realloc(p, sizeof *p * (size_t)cap))) rt_fatal("vk: out of memory for a texture write-back");
                }
                WbPart *q = &p[np];
                view_t vw = {.set_levels = 1, .set_layers = 1, .base_level = (uint8_t)lv, .min_layer = (uint16_t)ly};
                q->e = e;
                q->r = (NVNcopyRegion){.width = (int)mip(d->width, lv),
                                       .height = d->target == 0 || d->target == 3 ? 1 : (int)mip(d->height, lv),
                                       .depth = is3d ? (int)mip(d->depth, lv) : 1};
                if (region_of(&e->t, &vw, &q->r, 0, 0, &q->g) && texels_read_record(e, &q->g, &q->r, &q->t)) np++;
            }
    }
    if (np) vk_upload_flush(1);
    for (int i = 0; i < np; i++) {
        WbPart *q = &p[i];
        uint8_t *buf = tmp_alloc(q->g.img * (size_t)q->g.nz);
        if (!buf) rt_fatal("vk: out of memory for a texture write-back");
        texels_read_take(q->e, &q->g, &q->t, buf);
        storage_texels(&q->e->t, &q->g, &q->r, buf, 1);
        tmp_free(buf, q->g.img * (size_t)q->g.nz);
    }
    free(p);
    for (int i = 0; i < n; i++) v[i]->gpu_dirty = 0;
}

static int owned_dirty(uint64_t addr, uint64_t size, Tex **hit) {
    int n = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        Tex *e = owned[i];
        if (__atomic_load_n(&e->gpu_dirty, __ATOMIC_RELAXED) && e->pub.addr + e->t.L.size > addr) {
            if (!hit) return 1;
            hit[n++] = e;
        }
    }
    return n;
}

const VkTexImage *vk_texture_at_locked(const VkTexImage *img, VkFormat format) {
    for (Tex *p = nvn_hm_get(&by_addr, &((Tex *)img)->key.addr); p; p = p->next_at)
        if (!p->released && p->pub.format == format && p->pub.width == img->width && p->pub.height == img->height)
            return &p->pub;
    return NULL;
}

void vk_texture_storage_bound_locked(const VkTexImage *img) {
    Tex *e = (Tex *)img;
    uint64_t f = vk_exec_recording_fence();
    if (f > e->wr_fence) e->wr_fence = f;
}

uint64_t vk_texture_last_write(const tex_t *t) {
    lock();
    Tex *e = entry_of(t);
    uint64_t f = e ? e->wr_fence : 0;
    unlock();
    return f;
}

size_t vk_texture_texel_span(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img) {
    Region g;
    if (!region_of(t, v, r, row, img, &g)) return 0;
    return (size_t)(g.nz - 1) * g.img + (g.hb - 1) * g.row + g.wb;
}

int vk_texture_cpu_stale(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock_shared(&owned_lock);
    int dirty = owned_dirty(addr, size, NULL);
    plat_mutex_unlock_shared(&owned_lock);
    return dirty;
}

uint64_t vk_texture_dirty_fence(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    lock();
    uint64_t f = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        const Tex *e = owned[i];
        if (!e->gpu_dirty || e->pub.addr + e->t.L.size <= addr) continue;
        if (e->gpu_fence > f) f = e->gpu_fence;
        if (e->wr_fence > f) f = e->wr_fence;
    }
    unlock();
    return f;
}

void vk_texture_write_back(uint64_t addr, uint64_t size) {
    lock();
    Tex **hit = malloc(sizeof *hit * (size_t)(nowned + 1));
    if (!hit) rt_fatal("vk: out of memory for a texture write-back");
    int n = owned_dirty(addr, size, hit), k = 0;
    for (int i = 0; i < n; i++) {
        uint64_t now;
        if (storage_rewritten(hit[i], hit[i]->gpu_gen, &now) == 1) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    write_back_n(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
}

int vk_texture_small_dirty(void) { return __atomic_load_n(&small_pending, __ATOMIC_ACQUIRE); }

static void wb_note(const Tex *e, int skipped) {
    static uint64_t seen[2][64];
    static int n_seen[2];
    for (int i = 0; i < n_seen[skipped]; i++)
        if (seen[skipped][i] == e->pub.addr) return;
    if (n_seen[skipped] == 64) return;
    seen[skipped][n_seen[skipped]++] = e->pub.addr;
    const pool_t *p = e->t.pool;
    unsigned fl = p ? p->flags : 0;
    rt_log("vk: fence write-back %s %#llx %ux%u fmt %#x size %llu pool %s (limit %llu)",
           skipped ? "skipped-drawn-every-frame" : "over-default", (unsigned long long)e->pub.addr, e->t.d.width, e->t.d.height,
           e->t.d.format, (unsigned long long)e->t.L.size,
           !p ? "none" : (fl & NVN_MEMORY_POOL_FLAGS_CPU_CACHED) ? "cpu-cached" : (fl & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) ? "cpu-uncached" : "other",
           (unsigned long long)wb_limit);
}

void vk_texture_write_back_small(uint64_t fence) {
    lock();
    wb_cfg_init();
    __atomic_store_n(&small_pending, 0, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&owned_lock);
    int n = 0, later = 0;
    for (int i = 0; i < nowned; i++) n += owned[i]->gpu_dirty && wb_wanted(owned[i]);
    Tex **hit = n ? malloc(sizeof *hit * (size_t)n) : NULL;
    if (n && !hit) rt_fatal("vk: out of memory for a texture write-back");
    n = 0;
    for (int i = 0; i < nowned; i++) {
        Tex *e = owned[i];
        if (!e->gpu_dirty) continue;
        if (!wb_wanted(e)) {
            if (wb_trace && e->gpu_fence <= fence) {
                wb_note(e, 1);
                perf_count(PERF_CNT_WB_SKIP);
            }
            continue;
        }
        if (e->gpu_fence > fence) later = 1;
        else {
            if (wb_trace && e->t.L.size > WB_SMALL_DEFAULT) wb_note(e, 0);
            hit[n++] = e;
        }
    }
    plat_mutex_unlock_shared(&owned_lock);
    if (later) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint64_t now;
        if (storage_rewritten(hit[i], hit[i]->gpu_gen, &now) == 1) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    write_back_n(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
}

static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static struct { int init, mirror_clamp_edge, minmax, border_no_format; float aniso; } sf;

static float aniso_setting(void) {
    const char *an = cfg_value("NX2SARIA_ANISO");
    return strcmp(an, "game") ? (float)atoi(an) : 0;
}

static void aniso_log(void) {
    if (sf.aniso > 1) rt_log("vk: anisotropic filtering: %gx", sf.aniso);
    else rt_log("vk: anisotropic filtering: the game's");
}

static void sampler_features(VkCtx *c) {
    if (sf.init) return;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = c->has_custom_border_color ? &cbf : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    sf.mirror_clamp_edge = f12.samplerMirrorClampToEdge;
    sf.minmax = f12.samplerFilterMinmax;
    sf.border_no_format = c->has_custom_border_color && cbf.customBorderColors && cbf.customBorderColorWithoutFormat;
    sf.aniso = aniso_setting();
    aniso_log();
    sf.init = 1;
}

static int aniso_raised(const samp_par *d) {
    int tiled = (d->ws == NVN_WRAP_MODE_REPEAT || d->ws == NVN_WRAP_MODE_MIRRORED_REPEAT) &&
                (d->wt == NVN_WRAP_MODE_REPEAT || d->wt == NVN_WRAP_MODE_MIRRORED_REPEAT);
    return sf.aniso > d->aniso && tiled && !d->cmp_mode && d->magf == NVN_MAG_FILTER_LINEAR &&
           (d->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || d->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR);
}

static VkSampler make_sampler(VkCtx *c, const samp_par *d) {
    sampler_features(c);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->minf;
    si.minFilter = mf == 1 || mf == 3 || mf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    const uint8_t wrap[3] = {d->ws, d->wt, d->wr};
    int border = 0;
    for (int i = 0; i < 3; i++) {
        switch (wrap[i]) {
        case NVN_WRAP_MODE_CLAMP: case NVN_WRAP_MODE_CLAMP_TO_EDGE: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:
            modes[i] = sf.mirror_clamp_edge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: case NVN_WRAP_MODE_CLAMP_TO_BORDER:
            modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            border = 1;
            break;
        case NVN_WRAP_MODE_MIRRORED_REPEAT: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0], si.addressModeV = modes[1], si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= NVN_MIN_FILTER_LINEAR) si.maxLod = si.minLod + 0.25f;
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    float lim = c->props.limits.maxSamplerLodBias;
    si.mipLodBias = fmaxf(-lim, fminf(lim, d->lod_bias));
    if (d->aniso > 1) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = fminf(d->aniso, c->props.limits.maxSamplerAnisotropy);
    }
    if (d->cmp_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = compare_op(d->cmp_func);
        compare_sampler_note(d);
        if (shadow_scale() > 1 || shadow_pcf()) si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo rm = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    const void **tail = &si.pNext;
    if (border) {
        const float *b = d->border;
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (sf.border_no_format) {
            memcpy(cb.customBorderColor.float32, b, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            *tail = &cb;
            tail = &cb.pNext;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    if (d->reduction && sf.minmax && !si.compareEnable) {
        rm.reductionMode = d->reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        *tail = &rm;
    }
    VkSampler s = VK_NULL_HANDLE;
    VkResult r = vkCreateSampler(c->device, &si, NULL, &s);
    if (r == VK_SUCCESS) return s;
    VkResult r2 = r;
    if (si.pNext || si.anisotropyEnable) {
        VkSamplerCreateInfo plain = si;
        plain.pNext = NULL;
        plain.anisotropyEnable = VK_FALSE, plain.maxAnisotropy = 0;
        if (plain.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) plain.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        s = VK_NULL_HANDLE;
        r2 = vkCreateSampler(c->device, &plain, NULL, &s);
    }
    static volatile int32_t n_failed;
    uint32_t k = (uint32_t)plat_inc32(&n_failed);
    if (gfx_fail_log_due(k))
        rt_log("gfx: vkCreateSampler failed (%d), %u times: %s", (int)r, k,
               r2 == VK_SUCCESS ? "made without its custom border colour, reduction and anisotropy"
                                : "the default sampler in its place, made again in 250 ms");
    return r2 == VK_SUCCESS ? s : VK_NULL_HANDLE;
}

void vk_sampler_register(int id, const samp_par *s) {
    if (id < 0 || id >= (1 << 12)) rt_fatal("vk: sampler slot %d out of range", id);
    lock();
    if (id >= nsslots) {
        int n = nsslots ? nsslots : 256;
        while (n <= id) n *= 2;
        SampSlot *p = realloc(sslots, sizeof *p * (size_t)n);
        if (!p) rt_fatal("vk: out of memory for sampler slots");
        memset(p + nsslots, 0, sizeof *p * (size_t)(n - nsslots));
        sslots = p, nsslots = n;
    }
    sslots[id].p = *s;
    sslots[id].s = sslots[id].sp = sslots[id].sc = sslots[id].sd[0] = sslots[id].sd[1] = VK_NULL_HANDLE;
    sslots[id].used = 1;
    __atomic_add_fetch(&samp_gen, 1, __ATOMIC_RELAXED);
    unlock();
}

static float dlss_bias;

static int dlss_biased(const samp_par *k) {
    int rep_s = k->ws == NVN_WRAP_MODE_REPEAT || k->ws == NVN_WRAP_MODE_MIRRORED_REPEAT;
    int rep_t = k->wt == NVN_WRAP_MODE_REPEAT || k->wt == NVN_WRAP_MODE_MIRRORED_REPEAT;
    return !k->cmp_mode && k->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR && (rep_s || rep_t);
}

void vk_sampler_dlss_bias(float b) {
    lock();
    if (b != dlss_bias) {
        rt_log("vk: texture LOD bias %.2f on the scene's tiled textures (the upscaler's render scale; was %.2f)", b,
               dlss_bias);
        dlss_bias = b;
        for (int i = 0; i < nsslots; i++)
            sslots[i].s = sslots[i].sp = sslots[i].sc = sslots[i].sd[0] = sslots[i].sd[1] = VK_NULL_HANDLE;
        __atomic_add_fetch(&samp_gen, 1, __ATOMIC_RELAXED);
    }
    unlock();
}

void vk_sampler_aniso_live(void) {
    static unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v == seen) return;
    seen = v;
    lock();
    float a = aniso_setting();
    if (sf.init && a != sf.aniso) {
        sf.aniso = a;
        aniso_log();
        for (int i = 0; i < nsslots; i++)
            sslots[i].s = sslots[i].sp = sslots[i].sc = sslots[i].sd[0] = sslots[i].sd[1] = VK_NULL_HANDLE;
        __atomic_add_fetch(&samp_gen, 1, __ATOMIC_RELAXED);
    }
    unlock();
}

static VkSampler sampler_make_for(VkCtx *c, const samp_par *s, int gated) {
    sampler_features(c);
    samp_par k;
    memset(&k, 0, sizeof k);
    k = *s;
    if (dlss_bias != 0 && dlss_biased(&k)) k.lod_bias += dlss_bias;
    if (aniso_raised(&k)) k.aniso = sf.aniso;
    VkSampler v = (VkSampler)nvn_hm_get(&samplers, &k);
    if (!v) {
        uint64_t at = __atomic_load_n(&samp_retry_at, __ATOMIC_RELAXED);
        if (gated && at && plat_now_ns() < at) return VK_NULL_HANDLE;
        v = make_sampler(c, &k);
        if (!v) {
            __atomic_store_n(&samp_retry_at, plat_now_ns() + SAMP_RETRY_NS, __ATOMIC_RELAXED);
            return VK_NULL_HANDLE;
        }
        nvn_hm_put(&samplers, &k, (void *)v);
    }
    return v;
}

static VkSampler sampler_for(VkCtx *c, const samp_par *s) { return sampler_make_for(c, s, 1); }

VkSampler vk_sampler_for(const samp_par *s) {
    VkCtx *c = vk_ctx();
    if (!c || !s) return VK_NULL_HANDLE;
    lock();
    VkSampler v = sampler_make_for(c, s, 0);
    unlock();
    return v;
}

VkSampler vk_sampler_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c) return VK_NULL_HANDLE;
    VkSampler v = VK_NULL_HANDLE;
    if (id >= 0 && id < nsslots && sslots[id].used) {
        SampSlot *s = &sslots[id];
        if (!s->s) s->s = sampler_for(c, &s->p);
        v = s->s;
    }
    return v;
}

VkSampler vk_sampler_point_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    if (!s->sp) {
        samp_par k = s->p;
        k.magf = NVN_MAG_FILTER_NEAREST;
        k.minf = k.minf == NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST :
                 k.minf > NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST : k.minf;
        k.aniso = 1;
        s->sp = sampler_for(c, &k);
    }
    return s->sp;
}

VkSampler vk_sampler_compare_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    static int on = -1;
    if (on < 0) on = cfg_int("NX2SARIA_SHADOW_COMPARE") != 0;
    if (s->p.cmp_mode || !on) return vk_sampler_by_id_locked(id);
    if (!s->sc) {
        samp_par k = s->p;
        k.cmp_mode = 1;
        if (k.cmp_func < NVN_COMPARE_FUNC_NEVER || k.cmp_func > NVN_COMPARE_FUNC_ALWAYS) k.cmp_func = NVN_COMPARE_FUNC_LEQUAL;
        static int told;
        if (told++ < 8)
            rt_log("vk: a depth-compare read through sampler slot %d without a compare (function %u): compared with %u", id,
                   (unsigned)s->p.cmp_func, (unsigned)k.cmp_func);
        s->sc = sampler_for(c, &k);
    }
    return s->sc;
}

VkSampler vk_sampler_detail_by_id_locked(int id, uint32_t skip) {
    VkCtx *c = vk_ctx();
    if (!skip || skip > 2) return vk_sampler_by_id_locked(id);
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    if (!s->sd[skip - 1]) {
        samp_par k = s->p;
        if (k.lod_min > 0) k.lod_min = fmaxf(k.lod_min - (float)skip, 0);
        if (k.lod_max < 32) k.lod_max = fmaxf(k.lod_max - (float)skip, 0);
        if (k.lod_min == s->p.lod_min && k.lod_max == s->p.lod_max) {
            s->sd[skip - 1] = vk_sampler_by_id_locked(id);
        } else {
            static int told;
            if ((s->p.lod_min > 0 || s->p.lod_max < 8) && told++ < 8)
                rt_log("vk: texture detail: sampler slot %d clamps the LOD to %g..%g; %g..%g for a texture made without %u "
                       "levels", id, s->p.lod_min, s->p.lod_max, k.lod_min, k.lod_max, skip);
            s->sd[skip - 1] = sampler_for(c, &k);
        }
    }
    return s->sd[skip - 1];
}

VkSampler vk_sampler_by_id(int id) {
    lock();
    VkSampler v = vk_sampler_by_id_locked(id);
    unlock();
    return v;
}

int vk_sampler_params(int id, samp_par *out) {
    lock();
    int ok = id >= 0 && id < nsslots && sslots[id].used;
    if (ok) *out = sslots[id].p;
    unlock();
    return ok;
}

#define COMPACT_SPARSE_PCT 50
#define COMPACT_IDLE PRESENTS(600)
static int compact_held(uint64_t mem);

typedef struct {
    TcpBlock *b;
    int nb, cb;
    TcpAlloc *a;
    Tex **ae;
    int na, ca;
    nvn_hmap by_mem;
    uint32_t type;
    uint64_t tex_bytes, other_bytes;
    int marks;
} CompTable;

static int compact_marks(void) { return marks_tell(); }

static int compact_what(const Tex *e, uint64_t now, int marks, uint64_t most) {
    if (!e->pub.image || e->pub.gone || e->retiring || !comes_back_intact(e)) return TCP_STAY;
    if (e->released && now - e->rel_frame >= stale_after(e) && idle(e, now)) return TCP_DROP;
    if (!marks || !e->uploaded || e->up_retry || e->wr_fence || e->clobbered || e->pub.samples > 1 || e->f.depth ||
        e->f.stencil || e->bytes > most)
        return TCP_STAY;
    return now - e->samp_frame >= COMPACT_IDLE ? TCP_MOVE : TCP_STAY;
}

enum { KEEP_MIRROR = 1, KEEP_GPU = 2, KEEP_USED = 4, KEEP_OTHER = 8 };
static int stay_why(const Tex *e, int marks, uint64_t most) {
    if (!e) return KEEP_MIRROR;
    if (!e->pub.image || e->pub.gone || e->retiring) return KEEP_OTHER;
    if (!comes_back_intact(e)) return KEEP_GPU;
    if (!marks || !e->uploaded || e->up_retry || e->wr_fence || e->clobbered || e->pub.samples > 1 || e->f.depth ||
        e->f.stencil || e->bytes > most)
        return KEEP_OTHER;
    return KEEP_USED;
}

static void table_add(CompTable *t, uint64_t mem, uint64_t size, uint64_t bytes, int what, Tex *e) {
    void *v = nvn_hm_get(&t->by_mem, &mem);
    uint32_t bi = v ? (uint32_t)((uintptr_t)v - 1) : (uint32_t)t->nb;
    if (!v) {
        if (t->nb == t->cb) {
            t->cb = t->cb ? t->cb * 2 : 256;
            if (!(t->b = realloc(t->b, sizeof *t->b * (size_t)t->cb))) rt_fatal("vk: out of memory for the block table");
        }
        t->b[t->nb] = (TcpBlock){.id = mem, .size = size, .held = (uint8_t)compact_held(mem)};
        nvn_hm_put(&t->by_mem, &mem, (void *)(uintptr_t)(++t->nb));
    }
    if (t->na == t->ca) {
        t->ca = t->ca ? t->ca * 2 : 4096;
        t->a = realloc(t->a, sizeof *t->a * (size_t)t->ca), t->ae = realloc(t->ae, sizeof *t->ae * (size_t)t->ca);
        if (!t->a || !t->ae) rt_fatal("vk: out of memory for the block table");
    }
    t->a[t->na] = (TcpAlloc){.block = bi, .what = (uint8_t)what, .bytes = bytes};
    t->ae[t->na++] = e;
}

static void table_free(CompTable *t) {
    free(t->b), free(t->a), free(t->ae), free(t->by_mem.keys), free(t->by_mem.vals);
    memset(t, 0, sizeof *t);
}

static void compact_table(CompTable *t, const VkBlockRow *other, int nother, uint64_t most) {
    VkCtx *c = vk_ctx();
    memset(t, 0, sizeof *t);
    nvn_hm_init(&t->by_mem, 8);
    uint64_t by_type[VK_MAX_MEMORY_TYPES] = {0}, best = 0, now = frames_now();
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (e && e->bytes && !e->blk_own && e->blk_type < VK_MAX_MEMORY_TYPES) by_type[e->blk_type] += e->bytes;
    }
    for (uint32_t ty = 0; c && ty < c->mem_props.memoryTypeCount && ty < VK_MAX_MEMORY_TYPES; ty++)
        if ((c->mem_props.memoryTypes[ty].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && by_type[ty] > best)
            best = by_type[ty], t->type = ty;
    if (!best) return;
    t->marks = compact_marks();
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (!e || !e->bytes || e->blk_own || e->blk_type != t->type) continue;
        table_add(t, e->blk_mem, e->blk_size, e->bytes, compact_what(e, now, t->marks, most), e);
        t->tex_bytes += e->bytes;
    }
    for (int i = 0; i < nother; i++) {
        if (other[i].type != t->type) continue;
        table_add(t, other[i].mem, other[i].block_size, other[i].bytes, TCP_STAY, NULL);
        t->other_bytes += other[i].bytes;
    }
    tcp_tally(t->b, t->nb, t->a, t->na);
}

#define COMPACT_PERIOD PRESENTS(600)
#define COMPACT_WAIT_MAX PRESENTS(18000)
#define COMPACT_PASS_FRAMES PRESENTS(3600)
#define COMPACT_SETTLE PRESENTS(120)
#define COMPACT_HELD_FRAMES PRESENTS(36000)
#define COMPACT_ROOM_PCT 50
#define COMPACT_PASS_MAX ((uint64_t)1 << 30)
#define COMPACT_FRAME_N 4
#define COMPACT_WORK_NS 1000000
#define COMPACT_OLD_MAX ((uint64_t)64 << 20)
#define COMPACT_OLD_N 256
#define COMPACT_VICTIMS 32
#define COMPACT_HELD_N 64

typedef struct { VkImage img; VmaAllocation alloc; uint64_t bytes, takes, due, frame; int vram; } CompOld;
typedef struct { uint64_t mem, size; volatile int32_t freed; uint8_t gave_up; } CompVictim;
typedef struct { Tex *e; uint8_t v; } CompItem;

static struct {
    int on;
    uint64_t min_bytes, frame_bytes;
    volatile int32_t active;
    volatile uint64_t next_look;
    uint64_t wait;
    CompVictim v[COMPACT_VICTIMS];
    int nv;
    CompItem *item;
    int nitem, at;
    uint64_t frame0, done_frame;
    uint64_t plan_ns, step_ns;
    uint64_t holes0;
    uint64_t n_moved, moved_bytes, n_dropped, dropped_bytes, n_left;
    uint64_t frame, frame_n, frame_b;
    CompOld old[COMPACT_OLD_N];
    int nold;
    uint64_t old_bytes;
    struct { uint64_t mem, until; } held[COMPACT_HELD_N];
    unsigned held_next;
    volatile uint64_t last_ns, avg_ns;
    uint64_t passes, returned;
    int idle_logs, old_told;
} CP = {.on = -1};

static int compact_on(void) {
    if (CP.on < 0) {
        int mb = cfg_int("NX2SARIA_TEX_COMPACT_MIN_MB"), fmb = cfg_int("NX2SARIA_TEX_COMPACT_FRAME_MB");
        CP.min_bytes = (uint64_t)(mb < 64 ? 64 : mb > 65536 ? 65536 : mb) << 20;
        CP.frame_bytes = (uint64_t)(fmb < 1 ? 1 : fmb > 32 ? 32 : fmb) << 20;
        CP.on = cfg_bool("NX2SARIA_TEX_COMPACT") ? 1 : 0;
        if (CP.on)
            rt_log("vk: texture compaction on (NX2SARIA_TEX_COMPACT): idle textures move out of sparsely used blocks of "
                   "video memory once %llu MiB or more is free inside the blocks (NX2SARIA_TEX_COMPACT_MIN_MB), at most "
                   "%d textures and %llu MiB a frame (NX2SARIA_TEX_COMPACT_FRAME_MB)", (unsigned long long)(CP.min_bytes >> 20),
                   COMPACT_FRAME_N, (unsigned long long)(CP.frame_bytes >> 20));
    }
    return CP.on;
}

int vk_texture_compact_on(void) { return compact_on(); }

static void compact_frame_time(uint64_t d) {
    uint64_t avg = __atomic_load_n(&CP.avg_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&CP.last_ns, d, __ATOMIC_RELAXED);
    __atomic_store_n(&CP.avg_ns, avg ? avg + d / 16 - avg / 16 : d, __ATOMIC_RELAXED);
}

static int compact_slack(void) {
    if (vk_exec_pool_loading()) return 0;
    if (__atomic_load_n(&ts_ns[VK_TS_CREATE], __ATOMIC_RELAXED) + __atomic_load_n(&ts_ns[VK_TS_UPLOAD], __ATOMIC_RELAXED) >
        COMPACT_WORK_NS)
        return 0;
    uint64_t last = __atomic_load_n(&CP.last_ns, __ATOMIC_RELAXED), avg = __atomic_load_n(&CP.avg_ns, __ATOMIC_RELAXED);
    return last <= avg + avg / 4;
}

static int compact_held(uint64_t mem) {
    uint64_t now = frames_now();
    for (int i = 0; i < COMPACT_HELD_N; i++)
        if (CP.held[i].mem == mem && CP.held[i].until > now) return 1;
    return 0;
}

void vk_texture_block_freed(uint64_t mem) {
    if (!__atomic_load_n(&CP.active, __ATOMIC_ACQUIRE)) return;
    for (int i = 0; i < COMPACT_VICTIMS; i++)
        if (__atomic_load_n(&CP.v[i].mem, __ATOMIC_RELAXED) == mem) __atomic_store_n(&CP.v[i].freed, 1, __ATOMIC_RELEASE);
}

static int compact_victim(uint64_t mem) {
    for (int i = 0; i < CP.nv; i++)
        if (CP.v[i].mem == mem && !CP.v[i].gave_up) return 1;
    return 0;
}

static void compact_forget(Tex *e) {
    for (int i = CP.at; i < CP.nitem; i++)
        if (CP.item[i].e == e) CP.item[i].e = NULL;
    e->compacting = 0;
}

static void compact_old_sweep(void) {
    uint64_t takes = vk_pool_takes(), done = vk_sync_fence_done(), now = frames_now();
    int k = 0;
    for (int i = 0; i < CP.nold; i++) {
        CompOld *o = &CP.old[i];
        if (!o->due && takes - o->takes > VK_POOL_SETS) o->due = vk_sync_issued();
        if (!o->due && !CP.old_told && now - o->frame > COMPACT_PASS_FRAMES) {
            CP.old_told = 1;
            rt_log("vk: texture compaction: %d old images of moved textures (%.1f MiB) still wait for the texture "
                   "pool to take all its sets, %llu presents after the move (%llu takes since); the pass waits",
                   CP.nold, CP.old_bytes / 1048576.0, (unsigned long long)(now - o->frame),
                   (unsigned long long)(takes - o->takes));
        }
        if (o->due && done >= o->due) {
            vk_exec_image_gone(o->img);
            vk_defer_image(o->img, o->alloc);
            CP.old_bytes -= o->bytes;
            continue;
        }
        CP.old[k++] = *o;
    }
    CP.nold = k;
}

static void tex_move(VkCtx *c, Tex *e, VmaAllocation a, VkImage img);
static int compact_move(VkCtx *c, Tex *e) {
    const ImgKey *k = &e->ik;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = k->flags, .imageType = (VkImageType)k->type,
                            .format = (VkFormat)k->format, .extent = {k->w, k->h, k->d}, .mipLevels = k->levels,
                            .arrayLayers = k->layers, .samples = (VkSampleCountFlagBits)k->samples,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = k->usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(c->device, &ci, NULL, &img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c->device, img, &mr);
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT, .memoryTypeBits = 1u << e->blk_type};
    VmaAllocation a = NULL;
    VmaAllocationInfo at;
    int ok = (mr.memoryTypeBits >> e->blk_type & 1) && vmaAllocateMemory(c->vma, &mr, &ai, &a, &at) == VK_SUCCESS;
    if (ok && compact_victim((uint64_t)(uintptr_t)at.deviceMemory)) {
        vmaFreeMemory(c->vma, a);
        ok = 0;
    }
    if (ok && vmaBindImageMemory(c->vma, a, img) != VK_SUCCESS) {
        vmaFreeMemory(c->vma, a);
        ok = 0;
    }
    if (!ok) {
        vkDestroyImage(c->device, img, NULL);
        return 0;
    }
    CP.old[CP.nold++] = (CompOld){e->pub.image, e->alloc, e->bytes, vk_pool_takes(), 0, frames_now()};
    CP.old_bytes += e->bytes;
    tex_move(c, e, a, img);
    return 1;
}

static void tex_move(VkCtx *c, Tex *e, VmaAllocation a, VkImage img) {
    vk_exec_image_gone(img);
    VkCommandBuffer cb = vk_upload_begin();
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img,
                                .subresourceRange = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    VkImageCopy rg[16];
    for (uint32_t lv = 0; lv < e->pub.levels;) {
        uint32_t n = 0;
        for (; lv < e->pub.levels && n < 16; lv++, n++)
            rg[n] = (VkImageCopy){.srcSubresource = {e->pub.aspect, lv, 0, e->pub.layers},
                                  .dstSubresource = {e->pub.aspect, lv, 0, e->pub.layers},
                                  .extent = {mip(e->pub.width, lv), mip(e->pub.height, lv), mip(e->pub.depth, lv)}};
        vkCmdCopyImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    }
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    if (e->released) rel_bytes -= e->bytes;
    e->pub.image = img, e->alloc = a, e->reused = 0;
    own_memory(c, e);
    if (e->released) rel_bytes += e->bytes;
    tex_gen_bump(e->key.addr);
}

static void compact_finish(uint64_t now) {
    VkCtx *c = vk_ctx();
    uint64_t holes = 0, back = 0;
    int n_back = 0, n_gave = 0, n_kept = 0;
    for (int i = 0; i < CP.nv; i++) {
        CompVictim *v = &CP.v[i];
        if (__atomic_load_n(&v->freed, __ATOMIC_ACQUIRE)) {
            n_back++, back += v->size;
        } else if (v->gave_up) {
            n_gave++;
        } else {
            n_kept++;
            CP.held[CP.held_next++ % COMPACT_HELD_N] = (typeof(CP.held[0])){v->mem, now + COMPACT_HELD_FRAMES};
        }
    }
    if (c) {
        VmaBudget vb[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(c->vma, vb);
        for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
            if ((c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                vb[h].statistics.blockBytes > vb[h].statistics.allocationBytes)
                holes += vb[h].statistics.blockBytes - vb[h].statistics.allocationBytes;
    }
    CP.passes++, CP.returned += back;
    CP.wait = back ? 0 : CP.wait ? (CP.wait * 2 < COMPACT_WAIT_MAX ? CP.wait * 2 : COMPACT_WAIT_MAX) : 2 * COMPACT_PERIOD;
    uint64_t wait = CP.wait ? CP.wait : COMPACT_PERIOD;
    const double M = 1048576.0;
    rt_log("vk: texture compaction: pass %llu over %llu frames: %llu textures moved (%.1f MiB), %llu released ones dropped "
           "(%.1f MiB), %llu left where they were; %d of %d blocks emptied and given back to the driver, %.0f MiB; %d "
           "given up (a texture came into use or found no room elsewhere), %d emptied of textures and kept by the "
           "allocator (its one empty block, or memory of another kind); free inside the blocks %.0f MiB before, %.0f MiB "
           "now; %.2f ms on the submitting thread, %.2f ms for the plan; %.0f MiB given back by all passes so far; next "
           "look in %llu presents", (unsigned long long)CP.passes, (unsigned long long)(now - CP.frame0),
           (unsigned long long)CP.n_moved, CP.moved_bytes / M, (unsigned long long)CP.n_dropped, CP.dropped_bytes / M,
           (unsigned long long)CP.n_left, n_back, CP.nv, back / M, n_gave, n_kept, CP.holes0 / M, holes / M,
           CP.step_ns / 1e6, CP.plan_ns / 1e6, CP.returned / M, (unsigned long long)wait);
    free(CP.item);
    CP.item = NULL, CP.nitem = CP.at = 0;
    __atomic_store_n(&CP.next_look, now + wait, __ATOMIC_RELAXED);
    __atomic_store_n(&CP.active, 0, __ATOMIC_RELEASE);
}

static void compact_step(void) {
    if (CP.nold) compact_old_sweep();
    if (!CP.active) return;
    uint64_t t0 = plat_now_ns(), now = frames_now();
    if (now != CP.frame) CP.frame = now, CP.frame_n = 0, CP.frame_b = 0;
    if (CP.at < CP.nitem && now - CP.frame0 > COMPACT_PASS_FRAMES) {
        for (; CP.at < CP.nitem; CP.at++) {
            CompItem *it = &CP.item[CP.at];
            if (it->e) it->e->compacting = 0, CP.v[it->v].gave_up = 1, CP.n_left++;
        }
    }
    if (CP.at < CP.nitem && compact_slack()) {
        VkCtx *c = vk_ctx();
        int marks = compact_marks();
        while (c && CP.at < CP.nitem && CP.frame_n < COMPACT_FRAME_N && CP.frame_b < CP.frame_bytes &&
               CP.nold < COMPACT_OLD_N && CP.old_bytes < COMPACT_OLD_MAX) {
            CompItem it = CP.item[CP.at];
            Tex *e = it.e;
            if (e && CP.frame_b && CP.frame_b + e->bytes > CP.frame_bytes) break;
            CP.at++;
            if (!e) continue;
            e->compacting = 0;
            CompVictim *v = &CP.v[it.v];
            if (e->blk_mem != v->mem) continue;
            uint64_t bytes = e->bytes;
            int what = v->gave_up ? TCP_STAY : compact_what(e, now, marks, CP.frame_bytes);
            if (what == TCP_DROP) {
                drop_tex(e, DROP_COMPACT);
                CP.n_dropped++, CP.dropped_bytes += bytes, CP.frame_n++;
            } else if (what == TCP_MOVE && compact_move(c, e)) {
                CP.n_moved++, CP.moved_bytes += bytes, CP.frame_n++, CP.frame_b += bytes;
            } else {
                v->gave_up = 1, CP.n_left++;
            }
        }
    }
    CP.step_ns += plat_now_ns() - t0;
    if (CP.at < CP.nitem || CP.nold) return;
    if (!CP.done_frame) CP.done_frame = now;
    int waiting = 0;
    for (int i = 0; i < CP.nv; i++) waiting += !CP.v[i].gave_up && !__atomic_load_n(&CP.v[i].freed, __ATOMIC_ACQUIRE);
    if (!waiting || now - CP.done_frame >= COMPACT_SETTLE) compact_finish(now);
}

int vk_texture_compact_due(void) {
    return compact_on() && !__atomic_load_n(&CP.active, __ATOMIC_ACQUIRE) &&
           frames_now() >= __atomic_load_n(&CP.next_look, __ATOMIC_RELAXED);
}

void vk_texture_compact_plan(const VkBlockRow *other, int nother) {
    VkCtx *c = vk_ctx();
    if (!c || !compact_on()) return;
    uint64_t t0 = plat_now_ns();
    lock();
    uint64_t now = frames_now();
    if (CP.active || CP.nold || now < CP.next_look) {
        unlock();
        return;
    }
    __atomic_store_n(&CP.next_look, now + (CP.wait ? CP.wait : COMPACT_PERIOD), __ATOMIC_RELAXED);
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t holes = 0, used = 0, budget = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if ((c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            vb[h].statistics.blockBytes > vb[h].statistics.allocationBytes)
            holes += vb[h].statistics.blockBytes - vb[h].statistics.allocationBytes;
    int exact;
    int pressed = vram_pressure > 0 && vk_mem_vram(&used, &budget, &exact) && budget &&
                  used >= budget / 100 * (uint64_t)vram_pressure;
    uint64_t min = pressed ? vk_vma_block_size() : CP.min_bytes;
    if (holes < min) {
        unlock();
        return;
    }
    CompTable t;
    compact_table(&t, other, nother, CP.frame_bytes);
    uint32_t heap = c->mem_props.memoryTypes[t.type].heapIndex, order[COMPACT_VICTIMS];
    const VmaStatistics *hs = &vb[heap < VK_MAX_MEMORY_HEAPS ? heap : 0].statistics;
    TcpParams p = {.free_bytes = hs->blockBytes > hs->allocationBytes ? hs->blockBytes - hs->allocationBytes : 0,
                   .sparse_pct = COMPACT_SPARSE_PCT, .room_pct = COMPACT_ROOM_PCT, .min_gain = min,
                   .max_move = COMPACT_PASS_MAX, .max_blocks = COMPACT_VICTIMS};
    TcpPlan plan;
    int n = t.nb ? tcp_plan(t.b, t.nb, &p, order, COMPACT_VICTIMS, &plan) : 0;
    CompItem *item = n ? malloc(sizeof *item * (size_t)(plan.n_move + plan.n_drop + 1)) : NULL;
    if (!item) {
        TcpStats s;
        tcp_stats(t.b, t.nb, COMPACT_SPARSE_PCT, &s);
        CP.wait = CP.wait ? (CP.wait * 2 < COMPACT_WAIT_MAX ? CP.wait * 2 : COMPACT_WAIT_MAX) : 2 * COMPACT_PERIOD;
        __atomic_store_n(&CP.next_look, now + CP.wait, __ATOMIC_RELAXED);
        if (CP.idle_logs < 8) {
            CP.idle_logs++;
            const double M = 1048576.0;
            rt_log("vk: texture compaction: %.0f MiB free inside the blocks and no pass: %u of %u blocks with textures are "
                   "at most half full, %u of them (%.0f MiB) hold nothing but textures that could move or be dropped, "
                   "and a pass needs %.0f MiB of them with room elsewhere for what moves (%.0f MiB free in the memory "
                   "type's heap)%s; next look in %llu presents", holes / M, s.sparse, s.blocks, s.emptiable,
                   s.emptiable_bytes / M, min / M, p.free_bytes / M,
                   t.marks ? "" : "; the texture pool's marks are not in use, so no texture counts as unsampled",
                   (unsigned long long)CP.wait);
        }
        table_free(&t);
        unlock();
        return;
    }
    CP.nv = n, CP.nitem = 0;
    for (int j = 0; j < n; j++) {
        CP.v[j] = (CompVictim){.mem = t.b[order[j]].id, .size = t.b[order[j]].size};
        for (int i = 0; i < t.na; i++) {
            if (t.a[i].block != order[j] || !t.ae[i] || t.a[i].what == TCP_STAY) continue;
            item[CP.nitem++] = (CompItem){t.ae[i], (uint8_t)j};
            t.ae[i]->compacting = 1;
        }
    }
    for (int j = n; j < COMPACT_VICTIMS; j++) CP.v[j] = (CompVictim){0};
    table_free(&t);
    CP.item = item, CP.at = 0;
    CP.frame0 = now, CP.done_frame = 0, CP.holes0 = holes, CP.step_ns = 0;
    CP.n_moved = CP.moved_bytes = CP.n_dropped = CP.dropped_bytes = CP.n_left = 0, CP.old_told = 0;
    CP.plan_ns = plat_now_ns() - t0;
    __atomic_store_n(&CP.active, 1, __ATOMIC_RELEASE);
    unlock();
}

void vk_texture_holes(const VkBlockRow *other, int nother, VkTexHoles *h) {
    memset(h, 0, sizeof *h);
    lock();
    CompTable t;
    compact_on();
    compact_table(&t, other, nother, CP.frame_bytes);
    TcpStats s;
    tcp_stats(t.b, t.nb, COMPACT_SPARSE_PCT, &s);
    *h = (VkTexHoles){.type = t.type, .marks = t.marks, .blocks = s.blocks, .sparse = s.sparse, .emptiable = s.emptiable,
                      .block_bytes = s.block_bytes, .tex_bytes = t.tex_bytes, .other_bytes = t.other_bytes,
                      .move_bytes = s.move_bytes, .drop_bytes = s.drop_bytes, .sparse_bytes = s.sparse_bytes,
                      .sparse_listed = s.sparse_listed, .emptiable_bytes = s.emptiable_bytes,
                      .emptiable_move = s.emptiable_move, .emptiable_drop = s.emptiable_drop};
    uint8_t *why = t.nb ? calloc((size_t)t.nb, 1) : NULL;
    for (int i = 0; why && i < t.na; i++)
        if (t.a[i].what == TCP_STAY && t.a[i].block < (uint32_t)t.nb)
            why[t.a[i].block] |= (uint8_t)stay_why(t.ae[i], t.marks, CP.frame_bytes);
    for (int i = 0; why && i < t.nb; i++) {
        const TcpBlock *k = &t.b[i];
        if (!tcp_sparse(k, COMPACT_SPARSE_PCT) || tcp_emptiable(k, COMPACT_SPARSE_PCT) || !why[i]) continue;
        h->kept++, h->kept_bytes += k->size;
        for (int j = 0; j < VK_HOLES_KINDS; j++) {
            if (!(why[i] >> j & 1)) continue;
            h->kept_with[j]++;
            if (why[i] == 1u << j) h->kept_only[j]++, h->kept_only_bytes[j] += k->size;
        }
    }
    free(why);
    table_free(&t);
    unlock();
}

void vk_texture_stats(uint64_t *images, uint64_t *uploads, uint64_t *reuploads) {
    lock();
    if (images) *images = by_key.n;
    if (uploads) *uploads = n_uploads;
    if (reuploads) *reuploads = n_reuploads;
    unlock();
}

void vk_texture_census(VkTexCensus *s) {
    memset(s, 0, sizeof *s);
    lock();
    uint64_t now = frames_now();
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e) continue;
        uint64_t bytes = e->alloc ? e->bytes : 0;
        if (e->released) {
            s->released++, s->own_released += e->alloc != NULL, s->bytes_released += bytes;
            if (now - e->rel_frame < stale_after(e)) s->bytes_released_young += bytes;
        } else {
            s->live++, s->own_live += e->alloc != NULL, s->bytes_live += bytes;
            if (!comes_back_intact(e)) s->bytes_live_gpu += bytes;
            if (idle(e, now)) s->bytes_live_idle += bytes;
        }
    }
    unlock();
}

void vk_texture_host(VkTexHost *h) {
    memset(h, 0, sizeof *h);
    lock();
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e) continue;
        h->entries++;
        h->bytes += sizeof *e + (e->snap ? SNAP_MAX : 0);
    }
    h->bytes += (uint64_t)by_key.cap * (sizeof *by_key.vals + by_key.klen) + (uint64_t)ntslots * sizeof *tslots +
                (uint64_t)nsslots * sizeof *sslots;
    if (sinfo) h->bytes += (sizeof *sinfo + sizeof *slot_next + sizeof *slot_prev + sizeof *slot_grp + sizeof *slot_bucket) << 20;
    unlock();
    h->tmp = (uint64_t)__atomic_load_n(&tmp_now, __ATOMIC_RELAXED);
    h->tmp_peak = (uint64_t)__atomic_exchange_n(&tmp_peak, (int64_t)h->tmp, __ATOMIC_RELAXED);
}

enum { AGE_YOUNG, AGE_10S, AGE_1M, AGE_3M, AGE_5M, AGE_OLD, AGE_N };
static const char *const age_names[AGE_N] = {"under 2 s", "2-10 s", "10-60 s", "1-3 min", "3-5 min", "longer"};
static uint64_t rb_n[AGE_N], rb_bytes[AGE_N];
static uint64_t ra_n[AGE_N], ra_bytes, ra_why[DROP_N];
enum { RK_INTACT, RK_INTACT_BIG, RK_OVER, RK_OVER_BIG, RK_GPU, RK_GPU_BIG, RK_N };
static const char *const rk_names[RK_N] = {"intact under 1 MiB", "intact 1 MiB or more", "overwritten under 1 MiB",
                                           "overwritten 1 MiB or more", "made by the GPU under 1 MiB",
                                           "made by the GPU 1 MiB or more"};
static uint64_t rkb_n[RK_N][AGE_N], rkb_bytes[RK_N][AGE_N], rka_n[RK_N][AGE_N];
#define DROPPED_N 8192
static struct { uint64_t kh, frame; uint8_t why, kind; } dropped[DROPPED_N];

static int age_of(uint32_t s) {
    return s < 2 ? AGE_YOUNG : s < 10 ? AGE_10S : s < 60 ? AGE_1M : s < 180 ? AGE_3M : s < 300 ? AGE_5M : AGE_OLD;
}

static int reuse_kind(const Tex *e) {
    int big = e->alloc && e->bytes >= (1ull << 20);
    return (e->overwritten ? RK_OVER : comes_back_intact(e) ? RK_INTACT : RK_GPU) + big;
}

static void reuse_taken_back(const Tex *e) {
    int a = age_of(secs_since(e->rel_frame)), k = reuse_kind(e);
    uint64_t own = e->alloc ? e->bytes : 0;
    rb_n[a]++, rb_bytes[a] += own;
    rkb_n[k][a]++, rkb_bytes[k][a] += own;
}

static int drop_costs(int why) {
    return why == DROP_MAX || why == DROP_HARD || why == DROP_CAP || why == DROP_COMMIT || why == DROP_VRAM ||
           why == DROP_IDLE_VRAM || why == DROP_IDLE_COMMIT || why == DROP_DISUSE || why == DROP_AGE;
}

static void reuse_dropped(const Tex *e, int why) {
    if (!drop_costs(why)) return;
    uint64_t kh = key_hash(&e->key);
    dropped[kh % DROPPED_N].kh = kh, dropped[kh % DROPPED_N].frame = frames_now(), dropped[kh % DROPPED_N].why = (uint8_t)why;
    dropped[kh % DROPPED_N].kind = (uint8_t)reuse_kind(e);
}

static void reuse_made(const Tex *e) {
    uint64_t kh = key_hash(&e->key);
    if (dropped[kh % DROPPED_N].kh != kh || !dropped[kh % DROPPED_N].why) return;
    int a = age_of(secs_since(dropped[kh % DROPPED_N].frame));
    ra_n[a]++, ra_bytes += e->alloc ? e->bytes : 0, ra_why[dropped[kh % DROPPED_N].why]++;
    rka_n[dropped[kh % DROPPED_N].kind % RK_N][a]++;
    dropped[kh % DROPPED_N].why = 0;
}

void vk_texture_reuse_log(uint64_t frame) {
    char line[1536];
    const double M = 1048576.0;
    lock();
    int o = snprintf(line, sizeof line, "vk: released textures frame %llu, since the last line: taken back by the time "
                     "released:", (unsigned long long)frame);
    for (int a = 0; a < AGE_N; a++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %s %llu (%.0f MiB);", age_names[a], (unsigned long long)rb_n[a],
                      rb_bytes[a] / M);
    uint64_t again = 0;
    for (int a = 0; a < AGE_N; a++) again += ra_n[a];
    o += snprintf(line + o, sizeof line - (size_t)o, " made again after a drop %llu (%.0f MiB; by the time since the drop:",
                  (unsigned long long)again, ra_bytes / M);
    for (int a = 0; a < AGE_N; a++)
        o += snprintf(line + o, sizeof line - (size_t)o, "%s %s %llu", a ? "," : "", age_names[a], (unsigned long long)ra_n[a]);
    snprintf(line + o, sizeof line - (size_t)o, "; by the drop: released past the hold %llu, past the size bound "
             "%llu, past the count %llu (of them past the hard count %llu), VRAM pressure %llu released and %llu idle, "
             "commit %llu released and %llu idle, unsampled %llu); dropped past the hold %llu (%.0f MiB); now %d "
             "released, %.0f MiB of a bound of %.0f MiB, hold %u s",
             (unsigned long long)ra_why[DROP_AGE], (unsigned long long)ra_why[DROP_CAP],
             (unsigned long long)(ra_why[DROP_MAX] + ra_why[DROP_HARD]), (unsigned long long)ra_why[DROP_HARD],
             (unsigned long long)ra_why[DROP_VRAM],
             (unsigned long long)ra_why[DROP_IDLE_VRAM], (unsigned long long)ra_why[DROP_COMMIT],
             (unsigned long long)ra_why[DROP_IDLE_COMMIT], (unsigned long long)ra_why[DROP_DISUSE],
             (unsigned long long)n_age_drops, n_age_bytes / M, nreleased, rel_bytes / M,
             released_cap_bytes() == UINT64_MAX ? 0.0 : released_cap_bytes() / M, released_hold());
    char kline[1536];
    int ko = snprintf(kline, sizeof kline, "vk: released textures frame %llu by kind and own memory, since the last line "
                      "(ages under 2 s/2-10 s/10-60 s/1-3 min/3-5 min/longer):", (unsigned long long)frame);
    int kinds = 0;
    for (int k = 0; k < RK_N && ko < (int)sizeof kline - 200; k++) {
        uint64_t any = 0, late = 0;
        for (int a = 0; a < AGE_N; a++) any += rkb_n[k][a] + rka_n[k][a];
        for (int a = AGE_3M; a < AGE_N; a++) late += rkb_bytes[k][a];
        if (!any) continue;
        kinds++;
        ko += snprintf(kline + ko, sizeof kline - (size_t)ko, "%s %s: taken back %llu/%llu/%llu/%llu/%llu/%llu (%.0f MiB "
                       "after a minute), made again %llu/%llu/%llu/%llu/%llu/%llu", kinds > 1 ? ";" : "", rk_names[k],
                       (unsigned long long)rkb_n[k][0], (unsigned long long)rkb_n[k][1], (unsigned long long)rkb_n[k][2],
                       (unsigned long long)rkb_n[k][3], (unsigned long long)rkb_n[k][4], (unsigned long long)rkb_n[k][5],
                       late / M, (unsigned long long)rka_n[k][0], (unsigned long long)rka_n[k][1],
                       (unsigned long long)rka_n[k][2], (unsigned long long)rka_n[k][3], (unsigned long long)rka_n[k][4],
                       (unsigned long long)rka_n[k][5]);
    }
    memset(rb_n, 0, sizeof rb_n), memset(rb_bytes, 0, sizeof rb_bytes), memset(ra_n, 0, sizeof ra_n);
    memset(ra_why, 0, sizeof ra_why), ra_bytes = 0, n_age_drops = n_age_bytes = 0;
    memset(rkb_n, 0, sizeof rkb_n), memset(rkb_bytes, 0, sizeof rkb_bytes), memset(rka_n, 0, sizeof rka_n);
    unlock();
    rt_log("%s", line);
    if (kinds) rt_log("%s", kline);
}

void vk_texture_census_log(uint64_t frame) {
    enum { K_BC1, K_BC3, K_RGBA8, K_CPU, K_GPU, K_N };
    static const char *const kn[K_N] = {"ASTC as BC1", "ASTC as BC3", "ASTC as RGBA8", "other CPU-written", "GPU-written"};
    uint64_t n[K_N] = {0}, b[K_N] = {0}, in[K_N] = {0}, ib[K_N] = {0}, bound = 0, dup_n = 0, dup_b = 0;
    uint64_t sk_n[3] = {0}, sk_b[3] = {0};
    struct { int fmt; uint64_t n, b; } fm[48];
    int nfm = 0;
    struct { uint64_t b; uint32_t w, h, d, levels; int fmt, gpu; } big[4] = {{0}};
    struct { uint64_t addr, cnt, extra; struct { uint64_t b; uint32_t w, h, d; int fmt, gpu; } e[4]; } dup[3] = {{0}};
    const double M = 1048576.0;
    lock();
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e || e->released) continue;
        if (!e->bytes) { bound += e->bound_to != NULL; continue; }
        int k = !comes_back_intact(e) ? K_GPU
              : e->f.astc_bw ? (e->f.astc_bc == 2 ? K_BC1 : e->f.astc_bc ? K_BC3 : K_RGBA8) : K_CPU;
        n[k]++, b[k] += e->bytes;
        if (e->pub.skip < 3) sk_n[e->pub.skip]++, sk_b[e->pub.skip] += e->bytes;
        if (idle(e, now)) in[k]++, ib[k] += e->bytes;
        int j = 0;
        while (j < nfm && fm[j].fmt != (int)e->f.vk) j++;
        if (j == nfm && nfm < 48) fm[nfm++] = (typeof(fm[0])){(int)e->f.vk, 0, 0};
        if (j < nfm) fm[j].n++, fm[j].b += e->bytes;
        for (int q = 0; q < 4; q++)
            if (e->bytes > big[q].b) {
                memmove(big + q + 1, big + q, sizeof *big * (size_t)(3 - q));
                big[q] = (typeof(big[0])){e->bytes, e->pub.width, e->pub.height, e->pub.layers * e->pub.depth,
                                          e->pub.levels, (int)e->f.vk, k == K_GPU};
                break;
            }
    }
    for (uint32_t i = 0; i < by_addr.cap; i++) {
        uint64_t cnt = 0, sum = 0, most = 0;
        for (const Tex *p = by_addr.vals[i]; p; p = p->next_at)
            if (!p->released && p->bytes) cnt++, sum += p->bytes, most = p->bytes > most ? p->bytes : most;
        if (cnt > 1) dup_n += cnt - 1, dup_b += sum - most;
        for (int q = 0; cnt > 1 && q < 3; q++) {
            if (sum - most <= dup[q].extra) continue;
            memmove(dup + q + 1, dup + q, sizeof *dup * (size_t)(2 - q));
            memset(&dup[q], 0, sizeof dup[q]);
            dup[q].cnt = cnt, dup[q].extra = sum - most;
            for (const Tex *p = by_addr.vals[i]; p; p = p->next_at) {
                if (p->released || !p->bytes) continue;
                dup[q].addr = p->pub.addr;
                for (int x = 0; x < 4; x++)
                    if (p->bytes > dup[q].e[x].b) {
                        memmove(dup[q].e + x + 1, dup[q].e + x, sizeof dup[q].e[0] * (size_t)(3 - x));
                        dup[q].e[x].b = p->bytes, dup[q].e[x].w = p->pub.width, dup[q].e[x].h = p->pub.height;
                        dup[q].e[x].d = p->pub.layers * p->pub.depth, dup[q].e[x].fmt = (int)p->f.vk;
                        dup[q].e[x].gpu = !comes_back_intact(p);
                        break;
                    }
            }
            break;
        }
    }
    unlock();
    char line[1024];
    int o = snprintf(line, sizeof line, "mem: textures frame %llu: live with own memory by kind, idle part in brackets:",
                     (unsigned long long)frame);
    for (int k = 0; k < K_N; k++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %s %llu %.0f MiB (%llu %.0f MiB);", kn[k], (unsigned long long)n[k],
                      b[k] / M, (unsigned long long)in[k], ib[k] / M);
    o += snprintf(line + o, sizeof line - (size_t)o, " bound into pool memory %llu; sharing an address with another %llu %.0f MiB",
                  (unsigned long long)bound, (unsigned long long)dup_n, dup_b / M);
    if (detail_most())
        snprintf(line + o, sizeof line - (size_t)o, "; texture detail: made without their top level %llu %.0f MiB, without "
                 "two %llu %.0f MiB, made whole again for a shader %llu (%u shaders loaded read a texture by texel "
                 "position or size)", (unsigned long long)sk_n[1], sk_b[1] / M, (unsigned long long)sk_n[2], sk_b[2] / M,
                 (unsigned long long)n_exact, gfx_shader_exact_seen());
    rt_log("%s", line);
    if (vk_astc_bc1())
        rt_log("mem: ASTC as BC1 so far: %llu textures made BC1 at their first upload (%.0f MiB, half of what BC3 takes), "
               "%llu made BC3 again for content with alpha; checks in the uploads' copies %llu (%.0f MiB), of the storage "
               "before an upload that untiles on the CPU %llu (%.0f MiB) in %.1f ms", (unsigned long long)n_bc1_made,
               n_bc1_made_bytes / M, (unsigned long long)n_bc1_widened, (unsigned long long)n_bc1_up_checks,
               n_bc1_up_bytes / M, (unsigned long long)n_bc1_checks, n_bc1_check_bytes / M, n_bc1_check_ns / 1e6);
    for (int x = 0; x < nfm; x++)
        for (int y = x + 1; y < nfm; y++)
            if (fm[y].b > fm[x].b) { typeof(fm[0]) t = fm[x]; fm[x] = fm[y], fm[y] = t; }
    o = snprintf(line, sizeof line, "mem: textures frame %llu: VkFormats holding the most:", (unsigned long long)frame);
    for (int x = 0; x < nfm && x < 8; x++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %d: %llu %.0f MiB;", fm[x].fmt, (unsigned long long)fm[x].n,
                      fm[x].b / M);
    o += snprintf(line + o, sizeof line - (size_t)o, " largest:");
    for (int q = 0; q < 4 && big[q].b; q++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %ux%ux%u fmt %d %u levels %.0f MiB%s;", big[q].w, big[q].h,
                      big[q].d, big[q].fmt, big[q].levels, big[q].b / M, big[q].gpu ? " (GPU-written)" : "");
    rt_log("%s", line);
    if (!dup[0].cnt) return;
    o = snprintf(line, sizeof line, "mem: textures frame %llu: addresses with the most extra entries:", (unsigned long long)frame);
    for (int q = 0; q < 3 && dup[q].cnt && o < (int)sizeof line - 200; q++) {
        o += snprintf(line + o, sizeof line - (size_t)o, " %#llx: %llu entries, %.0f MiB beyond the largest (",
                      (unsigned long long)dup[q].addr, (unsigned long long)dup[q].cnt, dup[q].extra / M);
        for (int x = 0; x < 4 && dup[q].e[x].b && o < (int)sizeof line - 80; x++)
            o += snprintf(line + o, sizeof line - (size_t)o, "%s%ux%ux%u fmt %d %.1f MiB%s", x ? ", " : "", dup[q].e[x].w,
                          dup[q].e[x].h, dup[q].e[x].d, dup[q].e[x].fmt, dup[q].e[x].b / M, dup[q].e[x].gpu ? " GPU-written" : "");
        o += snprintf(line + o, sizeof line - (size_t)o, ");");
    }
    rt_log("%s", line);
}

void vk_texture_shutdown(void) {
    VkCtx *c = vk_ctx();
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++)
        if (by_key.vals[i]) list[n++] = by_key.vals[i];
    for (int i = 0; i < n; i++) drop_tex(list[i], DROP_SHUTDOWN);
    free(list);
    for (int i = 0; i < CP.nold; i++) {
        vk_exec_image_gone(CP.old[i].img);
        vk_defer_image(CP.old[i].img, CP.old[i].alloc);
    }
    free(CP.item);
    CP.item = NULL, CP.nitem = CP.at = CP.nold = 0, CP.old_bytes = 0;
    __atomic_store_n(&CP.active, 0, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < samplers.cap; i++)
        if (samplers.vals[i] && c) vkDestroySampler(c->device, (VkSampler)samplers.vals[i], NULL);
    free(by_key.keys), free(by_key.vals), free(by_addr.keys), free(by_addr.vals), free(samplers.keys), free(samplers.vals);
    by_key = (nvn_hmap){.klen = sizeof(TexKey)};
    by_addr = (nvn_hmap){.klen = 8};
    for (uint32_t i = 0; i < img_reqs.cap; i++) free(img_reqs.vals[i]);
    free(img_reqs.keys), free(img_reqs.vals), free(reuse.keys), free(reuse.vals);
    img_reqs = (nvn_hmap){.klen = sizeof(ImgKey)};
    reuse = (nvn_hmap){.klen = sizeof(ImgKey)};
    samplers = (nvn_hmap){.klen = sizeof(samp_par)};
    free(tslots), free(sslots);
    tslots = NULL, sslots = NULL, ntslots = nsslots = 0, nreleased = 0, pool_slots = 0;
    rel_head = rel_tail = NULL, rel_bytes = 0;
    if (sinfo) memset(sinfo, 0, sizeof *sinfo << 20);
    unlock();
}

void vk_texture_sizes(PerfSizes *s) {
    lock();
    uint64_t imgs = by_key.n, rel = (uint64_t)nreleased, relb = rel_bytes, ret = (uint64_t)nretiring, retb = retiring_bytes,
             smp = samplers.n, reqs = img_reqs.n, hold = holds.n, reuse_keys = reuse.n, tslot = pool_slots, sslot = 0;
    for (int i = 0; i < nsslots; i++) sslot += sslots[i].used != 0;
    unlock();
    perf_size(s, "textures", imgs, 0, PERF_SIZE_NONE);
    perf_size(s, "released textures", rel, 0, relb);
    perf_size(s, "retiring textures", ret, 0, retb);
    perf_size(s, "samplers", smp, 0, PERF_SIZE_NONE);
    perf_size(s, "image requirements", reqs, 0, PERF_SIZE_NONE);
    perf_size(s, "disuse holds", hold, HOLDS_MAX, PERF_SIZE_NONE);
    perf_size(s, "reuse keys", reuse_keys, 0, PERF_SIZE_NONE);
    perf_size(s, "texture pool slots", tslot, VK_POOL_TEX, PERF_SIZE_NONE);
    perf_size(s, "sampler slots", sslot, VK_POOL_SAMP, PERF_SIZE_NONE);
}

#define RES_COLD PRESENTS(45)
#define RES_HOT 3
#define RES_WARM PRESENTS(15)
#define RES_MARGIN ((uint64_t)256 << 20)
#define RES_BACK_MARGIN ((uint64_t)64 << 20)
#define RES_FRAME_BYTES ((uint64_t)64 << 20)
#define RES_OLD_N 2048
#define RES_BLOCK_COLD 90
static struct {
    int on;
    CompOld old[RES_OLD_N];
    int nold;
    uint64_t frame, frame_b;
    uint64_t scan_frame;
    uint64_t used, budget, used_frame;
    uint64_t *out_blk;
    int nout_blk, waiting;
    uint64_t wait_b;
    uint64_t cold_after;
    uint64_t pend_b;
    uint64_t n_down, down_b, n_up, up_b, n_fail, last_log, n_drop, drop_b, n_hdrop, hdrop_b;
    uint64_t host_used, host_budget;
} RS = {.on = -1, .cold_after = (uint64_t)-1};

static int res_sys(VkCtx *c, const Tex *e) {
    return !(c->mem_props.memoryHeaps[c->mem_props.memoryTypes[e->blk_type].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
}

static void res_old_sweep(void) {
    uint64_t takes = vk_pool_takes(), done = vk_sync_fence_done();
    int k = 0;
    for (int i = 0; i < RS.nold; i++) {
        CompOld *o = &RS.old[i];
        if (!o->due && takes - o->takes > VK_POOL_SETS) o->due = vk_sync_issued();
        if (o->due && done >= o->due) {
            if (o->vram) RS.pend_b = RS.pend_b > o->bytes ? RS.pend_b - o->bytes : 0;
            vk_exec_image_gone(o->img);
            vk_defer_image(o->img, o->alloc);
            continue;
        }
        RS.old[k++] = *o;
    }
    RS.nold = k;
}

static int res_move(VkCtx *c, Tex *e, int to_sys, int hole) {
    const ImgKey *k = &e->ik;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = k->flags, .imageType = (VkImageType)k->type,
                            .format = (VkFormat)k->format, .extent = {k->w, k->h, k->d}, .mipLevels = k->levels,
                            .arrayLayers = k->layers, .samples = (VkSampleCountFlagBits)k->samples,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = k->usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(c->device, &ci, NULL, &img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c->device, img, &mr);
    uint32_t bits = 0;
    for (uint32_t t = 0; t < c->mem_props.memoryTypeCount; t++) {
        int local = !!(c->mem_props.memoryHeaps[c->mem_props.memoryTypes[t].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
        if ((mr.memoryTypeBits >> t & 1) && local != to_sys) bits |= 1u << t;
    }
    if (!to_sys && hole && mr.size >= RES_DEDICATED_MIN) {
        vkDestroyImage(c->device, img, NULL);
        return 0;
    }
    VmaAllocationCreateInfo ai = {.memoryTypeBits = bits,
                                  .flags = to_sys ? (mr.size >= RES_DEDICATED_MIN ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0)
                                         : hole ? VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT
                                         : VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT |
                                               (mr.size >= RES_DEDICATED_MIN ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0)};
    VmaAllocation a = NULL;
    int ok = bits && vmaAllocateMemory(c->vma, &mr, &ai, &a, NULL) == VK_SUCCESS;
    if (ok && vmaBindImageMemory(c->vma, a, img) != VK_SUCCESS) vmaFreeMemory(c->vma, a), ok = 0;
    if (!ok) {
        vkDestroyImage(c->device, img, NULL);
        if (!hole) RS.n_fail++;
        return 0;
    }
    RS.old[RS.nold++] = (CompOld){e->pub.image, e->alloc, e->bytes, vk_pool_takes(), 0, frames_now(), to_sys};
    if (to_sys) RS.pend_b += e->bytes;
    tex_move(c, e, a, img);
    return 1;
}

#define RES_HOST_MARGIN ((uint64_t)1 << 30)
static int res_host(VkCtx *c, uint64_t *used, uint64_t *budget) {
    *used = *budget = 0;
    if (!c->has_memory_budget) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
    vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    for (uint32_t h = 0; h < p2.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (!(p2.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            *used += b.heapUsage[h], *budget += b.heapBudget[h];
    return *budget != 0;
}

static uint64_t res_host_free(VkCtx *c, uint64_t want, uint64_t now) {
    enum { HF_MAX = 1024 };
    Tex *v[HF_MAX];
    int n = 0;
    uint64_t got = 0;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < by_key.cap && n < HF_MAX && got < want; i++) {
            Tex *e = by_key.vals[i];
            if (!e || !e->alloc || !e->bytes || (pass == 0) != !!e->released || e->compacting || e->retiring ||
                !res_sys(c, e) || !comes_back_intact(e) || !idle(e, now))
                continue;
            v[n++] = e, got += e->bytes;
        }
    for (int i = 0; i < n; i++) {
        RS.n_hdrop++, RS.hdrop_b += v[i]->bytes;
        drop_tex(v[i], v[i]->released ? DROP_VRAM : DROP_IDLE_VRAM);
    }
    return got;
}

int vk_texture_host_room(uint64_t bytes) {
    VkCtx *c = vk_ctx();
    if (!c || RS.on != 1) return 0;
    lock();
    uint64_t got = res_host_free(c, bytes + ((uint64_t)256 << 20), frames_now());
    unlock();
    if (got)
        rt_log("vk: a pool's import found the host heaps full: %.0f MiB of textures in system memory that nothing "
               "reaches gave way", got / 1048576.0);
    return got >= bytes;
}

#define RES_NEW PRESENTS(8)
static int res_cold(const Tex *e, uint64_t now) {
    return e->alloc && comes_back_intact(e) && !e->compacting && !e->retiring && e->uploaded &&
           (e->released || now - e->samp_frame > RS.cold_after ||
            (e->samp_frame <= e->made_frame && now - e->made_frame > RES_NEW));
}

typedef struct { uint64_t mem, all, cold, age; } ResBlk;
static int resblk_mem(const void *a, const void *b) {
    uint64_t x = ((const ResBlk *)a)->mem, y = ((const ResBlk *)b)->mem;
    return x < y ? -1 : x > y;
}
static int resblk_age(const void *a, const void *b) {
    uint64_t x = ((const ResBlk *)a)->age, y = ((const ResBlk *)b)->age;
    return x > y ? -1 : x < y;
}
static int u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void res_plan(VkCtx *c, uint64_t now) {
    free(RS.out_blk);
    RS.out_blk = NULL, RS.nout_blk = 0;
    uint64_t target = RS.budget - RES_MARGIN, over = RS.used > target ? RS.used - target : 0;
    uint64_t room = RS.waiting && RS.used + RS.wait_b + (16ull << 20) > target ? RS.used + RS.wait_b + (16ull << 20) - target : 0;
    RS.cold_after = room ? RES_WARM : RES_COLD;
    ResBlk *b = malloc(sizeof *b * (by_key.cap ? by_key.cap : 1));
    if (!b) return;
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e || !e->alloc || res_sys(c, e)) continue;
        int cold = res_cold(e, now);
        b[n++] = (ResBlk){e->blk_mem, e->bytes, cold ? e->bytes : 0, cold ? (e->released ? UINT32_MAX : now - e->samp_frame) : 0};
    }
    qsort(b, (size_t)n, sizeof *b, resblk_mem);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && b[m - 1].mem == b[i].mem) {
            b[m - 1].all += b[i].all, b[m - 1].cold += b[i].cold;
            if (b[i].age < b[m - 1].age) b[m - 1].age = b[i].age;
        } else {
            b[m++] = b[i];
        }
    }
    int k = 0;
    for (int i = 0; i < m; i++)
        if (b[i].cold && b[i].cold * 100 >= b[i].all * RES_BLOCK_COLD) b[k++] = b[i];
    qsort(b, (size_t)k, sizeof *b, resblk_age);
    uint64_t need = over > room ? over : room;
    uint64_t *out = malloc(sizeof *out * (size_t)(k ? k : 1)), got = 0;
    int no = 0;
    for (int i = 0; out && i < k && got < need; i++) out[no++] = b[i].mem, got += b[i].cold;
    free(b);
    if (out && no) qsort(out, (size_t)no, sizeof *out, u64_cmp);
    RS.out_blk = out, RS.nout_blk = no;
}

static void residency_step(void) {
    if (RS.on < 0) {
        RS.on = cfg_bool("NX2SARIA_TEX_RESIDENCY");
        rt_log("vk: texture residency %s (NX2SARIA_TEX_RESIDENCY): textures no shader sampled for %d presents make room "
               "in system memory past the video memory budget less %d MiB", RS.on ? "on" : "off", (int)RES_COLD,
               (int)(RES_MARGIN >> 20));
    }
    if (RS.nold) res_old_sweep();
    VkCtx *c = vk_ctx();
    if (!RS.on || !c || !marks_tell() || CP.active) return;
    uint64_t now = frames_now();
    int new_frame = now != RS.used_frame;
    if (now != RS.frame) RS.frame = now, RS.frame_b = 0;
    if (new_frame) {
        int exact;
        if (!vk_mem_vram(&RS.used, &RS.budget, &exact) || RS.budget <= RES_MARGIN) return;
        RS.used = RS.used > RS.pend_b ? RS.used - RS.pend_b : 0;
        RS.used_frame = now;
        if (res_host(c, &RS.host_used, &RS.host_budget) && RS.host_used + RES_HOST_MARGIN > RS.host_budget) {
            uint64_t got = res_host_free(c, RS.host_used + RES_HOST_MARGIN - RS.host_budget, now);
            RS.host_used = RS.host_used > got ? RS.host_used - got : 0;
        }
    }
    if (RS.scan_frame == now) return;
    RS.scan_frame = now;
    uint64_t target = RS.budget - RES_MARGIN;
    int waiting = 0;
    uint64_t wait_b = UINT64_MAX;
    for (uint32_t i = 0; i < by_key.cap && RS.frame_b < RES_FRAME_BYTES && RS.nold < RES_OLD_N; i++) {
        Tex *e = by_key.vals[i];
        if (!e || !e->alloc || e->released || e->compacting || now - e->samp_frame > RES_HOT || !res_sys(c, e)) continue;
        uint64_t b = e->bytes;
        if (res_move(c, e, 0, 1)) {
            RS.frame_b += b, RS.n_up++, RS.up_b += b;
        } else if (RS.used + b + RES_BACK_MARGIN <= RS.budget && res_move(c, e, 0, 0)) {
            RS.used += b, RS.frame_b += b, RS.n_up++, RS.up_b += b;
        } else {
            waiting = 1;
            if (b < wait_b) wait_b = b;
        }
    }
    RS.waiting = waiting, RS.wait_b = waiting ? wait_b : 0;
    if (new_frame) {
        if (RS.used > target || RS.waiting) res_plan(c, now);
        else free(RS.out_blk), RS.out_blk = NULL, RS.nout_blk = 0;
    }
    enum { RES_DROPS = 256 };
    Tex *drops[RES_DROPS];
    int ndrops = 0;
    for (uint32_t i = 0; i < by_key.cap && RS.nout_blk && RS.frame_b < RES_FRAME_BYTES && RS.nold < RES_OLD_N; i++) {
        Tex *e = by_key.vals[i];
        if (!e || res_sys(c, e) || !res_cold(e, now) ||
            !bsearch(&e->blk_mem, RS.out_blk, (size_t)RS.nout_blk, sizeof *RS.out_blk, u64_cmp))
            continue;
        uint64_t b = e->bytes;
        if (e->released && idle(e, now)) {
            if (ndrops < RES_DROPS) drops[ndrops++] = e, RS.used = RS.used > b ? RS.used - b : 0;
            continue;
        }
        if ((RS.host_budget && RS.host_used + RS.frame_b + b + RES_HOST_MARGIN > RS.host_budget) || !res_move(c, e, 1, 0))
            break;
        RS.used = RS.used > b ? RS.used - b : 0, RS.frame_b += b, RS.n_down++, RS.down_b += b;
    }
    for (int i = 0; i < ndrops; i++) RS.n_drop++, RS.drop_b += drops[i]->bytes, drop_tex(drops[i], DROP_VRAM);
    if (now - RS.last_log >= PRESENTS(600) && (RS.n_down || RS.n_up || RS.n_fail)) {
        RS.last_log = now;
        uint64_t sys_n = 0, sys_b = 0, sys_hot = 0;
        for (uint32_t i = 0; i < by_key.cap; i++) {
            const Tex *e = by_key.vals[i];
            if (!e || !e->alloc || !res_sys(c, e)) continue;
            sys_n++, sys_b += e->bytes;
            if (now - e->samp_frame <= RES_HOT) sys_hot += e->bytes;
        }
        const double M = 1048576.0;
        rt_log("vk: texture residency frame %llu: video memory %.0f of %.0f MiB (target %.0f), host heaps %.0f of %.0f "
               "MiB; so far %llu textures moved to system memory (%.0f MiB), %llu back (%.0f MiB), %llu moves without "
               "memory, %llu released ones dropped (%.0f MiB), %llu for the host heaps (%.0f MiB); in system memory now "
               "%llu (%.0f MiB, %.0f MiB of it sampled lately), %s (the smallest %.0f MiB)", (unsigned long long)now, RS.used / M, RS.budget / M,
               target / M, RS.host_used / M, RS.host_budget / M, (unsigned long long)RS.n_down, RS.down_b / M,
               (unsigned long long)RS.n_up, RS.up_b / M, (unsigned long long)RS.n_fail, (unsigned long long)RS.n_drop,
               RS.drop_b / M, (unsigned long long)RS.n_hdrop, RS.hdrop_b / M, (unsigned long long)sys_n, sys_b / M,
               sys_hot / M, RS.waiting ? "sampled ones wait for room" : "none sampled waits", RS.wait_b / M);
    }
}

static int range_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

uint64_t *vk_texture_cpu_ranges(int *n) {
    *n = 0;
    lock();
    uint64_t *r = malloc(sizeof *r * 2 * (by_key.cap ? by_key.cap : 1));
    for (uint32_t i = 0; r && i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e || !comes_back_intact(e) || !e->t.L.size) continue;
        r[2 * *n] = e->pub.addr, r[2 * *n + 1] = e->pub.addr + e->t.L.size;
        (*n)++;
    }
    unlock();
    if (r && *n) qsort(r, (size_t)*n, sizeof *r * 2, range_cmp);
    else free(r), r = NULL;
    return r;
}
