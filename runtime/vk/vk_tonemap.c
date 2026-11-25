#include "platform/platform.h"
#include "vk.h"

#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fallback_tonemap_spv.h"
#include "fsr_easu_spv.h"
#include "host.h"

static struct {
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkPipeline easu;
    int easu_tried;
    int filter_logs;
    int filter_last;
    VkSampler sampler;
    VkSampler nearest;
    struct { VkImage img; VkFormat f; VkImageView view; } views[16];
    int nviews;
    VkImage out;
    VmaAllocation out_alloc;
    VkImageView out_view;
    uint32_t ow, oh;
} TM;

static int tm_out(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (TM.out && TM.ow == w && TM.oh == h) return 1;
    VkCtx *c = vk_ctx();
    if (TM.out) vk_defer_view(TM.out_view), vk_defer_image(TM.out, TM.out_alloc), TM.out = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &TM.out, &TM.out_alloc, NULL) != VK_SUCCESS) return TM.out = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = TM.out,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &TM.out_view) != VK_SUCCESS) {
        vmaDestroyImage(c->vma, TM.out, TM.out_alloc);
        return TM.out = VK_NULL_HANDLE, 0;
    }
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = TM.out, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
    TM.ow = w, TM.oh = h;
    return 1;
}

static int tm_init(void) {
    if (TM.tried) return TM.ok;
    TM.tried = 1;
    VkCtx *c = vk_ctx();
    TM.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[2] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof fallback_tonemap_spv, .pCode = fallback_tonemap_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    int ok = TM.push && vkCreateDescriptorSetLayout(c->device, &dci, NULL, &TM.dsl) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &TM.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo ni = si;
    ni.magFilter = ni.minFilter = VK_FILTER_NEAREST;
    ok = ok && vkCreatePipelineLayout(c->device, &li, NULL, &TM.layout) == VK_SUCCESS &&
         vkCreateSampler(c->device, &si, NULL, &TM.sampler) == VK_SUCCESS &&
         vkCreateSampler(c->device, &ni, NULL, &TM.nearest) == VK_SUCCESS &&
         vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS;
    if (ok) {
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                          .layout = TM.layout};
        ok = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &TM.pipe) == VK_SUCCESS;
    }
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    rt_log("vk_exec: upscaler stand-in: %s", ok ? "scale and square-decode pass" : "blit (its pass could not be created)");
    return TM.ok = ok;
}

static VkPipeline tm_easu(void) {
    if (TM.easu_tried) return TM.easu;
    TM.easu_tried = 1;
    VkCtx *c = vk_ctx();
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fsr_easu_spv,
                                   .pCode = fsr_easu_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    static const VkBool32 square = VK_TRUE;
    VkSpecializationMapEntry me = {.constantID = 0, .offset = 0, .size = sizeof square};
    VkSpecializationInfo sp = {.mapEntryCount = 1, .pMapEntries = &me, .dataSize = sizeof square, .pData = &square};
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS) {
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                    .pSpecializationInfo = &sp},
                                          .layout = TM.layout};
        if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &TM.easu) != VK_SUCCESS)
            TM.easu = VK_NULL_HANDLE;
        vkDestroyShaderModule(c->device, m, NULL);
    }
    if (!TM.easu) rt_log("vk_exec: upscaler stand-in: its edge-adaptive pass could not be created; it scales bilinearly");
    return TM.easu;
}

static void tm_filter_log(int easu, int edge, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
    int same = sw == dw && sh == dh, now = easu ? 2 : same && edge ? 3 : 1;
    if (TM.filter_last == now || TM.filter_logs >= 8) return;
    TM.filter_last = now, TM.filter_logs++;
    if (easu)
        rt_log("vk_exec: upscaler stand-in scales %ux%u to %ux%u with the edge-adaptive filter "
               "(NX2SARIA_STANDIN_FILTER=bilinear: the linear one)", sw, sh, dw, dh);
    else if (now == 3)
        rt_log("vk_exec: upscaler stand-in copies %ux%u 1:1, decoded and not scaled: the scene is the picture's size", sw,
               sh);
    else
        rt_log("vk_exec: upscaler stand-in scales %ux%u to %ux%u with the linear filter: %s", sw, sh, dw, dh,
               !edge ? "DLSS or FSR scales the scene and its picture goes over this one"
               : dw < sw || dh < sh ? "nothing to enlarge"
               : strcmp(cfg_value("NX2SARIA_STANDIN_FILTER"), "easu") ? "NX2SARIA_STANDIN_FILTER asks for it"
                                                                     : "the edge-adaptive pass is not there");
}

static PlatMutex tm_view_lock = PLAT_MUTEX_INIT;

void vk_tonemap_forget(VkImage src) {
    plat_mutex_lock(&tm_view_lock);
    for (int i = 0; i < TM.nviews;)
        if (TM.views[i].img == src) vk_defer_view(TM.views[i].view), TM.views[i] = TM.views[--TM.nviews];
        else i++;
    plat_mutex_unlock(&tm_view_lock);
}

static VkImageView tm_view(VkImage img, VkFormat f) {
    plat_mutex_lock(&tm_view_lock);
    VkImageView v = VK_NULL_HANDLE;
    for (int i = 0; i < TM.nviews && !v; i++)
        if (TM.views[i].img == img && TM.views[i].f == f) v = TM.views[i].view;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (!v && TM.nviews < 16 && vkCreateImageView(vk_ctx()->device, &vi, NULL, &v) == VK_SUCCESS)
        TM.views[TM.nviews].img = img, TM.views[TM.nviews].f = f, TM.views[TM.nviews].view = v, TM.nviews++;
    plat_mutex_unlock(&tm_view_lock);
    return v;
}

VkImage vk_tonemap_record(VkCommandBuffer cb, VkImage src, VkFormat sf, uint32_t sw, uint32_t sh, uint32_t dw,
                          uint32_t dh, int edge) {
    static int disp = -1;
    if (disp < 0) disp = rt_knob("TM_DISPATCH", "1");
    if (disp == 2 || !tm_init() || !tm_out(cb, dw, dh)) return VK_NULL_HANDLE;
    VkImageView sv = tm_view(src, sf), dv = TM.out_view;
    if (!sv) return VK_NULL_HANDLE;
    int want = edge && dw >= sw && dh >= sh && (dw > sw || dh > sh) && !strcmp(cfg_value("NX2SARIA_STANDIN_FILTER"), "easu");
    VkPipeline easu = want ? tm_easu() : VK_NULL_HANDLE;
    tm_filter_log(easu != VK_NULL_HANDLE, edge, sw, sh, dw, dh);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
    VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &dep);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, easu ? easu : TM.pipe);
    VkSampler smp = !easu && dw == sw && dh == sh ? TM.nearest : TM.sampler;
    VkDescriptorImageInfo ii[2] = {{smp, sv, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, dv, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0]},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[1]}};
    rec_push(TM.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, TM.layout, 0, 2, w);
    if (easu) {
        const float pc[8] = {(float)sw, (float)sh, (float)sw, (float)sh, (float)dw, (float)dh, 0, 0};
        vkCmdPushConstants(cb, TM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        if (disp) vkCmdDispatch(cb, (dw + 15) / 16, (dh + 15) / 16, 1);
    } else {
        struct { float inv[2], unused, pad; int32_t size[2]; } pc = {{1.0f / (float)dw, 1.0f / (float)dh}, 0, 0,
                                                                    {(int32_t)dw, (int32_t)dh}};
        vkCmdPushConstants(cb, TM.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        if (disp) vkCmdDispatch(cb, (dw + 7) / 8, (dh + 7) / 8, 1);
    }
    VkMemoryBarrier2 after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                              .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo dep2 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &after};
    vkCmdPipelineBarrier2(cb, &dep2);
    return TM.out;
}

