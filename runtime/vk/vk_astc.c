#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astc_decode_spv.h"
#include "bc3_encode_spv.h"
#include "cfg.h"
#include "host.h"
#include "tex.h"
#include "vk.h"
#include "platform/platform.h"

static const uint8_t wunq_data[] = {
    0,64,
    0,32,64,
    0,21,43,64,
    0,16,32,48,64,
    0,64,12,52,25,39,
    0,9,18,27,37,46,55,64,
    0,64,7,57,14,50,21,43,28,36,
    0,64,17,47,5,59,23,41,11,53,28,36,
    0,4,8,12,17,21,25,29,35,39,43,47,52,56,60,64,
    0,64,16,48,3,61,19,45,6,58,23,41,9,55,26,38,13,51,29,35,
    0,64,8,56,16,48,24,40,2,62,11,53,19,45,27,37,5,59,13,51,22,42,30,34,
    0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,
};
static const uint16_t wunq_off[12] = {0,2,5,9,14,20,28,38,50,66,86,110};
static const uint8_t cunq_data[] = {
    0,255,51,204,102,153,
    0,36,73,109,146,182,219,255,
    0,255,28,227,56,199,84,171,113,142,
    0,255,69,186,23,232,92,163,46,209,116,139,
    0,17,34,51,68,85,102,119,136,153,170,187,204,221,238,255,
    0,255,67,188,13,242,80,175,27,228,94,161,40,215,107,148,54,201,121,134,
    0,255,33,222,66,189,99,156,11,244,44,211,77,178,110,145,22,233,55,200,88,167,121,134,
    0,8,16,24,33,41,49,57,66,74,82,90,99,107,115,123,132,140,148,156,165,173,181,189,198,206,214,222,
    231,239,247,255,
    0,255,32,223,65,190,97,158,6,249,39,216,71,184,104,151,13,242,45,210,78,177,110,145,19,236,52,203,
    84,171,117,138,26,229,58,197,91,164,123,132,
    0,255,16,239,32,223,48,207,65,190,81,174,97,158,113,142,5,250,21,234,38,217,54,201,70,185,86,169,
    103,152,119,136,11,244,27,228,43,212,59,196,76,179,92,163,108,147,124,131,
    0,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,65,69,73,77,81,85,89,93,97,101,105,109,113,117,121,125,
    130,134,138,142,146,150,154,158,162,166,170,174,178,182,186,190,195,199,203,207,211,215,219,223,227,
    231,235,239,243,247,251,255,
    0,255,16,239,32,223,48,207,64,191,80,175,96,159,112,143,3,252,19,236,35,220,51,204,67,188,83,172,
    100,155,116,139,6,249,22,233,38,217,54,201,71,184,87,168,103,152,119,136,9,246,25,230,42,213,58,197,
    74,181,90,165,106,149,122,133,13,242,29,226,45,210,61,194,77,178,93,162,109,146,125,130,
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,2,253,10,245,18,237,26,229,35,220,43,212,51,204,59,196,67,188,75,180,83,172,91,164,
    99,156,107,148,115,140,123,132,5,250,13,242,21,234,29,226,37,218,45,210,53,202,61,194,70,185,78,177,
    86,169,94,161,102,153,110,145,118,137,126,129,
    0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,
    70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,110,112,114,116,118,120,122,124,
    126,129,131,133,135,137,139,141,143,145,147,149,151,153,155,157,159,161,163,165,167,169,171,173,175,
    177,179,181,183,185,187,189,191,193,195,197,199,201,203,205,207,209,211,213,215,217,219,221,223,225,
    227,229,231,233,235,237,239,241,243,245,247,249,251,253,255,
    0,255,8,247,16,239,24,231,32,223,40,215,48,207,56,199,64,191,72,183,80,175,88,167,96,159,104,151,
    112,143,120,135,1,254,9,246,17,238,25,230,33,222,41,214,49,206,57,198,65,190,73,182,81,174,89,166,
    97,158,105,150,113,142,121,134,3,252,11,244,19,236,27,228,35,220,43,212,51,204,59,196,67,188,75,180,
    83,172,91,164,99,156,107,148,115,140,123,132,4,251,12,243,20,235,28,227,36,219,44,211,52,203,60,195,
    68,187,76,179,84,171,92,163,100,155,108,147,116,139,124,131,6,249,14,241,22,233,30,225,38,217,46,
    209,54,201,62,193,70,185,78,177,86,169,94,161,102,153,110,145,118,137,126,129,
    0,255,4,251,8,247,12,243,16,239,20,235,24,231,28,227,32,223,36,219,40,215,44,211,48,207,52,203,56,
    199,60,195,64,191,68,187,72,183,76,179,80,175,84,171,88,167,92,163,96,159,100,155,104,151,108,147,
    112,143,116,139,120,135,124,131,1,254,5,250,9,246,13,242,17,238,21,234,25,230,29,226,33,222,37,218,
    41,214,45,210,49,206,53,202,57,198,61,194,65,190,69,186,73,182,77,178,81,174,85,170,89,166,93,162,
    97,158,101,154,105,150,109,146,113,142,117,138,121,134,125,130,2,253,6,249,10,245,14,241,18,237,22,
    233,26,229,30,225,34,221,38,217,42,213,46,209,50,205,54,201,58,197,62,193,66,189,70,185,74,181,78,
    177,82,173,86,169,90,165,94,161,98,157,102,153,106,149,110,145,114,141,118,137,122,133,126,129,
};
static const uint16_t cunq_off[16] = {0,6,14,24,36,52,72,96,128,168,216,280,360,456,584,744};
_Static_assert(sizeof wunq_data == 142 && sizeof cunq_data == 936, "unquantization table size");

enum { TAB_BYTES = 28 * 4, TAB_WUNQ = TAB_BYTES, TAB_CUNQ = TAB_BYTES + 144, TAB_SIZE = TAB_CUNQ + 936 };

typedef struct {
    uint32_t in_base, out_base, width, height, bw, bh, bx, bslice, srgb;
    uint32_t bl, th, td, nvn2;
} Push;

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static struct {
    VkDevice dev;
    int mode;
    int failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkPipeline bc_pipe;
    VkPipeline bc1_pipe;
    int bc;
    int bc1;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkBuffer tab;
    VmaAllocation tab_alloc;
    int tab_uploaded;
} G = {.mode = -1};
static uint64_t n_dispatch, n_texels, n_chunks;

#define TQ 256
static PlatMutex t_lock = PLAT_MUTEX_INIT;
static int t_on = -1;
static VkQueryPool t_pool;
static VkDevice t_dev;
static double t_ns_per_tick;
static uint64_t t_mask;
static struct { uint64_t texels; uint32_t id; uint8_t fp, busy, whole; } t_slot[TQ];
static int t_next;
static struct { uint64_t texels, chunks; double us; } t_fp[13 * 13];
#define TA 64
typedef struct { uint32_t id, w, h, regions, chunks, done; int complete; uint64_t texels; double us, total; uint8_t fp; } TCall;
static TCall t_call[TA];
static uint32_t t_id;
static uint64_t t_max_texels;
static double t_max_us, t_max_total;
static uint8_t t_max_fp;

static void time_init(VkCtx *c) {
    if (t_on < 0) {
        const char *e = cfg_get("NX2SARIA_ASTC_TIME");
        t_on = e && *e && *e != '0';
    }
    if (t_on <= 0 || (t_pool && t_dev == c->device)) return;
    t_pool = VK_NULL_HANDLE;
    memset(t_slot, 0, sizeof t_slot);
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    uint32_t bits = c->gfx_family < nq ? qp[c->gfx_family].timestampValidBits : 0;
    free(qp);
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 2 * TQ};
    if (!f12.hostQueryReset || !bits || vkCreateQueryPool(c->device, &qi, NULL, &t_pool) != VK_SUCCESS) {
        rt_log("vk: NX2SARIA_ASTC_TIME: no hostQueryReset or no timestamps on the queue; ASTC decodes not timed");
        t_pool = VK_NULL_HANDLE;
        t_on = 0;
        return;
    }
    vkResetQueryPool(c->device, t_pool, 0, 2 * TQ);
    t_dev = c->device;
    t_ns_per_tick = c->props.limits.timestampPeriod;
    t_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
}

static void time_poll_locked(VkCtx *c) {
    if (!t_pool || t_dev != c->device) return;
    for (int i = 0; i < TQ; i++) {
        if (!t_slot[i].busy) continue;
        uint64_t r[4];
        if (vkGetQueryPoolResults(c->device, t_pool, 2 * (uint32_t)i, 2, sizeof r, r, 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS ||
            !r[1] || !r[3])
            continue;
        double us = (double)((r[2] - r[0]) & t_mask) * t_ns_per_tick / 1e3;
        int fp = t_slot[i].fp;
        if (!t_slot[i].whole) t_fp[fp].texels += t_slot[i].texels, t_fp[fp].chunks++, t_fp[fp].us += us;
        TCall *k = &t_call[t_slot[i].id % TA];
        if (k->id == t_slot[i].id) {
            if (t_slot[i].whole) k->total = us;
            else k->us += us, k->texels += t_slot[i].texels;
            if (++k->done == k->chunks + 1 && k->complete && k->texels > t_max_texels) {
                t_max_texels = k->texels, t_max_us = k->us, t_max_total = k->total, t_max_fp = k->fp;
                rt_log("vk: ASTC time: largest decode so far %.2f M texels (%dx%d; %u regions, the first %ux%u; %u scratch "
                       "chunks): dispatches %.0f us of GPU time (%.0f us per M texel), %.0f us with the copies",
                       (double)k->texels / 1e6, k->fp / 13, k->fp % 13, k->regions, k->w, k->h, k->chunks, k->us,
                       k->us * 1e6 / (double)k->texels, k->total);
            }
        }
        t_slot[i].busy = 0;
        vkResetQueryPool(c->device, t_pool, 2 * (uint32_t)i, 2);
    }
}

static int time_slot(uint32_t id, uint8_t fp, uint64_t texels, int whole) {
    for (int i = 0; i < TQ; i++, t_next = (t_next + 1) % TQ)
        if (!t_slot[t_next].busy) {
            int k = t_next;
            t_slot[k].busy = 1, t_slot[k].id = id, t_slot[k].fp = fp, t_slot[k].texels = texels;
            t_slot[k].whole = (uint8_t)whole;
            t_next = (k + 1) % TQ;
            return k;
        }
    return -1;
}

void vk_astc_time_enable(int on) {
    plat_mutex_lock(&t_lock);
    t_on = on;
    plat_mutex_unlock(&t_lock);
}

int vk_astc_time_get(uint32_t bw, uint32_t bh, uint64_t *texels, uint64_t *calls, double *us) {
    VkCtx *c = vk_ctx();
    if (!c || bw > 12 || bh > 12) return 0;
    plat_mutex_lock(&t_lock);
    time_poll_locked(c);
    int fp = (int)(bw * 13 + bh);
    *texels = t_fp[fp].texels, *calls = t_fp[fp].chunks, *us = t_fp[fp].us;
    plat_mutex_unlock(&t_lock);
    return 1;
}

void vk_astc_time_summary(void) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    plat_mutex_lock(&t_lock);
    if (t_on > 0) {
        time_poll_locked(c);
        for (int fp = 0; fp < 13 * 13; fp++)
            if (t_fp[fp].chunks)
                rt_log("vk: ASTC time: %dx%d: %llu dispatch batches, %.2f M texels, %.0f us GPU (%.0f us per M texel)",
                       fp / 13, fp % 13, (unsigned long long)t_fp[fp].chunks, (double)t_fp[fp].texels / 1e6, t_fp[fp].us,
                       t_fp[fp].us * 1e6 / (double)t_fp[fp].texels);
        if (t_max_texels)
            rt_log("vk: ASTC time: largest decode %.2f M texels (%dx%d): dispatches %.0f us GPU, %.0f us with the copies",
                   (double)t_max_texels / 1e6, t_max_fp / 13, t_max_fp % 13, t_max_us, t_max_total);
    }
    plat_mutex_unlock(&t_lock);
}

static void fill_tables(uint8_t *p) {
    uint32_t off[28];
    for (int i = 0; i < 12; i++) off[i] = TAB_WUNQ + wunq_off[i];
    for (int i = 0; i < 16; i++) off[12 + i] = TAB_CUNQ + cunq_off[i];
    memset(p, 0, TAB_SIZE);
    memcpy(p, off, sizeof off);
    memcpy(p + TAB_WUNQ, wunq_data, sizeof wunq_data);
    memcpy(p + TAB_CUNQ, cunq_data, sizeof cunq_data);
}

static void reset_for(VkCtx *c) {
    if (G.dev == c->device) return;
    memset(&G.dsl, 0, sizeof G - offsetof(__typeof__(G), dsl));
    G.dev = c->device;
    G.failed = 0;
    G.mode = -1;
}

static int create_pipeline(VkCtx *c) {
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 3, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof astc_decode_spv,
                                   .pCode = astc_decode_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!G.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &G.layout) != VK_SUCCESS) return 0;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return 0;
    uint64_t t0 = plat_now_ns();
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = G.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &G.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) return 0;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = TAB_SIZE,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(c->vma, &bi, &aci, &G.tab, &G.tab_alloc, NULL) != VK_SUCCESS) return 0;
    rt_log("vk: ASTC compute decoder ready (pipeline built in %.1f ms)", (double)(plat_now_ns() - t0) / 1e6);
    return 1;
}

#define BC_KEEP_RGBA8_VRAM (20ull << 30)
#define BC_KEEP_RGBA8_RAM_MB (48.0f * 1024 - 1024)

static VkResult bc_pipeline(VkCtx *c, int bc1, VkPipeline *out) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof bc3_encode_spv,
                                   .pCode = bc3_encode_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(c->device, &mi, NULL, &m);
    if (r != VK_SUCCESS) return r;
    VkBool32 on = (VkBool32)(bc1 != 0);
    VkSpecializationMapEntry me = {.constantID = 0, .offset = 0, .size = sizeof on};
    VkSpecializationInfo si = {.mapEntryCount = 1, .pMapEntries = &me, .dataSize = sizeof on, .pData = &on};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &si},
                                      .layout = G.layout};
    r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, out);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) *out = VK_NULL_HANDLE;
    return r;
}

static int create_bc_pipeline(VkCtx *c) {
    const char *e = cfg_get("NX2SARIA_ASTC_BC");
    VkDeviceSize vram = 0;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && c->mem_props.memoryHeaps[h].size > vram)
            vram = c->mem_props.memoryHeaps[h].size;
    float ram_used, ram_mb, proc;
    plat_mem_info(&ram_used, &ram_mb, &proc);
    int on = e && *e ? *e != '0' : vram < BC_KEEP_RGBA8_VRAM || (ram_mb > 0 && ram_mb < BC_KEEP_RGBA8_RAM_MB);
    if (!(e && *e))
        rt_log("vk: NX2SARIA_ASTC_BC unset: BC3 %s (largest device-local heap %llu MiB, RAM %.0f MiB)", on ? "on" : "off",
               (unsigned long long)(vram >> 20), ram_mb);
    if (!on) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_BC3_SRGB_BLOCK, &p);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((p.optimalTilingFeatures & need) != need) {
        rt_log("vk: NX2SARIA_ASTC_BC: the device does not sample BC3; ASTC stays RGBA8");
        return 0;
    }
    VkResult r = bc_pipeline(c, 0, &G.bc_pipe);
    if (r != VK_SUCCESS) {
        G.bc_pipe = VK_NULL_HANDLE;
        rt_log("vk: NX2SARIA_ASTC_BC: the BC3 encoder pipeline failed (%d); ASTC stays RGBA8", (int)r);
        return 0;
    }
    rt_log("vk: ASTC textures are encoded to BC3 after the decode (NX2SARIA_ASTC_BC)");
    return 1;
}

static int bc1_on(VkCtx *c) {
    int on = cfg_bool("NX2SARIA_ASTC_BC1");
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    for (int i = 0; i < 2 && on; i++) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(c->phys, i ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK, &p);
        if ((p.optimalTilingFeatures & need) != need) on = 0, rt_log("vk: NX2SARIA_ASTC_BC1: the device lacks BC1 features");
    }
    VkResult r = on ? bc_pipeline(c, 1, &G.bc1_pipe) : VK_SUCCESS;
    if (r != VK_SUCCESS) on = 0, rt_log("vk: NX2SARIA_ASTC_BC1: the BC1 encoder pipeline failed (%d)", (int)r);
    rt_log("vk: opaque ASTC textures %s (NX2SARIA_ASTC_BC1)", on ? "are BC1, the others BC3" : "are BC3 like the others");
    return on;
}

static int native_device(VkCtx *c) { return c->has_astc_ldr; }

static const char *mode_name(int m) {
    return m == VK_ASTC_CPU ? "cpu" : m == VK_ASTC_GPU ? "gpu" : m == VK_ASTC_NATIVE ? "native" : "check";
}

int vk_astc_mode(void) {
    VkCtx *c = vk_ctx();
    if (!c) return VK_ASTC_CPU;
    plat_mutex_lock_shared(&g_lock);
    int m = G.dev == c->device ? G.mode : -1;
    plat_mutex_unlock_shared(&g_lock);
    if (m >= 0) return m;
    plat_mutex_lock(&g_lock);
    reset_for(c);
    if (G.mode < 0) {
        const char *e = cfg_value("NX2SARIA_ASTC");
        int want = !strcmp(e, "auto") ? -1 : !strcmp(e, "cpu") ? VK_ASTC_CPU : !strcmp(e, "gpu") ? VK_ASTC_GPU
                 : !strcmp(e, "native") ? VK_ASTC_NATIVE : !strcmp(e, "check") ? VK_ASTC_CHECK : -2;
        if (want == -2) rt_log("vk: NX2SARIA_ASTC=%s is not cpu, gpu, native, check or auto: auto", e), want = -1;
        if ((want == -1 || want == VK_ASTC_NATIVE) && native_device(c)) m = VK_ASTC_NATIVE;
        else {
            if (want == VK_ASTC_NATIVE) rt_log("vk: NX2SARIA_ASTC=native: the device lacks textureCompressionASTC_LDR; gpu");
            m = want == VK_ASTC_CPU ? VK_ASTC_CPU : want == VK_ASTC_CHECK ? VK_ASTC_CHECK : VK_ASTC_GPU;
        }
        if ((m == VK_ASTC_GPU || m == VK_ASTC_CHECK || m == VK_ASTC_NATIVE) && !G.pipe && !G.failed &&
            !create_pipeline(c)) {
            G.failed = 1;
            rt_log("vk: ASTC compute decoder unavailable on %s: ASTC decodes on the CPU", c->props.deviceName);
        }
        if (G.failed && m != VK_ASTC_NATIVE) m = VK_ASTC_CPU;
        G.bc = m == VK_ASTC_GPU && G.pipe && create_bc_pipeline(c);
        G.bc1 = G.bc && bc1_on(c);
        G.mode = m;
        rt_log("vk: ASTC textures: %s (textureCompressionASTC_LDR %d)", mode_name(m), native_device(c));
    }
    m = G.mode;
    plat_mutex_unlock(&g_lock);
    return m;
}

int vk_astc_native(VkFormat astc_format) {
    VkCtx *c = vk_ctx();
    if (!c || vk_astc_mode() != VK_ASTC_NATIVE) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(c->phys, astc_format, &p);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    return (p.optimalTilingFeatures & need) == need;
}

int vk_astc_gpu(void) {
    int m = vk_astc_mode();
    if (m == VK_ASTC_GPU || m == VK_ASTC_CHECK) return 1;
    if (m != VK_ASTC_NATIVE) return 0;
    plat_mutex_lock_shared(&g_lock);
    int ok = G.pipe != VK_NULL_HANDLE;
    plat_mutex_unlock_shared(&g_lock);
    return ok;
}

int vk_astc_bc(void) {
    VkCtx *c = vk_ctx();
    if (!c || vk_astc_mode() != VK_ASTC_GPU) return 0;
    plat_mutex_lock_shared(&g_lock);
    int on = G.dev == c->device && G.bc;
    plat_mutex_unlock_shared(&g_lock);
    return on;
}

int vk_astc_bc1(void) {
    VkCtx *c = vk_ctx();
    if (!vk_astc_bc()) return 0;
    plat_mutex_lock_shared(&g_lock);
    int on = G.dev == c->device && G.bc1;
    plat_mutex_unlock_shared(&g_lock);
    return on;
}

static void mem_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                        VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

#define CHUNK_OUT (16ull << 20)
#define SCRATCH_MIN (32ull << 20)

static struct { VkBuffer buf; VmaAllocation alloc; VkDeviceSize size; VkCommandBuffer cb; VkDevice dev; uint64_t big_ns; } S;
static uint64_t n_scratch_new, n_scratch_reuse, n_scratch_trim;

#define SCRATCH_SHRINK_NS 5000000000ull
static int scratch_keep(void) {
    static int keep = -1;
    if (keep < 0) {
        keep = cfg_bool("NX2SARIA_ASTC_SCRATCH_KEEP");
        rt_log("vk: ASTC scratch %s (NX2SARIA_ASTC_SCRATCH_KEEP=%d)", keep ? "kept across upload batches" : "made per upload batch",
               keep);
    }
    return keep;
}

static volatile int32_t scratch_fail_n;
void vk_astc_scratch_fail(int n) { plat_xchg32(&scratch_fail_n, n); }

static VkBuffer scratch_get(VkCtx *c, VkCommandBuffer cb, VkDeviceSize need, int *reused) {
    if (plat_cas32(&scratch_fail_n, 0, 0) > 0 && plat_add32(&scratch_fail_n, -1) >= 0) return VK_NULL_HANDLE;
    int keep = scratch_keep();
    plat_mutex_lock(&g_lock);
    uint64_t now = kern_now_ns();
    if (need > SCRATCH_MIN) S.big_ns = now;
    int shrink = keep && S.size > SCRATCH_MIN && now - S.big_ns > SCRATCH_SHRINK_NS;
    *reused = S.buf && S.dev == c->device && (S.cb == cb || keep) && S.size >= need && !shrink;
    if (!*reused) {
        uint64_t t0 = now;
        VkDeviceSize size = need < SCRATCH_MIN ? SCRATCH_MIN : need;
        if (keep && size > SCRATCH_MIN) size = (size + (32ull << 20) - 1) & ~((32ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer b;
        VmaAllocation a;
        VkResult r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, NULL);
        if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY && bi.size > need) {
            bi.size = need;
            r = vmaCreateBuffer(c->vma, &bi, &aci, &b, &a, NULL);
        }
        if (r != VK_SUCCESS) {
            plat_mutex_unlock(&g_lock);
            return VK_NULL_HANDLE;
        }
        if (S.buf && S.dev == c->device) vk_defer_buffer(S.buf, S.alloc);
        S.buf = b, S.alloc = a, S.size = bi.size, S.dev = c->device;
        n_scratch_new++;
        vk_texstat_time(VK_TS_SCRATCH, t0);
    } else {
        n_scratch_reuse++;
    }
    S.cb = cb;
    VkBuffer out = S.buf;
    plat_mutex_unlock(&g_lock);
    return out;
}

void vk_astc_scratch_trim(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("ASTC_SCRATCH_TRIM", "1") == 1;
    if (!on || !scratch_keep()) return;
    if (__atomic_load_n(&S.size, __ATOMIC_RELAXED) <= SCRATCH_MIN || !plat_mutex_trylock(&g_lock)) return;
    VkCtx *c = vk_ctx();
    if (S.buf && c && S.dev == c->device && S.size > SCRATCH_MIN && kern_now_ns() - S.big_ns > SCRATCH_SHRINK_NS) {
        if (!vk_defer_buffer_try(S.buf, S.alloc)) {
            plat_mutex_unlock(&g_lock);
            return;
        }
        S.buf = VK_NULL_HANDLE, S.alloc = NULL, S.size = 0, S.cb = VK_NULL_HANDLE;
        n_scratch_trim++;
    }
    plat_mutex_unlock(&g_lock);
}

uint64_t vk_astc_scratch_bytes(void) {
    plat_mutex_lock(&g_lock);
    uint64_t n = S.buf ? S.size : 0;
    plat_mutex_unlock(&g_lock);
    return n;
}

static VkDeviceSize region_out(const VkAstcRegion *r) { return (VkDeviceSize)r->w * r->h * r->slices * 4; }
static VkDeviceSize tight_in(const VkAstcRegion *r, uint32_t bw, uint32_t bh) {
    return (VkDeviceSize)((r->w + bw - 1) / bw) * ((r->h + bh - 1) / bh) * r->slices * 16;
}
static VkDeviceSize region_in(const VkAstcRegion *r, uint32_t bw, uint32_t bh) {
    if (!r->bl) return tight_in(r, bw, bh);
    uint64_t row_bytes = (uint64_t)(r->w + bw - 1) / bw * 16, rows = (r->h + bh - 1) / bh;
    uint64_t gh = 8ull << r->th, gd = 1ull << r->td;
    return (VkDeviceSize)((row_bytes + 63) / 64 * ((rows + gh - 1) / gh) * ((r->slices + gd - 1) / gd) * (512ull << r->th << r->td));
}
static VkDeviceSize region_bc(const VkAstcRegion *r, int bc) { return tight_in(r, 4, 4) / (bc == 2 ? 2 : 1); }
VkDeviceSize vk_astc_region_bytes(const VkAstcRegion *r, uint32_t bw, uint32_t bh) { return region_in(r, bw, bh); }

typedef struct { uint32_t in_base, out_base, width, height, bx, bslice; } BcPush;
_Static_assert(sizeof(BcPush) <= sizeof(Push), "the encoder's push constants lie in the decoder's range");

int vk_astc_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_size, const VkAstcRegion *rg, int n, uint32_t bw,
                   uint32_t bh, int srgb, int bc, VkImage img, VkImageAspectFlags aspect, VkBuffer readback) {
    static volatile int32_t n_fail, n_unaligned;
    (void)src_size;
    VkCtx *c = vk_ctx();
    if (!c || !vk_astc_gpu() || bw < 4 || bh < 4 || bw > 12 || bh > 12) return 0;
    plat_mutex_lock_shared(&g_lock);
    VkPipeline pipe = G.dev == c->device ? G.pipe : VK_NULL_HANDLE;
    VkPipeline bc_pipe = G.dev != c->device ? VK_NULL_HANDLE : bc == 2 ? G.bc1_pipe : G.bc_pipe;
    plat_mutex_unlock_shared(&g_lock);
    if (!pipe || (bc && !bc_pipe)) return 0;
    if (bc) {
        readback = VK_NULL_HANDLE;
        for (int i = 0; i < n; i++)
            if ((rg[i].offset.x & 3) || (rg[i].offset.y & 3)) {
                if (plat_inc32(&n_unaligned) <= 16)
                    rt_log("vk: ASTC to BC3: region at %d,%d is not on a 4x4 block corner: texture left as it was",
                           rg[i].offset.x, rg[i].offset.y);
                return 0;
            }
    }
    VkDeviceSize out_total = 0, cap_in = 0, cap_out = 0, cap_bc = 0, ci = 0, co = 0, cb3 = 0;
    for (int i = 0; i < n; i++) {
        VkDeviceSize o = region_out(&rg[i]), b = region_in(&rg[i], bw, bh);
        if (co && co + o > CHUNK_OUT) ci = co = cb3 = 0;
        ci += b, co += o, cb3 += region_bc(&rg[i], bc), out_total += o;
        if (ci > cap_in) cap_in = ci;
        if (co > cap_out) cap_out = co;
        if (cb3 > cap_bc) cap_bc = cb3;
    }
    if (!out_total) return 1;
    VkDeviceSize al = c->props.limits.minStorageBufferOffsetAlignment;
    if (al < 16) al = 16;
    cap_in = (cap_in + al - 1) / al * al;
    cap_out = (cap_out + al - 1) / al * al;
    if (!bc) cap_bc = 0;
    int reused;
    VkBuffer sb = scratch_get(c, cb, cap_in + cap_out + cap_bc, &reused);
    if (!sb) {
        if (plat_inc32(&n_fail) <= 16)
            rt_log("vk: ASTC scratch buffer of %llu bytes failed: texture left undecoded for now",
                   (unsigned long long)(cap_in + cap_out + cap_bc));
        return -1;
    }
    plat_mutex_lock(&g_lock);
    int need_tab = !G.tab_uploaded;
    G.tab_uploaded = 1;
    VkBuffer tab = G.tab;
    plat_mutex_unlock(&g_lock);
    if (need_tab) {
        VkBuffer tb;
        uint8_t *p = vk_staging(TAB_SIZE, &tb, NULL);
        fill_tables(p);
        VkBufferCopy cp = {0, 0, TAB_SIZE};
        vkCmdCopyBuffer(cb, tb, tab, 1, &cp);
    }
    int timed = 0, whole = -1;
    uint32_t id = 0;
    uint8_t fp = (uint8_t)(bw * 13 + bh);
    plat_mutex_lock(&t_lock);
    time_init(c);
    if (t_on > 0 && t_pool) {
        time_poll_locked(c);
        timed = 1;
        id = ++t_id;
        t_call[id % TA] = (TCall){.id = id, .w = rg[0].w, .h = rg[0].h, .regions = (uint32_t)n, .fp = fp, .complete = 1};
        whole = time_slot(id, fp, out_total / 4, 1);
        if (whole < 0) t_call[id % TA].complete = 0;
    }
    VkQueryPool tpool = t_pool;
    plat_mutex_unlock(&t_lock);
    if (whole >= 0) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, tpool, 2 * (uint32_t)whole);
    VkDescriptorBufferInfo dbi[3] = {{sb, 0, cap_in}, {sb, cap_in, cap_out}, {tab, 0, TAB_SIZE}};
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &dbi[i]};
    VkBufferImageCopy *copies = calloc((size_t)n, sizeof *copies);
    VkBufferCopy *bcp = calloc((size_t)n, sizeof *bcp);
    if (!copies || !bcp) rt_fatal("vk: out of memory (ASTC regions)");
    VkDeviceSize rb_off = 0;
    int chunks = 0;
    for (int i0 = 0; i0 < n; chunks++) {
        int i1 = i0;
        VkDeviceSize in = 0, out = 0;
        for (; i1 < n && (i1 == i0 || out + region_out(&rg[i1]) <= CHUNK_OUT); i1++) {
            bcp[i1 - i0] = (VkBufferCopy){rg[i1].src, in, region_in(&rg[i1], bw, bh)};
            in += region_in(&rg[i1], bw, bh), out += region_out(&rg[i1]);
        }
        vk_crumb(cb, VK_CRUMB_ASTC, ++n_chunks);
        if (chunks || reused)
            mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                            (scratch_keep() ? VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0),
                        VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdCopyBuffer(cb, src, sb, (uint32_t)(i1 - i0), bcp);
        mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        int slot = -1;
        if (timed) {
            plat_mutex_lock(&t_lock);
            slot = time_slot(id, fp, out / 4, 0);
            if (slot < 0) t_call[id % TA].complete = 0;
            plat_mutex_unlock(&t_lock);
        }
        if (slot >= 0) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, tpool, 2 * (uint32_t)slot);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 3, w);
        VkDeviceSize ib = 0, ob = 0;
        for (int i = i0; i < i1; i++) {
            const VkAstcRegion *r = &rg[i];
            uint32_t bx = (r->w + bw - 1) / bw, by = (r->h + bh - 1) / bh;
            Push pc = {.in_base = (uint32_t)(ib / 16), .out_base = (uint32_t)(ob / 4), .width = r->w, .height = r->h,
                       .bw = bw, .bh = bh, .bx = bx, .bslice = bx * by, .srgb = (uint32_t)(srgb != 0),
                       .bl = r->bl, .th = r->th, .td = r->td, .nvn2 = rt_nvn_api_version[0] >= 200};
            vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            uint32_t bpw = bw * bh <= 64 ? 64 / (bw * bh) : 1;
            vkCmdDispatch(cb, (bx + bpw - 1) / bpw, by, r->slices);
            copies[i - i0] = (VkBufferImageCopy){.bufferOffset = cap_in + ob, .bufferRowLength = r->w,
                                                 .bufferImageHeight = r->h, .imageSubresource = {aspect, r->level, r->layer, 1},
                                                 .imageOffset = r->offset, .imageExtent = {r->w, r->h, r->slices}};
            ib += region_in(r, bw, bh), ob += region_out(r);
            n_texels += (uint64_t)r->w * r->h * r->slices;
        }
        if (slot >= 0)
            vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, tpool, 2 * (uint32_t)slot + 1);
        if (bc) {
            mem_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            VkDescriptorBufferInfo bdi[2] = {{sb, cap_in, cap_out}, {sb, cap_in + cap_out, cap_bc}};
            VkWriteDescriptorSet bw2[2];
            for (int k = 0; k < 2; k++)
                bw2[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)k,
                                                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                .pBufferInfo = &bdi[k]};
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, bc_pipe);
            rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 2, bw2);
            VkDeviceSize to = 0, bo = 0;
            for (int i = i0; i < i1; i++) {
                const VkAstcRegion *r = &rg[i];
                uint32_t bx4 = (r->w + 3) / 4, by4 = (r->h + 3) / 4;
                uint32_t bb = bc == 2 ? 8 : 16;
                BcPush pc = {.in_base = (uint32_t)(to / 4), .out_base = (uint32_t)(bo / bb), .width = r->w, .height = r->h,
                             .bx = bx4, .bslice = bx4 * by4};
                vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                vkCmdDispatch(cb, (bx4 + 7) / 8, (by4 + 7) / 8, r->slices);
                VkBufferImageCopy *cp = &copies[i - i0];
                cp->bufferOffset = cap_in + cap_out + bo, cp->bufferRowLength = bx4 * 4, cp->bufferImageHeight = by4 * 4;
                to += region_out(r), bo += region_bc(r, bc);
            }
        }
        mem_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        vkCmdCopyBufferToImage(cb, sb, img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)(i1 - i0), copies);
        if (readback) {
            VkBufferCopy rc = {cap_in, rb_off, out};
            vkCmdCopyBuffer(cb, sb, readback, 1, &rc);
            rb_off += out;
        }
        i0 = i1;
    }
    if (readback)
        mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                    VK_ACCESS_2_HOST_READ_BIT);
    if (whole >= 0) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, tpool, 2 * (uint32_t)whole + 1);
    if (timed) {
        plat_mutex_lock(&t_lock);
        t_call[id % TA].chunks = (uint32_t)chunks;
        plat_mutex_unlock(&t_lock);
    }
    if ((n_dispatch + (uint64_t)n) / 1024 != n_dispatch / 1024 || !n_dispatch) {
        rt_log("vk: ASTC: %llu regions, %.1f M texels decoded on the GPU so far; shared scratch %llu MiB, made %llu "
               "times, reused %llu, given back idle %llu", (unsigned long long)(n_dispatch + (uint64_t)n),
               (double)n_texels / 1e6, (unsigned long long)(S.size >> 20), (unsigned long long)n_scratch_new,
               (unsigned long long)n_scratch_reuse, (unsigned long long)n_scratch_trim);
        vk_astc_time_summary();
    }
    n_dispatch += (uint64_t)n;
    free(copies), free(bcp);
    return 1;
}

void vk_astc_release(void) {
    VkCtx *c = vk_ctx();
    vk_astc_time_summary();
    plat_mutex_lock(&t_lock);
    if (c && t_pool && t_dev == c->device) vkDestroyQueryPool(c->device, t_pool, NULL);
    t_pool = VK_NULL_HANDLE, t_dev = VK_NULL_HANDLE;
    plat_mutex_unlock(&t_lock);
    plat_mutex_lock(&g_lock);
    if (c && G.dev == c->device) {
        if (G.pipe) vkDestroyPipeline(c->device, G.pipe, NULL);
        if (G.bc_pipe) vkDestroyPipeline(c->device, G.bc_pipe, NULL);
        if (G.layout) vkDestroyPipelineLayout(c->device, G.layout, NULL);
        if (G.dsl) vkDestroyDescriptorSetLayout(c->device, G.dsl, NULL);
        if (G.tab) vmaDestroyBuffer(c->vma, G.tab, G.tab_alloc);
    }
    if (c && S.buf && S.dev == c->device) vmaDestroyBuffer(c->vma, S.buf, S.alloc);
    memset(&S, 0, sizeof S);
    memset(&G.dsl, 0, sizeof G - offsetof(__typeof__(G), dsl));
    G.dev = VK_NULL_HANDLE;
    G.mode = -1;
    G.failed = 0;
    plat_mutex_unlock(&g_lock);
}

void vk_astc_stats(uint64_t *dispatches, uint64_t *texels) {
    *dispatches = n_dispatch;
    *texels = n_texels;
}
