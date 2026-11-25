// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "vk.h"

#ifndef NX2SARIA_TONEMAP_STANDIN
#define NX2SARIA_TONEMAP_STANDIN 1
#endif
#include "vk_exec.h"
#include "vk_gpa.h"
#include "mesh_fb.h"
#include "mesh_fb_hold.h"
#include "mesh_pipeline_check.h"
#include "vk_mesh_fb.h"
#include "vk_scratch_lru.h"
#include "vk_index_rewrite.h"
#include "platform/platform.h"
#include "stutter_map.h"
#include "vk_dlaa.h"
#include "mesh_emu_spirv.h"

#define MAX_BINDINGS 128
#define MAX_DESC 1024
#define CHUNK_BYTES (4u << 20)
#define ZERO_BYTES 65536u
#define SUPPORT_BYTES 1520u
#define NVN_STAGE_VS 0
#define NVN_STAGE_FS 1
#define NVN_STAGE_CS 5
#define NVN_STAGE_TASK 6
#define NVN_STAGE_MESH 7
static uint32_t oneshot_wait_ms(void) {
    static int64_t ms = -1;
    if (ms < 0) {
        ms = strtoll(cfg_value("NX2SARIA_ONESHOT_WAIT_MS"), NULL, 10);
        if (ms < 0) ms = 0;
        if (ms != 30000) rt_log("vk_exec: a one-shot draw waits at most %lld ms for its pipeline, then is skipped "
                                "(NX2SARIA_ONESHOT_WAIT_MS)", (long long)ms);
    }
    return (uint32_t)ms;
}
#define ONESHOT_WAIT_MS oneshot_wait_ms()
#define MESH_WAIT_MS 8u
static int oneshot_budget_on(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("ONESHOT_BUDGET", "1") == 1;
        if (!on) rt_log("vk_exec: a one-shot draw waits up to %u ms for its shaders and pipeline, outside the frame's wait "
                        "pool (NX2SARIA_ONESHOT_BUDGET=0)", ONESHOT_WAIT_MS);
    }
    return on;
}

enum { W_PROGRAM, W_SHADER, W_PIPELINE, W_NO_VS, W_INDEX, W_UB, W_BIND, W_SSBO_ALIGN, W_SSBO_SLOT, W_RESTART, W_INDIRECT, W_MESH, W_QUAD,
       W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

static void *grow(void *p, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 8;
    p = realloc(p, elem * (size_t)*cap);
    if (!p) rt_fatal("vk: out of memory");
    return p;
}


typedef struct { VkBuffer buf; VmaAllocation alloc; uint8_t *map; uint64_t size, used; } Chunk;
typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    uint32_t aspect, level0, levels, layer0, layers, swz;
} ViewKey;
_Static_assert(sizeof(ViewKey) == 40, "ViewKey is hashed as bytes: no padding");
typedef struct { uint64_t addr, size; int back; } Range;
typedef struct {
    Chunk *chunks;
    int nchunks, cchunks;
    VkDescriptorPool *pools;
    int npools, cpools;
    Range *written;
    int nwritten, cwritten;
} CbRes;

#define SPARE_POOLS 64
#define SPARE_CHUNKS 16
static PlatMutex spare_lock = PLAT_MUTEX_INIT;
static VkDescriptorPool spare_pool[SPARE_POOLS];
static Chunk spare_chunk[SPARE_CHUNKS];
static int nspare_pool, nspare_chunk;
static volatile int64_t npools_live;
#define REPLAY_POOL_SETS 256

typedef struct {
    VkPipeline pipe[2];
    VkPipeState st;
    int st_valid;
    VkDynShadow dyn;
    VkBuffer vb[17];
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17];
    void *vpool[17];
    NVNbufferRange vin[16];
    uint64_t vgen;
    int vb_valid;
    VkBuffer ib;
    VkDeviceSize ib_off;
    VkIndexType ib_type;
    int ib_valid;
    uint8_t support[SUPPORT_BYTES];
    float sup_in[3];
    VkBuffer sup_buf;
    VkDeviceSize sup_off;
    int sup_valid;
    uint8_t c0[5][VK_C0_BYTES];
    VkBuffer c0_buf[5];
    VkDeviceSize c0_off[5];
    int c0_valid[5];
    struct { p_bind_buf ssbo[16], ubo[16]; uint32_t dim[3], binst; uint8_t push[1024]; } c0_in[5];
    VkPipelineLayout ds_layout;
    VkDescriptorSet ds_bound[2];
    VkDescriptorSet ds_last[2];
    VkDescriptorSetLayout ds_last_layout[2];
    int ds_pushed[2];
    int res_valid;
} CbState;

#define DS_SIG_BYTES (MAX_DESC * sizeof(VkDescSlot))
typedef struct { uint32_t n[2], pn[2]; uint8_t sig[2][DS_SIG_BYTES], psig[2][DS_SIG_BYTES]; } DsSig;

static struct {
    int on;
    uint64_t draws, bind[2], desc[2], push, push_skip, set_new, set_reuse, ac_miss, same_prog, same_all, bind_skip;
} BS = {-1};

#define RS_BYTES (MAX_BINDINGS * sizeof(((VkExec *)0)->ssbo[0]) + MAX_DESC * sizeof(p_bind_buf))
typedef struct { int cls, use; uint64_t bytes; } RsCount;
typedef struct {
    uint32_t hn;
    uint64_t h[24];
    const VkPipeInfo *info;
    uint64_t gen;
    uint8_t body[RS_BYTES];
    int rec;
    int volatile_;
    RsCount cnt[2 * VK_MCLS_N], pcnt[2 * VK_MCLS_N];
    int ncnt, pncnt, cnt_over;
    int cmds;
    uint32_t ckn;
    uint8_t ck[DS_SIG_BYTES];
} RsState;
static int rs_on = -1;

#define RS_PTRS 32
typedef struct { uint32_t off, len; } RsRange;
typedef struct { int st; const GfxShaderRes *res; } RsPtr;
typedef struct { uint32_t n, bytes; uint8_t sup, c0, ok, np; RsPtr p[RS_PTRS]; RsRange r[]; } RsList;
static nvn_hmap rs_lists;

#define STAGEM_N 512
#define SLOTM_N 4096
#define PB_N 8
#define PB_DESC 192
#define PB_G 17
typedef struct {
    uint64_t tex[32], img[32], ttok[32], itok[32];
    uint32_t tgen[32], igen[32];
    uint8_t tt[32], it[32], ifmt[32];
} AcSlots;
typedef struct { uint64_t serial; int8_t idx[6]; uint32_t len[6]; } StageMemo;
typedef struct { uint64_t serial; int st; uint32_t m; } SlotMask;
typedef struct { int cls, use, has; uint64_t n; } PbCount;
typedef struct {
    const VkPipeInfo *info;
    uint64_t g[PB_G];
    uint8_t ok[PB_DESC];
    uint64_t in[PB_DESC][4];
    VkDescSlot d[PB_DESC];
    PbCount cnt[PB_DESC];
} PbEntry;
typedef struct {
    const prog_t *prog;
    uint64_t serial;
    uint32_t gen, tex_serial;
    uint64_t tex[2][32], img[2][32];
    VkPipeState pipe;
} MemoKey;

#define TM_N 512
typedef struct {
    uint64_t h, tok;
    uint32_t gen, ep, sg;
    uint16_t type;
    uint8_t separate, valid;
    VkImage image;
    VkDescriptorImageInfo out;
} TexMemo;

typedef struct DrawCtx {
    CbRes *res;
    uint64_t res_fence;
    CbState B;
    DsSig DS;
    RsState RS;
    const VkPipeInfo *rs_last;
    const RsList *rs_last_l;
    uint32_t c0_dim[3], c0_binst;
    uint64_t bind_gen;
    AcSlots AC[8];
    StageMemo stagem[STAGEM_N];
    SlotMask slotm[SLOTM_N];
    int sf_mvprev;
    int mfb_stage;
    const GfxShader *mfb_cur_vs;
    uint64_t mfb_list;
    PbCount *pb_cap;
    int pb_spoiled;
    PbEntry PB[PB_N];
    PbEntry *pb_e;
    uint64_t pb_key_cur[4];
    int pb_key_ok;
    PbCount pb_cnt_cur;
    int mo_ovr_on;
    VkDescriptorBufferInfo mo_ovr[2][32];
    struct { int valid, has_fs; MemoKey k; const GfxShader *vs, *fs; } SM;
    struct { int valid; VkPipeState s; const VkPipeInfo *info; } PM;
    void *fm;
    void *mo_vm;
    struct { const void *d; uint64_t ind; VkDeviceSize at; } ma[64];
    int ma_n, ma_i;
    uint32_t ma_brk;
    TexMemo TM[TM_N];
    VkWriteDescriptorSet bind_w[MAX_BINDINGS];
    VkDescSlot bind_d[MAX_DESC];
    VkBufferView bind_bv[MAX_DESC];
} DrawCtx;
static DrawCtx dc_main = {.bind_gen = 1};
static _Thread_local DrawCtx *dc = &dc_main;

void *vk_exec_draw_ctx_new(void) {
    DrawCtx *c = calloc(1, sizeof *c);
    if (!c) rt_fatal("vk: out of memory (draw context)");
    c->bind_gen = 1;
    return c;
}

void vk_exec_draw_ctx_use(void *c) { dc = c ? c : &dc_main; }

static PlatMutex shader_lock = PLAT_MUTEX_INIT;
static PlatMutex rs_lock = PLAT_MUTEX_INIT;
static PlatMutex pool_mt = PLAT_MUTEX_INIT;

static void cb_done(void *arg) {
    CbRes *r = arg;
    VkCtx *c = vk_ctx();
    for (int i = 0; i < r->nwritten; i++)
        if (r->written[i].back) vk_gpu_written_back(r->written[i].addr, r->written[i].size);
        else vk_gpu_written(r->written[i].addr, r->written[i].size);
    for (int i = 0; i < r->npools; i++) {
        vkResetDescriptorPool(c->device, r->pools[i], 0);
        plat_mutex_lock(&spare_lock);
        int kept = nspare_pool < SPARE_POOLS;
        if (kept) spare_pool[nspare_pool++] = r->pools[i];
        plat_mutex_unlock(&spare_lock);
        if (!kept) vkDestroyDescriptorPool(c->device, r->pools[i], NULL), __atomic_sub_fetch(&npools_live, 1, __ATOMIC_RELAXED);
    }
    for (int i = 0; i < r->nchunks; i++) {
        plat_mutex_lock(&spare_lock);
        int kept = r->chunks[i].size == CHUNK_BYTES && nspare_chunk < SPARE_CHUNKS;
        if (kept) spare_chunk[nspare_chunk++] = r->chunks[i];
        plat_mutex_unlock(&spare_lock);
        if (!kept) vmaDestroyBuffer(c->vma, r->chunks[i].buf, r->chunks[i].alloc);
    }
    free(r->written), free(r->pools), free(r->chunks), free(r);
}

void vk_exec_draw_new_cb(void) { memset(&dc->B, 0, sizeof dc->B); }

void vk_exec_draw_shaders(const GfxShader **vs, const GfxShader **fs) {
    DrawCtx *const cx = dc;
    *vs = cx->B.st_valid ? cx->B.st.vs : NULL;
    *fs = cx->B.st_valid ? cx->B.st.fs : NULL;
}

static VkPassMark *dispatch_mark(VkExec *ex, const GfxShader *cs, uint32_t x, uint32_t y, uint32_t z) {
    VkPassMark *m = vk_pass_mark_begin(ex->cb, VK_MARK_DISPATCH, ex->fence);
    if (!m) return NULL;
    m->step = ex->mark_dispatches++;
    m->w = x, m->h = y, m->d = z, m->draws = 0, m->ncolor = 0;
    m->first[0] = cs, m->first[1] = m->last[0] = m->last[1] = NULL;
    return m;
}

static CbRes *cb_res(VkExec *ex) {
    DrawCtx *d = ex->dc;
    if (d->res && d->res_fence == ex->fence) return d->res;
    CbRes *r = calloc(1, sizeof *r);
    if (!r) rt_fatal("vk: out of memory");
    d->res = r, d->res_fence = ex->fence;
    memset(&d->B, 0, sizeof d->B);
    vk_completion_add(ex->fence, cb_done, r);
    return r;
}

static uint8_t *scratch(VkExec *ex, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    CbRes *r = cb_res(ex);
    size = (size + 255) & ~255ull;
    Chunk *c = r->nchunks ? &r->chunks[r->nchunks - 1] : NULL;
    if (!c || c->used + size > c->size) {
        if (r->nchunks == r->cchunks) r->chunks = grow(r->chunks, &r->cchunks, sizeof *r->chunks);
        c = &r->chunks[r->nchunks];
        uint64_t n = size > CHUNK_BYTES ? size : CHUNK_BYTES;
        int reused = 0;
        if (n == CHUNK_BYTES) {
            plat_mutex_lock(&spare_lock);
            if (nspare_chunk) *c = spare_chunk[--nspare_chunk], reused = 1;
            plat_mutex_unlock(&spare_lock);
        }
        if (reused) {
            c->used = 0;
            r->nchunks++;
            *buf = c->buf;
            *off = 0;
            c->used = size;
            return c->map;
        }
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                 .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo info;
        VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &c->buf, &c->alloc, &info);
        if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
            uint64_t freed = vk_mem_give_back();
            VkResult again = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &c->buf, &c->alloc, &info);
            static volatile int32_t told;
            if (plat_inc32(&told) <= 16)
                rt_log("vk: transient buffer of %llu bytes refused (%d): %.1f MiB given back, %s", (unsigned long long)n,
                       (int)res, freed / 1048576.0, again == VK_SUCCESS ? "made at the second try" : "refused again");
            res = again;
        }
        if (res != VK_SUCCESS) rt_fatal("vk: transient buffer of %llu bytes failed (%d)", (unsigned long long)n, (int)res);
        c->map = info.pMappedData, c->size = n, c->used = 0;
        r->nchunks++;
    }
    *buf = c->buf;
    *off = c->used;
    uint8_t *p = c->map + c->used;
    c->used += size;
    return p;
}

static void note_written(VkExec *ex, uint64_t addr, uint64_t size) {
    CbRes *r = cb_res(ex);
    for (int i = 0; i < r->nwritten; i++)
        if (r->written[i].addr == addr && r->written[i].size == size) return;
    if (r->nwritten == r->cwritten) r->written = grow(r->written, &r->cwritten, sizeof *r->written);
    r->written[r->nwritten++] = (Range){addr, size, vk_mem_mirrored(addr, size)};
    vk_mem_gpu_pending(addr, size);
}


enum { DV_1D, DV_1D_ARRAY, DV_2D, DV_2D_ARRAY, DV_3D, DV_CUBE, DV_CUBE_ARRAY, DV_2D_MS, DV_2D_MS_ARRAY, DV_COUNT };

static struct {
    int ready;
    VkBuffer zbuf;
    VmaAllocation zalloc;
    VkBufferView zview;
    VkBuffer sink;
    VmaAllocation sink_alloc;
    VkSampler samp, samp_shadow;
    struct { VkImage image; VmaAllocation alloc; VkImageView view, sview; } img[DV_COUNT];
} Z;

static const struct { VkImageType t; VkImageViewType vt; uint32_t layers; VkSampleCountFlagBits s; VkImageCreateFlags f; } dinfo[DV_COUNT] = {
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_4_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_4_BIT, 0}};

static void make_dummy(int i, VkFormat fmt, const VkClearColorValue *clear, VkImage *image, VmaAllocation *alloc,
                       VkImageView *view, VkImageView *sview) {
    VkCtx *c = vk_ctx();
    int ms = dinfo[i].s != VK_SAMPLE_COUNT_1_BIT;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = dinfo[i].f, .imageType = dinfo[i].t,
                            .format = fmt, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = dinfo[i].layers,
                            .samples = dinfo[i].s, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     (ms ? 0 : VK_IMAGE_USAGE_STORAGE_BIT),
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ia = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ia, image, alloc, NULL) != VK_SUCCESS) rt_fatal("vk: stand-in image %d failed", i);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *image,
                                .viewType = dinfo[i].vt, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    if (vkCreateImageView(c->device, &vi, NULL, view) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    *sview = *view;
    if (!ms && (dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE || dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (vkCreateImageView(c->device, &vi, NULL, sview) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    }
    VkImageMemoryBarrier2 bar = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = *image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    VkCommandBuffer cb = vk_upload_begin();
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar};
    bar.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, bar.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier2(cb, &di);
    vkCmdClearColorImage(cb, *image, VK_IMAGE_LAYOUT_GENERAL, clear, 1, &bar.subresourceRange);
    bar.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bar.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    bar.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier2(cb, &di);
    vk_upload_end();
}

static const VkClearColorValue DUMMY_SAMPLED = {.float32 = {0.5f, 0.5f, 0.5f, 1.0f}};
static const VkClearColorValue DUMMY_STORAGE = {.uint32 = {0, 0, 0, 0}};

static const VkFormat tex_format[40] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
    VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
    VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
    VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32};
static struct { int used, di; VkFormat fmt; VkImage image; VmaAllocation alloc; VkImageView view, sview; } SD[48];

static VkImageView storage_dummy(int di, int code) {
    VkFormat fmt = code > 0 && code < 40 ? tex_format[code] : VK_FORMAT_R8G8B8A8_UNORM;
    if (fmt == VK_FORMAT_R8G8B8A8_UNORM || dinfo[di].s != VK_SAMPLE_COUNT_1_BIT) return Z.img[di].sview;
    for (int i = 0; i < 48; i++) {
        if (!SD[i].used) {
            SD[i].used = 1, SD[i].di = di, SD[i].fmt = fmt;
            make_dummy(di, fmt, &DUMMY_STORAGE, &SD[i].image, &SD[i].alloc, &SD[i].view, &SD[i].sview);
            return SD[i].sview;
        }
        if (SD[i].di == di && SD[i].fmt == fmt) return SD[i].sview;
    }
    return Z.img[di].sview;
}

static void dummies_init(void) {
    if (Z.ready) return;
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ZERO_BYTES,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &Z.zbuf, &Z.zalloc, &info) != VK_SUCCESS) rt_fatal("vk: zero buffer failed");
    memset(info.pMappedData, 0, ZERO_BYTES);
    VkBufferViewCreateInfo bv = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = Z.zbuf,
                                 .format = VK_FORMAT_R32G32B32A32_SFLOAT, .range = ZERO_BYTES};
    if (vkCreateBufferView(c->device, &bv, NULL, &Z.zview) != VK_SUCCESS) rt_fatal("vk: zero buffer view failed");
    VkBufferCreateInfo sbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ZERO_BYTES,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo sai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(c->vma, &sbi, &sai, &Z.sink, &Z.sink_alloc, NULL) != VK_SUCCESS) rt_fatal("vk: sink buffer failed");
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    sp.lod_max = 1000.0f;
    Z.samp = vk_sampler_for(&sp);
    sp.cmp_mode = 1, sp.cmp_func = NVN_DEPTH_FUNC_ALWAYS;
    Z.samp_shadow = vk_sampler_for(&sp);
    if (!Z.samp || !Z.samp_shadow) rt_fatal("vk: the default samplers could not be made (see the vkCreateSampler line above)");

    for (int i = 0; i < DV_COUNT; i++)
        make_dummy(i, VK_FORMAT_R8G8B8A8_UNORM, &DUMMY_SAMPLED, &Z.img[i].image, &Z.img[i].alloc, &Z.img[i].view, &Z.img[i].sview);
    Z.ready = 1;
}

static void twins_release(VkCtx *c);

static void mesh_ring_release(VkCtx *c);
static void pool_marks_release(VkCtx *c);

static void pose_shutdown(void);
static void skin_shutdown(void);
void vk_exec_draw_shutdown(void) {
    pose_shutdown();
    skin_shutdown();
    vk_meshfb_shutdown();
    if (vk_ctx()) mesh_ring_release(vk_ctx()), pool_marks_release(vk_ctx());
    if (!Z.ready) return;
    VkCtx *c = vk_ctx();
    twins_release(c);
    for (int i = 0; i < DV_COUNT; i++) {
        if (Z.img[i].sview != Z.img[i].view) vkDestroyImageView(c->device, Z.img[i].sview, NULL);
        vkDestroyImageView(c->device, Z.img[i].view, NULL);
        vmaDestroyImage(c->vma, Z.img[i].image, Z.img[i].alloc);
    }
    for (int i = 0; i < 48 && SD[i].used; i++) {
        if (SD[i].sview != SD[i].view) vkDestroyImageView(c->device, SD[i].sview, NULL);
        vkDestroyImageView(c->device, SD[i].view, NULL);
        vmaDestroyImage(c->vma, SD[i].image, SD[i].alloc);
    }
    memset(SD, 0, sizeof SD);
    nvn_hm_clear(&rs_lists, free);
    dc_main.rs_last = NULL, dc_main.rs_last_l = NULL;
    for (int i = 0; i < nspare_pool; i++) vkDestroyDescriptorPool(c->device, spare_pool[i], NULL);
    __atomic_sub_fetch(&npools_live, nspare_pool, __ATOMIC_RELAXED);
    for (int i = 0; i < nspare_chunk; i++) vmaDestroyBuffer(c->vma, spare_chunk[i].buf, spare_chunk[i].alloc);
    nspare_pool = nspare_chunk = 0;
    vk_usnap_shutdown();
    vkDestroyBufferView(c->device, Z.zview, NULL);
    vmaDestroyBuffer(c->vma, Z.zbuf, Z.zalloc);
    if (Z.sink) vmaDestroyBuffer(c->vma, Z.sink, Z.sink_alloc);
    memset(&Z, 0, sizeof Z);
}


static VkImageViewType res_view_type(uint16_t t) {
    int arr = t & 0x100;
    switch (t & 0xff) {
    case 1: return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 4: return VK_IMAGE_VIEW_TYPE_3D;
    case 5: return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

static int dummy_index(VkImageViewType vt, int ms) {
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D: return DV_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return DV_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return DV_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return DV_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return DV_CUBE_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? DV_2D_MS_ARRAY : DV_2D_ARRAY;
    default: return ms ? DV_2D_MS : DV_2D;
    }
}

static int gfx_type_of(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_target) return vk_pipeline_tex_type(v->target);
    int ms = img->samples > 1;
    switch (img->view_type) {
    case VK_IMAGE_VIEW_TYPE_1D: return GFX_TEX_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return GFX_TEX_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? GFX_TEX_2D_MS_ARRAY : GFX_TEX_2D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return GFX_TEX_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return GFX_TEX_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return GFX_TEX_CUBE_ARRAY;
    default: return ms ? GFX_TEX_2D_MS : GFX_TEX_2D;
    }
}

static VkComponentSwizzle swz_of(uint8_t s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    default: return VK_COMPONENT_SWIZZLE_A;
    }
}

VkImageAspectFlags vk_exec_view_aspect(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_dsmode && v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL && (img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    return img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT     ? VK_IMAGE_ASPECT_DEPTH_BIT
           : img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                       : VK_IMAGE_ASPECT_COLOR_BIT;
}

typedef struct ViewNode { struct ViewNode *next; ViewKey k; VkImageView view; } ViewNode;
static nvn_hmap view_of;
static nvn_hmap views_by_image;

static int int_view_format(uint32_t nvn, VkFormat *vk) {
    static const struct { uint16_t nvn; uint8_t code; VkFormat vk; } T[] = {
        {NVN_FORMAT_R8UI, 3, VK_FORMAT_R8_UINT}, {NVN_FORMAT_R8I, 4, VK_FORMAT_R8_SINT},
        {NVN_FORMAT_R16UI, 8, VK_FORMAT_R16_UINT}, {NVN_FORMAT_R16I, 9, VK_FORMAT_R16_SINT},
        {NVN_FORMAT_R32UI, 11, VK_FORMAT_R32_UINT}, {NVN_FORMAT_R32I, 12, VK_FORMAT_R32_SINT},
        {NVN_FORMAT_RG8UI, 15, VK_FORMAT_R8G8_UINT}, {NVN_FORMAT_RG8I, 16, VK_FORMAT_R8G8_SINT},
        {NVN_FORMAT_RG16UI, 20, VK_FORMAT_R16G16_UINT}, {NVN_FORMAT_RG16I, 21, VK_FORMAT_R16G16_SINT},
        {NVN_FORMAT_RG32UI, 23, VK_FORMAT_R32G32_UINT}, {NVN_FORMAT_RG32I, 24, VK_FORMAT_R32G32_SINT},
        {NVN_FORMAT_RGBA8UI, 27, VK_FORMAT_R8G8B8A8_UINT}, {NVN_FORMAT_RGBA8I, 28, VK_FORMAT_R8G8B8A8_SINT},
        {NVN_FORMAT_RGBA16UI, 32, VK_FORMAT_R16G16B16A16_UINT}, {NVN_FORMAT_RGBA16I, 33, VK_FORMAT_R16G16B16A16_SINT},
        {NVN_FORMAT_RGBA32UI, 35, VK_FORMAT_R32G32B32A32_UINT}, {NVN_FORMAT_RGBA32I, 36, VK_FORMAT_R32G32B32A32_SINT},
        {NVN_FORMAT_RGB10A2UI, 38, VK_FORMAT_A2B10G10R10_UINT_PACK32}};
    for (size_t i = 0; i < sizeof T / sizeof *T; i++)
        if (T[i].nvn == nvn) {
            if (vk) *vk = T[i].vk;
            return T[i].code;
        }
    return 0;
}

static int texel_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_UNORM: return 2;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return 4;
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 0;
    }
}

static struct { VkImage img; uint64_t frame, seq; } RAW[8];
static uint64_t raw_seq;

static void raw_note(VkImage img) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    int k = 0;
    raw_seq++;
    for (int i = 0; i < 8; i++) {
        if (RAW[i].img == img) { RAW[i].frame = f, RAW[i].seq = raw_seq; return; }
        if (RAW[i].frame < RAW[k].frame) k = i;
    }
    RAW[k].img = img, RAW[k].frame = f, RAW[k].seq = raw_seq;
}

static uint64_t raw_written(VkImage img) {
    uint64_t now = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    for (int i = 0; i < 8; i++)
        if (RAW[i].img == img && now - RAW[i].frame <= 1) return RAW[i].seq;
    return 0;
}

static int raw_9e5(VkImage img, VkFormat f) { return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && raw_written(img); }
static int raw_native_9e5(VkImage img, VkFormat f) { return f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && raw_written(img); }

static const VkTexImage *raw_9e5_twin(const VkTexImage *t) {
    const VkTexImage *o = vk_texture_at_locked(t, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    uint64_t ro = o && raw_9e5(o->image, o->format) ? raw_written(o->image) : 0;
    uint64_t rt = raw_native_9e5(t->image, t->format) ? raw_written(t->image) : 0;
    return rt > ro ? t : ro ? o : NULL;
}

int vk_exec_raw_9e5(VkImage img, VkFormat f) { return raw_9e5(img, f); }

static VkFormat storage_view_format(const VkTexImage *img, const view_t *v, int hv) {
    VkFormat f;
    if (hv && v->set_format && int_view_format(v->format, &f) && texel_bytes(f) && texel_bytes(f) == texel_bytes(img->format)) {
        if (img->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || img->format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
            raw_note(img->image);
        return f;
    }
    return img->format;
}

static VkImageView get_view(VkExec *ex, const VkTexImage *img, const view_t *v, int hv, uint16_t rtype, int storage) {
    VkImageViewType vt = res_view_type(rtype);
    int ms = (rtype & 0x200) != 0;
    if ((img->samples > 1) != ms || (storage && ms)) return VK_NULL_HANDLE;
    if (storage && !(img->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_NULL_HANDLE;
    int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    int ok = 0;
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D:
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: ok = img->type == VK_IMAGE_TYPE_1D; break;
    case VK_IMAGE_VIEW_TYPE_3D:
        ok = img->type == VK_IMAGE_TYPE_3D;
        if (!ok && !storage && !ms && img->type == VK_IMAGE_TYPE_2D) {
            const VkTexImage *f = vk_texture_flat3d_locked(img, hv && v->set_layers ? v->min_layer : 0);
            if (f) img = f, ok = 1;
        }
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        ok = img->type == VK_IMAGE_TYPE_2D && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
        break;
    default: ok = img->type == VK_IMAGE_TYPE_2D; break;
    }
    if (!ok) return VK_NULL_HANDLE;
    uint32_t l0 = hv && v->set_levels ? v->base_level : 0;
    uint32_t nl;
    if (img->skip) {
        uint32_t sk = img->skip, all = img->levels + sk;
        if (l0 >= all) return VK_NULL_HANDLE;
        uint32_t end = hv && v->set_levels && v->nlevels ? l0 + v->nlevels : all;
        if (end > all) end = all;
        l0 = l0 > sk ? l0 - sk : 0;
        end = end > sk ? end - sk : 1;
        nl = end > l0 ? end - l0 : 1;
    } else {
        if (l0 >= img->levels) return VK_NULL_HANDLE;
        nl = hv && v->set_levels && v->nlevels ? v->nlevels : img->levels - l0;
        if (l0 + nl > img->levels) nl = img->levels - l0;
    }
    if (storage) nl = 1;
    uint32_t y0 = 0, ny = 1;
    if (vt != VK_IMAGE_VIEW_TYPE_3D) {
        y0 = hv && v->set_layers ? v->min_layer : 0;
        if (y0 >= img->layers) return VK_NULL_HANDLE;
        uint32_t rest = hv && v->set_layers && v->nlayers ? v->nlayers : img->layers - y0;
        if (y0 + rest > img->layers) rest = img->layers - y0;
        if (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_2D) ny = 1;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE) ny = 6;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ny = rest / 6 * 6;
        else ny = rest;
        if (!ny || y0 + ny > img->layers) return VK_NULL_HANDLE;
    }
    if (storage && cube) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    uint32_t aspect = vk_exec_view_aspect(img, v, hv);
    uint8_t sw[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A};
    if (!storage) {
        memcpy(sw, hv && v->set_swizzle ? v->swz : img->swz, 4);
        if (img->alpha_one || img->chan[0])
            for (int i = 0; i < 4; i++) {
                if (img->alpha_one && sw[i] == NVN_TEXTURE_SWIZZLE_A) sw[i] = NVN_TEXTURE_SWIZZLE_ONE;
                else if (img->chan[0] && sw[i] >= NVN_TEXTURE_SWIZZLE_R && sw[i] <= NVN_TEXTURE_SWIZZLE_A)
                    sw[i] = img->chan[sw[i] - NVN_TEXTURE_SWIZZLE_R];
            }
    }
    uint32_t swz = (uint32_t)sw[0] | (uint32_t)sw[1] << 8 | (uint32_t)sw[2] << 16 | (uint32_t)sw[3] << 24;
    VkFormat vf = storage ? storage_view_format(img, v, hv) : img->format;
    ViewKey k = {img->image, vf, vt, aspect, l0, nl, y0, ny, swz};
    if (!view_of.klen) nvn_hm_init(&view_of, sizeof(ViewKey)), nvn_hm_init(&views_by_image, sizeof(VkImage));
    VkImageView found = (VkImageView)nvn_hm_get(&view_of, &k);
    if (found) return found;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image, .viewType = vt,
                                .format = vf,
                                .components = {swz_of(sw[0]), swz_of(sw[1]), swz_of(sw[2]), swz_of(sw[3])},
                                .subresourceRange = {aspect, l0, nl, y0, ny}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    if (img->block_views) ci.pNext = &use;
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    ViewNode *n = malloc(sizeof *n);
    if (!n) rt_fatal("vk: out of memory");
    *n = (ViewNode){nvn_hm_get(&views_by_image, &img->image), k, view};
    nvn_hm_put(&views_by_image, &img->image, n);
    nvn_hm_put(&view_of, &k, (void *)view);
    return view;
}

VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat format, VkImageViewType type, uint32_t aspect,
                                uint32_t level, uint32_t base, uint32_t layers, int blocks) {
    vk_texture_lock();
    ViewKey k = {img->image, format, type, aspect, level, 1, base, layers, blocks ? 0xfffffffeu : 0xffffffffu};
    if (!view_of.klen) nvn_hm_init(&view_of, sizeof(ViewKey)), nvn_hm_init(&views_by_image, sizeof(VkImage));
    VkImageView view = img->gone ? VK_NULL_HANDLE : (VkImageView)nvn_hm_get(&view_of, &k);
    if (!view) {
        VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image,
                                    .viewType = type, .format = format, .subresourceRange = {aspect, level, 1, base, layers}};
        VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                          .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        if (blocks) ci.pNext = &use;
        if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
            rt_fatal("vk: render target view (format %d) could not be created", (int)format);
        if (img->gone) {
            vk_defer_view(view);
            vk_texture_unlock();
            return view;
        }
        ViewNode *n = malloc(sizeof *n);
        if (!n) rt_fatal("vk: out of memory");
        *n = (ViewNode){nvn_hm_get(&views_by_image, &img->image), k, view};
        nvn_hm_put(&views_by_image, &img->image, n);
        nvn_hm_put(&view_of, &k, (void *)view);
    }
    vk_texture_unlock();
    return view;
}

int vk_exec_image_gone(VkImage image) {
    vk_exec_bc_gone(image);
    vk_tonemap_forget(image);
    vk_hdr_image_gone(image);
    if (!views_by_image.klen) return 0;
    int purged = 0;
    for (ViewNode *n = nvn_hm_del(&views_by_image, &image), *next; n; n = next, purged++) {
        next = n->next;
        nvn_hm_del(&view_of, &n->k);
        vk_defer_view(n->view);
        free(n);
    }
    return purged;
}


#define SHADER_SETS 2048
#define SHADER_WAYS 4
typedef struct { uint64_t serial; int stage; int used; GfxShaderAnswers a; const GfxShader *s; } ShaderEnt;
static ShaderEnt shader_tab[SHADER_SETS][SHADER_WAYS];

static int ctl_nvn2(const uint8_t *c) {
    uint32_t magic;
    memcpy(&magic, c, 4);
    return magic == 0x98761234u;
}
static void ctl_stage_len(const uint8_t *c, uint32_t *st, uint32_t *len) {
    if (ctl_nvn2(c)) {
        uint32_t c1s, c1e;
        memcpy(st, c + 0x54, 4), *st >>= 16;
        memcpy(len, c + 0x38, 4);
        memcpy(&c1s, c + 0x40, 4), memcpy(&c1e, c + 0x44, 4);
        if (c1s >= *len && c1e > c1s && c1e - *len < (1u << 20)) *len = c1e;
    } else {
        memcpy(st, c + 0x714, 4);
        memcpy(len, c + 0x704, 4);
    }
}
static uint32_t ctl_c1(const uint8_t *c, uint32_t len) {
    uint32_t c1 = len;
    if (!ctl_nvn2(c)) memcpy(&c1, c + 0x700, 4);
    else memcpy(&c1, c + 0x40, 4);
    return c1;
}
static const uint8_t *ctl_local(const uint8_t *c) { return c + (ctl_nvn2(c) ? 0x58 : 0x748); }

static int find_stage_scan(const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    for (uint32_t i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *c = p->sd[i].control;
        if (!c || !p->sd[i].data) continue;
        uint32_t st;
        ctl_stage_len(c, &st, len);
        if ((int)st != nvn_stage) continue;
        *ctl = c, *code = (const uint8_t *)(uintptr_t)p->sd[i].data;
        return 1;
    }
    return 0;
}

static int find_stage(DrawCtx *cx, const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    if (!p->serial || (unsigned)nvn_stage >= 6) return find_stage_scan(p, nvn_stage, ctl, code, len);
    StageMemo *m = &cx->stagem[p->serial * 0x9E3779B97F4A7C15ull >> 55];
    if (m->serial != p->serial) {
        m->serial = p->serial;
        memset(m->idx, -1, sizeof m->idx);
        for (uint32_t i = 0; i < p->nstage && i < 6; i++) {
            const uint8_t *c = p->sd[i].control;
            if (!c || !p->sd[i].data) continue;
            uint32_t st, len;
            ctl_stage_len(c, &st, &len);
            if (st < 6 && m->idx[st] < 0) m->idx[st] = (int8_t)i, m->len[st] = len;
        }
    }
    int i = m->idx[nvn_stage];
    if (i < 0) return 0;
    *ctl = p->sd[i].control, *code = (const uint8_t *)(uintptr_t)p->sd[i].data, *len = m->len[nvn_stage];
    return 1;
}


static uint64_t token_of(uint64_t h) { return h ? vk_texture_slot_token((int)(h & 0xfffff)) : 1; }

static int slot_still(uint32_t *egen, uint64_t etok, uint64_t h, uint32_t gen) {
    if (*egen == gen) return 1;
    uint64_t t = token_of(h);
    if (!t || t != etok) return 0;
    *egen = gen;
    return 1;
}
static int shader_memo_on(void);

static uint32_t slotm_idx(uint64_t serial, int st) { return (uint32_t)((serial * 8 + (uint64_t)st) * 0x9E3779B97F4A7C15ull >> 52); }

static uint32_t slot_mask_of(DrawCtx *cx, uint64_t serial, int st) {
    const SlotMask *e = &cx->slotm[slotm_idx(serial, st)];
    return serial && e->serial == serial && e->st == st ? e->m : ~0u;
}

static void slot_mask_learn(DrawCtx *cx, uint64_t serial, int st, const GfxShader *s) {
    if (!serial || slot_mask_of(cx, serial, st) != ~0u) return;
    cx->slotm[slotm_idx(serial, st)] = (SlotMask){serial, st, gfx_shader_slot_mask(s)};
}

static uint8_t type_of_handle(uint64_t h) {
    view_t v;
    int hv;
    const VkTexImage *img = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
    return img ? (uint8_t)gfx_type_of(img, &v, hv) : 0;
}

static uint8_t format_of_handle(uint64_t h) {
    view_t v;
    int hv;
    const VkTexImage *img = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
    VkFormat f = VK_FORMAT_UNDEFINED;
    static int logged;
    if (img && hv && v.set_format && int_view_format(v.format, NULL) && logged < 8)
        logged++, rt_log("vk: image view format: handle %#llx view nvn format %#x on vk format %d -> %d",
                         (unsigned long long)h, v.format, img->format, int_view_format(v.format, NULL));
    if (!img || !hv || !v.set_format || !int_view_format(v.format, &f) || texel_bytes(f) != texel_bytes(img->format) || !texel_bytes(f))
        return 0;
    return (uint8_t)int_view_format(v.format, NULL);
}

static void answers_for(VkExec *ex, int st, const uint8_t *ctl, uint32_t mask, GfxShaderAnswers *a) {
    memset(a, 0, sizeof *a);
    if (st == NVN_STAGE_VS) vk_pipeline_answers(&ex->pipe, a, NULL);
    else if (st == NVN_STAGE_FS) vk_pipeline_answers(&ex->pipe, NULL, a);
    else
        for (int k = 0; k < 3; k++) {
            uint32_t l;
            memcpy(&l, ctl_local(ctl) + 4 * k, 4);
            a->local[k] = (uint16_t)(l ? l : 1);
        }
    AcSlots *c = &ex->dc->AC[st];
    uint32_t gen = vk_texture_slots_gen();
    int locked = 0, check = shader_memo_on() == 2;
    for (uint32_t m = mask; m; m &= m - 1) {
        int slot = __builtin_ctz(m);
        uint64_t h = ex->tex[st][slot], hi = ex->img[st][slot];
        int tmiss = c->tex[slot] != h || !c->ttok[slot] || !slot_still(&c->tgen[slot], c->ttok[slot], h, gen);
        int imiss = c->img[slot] != hi || !c->itok[slot] || !slot_still(&c->igen[slot], c->itok[slot], hi, gen);
        uint64_t tt = tmiss ? token_of(h) : 0, it = imiss ? token_of(hi) : 0;
        if ((tmiss || imiss || check) && !locked) vk_texture_lock(), locked = 1, BS.ac_miss++;
        if (tmiss) c->tt[slot] = type_of_handle(h), c->tex[slot] = h, c->ttok[slot] = tt, c->tgen[slot] = gen;
        if (imiss)
            c->it[slot] = type_of_handle(hi), c->ifmt[slot] = format_of_handle(hi), c->img[slot] = hi, c->itok[slot] = it,
            c->igen[slot] = gen;
        if (check && (type_of_handle(h) != c->tt[slot] || type_of_handle(hi) != c->it[slot]))
            rt_fatal("vk_exec: kept texture types of stage %d slot %d differ from a lookup", st, slot);
        a->tex_type[slot] = c->tt[slot], a->img_type[slot] = c->it[slot], a->img_format[slot] = c->ifmt[slot];
    }
    if (locked) vk_texture_unlock();
}


static struct { const uint8_t *ctl; const GfxShader *s; } probe_sh[16];
static int n_probe_sh;
void vk_exec_probe_shader(const uint8_t *ctl, const GfxShader *s) {
    if (n_probe_sh < 16) probe_sh[n_probe_sh].ctl = ctl, probe_sh[n_probe_sh].s = s, n_probe_sh++;
}
static const GfxShader *shader_for(VkExec *ex, int st, int *present, int *pending, uint32_t await_ms) {
    const uint8_t *ctl, *code;
    uint32_t len;
    *present = 0;
    if (pending) *pending = 0;
    if (!ex->prog || !find_stage(ex->dc, ex->prog, st, &ctl, &code, &len)) return NULL;
    *present = 1;
    for (int i = 0; i < n_probe_sh; i++)
        if (probe_sh[i].ctl == ctl) return probe_sh[i].s;
    GfxShaderAnswers a;
    uint64_t serial = ex->prog->serial;
    answers_for(ex, st, ctl, slot_mask_of(ex->dc, serial, st), &a);
    a.mvprev = (uint8_t)(ex->dc->sf_mvprev && st == NVN_STAGE_VS);
    ShaderEnt *set = NULL;
    if (serial) {
        uint64_t h = (serial * 0x9E3779B97F4A7C15ull ^ (uint64_t)st) ^ nvn_hm_hash(&a, sizeof a);
        set = shader_tab[(h ^ h >> 29) & (SHADER_SETS - 1)];
        int lk = vk_exec_shared();
        if (lk) plat_mutex_lock(&shader_lock);
        for (int w = 0; w < SHADER_WAYS && set[w].used; w++) {
            ShaderEnt *e = &set[w];
            if (e->serial != serial || e->stage != st || memcmp(&e->a, &a, sizeof a)) continue;
            if (w) {
                ShaderEnt hit = *e;
                memmove(&set[1], &set[0], w * sizeof *set);
                set[0] = hit;
            }
            const GfxShader *found = set[0].s;
            if (lk) plat_mutex_unlock(&shader_lock);
            return found;
        }
        if (lk) plat_mutex_unlock(&shader_lock);
    }
    int gs = st == NVN_STAGE_VS ? GFX_STAGE_VS : st == NVN_STAGE_FS ? GFX_STAGE_FS : st == NVN_STAGE_TASK ? GFX_STAGE_TS
           : st == NVN_STAGE_MESH ? GFX_STAGE_MS : GFX_STAGE_CS;
    int late = 0;
    if (await_ms && !pending) pending = &late;
    const GfxShader *s = await_ms ? gfx_shader_await(gs, ctl, code, len, &a, await_ms, pending)
                       : pending && (vk_pipeline_async_on() || a.mvprev) ? gfx_shader_get_async(gs, ctl, code, len, &a, pending)
                                                           : gfx_shader_get(gs, ctl, code, len, NULL, &a);
    if (pending && *pending) return NULL;
    if (s) slot_mask_learn(ex->dc, serial, st, s);
    if (!s) warn_once(W_SHADER, "a stage %d shader of program %llu failed to translate: draws with it are skipped", st,
                      (unsigned long long)serial);
    else if (set) {
        int lk = vk_exec_shared();
        if (lk) plat_mutex_lock(&shader_lock);
        memmove(&set[1], &set[0], (SHADER_WAYS - 1) * sizeof *set);
        set[0] = (ShaderEnt){.serial = serial, .stage = st, .used = 1, .a = a, .s = s};
        if (lk) plat_mutex_unlock(&shader_lock);
    }
    return s;
}



static int nvn_stage_of(int gfx_stage) {
    if (gfx_stage == GFX_STAGE_CS && dc->mfb_stage) return dc->mfb_stage;
    return gfx_stage == GFX_STAGE_VS ? NVN_STAGE_VS : gfx_stage == GFX_STAGE_FS ? NVN_STAGE_FS
         : gfx_stage == GFX_STAGE_TS ? NVN_STAGE_TASK : gfx_stage == GFX_STAGE_MS ? NVN_STAGE_MESH : NVN_STAGE_CS;
}

static VkDescriptorBufferInfo zero_info(uint64_t max) {
    return (VkDescriptorBufferInfo){Z.zbuf, 0, max < ZERO_BYTES ? max : ZERO_BYTES};
}


static void count_use(DrawCtx *cx, int cls, int use, uint64_t n) {
    vk_mem_count(cls, use, n);
    if (cx->pb_cap) {
        if (cx->pb_cap->has) cx->pb_spoiled = 1;
        else *cx->pb_cap = (PbCount){cls, use, 1, n};
    }
    if (!cx->RS.rec) return;
    for (int i = 0; i < cx->RS.ncnt; i++)
        if (cx->RS.cnt[i].cls == cls && cx->RS.cnt[i].use == use) {
            cx->RS.cnt[i].bytes += n;
            return;
        }
    if (cx->RS.ncnt < (int)(sizeof cx->RS.cnt / sizeof cx->RS.cnt[0])) cx->RS.cnt[cx->RS.ncnt++] = (RsCount){cls, use, n};
    else cx->RS.cnt_over = 1;
}

static int bind_range(VkExec *ex, uint64_t addr, uint64_t size, uint64_t align, uint64_t max, int use, int written,
                      VkDescriptorBufferInfo *out) {
    VkMemSpan sp;
    if (!addr || !vk_mem_resolve(addr, 1, &sp)) return 0;
    uint64_t n = size && size < sp.avail ? size : sp.avail;
    if (n > max) n = max;
    if (!n) return 0;
    if (sp.offset % align == 0) {
        VK_MEM_USE(sp);
        *out = (VkDescriptorBufferInfo){sp.buffer, sp.offset, n};
        if (written) vk_mem_gpu_wrote_shader(&sp, n), vk_usnap_drop(addr, n);
        count_use(ex->dc, sp.cls, use, n);
        VK_MEM_BOUND(sp);
        return 1;
    }
    if (sp.alloc) return 0;
    VkBuffer b;
    VkDeviceSize o;
    ex->dc->RS.volatile_ = 1;
    ex->dc->pb_spoiled = 1;
    memcpy(scratch(ex, n, &b, &o), (const void *)(uintptr_t)addr, n);
    vk_mem_count(sp.cls, VK_MUSE_CPU_READ, n);
    vk_mem_count(VK_MCLS_SCRATCH, use, n);
    *out = (VkDescriptorBufferInfo){b, o, n};
    return 1;
}

static void c0_table(uint8_t *at, const p_bind_buf b[16], int rows) {
    for (int r = 0; r < rows; r++) {
        uint64_t a = b[r].addr;
        uint32_t n = (uint32_t)b[r].size;
        memcpy(at + 16 * r, &a, 8);
        memcpy(at + 16 * r + 8, &n, 4);
    }
}

static int c0_ssbo_base(int stage) {
    return stage == NVN_STAGE_VS ? 0x110 : stage == NVN_STAGE_FS ? 0x510 : stage == NVN_STAGE_CS ? 0x310
         : stage == NVN_STAGE_TASK ? 0x670 : stage == NVN_STAGE_MESH ? 0x770 : -1;
}

void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance) {
    memset(bank, 0, VK_C0_BYTES);
    int base = c0_ssbo_base(stage);
    if (base < 0) return;
    int rows = (int)(VK_C0_BYTES - (uint32_t)base) / 16;
    c0_table(bank + base, ssbo, rows < 16 ? rows : 16);
    if (stage == NVN_STAGE_TASK || stage == NVN_STAGE_MESH) return;
    if (stage == NVN_STAGE_CS) {
        memcpy(bank + 0x200, dispatch, 12);
        c0_table(bank + 0x210, ubo, 16);
        return;
    }
    const float max_point = 2047.0f;
    memcpy(bank + 0x610, &max_point, 4);
    memcpy(bank + 0x644, &base_instance, 4);
}

static int c0_on(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_FIX_C0");
    }
    return on;
}

static void nvn2_cs_tables(const uint8_t *ctl, uint32_t *ubos, uint32_t *ssbos);
static uint32_t nvn2_cs_tables_at(const uint8_t *ctl);

static VkDescriptorBufferInfo c0_info(VkExec *ex, int st) {
    DrawCtx *const cx = ex->dc;
    int k = st == NVN_STAGE_VS ? 0 : st == NVN_STAGE_FS ? 1 : st == NVN_STAGE_TASK ? 3 : st == NVN_STAGE_MESH ? 4 : 2;
    if (k < 2 && cx->B.c0_valid[k] && cx->B.c0_in[k].binst == cx->c0_binst &&
        !memcmp(cx->B.c0_in[k].ssbo, ex->ssbo[st], sizeof cx->B.c0_in[k].ssbo))
        return (VkDescriptorBufferInfo){cx->B.c0_buf[k], cx->B.c0_off[k], VK_C0_BYTES};
    typeof(cx->B.c0_in[0]) in;
    memset(&in, 0, sizeof in);
    memcpy(in.ssbo, ex->ssbo[st], sizeof in.ssbo);
    if (st == NVN_STAGE_CS) memcpy(in.ubo, ex->ubo[st], sizeof in.ubo), memcpy(in.dim, cx->c0_dim, sizeof in.dim);
    else in.binst = cx->c0_binst;
    const uint8_t *pctl, *pcode;
    uint32_t plen;
    int nvn2_push = st == NVN_STAGE_CS && ex->prog && find_stage(ex->dc, ex->prog, st, &pctl, &pcode, &plen) && ctl_nvn2(pctl);
    if (nvn2_push) memcpy(in.push, ex->cs_push, sizeof in.push);
    if (cx->B.c0_valid[k] && !memcmp(&in, &cx->B.c0_in[k], sizeof in))
        return (VkDescriptorBufferInfo){cx->B.c0_buf[k], cx->B.c0_off[k], VK_C0_BYTES};
    cx->B.c0_in[k] = in;
    uint8_t tmp[VK_C0_BYTES];
    vk_c0_fill(tmp, st, ex->ssbo[st], ex->ubo[st], cx->c0_dim, cx->c0_binst);
    if (nvn2_push) {
        uint32_t nu, ns, ta = nvn2_cs_tables_at(pctl);
        nvn2_cs_tables(pctl, &nu, &ns);
        uint32_t pn = ta - 0x10 < sizeof ex->cs_push ? ta - 0x10 : sizeof ex->cs_push;
        memcpy(tmp + 0x10, ex->cs_push, pn < VK_C0_BYTES - 0x10 ? pn : VK_C0_BYTES - 0x10);
        memcpy(tmp, cx->c0_dim, 12);
        for (uint32_t k = 0; k < nu + ns && ta + 16 * k + 12 <= VK_C0_BYTES; k++) {
            const p_bind_buf *sb = k < nu ? (k < 18 ? &ex->ubo[st][k] : NULL) : (k - nu < 16 ? &ex->ssbo[st][k - nu] : NULL);
            if (!sb) continue;
            uint32_t sz = (uint32_t)sb->size;
            memcpy(tmp + ta + 16 * k, &sb->addr, 8), memcpy(tmp + ta + 16 * k + 8, &sz, 4);
        }
    }
    if (!cx->B.c0_valid[k] || memcmp(tmp, cx->B.c0[k], VK_C0_BYTES)) {
        memcpy(scratch(ex, VK_C0_BYTES, &cx->B.c0_buf[k], &cx->B.c0_off[k]), tmp, VK_C0_BYTES);
        memcpy(cx->B.c0[k], tmp, VK_C0_BYTES);
        cx->B.c0_valid[k] = 1;
    }
    return (VkDescriptorBufferInfo){cx->B.c0_buf[k], cx->B.c0_off[k], VK_C0_BYTES};
}

static struct { uint64_t ubo_max, ubo_align, sbo_max, sbo_align; } LIM;

static void limits_init(void) {
    const VkPhysicalDeviceLimits *l = &vk_ctx()->props.limits;
    LIM.ubo_align = l->minUniformBufferOffsetAlignment, LIM.sbo_max = l->maxStorageBufferRange;
    LIM.sbo_align = l->minStorageBufferOffsetAlignment;
    __atomic_store_n(&LIM.ubo_max, l->maxUniformBufferRange, __ATOMIC_RELEASE);
}


static VkDescriptorBufferInfo ubo_info(VkExec *ex, int st, int slot) {
    DrawCtx *const cx = ex->dc;
    if (cx->mo_ovr_on && (st == NVN_STAGE_VS || st == NVN_STAGE_FS) && slot >= 0 && slot < 32 &&
        cx->mo_ovr[st][slot].buffer)
        return cx->mo_ovr[st][slot];
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.ubo_max, align = LIM.ubo_align;
    VkDescriptorBufferInfo bi;
    if (slot == 0 && c0_on()) return c0_info(ex, st);
    if (slot == 1 && ex->prog) {
        const uint8_t *ctl, *code;
        uint32_t len, c1;
        if (find_stage(ex->dc, ex->prog, st, &ctl, &code, &len)) {
            c1 = ctl_c1(ctl, len);
            if (ctl_nvn2(ctl)) memcpy(&len, ctl + 0x44, 4);
            if (c1 < len && bind_range(ex, (uint64_t)(uintptr_t)code + c1, len - c1, align, max, VK_MUSE_UBO, 0, &bi))
                return bi;
        }
    } else if (slot >= 3 && slot - 3 < 18) {
        const p_bind_buf *b = &ex->ubo[st][slot - 3];
        if (b->size && vk_usnap_find(b->addr, b->size < max ? b->size : max, align, &bi)) {
            count_use(ex->dc, VK_MCLS_SCRATCH, VK_MUSE_UBO, bi.range);
            return bi;
        }
        if (bind_range(ex, b->addr, b->size, align, max, VK_MUSE_UBO, 0, &bi)) return bi;
    }
    return zero_info(max);
}

// Contains code Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
static int ssbo_slot(const GfxShaderRes *r, int st, int *ubo) {
    *ubo = 0;
    if (r->cbuf_slot != 0) return -1;
    int off = r->handle * 4, base = c0_ssbo_base(st);
    if (st == NVN_STAGE_CS && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 == 0) {
        *ubo = 1;
        return (off - 0x210) / 16;
    }
    return base >= 0 && off >= base && off < base + 0x100 && (off - base) % 16 == 0 ? (off - base) / 16 : -1;
}

static int ubo_ptr_res(const GfxShaderRes *r) {
    return r->cbuf_slot >= 3 && r->cbuf_slot - 3 < 18;
}

#define UPD_N 4096
static struct {
    uint64_t fence;
    uint32_t n;
    struct { uint64_t lo, hi; const uint8_t *data; } u[UPD_N];
} UPD;

static void upd_note(const VkExec *ex, const p_update_ub *u) {
    if (UPD.fence != ex->fence) UPD.fence = ex->fence, UPD.n = 0;
    UPD.u[UPD.n++ % UPD_N] = (typeof(UPD.u[0])){u->dst, u->dst + u->size, u->data};
}

static int upd_read(const VkExec *ex, uint64_t a, void *out, size_t n) {
    if (UPD.fence != ex->fence) return 0;
    uint32_t top = vk_exec_upd_top(UPD.n);
    for (uint32_t i = top, end = top > UPD_N ? top - UPD_N : 0; i-- > end;) {
        const typeof(UPD.u[0]) *e = &UPD.u[i % UPD_N];
        if (a >= e->hi || a + n <= e->lo) continue;
        if (a < e->lo || a + n > e->hi) return 0;
        memcpy(out, e->data + (a - e->lo), n);
        return 1;
    }
    return 0;
}

static uint64_t ubo_ptr(const VkExec *ex, int st, const GfxShaderRes *r) {
    const p_bind_buf *u = &ex->ubo[st][r->cbuf_slot - 3];
    uint64_t off = (uint64_t)r->handle * 4, a = 0;
    VkMemSpan sp;
    if (!u->addr || u->size < off + 8 || upd_read(ex, u->addr + off, &a, 8)) return a;
    if (vk_mem_lookup(u->addr + off, 8, &sp)) memcpy(&a, (const void *)(uintptr_t)(u->addr + off), 8);
    return a;
}

static void nvn2_cs_tables(const uint8_t *ctl, uint32_t *ubos, uint32_t *ssbos) {
    uint32_t w;
    memcpy(&w, ctl + 0xb0, 4);
    *ubos = w >> 8 & 0xff, *ssbos = w >> 16 & 0xff;
}

static uint32_t nvn2_cs_tables_at(const uint8_t *ctl) {
    uint32_t b0, b4, b8;
    memcpy(&b0, ctl + 0xb0, 4), memcpy(&b4, ctl + 0xb4, 4), memcpy(&b8, ctl + 0xb8, 4);
    uint32_t rest = 16 * ((b0 >> 8 & 0xff) + (b0 >> 16 & 0xff)) + 8 * ((b0 >> 24) + (b4 >> 16 & 0xff)), end = b8 >> 16;
    if (!(b0 >> 8 & 0xffff)) return 0x10 + 1024;
    return end >= 0x10 + rest ? end - rest : 0x10;
}

static int push_ptr(const VkExec *ex, int st, const GfxShaderRes *r, p_bind_buf *out) {
    const uint8_t *pctl, *pcode;
    uint32_t plen;
    uint32_t o = (uint32_t)r->handle * 4;
    if (st != NVN_STAGE_CS || r->cbuf_slot != 0 || o < 0x10 || !ex->prog || !find_stage(ex->dc, ex->prog, st, &pctl, &pcode, &plen) ||
        !ctl_nvn2(pctl))
        return 0;
    uint32_t nu, ns, ta = nvn2_cs_tables_at(pctl);
    nvn2_cs_tables(pctl, &nu, &ns);
    if (o < ta) {
        uint64_t a;
        uint32_t n = 0;
        if (o - 0x10 + 8 > sizeof ex->cs_push) return 0;
        memcpy(&a, ex->cs_push + (o - 0x10), 8);
        if (o - 0x10 + 12 <= sizeof ex->cs_push) memcpy(&n, ex->cs_push + (o - 0x10) + 8, 4);
        *out = (p_bind_buf){0, 0, a, n < (1u << 30) ? n : 0};
        return 1;
    }
    uint32_t t = (o - ta) / 16;
    if ((o - ta) % 16 == 0 && t < nu + ns) {
        *out = t < nu ? (t < 18 ? ex->ubo[st][t] : (p_bind_buf){0}) : (t - nu < 16 ? ex->ssbo[st][t - nu] : (p_bind_buf){0});
        return 1;
    }
    static int logged;
    if (logged++ < 16) {
        uint32_t b0, b4, b8;
        memcpy(&b0, pctl + 0xb0, 4), memcpy(&b4, pctl + 0xb4, 4), memcpy(&b8, pctl + 0xb8, 4);
        rt_log("vk: NVN2 compute c0+%#x is past the tables (prog %llu, +0xb0 %08x +0xb4 %08x +0xb8 %08x, tables at %#x)", o,
               (unsigned long long)ex->prog->serial, b0, b4, b8, ta);
    }
    return 0;
}

static const p_bind_buf *ssbo_direct(const VkExec *ex, int st, const GfxShaderRes *r, int *ubo, p_bind_buf *tmp) {
    int slot = ssbo_slot(r, st, ubo);
    *tmp = (p_bind_buf){0};
    if (slot >= 0 && slot < 16) return *ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot];
    if (ubo_ptr_res(r)) return tmp->addr = ubo_ptr(ex, st, r), tmp;
    return push_ptr(ex, st, r, tmp) ? tmp : NULL;
}

static uint64_t ptr_addr(const VkExec *ex, int st, const GfxShaderRes *r) {
    p_bind_buf tmp;
    int ubo;
    const p_bind_buf *b = ssbo_direct(ex, st, r, &ubo, &tmp);
    uint64_t off = r->ptr - 1u, a = 0;
    VkMemSpan sp;
    if (!b || !b->addr || (b->size && b->size < off + 8) || upd_read(ex, b->addr + off, &a, 8)) return a;
    if (vk_mem_lookup(b->addr + off, 8, &sp)) memcpy(&a, (const void *)(uintptr_t)(b->addr + off), 8);
    return a;
}

uint64_t vk_exec_storage_addr(const VkExec *ex, int st, const GfxShaderRes *r) {
    p_bind_buf tmp;
    int ubo;
    if (r->ptr) return ptr_addr(ex, st, r);
    const p_bind_buf *b = ssbo_direct(ex, st, r, &ubo, &tmp);
    return b ? b->addr : 0;
}

uint64_t vk_exec_cs_storage_addr(const VkExec *ex, const GfxShaderRes *r) { return vk_exec_storage_addr(ex, NVN_STAGE_CS, r); }

static VkDescriptorBufferInfo ssbo_info(VkExec *ex, int st, const GfxShaderRes *r, int written) {
    if (r->cbuf_slot == VK_GPA_CB_SLOT) {
        vk_mem_shadow_table_read();
        vk_mem_note_live();
        VkDescriptorBufferInfo t = vk_gpa_info();
        return t.buffer ? t : (VkDescriptorBufferInfo){Z.zbuf, 0, VK_WHOLE_SIZE};
    }
    if (r->cbuf_slot >= MESHFB_CB_TASK && r->cbuf_slot <= MESHFB_CB_LIST) return vk_meshfb_buffer(r->cbuf_slot);
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.sbo_max, align = LIM.sbo_align;
    VkDescriptorBufferInfo bi;
    int ubo = 0;
    p_bind_buf up = {0};
    const p_bind_buf *b;
    if (r->ptr) up.addr = ptr_addr(ex, st, r), b = &up;
    else b = ssbo_direct(ex, st, r, &ubo, &up);
    if (b) {
        uint64_t lead = vk_caps.sbuf_align ? b->addr & (vk_caps.sbuf_align - 1) : 0;
        if (bind_range(ex, b->addr - lead, b->size ? b->size + lead : 0, align, max, ubo ? VK_MUSE_UBO : VK_MUSE_SSBO,
                       written, &bi)) {
            if (written && bi.buffer != VK_NULL_HANDLE) note_written(ex, b->addr - lead, bi.range);
            return bi;
        }
        if (b->addr) warn_once(W_SSBO_ALIGN, "storage buffer %#llx is not backed or not aligned: a zero buffer is bound",
                               (unsigned long long)b->addr);
    } else {
        warn_once(W_SSBO_SLOT, "a storage buffer whose address is at c%d+%#x (stage %d) is not replayed: a zero buffer is bound",
                  r->cbuf_slot, r->handle * 4, st);
        static int logged;
        const uint8_t *ctl, *code;
        uint32_t len;
        if (logged < 4 && r->cbuf_slot == 1 && ex->prog && find_stage(ex->dc, ex->prog, st, &ctl, &code, &len)) {
            uint32_t c1s, c1e, size;
            memcpy(&size, ctl + 0x38, 4), memcpy(&c1s, ctl + 0x40, 4), memcpy(&c1e, ctl + 0x44, 4);
            char line[512];
            int o = snprintf(line, sizeof line, "vk: c1 storage address, prog %llu: code %#x, c1 [%#x, %#x), len %#x; c1 words:",
                             (unsigned long long)ex->prog->serial, size, c1s, c1e, len);
            for (uint32_t k = 0; k < 4 && c1s + 4 * k + 4 <= len; k++) {
                uint32_t wv;
                memcpy(&wv, code + c1s + 4 * k, 4);
                o += snprintf(line + o, sizeof line - o, " %08x", wv);
            }
            for (int k = 0; k < 6 && o < (int)sizeof line - 40; k++)
                if (ex->ubo[st][k].addr)
                    o += snprintf(line + o, sizeof line - o, " ubo%d=%#llx+%#x", k + 3, (unsigned long long)ex->ubo[st][k].addr,
                                  (unsigned)ex->ubo[st][k].size);
            rt_log("%s", line);
            logged++;
        }
    }
    if (written && Z.sink) return (VkDescriptorBufferInfo){Z.sink, 0, max < ZERO_BYTES ? max : ZERO_BYTES};
    return zero_info(max);
}

static int in_fbl(const VkExec *ex, VkImage img) {
    for (int i = 0; i < ex->nfbl; i++)
        if (ex->fbl_img[i] == img) return 1;
    return 0;
}

typedef struct { VkImage image; uint32_t l0, l1, a0, a1; } SubRange;

static SubRange sub_of(const VkTexImage *img, const view_t *v, int hv) {
    SubRange r = {img->image, 0, img->levels, 0, img->layers};
    if (hv && v->set_levels) r.l0 = v->base_level, r.l1 = v->base_level + (v->nlevels ? v->nlevels : img->levels);
    if (hv && v->set_layers) r.a0 = v->min_layer, r.a1 = v->min_layer + (v->nlayers ? v->nlayers : img->layers);
    return r;
}

static uint32_t attached_as(const VkExec *ex, const SubRange *r) {
    uint32_t m = 0;
    if (!r->image) return 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || t->image != r->image) continue;
        if (t->level < r->l0 || t->level >= r->l1) continue;
        if (!t->is3d && (t->base_layer + t->layers <= r->a0 || t->base_layer >= r->a1)) continue;
        m |= 1u << i;
    }
    return m;
}

static int knob_log_mesh(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("LOG_MESH", "0");
    return on;
}
static int knob_tm_dispatch(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("TM_DISPATCH", "1");
    return on;
}
static int knob_log_cs_binds(void) {
    static int on = -1;
    if (on < 0) on = cfg_get("NX2SARIA_LOG_CS_BINDS") != NULL;
    return on;
}

static int td_mode(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_value("NX2SARIA_TEX_MEMO");
        on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
    }
    return on;
}

#define FM_N 8192
typedef struct { uint64_t h, tok; uint32_t gen; SubRange r; } FmEnt;

static SubRange sampled_sub(uint64_t h) {
    view_t v;
    int hv;
    const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv);
    return img ? sub_of(img, &v, hv) : (SubRange){0};
}

static uint32_t feedback_scan(VkExec *ex, const GfxShader *fs) {
    if (ex->fb_fs == fs && ex->fb_serial == ex->tex_serial && ex->fb_pass == ex->pass_serial) return ex->fb_sampled;
    const GfxShaderRes *res;
    int n = gfx_shader_resources(fs, GFX_RES_TEX, &res);
    uint32_t m = 0, gen = vk_texture_slots_gen();
    int memo = td_mode(), locked = 0;
    FmEnt *fm = ex->dc->fm;
    if (!fm && !(fm = ex->dc->fm = calloc(FM_N, sizeof *fm))) rt_fatal("vk: out of memory (feedback memo)");
    for (int r = 0; r < n; r++) {
        if (res[r].pool || res[r].slot < 0) continue;
        int count = res[r].count ? res[r].count : 1;
        if (count > 128 - res[r].slot) count = 128 - res[r].slot;
        for (int k = 0; k < count; k++) {
            int slot = res[r].slot + k;
            uint64_t h = ex->tex[NVN_STAGE_FS][slot];
            if (!h) continue;
            FmEnt *f = &fm[(h * 0x9E3779B97F4A7C15ull) >> 51];
            if (memo && f->h == h && slot_still(&f->gen, f->tok, h, gen)) {
                if (memo == 2) {
                    if (!locked) vk_texture_lock(), locked = 1;
                    SubRange c = sampled_sub(h);
                    if (memcmp(&c, &f->r, sizeof c))
                        rt_fatal("vk_exec: feedback memo of handle %#llx differs from a lookup", (unsigned long long)h);
                }
                m |= attached_as(ex, &f->r);
                continue;
            }
            uint64_t tok = token_of(h);
            if (!locked) vk_texture_lock(), locked = 1;
            SubRange sr = sampled_sub(h);
            m |= attached_as(ex, &sr);
            if (tok) *f = (typeof(*f)){.h = h, .tok = tok, .gen = gen, .r = sr};
            else f->h = 0;
        }
    }
    if (locked) vk_texture_unlock();
    ex->fb_fs = fs, ex->fb_serial = ex->tex_serial, ex->fb_pass = ex->pass_serial, ex->fb_sampled = m;
    return m;
}

static uint32_t feedback_prepare(VkExec *ex, const GfxShader *fs) {
    int mode = vk_ctx()->feedback;
    if (mode == VK_FEEDBACK_OFF || !fs || !ex->in_pass) return 0;
    uint32_t m = feedback_scan(ex, fs);
    uint32_t d = m & ex->fb_dirty;
    if (d) vk_exec_frame.fb_draws++;
    if (d & ~(mode == VK_FEEDBACK_LAYOUT ? ex->fbl_mask : 0)) {
        if (mode == VK_FEEDBACK_LAYOUT)
            for (uint32_t i = 0; i < 9; i++) {
                const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
                if ((d >> i & 1) && t->img && t->img->fb_usage) ((VkTexImage *)t->img)->fb_learned = 1;
            }
        vk_exec_pass_end(ex);
        vk_exec_pass_begin(ex);
        vk_exec_frame.fb_restarts++;
    }
    return mode == VK_FEEDBACK_LAYOUT ? m & ex->fbl_mask : 0;
}

static void feedback_barrier(VkExec *ex) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
                           .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    ex->fb_dirty = 0;
    vk_exec_frame.fb_barriers++;
}

void vk_exec_feedback_war(VkExec *ex, uint32_t written) {
    if (!(written & ex->fb_read)) return;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
                           .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    ex->fb_read = 0;
    vk_exec_frame.fb_war_barriers++;
}

static uint32_t feedback_writes(const VkExec *ex, const VkPipeState *s) {
    uint32_t w = ex->depth.image && ((s->ds.dt && s->ds.dw) || s->ds.st) ? 0x100u : 0;
    for (uint32_t i = 0; i < ex->pass_ncolor; i++)
        if (ex->color[i].image && vk_pipeline_target_wmask(s, (int)i)) w |= 1u << i;
    return w;
}

static void feedback_written(VkExec *ex, const VkPipeState *s, uint32_t sampled) {
    if (vk_ctx()->feedback == VK_FEEDBACK_OFF) return;
    uint32_t w = feedback_writes(ex, s);
    vk_exec_feedback_war(ex, w);
    ex->fb_dirty |= w;
    ex->fb_read |= sampled;
}

static int tex_info_full(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                         VkDescriptorImageInfo *out, VkImage *image);

#define TD_N 8192
static struct {
    uint64_t h, tok;
    uint32_t gen, epoch, sgen;
    uint16_t type;
    uint8_t separate, valid;
    uint8_t reduced;
    VkImage image;
    VkDescriptorImageInfo out;
} TD[TD_N];

static void tex_info_lookup(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                            VkDescriptorImageInfo *out, VkImage *img_out) {
    int td_on = td_mode();
    VkImage image = VK_NULL_HANDLE;
    if (!h || storage || !td_on) {
        tex_info_full(ex, r, h, storage, separate, out, &image);
        if (img_out) *img_out = image;
        return;
    }
    uint32_t gen = vk_texture_slots_gen(), ep = vk_texture_epoch(), sg = vk_sampler_gen();
    typeof(TD[0]) *m = &TD[(h * 0x9E3779B97F4A7C15ull ^ (uint64_t)r->type << 7 ^ (uint64_t)separate) >> 51];
    if (m->valid && m->h == h && m->type == r->type && m->separate == separate && m->sgen == sg &&
        !(m->reduced && r->exact) && slot_still(&m->gen, m->tok, h, gen)) {
        if (m->epoch != ep) {
            vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL);
            m->epoch = ep;
        }
        *out = m->out;
        out->imageLayout = m->image && ex->nfbl && in_fbl(ex, m->image) ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                          : VK_IMAGE_LAYOUT_GENERAL;
        if (img_out) *img_out = m->image;
        if (td_on == 2) {
            VkDescriptorImageInfo full;
            tex_info_full(ex, r, h, storage, separate, &full, &image);
            if (full.sampler != out->sampler || full.imageView != out->imageView || full.imageLayout != out->imageLayout)
                rt_fatal("vk_exec: texture descriptor memo of handle %#llx type %#x differs from a lookup",
                         (unsigned long long)h, r->type);
        }
        return;
    }
    uint64_t tok = token_of(h);
    int reduced = tex_info_full(ex, r, h, storage, separate, out, &image);
    *m = (typeof(*m)){.h = h, .tok = tok, .gen = gen, .epoch = ep, .sgen = sg, .type = r->type,
                      .separate = (uint8_t)separate, .valid = 1, .reduced = (uint8_t)reduced, .image = image, .out = *out};
    if (img_out) *img_out = image;
}

static void direct_blank(uint32_t id) {
    static uint32_t seen[256];
    static uint64_t frame = ~0ull;
    static int n;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, n = 0;
    for (int i = 0; i < n; i++)
        if (seen[i] == id) return;
    if (n == (int)(sizeof seen / sizeof *seen)) return;
    seen[n++] = id;
    uint64_t addr;
    uint32_t fmt, w, h, why = vk_texture_slot_why_locked((int)id, &addr, &fmt, &w, &h);
    flight_tex(FLIGHT_TEX_DIRECT, addr, fmt, w, h, 0, 0, why, id);
}

static int tex_info_full(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                         VkDescriptorImageInfo *out, VkImage *image) {
    int ms = (r->type & 0x200) != 0, reduced = 0;
    *image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler samp = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    if (h) {
        view_t v;
        int hv;
        const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv);
        if (img && img->skip && r->exact) img = vk_texture_exact_by_id_locked((int)(h & 0xfffff), &v, &hv);
        reduced = img && img->skip;
        if (img) view = get_view(ex, img, &v, hv, r->type, storage), *image = img->image;
        if (img && storage) vk_texture_storage_bound_locked(img);
        if (img && !storage && ex->nfbl && in_fbl(ex, img->image)) layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        if (!storage && !separate) {
            int sid = (int)(h >> 20 & 0xfff);
            int point = img && (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && !(r->type & 0x400) &&
                        vk_exec_view_aspect(img, &v, hv) != VK_IMAGE_ASPECT_STENCIL_BIT;
            samp = point ? vk_sampler_point_by_id_locked(sid)
                 : r->type & 0x400 ? vk_sampler_compare_by_id_locked(sid)
                 : reduced ? vk_sampler_detail_by_id_locked(sid, img->skip) : vk_sampler_by_id_locked(sid);
        }
    }
    if (!view) {
        if (h && !(h >> 63) && !storage && flight_on) direct_blank((uint32_t)(h & 0xfffff));
        int di = dummy_index(res_view_type(r->type), ms);
        view = storage ? storage_dummy(di, r->format) : Z.img[di].view;
    }
    if (!samp) samp = r->type & 0x400 ? Z.samp_shadow : Z.samp;
    *out = (VkDescriptorImageInfo){separate || storage ? VK_NULL_HANDLE : samp, view, layout};
    return reduced;
}

volatile uint32_t vk_pool_kinds_used;
typedef struct { uint64_t kh; uint32_t kreg, stamp, fmask; } PoolSlot;
#define POOL_SETS VK_POOL_SETS
#define POOL_LOG (1u << 20)
static struct {
    VkDescriptorSetLayout layout, empty;
    VkDescriptorPool dpool;
    VkDescriptorSet set;
    VkDescriptorSet sets[POOL_SETS];
    uint64_t set_fence[POOL_SETS];
    uint64_t set_synced[POOL_SETS];
    int nsets, cur, all;
    uint64_t cur_fence;
    uint32_t *log;
    uint64_t logn;
    uint64_t *last;
    uint64_t switches, waits, catchup, written, rewrites, catch_ns, sync_ns, n_slots, n_refill, n_fresh;
    uint64_t n_walk[2], walk_visit[2], walk_refill[2], walk_ns[2];
    uint64_t n_visit;
    uint64_t n_gwalk, n_gskip, n_gskip_slots, n_grep;
    uint64_t n_gchk_writes, n_gchk_groups;
    uint64_t n_gchk_pending, n_gchk_bad;
    uint64_t n_gnouni, n_gchanged;
    int gmode;
    uint64_t n_by_reg, n_by_addr;
    uint64_t full_ns, n_full;
    uint64_t ep_ns, n_ep, n_ep_ent;
    uint64_t unres_ns, n_unres_walks;
    uint32_t unres_mg;
    int unres_more;
    PoolSlot *ps;
    uint8_t *unres_on;
    uint8_t *unres_in;
    int *unres, nunres, cunres;
    uint32_t *ent;
    uint64_t dirty_pos;
    int ready, uab, sealed;
    uint32_t kinds, gen, epoch, sgen;
    int retry;
    uint64_t frame, frame_ns;
    uint64_t n_frame_stops, n_defer_skips;
    uint64_t *tok;
    VkImageView *view;
    VkSampler *samp;
    uint64_t writes;
    VkBuffer marks;
    VmaAllocation marks_alloc;
    struct { VkBuffer buf; VmaAllocation alloc; const uint32_t *p; uint64_t fence; } rb[4];
    int rb_head, rb_n, marks_ready;
    uint32_t marks_since;
} PL;

static void pool_marks_make(void);

static void pool_limits(VkCtx *c, uint64_t *set, uint64_t *stage) {
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    if (PL.uab) {
        *set = p12.maxDescriptorSetUpdateAfterBindSampledImages;
        *stage = p12.maxPerStageDescriptorUpdateAfterBindSampledImages;
    } else {
        *set = p2.properties.limits.maxDescriptorSetSampledImages;
        *stage = p2.properties.limits.maxPerStageDescriptorSampledImages;
    }
}

static VkDescriptorSetLayout pool_layout_make(void) {
    VkCtx *c = vk_ctx();
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    PL.uab = f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingPartiallyBound &&
             f12.descriptorBindingUpdateUnusedWhilePending;
    uint64_t need = (uint64_t)VK_POOL_KINDS * VK_POOL_TEX, lim_set = 0, lim_stage = 0;
    pool_limits(c, &lim_set, &lim_stage);
    int over = (lim_set && need > lim_set) || (lim_stage && need > lim_stage);
    rt_log("vk: bindless texture pools: %d kinds x %d textures = %llu sampled images; the device allows %llu in a set "
           "and %llu in a stage (%s limits)%s", VK_POOL_KINDS, VK_POOL_TEX, (unsigned long long)need,
           (unsigned long long)lim_set, (unsigned long long)lim_stage, PL.uab ? "update-after-bind" : "plain",
           over ? ": WARNING, over the device's limit" : "");
    int marks = vk_pool_marks_enabled();
    uint32_t nb = VK_POOL_KINDS + 1 + (marks ? 1u : 0u);
    VkDescriptorSetLayoutBinding b[VK_POOL_KINDS + 2];
    VkDescriptorBindingFlags fl[VK_POOL_KINDS + 2];
    VkDescriptorBindingFlags f = (f12.descriptorBindingPartiallyBound ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0) |
        (PL.uab ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT : 0);
    for (uint32_t k = 0; k <= VK_POOL_KINDS; k++) {
        b[k] = (VkDescriptorSetLayoutBinding){k, k < VK_POOL_KINDS ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
                                              k < VK_POOL_KINDS ? VK_POOL_TEX : VK_POOL_SAMP, VK_SHADER_STAGE_ALL, NULL};
        fl[k] = f;
    }
    if (marks) {
        b[VK_POOL_MARK_BINDING] = (VkDescriptorSetLayoutBinding){VK_POOL_MARK_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                                 VK_SHADER_STAGE_ALL, NULL};
        fl[VK_POOL_MARK_BINDING] = 0;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                      .bindingCount = nb, .pBindingFlags = fl};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bf,
                                          .flags = PL.uab ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0,
                                          .bindingCount = nb, .pBindings = b};
    VkDescriptorSetLayout l;
    if (vkCreateDescriptorSetLayout(c->device, &li, NULL, &l) != VK_SUCCESS)
        rt_fatal("vk: the texture pool layout failed: it holds %llu sampled images, and this device allows %llu in a "
                 "set and %llu in a stage (%s limits)", (unsigned long long)need, (unsigned long long)lim_set,
                 (unsigned long long)lim_stage, PL.uab ? "update-after-bind" : "plain");
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_POOL_KINDS * VK_POOL_TEX},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, VK_POOL_SAMP}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    uint32_t nps = marks ? 3 : 2;
    for (int n = POOL_SETS; n >= 1 && !PL.nsets; n--) {
        VkDescriptorPoolSize psn[3];
        for (uint32_t i = 0; i < nps; i++) psn[i] = (VkDescriptorPoolSize){ps[i].type, ps[i].descriptorCount * (uint32_t)n};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                         .flags = PL.uab ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0,
                                         .maxSets = (uint32_t)n, .poolSizeCount = nps, .pPoolSizes = psn};
        if (vkCreateDescriptorPool(c->device, &pi, NULL, &PL.dpool) != VK_SUCCESS) continue;
        VkDescriptorSetLayout ls[POOL_SETS];
        for (int i = 0; i < n; i++) ls[i] = l;
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = PL.dpool,
                                          .descriptorSetCount = (uint32_t)n, .pSetLayouts = ls};
        if (vkAllocateDescriptorSets(c->device, &ai, PL.sets) == VK_SUCCESS) PL.nsets = n;
        else vkDestroyDescriptorPool(c->device, PL.dpool, NULL), PL.dpool = VK_NULL_HANDLE;
    }
    if (!PL.nsets) rt_fatal("vk: the texture pool set failed");
    if (marks) pool_marks_make();
    PL.set = PL.sets[0];
    PL.log = malloc(sizeof *PL.log * POOL_LOG);
    PL.last = calloc((size_t)VK_POOL_KINDS * VK_POOL_TEX + VK_POOL_SAMP, sizeof *PL.last);
    if (!PL.log || !PL.last) rt_fatal("vk: out of memory");
    PL.tok = calloc(VK_POOL_TEX, sizeof *PL.tok);
    PL.view = calloc((size_t)VK_POOL_KINDS * VK_POOL_TEX, sizeof *PL.view);
    PL.samp = calloc(VK_POOL_SAMP, sizeof *PL.samp);
    PL.ps = calloc(VK_POOL_TEX, sizeof *PL.ps);
    PL.unres_on = calloc(VK_POOL_TEX, 1);
    PL.unres_in = calloc(VK_POOL_TEX, 1);
    PL.ent = calloc(VK_POOL_TEX, sizeof *PL.ent);
    if (!PL.tok || !PL.view || !PL.samp || !PL.ps || !PL.unres_on || !PL.unres_in || !PL.ent) rt_fatal("vk: out of memory");
    rt_log("vk: bindless texture pools: %d kinds x %d textures, %d samplers, %s, %d sets (one per submission in turn)%s",
           VK_POOL_KINDS, VK_POOL_TEX, VK_POOL_SAMP,
           PL.uab ? "update after bind" : "no update-after-bind on this GPU (entries written while bound are undefined)", PL.nsets,
           marks ? "; shaders mark the slots they sample (NX2SARIA_TEX_DISUSE)" : "");
    return l;
}

#define MARKS_EVERY 4
#define MARKS_RB 4

static void pool_marks_make(void) {
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)(VK_POOL_TEX + 2) * 4,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(c->vma, &bi, &ai, &PL.marks, &PL.marks_alloc, NULL) != VK_SUCCESS)
        rt_fatal("vk: the texture pool marks buffer failed");
    bi.size = (VkDeviceSize)(VK_POOL_TEX + 2) * 4, bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo hi = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    for (int i = 0; i < MARKS_RB; i++) {
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &hi, &PL.rb[i].buf, &PL.rb[i].alloc, &info) != VK_SUCCESS || !info.pMappedData)
            rt_fatal("vk: the texture pool marks read-back buffer failed");
        PL.rb[i].p = info.pMappedData;
    }
    VkDescriptorBufferInfo di = {PL.marks, 0, VK_WHOLE_SIZE};
    for (int i = 0; i < PL.nsets; i++) {
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = PL.sets[i],
                                  .dstBinding = VK_POOL_MARK_BINDING, .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &di};
        vkUpdateDescriptorSets(c->device, 1, &w, 0, NULL);
    }
    __atomic_store_n(&PL.marks_ready, 1, __ATOMIC_RELEASE);
}

static void pool_marks_release(VkCtx *c) {
    if (!PL.marks) return;
    __atomic_store_n(&PL.marks_ready, 0, __ATOMIC_RELEASE);
    vmaDestroyBuffer(c->vma, PL.marks, PL.marks_alloc);
    for (int i = 0; i < MARKS_RB; i++)
        if (PL.rb[i].buf) vmaDestroyBuffer(c->vma, PL.rb[i].buf, PL.rb[i].alloc), PL.rb[i].buf = VK_NULL_HANDLE;
    PL.marks = VK_NULL_HANDLE;
}

void vk_exec_pool_marks(VkExec *ex) {
    if (!__atomic_load_n(&PL.marks_ready, __ATOMIC_ACQUIRE)) return;
    VkCtx *c = vk_ctx();
    uint64_t done = vk_sync_fence_done();
    while (PL.rb_n) {
        int i = (PL.rb_head - PL.rb_n + MARKS_RB) % MARKS_RB;
        if (PL.rb[i].fence > done) break;
        vmaInvalidateAllocation(c->vma, PL.rb[i].alloc, 0, VK_WHOLE_SIZE);
        vk_pool_marks_read(PL.rb[i].p, VK_POOL_TEX);
        PL.rb_n--;
    }
    if (++PL.marks_since < MARKS_EVERY || PL.rb_n == MARKS_RB) return;
    PL.marks_since = 0;
    int i = PL.rb_head;
    PL.rb_head = (PL.rb_head + 1) % MARKS_RB, PL.rb_n++;
    PL.rb[i].fence = ex->fence;
    VkCommandBuffer cb = ex->cb;
    VkMemoryBarrier2 mb[2] = {
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT}};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = mb};
    vkCmdPipelineBarrier2(cb, &dep);
    VkBufferCopy region = {0, 0, (VkDeviceSize)(VK_POOL_TEX + 2) * 4};
    vk_gpu_gap_note(VK_GAP_POOL_MARKS, region.size);
    vkCmdCopyBuffer(cb, PL.marks, PL.rb[i].buf, 1, &region);
    mb[0] = (VkMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT};
    vkCmdPipelineBarrier2(cb, &dep);
    vkCmdFillBuffer(cb, PL.marks, 0, VK_WHOLE_SIZE, 0);
    mb[0] = (VkMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    mb[1] = (VkMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    dep.memoryBarrierCount = 2;
    vkCmdPipelineBarrier2(cb, &dep);
}

static PlatMutex pool_lock_ = PLAT_MUTEX_INIT;

VkDescriptorSetLayout vk_pool_set_layout(void) {
    plat_mutex_lock(&pool_lock_);
    if (!PL.layout) PL.layout = pool_layout_make();
    plat_mutex_unlock(&pool_lock_);
    return PL.layout;
}

VkDescriptorSetLayout vk_empty_set_layout(void) {
    plat_mutex_lock(&pool_lock_);
    if (!PL.empty) {
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (vkCreateDescriptorSetLayout(vk_ctx()->device, &li, NULL, &PL.empty) != VK_SUCCESS) rt_fatal("vk: empty set layout failed");
    }
    plat_mutex_unlock(&pool_lock_);
    return PL.empty;
}

static uint16_t pool_kind_type(int k) {
    static const uint16_t dim[4] = {1, 3, 4, 5};
    return (uint16_t)(dim[k >> 1 & 3] | (k & 1 ? 0x100 : 0) | (k & 8 ? 0x400 : 0));
}

#define POOL_WBATCH 512
#define POOL_LOAD_END 64
static volatile int32_t pool_loading;
static uint64_t pool_load_made, pool_load_frame0;
int vk_exec_pool_loading(void) { return __atomic_load_n(&pool_loading, __ATOMIC_RELAXED); }
static int pool_load_backlog(void) {
    static int n = -1;
    if (n < 0) n = cfg_int("NX2SARIA_POOL_LOAD_BACKLOG");
    return n;
}

static int pool_create_budget(void) {
    static int n = -1;
    if (n < 0) {
        n = cfg_int("NX2SARIA_POOL_CREATE_BUDGET");
        if (n < 1) n = 1;
    }
    return pool_loading ? 1 << 20 : n;
}

static uint64_t pool_create_ns(void) {
    static int64_t ns = -1, load_ns;
    if (ns < 0) {
        int ms = cfg_int("NX2SARIA_POOL_CREATE_MS"), lms = cfg_int("NX2SARIA_POOL_LOAD_MS");
        load_ns = (int64_t)(lms < 1 ? 1 : lms) * 1000000;
        ns = (int64_t)(ms < 0 ? 0 : ms) * 1000000;
    }
    return (uint64_t)(pool_loading && ns ? load_ns : ns);
}
static uint32_t pool_defer[VK_POOL_TEX];
static uint32_t pool_left[VK_POOL_TEX], pool_nleft;
static uint8_t pool_is_left[VK_POOL_TEX];
static int pool_left_all = 1;
static void pool_left_add(uint32_t id) {
    if (id >= VK_POOL_TEX || pool_is_left[id]) return;
    pool_is_left[id] = 1, pool_left[pool_nleft++] = id;
}
static void pool_left_clear(void) {
    for (uint32_t i = 0; i < pool_nleft; i++) pool_is_left[pool_left[i]] = 0;
    pool_nleft = 0;
}
static VkWriteDescriptorSet pool_w[POOL_WBATCH];
static VkDescriptorImageInfo pool_ii[POOL_WBATCH];
static uint32_t pool_nw;

static void pool_flush(void) {
    if (pool_nw) vkUpdateDescriptorSets(vk_ctx()->device, pool_nw, pool_w, 0, NULL);
    PL.writes += pool_nw;
    pool_nw = 0;
}

static void pool_write_to(VkDescriptorSet set, uint32_t binding, uint32_t id, VkImageView view, VkSampler samp) {
    if (pool_nw == POOL_WBATCH) pool_flush();
    pool_ii[pool_nw] = (VkDescriptorImageInfo){samp, view, VK_IMAGE_LAYOUT_GENERAL};
    pool_w[pool_nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                             .dstBinding = binding, .dstArrayElement = id, .descriptorCount = 1,
                                             .descriptorType = view ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
                                             .pImageInfo = &pool_ii[pool_nw]};
    pool_nw++;
}

static void pool_write(uint32_t binding, uint32_t id, VkImageView view, VkSampler samp) {
    if (PL.all) {
        for (int i = 0; i < PL.nsets; i++) pool_write_to(PL.sets[i], binding, id, view, samp);
        return;
    }
    pool_write_to(PL.set, binding, id, view, samp);
    uint32_t e = binding * VK_POOL_TEX + id;
    PL.last[e] = PL.logn;
    PL.log[PL.logn++ % POOL_LOG] = e;
}

static void pool_entry_to(VkDescriptorSet set, uint32_t e) {
    uint32_t binding = e / VK_POOL_TEX, id = e % VK_POOL_TEX;
    if (binding < VK_POOL_KINDS) {
        VkImageView v = PL.view[(size_t)binding * VK_POOL_TEX + id];
        if (v) pool_write_to(set, binding, id, v, VK_NULL_HANDLE);
    } else if (id < VK_POOL_SAMP && PL.samp[id]) {
        pool_write_to(set, binding, id, VK_NULL_HANDLE, PL.samp[id]);
    }
}

static int pool_current(void);
static volatile uint64_t pool_takes;

static int pool_take_set(VkExec *ex, uint64_t *from) {
    if (PL.cur_fence == ex->fence && (!PL.sealed || pool_current())) return 0;
    int i = (PL.cur + 1) % PL.nsets;
    if (PL.nsets > 1) {
        const GfxBackend *b = gfx_backend();
        if (b->fence_done() < PL.set_fence[i]) PL.waits++, b->fence_wait(PL.set_fence[i], UINT64_MAX);
    }
    *from = PL.set_synced[i];
    PL.cur = i, PL.set = PL.sets[i], PL.cur_fence = ex->fence, PL.set_fence[i] = ex->fence, PL.sealed = 0;
    return PL.nsets > 1;
}

int vk_exec_pool_split(uint64_t fence) {
    if (PL.cur_fence != fence || PL.sealed) return 1;
    if (PL.nsets < 2 || PL.set_fence[(PL.cur + 1) % PL.nsets] == fence) return 0;
    PL.sealed = 1;
    return 1;
}

uint64_t vk_pool_takes(void) { return __atomic_load_n(&pool_takes, __ATOMIC_ACQUIRE); }

static uint64_t pool_memo_hits;
static uint64_t pool_memo_same;

static void pool_catch_up(uint64_t from, uint64_t to) {
    VkDescriptorSet set = PL.set;
    if (to - from > POOL_LOG) {
        PL.rewrites++;
        for (uint32_t e = 0; e < VK_POOL_KINDS * VK_POOL_TEX + VK_POOL_SAMP; e++) pool_entry_to(set, e);
    } else {
        PL.catchup += to - from;
        for (uint64_t q = from; q < to; q++) {
            uint32_t e = PL.log[q % POOL_LOG];
            if (PL.last[e] == q) pool_entry_to(set, e), PL.written++;
        }
    }
    pool_flush();
    PL.switches++;
    if (PL.switches % 3600 == 0) {
        char bs[1024];
        vk_texture_bucket_stats(bs, sizeof bs);
        rt_log("vk: texture pool buckets over 3600 switches: %s", bs);
    }
    if (PL.switches % 3600 == 0)
        rt_log("vk: texture pool sets: %llu switches, %llu waited for the GPU, %.1f log entries and %.1f writes per "
               "catch-up, %llu whole rewrites; %.3f ms catch-up and %.3f ms entry refresh per switch (%.3f ms of it in "
               "%.2f walks over every slot, %.3f ms in %.2f checks of %.0f listed entries and %d slots without one "
               "(%.3f ms in %.2f walks over those); "
               "%.0f slots, %.0f refilled: %.0f registered, %.0f address changes, %.0f "
               "shared a lookup, %.0f of them holding its views already; %.0f checked); %llu identical registrations kept; "
               "%llu refreshes created nothing for their frame's time (pool_create_ms), %llu lookups of slots left in the frame skipped",
               (unsigned long long)PL.switches, (unsigned long long)PL.waits, (double)PL.catchup / (double)PL.switches,
               (double)PL.written / (double)PL.switches, (unsigned long long)PL.rewrites,
               PL.catch_ns / 1e6 / (double)PL.switches, PL.sync_ns / 1e6 / (double)PL.switches,
               PL.full_ns / 1e6 / (double)PL.switches, (double)PL.n_full / (double)PL.switches,
               PL.ep_ns / 1e6 / (double)PL.switches, (double)PL.n_ep / (double)PL.switches,
               PL.n_ep ? (double)PL.n_ep_ent / (double)PL.n_ep : 0.0, PL.nunres,
               PL.unres_ns / 1e6 / (double)PL.switches, (double)PL.n_unres_walks / (double)PL.switches,
               (double)PL.n_slots / (double)PL.switches, (double)PL.n_refill / (double)PL.switches,
               (double)PL.n_by_reg / (double)PL.switches, (double)PL.n_by_addr / (double)PL.switches,
               (double)pool_memo_hits / (double)PL.switches, (double)pool_memo_same / (double)PL.switches,
               (double)PL.n_fresh / (double)PL.switches, (unsigned long long)vk_texture_same_regs(),
               (unsigned long long)PL.n_frame_stops, (unsigned long long)PL.n_defer_skips);
    if (PL.switches % 3600 == 0)
        rt_log("vk: texture pool dirty walks per switch: %.2f at a submission's start (%.0f slots looked at, %.0f "
               "refilled, %.3f ms), %.2f within one (%.0f looked at, %.0f refilled, %.3f ms)",
               (double)PL.n_walk[0] / (double)PL.switches, (double)PL.walk_visit[0] / (double)PL.switches,
               (double)PL.walk_refill[0] / (double)PL.switches, PL.walk_ns[0] / 1e6 / (double)PL.switches,
               (double)PL.n_walk[1] / (double)PL.switches, (double)PL.walk_visit[1] / (double)PL.switches,
               (double)PL.walk_refill[1] / (double)PL.switches, PL.walk_ns[1] / 1e6 / (double)PL.switches);
    if (PL.switches % 3600 == 0)
        rt_log("vk: texture pool slot groups per switch (NX2SARIA_POOL_GROUP): %.0f walked, %.0f left alone with %.0f "
               "slots, %.0f first members refilled (%.0f changed), %.0f not known to hold one resolution; check "
               "(NX2SARIA_POOL_GROUP_CHECK): %llu writes in %llu groups left alone so far, %llu more to members registered "
               "since their last fill",
               (double)PL.n_gwalk / (double)PL.switches, (double)PL.n_gskip / (double)PL.switches,
               (double)PL.n_gskip_slots / (double)PL.switches, (double)PL.n_grep / (double)PL.switches,
               (double)PL.n_gchanged / (double)PL.switches, (double)PL.n_gnouni / (double)PL.switches,
               (unsigned long long)PL.n_gchk_writes, (unsigned long long)PL.n_gchk_groups,
               (unsigned long long)PL.n_gchk_pending);
}

static int shader_skipped(const GfxShader *sh) {
    static uint64_t list[16];
    static int n = -1;
    static struct { const GfxShader *s; uint8_t skip; } memo[256];
    if (n < 0) {
        n = 0;
        const char *e = cfg_get("NX2SARIA_SKIP_SHADERS");
        for (const char *p = e; p && *p && n < 16;) {
            char *end;
            uint64_t v = strtoull(p, &end, 16);
            if (end == p) break;
            list[n++] = v;
            p = *end ? end + 1 : end;
        }
        if (n) rt_log("vk: NX2SARIA_SKIP_SHADERS: %d shader hashes skipped", n);
    }
    if (!n || !sh) return 0;
    uint32_t i = (uint32_t)(((uintptr_t)sh * 0x9E3779B97F4A7C15ull) >> 56);
    if (memo[i].s == sh) return memo[i].skip;
    uint64_t h = gfx_shader_spirv_fnv(sh);
    uint8_t skip = 0;
    for (int k = 0; k < n; k++) skip |= list[k] == h;
    memo[i].s = sh, memo[i].skip = skip;
    return skip;
}

static int pool_on(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("POOL", "1");
    return on;
}

#define POOL_MEMO 4096
static struct { uint64_t kh; uint32_t agen, egen, mask, stamp, got, ent; VkImageView view[VK_POOL_KINDS]; } pool_memo[POOL_MEMO];
static uint32_t pool_stamp;
static uint64_t pool_memo_hits, pool_memo_same;

static int pool_fast(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("POOL_FAST", "1");
    return RT_KNOB_ON(on);
}

static void pool_hold(uint32_t id, uint32_t ent) {
    if (PL.ent[id] == ent) return;
    vk_texture_pool_hold_locked(PL.ent[id], ent);
    PL.ent[id] = ent;
}

static void pool_unres(uint32_t id, int got) {
    if (got || PL.unres_on[id]) {
        if (got) PL.unres_on[id] = 0;
        return;
    }
    PL.unres_on[id] = 1;
    if (PL.unres_in[id]) return;
    PL.unres_in[id] = 1;
    if (PL.nunres == PL.cunres) {
        int n = PL.cunres ? PL.cunres * 2 : 1024;
        int *p = realloc(PL.unres, sizeof *p * (size_t)n);
        if (!p) rt_fatal("vk: out of memory");
        PL.unres = p, PL.cunres = n;
    }
    PL.unres[PL.nunres++] = (int)id;
}

static int pool_view_blank(VkImageView v) {
    for (int i = 0; i < DV_COUNT; i++)
        if (v == Z.img[i].view || v == Z.img[i].sview) return 1;
    return 0;
}

static uint32_t pool_no_3d(void) { return __atomic_load_n(&vk_pool_kinds_used, __ATOMIC_RELAXED) & 0x3030u ? 0 : 16384u; }

static uint32_t pool_blanked[512];
static int pool_nblanked;
static void pool_note_blank(uint32_t id, VkImageView was, VkImageView now, int *noted) {
    if (!flight_on || *noted || !was || pool_view_blank(was) || !pool_view_blank(now)) return;
    uint64_t addr;
    uint32_t fmt, w, h, why = vk_texture_slot_why_locked((int)id, &addr, &fmt, &w, &h) | pool_no_3d();
    flight_tex(FLIGHT_TEX_TOBLANK, addr, fmt, w, h, 0, 0, why, id);
    if (pool_nblanked < (int)(sizeof pool_blanked / sizeof *pool_blanked)) pool_blanked[pool_nblanked++] = id;
    *noted = 1;
}

static void pool_blank_check(void) {
    if (!pool_nblanked) return;
    vk_texture_lock();
    for (int i = 0; i < pool_nblanked; i++) {
        uint32_t id = pool_blanked[i];
        int real = 0;
        for (int k = 0; k < VK_POOL_KINDS && !real; k++) {
            VkImageView v = PL.view[(size_t)k * VK_POOL_TEX + id];
            real = v && !pool_view_blank(v);
        }
        if (real) continue;
        uint64_t addr;
        uint32_t fmt, w, h, why = vk_texture_slot_why_locked((int)id, &addr, &fmt, &w, &h) | pool_no_3d();
        flight_tex(FLIGHT_TEX_BLANKSUB, addr, fmt, w, h, 0, 0, why, id);
    }
    vk_texture_unlock();
    pool_nblanked = 0;
}

static int pool_slot_shown(uint32_t id, uint32_t mask) {
    for (int k = 0; k < VK_POOL_KINDS; k++) {
        if (!(mask >> k & 1)) continue;
        VkImageView v = PL.view[(size_t)k * VK_POOL_TEX + id];
        if (v && !pool_view_blank(v)) return 1;
    }
    return 0;
}

static int pool_fill(VkExec *ex, uint32_t id, uint32_t mask, uint64_t tok) {
    int noted = 0;
    vk_texture_create_deferred_locked();
    PoolSlot *ps = &PL.ps[id];
    uint64_t kh = 0;
    if (tok) {
        uint32_t reg = (uint32_t)(tok >> 32);
        if (ps->kreg != reg || !ps->kh) ps->kh = vk_texture_slot_khash_locked((int)id), ps->kreg = reg;
        kh = ps->kh;
    }
    uint32_t ag = (uint32_t)tok;
    typeof(pool_memo[0]) *m = kh ? &pool_memo[(kh >> 20) % POOL_MEMO] : NULL;
    if (m && m->kh == kh && m->agen == ag && m->egen == vk_texture_entry_gen() && (m->mask & mask) == mask) {
        pool_memo_hits++;
        pool_unres(id, (int)m->got);
        pool_hold(id, m->ent);
        if (ps->stamp && ps->stamp == m->stamp && (ps->fmask & mask) == mask && pool_fast()) {
            pool_memo_same++;
            return 1;
        }
        for (int k = 0; k < VK_POOL_KINDS; k++) {
            if (!(mask >> k & 1)) continue;
            VkImageView *e = &PL.view[(size_t)k * VK_POOL_TEX + id];
            if (*e != m->view[k]) {
                pool_note_blank(id, *e, m->view[k], &noted);
                *e = m->view[k], pool_write((uint32_t)k, id, m->view[k], VK_NULL_HANDLE);
            }
        }
        ps->fmask = ps->stamp == m->stamp ? ps->fmask | mask : mask;
        ps->stamp = m->stamp;
        return 1;
    }
    vk_texture_pool_shown_locked(tok && pool_slot_shown(id, mask));
    for (int k = 0; k < VK_POOL_KINDS; k++) {
        if (!(mask >> k & 1)) continue;
        GfxShaderRes r = {0};
        r.type = pool_kind_type(k);
        VkDescriptorImageInfo ii;
        VkImage img;
        tex_info_full(ex, &r, tok ? id | 1ull << 63 : 0, 0, 1, &ii, &img);
        VkImageView *e = &PL.view[(size_t)k * VK_POOL_TEX + id];
        if (*e != ii.imageView) {
            pool_note_blank(id, *e, ii.imageView, &noted);
            *e = ii.imageView, pool_write((uint32_t)k, id, ii.imageView, VK_NULL_HANDLE);
        }
    }
    vk_texture_pool_shown_locked(0);
    int ok = !vk_texture_create_deferred_locked();
    uint32_t ent = 0;
    int got = vk_texture_pool_note_locked((int)id, &ent);
    if (tok) pool_unres(id, got);
    pool_hold(id, tok ? ent : 0);
    ps->stamp = 0;
    if (m && ok) {
        uint32_t eg = vk_texture_entry_gen();
        int changed = !m->stamp;
        for (int k = 0; k < VK_POOL_KINDS; k++)
            if (mask >> k & 1) {
                VkImageView v = PL.view[(size_t)k * VK_POOL_TEX + id];
                if (m->view[k] != v) m->view[k] = v, changed = 1;
            }
        if (m->kh != kh || m->agen != ag || m->egen != eg) m->kh = kh, m->agen = ag, m->egen = eg, m->mask = 0;
        m->mask |= mask;
        m->got = (uint32_t)got, m->ent = ent;
        if (changed) {
            if (!++pool_stamp) pool_stamp = 1;
            m->stamp = pool_stamp;
        }
        ps->stamp = m->stamp, ps->fmask = mask;
    }
    return ok;
}

static void pool_sync_entries(VkExec *ex);

static void pool_sync(VkExec *ex);

static void pool_sync_draw(VkExec *ex) {
    if (!vk_exec_shared()) return pool_sync(ex);
    if (!vk_exec_in_chunk()) {
        plat_mutex_lock(&pool_mt);
        pool_sync(ex);
        plat_mutex_unlock(&pool_mt);
        return;
    }
    if (__atomic_load_n(&PL.cur_fence, __ATOMIC_ACQUIRE) == ex->fence) return;
    vk_exec_turn();
    plat_mutex_lock(&pool_mt);
    pool_sync(ex);
    plat_mutex_unlock(&pool_mt);
}

static int mesh_ring_make(void);

uint32_t vk_exec_draw_group_begin(VkExec *ex) {
    dummies_init();
    if (vk_ctx()->has_mesh_shader && !vk_ctx()->mesh_fallback) mesh_ring_make();
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    if (pool_on() && (PL.cur_fence != ex->fence || !PL.sealed)) {
        int lk = vk_exec_shared();
        if (lk) plat_mutex_lock(&pool_mt);
        pool_sync(ex);
        if (lk) plat_mutex_unlock(&pool_mt);
    }
    return UPD.fence == ex->fence ? UPD.n : 0;
}

static void pool_sync(VkExec *ex) {
    if (!PL.layout) vk_pool_set_layout();
    uint64_t from = 0, pre = PL.logn;
    int took = PL.cur_fence != ex->fence;
    if (took && flight_on) pool_blank_check();
    int switched = pool_take_set(ex, &from);
    if (PL.sealed) return;
    uint64_t t0 = kern_now_ns(), f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != PL.frame) PL.frame = f, PL.frame_ns = 0;
    vk_texture_pool_make_ns_take();
    pool_sync_entries(ex);
    uint64_t t1 = kern_now_ns();
    PL.sync_ns += t1 - t0;
    PL.frame_ns += vk_texture_pool_make_ns_take();
    if (switched) {
        vk_texture_lock();
        pool_catch_up(from, pre);
        vk_texture_unlock();
        PL.catch_ns += kern_now_ns() - t1;
    }
    PL.set_synced[PL.cur] = PL.logn;
    if (took && !PL.retry) __atomic_fetch_add(&pool_takes, 1, __ATOMIC_RELEASE);
}

typedef struct { VkExec *ex; uint32_t used; int n, retry, spent; } PoolDirty;

static int pool_refill(VkExec *ex, uint32_t id, uint32_t used, uint64_t t, int spent) {
    uint32_t mark = (uint32_t)PL.frame + 1;
    if (spent && pool_defer[id] == mark) return PL.n_defer_skips++, 0;
    if (pool_fill(ex, id, used, t)) return pool_defer[id] = 0, 1;
    pool_defer[id] = mark;
    return 0;
}

static void pool_dirty_slot(int id, void *arg) {
    PoolDirty *d = arg;
    PL.n_visit++;
    if (id >= d->n) return;
    uint64_t t = vk_texture_slot_token(id);
    if (t == PL.tok[id]) return;
    PL.n_refill++;
    if (t >> 32 != PL.tok[id] >> 32) PL.n_by_reg++;
    else PL.n_by_addr++;
    if (pool_refill(d->ex, (uint32_t)id, d->used, t, d->spent)) PL.tok[id] = t;
    else d->retry++, pool_left_add((uint32_t)id);
}

static int pool_current(void) {
    return PL.ready && !PL.retry && __atomic_load_n(&vk_pool_kinds_used, __ATOMIC_RELAXED) == PL.kinds &&
           vk_texture_slots_gen() == PL.gen && vk_texture_epoch() == PL.epoch && vk_sampler_gen() == PL.sgen;
}

static int pool_group(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("POOL_GROUP", "1");
    return RT_KNOB_ON(mode);
}

static int pool_group_check(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("POOL_GROUP_CHECK", "0") == 1;
    return on;
}

static void pool_dirty_group(int g, void *arg) {
    PoolDirty *d = arg;
    uint64_t chk = 0;
    VkSlotGroup *G = vk_texture_group_locked(g);
    int rep = G->head;
    PL.n_gwalk++;
    if (!G->stamp) PL.n_gnouni++;
    if (G->stamp && (G->fmask & d->used) == d->used && rep >= 0 && rep < d->n && !PL.unres_on[rep]) {
        uint64_t t = vk_texture_slot_token(rep);
        int same = t == PL.tok[rep];
        if (!same) {
            uint64_t l0 = PL.logn;
            PL.n_grep++;
            pool_dirty_slot(rep, arg);
            same = PL.tok[rep] == t && PL.logn == l0 && !PL.unres_on[rep];
            if (!same) PL.n_gchanged++;
        }
        if (same) {
            PL.n_gskip++, PL.n_gskip_slots += (uint64_t)G->n;
            if (!pool_group_check()) return;
            chk = PL.logn + 1;
        }
    }
    int r0 = d->retry, uni = 1;
    d->retry = 0;
    for (int id = G->head; id >= 0; id = vk_texture_group_next_locked(id)) {
        if (id >= d->n) continue;
        if (chk) {
            uint64_t l0 = PL.logn;
            int pending = vk_texture_slot_token(id) >> 32 != PL.tok[id] >> 32;
            pool_dirty_slot(id, arg);
            if (PL.logn != l0) {
                if (pending) PL.n_gchk_pending += PL.logn - l0;
                else PL.n_gchk_writes += PL.logn - l0, PL.n_gchk_bad++;
            }
        } else {
            pool_dirty_slot(id, arg);
        }
        if (PL.unres_on[id]) uni = 0;
    }
    if (d->retry) uni = 0;
    d->retry |= r0;
    G = vk_texture_group_locked(g);
    G->stamp = (uint32_t)uni, G->fmask = uni ? d->used : 0;
    if (chk && PL.n_gchk_bad) PL.n_gchk_groups++, PL.n_gchk_bad = 0;
}

static void pool_sync_entries(VkExec *ex) {
    uint32_t used = __atomic_load_n(&vk_pool_kinds_used, __ATOMIC_RELAXED);
    uint32_t gen = vk_texture_slots_gen(), ep = vk_texture_epoch(), sg = vk_sampler_gen();
    if (PL.ready && !PL.retry && used == PL.kinds && gen == PL.gen && ep == PL.epoch && sg == PL.sgen) return;
    int spent = pool_create_ns() && PL.frame_ns >= pool_create_ns();
    if (spent && PL.ready && used == PL.kinds && gen == PL.gen && ep == PL.epoch && sg == PL.sgen) return;
    vk_texture_lock();
    vk_texture_pool_ctx(1);
    uint32_t fresh = used & ~PL.kinds;
    int n = vk_texture_slot_count_locked();
    if (n > VK_POOL_TEX) n = VK_POOL_TEX;
    if (!PL.ready) {
        PL.all = 1;
        for (uint32_t id = 0; id < VK_POOL_TEX; id++) pool_fill(ex, id, 0xffffu, 0);
        for (uint32_t id = 0; id < VK_POOL_SAMP; id++) PL.samp[id] = Z.samp, pool_write(VK_POOL_KINDS, id, VK_NULL_HANDLE, Z.samp);
        pool_flush();
        PL.all = 0;
        PL.ready = 1, PL.sgen = 0;
    }
    int retry = 0;
    if (fresh || gen != PL.gen || ep != PL.epoch || PL.retry) {
        vk_texture_create_budget_locked(spent ? 0 : pool_create_budget());
        PL.n_frame_stops += (uint64_t)spent;
        PoolDirty pd = {ex, used, n, 0, spent};
        int newep = ep != PL.epoch;
        uint64_t v0 = PL.n_visit, f0 = PL.n_refill, w0 = kern_now_ns();
        int grp = pool_group();
        if (grp && !PL.gmode) vk_texture_groups_unstamp_locked();
        PL.gmode = grp;
        int quick = !fresh && (!newep || pool_fast()) && (!PL.retry || !pool_left_all) && PL.dirty_pos;
        if (quick && PL.retry) {
            uint32_t k = 0;
            for (uint32_t i = 0; i < pool_nleft; i++) {
                uint32_t id = pool_left[i];
                uint64_t t = (int)id < n ? vk_texture_slot_token((int)id) : 0;
                int left = 0;
                if (t != PL.tok[id]) {
                    PL.n_refill++;
                    if (t >> 32 != PL.tok[id] >> 32) PL.n_by_reg++;
                    else PL.n_by_addr++;
                    if (pool_refill(ex, id, used, t, spent)) PL.tok[id] = t;
                    else left = 1;
                }
                if (left) pool_left[k++] = id;
                else pool_is_left[id] = 0;
            }
            pool_nleft = k;
            PL.n_slots += (uint64_t)k;
        }
        int walked = quick && vk_texture_dirty_slots_locked(&PL.dirty_pos, pool_dirty_slot, grp ? pool_dirty_group : NULL, &pd);
        if (walked) {
            int at = newep ? 0 : 1;
            PL.n_walk[at]++, PL.walk_visit[at] += PL.n_visit - v0, PL.walk_refill[at] += PL.n_refill - f0;
            PL.walk_ns[at] += kern_now_ns() - w0;
        }
        retry = pd.retry;
        if (walked && newep) {
            uint64_t te = kern_now_ns();
            PL.n_ep_ent += (uint64_t)vk_texture_pool_fresh_locked();
            uint32_t mg = mem_map_gen();
            uint64_t tu = kern_now_ns();
            int walk_unres = mg != PL.unres_mg || PL.unres_more || !(PL.n_ep & 7);
            if (walk_unres) PL.unres_mg = mg, PL.n_unres_walks++, vk_texture_create_deferred_locked();
            for (int i = 0; walk_unres && i < PL.nunres;) {
                int id = PL.unres[i];
                uint64_t t = PL.unres_on[id] ? vk_texture_slot_token(id) : 0;
                int r = t && t == PL.tok[id] ? vk_texture_slot_retry_locked(id) : -1;
                if (r) {
                    PL.unres_on[id] = 0;
                    PL.unres_in[id] = 0;
                    PL.unres[i] = PL.unres[--PL.nunres];
                    continue;
                }
                i++;
            }
            if (walk_unres) PL.unres_more = vk_texture_create_deferred_locked();
            PL.unres_ns += kern_now_ns() - tu;
            PL.n_ep++, PL.ep_ns += kern_now_ns() - te;
        }
        if (!walked) PL.dirty_pos = vk_texture_dirty_pos_locked(), pool_left_clear(), pool_left_all = 0;
        uint64_t tw = walked ? 0 : kern_now_ns();
        for (int id = 0; id < n && !walked; id++) {
            uint64_t t = vk_texture_slot_token(id);
            if (t != PL.tok[id]) {
                PL.n_refill++;
                if (t >> 32 != PL.tok[id] >> 32) PL.n_by_reg++;
                else PL.n_by_addr++;
                if (pool_refill(ex, (uint32_t)id, used, t, spent)) PL.tok[id] = t;
                else retry++, pool_left_add((uint32_t)id);
            } else if (t && fresh) {
                pool_fill(ex, (uint32_t)id, fresh, t);
            } else if (t && ep != PL.epoch) {
                PL.n_fresh++;
                vk_texture_by_id_locked(id, NULL, NULL);
            }
        }
        if (!walked) {
            PL.n_slots += (uint64_t)n, PL.n_full++, PL.full_ns += kern_now_ns() - tw;
            vk_texture_groups_unstamp_locked();
        }
        retry = (int)pool_nleft;
        vk_texture_create_budget_locked(-1);
    }
    PL.retry = retry != 0;
    int backlog = pool_load_backlog();
    if (!pool_loading && backlog > 0 && retry >= backlog) {
        pool_load_made = (uint64_t)retry, pool_load_frame0 = PL.frame;
        __atomic_store_n(&pool_loading, 1, __ATOMIC_RELEASE);
        stutter_load();
    } else if (pool_loading && retry < POOL_LOAD_END) {
        __atomic_store_n(&pool_loading, 0, __ATOMIC_RELEASE);
        stutter_load();
        rt_log("vk: texture pool: an area load of about %llu textures was made in %llu frames (NX2SARIA_POOL_LOAD_BACKLOG)",
               (unsigned long long)pool_load_made, (unsigned long long)(PL.frame - pool_load_frame0 + 1));
    }
    if (sg != PL.sgen) {
        int ns = vk_sampler_slot_count_locked();
        for (int id = 0; id < VK_POOL_SAMP; id++) {
            VkSampler s = id < ns ? vk_sampler_by_id_locked(id) : VK_NULL_HANDLE;
            if (!s) s = Z.samp;
            if (s != PL.samp[id]) PL.samp[id] = s, pool_write(VK_POOL_KINDS, (uint32_t)id, VK_NULL_HANDLE, s);
        }
    }
    pool_flush();
    vk_texture_pool_ctx(0);
    vk_texture_unlock();
    PL.kinds = used, PL.gen = gen, PL.epoch = ep, PL.sgen = sg;
}

#define TWINS 8
typedef struct { VkImage src, img; VmaAllocation mem; VkImageView view[2]; uint32_t w, h, layers; VkFormat fmt; } Twin;
static Twin twins[TWINS];
static unsigned twin_next;
static _Thread_local int twin_dispatch;
static VkBuffer twin_buf;
static VmaAllocation twin_buf_mem;
static VkDeviceSize twin_buf_cap;

static void twin_free(void *arg) {
    Twin *t = arg;
    VkCtx *c = vk_ctx();
    for (int k = 0; k < 2; k++)
        if (t->view[k]) vkDestroyImageView(c->device, t->view[k], NULL);
    vmaDestroyImage(c->vma, t->img, t->mem);
    free(t);
}

typedef struct { VkBuffer buf; VmaAllocation mem; } TwinBuf;
static void twin_buf_free(void *arg) {
    TwinBuf *b = arg;
    vmaDestroyBuffer(vk_ctx()->vma, b->buf, b->mem);
    free(b);
}

static VkFormat twin_format(const VkTexImage *img) {
    static int on = -1;
    if (on < 0) on = rt_knob("DEPTH_TWIN", "1");
    if (!on || (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || img->samples > 1 || img->depth > 1) return VK_FORMAT_UNDEFINED;
    VkFormat df = img->format == VK_FORMAT_R32_SFLOAT ? VK_FORMAT_D32_SFLOAT
                : img->format == VK_FORMAT_R16_UNORM  ? VK_FORMAT_D16_UNORM : VK_FORMAT_UNDEFINED;
    if (!df) return df;
    static int compares[2] = {-1, -1};
    int i = df == VK_FORMAT_D16_UNORM;
    if (compares[i] < 0) {
        VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
        vkGetPhysicalDeviceFormatProperties2(vk_ctx()->phys, img->format, &p2);
        compares[i] = (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
        rt_log("vk_exec: %s under a compare sampler: %s", i ? "R16_UNORM" : "R32_SFLOAT",
               compares[i] ? "compared by the driver" : "sampled through a depth twin in compute passes");
    }
    return compares[i] ? VK_FORMAT_UNDEFINED : df;
}

static void twins_release(VkCtx *c) {
    for (int i = 0; i < TWINS; i++) {
        for (int k = 0; k < 2; k++)
            if (twins[i].view[k]) vkDestroyImageView(c->device, twins[i].view[k], NULL);
        if (twins[i].img) vmaDestroyImage(c->vma, twins[i].img, twins[i].mem);
    }
    memset(twins, 0, sizeof twins);
    if (twin_buf) vmaDestroyBuffer(c->vma, twin_buf, twin_buf_mem);
    twin_buf = VK_NULL_HANDLE, twin_buf_mem = NULL, twin_buf_cap = 0;
}

static Twin *twin_update(VkExec *ex, const VkTexImage *img) {
    VkFormat df = twin_format(img);
    if (!df) return NULL;
    VkCtx *c = vk_ctx();
    uint32_t bpp = df == VK_FORMAT_D32_SFLOAT ? 4 : 2, layers = img->layers ? img->layers : 1;
    VkDeviceSize bytes = (VkDeviceSize)img->width * img->height * layers * bpp;
    if (bytes > (64u << 20)) return NULL;
    if (bytes > twin_buf_cap) {
        VkDeviceSize want = twin_buf_cap ? twin_buf_cap * 2 : 1u << 16;
        while (want < bytes) want *= 2;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = want,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer nb;
        VmaAllocation nm;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &nb, &nm, NULL) != VK_SUCCESS) return NULL;
        if (twin_buf) {
            TwinBuf *old = malloc(sizeof *old);
            if (!old) rt_fatal("vk: out of memory (depth twin)");
            *old = (TwinBuf){twin_buf, twin_buf_mem};
            vk_completion_add(vk_sync_issued() + 1, twin_buf_free, old);
        }
        twin_buf = nb, twin_buf_mem = nm, twin_buf_cap = want;
    }
    Twin *t = NULL;
    for (int i = 0; i < TWINS && !t; i++)
        if (twins[i].img && twins[i].src == img->image && twins[i].w == img->width && twins[i].h == img->height &&
            twins[i].layers == layers && twins[i].fmt == df)
            t = &twins[i];
    if (!t) {
        t = &twins[twin_next++ % TWINS];
        if (t->img) {
            Twin *old = malloc(sizeof *old);
            if (!old) rt_fatal("vk: out of memory (depth twin)");
            *old = *t;
            vk_completion_add(vk_sync_issued() + 1, twin_free, old);
        }
        *t = (Twin){.src = img->image, .w = img->width, .h = img->height, .layers = layers, .fmt = df};
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = df,
                                .extent = {img->width, img->height, 1}, .mipLevels = 1, .arrayLayers = layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &t->img, &t->mem, NULL) != VK_SUCCESS) {
            *t = (Twin){0};
            return NULL;
        }
        for (int k = 0; k < 2; k++) {
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
                                        .viewType = k ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .format = df,
                                        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, k ? layers : 1}};
            if (vkCreateImageView(c->device, &vi, NULL, &t->view[k]) != VK_SUCCESS) t->view[k] = VK_NULL_HANDLE;
        }
        static int nlog;
        if (nlog++ < 8)
            rt_log("vk_exec: depth twin %ux%ux%u %s for a compute pass's compare sampler", img->width, img->height,
                   layers, df == VK_FORMAT_D32_SFLOAT ? "D32_SFLOAT" : "D16_UNORM");
    }
    VkMemoryBarrier2 m0 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = t->img, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers}};
    VkDependencyInfo d0 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m0,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(ex->cb, &d0);
    VkBufferImageCopy src = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                             .imageExtent = {img->width, img->height, 1}};
    vk_gpu_gap_note(VK_GAP_TWIN, (uint64_t)img->width * img->height);
    vkCmdCopyImageToBuffer(ex->cb, img->image, VK_IMAGE_LAYOUT_GENERAL, twin_buf, 1, &src);
    VkMemoryBarrier2 m1 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo d1 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m1};
    vkCmdPipelineBarrier2(ex->cb, &d1);
    VkBufferImageCopy dst = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, layers},
                             .imageExtent = {img->width, img->height, 1}};
    vkCmdCopyBufferToImage(ex->cb, twin_buf, t->img, VK_IMAGE_LAYOUT_GENERAL, 1, &dst);
    VkMemoryBarrier2 m2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d2 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m2};
    vkCmdPipelineBarrier2(ex->cb, &d2);
    ex->recorded += 5;
    return t;
}

static void net_binding(VkExec *ex, uint64_t h, int storage) {
    const VkTexImage *t = vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL);
    if (!t) return;
    if (storage) {
        static int told;
        if (!(t->usage & VK_IMAGE_USAGE_STORAGE_BIT) && told++ < 4)
            rt_log("vk_exec: a network program's output image %ux%u fmt %d at %#llx has no storage use: nothing is written to it",
                   t->width, t->height, (int)t->format, (unsigned long long)t->addr);
        return;
    }
    if (t->format != VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 || t->type != VK_IMAGE_TYPE_2D || t->samples != 1) return;
    const VkTexImage *o = raw_9e5_twin(t);
    if (!o) {
        static int told;
        if (!told++)
            rt_log("vk_exec: a network program samples the RGB9E5 picture %ux%u at %#llx, which nothing wrote this frame or the last",
                   t->width, t->height, (unsigned long long)t->addr);
        return;
    }
    if (o == t) return;
    static struct { VkImage dst; uint64_t seq; } done[4];
    static unsigned next;
    uint64_t seq = raw_written(o->image);
    for (int i = 0; i < 4; i++)
        if (done[i].dst == t->image && done[i].seq == seq) return;
    done[next++ % 4] = (typeof(done[0])){t->image, seq};
    VkMemoryBarrier2 m0 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d0 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m0};
    vkCmdPipelineBarrier2(ex->cb, &d0);
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {t->width, t->height, 1}};
    vkCmdCopyImage(ex->cb, o->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    VkMemoryBarrier2 m1 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d1 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m1};
    vkCmdPipelineBarrier2(ex->cb, &d1);
    ex->recorded += 3;
    static int told;
    if (!told++)
        rt_log("vk_exec: network input: the post pass's RGB9E5 words (%ux%u at %#llx) are copied into the texture the network samples",
               t->width, t->height, (unsigned long long)t->addr);
}

static void tex_info(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate, VkDescriptorImageInfo *out,
                     VkImage *img_out) {
    tex_info_lookup(ex, r, h, storage, separate, out, img_out);
    if (h && !storage && vk_dlaa_swap_img && ex->in_pass && ex->pass_w > vk_dlaa_swap_img->width + 1 &&
        vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL) == vk_dlaa_swap_img)
        out->imageView = vk_dlaa_swap_view, out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (twin_dispatch == 2 && h) net_binding(ex, h, storage);
    if (!twin_dispatch || storage || separate || !h || !(r->type & 0x400)) return;
    VkImageViewType vt = res_view_type(r->type);
    if (vt != VK_IMAGE_VIEW_TYPE_2D && vt != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return;
    const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL);
    Twin *t = img ? twin_update(ex, img) : NULL;
    VkImageView v = t ? t->view[vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY] : VK_NULL_HANDLE;
    if (v) out->imageView = v, out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

static VkDescriptorSet alloc_set(VkExec *ex, VkDescriptorSetLayout layout) {
    CbRes *r = cb_res(ex);
    VkCtx *c = vk_ctx();
    for (int attempt = 0; attempt < 2; attempt++) {
        if (r->npools) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = r->pools[r->npools - 1], .descriptorSetCount = 1,
                                              .pSetLayouts = &layout};
            VkDescriptorSet set;
            VkResult res = vkAllocateDescriptorSets(c->device, &ai, &set);
            if (res == VK_SUCCESS) return set;
            if (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL) rt_fatal("vk: descriptor set allocation failed (%d)", (int)res);
        }
        static const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 256},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256}};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = REPLAY_POOL_SETS,
                                         .poolSizeCount = sizeof sizes / sizeof sizes[0], .pPoolSizes = sizes};
        if (r->npools == r->cpools) r->pools = grow(r->pools, &r->cpools, sizeof *r->pools);
        VkDescriptorPool pool = VK_NULL_HANDLE;
        plat_mutex_lock(&spare_lock);
        if (nspare_pool) pool = spare_pool[--nspare_pool];
        plat_mutex_unlock(&spare_lock);
        if (!pool) {
            VkResult res = vkCreateDescriptorPool(c->device, &pi, NULL, &pool);
            if (res == VK_ERROR_OUT_OF_DEVICE_MEMORY || res == VK_ERROR_OUT_OF_HOST_MEMORY) {
                uint64_t freed = vk_mem_give_back();
                res = vkCreateDescriptorPool(c->device, &pi, NULL, &pool);
                rt_log("vk: descriptor pool refused: %.1f MiB given back, %s", freed / 1048576.0,
                       res == VK_SUCCESS ? "made at the second try" : "refused again");
            }
            if (res != VK_SUCCESS) rt_fatal("vk: descriptor pool failed (%d)", (int)res);
            __atomic_add_fetch(&npools_live, 1, __ATOMIC_RELAXED);
        }
        r->pools[r->npools++] = pool;
    }
    rt_fatal("vk: descriptor set allocation failed twice");
    return VK_NULL_HANDLE;
}

static void support_in(const VkExec *ex, float in[3]) {
    float k = ex->pass_scale > 1 ? (float)ex->pass_scale : 1.0f;
    in[0] = ex->in_pass && ex->has_vp ? ex->vp[0][2] * k : (float)ex->pass_w;
    in[1] = ex->in_pass && ex->has_vp ? ex->vp[0][3] * k : (float)ex->pass_h;
    in[2] = ex->point_size > 0 ? ex->point_size : 1.0f;
}

static const float *pcss_params(void) {
    static float p[4] = {-1, 0, 0, 0};
    if (p[0] < 0) {
        int pct = cfg_int("NX2SARIA_PCSS");
        pct = pct < 0 ? 0 : pct > 200 ? 200 : pct;
        const char *k = cfg_get("NX2SARIA_PCSS_K");
        float k100 = k && *k ? (float)atof(k) : 0.5f;
        p[0] = k100 * (float)pct / 100.0f;
        p[1] = 0.35f / 960.0f, p[2] = 5.0f / 960.0f, p[3] = 4.0f / 960.0f;
        if (pct) rt_log("vk: soft shadows (PCSS) at %d%% (depth factor %g)", pct, p[0]);
    }
    return p;
}

static const float *shadow_spread_params(void) {
    static float p[4] = {0, 0, 0, 0};
    if (p[0] == 0) {
        int scale = cfg_int("NX2SARIA_SHADOW_SCALE");
        scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
        p[0] = 1.0f / (float)scale;
    }
    return p;
}

static void support_fill(uint8_t *b, float w, float h, float point_size) {
    memset(b, 0, SUPPORT_BYTES);
    float inv[4] = {w > 0 ? 2.0f / w : 0, h > 0 ? 2.0f / h : 0, 0, 0}, size[4] = {w, h, 0, 0};
    memcpy(b + 144, inv, 16);
    memcpy(b + 160, size, 16);
    const float one = 1.0f;
    memcpy(b + 1392, &point_size, 4);
    memcpy(b + 1408, pcss_params(), 16);
    memcpy(b + 1424, shadow_spread_params(), 16);
    for (int i = 0; i < 73; i++) memcpy(b + 192 + 16 * i, &one, 4);
    if (vk_hdr_armed()) {
        float hdr[20];
        vk_hdr_support(hdr);
        memcpy(b + 1440, hdr, sizeof hdr);
    }
}

static int bind_stats_on(void) {
    if (BS.on < 0) {
        const char *e = cfg_get("NX2SARIA_BIND_STATS");
        BS.on = e && *e && strcmp(e, "0");
    }
    return BS.on;
}

static void bind_stats(const VkPipeInfo *info) {
    if (!bind_stats_on() || info->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        int fs = info->n_sets > 1 ? b->set : !(b->stages & VK_SHADER_STAGE_VERTEX_BIT);
        BS.bind[fs]++, BS.desc[fs] += b->count ? b->count : 1;
    }
    if (++BS.draws % 20000 == 0) {
        double d = (double)BS.draws;
        rt_log("vk_exec: per draw over %llu draws: vertex %.2f bindings %.2f descriptors, fragment %.2f bindings %.2f descriptors",
               (unsigned long long)BS.draws, BS.bind[0] / d, BS.desc[0] / d, BS.bind[1] / d, BS.desc[1] / d);
        rt_log("vk_exec: per draw: pushes %.3f skipped %.3f, sets written %.3f reused %.3f, texture types %.3f; "
               "same program and state as the draw before %.3f, also same buffers %.3f; descriptor builds skipped %.3f",
               BS.push / d, BS.push_skip / d, BS.set_new / d, BS.set_reuse / d, BS.ac_miss / d, BS.same_prog / d,
               BS.same_all / d, BS.bind_skip / d);
    }
}

static void repeat_stats(const VkExec *ex, const VkPipeInfo *info) {
    static struct {
        const prog_t *prog;
        VkPipeState pipe;
        uint64_t tex[2][128], img[2][32];
        const VkPipeInfo *info;
        p_bind_buf ubo[2][18], ssbo[2][16];
        float vp[6], point_size;
    } last, cur;
    if (!bind_stats_on()) return;
    memset(&cur, 0, sizeof cur);
    cur.prog = ex->prog, cur.pipe = ex->pipe, cur.info = info;
    const int st[2] = {NVN_STAGE_VS, NVN_STAGE_FS};
    for (int k = 0; k < 2; k++) {
        memcpy(cur.tex[k], ex->tex[st[k]], sizeof cur.tex[k]), memcpy(cur.img[k], ex->img[st[k]], sizeof cur.img[k]);
        memcpy(cur.ubo[k], ex->ubo[st[k]], sizeof cur.ubo[k]), memcpy(cur.ssbo[k], ex->ssbo[st[k]], sizeof cur.ssbo[k]);
    }
    memcpy(cur.vp, ex->vp[0], sizeof cur.vp), cur.point_size = ex->point_size;
    size_t prog_part = offsetof(typeof(cur), info);
    if (!memcmp(&cur, &last, prog_part)) {
        BS.same_prog++;
        if (!memcmp((const char *)&cur + prog_part, (const char *)&last + prog_part, sizeof cur - prog_part)) BS.same_all++;
    }
    last = cur;
}

static int push_skip_on(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_PUSH_SKIP");
    }
    return on;
}

static int bind_skip_on(void) {
    if (rs_on < 0) {
        const char *e = cfg_value("NX2SARIA_BIND_SKIP");
        rs_on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
        if (rs_on != 1) rt_log("vk_exec: descriptor build skip %s", rs_on ? "check" : "off");
    }
    return rs_on;
}

static void rs_add(RsRange *r, uint32_t *n, const VkExec *ex, const void *at, size_t len) {
    uint32_t off = (uint32_t)((const uint8_t *)at - (const uint8_t *)ex);
    if (*n && r[*n - 1].off + r[*n - 1].len == off) r[*n - 1].len += (uint32_t)len;
    else r[(*n)++] = (RsRange){off, (uint32_t)len};
}

static const RsList *rs_list(const VkExec *ex, const VkPipeInfo *info) {
    DrawCtx *const cx = ex->dc;
    if (info == cx->rs_last) return cx->rs_last_l;
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&rs_lock);
    if (!rs_lists.klen) nvn_hm_init(&rs_lists, sizeof info);
    RsList *l = nvn_hm_get(&rs_lists, &info);
    if (!l) {
        static RsRange tmp[MAX_DESC];
        RsPtr ptr[RS_PTRS];
        uint32_t n = 0, nd = 0, np = 0;
        int sup = 0, c0 = 0, ok = 1;
        for (uint32_t i = 0; i < info->n_bindings && ok; i++) {
            const VkPipeBinding *b = &info->bindings[i];
            const GfxShaderRes *r = b->res;
            uint32_t cnt = b->count ? b->count : 1;
            if ((nd += cnt) > MAX_DESC) { ok = 0; break; }
            int st = nvn_stage_of(b->stage);
            if (b->kind == VK_PIPE_SUPPORT) {
                sup = 1;
            } else if (b->kind == GFX_RES_UBO) {
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot == 0 && c0_on()) c0 = 1, rs_add(tmp, &n, ex, ex->ssbo[st], sizeof ex->ssbo[st]);
                    else if (slot >= 3 && slot - 3 < 18) rs_add(tmp, &n, ex, &ex->ubo[st][slot - 3], sizeof(p_bind_buf));
                }
            } else if (b->kind == GFX_RES_SSBO) {
                int ubo, slot = ssbo_slot(r, st, &ubo), direct = slot >= 0 && slot < 16;
                if (direct) rs_add(tmp, &n, ex, ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot], sizeof(p_bind_buf));
                else if (ubo_ptr_res(r)) rs_add(tmp, &n, ex, &ex->ubo[st][r->cbuf_slot - 3], sizeof(p_bind_buf));
                if (r->ptr || (!direct && ubo_ptr_res(r))) {
                    if (np == RS_PTRS) { ok = 0; break; }
                    ptr[np++] = (RsPtr){st, r};
                }
            } else if ((b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                       b->type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                int img = b->kind == GFX_RES_IMG, cap = img ? 32 : 128;
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot >= 0 && slot < cap) rs_add(tmp, &n, ex, img ? &ex->img[st][slot] : &ex->tex[st][slot], 8);
                }
            }
        }
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < n; i++) bytes += tmp[i].len;
        if (bytes > RS_BYTES) ok = 0;
        l = malloc(sizeof *l + sizeof *tmp * n);
        if (!l) rt_fatal("vk: out of memory");
        l->n = n, l->bytes = (uint32_t)bytes, l->sup = (uint8_t)sup, l->c0 = (uint8_t)c0, l->ok = (uint8_t)ok;
        l->np = (uint8_t)np;
        memcpy(l->p, ptr, sizeof *ptr * np);
        memcpy(l->r, tmp, sizeof *tmp * n);
        nvn_hm_put(&rs_lists, &info, l);
    }
    if (lk) plat_mutex_unlock(&rs_lock);
    cx->rs_last = info, cx->rs_last_l = l;
    return l;
}

static int bind_sig(const VkExec *ex, const VkPipeInfo *info) {
    DrawCtx *const cx = ex->dc;
    const RsList *l = rs_list(ex, info);
    if (!l->ok) return cx->RS.hn = 0, cx->RS.info = NULL, 0;
    uint64_t *h = cx->RS.h, diff = 0;
    uint32_t n = 0;
#define PUT(v) do { uint64_t v_ = (uint64_t)(v); diff |= h[n] ^ v_; h[n++] = v_; } while (0)
    PUT((uintptr_t)info);
    PUT((uintptr_t)ex->prog);
    PUT(ex->prog->serial);
    PUT((uint64_t)vk_texture_slots_gen() << 32 | vk_texture_epoch());
    PUT((uint64_t)vk_sampler_gen() << 32 | (uint32_t)ex->nfbl);
    PUT(vk_mem_gen() + vk_usnap_gen());
    for (int i = 0; i < ex->nfbl && i < 9; i++) PUT((uintptr_t)ex->fbl_img[i]);
    if (l->sup) {
        float in[3];
        support_in(ex, in);
        uint64_t a;
        uint32_t ps;
        memcpy(&a, in, 8), memcpy(&ps, &in[2], 4);
        PUT(a);
        PUT(ps);
    }
    if (l->c0) PUT(cx->c0_binst);
    for (int i = 0; i < l->np; i++)
        PUT(l->p[i].res->ptr ? ptr_addr(ex, l->p[i].st, l->p[i].res) : ubo_ptr(ex, l->p[i].st, l->p[i].res));
#undef PUT
    diff |= n != cx->RS.hn;
    cx->RS.hn = n;
    int fast = cx->RS.info == info && cx->RS.gen == cx->bind_gen;
    if (fast && rs_on != 2) return !diff;
    const uint8_t *e = (const uint8_t *)ex;
    uint8_t *d = cx->RS.body;
    int bd = 0;
    for (uint32_t i = 0; i < l->n; i++) {
        const RsRange *r = &l->r[i];
        if (!bd && memcmp(d, e + r->off, r->len)) bd = 1;
        if (bd) memcpy(d, e + r->off, r->len);
        d += r->len;
    }
    if (fast && bd) rt_fatal("vk_exec: descriptor build skip check: bound slots changed without a bind op");
    cx->RS.info = info, cx->RS.gen = cx->bind_gen;
    return !diff && !bd;
}

enum { PH_ENTRY, PH_SHADER, PH_PIPE, PH_STATE, PH_HOOKS, PH_BIND, PH_BIND_TEX, PH_BIND_BUF, PH_BIND_SIG, PH_BIND_PUSH,
       PH_VBUF, PH_REST, PH_N };
static inline void ph_mark(int p);
static inline void ph_add(int p, uint64_t ns);
static _Thread_local uint64_t ph_t;
static inline uint64_t bt_now(void) { return rt_trace & RT_TRACE_GFX || ph_t ? plat_now_ns() : 0; }

static int pb_mode = -1;

static int pb_on(void) {
    if (pb_mode < 0) {
        const char *e = cfg_value("NX2SARIA_BIND_REUSE");
        pb_mode = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
        if (pb_mode != 1) rt_log("vk_exec: per-binding descriptor reuse %s", pb_mode ? "check" : "off");
    }
    return pb_mode;
}

static void pb_globals(const VkExec *ex, uint64_t g[PB_G]) {
    int swap = vk_dlaa_swap_img && ex->in_pass && ex->pass_w > vk_dlaa_swap_img->width + 1;
    memset(g, 0, sizeof(uint64_t) * PB_G);
    g[0] = (uintptr_t)ex->cb;
    g[1] = (uintptr_t)ex->prog;
    g[2] = ex->prog ? ex->prog->serial : 0;
    g[3] = (uint64_t)vk_texture_slots_gen() << 32 | vk_texture_epoch();
    g[4] = (uint64_t)vk_sampler_gen() << 32 | (uint32_t)ex->nfbl;
    g[5] = vk_mem_gen() + vk_usnap_gen();
    g[6] = swap ? (uintptr_t)vk_dlaa_swap_img : 0;
    g[7] = swap ? (uint64_t)(uintptr_t)vk_dlaa_swap_view : 0;
    for (int i = 0; i < ex->nfbl && i < 9; i++) g[8 + i] = (uint64_t)(uintptr_t)ex->fbl_img[i];
}

static PbEntry *pb_entry(DrawCtx *cx, const VkPipeInfo *info, const uint64_t g[PB_G]) {
    uintptr_t p = (uintptr_t)info;
    PbEntry *e = &cx->PB[(p >> 6 ^ p >> 13) % PB_N];
    if (e->info != info || memcmp(e->g, g, sizeof e->g)) {
        e->info = info;
        memcpy(e->g, g, sizeof e->g);
        memset(e->ok, 0, sizeof e->ok);
    }
    return e;
}

static int pb_key(const VkExec *ex, const VkPipeBinding *b, uint32_t k, uint64_t key[4]) {
    const GfxShaderRes *r = b->res;
    int st = nvn_stage_of(b->stage);
    key[0] = key[1] = 0, key[2] = (uintptr_t)r, key[3] = (uint64_t)(uint32_t)b->kind << 32 | k;
    if (b->kind == GFX_RES_UBO) {
        int slot = r->slot + (int)k;
        if (slot == 0 && c0_on()) return 0;
        if (slot >= 3 && slot - 3 < 18) key[0] = ex->ubo[st][slot - 3].addr, key[1] = ex->ubo[st][slot - 3].size;
        return 1;
    }
    if (b->kind == GFX_RES_SSBO) {
        if ((r->flags & 1) || r->ptr || r->cbuf_slot >= VK_GPA_CB_SLOT) return 0;
        int ubo, slot = ssbo_slot(r, st, &ubo);
        if (slot < 0 || slot >= 16) return 0;
        const p_bind_buf *p = ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot];
        key[0] = p->addr, key[1] = p->size;
        return 1;
    }
    if (b->kind == GFX_RES_TEX && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
        b->type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
        int slot = r->slot + (int)k;
        key[0] = slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
        key[1] = (uint64_t)r->separate;
        return 1;
    }
    return 0;
}

static int pb_take(const VkExec *ex, const VkPipeBinding *b, uint32_t j, uint32_t k, VkDescSlot *o) {
    DrawCtx *const cx = ex->dc;
    cx->pb_key_ok = 0;
    if (!cx->pb_e || j >= PB_DESC || !pb_key(ex, b, k, cx->pb_key_cur)) return 0;
    cx->pb_key_ok = 1;
    if (pb_mode != 2 && cx->pb_e->ok[j] && !memcmp(cx->pb_e->in[j], cx->pb_key_cur, sizeof cx->pb_key_cur)) {
        const PbCount *c = &cx->pb_e->cnt[j];
        *o = cx->pb_e->d[j];
        if (c->has) count_use(cx, c->cls, c->use, c->n);
        vk_exec_bind_count(1);
        return 1;
    }
    cx->pb_cnt_cur = (PbCount){0};
    cx->pb_cap = &cx->pb_cnt_cur, cx->pb_spoiled = 0;
    return 0;
}

static void pb_keep(DrawCtx *cx, uint32_t j, const VkDescSlot *o) {
    cx->pb_cap = NULL;
    if (!cx->pb_e || j >= PB_DESC) return;
    vk_exec_bind_count(0);
    if (!cx->pb_key_ok) {
        cx->pb_e->ok[j] = 0;
        return;
    }
    if (pb_mode == 2 && cx->pb_e->ok[j] && !cx->pb_spoiled &&
        !memcmp(cx->pb_e->in[j], cx->pb_key_cur, sizeof cx->pb_key_cur) &&
        memcmp(&cx->pb_e->d[j], o, sizeof *o))
        rt_fatal("vk_exec: per-binding descriptor reuse check: descriptor %u resolves differently from its kept result "
                 "with the same inputs", j);
    cx->pb_e->ok[j] = (uint8_t)!cx->pb_spoiled;
    memcpy(cx->pb_e->in[j], cx->pb_key_cur, sizeof cx->pb_key_cur);
    cx->pb_e->d[j] = *o;
    cx->pb_e->cnt[j] = cx->pb_cnt_cur;
}

static void write_set(const VkPipeInfo *info, uint32_t s, VkDescriptorSet set, const VkDescSlot *d, VkWriteDescriptorSet *w,
                      uint32_t i0, uint32_t i1) {
    if (i1 == i0) return;
    if (info->tpl[s]) {
        vk_pipeline_update_tpl_fn()(vk_ctx()->device, set, info->tpl[s], d);
        return;
    }
    for (uint32_t k = i0; k < i1; k++) w[k].dstSet = set;
    vkUpdateDescriptorSets(vk_ctx()->device, i1 - i0, w + i0, 0, NULL);
}

static _Thread_local const VkDescSlot *emu_d;
static uint32_t emu_d_bytes;
static void gpa_reader(const VkPipeInfo *info) {
    int8_t g = __atomic_load_n(&info->gpa, __ATOMIC_RELAXED);
    if (!g) {
        g = 1;
        for (uint32_t i = 0; i < info->n_bindings && g == 1; i++) {
            const VkPipeBinding *b = &info->bindings[i];
            if (b->kind == GFX_RES_SSBO && b->res && b->res->cbuf_slot == VK_GPA_CB_SLOT) g = 2;
        }
        __atomic_store_n(&((VkPipeInfo *)info)->gpa, g, __ATOMIC_RELAXED);
    }
    if (g == 2) vk_mem_note_live();
}

static void bind_resources(VkExec *ex, const VkPipeInfo *info) {
    gpa_reader(info);
    DrawCtx *const cx = ex->dc;
    VkWriteDescriptorSet *w = cx->bind_w;
    VkDescSlot *d = cx->bind_d;
    emu_d = d;
    VkBufferView *bv = cx->bind_bv;
    if (info->n_bindings > MAX_BINDINGS) rt_fatal("vk: %u descriptor bindings exceed the replay's %d", info->n_bindings, MAX_BINDINGS);
    bind_stats(info);
    int gfx = info->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    int skip = gfx ? bind_skip_on() : 0, same = 0, was_valid = cx->B.res_valid;
    if (skip) {
        uint64_t sig_t0 = bt_now();
        same = bind_sig(ex, info);
        uint64_t sig_dt = bt_now() - sig_t0;
        vk_exec_frame.bind_sig_ns += sig_dt, ph_add(PH_BIND_SIG, sig_dt);
        if (same && was_valid && skip == 1) {
            for (int i = 0; i < cx->RS.ncnt; i++) vk_mem_count(cx->RS.cnt[i].cls, cx->RS.cnt[i].use, cx->RS.cnt[i].bytes);
            BS.bind_skip++;
            ex->recorded++;
            return;
        }
        cx->B.res_valid = 0;
        if (skip == 2) memcpy(cx->RS.pcnt, cx->RS.cnt, sizeof cx->RS.cnt), cx->RS.pncnt = cx->RS.ncnt;
        cx->RS.rec = 1, cx->RS.ncnt = 0, cx->RS.cnt_over = 0, cx->RS.volatile_ = 0, cx->RS.cmds = 0;
    }
    cx->pb_e = NULL;
    if (gfx && !cx->mo_ovr_on && pb_on()) {
        uint64_t g[PB_G];
        pb_globals(ex, g);
        cx->pb_e = pb_entry(cx, info, g);
    }
    uint32_t n_sets = info->n_sets ? info->n_sets : 1;
    uint32_t lo[2] = {0, 0}, hi[2] = {0, 0}, d0[2] = {0, 0};
    int need_w = 0;
    for (uint32_t s = 0, i0 = 0; s < n_sets; s++) {
        uint32_t i1 = i0;
        while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s)) i1++;
        lo[s] = i0, hi[s] = i1, i0 = i1;
        if (i1 > lo[s] && !info->tpl[s]) need_w = 1;
    }
    uint32_t nd = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        if (nd + cnt > MAX_DESC) rt_fatal("vk: %u descriptors exceed the replay's %d", nd + cnt, MAX_DESC);
        if (n_sets == 2 && i == lo[1]) d0[1] = nd;
        VkDescSlot *o = &d[nd];
        int st = nvn_stage_of(b->stage), kind = 0;
        if (b->kind == VK_PIPE_SUPPORT) {
            float in[3];
            support_in(ex, in);
            if (!cx->B.sup_valid || memcmp(in, cx->B.sup_in, sizeof in)) {
                uint8_t tmp[SUPPORT_BYTES];
                support_fill(tmp, in[0], in[1], in[2]);
                memcpy(cx->B.sup_in, in, sizeof in);
                if (!cx->B.sup_valid || memcmp(tmp, cx->B.support, SUPPORT_BYTES)) {
                    VkBuffer sb;
                    VkDeviceSize so;
                    memcpy(scratch(ex, SUPPORT_BYTES, &sb, &so), tmp, SUPPORT_BYTES);
                    memcpy(cx->B.support, tmp, SUPPORT_BYTES);
                    cx->B.sup_buf = sb, cx->B.sup_off = so, cx->B.sup_valid = 1;
                }
            }
            o->buffer = (VkDescriptorBufferInfo){cx->B.sup_buf, cx->B.sup_off, SUPPORT_BYTES};
        } else if (b->kind == GFX_RES_UBO) {
            uint64_t bt = bt_now();
            for (uint32_t k = 0; k < cnt; k++) {
                if (pb_take(ex, b, nd + k, k, &o[k])) continue;
                o[k].buffer = ubo_info(ex, st, r->slot + (int)k);
                pb_keep(cx, nd + k, &o[k]);
            }
            {
                uint64_t buf_dt = bt_now() - bt;
                vk_exec_frame.bind_buf_ns += buf_dt, ph_add(PH_BIND_BUF, buf_dt);
            }
        } else if (b->kind == GFX_RES_SSBO) {
            uint64_t bt = bt_now();
            for (uint32_t k = 0; k < cnt; k++) {
                if (pb_take(ex, b, nd + k, k, &o[k])) continue;
                o[k].buffer = ssbo_info(ex, st, r, r->flags & 1);
                pb_keep(cx, nd + k, &o[k]);
            }
            {
                uint64_t buf_dt = bt_now() - bt;
                vk_exec_frame.bind_buf_ns += buf_dt, ph_add(PH_BIND_BUF, buf_dt);
            }
        } else if (b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) {
            int img = b->kind == GFX_RES_IMG;
            if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                kind = 2;
                for (uint32_t k = 0; k < cnt; k++) {
                    o[k].w[0] = 0, o[k].w[1] = 0, o[k].w[2] = 0;
                    o[k].view = Z.zview;
                    if (need_w) bv[nd + k] = Z.zview;
                }
            } else {
                kind = 1;
                int locked = 0;
                int tm = !img && !r->exact && td_mode() == 1 && !vk_dlaa_swap_img && !twin_dispatch && vk_exec_shared();
                uint32_t tg = tm ? vk_texture_slots_gen() : 0, te = tm ? vk_texture_epoch() : 0;
                uint32_t ts = tm ? vk_sampler_gen() : 0;
                for (uint32_t k = 0; k < cnt; k++) {
                    if (pb_take(ex, b, nd + k, k, &o[k])) continue;
                    int slot = r->slot + (int)k;
                    uint64_t h = 0;
                    if (img) h = slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
                    else h = slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
                    uint64_t tt = bt_now();
                    TexMemo *m = tm && h ? &cx->TM[(h * 0x9E3779B97F4A7C15ull ^ (uint64_t)r->type << 9 ^
                                                    (uint64_t)r->separate) >> 55 & (TM_N - 1)] : NULL;
                    if (m && m->valid && m->h == h && m->type == r->type && m->separate == (uint8_t)r->separate &&
                        m->ep == te && m->sg == ts && slot_still(&m->gen, m->tok, h, tg)) {
                        o[k].image = m->out;
                        o[k].image.imageLayout = m->image && ex->nfbl && in_fbl(ex, m->image)
                                                     ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                     : VK_IMAGE_LAYOUT_GENERAL;
                    } else {
                        if (!locked) vk_texture_lock(), locked = 1;
                        VkImage ti = VK_NULL_HANDLE;
                        uint64_t tok = m ? token_of(h) : 0;
                        tex_info(ex, r, h, img, r->separate, &o[k].image, &ti);
                        if (m) *m = (TexMemo){.h = h, .tok = tok, .gen = tg, .ep = te, .sg = ts, .type = r->type,
                                              .separate = (uint8_t)r->separate, .valid = 1, .image = ti,
                                              .out = o[k].image};
                    }
                    {
                uint64_t tex_dt = bt_now() - tt;
                vk_exec_frame.bind_tex_ns += tex_dt, ph_add(PH_BIND_TEX, tex_dt);
            }
                    o[k].w[2] = (uint32_t)o[k].image.imageLayout;
                    pb_keep(cx, nd + k, &o[k]);
                }
                if (locked) vk_texture_unlock();
            }
        } else {
            rt_fatal("vk: binding %u has an unknown kind %d", (unsigned)b->binding, (int)b->kind);
        }
        if (need_w) {
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstBinding = b->binding, .descriptorCount = cnt, .descriptorType = b->type};
            if (kind == 0) w[i].pBufferInfo = &o->buffer;
            else if (kind == 1) w[i].pImageInfo = &o->image;
            else w[i].pTexelBufferView = &bv[nd];
        }
        nd += cnt;
    }
    cx->pb_e = NULL;
    uint64_t push_t0 = bt_now();
    if (n_sets == 2 && lo[1] == hi[1]) d0[1] = nd;
    emu_d_bytes = (uint32_t)((n_sets > 1 ? d0[1] : nd) * sizeof *d);
    int set1_first = 0;
    if (gfx && info->layout != cx->B.ds_layout) {
        cx->B.ds_layout = info->layout;
        cx->B.ds_bound[0] = cx->B.ds_bound[1] = VK_NULL_HANDLE;
        cx->B.ds_pushed[0] = cx->B.ds_pushed[1] = 0;
        set1_first = n_sets == 2 && vk_ctx()->validation;
    }
    for (uint32_t k = 0; k < n_sets; k++) {
        uint32_t s = set1_first ? 1 - k : k, i0 = lo[s], i1 = hi[s];
        const VkDescSlot *ds = &d[d0[s]];
        uint32_t dbytes = (uint32_t)(((s + 1 < n_sets ? d0[s + 1] : nd) - d0[s]) * sizeof *d);
        if (s && i1 == i0) {
            if (set1_first) {
                VkDescriptorSet set = alloc_set(ex, info->set_layout1);
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 1, 1, &set, 0, NULL);
                cx->B.ds_bound[1] = set, cx->B.ds_pushed[1] = 0;
                cx->RS.cmds++;
            }
            continue;
        }
        if (s ? info->push1 : info->push) {
            uint32_t n = gfx && push_skip_on() ? dbytes : 0;
            if (n && cx->B.ds_pushed[s] && cx->DS.pn[s] == n && !memcmp(cx->DS.psig[s], ds, n)) {
                BS.push_skip++;
                continue;
            }
            if (info->tpl[s]) rec_push_tpl(vk_pipeline_push_tpl_fn(), ex->cb, info->tpl[s], info->layout, s, ds, dbytes);
            else rec_push(vk_pipeline_push_fn(), ex->cb, info->bind_point, info->layout, s, i1 - i0, w + i0);
            BS.push++;
            cx->RS.cmds++;
            if (gfx) {
                cx->B.ds_bound[s] = VK_NULL_HANDLE, cx->B.ds_pushed[s] = n != 0;
                if (n) cx->DS.pn[s] = n, memcpy(cx->DS.psig[s], ds, n);
            }
        } else if (!gfx || s > 1) {
            VkDescriptorSet set = alloc_set(ex, s ? info->set_layout1 : info->set_layout);
            write_set(info, s, set, ds, w, i0, i1);
            vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
        } else {
            VkDescriptorSetLayout sl = s ? info->set_layout1 : info->set_layout;
            VkDescriptorSet set = cx->B.ds_last[s];
            if (!set || cx->B.ds_last_layout[s] != sl || cx->DS.n[s] != dbytes || memcmp(cx->DS.sig[s], ds, dbytes)) {
                set = alloc_set(ex, sl);
                write_set(info, s, set, ds, w, i0, i1);
                cx->B.ds_last[s] = set, cx->B.ds_last_layout[s] = sl, cx->DS.n[s] = dbytes;
                memcpy(cx->DS.sig[s], ds, dbytes);
                BS.set_new++;
                cx->RS.cmds++;
            } else {
                BS.set_reuse++;
            }
            if (cx->B.ds_bound[s] != set) {
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
                cx->B.ds_bound[s] = set, cx->B.ds_pushed[s] = 0;
                cx->RS.cmds++;
            }
        }
    }
    if (info->pool) {
        pool_sync_draw(ex);
        vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 2, 1, &PL.set, 0, NULL);
        cx->RS.volatile_ = 1;
    }
    if (skip) {
        cx->RS.rec = 0;
        int valid = !cx->RS.volatile_ && !cx->RS.cnt_over && cx->RS.hn;
        for (uint32_t s = 0; s < n_sets; s++)
            if (hi[s] > lo[s] && (s ? info->push1 : info->push) && !cx->B.ds_pushed[s]) valid = 0;
        if (skip == 2) {
            uint32_t n = (uint32_t)(nd * sizeof *d);
            int wr = n != cx->RS.ckn || memcmp(cx->RS.ck, d, n);
            int cn = cx->RS.ncnt != cx->RS.pncnt ||
                     memcmp(cx->RS.cnt, cx->RS.pcnt, sizeof cx->RS.cnt[0] * (size_t)cx->RS.ncnt);
            if (same && was_valid && (wr || cn || cx->RS.cmds))
                rt_fatal("vk_exec: descriptor build skip check: a draw of program %llu with the inputs of the previous one "
                         "gets %s", (unsigned long long)ex->prog->serial,
                         cx->RS.cmds ? "descriptor commands" : wr ? "other writes" : "other census counts");
            if (same && was_valid) BS.bind_skip++;
            cx->RS.ckn = n;
            memcpy(cx->RS.ck, d, n);
        }
        cx->B.res_valid = valid;
    }
    {
        uint64_t push_dt = bt_now() - push_t0;
        vk_exec_frame.bind_push_ns += push_dt, ph_add(PH_BIND_PUSH, push_dt);
    }
    ex->recorded++;
}


static void bind_pipeline(VkExec *ex, VkPipelineBindPoint bp, VkPipeline p) {
    DrawCtx *const cx = ex->dc;
    int k = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (cx->B.pipe[k] == p) return;
    vkCmdBindPipeline(ex->cb, bp, p);
    ex->recorded++;
    cx->B.pipe[k] = p;
    if (!k) cx->B.st_valid = 0;
}

static VkViewport base_viewport(VkExec *ex) {
    float x = 0, y = 0, w = (float)ex->pass_w, h = (float)ex->pass_h;
    if (ex->has_vp) {
        float k = ex->pass_scale > 1 ? (float)ex->pass_scale : 1.0f;
        x = ex->vp[0][0] * k, y = ex->vp[0][1] * k, w = ex->vp[0][2] * k, h = ex->vp[0][3] * k;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    VkViewport v = {x, y + h, w, -h, ex->depth_range[0], ex->depth_range[1]};
    return v;
}

static VkViewport draw_viewport(VkExec *ex) {
    VkViewport v = base_viewport(ex);
    if (ex->mark) {
        if ((uint32_t)v.width > ex->mark->vw) ex->mark->vw = (uint32_t)v.width;
        if ((uint32_t)-v.height > ex->mark->vh) ex->mark->vh = (uint32_t)-v.height;
    }
    vk_dlaa_viewport(ex, &v);
    return v;
}

static float slope_bias(const VkExec *ex) {
    if (ex->pass_scale > 1) {
        static uint32_t named;
        static int32_t lines;
        uint32_t k = ex->pass_scale;
        if (__atomic_exchange_n(&named, k, __ATOMIC_RELAXED) != k && __atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) < 8)
            rt_log("vk: a pass into a shadow map %u times larger on each side (shadow_scale): its slope-scaled depth bias "
                   "%u times the game's", k, k);
    }
    return ex->pass_scale > 1 ? ex->poly_offset.factor * (float)ex->pass_scale : ex->poly_offset.factor;
}

static void record_fixed(VkExec *ex) {
    VkCommandBuffer cb = ex->cb;
    if (ex->dirty & VK_EXEC_DIRTY_VIEWPORT) {
        VkViewport v = draw_viewport(ex);
        vkCmdSetViewportWithCount(cb, 1, &v);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_SCISSOR) {
        VkRect2D r = vk_exec_scissor(ex);
        vkCmdSetScissorWithCount(cb, 1, &r);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_DEPTH_BIAS) {
        static float bx = -1.0f;
        if (bx < 0.0f) {
            const char *e = cfg_env("NX2SARIA_DEPTH_BIAS_X");
            bx = e && *e ? (float)atof(e) : 1.0f;
            if (bx != 1.0f) rt_log("vk: NX2SARIA_DEPTH_BIAS_X: constant depth bias multiplied by %.1f", bx);
        }
        vkCmdSetDepthBias(cb, ex->poly_offset.units * bx, ex->poly_offset.clamp, slope_bias(ex));
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_BLEND_CONST) {
        vkCmdSetBlendConstants(cb, ex->blend_color);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_STENCIL) {
        for (int f = 0; f < 2; f++) {
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilCompareMask(cb, face, ex->st_vmask[f]);
            vkCmdSetStencilWriteMask(cb, face, ex->st_wmask[f]);
            vkCmdSetStencilReference(cb, face, ex->st_ref[f]);
            ex->recorded += 3;
        }
    }
    ex->dirty = 0;
}

static void bind_vertex_buffers(VkExec *ex) {
    CbState *held = &ex->dc->B;
    VkBuffer vb[17];
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17] = {0};
    void *vpool[17] = {0};
    int dyn = vk_pipeline_vertex_input_dynamic();
    uint64_t gen = vk_mem_gen();
    int same_gen = held->vb_valid && held->vgen == gen;
    if (same_gen && !memcmp(held->vin, ex->vbuf, sizeof held->vin)) {
        int same = 1;
        for (int b = 0; !dyn && b < 16 && same; b++) same = held->vstr[b] == ex->pipe.vstream[b].stride;
        if (same) return;
    }
    for (int b = 0; b < 17; b++) {
        vb[b] = Z.zbuf, vo[b] = 0, vsz[b] = ZERO_BYTES, vstr[b] = b < 16 ? ex->pipe.vstream[b].stride : 0;
        if (b == VK_PIPE_ZERO_BINDING) continue;
        const NVNbufferRange *r = &ex->vbuf[b];
        if (same_gen && held->vin[b].address == r->address && held->vin[b].size == r->size) {
            vb[b] = held->vb[b], vo[b] = held->vo[b], vsz[b] = held->vsz[b];
            vcls[b] = held->vcls[b], vpool[b] = held->vpool[b];
            continue;
        }
        VkMemSpan sp;
        if (!r->address || !vk_mem_resolve(r->address, 1, &sp)) continue;
        VK_MEM_USE(sp);
        uint64_t n = r->size && r->size < sp.avail ? r->size : sp.avail;
        vb[b] = sp.buffer, vo[b] = sp.offset, vsz[b] = n, vcls[b] = sp.cls, vpool[b] = sp.pool;
    }
    memcpy(held->vcls, vcls, sizeof vcls), memcpy(held->vpool, vpool, sizeof vpool);
    memcpy(held->vin, ex->vbuf, sizeof held->vin);
    held->vgen = gen;
    if (held->vb_valid && !memcmp(held->vb, vb, sizeof vb) && !memcmp(held->vo, vo, sizeof vo) &&
        !memcmp(held->vsz, vsz, sizeof vsz) &&
        (dyn || !memcmp(held->vstr, vstr, sizeof vstr)))
        return;
    vkCmdBindVertexBuffers2(ex->cb, 0, 17, vb, vo, vsz, dyn ? NULL : vstr);
    ex->recorded++;
    for (int b = 0; b < 17; b++)
        if (vcls[b]) vk_mem_count(vcls[b], VK_MUSE_VB, vsz[b]);
    if (rt_trace & RT_TRACE_PERF)
        for (int b = 0; b < 17; b++)
            if (vpool[b]) vk_mem_bound_note(vpool[b]);
    memcpy(held->vb, vb, sizeof vb), memcpy(held->vo, vo, sizeof vo), memcpy(held->vsz, vsz, sizeof vsz);
    memcpy(held->vstr, vstr, sizeof vstr);
    held->vb_valid = 1;
}

static void foliage_shadow_cull(const VkExec *ex, VkPipeState *s) {
    static int from = -1, face;
    if (from < 0) {
        const char *e = cfg_get("NX2SARIA_FOLIAGE_SHADOW_CULL");
        from = e && *e ? atoi(e) : 1;
        face = e && strchr(e, 'f') ? NVN_FACE_FRONT : NVN_FACE_BACK;
    }
    if (!ex->color[0].image && ex->depth.image && (int)ex->depth.base_layer >= from && s->poly.cull == NVN_FACE_NONE)
        s->poly.cull = (unsigned)face;
}

static int pipe_skip_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = cfg_value("NX2SARIA_PIPE_SKIP");
        mode = !strcmp(e, "strict") || !strcmp(e, "0") ? 0 : !strcmp(e, "fullscreen") || !strcmp(e, "2") ? 2 : 1;
        static const char *const what[3] = {"only draws into per-frame passes (NX2SARIA_PIPE_SKIP=strict)",
                                            "draws into passes with a target the previous frame rendered; full-screen "
                                            "draws wait (NX2SARIA_PIPE_SKIP=any)",
                                            "those and full-screen draws (NX2SARIA_PIPE_SKIP=fullscreen)"};
        rt_log("vk_exec: draws skipped while their pipeline builds: %s", what[mode]);
    }
    return mode;
}

static int writers_wait_on(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_DRAW_WRITERS_WAIT");
        if (!on) rt_log("vk_exec: a draw whose shaders write memory is skipped like any other while its pipeline builds "
                        "(NX2SARIA_DRAW_WRITERS_WAIT=0)");
    }
    return on;
}

static int stage_reads_images(const GfxShader *s) {
    const GfxShaderRes *r;
    return s && (gfx_shader_resources(s, GFX_RES_TEX, &r) > 0 || gfx_shader_resources(s, GFX_RES_IMG, &r) > 0);
}


static int stage_writes(const GfxShader *s) {
    const GfxShaderRes *r;
    if (!s) return 0;
    for (int i = 0, n = gfx_shader_resources(s, GFX_RES_SSBO, &r); i < n; i++)
        if (r[i].flags & 1) return 1;
    for (int i = 0, n = gfx_shader_resources(s, GFX_RES_IMG, &r); i < n; i++)
        if (r[i].flags & 4) return 1;
    return 0;
}

static int draw_writes(const GfxShader *vs, const GfxShader *ts, const GfxShader *fs) {
    return writers_wait_on() && (stage_writes(vs) || stage_writes(ts) || stage_writes(fs));
}

static uint64_t *frame_wait(void) {
    static uint64_t frame = ~0ull, left, per_frame = ~0ull;
    if (per_frame == ~0ull) {
        long long ms = cfg_int("NX2SARIA_PIPE_FRAME_WAIT_MS");
        per_frame = (uint64_t)(ms < 0 ? 0 : ms > 500 ? 500 : ms) * 1000000ull;
        rt_log("vk_exec: the full-screen and one-shot draws of a frame wait up to %llu ms in all for their shaders and "
               "pipelines (NX2SARIA_PIPE_FRAME_WAIT_MS), then are skipped while a worker builds them",
               (unsigned long long)(per_frame / 1000000));
    }
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = per_frame;
    return &left;
}

static uint32_t pool_ms(const uint64_t *pool, uint64_t t0) {
    if (!pool) return UINT32_MAX;
    uint64_t spent = kern_now_ns() - t0;
    return spent < *pool ? (uint32_t)((*pool - spent) / 1000000) : 0;
}

static void pool_charge(uint64_t *pool, uint64_t t0) {
    if (!pool) return;
    uint64_t spent = kern_now_ns() - t0;
    *pool = spent < *pool ? *pool - spent : 0;
}

static int mesh_whole(int mesh) { return mesh && !vk_pipeline_mesh_libs(); }

static uint32_t oneshot_ms(const uint64_t *pool, int mesh, const VkPipeState *s) {
    uint32_t ms = (uint32_t)(*pool / 1000000);
    if (!mesh || !ms) return ms;
    if (vk_pipeline_building_ms(s) > MESH_WAIT_MS) return 0;
    return ms > MESH_WAIT_MS ? MESH_WAIT_MS : ms;
}

enum { WK_LINK, WK_FULL, WK_LAST, WK_ONESHOT, WK_ONESHOT_MESH, WK_SHADER, WK_N };
static const char *const wk_names[WK_N] = {"a fast link", "full-screen", "full-screen with no picture to hold",
                                           "one-shot", "one-shot mesh", "one-shot, for a shader"};
static struct { uint64_t n[WK_N], ns[WK_N], max_ns[WK_N], left[WK_N], frame_ns, frame_max_ns, frame, writers; } WS;

static void wait_note(int kind, uint64_t t0, int left) {
    uint64_t ns = kern_now_ns() - t0, f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    WS.n[kind]++, WS.ns[kind] += ns, WS.left[kind] += left != 0;
    if (ns > WS.max_ns[kind]) WS.max_ns[kind] = ns;
    if (f != WS.frame) WS.frame = f, WS.frame_ns = 0;
    WS.frame_ns += ns;
    if (WS.frame_ns > WS.frame_max_ns) WS.frame_max_ns = WS.frame_ns;
}

static uint32_t link_draw_ms(void) {
    static int ms = -1;
    if (ms < 0) {
        long long v = cfg_int("NX2SARIA_PIPE_LINK_DRAW_MS");
        ms = v < 1 ? 1 : v > 50 ? 50 : (int)v;
    }
    return (uint32_t)ms;
}
#define LINK_DRAW_MS link_draw_ms()
#define FULLSCREEN_LAST_MS 40u
static uint64_t *last_wait(void) {
    static uint64_t frame = ~0ull, left;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = FULLSCREEN_LAST_MS * 1000000ull;
    return &left;
}
static uint64_t *link_budget(void) {
    static uint64_t frame = ~0ull, left, per_frame = ~0ull;
    if (per_frame == ~0ull) {
        long long ms = cfg_int("NX2SARIA_PIPE_LINK_MS");
        per_frame = (uint64_t)(ms < 0 ? 0 : ms > 100 ? 100 : ms) * 1000000ull;
        rt_log("vk_exec: draws wait up to %llu ms a frame in all for the fast link of a missing pipeline "
               "(NX2SARIA_PIPE_LINK_MS), %u ms at most each (NX2SARIA_PIPE_LINK_DRAW_MS)",
               (unsigned long long)(per_frame / 1000000), LINK_DRAW_MS);
    }
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame) frame = f, left = per_frame;
    return &left;
}

static int sm_on = -1;

static void memo_key(const VkExec *ex, uint32_t gen, MemoKey *k) {
    k->prog = ex->prog, k->serial = ex->prog->serial, k->gen = gen, k->tex_serial = ex->tex_serial;
    memcpy(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]), memcpy(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]);
    memcpy(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]), memcpy(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]);
    memcpy(&k->pipe, &ex->pipe, sizeof k->pipe);
}

static int memo_match(const VkExec *ex, uint32_t gen, MemoKey *k) {
    if (k->prog != ex->prog || k->serial != ex->prog->serial || k->gen != gen || memcmp(&k->pipe, &ex->pipe, sizeof k->pipe))
        return 0;
    if (k->tex_serial == ex->tex_serial) return 1;
    if (memcmp(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]) || memcmp(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]) ||
        memcmp(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]) || memcmp(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]))
        return 0;
    k->tex_serial = ex->tex_serial;
    return 1;
}

static int shader_memo_on(void) {
    if (sm_on < 0) {
        const char *e = cfg_value("NX2SARIA_SHADER_MEMO");
        sm_on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
    }
    return sm_on;
}

static int pm_on = -1;

static int pipe_memo_on(void) {
    if (pm_on < 0) pm_on = cfg_bool("NX2SARIA_PIPE_MEMO");
    return pm_on;
}

const char *const vk_draw_skip_names[SK_COUNT] = {
    "rendering disabled", "empty scissor", "no program bound",
    "no vertex (or mesh) stage in the program", "shader still translating",
    "shader failed to translate (see gfx_shader: translation failed)", "replaced by the upscaler",
    "pipeline still building", "pipeline could not be created", "uses the texture pools (NX2SARIA_POOL=0)",
    "excluded by NX2SARIA_SKIP_PROG", "excluded by NX2SARIA_SKIP_SHADERS"};
#define DRAW_STATS_FRAMES 600
static struct { uint64_t issued[2], skipped[2][SK_COUNT], frame; uint32_t first[2]; } DRS;

static void draw_stats_tick(void) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!DRS.frame) DRS.frame = f;
    if (f - DRS.frame < DRAW_STATS_FRAMES) return;
    uint64_t waits = 0, wait_ns = 0;
    for (int k = 0; k < WK_N; k++) waits += WS.n[k], wait_ns += WS.ns[k];
    static uint64_t steady_logged;
    if (waits || vk_exec_steady_passes != steady_logged) {
        char line[1200];
        int o = snprintf(line, sizeof line, "vk_exec: over %llu frames %llu draws found a shader or pipeline missing and "
                         "held the replay thread %.1f ms in all, %.1f ms in the worst frame; by the wait they were allowed:",
                         (unsigned long long)(f - DRS.frame), (unsigned long long)waits, wait_ns / 1e6, WS.frame_max_ns / 1e6);
        for (int k = 0; k < WK_N && o < (int)sizeof line - 320; k++)
            if (WS.n[k])
                o += snprintf(line + o, sizeof line - o, " %s %llu (%.1f ms, longest %.1f ms, %llu skipped at its end);",
                              wk_names[k], (unsigned long long)WS.n[k], WS.ns[k] / 1e6, WS.max_ns[k] / 1e6,
                              (unsigned long long)WS.left[k]);
        snprintf(line + o, sizeof line - o, " %llu lookups found only steady targets and skipped a draw that would have "
                 "waited as one-shot; %llu draws into per-frame targets waited as one-shot because their shaders write "
                 "memory (NX2SARIA_DRAW_WRITERS_WAIT)", (unsigned long long)(vk_exec_steady_passes - steady_logged),
                 (unsigned long long)WS.writers);
        rt_log("%s", line);
        steady_logged = vk_exec_steady_passes;
        memset(&WS, 0, sizeof WS);
    }
    for (int m = 0; m < 2; m++) {
        uint64_t any = 0;
        for (int k = 0; k < SK_COUNT; k++) any += DRS.skipped[m][k];
        if (!any) continue;
        char line[512];
        int o = snprintf(line, sizeof line, "vk: %s draws over %llu frames: %llu issued, skipped:", m ? "mesh" : "vertex",
                         (unsigned long long)(f - DRS.frame), (unsigned long long)DRS.issued[m]);
        for (int k = 0; k < SK_COUNT && o < (int)sizeof line - 60; k++)
            if (DRS.skipped[m][k]) o += snprintf(line + o, sizeof line - o, " %s %llu;", vk_draw_skip_names[k], (unsigned long long)DRS.skipped[m][k]);
        rt_log("%s", line);
    }
    memset(&DRS, 0, sizeof DRS.issued + sizeof DRS.skipped);
    DRS.frame = f;
}

static uint32_t pending_draws;
uint32_t vk_exec_pending_draws_take(void) { return __atomic_exchange_n(&pending_draws, 0, __ATOMIC_RELAXED); }

static int draw_skipped(const VkExec *ex, int mesh, int why) {
    DRS.skipped[mesh][why]++;
    if (vk_dump_armed) vk_dump_draw_skipped(ex, mesh, why);
    if (why == SK_SHADER_PENDING || why == SK_PIPE_PENDING) __atomic_add_fetch(&pending_draws, 1, __ATOMIC_RELAXED);
    if (why == SK_PIPE_PENDING) perf_count(PERF_CNT_PIPE_SKIP);
    if (!(DRS.first[mesh] >> why & 1)) {
        DRS.first[mesh] |= 1u << why;
        rt_log("vk: first %s draw skipped because: %s (program %llu)", mesh ? "mesh" : "vertex", vk_draw_skip_names[why],
               ex->prog ? (unsigned long long)ex->prog->serial : 0ull);
    }
    draw_stats_tick();
    return 0;
}

static int skip_prog_list[32], n_skip_prog = -1;
static int skip_prog_wanted(uint64_t serial) {
    if (n_skip_prog < 0) {
            const char *e = cfg_env("NX2SARIA_SKIP_PROG");
        n_skip_prog = 0;
        while (e && *e && n_skip_prog < 32) {
            char *end;
            long v = strtol(e, &end, 10);
            if (end == e) break;
            skip_prog_list[n_skip_prog++] = (int)v;
            e = *end == ',' ? end + 1 : end;
        }
        if (n_skip_prog) rt_log("vk: NX2SARIA_SKIP_PROG: dropping draws of %d program(s)", n_skip_prog);
    }
    for (int i = 0; i < n_skip_prog; i++)
        if ((uint64_t)skip_prog_list[i] == serial) return 1;
    return 0;
}

#define PROG_LEAD_N 16384
static struct { int on; uint64_t seen[PROG_LEAD_N]; uint64_t n[3][6], logged; } PLD = {-1};

static int prog_lead_on(void) {
    if (PLD.on < 0) PLD.on = cfg_get("NX2SARIA_PROG_LEAD") != NULL && strcmp(cfg_get("NX2SARIA_PROG_LEAD"), "0") != 0;
    return PLD.on;
}

static void prog_lead(const prog_t *p, int mesh) {
    uint32_t i = (uint32_t)p->serial & (PROG_LEAD_N - 1);
    if (PLD.seen[i] == p->serial) return;
    PLD.seen[i] = p->serial;
    uint64_t made = nvn_prog_made_ns(p->serial);
    if (!made) return;
    double ms = (double)(plat_now_ns() - made) / 1e6;
    PLD.n[mesh][ms < 17 ? 0 : ms < 100 ? 1 : ms < 1000 ? 2 : ms < 5000 ? 3 : ms < 30000 ? 4 : 5]++;
    uint64_t tot = 0;
    for (int m = 0; m < 3; m++)
        for (int k = 0; k < 6; k++) tot += PLD.n[m][k];
    if (tot - PLD.logged < 200) return;
    PLD.logged = tot;
    static const char *const kind[3] = {"vertex", "mesh", "compute"};
    for (int m = 0; m < 3; m++)
        rt_log("vk: %s programs were made before their first use by: under 17 ms %llu, 17-100 ms %llu, 0.1-1 s %llu, 1-5 s "
               "%llu, 5-30 s %llu, longer %llu (NX2SARIA_PROG_LEAD)", kind[m], (unsigned long long)PLD.n[m][0],
               (unsigned long long)PLD.n[m][1], (unsigned long long)PLD.n[m][2], (unsigned long long)PLD.n[m][3],
               (unsigned long long)PLD.n[m][4], (unsigned long long)PLD.n[m][5]);
}


static volatile uint64_t mfb_hold[MFB_HOLD_SLOTS];
static volatile uint64_t mfb_held_draws;

static int mfb_hold_on(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("MESH_FALLBACK_HOLD", "1");
    return on;
}

static void mfb_hold_set(uint64_t list, int waits) {
    mfb_hold_mark(mfb_hold, list, waits, __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
}

static int mfb_held(uint64_t list) {
    return mfb_hold_test(mfb_hold, list, __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
}

static int mfb_hold_skip(VkExec *ex) {
    uint64_t n = __atomic_add_fetch(&mfb_held_draws, 1, __ATOMIC_RELAXED);
    if (!(n & (n - 1)))
        rt_log("vk: mesh fallback: a list's depth-only draw is left out while its colour draw waits for a pipeline (%llu "
               "so far; NX2SARIA_MESH_FALLBACK_HOLD=0 draws it)", (unsigned long long)n);
    return draw_skipped(ex, 1, SK_PIPE_PENDING);
}

static int mfb_hold_colour(const VkExec *ex) {
    for (int i = 0; i < ex->rt.n && i < 8; i++)
        if (ex->rt.color[i]) return 1;
    return 0;
}

static int mfb_hold_role(const VkExec *ex, const VkPipeState *s) {
    for (uint32_t i = 0; i < ex->pass_ncolor; i++)
        if (ex->color[i].image && vk_pipeline_target_wmask(s, (int)i)) return 1;
    return ex->depth.image && s->ds.dt && s->ds.dw ? 2 : 0;
}

typedef struct { const VkPipeInfo *info; VkPipeState requested; } MeshPrepared;

static int prepare_draw_ex(VkExec *ex, int prim, int32_t count, int restart, int mesh, MeshPrepared *result) {
    if (result) result->info = NULL;
    DrawCtx *const cx = ex->dc;
    int fb = mesh == 2;
    mesh = mesh != 0;
    if (ex->render_disabled) return vk_cond_draws_skip += ex->has_render_cond, draw_skipped(ex, mesh, SK_DISABLED);
    cb_res(ex);
    dummies_init();
    if (ex->pass_noatt) {
        uint32_t w, h;
        vk_exec_noatt_dims(ex, &w, &h);
        if (ex->in_pass && (w > ex->pass_w || h > ex->pass_h)) vk_exec_pass_end(ex);
        if (!ex->in_pass) ex->rt_resolved = 0;
    }
    if (!ex->in_pass) ex->dirty |= VK_EXEC_DIRTY_VIEWPORT | VK_EXEC_DIRTY_SCISSOR | VK_EXEC_DIRTY_DEPTH_BIAS;
    vk_exec_pass_begin(ex);
    VkRect2D sc = vk_exec_scissor(ex);
    if (!sc.extent.width || !sc.extent.height) return draw_skipped(ex, mesh, SK_SCISSOR);
    if (!ex->prog) return draw_skipped(ex, mesh, SK_NO_PROGRAM);
    if (prog_lead_on()) prog_lead(ex->prog, mesh);
    if (n_skip_prog && skip_prog_wanted(ex->prog->serial)) return draw_skipped(ex, mesh, SK_PROG_FILTER);
    ph_mark(PH_ENTRY);
    int has_vs, has_fs, vs_wait = 0, fs_wait = 0, memo = mesh ? 0 : shader_memo_on();
    int has_ts = 0, ts_wait = 0, vst = mesh ? NVN_STAGE_MESH : NVN_STAGE_VS;
    const GfxShader *vs, *fs, *ts = NULL;
    uint32_t gen = memo ? vk_texture_slots_gen() : 0;
    if (memo && cx->SM.valid && memo_match(ex, gen, &cx->SM.k)) {
        vs = cx->SM.vs, fs = cx->SM.fs, has_vs = 1, has_fs = cx->SM.has_fs;
        if (memo == 2) {
            int hv, hf, wv, wf;
            const GfxShader *v2 = shader_for(ex, NVN_STAGE_VS, &hv, &wv, 0), *f2 = shader_for(ex, NVN_STAGE_FS, &hf, &wf, 0);
            if (v2 != vs || f2 != fs || hf != has_fs || wv || wf)
                rt_fatal("vk_exec: shader memo of program %llu differs from the lookup", (unsigned long long)ex->prog->serial);
        }
    } else {
        cx->SM.valid = 0;
        if (fb) vs = cx->mfb_cur_vs, has_vs = 1;
        else vs = shader_for(ex, vst, &has_vs, &vs_wait, 0);
        if (mesh && !fb) ts = shader_for(ex, NVN_STAGE_TASK, &has_ts, &ts_wait, 0);
        fs = shader_for(ex, NVN_STAGE_FS, &has_fs, &fs_wait, 0);
        if (memo && vs && !vs_wait && !fs_wait && (!has_fs || fs))
            memo_key(ex, gen, &cx->SM.k), cx->SM.vs = vs, cx->SM.fs = fs, cx->SM.has_fs = has_fs, cx->SM.valid = 1;
    }
    if (!has_vs) return draw_skipped(ex, mesh, SK_NO_VERTEX_STAGE);
    int oneshot = -1, skip = pipe_skip_mode();
    int writer = 0;
    if (vs_wait || fs_wait || ts_wait) {
        vk_exec_ahead(ex);
        oneshot = !vk_exec_targets_recent(ex, skip >= 1);
        if (!oneshot && draw_writes(vs, ts, fs)) oneshot = writer = 1, WS.writers++;
        if (!oneshot)
            return perf_count(PERF_CNT_SHADER_SKIP), draw_skipped(ex, mesh, SK_SHADER_PENDING);
        perf_count(PERF_CNT_ONESHOT_WAIT);
        uint64_t *pool = oneshot_budget_on() ? frame_wait() : NULL, x0 = kern_now_ns();
        uint32_t ms;
        if (vs_wait && (ms = pool_ms(pool, x0))) vs = shader_for(ex, vst, &has_vs, &vs_wait, ms);
        if (ts_wait && (ms = pool_ms(pool, x0))) ts = shader_for(ex, NVN_STAGE_TASK, &has_ts, &ts_wait, ms);
        if (fs_wait && (ms = pool_ms(pool, x0))) fs = shader_for(ex, NVN_STAGE_FS, &has_fs, &fs_wait, ms);
        pool_charge(pool, x0);
        wait_note(WK_SHADER, x0, vs_wait || ts_wait || fs_wait);
        if (stutter_map_on)
            stutter_miss(1, mesh && !fb, vs && !vs_wait ? gfx_shader_input_id(vs) : NULL,
                         fs && !fs_wait ? gfx_shader_input_id(fs) : NULL, kern_now_ns() - x0, vs_wait || ts_wait || fs_wait);
        if (vs_wait || ts_wait || fs_wait)
            return perf_count(PERF_CNT_SHADER_SKIP), draw_skipped(ex, mesh, SK_SHADER_PENDING);
    }
    if (!vs || (has_fs && !fs) || (has_ts && !ts)) return draw_skipped(ex, mesh, SK_SHADER_FAILED);
    if (shader_skipped(vs) || shader_skipped(fs)) return draw_skipped(ex, mesh, SK_SHADER_FILTER);
    if (vk_dlaa_skip_draw(ex, fs)) return draw_skipped(ex, mesh, SK_DLAA);
    ph_mark(PH_SHADER);
    uint32_t fbl = feedback_prepare(ex, fs);
    VkPipeState s = ex->pipe;
    s.vs = vs, s.fs = fs, s.ts = ts;
    if (mesh) foliage_shadow_cull(ex, &s);
    if (fb) memset(s.vattr, 0, sizeof s.vattr), memset(s.vstream, 0, sizeof s.vstream);
    s.feedback = (uint8_t)((fbl & 0xff ? 1 : 0) | (fbl & 0x100 ? 2 : 0));
    s.prim = (uint8_t)prim;
    s.depth_minus_w = (uint8_t)(nvn_depth_minus_w() != 0);
    s.y_flip = 1;
    s.prim_restart = (uint8_t)(restart != 0);
    VkPipeState requested;
    if (result) requested = s;
    const VkPipeInfo *info = NULL;
    int pending;
    uint32_t wait_ms = 0;
    uint64_t t0 = 0;
    uint64_t *budget = NULL;
    int linking = 0;
    int fullscreen = 0;
    if (oneshot > 0 && oneshot_budget_on()) {
        budget = frame_wait();
        wait_ms = oneshot_ms(budget, mesh && !fb, &s);
        if (wait_ms) t0 = kern_now_ns();
    } else if (oneshot > 0) {
        wait_ms = ONESHOT_WAIT_MS;
    } else if (skip < 2 && count <= 6 && (uint64_t)sc.extent.width * sc.extent.height * 2 >= (uint64_t)ex->pass_w * ex->pass_h) {
        budget = frame_wait(), fullscreen = 1;
        wait_ms = (uint32_t)(*budget / 1000000);
        if (wait_ms) t0 = kern_now_ns();
    } else {
        budget = link_budget();
        wait_ms = __atomic_load_n(&pending_draws, __ATOMIC_RELAXED) ? 0 : (uint32_t)(*budget / 1000000);
        if (wait_ms > LINK_DRAW_MS) wait_ms = LINK_DRAW_MS;
        linking = wait_ms != 0;
        if (!wait_ms) budget = NULL;
    }
    VkPipeline p;
    const char *why = NULL;
    uint64_t w0 = 0, m0 = 0;
    int kind = WK_LINK;
    if (pipe_memo_on() && cx->PM.valid && !memcmp(&cx->PM.s, &s, sizeof s)) {
        info = cx->PM.info, pending = 0;
        p = __atomic_load_n(&info->pipeline, __ATOMIC_ACQUIRE);
    } else {
        p = vk_pipeline_graphics_async(&s, &info, &pending, 0);
        if (pending) {
            m0 = kern_now_ns();
            kind = oneshot > 0 ? (mesh ? WK_ONESHOT_MESH : WK_ONESHOT) : fullscreen ? WK_FULL : WK_LINK;
            if (linking) t0 = kern_now_ns();
            else w0 = t0 ? t0 : wait_ms ? kern_now_ns() : 0;
            vk_exec_ahead(ex);
            double building = wait_ms && linking ? vk_pipeline_building_ms(&s) : 0;
            if (wait_ms && linking) {
                uint32_t left = building >= LINK_DRAW_MS ? 0 : LINK_DRAW_MS - (uint32_t)building;
                if (wait_ms > left) wait_ms = left;
            }
            if (wait_ms && linking && building > MESH_WAIT_MS && mesh_whole(mesh && !fb)) wait_ms = 0;
            if (wait_ms) p = vk_pipeline_graphics_async(&s, &info, &pending, wait_ms);
        }
        if (t0 && budget) {
            uint64_t spent = kern_now_ns() - t0;
            *budget = spent < *budget ? *budget - spent : 0;
        }
        why = writer ? "writes memory, shader" : oneshot > 0 ? "one-shot, shader" : linking ? NULL
            : wait_ms ? "full-screen" : NULL;
        if (pending && oneshot < 0 && (!vk_exec_targets_recent(ex, skip >= 1) || (writer = draw_writes(vs, ts, fs)))) {
            perf_count(PERF_CNT_ONESHOT_WAIT);
            WS.writers += (uint64_t)writer;
            why = writer ? "writes memory" : "one-shot";
            kind = mesh ? WK_ONESHOT_MESH : WK_ONESHOT;
            if (!w0) w0 = kern_now_ns();
            if (oneshot_budget_on()) {
                uint64_t *pool = frame_wait(), b0 = kern_now_ns();
                p = vk_pipeline_graphics_async(&s, &info, &pending, oneshot_ms(pool, mesh && !fb, &s));
                pool_charge(pool, b0);
            } else {
                p = vk_pipeline_graphics_async(&s, &info, &pending, ONESHOT_WAIT_MS);
            }
        }
        uint64_t *last = pending && fullscreen && !vk_frame_hold_left() ? last_wait() : NULL;
        if (last && *last >= 1000000) {
            why = "full-screen, nothing left to hold the picture";
            kind = WK_LAST;
            if (!w0) w0 = kern_now_ns();
            uint64_t b0 = kern_now_ns();
            p = vk_pipeline_graphics_async(&s, &info, &pending, (uint32_t)(*last / 1000000));
            pool_charge(last, b0);
        }
        if (m0) wait_note(kind, m0, pending), draw_stats_tick();
        if (m0 && stutter_map_on)
            stutter_miss(0, mesh && !fb, vs ? gfx_shader_input_id(vs) : NULL, fs ? gfx_shader_input_id(fs) : NULL,
                         kern_now_ns() - m0, pending);
        cx->PM.valid = !pending && p && info;
        if (cx->PM.valid) cx->PM.s = s, cx->PM.info = info;
    }
    uint64_t waited = w0 ? kern_now_ns() - w0 : 0;
    static int wait_logs;
    if (why && (waited >= 2000000 || (pending && kind == WK_ONESHOT)) && wait_logs < 200) {
        char tg[512];
        wait_logs++;
        vk_exec_targets_describe(ex, tg, sizeof tg);
        rt_log("vk_exec: frame %llu: a draw waited %.1f ms for its pipeline (%s%s%s): %u vertices, pass %ux%u, %s",
               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, waited / 1e6, why,
               s.feedback ? ", feedback loop" : "", pending ? ", skipped after all" : "", count, ex->pass_w, ex->pass_h,
               tg);
    }
    if (fb && cx->mfb_list) {
        int role = mfb_hold_role(ex, &s);
        if (role == 1) mfb_hold_set(cx->mfb_list, pending);
        else if (role == 2 && !pending && mfb_held(cx->mfb_list)) return mfb_hold_skip(ex);
    }
    if (pending) return draw_skipped(ex, mesh, SK_PIPE_PENDING);
    if (!p || !info) return draw_skipped(ex, mesh, SK_PIPE_FAILED);
    if (info->pool && !pool_on()) return draw_skipped(ex, mesh, SK_POOL_OFF);
    ph_mark(PH_PIPE);
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    if (!cx->B.st_valid || memcmp(&cx->B.st, &s, sizeof s)) {
        static int delta = -1;
        if (delta < 0) {
            delta = cfg_bool("NX2SARIA_DYN_DELTA");
            if (!delta) rt_log("vk_exec: every dynamic state recorded after each pipeline bind (NX2SARIA_DYN_DELTA=0)");
        }
        if (delta) vk_exec_frame.dyn_calls += (uint64_t)vk_pipeline_set_dynamic_delta(ex->cb, &s, &cx->B.dyn, mesh && !fb && !info->emu,
                                                                                       &vk_exec_frame.dyn_left);
        else vk_pipeline_set_dynamic(ex->cb, &s);
        ex->recorded++;
        cx->B.st = s, cx->B.st_valid = 1;
        if (ex->mark && vk_exec_frame.draws == ex->mark_draws0) ex->mark->first[0] = vs, ex->mark->first[1] = fs;
        if (vk_draw_marks_on)
            vk_draw_mark(ex->cb, ex->mark, vs, fs, (uint32_t)(vk_exec_frame.draws - ex->mark_draws0));
    }
    vk_pipeline_line_width(ex->cb, &s, ex->line_width);
    ph_mark(PH_STATE);
    if (!mesh) {
        vk_dlaa_draw(ex);
        vk_mvsurvey_draw(ex);
        vk_mvprev_draw(ex);
    }
    vk_dlaa_jitter_check(ex);
    record_fixed(ex);
    repeat_stats(ex, info);
    ph_mark(PH_HOOKS);
    {
        uint64_t bt = bt_now();
        bind_resources(ex, info);
        vk_exec_frame.bind_ns += (uint64_t)(bt_now() - bt);
        vk_exec_frame.bind_calls++;
    }
    ph_mark(PH_BIND);
    if (vk_texture_deferred()) return 0;
    if (fbl & ex->fb_dirty) feedback_barrier(ex);
    feedback_written(ex, &s, fbl);
    if (!mesh) bind_vertex_buffers(ex);
    if (vk_dump_armed) vk_dump_draw(ex, &s, p, info, mesh && !fb);
    if (vk_hdr_track) vk_hdr_draw(ex, info);
    if (vk_texture_deferred()) return 0;
    vk_exec_oq_draw(ex);
    DRS.issued[mesh]++;
    draw_stats_tick();
    if (result) result->info = info, result->requested = requested;
    ph_mark(PH_VBUF);
    return 1;
}

static void mesh_prepared_check(const MeshPrepared *result) {
    int match = vk_pipeline_graphics_key_matches(&result->requested, result->info);
    mesh_pipeline_check_report(match);
}

static int prepare_draw(VkExec *ex, int prim, int32_t count, int restart) {
    return prepare_draw_ex(ex, prim, count, restart, 0, NULL);
}

uint64_t vk_exec_ahead_draws, vk_exec_ahead_lookups;
static struct { int valid; const prog_t *prog; uint64_t serial; uint32_t tex_serial; VkPipeState pipe; } AHL;

void vk_exec_ahead_begin(void) { AHL.valid = 0; }

static void ahead_draw(VkExec *sx, int mesh) {
    vk_exec_ahead_draws++;
    if (!sx->prog || (mesh && (!vk_pipeline_mesh_libs() || vk_ctx()->mesh_fallback))) return;
    if (AHL.valid && AHL.prog == sx->prog && AHL.serial == sx->prog->serial && AHL.tex_serial == sx->tex_serial &&
        !memcmp(&AHL.pipe, &sx->pipe, sizeof AHL.pipe))
        return;
    AHL.valid = 1, AHL.prog = sx->prog, AHL.serial = sx->prog->serial, AHL.tex_serial = sx->tex_serial, AHL.pipe = sx->pipe;
    vk_exec_ahead_lookups++;
    int has_vs, has_fs, has_ts = 0, vs_wait = 0, fs_wait = 0, ts_wait = 0;
    const GfxShader *vs = shader_for(sx, mesh ? NVN_STAGE_MESH : NVN_STAGE_VS, &has_vs, &vs_wait, 0), *ts = NULL;
    if (!has_vs) return;
    if (mesh) ts = shader_for(sx, NVN_STAGE_TASK, &has_ts, &ts_wait, 0);
    const GfxShader *fs = shader_for(sx, NVN_STAGE_FS, &has_fs, &fs_wait, 0);
    if (!vs || vs_wait || fs_wait || ts_wait || (has_fs && !fs) || (has_ts && !ts) || shader_skipped(vs) ||
        shader_skipped(fs))
        return;
    VkPipeState s = sx->pipe;
    s.vs = vs, s.fs = fs, s.ts = ts;
    s.feedback = 0;
    s.prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    s.depth_minus_w = (uint8_t)(nvn_depth_minus_w() != 0);
    s.y_flip = 1;
    s.prim_restart = 0;
    vk_pipeline_hint(&s);
}

void vk_exec_ahead_op(VkExec *sx, uint32_t op, const void *payload) {
    if (!sx->dc) sx->dc = dc;
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = payload;
        sx->prog = b->prog, sx->prog_stages = b->stages;
        break;
    }
    case OP_BIND_STATE: {
        const p_bind_state *b = payload;
        blend_st bs;
        switch (b->kind) {
        case 0:
            memcpy(&bs, b->bytes, sizeof bs);
            if (bs.target < 8) sx->pipe.blend[bs.target] = bs;
            break;
        case 1: memcpy(&sx->pipe.color, b->bytes, sizeof sx->pipe.color); break;
        case 2: memcpy(&sx->pipe.chmask, b->bytes, sizeof sx->pipe.chmask); break;
        case 3: memcpy(&sx->pipe.ms, b->bytes, sizeof sx->pipe.ms); break;
        case 4: memcpy(&sx->pipe.poly, b->bytes, sizeof sx->pipe.poly); break;
        case 5: memcpy(&sx->pipe.ds, b->bytes, sizeof sx->pipe.ds); break;
        }
        break;
    }
    case OP_BIND_VATTR: {
        const p_bind_vattr *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(sx->pipe.vattr, 0, sizeof sx->pipe.vattr);
        memcpy(&sx->pipe.vattr[b->first], b->st, sizeof(uint32_t) * (size_t)b->n);
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(sx->pipe.vstream, 0, sizeof sx->pipe.vstream);
        memcpy(&sx->pipe.vstream[b->first], b->st, sizeof(vs_st) * (size_t)b->n);
        break;
    }
    case OP_BIND_TEX: case OP_BIND_IMG: {
        const p_bind_handles *b = payload;
        int cap = op == OP_BIND_TEX ? 128 : 32;
        if (b->stage < 0 || b->stage >= 8 || b->first < 0 || b->n < 0 || b->n > 32 || b->first + b->n > cap) break;
        memcpy(op == OP_BIND_TEX ? &sx->tex[b->stage][b->first] : &sx->img[b->stage][b->first], b->h,
               sizeof(uint64_t) * (size_t)b->n);
        sx->tex_serial++;
        break;
    }
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: ahead_draw(sx, 0); break;
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND: ahead_draw(sx, 1); break;
    default: break;
    }
}

#include "mvobj_fs_spv.h"
#include "link_motion_spv.h"
static const char MO_FS_RES[] = "{\"key\":\"mvobj\",\"stage\":\"ps\",\"notes\":[],\"cbufs\":[{\"binding\":1,\"slot\":3}],"
                                "\"sbufs\":[],\"textures\":[],\"images\":[]}";
static const char LINK_FS_RES[] = "{\"key\":\"link-motion\",\"stage\":\"ps\",\"cbufs\":[{\"binding\":1,\"slot\":3}],"
    "\"textures\":[{\"binding\":2,\"type\":3}],\"sbufs\":[],\"images\":[]}";
static struct {
    const GfxShader *fs[16], *link_fs[16];
    int failed;
    uint64_t cand, still, draws, nodata, building, ran, dropped;
    uint64_t ns_check, ns_rec, n_rec, ns_emit, ns_still;
    uint64_t ns_part[3], n_part[3], misses, cyc[3], ncyc, big;
    uint64_t why[5];
    uint64_t no_out, storage, no_fs;
    uint64_t other_cam;
    uint64_t lk[14][5];
    uint64_t e_draws, e_pipe, e_dyn, e_push, e_pushw, e_sets, e_vb, e_setw, e_setreuse;
} MO;

typedef struct {
    VkPipeline pipe;
    const VkPipeInfo *info;
    uint32_t st, desc, vb;
    VkDescriptorSet set[2];
    VkViewport v;
    VkRect2D sc;
    float bias[3];
    VkBuffer ib;
    VkDeviceSize ib_off;
    VkIndexType it;
    uint32_t count, instances, base_instance;
    int32_t base_vertex;
    uint64_t cam;
    int pose_index, pose_view;
    int skin;
} MoDraw;
typedef struct { VkBuffer b[17]; VkDeviceSize o[17], sz[17], str[17]; } MoVb;
static struct {
    uint64_t frame, fence;
    VkFormat depth_fmt;
    MoDraw *d;
    VkPipeState *st;
    VkDescSlot *desc;
    MoVb *vb;
    int n, cap, nst, cst, nvb, cvb;
    uint32_t ndesc, cdesc;
    NVNbufferRange vin[17];
    uint64_t vgen;
    float sup_in[3];
    VkDescriptorBufferInfo sup;
    float par_in[8];
    VkDescriptorBufferInfo par;
    VkDescriptorSetLayout aset_layout[2];
    VkDescriptorSet aset[2];
    uint32_t aset_n[2];
    VkDescSlot aset_d[2][MAX_DESC];
} MQ;

static void *mo_grow(void *p, int *cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    if (!(p = realloc(p, elem * (size_t)c))) rt_fatal("vk_exec: out of memory");
    *cap = c;
    return p;
}

static struct { uint32_t mask; uint64_t n; int ubo, ssbo, tex; } MM[32];
static void mo_mask_note(uint32_t banks, const GfxShader *vs) {
    for (int i = 0; i < 32; i++)
        if (MM[i].mask == banks || !MM[i].n) {
            if (!MM[i].n) {
                const GfxShaderRes *r;
                MM[i].ubo = gfx_shader_resources(vs, GFX_RES_UBO, &r), MM[i].ssbo = gfx_shader_resources(vs, GFX_RES_SSBO, &r);
                MM[i].tex = gfx_shader_resources(vs, GFX_RES_TEX, &r);
            }
            MM[i].mask = banks, MM[i].n++;
            return;
        }
}

static void mo_drop(void) {
    if (MQ.n) MO.dropped++;
    MQ.n = MQ.nst = MQ.nvb = 0, MQ.ndesc = 0, MQ.frame = 0;
}

static uint64_t mo_gen = 1;
static struct { uint32_t mask, chg, n; } MF[16];
static char fm_last[400];

static void skin_begin(uint64_t frame);
static void mo_begin(VkExec *ex, uint64_t frame) {
    mo_drop();
    skin_begin(frame);
    mo_gen++;
    memset(MF, 0, sizeof MF);
    MQ.frame = frame, MQ.fence = ex->fence, MQ.depth_fmt = ex->depth.format;
    memset(MQ.vin, 0, sizeof MQ.vin);
    MQ.vgen = 0, MQ.sup.buffer = MQ.par.buffer = VK_NULL_HANDLE;
    memset(MQ.aset, 0, sizeof MQ.aset);
}

static const GfxShader *mo_fs(int loc) {
    if (loc < 0 || loc >= 16) return NULL;
    if (!MO.fs[loc]) {
        uint32_t w[sizeof mvobj_fs_spv / 4];
        memcpy(w, mvobj_fs_spv, sizeof w);
        w[MVOBJ_FS_LOCATION_WORD] = (uint32_t)loc;
        MO.fs[loc] = gfx_shader_own(w, sizeof w / 4, MO_FS_RES);
        if (!MO.fs[loc]) MO.failed = 1, rt_log("vk_exec: the per-object motion fragment shader is malformed: off");
    }
    return MO.fs[loc];
}

static const GfxShader *link_motion_fs(int loc) {
    if (loc < 0 || loc >= 16) return NULL;
    if (MO.link_fs[loc]) return MO.link_fs[loc];
    uint32_t words[sizeof link_motion_spv / sizeof(uint32_t)];
    memcpy(words, link_motion_spv, sizeof words);
    for (size_t i = 5; i < sizeof words / sizeof *words; i += words[i] >> 16) {
        if (words[i] == ((4u << 16) | 71u) && words[i + 2] == 30 && words[i + 3] == 15) {
            words[i + 3] = (uint32_t)loc;
            MO.link_fs[loc] = gfx_shader_own(words, sizeof words / sizeof *words, LINK_FS_RES);
            break;
        }
    }
    return MO.link_fs[loc];
}

static VkWriteDescriptorSet mo_write(const VkPipeBinding *b, const VkDescSlot *d) {
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                              .descriptorCount = b->count ? b->count : 1, .descriptorType = b->type};
    if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
        b->type == VK_DESCRIPTOR_TYPE_SAMPLER)
        w.pImageInfo = &d->image;
    else
        w.pBufferInfo = &d->buffer;
    return w;
}

static int mo_desc(VkExec *ex, const VkPipeInfo *info, MoDraw *q, const VkDescriptorImageInfo *pose_depth) {
    gpa_reader(info);
    if (info->n_bindings > MAX_BINDINGS || info->pool) return 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) total += info->bindings[i].count ? info->bindings[i].count : 1;
    if (total > MAX_DESC) return 0;
    int cdesc = (int)MQ.cdesc;
    MQ.desc = mo_grow(MQ.desc, &cdesc, (int)(MQ.ndesc + total), sizeof *MQ.desc);
    MQ.cdesc = (uint32_t)cdesc;
    VkDescSlot *d = MQ.desc + MQ.ndesc;
    memset(d, 0, total * sizeof *d);
    for (uint32_t i = 0, nd = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        VkDescSlot *o = &d[nd];
        int st = nvn_stage_of(b->stage);
        if (b->kind == VK_PIPE_SUPPORT) {
            float in[3];
            support_in(ex, in);
            if (!MQ.sup.buffer || memcmp(in, MQ.sup_in, sizeof in)) {
                VkBuffer sb;
                VkDeviceSize so;
                support_fill(scratch(ex, SUPPORT_BYTES, &sb, &so), in[0], in[1], in[2]);
                MQ.sup = (VkDescriptorBufferInfo){sb, so, SUPPORT_BYTES};
                memcpy(MQ.sup_in, in, sizeof in);
            }
            o->buffer = MQ.sup;
        } else if (b->kind == GFX_RES_UBO) {
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ubo_info(ex, st, r->slot + (int)k);
        } else if (b->kind == GFX_RES_SSBO && !(r->flags & 1)) {
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ssbo_info(ex, st, r, 0);
        } else if (pose_depth && st == NVN_STAGE_FS && b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            if (cnt != 1) return 0;
            o->image = *pose_depth;
        } else if (b->kind == GFX_RES_TEX && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            vk_texture_lock();
            for (uint32_t k = 0; k < cnt; k++) {
                int slot = r->slot + (int)k;
                tex_info(ex, r, slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0, 0, r->separate, &o[k].image, NULL);
            }
            vk_texture_unlock();
        } else {
            return 0;
        }
        nd += cnt;
    }
    uint32_t n_sets = info->n_sets ? info->n_sets : 1;
    q->set[0] = q->set[1] = VK_NULL_HANDLE;
    for (uint32_t s = 0, i0 = 0, k0 = 0; s < n_sets && s < 2; s++) {
        uint32_t i1 = i0, k1 = k0;
        while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s))
            k1 += info->bindings[i1].count ? info->bindings[i1].count : 1, i1++;
        if (i1 > i0 && !(s ? info->push1 : info->push)) {
            VkDescriptorSetLayout sl = s ? info->set_layout1 : info->set_layout;
            if (MQ.aset[s] && MQ.aset_layout[s] == sl && MQ.aset_n[s] == k1 - k0 &&
                !memcmp(MQ.aset_d[s], d + k0, (k1 - k0) * sizeof *d)) {
                q->set[s] = MQ.aset[s];
                MO.e_setreuse++;
            } else {
                static VkWriteDescriptorSet w[MAX_BINDINGS];
                VkDescriptorSet set = alloc_set(ex, sl);
                for (uint32_t i = i0, k = k0; i < i1; k += info->bindings[i].count ? info->bindings[i].count : 1, i++)
                    w[i - i0] = mo_write(&info->bindings[i], d + k), w[i - i0].dstSet = set;
                vkUpdateDescriptorSets(vk_ctx()->device, i1 - i0, w, 0, NULL);
                q->set[s] = MQ.aset[s] = set, MQ.aset_layout[s] = sl, MQ.aset_n[s] = k1 - k0;
                memcpy(MQ.aset_d[s], d + k0, (k1 - k0) * sizeof *d);
                MO.e_setw++;
            }
        }
        i0 = i1, k0 = k1;
    }
    q->desc = MQ.ndesc;
    MQ.ndesc += total;
    return 1;
}

static uint32_t mo_vertex_buffers(VkExec *ex) {
    uint64_t gen = vk_mem_gen();
    if (MQ.nvb && MQ.vgen == gen && !memcmp(MQ.vin, ex->vbuf, sizeof MQ.vin)) {
        const MoVb *l = &MQ.vb[MQ.nvb - 1];
        int same = 1;
        if (!vk_pipeline_vertex_input_dynamic())
            for (int b = 0; b < 16 && same; b++) same = l->str[b] == ex->pipe.vstream[b].stride;
        if (same) return (uint32_t)MQ.nvb - 1;
    }
    MQ.vb = mo_grow(MQ.vb, &MQ.cvb, MQ.nvb + 1, sizeof *MQ.vb);
    MoVb *v = &MQ.vb[MQ.nvb];
    for (int b = 0; b < 17; b++) {
        v->b[b] = Z.zbuf, v->o[b] = 0, v->sz[b] = ZERO_BYTES, v->str[b] = b < 16 ? ex->pipe.vstream[b].stride : 0;
        if (b == VK_PIPE_ZERO_BINDING) continue;
        const NVNbufferRange *r = &ex->vbuf[b];
        VkMemSpan sp;
        if (!r->address || !vk_mem_resolve(r->address, 1, &sp)) continue;
        VK_MEM_USE(sp);
        v->b[b] = sp.buffer, v->o[b] = sp.offset, v->sz[b] = r->size && r->size < sp.avail ? r->size : sp.avail;
    }
    memcpy(MQ.vin, ex->vbuf, sizeof MQ.vin);
    MQ.vgen = gen;
    return (uint32_t)MQ.nvb++;
}

static void mo_emit(VkCommandBuffer cb) {
    int dyn_vi = vk_pipeline_vertex_input_dynamic();
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t st = UINT32_MAX, vb = UINT32_MAX;
    VkPipelineLayout lay[2] = {0};
    const VkPipeInfo *pinfo[2] = {0};
    int pdesc[2] = {-1, -1};
    VkDescriptorSet bound[2] = {0};
    const MoDraw *p = NULL;
    for (int n = 0; n < MQ.n; p = &MQ.d[n++]) {
        const MoDraw *q = &MQ.d[n];
        const VkPipeInfo *info = q->info;
        if (q->pipe != pipe) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, q->pipe), pipe = q->pipe, MO.e_pipe++;
        if (q->st != st) vk_pipeline_set_dynamic(cb, &MQ.st[q->st]), st = q->st, MO.e_dyn++;
        if (!p || memcmp(&q->v, &p->v, sizeof q->v)) vkCmdSetViewportWithCount(cb, 1, &q->v);
        if (!p || memcmp(&q->sc, &p->sc, sizeof q->sc)) vkCmdSetScissorWithCount(cb, 1, &q->sc);
        if (!p || memcmp(q->bias, p->bias, sizeof q->bias)) vkCmdSetDepthBias(cb, q->bias[0], q->bias[1], q->bias[2]);
        uint32_t n_sets = info->n_sets ? info->n_sets : 1;
        const VkDescSlot *d = MQ.desc + q->desc;
        for (uint32_t s = 0, i0 = 0, k0 = 0; s < n_sets && s < 2; s++) {
            uint32_t i1 = i0, k1 = k0;
            while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s))
                k1 += info->bindings[i1].count ? info->bindings[i1].count : 1, i1++;
            if (i1 > i0) {
                if (s ? info->push1 : info->push) {
                    int same = lay[s] == info->layout && pinfo[s] == info && pdesc[s] >= 0;
                    const VkDescSlot *pd = same ? MQ.desc + pdesc[s] : NULL;
                    VkWriteDescriptorSet w[MAX_BINDINGS];
                    uint32_t nw = 0;
                    for (uint32_t i = i0, k = k0; i < i1; i++) {
                        uint32_t cnt = info->bindings[i].count ? info->bindings[i].count : 1;
                        if (!same || memcmp(d + k, pd + k, cnt * sizeof *d)) w[nw++] = mo_write(&info->bindings[i], d + k);
                        k += cnt;
                    }
                    if (nw) rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, s, nw, w);
                    MO.e_push += nw != 0, MO.e_pushw += nw;
                    lay[s] = info->layout, pinfo[s] = info, pdesc[s] = (int)q->desc, bound[s] = VK_NULL_HANDLE;
                } else if (q->set[s] != bound[s] || lay[s] != info->layout) {
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, s, 1, &q->set[s], 0, NULL);
                    bound[s] = q->set[s], lay[s] = info->layout, pinfo[s] = info, pdesc[s] = -1, MO.e_sets++;
                }
            }
            i0 = i1, k0 = k1;
        }
        if (q->vb != vb) {
            const MoVb *v = &MQ.vb[q->vb];
            vkCmdBindVertexBuffers2(cb, 0, 17, v->b, v->o, v->sz, dyn_vi ? NULL : v->str);
            vb = q->vb, MO.e_vb++;
        }
        if (!p || q->ib != p->ib || q->ib_off != p->ib_off || q->it != p->it) vkCmdBindIndexBuffer(cb, q->ib, q->ib_off, q->it);
        vkCmdDrawIndexed(cb, q->count, q->instances, 0, q->base_vertex, q->base_instance);
        MO.e_draws++;
    }
}

#include "vk_fg_pose.inc"
#include "vk_skin_motion.inc"

static int mvobj_pick(VkExec *ex, const p_draw_idx *d, const GfxShader **vs_out, uint32_t *banks_out, MotionOwner *owner_out,
                      SkinPick *skin_out, uint64_t t0);
static void mvobj_draw_(VkExec *ex, const p_draw_idx *d, VkBuffer ib, VkDeviceSize ib_off, uint64_t t0,
                        const GfxShader *vs, uint32_t banks, const MotionOwner *owner, const SkinPick *skin);
static PlatMutex mo_lock = PLAT_MUTEX_INIT;

static void mvobj_draw(VkExec *ex, const p_draw_idx *d, VkBuffer ib, VkDeviceSize ib_off) {
    if (MO.failed || !ex->dc->B.st_valid) return;
    if (RT_V111) {
        if (!(vk_dlaa_available() || vk_dlaa_fg_only()) || (!pose_enabled() && !skin_enabled())) return;
    } else if (!vk_mvprev_on()) return;
    static int timed = -1;
    if (timed < 0) timed = rt_knob("MVOBJ_TIMES", "0");
    uint64_t t0 = timed ? kern_now_ns() : 0;
    const GfxShader *vs;
    uint32_t banks;
    MotionOwner owner = {0};
    SkinPick skin = {0};
    int listed = mvobj_pick(ex, d, &vs, &banks, &owner, &skin, t0);
    int lk = listed && vk_exec_shared();
    if (lk) plat_mutex_lock(&mo_lock);
    if (listed) mvobj_draw_(ex, d, ib, ib_off, t0, vs, banks, &owner, skin.stride ? &skin : NULL);
    if (t0) __atomic_add_fetch(&MO.ns_check, kern_now_ns() - t0, __ATOMIC_RELAXED);
    if (lk) plat_mutex_unlock(&mo_lock);
}

static void mvobj_banks(VkExec *ex) {
    DrawCtx *const cx = ex->dc;
    static int on = -1;
    if (on < 0) on = rt_knob("MVOBJ_BANKS", "0");
    if (!on || !cx->B.st_valid) return;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (frame < 2999 || frame > 3000) return;
    static struct { const GfxShader *vs; int bank; uint64_t frame; float w[64]; int n; } tab[4096];
    static int logged;
    const GfxShaderRes *ur;
    int nu = gfx_shader_resources(cx->B.st.vs, GFX_RES_UBO, &ur);
    for (int i = 0; i < nu; i++) {
        int bank = ur[i].slot;
        if (bank < 3 || bank == 4 || bank == 5 || bank == 7 || bank > 16) continue;
        const p_bind_buf *u = &ex->ubo[NVN_STAGE_VS][bank - 3];
        uint32_t n = u->size < 256 ? (uint32_t)u->size / 4 : 64;
        const float *now = n ? (const float *)vk_usnap_bytes(u->addr, n * 4) : NULL;
        if (!now) continue;
        uint32_t k =
            (uint32_t)(((uintptr_t)cx->B.st.vs ^ (uint64_t)bank * 0x9E3779B97F4A7C15ull) * 0xff51afd7ed558ccdull >> 52);
        if (frame == 2999) {
            tab[k].vs = cx->B.st.vs, tab[k].bank = bank, tab[k].frame = frame, tab[k].n = (int)n;
            memcpy(tab[k].w, now, n * 4);
        } else if (tab[k].vs == cx->B.st.vs && tab[k].bank == bank && tab[k].frame == 2999 && logged < 80) {
            char line[700];
            int o = 0, nd = 0;
            for (int j = 0; j < tab[k].n && j < (int)n && o < 600; j++)
                if (memcmp(&tab[k].w[j], &now[j], 4)) nd++, o += snprintf(line + o, sizeof line - o, " [%d] %g>%g", j, tab[k].w[j], now[j]);
            if (nd) logged++, rt_log("mvbanks: vs %p c%d size %#llx: %d words%s", (const void *)cx->B.st.vs, bank,
                                     (unsigned long long)u->size, nd, line);
            tab[k].frame = 0;
        }
    }
}

static int mo_similar(const uint8_t *cur, const uint8_t *old, uint64_t n, int *changed) {
    *changed = 0;
    if (!memcmp(cur, old, n)) return 1;
    const uint32_t *x = (const uint32_t *)cur, *y = (const uint32_t *)old;
    for (uint64_t w = 0; w < n / 4; w++) {
        if (x[w] == y[w]) continue;
        uint32_t ex = x[w] >> 23 & 0xff, ey = y[w] >> 23 & 0xff;
        if (!ex || ex == 0xff || !ey || ey == 0xff) continue;
        float fx, fy;
        memcpy(&fx, &x[w], 4), memcpy(&fy, &y[w], 4);
        float dd = fabsf(fx - fy), tol = 0.1f * fabsf(fy);
        if (dd > (tol < 1.0f ? 1.0f : tol > 4.0f ? 4.0f : tol)) return 0;
    }
    *changed = 1;
    return 1;
}

static int mo_prev_block(int index, const p_bind_buf *u, VkDescriptorBufferInfo *out, int *changed) {
    uint64_t max = LIM.ubo_max, n = u->size < max ? u->size : max, pa;
    int how;
    *changed = 0;
    static long skip = -2;
    if (skip == -2) {
        const char *e = cfg_get("NX2SARIA_MVPREV_SKIP");
        skip = e && *e ? strtol(e, NULL, 16) : 1L << 9 | 1L << 11;
    }
    if (skip >> (index + 3) & 1) return 0;
    uint64_t c0 = __builtin_ia32_rdtsc();
    int found = vk_mvprev_find(index, u->addr, u->size, &pa, &how);
    uint64_t c1 = __builtin_ia32_rdtsc();
    MO.cyc[0] += c1 - c0, MO.ncyc++, MO.big += n;
    if (index >= 0 && index < 14) MO.lk[index][found ? how : MVPREV_NONE]++;
    if (!found || !vk_usnap_find(pa, n, LIM.ubo_align, out)) return 0;
    if (how == MVPREV_SELF) return 1;
    const uint8_t *map = vk_usnap_map(), *cur = vk_usnap_bytes(u->addr, n);
    uint64_t c2 = __builtin_ia32_rdtsc();
    MO.cyc[1] += c2 - c1;
    if (!cur) return 0;
    const uint8_t *old = map + out->offset;
    static long trust = -2;
    if (trust == -2) {
        const char *e = cfg_get("NX2SARIA_MVPREV_TRUST");
        trust = e && *e ? strtol(e, NULL, 16) : 0;
    }
    if (trust >> (index + 3) & 1) {
        *changed = memcmp(cur, old, n) != 0;
        return 1;
    }
    int r = mo_similar(cur, old, n, changed);
    MO.cyc[2] += __builtin_ia32_rdtsc() - c2;
    if (!r && index >= 0 && index < 14) MO.lk[index][4]++;
    return r;
}

static int mo_prev_cached(int index, const p_bind_buf *u, VkDescriptorBufferInfo *out, int *changed) {
    static struct { uint64_t gen, addr, size; int index, r, changed; VkDescriptorBufferInfo out; } bc[4096];
    uint32_t i = (uint32_t)((u->addr ^ (uint64_t)index << 56 ^ u->size << 40) * 0x9E3779B97F4A7C15ull >> 52);
    if (bc[i].gen == mo_gen && bc[i].addr == u->addr && bc[i].size == u->size && bc[i].index == index) {
        *out = bc[i].out, *changed = bc[i].changed;
        return bc[i].r;
    }
    int r = mo_prev_block(index, u, out, changed);
    bc[i].gen = mo_gen, bc[i].addr = u->addr, bc[i].size = u->size, bc[i].index = index;
    bc[i].r = r, bc[i].changed = *changed, bc[i].out = *out;
    return r;
}

static int mo_legacy_previous(VkExec *ex, uint64_t frame, uint32_t banks, uint64_t t0, uint64_t tp,
                              VkDescriptorBufferInfo prevb[17], uint32_t *have_out, uint32_t *changed_mask) {
    static struct { uint64_t frame, key; int moving; VkDescriptorBufferInfo b[17]; uint32_t have; } memo[8192];
    uint64_t key = banks;
    for (int b = 3; b <= 16; b++)
        if (banks >> b & 1 && b != 4) key = (key ^ ex->ubo[NVN_STAGE_VS][b - 3].addr) * 0x100000001b3ull;
    uint32_t mi = (uint32_t)(key * 0xff51afd7ed558ccdull >> 51);
    uint32_t have = 0;
    if (memo[mi].frame == frame && memo[mi].key == key) {
        if (!memo[mi].moving) {
            if (t0) MO.ns_still += kern_now_ns() - tp;
            MO.still++;
            return 0;
        }
        memcpy(prevb, memo[mi].b, sizeof memo[mi].b), have = memo[mi].have;
    } else {
        MO.misses++;
        int moving = 0;
        for (int b = 3; b <= 16; b++) {
            if (!(banks >> b & 1) || b == 4) continue;
            const p_bind_buf *u = &ex->ubo[NVN_STAGE_VS][b - 3];
            int changed;
            if (u->addr && u->size && mo_prev_cached(b - 3, u, &prevb[b], &changed))
                have |= 1u << b, moving |= changed << b;
        }
        memo[mi].frame = frame, memo[mi].key = key, memo[mi].moving = moving;
        memcpy(memo[mi].b, prevb, sizeof memo[mi].b), memo[mi].have = have;
        if (!moving) {
            if (t0) MO.ns_still += kern_now_ns() - tp;
            MO.still++;
            return 0;
        }
    }
    *have_out = have;
    *changed_mask = (uint32_t)memo[mi].moving;
    return 1;
}

static int mvobj_pick(VkExec *ex, const p_draw_idx *d, const GfxShader **vs_out, uint32_t *banks_out, MotionOwner *owner_out,
                      SkinPick *skin_out, uint64_t t0) {
    DrawCtx *const cx = ex->dc;
    if (!vk_exec_in_chunk()) mvobj_banks(ex);
    const VkExecTarget *z = &ex->depth;
    if (!z->image || z->width < DLAA_SCENE_MIN_W || z->format != VK_FORMAT_D32_SFLOAT || cx->B.st.samples > 1 ||
        cx->B.st.raster_discard)
        return 0;
    if (!cx->B.st.ds.dt) return 0;
    if (!cx->B.st.ds.dw && !(RT_V111 && cx->B.st.ds.df == NVN_DEPTH_FUNC_EQUAL && ex->color[0].image &&
                         (vk_pipeline_target_wmask(&cx->B.st, 0) & 7))) return 0;
    if (RT_V111) {
        VkViewport v = base_viewport(ex);
        if (ex->ubo[0][0].size != 0x3500 || d->instances > 1 || d->base_instance || cx->B.st.color.blend_en || cx->B.st.ds.st ||
            ex->pass_layers != 1 || z->level || z->base_layer || z->layers != 1 ||
            v.x != 0.0f || v.y != (float)z->height ||
            v.width != (float)z->width || v.height != -(float)z->height) return 0;
        if (!pose_owner(ex, owner_out)) {
            __atomic_add_fetch(&pose_unowned, 1, __ATOMIC_RELAXED);
            if (!skin_pick(ex, skin_out)) return 0;
        }
    }
    const GfxShaderRes *ur;
    int nu = gfx_shader_resources(cx->B.st.vs, GFX_RES_UBO, &ur), reads = 0;
    for (int i = 0; i < nu; i++) reads |= ur[i].slot >= 3 && ur[i].slot <= 16 && ur[i].slot != 4;
    if (!reads) return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    __atomic_add_fetch(&MO.cand, 1, __ATOMIC_RELAXED);
    if (t0) {
        __atomic_add_fetch(&MO.ns_part[0], kern_now_ns() - t0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&MO.n_part[0], 1, __ATOMIC_RELAXED);
    }
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    typedef struct { const GfxShader *of, *vs; uint32_t banks; } VMemo;
    VMemo *vmemo = cx->mo_vm;
    if (!vmemo && !(vmemo = cx->mo_vm = calloc(1024, sizeof *vmemo))) rt_fatal("vk: out of memory (motion memo)");
    uint32_t vi = (uint32_t)((uintptr_t)cx->B.st.vs * 0x9E3779B97F4A7C15ull >> 54);
    if (vmemo[vi].of != cx->B.st.vs) {
        int has, pending = 0;
        cx->sf_mvprev = 1;
        const GfxShader *v = shader_for(ex, NVN_STAGE_VS, &has, &pending, 0);
        cx->sf_mvprev = 0;
        if (!v || pending) {
            __atomic_add_fetch(&MO.building, 1, __ATOMIC_RELAXED);
            return skin_pick_left(skin_out, SKIN_NOT_READY);
        }
        GfxShaderIo io[32];
        uint32_t extra = gfx_shader_outputs(v, io) & ~gfx_shader_outputs(cx->B.st.vs, io);
        uint32_t banks = extra ? gfx_shader_mvprev_banks(v) & 0x1fff8u : 0;
        vmemo[vi].of = cx->B.st.vs, vmemo[vi].vs = v, vmemo[vi].banks = banks;
        if (!RT_V111) vk_mvprev_want((banks & ~(1u << 4)) >> 3);
    }
    const GfxShader *vs = vmemo[vi].vs;
    uint32_t banks = vmemo[vi].banks;
    if (!banks || !(banks & ~(1u << 4))) {
        __atomic_add_fetch(&MO.no_out, 1, __ATOMIC_RELAXED);
        return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    }
    static int noc4 = -1;
    if (noc4 < 0) noc4 = rt_knob("MVOBJ_NOC4", "0");
    if (!(banks >> 4 & 1) && !noc4) {
        __atomic_add_fetch(&MO.no_out, 1, __ATOMIC_RELAXED);
        return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    }
    static int all = -1;
    if (all < 0) all = rt_knob("MVOBJ_ALL", "0");
    if (!all && !(banks & (1u << 5 | 1u << 6 | 1u << 7))) {
        __atomic_add_fetch(&MO.no_out, 1, __ATOMIC_RELAXED);
        return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    }
    static long only = -2;
    if (only == -2) {
        const char *e = cfg_get("NX2SARIA_MVOBJ_ONLY");
        only = e && *e ? strtol(e, NULL, 16) : -1;
    }
    if (only >= 0 && banks & ~(uint32_t)only & ~(1u << 4)) return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    static long exact = -2;
    if (exact == -2) {
        const char *e = cfg_get("NX2SARIA_MVOBJ_EXACT");
        exact = e && *e ? strtol(e, NULL, 16) : -1;
    }
    if (exact >= 0 && banks != (uint32_t)exact) return skin_pick_left(skin_out, SKIN_BANKS_OTHER);
    *vs_out = vs, *banks_out = banks;
    return 1;
}

static void mvobj_draw_(VkExec *ex, const p_draw_idx *d, VkBuffer ib, VkDeviceSize ib_off, uint64_t t0,
                        const GfxShader *vs, uint32_t banks, const MotionOwner *owner, const SkinPick *skin) {
    DrawCtx *const cx = ex->dc;
    const VkExecTarget *z = &ex->depth;
    uint64_t tp = t0 ? kern_now_ns() : 0;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (MQ.frame != frame || MQ.fence != ex->fence || MQ.depth_fmt != z->format) mo_begin(ex, frame);
    mo_mask_note(banks, cx->B.st.vs);
    PoseRange *pose = RT_V111 && !skin ? pose_match(ex, d, owner) : NULL;
    if (RT_V111 && !pose && !skin) return;
    SkinDraw skin_draw;
    SkinEntry skin_entry;
    uint64_t skin_listed = 0;
    VkDescriptorBufferInfo cam = {0}, prevb[17] = {0};
    uint32_t have = 0, changed = 0;
    const ActorPose *previous_pose = NULL;
    ActorPose *pose_view = NULL;
    VkDescriptorImageInfo pose_depth = {0};
    int pose_visibility_guard = 0;
    float history[4] = {0};
    float jit[2];
    if (pose) {
        const uint32_t required = (1u << 3) | (1u << 4) | (1u << 6);
        if ((banks & required) != required || (banks & ~0xf8u)) return;
        const GfxShaderRes *resources;
        if (gfx_shader_resources(vs, GFX_RES_SSBO, &resources) || gfx_shader_resources(vs, GFX_RES_TEX, &resources) ||
            gfx_shader_resources(vs, GFX_RES_IMG, &resources)) return;
        previous_pose = pose_previous(pose, ex, d, frame, banks, &pose_view);
        if (!previous_pose) return;
        pose_visibility_guard = pose_guard_needed(pose, pose_view, previous_pose);
        if (pose_visibility_guard && !vk_dlaa_pose_depth(frame, z->width, z->height, &pose_depth, history)) {
            pose_view->guard_missing_depth = 1;
            pose_visibility_guard = 0;
        }
        pose_view->visibility_guard = pose_visibility_guard;
        VkViewport v = draw_viewport(ex);
        jit[0] = v.x;
        jit[1] = v.y - (float)z->height;
    } else if (skin) {
        if (!skin_try(ex, d, frame, vs, banks, skin, &skin_draw, &skin_entry, &skin_listed, &pose_visibility_guard)) return;
        if (pose_visibility_guard && !vk_dlaa_pose_depth(frame, z->width, z->height, &pose_depth, history))
            pose_visibility_guard = 0;
        VkViewport v = draw_viewport(ex);
        jit[0] = v.x;
        jit[1] = v.y - (float)z->height;
    } else {
        if (!vk_dlaa_prev_camera(&cam, jit)) {
            MO.nodata++, MO.why[0]++;
            return;
        }
        if (!mo_legacy_previous(ex, frame, banks, t0, tp, prevb, &have, &changed)) return;
    }
    if (t0) MO.ns_part[1] += kern_now_ns() - tp, MO.n_part[1]++, tp = kern_now_ns();
    GfxShaderIo io[32];
    uint32_t extra = gfx_shader_outputs(vs, io) & ~gfx_shader_outputs(cx->B.st.vs, io);
    const GfxShaderRes *r;
    if (gfx_shader_resources(vs, GFX_RES_IMG, &r)) {
        MO.storage++;
        return;
    }
    int loc = 31 - __builtin_clz(extra);
    const GfxShader *fs = pose_visibility_guard ? link_motion_fs(loc) : mo_fs(loc);
    if (!fs) {
        MO.no_fs++;
        if (skin) skin_left(SKIN_NOT_READY);
        return;
    }
    VkPipeState ms = cx->B.st;
    ms.vs = vs, ms.fs = fs, ms.feedback = 0;
    memset(ms.color_fmt, 0, sizeof ms.color_fmt);
    ms.color_fmt[0] = NVN_FORMAT_RG16F;
    ms.color.blend_en = 0, ms.color.logic_op = 0;
    ms.chmask.mask = 0x3;
    ms.ms.a2c = 0;
    static int eq = -1;
    if (eq < 0) eq = rt_knob("MVOBJ_EQUAL", "0");
    int df = cx->B.st.ds.df;
    df = (eq || pose || skin) ? NVN_DEPTH_FUNC_EQUAL : df == NVN_DEPTH_FUNC_LESS ? NVN_DEPTH_FUNC_LEQUAL
       : df == NVN_DEPTH_FUNC_GREATER ? NVN_DEPTH_FUNC_GEQUAL : df == NVN_DEPTH_FUNC_LEQUAL || df == NVN_DEPTH_FUNC_GEQUAL ? df
       : NVN_DEPTH_FUNC_EQUAL;
    ms.ds.dt = 1, ms.ds.dw = 0, ms.ds.df = (uint64_t)df, ms.ds.st = 0;
    static struct { VkPipeState st; VkPipeline p; const VkPipeInfo *info; int used; } pmemo[64];
    static int plast = -1;
    uint32_t pi;
    if (plast >= 0 && pmemo[plast].used && !memcmp(&pmemo[plast].st, &ms, sizeof ms)) {
        pi = (uint32_t)plast;
    } else {
        uint64_t ph = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < sizeof ms; i += 8) {
            uint64_t w8;
            memcpy(&w8, (const uint8_t *)&ms + i, sizeof ms - i < 8 ? sizeof ms - i : 8);
            ph = (ph ^ w8) * 0x100000001b3ull;
        }
        pi = (uint32_t)(ph >> 58);
    }
    const VkPipeInfo *info = NULL;
    VkPipeline pipe;
    int pending = 0;
    if (pmemo[pi].used && !memcmp(&pmemo[pi].st, &ms, sizeof ms)) {
        info = pmemo[pi].info;
        pipe = __atomic_load_n(&info->pipeline, __ATOMIC_ACQUIRE);
        if (!pipe) {
            if (skin) skin_left(SKIN_NOT_READY);
            return;
        }
    } else {
        pipe = vk_pipeline_graphics_async(&ms, &info, &pending, 0);
        if (pending || !pipe || !info) {
            MO.building += pending != 0;
            if (skin) skin_left(SKIN_NOT_READY);
            return;
        }
        pmemo[pi].st = ms, pmemo[pi].p = pipe, pmemo[pi].info = info, pmemo[pi].used = 1;
    }
    plast = (int)pi;
    uint64_t t1 = kern_now_ns();
    if (t0) MO.ns_part[2] += t1 - tp, MO.n_part[2]++;
    if (t0) MO.ns_check -= t1 - t0;
    MQ.d = mo_grow(MQ.d, &MQ.cap, MQ.n + 1, sizeof *MQ.d);
    MoDraw *q = &MQ.d[MQ.n];
    float par_in[8] = {(float)z->width, (float)z->height, jit[0], jit[1], history[0], history[1], history[2], history[3]};
    if (!MQ.par.buffer || memcmp(par_in, MQ.par_in, sizeof par_in)) {
        VkBuffer pbuf;
        VkDeviceSize poff;
        memcpy(scratch(ex, sizeof par_in, &pbuf, &poff), par_in, sizeof par_in);
        MQ.par = (VkDescriptorBufferInfo){pbuf, poff, sizeof par_in};
        memcpy(MQ.par_in, par_in, sizeof par_in);
    }
    memset(cx->mo_ovr, 0, sizeof cx->mo_ovr);
    if (previous_pose) {
        const uint8_t *data = previous_pose->bytes;
        for (int i = 0; i < POSE_BANKS; ++i) {
            if (!previous_pose->size[i]) continue;
            VkDescriptorBufferInfo *b = &cx->mo_ovr[NVN_STAGE_VS][18 + i];
            b->range = previous_pose->size[i];
            memcpy(scratch(ex, b->range, &b->buffer, &b->offset), data, b->range);
            data += b->range;
        }
    } else if (skin) {
        for (int i = 0; i < SKIN_BANKS; i++)
            if (skin_draw.block[i] >= 0) cx->mo_ovr[NVN_STAGE_VS][18 + i] = SK.f[SK.cur].b[skin_draw.block[i]].was;
    } else {
        for (int bk = 3; bk <= 16; bk++)
            cx->mo_ovr[NVN_STAGE_VS][bk + 15] = bk == 4 ? cam : (have >> bk & 1) ? prevb[bk] : ubo_info(ex, NVN_STAGE_VS, bk);
    }
    cx->mo_ovr[NVN_STAGE_FS][3] = MQ.par;
    cx->mo_ovr_on = 1;
    int bound = mo_desc(ex, info, q, pose_visibility_guard ? &pose_depth : NULL);
    cx->mo_ovr_on = 0;
    if (!bound) {
        MO.storage++;
        if (skin) skin_left(SKIN_STORAGE);
        return;
    }
    q->pipe = pipe, q->info = info;
    if (!MQ.nst || memcmp(&MQ.st[MQ.nst - 1], &ms, sizeof ms)) {
        MQ.st = mo_grow(MQ.st, &MQ.cst, MQ.nst + 1, sizeof *MQ.st);
        MQ.st[MQ.nst++] = ms;
    }
    q->st = (uint32_t)MQ.nst - 1;
    q->v = draw_viewport(ex), q->sc = vk_exec_scissor(ex);
    q->bias[0] = ex->poly_offset.units, q->bias[1] = ex->poly_offset.clamp, q->bias[2] = slope_bias(ex);
    q->vb = mo_vertex_buffers(ex);
    q->ib = ib, q->ib_off = ib_off;
    q->it = d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    q->count = (uint32_t)d->count, q->instances = d->instances > 0 ? (uint32_t)d->instances : 1;
    q->base_vertex = d->base_vertex, q->base_instance = (uint32_t)d->base_instance;
    q->cam = ex->ubo[NVN_STAGE_VS][pose || skin ? 0 : 1].addr;
    q->pose_index = pose ? (int)(pose - pose_ranges) + 1 : 0;
    q->pose_view = pose ? (int)(pose_view - pose->history[frame & 1].view) : 0;
    q->skin = skin ? 1 + pose_visibility_guard : 0;
    if (skin) skin_commit(&skin_draw, &skin_entry, skin_listed, MQ.n);
    MQ.n++;
    MO.draws++;
    for (int i = 0; i < 16; i++)
        if (!MF[i].n || (MF[i].mask == banks && MF[i].chg == changed)) {
            MF[i].mask = banks, MF[i].chg = changed, MF[i].n++;
            break;
        }
    if (t0) MO.ns_rec += kern_now_ns() - t1, MO.n_rec++;
}

int vk_fg_pose_prepare(VkExec *ex, uint64_t cam, VkImage depth, uint32_t w, uint32_t h) {
    if (!RT_V111 || (!pose_enabled() && !skin_enabled())) return 0;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    pose_select(frame, cam, depth);
    skin_resolve(ex, frame, cam, depth, w, h);
    pose_status(frame);
    return MQ.n != 0;
}

int vk_mvobj_execute(VkExec *ex, uint64_t cam, VkImageView mv, VkImageView depth, VkFormat depth_fmt, uint32_t w,
                     uint32_t h) {
    static uint64_t frames;
    if (++frames % 60 == 0)
        rt_log("vk_exec: per-object motion: %.2f ms a frame checking, %.2f ms listing (%.1f us a draw), %.2f ms recording",
               MO.ns_check / 1e6 / (frames ? frames : 1), MO.ns_rec / 1e6 / (frames ? frames : 1),
               MO.n_rec ? MO.ns_rec / 1e3 / MO.n_rec : 0.0, MO.ns_emit / 1e6 / (frames ? frames : 1));
    if (frames % 600 == 0) {
        char l[1400];
        int o = 0;
        for (int i = 0; i < 32 && MM[i].n && o < 1300; i++)
            o += snprintf(l + o, sizeof l - o, " %#x:%llu (u%d s%d t%d)", MM[i].mask, (unsigned long long)MM[i].n, MM[i].ubo,
                          MM[i].ssbo, MM[i].tex);
        if (o) rt_log("vk_exec: per-object motion position banks (bit n: cn) and draws:%s", l);
        o = 0;
        for (int i = 0; i < 14 && o < 1300; i++)
            if (MO.lk[i][0] + MO.lk[i][1] + MO.lk[i][2] + MO.lk[i][3])
                o += snprintf(l + o, sizeof l - o, " c%d %llu/%llu/%llu/%llu/%llu", i + 3, (unsigned long long)MO.lk[i][0],
                              (unsigned long long)MO.lk[i][1], (unsigned long long)MO.lk[i][2],
                              (unsigned long long)MO.lk[i][3], (unsigned long long)MO.lk[i][4]);
        if (o) rt_log("vk_exec: per-object motion last-frame lookups (none/self/side/nearer/refused):%s", l);
    }
    if (frames % 60 == 0 && MO.ran)
        rt_log("vk_exec: per-object motion recorded a frame: %.0f draws, %.0f pipeline binds, %.0f dynamic states, %.0f "
               "pushes of %.1f bindings, %.0f set binds, %.0f vertex buffer binds; sets written %.0f, reused %.0f",
               (double)MO.e_draws / MO.ran, (double)MO.e_pipe / MO.ran, (double)MO.e_dyn / MO.ran, (double)MO.e_push / MO.ran,
               MO.e_push ? (double)MO.e_pushw / MO.e_push : 0.0, (double)MO.e_sets / MO.ran, (double)MO.e_vb / MO.ran,
               (double)MO.e_setw / MO.ran, (double)MO.e_setreuse / MO.ran);
    if (frames % 60 == 0)
        rt_log("vk_exec: per-object motion checks: filter %.2f ms (%.0f a frame), data %.2f ms (%.0f), variant and "
               "pipeline %.2f ms (%.0f); %.0f objects looked up; still draws %.2f ms", MO.ns_part[0] / 1e6 / frames,
               (double)MO.n_part[0] / frames,
               MO.ns_part[1] / 1e6 / frames, (double)MO.n_part[1] / frames, MO.ns_part[2] / 1e6 / frames,
               (double)MO.n_part[2] / frames, (double)MO.misses / frames, MO.ns_still / 1e6 / frames);
    if (frames % 60 == 0 && MO.ncyc)
        rt_log("vk_exec: per-object motion lookups: %llu cycles finding last frame's, %llu the copies, %llu comparing, "
               "a lookup; %llu bytes a lookup", (unsigned long long)(MO.cyc[0] / MO.ncyc),
               (unsigned long long)(MO.cyc[1] / MO.ncyc), (unsigned long long)(MO.cyc[2] / MO.ncyc),
               (unsigned long long)(MO.big / MO.ncyc));
    if (frames % 60 == 0)
        rt_log("vk_exec: per-object motion: %llu draws read place or bones, %llu of them still, %llu drawn again, %llu without last frame's data, %llu "
               "waiting for a shader or pipeline; %llu frames ran, %llu dropped (no data: camera %llu, bones %llu, place %llu, "
               "bones copy %llu, place copy %llu; uniform alignment %llu; variant without its output %llu, with storage %llu, "
               "no fragment shader %llu; another camera %llu)",
               (unsigned long long)MO.cand, (unsigned long long)MO.still, (unsigned long long)MO.draws, (unsigned long long)MO.nodata,
               (unsigned long long)MO.building,
               (unsigned long long)MO.ran, (unsigned long long)MO.dropped, (unsigned long long)MO.why[0],
               (unsigned long long)MO.why[1], (unsigned long long)MO.why[2], (unsigned long long)MO.why[3],
               (unsigned long long)MO.why[4], (unsigned long long)LIM.ubo_align, (unsigned long long)MO.no_out,
               (unsigned long long)MO.storage, (unsigned long long)MO.no_fs, (unsigned long long)MO.other_cam);
    if (!MQ.n) return 0;
    static int exec = -1;
    if (exec < 0) exec = rt_knob("MVOBJ_EXEC", "1");
    if (!exec) {
        mo_drop();
        return 0;
    }
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (MQ.frame != frame || MQ.fence != ex->fence || MQ.depth_fmt != depth_fmt) {
        mo_drop();
        return 0;
    }
    int fo = 0;
    for (int i = 0; i < 16 && MF[i].n && fo < (int)sizeof fm_last - 40; i++)
        fo += snprintf(fm_last + fo, sizeof fm_last - fo, " %#x/%#x:%u", MF[i].mask, MF[i].chg, MF[i].n);
    if (!fo) fm_last[0] = 0;
    int kept = 0;
    for (int i = 0; i < MQ.n; i++)
        if (MQ.d[i].cam == cam && (!MQ.d[i].pose_index ||
            pose_ranges[MQ.d[i].pose_index - 1].history[frame & 1].view[MQ.d[i].pose_view].selected) &&
            (!MQ.d[i].skin || (MQ.d[i].skin > 0 && SK.resolved))) {
            if (MQ.d[i].pose_index) ++pose_written[pose_ranges[MQ.d[i].pose_index - 1].actor];
            if (MQ.d[i].skin) skin_recorded(MQ.d[i].skin == 2);
            MQ.d[kept++] = MQ.d[i];
        }
        else if (!MQ.d[i].skin) MO.other_cam++;
    MQ.n = kept;
    if (!MQ.n) {
        mo_drop();
        return 0;
    }
    uint64_t t0 = kern_now_ns();
    VkCommandBuffer cb = ex->cb;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = mv,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &da};
    vkCmdBeginRendering(cb, &ri);
    mo_emit(cb);
    vkCmdEndRendering(cb);
    vk_exec_draw_new_cb();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->recorded += MQ.n;
    MQ.n = MQ.nst = MQ.nvb = 0, MQ.ndesc = 0, MQ.frame = 0;
    MO.ran++;
    MO.ns_emit += kern_now_ns() - t0;
    return 1;
}

static PFN_vkCmdDrawMeshTasksEXT draw_mesh_fn(void) {
    static PFN_vkCmdDrawMeshTasksEXT fn;
    static int looked;
    if (!__atomic_load_n(&looked, __ATOMIC_ACQUIRE)) {
        if (vk_ctx()->has_mesh_shader) fn = (PFN_vkCmdDrawMeshTasksEXT)vk_dpa(vk_ctx()->device, "vkCmdDrawMeshTasksEXT");
        __atomic_store_n(&looked, 1, __ATOMIC_RELEASE);
    }
    return fn;
}

static PFN_vkCmdDrawMeshTasksIndirectEXT draw_mesh_ind_fn(void) {
    static PFN_vkCmdDrawMeshTasksIndirectEXT fn;
    static int looked;
    if (!__atomic_load_n(&looked, __ATOMIC_ACQUIRE)) {
        if (vk_ctx()->has_mesh_shader)
            fn = (PFN_vkCmdDrawMeshTasksIndirectEXT)vk_dpa(vk_ctx()->device, "vkCmdDrawMeshTasksIndirectEXT");
        __atomic_store_n(&looked, 1, __ATOMIC_RELEASE);
    }
    return fn;
}

static void mesh_shading_rate(VkExec *ex) {
    static PFN_vkCmdSetFragmentShadingRateKHR fn;
    static int looked, from;
    if (!__atomic_load_n(&looked, __ATOMIC_ACQUIRE)) {
        const char *e = cfg_get("NX2SARIA_FOLIAGE_SHADOW_VRS");
        from = e && *e ? atoi(e) : 2;
        if (vk_ctx()->has_vrs)
            fn = (PFN_vkCmdSetFragmentShadingRateKHR)vkGetDeviceProcAddr(vk_ctx()->device, "vkCmdSetFragmentShadingRateKHR");
        __atomic_store_n(&looked, 1, __ATOMIC_RELEASE);
    }
    if (!fn) return;
    rec_shading_rate(fn, ex->cb, !ex->color[0].image && ex->depth.image && (int)ex->depth.base_layer >= from ? 2 : 1);
    ex->recorded++;
}

typedef struct MfbCs {
    const GfxShader *ms, *ts;
    MeshFbInfo mi, ti;
    volatile int32_t state;
    const GfxShader *cs_mesh, *cs_task;
    MfbHist hist;
    struct MfbCs *next;
} MfbCs;
typedef struct { const GfxShader *ms; uint32_t mask; uint8_t type[32]; const GfxShader *vs; } MfbVs;
#define MFB_TAB 4096
static MfbCs *mfb_cs_tab[MFB_TAB];
static MfbVs mfb_vs_tab[MFB_TAB];
static struct {
    PlatMutex lock;
    PlatCond wake;
    int threads, busy;
    MfbCs *head, *tail;
    uint64_t made, failed, pending_skips;
    uint64_t first_ms, drained;
} MFB = {.lock = PLAT_MUTEX_INIT};

static uint32_t mfb_binding_base(const GfxShader *s) {
    const uint32_t *b;
    uint32_t n = gfx_shader_bindings(s, &b), top = 0;
    for (uint32_t i = 0; i < n; i++)
        if (b[i] > top) top = b[i];
    return top + 1;
}

static const GfxShader *mfb_module(const MfbCs *e, int stage, char *err, size_t cap) {
    const GfxShader *src = stage == 6 ? e->ts : e->ms;
    uint64_t n, on = 0;
    const uint32_t *w = gfx_shader_spirv_pin(src, &n);
    uint32_t base = mfb_binding_base(src);
    uint32_t *out = stage == 6 ? meshfb_task_cs(w, n, &e->ti, base, &on, err, cap)
                               : meshfb_mesh_cs(w, n, &e->mi, base, e->ti.payload_bytes / 4, &on, err, cap);
    gfx_shader_spirv_unpin(src);
    if (!out) return NULL;
    static const int task_slots[1] = {MESHFB_CB_TASK}, mesh_slots[3] = {MESHFB_CB_TASK, MESHFB_CB_MESH, MESHFB_CB_LIST};
    char *res = stage == 6 ? meshfb_res_json(gfx_shader_res_json(src), base, task_slots, 1)
              : e->mi.has_task ? meshfb_res_json(gfx_shader_res_json(src), base, mesh_slots, 3)
                               : meshfb_res_json(gfx_shader_res_json(src), base + 1, mesh_slots + 1, 1);
    const GfxShader *s = res ? gfx_shader_own(out, on, res) : NULL;
    if (!s) snprintf(err, cap, "the converted module or its reflection is malformed");
    free(out), free(res);
    return s;
}

static void mfb_make(MfbCs *e) {
    char err[200] = "";
    const VkPipeInfo *info;
    int ok = (e->cs_mesh = mfb_module(e, 7, err, sizeof err)) != NULL;
    if (ok && e->ts) ok = (e->cs_task = mfb_module(e, 6, err, sizeof err)) != NULL;
    if (ok && !vk_pipeline_compute(e->cs_mesh, &info)) ok = 0, snprintf(err, sizeof err, "the mesh stage's compute pipeline failed");
    if (ok && e->ts && !vk_pipeline_compute(e->cs_task, &info)) ok = 0, snprintf(err, sizeof err, "the task stage's compute pipeline failed");
    plat_mutex_lock(&MFB.lock);
    uint64_t nth = ok ? ++MFB.made : ++MFB.failed;
    plat_mutex_unlock(&MFB.lock);
    if (!ok && (nth <= 8 || !(nth & (nth - 1))))
        rt_log("vk: mesh fallback: a program's modules could not be made (%s): its draws are skipped (%llu such programs)", err,
               (unsigned long long)nth);
    __atomic_store_n(&e->state, ok ? 1 : -1, __ATOMIC_RELEASE);
}

static int mfb_worker_count(void) {
    const char *v = cfg_value("NX2SARIA_MESH_FALLBACK_WORKERS");
    int n = strcmp(v, "auto") ? atoi(v) : (int)plat_cpu_usable() / 4;
    return n < 1 ? 1 : n > 4 ? 4 : n;
}

static void mfb_worker(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    for (;;) {
        plat_mutex_lock(&MFB.lock);
        while (!MFB.head) plat_cond_wait(&MFB.wake, &MFB.lock);
        MfbCs *e = MFB.head;
        if (!(MFB.head = e->next)) MFB.tail = NULL;
        MFB.busy++;
        plat_mutex_unlock(&MFB.lock);
        mfb_make(e);
        plat_mutex_lock(&MFB.lock);
        if (!--MFB.busy && !MFB.head && !MFB.drained++)
            rt_log("vk: mesh fallback: %llu programs made (%llu refused) on %d threads, the queue empty %llu ms after the "
                   "first program was queued (NX2SARIA_MESH_FALLBACK_WORKERS)", (unsigned long long)MFB.made,
                   (unsigned long long)MFB.failed, MFB.threads, (unsigned long long)(plat_now_ms() - MFB.first_ms));
        plat_mutex_unlock(&MFB.lock);
    }
}

static MfbCs *mfb_cs_get(VkExec *ex, const GfxShader *ms, const GfxShader *ts) {
    uint32_t i = (uint32_t)((((uintptr_t)ms >> 4) * 0x9e3779b1u) ^ (((uintptr_t)ts >> 4) * 0x85ebca6bu)) & (MFB_TAB - 1);
    for (uint32_t k = 0; k < MFB_TAB; k++, i = (i + 1) & (MFB_TAB - 1)) {
        MfbCs *e = mfb_cs_tab[i];
        if (e && (e->ms != ms || e->ts != ts)) continue;
        if (e) return e;
        const uint8_t *ctl, *code;
        uint32_t len;
        char err[160] = "";
        e = calloc(1, sizeof *e);
        if (!e) rt_fatal("vk: out of memory");
        e->ms = ms, e->ts = ts, e->state = -1;
        mfb_cs_tab[i] = e;
        int ok = find_stage_scan(ex->prog, NVN_STAGE_MESH, &ctl, &code, &len) && len >= 0x80 &&
                 meshfb_info(ctl, code, 7, &e->mi, err, sizeof err);
        if (ok && ts) ok = find_stage_scan(ex->prog, NVN_STAGE_TASK, &ctl, &code, &len) && meshfb_info(ctl, code, 6, &e->ti, err, sizeof err);
        if (ok && e->mi.has_task != (ts != NULL)) ok = 0, snprintf(err, sizeof err, "the mesh stage %s a task stage and the program has %s",
                                                                   e->mi.has_task ? "expects" : "does not expect", ts ? "one" : "none");
        if (!ok) {
            warn_once(W_MESH, "mesh fallback: a program is not carried (%s): its draws are skipped", *err ? err : "no stage data");
            return e;
        }
        e->state = 0;
        if (!vk_pipeline_async_on()) {
            mfb_make(e);
            return e;
        }
        plat_mutex_lock(&MFB.lock);
        if (!MFB.threads) {
            plat_cond_init(&MFB.wake);
            MFB.first_ms = plat_now_ms();
            for (int w = mfb_worker_count(); w > 0; w--) {
                PlatThread *t = plat_thread_new(mfb_worker, NULL, "mesh fallback");
                if (t) plat_thread_detach(t), MFB.threads++;
            }
            if (!MFB.threads) rt_fatal("vk: mesh fallback: cannot start a worker thread");
        }
        if (MFB.tail) MFB.tail->next = e;
        else MFB.head = e;
        MFB.tail = e;
        plat_cond_signal(&MFB.wake);
        plat_mutex_unlock(&MFB.lock);
        return e;
    }
    rt_fatal("vk: mesh fallback: more than %d task and mesh program pairs", MFB_TAB);
}

static const GfxShader *mfb_vs_get(const MfbCs *e, const GfxShader *fs) {
    GfxShaderIo fin[32];
    uint8_t type[32] = {0};
    uint32_t mask = fs ? gfx_shader_inputs(fs, fin) : 0;
    for (int g = 0; g < 32; g++)
        if (mask >> g & 1) type[g] = fin[g].type;
    uint32_t i = (uint32_t)((((uintptr_t)e->ms >> 4) * 0x9e3779b1u) ^ (mask * 0x85ebca6bu)) & (MFB_TAB - 1);
    for (uint32_t k = 0; k < MFB_TAB; k++, i = (i + 1) & (MFB_TAB - 1)) {
        MfbVs *v = &mfb_vs_tab[i];
        if (v->ms && (v->ms != e->ms || v->mask != mask || memcmp(v->type, type, 32))) continue;
        if (v->ms) return v->vs;
        static const char res[] = "{\"key\":\"meshfb\",\"stage\":\"vs\",\"notes\":[],\"cbufs\":[],\"sbufs\":[{\"binding\": 1, "
                                  "\"slot\": -1, \"cb_slot\": 66, \"cb_offset\": 0, \"flags\": 0}],\"textures\":[],\"images\":[]}";
        _Static_assert(MESHFB_CB_MESH == 66, "the vertex stage's reflection names the mesh buffer");
        uint64_t n = 0;
        uint32_t *w = meshfb_vs(&e->mi, mask, type, &n);
        v->ms = e->ms, v->mask = mask, memcpy(v->type, type, 32);
        v->vs = w ? gfx_shader_own(w, n, res) : NULL;
        free(w);
        if (!v->vs) warn_once(W_MESH, "mesh fallback: a vertex stage could not be made: its draws are skipped");
        return v->vs;
    }
    rt_fatal("vk: mesh fallback: more than %d vertex stages", MFB_TAB);
}

static int mfb_dispatch(VkExec *ex, const GfxShader *cs, int stage, VkBuffer rec, VkDeviceSize at, int mark) {
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info || (info->pool && !pool_on())) return 0;
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    ex->dc->mfb_stage = stage;
    bind_resources(ex, info);
    ex->dc->mfb_stage = 0;
    VkPassMark *m = mark ? dispatch_mark(ex, cs, 0, 0, 0) : NULL;
    vkCmdDispatchIndirect(ex->cb, rec, at);
    vk_pass_mark_end(ex->cb, m);
    ex->recorded++;
    return 1;
}

static void mfb_step(VkExec *ex, int batched) {
    if (batched) vk_meshfb_full_barrier(ex->cb), ex->recorded++;
    else vk_exec_barrier(ex);
}

static int mfb_stages(VkExec *ex, MfbCs *c, const VkMemSpan *ind, uint32_t count, uint32_t verts, uint32_t slot_words,
                      int batched, VkBuffer *rec, VkDeviceSize *at_out) {
    DrawCtx *const cx = ex->dc;
    const int has_ts = c->ts != NULL;
    int cmds;
    VkDeviceSize at = vk_meshfb_record(ex->cb, ind ? ind->buffer : VK_NULL_HANDLE, ind ? ind->offset : 0, count, has_ts,
                                       verts, slot_words, &c->hist, rec, &cmds);
    *at_out = at;
    ex->recorded += (uint32_t)cmds;
    mfb_step(ex, batched);
    if (ind) {
        vk_meshfb_clamp(ex->cb, at, has_ts ? c->ti.payload_bytes / 4 : 0);
        cx->B.pipe[1] = VK_NULL_HANDLE;
        ex->recorded += 4;
        mfb_step(ex, batched);
    }
    if (has_ts) {
        if (!mfb_dispatch(ex, c->cs_task, NVN_STAGE_TASK, *rec, at, !batched)) return 0;
        mfb_step(ex, batched);
        vk_meshfb_scan(ex->cb, at, c->ti.payload_bytes / 4, c->ti.threads, slot_words, verts, ind != NULL);
        cx->B.pipe[1] = VK_NULL_HANDLE;
        ex->recorded += 4;
        mfb_step(ex, batched);
    }
    if (!mfb_dispatch(ex, c->cs_mesh, NVN_STAGE_MESH, *rec, at + 16, !batched)) return 0;
    if (!batched) mfb_step(ex, batched);
    vk_meshfb_note(ex->cb, at, ex->fence);
    ex->recorded++;
    if (!batched) vk_exec_barrier(ex);
    return 1;
}

static struct {
    MfbCount n;
    uint64_t last_ns, lines;
} MFBB;
static void mfb_batch_line(void);
static int mfb_batch_on(void);
void vk_exec_mfb_batch_exit_log(void) {
    if (mfb_batch_on() && (MFBB.lines || MFBB.n.why[MFB_BATCHED])) mfb_batch_line();
}
void vk_exec_mfb_batch_counts(uint64_t *why, uint64_t *cuts) {
    memcpy(why, MFBB.n.why, sizeof MFBB.n.why);
    cuts[0] = MFBB.n.cuts_pass, cuts[1] = MFBB.n.cuts_room, cuts[2] = MFBB.n.cuts_lost;
}
static int mfb_dflt = -1, mfb_path = -1;
static const char *mfb_path_name;
static int mfb_batch_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *raw = cfg_get("NX2SARIA_MESH_FALLBACK_BATCH");
        int set = raw && *raw;
        const char *v = set ? raw : "1";
        int dflt = mfb_dflt >= 0 ? mfb_dflt : vk_ctx() && vk_ctx()->mesh_fallback_default;
        int path = mfb_path >= 0 ? mfb_path : !vk_ctx() || vk_ctx()->mesh_fallback;
        int autov = !strcasecmp(v, "auto");
        on = autov ? dflt
                   : !(!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "false") || !strcasecmp(v, "no"));
        char why[96];
        if (!set) snprintf(why, sizeof why, "the default");
        else if (autov) snprintf(why, sizeof why, "NX2SARIA_MESH_FALLBACK_BATCH=auto: the fallback is %sthis device's default",
                                 dflt ? "" : "not ");
        else snprintf(why, sizeof why, "NX2SARIA_MESH_FALLBACK_BATCH=%s", v);
        if (!path)
            rt_log("vk: mesh fallback batch: %s (%s), inactive on this device: its mesh programs are not drawn through the "
                   "fallback (%s)", on ? "on" : "off", why, mfb_path_name ? mfb_path_name : "native");
        else
            rt_log("vk: mesh fallback batch: %s (%s): the stages of a pass's draws %s", on ? "on" : "off", why,
                   on ? "run before the pass, in a command buffer of their own" : "break the pass, draw by draw");
    }
    return on;
}
void vk_exec_mfb_batch_start(int fallback, int fallback_default, const char *path_name) {
    mfb_path = fallback != 0, mfb_dflt = fallback_default != 0, mfb_path_name = path_name;
    (void)mfb_batch_on();
}
static void mfb_batch_log(void) {
    if (!mfb_batch_on()) return;
    uint64_t now = kern_now_ns();
    if (!MFBB.last_ns) MFBB.last_ns = now;
    if (now - MFBB.last_ns < 30000000000ull) return;
    MFBB.last_ns = now;
    mfb_batch_line();
}
static void mfb_batch_line(void) {
    MFBB.lines++;
    const uint64_t *w = MFBB.n.why;
    uint64_t all = 0, cap, peak = vk_meshfb_batch_peak(&cap);
    for (int i = 0; i < MFB_REASONS; i++) all += w[i];
    all += MFBB.n.failed;
    rt_log("vk: mesh fallback batch: %llu draws, %llu batched (%.0f%%); cuts: %llu at a pass, %llu for room, %llu for "
           "nothing; one by one: %llu off, %llu loading, %llu stage writes, %llu stage textures, %llu excluded, "
           "%llu no settled history, "
           "%llu no room, %llu no pre-pass; %llu batched whose stages failed; %llu batched draws short of their part (%llu programs taken back since); the parts took at most %.1f of the mesh "
           "buffer's %.0f MB (session totals)",
           (unsigned long long)all, (unsigned long long)w[MFB_BATCHED], all ? 100.0 * (double)w[MFB_BATCHED] / (double)all : 0.0,
           (unsigned long long)MFBB.n.cuts_pass, (unsigned long long)MFBB.n.cuts_room, (unsigned long long)MFBB.n.cuts_lost,
           (unsigned long long)w[MFB_ONE_OFF], (unsigned long long)w[MFB_ONE_LOADING], (unsigned long long)w[MFB_ONE_WRITES], (unsigned long long)w[MFB_ONE_TEXTURES],
           (unsigned long long)w[MFB_ONE_EXCLUDED], (unsigned long long)w[MFB_ONE_HISTORY],
           (unsigned long long)w[MFB_ONE_ROOM], (unsigned long long)w[MFB_ONE_PRE], (unsigned long long)MFBB.n.failed,
           (unsigned long long)vk_meshfb_batch_short(), (unsigned long long)vk_meshfb_batch_readmits(),
           (double)peak / 1048576.0, (double)cap / 1048576.0);
}

typedef struct { VkExec *ex; VkCommandBuffer pre; uint64_t tb, lb, mb; } MfbDrawSteps;
static int mfb_step_cut(void *u) { return vk_exec_split_now(((MfbDrawSteps *)u)->ex); }
static int mfb_step_pre(void *u) { MfbDrawSteps *d = u; return (d->pre = vk_sync_pre()) != VK_NULL_HANDLE; }
static int mfb_step_take(void *u) { MfbDrawSteps *d = u; return vk_meshfb_part_take(d->tb, d->lb, d->mb); }

static void mfb_draw(VkExec *ex, uint32_t count, const VkMemSpan *ind, uint64_t list) {
    DrawCtx *const cx = ex->dc;
    cx->mfb_list = 0;
    if (ex->render_disabled) {
        vk_cond_draws_skip += ex->has_render_cond;
        draw_skipped(ex, 1, SK_DISABLED);
        return;
    }
    if (!ex->prog) { draw_skipped(ex, 1, SK_NO_PROGRAM); return; }
    cb_res(ex);
    dummies_init();
    int has_ms, has_ts, has_fs, w_ms = 0, w_ts = 0, w_fs = 0;
    const GfxShader *ms = shader_for(ex, NVN_STAGE_MESH, &has_ms, &w_ms, 0);
    const GfxShader *ts = shader_for(ex, NVN_STAGE_TASK, &has_ts, &w_ts, 0);
    const GfxShader *fs = shader_for(ex, NVN_STAGE_FS, &has_fs, &w_fs, 0);
    if (!has_ms) { draw_skipped(ex, 1, SK_NO_VERTEX_STAGE); return; }
    if (w_ms || w_ts || w_fs) {
        vk_exec_ahead(ex);
        perf_count(PERF_CNT_SHADER_SKIP);
        draw_skipped(ex, 1, SK_SHADER_PENDING);
        return;
    }
    if (!ms || (has_ts && !ts) || (has_fs && !fs)) { draw_skipped(ex, 1, SK_SHADER_FAILED); return; }
    if (list && mfb_hold_on() && !stage_writes(ms) && !stage_writes(ts) && !stage_writes(fs)) cx->mfb_list = list;
    if (cx->mfb_list && !mfb_hold_colour(ex) && mfb_held(cx->mfb_list)) { mfb_hold_skip(ex); return; }
    MfbCs *c = mfb_cs_get(ex, ms, ts);
    int state = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
    if (!state && !vk_exec_targets_recent(ex, pipe_skip_mode() >= 1)) {
        uint64_t *pool = frame_wait(), t0 = kern_now_ns();
        while (!state && pool_ms(pool, t0)) plat_sleep_ms(1), state = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
        pool_charge(pool, t0);
    }
    if (!state) {
        if (cx->mfb_list && mfb_hold_colour(ex)) mfb_hold_set(cx->mfb_list, 1);
        MFB.pending_skips++;
        draw_skipped(ex, 1, SK_PIPE_PENDING);
        return;
    }
    const GfxShader *vs = state > 0 ? mfb_vs_get(c, fs) : NULL;
    if (!vs || !vk_meshfb_init()) { draw_skipped(ex, 1, SK_PIPE_FAILED); return; }
    uint32_t verts = c->mi.maxp * c->mi.ipp, slot_words = meshfb_slot_words(&c->mi);
    uint32_t most = vk_ctx()->props.limits.maxComputeWorkGroupCount[0];
    if (count > most) count = most;
    vk_meshfb_poll(vk_sync_fence_done());
    uint32_t b_slots, b_tasks;
    uint64_t cap_task, cap_mesh;
    vk_meshfb_caps(&cap_task, &cap_mesh);
    int why = mfb_batch_need(mfb_batch_on(), stage_writes(ms) || stage_writes(ts) || stage_writes(fs), ind == NULL, count,
                             ts != NULL, &c->hist, mfb_floor(cap_mesh, (uint64_t)slot_words * 4, MFB_MIN_SLOTS),
                             mfb_floor(cap_task, c->ti.payload_bytes, MFB_MIN_TASKS), &b_slots, &b_tasks);
    if (why == MFB_BATCHED && vk_exec_pool_loading()) why = MFB_ONE_LOADING;
    MfbDrawSteps ds = {.ex = ex};
    mfb_part_bytes(b_slots, b_tasks, ts != NULL, c->ti.payload_bytes / 4, slot_words, &ds.tb, &ds.lb, &ds.mb);
    const MfbSteps steps = {mfb_step_cut, mfb_step_pre, mfb_step_take, &ds};
    int cut;
    uint64_t cap_list = vk_meshfb_list_bytes();
    if (why == MFB_BATCHED && (ds.tb > cap_task || ds.lb > cap_list || ds.mb > cap_mesh)) why = MFB_ONE_ROOM;
    why = mfb_batch_steps(why, stage_reads_images(ms) || stage_reads_images(ts),
                          mfb_need_cut(ex->in_pass, ex->pre_pass, ex->pass_serial) || vk_meshfb_closed(), &steps, &cut);
    VkCommandBuffer pre = ds.pre;
    if (why != MFB_BATCHED) mfb_count(&MFBB.n, why, cut, 0);
    mfb_batch_log();
    VkBuffer rec;
    VkDeviceSize at;
    if (why != MFB_BATCHED) {
        vk_meshfb_one_by_one();
        vk_exec_barrier(ex);
        if (!mfb_stages(ex, c, ind, count, verts, slot_words, 0, &rec, &at)) { draw_skipped(ex, 1, SK_PIPE_FAILED); return; }
    } else {
        VkCommandBuffer part = ex->cb;
        int recorded = ex->recorded;
        ex->cb = pre;
        cx->B.pipe[1] = VK_NULL_HANDLE;
        int ok = mfb_stages(ex, c, ind, count, verts, slot_words, 1, &rec, &at);
        ex->cb = part, ex->recorded = recorded;
        cx->B.pipe[1] = VK_NULL_HANDLE;
        mfb_count(&MFBB.n, why, cut, ok);
        if (!ok) { vk_meshfb_part_done(); draw_skipped(ex, 1, SK_PIPE_FAILED); return; }
        cx->B.res_valid = 0;
    }
    cx->c0_binst = 0;
    cx->mfb_cur_vs = vs;
    int prim = c->mi.ipp == 1 ? NVN_DRAW_PRIMITIVE_POINTS : c->mi.ipp == 2 ? NVN_DRAW_PRIMITIVE_LINES : NVN_DRAW_PRIMITIVE_TRIANGLES;
    int drawn = prepare_draw_ex(ex, prim, 0x7fffffff, 0, 2, NULL);
    if (why == MFB_BATCHED) {
        vk_meshfb_part_done();
        cx->B.res_valid = 0;
        if (cut && ex->in_pass) ex->pre_pass = ex->pass_serial;
    }
    if (!drawn) return;
    vk_exec_frame.draws++;
    if (ex->mark && ind) ex->mark->mesh_ind++;
    else if (ex->mark) ex->mark->mesh++;
    vkCmdDrawIndirect(ex->cb, rec, at + 32, 1, sizeof(VkDrawIndirectCommand));
    ex->recorded++;
}

static int mesh_emu_draw(VkExec *ex, uint32_t count, const VkMemSpan *ind, const VkMemSpan *args);
static int mesh_emu_draw_prim(VkExec *ex) {
    static struct { const GfxShader *s; int prim; } memo[256];
    int present = 0, pending = 0;
    const GfxShader *ms = ex->prog ? shader_for(ex, NVN_STAGE_MESH, &present, &pending, 0) : NULL;
    if (!ms) return NVN_DRAW_PRIMITIVE_TRIANGLES;
    unsigned h = (unsigned)((uintptr_t)ms >> 4) & 255;
    if (memo[h].s != ms) {
        uint64_t n = 0;
        const uint32_t *w = gfx_shader_spirv_pin(ms, &n);
        memo[h].s = ms, memo[h].prim = w && mesh_emu_prim(w, (size_t)n) == 1 ? NVN_DRAW_PRIMITIVE_POINTS : NVN_DRAW_PRIMITIVE_TRIANGLES;
        gfx_shader_spirv_unpin(ms);
    }
    return memo[h].prim;
}
static int mesh_emu_on(void) {
    static int on = -1;
    if (on < 0) {
        on = vk_ctx()->mesh_emulation;
        if (on) rt_log("vk: mesh shaders emulated with compute (NX2SARIA_MESH_EMULATION)");
    }
    return on;
}

static void op_draw_mesh(VkExec *ex, const p_draw_mesh *d) {
    PFN_vkCmdDrawMeshTasksEXT fn = draw_mesh_fn();
    if (!fn && mesh_emu_on()) {
        if (d->first) warn_once(W_MESH, "an emulated mesh draw from workgroup %u: drawn from workgroup 0", d->first);
        if (d->count && rt_knob("MESH", "1")) mesh_emu_draw(ex, d->count, NULL, NULL);
        return;
    }
    static int on = -1;
    if (on < 0) on = rt_knob("MESH", "1");
    if (!d->count || !on) return;
    if (vk_ctx()->mesh_fallback) {
        if (d->first) warn_once(W_MESH, "a mesh draw from workgroup %u: drawn from workgroup 0", d->first);
        mfb_draw(ex, d->count, NULL, 0);
        return;
    }
    if (!fn) return warn_once(W_MESH, "mesh draws need VK_EXT_mesh_shader (mesh_emulation = 1 emulates them): skipped");
    ex->dc->c0_binst = 0;
    if (!prepare_draw_ex(ex, NVN_DRAW_PRIMITIVE_TRIANGLES, 0x7fffffff, 0, 1, NULL)) return;
    if (d->first) warn_once(W_MESH, "a mesh draw from workgroup %u: drawn from workgroup 0", d->first);
    vk_exec_frame.draws++;
    if (ex->mark) ex->mark->mesh++;
    static uint32_t n_logged;
    if (n_logged < 8 && knob_log_mesh()) {
        n_logged++;
        rt_log("vk: mesh draw: %u workgroups, program %llu", d->count, (unsigned long long)ex->prog->serial);
    }
    mesh_shading_rate(ex);
    int q = vk_dump_armed ? vk_dump_mesh_begin(ex) : -1;
    rec_draw_mesh(fn, ex->cb, d->count, 1, 1);
    ex->recorded++;
    vk_dump_mesh_end(ex, q);
}

enum { MESH_RING = 4096 };
static VkBuffer mesh_ring;
static VmaAllocation mesh_ring_alloc;
static uint32_t mesh_ring_next;

int vk_gpu_passes_capturing(void);
enum { MESH_COUNTS = 4096 };
static VkBuffer mesh_count_buf;
static VmaAllocation mesh_count_alloc;
static uint32_t *mesh_count_map, mesh_count_n, mesh_count_was;

static void mesh_ring_release(VkCtx *c) {
    if (mesh_ring) vmaDestroyBuffer(c->vma, mesh_ring, mesh_ring_alloc);
    if (mesh_count_buf) vmaDestroyBuffer(c->vma, mesh_count_buf, mesh_count_alloc);
    mesh_ring = mesh_count_buf = VK_NULL_HANDLE, mesh_count_map = NULL;
}
uint32_t vk_exec_mesh_counts(const uint32_t **out) {
    *out = mesh_count_map;
    return mesh_count_map ? mesh_count_n : 0;
}
static void mesh_count_note(VkExec *ex, VkCommandBuffer cb, const VkMemSpan *src) {
    uint32_t cap = (uint32_t)vk_gpu_passes_capturing();
    if (!cap) { mesh_count_was = 0; return; }
    if (!mesh_count_was) mesh_count_was = 1, mesh_count_n = 0;
    if (!mesh_count_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4ull * MESH_COUNTS,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &mesh_count_buf, &mesh_count_alloc, &info) != VK_SUCCESS) return;
        mesh_count_map = info.pMappedData;
    }
    if (mesh_count_n >= MESH_COUNTS) return;
    VkBufferCopy cp = {src->offset, 4ull * mesh_count_n++, 4};
    vkCmdCopyBuffer(cb, src->buffer, mesh_count_buf, 1, &cp);
    if (cb == ex->cb) ex->recorded++;
}

enum { MESH_AHEAD = 64 };
_Static_assert(MESH_AHEAD == sizeof dc_main.ma / sizeof dc_main.ma[0], "DrawCtx.ma holds MESH_AHEAD entries");
static uint64_t mesh_ahead_taken, mesh_ahead_own;

static int mesh_ring_make(void) {
    if (mesh_ring) return 1;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 12ull * MESH_RING,
                             .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer b;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &b, &mesh_ring_alloc, NULL) != VK_SUCCESS) return 0;
    __atomic_store_n(&mesh_ring, b, __ATOMIC_RELEASE);
    return 1;
}

static int mesh_ahead_on(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("MESH_AHEAD", "1");
    return RT_KNOB_ON(mode);
}

static int mesh_ring_entry(VkExec *ex, VkCommandBuffer cb, const p_draw_mesh_ind *d, const VkMemSpan *src, VkDeviceSize *at) {
    DrawCtx *const cx = ex->dc;
    if ((rt_trace & RT_TRACE_PERF) && (mesh_ahead_taken + mesh_ahead_own) % 20000 == 19999) {
        char stops[160];
        vk_exec_mesh_run_stops(stops, sizeof stops);
        rt_log("perf: indirect mesh draws: %llu so far, %llu of them with their record filled ahead by an earlier draw of "
               "their pass; the runs ended: %s", (unsigned long long)(mesh_ahead_taken + mesh_ahead_own + 1),
               (unsigned long long)mesh_ahead_taken, stops);
    }
    if (d && cb == ex->cb && cx->ma_i < cx->ma_n) {
        if (cx->ma[cx->ma_i].d == d && cx->ma[cx->ma_i].ind == d->ind && cx->ma_brk == vk_exec_brk_now()) {
            *at = cx->ma[cx->ma_i++].at;
            __atomic_add_fetch(&mesh_ahead_taken, 1, __ATOMIC_RELAXED);
            return 1;
        }
        cx->ma_n = cx->ma_i = 0;
    }
    __atomic_add_fetch(&mesh_ahead_own, 1, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&mesh_ring, __ATOMIC_ACQUIRE) && (vk_exec_in_chunk() || !mesh_ring_make())) return 0;
    *at = 12ull * (__atomic_fetch_add(&mesh_ring_next, 1, __ATOMIC_RELAXED) % MESH_RING);
    int own = cb == ex->cb;
    if (own) {
        vk_exec_barrier(ex);
        cb = ex->cb;
        vk_gpu_gap_note(VK_GAP_MESH_RING, 12);
    }
    vkCmdFillBuffer(cb, mesh_ring, *at + 4, 8, 1);
    VkBufferCopy cp = {src->offset, *at, 4};
    vkCmdCopyBuffer(cb, src->buffer, mesh_ring, 1, &cp);
    mesh_count_note(ex, cb, src);
    cx->ma_n = cx->ma_i = 0;
    if (d && own && mesh_ahead_on()) {
        const void *run[MESH_AHEAD];
        int n = vk_exec_mesh_run(d, run, MESH_AHEAD);
        for (int i = 0; i < n; i++) {
            const p_draw_mesh_ind *f = run[i];
            VkMemSpan ip;
            if (!vk_mem_resolve(f->ind, 12, &ip) || ip.offset % 4) break;
            VK_MEM_USE(ip);
            VkDeviceSize fa = 12ull * (__atomic_fetch_add(&mesh_ring_next, 1, __ATOMIC_RELAXED) % MESH_RING);
            vk_gpu_gap_note(VK_GAP_MESH_RING, 12);
            vkCmdFillBuffer(ex->cb, mesh_ring, fa + 4, 8, 1);
            VkBufferCopy fc = {ip.offset, fa, 4};
            vkCmdCopyBuffer(ex->cb, ip.buffer, mesh_ring, 1, &fc);
            mesh_count_note(ex, ex->cb, &ip);
            ex->recorded += 2;
            cx->ma[cx->ma_n].d = f, cx->ma[cx->ma_n].ind = f->ind, cx->ma[cx->ma_n].at = fa;
            cx->ma_n++;
        }
        cx->ma_brk = vk_exec_brk_now();
    }
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                           .dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    if (own) ex->recorded += 3;
    return 1;
}

static int foliage_skip(VkExec *ex) {
    static int mode = -1;
    if (mode < 0) {
        const char *v = cfg_get("NX2SARIA_FOLIAGE");
        mode = v && !strcmp(v, "off") ? 2 : v && !strcmp(v, "reduced") ? 1 : 0;
        rt_log("vk: foliage %s (NX2SARIA_FOLIAGE)", mode == 2 ? "off" : mode ? "reduced: no small grass or moss" : "full");
    }
    if (!mode) return 0;
    if (mode == 2) return 1;
    const uint8_t *ctl, *code;
    uint32_t len;
    return ex->prog && find_stage(ex->dc, ex->prog, NVN_STAGE_TASK, &ctl, &code, &len);
}

static void op_draw_mesh_ind(VkExec *ex, const p_draw_mesh_ind *d) {
    PFN_vkCmdDrawMeshTasksIndirectEXT fn = draw_mesh_ind_fn();
    int emu = !fn && mesh_emu_on(), fallback = !emu && vk_ctx()->mesh_fallback;
    if (!fn && !emu && !fallback)
        return warn_once(W_MESH, "mesh draws need VK_EXT_mesh_shader (mesh_emulation = 1 emulates them): skipped");
    static int on = -1;
    if (on < 0) on = rt_knob("MESH", "1");
    if (!on || foliage_skip(ex)) return;
    VkMemSpan ip;
    if (!vk_mem_resolve(d->ind, 12, &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect mesh draw arguments at %#llx are not backed or not aligned: skipped",
                         (unsigned long long)d->ind);
    ex->dc->c0_binst = 0;
    VK_MEM_USE(ip);
    if (emu) {
        mesh_emu_draw(ex, 0, NULL, &ip);
        vk_mem_count(ip.cls, VK_MUSE_IND, 4);
        VK_MEM_BOUND(ip);
        return;
    }
    if (fallback) {
        mfb_draw(ex, 0, &ip, d->ind);
        vk_mem_count(ip.cls, VK_MUSE_IND, 4);
        VK_MEM_BOUND(ip);
        return;
    }
    VkDeviceSize at;
    if (!mesh_ring_entry(ex, ex->cb, d, &ip, &at)) return warn_once(W_MESH, "the indirect mesh ring could not be created: skipped");
    if (!prepare_draw_ex(ex, NVN_DRAW_PRIMITIVE_TRIANGLES, 0x7fffffff, 0, 1, NULL)) return;
    vk_exec_frame.draws++;
    if (ex->mark) ex->mark->mesh_ind++;
    static uint32_t n_logged;
    if (n_logged < 8 && knob_log_mesh()) {
        n_logged++;
        rt_log("vk: indirect mesh draw: arguments at %#llx, program %llu", (unsigned long long)d->ind,
               (unsigned long long)ex->prog->serial);
    }
    mesh_shading_rate(ex);
    int q = vk_dump_armed ? vk_dump_mesh_begin(ex) : -1;
    rec_draw_mesh_ind(fn, ex->cb, mesh_ring, at, 1, 12);
    ex->recorded++;
    vk_dump_mesh_end(ex, q);
    vk_mem_count(ip.cls, VK_MUSE_IND, 4);
    VK_MEM_BOUND(ip);
}

#define ME_RB 64
static struct {
    VkBuffer buf, rb;
    VmaAllocation alloc, rb_alloc;
    uint32_t *rb_map;
    uint64_t addr, resets;
    uint32_t iw, icap, vw, vcap, trecs, epoch, rec;
    int failed;
    uint64_t draws, skipped;
    uint32_t max[4];
    VkPipelineLayout alayout;
    VkPipeline apipe;
} ME;

static int me_alloc_pipe(void) {
    VkDevice dev = vk_ctx()->device;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pcr};
    if (vkCreatePipelineLayout(dev, &li, NULL, &ME.alayout) != VK_SUCCESS) return ME.alayout = VK_NULL_HANDLE, 0;
    size_t n = 0;
    const uint32_t *w = mesh_emu_alloc_spirv(&n);
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n * 4, .pCode = w};
    VkShaderModule m;
    if (vkCreateShaderModule(dev, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = ME.alayout};
    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &ME.apipe);
    vkDestroyShaderModule(dev, m, NULL);
    if (r != VK_SUCCESS) return ME.apipe = VK_NULL_HANDLE, 0;
    return 1;
}

static int me_arena(void) {
    if (ME.buf) return 1;
    if (ME.failed) return 0;
    const char *v = cfg_get("NX2SARIA_MESH_EMU_MB"), *t = cfg_get("NX2SARIA_MESH_EMU_TASKS");
    int mb = v && atoi(v) >= 32 ? atoi(v) : 256;
    if (mb > 2048) mb = 2048;
    ME.trecs = t && atoi(t) >= 256 ? (uint32_t)atoi(t) : 16384;
    if (ME.trecs > 262144) ME.trecs = 262144;
    uint64_t tbytes = (uint64_t)MESH_EMU_TOFF + (uint64_t)ME.trecs * MESH_EMU_TWORDS * 4;
    if (tbytes + (32ull << 20) > ((uint64_t)mb << 20)) mb = (int)((tbytes >> 20) + 64);
    VkDeviceSize size = (VkDeviceSize)mb << 20;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!me_alloc_pipe()) {
        ME.failed = 1;
        rt_log("vk: mesh emulation: the allocation shader's pipeline could not be created: mesh draws skipped");
        return 0;
    }
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &ME.buf, &ME.alloc, NULL) != VK_SUCCESS) {
        ME.buf = VK_NULL_HANDLE, ME.failed = 1;
        rt_log("vk: mesh emulation: the %d MiB arena could not be created: mesh draws skipped", mb);
        return 0;
    }
    VkBufferDeviceAddressInfo dai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = ME.buf};
    ME.addr = vkGetBufferDeviceAddress(vk_ctx()->device, &dai);
    uint64_t ioff = (tbytes + 4095) & ~4095ull, ibytes = ((size - ioff) / 4) & ~4095ull;
    uint64_t voff = ioff + ibytes;
    ME.iw = (uint32_t)(ioff / 4), ME.icap = (uint32_t)(ibytes / 4), ME.vw = (uint32_t)(voff / 16), ME.vcap = (uint32_t)((size - voff) / 16);
    rt_log("vk: mesh emulation arena: %d MiB, %u task records, %u indices, %u vertex slots (16 bytes)", mb, ME.trecs,
           ME.icap, ME.vcap);
    VkBufferCreateInfo rbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ME_RB * 16,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo rai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo rinfo;
    if (vmaCreateBuffer(vk_ctx()->vma, &rbi, &rai, &ME.rb, &ME.rb_alloc, &rinfo) == VK_SUCCESS) ME.rb_map = rinfo.pMappedData;
    else ME.rb = VK_NULL_HANDLE;
    return 1;
}

static void me_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss,
                           .srcAccessMask = sa | VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = ds,
                           .dstAccessMask = da | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct {
    VkPipeline tp, mp;
    VkPipelineLayout layout;
    VkDescriptorUpdateTemplate tpl_c;
    VkDescriptorSet set0, pset;
    int push;
    uint32_t count, rec, pc[8];
    VkBuffer ib;
    VkDeviceSize io;
    size_t doff;
    uint32_t dbytes;
} MeQ;
#define MEQ_MAX 256
static struct {
    MeQ q[MEQ_MAX];
    int n, tasks;
    uint8_t *d;
    size_t dn, dcap;
} MEQ;

static void me_bind(VkCommandBuffer cb, const MeQ *q) {
    if (q->push) rec_push_tpl(vk_pipeline_push_tpl_fn(), cb, q->tpl_c, q->layout, 0, MEQ.d + q->doff, q->dbytes);
    else vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->layout, 0, 1, &q->set0, 0, NULL);
    if (q->pset) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->layout, 2, 1, &q->pset, 0, NULL);
    vkCmdPushConstants(cb, q->layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, MESH_EMU_PC_BYTES, q->pc);
}

static void me_first(VkCommandBuffer cb, const MeQ *q) {
    if (q->ib) vkCmdDispatchIndirect(cb, q->ib, q->io);
    else vkCmdDispatch(cb, q->count, 1, 1);
}

static void me_alloc(VkCommandBuffer cb, uint32_t mode) {
    uint32_t pc[8] = {(uint32_t)ME.addr, (uint32_t)(ME.addr >> 32), MEQ.q[0].rec, (uint32_t)MEQ.n, mode, ME.icap, ME.trecs, 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ME.apipe);
    vkCmdPushConstants(cb, ME.alayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, 1, 1);
}

static int me_flush(VkCommandBuffer cb) {
    if (!MEQ.n || !cb) return 0;
    const VkAccessFlags2 rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    const VkPipelineStageFlags2 cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    int k = 0;
    me_barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, cs | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               rw | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    me_alloc(cb, 0), k += 4;
    me_barrier(cb, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, cs | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
               rw | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    if (MEQ.tasks) {
        for (int i = 0; i < MEQ.n; i++) {
            const MeQ *q = &MEQ.q[i];
            if (!q->tp) continue;
            me_bind(cb, q);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->tp);
            me_first(cb, q), k += 4;
        }
        me_barrier(cb, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, cs | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   rw | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
        me_alloc(cb, 1), k += 4;
        me_barrier(cb, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, cs | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                   rw | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    }
    for (int i = 0; i < MEQ.n; i++) {
        const MeQ *q = &MEQ.q[i];
        me_bind(cb, q);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, q->mp);
        if (q->tp) vkCmdDispatchIndirect(cb, ME.buf, MESH_EMU_DOFF + (VkDeviceSize)q->rec * MESH_EMU_DWORDS * 4 + 20);
        else me_first(cb, q);
        k += 4;
    }
    me_barrier(cb, cs, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                   cs | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | rw | VK_ACCESS_2_TRANSFER_READ_BIT);
    MEQ.n = MEQ.tasks = 0, MEQ.dn = 0;
    return k + 4;
}

void vk_exec_mesh_flush(VkCommandBuffer cb) { me_flush(cb); }

static int mesh_emu_draw(VkExec *ex, uint32_t count, const VkMemSpan *ind, const VkMemSpan *args) {
    if (!me_arena()) return 0;
    ex->dc->c0_binst = 0;
    int prim = mesh_emu_draw_prim(ex);
    MeshPrepared prepared;
    if (!prepare_draw_ex(ex, prim, 0x7fffffff, 0, 1, &prepared)) return 0;
    mesh_prepared_check(&prepared);
    const VkPipeInfo *info = prepared.info;
    const GfxShader *ms = ex->dc->B.st.vs, *ts = ex->dc->B.st.ts;
    VkPipeline mp = VK_NULL_HANDLE, tp = VK_NULL_HANDLE;
    MeshEmuInfo mi;
    if (!info || !info->emu || !ex->dc->B.st_valid || !vk_pipeline_mesh_emu(ms, ts, info->layout, &mp, &tp, &mi) || !!ts != !!tp ||
        (info->push && !info->emu_tpl_c) || (!info->push && !ex->dc->B.ds_bound[0])) {
        static int logged;
        if (logged++ < 8)
            rt_log("vk: mesh emulation: a draw of program %llu skipped: %s", ex->prog ? (unsigned long long)ex->prog->serial : 0ull,
                   !info || !info->emu ? "its pipeline is not an emulated one"
                   : !mp ? "its stages are not emulated (see vk_pipeline: mesh emulation)"
                   : "its descriptors cannot be bound for compute");
        ME.skipped++;
        return 0;
    }
    VkPipelineLayout layout = info->layout;
    int push = info->push, pool = info->pool;
    VkDescriptorUpdateTemplate tpl_c = info->emu_tpl_c;
    VkDescriptorSet set0 = ex->dc->B.ds_bound[0];
    static int batch = -1;
    if (batch < 0) {
        batch = rt_knob("MESH_EMU_BATCH", "1");
        rt_log("vk: mesh emulation: the stages of a pass's emulated draws %s (NX2SARIA_MESH_EMU_BATCH=%d)",
               batch ? "run before the pass, in a command buffer of their own" : "split the pass", batch);
    }
    VkCommandBuffer pre = VK_NULL_HANDLE;
    if (batch && ex->in_pass) {
        if (ex->pre_pass != ex->pass_serial && vk_exec_split_now(ex)) {
            MeshPrepared again;
            if (!prepare_draw_ex(ex, prim, 0x7fffffff, 0, 1, &again)) { ME.skipped++; return 0; }
            mesh_prepared_check(&again);
            if (again.info != info) {
                ME.skipped++;
                return 0;
            }
            ex->pre_pass = ex->pass_serial;
            set0 = ex->dc->B.ds_bound[0];
        }
        if (ex->pre_pass == ex->pass_serial) pre = vk_sync_pre();
    }
    if (!pre) vk_exec_barrier(ex);
    if (!pre && MEQ.n) me_flush(vk_sync_pre());
    VkCommandBuffer cb = pre ? pre : ex->cb;
    VkMemSpan ring = {0};
    if (args) {
        if (!mesh_ring_entry(ex, cb, NULL, args, &ring.offset)) {
            warn_once(W_MESH, "the indirect mesh ring could not be created: skipped");
            ME.skipped++;
            return 0;
        }
        ring.buffer = mesh_ring, ind = &ring;
        cb = pre ? pre : ex->cb;
    }
    uint32_t ep = vk_texture_epoch();
    if (ep != ME.epoch) {
        ME.epoch = ep;
        if (ME.rb && ME.rb_map) {
            if (ME.resets >= 32) {
                vmaInvalidateAllocation(vk_ctx()->vma, ME.rb_alloc, 0, VK_WHOLE_SIZE);
                const uint32_t *h = ME.rb_map + 4 * ((ME.resets - 32) % ME_RB);
                for (int k = 0; k < 4; k++)
                    if (h[k] > ME.max[k]) ME.max[k] = h[k];
            }
            VkBufferCopy rc = {0, 16 * (ME.resets % ME_RB), 16};
            vkCmdCopyBuffer(cb, ME.buf, ME.rb, 1, &rc);
            ME.resets++;
            me_barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        vkCmdFillBuffer(cb, ME.buf, 0, 32, 0);
        me_barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    uint32_t rec = ME.rec++ % MESH_EMU_DRECS;
    VkDeviceSize ro = MESH_EMU_DOFF + (VkDeviceSize)rec * MESH_EMU_DWORDS * 4;
    if (MEQ.n && (MEQ.n == MEQ_MAX || (MEQ.q[0].rec + (uint32_t)MEQ.n) % MESH_EMU_DRECS != rec))
        me_flush(vk_sync_pre());
    vkCmdFillBuffer(cb, ME.buf, ro, 4, 0);
    vkCmdFillBuffer(cb, ME.buf, ro + 4, 4, 1);
    vkCmdFillBuffer(cb, ME.buf, ro + 8, (MESH_EMU_R_FIRST - 2) * 4, 0);
    if (ind) {
        const VkMemSpan *src = args ? args : ind;
        VkBufferCopy cp = {src->offset, ro + MESH_EMU_R_FIRST * 4, 4};
        vkCmdCopyBuffer(cb, src->buffer, ME.buf, 1, &cp);
    } else vkCmdFillBuffer(cb, ME.buf, ro + MESH_EMU_R_FIRST * 4, 4, count);
    vkCmdFillBuffer(cb, ME.buf, ro + MESH_EMU_R_ICUR * 4, 8, 0);
    vkCmdFillBuffer(cb, ME.buf, ro + MESH_EMU_R_PER * 4, 4, mi.max_p * (mi.prim ? mi.prim : 3));
    vkCmdFillBuffer(cb, ME.buf, ro + MESH_EMU_R_TASK * 4, 8, tp ? 1 : 0);
    uint32_t pc[8] = {(uint32_t)ME.addr, (uint32_t)(ME.addr >> 32), (uint32_t)(ro / 4), ME.iw, ME.icap, ME.vw, ME.vcap, ME.trecs};
    MeQ *q = &MEQ.q[MEQ.n];
    *q = (MeQ){.tp = tp, .mp = mp, .layout = layout, .tpl_c = tpl_c, .set0 = set0, .pset = pool ? PL.set : VK_NULL_HANDLE,
               .push = push, .count = count, .rec = rec, .ib = ind ? ind->buffer : VK_NULL_HANDLE, .io = ind ? ind->offset : 0};
    memcpy(q->pc, pc, sizeof pc);
    if (push) {
        if (MEQ.dn + emu_d_bytes > MEQ.dcap) {
            size_t c = MEQ.dcap ? MEQ.dcap * 2 : 65536;
            while (c < MEQ.dn + emu_d_bytes) c *= 2;
            uint8_t *d = realloc(MEQ.d, c);
            if (!d) {
                ME.skipped++;
                return 0;
            }
            MEQ.d = d, MEQ.dcap = c;
        }
        memcpy(MEQ.d + MEQ.dn, emu_d, emu_d_bytes);
        q->doff = MEQ.dn, q->dbytes = emu_d_bytes, MEQ.dn += emu_d_bytes;
    }
    MEQ.n++, MEQ.tasks += !!tp;
    if (!pre) ex->recorded += 3 + me_flush(cb);
    if (!pre) {
        vk_exec_draw_new_cb();
        ex->dirty = VK_EXEC_DIRTY_ALL;
        MeshPrepared again;
        if (!prepare_draw_ex(ex, prim, 0x7fffffff, 0, 1, &again)) { ME.skipped++; return 0; }
        mesh_prepared_check(&again);
        if (again.info != info) {
            ME.skipped++;
            return 0;
        }
    }
    cb = ex->cb;
    vkCmdBindIndexBuffer(cb, ME.buf, (VkDeviceSize)ME.iw * 4, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, MESH_EMU_PC_BYTES, pc);
    vkCmdDrawIndexedIndirect(cb, ME.buf, ro, 1, 20);
    ex->recorded += 3;
    vk_exec_frame.draws++;
    vk_exec_draw_new_cb();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ME.draws++;
    static uint64_t log_frame;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if ((rt_trace & RT_TRACE_PERF) && f / 600 != log_frame / 600) {
        log_frame = f;
        rt_log("perf: frame %llu: mesh emulation: %llu draws, %llu skipped (not emulated or not ready); most in one submission: "
               "%u of %u vertex slots, %u of %u indices, %u of %u task records, %u mesh workgroups that did not fit",
               (unsigned long long)f, (unsigned long long)ME.draws, (unsigned long long)ME.skipped, ME.max[0], ME.vcap,
               ME.max[1], ME.icap, ME.max[2], ME.trecs, ME.max[3]);
        ME.draws = ME.skipped = 0;
        memset(ME.max, 0, sizeof ME.max);
    }
    return 1;
}

static void bind_index_buffer(VkExec *ex, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    CbState *held = &ex->dc->B;
    if (held->ib_valid && held->ib == b && held->ib_off == o && held->ib_type == t) return;
    vkCmdBindIndexBuffer(ex->cb, b, o, t);
    ex->recorded++;
    held->ib = b, held->ib_off = o, held->ib_type = t, held->ib_valid = 1;
}

static void bind_index(VkExec *ex, const void *data, uint64_t bytes, VkIndexType type) {
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, bytes, &b, &o), data, bytes);
    bind_index_buffer(ex, b, o, type);
}

static void op_draw(VkExec *ex, const p_draw *d) {
    ex->dc->c0_binst = (uint32_t)d->base_instance;
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, 0)) return;
    vk_exec_frame.draws++;
    uint32_t inst = d->instances > 0 ? (uint32_t)d->instances : 1, first = (uint32_t)d->first, n = (uint32_t)d->count;
    if (d->prim == NVN_DRAW_PRIMITIVE_QUADS && n >= 4) {
        uint32_t q = n / 4;
        uint32_t *ix = malloc(sizeof *ix * 6 * q);
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < q; i++) {
            uint32_t v = first + 4 * i, k[6] = {0, 1, 2, 0, 2, 3};
            for (int j = 0; j < 6; j++) ix[6 * i + j] = v + k[j];
        }
        bind_index(ex, ix, sizeof *ix * 6 * q, VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, 6 * q, inst, 0, 0, (uint32_t)d->base_instance);
    } else if (d->prim == NVN_DRAW_PRIMITIVE_LINE_LOOP && n >= 2) {
        uint32_t *ix = malloc(sizeof *ix * (n + 1));
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < n; i++) ix[i] = first + i;
        ix[n] = first;
        bind_index(ex, ix, sizeof *ix * (n + 1), VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, n + 1, inst, 0, 0, (uint32_t)d->base_instance);
    } else {
        vkCmdDraw(ex->cb, n, inst, first, (uint32_t)d->base_instance);
    }
    ex->recorded++;
}

static int restart_for(const VkExec *ex, int prim, int itype) {
    if (!ex->prim_restart.enable) return 0;
    switch (vk_pipeline_topology(prim)) {
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: break;
    default: return 0;
    }
    static const uint32_t all_ones[3] = {0xffu, 0xffffu, 0xffffffffu};
    if (ex->prim_restart.index == all_ones[itype]) return 1;
    warn_once(W_RESTART, "primitive restart index %#x with %d-byte indices is not replayed: strips drawn without restart",
              ex->prim_restart.index, 1 << itype);
    return 0;
}

static int quad_index_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2SARIA_QUAD_INDEX");
    return on;
}

static int draw_idx_rewritten(VkExec *ex, const p_draw_idx *d, const VkMemSpan *sp, int restart) {
    static const uint32_t isize[3] = {1, 2, 4};
    int quads = d->prim == NVN_DRAW_PRIMITIVE_QUADS, loop = d->prim == NVN_DRAW_PRIMITIVE_LINE_LOOP;
    if (!quads && !loop) return 0;
    if (!quad_index_on()) {
        warn_once(W_QUAD, "an indexed %s (%d indices): drawn with its indices as a %s (quad_index = 1 rewrites them)",
                  quads ? "quad list" : "line loop", d->count, quads ? "triangle list" : "line strip");
        return 0;
    }
    if (sp->alloc || (loop && restart)) {
        warn_once(W_QUAD, "an indexed %s (%d indices, %s): drawn with its indices as they are", quads ? "quad list" : "line loop",
                  d->count, sp->alloc ? "indices in device-local memory" : "primitive restart");
        return 0;
    }
    uint32_t n = (uint32_t)d->count, unit = isize[d->itype];
    if (quads && n < 4) return 1;
    if (loop && n < 2) return 0;
    uint32_t cap = quads ? 6 * (n / 4) : n + 1;
    uint32_t *ix = malloc(sizeof *ix * (size_t)cap);
    if (!ix) rt_fatal("vk: out of memory");
    const uint8_t *src = (const uint8_t *)(uintptr_t)d->ibuf;
    uint32_t m = quads ? idx_quads(src, unit, n, ix) : idx_loop(src, unit, n, ix);
    vk_mem_count(sp->cls, VK_MUSE_CPU_READ, (uint64_t)n * unit);
    vk_mem_count(VK_MCLS_SCRATCH, VK_MUSE_IB, sizeof *ix * (uint64_t)m);
    bind_index(ex, ix, sizeof *ix * (uint64_t)m, VK_INDEX_TYPE_UINT32);
    free(ix);
    static int told;
    if (!told++)
        rt_log("vk: an indexed %s of %u indices drawn as %u indices of a %s (NX2SARIA_QUAD_INDEX)", quads ? "quad list" : "line loop",
               n, m, quads ? "triangle list" : "line strip");
    vkCmdDrawIndexed(ex->cb, m, d->instances > 0 ? (uint32_t)d->instances : 1, 0, d->base_vertex, (uint32_t)d->base_instance);
    ex->recorded++;
    return 1;
}

static void op_draw_idx(VkExec *ex, const p_draw_idx *d) {
    ex->dc->c0_binst = (uint32_t)d->base_instance;
    static const uint32_t isize[3] = {1, 2, 4};
    if (d->itype < 0 || d->itype > 2) return warn_once(W_INDEX, "index type %d is unknown: draw skipped", d->itype);
    int restart = restart_for(ex, d->prim, d->itype);
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, restart)) return;
    vk_exec_frame.draws++;
    uint64_t bytes = (uint64_t)d->count * isize[d->itype];
    VkMemSpan sp;
    if (!vk_mem_resolve(d->ibuf, bytes, &sp)) return warn_once(W_INDEX, "index buffer %#llx is not backed: draws with it are skipped", (unsigned long long)d->ibuf);
    if (draw_idx_rewritten(ex, d, &sp, restart)) return;
    if (d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) {
        if (sp.alloc) return warn_once(W_INDEX, "byte indices in device-local memory: draw skipped");
        vk_mem_count(sp.cls, VK_MUSE_CPU_READ, bytes);
        vk_mem_count(VK_MCLS_SCRATCH, VK_MUSE_IB, 2 * bytes);
        uint16_t *ix = malloc(sizeof *ix * (size_t)d->count);
        if (!ix) rt_fatal("vk: out of memory");
        for (int i = 0; i < d->count; i++) {
            uint8_t v = ((const uint8_t *)(uintptr_t)d->ibuf)[i];
            ix[i] = restart && v == 0xff ? 0xffff : v;
        }
        bind_index(ex, ix, sizeof *ix * (size_t)d->count, VK_INDEX_TYPE_UINT16);
        free(ix);
    } else if (sp.offset % isize[d->itype]) {
        return warn_once(W_INDEX, "index buffer %#llx is not aligned to its index size: draw skipped", (unsigned long long)d->ibuf);
    } else {
        VK_MEM_USE(sp);
        bind_index_buffer(ex, sp.buffer, sp.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vk_mem_count(sp.cls, VK_MUSE_IB, bytes);
        VK_MEM_BOUND(sp);
    }
    vkCmdDrawIndexed(ex->cb, (uint32_t)d->count, d->instances > 0 ? (uint32_t)d->instances : 1, 0, d->base_vertex,
                     (uint32_t)d->base_instance);
    ex->recorded++;
    if (d->itype != NVN_INDEX_TYPE_UNSIGNED_BYTE) mvobj_draw(ex, d, sp.buffer, sp.offset);
}

static void op_draw_idx_ind(VkExec *ex, const p_draw_idx_ind *d) {
    ex->dc->c0_binst = 0;
    if (d->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT && d->itype != NVN_INDEX_TYPE_UNSIGNED_INT)
        return warn_once(W_INDEX, "indirect draw with index type %d: skipped", d->itype);
    VkMemSpan ib, ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDrawIndexedIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect draw arguments at %#llx are not backed or not aligned: draws skipped", (unsigned long long)d->ind);
    if (!vk_mem_resolve(d->ibuf, 1, &ib) || ib.offset % (d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4))
        return warn_once(W_INDEX, "index buffer %#llx is not backed or not aligned: indirect draws skipped", (unsigned long long)d->ibuf);
    if (!prepare_draw(ex, d->prim, 0x7fffffff, restart_for(ex, d->prim, d->itype))) return;
    vk_exec_frame.draws++;
    VK_MEM_USE(ip);
    VK_MEM_USE(ib);
    bind_index_buffer(ex, ib.buffer, ib.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(ex->cb, ip.buffer, ip.offset, 1, sizeof(VkDrawIndexedIndirectCommand));
    ex->recorded++;
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDrawIndexedIndirectCommand));
    VK_MEM_BOUND(ip);
    VK_MEM_BOUND(ib);
}


static int fmt_srgb(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32; }
static int fmt_rgba8(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM ||
           f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

#define SCRATCH_N 8
static VkImage fallback_scratch(VkExec *ex, uint32_t w, uint32_t h, VkFormat f) {
    static ScrEntry ent[SCRATCH_N];
    static struct { VkImage img; VmaAllocation a; } cache[SCRATCH_N];
    static int evictions;
    int hit, evict, i = scr_pick(ent, SCRATCH_N, w, h, (uint32_t)f,
                                 __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), &hit, &evict);
    if (hit) return cache[i].img;
    if (evict) {
        if (evictions++ < 8)
            rt_log("vk: a stand-in scratch image of %ux%u (format %d) makes room for one of %ux%u (format %d)",
                   ent[i].w, ent[i].h, (int)ent[i].f, w, h, (int)f);
        vk_tonemap_forget(cache[i].img);
        vk_defer_image(cache[i].img, cache[i].a);
        ent[i].live = 0, cache[i].img = VK_NULL_HANDLE, cache[i].a = NULL;
    }
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     (f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && knob_tm_dispatch() != 2 ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ia = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img; VmaAllocation a;
    if (vmaCreateImage(c->vma, &ii, &ia, &img, &a, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkImageMemoryBarrier2 bar = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ent[i] = (ScrEntry){w, h, (uint32_t)f, __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), 1};
    cache[i].img = img, cache[i].a = a;
    return img;
}

static int uses_tensor_ops(const VkExec *ex) {
    static struct { uint64_t serial; int8_t v; } memo[256];
    const uint8_t *ctl, *code;
    uint32_t len;
    if (!ex->prog || !find_stage(ex->dc, ex->prog, NVN_STAGE_CS, &ctl, &code, &len)) return 0;
    uint64_t sr = ex->prog->serial;
    typeof(memo[0]) *m = &memo[(sr * 0x9E3779B97F4A7C15ull) >> 56];
    if (sr && m->serial == sr) return m->v;
    int v = 0;
    if (ctl_nvn2(ctl))
        for (uint32_t o = 0; o + 16 <= len && !v; o += 16) {
            uint32_t op = (uint32_t)(code[o] | code[o + 1] << 8) & 0xfff;
            v = op == 0x23c || op == 0x237 || op == 0x83b;
        }
    m->serial = sr, m->v = (int8_t)v;
    return v;
}

static int native_tensor(const VkExec *ex) {
    static int on = -1;
    if (on < 0) on = rt_knob("NATIVE_TENSOR", "0");
    return on || !uses_tensor_ops(ex);
}

static void log_cs_bindings_locked(const VkExec *ex) {
    char line[1600];
    int o = 0;
    line[0] = 0;
    for (int i = 0; i < 160 && o < (int)sizeof line - 80; i++) {
        int im = i >= 128;
        uint64_t h = im ? ex->img[NVN_STAGE_CS][i - 128 < 32 ? i - 128 : 0] : ex->tex[NVN_STAGE_CS][i];
        if (im && i - 128 >= 32) break;
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (t && (i == 0 || h != ex->tex[NVN_STAGE_CS][0] || im))
            o += snprintf(line + o, sizeof line - o, " %s%d=%ux%u/f%d@%#llx", im ? "img" : "tex", im ? i - 128 : i,
                          t->width, t->height, t->format, (unsigned long long)t->addr);
    }
    for (int i = 0; i < 18 && o < (int)sizeof line - 60; i++)
        if (ex->ubo[NVN_STAGE_CS][i].size)
            o += snprintf(line + o, sizeof line - o, " ubo%d=%#llx+%#x", i + 3,
                          (unsigned long long)ex->ubo[NVN_STAGE_CS][i].addr, (unsigned)ex->ubo[NVN_STAGE_CS][i].size);
    const uint8_t *ctl, *code;
    uint32_t len;
    int ntex = -1, nimg = -1;
    if (ex->prog && find_stage(ex->dc, ex->prog, NVN_STAGE_CS, &ctl, &code, &len) && ctl_nvn2(ctl)) ntex = ctl[0xb3], nimg = ctl[0xb6];
    rt_log("vk_exec:   its bindings (control: %d texture, %d image entries):%s", ntex, nimg, line);
}

static void log_fallback_skip_locked(const VkExec *ex, const VkTexImage *out, int in_place) {
    static struct { uint64_t prog, d; } seen[256];
    static int n;
    uint64_t pr = ex->prog ? ex->prog->serial : 0, d = out ? out->addr : 0;
    for (int k = 0; k < n; k++)
        if (seen[k].prog == pr && seen[k].d == d) return;
    if (n == 256) return;
    seen[n].prog = pr, seen[n].d = d, n++;
    if (in_place < 0)
        rt_log("vk_exec: fallback leaves prog %llu alone: not translated and no tensor-core ops, so nothing stands in",
               (unsigned long long)pr);
    else if (!out)
        rt_log("vk_exec: fallback leaves prog %llu alone: it binds no single-sample 2D colour image to write",
               (unsigned long long)pr);
    else
        rt_log("vk_exec: fallback leaves prog %llu alone: %s; its output %ux%u fmt %d at %#llx", (unsigned long long)pr,
               in_place ? "it also samples its output (in place)"
                        : "no RGBA8 input and no input in the output's format, no larger than the output",
               out->width, out->height, out->format, (unsigned long long)out->addr);
    log_cs_bindings_locked(ex);
}

static int fallback_9e5_scene(VkFormat src, VkFormat dst) {
    return src == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && dst == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
}

static uint64_t cs_guest_hash(const prog_t *p) {
    static struct { uint64_t serial, hash; } memo[1024];
    typeof(memo[0]) *m = p->serial ? &memo[p->serial * 0x9E3779B97F4A7C15ull >> 54] : NULL;
    if (m && m->serial == p->serial) return m->hash;
    const uint8_t *ctl, *code;
    uint32_t len;
    uint64_t h = 0;
    if (find_stage_scan(p, NVN_STAGE_CS, &ctl, &code, &len) && code && len) {
        h = 0xcbf29ce484222325ull;
        for (uint32_t i = 0; i < len; i++) h = (h ^ code[i]) * 0x100000001b3ull;
    }
    if (m) m->serial = p->serial, m->hash = h;
    return h;
}

static int aa_dispatch(VkExec *ex, int x, int y, int z) {
    int mode = vk_aa_mode();
    if (mode == VK_AA_GAME || !ex->prog) return 0;
    uint64_t gh = cs_guest_hash(ex->prog);
    static int log_on = -1;
    if (log_on < 0) log_on = rt_knob("AA_LOG", "0");
    if (log_on) {
        static uint64_t seen[512];
        static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i] == gh;
        if (!known && nseen < 512) {
            seen[nseen++] = gh;
            const VkTexImage *o = NULL;
            vk_texture_lock();
            for (int i = 0; i < 32 && !o; i++) {
                uint64_t h = ex->img[NVN_STAGE_CS][i];
                view_t v; int hv = 0;
                o = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
            }
            rt_log("aa: compute prog %llu guest %016llx dispatch %dx%dx%d output %ux%u fmt %d",
                   (unsigned long long)ex->prog->serial, (unsigned long long)gh, x, y, z, o ? o->width : 0,
                   o ? o->height : 0, o ? (int)o->format : 0);
            vk_texture_unlock();
        }
    }
    int role = vk_aa_role(gh);
    if (role == VK_AA_ROLE_NONE) return 0;
    if (role != VK_AA_ROLE_COMPOSE) return vk_aa_skip_ok();
    const VkTexImage *dst = NULL, *src = NULL;
    VkImageView dv = VK_NULL_HANDLE, sv = VK_NULL_HANDLE;
    vk_texture_lock();
    for (int i = 0; i < 32 && !dst; i++) {
        uint64_t h = ex->img[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (!t || (t->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && t->format != VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) ||
            t->type != VK_IMAGE_TYPE_2D || t->samples != 1)
            continue;
        dst = t;
        if (storage_view_format(t, &v, hv) == VK_FORMAT_R32_UINT) dv = get_view(ex, t, &v, hv, 3, 1);
    }
    for (int i = 0; dst && i < 128 && !src; i++) {
        uint64_t h = ex->tex[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (!t || t->image == dst->image || t->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || t->width != dst->width ||
            t->height != dst->height || t->type != VK_IMAGE_TYPE_2D || t->samples != 1)
            continue;
        src = t;
        view_t nv;
        memset(&nv, 0, sizeof nv);
        sv = get_view(ex, t, &nv, 0, 3, 0);
    }
    uint32_t w = dst ? dst->width : 0, hh = dst ? dst->height : 0;
    if (dv && sv) vk_texture_storage_bound_locked(dst);
    static int told;
    if ((!dv || !sv) && !told++)
        rt_log("aa: a composition of the game's SMAA left to the game: target %s (%ux%u fmt %d, storage %d), composite %s",
               dst ? (dv ? "ok" : "without an integer storage view") : "not found", w, hh, dst ? (int)dst->format : 0,
               dst ? !!(dst->usage & VK_IMAGE_USAGE_STORAGE_BIT) : 0, src ? (sv ? "ok" : "without a view") : "not found");
    vk_texture_unlock();
    int ok = 0;
    if (dv && sv) {
        vk_exec_barrier_dispatch(ex);
        ok = vk_aa_record(ex->cb, mode, sv, dv, w, hh);
        if (ok) {
            ex->dc->B.pipe[1] = VK_NULL_HANDLE;
            ex->recorded++;
            vk_exec_barrier(ex);
        }
    }
    vk_aa_composed(ok);
    return ok;
}

static void untranslated_dispatch_fallback(VkExec *ex) {
    static int on = -1;
    if (on < 0) on = rt_knob("CS_FALLBACK", "1");
    if (!on) return;
    if (!uses_tensor_ops(ex)) {
        if (knob_log_cs_binds()) {
            vk_texture_lock();
            log_fallback_skip_locked(ex, NULL, -1);
            vk_texture_unlock();
        }
        return;
    }
    const VkTexImage *dst = NULL, *src = NULL, *out;
    int in_place = 0;
    int nimg = 32;
    const uint8_t *ctl, *code;
    uint32_t len;
    if (ex->prog && find_stage(ex->dc, ex->prog, NVN_STAGE_CS, &ctl, &code, &len) && ctl_nvn2(ctl)) nimg = ctl[0xb6] ? ctl[0xb6] : 1;
    if (nimg > 32) nimg = 32;
    vk_texture_lock();
    for (int i = 0; i < nimg; i++) {
        uint64_t h = ex->img[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (t && t->aspect == VK_IMAGE_ASPECT_COLOR_BIT && t->type == VK_IMAGE_TYPE_2D && t->samples == 1 &&
            (!dst || (uint64_t)t->width * t->height > (uint64_t)dst->width * dst->height))
            dst = t;
    }
    out = dst;
    for (int i = 0; dst && i < 128; i++) {
        uint64_t h = ex->tex[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        if (t && t->image == dst->image) { dst = NULL; in_place = 1; break; }
    }
    for (int i = 0; dst && i < 128; i++) {
        uint64_t h = ex->tex[NVN_STAGE_CS][i];
        view_t v; int hv = 0;
        const VkTexImage *t = h ? vk_texture_by_id_locked(NVN_HANDLE_TEX(h), &v, &hv) : NULL;
        int rgba8 = t && (t->format == VK_FORMAT_R8G8B8A8_UNORM || t->format == VK_FORMAT_R8G8B8A8_SRGB);
        int same = t && (t->format == dst->format || (fallback_9e5_scene(t->format, dst->format) && raw_9e5_twin(t)));
        if (t && (rgba8 || same) && t->image != dst->image && t->aspect == VK_IMAGE_ASPECT_COLOR_BIT && t->type == VK_IMAGE_TYPE_2D &&
            t->samples == 1 && t->width <= dst->width && t->height <= dst->height && t->width >= 16 && t->height >= 16 &&
            (!src || (uint64_t)t->width * t->height > (uint64_t)src->width * src->height))
            src = t;
    }
    if (dst && src) vk_texture_storage_bound_locked(dst);
    else if (knob_log_cs_binds()) log_fallback_skip_locked(ex, out, in_place);
    if (dst && src && knob_log_cs_binds()) {
        static struct { uint64_t prog, s, d; } seen[256];
        static int n;
        uint64_t pr = ex->prog ? ex->prog->serial : 0;
        int k = 0;
        while (k < n && (seen[k].prog != pr || seen[k].s != src->addr || seen[k].d != dst->addr)) k++;
        if (k == n && n < 256) seen[n++] = (typeof(seen[0])){pr, src->addr, dst->addr}, k = -1;
        if (k < 0) {
            rt_log("vk_exec: fallback blit prog %llu: %ux%u fmt %d at %#llx -> %ux%u fmt %d at %#llx",
                   (unsigned long long)(ex->prog ? ex->prog->serial : 0), src->width, src->height, src->format,
                   (unsigned long long)src->addr, dst->width, dst->height, dst->format, (unsigned long long)dst->addr);
            log_cs_bindings_locked(ex);
        }
    }
    VkImage si = src ? src->image : VK_NULL_HANDLE, di = dst ? dst->image : VK_NULL_HANDLE;
    VkFormat sf = src ? src->format : VK_FORMAT_UNDEFINED;
    if (src && dst && fallback_9e5_scene(sf, dst->format)) {
        const VkTexImage *twin = raw_9e5_twin(src);
        si = twin ? twin->image : VK_NULL_HANDLE, sf = twin ? twin->format : VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
    int native = raw_native_9e5(si, sf);
    VkImageView dst_view = VK_NULL_HANDLE;
    if (src && dst && (dst->usage & VK_IMAGE_USAGE_STORAGE_BIT) && (raw_9e5(si, sf) || native) &&
        dst->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && knob_tm_dispatch() != 2) {
        view_t nv;
        memset(&nv, 0, sizeof nv);
        dst_view = get_view(ex, dst, &nv, 0, 3, 1);
    }
    uint32_t sw = src ? src->width : 0, sh = src ? src->height : 0, dw = dst ? dst->width : 0, dh = dst ? dst->height : 0;
    VkFormat df = dst ? dst->format : VK_FORMAT_UNDEFINED;
    vk_texture_unlock();
    if (!si || !di) return;
    static int logged;
    if (!logged++) rt_log("vk_exec: untranslated compute dispatch: its output image (%ux%u) gets a linear blit of its "
                          "largest input (%ux%u) instead (NX2SARIA_CS_FALLBACK=0 turns this off)", dw, dh, sw, sh);
    vk_exec_pass_end(ex);
    vk_exec_barrier(ex);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}};
    bl.srcOffsets[1] = (VkOffset3D){(int32_t)sw, (int32_t)sh, 1};
    bl.dstOffsets[1] = (VkOffset3D){(int32_t)dw, (int32_t)dh, 1};
    VkImage scratch = fmt_rgba8(df) && fmt_srgb(sf) != fmt_srgb(df) ? fallback_scratch(ex, dw, dh, sf) : VK_NULL_HANDLE;
    static int e5_blit = -1;
    if (e5_blit < 0) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        e5_blit = (fp.optimalTilingFeatures & need) == need ? 1 + !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) : 0;
        rt_log("vk_exec: RGB9E5 fallback decode %s", e5_blit ? "on" : "unavailable (E5B9G9R9 is no blit source here)");
    }
    VkImage e5 = scratch || !e5_blit ? VK_NULL_HANDLE : native ? si
               : raw_9e5(si, sf) ? fallback_scratch(ex, sw, sh, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) : VK_NULL_HANDLE;
    if (e5 && !native) {
        VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {sw, sh, 1}};
        vkCmdCopyImage(ex->cb, si, VK_IMAGE_LAYOUT_GENERAL, e5, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(ex->cb, &dep);
        si = e5;
    }
#if NX2SARIA_TONEMAP_STANDIN
    VkImage tm = e5 && dst_view ? vk_tonemap_record(ex->cb, e5, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, sw, sh, dw, dh,
                                                    !vk_dlaa_active())
                                : VK_NULL_HANDLE;
    if (tm) {
        ex->dc->B.pipe[1] = VK_NULL_HANDLE;
        si = tm, sw = dw, sh = dh;
        bl.srcOffsets[1] = (VkOffset3D){(int32_t)sw, (int32_t)sh, 1};
    }
#else
    VkImage tm = VK_NULL_HANDLE;
    (void)dst_view;
#endif
    int filter_nearest = tm || (e5 && e5_blit == 1);
    VkImage g8 = VK_NULL_HANDLE, g8s = VK_NULL_HANDLE;
    if (e5 && !tm && !scratch && !fmt_rgba8(df)) {
        g8 = fallback_scratch(ex, dw, dh, VK_FORMAT_R8G8B8A8_UNORM);
        g8s = g8 ? fallback_scratch(ex, dw, dh, VK_FORMAT_R8G8B8A8_SRGB) : VK_NULL_HANDLE;
    }
    if (g8 && g8s) {
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdBlitImage(ex->cb, si, VK_IMAGE_LAYOUT_GENERAL, g8, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                       filter_nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        vkCmdPipelineBarrier2(ex->cb, &dep);
        VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {dw, dh, 1}};
        vkCmdCopyImage(ex->cb, g8, VK_IMAGE_LAYOUT_GENERAL, g8s, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        vkCmdPipelineBarrier2(ex->cb, &dep);
        si = g8s, sw = dw, sh = dh, filter_nearest = 1;
        bl.srcOffsets[1] = (VkOffset3D){(int32_t)sw, (int32_t)sh, 1};
    }
    vkCmdBlitImage(ex->cb, si, VK_IMAGE_LAYOUT_GENERAL, scratch ? scratch : di, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                   filter_nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    if (scratch) {
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(ex->cb, &dep);
        VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {dw, dh, 1}};
        vkCmdCopyImage(ex->cb, scratch, VK_IMAGE_LAYOUT_GENERAL, di, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    }
    ex->recorded++;
    vk_exec_barrier(ex);
    vk_dlaa_upscaler(ex, di, df, dw, dh);
}

static void op_dispatch(VkExec *ex, const p_dispatch *d) {
    DrawCtx *const cx = ex->dc;
    if (d->x <= 0 || d->y <= 0 || d->z <= 0) return;
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    if (prog_lead_on()) prog_lead(ex->prog, 2);
    vk_contact_flush(ex);
    cb_res(ex);
    dummies_init();
    if (aa_dispatch(ex, d->x, d->y, d->z)) return;
    if (!native_tensor(ex)) { untranslated_dispatch_fallback(ex); return; }
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);
    if (knob_log_cs_binds()) {
        static uint64_t seen[64]; static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) known |= seen[i] == ex->prog->serial;
        if (!known && nseen < 64) {
            seen[nseen++] = ex->prog->serial;
            char line[2048]; int o = snprintf(line, sizeof line, "cs binds prog %llu %dx%dx%d:", (unsigned long long)ex->prog->serial, d->x, d->y, d->z);
            for (int i = 0; i < 128 && o < 1900; i++) if (ex->tex[NVN_STAGE_CS][i]) o += snprintf(line + o, sizeof line - o, " tex%d=%llx", i, (unsigned long long)ex->tex[NVN_STAGE_CS][i]);
            for (int i = 0; i < 32 && o < 1900; i++) if (ex->img[NVN_STAGE_CS][i]) o += snprintf(line + o, sizeof line - o, " img%d=%llx", i, (unsigned long long)ex->img[NVN_STAGE_CS][i]);
            const GfxShaderRes *rs;
            int nt = cs ? gfx_shader_resources(cs, GFX_RES_TEX, &rs) : 0;
            for (int i = 0; i < nt && o < 1900; i++) o += snprintf(line + o, sizeof line - o, " want-tex(h%u,slot%d)", rs[i].handle, rs[i].slot);
            int ni = cs ? gfx_shader_resources(cs, GFX_RES_IMG, &rs) : 0;
            for (int i = 0; i < ni && o < 1900; i++) o += snprintf(line + o, sizeof line - o, " want-img(h%u,slot%d)", rs[i].handle, rs[i].slot);
            rt_log("%s%s", line, cs ? "" : " (not translated)");
        }
    }
    if (!cs || !native_tensor(ex)) { untranslated_dispatch_fallback(ex); return; }
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    if ((info->pool && !pool_on()) || shader_skipped(cs)) return;
    vk_exec_barrier_dispatch(ex);
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    cx->c0_dim[0] = (uint32_t)d->x, cx->c0_dim[1] = (uint32_t)d->y, cx->c0_dim[2] = (uint32_t)d->z;
    twin_dispatch = uses_tensor_ops(ex) ? 2 : 1;
    {
        uint64_t bt = bt_now();
        bind_resources(ex, info);
        vk_exec_frame.bind_ns += (uint64_t)(bt_now() - bt);
        vk_exec_frame.bind_calls++;
    }
    twin_dispatch = 0;
    if (vk_texture_deferred()) return;
    VkPassMark *m = dispatch_mark(ex, cs, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vkCmdDispatch(ex->cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vk_pass_mark_end(ex->cb, m);
    ex->recorded++;
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, d);
}

static void op_dispatch_ind(VkExec *ex, const p_dispatch_ind *d) {
    DrawCtx *const cx = ex->dc;
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    VkMemSpan ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDispatchIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect dispatch arguments at %#llx are not backed or not aligned: skipped", (unsigned long long)d->ind);
    cb_res(ex);
    dummies_init();
    if (aa_dispatch(ex, 0, 0, 0)) return;
    if (!native_tensor(ex)) { untranslated_dispatch_fallback(ex); return; }
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);
    if (!cs || !native_tensor(ex)) { untranslated_dispatch_fallback(ex); return; }
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    if ((info->pool && !pool_on()) || shader_skipped(cs)) return;
    vk_exec_barrier_dispatch(ex);
    VK_MEM_USE(ip);
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    cx->c0_dim[0] = cx->c0_dim[1] = cx->c0_dim[2] = 0;
    cx->B.c0_valid[2] = 0;
    twin_dispatch = uses_tensor_ops(ex) ? 2 : 1;
    {
        uint64_t bt = bt_now();
        bind_resources(ex, info);
        vk_exec_frame.bind_ns += (uint64_t)(bt_now() - bt);
        vk_exec_frame.bind_calls++;
    }
    twin_dispatch = 0;
    if (vk_texture_deferred()) return;
    if (cx->B.c0_valid[2]) {
        const uint8_t *pctl, *pcode;
        uint32_t plen;
        int nvn2 = find_stage(ex->dc, ex->prog, NVN_STAGE_CS, &pctl, &pcode, &plen) && ctl_nvn2(pctl);
        VkBufferCopy cp = {ip.offset, cx->B.c0_off[2] + (nvn2 ? 0 : 0x200), sizeof(VkDispatchIndirectCommand)};
        vkCmdCopyBuffer(ex->cb, ip.buffer, cx->B.c0_buf[2], 1, &cp);
        ex->recorded++;
        vk_exec_barrier(ex);
        cx->B.c0_valid[2] = 0;
    }
    VkPassMark *m = dispatch_mark(ex, cs, 0, 0, 0);
    vkCmdDispatchIndirect(ex->cb, ip.buffer, ip.offset);
    vk_pass_mark_end(ex->cb, m);
    ex->recorded++;
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDispatchIndirectCommand));
    VK_MEM_BOUND(ip);
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, &(p_dispatch){0, 0, 0, d->caller});
}

static void op_update_ub(VkExec *ex, const p_update_ub *u) {
    if (!u->size) return;
    VkMemSpan sp;
    if (!vk_mem_resolve(u->dst, u->size, &sp)) return warn_once(W_UB, "UpdateUniformBuffer to %#llx is not backed: dropped", (unsigned long long)u->dst);
    cb_res(ex);
    vk_exec_frame.ub_updates++;
    if (ex->in_pass) vk_exec_frame.ub_pass_breaks++;
    vk_exec_barrier(ex);
    VkBuffer b;
    VkDeviceSize o;
    memcpy(scratch(ex, u->size, &b, &o), u->data, u->size);
    VK_MEM_PICK(sp);
    VkBufferCopy cp = {o, sp.offset, u->size};
    vkCmdCopyBuffer(ex->cb, b, sp.buffer, 1, &cp);
    if (vk_exec_tail() && vk_mem_mirrored(u->dst, u->size) &&
        (!vk_mem_wb_lazy() || !mem_lazy_armed(u->dst, u->size)))
        vk_exec_tail_ub(ex->fence, u->dst, u->data, u->size);
    else vk_mem_gpu_wrote(&sp, u->size);
    vk_usnap_drop(u->dst, u->size);
    ex->recorded++;
    vk_mem_count(sp.cls, VK_MUSE_GPU_WRITE, u->size);
    note_written(ex, u->dst, u->size);
    upd_note(ex, u);
}


static void bind_handles(uint64_t *dst, int cap, const p_bind_handles *b) {
    if (b->stage < 0 || b->stage >= 8 || b->first < 0 || b->n < 0 || b->n > 32 || b->first + b->n > cap) {
        warn_once(W_BIND, "a bind of %d handles at %d of stage %d is out of range: ignored", b->n, b->first, b->stage);
        return;
    }
    memcpy(&dst[b->first], b->h, sizeof(uint64_t) * (size_t)b->n);
}

static void op_draw_impl(VkExec *ex, uint32_t op, const void *payload) {
    DrawCtx *const cx = ex->dc;
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = payload;
        ex->prog = b->prog, ex->prog_stages = b->stages;
        cx->bind_gen++;
        break;
    }
    case OP_BIND_STATE: {
        const p_bind_state *b = payload;
        blend_st bs;
        switch (b->kind) {
        case 0:
            memcpy(&bs, b->bytes, sizeof bs);
            if (bs.target < 8) ex->pipe.blend[bs.target] = bs;
            break;
        case 1: memcpy(&ex->pipe.color, b->bytes, sizeof ex->pipe.color); break;
        case 2: memcpy(&ex->pipe.chmask, b->bytes, sizeof ex->pipe.chmask); break;
        case 3: memcpy(&ex->pipe.ms, b->bytes, sizeof ex->pipe.ms); break;
        case 4: memcpy(&ex->pipe.poly, b->bytes, sizeof ex->pipe.poly); break;
        case 5: memcpy(&ex->pipe.ds, b->bytes, sizeof ex->pipe.ds); break;
        case 6: memcpy(&ex->point_size, b->bytes, sizeof ex->point_size); break;
        case 7: memcpy(&ex->line_width, b->bytes, sizeof ex->line_width); break;
        }
        break;
    }
    case OP_BIND_VATTR: {
        const p_bind_vattr *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vattr, 0, sizeof ex->pipe.vattr);
        memcpy(&ex->pipe.vattr[b->first], b->st, sizeof(uint32_t) * (size_t)b->n);
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memset(ex->pipe.vstream, 0, sizeof ex->pipe.vstream);
        memcpy(&ex->pipe.vstream[b->first], b->st, sizeof(vs_st) * (size_t)b->n);
        break;
    }
    case OP_BIND_VBUF: {
        const p_bind_vbuf *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        memcpy(&ex->vbuf[b->first], b->r, sizeof(NVNbufferRange) * (size_t)b->n);
        break;
    }
    case OP_BIND_UBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 8 && b->index >= 0 && b->index < 18) ex->ubo[b->stage][b->index] = *b;
        else if (b->index >= 18 && b->index < 64) {
            static uint64_t seen;
            if (!(__atomic_fetch_or(&seen, 1ull << b->index, __ATOMIC_RELAXED) >> b->index & 1))
                rt_log("vk: uniform buffer bind to index %d (bank %d), stage %d, %llu bytes: beyond Maxwell's banks, dropped",
                       b->index, b->index + 3, b->stage, (unsigned long long)b->size);
        }
        cx->bind_gen++;
        break;
    }
    case OP_BIND_SSBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 8 && b->index >= 0 && b->index < 16) ex->ssbo[b->stage][b->index] = *b;
        if (b->stage >= NVN_STAGE_TASK && b->stage <= NVN_STAGE_MESH && b->index >= 0 && b->index < 16) {
            static uint32_t seen;
            uint32_t bit = 1u << (16 * (b->stage - NVN_STAGE_TASK) + b->index);
            if (!(__atomic_fetch_or(&seen, bit, __ATOMIC_RELAXED) & bit))
                rt_log("vk: storage buffer bind, stage %d slot %d: %#llx, %llu bytes", b->stage, b->index,
                       (unsigned long long)b->addr, (unsigned long long)b->size);
        }
        cx->bind_gen++;
        break;
    }
    case OP_BIND_TEX: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 8 ? ex->tex[b->stage] : NULL, 128, b);
        ex->tex_serial++;
        cx->bind_gen++;
        break;
    }
    case OP_BIND_SAMP: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 8 ? ex->samp[b->stage] : NULL, 128, b);
        break;
    }
    case OP_BIND_IMG: {
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 8 ? ex->img[b->stage] : NULL, 32, b);
        ex->tex_serial++;
        cx->bind_gen++;
        break;
    }
    case OP_UPDATE_UB: op_update_ub(ex, payload); break;
    case OP_PUSH_CC: {
        const p_push_cc *p = payload;
        if (p->offset + (uint64_t)p->size <= sizeof ex->cs_push) memcpy(ex->cs_push + p->offset, p->data, p->size);
        cx->bind_gen++;
        break;
    }
    case OP_DRAW: op_draw(ex, payload); break;
    case OP_DRAW_MESH: vk_exec_frame.draws_seen++; op_draw_mesh(ex, payload); break;
    case OP_DRAW_MESH_IND: vk_exec_frame.draws_seen++; op_draw_mesh_ind(ex, payload); break;
    case OP_DRAW_IDX: op_draw_idx(ex, payload); break;
    case OP_DISPATCH: op_dispatch(ex, payload); break;
    case OP_DRAW_IDX_IND: op_draw_idx_ind(ex, payload); break;
    case OP_DISPATCH_IND: op_dispatch_ind(ex, payload); break;
    default: rt_fatal("vk: op %s is not a draw op", nvn_op_name(op));
    }
}

const char *vk_mvobj_frame_masks(void) { return fm_last; }

static int op_is_draw(uint32_t op) {
    switch (op) {
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND:
    case OP_DISPATCH: case OP_DISPATCH_IND:
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
    case OP_UPDATE_UB:
        return 1;
    default:
        return 0;
    }
}

enum { PK_DRAW, PK_MESH, PK_DISPATCH, PK_UB, PK_OTHER, PK_N };
#define PH_EVERY 600
static int ph_on = -1;
static _Thread_local int ph_worker;
static uint64_t ph_ns[2][PH_N], ph_ops[2][PK_N], ph_op_ns[2][PK_N];

void vk_exec_phase_worker(void) { ph_worker = 1; }

static uint64_t ph_code_ns[2][OP_COUNT_], ph_code_n[2][OP_COUNT_], ph_rec_ns[2], ph_rec_n[2];
int vk_exec_phases_on(void) {
    if (ph_on < 0) {
        int on = cfg_bool("NX2SARIA_REPLAY_PHASES");
        if (on) rt_log("vk_exec: replay phases timed (NX2SARIA_REPLAY_PHASES=1): a line for the submitting thread and "
                       "one for the replay workers every %d frames", PH_EVERY);
        ph_on = on;
    }
    return ph_on;
}
void vk_exec_phase_op(uint32_t op, uint64_t ns) {
    if (op >= OP_COUNT_) return;
    __atomic_fetch_add(&ph_code_ns[ph_worker][op], ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&ph_code_n[ph_worker][op], 1, __ATOMIC_RELAXED);
}
static uint64_t ph_seg_ns[SEG_N];
void vk_exec_phase_seg(int seg, uint64_t ns) {
    if (seg >= 0 && seg < SEG_N) __atomic_fetch_add(&ph_seg_ns[seg], ns, __ATOMIC_RELAXED);
}
void vk_exec_phase_record(uint64_t ns) {
    __atomic_fetch_add(&ph_rec_ns[ph_worker], ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&ph_rec_n[ph_worker], 1, __ATOMIC_RELAXED);
}

static inline void ph_mark(int p) {
    if (!ph_t) return;
    uint64_t n = plat_now_ns();
    __atomic_fetch_add(&ph_ns[ph_worker][p], n - ph_t, __ATOMIC_RELAXED);
    ph_t = n;
}
static inline void ph_add(int p, uint64_t ns) {
    if (ph_t) __atomic_fetch_add(&ph_ns[ph_worker][p], ns, __ATOMIC_RELAXED);
}

static int op_kind(uint32_t op) {
    switch (op) {
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: return PK_DRAW;
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND: return PK_MESH;
    case OP_DISPATCH: case OP_DISPATCH_IND: return PK_DISPATCH;
    case OP_UPDATE_UB: return PK_UB;
    default: return PK_OTHER;
    }
}

void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    if (!ex->dc) ex->dc = dc;
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2SARIA_REPLAY_TIME");
    if (ph_on < 0) vk_exec_phases_on();
    if (!on && !ph_on) { op_draw_impl(ex, op, payload); return; }
    uint64_t t0 = plat_now_ns();
    const int k = ph_on ? op_kind(op) : PK_OTHER;
    if (ph_on && k != PK_OTHER) ph_t = t0;
    op_draw_impl(ex, op, payload);
    uint64_t t1 = plat_now_ns();
    if (ph_on) {
        if (ph_t && (k == PK_DRAW || k == PK_MESH)) ph_mark(PH_REST);
        ph_t = 0;
        __atomic_fetch_add(&ph_ops[ph_worker][k], 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&ph_op_ns[ph_worker][k], t1 - t0, __ATOMIC_RELAXED);
    }
    if (on) perf_replay_op_at(op_is_draw(op) ? PERF_RP_DRAW : PERF_RP_BIND, op, t1 - t0);
}

void vk_exec_phases_frame(uint64_t frame) {
    static uint64_t frames;
    if (ph_on <= 0 || ++frames % PH_EVERY) return;
    static const char *const who[2] = {"the submitting thread", "the replay workers"};
    for (int w = 0; w < 2; w++) {
        uint64_t ns[PH_N], n[PK_N], t[PK_N];
        for (int i = 0; i < PH_N; i++) ns[i] = __atomic_exchange_n(&ph_ns[w][i], 0, __ATOMIC_RELAXED);
        for (int i = 0; i < PK_N; i++)
            n[i] = __atomic_exchange_n(&ph_ops[w][i], 0, __ATOMIC_RELAXED),
            t[i] = __atomic_exchange_n(&ph_op_ns[w][i], 0, __ATOMIC_RELAXED);
        const double d = (double)(n[PK_DRAW] + n[PK_MESH] ? n[PK_DRAW] + n[PK_MESH] : 1), per = 1e6 * PH_EVERY;
#define US(i) ((double)ns[i] / d / 1e3)
#define OPS(i) (double)n[i] / PH_EVERY, n[i] ? (double)t[i] / (double)n[i] / 1e3 : 0.0, (double)t[i] / per
        rt_log("vk_exec: frame %llu: replay phases on %s, per frame over %d frames: draws %.0f (%.2f us each, %.2f ms), "
               "mesh draws %.0f (%.2f us, %.2f ms), dispatches %.0f (%.2f us, %.2f ms), uniform updates %.0f (%.2f us, "
               "%.2f ms), other ops %.0f (%.2f us, %.2f ms); a draw's phases in us: entry %.2f, shader %.2f, pipeline "
               "%.2f, its state %.2f, hooks %.2f, binding %.2f (textures %.2f, buffers %.2f, signature %.2f, sets and "
               "pushes %.2f), buffers and after %.2f, the rest %.2f", (unsigned long long)frame, who[w], PH_EVERY,
               OPS(PK_DRAW), OPS(PK_MESH), OPS(PK_DISPATCH), OPS(PK_UB), OPS(PK_OTHER), US(PH_ENTRY), US(PH_SHADER),
               US(PH_PIPE), US(PH_STATE), US(PH_HOOKS), US(PH_BIND), US(PH_BIND_TEX), US(PH_BIND_BUF), US(PH_BIND_SIG),
               US(PH_BIND_PUSH), US(PH_VBUF), US(PH_REST));
#undef US
#undef OPS
        uint64_t cn[OP_COUNT_], cnn[OP_COUNT_], sum = 0;
        for (int i = 0; i < OP_COUNT_; i++)
            cn[i] = __atomic_exchange_n(&ph_code_ns[w][i], 0, __ATOMIC_RELAXED),
            cnn[i] = __atomic_exchange_n(&ph_code_n[w][i], 0, __ATOMIC_RELAXED), sum += cn[i];
        uint64_t rec = __atomic_exchange_n(&ph_rec_ns[w], 0, __ATOMIC_RELAXED),
                 recn = __atomic_exchange_n(&ph_rec_n[w], 0, __ATOMIC_RELAXED);
        char line[1024];
        int o = 0;
        for (int k = 0; k < 8; k++) {
            int best = -1;
            for (int i = 0; i < OP_COUNT_; i++)
                if (cn[i] && (best < 0 || cn[i] > cn[best])) best = i;
            if (best < 0) break;
            o += snprintf(line + o, sizeof line - (size_t)o, "%s%s %.0f x %.2f us = %.2f ms", k ? ", " : "",
                          nvn_op_name((uint32_t)best), (double)cnn[best] / PH_EVERY,
                          (double)cn[best] / (double)cnn[best] / 1e3, (double)cn[best] / 1e6 / PH_EVERY);
            cn[best] = 0;
            if (o >= (int)sizeof line - 80) break;
        }
        double sg[SEG_N];
        for (int i = 0; i < SEG_N; i++)
            sg[i] = w ? 0 : (double)__atomic_exchange_n(&ph_seg_ns[i], 0, __ATOMIC_RELAXED) / 1e6 / PH_EVERY;
        rt_log("vk_exec: frame %llu: replay ops on %s by code, per frame: all ops %.2f ms; vk_exec_record %.2f ms in %.1f "
               "calls (before the walk %.2f: pins %.2f, event sync %.2f, mirror begin %.2f, uniform snapshot %.2f; the "
               "walk %.2f, of it in groups: peek %.2f, barrier and begin %.2f, front pass %.2f, hand-out %.2f, own chunks "
               "%.2f; the groups' drain to the last barrier %.2f, the mirror's and uniform snapshot's end %.2f, the rest "
               "%.2f); the most: %s", (unsigned long long)frame, who[w], (double)sum / 1e6 / PH_EVERY,
               (double)rec / 1e6 / PH_EVERY, (double)recn / PH_EVERY, sg[SEG_PROLOGUE], sg[SEG_PINS], sg[SEG_EVSYNC],
               sg[SEG_MIRROR_BEGIN], sg[SEG_USNAP_BEGIN], sg[SEG_WALK], sg[SEG_PEEK], sg[SEG_GBEGIN], sg[SEG_FRONT],
               sg[SEG_HANDOUT], sg[SEG_OWN], sg[SEG_DRAIN], sg[SEG_MIRROR_END], sg[SEG_TAIL], o ? line : "none");
    }
}

void vk_exec_sizes(PerfSizes *s) {
    vk_texture_lock();
    uint64_t views = view_of.n, imgs = views_by_image.n;
    vk_texture_unlock();
    plat_mutex_lock(&spare_lock);
    uint64_t sp = (uint64_t)nspare_pool, sc = (uint64_t)nspare_chunk;
    plat_mutex_unlock(&spare_lock);
    perf_size(s, "image views", views, 0, PERF_SIZE_NONE);
    perf_size(s, "images with views", imgs, 0, PERF_SIZE_NONE);
    perf_size(s, "spare descriptor pools", sp, SPARE_POOLS, PERF_SIZE_NONE);
    perf_size(s, "spare transient chunks", sc, SPARE_CHUNKS, sc * CHUNK_BYTES);
    perf_size(s, "held continuations", (uint64_t)vk_exec_held(), 36, PERF_SIZE_NONE);
}

void vk_exec_desc_mem(VkDescMem *d) {
    plat_mutex_lock(&spare_lock);
    d->spare = (uint64_t)nspare_pool;
    plat_mutex_unlock(&spare_lock);
    int64_t live = __atomic_load_n(&npools_live, __ATOMIC_RELAXED);
    d->pools = live > 0 ? (uint64_t)live : 0;
    d->sets_each = REPLAY_POOL_SETS;
    d->tex_sets = PL.dpool ? (uint64_t)PL.nsets : 0;
}
