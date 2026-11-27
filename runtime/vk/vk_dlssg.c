#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dlssg_pace.h"
#include "host.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
#include "vk_dlssg_device.h"
#include "vk_fg_stats.h"
#include "dlssg/nx2_sl.h"
#include "ui/menu.h"

static struct {
    int wanted, loaded, active;
    int fail;
    int mode, pending;
    int shown_mult;
    PFN_nx2sl_frames_max frames_max;
    PFN_nx2sl_init init;
    PFN_nx2sl_instance_proc gipa;
    PFN_nx2sl_device_proc gdpa;
    PFN_nx2sl_available available;
    PFN_nx2sl_log_unavailable log_unavailable;
    PFN_nx2sl_support support;
    int no_gpu;
    const char *sl_why;
    PFN_nx2sl_frame frame;
    PFN_nx2sl_present_marker marker;
    PFN_nx2sl_sleep sleep;
    PFN_nx2sl_submit_end submit_end;
    PFN_nx2sl_shown shown;
    PFN_nx2sl_reflex reflex;
    PFN_nx2sl_reflex2 reflex2;
    uint32_t game;
    uint32_t cur;
    uint32_t started;
    uint64_t hosted_ns;
    uint32_t limit_want, limit_set;
    int limit_refused;
    DlssgPace pace;
    int pace_said;
    uint64_t n_hold, hold_ns, n_unheld;
    PFN_nx2sl_shutdown shutdown;
    PFN_vkCreateSwapchainKHR create_sc;
    PFN_vkDestroySwapchainKHR destroy_sc;
    PFN_vkGetSwapchainImagesKHR images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
} S = {.wanted = -1, .fail = DLSSG_NO_FILES, .shown_mult = 1, .pace_said = -1};

static void sl_line(const char *line) { rt_log("%s", line); }

static int reflex_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *s = cfg_get("NX2SARIA_REFLEX");
        m = s && *s ? atoi(s) : 1;
        if (m < 0 || m > 2) m = 1;
    }
    return m;
}

static void reflex_apply(uint32_t us) {
    if (!S.reflex2) {
        S.reflex(reflex_mode());
        return;
    }
    if (__atomic_load_n(&S.limit_refused, __ATOMIC_ACQUIRE)) us = 0;
    int ok = S.reflex2(reflex_mode(), us);
    if (!ok && us) {
        __atomic_store_n(&S.limit_refused, 1, __ATOMIC_RELEASE);
        rt_log("dlssg: Streamline did not take Reflex's frame limit of %.3f ms: the game's own limiter keeps the limit "
               "for the rest of the run", us / 1000.0);
        ok = S.reflex2(reflex_mode(), 0), us = 0;
    }
    __atomic_store_n(&S.limit_set, ok ? us : 0, __ATOMIC_RELEASE);
}

static int limit_allowed(void) {
    static unsigned seen;
    static int read, on;
    unsigned v = cfg_version();
    if (!read || v != seen) on = cfg_bool("NX2SARIA_REFLEX_LIMIT"), seen = v, read = 1;
    return on && S.reflex2 && !__atomic_load_n(&S.limit_refused, __ATOMIC_ACQUIRE);
}

int vk_dlssg_wanted(void) {
    if (S.wanted < 0) S.wanted = !strcmp(cfg_value("NX2SARIA_FRAMEGEN"), "dlss");
    return S.wanted;
}

int vk_dlssg_early(void) {
    if (!vk_dlssg_wanted()) return 0;
    if (S.loaded) return S.loaded > 0;
    S.loaded = -1;
#ifdef _WIN32
    wchar_t dir[MAX_PATH], path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\') n--;
    dir[n ? n - 1 : 0] = 0;
    swprintf(path, MAX_PATH + 32, L"%ls\\nx2navi_dlssg.dll", dir);
    HMODULE m = LoadLibraryW(path);
    if (!m) {
        rt_log("dlssg: nx2navi_dlssg.dll is not next to the game: DLSS Frame Generation off");
        return 0;
    }
    PFN_nx2sl_version ver = (PFN_nx2sl_version)(void *)GetProcAddress(m, "nx2sl_version");
    S.init = (PFN_nx2sl_init)(void *)GetProcAddress(m, "nx2sl_init");
    S.gipa = (PFN_nx2sl_instance_proc)(void *)GetProcAddress(m, "nx2sl_instance_proc");
    S.gdpa = (PFN_nx2sl_device_proc)(void *)GetProcAddress(m, "nx2sl_device_proc");
    S.available = (PFN_nx2sl_available)(void *)GetProcAddress(m, "nx2sl_available");
    S.log_unavailable = (PFN_nx2sl_log_unavailable)(void *)GetProcAddress(m, "nx2sl_log_unavailable");
    S.support = (PFN_nx2sl_support)(void *)GetProcAddress(m, "nx2sl_support");
    S.frame = (PFN_nx2sl_frame)(void *)GetProcAddress(m, "nx2sl_frame");
    S.marker = (PFN_nx2sl_present_marker)(void *)GetProcAddress(m, "nx2sl_present_marker");
    S.sleep = (PFN_nx2sl_sleep)(void *)GetProcAddress(m, "nx2sl_sleep");
    S.submit_end = (PFN_nx2sl_submit_end)(void *)GetProcAddress(m, "nx2sl_submit_end");
    S.shown = (PFN_nx2sl_shown)(void *)GetProcAddress(m, "nx2sl_shown");
    S.shutdown = (PFN_nx2sl_shutdown)(void *)GetProcAddress(m, "nx2sl_shutdown");
    S.reflex = (PFN_nx2sl_reflex)(void *)GetProcAddress(m, "nx2sl_reflex");
    S.frames_max = (PFN_nx2sl_frames_max)(void *)GetProcAddress(m, "nx2sl_frames_max");
    if (!S.frames_max) rt_log("dlssg: nx2navi_dlssg.dll is older than multi frame generation: 2x at most");
    S.reflex2 = (PFN_nx2sl_reflex2)(void *)GetProcAddress(m, "nx2sl_reflex2");
    if (!S.reflex2) rt_log("dlssg: nx2navi_dlssg.dll is older than Reflex's frame limit: the game's own limiter keeps it");
    if (!ver || ver() != NX2SL_VERSION || !S.init || !S.gipa || !S.gdpa || !S.available || !S.frame || !S.marker ||
        !S.sleep || !S.submit_end || !S.shown || !S.reflex) {
        rt_log("dlssg: nx2navi_dlssg.dll is from another build: DLSS Frame Generation off");
        return 0;
    }
    if (!S.init(dir, sl_line)) return 0;
    S.loaded = 1;
    return 1;
#else
    return 0;
#endif
}

VkResult vk_dlssg_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out) {
    PFN_vkCreateInstance f = S.loaded > 0 ? (PFN_vkCreateInstance)S.gipa()(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
    return f ? f(ci, NULL, out) : vkCreateInstance(ci, NULL, out);
}

PFN_vkVoidFunction vk_dlssg_instance_proc(VkInstance inst, const char *name) {
    return S.loaded > 0 ? S.gipa()(inst, name) : NULL;
}

VkResult vk_dlssg_enumerate(VkInstance inst, uint32_t *n, VkPhysicalDevice *pd) {
    PFN_vkEnumeratePhysicalDevices f =
        S.loaded > 0 ? (PFN_vkEnumeratePhysicalDevices)S.gipa()(inst, "vkEnumeratePhysicalDevices") : NULL;
    return f ? f(inst, n, pd) : vkEnumeratePhysicalDevices(inst, n, pd);
}

int vk_dlssg_interposed(void) { return S.loaded > 0; }

static void give_up(const char *why) {
    S.loaded = -1, S.fail = DLSSG_UNAVAILABLE;
    rt_log("dlssg: DLSS Frame Generation is off in this run: %s", why);
}

typedef struct { PFN_vkCreateDevice f; VkPhysicalDevice pd; const VkDeviceCreateInfo *ci; VkDevice *out; } DevCreate;
static int create_interposed(void *u) {
    const DevCreate *d = u;
    return (int)d->f(d->pd, d->ci, NULL, d->out);
}
static int create_loader(void *u) {
    const DevCreate *d = u;
    return (int)vkCreateDevice(d->pd, d->ci, NULL, d->out);
}

VkResult vk_dlssg_create_device(VkInstance inst, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    DevCreate d = {S.loaded > 0 ? (PFN_vkCreateDevice)S.gipa()(inst, "vkCreateDevice") : NULL, pd, ci, out};
    int how, refused;
    VkResult r = (VkResult)dlssg_device_create(d.f != NULL, p.vendorID, create_interposed, create_loader, &d, &how, &refused);
    if (how == DLSSG_DEV_NOT_NVIDIA) {
        S.no_gpu = 1;
        give_up("the graphics card in use is not an NVIDIA card (the Graphics card setting, gpu_index)");
    } else if (how == DLSSG_DEV_REFUSED) {
        char why[96];
        snprintf(why, sizeof why, "Streamline could not make the device (%d)", refused);
        give_up(why);
    }
    return r;
}

void vk_dlssg_device_ready(VkPhysicalDevice physical, VkDevice dev) {
    if (S.loaded <= 0) return;
    if (!S.available()) {
        S.fail = DLSSG_UNAVAILABLE;
        if (S.log_unavailable) S.log_unavailable(physical);
        const char *name = NULL;
        int v = S.support ? S.support(&name) : -1;
        if (v == 0) S.no_gpu = 1;
        else if (v == 2) S.sl_why = name;
        return;
    }
    PFN_vkGetDeviceProcAddr g = S.gdpa();
    S.create_sc = (PFN_vkCreateSwapchainKHR)g(dev, "vkCreateSwapchainKHR");
    S.destroy_sc = (PFN_vkDestroySwapchainKHR)g(dev, "vkDestroySwapchainKHR");
    S.images = (PFN_vkGetSwapchainImagesKHR)g(dev, "vkGetSwapchainImagesKHR");
    S.acquire = (PFN_vkAcquireNextImageKHR)g(dev, "vkAcquireNextImageKHR");
    S.present = (PFN_vkQueuePresentKHR)g(dev, "vkQueuePresentKHR");
    S.active = S.create_sc && S.destroy_sc && S.images && S.acquire && S.present;
    rt_log("dlssg: %s", S.active ? "DLSS Frame Generation on the swapchain" : "the swapchain hooks are missing: off");
    if (S.active) reflex_apply(0);
}

int vk_dlssg_active(void) { return S.active; }

int vk_dlssg_status(void) {
    if (S.active) return DLSSG_RUNS;
    return vk_dlssg_wanted() ? S.fail : DLSSG_NOT_WANTED;
}

int vk_dlssg_why(const char **sl_why) {
    if (sl_why) *sl_why = S.no_gpu ? NULL : S.sl_why;
    return S.no_gpu;
}

int vk_dlssg_gpu_may(void) {
    const VkCtx *c = vk_ctx();
    return !c || (c->props.vendorID == 0x10de && c->props.deviceID >= 0x2600);
}

int vk_dlssg_multiplier_of(const char *value, int max, const char **why) {
    const char *w = NULL;
    long n = 2;
    if (value && *value) {
        char *end;
        n = strtol(value, &end, 10);
        if (*end == 'x' || *end == 'X') end++;
        if (*end || end == value || n < 2) w = "is not 2, 3 or 4", n = 2;
        else if (n > 4) w = "is above 4, the most there is", n = 4;
    }
    if (max >= 2 && n > max) w = "is above what this GPU and driver allow", n = max;
    if (why) *why = w;
    return (int)n;
}

static int gpu_multiplier_max(int *old_dll) {
    if (old_dll) *old_dll = !S.frames_max;
    if (!S.frames_max) return 2;
    uint32_t f = S.frames_max();
    return !f ? 0 : f >= 3 ? 4 : (int)f + 1;
}

int vk_dlssg_multiplier_max(int *old_dll) {
    if (old_dll) *old_dll = 0;
    return S.active ? gpu_multiplier_max(old_dll) : 0;
}

int vk_dlssg_multiplier(void) { return S.active ? __atomic_load_n(&S.shown_mult, __ATOMIC_RELAXED) : 1; }

static int multiplier_wanted(void) {
    static char last[24];
    static int last_max = -1, m = 2;
    const char *value = cfg_value("NX2SARIA_FRAMEGEN_MULTIPLIER");
    int max = gpu_multiplier_max(NULL);
    if (max == last_max && !strncmp(value, last, sizeof last - 1)) return m;
    snprintf(last, sizeof last, "%s", value);
    last_max = max;
    const char *why;
    int n = vk_dlssg_multiplier_of(value, max, &why);
    if (why) rt_log("dlssg: framegen_multiplier = %s %s (up to %dx here): %dx", value, why, max ? max : 4, n);
    m = max ? n : 2;
    return m;
}

PFN_vkGetDeviceProcAddr vk_dlssg_dpa(void) { return S.loaded > 0 && S.gdpa ? S.gdpa() : NULL; }

VkResult vk_dlssg_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc) {
    VkResult r = S.create_sc(d, ci, NULL, sc);
    if (r == VK_SUCCESS) {
        static int made;
        uint32_t want = __atomic_load_n(&S.limit_want, __ATOMIC_ACQUIRE);
        reflex_apply(want);
        if (S.reflex2 && made++ < 16)
            rt_log("dlssg: swapchain %d: Reflex's mode and frame limit sent again (%.3f ms asked, %.3f ms taken; 0 is "
                   "none)", made, want / 1000.0, __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE) / 1000.0);
    }
    return r;
}
void vk_dlssg_destroy_swapchain(VkDevice d, VkSwapchainKHR sc) { S.destroy_sc(d, sc, NULL); }
VkResult vk_dlssg_get_images(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *img) { return S.images(d, sc, n, img); }
VkResult vk_dlssg_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore sem, uint32_t *idx) {
    return S.acquire(d, sc, t, sem, VK_NULL_HANDLE, idx);
}

static FgsAge ages;

static void stats(void) {
    static uint64_t shown, next = 600;
    if (++shown < next) return;
    next = shown + (rt_trace & RT_TRACE_PERF ? 600 : 3600);
    uint64_t n = __atomic_exchange_n(&S.n_hold, 0, __ATOMIC_RELAXED);
    uint64_t ns = __atomic_exchange_n(&S.hold_ns, 0, __ATOMIC_RELAXED);
    uint64_t un = __atomic_exchange_n(&S.n_unheld, 0, __ATOMIC_RELAXED);
    uint32_t us = __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE);
    char limit[96];
    if (us) snprintf(limit, sizeof limit, "Reflex holds frame starts to %.3f ms", us / 1000.0);
    else snprintf(limit, sizeof limit, "the game's own limiter holds the frame limit");
    char age[256];
    fgs_age_text(&ages, age, sizeof age);
    rt_log("dlssg: present %llu: %s; %llu frame starts held by Reflex's sleep, %.2f ms on average; %llu not held; %s",
           (unsigned long long)shown, limit, (unsigned long long)n, n ? (double)ns / (double)n / 1e6 : 0.0,
           (unsigned long long)un, age);
}

VkResult vk_dlssg_present(VkQueue q, const VkPresentInfoKHR *pi) {
    S.marker(0, S.cur);
    VkResult r = S.present(q, pi);
    S.marker(1, S.cur);
    __atomic_store_n(&S.hosted_ns, kern_now_ns(), __ATOMIC_RELEASE);
    return r;
}

#define FG_SETTLE_FRAMES 12

void vk_dlssg_frame_start(void) {
    if (!S.active) return;
    uint32_t f = __atomic_load_n(&S.game, __ATOMIC_RELAXED) + 1;
    if (f == S.started) return;
    S.started = f;
    uint64_t hosted = __atomic_load_n(&S.hosted_ns, __ATOMIC_ACQUIRE), t0 = kern_now_ns();
    int hold = hosted && t0 - hosted < DLSSG_HOLD_IDLE_NS;
    int paced = hold && __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE) != 0;
    uint64_t p0 = paced ? perf_begin() : 0;
    S.sleep(f, hold);
    if (!hold) {
        __atomic_add_fetch(&S.n_unheld, 1, __ATOMIC_RELAXED);
        return;
    }
    if (paced) perf_end_any(PERF_PACE, p0);
    __atomic_add_fetch(&S.hold_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&S.n_hold, 1, __ATOMIC_RELAXED);
}

static void pace_say(int reflex, uint32_t us, const char *why) {
    static int lines;
    if (S.pace_said == reflex || lines >= 40) return;
    S.pace_said = reflex, lines++;
    if (reflex)
        rt_log("frame rate: Reflex holds the frame limit where each frame starts (%.3f ms%s); the game's own limiter "
               "keeps only its ceiling", us / 1000.0, S.pace.shown ? ", the frames on screen" : "");
    else rt_log("frame rate: the game's own limiter (DLSS Frame Generation: %s)", why);
}

int vk_dlssg_pace(uint64_t period_ns) {
    if (!S.active) return 0;
    uint64_t hosted = __atomic_load_n(&S.hosted_ns, __ATOMIC_ACQUIRE), now = kern_now_ns();
    int allowed = limit_allowed(), mult = vk_dlssg_multiplier(), found;
    uint32_t applied = __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE), want;
    int held = dlssg_pace_step(&S.pace, now, period_ns, mult, hosted, allowed, applied, &want, &found);
    __atomic_store_n(&S.limit_want, want, __ATOMIC_RELEASE);
    double ms = (double)period_ns / 1e6;
    if (found == DLSSG_PACE_FAST)
        rt_log("dlssg: Reflex does not hold the frame limit (over half of %d frames came sooner than three quarters of "
               "%.2f ms): the game's own limiter holds it for the rest of the run", DLSSG_PACE_WINDOW, ms);
    else if (found == DLSSG_PACE_SHOWN)
        rt_log("dlssg: Reflex held the frames on screen to the limit, not the frames the game draws (over half of %d "
               "came about %d times %.2f ms apart): it is given %.2f ms over the multiplier from now on",
               DLSSG_PACE_WINDOW, mult, ms, ms);
    else if (found == DLSSG_PACE_SLOW)
        rt_log("dlssg: the frames still came about %d times %.2f ms apart: the game's own limiter holds the limit for the "
               "rest of the run", mult, ms);
    if (held) pace_say(1, want, NULL);
    else if (!want)
        pace_say(0, 0, !allowed     ? (!S.reflex2 ? "nx2navi_dlssg.dll is older than Reflex's frame limit"
                                       : __atomic_load_n(&S.limit_refused, __ATOMIC_ACQUIRE) ? "Streamline refused the limit"
                                                                                             : "NX2SARIA_REFLEX_LIMIT=0")
                       : S.pace.bad ? "Reflex did not hold the limit"
                       : !period_ns ? "a movie plays, or no limit"
                       : hosted     ? "no host present for 200 ms"
                                    : "no host present yet");
    return held;
}

void vk_dlssg_game_present(void) { __atomic_add_fetch(&S.game, 1, __ATOMIC_RELAXED); }

void vk_dlssg_submitted(uint64_t frame) {
    if (S.active) S.submit_end((uint32_t)frame);
}

void vk_dlssg_present_frame(uint64_t frame) { S.cur = (uint32_t)frame; }

uint64_t vk_dlssg_shown(void) { return S.active ? S.shown() : 0; }

void vk_dlssg_frame(VkCommandBuffer cb, VkExtent2D ext) {
    if (!S.active) return;
    VkDlaaFgSet s = {0};
    int have = vk_dlaa_fg_take(&s) && s.depth_view && s.mv_view && s.cam_ok;
    const VkFfxImage d = s.depth, m = s.mv;
    const VkImageView dv = s.depth_view, mvv = s.mv_view;
    const VkDlaaCamera c = s.cam;
    const float jx = s.jx, jy = s.jy;
    const int reset = s.reset;
    int want = have && !menu_is_open();
    if (want == S.mode) S.pending = 0;
    else if (++S.pending >= FG_SETTLE_FRAMES) { S.mode = want; S.pending = 0; }
    else want = S.mode;
    int on = want, mult = multiplier_wanted();
    __atomic_store_n(&S.shown_mult, on ? mult : 1, __ATOMIC_RELAXED);
    uint32_t limit = __atomic_load_n(&S.limit_want, __ATOMIC_ACQUIRE);
    if (S.reflex2 && limit != __atomic_load_n(&S.limit_set, __ATOMIC_ACQUIRE)) reflex_apply(limit);
    if (on && have) fgs_age_note(&ages, s.age, vk_dlaa_fg_overlaps());
    else ages.overlaps = vk_dlaa_fg_overlaps();
    stats();
    Nx2SlImage zd = {d.image, dv, d.format, d.width, d.height, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT};
    Nx2SlImage zm = {m.image, mvv, m.format, m.width, m.height, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
    Nx2SlCamera k;
    memset(&k, 0, sizeof k);
    if (have) {
        memcpy(k.view_to_clip, c.view_to_clip, 64), memcpy(k.clip_to_view, c.clip_to_view, 64);
        memcpy(k.clip_to_prev_clip, c.clip_to_prev_clip, 64), memcpy(k.prev_clip_to_clip, c.prev_clip_to_clip, 64);
        memcpy(k.pos, c.pos, 12), memcpy(k.up, c.up, 12), memcpy(k.right, c.right, 12), memcpy(k.fwd, c.fwd, 12);
        k.znear = c.znear, k.zfar = c.zfar, k.fov_y = c.fov_y, k.aspect = c.aspect;
        static int told;
        if (c.inverted && !told++) rt_log("dlssg: the scene's depth is inverted, which Streamline is not told");
        k.jitter[0] = jx, k.jitter[1] = jy;
        k.mv_scale[0] = 1.0f / (float)d.width, k.mv_scale[1] = 1.0f / (float)d.height;
        k.reset = reset;
    }
    S.frame(cb, have ? &zd : NULL, have ? &zm : NULL, NULL, have ? &k : NULL, ext.width, ext.height, on ? mult - 1 : 0, S.cur);
}

void vk_dlssg_release(void) {
    if (S.loaded > 0 && S.shutdown) S.shutdown();
    S.active = 0;
    __atomic_store_n(&S.limit_set, 0, __ATOMIC_RELEASE);
}
