#ifdef _WIN32
#include <windows.h>
#endif
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "third_party/ffx_api/ffx_api.h"
#include "third_party/ffx_api/ffx_framegeneration.h"
#include "third_party/ffx_api/vk/ffx_api_vk.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
#include "vk_ffx.h"
#include "vk_fg.h"
#include "vk_fg_reset.h"
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
    VkExtent2D fg_ext = {0, 0};
    VkExtent2D fg_max = {0, 0};
    VkFormat fg_fmt = VK_FORMAT_UNDEFINED;
    ffxQueryDescSwapchainReplacementFunctionsVK fn = {};
    uint64_t frame = 0;
    int failed = 0;
    uint64_t generated = 0;
    FgStats st;
    int timed, gen_timed;
    int on = 0;
    FgrRing cuts;
    int prepared = 0;
    uint64_t vram = 0;
} F;

VkResult submit_game(uint32_t n, const VkSubmitInfo *si, VkFence fence) {
    vk_queue_lock();
    VkResult r = vkQueueSubmit(vk_ctx()->gfx_queue, n, si, fence);
    vk_queue_unlock();
    return r;
}

ffxReturnCode_t generate(ffxDispatchDescFrameGeneration *d, void *user) {
    __atomic_add_fetch(&F.generated, 1, __ATOMIC_RELAXED);
    if (fgr_cut(&F.cuts, d->frameID)) d->reset = true;
    VkCommandBuffer cb = (VkCommandBuffer)d->commandList;
    int t = cb && fgt_generate_begin(cb, d->frameID);
    ffxReturnCode_t r = vk_ffx_dispatch((ffxContext *)user, &d->header);
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
    rt_log("fg: %s failed (%u): frame generation off for this run", what, r);
    F.failed = 1;
}

}

static int use4(void) {
    return cfg_bool("NX2SARIA_FSR4_FG") && cfg_bool("NX2SARIA_DLAA") && vk_ffx_fsr4() && !vk_fg4_failed() &&
           vk_fg4_available();
}

extern "C" int vk_fg_wanted(void) {
#ifndef _WIN32
    return 0;
#endif
    if (strcmp(cfg_value("NX2SARIA_FRAMEGEN"), "fsr3") || vk_dlssg_dpa()) return 0;
    int four = use4();
    if (vk_fg4_active()) return four;
    if (F.sc) return !F.failed && !four;
    if (four) return 1;
    if (F.failed) return 0;
    if (!F.present) {
        static int told_q;
        if (!__atomic_exchange_n(&told_q, 1, __ATOMIC_RELAXED))
            rt_log("fg: AMD FSR 3.1 frame generation is set, but the device got no queue for its presents: off");
        return 0;
    }
    if (vk_ffx_framegen_available()) return 1;
    static int told;
    if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED))
        rt_log("fg: AMD FSR 3.1 frame generation is set, but FSR 3.1 is not available here: off");
    return 0;
}

extern "C" uint32_t vk_fg_queue_count(void) { return 3; }

extern "C" void vk_fg_queues(VkQueue present, uint32_t present_family, VkQueue acquire, uint32_t acquire_family) {
    F.present = present, F.acquire = acquire, F.pfam = present_family, F.afam = acquire_family;
    vk_fg4_queues(present, present_family, acquire, acquire_family);
}

extern "C" int vk_fg_has_queues(void) { return F.present != VK_NULL_HANDLE; }

extern "C" int vk_fg_active(void) { return F.sc != nullptr || vk_fg4_active(); }

extern "C" int vk_fg_generating(void) {
    if (vk_fg4_active()) return vk_fg4_generating();
    return F.sc && !F.failed && __atomic_load_n(&F.on, __ATOMIC_RELAXED);
}

extern "C" VkResult vk_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    if (vk_fg4_active()) return vk_fg4_create_swapchain(ci, sc);
    if (!F.sc && use4()) {
        VkResult r = vk_fg4_create_swapchain(ci, sc);
        if (r == VK_SUCCESS || vk_fg4_active()) return r;
    }
    if (F.sc) return F.fn.pOutCreateSwapchainFFXAPI(vk_ctx()->device, ci, nullptr, sc, F.sc);
    if (!vk_fg_wanted() || F.failed || !F.present || !vk_ffx_load()) return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkCtx *c = vk_ctx();
    ffxCreateContextDescFrameGenerationSwapChainVK d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    d.physicalDevice = c->phys;
    d.device = c->device;
    VkSwapchainKHR none = VK_NULL_HANDLE;
    d.swapchain = &none;
    d.createInfo = *ci;
    d.createInfo.oldSwapchain = VK_NULL_HANDLE;
    d.gameQueue = {c->gfx_queue, c->gfx_family, submit_game};
    d.asyncComputeQueue = {VK_NULL_HANDLE, c->gfx_family, nullptr};
    d.presentQueue = {F.present, F.pfam, nullptr};
    d.imageAcquireQueue = {F.acquire, F.afam, nullptr};
    ffxReturnCode_t r = vk_ffx_create(&F.sc, &d.header);
    if (r != FFX_API_RETURN_OK) {
        F.sc = nullptr;
        fail("the frame generation swapchain", r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    F.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (vk_ffx_query(&F.sc, &F.fn.header) != FFX_API_RETURN_OK || !F.fn.pOutQueuePresentKHR) {
        vk_ffx_destroy(&F.sc), F.sc = nullptr;
        fail("the swapchain functions", 0);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *sc = none;
    rt_log("fg: AMD FSR 3.1 frame generation swapchain, %ux%u", ci->imageExtent.width, ci->imageExtent.height);
    return VK_SUCCESS;
}

extern "C" void vk_fg_destroy_swapchain(VkSwapchainKHR sc) {
    if (vk_fg4_active()) return vk_fg4_destroy_swapchain(sc);
    if (F.sc && sc) F.fn.pOutDestroySwapchainFFXAPI(vk_ctx()->device, sc, nullptr, F.sc);
}

extern "C" VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img) {
    if (vk_fg4_active()) return vk_fg4_get_images(sc, n, img);
    return F.fn.pOutGetSwapchainImagesKHR(vk_ctx()->device, sc, n, img);
}

extern "C" VkResult vk_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx) {
    if (vk_fg4_active()) return vk_fg4_acquire(sc, timeout, sem, idx);
    return F.fn.pOutAcquireNextImageKHR(vk_ctx()->device, sc, timeout, sem, VK_NULL_HANDLE, idx);
}

extern "C" VkResult vk_fg_present(const VkPresentInfoKHR *pi) {
    if (vk_fg4_active()) return vk_fg4_present(pi);
    return F.fn.pOutQueuePresentKHR(vk_ctx()->gfx_queue, pi);
}

extern "C" uint64_t vk_fg_presents(VkSwapchainKHR sc) {
    if (vk_fg4_active()) return vk_fg4_presents(sc);
    return F.sc && F.fn.pOutGetLastPresentCountFFXAPI && sc ? F.fn.pOutGetLastPresentCountFFXAPI(sc) : 0;
}

extern "C" PFN_vkSetHdrMetadataEXT vk_fg_hdr_metadata_fn(void) {
    if (vk_fg4_active()) return vk_fg4_hdr_metadata_fn();
    return F.sc ? F.fn.pOutSetHdrMetadataEXT : nullptr;
}

static bool hdr_format(VkFormat f) {
    return f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ||
           f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

extern "C" void vk_fg_frame(VkCommandBuffer cb, VkSwapchainKHR sc, VkFormat fmt, VkExtent2D ext, VkImage hudless,
                            VkImageView hudless_view, const int pic[4]) {
    (void)hudless_view;
    if (vk_fg4_active()) return vk_fg4_frame(cb, sc, fmt, ext, hudless, pic);
    if (!F.sc || F.failed) return;
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
        vk_ffx_destroy(&F.fg);
        F.fg = nullptr;
    }
    if (!F.fg) {
        ffxCreateBackendVKDesc be = {};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.vkDevice = c->device;
        be.vkPhysicalDevice = c->phys;
        be.vkDeviceProcAddr = vk_ffx_proc_addr;
        ffxCreateContextDescFrameGenerationHudless hl = {};
        hl.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hl.header.pNext = &be.header;
        hl.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(fmt);
        ffxCreateContextDescFrameGeneration d = {};
        d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        d.header.pNext = &hl.header;
        d.flags = hdr_format(fmt) ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0;
        d.displaySize = {ext.width, ext.height};
        d.maxRenderSize = {max_render.width, max_render.height};
        d.backBufferFormat = ffxApiGetSurfaceFormatVK(fmt);
        ffxReturnCode_t r = vk_ffx_create(&F.fg, &d.header);
        if (r != FFX_API_RETURN_OK) return F.fg = nullptr, fail("the frame generation context", r);
        F.fg_ext = ext, F.fg_fmt = fmt, F.fg_max = max_render;
        FfxApiEffectMemoryUsage mem = {};
        ffxQueryDescFrameGenerationGetGPUMemoryUsage mq = {};
        mq.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
        mq.gpuMemoryUsageFrameGeneration = &mem;
        F.vram = vk_ffx_query(&F.fg, &mq.header) == FFX_API_RETURN_OK ? mem.totalUsageInBytes : 0;
        rt_log("fg: frame generation context for %ux%u%s, render size up to %ux%u", ext.width, ext.height,
               hdr_format(fmt) ? " (HDR)" : "", max_render.width, max_render.height);
        F.timed = fgt_init(F.pfam, &F.gen_timed);
        rt_log("fg: path: AMD's FSR 3.1 library (amd_fidelityfx_vk.dll), 2x (one generated frame a rendered frame), "
               "inputs %s; a line every %d gameplay frames", vk_dlaa_fg_only() ? "without an upscaler (the scene's own depth "
               "and motion vectors)" : "from the upscaler", FGS_PERIOD);
    }
    int fits = !have || (depth.width <= F.fg_max.width && depth.height <= F.fg_max.height);
    if (!fits) {
        static int told;
        if (!told++)
            rt_log("fg: a %ux%u scene is larger than the context's %ux%u: those frames are not generated", depth.width,
                   depth.height, F.fg_max.width, F.fg_max.height);
        have = 0;
    }
    int on = have && hudless && !menu;
    __atomic_store_n(&F.on, on, __ATOMIC_RELAXED);
    F.frame++;
    int cut = reset || !F.prepared;
    fgr_note(&F.cuts, F.frame, cut);
    F.prepared = on;
    uint64_t presents = vk_fg_presents(sc), t_now = plat_now_ns();
    fgs_count(&F.st, fgs_classify(menu, vk_dlaa_fg_scene(), got, fits, hudless != VK_NULL_HANDLE, cut), presents, t_now);
    double ms[16];
    for (int i = 0, n = fgt_poll(F.frame, ms, 16); i < n; i++) fgs_gpu(&F.st, ms[i]);
    char line[1024];
    if (fgs_line(&F.st, "fg", presents, t_now, line, sizeof line) == 1)
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
    ffxReturnCode_t r = vk_ffx_configure(&F.fg, &cfg.header);
    if (r != FFX_API_RETURN_OK) return fail("frame generation configuration", r);
    if (!on) return;
    ffxDispatchDescFrameGenerationPrepare p = {};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
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
    VkFfxCamera cam;
    if (!vk_dlaa_ffx_camera(&cam)) memset(&cam, 0, sizeof cam);
    p.cameraNear = cam.znear > 0 ? cam.znear : 0.1f;
    p.cameraFar = cam.zfar > 0 ? cam.zfar : 1e8f;
    p.cameraFovAngleVertical = cam.fov_y > 0 ? cam.fov_y : 1.0f;
    p.viewSpaceToMetersFactor = cam.meters > 0 ? cam.meters : 1.0f;
    p.depth = image(&depth);
    p.motionVectors = image(&mv);
    int t = fgt_prepare_begin(cb, F.frame);
    r = vk_ffx_dispatch(&F.fg, &p.header);
    if (t) fgt_prepare_end(cb, F.frame);
    if (r != FFX_API_RETURN_OK) return fail("frame generation preparation", r);
}

extern "C" void vk_fg_release(void) {
    vk_dlaa_hudless_wanted = 0;
    vk_fg4_release();
    if (F.fg) vk_ffx_destroy(&F.fg), F.fg = nullptr;
    if (F.sc) vk_ffx_destroy(&F.sc), F.sc = nullptr;
    fgt_release();
    F.st.started = 0;
    __atomic_store_n(&F.on, 0, __ATOMIC_RELAXED);
    F.prepared = 0;
}

extern "C" uint64_t vk_fg_vram(void) {
    return (__atomic_load_n(&F.fg, __ATOMIC_RELAXED) ? __atomic_load_n(&F.vram, __ATOMIC_RELAXED) : 0) + vk_fg4_vram();
}
