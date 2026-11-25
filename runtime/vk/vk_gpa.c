#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_gpa.h"
#include "platform/platform.h"

#define L1_WORD 16u
#define L1_N 65536u
#define L2_WORD (L1_WORD + L1_N)
#define L2_N 4096u
#define BLOCKS 1024u
#define REC_WORD (L2_WORD + BLOCKS * L2_N)
#define RECS 65536u
#define REC_WORDS 4u
#define WORDS (REC_WORD + RECS * REC_WORDS)
#define PAGE_SHIFT 12

static struct {
    int state;
    VkBuffer buf, dummy, scratch;
    VmaAllocation alloc, dummy_alloc, scratch_alloc;
    volatile uint32_t *gpu;
    uint32_t *cpu;
    uint32_t nblocks;
    uint16_t blk_pages[BLOCKS];
    uint16_t blk_l1[BLOCKS];
    uint8_t blk_wait[BLOCKS];
    uint32_t blk_gen[BLOCKS];
    uint16_t free_blks[BLOCKS];
    uint32_t nfree_blk, blk_waiting;
    uint64_t blk_returned;
    uint32_t nrec;
    uint32_t *free_recs, nfree;
    uint64_t mapped_pages, maps, unmaps, refused;
} G;
static PlatMutex gpa_lock = PLAT_MUTEX_INIT;

static void put(uint32_t word, uint32_t v) {
    G.cpu[word] = v;
    __atomic_store_n(&G.gpu[word], v, __ATOMIC_RELAXED);
}

int vk_gpa_wanted_ctx(const VkCtx *c) {
    static int knob = -1;
    if (knob < 0) {
        const char *e = cfg_get("NX2SARIA_GPA");
        knob = !(e && !strcmp(e, "0"));
    }
    return knob && c && c->has_buffer_device_address;
}

int vk_gpa_wanted(void) { return vk_gpa_wanted_ctx(vk_ctx()); }

uint64_t vk_gpa_device_address(VkBuffer b) {
    VkBufferDeviceAddressInfo ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b};
    return b ? vkGetBufferDeviceAddress(vk_ctx()->device, &ai) : 0;
}

static int create(void) {
    VkCtx *c = vk_ctx();
    if (!vk_gpa_wanted()) return 0;
    G.cpu = calloc(WORDS, 4);
    G.free_recs = malloc(RECS * sizeof *G.free_recs);
    if (!G.cpu || !G.free_recs) return 0;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)WORDS * 4,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &G.buf, &G.alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        rt_log("vk_gpa: the %.1f MiB table could not be allocated: global accesses through pointers read 0", WORDS * 4 / 1048576.0);
        return 0;
    }
    G.gpu = info.pMappedData;
    memset((void *)G.gpu, 0, (size_t)WORDS * 4);
    VkBufferCreateInfo di = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationInfo dinfo;
    if (vmaCreateBuffer(c->vma, &di, &ai, &G.dummy, &G.dummy_alloc, &dinfo) != VK_SUCCESS || !dinfo.pMappedData) {
        rt_log("vk_gpa: the zero buffer could not be allocated");
        return 0;
    }
    memset(dinfo.pMappedData, 0, 4096);
    VmaAllocationCreateInfo si = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(c->vma, &di, &si, &G.scratch, &G.scratch_alloc, NULL) != VK_SUCCESS) {
        rt_log("vk_gpa: the scratch buffer could not be allocated");
        return 0;
    }
    uint64_t z = vk_gpa_device_address(G.dummy), sc = vk_gpa_device_address(G.scratch);
    put(0, (uint32_t)z), put(1, (uint32_t)(z >> 32)), put(2, REC_WORD), put(3, (uint32_t)sc), put(4, (uint32_t)(sc >> 32));
    G.nblocks = 1, G.nrec = 1;
    rt_log("vk_gpa: device address table on (%.1f MiB, %u blocks of 16 MiB, %u records)", WORDS * 4 / 1048576.0, BLOCKS, RECS);
    return 1;
}

int vk_gpa_on(void) {
    if (__atomic_load_n(&G.state, __ATOMIC_ACQUIRE)) return G.state > 0;
    plat_mutex_lock(&gpa_lock);
    if (!G.state) __atomic_store_n(&G.state, create() ? 1 : -1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&gpa_lock);
    return G.state > 0;
}

VkDescriptorBufferInfo vk_gpa_info(void) {
    if (!vk_gpa_on()) return (VkDescriptorBufferInfo){0};
    return (VkDescriptorBufferInfo){G.buf, 0, VK_WHOLE_SIZE};
}

static uint32_t block_take(uint32_t l1) {
    uint32_t blk = 0;
    for (uint32_t b = 1; G.blk_waiting && b < G.nblocks && !blk; b++)
        if (G.blk_wait[b] && G.blk_l1[b] == l1) blk = b;
    if (blk) G.blk_wait[blk] = 0, G.blk_waiting--;
    else if (G.nfree_blk) blk = G.free_blks[--G.nfree_blk];
    else if (G.nblocks < BLOCKS) blk = G.nblocks++;
    else return 0;
    G.blk_l1[blk] = (uint16_t)l1;
    return blk;
}

static uint32_t page_word(uint64_t page, int alloc) {
    uint64_t l1 = page >> 12;
    if (l1 >= L1_N) return 0;
    uint32_t blk = G.cpu[L1_WORD + l1];
    if (!blk) {
        if (!alloc || !(blk = block_take((uint32_t)l1))) return 0;
        put(L1_WORD + (uint32_t)l1, blk);
    }
    return L2_WORD + blk * L2_N + (uint32_t)(page & (L2_N - 1));
}

static void page_put(uint32_t w, uint32_t r) {
    uint32_t blk = (w - L2_WORD) / L2_N, old = G.cpu[w];
    if (!old && r) G.blk_pages[blk]++, G.mapped_pages++;
    else if (old && !r) G.blk_pages[blk]--, G.mapped_pages--;
    put(w, r);
}

static void blk_free_cb(void *arg) {
    uint32_t blk = (uint32_t)((uintptr_t)arg & 0xffff), gen = (uint32_t)((uintptr_t)arg >> 16);
    plat_mutex_lock(&gpa_lock);
    if (G.state > 0 && G.blk_wait[blk] && G.blk_gen[blk] == gen) {
        G.blk_wait[blk] = 0, G.blk_waiting--;
        G.free_blks[G.nfree_blk++] = (uint16_t)blk;
    }
    plat_mutex_unlock(&gpa_lock);
}

static void blocks_release(uint64_t p0, uint64_t p1) {
    if (p1 <= p0) return;
    for (uint64_t l1 = p0 >> 12; l1 <= (p1 - 1) >> 12 && l1 < L1_N; l1++) {
        plat_mutex_lock(&gpa_lock);
        uint32_t blk = G.cpu[L1_WORD + l1], gen = 0;
        if (blk && !G.blk_pages[blk]) {
            put(L1_WORD + (uint32_t)l1, 0);
            G.blk_wait[blk] = 1, G.blk_waiting++, G.blk_returned++;
            gen = ++G.blk_gen[blk];
        } else {
            blk = 0;
        }
        plat_mutex_unlock(&gpa_lock);
        if (blk)
            vk_completion_add(vk_sync_issued() + 1, blk_free_cb, (void *)((uintptr_t)blk | (uintptr_t)gen << 16));
    }
}

static void out_of_blocks(uint64_t page) {
    if (!G.refused++)
        rt_log("vk_gpa: out of L2 blocks at %#llx (%u in use, %u waiting for the GPU): shaders read zeros through "
               "pointers into the pages left out", (unsigned long long)(page << PAGE_SHIFT),
               G.nblocks - 1 - G.nfree_blk - G.blk_waiting, G.blk_waiting);
}

static uint32_t rec_alloc(uint64_t delta, uint64_t twin) {
    uint32_t r;
    if (G.nfree) r = G.free_recs[--G.nfree];
    else if (G.nrec < RECS) r = G.nrec++;
    else return 0;
    put(REC_WORD + r * REC_WORDS, (uint32_t)delta);
    put(REC_WORD + r * REC_WORDS + 1, (uint32_t)(delta >> 32));
    put(REC_WORD + r * REC_WORDS + 2, (uint32_t)twin);
    put(REC_WORD + r * REC_WORDS + 3, (uint32_t)(twin >> 32));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    return r;
}

static void rec_free_cb(void *arg) {
    uint32_t r = (uint32_t)(uintptr_t)arg;
    plat_mutex_lock(&gpa_lock);
    G.free_recs[G.nfree++] = r;
    plat_mutex_unlock(&gpa_lock);
}

uint32_t vk_gpa_map(uint64_t lo, uint64_t hi, VkBuffer buf, uint64_t buf_lo) {
    if (!vk_gpa_on() || !buf || hi <= lo) return 0;
    static int empty = -1;
    if (empty < 0) { const char *e = cfg_get("NX2SARIA_GPA_EMPTY"); empty = e && !strcmp(e, "1"); }
    if (empty) return 0;
    uint64_t dev = vk_gpa_device_address(buf);
    if (!dev) return 0;
    uint64_t p0 = (lo + 4095) >> PAGE_SHIFT, p1 = hi >> PAGE_SHIFT;
    if (p1 <= p0) return 0;
    plat_mutex_lock(&gpa_lock);
    uint32_t r = rec_alloc(dev - buf_lo, dev - buf_lo);
    if (!r) {
        G.refused++;
        plat_mutex_unlock(&gpa_lock);
        if (G.refused == 1) rt_log("vk_gpa: out of records: a buffer at %#llx is not in the table", (unsigned long long)lo);
        return 0;
    }
    for (uint64_t p = p0; p < p1; p++) {
        uint32_t w = page_word(p, 1);
        if (!w) {
            out_of_blocks(p);
            p |= L2_N - 1;
            continue;
        }
        page_put(w, r);
    }
    G.maps++;
    plat_mutex_unlock(&gpa_lock);
    return r;
}

void vk_gpa_unmap(uint64_t lo, uint64_t hi, uint32_t rec, uint32_t (*cover)(uint64_t page_addr)) {
    if (!rec || G.state <= 0) return;
    uint64_t p0 = (lo + 4095) >> PAGE_SHIFT, p1 = hi >> PAGE_SHIFT;
    plat_mutex_lock(&gpa_lock);
    for (uint64_t p = p0; p < p1; p++) {
        uint32_t w = page_word(p, 0);
        if (w && G.cpu[w] == rec) page_put(w, 0);
    }
    G.unmaps++;
    plat_mutex_unlock(&gpa_lock);
    if (cover) {
        for (uint64_t p = p0; p < p1; p++) {
            plat_mutex_lock(&gpa_lock);
            uint32_t w = page_word(p, 0), now = w ? G.cpu[w] : 0;
            plat_mutex_unlock(&gpa_lock);
            if (!w || now) continue;
            uint32_t other = cover(p << PAGE_SHIFT);
            if (!other || other == rec) continue;
            plat_mutex_lock(&gpa_lock);
            if (!G.cpu[w]) page_put(w, other);
            plat_mutex_unlock(&gpa_lock);
        }
    }
    blocks_release(p0, p1);
    vk_completion_add(vk_sync_issued() + 1, rec_free_cb, (void *)(uintptr_t)rec);
}

void vk_gpa_clear(uint64_t lo, uint64_t hi) {
    if (G.state <= 0 || hi <= lo) return;
    uint64_t p0 = (lo + 4095) >> PAGE_SHIFT, p1 = hi >> PAGE_SHIFT;
    plat_mutex_lock(&gpa_lock);
    for (uint64_t p = p0; p < p1; p++) {
        uint32_t w = page_word(p, 0);
        if (w && G.cpu[w]) page_put(w, 0);
    }
    plat_mutex_unlock(&gpa_lock);
    blocks_release(p0, p1);
}

static int twins_on(void) {
    static int twins = -1;
    if (twins < 0) { const char *e = cfg_get("NX2SARIA_GPA_TWIN"); twins = !(e && !strcmp(e, "0")); }
    return twins;
}

static uint64_t rec_first(uint32_t rec) {
    return (uint64_t)G.cpu[REC_WORD + rec * REC_WORDS] | (uint64_t)G.cpu[REC_WORD + rec * REC_WORDS + 1] << 32;
}

static void pages_move(uint64_t p0, uint64_t p1, uint32_t old_rec, uint32_t r) {
    for (uint64_t p = p0; p < p1; p++) {
        uint32_t w = page_word(p, 1);
        if (!w) {
            out_of_blocks(p);
            p |= L2_N - 1;
            continue;
        }
        if (!G.cpu[w] || G.cpu[w] == old_rec) page_put(w, r);
    }
}

static uint32_t remap(uint64_t lo, uint64_t hi, uint32_t old_rec, VkBuffer buf, uint64_t buf_lo, int keep_twin) {
    if (!vk_gpa_on() || !buf) return old_rec;
    uint64_t dev = vk_gpa_device_address(buf);
    uint64_t p0 = (lo + 4095) >> PAGE_SHIFT, p1 = hi >> PAGE_SHIFT;
    plat_mutex_lock(&gpa_lock);
    uint64_t twin = old_rec && twins_on() && keep_twin ? rec_first(old_rec) : dev - buf_lo;
    uint32_t r = dev ? rec_alloc(dev - buf_lo, twin) : 0;
    if (!r) {
        static int said;
        plat_mutex_unlock(&gpa_lock);
        if (!said++)
            rt_log("vk_gpa: WARNING: no record left for a buffer that takes over %llu pages (%u records in all): they "
                   "keep naming the buffer they named", (unsigned long long)(p1 > p0 ? p1 - p0 : 0), RECS - 1);
        return old_rec;
    }
    pages_move(p0, p1, old_rec, r);
    plat_mutex_unlock(&gpa_lock);
    if (old_rec) vk_completion_add(vk_sync_issued() + 1, rec_free_cb, (void *)(uintptr_t)old_rec);
    return r;
}

int vk_gpa_flip_cmd(VkCommandBuffer cb, uint32_t rec, VkBuffer buf, uint64_t buf_lo, int keep_twin) {
    if (!rec || !buf || !vk_gpa_on()) return 0;
    uint64_t dev = vk_gpa_device_address(buf);
    if (!dev) return 0;
    uint32_t at = REC_WORD + rec * REC_WORDS, w[REC_WORDS];
    plat_mutex_lock(&gpa_lock);
    uint64_t first = dev - buf_lo, twin = keep_twin && twins_on() ? rec_first(rec) : first;
    w[0] = (uint32_t)first, w[1] = (uint32_t)(first >> 32), w[2] = (uint32_t)twin, w[3] = (uint32_t)(twin >> 32);
    memcpy(G.cpu + at, w, sizeof w);
    plat_mutex_unlock(&gpa_lock);
    vkCmdUpdateBuffer(cb, G.buf, (VkDeviceSize)at * 4, sizeof w, w);
    return 1;
}

uint32_t vk_gpa_remap(uint64_t lo, uint64_t hi, uint32_t old_rec, VkBuffer buf, uint64_t buf_lo) {
    return remap(lo, hi, old_rec, buf, buf_lo, 1);
}

uint32_t vk_gpa_remap_plain(uint64_t lo, uint64_t hi, uint32_t old_rec, VkBuffer buf, uint64_t buf_lo) {
    return remap(lo, hi, old_rec, buf, buf_lo, 0);
}

int vk_gpa_probe(uint64_t addr, uint64_t *first, uint64_t *twin) {
    if (G.state <= 0 || addr >> 40) return 0;
    vmaInvalidateAllocation(vk_ctx()->vma, G.alloc, 0, VK_WHOLE_SIZE);
    uint32_t blk = G.gpu[L1_WORD + (addr >> 24)];
    uint32_t rec = blk ? G.gpu[L2_WORD + blk * L2_N + ((addr >> PAGE_SHIFT) & (L2_N - 1))] : 0;
    if (!rec) return 0;
    const volatile uint32_t *r = G.gpu + REC_WORD + rec * REC_WORDS;
    *first = ((uint64_t)r[0] | (uint64_t)r[1] << 32) + addr;
    *twin = ((uint64_t)r[2] | (uint64_t)r[3] << 32) + addr;
    return 1;
}

void vk_gpa_report(void) {
    if (G.state <= 0) return;
    plat_mutex_lock(&gpa_lock);
    rt_log("vk_gpa: %llu pages (%.0f MiB) mapped, %u of %u blocks in use (%u ever, %u waiting for the GPU, %llu given "
           "back), %u records in use, %llu maps, %llu unmaps, %llu refused",
           (unsigned long long)G.mapped_pages, G.mapped_pages * 4096.0 / 1048576.0,
           G.nblocks - 1 - G.nfree_blk - G.blk_waiting, BLOCKS - 1, G.nblocks - 1, G.blk_waiting,
           (unsigned long long)G.blk_returned, G.nrec - 1 - G.nfree, (unsigned long long)G.maps,
           (unsigned long long)G.unmaps, (unsigned long long)G.refused);
    plat_mutex_unlock(&gpa_lock);
}

void vk_gpa_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (G.state <= 0 || !c) return;
    vmaDestroyBuffer(c->vma, G.buf, G.alloc);
    vmaDestroyBuffer(c->vma, G.dummy, G.dummy_alloc);
    vmaDestroyBuffer(c->vma, G.scratch, G.scratch_alloc);
    free(G.cpu), free(G.free_recs);
    memset(&G, 0, sizeof G);
    G.state = -1;
}

void vk_gpa_sizes(PerfSizes *s) {
    plat_mutex_lock(&gpa_lock);
    uint64_t recs = G.nrec > 1 + G.nfree ? G.nrec - 1 - G.nfree : 0;
    uint64_t blks = G.nblocks > 1 + G.nfree_blk + G.blk_waiting ? G.nblocks - 1 - G.nfree_blk - G.blk_waiting : 0;
    plat_mutex_unlock(&gpa_lock);
    perf_size(s, "device address records", recs, RECS, PERF_SIZE_NONE);
    perf_size(s, "device address blocks", blks, BLOCKS, PERF_SIZE_NONE);
}
