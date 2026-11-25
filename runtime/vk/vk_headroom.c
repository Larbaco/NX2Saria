#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define HR_PERIOD_MS 500
#define HR_SMALL_HEAP (1ull << 30)

static struct {
    int on, stop;
    PlatMutex lock;
    int started;
    PlatEvent ev;
    volatile uint32_t tid;
    volatile int64_t types;
    volatile int64_t made;
    volatile int64_t other;
    VmaAllocation anchor[64];
    volatile int64_t held;
    volatile uint32_t hold_until;
} H;

VkDeviceSize vk_vma_block_size(void) {
    static VkDeviceSize size;
    if (!size) {
        int mb = cfg_int("NX2SARIA_VMA_BLOCK_MB");
        mb = mb < 16 ? 16 : mb > 256 ? 256 : mb;
        size = (VkDeviceSize)mb << 20;
        rt_log("vk: VMA blocks of %d MiB on heaps over 1 GiB (NX2SARIA_VMA_BLOCK_MB)", mb);
    }
    return size;
}

static int spare_type(const VkCtx *c, uint32_t type) {
    const VkPhysicalDeviceMemoryProperties *m = &c->mem_props;
    VkMemoryPropertyFlags f = m->memoryTypes[type].propertyFlags;
    if (!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return 0;
    if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return 1;
    for (uint32_t t = 0; t < m->memoryTypeCount; t++)
        if ((m->memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) ==
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            return 0;
    return 1;
}

uint64_t vk_headroom_held(void) { return (uint64_t)__atomic_load_n(&H.held, __ATOMIC_RELAXED); }

static void VKAPI_PTR on_allocate(VmaAllocator a, uint32_t type, VkDeviceMemory m, VkDeviceSize size, void *user) {
    (void)a, (void)m, (void)size, (void)user;
    if (!H.on || type >= 64) return;
    if (plat_thread_id() == __atomic_load_n(&H.tid, __ATOMIC_ACQUIRE)) {
        plat_inc64(&H.made);
        return;
    }
    if (size >= 32ull << 20) {
        plat_inc64(&H.other);
        vk_texstat_add(VK_TS_DEVMEM, 1, size);
    }
    plat_or64(&H.types, (int64_t)(1ull << type));
    plat_event_set(&H.ev);
}

static void VKAPI_PTR on_free(VmaAllocator a, uint32_t type, VkDeviceMemory m, VkDeviceSize size, void *user) {
    (void)a, (void)type, (void)size, (void)user;
    vk_texture_block_freed((uint64_t)(uintptr_t)m);
}

const VmaDeviceMemoryCallbacks *vk_headroom_callbacks(void) {
    static const VmaDeviceMemoryCallbacks cb = {.pfnAllocate = on_allocate};
    static const VmaDeviceMemoryCallbacks cb_free = {.pfnAllocate = on_allocate, .pfnFree = on_free};
    const char *v = cfg_get("NX2SARIA_VRAM_HEADROOM");
    H.on = !v || !*v || cfg_bool("NX2SARIA_VRAM_HEADROOM");
    if (vk_texture_compact_on()) return &cb_free;
    return H.on ? &cb : NULL;
}

static VkDeviceSize block_size(const VkCtx *c, uint32_t type) {
    VkDeviceSize heap = c->mem_props.memoryHeaps[c->mem_props.memoryTypes[type].heapIndex].size;
    return heap <= HR_SMALL_HEAP ? heap / 8 : vk_vma_block_size();
}

static int room(const VkCtx *c, uint32_t type, VkDeviceSize block) {
    if (vk_texture_commit_low() &&
        (plat_commit_counts_vram() || c->props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
        return 0;
    VmaBudget b[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(c->vma, b);
    const VmaBudget *h = &b[c->mem_props.memoryTypes[type].heapIndex];
    if (!h->budget || h->usage + 2 * block > h->budget / 100 * 85) return 0;
    uint64_t all = 0, used, capped;
    int exact;
    for (uint32_t i = 0; i < c->mem_props.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++)
        if (c->mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) all += b[i].budget;
    if (vk_mem_vram(&used, &capped, &exact) && capped < all && used + 2 * block > capped / 100 * 85) return 0;
    return 1;
}

static void drop_anchor(const VkCtx *c, uint32_t type) {
    if (H.anchor[type]) {
        VmaAllocationInfo ai;
        vmaGetAllocationInfo(c->vma, H.anchor[type], &ai);
        plat_add64(&H.held, -(int64_t)ai.size);
        vmaFreeMemory(c->vma, H.anchor[type]);
    }
    H.anchor[type] = NULL;
}

static void top_up(const VkCtx *c, uint32_t type) {
    VkDeviceSize block = block_size(c, type);
    if (!room(c, type, block) || (int32_t)(__atomic_load_n(&H.hold_until, __ATOMIC_RELAXED) - (uint32_t)plat_now_ms()) > 0) {
        drop_anchor(c, type);
        return;
    }
    VkMemoryRequirements req = {.size = block / 2, .alignment = 256, .memoryTypeBits = 1u << type};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_UNKNOWN};
    VmaAllocation a;
    int64_t before = H.made;
    if (vmaAllocateMemory(c->vma, &req, &ai, &a, NULL) != VK_SUCCESS) return;
    if (H.made == before) {
        vmaFreeMemory(c->vma, a);
        return;
    }
    drop_anchor(c, type);
    H.anchor[type] = a;
    VmaAllocationInfo info;
    vmaGetAllocationInfo(c->vma, a, &info);
    plat_add64(&H.held, (int64_t)info.size);
    static volatile int32_t logged;
    if (plat_inc32(&logged) <= 32)
        rt_log("vk: headroom: a %.0f MiB block of memory type %u allocated ahead (%lld so far; %lld allocations of 32 MiB "
               "or more on other threads)", (double)block / 1048576.0, type, (long long)H.made, (long long)H.other);
}

static void headroom_main(void *arg) {
    (void)arg;
    __atomic_store_n(&H.tid, plat_thread_id(), __ATOMIC_RELEASE);
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint64_t watch = 0;
    for (;;) {
        if (watch) plat_event_wait_ns(&H.ev, (uint64_t)HR_PERIOD_MS * 1000000u);
        else plat_event_wait(&H.ev);
        watch |= (uint64_t)plat_xchg64(&H.types, 0);
        plat_mutex_lock(&H.lock);
        const VkCtx *c = H.stop ? NULL : vk_ctx();
        for (uint32_t t = 0; c && t < c->mem_props.memoryTypeCount && t < 64; t++)
            if ((watch >> t & 1) && spare_type(c, t)) top_up(c, t);
        int stop = H.stop;
        if (stop && (c = vk_ctx()))
            for (uint32_t t = 0; t < 64; t++) drop_anchor(c, t);
        plat_mutex_unlock(&H.lock);
        if (stop) return;
    }
}

#define HR_RELEASE_HOLD_MS 30000
uint64_t vk_headroom_release(void) {
    if (!H.started) return 0;
    __atomic_store_n(&H.hold_until, (uint32_t)plat_now_ms() + HR_RELEASE_HOLD_MS, __ATOMIC_RELAXED);
    uint64_t was = (uint64_t)__atomic_load_n(&H.held, __ATOMIC_RELAXED);
    plat_mutex_lock(&H.lock);
    const VkCtx *c = vk_ctx();
    for (uint32_t t = 0; c && t < 64; t++) drop_anchor(c, t);
    plat_mutex_unlock(&H.lock);
    return was;
}

void vk_headroom_stop(void) {
    if (!H.started) return;
    plat_mutex_lock(&H.lock);
    H.stop = 1;
    const VkCtx *c = vk_ctx();
    for (uint32_t t = 0; c && t < 64; t++) drop_anchor(c, t);
    plat_mutex_unlock(&H.lock);
    plat_event_set(&H.ev);
}

void vk_headroom_start(void) {
    if (!H.on || H.started) return;
    plat_mutex_init(&H.lock);
    plat_event_init(&H.ev, 0, 0);
    if (!plat_thread_spawn(headroom_main, NULL, "vk headroom", 0)) {
        H.on = 0;
        rt_log("vk: headroom: no thread, new VMA blocks stay on the allocating thread");
        return;
    }
    H.started = 1;
    rt_log("vk: headroom: the next VMA block of each memory type in use is allocated on a background thread "
           "(NX2SARIA_VRAM_HEADROOM=0 turns it off)");
}
