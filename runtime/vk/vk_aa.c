#include "vk.h"

#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "aa_post_spv.h"
#include "smaa_blend_spv.h"
#include "smaa_edge_spv.h"
#include "smaa_encode_spv.h"
#include "smaa_weight_spv.h"

static const unsigned char area_bytes[] = {
#include "smaa_area_tex.h"
};
static const unsigned char search_bytes[] = {
#include "smaa_search_tex.h"
};
#define AREA_W 160
#define AREA_H 560
#define SEARCH_W 64
#define SEARCH_H 16

static const struct { uint64_t hash; int role; } GAME_SMAA[] = {
    {0xfcb21f2afa88378cull, VK_AA_ROLE_EDGES},
    {0xe44377cbfafc1101ull, VK_AA_ROLE_WEIGHTS},
    {0x470d661afb1cb73aull, VK_AA_ROLE_COMPOSE},
};

int vk_aa_role(uint64_t guest_hash) {
    if (!guest_hash) return VK_AA_ROLE_NONE;
    for (size_t i = 0; i < sizeof GAME_SMAA / sizeof GAME_SMAA[0]; i++)
        if (GAME_SMAA[i].hash == guest_hash) return GAME_SMAA[i].role;
    return VK_AA_ROLE_NONE;
}

static const char *const NAMES[] = {"game", "off", "fxaa", "smaa"};

static int temporal(void) { return cfg_bool("NX2SARIA_DLAA") && (vk_dlaa_available() || vk_dlaa_fsr3()); }

static struct {
    uint64_t frame_read;
    int mode;
    int said;
    int composed;
    uint64_t frames_on, composes;
    int none_told;
} A = {.frame_read = ~0ull, .composed = 1};

static void work_idle(uint64_t frame);

int vk_aa_mode(void) {
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    uint64_t seen = __atomic_load_n(&A.frame_read, __ATOMIC_ACQUIRE);
    if (f == seen || !__atomic_compare_exchange_n(&A.frame_read, &seen, f, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&A.mode, __ATOMIC_ACQUIRE);
    work_idle(f);
    const char *v = cfg_value("NX2SARIA_AA");
    int want = VK_AA_GAME;
    for (int i = 1; i < 4; i++)
        if (!strcmp(v, NAMES[i])) want = i;
    int said = !strcmp(v, "taa") && !temporal();
    if (said != A.said) {
        A.said = said;
        if (said) rt_log("aa: taa needs DLSS or FSR at work (dlaa = 1): the game's SMAA stays");
    }
    __atomic_store_n(&A.mode, want, __ATOMIC_RELEASE);
    if (want != VK_AA_GAME && !A.none_told && ++A.frames_on == 3600 && !A.composes) {
        A.none_told = 1;
        rt_log("aa: aa = %s found none of the game's SMAA passes in %llu frames: the picture stays the game's",
               NAMES[want], (unsigned long long)A.frames_on);
    }
    return want;
}

int vk_aa_skip_ok(void) { return A.composed; }

void vk_aa_composed(int ok) {
    if (ok && !A.composes++) rt_log("aa: the game's SMAA replaced by %s", NAMES[A.mode]);
    A.composed = ok;
}

enum { P_POST, P_LUMA, P_EDGE, P_WEIGHT, P_BLEND, P_N };
#define S VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
#define I VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
static const struct { const uint32_t *code; size_t size; int n; VkDescriptorType t[4]; const char *name; } PIPES[P_N] = {
    {aa_post_spv, sizeof aa_post_spv, 2, {S, I}, "aa_post"},
    {smaa_encode_spv, sizeof smaa_encode_spv, 2, {S, I}, "smaa_encode"},
    {smaa_edge_spv, sizeof smaa_edge_spv, 2, {S, I}, "smaa_edge"},
    {smaa_weight_spv, sizeof smaa_weight_spv, 4, {S, S, S, I}, "smaa_weight"},
    {smaa_blend_spv, sizeof smaa_blend_spv, 3, {S, S, I}, "smaa_blend"},
};
#undef S
#undef I

typedef struct { VkImage img; VmaAllocation mem; VkImageView view; } Img;

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl[P_N];
    VkPipelineLayout layout[P_N];
    VkPipeline pipe[P_N];
    VkSampler linear, nearest;
    int tables;
    Img area, search;
    Img luma, edges, weights;
    uint32_t w, h;
    uint64_t smaa_frame;
} P;

static void img_free(Img *m);

#define WORK_IDLE 3600
static void work_idle(uint64_t frame) {
    if (!P.luma.img || frame - P.smaa_frame < WORK_IDLE) return;
    rt_log("aa: SMAA's work images unused for %d frames: freed (%ux%u)", WORK_IDLE, P.w, P.h);
    img_free(&P.luma), img_free(&P.edges), img_free(&P.weights);
    P.w = P.h = 0;
}

static int pipes_init(void) {
    if (P.tried) return P.ok;
    P.tried = 1;
    VkCtx *c = vk_ctx();
    P.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    int ok = P.push != NULL;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    ok = ok && vkCreateSampler(c->device, &si, NULL, &P.linear) == VK_SUCCESS;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    ok = ok && vkCreateSampler(c->device, &si, NULL, &P.nearest) == VK_SUCCESS;
    for (int p = 0; ok && p < P_N; p++) {
        VkDescriptorSetLayoutBinding b[4];
        for (int i = 0; i < PIPES[p].n; i++)
            b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = PIPES[p].t[i],
                                                  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
        VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                               .bindingCount = (uint32_t)PIPES[p].n, .pBindings = b};
        VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &P.dsl[p], .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = PIPES[p].size,
                                       .pCode = PIPES[p].code};
        VkShaderModule m = VK_NULL_HANDLE;
        ok = vkCreateDescriptorSetLayout(c->device, &dci, NULL, &P.dsl[p]) == VK_SUCCESS &&
             vkCreatePipelineLayout(c->device, &li, NULL, &P.layout[p]) == VK_SUCCESS &&
             vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS;
        if (ok) {
            VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                              .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                              .layout = P.layout[p]};
            ok = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &P.pipe[p]) == VK_SUCCESS;
        }
        if (m) vkDestroyShaderModule(c->device, m, NULL);
        if (!ok) rt_log("aa: the %s pipeline could not be created", PIPES[p].name);
    }
    rt_log("aa: anti-aliasing passes %s", ok ? "ready" : "unavailable: the game's SMAA stays");
    return P.ok = ok;
}

static void img_free(Img *m) {
    if (m->view) vk_defer_view(m->view);
    if (m->img) vk_defer_image(m->img, m->mem);
    memset(m, 0, sizeof *m);
}

static int img_make(VkCommandBuffer cb, Img *m, VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &m->img, &m->mem, NULL) != VK_SUCCESS) return m->img = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &m->view) != VK_SUCCESS) {
        vmaDestroyImage(c->vma, m->img, m->mem);
        memset(m, 0, sizeof *m);
        return 0;
    }
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                                                                                       : VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = m->img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
    return 1;
}

static int tables_make(void) {
    if (P.tables) return P.tables > 0;
    P.tables = -1;
    VkCommandBuffer cb = vk_upload_begin();
    int ok = img_make(cb, &P.area, VK_FORMAT_R8G8_UNORM, AREA_W, AREA_H, VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
             img_make(cb, &P.search, VK_FORMAT_R8_UNORM, SEARCH_W, SEARCH_H, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    uint8_t *st = ok ? vk_staging_at(sizeof area_bytes + sizeof search_bytes, &buf, &off) : NULL;
    if (st) {
        memcpy(st, area_bytes, sizeof area_bytes);
        memcpy(st + sizeof area_bytes, search_bytes, sizeof search_bytes);
        VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {AREA_W, AREA_H, 1}};
        vkCmdCopyBufferToImage(cb, buf, P.area.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        r.bufferOffset = off + sizeof area_bytes, r.imageExtent = (VkExtent3D){SEARCH_W, SEARCH_H, 1};
        vkCmdCopyBufferToImage(cb, buf, P.search.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        VkImageMemoryBarrier2 b[2];
        for (int i = 0; i < 2; i++)
            b[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,
                                           .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .image = i ? P.search.img : P.area.img,
                                           .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = b};
        vkCmdPipelineBarrier2(cb, &d);
    }
    vk_upload_end();
    if (!st) {
        img_free(&P.area), img_free(&P.search);
        rt_log("aa: SMAA's lookup textures could not be made: smaa runs as off");
        return 0;
    }
    vk_upload_flush(1);
    P.tables = 1;
    return 1;
}

static int work_images(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (P.luma.img && P.w == w && P.h == h) return 1;
    img_free(&P.luma), img_free(&P.edges), img_free(&P.weights);
    P.w = P.h = 0;
    if (!img_make(cb, &P.luma, VK_FORMAT_R16_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT) ||
        !img_make(cb, &P.edges, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_STORAGE_BIT) ||
        !img_make(cb, &P.weights, VK_FORMAT_R8G8B8A8_UNORM, w, h, VK_IMAGE_USAGE_STORAGE_BIT)) {
        img_free(&P.luma), img_free(&P.edges), img_free(&P.weights);
        return 0;
    }
    P.w = w, P.h = h;
    return 1;
}

static void pass(VkCommandBuffer cb, int p, const VkImageView *views, const VkSampler *samp, const void *pc, size_t pcn,
                 uint32_t w, uint32_t h) {
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet wr[4];
    for (int i = 0; i < PIPES[p].n; i++) {
        int sampled = PIPES[p].t[i] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ii[i] = (VkDescriptorImageInfo){sampled ? samp[i] : VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                       .descriptorCount = 1, .descriptorType = PIPES[p].t[i], .pImageInfo = &ii[i]};
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.pipe[p]);
    rec_push(P.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, P.layout[p], 0, (uint32_t)PIPES[p].n, wr);
    vkCmdPushConstants(cb, P.layout[p], VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)pcn, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

int vk_aa_record(VkCommandBuffer cb, int mode, VkImageView src, VkImageView dst, uint32_t w, uint32_t h) {
    if (mode == VK_AA_GAME || !w || !h || !pipes_init()) return 0;
    if (mode == VK_AA_SMAA && (!tables_make() || !work_images(cb, w, h))) {
        static int told;
        if (!told++) rt_log("aa: SMAA's images could not be made (%ux%u): smaa runs as off", w, h);
        mode = VK_AA_OFF;
    }
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &dep);
    if (mode != VK_AA_SMAA) {
        struct { float inv[2]; int32_t size[2]; int32_t mode; } pc = {{1.0f / (float)w, 1.0f / (float)h},
                                                                      {(int32_t)w, (int32_t)h}, mode == VK_AA_FXAA};
        const VkImageView v[2] = {src, dst};
        const VkSampler s[2] = {P.linear, VK_NULL_HANDLE};
        pass(cb, P_POST, v, s, &pc, sizeof pc, w, h);
        return 1;
    }
    P.smaa_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    const float metrics[4] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h};
    const VkSampler lin[4] = {P.linear, P.linear, P.linear, P.linear};
    pass(cb, P_LUMA, (const VkImageView[]){src, P.luma.view}, lin, metrics, sizeof metrics, w, h);
    pass(cb, P_EDGE, (const VkImageView[]){P.luma.view, P.edges.view}, lin, metrics, sizeof metrics, w, h);
    pass(cb, P_WEIGHT, (const VkImageView[]){P.edges.view, P.area.view, P.search.view, P.weights.view},
         (const VkSampler[]){P.linear, P.linear, P.nearest, VK_NULL_HANDLE}, metrics, sizeof metrics, w, h);
    pass(cb, P_BLEND, (const VkImageView[]){src, P.weights.view, dst}, lin, metrics, sizeof metrics, w, h);
    return 1;
}
