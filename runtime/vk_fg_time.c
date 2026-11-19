// The GPU time of frame generation through AMD's libraries: vk_fg_time.h says what is timed and how.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_fg_time.h"

#define FGT_SLOTS 64
enum { FREE, PREPARING, PREPARED, GENERATING, GENERATED, RESETTING };

static struct {
    int tried, on, gen;
    VkDevice dev;
    VkQueryPool pool;
    double ns_per_tick;
    uint64_t mask;
    struct { uint64_t id; int st; } slot[FGT_SLOTS];
} T;

static uint32_t family_bits(const VkQueueFamilyProperties *qp, uint32_t n, uint32_t f) { return f < n ? qp[f].timestampValidBits : 0; }

int fgt_init(uint32_t present_family, int *gen) {
    const VkCtx *c = vk_ctx();
    if (T.tried && T.dev == c->device) return *gen = T.gen, T.on;
    memset(&T, 0, sizeof T);   // first use, or after fgt_release
    T.tried = 1, T.dev = c->device;
    *gen = 0;
    const char *e = cfg_get("NX2SARIA_FG_GPU_TIME");
    if (e && *e == '0') {
        rt_log("fg: GPU time of the generated frames not measured (NX2SARIA_FG_GPU_TIME=0)");
        return 0;
    }
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    uint32_t game = family_bits(qp, nq, c->gfx_family), pres = family_bits(qp, nq, present_family);
    free(qp);
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 4 * FGT_SLOTS};
    if (!f12.hostQueryReset || !game || c->props.limits.timestampPeriod <= 0 ||
        vkCreateQueryPool(c->device, &qi, NULL, &T.pool) != VK_SUCCESS) {
        T.pool = VK_NULL_HANDLE;
        rt_log("fg: GPU time of the generated frames not measured: no hostQueryReset or no timestamps on the game's queue");
        return 0;
    }
    vkResetQueryPool(c->device, T.pool, 0, 4 * FGT_SLOTS);
    // the generation runs on a queue AMD's swapchain picks: the game's or the present queue, timed where both have timestamps
    T.gen = pres != 0;
    uint32_t bits = T.gen && pres < game ? pres : game;
    T.mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    T.ns_per_tick = c->props.limits.timestampPeriod;
    T.on = 1;
    rt_log("fg: GPU time of the generated frames measured with timestamps: %s",
           T.gen ? "their preparation and their generation" : "their preparation (the present queue has no timestamps)");
    return *gen = T.gen, 1;
}

static int take(int i, int from, int to) { return __atomic_compare_exchange_n(&T.slot[i].st, &from, to, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }

int fgt_prepare_begin(VkCommandBuffer cb, uint64_t id) {
    if (!T.on || !id) return 0;
    int i = (int)(id % FGT_SLOTS);
    if (!take(i, FREE, PREPARING)) return 0;   // still in use by a frame FGT_SLOTS ago
    __atomic_store_n(&T.slot[i].id, id, __ATOMIC_RELEASE);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, T.pool, 4 * (uint32_t)i);
    return 1;
}

void fgt_prepare_end(VkCommandBuffer cb, uint64_t id) {
    int i = (int)(id % FGT_SLOTS);
    if (!T.on || __atomic_load_n(&T.slot[i].id, __ATOMIC_ACQUIRE) != id) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, T.pool, 4 * (uint32_t)i + 1);
    take(i, PREPARING, PREPARED);
}

int fgt_generate_begin(VkCommandBuffer cb, uint64_t id) {
    if (!T.on || !T.gen || !id) return 0;
    int i = (int)(id % FGT_SLOTS);
    if (__atomic_load_n(&T.slot[i].id, __ATOMIC_ACQUIRE) != id || !take(i, PREPARED, GENERATING)) return 0;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, T.pool, 4 * (uint32_t)i + 2);
    return 1;
}

void fgt_generate_end(VkCommandBuffer cb, uint64_t id) {
    int i = (int)(id % FGT_SLOTS);
    if (!T.on || __atomic_load_n(&T.slot[i].id, __ATOMIC_ACQUIRE) != id) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, T.pool, 4 * (uint32_t)i + 3);
    take(i, GENERATING, GENERATED);
}

int fgt_poll(uint64_t now_id, double *ms, int max) {
    if (!T.on) return 0;
    int n = 0;
    for (int i = 0; i < FGT_SLOTS && n < max; i++) {
        int st = __atomic_load_n(&T.slot[i].st, __ATOMIC_ACQUIRE);
        uint64_t id = __atomic_load_n(&T.slot[i].id, __ATOMIC_ACQUIRE);
        if (st != GENERATED && st != PREPARED) continue;
        int nts = st == GENERATED ? 4 : 2;
        uint64_t r[8] = {0};
        VkResult q = vkGetQueryPoolResults(T.dev, T.pool, 4 * (uint32_t)i, (uint32_t)nts, sizeof r, r, 2 * sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        int ready = q == VK_SUCCESS && r[1] && r[3] && (nts == 2 || (r[5] && r[7]));
        if (st == PREPARED) {
            // generated only where the generation is timed; a prepared frame not generated after half the ring is let
            // go once its preparation is done (nothing writes its slot any more)
            int late = T.gen ? now_id > id + FGT_SLOTS / 2 : 1;
            if (!late || !ready || !take(i, PREPARED, RESETTING)) continue;
            if (!T.gen) ms[n++] = (double)((r[2] - r[0]) & T.mask) * T.ns_per_tick / 1e6;
        } else {
            // a generation recorded but not done yet waits: its queries may still be written, so they are never reset early
            if (!ready || !take(i, GENERATED, RESETTING)) continue;
            double prep = (double)((r[2] - r[0]) & T.mask), gen = (double)((r[6] - r[4]) & T.mask);
            ms[n++] = (prep + gen) * T.ns_per_tick / 1e6;
        }
        vkResetQueryPool(T.dev, T.pool, 4 * (uint32_t)i, 4);
        __atomic_store_n(&T.slot[i].st, FREE, __ATOMIC_RELEASE);
    }
    return n;
}

void fgt_idle(void) {
    if (!T.on) return;
    for (int i = 0; i < FGT_SLOTS; i++)
        if (take(i, PREPARED, RESETTING) || take(i, GENERATED, RESETTING)) {
            vkResetQueryPool(T.dev, T.pool, 4 * (uint32_t)i, 4);
            __atomic_store_n(&T.slot[i].st, FREE, __ATOMIC_RELEASE);
        }
}

// After AMD's contexts are destroyed (vk_fg.cpp vk_fg_release), with the device idle: nothing records into the pool.
void fgt_release(void) {
    if (T.pool && T.dev) vkDestroyQueryPool(T.dev, T.pool, NULL);
    memset(&T, 0, sizeof T);
}
