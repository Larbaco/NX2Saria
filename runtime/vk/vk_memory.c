//
// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "freeze_hold.h"
#include "gfx.h"
#include "host.h"
#include "logq.h"
#include "mem_budget.h"
#include "mem_growth.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_gpa.h"
#include "vk_mem_heap.h"
#include "wb_lazy_choice.h"
#include "wb_merge_spv.h"
#include "wb_diffcopy_spv.h"
#include "wb_listcopy_spv.h"
#include "wb_mergelist_spv.h"
#include "platform/platform.h"

static uint64_t qpc(void) { return plat_now_ns(); }

static double qpc_ms(uint64_t ticks) { return (double)ticks / 1e6; }

enum { POOL_IMPORT = 1, POOL_PHYS, POOL_VIRT };
enum { MS_NONE, MS_ON, MS_REFUSED };

typedef struct { struct PoolRec *phys; uint64_t off; } VPage;

#define FLUSH_RANGES 32
typedef struct { uint64_t lo, hi; } FlushRange;

typedef struct PoolRec {
    const pool_t *key;
    int kind;
    uint32_t flags;
    uint64_t lo, size;
    uint64_t import_lo, import_hi;
    VkBuffer buf;
    VkDeviceMemory mem;
    struct Span *span;
    VmaAllocation alloc;
    VPage *pt;
    uint64_t npages;
    volatile int32_t used;
    uint8_t tex_off;
    uint32_t bound_sub;
    uint64_t synced_gen;
    uint8_t bufbound;
    uint8_t untracked;
    uint8_t mstate;
    uint32_t gpa_rec;
    uint8_t gpa_flip;
    struct GpaMap { uint64_t lo, hi; uint32_t rec; } *vmaps;
    int nvmaps, cvmaps;
    VkBuffer mbuf;
    VmaAllocation malloc_;
    uint32_t acc_sub;
    uint32_t pm_frame;
    uint8_t pm_cur, pm_prev;
    uint32_t wb_sub;
    uint64_t wb_fence;
    uint8_t wb_kept;
    uint32_t retry_sub;
    uint8_t refused_why;
    uint8_t asked_room;
    uint8_t twin;
    uint32_t vd_wrote_sub, vd_use_sub;
    uint64_t vd_wrote_n;
    struct WbSpan { uint64_t lo, hi, fence; } *cws;
    int ncws, ccws;
    uint64_t mgen;
    FlushRange fl[FLUSH_RANGES + 1];
    int fl_n;
    uint64_t fl_calls, fl_sum;
    uint64_t hot_bytes;
    uint64_t hot_up;
    VmaAllocation salloc;
    uint8_t *smap;
    uint64_t sgen;
    uint64_t sborn;
    volatile uint8_t stouched;
    uint32_t stab_i;
    struct SBase { uint64_t page; uint8_t *base; } *sb;
    uint32_t nsb, csb;
    uint32_t *smark;
    uint64_t *tpg;
    uint32_t ntpg, ctpg;
    uint8_t tlisted;
    uint64_t t_over;
    uint64_t t_over_log;
    uint64_t s_in;
    uint64_t st_pages, st_flush, st_wb, st_merge;
    uint32_t st_syncs;
} PoolRec;

static inline uint32_t acc_of(const PoolRec *r) { return __atomic_load_n(&r->acc_sub, __ATOMIC_RELAXED); }
static inline void acc_set(PoolRec *r, uint32_t v) { __atomic_store_n(&r->acc_sub, v, __ATOMIC_RELAXED); }
static inline void used_by(PoolRec *r, uint32_t sub) { __atomic_store_n(&r->vd_use_sub, sub, __ATOMIC_RELAXED); }

static void mirror_forget(PoolRec *r);
static void cpu_span_add(PoolRec *r, uint64_t lo, uint64_t hi, uint64_t fence);
static uint64_t cpu_span_wait(const PoolRec *r, uint64_t lo, uint64_t hi, uint64_t fence);
static int shadow_on;
static int shadow_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size);
static void shadow_touch(PoolRec *r);
static void shadow_forget(PoolRec *r);
static void pool_retiring(PoolRec *r);
static void shadow_table_add(PoolRec *r);
static void pool_retire(void *arg);
static int vk_mem_tex_pools_now(void);
static int pool_fini_idle(void);
static void pool_fini_wait(void);
static void pool_wait_add(PoolRec *r);
static void grow(void **arr, int *cap, size_t elem);

static PlatMutex pool_lock = PLAT_MUTEX_INIT;
static uint64_t pool_gen = 1;
static uint64_t mirror_gen;
static pidx pool_index;
static PoolRec **phys_list;
static int nphys, cap_phys;
static int32_t n_imports, n_import_failures;
static PFN_vkGetMemoryHostPointerPropertiesEXT p_host_props;

static VkBufferUsageFlags pool_usage(const VkCtx *c) {
    VkBufferUsageFlags u = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (c->has_transform_feedback)
        u |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
    if (c->has_conditional_rendering) u |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    if (vk_gpa_wanted()) u |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    return u;
}

static int memory_type(const VkCtx *c, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++)
        if ((bits >> i & 1) && (c->mem_props.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++)
        if (bits >> i & 1) return (int)i;
    return -1;
}

typedef struct Span { uint64_t lo, hi; VkDeviceMemory mem; int32_t refs; uint64_t idle_frame; } Span;
#define SPAN_KEEP 600
static int dmabuf_on;
static int dmabuf_exts;
static uint64_t dmabuf_limit;
static uint64_t span_size = 64ull << 20;
static PlatMutex span_lock = PLAT_MUTEX_INIT;
static Span **spans;
static int nspans, cspans;
static uint32_t dmabuf_type = UINT32_MAX;
static PFN_vkGetMemoryFdPropertiesKHR p_fd_props;
static int64_t sp_live_bytes;
static uint64_t sp_made, sp_made_bytes, sp_ns, sp_max_ns, sp_freed, sp_freed_bytes, sp_fail, sp_fallback;
static uint64_t sp_fail_all;
static volatile uint64_t span_frame;
#ifdef _WIN32
static void dmabuf_close(int fd) { (void)fd; }
#else
#include <unistd.h>
static void dmabuf_close(int fd) { close(fd); }
#endif

void vk_mem_dmabuf_caps(int exts) { dmabuf_exts = exts; }
int vk_mem_dmabuf_on(void) { return dmabuf_on; }

static uint32_t dmabuf_pick_type(VkDevice dev, const VkPhysicalDeviceMemoryProperties *mp, int fd) {
    if (!p_fd_props) p_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vk_dpa(dev, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    uint32_t bits = p_fd_props && p_fd_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) == VK_SUCCESS
                        ? fp.memoryTypeBits : 0;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    dmabuf_type = UINT32_MAX;
    for (int w = 0; w < 3 && dmabuf_type == UINT32_MAX; w++)
        for (uint32_t i = 0; i < mp->memoryTypeCount && dmabuf_type == UINT32_MAX; i++) {
            VkMemoryPropertyFlags f = mp->memoryTypes[i].propertyFlags;
            if ((bits >> i & 1) && (w == 0 ? f == (hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : w == 1 ? f == hv : (f & hv) == hv))
                dmabuf_type = i;
        }
    rt_log("mem: dma-bufs of guest memory may have memory types %#x; type %d taken", bits,
           dmabuf_type == UINT32_MAX ? -1 : (int)dmabuf_type);
    return bits;
}

static uint64_t dmabuf_any;

static int s_learn, s_check;
void vk_mem_dmabuf_probe(VkDevice dev, const VkPhysicalDeviceMemoryProperties *mp, int bda) {
    if (!dmabuf_on) return;
    const char *no = NULL;
    long err = 0;
    uint32_t bits = 0;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    int fd = plat_mem_dmabuf(dmabuf_any, 4096);
    if (fd < 0) {
        no = "udmabuf refused a dma-buf of guest memory", err = errno;
    } else if (bits = dmabuf_pick_type(dev, mp, fd), dmabuf_type == UINT32_MAX) {
        no = "the device offers no host-visible coherent memory type for it";
        dmabuf_close(fd);
    } else {
        VkImportMemoryFdInfoKHR im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd};
        VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &im,
                                        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = bda ? (void *)&fi : (void *)&im,
                                   .allocationSize = 4096, .memoryTypeIndex = dmabuf_type};
        VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                               .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = 4096,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                          (bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0),
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkMemoryRequirements req;
        VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem);
        if (r != VK_SUCCESS) {
            mem = VK_NULL_HANDLE;
            dmabuf_close(fd);
            no = "the device refused to import it", err = r;
        } else if ((r = vkCreateBuffer(dev, &bi, NULL, &buf)) != VK_SUCCESS) {
            buf = VK_NULL_HANDLE;
            no = "no buffer for it", err = r;
        } else if (vkGetBufferMemoryRequirements(dev, buf, &req), !(req.memoryTypeBits >> dmabuf_type & 1)) {
            no = "a buffer cannot be bound to its memory type", err = (long)req.memoryTypeBits;
        } else if ((r = vkBindBufferMemory(dev, buf, mem, 0)) != VK_SUCCESS) {
            no = "binding a buffer to it failed", err = r;
        }
    }
    if (buf) vkDestroyBuffer(dev, buf, NULL);
    if (mem) vkFreeMemory(dev, mem, NULL);
    if (fd >= 0) plat_mem_dmabuf_done(dmabuf_any, 4096);
    if (!no) return;
    dmabuf_on = 0, shadow_on = 1;
    s_learn = rt_knob("SHADOW_TABLE_STORES", "0") == 1;
    s_check = cfg_bool("NX2SARIA_SHADOW_CHECK");
    rt_log("mem: dma-buf imports off: %s (%ld; memory types %#x): CPU-visible pools are host-visible copies of guest "
           "memory (shadow storage), as with NX2SARIA_DMABUF=0", no, err, bits);
}

static void span_fail(const char *what, uint64_t lo, uint64_t hi, long err) {
    sp_fail++;
    if (++sp_fail_all <= 8)
        rt_log("mem: dma-buf of %#llx+%#llx: %s (%ld)%s", (unsigned long long)lo, (unsigned long long)(hi - lo), what, err,
               sp_fail_all == 8 ? "; no more of these lines" : "");
}

static Span *span_create(VkCtx *c, uint64_t lo, uint64_t hi) {
    uint64_t t0 = qpc();
    int fd = plat_mem_dmabuf(lo, hi - lo);
    if (fd < 0) {
        span_fail("udmabuf refused it", lo, hi, errno);
        return NULL;
    }
    if (dmabuf_type == UINT32_MAX) {
        dmabuf_pick_type(c->device, &c->mem_props, fd);
        if (dmabuf_type == UINT32_MAX) {
            dmabuf_close(fd);
            plat_mem_dmabuf_done(lo, hi - lo);
            span_fail("no host-visible coherent memory type", lo, hi, 0);
            dmabuf_type = UINT32_MAX - 1;
            return NULL;
        }
    }
    if (dmabuf_type >= c->mem_props.memoryTypeCount) {
        dmabuf_close(fd);
        plat_mem_dmabuf_done(lo, hi - lo);
        return NULL;
    }
    VkImportMemoryFdInfoKHR im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                  .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd};
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &im,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = vk_gpa_wanted() ? (void *)&fi : (void *)&im, .allocationSize = hi - lo,
                               .memoryTypeIndex = dmabuf_type};
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult res = vkAllocateMemory(c->device, &ai, NULL, &mem);
    Span *s = res == VK_SUCCESS ? calloc(1, sizeof *s) : NULL;
    if (!s) {
        if (res == VK_SUCCESS) vkFreeMemory(c->device, mem, NULL);
        else dmabuf_close(fd);
        plat_mem_dmabuf_done(lo, hi - lo);
        span_fail("the import failed", lo, hi, res);
        return NULL;
    }
    s->lo = lo, s->hi = hi, s->mem = mem;
    if (nspans == cspans) grow((void **)&spans, &cspans, sizeof *spans);
    spans[nspans++] = s;
    uint64_t ns = qpc() - t0;
    sp_made++, sp_made_bytes += hi - lo, sp_ns += ns, sp_live_bytes += (int64_t)(hi - lo);
    if (ns > sp_max_ns) sp_max_ns = ns;
    return s;
}

static Span *span_get(VkCtx *c, uint64_t lo, uint64_t hi, uint64_t al) {
    Span *best = NULL;
    for (int i = 0; i < nspans; i++)
        if (spans[i]->lo <= lo && hi <= spans[i]->hi && (!best || spans[i]->hi - spans[i]->lo > best->hi - best->lo))
            best = spans[i];
    if (!best) {
        uint64_t a = lo / span_size * span_size, b = (hi + span_size - 1) / span_size * span_size;
        for (int i = 0; i < nspans; i++) {
            const Span *s = spans[i];
            if (s->lo < lo && s->hi > a) a = s->hi < lo ? s->hi : lo;
            if (s->hi > hi && s->lo < b) b = s->lo > hi ? s->lo : hi;
        }
        if (al > 1) a = lo - (lo - a) / al * al;
        if (b - a <= dmabuf_limit) best = span_create(c, a, b);
        if (!best && (a != lo || b != hi) && hi - lo <= dmabuf_limit) best = span_create(c, lo, hi);
    }
    if (best) best->refs++;
    return best;
}

static void span_put(Span *s) {
    plat_mutex_lock(&span_lock);
    if (!--s->refs) s->idle_frame = __atomic_load_n(&span_frame, __ATOMIC_RELAXED);
    plat_mutex_unlock(&span_lock);
}

static void span_gc(uint64_t frame, int all) {
    VkCtx *c = vk_ctx();
    if (!c || !dmabuf_on) return;
    Span *gone[64];
    int ng;
    do {
        ng = 0;
        plat_mutex_lock(&span_lock);
        for (int i = 0; i < nspans && ng < 64;)
            if (!spans[i]->refs && (all || frame - spans[i]->idle_frame >= SPAN_KEEP)) {
                gone[ng++] = spans[i];
                sp_freed++, sp_freed_bytes += spans[i]->hi - spans[i]->lo;
                sp_live_bytes -= (int64_t)(spans[i]->hi - spans[i]->lo);
                spans[i] = spans[--nspans];
            } else {
                i++;
            }
        plat_mutex_unlock(&span_lock);
        for (int i = 0; i < ng; i++) {
            vkFreeMemory(c->device, gone[i]->mem, NULL);
            plat_mem_dmabuf_done(gone[i]->lo, gone[i]->hi - gone[i]->lo);
            free(gone[i]);
        }
    } while (ng == 64);
}

static void span_report(const char *when, uint32_t wait_ms) {
    if (!dmabuf_on) return;
    if (!wait_ms) {
        plat_mutex_lock(&span_lock);
    } else {
        uint64_t until = plat_now_ms() + wait_ms;
        while (!plat_mutex_trylock(&span_lock)) {
            if (plat_now_ms() >= until) {
                rt_log("mem: dma-buf spans %s: not written, a span is being made and its lock is held", when);
                return;
            }
            plat_sleep_ms(5);
        }
    }
    int64_t refs = 0;
    for (int i = 0; i < nspans; i++) refs += spans[i]->refs;
    rt_log("mem: dma-buf spans %s: %d live, %.0f MiB, %lld pools in them; since the last line %llu made "
           "(%.0f MiB, %.1f ms, the longest %.1f ms), %llu freed (%.0f MiB), %llu refused, %llu pools left out",
           when, nspans, sp_live_bytes / 1048576.0, (long long)refs, (unsigned long long)sp_made,
           sp_made_bytes / 1048576.0, qpc_ms(sp_ns), qpc_ms(sp_max_ns), (unsigned long long)sp_freed,
           sp_freed_bytes / 1048576.0, (unsigned long long)sp_fail, (unsigned long long)sp_fallback);
    sp_made = sp_made_bytes = sp_ns = sp_max_ns = sp_freed = sp_freed_bytes = sp_fail = sp_fallback = 0;
    plat_mutex_unlock(&span_lock);
}

static void span_log(uint64_t frame) {
    char when[48];
    snprintf(when, sizeof when, "at frame %llu", (unsigned long long)frame);
    span_report(when, 0);
}

static int import_dmabuf(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    r->import_lo = addr & ~0xfffull;
    r->import_hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t n = r->import_hi - r->import_lo;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = n,
                             .usage = pool_usage(c), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult res = vkCreateBuffer(c->device, &bi, NULL, &r->buf);
    if (res != VK_SUCCESS) {
        r->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c->device, r->buf, &req);
    uint64_t need = ((req.size > n ? req.size : n) + 0xfff) & ~0xfffull;
    plat_mutex_lock(&span_lock);
    Span *s = span_get(c, r->import_lo, r->import_lo + need, req.alignment);
    int ok = s && (r->import_lo - s->lo) % (req.alignment ? req.alignment : 1) == 0 && (req.memoryTypeBits >> dmabuf_type & 1);
    if (!s) sp_fallback++;
    plat_mutex_unlock(&span_lock);
    if (ok && (res = vkBindBufferMemory(c->device, r->buf, s->mem, r->import_lo - s->lo)) != VK_SUCCESS) ok = 0;
    if (!ok) {
        if (s) {
            rt_log("vk: pool %#llx+%#llx: binding into its dma-buf span failed (alignment %llu, types %#x, %d)",
                   (unsigned long long)addr, (unsigned long long)size, (unsigned long long)req.alignment,
                   req.memoryTypeBits, (int)res);
            span_put(s);
        }
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        return 0;
    }
    r->span = s;
    plat_inc32(&n_imports);
    return 1;
}

static int import_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    if (shadow_on) return shadow_pool(c, r, addr, size);
    if (dmabuf_on && import_dmabuf(c, r, addr, size)) return 1;
    if (!p_host_props)
        p_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vk_dpa(c->device, "vkGetMemoryHostPointerPropertiesEXT");
    uint64_t al = c->min_imported_host_alignment ? c->min_imported_host_alignment : 0x1000;
    r->import_lo = addr & ~(al - 1);
    r->import_hi = (addr + size + al - 1) & ~(al - 1);
    uint64_t n = r->import_hi - r->import_lo;
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult res = p_host_props ? p_host_props(c->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                               (void *)(uintptr_t)r->import_lo, &hp)
                                : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (res != VK_SUCCESS || !hp.memoryTypeBits) {
        rt_log("vk: pool %#llx+%#llx: host pointer not importable (%d)", (unsigned long long)addr, (unsigned long long)size, (int)res);
        return 0;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = n,
                             .usage = pool_usage(c), .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if ((res = vkCreateBuffer(c->device, &bi, NULL, &r->buf)) != VK_SUCCESS) {
        rt_log("vk: pool %#llx: vkCreateBuffer for the import failed (%d)", (unsigned long long)addr, (int)res);
        return 0;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c->device, r->buf, &req);
    int type = memory_type(c, req.memoryTypeBits & hp.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type < 0 || req.size > n) {
        rt_log("vk: pool %#llx: no memory type for the import (buffer %#x, host %#x, size %#llx of %#llx)",
               (unsigned long long)addr, req.memoryTypeBits, hp.memoryTypeBits, (unsigned long long)req.size,
               (unsigned long long)n);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        return 0;
    }
    VkImportMemoryHostPointerInfoEXT im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                           .pHostPointer = (void *)(uintptr_t)r->import_lo};
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &im,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = vk_gpa_wanted() ? (void *)&fi : (void *)&im,
                               .allocationSize = n, .memoryTypeIndex = (uint32_t)type};
    if ((res = vkAllocateMemory(c->device, &ai, NULL, &r->mem)) != VK_SUCCESS ||
        (res = vkBindBufferMemory(c->device, r->buf, r->mem, 0)) != VK_SUCCESS) {
        rt_log("vk: pool %#llx+%#llx: host memory import failed (%d)", (unsigned long long)addr, (unsigned long long)size, (int)res);
        if (r->mem) vkFreeMemory(c->device, r->mem, NULL);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        r->mem = VK_NULL_HANDLE;
        return 0;
    }
    plat_inc32(&n_imports);
    return 1;
}

static int alloc_physical(VkCtx *c, PoolRec *r) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = r->size, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult res = vkCreateBuffer(c->device, &bi, NULL, &r->buf);
    if (res != VK_SUCCESS) { rt_log("vk: physical pool: vkCreateBuffer failed (%d)", (int)res); return 0; }
    VkMemoryRequirements rb;
    vkGetBufferMemoryRequirements(c->device, r->buf, &rb);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {256, 256, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkDeviceImageMemoryRequirements di = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &ii};
    VkMemoryRequirements2 ri = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(c->device, &di, &ri);
    uint32_t bits = rb.memoryTypeBits & ri.memoryRequirements.memoryTypeBits;
    if (!bits) bits = rb.memoryTypeBits;
    VkDeviceSize align = NVN_VPOOL_PAGE;
    if (rb.alignment > align) align = rb.alignment;
    if (ri.memoryRequirements.alignment > align) align = ri.memoryRequirements.alignment;
    VkMemoryRequirements req = {.size = (rb.size + align - 1) & ~(align - 1), .alignment = align, .memoryTypeBits = bits};
    VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                   .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, .priority = 1.0f};
    res = vmaAllocateMemory(c->vma, &req, &aci, &r->alloc, NULL);
    if (res != VK_SUCCESS) {
        VmaAllocationCreateInfo any = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                       .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        r->alloc = NULL;
        res = vmaAllocateMemory(c->vma, &req, &any, &r->alloc, NULL);
        if (res == VK_SUCCESS)
            rt_log("vk: physical pool of %#llx bytes: no video memory left, placed in system memory",
                   (unsigned long long)r->size);
    }
    if (res != VK_SUCCESS || (res = vmaBindBufferMemory(c->vma, r->alloc, r->buf)) != VK_SUCCESS) {
        rt_log("vk: physical pool of %#llx bytes: device-local allocation failed (%d)", (unsigned long long)r->size, (int)res);
        if (r->alloc) vmaFreeMemory(c->vma, r->alloc);
        vkDestroyBuffer(c->device, r->buf, NULL);
        r->buf = VK_NULL_HANDLE;
        r->alloc = NULL;
        return 0;
    }
    if (vk_fix_zero()) {
        VkCommandBuffer cb = vk_upload_begin();
        vkCmdFillBuffer(cb, r->buf, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
        vk_upload_end();
    }
    return 1;
}

int vk_fix_zero(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_FIX_ZERO");
    }
    return on;
}

static void index_put(PoolRec *r) {
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    pidx_ent e = {.lo = r->lo, .hi = r->lo + r->size, .base = r->lo, .buf = r, .gpu = r->lo, .key = r->key};
    pidx_remove(&pool_index, r->key, NULL);
    if (!pidx_insert(&pool_index, &e)) rt_fatal("vk: out of memory for the pool index");
}

static PoolRec *phys_find(const pool_t *key) {
    for (int i = 0; i < nphys; i++) if (phys_list[i]->key == key) return phys_list[i];
    return NULL;
}

static void destroy_rec(VkCtx *c, PoolRec *r) {
    if (r->mbuf) vmaDestroyBuffer(c->vma, r->mbuf, r->malloc_);
    if (r->salloc) vmaDestroyBuffer(c->vma, r->buf, r->salloc), r->buf = VK_NULL_HANDLE;
    if (r->buf) vkDestroyBuffer(c->device, r->buf, NULL);
    if (r->mem) vkFreeMemory(c->device, r->mem, NULL);
    if (r->span) span_put(r->span);
    if (r->alloc) vmaFreeMemory(c->vma, r->alloc);
    for (uint32_t i = 0; i < r->csb; i++) free(r->sb[i].base);
    free(r->sb);
    free(r->cws);
    free(r->pt);
    free(r);
}

static int pool_init(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) { rt_log("vk: MemoryPoolInitialize before vk_init"); return 0; }
    if ((p->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS) && !(p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) return 1;
    if (!p->size) return 1;
    PoolRec *r = calloc(1, sizeof *r);
    if (!r) rt_fatal("vk: out of memory for a pool record");
    r->key = p;
    r->flags = p->flags;
    r->lo = (uintptr_t)p->mem;
    r->size = p->size;
    int ok;
    if (p->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        r->kind = POOL_VIRT;
        r->npages = p->size / NVN_VPOOL_PAGE;
        r->pt = calloc(r->npages ? r->npages : 1, sizeof *r->pt);
        ok = r->pt != NULL;
    } else if (p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) {
        r->kind = POOL_PHYS;
        ok = alloc_physical(c, r);
    } else {
        r->kind = POOL_IMPORT;
        mem_thaw((uintptr_t)p->mem, p->size);
        ok = import_pool(c, r, (uintptr_t)p->mem, p->size);
        if (!ok && vk_mem_tex_pools_now()) ok = import_pool(c, r, (uintptr_t)p->mem, p->size);
        if (!ok && vk_texture_host_room(p->size)) ok = import_pool(c, r, (uintptr_t)p->mem, p->size);
        if (!ok) {
            long n = plat_inc32(&n_import_failures);
            if (n <= 8) rt_log("vk: pool %p (flags %#x, %#llx bytes) has no GPU buffer (%ld so far)", (void *)p->mem,
                               p->flags, (unsigned long long)p->size, n);
            r->buf = VK_NULL_HANDLE;
            ok = 1;
        }
    }
    if (!ok) { free(r->pt); free(r); return 0; }
    plat_mutex_lock(&pool_lock);
    index_put(r);
    if (r->kind == POOL_PHYS) {
        if (nphys == cap_phys) {
            cap_phys = cap_phys ? cap_phys * 2 : 16;
            phys_list = realloc(phys_list, (size_t)cap_phys * sizeof *phys_list);
            if (!phys_list) rt_fatal("vk: out of memory for the physical pool list");
        }
        phys_list[nphys++] = r;
    }
    plat_mutex_unlock(&pool_lock);
    if (r->kind == POOL_IMPORT && (r->mem || r->span || r->smap) && r->buf && r->import_hi > r->import_lo) {
        r->gpa_rec = vk_gpa_map(r->import_lo, r->import_hi, r->buf, r->import_lo);
        if (r->smap && r->gpa_rec) shadow_table_add(r);
    }
    return 1;
}

static uint32_t gpa_cover(uint64_t a) {
    uint32_t rec = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = pidx_upper(&pool_index, a) - 1; i >= 0 && !rec; i--) {
        const PoolRec *o = pool_index.e[i].buf;
        if (o->kind == POOL_IMPORT && o->gpa_rec && o->import_lo <= a && o->import_hi >= a + 4096) rec = o->gpa_rec;
        if (i && pool_index.maxhi[i - 1] <= a) break;
    }
    plat_mutex_unlock_shared(&pool_lock);
    return rec;
}

static void pool_fini(pool_t *p) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    pidx_ent e;
    plat_mutex_lock(&pool_lock);
    int had = pidx_remove(&pool_index, p, &e);
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    PoolRec *r = had ? e.buf : NULL;
    if (r && r->mstate == MS_ON) mirror_forget(r);
    if (r && r->kind == POOL_PHYS) {
        for (int i = 0; i < nphys; i++)
            if (phys_list[i] == r) { phys_list[i] = phys_list[--nphys]; break; }
        for (int i = 0; i < pool_index.n; i++) {
            PoolRec *v = pool_index.e[i].buf;
            if (v->kind != POOL_VIRT) continue;
            for (uint64_t k = 0; k < v->npages; k++)
                if (v->pt[k].phys == r) {
                    v->pt[k].phys = NULL;
                    vk_gpa_clear(v->lo + k * NVN_VPOOL_PAGE, v->lo + (k + 1) * NVN_VPOOL_PAGE);
                }
        }
    }
    plat_mutex_unlock(&pool_lock);
    if (!r) return;
    if (r->smap) shadow_forget(r);
    if (r->gpa_rec) vk_gpa_unmap(r->import_lo, r->import_hi, r->gpa_rec, gpa_cover);
    for (int i = 0; i < r->nvmaps; i++) vk_gpa_unmap(r->vmaps[i].lo, r->vmaps[i].hi, r->vmaps[i].rec, NULL);
    free(r->vmaps), r->vmaps = NULL, r->nvmaps = 0;
    if (r->kind == POOL_IMPORT) mem_thaw(r->lo, r->size);
    vk_texture_range_gone(r->lo, r->size, r->alloc);
    int narrow = 0;
    if (r->used && r->kind != POOL_VIRT) {
        if (pool_fini_idle()) {
            vk_upload_flush(1);
            vk_queue_lock();
            vkQueueWaitIdle(c->gfx_queue);
            vk_queue_unlock();
        } else {
            narrow = 1;
            pool_fini_wait();
        }
    }
    vk_mem_collect();
    if (narrow) {
        pool_retiring(r);
        pool_wait_add(r);
        return;
    }
    if (r->mbuf) pool_retiring(r), vk_completion_add(vk_sync_issued() + 1, pool_retire, r);
    else if (r->gpa_rec && !r->used) pool_retiring(r), vk_completion_add(vk_sync_issued() + 1, pool_retire, r);
    else if (r->kind == POOL_PHYS && !r->used) pool_retiring(r), vk_completion_add(vk_sync_issued() + 1, pool_retire, r);
    else destroy_rec(c, r);
}

int vk_pool_init(pool_t *p) {
    uint64_t t0 = perf_mark_begin();
    int r = pool_init(p);
    perf_mark_end(PERF_M_POOL, t0, p->size);
    return r;
}

void vk_pool_fini(pool_t *p) {
    uint64_t t0 = perf_mark_begin(), size = p->size;
    pool_fini(p);
    perf_mark_end(PERF_M_POOL, t0, size);
}

static void gpa_map_virtual(PoolRec *v, uint64_t lo, uint64_t hi, PoolRec *ph, uint64_t poff) {
    if (!vk_gpa_wanted()) return;
    for (int i = 0; i < v->nvmaps;)
        if (v->vmaps[i].lo >= lo && v->vmaps[i].hi <= hi) {
            vk_gpa_unmap(v->vmaps[i].lo, v->vmaps[i].hi, v->vmaps[i].rec, NULL);
            v->vmaps[i] = v->vmaps[--v->nvmaps];
        } else {
            i++;
        }
    vk_gpa_clear(lo, hi);
    if (!ph || !ph->buf) return;
    uint32_t rec = vk_gpa_map(lo, hi, ph->buf, lo - poff);
    if (!rec) return;
    if (v->nvmaps == v->cvmaps) grow((void **)&v->vmaps, &v->cvmaps, sizeof *v->vmaps);
    v->vmaps[v->nvmaps++] = (struct GpaMap){lo, hi, rec};
}

int vk_pool_map_virtual(pool_t *v, int n, const void *requests) {
    const nvn_mapping_t *rq = requests;
    plat_mutex_lock(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)v->mem, 0);
    PoolRec *r = e && e->key == v ? e->buf : NULL;
    int ok = r && r->kind == POOL_VIRT;
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    for (int i = 0; ok && i < n; i++) {
        PoolRec *ph = rq[i].physical_pool ? phys_find((const pool_t *)(const void *)rq[i].physical_pool) : NULL;
        if (rq[i].physical_pool && !ph) {
            rt_log("vk: MapVirtual request %d names physical pool %p that has no Vulkan memory", i,
                   (const void *)rq[i].physical_pool);
            ok = 0;
            break;
        }
        uint64_t first = (uint64_t)rq[i].virtual_offset / NVN_VPOOL_PAGE, count = rq[i].size / NVN_VPOOL_PAGE;
        if (first + count > r->npages) { ok = 0; break; }
        for (uint64_t k = 0; k < count; k++)
            r->pt[first + k] = (VPage){ph, ph ? (uint64_t)rq[i].physical_offset + k * NVN_VPOOL_PAGE : 0};
        gpa_map_virtual(r, r->lo + first * NVN_VPOOL_PAGE, r->lo + (first + count) * NVN_VPOOL_PAGE, ph,
                        (uint64_t)rq[i].physical_offset);
    }
    plat_mutex_unlock(&pool_lock);
    if (!ok) rt_log("vk: MapVirtual on pool %p failed", (void *)v->mem);
    return ok;
}

static int rec_class(const PoolRec *r) {
    if (r->kind != POOL_IMPORT) return VK_MCLS_DEVICE;
    return r->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS ? VK_MCLS_HOST_NOACC
         : r->flags & NVN_MEMORY_POOL_FLAGS_CPU_CACHED ? VK_MCLS_HOST_CACHED : VK_MCLS_HOST_UNC;
}

typedef struct { uint64_t gen, lo, hi, base, end; VkBuffer buf; VmaAllocation alloc; int cls; void *pool; } ResolveHit;
#define RHIT_SLOTS 512
static _Thread_local ResolveHit rhit[RHIT_SLOTS];

static ResolveHit *rhit_slot(uint64_t addr) { return &rhit[(addr >> 12 ^ addr >> 21) & (RHIT_SLOTS - 1)]; }

static int resolve_check_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("NX2SARIA_RESOLVE_CHECK");
        on = e && *e && strcmp(e, "0");
    }
    return on;
}

static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out, int touch);

static _Thread_local int resolve_off;
static int tex_pool_back(uint64_t addr);

static int mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out, int touch) {
    memset(out, 0, sizeof *out);
    uint64_t g = __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE), end = addr + size;
    const ResolveHit *h = rhit_slot(addr);
    if (end >= addr && h->gen == g && addr >= h->lo && end <= h->hi) {
        out->buffer = h->buf;
        out->offset = addr - h->base;
        out->avail = h->end - addr;
        out->alloc = h->alloc;
        out->cls = h->cls;
        out->pool = h->pool;
        if (h->alloc) out->alloc_offset = out->offset;
        if (shadow_on && touch && h->pool && !((PoolRec *)h->pool)->stouched) shadow_touch(h->pool);
        if (resolve_check_on()) {
            VkMemSpan s;
            if (!resolve_locked(addr, size, &s, touch) || memcmp(&s, out, sizeof s))
                rt_fatal("vk: resolve cache of 0x%llx differs from the pool index", (unsigned long long)addr);
        }
        return 1;
    }
    int ok = resolve_locked(addr, size, out, touch);
    if (!ok && resolve_off && tex_pool_back(addr)) ok = resolve_locked(addr, size, out, touch);
    return ok;
}

int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out) { return mem_resolve(addr, size, out, 1); }
int vk_mem_lookup(uint64_t addr, uint64_t size, VkMemSpan *out) { return mem_resolve(addr, size, out, 0); }

static int resolve_locked(uint64_t addr, uint64_t size, VkMemSpan *out, int touch) {
    memset(out, 0, sizeof *out);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, size);
    PoolRec *r = e ? e->buf : NULL;
    int ok = 0;
    resolve_off = r && r->kind == POOL_IMPORT && r->tex_off;
    if (r && (r->kind == POOL_IMPORT ? r->buf != VK_NULL_HANDLE : r->kind == POOL_PHYS)) {
        int i = (int)(e - pool_index.e);
        if ((i == 0 || pool_index.maxhi[i - 1] <= e->lo) && (i + 1 == pool_index.n || pool_index.e[i + 1].lo >= e->hi)) {
            ResolveHit *h = rhit_slot(addr);
            h->gen = __atomic_load_n(&pool_gen, __ATOMIC_RELAXED);
            h->lo = e->lo, h->hi = e->hi, h->buf = r->buf, h->cls = rec_class(r);
            if (r->kind == POOL_IMPORT) h->base = r->import_lo, h->end = r->import_hi, h->alloc = NULL, h->pool = r;
            else h->base = r->lo, h->end = r->lo + r->size, h->alloc = r->alloc, h->pool = NULL;
        }
    }
    if (r && r->kind == POOL_IMPORT && r->buf) {
        out->cls = rec_class(r);
        out->pool = r;
        out->buffer = r->buf;
        out->offset = addr - r->import_lo;
        out->avail = r->import_hi - addr;
        if (r->smap && touch && !r->stouched) shadow_touch(r);
        ok = 1;
    } else if (r && r->kind == POOL_PHYS) {
        out->buffer = r->buf;
        out->offset = out->alloc_offset = addr - r->lo;
        out->avail = r->size - out->offset;
        out->alloc = r->alloc;
        out->cls = VK_MCLS_DEVICE;
        ok = 1;
    } else if (r && r->kind == POOL_VIRT) {
        uint64_t rel = addr - r->lo, k = rel / NVN_VPOOL_PAGE;
        if (k < r->npages && r->pt[k].phys) {
            PoolRec *ph = r->pt[k].phys;
            uint64_t off = r->pt[k].off + rel % NVN_VPOOL_PAGE, end = r->pt[k].off + NVN_VPOOL_PAGE;
            for (uint64_t j = k + 1; j < r->npages && r->pt[j].phys == ph && r->pt[j].off == end; j++) end += NVN_VPOOL_PAGE;
            out->buffer = ph->buf;
            out->offset = out->alloc_offset = off;
            out->avail = end - off;
            out->alloc = ph->alloc;
            out->cls = VK_MCLS_DEVICE;
            ok = out->avail >= size;
            if (ok && !ph->used) plat_xchg32(&ph->used, 1);
        }
    }
    if (ok && !r->used) plat_xchg32(&r->used, 1);
    plat_mutex_unlock_shared(&pool_lock);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

uint64_t vk_mem_gen(void) {
    return __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE) + __atomic_load_n(&mirror_gen, __ATOMIC_ACQUIRE);
}

int vk_mem_class_of(uint64_t addr) {
    VkMemSpan m;
    return vk_mem_lookup(addr, 1, &m) ? m.cls : VK_MCLS_NONE;
}

static volatile int64_t traffic[VK_MCLS_N][VK_MUSE_N];

void vk_mem_count(int cls, int use, uint64_t bytes) {
    if ((unsigned)cls < VK_MCLS_N && (unsigned)use < VK_MUSE_N)
        __atomic_store_n(&traffic[cls][use], __atomic_load_n(&traffic[cls][use], __ATOMIC_RELAXED) + (int64_t)bytes,
                         __ATOMIC_RELAXED);
}

static void flag_names(uint32_t f, char *buf, size_t n) {
    static const char *const names[] = {"CPU_NO_ACCESS", "CPU_UNCACHED", "CPU_CACHED", "GPU_NO_ACCESS", "GPU_UNCACHED",
                                        "GPU_CACHED", "SHADER_CODE", "COMPRESSIBLE", "PHYSICAL", "VIRTUAL"};
    size_t k = 0;
    buf[0] = 0;
    for (int i = 0; i < 10; i++)
        if (f >> i & 1) k += (size_t)snprintf(buf + k, k < n ? n - k : 0, "%s%s", k ? "|" : "", names[i]);
    if (!k) snprintf(buf, n, "none");
}

static void dirty_frame(uint64_t frame);

static void mirror_log(uint64_t frame);
static void mirror_pool_log(uint64_t frame);
static void shadow_log(uint64_t frame);
static int shadow_check_on(void);
static int dead_pending(void);

static void vram_log(uint64_t frame);
#define HOLES_PERIOD 3600
static void holes_log(uint64_t frame);
static void owner_log(uint64_t frame);
static void compact_look(void);
static void outside_watch(uint64_t frame);
static void up_ts_log(uint64_t frame);
static void slow_mem_window(uint64_t frame);

static volatile uint64_t mirror_frame;

void vk_mem_snapshot(uint64_t frame) {
    if (rt_trace & RT_TRACE_PERF) vram_log(frame);
}

void vk_mem_census(uint64_t frame) {
    mirror_frame = frame;
    __atomic_store_n(&span_frame, frame, __ATOMIC_RELAXED);
    if (dmabuf_on && frame % 60 == 0) span_gc(frame, 0);
    static uint64_t window_start;
    uint64_t r0 = qpc(), rv = 0;
    if ((rt_trace & RT_TRACE_PERF) && frame % 600 == 0) vram_log(frame), mirror_pool_log(frame), rv = qpc() - r0;
    if (frame && frame % ((rt_trace & RT_TRACE_PERF) ? 600 : HOLES_PERIOD) == 0) {
        uint64_t h0 = qpc();
        holes_log(frame);
        uint64_t h1 = qpc();
        owner_log(frame);
        uint64_t h2 = qpc();
        rt_log("mem: the report of frame %llu took %.2f ms on the presenting thread: vram lines %.2f ms, holes %.2f ms, "
               "owners and the rest %.2f ms", (unsigned long long)frame, qpc_ms(h2 - r0), qpc_ms(rv), qpc_ms(h1 - h0),
               qpc_ms(h2 - h1));
        perf_report_part(h2 - r0);
    }
    if (vk_texture_compact_due()) compact_look();
    if ((rt_trace & RT_TRACE_PERF) && frame % 60 == 0)
        outside_watch(frame), up_ts_log(frame), vk_gtl_log(frame, 60), slow_mem_window(frame);
    if ((rt_trace & RT_TRACE_PERF) && !vk_mirror_on) dirty_frame(frame);
    if ((rt_trace & RT_TRACE_PERF) ? frame % 60 == 0 : frame == 600 || frame == 1800)
        mirror_log(frame), shadow_log(frame), span_log(frame);
    else if (shadow_check_on() && frame % 60 == 0)
        shadow_log(frame);
    if (frame == 1200) {
        for (int c = 0; c < VK_MCLS_N; c++)
            for (int u = 0; u < VK_MUSE_N; u++) plat_xchg64(&traffic[c][u], 0);
        window_start = frame;
        return;
    }
    if (frame != 600 && (frame % 1800 || !(frame == 1800 || (rt_trace & RT_TRACE_PERF)))) return;
    typedef struct { int kind; uint32_t flags; uint64_t n, bytes; } Group;
    Group g[64];
    int ng = 0;
    uint64_t total[4] = {0}, count[4] = {0};
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        int j = 0;
        while (j < ng && (g[j].kind != r->kind || g[j].flags != r->flags)) j++;
        if (j == ng && ng < 64) g[ng++] = (Group){r->kind, r->flags, 0, 0};
        if (j < ng) g[j].n++, g[j].bytes += r->size;
        total[r->kind] += r->size, count[r->kind]++;
    }
    plat_mutex_unlock_shared(&pool_lock);
    rt_log("mem: census at frame %llu: import %llu pools %.1f MiB (%d without a GPU buffer), physical %llu pools %.1f MiB, "
           "virtual %llu pools %.1f MiB of address space",
           (unsigned long long)frame, (unsigned long long)count[POOL_IMPORT], total[POOL_IMPORT] / 1048576.0,
           n_import_failures, (unsigned long long)count[POOL_PHYS], total[POOL_PHYS] / 1048576.0,
           (unsigned long long)count[POOL_VIRT], total[POOL_VIRT] / 1048576.0);
    static const char *const kinds[] = {"", "import", "physical", "virtual"};
    for (int j = 0; j < ng; j++) {
        char f[160];
        flag_names(g[j].flags, f, sizeof f);
        rt_log("mem:   %s %#x %s: %llu pools, %.1f MiB", kinds[g[j].kind], g[j].flags, f, (unsigned long long)g[j].n,
               g[j].bytes / 1048576.0);
    }
    uint64_t frames = window_start && frame > window_start ? frame - window_start : frame;
    static const char *const cls[] = {"", "host CPU_NO_ACCESS", "host CPU_UNCACHED", "host CPU_CACHED", "device", "scratch"};
    rt_log("mem: traffic per frame over %llu frames, MiB (bound ranges, not bytes read): class: vertex index uniform "
           "storage indirect copy-src gpu-write cpu-read", (unsigned long long)frames);
    for (int c = 1; c < VK_MCLS_N; c++) {
        double v[VK_MUSE_N];
        for (int u = 0; u < VK_MUSE_N; u++) v[u] = plat_xchg64(&traffic[c][u], 0) / 1048576.0 / (double)(frames ? frames : 1);
        rt_log("mem:   %s: %.2f %.2f %.2f %.2f %.3f %.2f %.2f %.3f", cls[c], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    }
    window_start = frame;
}

static uint64_t budget_cap(void) {
    static int64_t mb = -1;
    if (mb < 0) {
        const char *s = cfg_get("NX2SARIA_VRAM_BUDGET_MB");
        int64_t v = s && *s ? atoll(s) : 0;
        if (v > 0) rt_log("mem: the VRAM budget is taken as at most %lld MiB (NX2SARIA_VRAM_BUDGET_MB, a test)", (long long)v);
        mb = v > 0 ? v : 0;
    }
    return (uint64_t)mb << 20;
}

#define BALLAST_PIECE (256ull << 20)
#define BALLAST_MAX 512
static VkBuffer ballast_buf[BALLAST_MAX];
static VmaAllocation ballast_alloc[BALLAST_MAX];
static int nballast;

void vk_mem_ballast(VmaAllocator vma) {
    const char *s = cfg_get("NX2SARIA_VRAM_BALLAST_MB");
    uint64_t want = s && *s && atoll(s) > 0 ? (uint64_t)atoll(s) << 20 : 0, got = 0;
    if (!want) return;
    while (got < want && nballast < BALLAST_MAX) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = want - got < BALLAST_PIECE ? want - got : BALLAST_PIECE,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                      .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        if (vmaCreateBuffer(vma, &bi, &ai, &ballast_buf[nballast], &ballast_alloc[nballast], NULL) != VK_SUCCESS) break;
        got += bi.size, nballast++;
    }
    rt_log("mem: %llu of %llu MiB of video memory taken and held for the session (NX2SARIA_VRAM_BALLAST_MB, a test)",
           (unsigned long long)(got >> 20), (unsigned long long)(want >> 20));
}

void vk_mem_ballast_release(VmaAllocator vma) {
    for (int i = 0; i < nballast; i++) vmaDestroyBuffer(vma, ballast_buf[i], ballast_alloc[i]);
    nballast = 0;
}

static int vram_query(uint64_t *used, uint64_t *budget, int *exact) {
    VkCtx *c = vk_ctx();
    *used = *budget = 0, *exact = 0;
    if (!c) return 0;
    const VkPhysicalDeviceMemoryProperties *mp = &c->mem_props;
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &b};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
        for (uint32_t h = 0; h < p2.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
            if (p2.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                *used += b.heapUsage[h], *budget += b.heapBudget[h];
        *exact = 1;
        return *budget != 0;
    }
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    for (uint32_t h = 0; h < mp->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            *used += vb[h].statistics.blockBytes, *budget += mp->memoryHeaps[h].size;
    return *budget != 0;
}

int vk_mem_vram(uint64_t *used, uint64_t *budget, int *exact) {
    int ok = vram_query(used, budget, exact);
    uint64_t cap = budget_cap();
    if (ok && cap && *budget > cap) *budget = cap;
    return ok;
}

int vk_mem_vram_live(uint64_t *used, uint64_t *budget) {
    int exact;
    if (!vk_mem_vram(used, budget, &exact)) return 0;
    VkCtx *c = vk_ctx();
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t slack = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if ((c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            vb[h].statistics.blockBytes > vb[h].statistics.allocationBytes)
            slack += vb[h].statistics.blockBytes - vb[h].statistics.allocationBytes;
    *used = *used > slack ? *used - slack : 0;
    return 1;
}

#define GEN_CHUNK_SHIFT 28
#define GEN_PAGE_SHIFT 12
#define GEN_CHUNKS (RT_ADDRESS_LIMIT >> GEN_CHUNK_SHIFT)
#define GEN_PER_CHUNK (1u << (GEN_CHUNK_SHIFT - GEN_PAGE_SHIFT))
static uint64_t *gen_chunk[GEN_CHUNKS];
static uint64_t gen_now;
static uint32_t n_gen_chunks;
static PlatMutex gen_lock = PLAT_MUTEX_INIT;
static uint64_t gen_pages[1024];
_Static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "64-bit page generations need lock-free atomics");

static void gen_mark(uint64_t g) {
    uint64_t frames = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    uint64_t guest_start = __atomic_load_n(&rt_guest_start_ns, __ATOMIC_ACQUIRE);
    double secs = guest_start ? (double)(kern_now_ns() - guest_start) / 1e9 : 0;
    rt_log("mem: page generations reached %llu at frame %llu after %.1f h of play (%.0f a frame); before 0.11.0 they "
           "were 32 bits and ran out here, after which textures and pool mirrors could miss the CPU's writes",
           (unsigned long long)g, (unsigned long long)frames, secs / 3600, frames ? (double)g / (double)frames : 0.0);
}

#define GEN_SEED_MAX (1ull << 62)
static uint64_t gen_seed(void) {
    const char *e = cfg_get("NX2SARIA_GEN_SEED");
    if (!e || !*e) return gen_now;
    char *end;
    uint64_t seed = strtoull(e, &end, 0);
    if (end == e || *end || strchr(e, '-')) {
        rt_log("mem: NX2SARIA_GEN_SEED=%s is not a number from 0 to %llu: the page generations start at %llu", e,
               GEN_SEED_MAX, (unsigned long long)gen_now);
        return gen_now;
    }
    int capped = seed > GEN_SEED_MAX;
    if (capped) seed = GEN_SEED_MAX;
    if (seed <= gen_now) return gen_now;
    rt_log("mem: page generations start at %llu (NX2SARIA_GEN_SEED%s, a test switch)", (unsigned long long)seed,
           capped ? " capped at 2^62" : "");
    return seed;
}

static inline uint64_t gen_next(void) {
    uint64_t g = gen_now + 1;
    if (__builtin_expect(g == 1, 0)) g = gen_seed() + 1;
    if (__builtin_expect(g == 1ull << 32, 0)) gen_mark(g);
    __atomic_store_n(&gen_now, g, __ATOMIC_RELAXED);
    return g;
}
#define REG_SHIFT 21
static uint64_t reg_max[RT_ADDRESS_LIMIT >> REG_SHIFT];
static uint64_t *cgen_chunk[GEN_CHUNKS];
static uint64_t creg_max[RT_ADDRESS_LIMIT >> REG_SHIFT];

static uint64_t ww_calls, ww_bytes, ww_ticks, ww_dirty_calls, ww_dirty_ticks, ww_pages;

static volatile int64_t slow_more_n, slow_more_ns, slow_more_max;

static void slow_mem_call(const char *what, uint64_t addr, uint64_t size, uint64_t pages, uint64_t ns) {
    static uint32_t logged;
    if (ns < 4000000ull || !(rt_trace & RT_TRACE_PERF)) return;
    if (__atomic_add_fetch(&logged, 1, __ATOMIC_RELAXED) > 200) {
        plat_add64(&slow_more_n, 1), plat_add64(&slow_more_ns, (int64_t)ns);
        int64_t m = __atomic_load_n(&slow_more_max, __ATOMIC_RELAXED);
        while ((int64_t)ns > m && !__atomic_compare_exchange_n(&slow_more_max, &m, (int64_t)ns, 1, __ATOMIC_RELAXED,
                                                               __ATOMIC_RELAXED)) {}
        return;
    }
    rt_log("mem: slow %s: %.1f ms over %#llx + %.1f MiB, %lld pages written (frame %llu)", what, ns / 1e6,
           (unsigned long long)addr, size / 1048576.0, pages == UINT64_MAX ? -1ll : (long long)pages,
           (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
}

static void slow_mem_window(uint64_t frame) {
    int64_t n = plat_xchg64(&slow_more_n, 0);
    if (!n) return;
    int64_t ns = plat_xchg64(&slow_more_ns, 0), max = plat_xchg64(&slow_more_max, 0);
    rt_log("mem: frame %llu: %lld more slow write-watch queries and freezes (over 4 ms) since the last such line, %.1f ms, "
           "the longest %.1f ms", (unsigned long long)frame, (long long)n, ns / 1e6, max / 1e6);
}

static uint64_t ww_query(uint64_t addr, uint64_t size) {
    uint64_t t0 = qpc();
    uint64_t n = mem_written_pages(addr, size, gen_pages, sizeof gen_pages / sizeof *gen_pages);
    uint64_t dt = qpc() - t0;
    ww_ticks += dt, ww_calls++, ww_bytes += size;
    if (n && n != UINT64_MAX) ww_dirty_calls++, ww_dirty_ticks += dt, ww_pages += n;
    slow_mem_call("write-watch query", addr, size, n, dt);
    return n;
}

#define FREEZE_PER_SUB 32
static int freeze_idle;
static uint32_t ww_ep = 1;
static uint32_t reg_last[RT_ADDRESS_LIMIT >> REG_SHIFT];
static uint32_t reg_tried[RT_ADDRESS_LIMIT >> REG_SHIFT];
static uint8_t reg_queued[RT_ADDRESS_LIMIT >> REG_SHIFT];
static uint32_t *fcand;
static int nfcand, cfcand;
static uint64_t n_frz_refused;

static uint64_t *slot_in(uint64_t **tab, uint64_t page_addr, int create) {
    uint64_t ci = page_addr >> GEN_CHUNK_SHIFT;
    if (ci >= GEN_CHUNKS) return NULL;
    if (!tab[ci]) {
        if (!create) return NULL;
        uint64_t *p = calloc(GEN_PER_CHUNK, sizeof *p);
        if (!p) rt_fatal("vk: out of memory for page generations");
        __atomic_store_n(&tab[ci], p, __ATOMIC_RELEASE);
        __atomic_add_fetch(&n_gen_chunks, 1, __ATOMIC_RELAXED);
    }
    return &tab[ci][(page_addr >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
}

static uint64_t *gen_slot(uint64_t page_addr, int create) { return slot_in(gen_chunk, page_addr, create); }

static void stamp_page_as(uint64_t a, uint64_t g, int back) {
    uint64_t *s = gen_slot(a, 1);
    if (!s) return;
    __atomic_store_n(s, g, __ATOMIC_RELAXED);
    reg_max[a >> REG_SHIFT] = g;
    reg_last[a >> REG_SHIFT] = ww_ep;
    if (back) return;
    uint64_t *c = slot_in(cgen_chunk, a, 1);
    if (c) __atomic_store_n(c, g, __ATOMIC_RELAXED), creg_max[a >> REG_SHIFT] = g;
}

static void stamp_page(uint64_t a, uint64_t g) { stamp_page_as(a, g, 0); }

static void stamp_range(uint64_t lo, uint64_t hi, uint64_t g) {
    for (uint64_t a = lo; a < hi; a += 1ull << GEN_PAGE_SHIFT) stamp_page(a, g);
}

void vk_guest_written(uint64_t addr, uint64_t size) {
    if (!size || nvn_gpu_only(addr)) return;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    plat_mutex_lock(&gen_lock);
    stamp_range(lo, hi, gen_next());
    plat_mutex_unlock(&gen_lock);
}

int vk_mem_changed(uint64_t addr, uint64_t size, uint64_t since, uint64_t *now) {
    if (!size) { *now = since; return 0; }
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    plat_mutex_lock(&gen_lock);
    uint64_t g = gen_next();
    int tracked = 1, frozen = 1;
    for (uint64_t a = lo & ~((1ull << REG_SHIFT) - 1); a < hi && frozen; a += 1ull << REG_SHIFT) frozen = mem_frozen(a);
    while (!frozen) {
        uint64_t n = ww_query(lo, hi - lo);
        if (n == UINT64_MAX) { tracked = 0; break; }
        for (uint64_t k = 0; k < n; k++) stamp_page(gen_pages[k], g);
        if (n < sizeof gen_pages / sizeof *gen_pages) break;
    }
    int changed = 0;
    for (uint64_t a = lo; tracked && a < hi && !changed; a += 1ull << GEN_PAGE_SHIFT) {
        uint64_t *s = gen_slot(a, 0);
        if (s && *s > since) changed = 1;
    }
    plat_mutex_unlock(&gen_lock);
    *now = g;
    return tracked ? changed : -1;
}

#define SNAP_SHIFT 21
#define SNAP_REGIONS (RT_ADDRESS_LIMIT >> SNAP_SHIFT)
static uint32_t snap_at[SNAP_REGIONS];
static uint8_t snap_untracked[SNAP_REGIONS];
static uint64_t n_snap_checks, n_snap_calls, n_snap_untracked;

static uint64_t snap_regions(uint64_t r0, uint64_t r1, uint32_t epoch) {
    uint64_t g = 0;
    uint64_t calls = 0;
    for (uint64_t r = r0; r <= r1;) {
        if (snap_at[r] == epoch) { r++; continue; }
        if (mem_frozen(r << SNAP_SHIFT)) {
            snap_at[r] = epoch, snap_untracked[r] = 0;
            r++;
            continue;
        }
        uint64_t k = r;
        while (k + 1 <= r1 && snap_at[k + 1] != epoch && !mem_frozen((k + 1) << SNAP_SHIFT)) k++;
        uint64_t a = r << SNAP_SHIFT, b = (k + 1) << SNAP_SHIFT;
        int tracked = 1;
        if (!g) g = gen_next();
        for (;;) {
            uint64_t n = ww_query(a, b - a);
            if (n == UINT64_MAX) { tracked = 0; break; }
            for (uint64_t j = 0; j < n; j++) stamp_page(gen_pages[j], g);
            if (n < sizeof gen_pages / sizeof *gen_pages) break;
        }
        calls++;
        for (uint64_t j = r; j <= k; j++) {
            snap_at[j] = epoch, snap_untracked[j] = (uint8_t)!tracked;
            if (tracked && freeze_idle && ww_ep - reg_last[j] >= (uint32_t)freeze_idle &&
                ww_ep - reg_tried[j] >= (uint32_t)freeze_idle && !reg_queued[j]) {
                reg_tried[j] = ww_ep;
                if (nfcand == cfcand) grow((void **)&fcand, &cfcand, sizeof *fcand);
                fcand[nfcand++] = (uint32_t)j;
                reg_queued[j] = 1;
            }
        }
        r = k + 1;
    }
    return calls;
}

static int snap_widen_n(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = cfg_get("NX2SARIA_SNAP_WIDEN");
        n = e && *e ? atoi(e) : 0;
        if (n < 0) n = 0;
        rt_log("mem: a texture check's write-watch query takes up to %d neighbouring 2 MiB regions of its pool on each "
               "side (NX2SARIA_SNAP_WIDEN)", n);
    }
    return n;
}

static void snap_widen(uint64_t lo, uint64_t hi, uint64_t *r0, uint64_t *r1) {
    int w = snap_widen_n();
    if (!w) return;
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)lo, hi - lo);
    const PoolRec *r = e ? e->buf : NULL;
    if (r && r->kind == POOL_IMPORT && r->buf && r->import_hi > r->import_lo) {
        uint64_t p0 = (r->import_lo + (1ull << SNAP_SHIFT) - 1) >> SNAP_SHIFT;
        uint64_t p1 = r->import_hi >> SNAP_SHIFT;
        if (p1 > p0) {
            uint64_t a = *r0 > (uint64_t)w ? *r0 - (uint64_t)w : 0, b = *r1 + (uint64_t)w;
            if (a < p0) a = p0;
            if (b > p1 - 1) b = p1 - 1;
            if (a < *r0) *r0 = a;
            if (b > *r1 && b < SNAP_REGIONS) *r1 = b;
        }
    }
    plat_mutex_unlock_shared(&pool_lock);
}

int vk_mem_changed_snap(uint64_t addr, uint64_t size, uint64_t since, uint32_t epoch, uint64_t *now) {
    if (!size) { *now = since; return 0; }
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    if (!epoch || r1 >= SNAP_REGIONS) return vk_mem_changed(addr, size, since, now);
    int excl = 0;
    plat_mutex_lock_shared(&gen_lock);
    for (uint64_t r = r0; r <= r1; r++)
        if (snap_at[r] != epoch) { excl = 1; break; }
    if (excl) {
        plat_mutex_unlock_shared(&gen_lock);
        uint64_t w0 = r0, w1 = r1;
        snap_widen(lo, hi, &w0, &w1);
        plat_mutex_lock(&gen_lock);
        n_snap_calls += snap_regions(w0, w1, epoch);
    }
    int untracked = 0, changed = 0;
    for (uint64_t r = r0; r <= r1; r++) untracked |= snap_untracked[r];
    for (uint64_t r = r0; !untracked && r <= r1 && !changed; r++) {
        if (reg_max[r] <= since) continue;
        uint64_t a = r << REG_SHIFT > lo ? r << REG_SHIFT : lo, b = (r + 1) << REG_SHIFT < hi ? (r + 1) << REG_SHIFT : hi;
        for (; a < b && !changed; a += 1ull << GEN_PAGE_SHIFT) {
            uint64_t *s = gen_slot(a, 0);
            if (s && *s > since) changed = 1;
        }
    }
    *now = gen_now;
    __atomic_add_fetch(&n_snap_checks, 1, __ATOMIC_RELAXED);
    if (untracked) __atomic_add_fetch(&n_snap_untracked, 1, __ATOMIC_RELAXED);
    if (excl) plat_mutex_unlock(&gen_lock);
    else plat_mutex_unlock_shared(&gen_lock);
    return untracked ? -1 : changed;
}

void vk_mem_gen_lock_shared(void) { plat_mutex_lock_shared(&gen_lock); }
void vk_mem_gen_unlock_shared(void) { plat_mutex_unlock_shared(&gen_lock); }

void vk_mem_gen_lock(void) { plat_mutex_lock(&gen_lock); }
void vk_mem_gen_unlock(void) { plat_mutex_unlock(&gen_lock); }

int vk_mem_unchanged_why_locked(uint64_t addr, uint64_t size, uint64_t since, uint32_t epoch) {
    if (!size || !epoch) return 4;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return 4;
    for (uint64_t r = r0; r <= r1; r++) {
        if (snap_at[r] != epoch) return 1;
        if (snap_untracked[r]) return 2;
        if (reg_max[r] > since) return 3;
    }
    return 0;
}

int vk_mem_unchanged_locked(uint64_t addr, uint64_t size, uint64_t since, uint32_t epoch) {
    return !vk_mem_unchanged_why_locked(addr, size, since, epoch);
}

uint64_t vk_mem_snap_range_locked(uint64_t addr, uint64_t size, uint32_t epoch) {
    if (!size || !epoch) return 0;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return 0;
    for (uint64_t r = r0; r <= r1; r++)
        if (snap_at[r] != epoch) {
            uint64_t q = snap_regions(r0, r1, epoch);
            n_snap_calls += q;
            return q;
        }
    return 0;
}

static uint32_t snap_noted[SNAP_REGIONS];
static uint32_t snap_pass = 1;
static uint32_t *snap_want;
static int nsnap_want, csnap_want;

void vk_mem_snap_note_locked(uint64_t addr, uint64_t size, uint32_t epoch) {
    if (!size || !epoch) return;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return;
    for (uint64_t r = r0; r <= r1; r++) {
        if (snap_at[r] == epoch || snap_noted[r] == snap_pass) continue;
        snap_noted[r] = snap_pass;
        if (nsnap_want == csnap_want) grow((void **)&snap_want, &csnap_want, sizeof *snap_want);
        snap_want[nsnap_want++] = (uint32_t)r;
    }
}

static int snap_want_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

uint64_t vk_mem_snap_noted_locked(uint32_t epoch, int *left) {
    uint64_t calls = 0;
    *left = 0;
    if (nsnap_want > 1) qsort(snap_want, (size_t)nsnap_want, sizeof *snap_want, snap_want_cmp);
    for (int i = 0, k; i < nsnap_want; i = k + 1) {
        for (k = i; k + 1 < nsnap_want && snap_want[k + 1] == snap_want[k] + 1;) k++;
        uint64_t r0 = snap_want[i], r1 = snap_want[k];
        if (r1 > r0 && !mem_watched(r0 << SNAP_SHIFT, (r1 - r0 + 1) << SNAP_SHIFT)) *left = 1;
        else calls += snap_regions(r0, r1, epoch);
    }
    nsnap_want = 0;
    if (!++snap_pass) memset(snap_noted, 0, sizeof snap_noted), snap_pass = 1;
    n_snap_calls += calls;
    return calls;
}

void vk_mem_snap_stats(uint64_t *checks, uint64_t *calls, uint64_t *untracked) {
    *checks = __atomic_exchange_n(&n_snap_checks, 0, __ATOMIC_RELAXED);
    *calls = __atomic_exchange_n(&n_snap_calls, 0, __ATOMIC_RELAXED);
    *untracked = __atomic_exchange_n(&n_snap_untracked, 0, __ATOMIC_RELAXED);
}

#define DIRTY_UNSYNCED UINT64_MAX
#define DIRTY_GAP 0x100000ull
#define DIRTY_WIN 60
enum { DC_CACHED, DC_UNC, DC_OTHER, DC_N };
typedef struct {
    uint64_t eager[DC_N], lazy[DC_N], first[DC_N];
    uint64_t submits, queries, scan_ns, sub_max, frames;
} DirtyAcc;
static uint32_t dirty_sub = 1;
static uint64_t dirty_gen;
static uint64_t first_pending[DC_N], ever_n[DC_N], ever_bytes[DC_N];
static PoolRec **dlist;
static int dcap;
static PlatMutex dirty_lock = PLAT_MUTEX_INIT;
static DirtyAcc dfr, dwin;
static uint32_t dwin_kib[DIRTY_WIN][DC_N];
static uint64_t live_n[DC_N], live_bytes[DC_N], live_untracked, ever_snap_n[DC_N], ever_snap_bytes[DC_N];

static int dc_class(const PoolRec *r) {
    int c = rec_class(r);
    return c == VK_MCLS_HOST_CACHED ? DC_CACHED : c == VK_MCLS_HOST_UNC ? DC_UNC : DC_OTHER;
}

void vk_mem_bound_note(void *pool) {
    PoolRec *r = pool;
    if (r->bound_sub == dirty_sub) return;
    r->bound_sub = dirty_sub;
    if (r->bufbound) return;
    r->bufbound = 1, r->synced_gen = DIRTY_UNSYNCED;
    int k = dc_class(r);
    ever_n[k]++, ever_bytes[k] += r->size, first_pending[k] += r->size;
}

static uint64_t page_lo(const PoolRec *r) { return r->lo & ~0xfffull; }
static uint64_t page_hi(const PoolRec *r) { return (r->lo + r->size + 0xfff) & ~0xfffull; }

static int query_stamp(uint64_t lo, uint64_t hi, uint64_t g) {
    for (;;) {
        uint64_t n = ww_query(lo, hi - lo);
        if (n == UINT64_MAX) return 0;
        for (uint64_t k = 0; k < n; k++) stamp_page(gen_pages[k], g);
        if (n < sizeof gen_pages / sizeof *gen_pages) return 1;
    }
}

#define SB_PAGE 4096ull
static PlatMutex shadow_lock = PLAT_MUTEX_INIT;
static PoolRec **stouch;
static int nstouch, cstouch;
static uint64_t s_sb_pages;
static int64_t s_live_n, s_live_bytes;
static uint64_t s_syncs, s_copied, s_queries, s_untracked, s_wb_bytes, s_ticks;
static uint64_t s_wb_seen, s_wb_ticks;
static uint64_t s_q_ticks;
static int wb_no_reup(void);
static PoolRec **tpools;
static int ntpools, ctpools;
static uint64_t s_tab_pages;
static uint64_t s_tab_over;
static uint64_t s_tab_wb, s_tab_wb_seen, s_tab_wb_ticks, s_tab_wb_jobs;
static uint64_t s_marked, s_mark_ticks;
static uint64_t s_plain_ticks;
static uint64_t s_both_cpu, s_both_gpu;
static uint64_t s_in_merge, s_in_nomirror;
static uint64_t s_tab_ticks, s_tab_syncs;
static PoolRec **stab;
static int nstab, cstab;
static uint32_t stab_want;
static uint64_t s_t_over_all;
static uint64_t s_late_pages, s_late_bytes;
static uint64_t s_stale_pages;

static int shadow_pool(VkCtx *c, PoolRec *r, uint64_t addr, uint64_t size) {
    r->import_lo = addr & ~0xfffull;
    r->import_hi = (addr + size + 0xfff) & ~0xfffull;
    uint64_t n = r->import_hi - r->import_lo;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  .preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(c->vma, &bi, &ai, &r->buf, &r->salloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk: pool %#llx+%#llx: shadow allocation failed (%d)", (unsigned long long)addr, (unsigned long long)size,
               (int)res);
        r->buf = VK_NULL_HANDLE, r->salloc = NULL;
        return 0;
    }
    r->smap = info.pMappedData;
    r->sborn = vk_sync_issued() + 1;
    plat_mutex_lock(&gen_lock);
    r->sgen = gen_now;
    plat_mutex_unlock(&gen_lock);
    memcpy(r->smap, (const void *)(uintptr_t)r->import_lo, n);
    plat_mutex_lock(&shadow_lock);
    s_live_n++, s_live_bytes += (int64_t)n;
    plat_mutex_unlock(&shadow_lock);
    plat_inc32(&n_imports);
    return 1;
}

static void shadow_touch(PoolRec *r) {
    plat_mutex_lock(&shadow_lock);
    if (!r->stouched) {
        if (nstouch == cstouch) grow((void **)&stouch, &cstouch, sizeof *stouch);
        stouch[nstouch++] = r;
        r->stouched = 1;
    }
    plat_mutex_unlock(&shadow_lock);
}

static uint32_t sbase_hash(uint64_t pg) { return (uint32_t)((pg >> 12) * 0x9E3779B97F4A7C15ull >> 32); }

static uint8_t *sbase_find(const PoolRec *r, uint64_t pg) {
    if (!r->nsb) return NULL;
    for (uint32_t i = sbase_hash(pg) & (r->csb - 1);; i = (i + 1) & (r->csb - 1)) {
        if (r->sb[i].page == pg) return r->sb[i].base;
        if (!r->sb[i].page) return NULL;
    }
}

static void sbase_insert(struct SBase *t, uint32_t cap, uint64_t pg, uint8_t *base) {
    uint32_t i = sbase_hash(pg) & (cap - 1);
    while (t[i].page) i = (i + 1) & (cap - 1);
    t[i].page = pg, t[i].base = base;
}

static void sbase_make(PoolRec *r, uint64_t pg) {
    if (sbase_find(r, pg)) return;
    if ((r->nsb + 1) * 4 > r->csb * 3) {
        uint32_t cap = r->csb ? r->csb * 2 : 16;
        struct SBase *t = calloc(cap, sizeof *t);
        if (!t) rt_fatal("vk: out of memory for the shadow's base copies");
        for (uint32_t i = 0; i < r->csb; i++)
            if (r->sb[i].page) sbase_insert(t, cap, r->sb[i].page, r->sb[i].base);
        free(r->sb);
        r->sb = t, r->csb = cap;
    }
    uint8_t *b = malloc(SB_PAGE);
    if (!b) rt_fatal("vk: out of memory for the shadow's base copies");
    memcpy(b, r->smap + (pg - r->import_lo), SB_PAGE);
    sbase_insert(r->sb, r->csb, pg, b);
    r->nsb++;
    s_sb_pages++;
}

static void sbase_free(PoolRec *r) {
    for (uint32_t i = 0; i < r->csb; i++) free(r->sb[i].base);
    free(r->sb);
    s_sb_pages -= r->nsb;
    r->sb = NULL, r->nsb = r->csb = 0;
}

static inline uint64_t byte_mask(uint64_t x) {
    x |= x >> 4, x |= x >> 2, x |= x >> 1;
    return (x & 0x0101010101010101ull) * 0xff;
}

static inline void store_bytes(uint8_t *dst, uint64_t v, uint64_t m) {
    if (m == ~0ull) {
        memcpy(dst, &v, 8);
        return;
    }
    for (int k = 0; k < 8; k++)
        if (m >> (8 * k) & 0xff) dst[k] = (uint8_t)(v >> (8 * k));
}

static uint64_t merge_page(uint8_t *dst, const uint8_t *src, uint8_t *b, uint64_t n) {
    uint64_t moved = 0, i = 0;
    for (; i < n && ((uintptr_t)(src + i) & 7); i++) {
        uint8_t v = ((const volatile uint8_t *)src)[i];
        if (v != b[i]) dst[i] = b[i] = v, moved++;
    }
    for (; i + 8 <= n; i += 8) {
        if (!((uintptr_t)(src + i) & 63) && i + 64 <= n) {
            uint64_t x = 0;
            for (int k = 0; k < 64; k += 8) {
                uint64_t sv, ov;
                memcpy(&sv, src + i + k, 8), memcpy(&ov, b + i + k, 8);
                x |= sv ^ ov;
            }
            if (!x) {
                i += 56;
                continue;
            }
        }
        uint64_t s = *(const volatile uint64_t *)(src + i), o;
        memcpy(&o, b + i, 8);
        uint64_t m = byte_mask(s ^ o);
        if (!m) continue;
        store_bytes(dst + i, s, m);
        memcpy(b + i, &s, 8);
        moved += (uint64_t)__builtin_popcountll(m) / 8;
    }
    for (; i < n; i++) {
        uint8_t v = ((const volatile uint8_t *)src)[i];
        if (v != b[i]) dst[i] = b[i] = v, moved++;
    }
    return moved;
}

static uint32_t page_mark(const uint8_t *p) {
    uint64_t h[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    for (uint64_t i = 0; i < SB_PAGE; i += 32) {
        uint64_t w[4];
        memcpy(w, p + i, 32);
        for (int k = 0; k < 4; k++) {
            h[k] = (h[k] ^ w[k]) * 0xFF51AFD7ED558CCDull;
            h[k] = h[k] << 29 | h[k] >> 35;
        }
    }
    uint64_t x = (h[0] ^ h[1] << 13 ^ h[2] << 27 ^ h[3] << 41 ^ h[1] >> 51 ^ h[2] >> 37 ^ h[3] >> 23) * 0xC4CEB9FE1A85EC53ull;
    uint32_t m = (uint32_t)(x >> 32);
    return m ? m : 1;
}

static void table_page_add(PoolRec *r, uint64_t pg) {
    sbase_make(r, pg);
    if (r->ntpg == r->ctpg) {
        uint32_t cap = r->ctpg ? r->ctpg * 2 : 16;
        uint64_t *t = realloc(r->tpg, cap * sizeof *t);
        if (!t) rt_fatal("vk: out of memory for the shadow's table pages");
        r->tpg = t, r->ctpg = cap;
    }
    r->tpg[r->ntpg++] = pg;
    s_tab_pages++;
    if (r->tlisted) return;
    if (ntpools == ctpools) grow((void **)&tpools, &ctpools, sizeof *tpools);
    tpools[ntpools++] = r;
    r->tlisted = 1;
}

static uint64_t both_changed(const uint8_t *dst, const uint8_t *src, const uint8_t *b, uint64_t n) {
    uint64_t k = 0;
    for (uint64_t i = 0; i < n; i++) k += (src[i] != b[i]) & (dst[i] != b[i]) & (dst[i] != src[i]);
    return k;
}

static void copy_marked(PoolRec *r, uint64_t a, uint64_t e) {
    if (!r->smark) {
        r->smark = calloc((r->import_hi - r->import_lo) / SB_PAGE, sizeof *r->smark);
        if (!r->smark) rt_fatal("vk: out of memory for the shadow's page marks");
    }
    for (uint64_t pg = a; pg < e; pg += SB_PAGE) {
        uint8_t *s = r->smap + (pg - r->import_lo);
        uint32_t *m = &r->smark[(pg - r->import_lo) / SB_PAGE];
        int stored = *m && page_mark(s) != *m;
        memcpy(s, (const void *)(uintptr_t)pg, SB_PAGE);
        if (stored) {
            s_tab_over++, s_t_over_all++, r->t_over++;
            if (s_learn) {
                table_page_add(r, pg);
                continue;
            }
        }
        *m = page_mark(s);
    }
    s_marked += (e - a) / SB_PAGE;
}

static void shadow_forget(PoolRec *r) {
    plat_mutex_lock(&shadow_lock);
    for (int i = 0; i < nstouch; i++)
        if (stouch[i] == r) { stouch[i] = stouch[--nstouch]; break; }
    for (int i = 0; r->tlisted && i < ntpools; i++)
        if (tpools[i] == r) { tpools[i] = tpools[--ntpools]; break; }
    if (r->stab_i) {
        PoolRec *last = stab[--nstab];
        stab[r->stab_i - 1] = last, last->stab_i = r->stab_i;
        r->stab_i = 0;
    }
    s_live_n--, s_live_bytes -= (int64_t)(r->import_hi - r->import_lo);
    s_tab_pages -= r->ntpg;
    free(r->tpg), free(r->smark);
    r->tpg = NULL, r->smark = NULL, r->ntpg = r->ctpg = 0, r->tlisted = 0;
    sbase_free(r);
    plat_mutex_unlock(&shadow_lock);
}

static void shadow_copy(PoolRec *r, uint64_t a, uint64_t b) {
    while (a < b) {
        uint64_t e = b;
        uint8_t *base = NULL;
        if (r->nsb) {
            for (e = a; e < b; e = (e & ~(SB_PAGE - 1)) + SB_PAGE)
                if ((base = sbase_find(r, e & ~(SB_PAGE - 1)))) break;
            if (e > b) e = b;
            if (e == a) {
                uint64_t pg = a & ~(SB_PAGE - 1), pe = pg + SB_PAGE < b ? pg + SB_PAGE : b;
                uint8_t *s = r->smap + (a - r->import_lo);
                if (s_check) s_both_cpu += both_changed(s, (const uint8_t *)(uintptr_t)a, base + (a - pg), pe - a);
                uint64_t n = merge_page(s, (const uint8_t *)(uintptr_t)a, base + (a - pg), pe - a);
                s_copied += n, s_in_merge += n;
                a = pe;
                continue;
            }
        }
        uint64_t t0 = s_check ? qpc() : 0;
        int marks = (s_learn || s_check) && r->gpa_rec && !((a | e) & (SB_PAGE - 1));
        if (marks) copy_marked(r, a, e);
        else memcpy(r->smap + (a - r->import_lo), (const void *)(uintptr_t)a, e - a);
        if (s_check) {
            *(marks ? &s_mark_ticks : &s_plain_ticks) += qpc() - t0;
            if (r->mstate != MS_ON) s_in_nomirror += e - a;
            r->s_in += e - a;
        }
        s_copied += e - a;
        a = e;
    }
}

static void shadow_sync_pool(PoolRec *r, uint32_t epoch, int cpu) {
    uint64_t lo = r->import_lo, hi = r->import_hi;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    int tracked = 1;
    uint64_t t0 = qpc();
    if (epoch && r1 < SNAP_REGIONS) {
        s_queries += snap_regions(r0, r1, epoch);
        for (uint64_t k = r0; k <= r1 && tracked; k++) tracked = !snap_untracked[k];
    } else {
        uint64_t g = gen_next();
        for (uint64_t a = lo; a < hi;) {
            uint64_t b = (a | ((1ull << REG_SHIFT) - 1)) + 1;
            if (b > hi) b = hi;
            if (mem_frozen(a)) { a = b; continue; }
            while (b < hi && !mem_frozen(b)) b = b + (1ull << REG_SHIFT) < hi ? b + (1ull << REG_SHIFT) : hi;
            s_queries++;
            if (!query_stamp(a, b, g)) tracked = 0;
            a = b;
        }
    }
    s_q_ticks += qpc() - t0;
    uint64_t since = r->sgen;
    r->sgen = gen_now;
    if (!tracked) {
        s_untracked++;
        shadow_copy(r, lo, hi);
        return;
    }
    uint64_t run = 0;
    for (uint64_t k = lo >> REG_SHIFT; k <= (hi - 1) >> REG_SHIFT; k++) {
        uint64_t a = k << REG_SHIFT, b = a + (1ull << REG_SHIFT);
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        const uint64_t *ch = (cpu ? cgen_chunk : gen_chunk)[a >> GEN_CHUNK_SHIFT];
        if ((cpu ? creg_max : reg_max)[k] <= since || !ch) {
            if (run) shadow_copy(r, run, a), run = 0;
            continue;
        }
        for (uint64_t x = a; x < b; x += 1ull << GEN_PAGE_SHIFT) {
            int dirty = ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] > since;
            if (dirty && !run) run = x;
            else if (!dirty && run) shadow_copy(r, run, x), run = 0;
        }
    }
    if (run) shadow_copy(r, run, hi);
}

void vk_mem_gpu_pending(uint64_t addr, uint64_t size) {
    if (!shadow_on || !size) return;
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, addr, size);
    PoolRec *r = e ? e->buf : NULL;
    plat_mutex_lock(&shadow_lock);
    if (r && r->smap && addr >= r->import_lo && addr + size <= r->import_hi)
        for (uint64_t pg = addr & ~(SB_PAGE - 1); pg < addr + size; pg += SB_PAGE) sbase_make(r, pg);
    plat_mutex_unlock(&shadow_lock);
    plat_mutex_unlock_shared(&pool_lock);
}

static int wb_reup_off = -1;
static int wb_no_reup(void) {
    if (wb_reup_off < 0) {
        int on = rt_knob("WB_NO_REUP", "1") == 1;
        rt_log("mem: pages a VRAM mirror wrote back %s (NX2SARIA_WB_NO_REUP)",
               on ? "are not copied up into it again" : "are copied up into it again");
        __atomic_store_n(&wb_reup_off, on, __ATOMIC_RELEASE);
    }
    return wb_reup_off;
}

static int shadow_bulk_write(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_SHADOW_BULK_WRITE");
        if (on) rt_log("mem: shadow write-backs clear the write watch's protection of their range in one call "
                       "(NX2SARIA_SHADOW_BULK_WRITE)");
    }
    return on;
}

static int cpu_wrote_since(uint64_t pg, uint64_t sgen) {
    const uint64_t *ch = __atomic_load_n(&cgen_chunk[pg >> GEN_CHUNK_SHIFT], __ATOMIC_ACQUIRE);
    if (ch && __atomic_load_n(&ch[(pg >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)], __ATOMIC_RELAXED) > sgen) return 1;
    uint64_t p[1], n = mem_written_peek(pg, SB_PAGE, p, 1);
    return n && n != UINT64_MAX;
}

static uint64_t shadow_write_back(uint64_t addr, uint64_t size) {
    uint64_t moved = 0;
    int bulk = shadow_bulk_write();
    uint64_t value = vk_sync_cb_value();
    if (!value) value = vk_sync_issued();
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, addr, size);
    PoolRec *r = e ? e->buf : NULL;
    plat_mutex_lock(&shadow_lock);
    if (r && r->smap && addr >= r->import_lo && addr + size <= r->import_hi) {
        uint64_t t0 = qpc();
        mem_thaw(addr, size);
        if (bulk) plat_mem_prepare_write(addr, size);
        for (uint64_t a = addr, end = addr + size; a < end;) {
            uint64_t pg = a & ~(SB_PAGE - 1), pe = pg + SB_PAGE < end ? pg + SB_PAGE : end;
            uint8_t *base = sbase_find(r, pg), *s = r->smap + (a - r->import_lo);
            if (base && s_check) s_both_gpu += both_changed((const uint8_t *)(uintptr_t)a, s, base + (a - pg), pe - a);
            int late = base && s_check && cpu_wrote_since(pg, r->sgen);
            uint64_t n = 0;
            if (base) moved += n = merge_page((uint8_t *)(uintptr_t)a, s, base + (a - pg), pe - a);
            else if (r->sborn > value || cpu_wrote_since(pg, r->sgen)) s_stale_pages++;
            else memcpy((void *)(uintptr_t)a, s, pe - a), moved += pe - a;
            if (late && n) s_late_pages++, s_late_bytes += n;
            a = pe;
        }
        s_wb_bytes += moved, s_wb_seen += size, s_wb_ticks += qpc() - t0;
    }
    plat_mutex_unlock(&shadow_lock);
    plat_mutex_unlock_shared(&pool_lock);
    return moved;
}

static void table_wb_cb(void *arg) {
    (void)arg;
    static uint64_t *wrote;
    static int cwrote;
    int nwrote = 0, bulk = shadow_bulk_write(), back = wb_no_reup();
    uint64_t t0 = qpc(), moved = 0, seen = 0;
    plat_mutex_lock(&shadow_lock);
    for (int i = 0; i < ntpools; i++) {
        PoolRec *r = tpools[i];
        for (uint32_t k = 0; k < r->ntpg; k++) {
            uint64_t pg = r->tpg[k];
            uint8_t *base = sbase_find(r, pg), *s = r->smap + (pg - r->import_lo);
            seen += SB_PAGE;
            if (!base || !memcmp(s, base, SB_PAGE)) continue;
            if (mem_frozen(pg)) mem_thaw(pg, SB_PAGE);
            if (bulk) plat_mem_prepare_write(pg, SB_PAGE);
            if (s_check) s_both_gpu += both_changed((const uint8_t *)(uintptr_t)pg, s, base, SB_PAGE);
            uint64_t n = merge_page((uint8_t *)(uintptr_t)pg, s, base, SB_PAGE);
            if (!n) continue;
            moved += n;
            if (nvn_gpu_only(pg)) continue;
            if (nwrote == cwrote) grow((void **)&wrote, &cwrote, sizeof *wrote);
            wrote[nwrote++] = pg;
        }
    }
    s_tab_wb += moved, s_tab_wb_seen += seen, s_tab_wb_jobs++;
    if (nwrote) {
        plat_mutex_lock(&gen_lock);
        uint64_t g = gen_next();
        for (int i = 0; i < nwrote; i++) stamp_page_as(wrote[i], g, back);
        plat_mutex_unlock(&gen_lock);
    }
    s_tab_wb_ticks += qpc() - t0;
    plat_mutex_unlock(&shadow_lock);
}

static void gpu_written(uint64_t addr, uint64_t size, int back) {
    if (shadow_on && size && !back) shadow_write_back(addr, size);
    if (!size || nvn_gpu_only(addr)) return;
    uint64_t lo = addr & ~0xfffull, hi = (addr + size + 0xfff) & ~0xfffull;
    back = back && wb_no_reup();
    plat_mutex_lock(&gen_lock);
    uint64_t g = gen_next();
    for (uint64_t a = lo; a < hi; a += 1ull << GEN_PAGE_SHIFT) stamp_page_as(a, g, back);
    plat_mutex_unlock(&gen_lock);
}

void vk_gpu_written(uint64_t addr, uint64_t size) { gpu_written(addr, size, 0); }
static int wb_lazy(void);
static int lazy_clean(uint64_t lo, uint64_t hi, void *ctx);

void vk_gpu_written_back(uint64_t addr, uint64_t size) {
    gpu_written(addr, size, 1);
    if (!shadow_on) mem_probe(addr, size);
    VkMemSpan sp;
    if (size && wb_lazy() && vk_mem_lookup(addr, size, &sp) && sp.pool) {
        PoolRec *r = sp.pool;
        plat_mutex_lock_shared(&pool_lock);
        const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, size);
        if (e && e->buf == r && r->mstate == MS_ON && r->mbuf && r->buf && addr >= r->import_lo &&
            addr + size <= r->import_hi)
            mem_lazy_arm(addr, size, (uint64_t)(uintptr_t)r->mbuf, (uint64_t)(uintptr_t)r->buf, addr - r->import_lo, r);
        plat_mutex_unlock_shared(&pool_lock);
    }
}

int vk_mem_mirrored(uint64_t addr, uint64_t size) {
    VkMemSpan sp;
    if (!vk_mirror_on || !size || !vk_mem_lookup(addr, size, &sp) || !sp.pool) return 0;
    const PoolRec *r = sp.pool;
    return r->mstate == MS_ON && addr >= r->import_lo && addr + size <= r->import_hi;
}

#define TABLE_LOG_TOP 6
static void shadow_table_log(uint64_t frame) {
    rt_log("mem: shadow table stores at frame %llu (%s): %llu pages in %d pools keep a base copy; since the last: %llu whole "
           "copy-ins overwrote a store (%llu so far), %llu pages copied in with a mark; written back %.3f MiB of %.1f "
           "compared in %llu fence jobs, %.2f ms",
           (unsigned long long)frame, s_learn ? "NX2SARIA_SHADOW_TABLE_STORES=1: kept from the second copy-in on" : "overwritten",
           (unsigned long long)s_tab_pages, ntpools, (unsigned long long)s_tab_over, (unsigned long long)s_t_over_all,
           (unsigned long long)s_marked, s_tab_wb / 1048576.0, s_tab_wb_seen / 1048576.0, (unsigned long long)s_tab_wb_jobs,
           qpc_ms(s_tab_wb_ticks));
    if (s_check) {
        rt_log("mem: shadow check at frame %llu: copied in whole %.2f ms with marks, %.2f ms without; %.1f MiB by merge, "
               "%.1f MiB whole into pools without a mirror; %llu table syncs %.2f ms; bytes both sides changed: %llu taken "
               "from the CPU at a copy-in, %llu from the GPU at a fence; write-backs into %llu pages the CPU had written "
               "since their copy-in, %llu bytes",
               (unsigned long long)frame, qpc_ms(s_mark_ticks), qpc_ms(s_plain_ticks), s_in_merge / 1048576.0,
               s_in_nomirror / 1048576.0, (unsigned long long)s_tab_syncs, qpc_ms(s_tab_ticks),
               (unsigned long long)s_both_cpu, (unsigned long long)s_both_gpu, (unsigned long long)s_late_pages,
               (unsigned long long)s_late_bytes);
        PoolRec *top[TABLE_LOG_TOP];
        int nt = 0;
        for (int i = 0; i < pool_index.n; i++) {
            PoolRec *r = pool_index.e[i].buf;
            uint64_t d = r->kind == POOL_IMPORT ? r->t_over - r->t_over_log : 0;
            int k = nt;
            while (k > 0 && top[k - 1]->t_over - top[k - 1]->t_over_log < d) k--;
            if (!d || k >= TABLE_LOG_TOP) continue;
            int last = nt < TABLE_LOG_TOP ? nt : TABLE_LOG_TOP - 1;
            memmove(top + k + 1, top + k, (size_t)(last - k) * sizeof *top);
            top[k] = r;
            if (nt < TABLE_LOG_TOP) nt++;
        }
        for (int i = 0; i < nt; i++) {
            char f[160];
            flag_names(top[i]->flags, f, sizeof f);
            rt_log("mem: shadow check:   pool %#llx+%#llx (%s, %s): %llu whole copy-ins overwrote a store (%llu so far), "
                   "%u of its pages keep a base copy for it",
                   (unsigned long long)top[i]->lo, (unsigned long long)top[i]->size, f,
                   top[i]->mstate == MS_ON ? "mirrored" : "no mirror",
                   (unsigned long long)(top[i]->t_over - top[i]->t_over_log), (unsigned long long)top[i]->t_over, top[i]->ntpg);
        }
        struct { uint32_t flags; uint64_t bytes; int n; } cls[16];
        int ncls = 0;
        nt = 0;
        for (int i = 0; i < pool_index.n; i++) {
            PoolRec *r = pool_index.e[i].buf;
            if (r->kind != POOL_IMPORT || !r->s_in) continue;
            int c = 0, k = nt;
            while (c < ncls && cls[c].flags != r->flags) c++;
            if (c == ncls && ncls < 16) cls[ncls++] = (__typeof__(cls[0])){r->flags, 0, 0};
            if (c < ncls) cls[c].bytes += r->s_in, cls[c].n++;
            while (k > 0 && top[k - 1]->s_in < r->s_in) k--;
            if (k >= TABLE_LOG_TOP) continue;
            int last = nt < TABLE_LOG_TOP ? nt : TABLE_LOG_TOP - 1;
            memmove(top + k + 1, top + k, (size_t)(last - k) * sizeof *top);
            top[k] = r;
            if (nt < TABLE_LOG_TOP) nt++;
        }
        for (int i = 0; i < ncls; i++) {
            char f[160];
            flag_names(cls[i].flags, f, sizeof f);
            rt_log("mem: shadow check:   copied in whole into %d pools %s: %.2f MiB", cls[i].n, f, cls[i].bytes / 1048576.0);
        }
        for (int i = 0; i < nt; i++) {
            char f[160];
            flag_names(top[i]->flags, f, sizeof f);
            rt_log("mem: shadow check:   pool %#llx+%#llx (%s, %s, %s): %.2f MiB copied in whole",
                   (unsigned long long)top[i]->lo, (unsigned long long)top[i]->size, f,
                   top[i]->mstate == MS_ON ? "mirrored" : "no mirror", top[i]->used ? "resolved by the GPU" : "never resolved",
                   top[i]->s_in / 1048576.0);
        }
        for (int i = 0; i < pool_index.n; i++) {
            PoolRec *r = pool_index.e[i].buf;
            if (r->kind == POOL_IMPORT) r->t_over_log = r->t_over, r->s_in = 0;
        }
    }
    s_tab_over = s_tab_wb = s_tab_wb_seen = s_tab_wb_ticks = s_tab_wb_jobs = s_marked = s_mark_ticks = s_plain_ticks = 0;
    s_both_cpu = s_both_gpu = s_in_merge = s_in_nomirror = s_tab_ticks = s_tab_syncs = s_late_pages = s_late_bytes = 0;
}

static void shadow_log(uint64_t frame) {
    if (!shadow_on) return;
    if (s_check) plat_mutex_lock_shared(&pool_lock);
    plat_mutex_lock(&shadow_lock);
    rt_log("mem: shadow at frame %llu: %lld pools %.1f MiB; since the last: %llu pool syncs, %llu queries, %.1f MiB "
           "copied in (%llu untracked), %.2f ms (queries %.2f); %.1f MiB written back of %.1f MiB the GPU may have written, %.2f ms; %llu "
           "pages the GPU writes keep a base copy; %llu pages not written back, their pool was made again or the CPU "
           "wrote them since",
           (unsigned long long)frame, (long long)s_live_n, s_live_bytes / 1048576.0, (unsigned long long)s_syncs,
           (unsigned long long)s_queries, s_copied / 1048576.0, (unsigned long long)s_untracked, qpc_ms(s_ticks),
           qpc_ms(s_q_ticks), s_wb_bytes / 1048576.0, s_wb_seen / 1048576.0, qpc_ms(s_wb_ticks), (unsigned long long)s_sb_pages,
           (unsigned long long)s_stale_pages);
    s_syncs = s_queries = s_copied = s_untracked = s_wb_bytes = s_ticks = s_wb_seen = s_wb_ticks = s_q_ticks = 0;
    s_stale_pages = 0;
    if (s_learn || s_check) shadow_table_log(frame);
    plat_mutex_unlock(&shadow_lock);
    if (s_check) plat_mutex_unlock_shared(&pool_lock);
}

void vk_mem_dirty_submit(void) {
    if (!(rt_trace & RT_TRACE_PERF) || vk_mirror_on) return;
    uint64_t t0 = kern_now_ns();
    uint32_t prev = dirty_sub;
    dirty_sub = prev + 1 ? prev + 1 : 1;
    uint64_t eager[DC_N] = {0}, lazy[DC_N] = {0}, ln[DC_N] = {0}, lb[DC_N] = {0}, untracked = 0, queries = 0;
    plat_mutex_lock_shared(&pool_lock);
    if (dcap < pool_index.n) {
        dcap = pool_index.n * 2;
        dlist = realloc(dlist, (size_t)dcap * sizeof *dlist);
        if (!dlist) rt_fatal("vk: out of memory for the dirty census");
    }
    int n = 0;
    for (int i = 0; i < pool_index.n; i++) {
        PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT && r->bufbound) dlist[n++] = r;
    }
    plat_mutex_lock(&gen_lock);
    uint64_t g = gen_next(), since = dirty_gen;
    dirty_gen = g;
    for (int i = 0; i < n;) {
        uint64_t lo = page_lo(dlist[i]), hi = page_hi(dlist[i]);
        int j = i + 1;
        for (; j < n && page_lo(dlist[j]) <= hi + DIRTY_GAP; j++)
            if (page_hi(dlist[j]) > hi) hi = page_hi(dlist[j]);
        queries++;
        int ok = query_stamp(lo, hi, g);
        for (int k = i; k < j; k++) {
            dlist[k]->untracked = 0;
            if (!ok) queries++, dlist[k]->untracked = (uint8_t)!query_stamp(page_lo(dlist[k]), page_hi(dlist[k]), g);
        }
        i = j;
    }
    uint64_t covered = 0;
    for (int i = 0; i < n; i++) {
        PoolRec *r = dlist[i];
        int k = dc_class(r);
        ln[k]++, lb[k] += r->size;
        int fresh = r->synced_gen == DIRTY_UNSYNCED, lz = !fresh && r->bound_sub == prev;
        uint64_t ls = r->synced_gen;
        if (fresh || r->bound_sub == prev) r->synced_gen = g;
        if (r->untracked) { untracked += r->size; continue; }
        if (fresh) continue;
        uint64_t lo = page_lo(r), hi = page_hi(r), ne = 0, nl = 0;
        if (lo < covered) lo = covered;
        if (lo >= hi) continue;
        covered = hi;
        for (uint64_t a = lo; a < hi;) {
            uint64_t ci = a >> GEN_CHUNK_SHIFT, ce = (ci + 1) << GEN_CHUNK_SHIFT;
            if (ce > hi) ce = hi;
            const uint64_t *ch = ci < GEN_CHUNKS ? gen_chunk[ci] : NULL;
            if (ch)
                for (uint64_t x = a; x < ce; x += 1ull << GEN_PAGE_SHIFT) {
                    uint64_t v = ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)];
                    ne += v > since;
                    nl += lz && v > ls;
                }
            a = ce;
        }
        eager[k] += ne << GEN_PAGE_SHIFT, lazy[k] += nl << GEN_PAGE_SHIFT;
    }
    plat_mutex_unlock(&gen_lock);
    plat_mutex_unlock_shared(&pool_lock);
    uint64_t tot = 0;
    for (int k = 0; k < DC_N; k++) tot += eager[k];
    plat_mutex_lock(&dirty_lock);
    for (int k = 0; k < DC_N; k++) {
        dfr.eager[k] += eager[k], dfr.lazy[k] += lazy[k], dfr.first[k] += first_pending[k];
        first_pending[k] = 0;
        live_n[k] = ln[k], live_bytes[k] = lb[k], ever_snap_n[k] = ever_n[k], ever_snap_bytes[k] = ever_bytes[k];
    }
    live_untracked = untracked;
    dfr.submits++, dfr.queries += queries;
    if (tot > dfr.sub_max) dfr.sub_max = tot;
    dfr.scan_ns += kern_now_ns() - t0;
    plat_mutex_unlock(&dirty_lock);
}

static void dirty_frame(uint64_t frame) {
    plat_mutex_lock(&dirty_lock);
    if (dwin.frames < DIRTY_WIN)
        for (int k = 0; k < DC_N; k++) dwin_kib[dwin.frames][k] = (uint32_t)(dfr.eager[k] >> 10);
    for (int k = 0; k < DC_N; k++) dwin.eager[k] += dfr.eager[k], dwin.lazy[k] += dfr.lazy[k], dwin.first[k] += dfr.first[k];
    dwin.submits += dfr.submits, dwin.queries += dfr.queries, dwin.scan_ns += dfr.scan_ns, dwin.frames++;
    if (dfr.sub_max > dwin.sub_max) dwin.sub_max = dfr.sub_max;
    memset(&dfr, 0, sizeof dfr);
    if (frame % DIRTY_WIN) { plat_mutex_unlock(&dirty_lock); return; }
    DirtyAcc w = dwin;
    uint32_t kib[DIRTY_WIN][DC_N];
    memcpy(kib, dwin_kib, sizeof kib);
    uint64_t lv[DC_N], lvb[DC_N], ev[DC_N], evb[DC_N], unt = live_untracked;
    for (int k = 0; k < DC_N; k++) lv[k] = live_n[k], lvb[k] = live_bytes[k], ev[k] = ever_snap_n[k], evb[k] = ever_snap_bytes[k];
    memset(&dwin, 0, sizeof dwin);
    plat_mutex_unlock(&dirty_lock);
    if (!w.frames) return;
    uint64_t fl[2][3][2];
    nvn_flush_stats(fl);
    double f = (double)w.frames, M = 1048576.0;
    uint64_t sub_tot = 0;
    for (int k = 0; k < DC_N; k++) sub_tot += w.eager[k];
#define C3(a, d) (a)[0] / (d), (a)[1] / (d), (a)[2] / (d)
    rt_log("mem: dirty frame %llu: frames %llu submits_per_frame %.2f; per frame, cached/uncached/other: eager_mib "
           "%.3f/%.3f/%.3f lazy_mib %.3f/%.3f/%.3f first_mib %.3f/%.3f/%.3f flush_pool_calls %.1f/%.1f/%.1f "
           "flush_pool_mib %.3f/%.3f/%.3f flush_buf_calls %.1f/%.1f/%.1f flush_buf_mib %.3f/%.3f/%.3f; per submit: "
           "avg_mib %.3f max_mib %.3f; mirror: live_pools %llu/%llu/%llu live_mib %.1f/%.1f/%.1f ever_pools %llu/%llu/%llu "
           "ever_mib %.1f/%.1f/%.1f untracked_mib %.1f; cost: queries_per_frame %.1f scan_ms_per_frame %.3f",
           (unsigned long long)frame, (unsigned long long)w.frames, w.submits / f,
           C3(w.eager, f * M), C3(w.lazy, f * M), C3(w.first, f * M),
           fl[0][0][0] / f, fl[0][1][0] / f, fl[0][2][0] / f, fl[0][0][1] / f / M, fl[0][1][1] / f / M, fl[0][2][1] / f / M,
           fl[1][0][0] / f, fl[1][1][0] / f, fl[1][2][0] / f, fl[1][0][1] / f / M, fl[1][1][1] / f / M, fl[1][2][1] / f / M,
           w.submits ? sub_tot / (double)w.submits / M : 0.0, w.sub_max / M,
           (unsigned long long)lv[0], (unsigned long long)lv[1], (unsigned long long)lv[2], C3(lvb, M),
           (unsigned long long)ev[0], (unsigned long long)ev[1], (unsigned long long)ev[2], C3(evb, M), unt / M,
           w.queries / f, w.scan_ns / 1e6 / f);
#undef C3
    char line[DIRTY_WIN * 36 + 64];
    size_t k = (size_t)snprintf(line, sizeof line, "mem: dirty frames KiB cached/uncached/other:");
    for (uint64_t i = 0; i < w.frames && i < DIRTY_WIN; i++)
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, " %u/%u/%u", kib[i][0], kib[i][1], kib[i][2]);
    rt_log("%s", line);
}

#define VERIFY_BYTES (4u << 20)
#define VERIFY_SAMPLES 64
#define VERIFY_LOG 200
enum { MR_BUDGET, MR_UNTRACKED, MR_OVERLAP, MR_ALLOC, MR_N };
#define RETRY_SUBS 600

int vk_mirror_on;
static int mirror_verify;
static int mirror_gap = 2;
static uint32_t pframe;
static int pplace = 99;

static int presnap_place(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("PRESNAP_PLACE", "1");
    return RT_KNOB_ON(mode);
}

static int sub_place(void) { return vk_sub_in_frame >= 0 && vk_sub_in_frame < 7 ? vk_sub_in_frame : 7; }

static void pm_note(PoolRec *r) {
    if (r->pm_frame != pframe) {
        r->pm_prev = r->pm_frame + 1 == pframe ? r->pm_cur : 0;
        r->pm_cur = 0, r->pm_frame = pframe;
    }
    r->pm_cur |= (uint8_t)(1u << sub_place());
}

static uint8_t pm_last(const PoolRec *r) {
    return r->pm_frame == pframe ? r->pm_prev : r->pm_frame + 1 == pframe ? r->pm_cur : 0;
}
static PoolRec **mlist;
static int nm, mcap;
static uint32_t msub;
static uint32_t mepoch;
typedef struct { PoolRec *r; uint64_t off, size; } MRange;
static MRange *pend, *wbl;
static int npend, cpend, nwbl, cwbl;
static VkBufferCopy *mregions;
static int cregions;
static PlatMutex flush_lock = PLAT_MUTEX_INIT;
static int flush_cap = FLUSH_RANGES;
static int flush_upload;

typedef struct {
    uint64_t subs, syncs, upload_bytes, upload_copies, first_n, first_bytes, wb_bytes, wb_copies, wb_ranges;
    uint64_t presnap_queries, sync_queries, presnap_t, sync_t, prologues, presnap_bytes;
    uint64_t snap_bytes, merge_bytes, merge_dispatches, snap_allocs;
    uint64_t diff_bytes, diff_dispatches;
    uint64_t page_bytes, flush_n, flush_bytes, flush_calls, flush_sum;
    uint64_t flush_skipped;
    uint64_t cap_bytes, cap_ranges, cap_left, cap_t;
} MirrorAcc;
static MirrorAcc mrep;
static MirrorAcc macc;
static PlatMutex mirror_stat_lock = PLAT_MUTEX_INIT;
static volatile int64_t m_live_n, m_live_bytes, m_refused_n[MR_N], m_refused_bytes[MR_N];
static volatile int64_t m_flush_calls, m_flush_bytes, v_pages, v_skipped, v_mismatch;

static void mirror_mode(int discrete, int nvidia_own) {
    const char *e = cfg_value("NX2SARIA_MEMORY");
    int mode = discrete ? 1 : 0;
    const char *why = discrete ? "auto, discrete gpu" : "auto, uma";
    if (strcmp(e, "auto")) {
        if (!strcmp(e, "import")) mode = 0, why = "NX2SARIA_MEMORY=import";
        else if (!strcmp(e, "mirror")) mode = 1, why = "NX2SARIA_MEMORY=mirror";
        else if (!strcmp(e, "verify")) mode = 2, why = "NX2SARIA_MEMORY=verify";
        else rt_log("mem: NX2SARIA_MEMORY=%s is not import, mirror, verify or auto: auto", e);
    }
    vk_mirror_on = mode > 0;
    mirror_verify = mode == 2;
    const char *g = cfg_get("NX2SARIA_MIRROR_GAP");
    mirror_gap = g && *g ? atoi(g) : 2;
    if (mirror_gap < 0) mirror_gap = 0;
    if (rt_knob("FLUSH_SPAN", "0")) {
        flush_cap = 1;
        rt_log("mem: a pool's flushes copy one span from the lowest to the highest (NX2SARIA_FLUSH_SPAN=1)");
    }
    flush_upload = rt_knob("FLUSH_UPLOAD", "0") == 1;
    if (flush_upload) rt_log("mem: flushed ranges are copied into the mirrors as well as written pages (NX2SARIA_FLUSH_UPLOAD=1)");
    const char *fi = cfg_get("NX2SARIA_FREEZE_IDLE");
    freeze_idle = fi && *fi ? atoi(fi) : 1800;
    if (freeze_idle < 0) freeze_idle = 0;
    const char *sh = cfg_get("NX2SARIA_SHADOW");
#ifdef _WIN32
    shadow_on = sh && *sh ? strcmp(sh, "0") != 0 : plat_is_wine();
#else
    shadow_on = sh && *sh ? strcmp(sh, "0") != 0 : !nvidia_own;
    uint64_t lim = 0;
    const char *dmwhy = "";
    static char small[200];
    if (plat_mem_dmabuf_ok(&lim, &dmabuf_any, &dmwhy)) {
        int forced = sh && *sh && strcmp(sh, "0");
        if (lim < (1ull << 30))
            snprintf(small, sizeof small, "udmabuf's size_limit_mb is %llu, below the 1024 a pool of the game may need "
                     "(/sys/module/udmabuf/parameters/size_limit_mb)", (unsigned long long)(lim >> 20));
        dmabuf_on = dmabuf_exts && !forced && lim >= (1ull << 30);
        shadow_on = !dmabuf_on;
        dmabuf_limit = lim;
        const char *mbs = cfg_get("NX2SARIA_DMABUF_MB");
        int mb = mbs && *mbs ? atoi(mbs) : 64;
        span_size = (uint64_t)(mb > 0 ? mb : 64) << 20;
        if (span_size > dmabuf_limit) span_size = dmabuf_limit & ~0xfffull;
        if (span_size < (2ull << 20)) span_size = 2ull << 20;
        if (dmabuf_on)
            rt_log("mem: guest memory is a memfd's: CPU-visible pools are imported as dma-bufs of it, in spans of %llu MiB "
                   "(NX2SARIA_DMABUF_MB; udmabuf's limit %llu MiB)", (unsigned long long)(span_size >> 20),
                   (unsigned long long)(dmabuf_limit >> 20));
        else
            rt_log("mem: guest memory is a memfd's, but its pools get shadow copies: %s",
                   forced ? "NX2SARIA_SHADOW asks for them"
                   : !dmabuf_exts ? "the device cannot import dma-bufs (VK_KHR_external_memory_fd, VK_EXT_external_memory_dma_buf)"
                                  : small);
    } else if (*dmwhy) {
        rt_log("mem: guest memory is not a memfd's (NX2SARIA_DMABUF): %s: CPU-visible pools are %s, as with NX2SARIA_DMABUF=0", dmwhy,
               shadow_on ? "host-visible copies of guest memory (shadow storage)" : "host-pointer imports");
    }
#endif
    if (shadow_on) rt_log("mem: shadow storage: CPU-visible pools are host-visible copies of guest memory, not imports%s",
                          sh && *sh ? " (NX2SARIA_SHADOW)" : "");
    s_learn = shadow_on && rt_knob("SHADOW_TABLE_STORES", "0") == 1;
    s_check = shadow_on && cfg_bool("NX2SARIA_SHADOW_CHECK");
    if (shadow_on)
        rt_log("mem: what shaders store through the device address table into a shadow copy %s "
               "(NX2SARIA_SHADOW_TABLE_STORES)%s",
               s_learn ? "is kept through the copy-ins and written back to guest memory, from the second copy-in of its page on"
                       : "is overwritten by the next whole copy-in of its page",
               s_check ? "; NX2SARIA_SHADOW_CHECK counts the overwrites" : "");
    rt_log("mem: mode %s (%s): %s", mode == 2 ? "verify" : mode ? "mirror" : "import", why,
           mode ? "pools used as buffers get a device-local mirror" : "every CPU-visible pool is imported host memory");
    if (!mode && discrete)
        rt_log("mem: warning: import on a discrete GPU: the GPU reads the game's pools over PCIe on every draw, far "
               "slower than mirror (the auto default on a discrete GPU); use memory = auto unless testing");
}

static void range_add(MRange **a, int *n, int *cap, PoolRec *r, uint64_t off, uint64_t size) {
    if (*n && (*a)[*n - 1].r == r && (*a)[*n - 1].off + (*a)[*n - 1].size == off) {
        (*a)[*n - 1].size += size;
        return;
    }
    if (*n == *cap) grow((void **)a, cap, sizeof **a);
    (*a)[(*n)++] = (MRange){r, off, size};
}

static int range_cmp(const void *x, const void *y) {
    const MRange *a = x, *b = y;
    if (a->r != b->r) return (uintptr_t)a->r < (uintptr_t)b->r ? -1 : 1;
    return a->off < b->off ? -1 : a->off > b->off;
}

static int copy_ranges(VkCommandBuffer cb, MRange *a, int n, int to_mirror, uint64_t *bytes, uint64_t *copies) {
    qsort(a, (size_t)n, sizeof *a, range_cmp);
    int rec = 0;
    for (int i = 0; i < n;) {
        PoolRec *r = a[i].r;
        uint64_t lim = r->import_hi - r->import_lo;
        int k = 0;
        for (; i < n && a[i].r == r; i++) {
            uint64_t lo = a[i].off, hi = a[i].off + a[i].size;
            if (hi > lim) hi = lim;
            if (lo >= hi) continue;
            if (k && lo <= mregions[k - 1].srcOffset + mregions[k - 1].size) {
                uint64_t end = mregions[k - 1].srcOffset + mregions[k - 1].size;
                if (hi > end) mregions[k - 1].size += hi - end;
                continue;
            }
            if (k == cregions) grow((void **)&mregions, &cregions, sizeof *mregions);
            mregions[k++] = (VkBufferCopy){lo, lo, hi - lo};
        }
        if (!k) continue;
        if (to_mirror) vkCmdCopyBuffer(cb, r->buf, r->mbuf, (uint32_t)k, mregions);
        else vkCmdCopyBuffer(cb, r->mbuf, r->buf, (uint32_t)k, mregions);
        rec++;
        for (int j = 0; j < k; j++) *bytes += mregions[j].size;
        *copies += (uint64_t)k;
    }
    return rec;
}

static void mbarrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                     VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                           .dstStageMask = ds, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void refuse(PoolRec *r, int why, const char *what) {
    r->mstate = MS_REFUSED;
    r->retry_sub = why == MR_BUDGET || why == MR_ALLOC ? (msub + RETRY_SUBS ? msub + RETRY_SUBS : 1) : 0;
    r->refused_why = (uint8_t)why;
    uint64_t n = r->import_hi - r->import_lo;
    static volatile int32_t logged[MR_N];
    plat_inc64(&m_refused_n[why]);
    if (plat_inc32(&logged[why]) <= 4) {
        char f[160];
        flag_names(r->flags, f, sizeof f);
        rt_log("mem: pool %#llx+%#llx (%s) stays imported: %s", (unsigned long long)r->lo, (unsigned long long)r->size,
               f, what);
    }
    plat_add64(&m_refused_bytes[why], (int64_t)n);
}

#define MIRROR_IDLE 120
#define MIRROR_SHARE 2
#define RETRY_SOON 16
static int mirror_evict_on = -1;
static int64_t m_evicted_n, m_evicted_bytes, m_retries;
static volatile int64_t m_over_n, m_over_bytes;
static uint64_t room_bytes, room_fence;

static int evict_enabled(void) {
    if (mirror_evict_on < 0) {
        mirror_evict_on = rt_knob("MIRROR_EVICT", "1");
        rt_log("mem: idle mirrors %s (NX2SARIA_MIRROR_EVICT=%d)", mirror_evict_on ? "make room for new ones and for textures under VRAM pressure"
                                                                               : "stay; a pool over the VRAM budget stays imported", mirror_evict_on);
    }
    return mirror_evict_on;
}

static uint64_t evict_room(void) {
    if (room_bytes && vk_sync_fence_done() >= room_fence) room_bytes = 0;
    return room_bytes;
}

static int evict_unlocked_on = -1;

static int evict_unlocked(void) {
    if (evict_unlocked_on < 0) evict_unlocked_on = cfg_bool("NX2SARIA_WB_LAZY_UNLOCKED");
    return evict_unlocked_on;
}

static uint64_t mirror_evict(uint64_t need, uint32_t min_idle, const char *why) {
    enum { MAXV = 64 };
    PoolRec *v[MAXV];
    int nv = 0;
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < nm; i++) {
        PoolRec *r = mlist[i];
        uint32_t age = msub - acc_of(r);
        if (age < min_idle || r->wb_sub == msub || msub - r->wb_sub < 2) continue;
        if (!r->twin && r->gpa_rec) continue;
        if (r->wb_kept) continue;
        int at = nv;
        while (at > 0 && msub - acc_of(v[at - 1]) < age) at--;
        if (at == MAXV) continue;
        if (nv < MAXV) nv++;
        memmove(v + at + 1, v + at, sizeof *v * (size_t)(nv - 1 - at));
        v[at] = r;
    }
    uint64_t can = 0;
    for (int i = 0; i < nv; i++) can += v[i]->import_hi - v[i]->import_lo;
    if (can < need) nv = 0;
    uint64_t rel[MAXV][3], f = 0;
    int nr = 0;
    for (int i = 0; i < nv; i++) {
        PoolRec *r = v[i];
        rel[i][0] = r->import_lo, rel[i][1] = r->import_hi, rel[i][2] = (uint64_t)(uintptr_t)r->mbuf;
        if (f < need) f += r->import_hi - r->import_lo, nr++;
    }
    if (nr && evict_unlocked()) {
        plat_mutex_unlock(&pool_lock);
        mem_lazy_release((const uint64_t (*)[3])rel, nr);
        plat_mutex_lock(&pool_lock);
    }
    uint64_t freed = 0;
    uint32_t idle_for[MAXV];
    VkBuffer dead[MAXV];
    VmaAllocation dead_a[MAXV];
    int n = 0;
    for (int k = 0; k < nv && freed < need; k++) {
        PoolRec *r = v[k];
        int on = 0;
        for (int i = 0; i < nm && !on; i++) on = mlist[i] == r;
        if (!on || r->mbuf != (VkBuffer)(uintptr_t)rel[k][2]) continue;
        idle_for[n] = msub - acc_of(r);
        mem_lazy_release((const uint64_t (*)[3])&rel[k], 1);
        mirror_forget(r);
        r->mstate = MS_NONE, acc_set(r, 0), r->retry_sub = 0;
        dead[n] = r->mbuf, dead_a[n] = r->malloc_;
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        plat_mutex_lock(&flush_lock);
        __atomic_store_n(&r->fl_n, 0, __ATOMIC_RELEASE);
        r->fl_calls = r->fl_sum = 0;
        plat_mutex_unlock(&flush_lock);
        if (r->gpa_rec) r->gpa_rec = vk_gpa_remap_plain(r->import_lo, r->import_hi, r->gpa_rec, r->buf, r->import_lo);
        freed += r->import_hi - r->import_lo;
        if (m_evicted_n + n < 8)
            rt_log("mem: mirror of pool %#llx+%#llx (%.1f MiB) evicted, idle for %u submissions: %s", (unsigned long long)r->lo,
                   (unsigned long long)r->size, (r->import_hi - r->import_lo) / 1048576.0, idle_for[n], why);
        n++;
    }
    plat_mutex_unlock(&pool_lock);
    for (int i = 0; i < n; i++) vk_defer_buffer(dead[i], dead_a[i]);
    if (n) {
        plat_add64(&m_evicted_n, n);
        plat_add64(&m_evicted_bytes, (int64_t)freed);
        room_bytes += freed, room_fence = vk_sync_issued() + 2;
    }
    return freed;
}

static int64_t twin_max = -1;
static int64_t m_untwinned_n, m_untwinned_bytes;

static int twin_wanted(const PoolRec *r) {
    if (twin_max < 0) {
        int kb = cfg_int("NX2SARIA_GPA_TWIN_MAX_KB");
        twin_max = kb > 0 ? (int64_t)kb << 10 : 0;
        if (twin_max)
            rt_log("mem: only mirrors of pools up to %d KiB send the device address table's writes to guest memory "
                   "too (NX2SARIA_GPA_TWIN_MAX_KB); larger ones keep them and are not evicted", kb);
    }
    return !twin_max || (int64_t)(r->import_hi - r->import_lo) <= twin_max;
}

static int n_gpa_flip;
static uint64_t n_gpa_late;

static int gpa_late(void);
int vk_mem_gpa_late(void) { return gpa_late(); }

static int gpa_late(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("GPA_LATE", "1") == 1;
        rt_log("mem: the device address table takes up a new VRAM mirror %s (NX2SARIA_GPA_LATE)",
               on ? "in queue order, after the mirror's first copy" : "at once, as before");
    }
    return on;
}

static void mbarrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                     VkAccessFlags2 da);

static void gpa_flip_cmds(VkCommandBuffer cb) {
    if (!n_gpa_flip) return;
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
             VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < nm && n_gpa_flip; i++) {
        PoolRec *r = mlist[i];
        if (!r->gpa_flip) continue;
        r->gpa_flip = 0, n_gpa_flip--;
        if (r->gpa_rec && r->mbuf && vk_gpa_flip_cmd(cb, r->gpa_rec, r->mbuf, r->import_lo, r->twin)) n_gpa_late++;
    }
    n_gpa_flip = 0;
    plat_mutex_unlock(&pool_lock);
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

static void mirror_create(PoolRec *r) {
    VkCtx *c = vk_ctx();
    uint64_t n = r->import_hi - r->import_lo;
    r->mstate = MS_REFUSED;
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    if (r->kind != POOL_IMPORT || !r->buf || !n) return;
    const uint64_t lo = r->lo;
    plat_mutex_lock_shared(&pool_lock);
    int i = pidx_upper(&pool_index, lo) - 1;
    while (i >= 0 && pool_index.e[i].buf != r) i--;
    int alone = i >= 0 && (i == 0 || pool_index.maxhi[i - 1] <= pool_index.e[i].lo) &&
                (i + 1 == pool_index.n || pool_index.e[i + 1].lo >= pool_index.e[i].hi);
    plat_mutex_unlock_shared(&pool_lock);
    if (i < 0) return;
    if (!alone) return refuse(r, MR_OVERLAP, "it overlaps another pool");
    uint64_t r0 = r->import_lo >> SNAP_SHIFT, r1 = (r->import_hi - 1) >> SNAP_SHIFT;
    if (r1 >= SNAP_REGIONS) return refuse(r, MR_UNTRACKED, "outside the write-watched address space");
    uint64_t used, budget;
    int exact;
    int evict = evict_enabled();
    if (evict ? vk_mem_vram_live(&used, &budget) : vk_mem_vram(&used, &budget, &exact)) {
        if (evict) {
            uint64_t room = evict_room();
            used = used > room ? used - room : 0;
        }
        if (used + n + budget / 8 > budget && evict) {
            char why[96];
            snprintf(why, sizeof why, "room for a %.1f MiB mirror", n / 1048576.0);
            uint64_t f = mirror_evict(used + n + budget / 8 - budget, MIRROR_IDLE, why);
            used = used > f ? used - f : 0;
        }
        if (used + n + budget / 8 > budget) {
            uint64_t over = used + n + budget / 8 - budget;
            if (evict && (uint64_t)m_live_bytes + n <= budget / MIRROR_SHARE) {
                vk_texture_want_room(over);
                plat_inc64(&m_over_n);
                plat_add64(&m_over_bytes, (int64_t)n);
                if (m_over_n <= 4 || !(m_over_n & 255))
                    rt_log("mem: mirror of %.1f MiB made over the VRAM budget (%.0f of %.0f MiB in use, mirrors %.0f MiB): "
                           "%lld such so far, %.0f MiB; idle textures are asked for the room",
                           n / 1048576.0, used / 1048576.0, budget / 1048576.0, m_live_bytes / 1048576.0,
                           (long long)m_over_n, m_over_bytes / 1048576.0);
            } else {
                char why[128];
                snprintf(why, sizeof why, "its %.1f MiB mirror does not fit the VRAM budget (%.0f of %.0f MiB in use)",
                         n / 1048576.0, used / 1048576.0, budget / 1048576.0);
                refuse(r, MR_BUDGET, why);
                if (evict && !r->asked_room) {
                    vk_texture_want_room(over);
                    r->retry_sub = msub + RETRY_SOON ? msub + RETRY_SOON : 1;
                    r->asked_room = 1;
                }
                return;
            }
        }
    }
    plat_mutex_lock(&gen_lock);
    mrep.sync_queries += snap_regions(r0, r1, mepoch);
    int untracked = 0;
    for (uint64_t k = r0; k <= r1; k++) untracked |= snap_untracked[k];
    __atomic_store_n(&r->mgen, gen_now, __ATOMIC_RELEASE);
    plat_mutex_unlock(&gen_lock);
    if (untracked) return refuse(r, MR_UNTRACKED, "its range is not write-watched");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = pool_usage(c),
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, .priority = 1.0f};
    VkResult res = vmaCreateBuffer(c->vma, &bi, &ai, &r->mbuf, &r->malloc_, NULL);
    if (res != VK_SUCCESS) {
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        char why[96];
        snprintf(why, sizeof why, "device-local allocation of %.1f MiB failed (%d)", n / 1048576.0, (int)res);
        return refuse(r, MR_ALLOC, why);
    }
    int twin = twin_wanted(r), late = r->gpa_rec && gpa_late();
    plat_mutex_lock(&pool_lock);
    int k = pidx_upper(&pool_index, lo) - 1;
    while (k >= 0 && pool_index.e[k].buf != r) k--;
    if (k < 0) {
        plat_mutex_unlock(&pool_lock);
        vk_defer_buffer(r->mbuf, r->malloc_);
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        return;
    }
    if (nm == mcap) grow((void **)&mlist, &mcap, sizeof *mlist);
    int at = nm;
    while (at > 0 && mlist[at - 1]->import_lo > r->import_lo) at--;
    memmove(mlist + at + 1, mlist + at, (size_t)(nm - at) * sizeof *mlist);
    mlist[at] = r;
    nm++;
    acc_set(r, msub);
    r->mstate = MS_ON;
    r->asked_room = 0;
    r->twin = (uint8_t)twin;
    if (late) {
        r->gpa_flip = 1, n_gpa_flip++;
    } else if (r->gpa_rec) {
        r->gpa_rec = r->twin ? vk_gpa_remap(r->import_lo, r->import_hi, r->gpa_rec, r->mbuf, r->import_lo)
                             : vk_gpa_remap_plain(r->import_lo, r->import_hi, r->gpa_rec, r->mbuf, r->import_lo);
    }
    plat_mutex_unlock(&pool_lock);
    if (!twin) {
        plat_inc64(&m_untwinned_n);
        plat_add64(&m_untwinned_bytes, (int64_t)n);
    }
    range_add(&pend, &npend, &cpend, r, 0, n);
    mrep.first_n++, mrep.first_bytes += n;
    plat_inc64(&m_live_n);
    plat_add64(&m_live_bytes, (int64_t)n);
}

static void mirror_forget(PoolRec *r) {
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    if (r->gpa_flip) r->gpa_flip = 0, n_gpa_flip--;
    for (int i = 0; i < nm; i++)
        if (mlist[i] == r) {
            memmove(mlist + i, mlist + i + 1, (size_t)(nm - i - 1) * sizeof *mlist);
            nm--;
            plat_dec64(&m_live_n);
            plat_add64(&m_live_bytes, -(int64_t)(r->import_hi - r->import_lo));
            break;
        }
}

static PoolRec **retiring;
static int nretiring, cretiring;
static PlatMutex retire_lock = PLAT_MUTEX_INIT;
static PoolRec **pool_dead;
static int npool_dead, cpool_dead;
static uint64_t n_pool_destroyed;
enum { PD_BY_COMPLETION, PD_BY_FINI };
static volatile int64_t pd_n[2], pd_ns[2], pd_max[2];

static void pool_retiring(PoolRec *r) {
    plat_mutex_lock(&retire_lock);
    if (nretiring == cretiring) grow((void **)&retiring, &cretiring, sizeof *retiring);
    retiring[nretiring++] = r;
    plat_mutex_unlock(&retire_lock);
}

static void pool_retire(void *arg) {
    plat_mutex_lock(&retire_lock);
    for (int i = 0; i < nretiring; i++)
        if (retiring[i] == arg) { retiring[i] = retiring[--nretiring]; break; }
    if (npool_dead == cpool_dead) grow((void **)&pool_dead, &cpool_dead, sizeof *pool_dead);
    pool_dead[npool_dead++] = arg;
    plat_mutex_unlock(&retire_lock);
}

static void pool_dead_drain(VkCtx *c, int by) {
    if (!__atomic_load_n(&npool_dead, __ATOMIC_RELAXED)) return;
    plat_mutex_lock(&retire_lock);
    PoolRec **go = pool_dead;
    int n = npool_dead;
    pool_dead = NULL, npool_dead = cpool_dead = 0;
    plat_mutex_unlock(&retire_lock);
    if (!n) {
        free(go);
        return;
    }
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < n; i++) destroy_rec(c, go[i]);
    free(go);
    int64_t ns = (int64_t)(plat_now_ns() - t0);
    __atomic_add_fetch(&n_pool_destroyed, (uint64_t)n, __ATOMIC_RELAXED);
    plat_add64(&pd_n[by], n), plat_add64(&pd_ns[by], ns);
    int64_t m = __atomic_load_n(&pd_max[by], __ATOMIC_RELAXED);
    while (ns > m && !__atomic_compare_exchange_n(&pd_max[by], &m, ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void mirror_sync(PoolRec *r) {
    uint64_t t0 = qpc();
    acc_set(r, msub);
    pm_note(r);
    uint64_t lo = r->import_lo, hi = r->import_hi;
    uint64_t r0 = lo >> SNAP_SHIFT, r1 = (hi - 1) >> SNAP_SHIFT;
    int excl = 0;
    plat_mutex_lock_shared(&gen_lock);
    for (uint64_t k = r0; k <= r1; k++)
        if (snap_at[k] != mepoch) { excl = 1; break; }
    if (excl) {
        plat_mutex_unlock_shared(&gen_lock);
        plat_mutex_lock(&gen_lock);
        mrep.sync_queries += snap_regions(r0, r1, mepoch);
    }
    uint64_t since = r->mgen;
    int untracked = 0;
    for (uint64_t k = r0; k <= r1; k++) {
        untracked |= snap_untracked[k];
        if (creg_max[k] <= since) continue;
        uint64_t a = k << SNAP_SHIFT, b = a + (1ull << SNAP_SHIFT);
        if (a < lo) a = lo;
        if (b > hi) b = hi;
        const uint64_t *ch = cgen_chunk[a >> GEN_CHUNK_SHIFT];
        if (!ch) continue;
        for (uint64_t x = a; x < b; x += 1ull << GEN_PAGE_SHIFT)
            if (ch[(x >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)] > since) {
                range_add(&pend, &npend, &cpend, r, x - lo, 1ull << GEN_PAGE_SHIFT), mrep.page_bytes += 1ull << GEN_PAGE_SHIFT;
                r->st_pages += 1ull << GEN_PAGE_SHIFT, r->hot_bytes += 1ull << GEN_PAGE_SHIFT, r->hot_up += 1ull << GEN_PAGE_SHIFT;
            }
    }
    __atomic_store_n(&r->mgen, gen_now, __ATOMIC_RELEASE);
    if (excl) plat_mutex_unlock(&gen_lock);
    else plat_mutex_unlock_shared(&gen_lock);
    if (__atomic_load_n(&r->fl_n, __ATOMIC_ACQUIRE)) {
        FlushRange fl[FLUSH_RANGES + 1];
        plat_mutex_lock(&flush_lock);
        int n = r->fl_n;
        memcpy(fl, r->fl, (size_t)n * sizeof *fl);
        uint64_t calls = r->fl_calls, sum = r->fl_sum;
        r->fl_n = 0, r->fl_calls = r->fl_sum = 0;
        plat_mutex_unlock(&flush_lock);
        mrep.flush_calls += calls, mrep.flush_sum += sum;
        r->hot_up += sum;
        for (int i = 0; i < n; i++) {
            uint64_t a = fl[i].lo > lo ? fl[i].lo : lo, b = fl[i].hi < hi ? fl[i].hi : hi;
            if (a >= b) continue;
            if (!untracked && !flush_upload) {
                mrep.flush_skipped += b - a;
                continue;
            }
            range_add(&pend, &npend, &cpend, r, a - lo, b - a);
            mrep.flush_n++, mrep.flush_bytes += b - a, r->st_flush += b - a;
            if ((rt_trace & RT_TRACE_PERF) && b - a >= 16u << 20)
                rt_log("mem: mirror frame %llu: flush range %.1f MiB at +%.1f MiB of a %.1f MiB pool (%d ranges from %llu "
                       "flushes of %.3f MiB)", (unsigned long long)mirror_frame, (b - a) / 1048576.0, (a - lo) / 1048576.0,
                       (hi - lo) / 1048576.0, n, (unsigned long long)calls, sum / 1048576.0);
        }
    }
    mrep.syncs++, r->st_syncs++;
    mrep.sync_t += qpc() - t0;
}

int vk_mem_shadow_storage(void) { return shadow_on; }
static int shadow_check_on(void) { return s_check; }
int vk_mem_shadow_table_stores(void) { return s_learn; }

static void shadow_table_add(PoolRec *r) {
    plat_mutex_lock(&shadow_lock);
    if (!r->stab_i) {
        if (nstab == cstab) grow((void **)&stab, &cstab, sizeof *stab);
        stab[nstab++] = r;
        r->stab_i = (uint32_t)nstab;
    }
    plat_mutex_unlock(&shadow_lock);
}

void vk_mem_shadow_table_read(void) {
    if (shadow_on) __atomic_store_n(&stab_want, vk_texture_epoch(), __ATOMIC_RELAXED);
}

static int shadow_pool_clean(const PoolRec *r, uint32_t epoch, int cpu) {
    uint64_t r0 = r->import_lo >> SNAP_SHIFT, r1 = (r->import_hi - 1) >> SNAP_SHIFT;
    if (!epoch || r1 >= SNAP_REGIONS) return 0;
    const uint64_t *mx = cpu ? creg_max : reg_max;
    for (uint64_t k = r0; k <= r1; k++)
        if (snap_at[k] != epoch || snap_untracked[k] || mx[k] > r->sgen) return 0;
    return 1;
}

void vk_mem_shadow_sync(uint64_t fence) {
    if (!shadow_on) return;
    uint64_t t0 = qpc();
    uint32_t epoch = vk_texture_epoch();
    int cpu = vk_mirror_on && wb_no_reup();
    static uint32_t tab_epoch;
    static uint64_t wb_fence;
    int table = epoch && epoch != tab_epoch && __atomic_load_n(&stab_want, __ATOMIC_RELAXED) == epoch;
    if (table) {
        tab_epoch = epoch;
        if (s_learn && fence && fence != wb_fence && __atomic_load_n(&ntpools, __ATOMIC_RELAXED)) {
            wb_fence = fence;
            vk_completion_add(fence, table_wb_cb, NULL);
        }
    }
    plat_mutex_lock(&shadow_lock);
    if (nstouch || (table && nstab)) {
        plat_mutex_lock(&gen_lock);
        for (int i = 0; i < nstouch; i++) shadow_sync_pool(stouch[i], epoch, cpu);
        uint64_t t1 = qpc();
        for (int i = 0; table && i < nstab; i++) {
            PoolRec *r = stab[i];
            if (r->stouched) continue;
            uint64_t r0 = r->import_lo >> SNAP_SHIFT, r1 = (r->import_hi - 1) >> SNAP_SHIFT;
            if (r1 < SNAP_REGIONS) s_queries += snap_regions(r0, r1, epoch);
            if (!shadow_pool_clean(r, epoch, cpu)) shadow_sync_pool(r, epoch, cpu), s_syncs++;
        }
        for (int i = 0; i < nstouch; i++) stouch[i]->stouched = 0;
        plat_mutex_unlock(&gen_lock);
        s_syncs += (uint64_t)nstouch;
        nstouch = 0;
        for (int i = 0; table && i < nstab; i++)
            if (stab[i]->mstate == MS_ON && acc_of(stab[i]) != msub) mirror_sync(stab[i]);
        if (table) s_tab_ticks += qpc() - t1, s_tab_syncs++;
    }
    s_ticks += qpc() - t0;
    plat_mutex_unlock(&shadow_lock);
}

static PlatMutex use_lock = PLAT_MUTEX_INIT;

static void mem_use_locked(VkMemSpan *sp);
void vk_mem_use(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (r->mstate == MS_ON && acc_of(r) == msub) {
        used_by(r, msub);
        sp->buffer = r->mbuf;
        return;
    }
    if (r->mstate == MS_REFUSED && !(r->retry_sub && (int32_t)(msub - r->retry_sub) >= 0)) {
        vk_mem_note_live();
        return;
    }
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&use_lock);
    mem_use_locked(sp);
    if (lk) plat_mutex_unlock(&use_lock);
}

static void mem_use_locked(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (r->mstate == MS_REFUSED && r->retry_sub && (int32_t)(msub - r->retry_sub) >= 0 && evict_enabled()) {
        plat_dec64(&m_refused_n[r->refused_why]);
        plat_add64(&m_refused_bytes[r->refused_why], -(int64_t)(r->import_hi - r->import_lo));
        plat_inc64(&m_retries);
        r->mstate = MS_NONE, r->retry_sub = 0;
    }
    if (r->mstate == MS_NONE) mirror_create(r);
    if (r->mstate != MS_ON) {
        vk_mem_note_live();
        return;
    }
    if (acc_of(r) != msub) mirror_sync(r);
    used_by(r, msub);
    sp->buffer = r->mbuf;
}

void vk_mem_pick(VkMemSpan *sp) {
    PoolRec *r = sp->pool;
    if (!r) return;
    if (r->mstate != MS_ON) {
        vk_mem_note_live();
        return;
    }
    if (acc_of(r) != msub) {
        int lk = vk_exec_shared();
        if (lk) plat_mutex_lock(&use_lock);
        if (acc_of(r) != msub) mirror_sync(r);
        if (lk) plat_mutex_unlock(&use_lock);
    }
    sp->buffer = r->mbuf;
}

VkBuffer vk_mem_guest_twin(const VkMemSpan *sp) {
    const PoolRec *r = sp->pool;
    return r && r->mstate == MS_ON && r->twin && sp->buffer == r->mbuf ? r->buf : VK_NULL_HANDLE;
}

int vk_mem_mirror_probe(uint64_t addr) {
    VkMemSpan sp;
    if (!vk_mirror_on || !vk_mem_resolve(addr, 1, &sp) || !sp.pool) return 0;
    mepoch = vk_texture_epoch();
    vk_mem_use(&sp);
    return ((PoolRec *)sp.pool)->mstate == MS_ON;
}

#define SNAP_CHUNK (1u << 20)
#define MERGE_SPAN (64ull << 20)
#define MERGE_WORDS (65535u * 64u)
#define MERGE_PIECE (4ull << 20)
#define MERGE_MAX_LIMIT (256ull << 20)
static uint64_t merge_max(void) {
    static uint64_t m;
    if (!m) {
        const char *e = cfg_get("NX2SARIA_WB_MERGE_MAX");
        uint64_t mib = e && *e ? (uint64_t)strtoull(e, NULL, 10) : 32;
        m = mib << 20;
        if (!m) m = 1;
        if (m > MERGE_MAX_LIMIT) m = MERGE_MAX_LIMIT;
        rt_log("mem: write-back merge covers ranges up to %.1f MiB (NX2SARIA_WB_MERGE_MAX)", m / 1048576.0);
    }
    return m;
}
#define DIFF_MIN (4ull << 20)
#define DIFF_MAX MERGE_SPAN
#define DIFF_WORDS (65535u * 256u)
typedef struct { PoolRec *r; uint64_t off, size, soff; int chunk, diff; } SnapRange;
typedef struct { VkBuffer buf; VmaAllocation alloc; uint64_t size, used; int host; } SnapChunk;
typedef struct { int n; SnapChunk c[]; } SnapFree;
static SnapRange *snl;
static int nsnl, csnl, snl_copied;
static SnapChunk *chunks;
static int nchunks, cchunks;
#define SNAP_LIST_MIN (64u << 10)
#define SNAP_POOL_N 128
static uint64_t snap_pool_cap(void) {
    static uint64_t cap;
    if (!cap) {
        uint64_t used = 0, budget = 0;
        int exact = 0;
        uint64_t c = vk_mem_vram(&used, &budget, &exact) && budget ? budget / 64 : (64ull << 20);
        if (c < (64ull << 20)) c = 64ull << 20;
        if (c > (256ull << 20)) c = 256ull << 20;
        cap = c;
        rt_log("mem: write-back snapshot pool keeps up to %llu MiB", (unsigned long long)(cap >> 20));
    }
    return cap;
}
static PlatMutex snap_pool_lock = PLAT_MUTEX_INIT;
static SnapChunk snap_pool[SNAP_POOL_N];
static int nsnap_pool;
static uint64_t snap_pool_bytes;
enum { CHUNK_LIST = 1, CHUNK_MARKS = 2, CHUNK_MLIST = 3, CHUNK_MCOPY = 4 };
#define SNAP_POOL_LISTS 4
static int nsnap_lists;
static int chunk_is_mlist(int host) { return host == CHUNK_MLIST || host == CHUNK_MCOPY; }
static int merge_on = -1;
static struct {
    int tried, ok, dok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl, ddsl;
    VkPipelineLayout layout, dlayout;
    VkPipeline pipe, dpipe;
    int ltried, lok;
    VkDescriptorSetLayout ldsl, cdsl;
    VkPipelineLayout llayout, clayout;
    VkPipeline lpipe, cpipe;
    VkSemaphore asem;
    uint64_t asem_val;
} WM;

static int comp_pipe(VkCtx *c, const uint32_t *code, size_t size, int nb, uint32_t pc, VkDescriptorSetLayout *dsl,
                     VkPipelineLayout *layout, VkPipeline *pipe) {
    VkDescriptorSetLayoutBinding b[8];
    for (int i = 0; i < nb; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = (uint32_t)nb, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, pc};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = *layout};
    VkResult res = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    return res == VK_SUCCESS;
}

static int diff_init(VkCtx *c) {
    return comp_pipe(c, wb_diffcopy_spv, sizeof wb_diffcopy_spv, 4, 8, &WM.ddsl, &WM.dlayout, &WM.dpipe);
}

static int merge_init(VkCtx *c) {
    WM.tried = 1;
    WM.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[5];
    for (int i = 0; i < 5; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 5, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof wb_merge_spv,
                                   .pCode = wb_merge_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!WM.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &WM.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &WM.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &WM.layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = WM.layout};
    VkResult res = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &WM.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (res != VK_SUCCESS) goto fail;
    if (!(WM.dok = diff_init(c))) rt_log("mem: the write-back by difference could not be created: large ranges stay copies");
    return WM.ok = 1;
fail:
    rt_log("mem: write-back merge off: its pipeline could not be created");
    return 0;
}

static int merging(void) {
    if (merge_on < 0) {
        merge_on = cfg_bool("NX2SARIA_WB_MERGE");
        rt_log("mem: write-back merge %s (NX2SARIA_WB_MERGE)", merge_on ? "on" : "off");
    }
    if (!merge_on) return 0;
    if (!WM.tried) merge_init(vk_ctx());
    return WM.ok;
}

static int wb_lazy(void);
static int wb_direct(void) { return !shadow_on && !wb_lazy(); }

static int wb_diff(void) {
    static int mode = -1;
    if (mode < 0) {
        mode = rt_knob("WB_DIFF", "1");
        if (mode != 1) rt_log("mem: written ranges up to %llu MiB go back by difference%s (NX2SARIA_WB_DIFF)",
                              (unsigned long long)(DIFF_MAX >> 20), mode == 2 ? " in the B blocks" : " never");
    }
    return RT_KNOB_ON(mode) && WM.dok && wb_direct();
}

#define MERGE_CPU_MAX (32ull << 20)
static int merge_cpu_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        mode = rt_knob("MERGE_CPU", "1");
        if (mode != 1) rt_log("mem: merges go back through the CPU %s (NX2SARIA_MERGE_CPU)", mode == 2 ? "in the B blocks" : "never");
    }
    return mode;
}

static uint64_t merge_cpu_min(void) {
    static int64_t v = -1;
    if (v < 0) {
        const char *s = cfg_get("NX2SARIA_MERGE_CPU_MIN");
        v = s && *s ? strtoll(s, NULL, 0) : 256;
        if (v < 0) v = 0;
    }
    return (uint64_t)v << 10;
}

static int merge_cpu_check(void) {
    static int v = -1;
    if (v < 0) {
        const char *s = cfg_get("NX2SARIA_MERGE_CPU_CHECK");
        v = s && *s == '1';
        if (v) rt_log("mem: NX2SARIA_MERGE_CPU_CHECK: the GPU merges, the CPU compares the lists");
    }
    return v;
}

static int list_init(VkCtx *c) {
    WM.ltried = 1;
    VkSemaphoreTypeCreateInfo ti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti};
    if (!comp_pipe(c, wb_mergelist_spv, sizeof wb_mergelist_spv, 5, 20, &WM.ldsl, &WM.llayout, &WM.lpipe) ||
        !comp_pipe(c, wb_listcopy_spv, sizeof wb_listcopy_spv, 2, 4, &WM.cdsl, &WM.clayout, &WM.cpipe) ||
        vkCreateSemaphore(c->device, &si, NULL, &WM.asem) != VK_SUCCESS) {
        rt_log("mem: NX2SARIA_MERGE_CPU: the merge list's pipelines could not be created: merges stay on the GPU");
        return 0;
    }
    return WM.lok = 1;
}

static int merge_cpu(uint64_t merge_bytes) {
    int m = merge_cpu_mode();
    if (!m || !merge_bytes || merge_bytes < merge_cpu_min() || merge_bytes > MERGE_CPU_MAX || !RT_KNOB_ON(m) || !WM.ok ||
        !wb_direct())
        return 0;
    if (!WM.ltried) list_init(vk_ctx());
    return WM.lok;
}

static volatile int64_t mc_lists, mc_words, mc_ns, mc_waits[3], mc_wait_ns[3], mc_over, mc_bad, mc_diff;

typedef struct { uint32_t first, words; uint64_t host; } MergeSpan;
typedef struct {
    uint64_t fence;
    const uint8_t *map;
    VmaAllocation alloc;
    uint32_t cap;
    int n, check;
    MergeSpan t[];
} MergeJob;

static void merge_apply(void *arg) {
    MergeJob *j = arg;
    uint64_t t0 = qpc();
    VkCtx *c = vk_ctx();
    vmaInvalidateAllocation(c->vma, j->alloc, 0, 16);
    uint32_t n = *(const volatile uint32_t *)j->map;
    if (n > j->cap) plat_add64(&mc_over, (int64_t)(n - j->cap)), n = j->cap;
    if (n) vmaInvalidateAllocation(c->vma, j->alloc, 16, (uint64_t)n * 8);
    const uint32_t *e = (const uint32_t *)(j->map + 16);
    int k = 0;
    int64_t bad = 0, diff = 0;
    static volatile int32_t logged;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t g = e[2 * i], v = e[2 * i + 1];
        if (g - j->t[k].first >= j->t[k].words) {
            int lo = 0, hi = j->n;
            while (hi - lo > 1) {
                int m = (lo + hi) >> 1;
                if (j->t[m].first <= g) lo = m;
                else hi = m;
            }
            k = lo;
            if (g - j->t[k].first >= j->t[k].words) { bad++; continue; }
        }
        uint32_t *p = (uint32_t *)(uintptr_t)(j->t[k].host + 4ull * (g - j->t[k].first));
        if (!j->check) *p = v;
        else if (*p != v) {
            diff++;
            if (plat_inc32(&logged) <= 8)
                rt_log("mem: NX2SARIA_MERGE_CPU_CHECK: %#llx holds %#x, the list %#x (value %llu)",
                       (unsigned long long)(uintptr_t)p, *p, v, (unsigned long long)j->fence);
        }
    }
    if (j->fence > WM.asem_val) {
        VkSemaphoreSignalInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = WM.asem, .value = j->fence};
        VkResult r = vkSignalSemaphore(c->device, &si);
        if (r != VK_SUCCESS) {
            vk_device_lost_check(r, "the signal of the merge lists' timeline");
            rt_fatal("vk: vkSignalSemaphore of the merge lists' timeline failed (%d)", (int)r);
        }
        WM.asem_val = j->fence;
    }
    uint64_t dt = qpc() - t0;
    plat_inc64(&mc_lists);
    plat_add64(&mc_words, (int64_t)n);
    plat_add64(&mc_ns, (int64_t)dt);
    if (bad) plat_add64(&mc_bad, bad);
    if (diff) plat_add64(&mc_diff, diff);
    free(j);
}

#define MC_RING 64
static uint64_t mc_ring[MC_RING], mc_n;

static void apply_wait(uint64_t v, int why) {
    if (!v || v <= vk_sync_fence_done()) return;
    uint64_t t0 = qpc();
    vk_sync_release_held(v);
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &WM.asem,
                              .pValues = &v};
    VkResult r = vkWaitSemaphores(vk_ctx()->device, &wi, UINT64_MAX);
    if (r != VK_SUCCESS) {
        vk_device_lost_check(r, "the wait for the merge lists' timeline");
        rt_fatal("vk: vkWaitSemaphores on the merge lists' timeline failed (%d; device lost?)", (int)r);
    }
    plat_inc64(&mc_waits[why]);
    plat_add64(&mc_wait_ns[why], (int64_t)(qpc() - t0));
}

void vk_mem_merge_cpu_wait(uint64_t value) {
    uint64_t n = __atomic_load_n(&mc_n, __ATOMIC_ACQUIRE);
    for (uint64_t i = n; i > 0 && i + MC_RING > n; i--) {
        uint64_t v = __atomic_load_n(&mc_ring[(i - 1) % MC_RING], __ATOMIC_RELAXED);
        if (v <= value) {
            apply_wait(v, 2);
            return;
        }
    }
}

static int chunk_new(uint64_t size, int host) {
    uint64_t want = host ? SNAP_LIST_MIN : SNAP_CHUNK;
    while (want < size) want <<= 1;
    if (nchunks == cchunks) grow((void **)&chunks, &cchunks, sizeof *chunks);
    SnapChunk *ch = &chunks[nchunks];
    int best = -1;
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < nsnap_pool; i++)
        if (snap_pool[i].host == host && snap_pool[i].size >= want && (best < 0 || snap_pool[i].size < snap_pool[best].size))
            best = i;
    if (best >= 0) {
        *ch = snap_pool[best];
        snap_pool[best] = snap_pool[--nsnap_pool];
        if (chunk_is_mlist(host)) nsnap_lists--;
        else snap_pool_bytes -= ch->size;
    }
    plat_mutex_unlock(&snap_pool_lock);
    if (best < 0) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = want,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        int dev = !host || host == CHUNK_MLIST, rd = host == CHUNK_MARKS || host == CHUNK_MCOPY;
        VmaAllocationCreateInfo ai = {.usage = dev ? VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE : VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = rd ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                        VMA_ALLOCATION_CREATE_MAPPED_BIT
                                             : !dev ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                          VMA_ALLOCATION_CREATE_MAPPED_BIT : 0};
        if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &ch->buf, &ch->alloc, NULL) != VK_SUCCESS) return -1;
        ch->size = want, ch->host = host;
        mrep.snap_allocs++;
    }
    ch->used = 0;
    return nchunks++;
}

static int snap_add(PoolRec *r, uint64_t off, uint64_t size, int diff) {
    for (int i = nsnl; i-- > 0;)
        if (snl[i].r == r && snl[i].off == off && snl[i].size == size) return 1;
    int k = -1;
    for (int i = nchunks; i-- > 0;)
        if (chunks[i].used != UINT64_MAX && chunks[i].size - chunks[i].used >= size) { k = i; break; }
    if (k < 0 && (k = chunk_new(size > SNAP_CHUNK ? size : SNAP_CHUNK, 0)) < 0) return 0;
    if (nsnl == csnl) grow((void **)&snl, &csnl, sizeof *snl);
    snl[nsnl++] = (SnapRange){r, off, size, chunks[k].used, k, diff};
    chunks[k].used += (size + 15) & ~15ull;
    return 1;
}

static void snap_copies(VkCommandBuffer cb) {
    for (int i = snl_copied; i < nsnl; i++) {
        VkBufferCopy cp = {snl[i].off, snl[i].soff, snl[i].size};
        vkCmdCopyBuffer(cb, snl[i].r->mbuf, chunks[snl[i].chunk].buf, 1, &cp);
        mrep.snap_bytes += snl[i].size;
    }
    snl_copied = nsnl;
}

static int snap_cmp(const void *x, const void *y) {
    const SnapRange *a = x, *b = y;
    if (a->diff != b->diff) return a->diff - b->diff;
    if (a->r != b->r) return (uintptr_t)a->r < (uintptr_t)b->r ? -1 : 1;
    if (a->chunk != b->chunk) return a->chunk < b->chunk ? -1 : 1;
    return a->off < b->off ? -1 : a->off > b->off;
}

static void merge_dispatch(VkCommandBuffer cb, const SnapRange *s, int n, uint64_t base, uint64_t span, int list,
                           uint64_t list_off, uint32_t total, int marks, uint64_t marks_off) {
    uint64_t npg = ((base & (SB_PAGE - 1)) + span + SB_PAGE - 1) / SB_PAGE;
    VkDescriptorBufferInfo bi[5] = {{s->r->mbuf, base, span}, {chunks[s->chunk].buf, 0, chunks[s->chunk].size},
                                    {s->r->buf, base, span}, {chunks[list].buf, list_off, (uint64_t)n * 16},
                                    marks >= 0 ? (VkDescriptorBufferInfo){chunks[marks].buf, marks_off, npg * 4}
                                               : (VkDescriptorBufferInfo){chunks[list].buf, list_off, (uint64_t)n * 16}};
    VkWriteDescriptorSet w[5];
    for (int i = 0; i < 5; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi[i]};
    rec_push(WM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.layout, 0, 5, w);
    uint32_t pc[4] = {(uint32_t)n, total, (uint32_t)(base & (SB_PAGE - 1)), marks >= 0};
    vkCmdPushConstants(cb, WM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (total + 63) / 64, 1, 1);
}

static void list_dispatch(VkCommandBuffer cb, const SnapRange *s, int n, uint64_t base, uint64_t span, int list,
                          uint64_t list_off, uint32_t total, VkBuffer lbuf, uint64_t lsize, uint32_t gbase, uint32_t cap,
                          int check) {
    VkDescriptorBufferInfo bi[5] = {{s->r->mbuf, base, span}, {chunks[s->chunk].buf, 0, chunks[s->chunk].size},
                                    {s->r->buf, base, span}, {chunks[list].buf, list_off, (uint64_t)n * 16},
                                    {lbuf, 0, lsize}};
    VkWriteDescriptorSet w[5];
    for (int i = 0; i < 5; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi[i]};
    rec_push(WM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.llayout, 0, 5, w);
    uint32_t pc[5] = {(uint32_t)n, total, gbase, cap, (uint32_t)check};
    vkCmdPushConstants(cb, WM.llayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (total + 63) / 64, 1, 1);
}

static void diff_dispatch(VkCommandBuffer cb, const SnapRange *s, int n, uint64_t base, uint64_t span, int list,
                          uint64_t list_off, uint32_t total) {
    VkDescriptorBufferInfo bi[4] = {{s->r->mbuf, base, span}, {chunks[s->chunk].buf, 0, chunks[s->chunk].size},
                                    {s->r->buf, base, span}, {chunks[list].buf, list_off, (uint64_t)n * 16}};
    VkWriteDescriptorSet w[4];
    for (int i = 0; i < 4; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi[i]};
    rec_push(WM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.dlayout, 0, 4, w);
    uint32_t pc[2] = {(uint32_t)n, total};
    vkCmdPushConstants(cb, WM.dlayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (total + 255) / 256, 1, 1);
}

static uint64_t diff_first(uint64_t total, uint64_t x) { return total + ((x - total) & 31); }

static int merge_group(const SnapRange *s, int i, int n, uint64_t align, uint64_t *base, uint64_t *end, uint32_t *total) {
    int diff = s[i].diff;
    if (diff && align < 128) align = 128;
    uint64_t b = s[i].off & ~(align - 1), e = s[i].off + s[i].size;
    uint64_t tot = 0;
    int j = i;
    for (; j < n && s[j].r == s[i].r && s[j].chunk == s[i].chunk && s[j].diff == s[i].diff; j++) {
        uint64_t f = s[j].off + s[j].size > e ? s[j].off + s[j].size : e;
        uint64_t t = (diff ? diff_first(tot, (s[j].off - b) / 4) : tot) + s[j].size / 4;
        if (j > i && (f - b > MERGE_SPAN || t > (diff ? DIFF_WORDS : MERGE_WORDS))) break;
        e = f, tot = t;
    }
    *base = b, *end = e, *total = (uint32_t)tot;
    return j;
}

typedef struct { uint64_t addr, size; uint32_t mark0; } SwbRange;
#define SWB_ALL UINT32_MAX
typedef struct { VmaAllocation marks_alloc; const uint32_t *marks; int n; SwbRange g[]; } SwbJob;
static SwbRange *swl;
static int nswl, cswl;
static int swl_marks = -1;

static void swl_add(uint64_t addr, uint64_t size, uint32_t mark0) {
    if (nswl == cswl) grow((void **)&swl, &cswl, sizeof *swl);
    swl[nswl++] = (SwbRange){addr, size, mark0};
}

static void swb_run(uint64_t a, uint64_t b) {
    if (b <= a || !shadow_write_back(a, b - a) || nvn_gpu_only(a)) return;
    int back = wb_no_reup();
    plat_mutex_lock(&gen_lock);
    uint64_t g = gen_next();
    for (uint64_t x = a & ~0xfffull; x < b; x += 1ull << GEN_PAGE_SHIFT) stamp_page_as(x, g, back);
    plat_mutex_unlock(&gen_lock);
}

static void shadow_wb_cb(void *arg) {
    SwbJob *j = arg;
    if (j->marks_alloc) vmaInvalidateAllocation(vk_ctx()->vma, j->marks_alloc, 0, VK_WHOLE_SIZE);
    for (int i = 0; i < j->n; i++) {
        const SwbRange *g = &j->g[i];
        uint64_t end = g->addr + g->size;
        if (g->mark0 == SWB_ALL) {
            swb_run(g->addr, end);
            continue;
        }
        uint64_t run = 0;
        uint32_t k = g->mark0;
        for (uint64_t pg = g->addr & ~(SB_PAGE - 1); pg < end; pg += SB_PAGE, k++) {
            uint64_t a = pg > g->addr ? pg : g->addr;
            if (j->marks[k]) {
                if (!run) run = a;
            } else if (run) {
                swb_run(run, a);
                run = 0;
            }
        }
        if (run) swb_run(run, end);
    }
    for (int i = 0; i < j->n; i++) mem_probe(j->g[i].addr, j->g[i].size);
    free(j);
}

static void swb_job(uint64_t fence) {
    SwbJob *j = malloc(sizeof *j + sizeof(SwbRange) * (size_t)(nswl + nwbl));
    if (!j) rt_fatal("vk: out of memory");
    j->marks_alloc = NULL, j->marks = NULL;
    if (swl_marks >= 0) {
        VmaAllocationInfo info;
        vmaGetAllocationInfo(vk_ctx()->vma, chunks[swl_marks].alloc, &info);
        j->marks_alloc = chunks[swl_marks].alloc, j->marks = info.pMappedData;
    }
    memcpy(j->g, swl, sizeof *swl * (size_t)nswl);
    j->n = nswl;
    for (int i = 0; i < nwbl; i++) j->g[j->n++] = (SwbRange){wbl[i].r->import_lo + wbl[i].off, wbl[i].size, SWB_ALL};
    nswl = 0, swl_marks = -1;
    vk_completion_add(fence, shadow_wb_cb, j);
}

static int wb_merge_record(VkCommandBuffer cb, uint64_t fence, int *listed) {
    VkCtx *c = vk_ctx();
    uint64_t align = c->props.limits.minStorageBufferOffsetAlignment;
    if (align < 16) align = 16;
    SnapRange *s = malloc(sizeof *s * (size_t)nsnl);
    struct MGroup { int i, j; uint64_t base, end; uint32_t total; } *gr = malloc(sizeof *gr * (size_t)nsnl);
    if (!s || !gr) rt_fatal("vk: out of memory");
    memcpy(s, snl, sizeof *s * (size_t)nsnl);
    qsort(s, (size_t)nsnl, sizeof *s, snap_cmp);
    *listed = 0;
    int ng = 0;
    uint64_t nmarks = 0;
    uint64_t mwords = 0;
    for (int i = 0; i < nsnl;) {
        uint64_t base, end;
        uint32_t total;
        int j = merge_group(s, i, nsnl, align, &base, &end, &total);
        gr[ng++] = (struct MGroup){i, j, base, end, total};
        nmarks += (((base & (SB_PAGE - 1)) + end - base + SB_PAGE - 1) / SB_PAGE * 4 + align - 1) & ~(align - 1);
        if (!s[i].diff) mwords += total;
        i = j;
    }
    int list = chunk_new((uint64_t)nsnl * 16 + (uint64_t)nsnl * align, CHUNK_LIST);
    if (list < 0) {
        free(s), free(gr);
        return 0;
    }
    chunks[list].used = UINT64_MAX;
    int marks = shadow_on ? chunk_new(nmarks, CHUNK_MARKS) : -1;
    uint32_t *mm = NULL;
    if (marks >= 0) {
        chunks[marks].used = UINT64_MAX;
        VmaAllocationInfo mi;
        vmaGetAllocationInfo(c->vma, chunks[marks].alloc, &mi);
        mm = mi.pMappedData;
        memset(mm, 0, nmarks);
        vmaFlushAllocation(c->vma, chunks[marks].alloc, 0, VK_WHOLE_SIZE);
        swl_marks = marks;
    }
    VmaAllocationInfo li;
    vmaGetAllocationInfo(c->vma, chunks[list].alloc, &li);
    uint32_t *lm = li.pMappedData;
    uint64_t lsize = 0;
    int ml = -1, mh = -1, check = 0;
    MergeJob *job = NULL;
    if (mwords && mwords < 0x7fffffffull && merge_cpu(mwords * 4)) {
        lsize = (16 + mwords * 8 + 15) & ~15ull;
        if ((ml = chunk_new(lsize, CHUNK_MLIST)) >= 0) chunks[ml].used = UINT64_MAX;
        if (ml >= 0 && (mh = chunk_new(lsize, CHUNK_MCOPY)) >= 0) chunks[mh].used = UINT64_MAX;
        if (mh >= 0 && (job = malloc(sizeof *job + sizeof(MergeSpan) * (size_t)nsnl))) {
            VmaAllocationInfo hi;
            vmaGetAllocationInfo(c->vma, chunks[mh].alloc, &hi);
            check = merge_cpu_check();
            *job = (MergeJob){.fence = fence, .map = hi.pMappedData, .alloc = chunks[mh].alloc, .cap = (uint32_t)mwords,
                              .check = check};
            vkCmdFillBuffer(cb, chunks[ml].buf, 0, 16, 0);
            mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
    }
    uint64_t lo = 0, mo = 0;
    int rec = 0, nt = 0;
    int bound = -1;
    uint32_t gbase = 0;
    for (int k = 0; k < ng; k++) {
        const struct MGroup *d = &gr[k];
        int diff = s[d->i].diff;
        int kind = diff ? 1 : job ? 2 : 0;
        if (bound != kind)
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, kind == 1 ? WM.dpipe : kind == 2 ? WM.lpipe : WM.pipe),
                bound = kind;
        lo = (lo + align - 1) & ~(align - 1);
        uint64_t l0 = lo;
        uint32_t first = 0;
        for (int j = d->i; j < d->j; j++) {
            uint32_t *q = lm + lo / 4, x = (uint32_t)((s[j].off - d->base) / 4);
            if (diff) first = (uint32_t)diff_first(first, x);
            q[0] = x, q[1] = (uint32_t)(s[j].soff / 4), q[2] = (uint32_t)(s[j].size / 4);
            q[3] = first;
            if (kind == 2) {
                job->t[nt++] = (MergeSpan){gbase + first, (uint32_t)(s[j].size / 4), s[j].r->import_lo + s[j].off};
                if (!check) cpu_span_add(s[j].r, s[j].off, s[j].off + s[j].size, fence);
            }
            first += (uint32_t)(s[j].size / 4);
            lo += 16;
            if (diff) mrep.diff_bytes += s[j].size;
            else mrep.merge_bytes += s[j].size;
        }
        if (kind == 1) {
            diff_dispatch(cb, s + d->i, d->j - d->i, d->base, d->end - d->base, list, l0, d->total);
            mrep.diff_dispatches++;
        } else if (kind == 2) {
            list_dispatch(cb, s + d->i, d->j - d->i, d->base, d->end - d->base, list, l0, d->total, chunks[ml].buf, lsize,
                          gbase, job->cap, check);
            gbase += d->total;
            mrep.merge_dispatches++;
        } else {
            merge_dispatch(cb, s + d->i, d->j - d->i, d->base, d->end - d->base, list, l0, d->total, marks, mo);
            if (shadow_on) {
                uint64_t ga = s[d->i].r->import_lo + d->base;
                if (marks >= 0) swl_add(ga, d->end - d->base, (uint32_t)(mo / 4));
                else
                    for (int j = d->i; j < d->j; j++) swl_add(s[j].r->import_lo + s[j].off, s[j].size, SWB_ALL);
            }
            mrep.merge_dispatches++;
        }
        mo += (((d->base & (SB_PAGE - 1)) + d->end - d->base + SB_PAGE - 1) / SB_PAGE * 4 + align - 1) & ~(align - 1);
        rec += 3;
    }
    vmaFlushAllocation(c->vma, chunks[list].alloc, 0, VK_WHOLE_SIZE);
    if (job) {
        job->n = nt;
        mbarrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.cpipe);
        VkDescriptorBufferInfo bi[2] = {{chunks[ml].buf, 0, lsize}, {chunks[mh].buf, 0, lsize}};
        VkWriteDescriptorSet w[2];
        for (int i = 0; i < 2; i++)
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .pBufferInfo = &bi[i]};
        rec_push(WM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, WM.clayout, 0, 2, w);
        uint32_t cap = job->cap;
        vkCmdPushConstants(cb, WM.clayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof cap, &cap);
        uint64_t groups = (1 + (mwords + 1) / 2 + 255) / 256;
        vkCmdDispatch(cb, (uint32_t)(groups < 256 ? groups : 256), 1, 1);
        rec += 7;
        vk_completion_add_first(fence, merge_apply, job);
        *listed = !check;
        __atomic_store_n(&mc_ring[mc_n % MC_RING], fence, __ATOMIC_RELAXED);
        __atomic_store_n(&mc_n, mc_n + 1, __ATOMIC_RELEASE);
    }
    for (int i = 0; i < nsnl; i++) snl[i].r->wb_sub = msub;
    free(s), free(gr);
    return rec;
}

static void snap_free(void *arg) {
    SnapFree *f = arg;
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < f->n; i++) {
        SnapChunk *ch = &f->c[i];
        int ml = chunk_is_mlist(ch->host);
        if (nsnap_pool == SNAP_POOL_N || (ml ? nsnap_lists == SNAP_POOL_LISTS : snap_pool_bytes + ch->size > snap_pool_cap()))
            continue;
        snap_pool[nsnap_pool++] = *ch;
        if (ml) nsnap_lists++;
        else snap_pool_bytes += ch->size;
        ch->buf = VK_NULL_HANDLE;
    }
    plat_mutex_unlock(&snap_pool_lock);
    for (int i = 0; i < f->n; i++)
        if (f->c[i].buf) vmaDestroyBuffer(vk_ctx()->vma, f->c[i].buf, f->c[i].alloc);
    free(f);
}

#define LZ_SLOTS 4
static struct { VkCommandPool pool; VkCommandBuffer cb; VkFence fence; } lz_slot[LZ_SLOTS];
static volatile uint32_t lz_slot_free;
static volatile int64_t lz_slot_waits;
static volatile int32_t lz_state;
static PlatMutex lz_init_lock = PLAT_MUTEX_INIT;
typedef struct { VkMemSpan sp; uint64_t size; int shader; } LzSkip;
static LzSkip *lz_skip;
static int lz_nskip, lz_cskip;
static int lz_win;
static uint64_t lz_skipped;

static void mbarrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                     VkAccessFlags2 da);

static int lz_slot_take(void) {
    for (int waited = 0;;) {
        uint32_t m = __atomic_load_n(&lz_slot_free, __ATOMIC_ACQUIRE);
        if (m) {
            int i = __builtin_ctz(m);
            if (__atomic_compare_exchange_n(&lz_slot_free, &m, m & ~(1u << i), 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return i;
            continue;
        }
        if (!waited++) plat_inc64(&lz_slot_waits);
        plat_wait_ns(20000);
    }
}

static void lazy_fetch(const MemLazyCopy *cp, int n) {
    if (vk_device_is_lost() || n <= 0) return;
    VkCtx *c = vk_ctx();
    int s = lz_slot_take();
    VkCommandBuffer cb = lz_slot[s].cb;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkResetCommandPool(c->device, lz_slot[s].pool, 0);
    vkBeginCommandBuffer(cb, &bi);
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    uint64_t bytes = 0;
    for (int i = 0; i < n; i++) {
        VkBufferCopy reg = {cp[i].off, cp[i].off, cp[i].size};
        vkCmdCopyBuffer(cb, (VkBuffer)(uintptr_t)cp[i].h0, (VkBuffer)(uintptr_t)cp[i].h1, 1, &reg);
        bytes += cp[i].size;
    }
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    vk_queue_lock();
    vk_sync_flush_held();
    VkResult r = vkQueueSubmit(c->gfx_queue, 1, &si, lz_slot[s].fence);
    vk_queue_unlock();
    if (r == VK_SUCCESS) r = vkWaitForFences(c->device, 1, &lz_slot[s].fence, VK_TRUE, 10000000000ull);
    if (r != VK_SUCCESS) {
        vk_device_lost_check(r, "the submit or the wait that brings GPU writes down to guest memory");
        rt_fatal("vk: bringing %llu bytes the GPU wrote down to guest memory failed (%d)", (unsigned long long)bytes,
                 (int)r);
    }
    vkResetFences(c->device, 1, &lz_slot[s].fence);
    __atomic_fetch_or(&lz_slot_free, 1u << s, __ATOMIC_RELEASE);
}

static int lazy_clean(uint64_t lo, uint64_t hi, void *ctx) {
    const PoolRec *r = ctx;
    uint64_t mgen = __atomic_load_n(&r->mgen, __ATOMIC_ACQUIRE);
    for (uint64_t a = lo; a < hi; a += 1ull << GEN_PAGE_SHIFT) {
        const uint64_t *ch = __atomic_load_n(&cgen_chunk[a >> GEN_CHUNK_SHIFT], __ATOMIC_ACQUIRE);
        if (ch && __atomic_load_n(&ch[(a >> GEN_PAGE_SHIFT) & (GEN_PER_CHUNK - 1)], __ATOMIC_RELAXED) > mgen) return 0;
    }
    uint64_t p[1];
    return mem_written_peek(lo, hi - lo, p, 1) == 0;
}

_Static_assert(WBL_DRIVER_AMD == VK_DRIVER_ID_AMD_PROPRIETARY, "wb_lazy_choice.h names AMD's own driver");
static int lz_wanted = -1;
static const char *lz_why = "";

void vk_mem_lazy_decide(void) {
    const char *set = cfg_is_set("NX2SARIA_WB_LAZY") ? cfg_get("NX2SARIA_WB_LAZY") : NULL;
    WblChoice ch = wb_lazy_choice(set, WBL_ON_WINDOWS, vk_caps.driver_id);
    if (!vk_mirror_on) ch = (WblChoice){0, "no pool has a mirror in import mode"};
    else if (shadow_on) ch = (WblChoice){0, "shadow storage keeps it off"};
    else if (ch.on && cfg_bool("NX2SARIA_WB_PROBE")) ch = (WblChoice){0, "the write-back probe watches the same pages"};
    lz_why = ch.why;
    __atomic_store_n(&lz_wanted, ch.on, __ATOMIC_RELEASE);
    rt_log("mem: lazy write-back %s: %s", ch.on ? "on" : "off", ch.why);
}

static int wb_lazy(void) {
    int s = __atomic_load_n(&lz_state, __ATOMIC_ACQUIRE);
    if (s) return s == 1;
    plat_mutex_lock(&lz_init_lock);
    if (!lz_state) {
        VkCtx *c = vk_ctx();
        int on = c && __atomic_load_n(&lz_wanted, __ATOMIC_ACQUIRE) == 1;
        if (on) {
            VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .queueFamilyIndex = c->gfx_family};
            VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            int made = 0;
            for (; made < LZ_SLOTS; made++) {
                if (vkCreateCommandPool(c->device, &pi, NULL, &lz_slot[made].pool) != VK_SUCCESS) break;
                VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                  .commandPool = lz_slot[made].pool,
                                                  .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
                if (vkAllocateCommandBuffers(c->device, &ai, &lz_slot[made].cb) != VK_SUCCESS ||
                    vkCreateFence(c->device, &fi, NULL, &lz_slot[made].fence) != VK_SUCCESS) {
                    vkDestroyCommandPool(c->device, lz_slot[made].pool, NULL);
                    break;
                }
            }
            __atomic_store_n(&lz_slot_free, (1u << made) - 1, __ATOMIC_RELEASE);
            on = made > 0 && mem_lazy_enable(lazy_fetch, lazy_clean);
        }
        rt_log("mem: what shaders write into mirrored pools %s (lazy write-back: %s)",
               on ? "goes back to guest memory once and then only when the CPU touches it"
                  : "goes back to guest memory at the end of every submission",
               on || lz_wanted != 1 ? lz_why : "its command buffer and fence could not be made");
        __atomic_store_n(&lz_state, on ? 1 : 2, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&lz_init_lock);
    return lz_state == 1;
}

static void gpu_wrote_range(const VkMemSpan *sp, uint64_t size, int shader);

#define WB_SKIP_HEAD (64u << 10)
static uint64_t wb_skip(void) {
    static int64_t at = -1;
    if (at < 0) {
        int mb = cfg_int("NX2SARIA_WB_SKIP_MB");
        at = mb > 0 ? (int64_t)mb << 20 : 0;
        if (at)
            rt_log("mem: without the lazy write-back, written ranges of %d MiB or more go back to guest memory only for "
                   "their first %u KiB (NX2SARIA_WB_SKIP_MB)", mb, WB_SKIP_HEAD >> 10);
    }
    return (uint64_t)at;
}
static int64_t m_wb_skipped;

static volatile uint64_t n_gpu_wrote;
uint64_t vk_mem_gpu_wrote_count(void) { return __atomic_load_n(&n_gpu_wrote, __ATOMIC_RELAXED); }

static void gpu_wrote(const VkMemSpan *sp, uint64_t size, int shader) {
    __atomic_fetch_add(&n_gpu_wrote, 1, __ATOMIC_RELAXED);
    PoolRec *r = sp->pool;
    if (!r || r->mstate != MS_ON || sp->buffer != r->mbuf || !size || !wb_lazy()) {
        uint64_t skip = r && r->mstate == MS_ON && sp->buffer == r->mbuf ? wb_skip() : 0;
        if (skip && size >= skip && !wb_lazy()) {
            gpu_wrote_range(sp, WB_SKIP_HEAD, shader);
            r->wb_kept = 1;
            plat_add64(&m_wb_skipped, (int64_t)(size - WB_SKIP_HEAD));
            return;
        }
        gpu_wrote_range(sp, size, shader);
        return;
    }
    uint64_t g0 = r->import_lo + sp->offset, g1 = g0 + size, run = g0, out[16][2];
    int k = mem_lazy_skip(g0, size, (uint64_t)(uintptr_t)r->mbuf, -(int64_t)r->import_lo, out, 16);
    for (int i = 0; i <= k; i++) {
        uint64_t lo = i < k ? out[i][0] : g1, hi = i < k ? out[i][1] : g1;
        if (lo > run) {
            VkMemSpan part = *sp;
            part.offset = sp->offset + (run - g0);
            gpu_wrote_range(&part, lo - run, shader);
        }
        if (i < k) {
            if (lz_nskip == lz_cskip) grow((void **)&lz_skip, &lz_cskip, sizeof *lz_skip);
            LzSkip k = {*sp, hi - lo, shader};
            k.sp.offset = sp->offset + (lo - g0);
            lz_skip[lz_nskip++] = k;
            lz_skipped += hi - lo;
        }
        run = hi;
    }
}

static void lazy_part_check(void) {
    if (lz_win || !lz_nskip) return;
    mem_lazy_window(1);
    lz_win = 1;
    for (int i = 0; i < lz_nskip; i++) {
        const PoolRec *r = lz_skip[i].sp.pool;
        if (!mem_lazy_armed(r->import_lo + lz_skip[i].sp.offset, lz_skip[i].size))
            gpu_wrote_range(&lz_skip[i].sp, lz_skip[i].size, lz_skip[i].shader);
    }
}

void vk_mem_lazy_part_end(void) { lazy_part_check(); }

void vk_mem_lazy_handed(void) {
    lz_nskip = 0;
    if (lz_win) mem_lazy_window(0), lz_win = 0;
}

static void gpu_wrote_range(const VkMemSpan *sp, uint64_t size, int shader) {
    PoolRec *r = sp->pool;
    if (!r || r->mstate != MS_ON || sp->buffer != r->mbuf || !size) return;
    r->vd_wrote_sub = msub, r->vd_wrote_n++;
    r->hot_bytes += size;
    uint64_t o = 0;
    if (shader && !(sp->offset & 3) && !(size & 3) && merging()) {
        if (size > DIFF_MIN && size <= DIFF_MAX && wb_diff()) {
            if (snap_add(r, sp->offset, size, 1)) o = size;
        } else if (size <= merge_max())
            for (uint64_t n; o < size; o += n)
                if (!snap_add(r, sp->offset + o, n = size - o < MERGE_PIECE ? size - o : MERGE_PIECE, 0)) break;
    }
    r->st_merge += o;
    if (o == size) return;
    r->st_wb += size - o;
    range_add(&wbl, &nwbl, &cwbl, r, sp->offset + o, size - o);
}
void vk_mem_gpu_wrote(const VkMemSpan *sp, uint64_t size) {
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&use_lock);
    gpu_wrote(sp, size, 0);
    if (lk) plat_mutex_unlock(&use_lock);
}
void vk_mem_gpu_wrote_shader(const VkMemSpan *sp, uint64_t size) {
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&use_lock);
    gpu_wrote(sp, size, 1);
    if (lk) plat_mutex_unlock(&use_lock);
}

static int flush_add(FlushRange *l, int n, uint64_t a, uint64_t b, int cap) {
    int i = 0;
    while (i < n && l[i].hi < a) i++;
    int j = i;
    for (; j < n && l[j].lo <= b; j++) {
        if (l[j].lo < a) a = l[j].lo;
        if (l[j].hi > b) b = l[j].hi;
    }
    memmove(l + i + 1, l + j, (size_t)(n - j) * sizeof *l);
    l[i] = (FlushRange){a, b};
    n += 1 - (j - i);
    if (n > cap) {
        int k = 0;
        for (int x = 1; x + 1 < n; x++)
            if (l[x + 1].lo - l[x].hi < l[k + 1].lo - l[k].hi) k = x;
        l[k].hi = l[k + 1].hi;
        memmove(l + k + 1, l + k + 2, (size_t)(n - k - 2) * sizeof *l);
        n--;
    }
    return n;
}

int vk_mem_flush_add_probe(void) {
    FlushRange l[5];
    int n = 0;
    n = flush_add(l, n, 0x8000, 0x9000, 4);
    n = flush_add(l, n, 0x1000, 0x2000, 4);
    n = flush_add(l, n, 0x4000, 0x5000, 4);
    if (n != 3 || l[0].lo != 0x1000 || l[1].lo != 0x4000 || l[2].hi != 0x9000) return 1;
    n = flush_add(l, n, 0x2000, 0x3000, 4);
    if (n != 3 || l[0].hi != 0x3000) return 2;
    n = flush_add(l, n, 0x800, 0x8800, 4);
    if (n != 1 || l[0].lo != 0x800 || l[0].hi != 0x9000) return 3;
    n = 0;
    n = flush_add(l, n, 0x10000, 0x11000, 2);
    n = flush_add(l, n, 0x40000, 0x41000, 2);
    n = flush_add(l, n, 0x12000, 0x13000, 2);
    if (n != 2 || l[0].lo != 0x10000 || l[0].hi != 0x13000 || l[1].lo != 0x40000) return 4;
    n = 0;
    n = flush_add(l, n, 0x10000, 0x11000, 1);
    n = flush_add(l, n, 0x40000, 0x41000, 1);
    if (n != 1 || l[0].lo != 0x10000 || l[0].hi != 0x41000) return 5;
    return 0;
}

uint64_t vk_mem_mirror_uploaded(void) {
    plat_mutex_lock(&mirror_stat_lock);
    uint64_t n = macc.upload_bytes;
    plat_mutex_unlock(&mirror_stat_lock);
    return n;
}

void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size) {
    if (!vk_mirror_on || !size || !p->mem) return;
    plat_inc64(&m_flush_calls);
    plat_add64(&m_flush_bytes, (int64_t)size);
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)p->mem, 0);
    PoolRec *r = e && e->key == p ? e->buf : NULL;
    if (r && r->mstate == MS_ON) {
        uint64_t a = ((uintptr_t)p->mem + off) & ~0xfffull, b = ((uintptr_t)p->mem + off + size + 0xfff) & ~0xfffull;
        plat_mutex_lock(&flush_lock);
        __atomic_store_n(&r->fl_n, flush_add(r->fl, r->fl_n, a, b, flush_cap), __ATOMIC_RELEASE);
        r->fl_calls++, r->fl_sum += size;
        plat_mutex_unlock(&flush_lock);
    }
    plat_mutex_unlock_shared(&pool_lock);
}

static uint64_t freeze_budget_ns = ~0ull;
static uint64_t frz_quiet_ns, frz_load_hold_ns;
static FrzHold frz;
static uint64_t frz_held_passes, frz_slow, frz_load_held;

static void freeze_pass(void) {
    if (freeze_budget_ns == ~0ull) {
        long long us = cfg_int("NX2SARIA_FREEZE_BUDGET_US");
        freeze_budget_ns = us > 0 ? (uint64_t)us * 1000 : 0;
        long long q = cfg_int("NX2SARIA_FREEZE_QUIET_MS"), h = cfg_int("NX2SARIA_FREEZE_LOAD_HOLD_MS");
        frz_quiet_ns = q > 0 ? (uint64_t)q * 1000000 : 0;
        frz_load_hold_ns = h > 0 && freeze_budget_ns ? (uint64_t)h * 1000000 : 0;
    }
    uint64_t start = qpc();
    if (freeze_budget_ns && !frz_may_run(&frz, start, frz_load_hold_ns, vk_exec_pool_loading(), perf_area_load_ns())) {
        if (nfcand) {
            frz_held_passes++;
            if (start >= frz.hold_until) frz_load_held++;
        }
        return;
    }
    for (int done = 0; nfcand && done < FREEZE_PER_SUB;) {
        uint64_t j = fcand[--nfcand], a = j << REG_SHIFT;
        reg_queued[j] = 0;
        if (ww_ep - reg_last[j] < (uint32_t)freeze_idle || mem_frozen(a)) continue;
        done++;
        uint64_t t0 = qpc();
        int ok = mem_freeze(a);
        uint64_t t1 = qpc();
        slow_mem_call("freeze", a, 1ull << REG_SHIFT, 0, t1 - t0);
        if (ok) query_stamp(a, a + (1ull << REG_SHIFT), gen_next());
        else n_frz_refused++;
        if (!freeze_budget_ns) continue;
        frz_freeze_done(&frz, t1, t1 - t0, frz_quiet_ns);
        if (t1 - t0 >= FRZ_SLOW_NS) {
            frz_slow++;
            static uint32_t logged;
            if (++logged <= 40)
                rt_log("mem: freezing an idle region took %.1f ms: no freezes for %.0f ms (%llu slow freezes so far, %llu "
                       "passes held, %llu of them by area loads, %d regions waiting)", (t1 - t0) / 1e6, frz.backoff / 1e6,
                       (unsigned long long)frz_slow, (unsigned long long)frz_held_passes,
                       (unsigned long long)frz_load_held, nfcand);
            break;
        }
        if (qpc() - start >= freeze_budget_ns) break;
    }
}

#define HOT_UP_MAX (64ull << 10)
#define HOT_FRAMES 120
#define HOT_MIN (1ull << 20)
static int mirror_hot = -1;
static uint64_t hot_frame0, n_demoted;

static void hot_check(void) {
    if (mirror_hot < 0) {
        const char *v = cfg_value("NX2SARIA_MIRROR_HOT");
        int p = v && *v ? atoi(v) : 0;
        mirror_hot = p < 0 ? 0 : p > 1000 ? 1000 : p;
        if (mirror_hot) rt_log("mem: pools copied %d%% of their size a frame stay in system memory (NX2SARIA_MIRROR_HOT)", mirror_hot);
    }
    if (!mirror_hot) return;
    uint64_t fr = mirror_frame;
    if (!hot_frame0 || fr < hot_frame0) {
        hot_frame0 = fr ? fr : 1;
        return;
    }
    if (fr - hot_frame0 < HOT_FRAMES || npend || nwbl) return;
    double frames = (double)(fr - hot_frame0);
    hot_frame0 = fr;
    PoolRec *cand[8];
    uint64_t rel[8][3];
    int nc = 0;
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < nm; i++) {
        PoolRec *r = mlist[i];
        uint64_t n = r->import_hi - r->import_lo, per = (uint64_t)(r->hot_bytes / frames), up = (uint64_t)(r->hot_up / frames);
        r->hot_bytes = (uint64_t)per;
        r->hot_up = 0;
        if (nc == 8 || per < HOT_MIN || per * 100 < n * (uint64_t)mirror_hot || up > HOT_UP_MAX || !r->mbuf ||
            (!r->twin && r->gpa_rec)) {
            r->hot_bytes = 0;
            continue;
        }
        rel[nc][0] = r->import_lo, rel[nc][1] = r->import_hi, rel[nc][2] = (uint64_t)(uintptr_t)r->mbuf;
        cand[nc++] = r;
    }
    if (!nc) {
        plat_mutex_unlock(&pool_lock);
        return;
    }
    if (evict_unlocked()) {
        plat_mutex_unlock(&pool_lock);
        mem_lazy_release((const uint64_t (*)[3])rel, nc);
        plat_mutex_lock(&pool_lock);
    }
    struct { uint64_t lo, n, per; uint32_t flags; } note[8];
    VkBuffer dead[8];
    VmaAllocation dead_a[8];
    int nout = 0;
    for (int k = 0; k < nc; k++) {
        PoolRec *r = cand[k];
        int on = 0;
        for (int i = 0; i < nm && !on; i++) on = mlist[i] == r;
        if (!on || r->mbuf != (VkBuffer)(uintptr_t)rel[k][2]) continue;
        mem_lazy_release((const uint64_t (*)[3])&rel[k], 1);
        note[nout].lo = r->lo, note[nout].n = r->import_hi - r->import_lo, note[nout].per = r->hot_bytes;
        note[nout].flags = r->flags;
        r->hot_bytes = 0;
        mirror_forget(r);
        r->mstate = MS_REFUSED, r->retry_sub = 0;
        plat_mutex_lock(&flush_lock);
        __atomic_store_n(&r->fl_n, 0, __ATOMIC_RELEASE);
        r->fl_calls = r->fl_sum = 0;
        plat_mutex_unlock(&flush_lock);
        if (r->gpa_rec) r->gpa_rec = vk_gpa_remap_plain(r->import_lo, r->import_hi, r->gpa_rec, r->buf, r->import_lo);
        dead[nout] = r->mbuf, dead_a[nout] = r->malloc_;
        r->mbuf = VK_NULL_HANDLE, r->malloc_ = NULL;
        nout++;
    }
    plat_mutex_unlock(&pool_lock);
    for (int k = 0; k < nout; k++) {
        vk_defer_buffer(dead[k], dead_a[k]);
        char f[160];
        flag_names(note[k].flags, f, sizeof f);
        if (++n_demoted <= 32)
            rt_log("mem: pool %#llx (%.1f MiB, %s) copied %.1f MiB a frame through its mirror: system memory from now on",
                   (unsigned long long)note[k].lo, note[k].n / 1048576.0, f, note[k].per / 1048576.0);
    }
}

void vk_mem_mirror_begin(void) {
    if (vk_mirror_on) hot_check();
    if (!vk_mirror_on) {
        if (!freeze_idle) return;
        plat_mutex_lock(&gen_lock);
        ww_ep++;
        freeze_pass();
        plat_mutex_unlock(&gen_lock);
        return;
    }
    uint64_t t0 = qpc();
    nsnl = snl_copied = nchunks = 0;
    uint32_t prev = msub;
    msub = msub + 1 ? msub + 1 : 1;
    __atomic_add_fetch(&mirror_gen, 1, __ATOMIC_RELEASE);
    mepoch = vk_texture_epoch();
    uint64_t q = 0, run0 = 0, run1 = 0;
    int have = 0;
    plat_mutex_lock_shared(&pool_lock);
    plat_mutex_lock(&gen_lock);
    ww_ep++;
    uint64_t b0 = ww_bytes;
    int pl = sub_place(), pp = presnap_place();
    if (pl <= pplace) pframe++;
    pplace = pl;
    for (int i = 0; i < nm; i++) {
        const PoolRec *r = mlist[i];
        if (pp ? !(pm_last(r) >> pl & 1) : acc_of(r) != prev) continue;
        uint64_t a = r->import_lo >> SNAP_SHIFT, b = (r->import_hi - 1) >> SNAP_SHIFT;
        if (have && a <= run1 + 1 + (uint64_t)mirror_gap) {
            if (b > run1) run1 = b;
            continue;
        }
        if (have) q += snap_regions(run0, run1, mepoch);
        run0 = a, run1 = b, have = 1;
    }
    if (have) q += snap_regions(run0, run1, mepoch);
    mrep.presnap_bytes += ww_bytes - b0;
    if (freeze_idle) freeze_pass();
    plat_mutex_unlock(&gen_lock);
    plat_mutex_unlock_shared(&pool_lock);
    mrep.subs++;
    mrep.presnap_queries += q;
    mrep.presnap_t += qpc() - t0;
    slow_mem_call("mirror begin (its queries and freezes)", 0, ww_bytes - b0, q, qpc() - t0);
}

#define SPAN_MAX 64
static void span_add(struct WbSpan **v, int *n, int *cap, uint64_t lo, uint64_t hi, uint64_t fence) {
    if (*n && (*v)[0].fence <= vk_sync_fence_done()) {
        uint64_t done = vk_sync_fence_done();
        int k = 0;
        for (int i = 0; i < *n; i++)
            if ((*v)[i].fence > done) (*v)[k++] = (*v)[i];
        *n = k;
    }
    struct WbSpan *l = *n ? &(*v)[*n - 1] : NULL;
    if (l && l->fence == fence && ((lo <= l->hi && hi >= l->lo) || *n >= SPAN_MAX)) {
        if (lo < l->lo) l->lo = lo;
        if (hi > l->hi) l->hi = hi;
        return;
    }
    if (*n == *cap) grow((void **)v, cap, sizeof **v);
    (*v)[(*n)++] = (struct WbSpan){lo, hi, fence};
}

static uint64_t span_wait(const struct WbSpan *v, int n, uint64_t lo, uint64_t hi, uint64_t fence) {
    uint64_t w = 0;
    for (int i = 0; i < n; i++)
        if (v[i].fence < fence && v[i].fence > w && v[i].lo < hi && lo < v[i].hi) w = v[i].fence;
    return w;
}

static void cpu_span_add(PoolRec *r, uint64_t lo, uint64_t hi, uint64_t fence) {
    span_add(&r->cws, &r->ncws, &r->ccws, lo, hi, fence);
}

static uint64_t cpu_span_wait(const PoolRec *r, uint64_t lo, uint64_t hi, uint64_t fence) {
    return r->ncws ? span_wait(r->cws, r->ncws, lo, hi, fence) : 0;
}

int vk_mem_mirror_pending(void) { return npend || snl_copied < nsnl || n_gpa_flip; }

#define CAPTURE_STAGE_KB (64 << 10)
static volatile int32_t mirror_live;

static int stage_on(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("MIRROR_STAGE", "0") != 0;
    return on;
}

static uint64_t capture_cap(void) {
    static int64_t cap = -1;
    if (cap < 0) {
        int kb = cfg_int("NX2SARIA_MIRROR_CAPTURE_KB");
        if (kb <= 0 && stage_on()) kb = CAPTURE_STAGE_KB;
        cap = kb > 0 ? (int64_t)kb << 10 : 0;
        if (cap)
            rt_log("mem: mirror pages the CPU wrote are copied out of guest memory when their part is handed over, up to "
                   "%d KiB a part (NX2SARIA_MIRROR_CAPTURE_KB, NX2SARIA_MIRROR_STAGE)", kb);
    }
    return (uint64_t)cap;
}

void vk_mem_note_live(void) { __atomic_store_n(&mirror_live, 1, __ATOMIC_RELAXED); }
int vk_mem_live_swap(int v) { return __atomic_exchange_n(&mirror_live, v, __ATOMIC_RELAXED); }
static int wb_lazy(void);
int vk_mem_wb_lazy(void) { return wb_lazy(); }

int vk_mem_take_live(void) {
    int live = __atomic_exchange_n(&mirror_live, 0, __ATOMIC_RELAXED);
    if (!vk_mirror_on || shadow_on || !capture_cap()) live = 1;
    return live;
}

static MRange *capm;
static uint64_t *caps;
static uint8_t **capd, *capok;
static uint64_t (*capsrc)[2];
static int ccapm, ccaps, ccapd, ccapsrc, ccapok;

static void mirror_capture(VkCommandBuffer cb) {
    uint64_t cap = capture_cap();
    if (!cap || shadow_on || !npend) return;
    uint64_t t0 = qpc(), done = vk_sync_fence_done(), total = 0;
    qsort(pend, (size_t)npend, sizeof *pend, range_cmp);
    int n = 0;
    for (int i = 0; i < npend; i++) {
        PoolRec *r = pend[i].r;
        uint64_t lim = r->import_hi - r->import_lo, lo = pend[i].off, hi = pend[i].off + pend[i].size;
        if (hi > lim) hi = lim;
        if (lo >= hi) continue;
        if (n && capm[n - 1].r == r && lo <= capm[n - 1].off + capm[n - 1].size) {
            if (hi > capm[n - 1].off + capm[n - 1].size) capm[n - 1].size = hi - capm[n - 1].off;
            continue;
        }
        if (n == ccapm) grow((void **)&capm, &ccapm, sizeof *capm);
        capm[n++] = (MRange){r, lo, hi - lo};
    }
    while (ccaps < ccapm) grow((void **)&caps, &ccaps, sizeof *caps);
    for (int i = 0; i < n; i++) {
        const PoolRec *r = capm[i].r;
        uint64_t sz = (capm[i].size + 15) & ~15ull;
        caps[i] = UINT64_MAX;
        if (r->twin || r->wb_fence > done || total + sz > cap) continue;
        caps[i] = total;
        total += sz;
    }
    int k = total ? chunk_new(total, CHUNK_LIST) : -1;
    uint8_t *map = NULL;
    if (k >= 0) {
        chunks[k].used = UINT64_MAX;
        VmaAllocationInfo info;
        vmaGetAllocationInfo(vk_ctx()->vma, chunks[k].alloc, &info);
        map = info.pMappedData;
    }
    while (ccapd < ccapm) grow((void **)&capd, &ccapd, sizeof *capd);
    while (ccapsrc < ccapm) grow((void **)&capsrc, &ccapsrc, sizeof *capsrc);
    while (ccapok < ccapm) grow((void **)&capok, &ccapok, sizeof *capok);
    int nc = 0;
    for (int i = 0; map && i < n; i++)
        if (caps[i] != UINT64_MAX) {
            capd[nc] = map + caps[i];
            capsrc[nc][0] = capm[i].r->import_lo + capm[i].off, capsrc[nc][1] = capm[i].size;
            nc++;
        }
    if (nc) mem_copy_readable(capd, (const uint64_t (*)[2])capsrc, nc, capok);
    npend = 0;
    for (int i = 0, c = 0; i < n;) {
        PoolRec *r = capm[i].r;
        int nr = 0;
        for (; i < n && capm[i].r == r; i++) {
            int got = map && caps[i] != UINT64_MAX && capok[c++];
            if (got) {
                if (nr == cregions) grow((void **)&mregions, &cregions, sizeof *mregions);
                mregions[nr++] = (VkBufferCopy){caps[i], capm[i].off, capm[i].size};
                mrep.cap_bytes += capm[i].size, mrep.cap_ranges++;
                continue;
            }
            range_add(&pend, &npend, &cpend, r, capm[i].off, capm[i].size);
            mrep.cap_left += capm[i].size;
        }
        if (nr) vkCmdCopyBuffer(cb, chunks[k].buf, r->mbuf, (uint32_t)nr, mregions);
    }
    if (map) vmaFlushAllocation(vk_ctx()->vma, chunks[k].alloc, 0, VK_WHOLE_SIZE);
    mrep.cap_t += qpc() - t0;
}

void vk_mem_mirror_prologue(VkCommandBuffer cb) {
    if (!npend && snl_copied == nsnl && !n_gpa_flip) return;
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (npend && merge_cpu_mode()) {
        uint64_t aw = 0, f = vk_sync_rec_fence(), w;
        for (int i = 0; i < npend; i++)
            if ((w = cpu_span_wait(pend[i].r, pend[i].off, pend[i].off + pend[i].size, f)) > aw) aw = w;
        apply_wait(aw, 1);
    }
    mirror_capture(cb);
    if (npend) vk_mem_note_live();
    copy_ranges(cb, pend, npend, 1, &mrep.upload_bytes, &mrep.upload_copies);
    if (snl_copied < nsnl) {
        if (npend) mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                            VK_ACCESS_2_TRANSFER_READ_BIT);
        snap_copies(cb);
    }
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    gpa_flip_cmds(cb);
    npend = 0;
    mrep.prologues++;
}

typedef struct { PoolRec *r; uint64_t off, len, dst, mgen; } VSample;
typedef struct { uint32_t sub; int n; VkBuffer buf; VmaAllocation alloc; const uint8_t *map; VSample s[VERIFY_SAMPLES]; } VJob;
static int ver_i;
static uint64_t ver_off;

static int page_dirty(uint64_t a, uint64_t mgen) {
    uint64_t *s = gen_slot(a, 0);
    if (s && __atomic_load_n(s, __ATOMIC_RELAXED) > mgen) return 1;
    uint64_t p[1];
    return mem_written_peek(a, 1ull << GEN_PAGE_SHIFT, p, 1) != 0;
}

static void verify_cb(void *arg) {
    VJob *j = arg;
    static volatile int32_t logged;
    uint64_t pages = 0, skipped = 0, bad = 0;
    for (int i = 0; i < j->n; i++) {
        const VSample *s = &j->s[i];
        PoolRec *r = s->r;
        uint32_t wb = __atomic_load_n(&r->wb_sub, __ATOMIC_RELAXED);
        if (wb != j->sub && wb - j->sub < 0x80000000u) {
            skipped += s->len >> GEN_PAGE_SHIFT;
            continue;
        }
        for (uint64_t o = 0; o < s->len; o += 1ull << GEN_PAGE_SHIFT) {
            uint64_t a = r->import_lo + s->off + o;
            const uint8_t *m = j->map + s->dst + o, *g = (const uint8_t *)(uintptr_t)a;
            pages++;
            if (!memcmp(m, g, 1ull << GEN_PAGE_SHIFT)) continue;
            plat_mutex_lock_shared(&gen_lock);
            int dirty = page_dirty(a, s->mgen);
            plat_mutex_unlock_shared(&gen_lock);
            if (dirty) { skipped++; continue; }
            bad++;
            if (plat_inc32(&logged) <= VERIFY_LOG) {
                int k = 0, e = (1 << GEN_PAGE_SHIFT) - 1;
                while (k < e && m[k] == g[k]) k++;
                while (e > k && m[e] == g[e]) e--;
                char f[160];
                flag_names(r->flags, f, sizeof f);
                rt_log("mem: verify mismatch: pool %#llx+%#llx (%s) offset %#llx: bytes %#x..%#x of the page differ, "
                       "mirror %02x guest %02x at the first (submission %u)", (unsigned long long)r->lo,
                       (unsigned long long)r->size, f, (unsigned long long)(a - r->lo), k, e, m[k], g[k], j->sub);
                const uint64_t *gs = gen_slot(a & ~((1ull << GEN_PAGE_SHIFT) - 1), 0);
                char mh[100], gh[100];
                for (int b = 0; b < 16; b++) snprintf(mh + 3 * b, 4, "%02x ", m[b]), snprintf(gh + 3 * b, 4, "%02x ", g[b]);
                rt_log("mem: verify mismatch detail: written binds %llu, last %u; used %u; written back %u; gpa record %u; "
                       "page cpu stamp %llu (synced to %llu, now %llu); mirror %s| guest %s",
                       (unsigned long long)r->vd_wrote_n, r->vd_wrote_sub, __atomic_load_n(&r->vd_use_sub, __ATOMIC_RELAXED),
                       wb, r->gpa_rec,
                       (unsigned long long)(gs ? __atomic_load_n(gs, __ATOMIC_RELAXED) : 0),
                       (unsigned long long)s->mgen, (unsigned long long)__atomic_load_n(&gen_now, __ATOMIC_RELAXED),
                       mh, gh);
            }
        }
    }
    plat_add64(&v_pages, (int64_t)pages);
    plat_add64(&v_skipped, (int64_t)skipped);
    plat_add64(&v_mismatch, (int64_t)bad);
    vmaDestroyBuffer(vk_ctx()->vma, j->buf, j->alloc);
    free(j);
}

static int verify_record(VkCommandBuffer cb, uint64_t fence) {
    VJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk: out of memory");
    uint64_t left = VERIFY_BYTES, dst = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int tries = 0; nm && left && j->n < VERIFY_SAMPLES && tries <= nm; tries++) {
        if (ver_i >= nm) ver_i = 0, ver_off = 0;
        PoolRec *r = mlist[ver_i];
        uint64_t size = r->import_hi - r->import_lo;
        if (ver_off >= size) { ver_i++, ver_off = 0; continue; }
        uint64_t len = size - ver_off < left ? size - ver_off : left;
        j->s[j->n++] = (VSample){r, ver_off, len, dst, r->mgen};
        dst += len, left -= len, ver_off += len;
        if (ver_off >= size) ver_i++, ver_off = 0;
    }
    plat_mutex_unlock_shared(&pool_lock);
    if (!j->n) { free(j); return 0; }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = dst,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS) { free(j); return 0; }
    j->map = info.pMappedData;
    j->sub = msub;
    mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
             VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int i = 0; i < j->n; i++) {
        VkBufferCopy c = {j->s[i].off, j->s[i].dst, j->s[i].len};
        vkCmdCopyBuffer(cb, j->s[i].r->mbuf, j->buf, 1, &c);
    }
    mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
             VK_ACCESS_2_HOST_READ_BIT);
    vk_completion_add(fence, verify_cb, j);
    return j->n + 2;
}

int vk_mem_mirror_wb_pending(void) { return vk_mirror_on && (nsnl || nwbl); }

int vk_mem_mirror_end(VkCommandBuffer cb, uint64_t fence) {
    if (!vk_mirror_on) return 0;
    int rec = 0;
    int listed = 0;
    lazy_part_check();
    if (nsnl) {
        mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vk_gpu_gap_note(VK_GAP_WB_MERGE, 0);
        int m = wb_merge_record(cb, fence, &listed);
        if (!m) {
            for (int i = 0; i < nsnl; i++) range_add(&wbl, &nwbl, &cwbl, snl[i].r, snl[i].off, snl[i].size);
        }
        for (int i = 0; i < nsnl; i++) snl[i].r->wb_fence = fence;
        rec += m + 1;
        mbarrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }
    if (merge_cpu_mode()) {
        uint64_t hw = 0, w;
        for (int i = 0; i < nsnl; i++)
            if ((snl[i].diff || !listed) && (w = cpu_span_wait(snl[i].r, snl[i].off, snl[i].off + snl[i].size, fence)) > hw)
                hw = w;
        for (int i = 0; i < nwbl; i++)
            if ((w = cpu_span_wait(wbl[i].r, wbl[i].off, wbl[i].off + wbl[i].size, fence)) > hw) hw = w;
        apply_wait(hw, 0);
    }
    if (shadow_on && (nswl || nwbl)) swb_job(fence);
    if (nchunks) {
        SnapFree *f = malloc(sizeof *f + sizeof(SnapChunk) * (size_t)nchunks);
        if (!f) rt_fatal("vk: out of memory");
        f->n = nchunks;
        memcpy(f->c, chunks, sizeof(SnapChunk) * (size_t)nchunks);
        vk_completion_add(fence, snap_free, f);
    }
    if (nsnl || nwbl) vk_exec_note_results(VK_RES_WRITEBACK);
    if (nwbl) {
        mbarrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
        for (int i = 0; i < nwbl; i++) wbl[i].r->wb_sub = msub, wbl[i].r->wb_fence = fence;
        vk_gpu_gap_note(VK_GAP_WB_COPY, 0);
        mrep.wb_ranges += (uint64_t)nwbl;
        rec += copy_ranges(cb, wbl, nwbl, 0, &mrep.wb_bytes, &mrep.wb_copies) + 2;
        mbarrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        nwbl = 0;
    }
    if (mirror_verify) rec += verify_record(cb, fence);
    plat_mutex_lock(&mirror_stat_lock);
    uint64_t *d = (uint64_t *)&macc, *s = (uint64_t *)&mrep;
    for (size_t i = 0; i < sizeof macc / sizeof(uint64_t); i++) d[i] += s[i];
    plat_mutex_unlock(&mirror_stat_lock);
    memset(&mrep, 0, sizeof mrep);
    return rec;
}

#define POOL_LOG_TOP 12
static uint64_t pool_score(const PoolRec *r) { return r->st_pages + r->st_flush + r->st_wb + r->st_merge; }
static void mirror_pool_log(uint64_t frame) {
    static uint64_t last;
    double fr = frame > last ? (double)(frame - last) : 1.0;
    last = frame;
    PoolRec *top[POOL_LOG_TOP];
    int nt = 0;
    uint64_t all_up = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < nm; i++) {
        PoolRec *r = mlist[i];
        all_up += r->st_pages + r->st_flush;
        uint64_t sc = pool_score(r);
        if (!sc || (nt == POOL_LOG_TOP && sc <= pool_score(top[nt - 1]))) continue;
        int k = nt < POOL_LOG_TOP ? nt++ : nt - 1;
        for (; k > 0 && pool_score(top[k - 1]) < sc; k--) top[k] = top[k - 1];
        top[k] = r;
    }
    rt_log("mem: mirror pools frame %llu: %d mirrored, %.1f MiB a frame copied up in all; the top %d (MiB a frame: written pages, "
           "flush ranges (overlap with the pages counted twice), syncs a frame, GPU writes back merged / copied):",
           (unsigned long long)frame, nm, all_up / fr / 1048576.0, nt);
    for (int i = 0; i < nt; i++) {
        const PoolRec *r = top[i];
        char f[160];
        flag_names(r->flags, f, sizeof f);
        rt_log("mem:   pool %#llx %.1f MiB %s: up %.2f + %.2f, %.1f syncs, back %.2f / %.2f", (unsigned long long)r->lo,
               r->size / 1048576.0, f, r->st_pages / fr / 1048576.0, r->st_flush / fr / 1048576.0, r->st_syncs / fr,
               r->st_merge / fr / 1048576.0, r->st_wb / fr / 1048576.0);
    }
    for (int i = 0; i < nm; i++) {
        PoolRec *r = mlist[i];
        r->st_pages = r->st_flush = r->st_wb = r->st_merge = 0, r->st_syncs = 0;
    }
    plat_mutex_unlock_shared(&pool_lock);
}

static void mirror_log(uint64_t frame) {
    static uint64_t last, ww_last;
    plat_mutex_lock(&gen_lock);
    uint64_t wc = ww_calls, wb = ww_bytes, wt = ww_ticks, dc = ww_dirty_calls, dt = ww_dirty_ticks, wp = ww_pages;
    ww_pages = 0;
    uint64_t nr = n_frz_refused;
    ww_calls = ww_bytes = ww_ticks = ww_dirty_calls = ww_dirty_ticks = n_frz_refused = 0;
    plat_mutex_unlock(&gen_lock);
    double wf = frame > ww_last ? (double)(frame - ww_last) : 1.0;
    ww_last = frame;
    uint64_t fz, ff, ft, fo, fh;
    mem_freeze_stats(&fz, &ff, &ft, &fo, &fh);
    mem_probe_report(frame);
    mem_lazy_report(frame, lz_skipped);
    lz_skipped = 0;
    static int touch = -1;
    if (touch < 0) touch = cfg_int("NX2SARIA_WB_LAZY_TOUCH");
    if (touch > 0) {
        uint64_t at[16];
        int k = mem_lazy_pick(at, touch > 16 ? 16 : touch);
        for (int i = 0; i < k; i++) {
            volatile uint8_t v = *(volatile const uint8_t *)(uintptr_t)at[i];
            (void)v;
        }
    }
    if (rt_trace & RT_TRACE_PERF)
        rt_log("mem: write-watch frame %llu: per frame, every consumer: %.1f queries over %.1f MiB, %.3f ms (%.1f found "
               "%.0f written pages, %.3f ms); frozen %.0f MiB; since the last line: freezes %llu, thaws by a write %llu, by a file read "
               "or mapping change %llu, refused %llu; quick thaws that held a hot region thawed %llu",
               (unsigned long long)frame, wc / wf, wb / wf / 1048576.0, qpc_ms(wt) / wf, dc / wf, wp / wf, qpc_ms(dt) / wf,
               fz * 2.0, (unsigned long long)ff, (unsigned long long)ft, (unsigned long long)fo, (unsigned long long)nr,
               (unsigned long long)fh);
    if (!vk_mirror_on) return;
    plat_mutex_lock(&mirror_stat_lock);
    MirrorAcc a = macc;
    memset(&macc, 0, sizeof macc);
    plat_mutex_unlock(&mirror_stat_lock);
    double f = frame > last ? (double)(frame - last) : 1.0, M = 1048576.0;
    last = frame;
    uint64_t fc = (uint64_t)plat_xchg64(&m_flush_calls, 0), fb = (uint64_t)plat_xchg64(&m_flush_bytes, 0);
    char ver[160] = "";
    if (mirror_verify)
        snprintf(ver, sizeof ver, "; verify: pages compared %lld, skipped %lld (written since), mismatches %lld",
                 (long long)plat_xchg64(&v_pages, 0), (long long)plat_xchg64(&v_skipped, 0),
                 (long long)plat_xchg64(&v_mismatch, 0));
    vk_gpa_report();
    rt_log("mem: mirror frame %llu: pools %lld, %.1f MiB device-local; stay imported: budget %lld (%.1f MiB), untracked "
           "%lld, overlapping %lld, failed %lld; per frame: submits %.2f, syncs %.1f, upload %.3f MiB in %.1f ranges, "
           "first use %.2f pools %.3f MiB, write-back %.3f MiB in %.1f ranges, merge %.3f MiB in %.1f dispatches, by "
           "difference %.3f MiB in %.1f (snapshots %.3f MiB, new buffers %.2f), flush calls %.1f (%.3f MiB), queries "
           "%.1f pre-snap (%.1f MiB) + %.1f sync, prologues %.2f, tracking cpu %.3f ms (pre-snap %.3f, syncs %.3f); so far "
           "evicted %lld (%.1f MiB), budget refusals asked again %lld, made without a twin %lld (%.1f MiB)%s",
           (unsigned long long)frame, (long long)m_live_n, m_live_bytes / M, (long long)m_refused_n[MR_BUDGET],
           m_refused_bytes[MR_BUDGET] / M, (long long)m_refused_n[MR_UNTRACKED], (long long)m_refused_n[MR_OVERLAP],
           (long long)m_refused_n[MR_ALLOC], a.subs / f, a.syncs / f, a.upload_bytes / f / M, a.upload_copies / f,
           a.first_n / f, a.first_bytes / f / M, a.wb_bytes / f / M, a.wb_copies / f, a.merge_bytes / f / M,
           a.merge_dispatches / f, a.diff_bytes / f / M, a.diff_dispatches / f, a.snap_bytes / f / M, a.snap_allocs / f,
           fc / f, fb / f / M,
           a.presnap_queries / f, a.presnap_bytes / f / M, a.sync_queries / f, a.prologues / f, qpc_ms(a.presnap_t + a.sync_t) / f,
           qpc_ms(a.presnap_t) / f, qpc_ms(a.sync_t) / f, (long long)m_evicted_n, m_evicted_bytes / M, (long long)m_retries,
           (long long)m_untwinned_n, m_untwinned_bytes / M, ver);
    char capl[240] = "";
    if (wb_skip()) {
        snprintf(capl, sizeof capl, "; written ranges kept in the mirror alone %.3f MiB",
                 (uint64_t)plat_xchg64(&m_wb_skipped, 0) / f / M);
    }
    if (capture_cap())
        snprintf(capl + strlen(capl), sizeof capl - strlen(capl), "; copied by the CPU at hand-over %.3f MiB in %.1f "
                 "ranges, %.3f ms; left to the GPU copy %.3f MiB", a.cap_bytes / f / M, a.cap_ranges / f,
                 qpc_ms(a.cap_t) / f, a.cap_left / f / M);
    rt_log("mem: mirror frame %llu: upload sources per frame, overlaps counted twice: written pages %.3f MiB; flush ranges "
           "%.2f, %.3f MiB, from %.1f flushes of %.3f MiB; flushed and not copied (write-watched) %.3f MiB%s",
           (unsigned long long)frame, a.page_bytes / f / M, a.flush_n / f,
           a.flush_bytes / f / M, a.flush_calls / f, a.flush_sum / f / M, a.flush_skipped / f / M, capl);
    if (n_gpa_late)
        rt_log("mem: mirror frame %llu: the device address table took up %llu mirrors in queue order, after their first "
               "copy, so far (NX2SARIA_GPA_LATE)", (unsigned long long)frame, (unsigned long long)n_gpa_late);
    if (merge_cpu_mode()) {
        int64_t nl = plat_xchg64(&mc_lists, 0), nw = plat_xchg64(&mc_words, 0), ns = plat_xchg64(&mc_ns, 0), wn[3], wt[3];
        for (int i = 0; i < 3; i++) wn[i] = plat_xchg64(&mc_waits[i], 0), wt[i] = plat_xchg64(&mc_wait_ns[i], 0);
        int64_t ov = plat_xchg64(&mc_over, 0), bad = plat_xchg64(&mc_bad, 0), dif = plat_xchg64(&mc_diff, 0);
        rt_log("mem: mirror frame %llu: merges through the CPU per frame (NX2SARIA_MERGE_CPU): %.2f lists, %.0f words "
               "stored in %.3f ms (%.3f a list); the replay waited for a list %.3f times for a write-back (%.3f ms), %.3f "
               "for a prologue (%.3f ms), %.3f for an executed wait (%.3f ms); words past the room %lld, without a "
               "range %lld, "
               "different from the GPU's (check) %lld", (unsigned long long)frame, nl / f, nw / f, ns / f / 1e6,
               nl ? ns / 1e6 / nl : 0.0, wn[0] / f, wt[0] / f / 1e6, wn[1] / f, wt[1] / f / 1e6, wn[2] / f,
               wt[2] / f / 1e6, (long long)ov, (long long)bad, (long long)dif);
    }
}

typedef struct { VkCtx *c; uint64_t frame; } CensusJob;
static volatile int32_t census_busy;

static void census_main(void *arg) {
    CensusJob j = *(CensusJob *)arg;
    free(arg);
    const double M = 1048576.0;
    uint64_t priv = 0, wc = 0, wcn = 0, now;
    perf_census_walk(1);
    uint64_t w0 = kern_now_ns(), f0 = __atomic_load_n(&span_frame, __ATOMIC_RELAXED);
    int have = plat_mem_census(&priv, &wc, &wcn);
    uint64_t w1 = kern_now_ns(), f1 = __atomic_load_n(&span_frame, __ATOMIC_RELAXED);
    perf_census_walk(0);
    if (have) {
        rt_log("mem: process frame %llu: private %.0f MiB, write-combined %.0f MiB in %llu allocations; the walk from "
               "frame %llu to frame %llu, %.1f ms", (unsigned long long)j.frame, priv / M, wc / M,
               (unsigned long long)wcn, (unsigned long long)f0, (unsigned long long)f1, (w1 - w0) / 1e6);
        uint64_t used = 0, budget = 0;
        int exact = 0;
        if (!vk_mem_vram(&used, &budget, &exact)) used = 0;
        perf_note_wc(wc, used);
        now = wc;
    } else {
        VmaBudget vb[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(j.c->vma, vb);
        now = 0;
        for (uint32_t h = 0; h < j.c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) now += vb[h].statistics.blockBytes;
    }
    static uint64_t base;
    static int dumps = -1;
    if (dumps < 0) base = now, dumps = 0;
    else if (dumps < 4 && now >= base + (2ull << 30) && rt_log_path()) {
        base = now, dumps++;
        const char *lp = rt_log_path(), *sl = strrchr(lp, '/'), *bs = strrchr(lp, '\\');
        if (bs && (!sl || bs > sl)) sl = bs;
        char path[1024];
        snprintf(path, sizeof path, "%.*svma-frame%llu.json", sl ? (int)(sl - lp) + 1 : 0, lp, (unsigned long long)j.frame);
        char *json = NULL;
        vmaBuildStatsString(j.c->vma, &json, VK_TRUE);
        int ok = json && plat_file_write_cached(path, json, strlen(json));
        rt_log("mem: vma detailed map %s %s", ok ? "written to" : "not written:", path);
        if (json) vmaFreeStatsString(j.c->vma, json);
    }
    plat_xchg32(&census_busy, 0);
}

static void heap_log(VkCtx *c, uint64_t frame, const VmaBudget *vb) {
    const double M = 1048576.0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    }
    char line[1536];
    int o = snprintf(line, sizeof line, "mem: heaps frame %llu:", (unsigned long long)frame);
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS && o < (int)sizeof line - 160; h++)
        o += snprintf(line + o, sizeof line - (size_t)o, " heap %u (%s) used %.0f of %.0f MiB, vma %u blocks %.0f MiB, %u allocations %.0f MiB;",
                      h, c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device" : "host",
                      (c->has_memory_budget ? bp.heapUsage[h] : vb[h].usage) / M,
                      (c->has_memory_budget ? bp.heapBudget[h] : vb[h].budget) / M, vb[h].statistics.blockCount,
                      vb[h].statistics.blockBytes / M, vb[h].statistics.allocationCount, vb[h].statistics.allocationBytes / M);
    rt_log("%s", line);
    VmaTotalStatistics ts;
    vmaCalculateStatistics(c->vma, &ts);
    o = snprintf(line, sizeof line, "mem: vma by type frame %llu:", (unsigned long long)frame);
    for (uint32_t t = 0; t < c->mem_props.memoryTypeCount && t < VK_MAX_MEMORY_TYPES && o < (int)sizeof line - 120; t++) {
        const VmaDetailedStatistics *d = &ts.memoryType[t];
        if (!d->statistics.blockCount) continue;
        o += snprintf(line + o, sizeof line - (size_t)o,
                      " type %u: %u blocks %.0f MiB, %u allocations %.0f MiB, largest %.1f MiB, %u free ranges (largest %.1f MiB);",
                      t, d->statistics.blockCount, d->statistics.blockBytes / M, d->statistics.allocationCount,
                      d->statistics.allocationBytes / M, d->allocationSizeMax / M, d->unusedRangeCount,
                      d->unusedRangeCount ? d->unusedRangeSizeMax / M : 0.0);
    }
    rt_log("%s", line);
    static int census_on = -1;
    if (census_on < 0) census_on = cfg_bool("NX2SARIA_MEM_CENSUS");
    if (!census_on || perf_census_walked_frame() == frame || plat_xchg32(&census_busy, 1)) return;
    CensusJob *j = malloc(sizeof *j);
    if (j) *j = (CensusJob){c, frame};
    if (!j || !plat_thread_spawn(census_main, j, "mem census", 0)) free(j), plat_xchg32(&census_busy, 0);
}

static void pool_census_log(uint64_t frame) {
    uint64_t un = 0, ub = 0, nn = 0, nb = 0, mn = 0, mb = 0, in = 0, ib = 0;
    uint32_t sub = __atomic_load_n(&msub, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        if (r->kind != POOL_IMPORT || !r->buf) continue;
        uint64_t n = r->import_hi - r->import_lo;
        if (__atomic_load_n(&r->used, __ATOMIC_RELAXED)) un++, ub += n;
        else nn++, nb += n;
        if (r->mstate != MS_ON) continue;
        mn++, mb += n;
        if (sub - acc_of(r) >= MIRROR_IDLE) in++, ib += n;
    }
    plat_mutex_unlock_shared(&pool_lock);
    const double M = 1048576.0;
    rt_log("mem: pools frame %llu: imported %llu %.0f MiB resolved by the GPU, %llu %.0f MiB never; mirrors %llu %.0f MiB, "
           "idle for %d submissions %llu %.0f MiB", (unsigned long long)frame, (unsigned long long)un, ub / M,
           (unsigned long long)nn, nb / M, (unsigned long long)mn, mb / M, MIRROR_IDLE, (unsigned long long)in, ib / M);
}

static void vram_use_log(VkCtx *c, uint64_t frame, const VmaBudget *vb, const VkTexCensus *t) {
    uint64_t usage = 0, blocks = 0, allocs = 0, phys = 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    }
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            usage += c->has_memory_budget ? bp.heapUsage[h] : vb[h].statistics.blockBytes,
                blocks += vb[h].statistics.blockBytes, allocs += vb[h].statistics.allocationBytes;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < nphys; i++)
        if (phys_list[i]->alloc) phys += phys_list[i]->size;
    plat_mutex_unlock_shared(&pool_lock);
    uint64_t mir = (uint64_t)__atomic_load_n(&m_live_bytes, __ATOMIC_RELAXED), held = vk_headroom_held(),
             scratch = vk_astc_scratch_bytes();
    uint64_t known = t->bytes_live + t->bytes_released + mir + phys + held + scratch;
    int64_t dlss = vk_dlaa_vram();
    char ds[48] = "n/a";
    if (dlss >= 0) snprintf(ds, sizeof ds, "%.0f MiB", dlss / 1048576.0);
    const double M = 1048576.0;
    rt_log("mem: vram by use frame %llu: in use %.0f MiB = VMA blocks %.0f MiB (textures %.0f, released textures %.0f, "
           "VRAM mirrors %.0f, physical pools %.0f, ASTC scratch %.0f, headroom spare %.0f, other %.0f, free inside the "
           "blocks %.0f) + outside VMA %.0f MiB (swapchain, DLSS and frame generation, pipelines, driver; DLSS reports %s)",
           (unsigned long long)frame, usage / M, blocks / M, t->bytes_live / M, t->bytes_released / M, mir / M, phys / M,
           scratch / M, held / M, allocs > known ? (allocs - known) / M : 0.0, blocks > allocs ? (blocks - allocs) / M : 0.0,
           usage > blocks ? (usage - blocks) / M : 0.0, ds);
}

static VkBlockRow *mirror_rows(VkCtx *c, int *n) {
    VkBlockRow *rows = NULL;
    *n = 0;
    plat_mutex_lock_shared(&pool_lock);
    if (nm) rows = malloc(sizeof *rows * (size_t)nm);
    for (int i = 0; rows && i < nm; i++) {
        const PoolRec *r = mlist[i];
        if (!r->malloc_) continue;
        VmaAllocationInfo2 ai;
        vmaGetAllocationInfo2(c->vma, r->malloc_, &ai);
        if (ai.dedicatedMemory) continue;
        rows[(*n)++] = (VkBlockRow){(uint64_t)(uintptr_t)ai.allocationInfo.deviceMemory, ai.blockSize,
                                    ai.allocationInfo.size, ai.allocationInfo.memoryType};
    }
    plat_mutex_unlock_shared(&pool_lock);
    return rows;
}

static void compact_look(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    int n;
    VkBlockRow *rows = mirror_rows(c, &n);
    vk_texture_compact_plan(rows, n);
    free(rows);
}

static void holes_log(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    int n;
    VkBlockRow *rows = mirror_rows(c, &n);
    VkTexHoles h;
    vk_texture_holes(rows, n, &h);
    free(rows);
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t blocks = 0, allocs = 0;
    for (uint32_t i = 0; i < c->mem_props.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
        if (c->mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            blocks += vb[i].statistics.blockBytes, allocs += vb[i].statistics.allocationBytes;
    const double M = 1048576.0;
    uint64_t listed = h.tex_bytes + h.other_bytes;
    rt_log("mem: holes frame %llu: device-local VMA blocks %.0f MiB hold %.0f MiB of allocations, %.0f MiB free inside "
           "them; memory type %u: %u blocks with textures, %.0f MiB: textures %.0f MiB (%.0f MiB could move, %.0f MiB "
           "be dropped%s), VRAM mirrors %.0f MiB, free or of another kind %.0f MiB; %u of them at most half full, %.0f "
           "MiB holding %.0f MiB; %u of those, %.0f MiB, hold nothing but textures that could move (%.0f MiB) or be "
           "dropped (%.0f MiB)", (unsigned long long)frame, blocks / M, allocs / M,
           blocks > allocs ? (blocks - allocs) / M : 0.0, h.type, h.blocks, h.block_bytes / M, h.tex_bytes / M,
           h.move_bytes / M, h.drop_bytes / M, h.marks ? "" : "; the texture pool's marks are not in use, so none counts "
           "as unsampled", h.other_bytes / M, h.block_bytes > listed ? (h.block_bytes - listed) / M : 0.0, h.sparse,
           h.sparse_bytes / M, h.sparse_listed / M, h.emptiable, h.emptiable_bytes / M, h.emptiable_move / M,
           h.emptiable_drop / M);
    if (h.kept)
        rt_log("mem: holes frame %llu: %u more of the half-full blocks, %.0f MiB, keep something that cannot move: a VRAM "
               "mirror in %u (alone in %u, %.0f MiB), a texture only the GPU made in %u (alone in %u, %.0f MiB), a "
               "texture sampled lately in %u (alone in %u, %.0f MiB), another texture that cannot move in %u (alone in "
               "%u, %.0f MiB)", (unsigned long long)frame, h.kept, h.kept_bytes / M, h.kept_with[0], h.kept_only[0],
               h.kept_only_bytes[0] / M, h.kept_with[1], h.kept_only[1], h.kept_only_bytes[1] / M, h.kept_with[2],
               h.kept_only[2], h.kept_only_bytes[2] / M, h.kept_with[3], h.kept_only[3], h.kept_only_bytes[3] / M);
}

#define OUTSIDE_STEP (256ull << 20)
static void outside_watch(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c || !c->has_memory_budget) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
    vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t usage = 0, blocks = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            usage += bp.heapUsage[h], blocks += vb[h].statistics.blockBytes;
    uint64_t out = usage > blocks ? usage - blocks : 0;
    static uint64_t last;
    static int seen;
    if (seen && (out > last ? out - last : last - out) < OUTSIDE_STEP) return;
    const double M = 1048576.0;
    rt_log("mem: frame %llu: device memory outside VMA (swapchain, DLSS and frame generation, pipelines, driver) %s%.0f "
           "MiB (%s%.0f MiB)", (unsigned long long)frame, seen ? "now " : "", out / M, out >= last ? "+" : "-",
           (out >= last ? out - last : last - out) / M);
    last = out, seen = 1;
}

void vk_mem_lost_report(void) {
    uint64_t used, budget;
    int exact;
    if (!vram_query(&used, &budget, &exact)) return;
    rt_log("vk: video memory when the device was lost: %llu MiB in use of a budget of %llu MiB (%s)",
           (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20),
           exact ? "the driver's figures, every program counted in the budget" : "this game's own allocations only");
}

void vk_mem_fatal_note(char *out, size_t n) {
    char lazy[160];
    mem_lazy_note(lazy, sizeof lazy);
    int st = __atomic_load_n(&lz_state, __ATOMIC_ACQUIRE), want = __atomic_load_n(&lz_wanted, __ATOMIC_ACQUIRE);
    snprintf(out, n, "memory mode %s%s, lazy write-back %s (%s%s%s)", mirror_verify ? "verify" : vk_mirror_on ? "mirror" : "import",
             shadow_on ? " with shadow storage" : "", want < 0 ? "not decided" : st == 2 && want ? "failed to start" : want ? "on" : "off",
             lz_why, *lazy ? "; " : "", lazy);
}

float vk_mem_device_used_mb(void) {
    VkCtx *c = vk_ctx();
    if (!c || !c->has_memory_budget) return -1;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
    vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    uint64_t usage = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) usage += bp.heapUsage[h];
    return (float)(usage / 1048576.0);
}

void vk_mem_stage_log(const char *stage) {
    VkCtx *c = vk_ctx();
    double proc, used, limit;
    float ram_used, ram_total, ws;
    plat_commit_info(&proc, &used, &limit);
    plat_mem_info(&ram_used, &ram_total, &ws);
    const double M = 1048576.0;
    uint64_t usage = 0, blocks = 0;
    int budget = c && c->has_memory_budget;
    if (budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
        VmaBudget vb[VK_MAX_MEMORY_HEAPS] = {0};
        if (c->vma) vmaGetHeapBudgets(c->vma, vb);
        for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
            if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                usage += bp.heapUsage[h], blocks += vb[h].statistics.blockBytes;
    }
    char dev[96] = "device memory unknown (no VK_EXT_memory_budget)";
    if (budget)
        snprintf(dev, sizeof dev, "device memory in use %.0f MiB, outside VMA %.0f MiB", usage / M,
                 usage > blocks ? (usage - blocks) / M : 0.0);
    rt_log("mem: boot stage, after %s: %s; process commit %.0f MB, working set %.0f MB; system commit %.0f of %.0f MB",
           stage, dev, proc, (double)ws, used, limit);
}

static void vram_log(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t blocks = 0, block_bytes = 0, allocs = 0, alloc_bytes = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            blocks += vb[h].statistics.blockCount, block_bytes += vb[h].statistics.blockBytes,
                allocs += vb[h].statistics.allocationCount, alloc_bytes += vb[h].statistics.allocationBytes;
    VkTexCensus t;
    vk_texture_census(&t);
    const double M = 1048576.0;
    rt_log("mem: vram frame %llu: vma %llu blocks %.0f MiB, %llu allocations %.0f MiB; textures live %llu (own memory %llu, "
           "%.0f MiB), released %llu (own memory %llu, %.0f MiB); mirrors %lld %.0f MiB; releases pending %d",
           (unsigned long long)frame, (unsigned long long)blocks, block_bytes / M, (unsigned long long)allocs,
           alloc_bytes / M, (unsigned long long)t.live, (unsigned long long)t.own_live, t.bytes_live / M,
           (unsigned long long)t.released, (unsigned long long)t.own_released, t.bytes_released / M, (long long)m_live_n,
           m_live_bytes / M, dead_pending());
    vk_texture_census_log(frame);
    pool_census_log(frame);
    heap_log(c, frame, vb);
    vram_use_log(c, frame, vb, &t);
}

void vk_mem_class_log(const VkPhysicalDeviceProperties *p, const VkPhysicalDeviceMemoryProperties *m) {
    int big_local = -1, host_heap = -1;
    for (uint32_t h = 0; h < m->memoryHeapCount; h++) {
        if (m->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            if (m->memoryHeaps[h].size >= (1ull << 30) && big_local < 0) big_local = (int)h;
        } else if (host_heap < 0) host_heap = (int)h;
    }
    static const char *const types[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    int t = (int)p->deviceType;
    const char *tn = t >= 0 && t <= 4 ? types[t] : "unknown";
    const char *cls, *why;
    if (p->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) cls = "uma", why = "integrated gpu";
    else if (p->deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) cls = "uma", why = "not a discrete gpu";
    else if (big_local < 0) cls = "uma", why = "no device-local heap of 1 GiB or more";
    else if (host_heap < 0) cls = "uma", why = "every heap is device-local";
    else cls = "discrete", why = "device-local heap separate from host memory";
    int discrete = cls[0] == 'd';
    char line[2048];
    size_t k = (size_t)snprintf(line, sizeof line, "mem: class %s (%s; device type %d %s, %s)", cls, why, t, tn, p->deviceName);
    for (uint32_t h = 0; h < m->memoryHeapCount; h++)
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, "; heap %u %.0f MiB flags %#x%s%s", h,
                              m->memoryHeaps[h].size / 1048576.0, m->memoryHeaps[h].flags,
                              m->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
                              m->memoryHeaps[h].flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT ? " MULTI_INSTANCE" : "");
    rt_log("%s", line);
    k = (size_t)snprintf(line, sizeof line, "mem: types");
    for (uint32_t i = 0; i < m->memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = m->memoryTypes[i].propertyFlags;
        k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, "; %u heap %u flags %#x%s%s%s%s%s", i,
                              m->memoryTypes[i].heapIndex, f, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
                              f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " HOST_VISIBLE" : "",
                              f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " HOST_COHERENT" : "",
                              f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " HOST_CACHED" : "",
                              f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT ? " LAZY" : "");
    }
    rt_log("%s", line);
    mirror_mode(discrete, vk_caps.driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY);
}

typedef struct { VkBuffer buf; VmaAllocation alloc; VkDeviceSize size; } Staging;
typedef struct { VkBuffer buf; VmaAllocation alloc; uint8_t *map; } StgChunk;
typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    uint64_t seq;
    Staging *stg;
    int nstg, cap;
    StgChunk *chunk;
    int nchunk, cap_chunk;
    VkDeviceSize chunk_used;
    uint64_t bytes;
    int ts;
    uint64_t sub_ns;
} Batch;

enum { DEAD_IMAGE = 1, DEAD_IMAGE_VMA, DEAD_VIEW, DEAD_BUFFER_VMA, DEAD_HOST };
typedef struct { int kind; uint64_t handle; VmaAllocation alloc; uint64_t seq, tl; } Dead;

static PlatRMutex cmd_lock;
static PlatOnce cmd_once = PLAT_ONCE_INIT;
static VkCommandPool cmd_pool;
static Batch open_b;
static int open_active;
static Batch *inflight;
static int ninflight, cap_inflight;
static Batch *spare;

#define UP_TS_PAIRS 256
static VkQueryPool up_ts_pool;
static int up_ts_state;
static uint8_t up_ts_busy[UP_TS_PAIRS];
static uint32_t up_ts_next;
static uint64_t up_ts_ns, up_ts_n, up_ts_max_ns, up_ts_missed, up_n_batches;

static int up_ts_open(VkCtx *c, VkCommandBuffer cb) {
    if (!(rt_trace & RT_TRACE_PERF) || up_ts_state < 0) return -1;
    if (!up_ts_state) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = 2 * UP_TS_PAIRS};
        up_ts_state = c->props.limits.timestampComputeAndGraphics &&
                      vkCreateQueryPool(c->device, &qi, NULL, &up_ts_pool) == VK_SUCCESS ? 1 : -1;
        if (up_ts_state < 0) return -1;
    }
    uint32_t p = up_ts_next % UP_TS_PAIRS;
    if (up_ts_busy[p]) { up_ts_missed++; return -1; }
    up_ts_next++, up_ts_busy[p] = 1;
    vkCmdResetQueryPool(cb, up_ts_pool, 2 * p, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, up_ts_pool, 2 * p);
    return (int)p;
}

static void up_ts_read(VkCtx *c, Batch *b) {
    if (b->ts < 0) return;
    uint64_t t[2];
    if (vkGetQueryPoolResults(c->device, up_ts_pool, 2 * (uint32_t)b->ts, 2, sizeof t, t, sizeof t[0],
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] >= t[0]) {
        uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * c->props.limits.timestampPeriod);
        up_ts_ns += ns, up_ts_n++;
        vk_gtl_note(GTL_UPLOAD, b->sub_ns, t[0], t[1]);
        if (ns > up_ts_max_ns) up_ts_max_ns = ns;
    }
    up_ts_busy[b->ts] = 0;
    b->ts = -1;
}

static void cmd_init(void *p);
static void up_ts_log(uint64_t frame) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    rt_log("perf: frame %llu: upload batches %.1f per frame, on the GPU %.2f ms per frame (%llu timed, %.3f ms each, the "
           "longest %.2f ms; %llu not timed)", (unsigned long long)frame, up_n_batches / 60.0, up_ts_ns / 60e6,
           (unsigned long long)up_ts_n, up_ts_n ? up_ts_ns / 1e6 / (double)up_ts_n : 0.0, up_ts_max_ns / 1e6,
           (unsigned long long)up_ts_missed);
    up_ts_ns = up_ts_n = up_ts_max_ns = up_ts_missed = up_n_batches = 0;
    plat_rmutex_unlock(&cmd_lock);
}
static int nspare, cap_spare;
static uint64_t next_seq, done_seq;
static Dead *dead;
static int ndead, cap_dead;
static uint64_t tl_noted;

static void cmd_init(void *p) {
    (void)p;
    return;
}

static void grow(void **arr, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 16;
    void *n = realloc(*arr, (size_t)*cap * elem);
    if (!n) rt_fatal("vk: out of memory (upload bookkeeping)");
    *arr = n;
}

#define STG_CACHE 24
#define STG_CACHE_BYTES (192ull << 20)
static Staging stg_cache[STG_CACHE];
static int stg_ncache;
static VkDeviceSize stg_cache_bytes;

#define ARENA_CHUNK (8ull << 20)
#define ARENA_MAX (ARENA_CHUNK / 4)
#define ARENA_ALIGN 256ull
#define ARENA_KEEP 8
static StgChunk arena_free[ARENA_KEEP];
static int narena_free;
static uint64_t arena_made, arena_ranges;

static void free_staging(VkCtx *c, Batch *b) {
    for (int i = 0; i < b->nstg; i++) {
        if (stg_ncache < STG_CACHE && stg_cache_bytes + b->stg[i].size <= STG_CACHE_BYTES) {
            stg_cache[stg_ncache++] = b->stg[i];
            stg_cache_bytes += b->stg[i].size;
            continue;
        }
        vmaDestroyBuffer(c->vma, b->stg[i].buf, b->stg[i].alloc);
    }
    b->nstg = 0;
    for (int i = 0; i < b->nchunk; i++) {
        if (narena_free < ARENA_KEEP) arena_free[narena_free++] = b->chunk[i];
        else vmaDestroyBuffer(c->vma, b->chunk[i].buf, b->chunk[i].alloc);
    }
    b->nchunk = 0, b->chunk_used = 0;
    b->bytes = 0;
}

#define STG_FLUSH (64ull << 20)

static uint64_t batch_staging(const Batch *b) {
    uint64_t n = (uint64_t)b->nchunk * ARENA_CHUNK;
    for (int i = 0; i < b->nstg; i++) n += b->stg[i].size;
    return n;
}

static int staging_cache_take(VkCtx *c, VkDeviceSize n, Staging *out, void **map) {
    int best = -1;
    for (int i = 0; i < stg_ncache; i++) {
        if (stg_cache[i].size < n || stg_cache[i].size > n * 4 + 65536) continue;
        if (best < 0 || stg_cache[i].size < stg_cache[best].size) best = i;
    }
    if (best < 0) return 0;
    VmaAllocationInfo ai;
    vmaGetAllocationInfo(c->vma, stg_cache[best].alloc, &ai);
    *out = stg_cache[best], *map = ai.pMappedData;
    stg_cache_bytes -= stg_cache[best].size;
    stg_cache[best] = stg_cache[--stg_ncache];
    return 1;
}

static void retire(VkCtx *c, int wait_all) {
    int k = 0;
    for (; k < ninflight; k++) {
        Batch *b = &inflight[k];
        if (wait_all) vkWaitForFences(c->device, 1, &b->fence, VK_TRUE, UINT64_MAX);
        else if (vkGetFenceStatus(c->device, b->fence) != VK_SUCCESS) break;
        up_ts_read(c, b);
        free_staging(c, b);
        done_seq = b->seq;
        if (nspare == cap_spare) grow((void **)&spare, &cap_spare, sizeof *spare);
        spare[nspare++] = *b;
    }
    memmove(inflight, inflight + k, (size_t)(ninflight - k) * sizeof *inflight);
    ninflight -= k;
}

static void destroy_dead(VkCtx *c, const Dead *d) {
    switch (d->kind) {
    case DEAD_IMAGE: vkDestroyImage(c->device, (VkImage)(uintptr_t)d->handle, NULL); break;
    case DEAD_IMAGE_VMA: vmaDestroyImage(c->vma, (VkImage)(uintptr_t)d->handle, d->alloc); break;
    case DEAD_VIEW: vkDestroyImageView(c->device, (VkImageView)(uintptr_t)d->handle, NULL); break;
    case DEAD_BUFFER_VMA: vmaDestroyBuffer(c->vma, (VkBuffer)(uintptr_t)d->handle, d->alloc); break;
    }
}

typedef struct { PoolRec *r; uint64_t seq, tl; } PoolWait;
static PoolWait *pwait;
static int npwait, cpwait;
static uint64_t pf_n, pf_ns, pf_max_ns, pf_logged;

static int pool_fini_idle(void) {
    static int on = -1;
    if (on < 0) {
        on = rt_knob("POOL_FINI_IDLE", "0") == 1;
        rt_log("vk: a pool's finalize waits for %s (NX2SARIA_POOL_FINI_IDLE=%d)",
               on ? "the idle queue, the upload batches included" : "the submissions issued so far", on);
    }
    return on;
}

static void pool_fini_wait(void) {
    uint64_t t0 = kern_now_ns(), v = vk_sync_issued();
    vk_sync_fence_wait(v, UINT64_MAX);
    uint64_t d = kern_now_ns() - t0;
    uint64_t n = __atomic_add_fetch(&pf_n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&pf_ns, d, __ATOMIC_RELAXED);
    if (d > __atomic_load_n(&pf_max_ns, __ATOMIC_RELAXED)) __atomic_store_n(&pf_max_ns, d, __ATOMIC_RELAXED);
    if (d > 20000000ull && __atomic_add_fetch(&pf_logged, 1, __ATOMIC_RELAXED) <= 20)
        rt_log("vk: a pool's finalize waited %.1f ms for the submissions up to %llu (finalizes so far %llu, %.1f ms in all)",
               d / 1e6, (unsigned long long)v, (unsigned long long)n, __atomic_load_n(&pf_ns, __ATOMIC_RELAXED) / 1e6);
    else if (n % 256 == 0)
        rt_log("vk: pool finalizes that waited for the GPU: %llu, %.1f ms in all, the longest %.1f ms",
               (unsigned long long)n, __atomic_load_n(&pf_ns, __ATOMIC_RELAXED) / 1e6,
               __atomic_load_n(&pf_max_ns, __ATOMIC_RELAXED) / 1e6);
}

static void pool_wait_add(PoolRec *r) {
    PoolWait w = {r, __atomic_load_n(&next_seq, __ATOMIC_ACQUIRE), vk_sync_issued() + 1};
    plat_mutex_lock(&retire_lock);
    if (npwait == cpwait) grow((void **)&pwait, &cpwait, sizeof *pwait);
    pwait[npwait++] = w;
    plat_mutex_unlock(&retire_lock);
}

static void pool_wait_collect(uint64_t tl) {
    PoolRec *go[64];
    int n = 0;
    plat_mutex_lock(&retire_lock);
    for (int i = 0; i < npwait && n < 64;)
        if (pwait[i].seq <= done_seq && pwait[i].tl <= tl) go[n++] = pwait[i].r, pwait[i] = pwait[--npwait];
        else i++;
    plat_mutex_unlock(&retire_lock);
    for (int i = 0; i < n; i++) pool_retire(go[i]);
}

static void collect_locked_out(VkCtx *c, Dead **out, int *nout, int *cout) {
    retire(c, 0);
    uint64_t tl = 0;
    if (c->timeline) vkGetSemaphoreCounterValue(c->device, c->timeline, &tl);
    if (npwait) pool_wait_collect(tl);
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        Dead *d = &dead[i];
        if (d->seq > done_seq || d->tl > tl) { dead[k++] = *d; continue; }
        if (d->kind == DEAD_HOST) {
            free((void *)(uintptr_t)d->handle);
            continue;
        }
        rec_check_handle(d->kind == DEAD_VIEW ? REC_VIEW : d->kind == DEAD_BUFFER_VMA ? REC_BUFFER : REC_IMAGE, d->handle, 1,
                         d->tl, tl);
        if (!out) { destroy_dead(c, d); continue; }
        if (*nout == *cout) grow((void **)out, cout, sizeof **out);
        (*out)[(*nout)++] = *d;
    }
    ndead = k;
}

static void collect_locked(VkCtx *c) { collect_locked_out(c, NULL, NULL, NULL); }

VkCommandBuffer vk_upload_begin(void) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    VkCtx *c = vk_ctx();
    if (!c) rt_fatal("vk: upload before vk_init");
    if (open_active) return open_b.cb;
    if (!cmd_pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &cmd_pool) != VK_SUCCESS) rt_fatal("vk: upload command pool");
    }
    retire(c, 0);
    memset(&open_b, 0, sizeof open_b);
    if (nspare) {
        Batch s = spare[--nspare];
        open_b.cb = s.cb, open_b.fence = s.fence, open_b.stg = s.stg, open_b.cap = s.cap;
        open_b.chunk = s.chunk, open_b.cap_chunk = s.cap_chunk;
        vkResetCommandBuffer(open_b.cb, 0);
        vkResetFences(c->device, 1, &open_b.fence);
    } else {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cmd_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(c->device, &ai, &open_b.cb) != VK_SUCCESS ||
            vkCreateFence(c->device, &fi, NULL, &open_b.fence) != VK_SUCCESS)
            rt_fatal("vk: upload command buffer");
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(open_b.cb, &bi);
    open_b.ts = up_ts_open(c, open_b.cb);
    open_b.seq = ++next_seq;
    vk_crumb(open_b.cb, VK_CRUMB_UPLOAD, open_b.seq);
    open_active = 1;
    return open_b.cb;
}

static uint64_t upload_cap = UINT64_MAX - 1;
static uint64_t n_early;

void vk_upload_end(void) {
    if (upload_cap == UINT64_MAX - 1) {
        const char *e = cfg_get("NX2SARIA_UPLOAD_BATCH_MB");
        long mb = e && *e ? atol(e) : 0;
        upload_cap = mb > 0 ? (uint64_t)mb << 20 : UINT64_MAX;
        if (mb > 0) rt_log("vk: upload batches submitted early above %ld MiB of staging (NX2SARIA_UPLOAD_BATCH_MB)", mb);
        else rt_log("vk: upload batches submitted early above %llu MiB of staging only, the staging pool's limit "
                    "(NX2SARIA_UPLOAD_BATCH_MB=0)", (unsigned long long)(STG_FLUSH >> 20));
    }
    if (open_active && cmd_lock.depth == 1 && (open_b.bytes >= upload_cap || batch_staging(&open_b) >= STG_FLUSH)) {
        n_early++;
        if (n_early <= 4 || !(n_early & (n_early - 1)))
            rt_log("vk: upload batch submitted early with %.0f MiB of staging in %d buffers and %d arena chunks (%llu so far)",
                   open_b.bytes / 1048576.0, open_b.nstg, open_b.nchunk, (unsigned long long)n_early);
        vk_upload_flush(0);
    }
    plat_rmutex_unlock(&cmd_lock);
}

void vk_mem_oom_report(const char *what, VkResult r) {
    if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) return;
    VkCtx *c = vk_ctx();
    if (!c) return;
    rt_log("vk: %s failed: out of %s memory (%d)", what, r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? "device" : "host", (int)r);
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                            .pNext = c->has_memory_budget ? &b : NULL};
    vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    const double M = 1048576.0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        rt_log("vk:   heap %u (%s, %.0f MiB): driver use %.0f of budget %.0f MiB%s; vma %u blocks %.0f MiB, %u allocations "
               "%.0f MiB", h, c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "host",
               c->mem_props.memoryHeaps[h].size / M, c->has_memory_budget ? b.heapUsage[h] / M : vb[h].usage / M,
               c->has_memory_budget ? b.heapBudget[h] / M : vb[h].budget / M,
               c->has_memory_budget ? "" : " (VMA's estimate)", vb[h].statistics.blockCount,
               vb[h].statistics.blockBytes / M, vb[h].statistics.allocationCount, vb[h].statistics.allocationBytes / M);
    uint64_t ib = 0;
    for (int i = 0; i < ninflight; i++) ib += inflight[i].bytes;
    rt_log("vk:   upload batch open %d (%d staging buffers, %.0f MiB), %d in flight (%.0f MiB of staging), %llu submitted "
           "early; VRAM mirrors %lld (%.0f MiB); objects waiting for release %d", open_active, open_b.nstg,
           open_b.bytes / M, ninflight, ib / M, (unsigned long long)n_early, (long long)m_live_n, m_live_bytes / M, ndead);
}

#define STG_BLOCK (64ull << 20)
#define STG_MIN_BLOCKS 4
#define STG_MIN_BLOCKS_UMA 2
static VmaPool stg_pool;
static int stg_pool_tried;
static uint64_t stg_room_cache, stg_room_wait, stg_room_new;
static uint64_t stg_wait_ticks, stg_wait_max;

static VkResult stg_create(VkCtx *c, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai, VkBuffer *b,
                           VmaAllocation *a, VmaAllocationInfo *info, int chunk) {
    VmaAllocationCreateInfo in = *ai;
    in.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
    plat_rmutex_lock(&cmd_lock);
    VkResult r;
    for (;;) {
        if ((r = vmaCreateBuffer(c->vma, bi, &in, b, a, info)) == VK_SUCCESS) break;
        if (stg_ncache || narena_free) {
            for (int i = 0; i < stg_ncache; i++) vmaDestroyBuffer(c->vma, stg_cache[i].buf, stg_cache[i].alloc);
            for (int i = 0; i < narena_free; i++) vmaDestroyBuffer(c->vma, arena_free[i].buf, arena_free[i].alloc);
            stg_ncache = 0, stg_cache_bytes = 0, narena_free = 0;
            stg_room_cache++;
            continue;
        }
        if (ninflight && !vk_device_is_lost()) {
            int before = ninflight;
            uint64_t w0 = qpc();
            VkResult w = vkWaitForFences(c->device, 1, &inflight[0].fence, VK_TRUE, UINT64_MAX);
            if (w != VK_SUCCESS) {
                vk_device_lost_check(w, "the wait for an upload batch");
                rt_fatal("vk: the wait for an upload failed (%d)", (int)w);
            }
            uint64_t took = qpc() - w0;
            stg_wait_ticks += took;
            if (took > stg_wait_max) stg_wait_max = took;
            retire(c, 0);
            stg_room_wait++;
            if (stg_room_wait <= 4 || !(stg_room_wait & (stg_room_wait - 1)))
                rt_log("vk: a staging buffer of %.1f MiB waited %.1f ms for an upload batch to finish, with the upload "
                       "lock held (%llu such waits so far, %.0f ms in all, the longest %.1f ms; %d batches still in "
                       "flight). A replay submission that sends its uploads waits for that lock",
                       bi->size / 1048576.0, qpc_ms(took), (unsigned long long)stg_room_wait, qpc_ms(stg_wait_ticks),
                       qpc_ms(stg_wait_max), ninflight);
            if (chunk && narena_free) {
                r = VK_INCOMPLETE;
                break;
            }
            if (ninflight < before) continue;
        }
        r = vmaCreateBuffer(c->vma, bi, ai, b, a, info);
        if (r == VK_SUCCESS && stg_room_new++ < 16)
            rt_log("vk: the staging pool grew by a block for a buffer of %.1f MiB (the open upload batch holds %.0f MiB, "
                   "%d batches in flight; room found so far by dropping kept buffers %llu times, by waiting for a batch "
                   "%llu times; buffers callers hold and the blocks' fragmentation also take room)",
                   bi->size / 1048576.0, open_active ? batch_staging(&open_b) / 1048576.0 : 0.0, ninflight,
                   (unsigned long long)stg_room_cache, (unsigned long long)stg_room_wait);
        break;
    }
    plat_rmutex_unlock(&cmd_lock);
    return r;
}

uint64_t vk_mem_give_back(void) {
    VkCtx *c = vk_ctx();
    uint64_t freed = 0;
    plat_rmutex_lock(&cmd_lock);
    for (int i = 0; i < stg_ncache; i++) {
        freed += stg_cache[i].size;
        vmaDestroyBuffer(c->vma, stg_cache[i].buf, stg_cache[i].alloc);
    }
    stg_ncache = 0, stg_cache_bytes = 0;
    for (int i = 0; i < narena_free; i++) vmaDestroyBuffer(c->vma, arena_free[i].buf, arena_free[i].alloc);
    freed += (uint64_t)narena_free * ARENA_CHUNK;
    narena_free = 0;
    plat_rmutex_unlock(&cmd_lock);
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < nsnap_pool; i++) vmaDestroyBuffer(c->vma, snap_pool[i].buf, snap_pool[i].alloc);
    freed += snap_pool_bytes;
    nsnap_pool = 0, snap_pool_bytes = 0;
    plat_mutex_unlock(&snap_pool_lock);
    return freed + vk_headroom_release();
}

#define IDLE_MIN 10
#define GROWTH_STEP (64ull << 20)
#define RANK_EVERY_NS (120ull * 1000000000ull)
#define HEAP_SLOW_NS 2000000ull

typedef struct {
    uint64_t usage, blocks, allocs;
    uint64_t hblocks;
    uint64_t tex, pools, vma_other;
    uint64_t heap_c, maps;
} ReportFig;

static uint64_t import_bytes(void) {
    uint64_t b = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT && r->buf) b += r->import_hi - r->import_lo;
    }
    plat_mutex_unlock_shared(&pool_lock);
    return b;
}

static void outside_log(VkCtx *c, uint64_t frame, const VkPhysicalDeviceMemoryBudgetPropertiesEXT *bp,
                        const VmaBudget *vb, const VkPipeMem *pm) {
    const double M = 1048576.0;
    char line[2000];
    size_t k = 0;
    mg_put(line, sizeof line, &k, "mem: outside VMA frame %llu: by heap, the driver's usage of its budget, VMA's blocks "
           "in it and the rest:", (unsigned long long)frame);
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        uint64_t use = c->has_memory_budget ? bp->heapUsage[h] : vb[h].usage;
        uint64_t budget = c->has_memory_budget ? bp->heapBudget[h] : vb[h].budget, vma = vb[h].statistics.blockBytes;
        mg_put(line, sizeof line, &k, "%s heap %u (%s) %.0f of %.0f MiB, VMA's blocks %.0f, outside VMA %.0f", h ? ";" : "",
               h, c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device-local" : "host",
               use / M, budget / M, vma / M, use > vma ? (use - vma) / M : 0.0);
    }
    uint64_t imp = import_bytes();
    if (imp) mg_put(line, sizeof line, &k, " (the game's memory imported for the GPU, %.0f MiB, is in the host heaps' rest)",
                    imp / M);
    if (!c->has_memory_budget)
        mg_put(line, sizeof line, &k, " (VMA's own estimates: without VK_EXT_memory_budget nothing outside VMA shows)");
    mg_put(line, sizeof line, &k, ". Made outside VMA, counted where the driver gives no size: graphics pipelines %llu "
           "(whole %llu, fast-linked %llu, optimised after a fast link %llu; no draw for %u minutes %llu, none in this run "
           "%llu; building or failed %llu), stage libraries %llu (vertex input %llu, pre-rasterization %llu, fragment "
           "shader %llu, fragment output %llu), compute pipelines %llu (no dispatch for %u minutes %llu, none in this run "
           "%llu), pipeline layouts %llu", (unsigned long long)pm->gfx, (unsigned long long)pm->gfx_whole,
           (unsigned long long)pm->gfx_fast, (unsigned long long)pm->gfx_opt, pm->idle_min,
           (unsigned long long)pm->gfx_idle, (unsigned long long)pm->gfx_never, (unsigned long long)pm->gfx_none,
           (unsigned long long)(pm->lib[0] + pm->lib[1] + pm->lib[2] + pm->lib[3]), (unsigned long long)pm->lib[0],
           (unsigned long long)pm->lib[1], (unsigned long long)pm->lib[2], (unsigned long long)pm->lib[3],
           (unsigned long long)pm->comp, pm->idle_min, (unsigned long long)pm->comp_idle,
           (unsigned long long)pm->comp_never, (unsigned long long)pm->layouts);
    VkDescMem dm;
    vk_exec_desc_mem(&dm);
    mg_put(line, sizeof line, &k, "; descriptor pools %llu of %llu sets each (%llu of them spare), the texture pool's %llu "
           "sets", (unsigned long long)dm.pools, (unsigned long long)dm.sets_each, (unsigned long long)dm.spare,
           (unsigned long long)dm.tex_sets);
    uint32_t sn, sw, sh;
    uint64_t sb;
    vk_present_swapchain_mem(&sn, &sw, &sh, &sb);
    mg_put(line, sizeof line, &k, "; the swapchain %u images of %ux%u, %.0f MiB of pixels", sn, sw, sh, sb / M);
    int64_t dlss = vk_dlaa_vram();
    int nfsr = 0;
    uint64_t fg = 0, fsr = vk_ffx_vram(&nfsr, &fg);
    if (dlss >= 0) mg_put(line, sizeof line, &k, "; DLSS %.0f MiB", dlss / M);
    else mg_put(line, sizeof line, &k, "; DLSS reports nothing");
    mg_put(line, sizeof line, &k, "; FSR upscaling %.0f MiB in %d contexts, FSR frame generation %.0f MiB (DLSS frame "
           "generation gives no size)", fsr / M, nfsr, fg / M);
    rt_log("%s", line);
}

typedef struct { uint64_t commit, crt, shaders, pipes, texhost, pcache, other; } SysFig;

static void system_log(uint64_t frame, const ReportFig *f, const VkPipeMem *pm, SysFig *out) {
    const double M = 1048576.0;
    uint64_t res, priv;
    plat_process_memory(&res, &priv);
    static int heap_off, heap_census = -1;
    if (heap_census < 0) heap_census = cfg_bool("NX2SARIA_MEM_CENSUS");
    PlatHeapInfo hi = {0};
    if (mem_heap_summary_due(heap_census, frame_step_world_us() >= 0, heap_off)) {
        uint64_t t0 = plat_now_ns();
        plat_heap_info(&hi);
        uint64_t took = plat_now_ns() - t0;
        if (took > HEAP_SLOW_NS && !heap_census)
            heap_off = 1, rt_log("mem: the C runtime heap's summary took %.1f ms; it is not asked for again in this run",
                                 took / 1e6);
    }
    GfxShaderRam sr;
    gfx_shader_ram(&sr);
    VkTexHost th;
    vk_texture_host(&th);
    uint64_t frec, fthumb;
    flight_mem(&frec, &fthumb);
    uint64_t lq = logq_bytes();
    uint64_t shaders = sr.cache + sr.use + sr.pack + sr.records;
    char spv[400];
    if (sr.compress > 0)
        snprintf(spv, sizeof spv, " (their SPIR-V %.0f, uncompressed %.0f; %llu shaders compressed; %llu decompressions "
                 "in %.0f ms, %llu of them on the drawing threads in %.1f ms, the longest %.2f ms)", sr.spv_held / M,
                 sr.spv_full / M, (unsigned long long)sr.spv_packed, (unsigned long long)sr.decodes, sr.decode_ns / 1e6,
                 (unsigned long long)sr.draw_decodes, sr.draw_decode_ns / 1e6, sr.draw_decode_max_ns / 1e6);
    else
        snprintf(spv, sizeof spv, " (their SPIR-V %.0f, uncompressed%s)", sr.spv_held / M,
                 sr.compress < 0 ? ": shader_compress = 0" : "");
    uint64_t pipes = pm->ram_pipes + pm->ram_keys + pm->ram_seed + pm->ram_binaries;
    uint64_t cn, cb, cidle_n, cidle_b;
    nvn_copies_stats(&cn, &cb, &cidle_n, &cidle_b);
    uint64_t texhost = th.bytes + th.tmp, known = shaders + pipes + texhost + lq + frec + cb;
    uint64_t pcache = pm->cache[0] + pm->cache[1];
    int charged = plat_commit_counts_vram();
    uint64_t dev = charged ? f->usage : 0, crt = hi.committed ? hi.committed : known, wc = 0, wc_vram = 0, wc_gap = 0;
    if (charged && perf_wc_recent(60000000000ull, &wc, &wc_vram) && wc > wc_vram) wc_gap = wc - wc_vram;
    int64_t other = (int64_t)priv - (int64_t)(f->heap_c + f->maps + dev + wc_gap + f->hblocks + crt + pcache);
    char line[2000];
    size_t k = 0;
    mg_put(line, sizeof line, &k, "mem: system frame %llu: process commit %.0f MiB%s = the game's heap %.0f + its physical "
           "maps %.0f", (unsigned long long)frame, priv / M, charged ? "" : " (the resident anonymous pages)",
           f->heap_c / M, f->maps / M);
    if (charged) mg_put(line, sizeof line, &k, " + device-local memory, which Windows charges to the process, %.0f", dev / M);
    if (wc_gap)
        mg_put(line, sizeof line, &k, " + the driver's write-combined views beyond that figure %.0f (at the last walk of the "
               "address space)", wc_gap / M);
    mg_put(line, sizeof line, &k, " + VMA's host-visible blocks %.0f (the flight recorder's thumbnails %.0f of them)",
           f->hblocks / M, fthumb / M);
    if (hi.committed)
        mg_put(line, sizeof line, &k, " + the C runtime heap %.0f (%.0f of it handed out; of it known: shader caches %.0f in "
               "%llu shaders%s, pipeline tables %.0f, the texture cache %.0f with decode temporaries %.0f now and %.0f at most "
               "since the last report, the log queue %.0f, the flight recorder's records %.0f, NVN recording copies %.0f in "
               "%llu, %.0f of it in %llu unused for 5 minutes; the rest %.0f)",
               hi.committed / M, hi.used / M, shaders / M, (unsigned long long)sr.shaders, spv, pipes / M, texhost / M,
               th.tmp / M, th.tmp_peak / M, lq / M, frec / M, cb / M, (unsigned long long)cn, cidle_b / M,
               (unsigned long long)cidle_n, ((double)hi.committed - (double)known) / M);
    else
        mg_put(line, sizeof line, &k, " + what the runtime knows it allocated %.0f (the C runtime heap's size is not "
               "known: shader caches %.0f in %llu shaders%s, pipeline tables %.0f, the texture cache %.0f with decode "
               "temporaries %.0f now and %.0f at most since the last report, the log queue %.0f, the flight recorder's "
               "records %.0f, NVN recording copies %.0f in %llu, %.0f of it in %llu unused for 5 minutes)", known / M,
               shaders / M, (unsigned long long)sr.shaders, spv, pipes / M, texhost / M, th.tmp / M, th.tmp_peak / M,
               lq / M, frec / M, cb / M, (unsigned long long)cn, cidle_b / M, (unsigned long long)cidle_n);
    mg_put(line, sizeof line, &k, " + the driver's pipeline cache data %.0f (pipeline-cache.bin's %.0f, the play cache's "
           "%.0f, %s) + other %.0f (the driver's own memory, thread stacks, the program's data; %u other heaps)",
           pcache / M, pm->cache[0] / M, pm->cache[1] / M, pm->cache_asked ? "as last asked" : "as loaded at the boot",
           other / M, hi.other_heaps);
    char room[160];
    mem_budget_text(room, sizeof room);
    mg_put(line, sizeof line, &k, "; room: %s", room);
    uint32_t handles = plat_handle_count();
    if (handles) mg_put(line, sizeof line, &k, "; handles %u", handles);
    rt_log("%s", line);
    *out = (SysFig){priv, crt, shaders, pipes, texhost, pcache, other > 0 ? (uint64_t)other : 0};
}

enum { G_DEV, G_TEX, G_POOLS, G_VMA_OTHER, G_VMA_FREE, G_OUTSIDE, G_COMMIT, G_HEAP, G_MAPS, G_HOSTVMA, G_CRT, G_SHADERS,
       G_PIPES, G_TEXHOST, G_PCACHE, G_OTHER, G_N };
static const char *const grow_names[G_N] = {
    "device-local in use", "textures", "pools and VRAM mirrors", "VMA's other allocations", "free inside VMA's blocks",
    "outside VMA", "process commit", "the game's heap", "its physical maps", "VMA's host-visible blocks",
    "the C runtime heap", "shader caches", "pipeline tables", "the texture cache's host memory",
    "the driver's pipeline cache data", "other system memory"};
static PlatMutex grow_lock = PLAT_MUTEX_INIT;
static MgBook gbook;
static uint64_t grow_t0, grow_ranked;

static void rank_log_locked(const char *when, uint64_t now) {
    char line[1900];
    mg_rank(&gbook, line, sizeof line);
    rt_log("mem: growth ranking %s %.1f min of play (%u reports; only climbs: rose in a report and fell in none): %s", when,
           (now - grow_t0) / 60e9, gbook.play_lines, line);
}

static void growth_take(uint64_t frame, const uint64_t v[G_N]) {
    plat_mutex_lock(&grow_lock);
    if (!gbook.n)
        for (int i = 0; i < G_N; i++) mg_cat(&gbook, grow_names[i]);
    int was = gbook.play;
    mg_take(&gbook, v, frame_step_world_us() >= 0);
    uint64_t now = plat_now_ns();
    if (gbook.play && !was) grow_t0 = grow_ranked = now;
    char line[1800], when[48];
    if (gbook.play) snprintf(when, sizeof when, "%.1f min of play", (now - grow_t0) / 60e9);
    else snprintf(when, sizeof when, "before play");
    if (mg_grew(&gbook, GROWTH_STEP, line, sizeof line))
        rt_log("mem: growth frame %llu (%s), by more than %llu MiB: %s", (unsigned long long)frame, when,
               (unsigned long long)(GROWTH_STEP >> 20), line);
    if (gbook.play && now - grow_ranked >= RANK_EVERY_NS) grow_ranked = now, rank_log_locked("after", now);
    plat_mutex_unlock(&grow_lock);
}

void vk_mem_exit_log(void) {
    plat_mutex_lock(&grow_lock);
    if (gbook.play) rank_log_locked("at exit, after", plat_now_ns());
    plat_mutex_unlock(&grow_lock);
    char when[48];
    snprintf(when, sizeof when, "at exit (frame %llu)", (unsigned long long)__atomic_load_n(&span_frame, __ATOMIC_RELAXED));
    span_report(when, 200);
}

void vk_mem_rank_log(const char *when) {
    plat_mutex_lock(&grow_lock);
    if (gbook.play) rank_log_locked(when, plat_now_ns());
    plat_mutex_unlock(&grow_lock);
}

static void beyond_log(VkCtx *c, uint64_t frame, const VkPhysicalDeviceMemoryBudgetPropertiesEXT *bp, const VmaBudget *vb,
                       const ReportFig *f) {
    VkPipeMem pm;
    vk_pipeline_mem(&pm, IDLE_MIN);
    outside_log(c, frame, bp, vb, &pm);
    SysFig s;
    system_log(frame, f, &pm, &s);
    uint64_t v[G_N] = {
        [G_DEV] = f->usage, [G_TEX] = f->tex, [G_POOLS] = f->pools, [G_VMA_OTHER] = f->vma_other,
        [G_VMA_FREE] = f->blocks > f->allocs ? f->blocks - f->allocs : 0,
        [G_OUTSIDE] = f->usage > f->blocks ? f->usage - f->blocks : 0, [G_COMMIT] = s.commit, [G_HEAP] = f->heap_c,
        [G_MAPS] = f->maps, [G_HOSTVMA] = f->hblocks, [G_CRT] = s.crt, [G_SHADERS] = s.shaders, [G_PIPES] = s.pipes,
        [G_TEXHOST] = s.texhost, [G_PCACHE] = s.pcache, [G_OTHER] = s.other};
    growth_take(frame, v);
}

static void owner_log(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c || !c->vma) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    if (c->has_memory_budget) {
        VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &bp};
        vkGetPhysicalDeviceMemoryProperties2(c->phys, &p2);
    }
    VmaBudget vb[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, vb);
    uint64_t usage = 0, blocks = 0, allocs = 0, hblocks = 0, hallocs = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            usage += c->has_memory_budget ? bp.heapUsage[h] : vb[h].statistics.blockBytes,
                blocks += vb[h].statistics.blockBytes, allocs += vb[h].statistics.allocationBytes;
        else hblocks += vb[h].statistics.blockBytes, hallocs += vb[h].statistics.allocationBytes;
    }
    uint64_t phys = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < nphys; i++)
        if (phys_list[i]->alloc) phys += phys_list[i]->size;
    plat_mutex_unlock_shared(&pool_lock);
    VkTexCensus t;
    vk_texture_census(&t);
    uint64_t own[VK_OWN_N], hown[VK_OWN_N];
    for (int o = 0; o < VK_OWN_N; o++) own[o] = vk_vma_owner_bytes(o, 1), hown[o] = vk_vma_owner_bytes(o, 0);
    uint64_t stg_blocks = 0, kept;
    if (stg_pool) {
        VmaStatistics st;
        vmaGetPoolStatistics(c->vma, stg_pool, &st);
        stg_blocks = st.blockBytes;
    }
    kept = __atomic_load_n(&stg_cache_bytes, __ATOMIC_RELAXED) +
           (uint64_t)__atomic_load_n(&narena_free, __ATOMIC_RELAXED) * ARENA_CHUNK;
    uint64_t snaps = __atomic_load_n(&snap_pool_bytes, __ATOMIC_RELAXED);
    int64_t dlss = vk_dlaa_vram();
    char ds[48] = "nothing";
    if (dlss >= 0) snprintf(ds, sizeof ds, "%.0f MiB", dlss / 1048576.0);
    const double M = 1048576.0;
    uint64_t mir = (uint64_t)__atomic_load_n(&m_live_bytes, __ATOMIC_RELAXED);
    rt_log("mem: by owner frame %llu: device-local in use %.0f MiB (the driver's figure) = VMA's allocations %.0f + free "
           "inside VMA's blocks %.0f + outside VMA %.0f (compiled pipelines, descriptor memory, the swapchain, DLSS and "
           "frame generation features, the driver; DLSS reports %s). VMA's allocations: textures %.0f (live %.0f, of them "
           "render targets and other GPU-drawn %.0f and idle %.0f; released %.0f, of them too lately to drop %.0f), pools "
           "%.0f (VRAM mirrors %.0f, physical pools %.0f), upscalers %.0f, replay buffers %.0f, uniform snapshots %.0f, "
           "device address table %.0f, ASTC decoder %.0f, present and post-processing %.0f, headroom spares %.0f, other "
           "%.0f", (unsigned long long)frame, usage / M, allocs / M, blocks > allocs ? (blocks - allocs) / M : 0.0,
           usage > blocks ? (usage - blocks) / M : 0.0, ds, own[VK_OWN_TEXTURES] / M, t.bytes_live / M,
           t.bytes_live_gpu / M, t.bytes_live_idle / M, t.bytes_released / M, t.bytes_released_young / M,
           own[VK_OWN_POOLS] / M, mir / M, phys / M, own[VK_OWN_UPSCALER] / M, own[VK_OWN_REPLAY] / M,
           own[VK_OWN_USNAP] / M, own[VK_OWN_GPA] / M, own[VK_OWN_ASTC] / M, own[VK_OWN_PRESENT] / M,
           own[VK_OWN_HEADROOM] / M, own[VK_OWN_OTHER] / M);
    double proc, sys_used, sys_limit;
    float ram_used, ram_total, ws;
    plat_commit_info(&proc, &sys_used, &sys_limit);
    plat_mem_info(&ram_used, &ram_total, &ws);
    uint64_t heap, heap_c, maps;
    mem_guest_usage(&heap, &heap_c, &maps);
    rt_log("mem: host frame %llu: VMA's host-visible blocks %.0f MiB hold %.0f MiB: staging pool blocks %.0f (kept staging "
           "buffers and arena chunks %.0f), write-back snapshots %.0f; by owner: pools and staging %.0f, textures %.0f, "
           "upscalers %.0f, replay buffers %.0f, uniform snapshots %.0f, present and post-processing (the flight "
           "recorder's thumbnails among them) %.0f, other %.0f. Process commit %.0f MB, working set %.0f MB; system commit "
           "%.0f of %.0f MB; the game's heap %.0f MiB (%.0f committed), its physical maps %.0f MiB",
           (unsigned long long)frame, hblocks / M, hallocs / M, stg_blocks / M, kept / M, snaps / M, hown[VK_OWN_POOLS] / M,
           hown[VK_OWN_TEXTURES] / M, hown[VK_OWN_UPSCALER] / M, hown[VK_OWN_REPLAY] / M, hown[VK_OWN_USNAP] / M,
           hown[VK_OWN_PRESENT] / M, (hown[VK_OWN_OTHER] + hown[VK_OWN_GPA] + hown[VK_OWN_ASTC] + hown[VK_OWN_HEADROOM]) / M,
           proc, (double)ws, sys_used, sys_limit, heap / M, heap_c / M, maps / M);
    uint64_t tex = own[VK_OWN_TEXTURES], pools = own[VK_OWN_POOLS];
    ReportFig f = {usage, blocks, allocs, hblocks, tex, pools, allocs > tex + pools ? allocs - tex - pools : 0, heap_c, maps};
    beyond_log(c, frame, &bp, vb, &f);
    vk_texture_reuse_log(frame);
}

static int staging_fail_now(void) {
    static int64_t at = -1, n;
    if (at < 0) at = cfg_int("NX2SARIA_STAGING_FAIL_AT");
    return at > 0 && plat_inc64(&n) == at;
}

void *vk_staging(VkDeviceSize size, VkBuffer *buf, VmaAllocation *alloc) {
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size ? size : 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (!__atomic_load_n(&stg_pool_tried, __ATOMIC_ACQUIRE)) {
        plat_once(&cmd_once, cmd_init, NULL);
        plat_rmutex_lock(&cmd_lock);
        if (!stg_pool_tried) {
            uint32_t type;
            int keep = c->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? STG_MIN_BLOCKS : STG_MIN_BLOCKS_UMA;
            VmaPoolCreateInfo pi = {.blockSize = STG_BLOCK, .minBlockCount = (size_t)keep};
            if (vmaFindMemoryTypeIndexForBufferInfo(c->vma, &bi, &aci, &type) == VK_SUCCESS) {
                pi.memoryTypeIndex = type;
                if (vmaCreatePool(c->vma, &pi, &stg_pool) != VK_SUCCESS) stg_pool = NULL;
            }
            rt_log("vk: staging pool %s (%d blocks of %llu MiB kept)", stg_pool ? "created" : "unavailable: default pool",
                   keep, (unsigned long long)(STG_BLOCK >> 20));
            __atomic_store_n(&stg_pool_tried, 1, __ATOMIC_RELEASE);
        }
        plat_rmutex_unlock(&cmd_lock);
    }
    if (stg_pool && bi.size <= STG_BLOCK / 2) aci.pool = stg_pool;
    if (!alloc) {
        Staging cs;
        void *cmap;
        if (staging_cache_take(c, bi.size, &cs, &cmap)) {
            *buf = cs.buf;
            if (open_b.nstg == open_b.cap) grow((void **)&open_b.stg, &open_b.cap, sizeof *open_b.stg);
            open_b.stg[open_b.nstg++] = cs;
            return cmap;
        }
    }
    VmaAllocationInfo info;
    VkBuffer b;
    VmaAllocation a;
    VkResult r = aci.pool ? stg_create(c, &bi, &aci, &b, &a, &info, 0) : vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY && aci.pool) {
        aci.pool = NULL;
        r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
    }
    if (r == VK_SUCCESS && staging_fail_now()) {
        vmaDestroyBuffer(c->vma, b, a);
        r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) {
        VkResult first = r;
        uint64_t freed = vk_mem_give_back();
        aci.pool = NULL;
        r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, &info);
        static volatile int32_t told;
        if (plat_inc32(&told) <= 16)
            rt_log("vk: staging buffer of %llu bytes refused (%d): %.1f MiB of kept buffers and spare blocks given back, "
                   "%s", (unsigned long long)size, (int)first, freed / 1048576.0,
                   r == VK_SUCCESS ? "made at the second try" : "refused again");
    }
    if (r != VK_SUCCESS) rt_fatal("vk: staging buffer of %llu bytes failed (%d)", (unsigned long long)size, (int)r);
    vk_texstat_add(VK_TS_STG_NEW, 1, bi.size);
    *buf = b;
    if (alloc) *alloc = a;
    else {
        if (open_b.nstg == open_b.cap) grow((void **)&open_b.stg, &open_b.cap, sizeof *open_b.stg);
        open_b.stg[open_b.nstg++] = (Staging){b, a, bi.size};
        open_b.bytes += bi.size;
    }
    return info.pMappedData;
}

void *vk_staging_at(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_STG_ARENA");
        if (!on) rt_log("vk: staging arena off (NX2SARIA_STG_ARENA=0): every upload stages in a buffer of its own");
    }
    *off = 0;
    if (!on || !open_active || size > ARENA_MAX) return vk_staging(size, buf, NULL);
    VkCtx *c = vk_ctx();
    VkDeviceSize at = (open_b.chunk_used + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);
    if (!open_b.nchunk || at + size > ARENA_CHUNK) {
        StgChunk k;
        VkResult r = VK_INCOMPLETE;
        if (!narena_free) {
            VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ARENA_CHUNK,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
            VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                           .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                    VMA_ALLOCATION_CREATE_MAPPED_BIT};
            if (stg_pool) aci.pool = stg_pool;
            VmaAllocationInfo info;
            r = stg_pool ? stg_create(c, &bi, &aci, &k.buf, &k.alloc, &info, 1)
                         : vmaCreateBuffer(c->vma, &bi, &aci, &k.buf, &k.alloc, &info);
            if (r != VK_SUCCESS && r != VK_INCOMPLETE) return vk_staging(size, buf, NULL);
            if (r == VK_SUCCESS) {
                k.map = info.pMappedData;
                vk_texstat_add(VK_TS_STG_NEW, 1, ARENA_CHUNK);
                if (arena_made++ < 16)
                    rt_log("vk: staging arena chunk %llu made (%llu MiB)", (unsigned long long)arena_made,
                           (unsigned long long)(ARENA_CHUNK >> 20));
            }
        }
        if (r == VK_INCOMPLETE) k = arena_free[--narena_free];
        if (open_b.nchunk == open_b.cap_chunk) grow((void **)&open_b.chunk, &open_b.cap_chunk, sizeof *open_b.chunk);
        open_b.chunk[open_b.nchunk++] = k;
        open_b.bytes += ARENA_CHUNK;
        at = 0;
    }
    StgChunk *k = &open_b.chunk[open_b.nchunk - 1];
    open_b.chunk_used = at + (size ? size : 16);
    arena_ranges++;
    *buf = k->buf, *off = at;
    return k->map + at;
}

uint64_t vk_upload_flush(int wait) {
    if (vk_device_is_lost()) return 0;
    uint64_t f0 = kern_now_ns();
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    VkCtx *c = vk_ctx();
    uint64_t seq = next_seq;
    if (c && open_active) {
        for (int i = 0; i < open_b.nstg; i++) vmaFlushAllocation(c->vma, open_b.stg[i].alloc, 0, VK_WHOLE_SIZE);
        for (int i = 0; i < open_b.nchunk; i++)
            vmaFlushAllocation(c->vma, open_b.chunk[i].alloc, 0, i + 1 < open_b.nchunk ? VK_WHOLE_SIZE : open_b.chunk_used);
        vk_crumb(open_b.cb, VK_CRUMB_UPLOAD_END, open_b.seq);
        if (open_b.ts >= 0) vkCmdWriteTimestamp(open_b.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, up_ts_pool, 2 * (uint32_t)open_b.ts + 1);
        up_n_batches++;
        vkEndCommandBuffer(open_b.cb);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &open_b.cb};
        vk_queue_lock();
        open_b.sub_ns = plat_now_ns();
        VkResult r = vkQueueSubmit(c->gfx_queue, 1, &si, open_b.fence);
        vk_queue_unlock();
        if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY) {
            VkResult first = r;
            int flying = ninflight;
            retire(c, 1);
            collect_locked(c);
            uint64_t freed = vk_mem_give_back();
            vk_queue_lock();
            open_b.sub_ns = plat_now_ns();
            r = vkQueueSubmit(c->gfx_queue, 1, &si, open_b.fence);
            vk_queue_unlock();
            static volatile int32_t told;
            if (plat_inc32(&told) <= 16)
                rt_log("vk: upload submit refused (%d): waited for %d batches in flight, %.1f MiB of kept buffers and "
                       "spare blocks given back, %s", (int)first, flying, freed / 1048576.0,
                       r == VK_SUCCESS ? "taken at the second try" : "refused again");
        }
        if (r != VK_SUCCESS) {
            vk_mem_oom_report("upload submit", r);
            vk_device_lost_check(r, "the submit of an upload batch");
            rt_fatal("vk: upload submit failed (%d)", (int)r);
        }
        if (ninflight == cap_inflight) grow((void **)&inflight, &cap_inflight, sizeof *inflight);
        inflight[ninflight++] = open_b;
        open_active = 0;
    }
    if (c && wait) {
        while (ninflight && done_seq < seq) {
            VkResult w = vkWaitForFences(c->device, 1, &inflight[0].fence, VK_TRUE, UINT64_MAX);
            if (w != VK_SUCCESS) {
                vk_device_lost_check(w, "the wait for an upload batch");
                rt_fatal("vk: the wait for an upload failed (%d)", (int)w);
            }
            retire(c, 0);
        }
    }
    if (c) collect_locked(c);
    plat_rmutex_unlock(&cmd_lock);
    vk_texstat_time(VK_TS_FLUSH, f0);
    return seq;
}

static void defer(int kind, uint64_t handle, VmaAllocation alloc) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    if (ndead == cap_dead) grow((void **)&dead, &cap_dead, sizeof *dead);
    static int stamp = -1;
    if (stamp < 0) stamp = rt_knob("DEFER_STAMP", "1") != 0;
    uint64_t tl = tl_noted, rf = stamp ? vk_exec_recording_fence() : 0;
    if (stamp && !rf) rf = vk_sync_issued() + 1;
    if (rf > tl) tl = rf;
    dead[ndead++] = (Dead){kind, handle, alloc, open_active ? open_b.seq : next_seq, tl};
    if (kind != DEAD_HOST)
        rec_check_handle(kind == DEAD_VIEW ? REC_VIEW : kind == DEAD_BUFFER_VMA ? REC_BUFFER : REC_IMAGE, handle, 0, tl,
                         vk_sync_fence_done());
    plat_rmutex_unlock(&cmd_lock);
}

static int dead_pending(void) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    int n = ndead;
    plat_rmutex_unlock(&cmd_lock);
    return n;
}

void vk_defer_image(VkImage img, VmaAllocation alloc) {
    if (img) defer(alloc ? DEAD_IMAGE_VMA : DEAD_IMAGE, (uint64_t)(uintptr_t)img, alloc);
}
void vk_defer_view(VkImageView view) { if (view) defer(DEAD_VIEW, (uint64_t)(uintptr_t)view, NULL); }
void vk_defer_free(void *p) { if (p) defer(DEAD_HOST, (uint64_t)(uintptr_t)p, NULL); }
void vk_defer_buffer(VkBuffer buf, VmaAllocation alloc) {
    if (buf) defer(DEAD_BUFFER_VMA, (uint64_t)(uintptr_t)buf, alloc);
}
int vk_defer_buffer_try(VkBuffer buf, VmaAllocation alloc) {
    plat_once(&cmd_once, cmd_init, NULL);
    if (!plat_rmutex_trylock(&cmd_lock)) return 0;
    if (buf) defer(DEAD_BUFFER_VMA, (uint64_t)(uintptr_t)buf, alloc);
    plat_rmutex_unlock(&cmd_lock);
    return 1;
}

void vk_mem_note_submit(uint64_t timeline_value) {
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    if (timeline_value > tl_noted) tl_noted = timeline_value;
    plat_rmutex_unlock(&cmd_lock);
}

static void collect_held(VkCtx *c) {
    Dead *out = NULL;
    int nout = 0, cout = 0;
    collect_locked_out(c, &out, &nout, &cout);
    plat_rmutex_unlock(&cmd_lock);
    for (int i = 0; i < nout; i++) destroy_dead(c, &out[i]);
    free(out);
}

void vk_mem_collect(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    plat_once(&cmd_once, cmd_init, NULL);
    plat_rmutex_lock(&cmd_lock);
    collect_held(c);
    pool_dead_drain(c, PD_BY_FINI);
}

#define COLLECT_TRY_MAX 8
static uint64_t collect_skips;
void vk_mem_collect_try(void) {
    static int busy;
    VkCtx *c = vk_ctx();
    if (!c) return;
    plat_once(&cmd_once, cmd_init, NULL);
    if (!plat_rmutex_trylock(&cmd_lock)) {
        if (++busy <= COLLECT_TRY_MAX) {
            __atomic_add_fetch(&collect_skips, 1, __ATOMIC_RELAXED);
            pool_dead_drain(c, PD_BY_COMPLETION);
            return;
        }
        plat_rmutex_lock(&cmd_lock);
    }
    busy = 0;
    collect_held(c);
    pool_dead_drain(c, PD_BY_COMPLETION);
}

void vk_mem_stats(VkMemStats *s) {
    memset(s, 0, sizeof *s);
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) {
        const PoolRec *r = pool_index.e[i].buf;
        if (r->kind == POOL_IMPORT) s->imported_pools++;
        else if (r->kind == POOL_PHYS) s->physical_pools++;
        else s->virtual_pools++;
    }
    plat_mutex_unlock_shared(&pool_lock);
    s->import_failures = (uint64_t)n_import_failures;
    s->upload_batches = next_seq;
}

void vk_mem_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    vk_upload_flush(1);
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    vk_gpa_report();
    vk_gpa_shutdown();
    vk_texture_shutdown();
    plat_rmutex_lock(&cmd_lock);
    retire(c, 1);
    uint64_t keep = tl_noted;
    tl_noted = 0;
    for (int i = 0; i < ndead; i++) dead[i].seq = 0, dead[i].tl = 0;
    collect_locked(c);
    tl_noted = keep;
    for (int i = 0; i < nspare; i++) {
        vkDestroyFence(c->device, spare[i].fence, NULL);
        free(spare[i].stg), free(spare[i].chunk);
    }
    nspare = 0;
    for (int i = 0; i < stg_ncache; i++) vmaDestroyBuffer(c->vma, stg_cache[i].buf, stg_cache[i].alloc);
    stg_ncache = 0, stg_cache_bytes = 0;
    for (int i = 0; i < narena_free; i++) vmaDestroyBuffer(c->vma, arena_free[i].buf, arena_free[i].alloc);
    narena_free = 0;
    if (cmd_pool) vkDestroyCommandPool(c->device, cmd_pool, NULL);
    cmd_pool = VK_NULL_HANDLE;
    if (stg_pool) vmaDestroyPool(c->vma, stg_pool);
    stg_pool = NULL;
    stg_pool_tried = 0;
    plat_rmutex_unlock(&cmd_lock);
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < pool_index.n; i++) destroy_rec(c, pool_index.e[i].buf);
    pool_index.n = 0;
    nm = npend = nwbl = nsnl = snl_copied = nswl = n_gpa_flip = 0;
    swl_marks = -1;
    plat_mutex_lock(&snap_pool_lock);
    for (int i = 0; i < nsnap_pool; i++) vmaDestroyBuffer(c->vma, snap_pool[i].buf, snap_pool[i].alloc);
    nsnap_pool = 0, snap_pool_bytes = 0, nsnap_lists = 0;
    plat_mutex_unlock(&snap_pool_lock);
    plat_mutex_lock(&shadow_lock);
    nstouch = nstab = 0, s_sb_pages = 0;
    s_live_n = s_live_bytes = 0;
    plat_mutex_unlock(&shadow_lock);
    for (int i = 0; i < nretiring; i++) destroy_rec(c, retiring[i]);
    nretiring = 0;
    npwait = 0;
    for (int i = 0; i < npool_dead; i++) destroy_rec(c, pool_dead[i]);
    npool_dead = 0;
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    nphys = 0;
    plat_mutex_unlock(&pool_lock);
    span_gc(0, 1);
    plat_mutex_lock(&gen_lock);
    for (uint64_t i = 0; i < GEN_CHUNKS; i++) free(gen_chunk[i]), gen_chunk[i] = NULL, free(cgen_chunk[i]), cgen_chunk[i] = NULL;
    plat_mutex_unlock(&gen_lock);
}

void vk_mem_sizes(PerfSizes *s) {
    plat_mutex_lock_shared(&pool_lock);
    uint64_t pools = (uint64_t)pool_index.n, phys = (uint64_t)nphys;
    plat_mutex_unlock_shared(&pool_lock);
    plat_rmutex_lock(&cmd_lock);
    uint64_t dead_n = (uint64_t)ndead, infl = (uint64_t)ninflight, spare_b = (uint64_t)nspare, stg_n = (uint64_t)stg_ncache,
             stg_b = (uint64_t)stg_cache_bytes, arena_n = (uint64_t)narena_free;
    plat_rmutex_unlock(&cmd_lock);
    plat_mutex_lock(&snap_pool_lock);
    uint64_t snap_n = (uint64_t)nsnap_pool, snap_b = snap_pool_bytes;
    plat_mutex_unlock(&snap_pool_lock);
    plat_mutex_lock(&shadow_lock);
    uint64_t sb = s_sb_pages;
    plat_mutex_unlock(&shadow_lock);
    perf_size(s, "pool records", pools, 0, PERF_SIZE_NONE);
    perf_size(s, "of them physical", phys, 0, PERF_SIZE_NONE);
    perf_size(s, "VRAM mirrors", (uint64_t)__atomic_load_n(&m_live_n, __ATOMIC_RELAXED), 0,
              (uint64_t)__atomic_load_n(&m_live_bytes, __ATOMIC_RELAXED));
    perf_size(s, "lazy fetches that found every slot busy", (uint64_t)__atomic_load_n(&lz_slot_waits, __ATOMIC_RELAXED),
              0, PERF_SIZE_NONE);
    perf_size(s, "deferred destructions", dead_n, 0, PERF_SIZE_NONE);
    perf_size(s, "finalized pools destroyed after the GPU", __atomic_load_n(&n_pool_destroyed, __ATOMIC_RELAXED), 0,
              PERF_SIZE_NONE);
    int64_t dn[2], dns[2], dmax[2];
    for (int i = 0; i < 2; i++)
        dn[i] = plat_xchg64(&pd_n[i], 0), dns[i] = plat_xchg64(&pd_ns[i], 0), dmax[i] = plat_xchg64(&pd_max[i], 0);
    char pd[240];
    snprintf(pd, sizeof pd,
             "of them since the last line: %lld on the completion thread (%.1f ms, longest drain %.1f ms), %lld by "
             "pool_fini (%.1f ms, longest drain %.1f ms)",
             (long long)dn[PD_BY_COMPLETION], dns[PD_BY_COMPLETION] / 1e6, dmax[PD_BY_COMPLETION] / 1e6,
             (long long)dn[PD_BY_FINI], dns[PD_BY_FINI] / 1e6, dmax[PD_BY_FINI] / 1e6);
    perf_size(s, pd, PERF_SIZE_NONE, 0, PERF_SIZE_NONE);
    perf_size(s, "completion collects that found the upload lock busy",
              __atomic_load_n(&collect_skips, __ATOMIC_RELAXED), 0, PERF_SIZE_NONE);
    perf_size(s, "upload batches in flight", infl, 0, PERF_SIZE_NONE);
    perf_size(s, "spare upload batches", spare_b, 0, PERF_SIZE_NONE);
    perf_size(s, "staging cache", stg_n, STG_CACHE, stg_b);
    perf_size(s, "free staging arena chunks", arena_n, ARENA_KEEP, arena_n * ARENA_CHUNK);
    perf_size(s, "write-back snapshot pool", snap_n, SNAP_POOL_N, snap_b);
    perf_size(s, "shadow base copies", sb, 0, sb * SB_PAGE);
    perf_size(s, "page generations handed out", (uint64_t)__atomic_load_n(&gen_now, __ATOMIC_RELAXED), 0, PERF_SIZE_NONE);
    uint64_t gch = __atomic_load_n(&n_gen_chunks, __ATOMIC_RELAXED);
    perf_size(s, "page generation table chunks", gch, 0,
              gch * GEN_PER_CHUNK * sizeof **gen_chunk + sizeof reg_max + sizeof creg_max);
}

#define TEX_POOL_PCT 90
#define TEX_POOL_MIN ((uint64_t)16 << 20)
typedef struct { VkBuffer buf; VkDeviceMemory mem; } TexPoolGone;
static uint64_t tp_n, tp_bytes, tp_back, tp_back_bytes;

static void tex_pool_free(void *arg) {
    TexPoolGone *g = arg;
    VkCtx *c = vk_ctx();
    if (c) vkDestroyBuffer(c->device, g->buf, NULL), vkFreeMemory(c->device, g->mem, NULL);
    free(g);
}

static int tex_pool_back(uint64_t addr) {
    VkCtx *c = vk_ctx();
    int ok = 0;
    plat_mutex_lock(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, 1);
    PoolRec *r = e ? e->buf : NULL;
    if (c && r && r->kind == POOL_IMPORT && r->tex_off) {
        mem_thaw(r->lo, r->size);
        if (import_pool(c, r, r->lo, r->size)) {
            r->tex_off = 0, ok = 1;
            r->gpa_rec = vk_gpa_map(r->import_lo, r->import_hi, r->buf, r->import_lo);
            __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
            tp_back++, tp_back_bytes += r->import_hi - r->import_lo;
        }
    }
    plat_mutex_unlock(&pool_lock);
    if (ok && tp_back <= 8)
        rt_log("mem: a pool given back as texture storage was resolved and is imported again (%#llx; %llu so far)",
               (unsigned long long)addr, (unsigned long long)tp_back);
    return ok;
}

static int vk_mem_tex_pools_now(void) {
    uint64_t n0 = __atomic_load_n(&tp_n, __ATOMIC_RELAXED);
    vk_mem_tex_pools(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
    return __atomic_load_n(&tp_n, __ATOMIC_RELAXED) != n0;
}

void vk_mem_tex_pools(uint64_t frame) {
    static int on = -1;
    VkCtx *c = vk_ctx();
    if (on < 0) {
        on = cfg_bool("NX2SARIA_POOL_TEX_RELEASE") && !shadow_on && !dmabuf_on;
        rt_log("mem: imports of pools that hold only texture storage %s (NX2SARIA_POOL_TEX_RELEASE)",
               on ? "are given back" : "stay");
    }
    if (!on || !c) return;
    int nrg = 0;
    uint64_t *rg = vk_texture_cpu_ranges(&nrg);
    if (!rg) return;
    enum { CAND_MAX = 64 };
    PoolRec *cand[CAND_MAX];
    int nc = 0, rgi = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < pool_index.n && nc < CAND_MAX; i++) {
        PoolRec *r = pool_index.e[i].buf;
        if (r->kind != POOL_IMPORT || !r->buf || !r->mem || r->span || r->smap || r->mbuf || r->tex_off ||
            __atomic_load_n(&r->used, __ATOMIC_RELAXED))
            continue;
        uint64_t n = r->import_hi - r->import_lo, cov = 0, at = r->import_lo;
        if (n < TEX_POOL_MIN) continue;
        while (rgi < nrg && rg[2 * rgi + 1] <= r->import_lo) rgi++;
        for (int q = rgi; q < nrg && rg[2 * q] < r->import_hi; q++) {
            uint64_t lo = rg[2 * q] > at ? rg[2 * q] : at, hi = rg[2 * q + 1] < r->import_hi ? rg[2 * q + 1] : r->import_hi;
            if (hi > lo) cov += hi - lo, at = hi;
        }
        if (cov * 100 >= n * TEX_POOL_PCT) cand[nc++] = r;
    }
    plat_mutex_unlock_shared(&pool_lock);
    free(rg);
    for (int i = 0; i < nc; i++) {
        PoolRec *r = cand[i];
        plat_mutex_lock(&pool_lock);
        const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)r->lo, 1);
        int go = e && e->buf == r && r->buf && !r->tex_off && !r->mbuf && !__atomic_load_n(&r->used, __ATOMIC_RELAXED);
        TexPoolGone *g = go ? malloc(sizeof *g) : NULL;
        uint32_t rec = 0;
        uint64_t lo = r->import_lo, hi = r->import_hi;
        if (g) {
            *g = (TexPoolGone){r->buf, r->mem};
            rec = r->gpa_rec;
            r->buf = VK_NULL_HANDLE, r->mem = VK_NULL_HANDLE, r->gpa_rec = 0, r->tex_off = 1;
            __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
            tp_n++, tp_bytes += hi - lo;
        }
        plat_mutex_unlock(&pool_lock);
        if (!g) continue;
        if (rec) vk_gpa_unmap(lo, hi, rec, gpa_cover);
        vk_completion_add(vk_sync_issued() + 1, tex_pool_free, g);
    }
    if (nc)
        rt_log("mem: frame %llu: imports given back as texture storage %llu (%.0f MiB) so far, imported again %llu "
               "(%.0f MiB)", (unsigned long long)frame, (unsigned long long)tp_n, tp_bytes / 1048576.0,
               (unsigned long long)tp_back, tp_back_bytes / 1048576.0);
}
