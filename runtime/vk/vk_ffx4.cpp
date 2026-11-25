#define VK_FFX_NO_API
#ifndef _WIN32
extern "C" {
#include "vk.h"
#include "vk_ffx.h"
}
extern "C" int vk_ffx4_available(void) { return 0; }
extern "C" const char *vk_ffx4_name(void) { return "AMD FSR 4"; }
extern "C" int vk_ffx4_upscale(VkCommandBuffer, const VkFfxImage *, const VkFfxImage *, const VkFfxImage *, const VkFfxImage *,
                               float, float, int, float, const VkFfxCamera *) {
    return 0;
}
extern "C" void vk_ffx4_release(void) {}
extern "C" int vk_ffx4_holds(void) { return 0; }
extern "C" int vk_ffx4_failed(void) { return 0; }
extern "C" void vk_ffx4_retry(void) {}
extern "C" void vk_ffx4_trim(uint64_t) {}
extern "C" uint64_t vk_ffx4_vram(int *contexts) { return *contexts = 0, 0; }
extern "C" void vk_ffx4_release_idle(void) {}
extern "C" int vk_ffx4_dead_take(uint64_t) { return 0; }
extern "C" uint64_t vk_ffx4_dead_fence(void) { return 0; }
#else
#include <windows.h>
#include <atomic>
#include <mutex>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "third_party/ffx_sdk/upscalers/include/ffx_upscale.h"
#include "vk_ffx4_api.h"
#include "vk_up_defer.h"

extern "C" {
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_ffx.h"
}

namespace {

struct Api {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
} A;

std::mutex g_lock;
std::atomic<int> g_state{-1};
std::atomic<int> g_off{0};
uint64_t g_version;
char g_name[48] = "AMD FSR 4";
int g_failures;
const int FAIL_MAX = 4;
uint64_t g_retry_ns;
const uint64_t RETRY_NS = 2ull * 1000000000ull;

void message(uint32_t type, const wchar_t *msg) {
    char s[512];
    size_t n = 0;
    for (; msg && msg[n] && n < sizeof s - 1; n++) s[n] = msg[n] < 128 ? (char)msg[n] : '?';
    s[n] = 0;
    static int told;
    if (told++ < 40) rt_log("ffx4: %s %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error:" : "warning:", s);
}

int beside_exe(const char *name, char *path, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)size);
    while (n && path[n - 1] != '\\') n--;
    return snprintf(path + n, size - n, "%s", name) < (int)(size - n);
}

int file_there(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

int g_loader = -1;

int load(void) {
    if (g_loader >= 0) return g_loader;
    const VkCtx *c = vk_ctx();
    if (!c || !c->device) return -1;
    char loader[MAX_PATH];
    if (!beside_exe("amd_fidelityfx_loader_vk.dll", loader, sizeof loader) || !file_there(loader))
        return g_loader = 0;
    HMODULE m = LoadLibraryA(loader);
    if (!m) {
        rt_log("ffx4: %s does not load (error %lu): FSR 4 off", loader, (unsigned long)GetLastError());
        return g_loader = 0;
    }
    A.create = (PfnFfxCreateContext)(void *)GetProcAddress(m, "ffxCreateContext");
    A.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(m, "ffxDestroyContext");
    A.configure = (PfnFfxConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    A.query = (PfnFfxQuery)(void *)GetProcAddress(m, "ffxQuery");
    A.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    if (!A.create || !A.destroy || !A.configure || !A.query || !A.dispatch) {
        rt_log("ffx4: amd_fidelityfx_loader_vk.dll lacks the API entry points: FSR 4 off");
        return g_loader = 0;
    }
    return g_loader = 1;
}

int versions(uint64_t type, uint64_t *ids, const char **names, int cap) {
    const VkCtx *c = vk_ctx();
    VkDeviceContext dc = {c->device, c->phys, vkGetDeviceProcAddr};
    uint64_t n = 0;
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = type;
    q.device = &dc;
    q.outputCount = &n;
    if (A.query(nullptr, &q.header) != FFX_API_RETURN_OK || !n) return 0;
    if (n > (uint64_t)cap) n = (uint64_t)cap;
    q.versionIds = ids;
    q.versionNames = names;
    if (A.query(nullptr, &q.header) != FFX_API_RETURN_OK) return 0;
    return (int)n;
}

int probe(void) {
    int loaded = load();
    if (loaded <= 0) return loaded;
    char upscaler[MAX_PATH];
    if (!beside_exe("amd_fidelityfx_upscaler_vk.dll", upscaler, sizeof upscaler) || !file_there(upscaler)) return 0;
    uint64_t ids[16] = {};
    const char *names[16] = {};
    int n = versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, ids, names, 16);
    if (!n) {
        rt_log("ffx4: the FSR SDK's loader lists no upscaler for this device: FSR 4 off");
        return 0;
    }
    char list[200] = "";
    size_t len = 0;
    int best = -1, best_major = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = names[i] ? names[i] : "?";
        len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", i ? ", " : "", nm);
        if (len >= sizeof list) len = sizeof list - 1;
        int major = atoi(nm);
        if (major >= 4 && major > best_major) best = i, best_major = major;
    }
    if (best < 0) {
        rt_log("ffx4: upscaler versions for this device: %s; none is FSR 4 (it needs a Radeon RX 6000, 7000 or 9000 card "
               "or a supported Ryzen APU, and AMD's driver 26.10.1 or newer): FSR 3.1 stays", list);
        return 0;
    }
    g_version = ids[best];
    snprintf(g_name, sizeof g_name, "AMD FSR %.24s", names[best]);
    for (char *p = g_name; *p; p++)
        if (*p == '*') *p = 0;
    for (size_t k = strlen(g_name); k && g_name[k - 1] == ' '; k--) g_name[k - 1] = 0;
    rt_log("ffx4: upscaler versions for this device: %s; FSR 4 is %s (id %#llx)", list, g_name, (unsigned long long)g_version);
    return 1;
}

struct Made {
    ffxCreateContextDescUpscale desc;
    ffxCreateBackendVKDesc backend;
    ffxOverrideVersion version;
    ffxCreateContextDescUpscaleVersion built;
};
struct Up {
    ffxContext ctx = nullptr;
    uint32_t mw = 0, mh = 0, ow = 0, oh = 0;
    bool drs = false;
    uint64_t used = 0, used_ns = 0;
    Made *made = nullptr;
    uint64_t vram = 0;
    uint64_t fence = 0;
} U[4];
uint64_t up_tick;
const uint64_t UP_RECENT = 8;
const uint64_t UP_IDLE_NS = 90ull * 1000000000ull;

UpdList g_dead;
const int DEAD_CTX4 = 4;

void dead_release(const UpdEntry *e, void *) {
    uint64_t t0 = perf_mark_begin();
    ffxContext c = (ffxContext)e->handle;
    A.destroy(&c, nullptr);
    free(e->aux);
    perf_mark_end(PERF_M_UPREL, t0, 0);
}

void retire_up(Up *u, const char *why) {
    rt_log("ffx4: the %ux%u context (render sizes up to %ux%u) let go of (destroyed once the GPU is past its last "
           "upscale): %s", u->ow, u->oh, u->mw, u->mh, why);
    if (!upd_put(&g_dead, DEAD_CTX4, u->ctx, u->made, u->fence, 0)) {
        vk_upscaler_settle();
        if (!upd_put(&g_dead, DEAD_CTX4, u->ctx, u->made, u->fence, 0)) {
            upd_take_all(&g_dead, dead_release, nullptr);
            upd_put(&g_dead, DEAD_CTX4, u->ctx, u->made, u->fence, 0);
        }
    }
    *u = Up();
}

FfxApiResource image(const VkFfxImage *i, uint32_t state, uint32_t usage) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = i->format;
    ci.extent = {i->width, i->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.usage = i->storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0;
    return ffxApiGetResourceVK((void *)i->image, ffxApiGetImageResourceDescriptionVK(i->image, ci, usage), state);
}

Up *create(Up *slot, uint32_t mw, uint32_t mh, uint32_t ow, uint32_t oh, bool drs) {
    const VkCtx *c = vk_ctx();
    *slot = Up();
    Made *m = (Made *)calloc(1, sizeof *m);
    if (!m) {
        rt_log("ffx4: no memory for the descriptors of a %s context", g_name);
        return nullptr;
    }
    m->built.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    m->built.version = FFX_UPSCALER_VERSION;
    m->version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    m->version.header.pNext = &m->built.header;
    m->version.versionId = g_version;
    m->backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    m->backend.header.pNext = &m->version.header;
    m->backend.deviceContext = {c->device, c->phys, vkGetDeviceProcAddr};
    m->desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    m->desc.header.pNext = &m->backend.header;
    m->desc.flags =
        FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (rt_knob("FFX_DEBUG", "0")) m->desc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    if (drs) m->desc.flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    m->desc.maxRenderSize = {mw, mh};
    m->desc.maxUpscaleSize = {ow, oh};
    m->desc.fpMessage = message;
    uint64_t t0 = plat_now_ns();
    ffxReturnCode_t r = A.create(&slot->ctx, &m->desc.header, nullptr);
    if (r != FFX_API_RETURN_OK || !slot->ctx) {
        rt_log("ffx4: %s context creation for %ux%u (render sizes up to %ux%u) failed (%u)", g_name, ow, oh, mw, mh,
               (unsigned)r);
        free(m);
        *slot = Up();
        return nullptr;
    }
    slot->made = m;
    slot->mw = mw, slot->mh = mh, slot->ow = ow, slot->oh = oh, slot->drs = drs;
    ffxQueryGetProviderVersion v = {};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    A.query(&slot->ctx, &v.header);
    FfxApiEffectMemoryUsage mem = {};
    ffxQueryDescUpscaleGetGPUMemoryUsage mq = {};
    mq.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    mq.gpuMemoryUsageUpscaler = &mem;
    A.query(&slot->ctx, &mq.header);
    slot->vram = mem.totalUsageInBytes;
    rt_log("ffx4: FSR %s upscaling to %ux%u from render sizes up to %ux%u, made in %.0f ms, %.0f MiB of video memory",
           v.versionName ? v.versionName : "4", ow, oh, mw, mh, (double)(plat_now_ns() - t0) / 1e6,
           (double)mem.totalUsageInBytes / 1048576.0);
    if (v.versionName && atoi(v.versionName) < 4) {
        rt_log("ffx4: the context runs FSR %s, not FSR 4: FSR 4 off", v.versionName);
        A.destroy(&slot->ctx, nullptr);
        free(m);
        *slot = Up();
        return nullptr;
    }
    return slot;
}

Up *context_for(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    up_tick++;
    uint64_t now = plat_now_ns();
    const bool drs = cfg_bool("NX2SARIA_DYN_RES");
    Up *found = nullptr;
    for (Up &u : U)
        if (u.ctx && u.ow == ow && u.oh == oh && u.drs == drs && (drs ? w <= u.mw && h <= u.mh : w == u.mw && h == u.mh))
            found = &u;
    int gone = 0;
    for (Up &u : U)
        if (u.ctx && &u != found && u.ow == ow && u.oh == oh && up_tick - u.used >= UP_RECENT)
            retire_up(&u, "the render size changed"), gone++;
    if (gone) vk_upscaler_let_go();
    if (found) {
        found->used = up_tick, found->used_ns = now;
        return found;
    }
    Up *slot = nullptr, *lru = nullptr;
    for (Up &u : U) {
        if (!u.ctx) {
            if (!slot) slot = &u;
            continue;
        }
        if (up_tick - u.used >= UP_RECENT && (!lru || u.used < lru->used)) lru = &u;
    }
    if (!slot && lru) {
        retire_up(lru, "its slot went to another output size");
        vk_upscaler_let_go();
        slot = lru;
    }
    if (!slot) return nullptr;
    uint32_t mw = !drs ? w : w > ow ? w : ow, mh = !drs ? h : h > oh ? h : oh;
    Up *u = create(slot, mw, mh, ow, oh, drs);
    if (!u) {
        int freed = 0;
        for (Up &o : U)
            if (o.ctx && up_tick - o.used >= UP_RECENT) retire_up(&o, "room for a context whose creation failed"), freed++;
        freed += vk_ffx_trim_idle(UP_RECENT);
        const int dead = vk_ffx_dead_take(vk_upscaler_gpu_wait(vk_ffx_dead_fence()));
        if (freed || dead) u = create(slot, mw, mh, ow, oh, drs);
    }
    if (u) u->used = up_tick, u->used_ns = now;
    return u;
}

void failed(const char *what, unsigned code) {
    if (++g_failures == FAIL_MAX) {
        rt_log("ffx4: %s failed %d times in a row (last %u): FSR 4 is off, FSR 3.1 takes its place until FSR 4 is "
               "chosen again in the menu", what, FAIL_MAX, code);
        g_off.store(1, std::memory_order_release);
    }
}

}

extern "C" int vk_ffx4_available(void) {
    int s = g_state.load(std::memory_order_acquire);
    if (s >= 0) return s;
    std::lock_guard<std::mutex> hold(g_lock);
    s = g_state.load(std::memory_order_acquire);
    if (s >= 0) return s;
    s = probe();
    if (s >= 0) g_state.store(s, std::memory_order_release);
    return s > 0;
}

extern "C" const char *vk_ffx4_name(void) { return g_name; }

extern "C" int vk_ffx4_failed(void) { return g_off.load(std::memory_order_acquire); }

extern "C" void vk_ffx4_retry(void) {
    if (!g_off.exchange(0, std::memory_order_acq_rel)) return;
    g_failures = 0, g_retry_ns = 0;
    rt_log("ffx4: FSR 4 chosen again in the menu: tried anew");
}

extern "C" void vk_ffx4_trim(uint64_t now_ns) {
    for (Up &u : U)
        if (u.ctx && now_ns - u.used_ns > UP_IDLE_NS) retire_up(&u, "no upscale used it for 90 s");
}

extern "C" int vk_ffx4_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                               const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, const VkFfxCamera *cam) {
    if (!vk_ffx4_available() || g_off.load(std::memory_order_acquire)) return 0;
    uint64_t now = plat_now_ns();
    if (g_failures && now < g_retry_ns) return 0;
    Up *u = context_for(color->width, color->height, out->width, out->height);
    if (!u) {
        g_retry_ns = now + RETRY_NS;
        failed("making a context", 0);
        return 0;
    }
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    d.color = image(color, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.depth = image(depth, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.motionVectors = image(mv, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, 0);
    d.output = image(out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = {jx, jy};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {color->width, color->height};
    d.upscaleSize = {out->width, out->height};
    int sharp = cfg_int("NX2SARIA_SHARPNESS");
    d.enableSharpening = sharp > 0;
    d.sharpness = sharp > 100 ? 1.0f : sharp / 100.0f;
    d.frameTimeDelta = dt_ms;
    d.preExposure = 1.0f;
    d.reset = reset != 0;
    d.cameraNear = cam && cam->znear > 0 ? cam->znear : 0.1f;
    d.cameraFar = cam && cam->zfar > 0 ? cam->zfar : 1e8f;
    d.cameraFovAngleVertical = cam && cam->fov_y > 0 ? cam->fov_y : 1.0f;
    d.viewSpaceToMetersFactor = cam && cam->meters > 0 ? cam->meters : 1.0f;
    ffxReturnCode_t r = A.dispatch(&u->ctx, &d.header);
    u->fence = vk_exec_recording_fence();
    if (r != FFX_API_RETURN_OK) {
        static int told;
        if (told++ < 8) rt_log("ffx4: %s dispatch failed (%u)", g_name, (unsigned)r);
        failed("a dispatch", (unsigned)r);
        return 0;
    }
    g_failures = 0;
    return 1;
}

extern "C" int vk_ffx4_api_file(const char *name) {
    char path[MAX_PATH];
    return beside_exe(name, path, sizeof path) && file_there(path);
}
extern "C" int vk_ffx4_api_versions(uint64_t create_type, uint64_t *ids, const char **names, int cap) {
    std::lock_guard<std::mutex> hold(g_lock);
    return load() > 0 ? versions(create_type, ids, names, cap) : 0;
}
extern "C" ffxReturnCode_t vk_ffx4_api_create(ffxContext *ctx, ffxCreateContextDescHeader *desc) {
    return A.create ? A.create(ctx, desc, nullptr) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx4_api_destroy(ffxContext *ctx) {
    return A.destroy ? A.destroy(ctx, nullptr) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx4_api_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc) {
    return A.configure ? A.configure(ctx, desc) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx4_api_query(ffxContext *ctx, ffxQueryDescHeader *desc) {
    return A.query ? A.query(ctx, desc) : FFX_API_RETURN_ERROR;
}
extern "C" ffxReturnCode_t vk_ffx4_api_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc) {
    return A.dispatch ? A.dispatch(ctx, desc) : FFX_API_RETURN_ERROR;
}

extern "C" int vk_ffx4_holds(void) {
    for (Up &u : U)
        if (u.ctx) return 1;
    return 0;
}

extern "C" void vk_ffx4_release(void) {
    if (!A.destroy) return;
    for (Up &u : U)
        if (u.ctx) A.destroy(&u.ctx, nullptr), free(u.made), u = Up();
    upd_take_all(&g_dead, dead_release, nullptr);
}

extern "C" void vk_ffx4_release_idle(void) {
    for (Up &u : U)
        if (u.ctx) retire_up(&u, "the upscaler is off");
}

extern "C" int vk_ffx4_dead_take(uint64_t done) {
    return g_dead.n ? upd_take(&g_dead, done, UINT64_MAX, dead_release, nullptr) : 0;
}

extern "C" uint64_t vk_ffx4_dead_fence(void) { return upd_max_fence(&g_dead); }

extern "C" uint64_t vk_ffx4_vram(int *contexts) {
    uint64_t bytes = 0;
    int n = 0;
    for (Up &u : U)
        if (__atomic_load_n(&u.ctx, __ATOMIC_RELAXED)) n++, bytes += __atomic_load_n(&u.vram, __ATOMIC_RELAXED);
    *contexts = n;
    return bytes;
}
#endif
