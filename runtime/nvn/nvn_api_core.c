// Contains code Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "motion_owner.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "platform/platform.h"

#define DEV_MAGIC MAGIC('D', 'E', 'V', 0)
#define QUEUE_MAGIC MAGIC('Q', 'U', 'E', 'U')
#define BUF_MAGIC MAGIC('B', 'U', 'F', 0)
#define SYNC_MAGIC MAGIC('S', 'Y', 'N', 'C')
#define EVENT_MAGIC MAGIC('E', 'V', 'N', 'T')

void nvn_called(uint8_t *seen, const char *name) {
    static _Thread_local uint32_t calls;
    if (++calls == 1024) {
        __atomic_fetch_add(&gfx_counters.nvn_calls, calls, __ATOMIC_RELAXED);
        calls = 0;
    }
    if (__atomic_load_n(seen, __ATOMIC_RELAXED) || __atomic_exchange_n(seen, 1, __ATOMIC_RELAXED)) return;
    uint64_t n = __atomic_add_fetch(&gfx_counters.nvn_distinct, 1, __ATOMIC_RELAXED);
    NVN_LOG("first call %llu: %s", (unsigned long long)n, name);
}

uint64_t nvn_new_serial(void) {
    static uint64_t n;
    return __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
}

static uint64_t last_fence;
uint64_t nvn_fence_last(void) { return __atomic_load_n(&last_fence, __ATOMIC_ACQUIRE); }

static int depth_minus_w;
int nvn_depth_minus_w(void) { return __atomic_load_n(&depth_minus_w, __ATOMIC_RELAXED); }

void nvn_guest_written(uint64_t addr, uint64_t size) {
    if (size) gfx_backend()->guest_written(addr, size);
}

static void ov_memcpy(Cpu *c) {
    uint64_t dst = arg_x(c, 0), src = arg_x(c, 1), n = arg_x(c, 2);
    if (n >= 1024) gfx_backend()->guest_read(src, n);
    memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
    ret_x(c, dst);
}
HOST_OVERRIDE("memcpy", ov_memcpy);

__attribute__((weak)) void nvn_submit_prepare(const uint64_t *handles, uint64_t *out, int n) {
    memcpy(out, handles, sizeof *out * (size_t)n);
}
__attribute__((weak)) void nvn_submit_done(const uint64_t *handles, const uint64_t *out, int n) {
    (void)handles, (void)out, (void)n;
}
__attribute__((weak)) void nvn_submit_prepare_async(const uint64_t *handles, uint64_t *out, int n) {
    memcpy(out, handles, sizeof *out * (size_t)n);
}
__attribute__((weak)) int gfx_replay_async(void) { return 0; }
__attribute__((weak)) uint64_t gfx_replayed_submits(void) { return nvn_submit_count(); }
__attribute__((weak)) void gfx_event_signalling(const void *value, uint32_t to) { (void)value, (void)to; }

static PlatMutex pool_lock = PLAT_MUTEX_INIT;
typedef struct { uint64_t lo, hi; } Span;
enum { GVA_MAX = 1024 };
static Span gva[GVA_MAX];
static int ngva;
static pidx pool_index;

static uint64_t gva_alloc(uint64_t size) {
    plat_mutex_lock(&pool_lock);
    uint64_t at = RT_GPU_VA_BASE, r = 0;
    int i = 0;
    for (; i < ngva && gva[i].lo - at < size; i++) at = gva[i].hi;
    if (ngva < GVA_MAX && size <= RT_GPU_VA_BASE + RT_GPU_VA_SIZE - at) {
        memmove(gva + i + 1, gva + i, (size_t)(ngva - i) * sizeof *gva);
        gva[i] = (Span){at, at + size};
        ngva++;
        r = at;
    }
    plat_mutex_unlock(&pool_lock);
    return r;
}

static void gva_free(uint64_t lo) {
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < ngva; i++)
        if (gva[i].lo == lo) {
            memmove(gva + i, gva + i + 1, (size_t)(ngva - i - 1) * sizeof *gva);
            ngva--;
            break;
        }
    plat_mutex_unlock(&pool_lock);
}

static void index_add(const pool_t *o) {
    uintptr_t lo = (uintptr_t)o->mem;
    pidx_ent e = {.lo = lo, .hi = lo + o->size, .base = lo, .gpu = lo, .key = o};
    plat_mutex_lock(&pool_lock);
    pidx_remove(&pool_index, o, NULL);
    int ok = pidx_insert(&pool_index, &e);
    plat_mutex_unlock(&pool_lock);
    if (!ok) rt_fatal("nvn: out of memory for the pool index");
}

static void index_del(const pool_t *o) {
    plat_mutex_lock(&pool_lock);
    pidx_remove(&pool_index, o, NULL);
    plat_mutex_unlock(&pool_lock);
}

const pool_t *nvn_pool_find(uint64_t addr, uint64_t len) {
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, len);
    const pool_t *p = e ? e->key : NULL;
    plat_mutex_unlock_shared(&pool_lock);
    return p;
}

static int pool_place(pool_t *o) {
    int phys = (o->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) != 0, virt = (o->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) != 0;
    if (!phys && !virt) return 1;
    const char *kind = phys ? "physical" : "virtual";
    if (phys && virt) { rt_log("nvn: pool flags %#x are both PHYSICAL and VIRTUAL", o->flags); return 0; }
    if (!o->size || o->size % NVN_VPOOL_PAGE) {
        rt_log("nvn: %s pool size %#llx is not a multiple of 64 KiB", kind, (unsigned long long)o->size);
        return 0;
    }
    if (virt && o->mem) { rt_log("nvn: virtual pool given storage %p", (void *)o->mem); return 0; }
    if (phys && !o->mem) { rt_log("nvn: physical pool without storage"); return 0; }
    if (!(o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) {
        if (virt) {
            rt_log("nvn: CPU-visible virtual pool (flags %#x, size %#llx) refused", o->flags, (unsigned long long)o->size);
            return 0;
        }
        rt_log("nvn: CPU-visible physical pool %p (flags %#x, size %#llx) keeps the game's storage", (void *)o->mem,
               o->flags, (unsigned long long)o->size);
        return 1;
    }
    uint64_t a = gva_alloc(o->size);
    if (!a) {
        rt_log("nvn: GPU address window full, %s pool of %#llx bytes refused", kind, (unsigned long long)o->size);
        return 0;
    }
    o->mem = (uint8_t *)(uintptr_t)a;
    o->span = o->size;
    return 1;
}

static int pool_gpu_visible(const pool_t *o) { return !(o->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS); }

static const int devinfo[] = {
    [0x00] = 55, [0x01] = 15,
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};

NVN_API(nvnDeviceBuilderSetDefaults) { memset(arg_p(c, 0), 0, sizeof(dev_builder_t)); }
NVN_API(nvnDeviceBuilderSetFlags) { ((dev_builder_t *)arg_p(c, 0))->flags = (uint32_t)arg_x(c, 1); }
NVN_API(nvnDeviceInitialize) {
    dev_t_ *o = arg_p(c, 0);
    const dev_builder_t *b = arg_p(c, 1);
    memset(o, 0, sizeof *o);
    o->magic = DEV_MAGIC;
    o->flags = b->flags;
    NVN_LOG("DeviceInitialize %p flags %#x, back end %s", (void *)o, o->flags, gfx_backend()->name);
    ret_x(c, 1);
}
NVN_API(nvnDeviceFinalize) { ((dev_t_ *)arg_p(c, 0))->magic = 0; }
NVN_API(nvnDeviceGetInteger) {
    unsigned w = (unsigned)arg_x(c, 1);
    int *out = arg_p(c, 2);
    if (w >= sizeof devinfo / sizeof *devinfo) {
        if (w == 0x67) {
            if (out) *out = 0x1000;
            return;
        }
        static volatile uint64_t seen[4];
        if (w < 256 && !(__atomic_fetch_or(&seen[w / 64], 1ull << (w % 64), __ATOMIC_RELAXED) & (1ull << (w % 64))))
        {
            char at[160];
            rt_log("nvn: NVN2 device property %#x (unknown here) answered 0, asked from %s", w,
                   ldr_describe(c->x[30], at, sizeof at));
        }
        if (out) *out = 0;
        return;
    }
    *out = w <= NVN_DEVICE_INFO_API_MINOR_VERSION ? rt_nvn_api_version[w] : devinfo[w];
    NVN_LOG("DeviceGetInteger %#x -> %d", w, *out);
}
NVN_API(nvnDeviceRegisterFastClearColor) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearColori) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearColorui) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearDepth) { ret_x(c, 0); }
NVN_API(nvnDeviceGetImageHandle) { ret_x(c, (uint64_t)(int64_t)(int)arg_x(c, 1)); }
NVN_API(nvnDeviceGetTextureHandle) {
    int tid = (int)arg_x(c, 1), sid = (int)arg_x(c, 2);
    if (tid < 0 || tid >= (1 << 20) || sid < 0 || sid >= (1 << 12)) {
        static int said;
        if (said < 8 && ++said)
            rt_log("nvn: texture handle asked for with ids out of range (texture %d, sampler %d): the handle of the "
                   "reserved slot 0 is given, which draws the stand-in", tid, sid);
        if (tid < 0 || tid >= (1 << 20)) tid = 0;
        if (sid < 0 || sid >= (1 << 12)) sid = 0;
    }
    ret_x(c, (uint64_t)tid | (uint64_t)sid << 20);
}
NVN_API(nvnDeviceGetTexelFetchHandle) {
    int tid = (int)arg_x(c, 1);
    if (tid < 0 || tid >= (1 << 20)) rt_fatal("nvn: texel fetch id out of range %d", tid);
    ret_x(c, (uint64_t)tid);
}
NVN_API(nvnDeviceGetSeparateTextureHandle) { ret_x(c, (uint64_t)(int64_t)(int)arg_x(c, 1)); }
NVN_API(nvnDeviceGetSeparateSamplerHandle) { ret_x(c, (uint64_t)(int64_t)(int)arg_x(c, 1) << 20); }
NVN_API(nvnDeviceInstallDebugCallback) {
    dev_t_ *o = arg_p(c, 0);
    o->dbg_cb = nvn_arg_bool(c, 3) ? arg_p(c, 1) : NULL;
    o->dbg_data = arg_p(c, 2);
}
NVN_API(nvnDeviceSetWindowOriginMode) {
    uint32_t m = (uint32_t)arg_x(c, 1);
    ((dev_t_ *)arg_p(c, 0))->origin = m;
    NVN_LOG("window origin %s", m ? "upper left (as D3D and Vulkan)" : "lower left (GL)");
}
NVN_API(nvnDeviceSetDepthMode) {
    uint32_t m = (uint32_t)arg_x(c, 1);
    ((dev_t_ *)arg_p(c, 0))->depth_mode = m;
    __atomic_store_n(&depth_minus_w, m == NVN_DEPTH_MODE_NEAR_IS_MINUS_W, __ATOMIC_RELAXED);
    NVN_LOG("depth mode %s", m ? "near is 0 (as D3D and Vulkan)" : "near is -w (GL): vertex shaders map z to (z+w)/2");
}
NVN_API(nvnDeviceFinalizeCommandHandle) { (void)c; }

NVN_API(nvnMemoryPoolBuilderSetDefaults) { memset(arg_p(c, 0), 0, sizeof(pool_builder_t)); }
NVN_API(nvnMemoryPoolBuilderSetDevice) { ((pool_builder_t *)arg_p(c, 0))->dev = arg_p(c, 1); }
NVN_API(nvnMemoryPoolBuilderSetStorage) {
    pool_builder_t *b = arg_p(c, 0);
    b->mem = arg_p(c, 1);
    b->size = arg_x(c, 2);
}
NVN_API(nvnMemoryPoolBuilderSetFlags) { ((pool_builder_t *)arg_p(c, 0))->flags = (uint32_t)arg_x(c, 1); }
static void pool_finalize(pool_t *o);

NVN_API(nvnMemoryPoolInitialize) {
    NVN_BLOCKING();
    pool_t *o = arg_p(c, 0);
    const pool_builder_t *s = arg_p(c, 1);
    if (o->magic == NVN_POOL_MAGIC) {
        static uint64_t again;
        uint64_t n = __atomic_add_fetch(&again, 1, __ATOMIC_RELAXED);
        if (n <= 4 || !(n & (n - 1)))
            rt_log("nvn: MemoryPoolInitialize: pool %p (%#llx bytes) initialized again without Finalize, finalized first "
                   "(%llu so far)", (void *)o->mem, (unsigned long long)o->size, (unsigned long long)n);
        pool_finalize(o);
    }
    memset(o, 0, sizeof *o);
    o->magic = NVN_POOL_MAGIC;
    o->flags = s->flags;
    o->mem = o->game_mem = s->mem;
    o->size = s->size;
    int ok = pool_place(o);
    if (ok && !o->mem) {
        rt_log("nvn: MemoryPoolInitialize: pool without storage, flags %#x size %#llx", o->flags,
               (unsigned long long)o->size);
        ok = 0;
    }
    if (ok && !gfx_backend()->pool_init(o)) {
        rt_log("nvn: MemoryPoolInitialize: the %s back end refused pool %p (flags %#x, size %#llx)", gfx_backend()->name,
               (void *)o->mem, o->flags, (unsigned long long)o->size);
        ok = 0;
    }
    if (ok && pool_gpu_visible(o)) index_add(o);
    if (!ok) {
        if (nvn_gpu_only((uintptr_t)o->mem)) gva_free((uintptr_t)o->mem);
        o->magic = 0;
    }
    NVN_LOG("MemoryPool %p mem %p size %#llx flags %#x storage %p%s", (void *)o, (void *)o->mem,
            (unsigned long long)o->size, o->flags, (void *)o->game_mem, ok ? "" : " failed");
    ret_x(c, ok);
}
static void pool_finalize(pool_t *o) {
    if (o->magic != NVN_POOL_MAGIC) return;
    motion_owner_forget_range((uint64_t)(uintptr_t)o->mem,o->size);
    if (pool_gpu_visible(o)) index_del(o);
    gfx_backend()->pool_fini(o);
    if (nvn_gpu_only((uintptr_t)o->mem)) gva_free((uintptr_t)o->mem);
    o->mem = o->game_mem;
    o->span = 0;
    o->magic = 0;
}

NVN_API(nvnMemoryPoolFinalize) {
    NVN_BLOCKING();
    pool_finalize(arg_p(c, 0));
}
static uint64_t flush_stat[2][3][2];

static void flush_note(int kind, const pool_t *p, uint64_t size) {
    uint32_t f = p ? p->flags : 0;
    int k = f & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS ? 2 : f & NVN_MEMORY_POOL_FLAGS_CPU_CACHED ? 0
          : f & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED ? 1 : 2;
    __atomic_fetch_add(&flush_stat[kind][k][0], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&flush_stat[kind][k][1], size, __ATOMIC_RELAXED);
}

void nvn_flush_stats(uint64_t out[2][3][2]) {
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 2; j++) out[i][k][j] = __atomic_exchange_n(&flush_stat[i][k][j], 0, __ATOMIC_RELAXED);
}

NVN_API(nvnMemoryPoolFlushMappedRange) {
    const pool_t *p = arg_p(c, 0);
    int64_t off = (int64_t)arg_x(c, 1);
    uint64_t size = arg_x(c, 2);
    if (rt_trace & RT_TRACE_PERF) flush_note(0, p, size);
    if (p && off >= 0 && (uint64_t)off < p->size) nvn_pool_flushed(p, (uint64_t)off, size < p->size - (uint64_t)off ? size : p->size - (uint64_t)off);
}
int nvn_no_object(Cpu *c, const char *what) {
    if (arg_x(c, 0)) return 0;
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 8)
        rt_log("nvn: %s of no object (caller %#llx): 0", what, (unsigned long long)c->x[30]);
    ret_x(c, 0);
    return 1;
}

NVN_API(nvnMemoryPoolGetFlags) {
    if (nvn_no_object(c, "nvnMemoryPoolGetFlags")) return;
    ret_x(c, ((const pool_t *)arg_p(c, 0))->flags);
}
NVN_API(nvnMemoryPoolMap) {
    if (nvn_no_object(c, "nvnMemoryPoolMap")) return;
    const pool_t *o = arg_p(c, 0);
    if (o->magic != NVN_POOL_MAGIC || (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) { ret_x(c, 0); return; }
    ret_x(c, (uintptr_t)o->mem);
}
static int check_request(const pool_t *v, const nvn_mapping_t *r, int i) {
    uint64_t end = (uint64_t)r->virtual_offset + r->size;
    if (r->virtual_offset < 0 || r->physical_offset < 0 || !r->size) {
        rt_log("nvn: MapVirtual request %d has a negative or empty range", i);
        return 0;
    }
    if (((uint64_t)r->virtual_offset | (uint64_t)r->physical_offset | r->size) % NVN_VPOOL_PAGE) {
        rt_log("nvn: MapVirtual request %d not 64 KiB aligned (v %#llx p %#llx n %#llx)", i,
               (unsigned long long)r->virtual_offset, (unsigned long long)r->physical_offset, (unsigned long long)r->size);
        return 0;
    }
    if (end < r->size || end > v->size) {
        rt_log("nvn: MapVirtual request %d exceeds the virtual pool (%#llx > %#llx)", i, (unsigned long long)end,
               (unsigned long long)v->size);
        return 0;
    }
    if (!r->physical_pool) return 1;
    const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
    if (p->magic != NVN_POOL_MAGIC || !(p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) {
        rt_log("nvn: MapVirtual request %d source %p is not a physical pool", i, (const void *)p);
        return 0;
    }
    uint64_t pend = (uint64_t)r->physical_offset + r->size;
    if (pend < r->size || pend > p->size) {
        rt_log("nvn: MapVirtual request %d exceeds the physical pool (%#llx > %#llx)", i, (unsigned long long)pend,
               (unsigned long long)p->size);
        return 0;
    }
    return 1;
}
NVN_API(nvnMemoryPoolMapVirtual) {
    NVN_BLOCKING();
    pool_t *v = arg_p(c, 0);
    motion_owner_forget_range((uint64_t)(uintptr_t)v->mem,v->size);
    int n = (int)arg_x(c, 1);
    const nvn_mapping_t *r = arg_p(c, 2);
    if (v->magic != NVN_POOL_MAGIC || !(v->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || !v->mem) {
        rt_log("nvn: MapVirtual target %p is not a virtual pool", (void *)v);
        ret_x(c, 0);
        return;
    }
    if (n < 0 || (n && !r)) { rt_log("nvn: MapVirtual with %d requests at %p", n, (const void *)r); ret_x(c, 0); return; }
    for (int i = 0; i < n; i++)
        if (!check_request(v, &r[i], i)) { ret_x(c, 0); return; }
    if (!gfx_backend()->pool_map_virtual(v, n, r))
        rt_fatal("nvn: MapVirtual of %d requests failed after validation; virtual pool %p is inconsistent", n, (void *)v);
    NVN_LOG("MapVirtual pool %p, %d requests", (void *)v->mem, n);
    ret_x(c, 1);
}
NVN_API(nvnMemoryPoolGetBufferAddress) {
    if (nvn_no_object(c, "nvnMemoryPoolGetBufferAddress")) return;
    ret_x(c, (uintptr_t)((const pool_t *)arg_p(c, 0))->mem);
}

NVN_API(nvnBufferBuilderSetDevice) { ((buf_builder_t *)arg_p(c, 0))->dev = arg_p(c, 1); }
NVN_API(nvnBufferBuilderSetDefaults) {
    buf_builder_t *b = arg_p(c, 0);
    NVNdevice *d = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = d;
}
NVN_API(nvnBufferBuilderSetStorage) {
    buf_builder_t *b = arg_p(c, 0);
    b->pool = arg_p(c, 1);
    b->off = (int64_t)arg_x(c, 2);
    b->size = arg_x(c, 3);
}
NVN_API(nvnBufferInitialize) {
    buf_t *o = arg_p(c, 0);
    const buf_builder_t *s = arg_p(c, 1);
    if (!s->pool || s->pool->magic != NVN_POOL_MAGIC || s->off < 0 || (uint64_t)s->off > s->pool->size ||
        s->size > s->pool->size - (uint64_t)s->off) {
        ret_x(c, 0);
        return;
    }
    motion_owner_forget((uintptr_t)o);
    *o = (buf_t){BUF_MAGIC, s->pool, s->off, s->size};
    ret_x(c, 1);
}
NVN_API(nvnBufferFinalize) { motion_owner_forget((uintptr_t)arg_p(c, 0)); memset(arg_p(c, 0), 0, sizeof(buf_t)); }
static uint64_t buf_address(const buf_t *o) { return o->pool && o->pool->mem ? (uintptr_t)o->pool->mem + (uint64_t)o->off : 0; }
static uint64_t no_pool_mapping(const buf_t *o, uint64_t lr) {
    static uint8_t first[64 << 10] __attribute__((aligned(256)));
    static void *block = first;
    static uint64_t block_size = sizeof first;
    static int told;
    static PlatMutex mu = PLAT_MUTEX_INIT;
    uint64_t want = o && o->magic == BUF_MAGIC && o->size <= (256ull << 20) ? o->size : 0;
    plat_mutex_lock(&mu);
    if (want > block_size) {
        void *b = calloc(1, want);
        if (b) block = b, block_size = want;
    }
    void *r = block;
    if (told++ < 4)
        rt_log("nvn: nvnBufferMap of a buffer with no memory pool (caller %#llx): %llu bytes of scratch memory",
               (unsigned long long)lr, (unsigned long long)block_size);
    plat_mutex_unlock(&mu);
    return (uintptr_t)r;
}
NVN_API(nvnBufferMap) {
    const buf_t *o = arg_p(c, 0);
    if (!o || !o->pool) { ret_x(c, no_pool_mapping(o, c->x[30])); return; }
    uint64_t a = buf_address(o);
    ret_x(c, nvn_gpu_only(a) ? 0 : a);
}
NVN_API(nvnBufferGetAddress) {
    const buf_t *b = arg_p(c, 0);
    uint64_t address = buf_address(b);

    ret_x(c, address);
}
NVN_API(nvnBufferGetMemoryPool) { ret_x(c, (uintptr_t)((const buf_t *)arg_p(c, 0))->pool); }
NVN_API(nvnBufferGetMemoryOffset) { ret_x(c, (uint64_t)((const buf_t *)arg_p(c, 0))->off); }
NVN_API(nvnBufferGetSize) { ret_x(c, ((const buf_t *)arg_p(c, 0))->size); }
NVN_API(nvnBufferFlushMappedRange) {
    const buf_t *o = arg_p(c, 0);
    int64_t off = (int64_t)arg_x(c, 1);
    uint64_t size = arg_x(c, 2);
    if (off < 0 || (uint64_t)off > o->size || size > o->size - (uint64_t)off)
        rt_fatal("nvn: BufferFlushMappedRange %#llx+%#llx outside the buffer (%#llx bytes)", (unsigned long long)off,
                 (unsigned long long)size, (unsigned long long)o->size);
    if (rt_trace & RT_TRACE_PERF) flush_note(1, o->pool, size);
    uint64_t a = buf_address(o);
    if (a && !nvn_gpu_only(a)) nvn_guest_written(a + (uint64_t)off, size);
}

NVN_API(nvnSyncInitialize) {
    *(sync_t *)arg_p(c, 0) = (sync_t){SYNC_MAGIC, 0, 0};
    ret_x(c, 1);
}
NVN_API(nvnSyncFinalize) { ((sync_t *)arg_p(c, 0))->magic = 0; }
NVN_API(nvnQueueFenceSync) {
    sync_t *s = arg_p(c, 1);
    s->fenced = 1;
    s->fence = nvn_fence_last();
}
NVN_API(nvnQueueWaitSync) {
    const sync_t *s = arg_p(c, 1);
    if (s && !s->fenced) {
        static volatile int32_t logged;
        if (!plat_xchg32(&logged, 1)) rt_log("nvn: QueueWaitSync on a sync not fenced yet: not waited for");
    }
    ret_x(c, 1);
}
NVN_API(nvnSyncWait) {
    const sync_t *s = arg_p(c, 0);
    uint64_t timeout = arg_x(c, 1);
    if (!s->fenced) { ret_x(c, NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED); return; }
    const GfxBackend *b = gfx_backend();
    if (b->fence_done() >= s->fence) { b->cpu_synced(s->fence); ret_x(c, NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED); return; }
    int ok;
    {
        NVN_BLOCKING();
        ok = b->game_wait(s->fence, timeout);
    }
    if (ok) b->cpu_synced(s->fence);
    ret_x(c, ok ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
}
NVN_API(nvnEventBuilderSetDefaults) { memset(arg_p(c, 0), 0, sizeof(ev_builder_t)); }
NVN_API(nvnEventBuilderSetStorage) { *(ev_builder_t *)arg_p(c, 0) = (ev_builder_t){arg_p(c, 1), (int64_t)arg_x(c, 2)}; }
NVN_API(nvnEventInitialize) {
    const ev_builder_t *s = arg_p(c, 1);
    if (!s->pool || !s->pool->mem || nvn_gpu_only((uintptr_t)s->pool->mem)) {
        rt_log("nvn: EventInitialize without CPU-visible storage (pool %p)", (const void *)s->pool);
        ret_x(c, 0);
        return;
    }
    *(ev_t *)arg_p(c, 0) = (ev_t){EVENT_MAGIC, (volatile uint32_t *)(s->pool->mem + s->off)};
    ret_x(c, 1);
}
NVN_API(nvnEventFinalize) { ((ev_t *)arg_p(c, 0))->magic = 0; }
NVN_API(nvnEventGetValue) { ret_x(c, *((const ev_t *)arg_p(c, 0))->val); }
NVN_API(nvnEventSignal) {
    const ev_t *e = arg_p(c, 0);
    uint32_t v = (uint32_t)arg_x(c, 2);
    int add = (uint32_t)arg_x(c, 1) == NVN_EVENT_SIGNAL_MODE_ADD;
    gfx_event_signalling((const void *)e->val, add ? *e->val + v : v);
    if (add) *e->val += v;
    else *e->val = v;
}

NVN_API(nvnQueueBuilderSetDefaults) {
    queue_builder_t *b = arg_p(c, 0);
    NVNdevice *d = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = d;
    b->cmd_sz = 65536;
    b->comp_sz = 262144;
    b->ctl_sz = 16384;
    b->flush = 4096;
}
NVN_API(nvnQueueBuilderSetDevice) { ((queue_builder_t *)arg_p(c, 0))->dev = arg_p(c, 1); }
NVN_API(nvnQueueBuilderSetFlags) { ((queue_builder_t *)arg_p(c, 0))->flags = (uint32_t)arg_x(c, 1); }
NVN_API(nvnQueueBuilderSetCommandMemorySize) { ((queue_builder_t *)arg_p(c, 0))->cmd_sz = arg_x(c, 1); }
NVN_API(nvnQueueBuilderSetComputeMemorySize) { ((queue_builder_t *)arg_p(c, 0))->comp_sz = arg_x(c, 1); }
NVN_API(nvnQueueBuilderSetControlMemorySize) { ((queue_builder_t *)arg_p(c, 0))->ctl_sz = arg_x(c, 1); }
NVN_API(nvnQueueBuilderSetCommandFlushThreshold) { ((queue_builder_t *)arg_p(c, 0))->flush = arg_x(c, 1); }
NVN_API(nvnQueueBuilderGetQueueMemorySize) {
    const queue_builder_t *b = arg_p(c, 0);
    ret_x(c, (b->cmd_sz + b->comp_sz + b->ctl_sz + 4095) & ~(uint64_t)4095);
}
NVN_API(nvnQueueBuilderSetQueueMemory) {
    queue_builder_t *b = arg_p(c, 0);
    b->mem = arg_p(c, 1);
    b->mem_sz = arg_x(c, 2);
}
NVN_API(nvnQueueInitialize) {
    queue_t *q = arg_p(c, 0);
    *q = (queue_t){QUEUE_MAGIC, ((const queue_builder_t *)arg_p(c, 1))->dev, 0};
    NVN_LOG("QueueInitialize %p", (void *)q);
    ret_x(c, 1);
}
NVN_API(nvnQueueFinalize) { ((queue_t *)arg_p(c, 0))->magic = 0; }
NVN_API(nvnQueueGetError) {
    NVNqueueErrorInfo *info = arg_p(c, 1);
    if (info) memset(info, 0, sizeof *info);
    ret_x(c, NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR);
}
NVN_API(nvnQueueFlush) { (void)c; }
NVN_API(nvnQueueFinish) {
    {
        NVN_BLOCKING();
        gfx_backend()->finish();
    }
    gfx_backend()->cpu_synced(gfx_backend()->fence_done());
}
#define LOD_RING 64
_Thread_local uintptr_t gfx_submit_queue;
int nvn_submits_in_frame;
static uint64_t lod_entered, lod_issued, lod_fence[LOD_RING];

void nvn_call_note(int present, uintptr_t q, int enter) {
    if (!(rt_trace & RT_TRACE_PERF)) return;
    static volatile int32_t inside;
    static volatile int32_t overlaps;
    static PlatMutex seen_lock = PLAT_MUTEX_INIT;
    static struct { uint32_t tid; uintptr_t q; int present; } seen[32];
    static int nseen;
    if (!enter) {
        plat_dec32(&inside);
        return;
    }
    int32_t n = plat_inc32(&inside);
    uint32_t tid = plat_thread_id();
    if (n > 1) {
        int32_t k = plat_inc32(&overlaps);
        if (k <= 16 || !(k & (k - 1)))
            rt_log("nvn: thread %u enters %s on queue %#llx while %d other submit or present call%s run (overlap %d, "
                   "frame %llu)", tid, present ? "a present" : "a submit", (unsigned long long)q, n - 1, n > 2 ? "s" : "",
                   k, (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
    }
    plat_mutex_lock(&seen_lock);
    int i = 0;
    while (i < nseen && !(seen[i].tid == tid && seen[i].q == q && seen[i].present == present)) i++;
    int fresh = i == nseen && nseen < 32;
    if (fresh) seen[nseen++] = (typeof(seen[0])){tid, q, present};
    plat_mutex_unlock(&seen_lock);
    if (fresh)
        rt_log("nvn: thread %u %s on queue %#llx (first at frame %llu)", tid, present ? "presents" : "submits",
               (unsigned long long)q, (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
}

NVN_API(nvnQueueSubmitCommands) {
    queue_t *q = arg_p(c, 0);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    if (n < 0 || (n && !h)) rt_fatal("nvn: QueueSubmitCommands with %d handles at %p", n, (const void *)h);
    NVN_BLOCKING();
    nvn_call_note(0, (uintptr_t)q, 1);
    uint64_t no = __atomic_add_fetch(&lod_entered, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&q->submits, 1, __ATOMIC_RELAXED);
    uint64_t f;
    if (gfx_replay_async()) {
        static PlatMutex submit_lock = PLAT_MUTEX_INIT;
        uint64_t local[32], *hs = n <= 32 ? local : malloc(sizeof *hs * (size_t)n);
        if (!hs) rt_fatal("nvn: out of memory (submit of %d handles)", n);
        plat_mutex_lock(&submit_lock);
        nvn_submit_prepare_async(h, hs, n);
        gfx_submit_queue = (uintptr_t)q;
        f = gfx_backend()->submit(hs, n);
        gfx_submit_queue = 0;
        plat_mutex_unlock(&submit_lock);
        if (hs != local) free(hs);
    } else {
        uint64_t local[32], *hs = n <= 32 ? local : malloc(sizeof *hs * (size_t)n);
        if (!hs) rt_fatal("nvn: out of memory (submit of %d handles)", n);
        nvn_submit_prepare(h, hs, n);
        gfx_submit_queue = (uintptr_t)q;
        f = gfx_backend()->submit(hs, n);
        gfx_submit_queue = 0;
        nvn_submit_done(h, hs, n);
        if (hs != local) free(hs);
    }
    uint64_t prev = __atomic_load_n(&last_fence, __ATOMIC_RELAXED);
    while (f > prev && !__atomic_compare_exchange_n(&last_fence, &prev, f, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
    __atomic_store_n(&lod_fence[no % LOD_RING], f, __ATOMIC_RELAXED);
    __atomic_store_n(&lod_issued, no, __ATOMIC_SEQ_CST);
    static long long finish_mask = -2;
    if (finish_mask == -2) {
        const char *e = cfg_get("NX2SARIA_SUBMIT_FINISH");
        finish_mask = e && *e ? strtoll(e, NULL, 0) : 1;
        if (e && *e && finish_mask == 1 && !(e[0] == '0' && (e[1] == 'x' || e[1] == 'X'))) finish_mask = -1;
    }
    int idx = __atomic_fetch_add(&nvn_submits_in_frame, 1, __ATOMIC_RELAXED);
    if (finish_mask && f && (finish_mask == -1 || (idx < 63 && (finish_mask >> idx & 1)))) {
        gfx_tl_note('W', 0, f, 0);
        gfx_backend()->exec_wait(f, UINT64_MAX);
        gfx_tl_note('w', 0, 0, 0);
    }
    nvn_call_note(0, (uintptr_t)q, 0);
}

static void obs_release_read(Cpu *c) {
    uint64_t lr = c->x[30], obj = c->x[19];
    rt_original(c);
    if (rt_game_version != 121 || lr != 0x70008c70c8ull) return;
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_LOD_GUARD");
    }
    if (!on || *(volatile uint8_t *)(uintptr_t)(obj + 0x528) != *(volatile uint8_t *)(uintptr_t)(obj + 0x529)) return;
    uint64_t e = __atomic_load_n(&lod_entered, __ATOMIC_SEQ_CST);
    if (!e) return;
    NVN_BLOCKING();
    for (int i = 0; i < 200 && __atomic_load_n(&lod_issued, __ATOMIC_SEQ_CST) < e; i++) plat_sleep_ms(1);
    uint64_t f = __atomic_load_n(&lod_fence[e % LOD_RING], __ATOMIC_RELAXED);
    if (gfx_backend()->fence_done() < f) gfx_backend()->fence_wait(f, 200000000ull);
}
HOST_OBSERVER("_ZN2nn2os15ReleaseReadLockEPNS0_20ReaderWriterLockTypeE", obs_release_read);
