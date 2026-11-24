// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
#include "cfg.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "gfx.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

#define VI_R_INVALID 0xf601u

static OvNvState nv;
static PlatMutex lock = PLAT_MUTEX_INIT;

const OvNvState *ov_nv_state(void) { return &nv; }

static void set_allocator(Cpu *c, OvNvAllocator *a) {
    plat_mutex_lock(&lock);
    *a = (OvNvAllocator){arg_x(c, 0), arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
    plat_mutex_unlock(&lock);
}

static void ov_graphics_allocator(Cpu *c) { set_allocator(c, &nv.graphics); }
static void ov_devtools_allocator(Cpu *c) { set_allocator(c, &nv.devtools); }

static void ov_initialize_graphics(Cpu *c) {
    uint64_t mem = arg_x(c, 0), size = arg_x(c, 1);
    if (!mem != !size) rt_fatal("nv::InitializeGraphics: inconsistent graphics work memory (%#llx, %#llx)",
                                (unsigned long long)mem, (unsigned long long)size);
    if (!mem) rt_log("nv: no graphics work memory given; the driver allocates through the graphics allocator");
    plat_mutex_lock(&lock);
    nv.memory = mem;
    nv.memory_size = size;
    nv.initialized = 1;
    plat_mutex_unlock(&lock);
    rt_log("nv: graphics work memory %#llx+%#llx recorded", (unsigned long long)mem,
           (unsigned long long)size);
}

static int vi_ready;
static void *display, *layer, *window;

static void ov_vi_initialize(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    if (!display) {
        display = mem_kernel_alloc(64, 16);
        layer = mem_kernel_alloc(64, 16);
        window = mem_kernel_alloc(64, 16);
    }
    vi_ready = 1;
    plat_mutex_unlock(&lock);
}

static int vi_up(void) {
    plat_mutex_lock_shared(&lock);
    int r = vi_ready;
    plat_mutex_unlock_shared(&lock);
    return r;
}

static void ov_open_display(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up()) { ret_x(c, VI_R_INVALID); return; }
    *out = display;
    ret_x(c, 0);
}

static void ov_create_layer(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    *out = layer;
    ret_x(c, 0);
}

static void ov_native_window(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != layer) { ret_x(c, VI_R_INVALID); return; }
    *out = window;
    ret_x(c, 0);
}

static void ov_vsync_event(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    kern_bind_system_event(c, ev, 1);
    gfx_vsync_bind(ev);
    ret_x(c, 0);
}

static void ov_nvn_bootstrap(Cpu *c) {
    const char *name = arg_p(c, 0);
    if (!name) name = "(null)";
    static int32_t first = 1;
    if (rt_config()->boot && plat_xchg32(&first, 0)) rt_boot_event("nvn", c, name);
    ret_x(c, nvn_proc_address(name));
}

__attribute__((constructor)) static void ov_nvn_procs_register(void) { nvn_procs_register(); }

static void vi_ok(Cpu *c) { ret_x(c, 0); }
static void vi_hdr_meta(Cpu *c) {
    static int n;
    if (!n++) rt_log("vi: the game sets HDR content metadata (luminance %llu..%llu)", (unsigned long long)c->x[1],
                     (unsigned long long)c->x[2]);
    ret_x(c, 0);
}
static void ov_vi_hdr_status(Cpu *c) {
    uint8_t *on = arg_p(c, 0);
    uint32_t *format = arg_p(c, 1);
    uint16_t *peak = arg_p(c, 2);
    int hdr = vk_hdr_native();
    int nits = cfg_int("NX2SARIA_HDR_PEAK_NITS");
    if (on) *on = (uint8_t)hdr;
    if (format) *format = hdr ? 1 : 0;
    if (peak) *peak = hdr ? (uint16_t)(nits > 0 && nits < 65535 ? nits : 1000) : 0;
    static int logged;
    if (!logged++) rt_log("vi: QueryDisplayHdrStatus -> %s", hdr ? "HDR10 (the game's native HDR)" : "SDR");
    ret_x(c, 0);
}
static void ov_vi_paper_white(Cpu *c) {
    uint16_t *nits = arg_p(c, 0);
    int w = cfg_int("NX2SARIA_HDR_GAME_NITS");
    if (nits) *nits = (uint16_t)(vk_hdr_native() && w > 0 && w < 10000 ? w : 203);
    ret_x(c, 0);
}
static void ov_vi_refresh_rate(Cpu *c) {
    uint32_t *rate = arg_p(c, 0);
    uint8_t *variable = arg_p(c, 1);
    const char *hz = cfg_get("NX2SARIA_VI_HZ"), *vrr = cfg_get("NX2SARIA_VI_VRR");
    if (rate) *rate = hz && *hz ? (uint32_t)atoi(hz) : 60;
    if (variable) *variable = vrr && !strcmp(vrr, "1");
    static int logged;
    if (!logged++) rt_log("vi: QueryDisplayCurrentRefreshRate -> %u Hz, variable %d", rate ? *rate : 0, variable ? *variable : -1);
    ret_x(c, 0);
}
static void ov_vi_vrr_supported(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    const char *vrr = cfg_get("NX2SARIA_VI_VRR");
    if (out) *out = vrr && !strcmp(vrr, "1");
    static int logged;
    if (!logged++) rt_log("vi: QueryDisplayVrrSupported -> %d", out ? *out : -1);
    ret_x(c, 0);
}
HOST_OVERRIDE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", ov_vi_hdr_status);
HOST_OVERRIDE("_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", ov_vi_paper_white);
HOST_OVERRIDE("_ZN2nn2vi30QueryDisplayCurrentRefreshRateEPNS0_11RefreshRateEPbPKNS0_7DisplayE", ov_vi_refresh_rate);
HOST_OVERRIDE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", ov_vi_vrr_supported);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", vi_hdr_meta);
HOST_OVERRIDE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", vi_ok);
HOST_OVERRIDE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", vi_ok);
HOST_OVERRIDE("_ZN2nn2vi36SetDisplay120HzRefreshRateCapabilityEPNS0_7DisplayEb", vi_ok);
HOST_OVERRIDE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", vi_ok);

HOST_OVERRIDE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_graphics_allocator);
HOST_OVERRIDE("_ZN2nv18InitializeGraphicsEPvm", ov_initialize_graphics);
static void ov_nv_init_check(Cpu *c) {
    (void)c;
    if (!nv.initialized) rt_fatal("nv::InitCheck: graphics not initialised (nv::InitializeGraphics was not called)");
}
HOST_OVERRIDE("_ZN2nv9InitCheckEv", ov_nv_init_check);
HOST_OVERRIDE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_devtools_allocator);
HOST_OVERRIDE("_ZN2nn2vi10InitializeEv", ov_vi_initialize);
HOST_OVERRIDE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", ov_open_display);
HOST_OVERRIDE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", ov_create_layer);
HOST_OVERRIDE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", ov_native_window);
HOST_OVERRIDE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", ov_vsync_event);
HOST_OVERRIDE("nvnBootstrapLoader", ov_nvn_bootstrap);
