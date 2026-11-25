#define VK_FFX_NO_API
#ifndef _WIN32
extern "C" {
#include "vk.h"
#include "vk_ffx.h"
#include "vk_fg.h"
}
extern "C" int vk_fg4_available(void) { return 0; }
extern "C" int vk_fg4_failed(void) { return 0; }
extern "C" const char *vk_fg4_name(void) { return ""; }
extern "C" int vk_fg4_active(void) { return 0; }
extern "C" int vk_fg4_generating(void) { return 0; }
extern "C" void vk_fg4_queues(VkQueue, uint32_t, VkQueue, uint32_t) {}
extern "C" VkResult vk_fg4_create_swapchain(const VkSwapchainCreateInfoKHR *, VkSwapchainKHR *) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
extern "C" void vk_fg4_destroy_swapchain(VkSwapchainKHR) {}
extern "C" VkResult vk_fg4_get_images(VkSwapchainKHR, uint32_t *, VkImage *) { return VK_ERROR_FEATURE_NOT_PRESENT; }
extern "C" VkResult vk_fg4_acquire(VkSwapchainKHR, uint64_t, VkSemaphore, uint32_t *) { return VK_ERROR_FEATURE_NOT_PRESENT; }
extern "C" VkResult vk_fg4_present(const VkPresentInfoKHR *) { return VK_ERROR_FEATURE_NOT_PRESENT; }
extern "C" uint64_t vk_fg4_presents(VkSwapchainKHR) { return 0; }
extern "C" PFN_vkSetHdrMetadataEXT vk_fg4_hdr_metadata_fn(void) { return nullptr; }
extern "C" void vk_fg4_frame(VkCommandBuffer, VkSwapchainKHR, VkFormat, VkExtent2D, VkImage, const int *) {}
extern "C" void vk_fg4_release(void) {}
extern "C" uint64_t vk_fg4_vram(void) { return 0; }
#else
#include <windows.h>
#include <mutex>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "third_party/ffx_sdk/framegeneration/include/ffx_framegeneration.h"
#include "third_party/ffx_sdk/framegeneration/include/vk/ffx_api_framegeneration_vk.h"
#include "vk_ffx4_api.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_ffx.h"
#include "vk_fg.h"
#include "vk_fg_stats.h"
#include "vk_fg_time.h"
#include "ui/menu.h"
}

namespace {

struct {
    VkQueue present = VK_NULL_HANDLE, acquire = VK_NULL_HANDLE;
    uint32_t pfam = 0, afam = 0;
    ffxContext sc = nullptr;
    ffxContext fg = nullptr;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkExtent2D fg_ext = {0, 0};
    VkExtent2D fg_max = {0, 0};
    VkFormat fg_fmt = VK_FORMAT_UNDEFINED;
    ffxQueryDescSwapchainReplacementFunctionsVK fn = {};
    ffxCreateContextDescFrameGenerationSwapChainVK sc_desc = {};
    ffxCreateContextDescFrameGenerationSwapChainVersionVK sc_ver = {};
    ffxOverrideVersion sc_pin = {};
    ffxCreateContextDescFrameGeneration fg_desc = {};
    ffxCreateBackendVKDesc fg_be = {};
    ffxCreateContextDescFrameGenerationHudless fg_hl = {};
    ffxCreateContextDescFrameGenerationVersion fg_ver = {};
    ffxOverrideVersion fg_pin = {};
    uint64_t frame = 0;
    int failed = 0;
    uint64_t generated = 0;
    FgStats st;
    int prepared = 0;
    int timed = 0, gen_timed = 0;
    int on = 0;
    uint64_t vram = 0;
} F;

std::mutex g_lock;
int g_avail = -1;
char g_sc_list[96];
char g_fg_name[24];
uint64_t g_fg_pin, g_sc_pin;

VkResult submit_game(uint32_t n, const VkSubmitInfo *si, VkFence fence) {
    vk_queue_lock();
    VkResult r = vkQueueSubmit(vk_ctx()->gfx_queue, n, si, fence);
    vk_queue_unlock();
    return r;
}

ffxReturnCode_t generate(ffxDispatchDescFrameGeneration *d, void *user) {
    __atomic_add_fetch(&F.generated, 1, __ATOMIC_RELAXED);
    VkCommandBuffer cb = (VkCommandBuffer)d->commandList;
    int t = cb && fgt_generate_begin(cb, d->frameID);
    ffxReturnCode_t r = vk_ffx4_api_dispatch((ffxContext *)user, &d->header);
    if (t) fgt_generate_end(cb, d->frameID);
    return r;
}

FfxApiResource image(const VkFfxImage *i) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i->format;
    ci.extent = {i->width, i->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    return ffxApiGetResourceVK((void *)i->image, ffxApiGetImageResourceDescriptionVK(i->image, ci, 0),
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

void fail(const char *what, unsigned r) {
    rt_log("fg4: %s failed (%u): the FSR SDK's frame generation is off for this run, FSR 3.1's takes its place", what, r);
    F.failed = 1;
}

int list_versions(uint64_t type, const char *want, char *out, size_t size, uint64_t *pin) {
    uint64_t ids[8] = {};
    const char *names[8] = {};
    int n = vk_ffx4_api_versions(type, ids, names, 8);
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = names[i] ? names[i] : "?";
        len += (size_t)snprintf(out + len, size - len, "%s%s", i ? ", " : "", nm);
        if (len >= size) len = size - 1;
        if (want && *want && !*pin && !strncmp(nm, want, strlen(want))) *pin = ids[i];
    }
    return n;
}

const char *provider(ffxContext *ctx) {
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    vk_ffx4_api_query(ctx, &v.header);
    return v.versionName ? v.versionName : "?";
}

bool hdr_format(VkFormat f) {
    return f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ||
           f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

void wait_presents(void) {
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w = {};
    w.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    vk_ffx4_api_dispatch(&F.sc, &w.header);
}

}

extern "C" int vk_fg4_available(void) {
    int a = __atomic_load_n(&g_avail, __ATOMIC_ACQUIRE);
    if (a >= 0) return a;
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_avail >= 0) return g_avail;
    const VkCtx *c = vk_ctx();
    if (!c || !c->device) return 0;
    if (!vk_ffx4_api_file("amd_fidelityfx_framegeneration_vk.dll")) {
        __atomic_store_n(&g_avail, 0, __ATOMIC_RELEASE);
        return 0;
    }
    char fg[96];
    char *sc = g_sc_list;
    int nf = list_versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, cfg_get("NX2SARIA_FG4_VERSION"), fg, sizeof fg, &g_fg_pin);
    int ns = list_versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK, cfg_get("NX2SARIA_FG4_SWAPCHAIN"), sc, sizeof g_sc_list,
                           &g_sc_pin);
    a = nf > 0 && ns > 0;
    snprintf(g_fg_name, sizeof g_fg_name, "%.*s", (int)strcspn(fg, ",*"), fg);
    for (size_t k = strlen(g_fg_name); k && g_fg_name[k - 1] == ' '; k--) g_fg_name[k - 1] = 0;
    __atomic_store_n(&g_avail, a, __ATOMIC_RELEASE);
    if (a) rt_log("fg4: the FSR SDK lists for this device: frame generation %s; its swapchain %s", fg, sc);
    else rt_log("fg4: the FSR SDK lists no frame generation for this device: FSR 3.1's library generates the frames");
    return a;
}

extern "C" int vk_fg4_failed(void) { return F.failed; }

extern "C" const char *vk_fg4_name(void) { return g_fg_name; }

extern "C" void vk_fg4_queues(VkQueue present, uint32_t present_family, VkQueue acquire, uint32_t acquire_family) {
    F.present = present, F.acquire = acquire, F.pfam = present_family, F.afam = acquire_family;
}

extern "C" int vk_fg4_active(void) { return F.sc != nullptr; }

extern "C" int vk_fg4_generating(void) { return vk_fg4_active() && !F.failed && __atomic_load_n(&F.on, __ATOMIC_RELAXED); }

extern "C" VkResult vk_fg4_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    const VkCtx *c = vk_ctx();
    if (F.sc) {
        wait_presents();
        VkSwapchainCreateInfoKHR in = *ci;
        in.oldSwapchain = VK_NULL_HANDLE;
        VkSwapchainKHR h = F.handle;
        VkResult r = F.fn.pOutCreateSwapchainFFXAPI(c->device, &in, nullptr, &h, (void *)F.handle);
        if (r == VK_SUCCESS) *sc = F.handle = h;
        return r;
    }
    if (F.failed || !F.present || !vk_fg4_available()) return VK_ERROR_FEATURE_NOT_PRESENT;
    F.handle = VK_NULL_HANDLE;
    F.sc_desc = {};
    F.sc_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    F.sc_desc.physicalDevice = c->phys;
    F.sc_desc.device = c->device;
    F.sc_desc.swapchain = &F.handle;
    F.sc_desc.createInfo = *ci;
    F.sc_desc.createInfo.oldSwapchain = VK_NULL_HANDLE;
    F.sc_desc.gameQueue = {c->gfx_queue, c->gfx_family, submit_game};
    F.sc_desc.asyncComputeQueue = {VK_NULL_HANDLE, c->gfx_family, nullptr};
    F.sc_desc.presentQueue = {F.present, F.pfam, nullptr};
    F.sc_desc.imageAcquireQueue = {F.acquire, F.afam, nullptr};
    F.sc_ver = {};
    F.sc_ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    F.sc_ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
    F.sc_desc.header.pNext = &F.sc_ver.header;
    if (g_sc_pin) {
        F.sc_pin = {};
        F.sc_pin.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        F.sc_pin.versionId = g_sc_pin;
        F.sc_ver.header.pNext = &F.sc_pin.header;
    }
    ffxReturnCode_t r = vk_ffx4_api_create(&F.sc, &F.sc_desc.header);
    if (r != FFX_API_RETURN_OK || !F.sc) {
        F.sc = nullptr, F.handle = VK_NULL_HANDLE;
        fail("the frame generation swapchain", r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    F.fn = {};
    F.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (vk_ffx4_api_query(&F.sc, &F.fn.header) != FFX_API_RETURN_OK || !F.fn.pOutQueuePresentKHR || !F.fn.pOutCreateSwapchainFFXAPI ||
        !F.fn.pOutGetSwapchainImagesKHR || !F.fn.pOutAcquireNextImageKHR || !F.handle) {
        VkSwapchainKHR h = F.handle;
        PFN_vkDestroySwapchainFFXAPI destroy = F.fn.pOutDestroySwapchainFFXAPI;
        vk_ffx4_api_destroy(&F.sc), F.sc = nullptr, F.handle = VK_NULL_HANDLE;
        if (h && destroy) destroy(c->device, h, nullptr, nullptr);
        fail("the swapchain functions", 0);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *sc = F.handle;
    rt_log("fg4: AMD FSR frame generation swapchain, %ux%u (%s%s)", ci->imageExtent.width, ci->imageExtent.height,
           g_sc_pin ? "the version a test asked for, of " : "the loader's choice of ", g_sc_list);
    return VK_SUCCESS;
}

extern "C" void vk_fg4_destroy_swapchain(VkSwapchainKHR sc) { (void)sc; }

extern "C" VkResult vk_fg4_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img) {
    return F.fn.pOutGetSwapchainImagesKHR(vk_ctx()->device, sc, n, img);
}

extern "C" VkResult vk_fg4_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx) {
    return F.fn.pOutAcquireNextImageKHR(vk_ctx()->device, sc, timeout, sem, VK_NULL_HANDLE, idx);
}

extern "C" VkResult vk_fg4_present(const VkPresentInfoKHR *pi) { return F.fn.pOutQueuePresentKHR(vk_ctx()->gfx_queue, pi); }

extern "C" uint64_t vk_fg4_presents(VkSwapchainKHR sc) {
    return vk_fg4_active() && F.fn.pOutGetLastPresentCountFFXAPI && sc ? F.fn.pOutGetLastPresentCountFFXAPI(sc) : 0;
}

extern "C" PFN_vkSetHdrMetadataEXT vk_fg4_hdr_metadata_fn(void) { return vk_fg4_active() ? F.fn.pOutSetHdrMetadataEXT : nullptr; }

extern "C" void vk_fg4_frame(VkCommandBuffer cb, VkSwapchainKHR sc, VkFormat fmt, VkExtent2D ext, VkImage hudless, const int pic[4]) {
    if (!vk_fg4_active() || F.failed) return;
    vk_dlaa_hudless_wanted = 1;
    const VkCtx *c = vk_ctx();
    VkFfxImage depth, mv;
    float jx = 0, jy = 0;
    int reset = 0;
    const int menu = menu_is_open();
    const int got = !menu && vk_dlaa_fg_inputs(&depth, &mv, &jx, &jy, &reset);
    int have = got;
    unsigned pw, ph;
    wide_picture(&pw, &ph);
    VkExtent2D max_render = {ext.width > pw ? ext.width : pw, ext.height > ph ? ext.height : ph};
    if (F.fg && (F.fg_ext.width != ext.width || F.fg_ext.height != ext.height || F.fg_fmt != fmt)) {
        vk_queue_lock(), vkDeviceWaitIdle(c->device), vk_queue_unlock();
        fgt_idle();
        vk_ffx4_api_destroy(&F.fg);
        F.fg = nullptr;
    }
    if (!F.fg) {
        F.fg_be = {};
        F.fg_be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        F.fg_be.deviceContext = {c->device, c->phys, vkGetDeviceProcAddr};
        F.fg_hl = {};
        F.fg_hl.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        F.fg_hl.header.pNext = &F.fg_be.header;
        F.fg_hl.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(fmt);
        F.fg_ver = {};
        F.fg_ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        F.fg_ver.header.pNext = &F.fg_hl.header;
        F.fg_ver.version = FFX_FRAMEGENERATION_VERSION;
        if (g_fg_pin) {
            F.fg_pin = {};
            F.fg_pin.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            F.fg_pin.versionId = g_fg_pin;
            F.fg_be.header.pNext = &F.fg_pin.header;
        }
        F.fg_desc = {};
        F.fg_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        F.fg_desc.header.pNext = &F.fg_ver.header;
        F.fg_desc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE | (hdr_format(fmt) ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0);
        F.fg_desc.displaySize = {ext.width, ext.height};
        F.fg_desc.maxRenderSize = {max_render.width, max_render.height};
        F.fg_desc.backBufferFormat = ffxApiGetSurfaceFormatVK(fmt);
        uint64_t t0 = plat_now_ns();
        ffxReturnCode_t r = vk_ffx4_api_create(&F.fg, &F.fg_desc.header);
        if (r != FFX_API_RETURN_OK || !F.fg) return F.fg = nullptr, fail("the frame generation context", r);
        F.fg_ext = ext, F.fg_fmt = fmt, F.fg_max = max_render;
        FfxApiEffectMemoryUsage mem = {};
        ffxQueryDescFrameGenerationGetGPUMemoryUsage mq = {};
        mq.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
        mq.gpuMemoryUsageFrameGeneration = &mem;
        vk_ffx4_api_query(&F.fg, &mq.header);
        F.vram = mem.totalUsageInBytes;
        rt_log("fg4: AMD FSR frame generation %s for %ux%u%s, render size up to %ux%u, made in %.0f ms, %.0f MiB of video memory",
               provider(&F.fg), ext.width, ext.height, hdr_format(fmt) ? " (HDR)" : "", max_render.width, max_render.height,
               (double)(plat_now_ns() - t0) / 1e6, (double)mem.totalUsageInBytes / 1048576.0);
        F.timed = fgt_init(F.pfam, &F.gen_timed);
        rt_log("fg4: path: AMD's FSR SDK 2.4 (amd_fidelityfx_loader_vk.dll, frame generation %s), 2x, inputs %s; a line "
               "every %d gameplay frames", provider(&F.fg), vk_dlaa_fg_only() ? "without an upscaler" : "from the upscaler (FSR 4)",
               FGS_PERIOD);
    }
    int fits = !have || (depth.width <= F.fg_max.width && depth.height <= F.fg_max.height);
    if (!fits) {
        static int told;
        if (!told++)
            rt_log("fg4: a %ux%u scene is larger than the context's %ux%u: those frames are not generated", depth.width,
                   depth.height, F.fg_max.width, F.fg_max.height);
        have = 0;
    }
    int on = have && hudless && !menu;
    __atomic_store_n(&F.on, on, __ATOMIC_RELAXED);
    F.frame++;
    int cut = reset || !F.prepared;
    F.prepared = on;
    uint64_t presents = vk_fg4_presents(sc), t_now = plat_now_ns();
    fgs_count(&F.st, fgs_classify(menu, vk_dlaa_fg_scene(), got, fits, hudless != VK_NULL_HANDLE, cut), presents, t_now);
    double ms[16];
    for (int i = 0, n = fgt_poll(F.frame, ms, 16); i < n; i++) fgs_gpu(&F.st, ms[i]);
    char line[1024];
    if (fgs_line(&F.st, "fg4", presents, t_now, line, sizeof line) == 1)
        rt_log("%s; %llu frames generated by the swapchain", line, (unsigned long long)__atomic_load_n(&F.generated, __ATOMIC_RELAXED));
    ffxConfigureDescFrameGeneration cfg = {};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = (void *)sc;
    cfg.frameGenerationCallback = generate;
    cfg.frameGenerationCallbackUserContext = &F.fg;
    cfg.frameGenerationEnabled = on != 0;
    cfg.allowAsyncWorkloads = false;
    cfg.generationRect = pic && pic[2] > 0 && pic[3] > 0 ? FfxApiRect2D{pic[0], pic[1], pic[2], pic[3]}
                                                         : FfxApiRect2D{0, 0, (int32_t)ext.width, (int32_t)ext.height};
    if (on) {
        VkFfxImage h = {hudless, fmt, ext.width, ext.height, 1};
        cfg.HUDLessColor = image(&h);
    }
    cfg.frameID = F.frame;
    ffxReturnCode_t r = vk_ffx4_api_configure(&F.fg, &cfg.header);
    if (r != FFX_API_RETURN_OK) return fail("frame generation configuration", r);
    if (!on) return;
    ffxDispatchDescFrameGenerationPrepareV2 p = {};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = F.frame;
    p.commandList = cb;
    p.renderSize = {depth.width, depth.height};
    p.jitterOffset = {jx, jy};
    p.motionVectorScale = {1.0f, 1.0f};
    static uint64_t last_ns;
    uint64_t now = plat_now_ns();
    double dt = last_ns ? (now - last_ns) / 1e6 : 16.67;
    last_ns = now;
    p.frameTimeDelta = dt > 0 && dt < 100 ? (float)dt : 16.67f;
    p.reset = reset != 0;
    p.cameraNear = 0.1f;
    p.cameraFar = 1e8f;
    p.cameraFovAngleVertical = 1.0f;
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = image(&depth);
    p.motionVectors = image(&mv);
    VkDlaaCamera cam;
    if (vk_dlaa_fg_camera(&cam)) {
        memcpy(p.cameraPosition, cam.pos, sizeof p.cameraPosition), memcpy(p.cameraUp, cam.up, sizeof p.cameraUp);
        memcpy(p.cameraRight, cam.right, sizeof p.cameraRight), memcpy(p.cameraForward, cam.fwd, sizeof p.cameraForward);
        if (cam.fov_y > 0) p.cameraFovAngleVertical = cam.fov_y;
    } else {
        p.cameraUp[1] = 1.0f, p.cameraRight[0] = 1.0f, p.cameraForward[2] = -1.0f;
    }
    int t = fgt_prepare_begin(cb, F.frame);
    r = vk_ffx4_api_dispatch(&F.fg, &p.header);
    if (t) fgt_prepare_end(cb, F.frame);
    if (r != FFX_API_RETURN_OK) return fail("frame generation preparation", r);
}

extern "C" void vk_fg4_release(void) {
    __atomic_store_n(&F.on, 0, __ATOMIC_RELAXED);
    F.prepared = 0;
    F.st.started = 0;
    if (!F.sc && !F.fg) return;
    if (F.sc) wait_presents();
    if (F.fg) vk_ffx4_api_destroy(&F.fg), F.fg = nullptr;
    if (F.sc) {
        VkSwapchainKHR h = F.handle;
        vk_ffx4_api_destroy(&F.sc), F.sc = nullptr, F.handle = VK_NULL_HANDLE;
        if (h && F.fn.pOutDestroySwapchainFFXAPI) F.fn.pOutDestroySwapchainFFXAPI(vk_ctx()->device, h, nullptr, nullptr);
    }
    F.fn = {};
}

extern "C" uint64_t vk_fg4_vram(void) {
    return __atomic_load_n(&F.fg, __ATOMIC_RELAXED) ? __atomic_load_n(&F.vram, __ATOMIC_RELAXED) : 0;
}
#endif
