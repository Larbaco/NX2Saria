#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "session_summary.h"
#include "vk.h"
#include "vk_gpa.h"

VkCaps vk_caps = {.float64 = 1, .sg_size = 32};
static int f64_supported;
static VkBool32 gpl_lib, gpl_fast;
static const char *gpl_off;
static char summary[832];

const char *vk_caps_summary(void) { return summary; }

static SsGpu gpu;
static volatile int32_t gpu_known;

int vk_caps_gpu(SsGpu *out) {
    if (!__atomic_load_n(&gpu_known, __ATOMIC_ACQUIRE)) return 0;
    *out = gpu;
    return 1;
}

static uint32_t env_u32(const char *name, uint32_t def) {
    const char *e = cfg_get(name);
    return e && *e ? (uint32_t)strtoul(e, NULL, 0) : def;
}

void vk_caps_features(VkPhysicalDeviceFeatures *f) {
    f64_supported = f->shaderFloat64;
    if (env_u32("NX2SARIA_VK_NO_FLOAT64", 0)) f->shaderFloat64 = VK_FALSE;
    vk_caps.float64 = f->shaderFloat64;
}

static int amd_proprietary;

int vk_caps_vec_index_bug(void) { return amd_proprietary; }

void vk_caps_driver(VkPhysicalDevice p) {
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    vkGetPhysicalDeviceProperties2(p, &p2);
    vk_caps.amd = p2.properties.vendorID == 0x1002;
    vk_caps.driver_id = (uint32_t)drv.driverID;
    vk_caps.amd_sc = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY && strstr(drv.driverInfo, "shader compiler");
    vk_caps.amd_llpc = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY && !vk_caps.amd_sc;
    amd_proprietary = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY;
    vk_caps.mesa = drv.driverID == VK_DRIVER_ID_MESA_RADV || drv.driverID == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA ||
                   drv.driverID == VK_DRIVER_ID_MESA_NVK;
}

int vk_caps_gpl(const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *f,
                const VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *p) {
    gpl_lib = f->graphicsPipelineLibrary;
    gpl_fast = p->graphicsPipelineLibraryFastLinking;
    const char *e = cfg_get("NX2SARIA_GPL");
    int want = !e || !*e ? !vk_caps.amd_sc : strcmp(e, "0") != 0;
    vk_caps.gpl = gpl_lib && gpl_fast && want;
    gpl_off = vk_caps.gpl ? NULL
             : !gpl_lib || !gpl_fast ? "the driver has no fast linking"
             : e && *e ? "NX2SARIA_GPL=0"
                       : "default on AMD's driver for RDNA 2 and older (NX2SARIA_GPL=1 turns it on)";
    return vk_caps.gpl;
}

static int has_ext(const VkExtensionProperties *ep, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ep[i].extensionName, name)) return 1;
    return 0;
}

static char depth_ok(VkPhysicalDevice p, VkFormat f) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(p, f, &fp);
    return fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? 'y' : 'n';
}

static void driver_version(char *o, size_t n, uint32_t vendor, uint32_t v) {
    if (vendor == 0x10de) snprintf(o, n, "%u.%u.%u.%u", v >> 22, v >> 14 & 0xff, v >> 6 & 0xff, v & 0x3f);
    else if (vendor == 0x8086) snprintf(o, n, "%u.%u", v >> 14, v & 0x3fff);
    else snprintf(o, n, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

void vk_caps_init(VkCtx *c) {
    VkPhysicalDeviceLimits *l = &c->props.limits;
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, .pNext = &drv};
    VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, .pNext = &sg};
    VkPhysicalDevicePushDescriptorPropertiesKHR pd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
                                                      .pNext = &p13};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pd};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = c->has_provoking_vertex ? &pvf : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(c->phys, NULL, &ne, ep);

    uint32_t real_align = (uint32_t)l->minStorageBufferOffsetAlignment, a = env_u32("NX2SARIA_VK_SSBO_ALIGN", 0);
    if (a && !(a & (a - 1)) && a > real_align) l->minStorageBufferOffsetAlignment = a;
    vk_caps.sbuf_align = l->minStorageBufferOffsetAlignment > 4 ? (uint32_t)l->minStorageBufferOffsetAlignment : 0;

    uint32_t want = env_u32("NX2SARIA_VK_SUBGROUP", 32);
    int pin = f13.subgroupSizeControl && cfg_bool("NX2SARIA_VK_SUBGROUP_PIN") && p13.minSubgroupSize <= want &&
              want <= p13.maxSubgroupSize;
    vk_caps.sg_size = want;
    vk_caps.sg_pin_stages = pin ? p13.requiredSubgroupSizeStages : 0;
    static const VkShaderStageFlags stage[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    uint32_t told[3];
    for (int i = 0; i < 3; i++) told[i] = vk_caps.sg_pin_stages & stage[i] ? want : sg.subgroupSize;
    uint32_t gather = env_u32("NX2SARIA_GATHER_BIAS", c->props.vendorID == 0x10de ? 0 : l->subTexelPrecisionBits);
    uint32_t zero_locals = env_u32("NX2SARIA_ZERO_LOCALS", 1);
    gfx_shader_set_host(vk_caps.float64, vk_caps.sbuf_align, told, gather, zero_locals != 0, vk_gpa_wanted_ctx(c));
    if (gather) rt_log("vk caps: gather coordinates biased for %u sub-texel bits (NX2SARIA_GATHER_BIAS)", gather);

    char dv[32];
    driver_version(dv, sizeof dv, c->props.vendorID, c->props.driverVersion);
    snprintf(summary, sizeof summary, "%s, driver %s \"%s\" version %s", c->props.deviceName, drv.driverName,
             drv.driverInfo, dv);
    snprintf(gpu.name, sizeof gpu.name, "%s", c->props.deviceName);
    snprintf(gpu.driver, sizeof gpu.driver, "%s %s", drv.driverName, dv);
    gpu.vendor = c->props.vendorID, gpu.device = c->props.deviceID;
    for (uint32_t h = 0; h < c->mem_props.memoryHeapCount; h++)
        if (c->mem_props.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && c->mem_props.memoryHeaps[h].size >> 20 > gpu.vram_mb)
            gpu.vram_mb = c->mem_props.memoryHeaps[h].size >> 20;
    __atomic_store_n(&gpu_known, 1, __ATOMIC_RELEASE);
    uint32_t v = c->props.apiVersion;
    VkPhysicalDevice p = c->phys;
    rt_log("vk caps: %s, vendor %#06x device %#06x, driver %s \"%s\" (id %d, version %s, raw %#x), api %u.%u.%u",
           c->props.deviceName, c->props.vendorID, c->props.deviceID, drv.driverName, drv.driverInfo, (int)drv.driverID, dv,
           c->props.driverVersion, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    rt_log("vk caps: subgroup size %u, stages %#x, ops %#x; size control %d (%u-%u, required stages %#x)", sg.subgroupSize,
           (unsigned)sg.supportedStages, (unsigned)sg.supportedOperations, (int)f13.subgroupSizeControl, p13.minSubgroupSize,
           p13.maxSubgroupSize, (unsigned)p13.requiredSubgroupSizeStages);
    rt_log("vk caps: shaderFloat64 %d (enabled %d), storage buffer offset alignment %u (device %u), uniform %llu, "
           "maxPushDescriptors %u, provoking vertex ext %d last %d",
           f64_supported, vk_caps.float64, (unsigned)l->minStorageBufferOffsetAlignment, real_align,
           (unsigned long long)l->minUniformBufferOffsetAlignment, pd.maxPushDescriptors, c->has_provoking_vertex,
           (int)pvf.provokingVertexLast);
    rt_log("vk caps: ext index_type_uint8 khr %d ext %d, attachment_feedback_loop_layout %d dynamic_state %d, "
           "graphics_pipeline_library %d; depth attachment D16 %c D24S8 %c X8D24 %c D32 %c D32S8 %c",
           has_ext(ep, ne, "VK_KHR_index_type_uint8"), has_ext(ep, ne, "VK_EXT_index_type_uint8"),
           has_ext(ep, ne, "VK_EXT_attachment_feedback_loop_layout"), has_ext(ep, ne, "VK_EXT_attachment_feedback_loop_dynamic_state"),
           has_ext(ep, ne, "VK_EXT_graphics_pipeline_library"), depth_ok(p, VK_FORMAT_D16_UNORM),
           depth_ok(p, VK_FORMAT_D24_UNORM_S8_UINT), depth_ok(p, VK_FORMAT_X8_D24_UNORM_PACK32), depth_ok(p, VK_FORMAT_D32_SFLOAT),
           depth_ok(p, VK_FORMAT_D32_SFLOAT_S8_UINT));
    char gpl_note[128];
    if (gpl_off) snprintf(gpl_note, sizeof gpl_note, ", off by %s", gpl_off);
    else gpl_note[0] = '\0';
    rt_log("vk caps: gpl %s fast-link %s (graphicsPipelineLibrary %u%s)", vk_caps.gpl ? "yes" : "no",
           gpl_fast ? "yes" : "no", (unsigned)gpl_lib, gpl_note);
    rt_log("vk caps: fallbacks: float64 %s, storage buffers %s %u, subgroup pin %u on stages %#x, translator subgroup vs/fs/cs %u/%u/%u",
           vk_caps.float64 ? "native" : "softfloat64", vk_caps.sbuf_align ? "rounded down to" : "exact, alignment",
           (unsigned)l->minStorageBufferOffsetAlignment, want, (unsigned)vk_caps.sg_pin_stages, told[0], told[1], told[2]);
    free(ep);
}
