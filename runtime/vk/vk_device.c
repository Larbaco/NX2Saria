// Contains code Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <errno.h>
#include <unistd.h>
#endif

#include "cfg.h"
#include "gpu_choice.h"
#include "host.h"
#include "hw_detect.h"
#include "log_stall.h"
#include "mem_budget.h"
#include "nvidia_cache.h"
#include "vk.h"
#include "platform/platform.h"
#include "session_summary.h"
#include "vk_dlaa.h"
#include "vk_fsr3.h"
#include "vk_gpa.h"
#include "vk_fg.h"
#include "vk_dlssg.h"
#include "vk_dlssg_device.h"
#include "vk_lowlat.h"
#include "mesh_pipeline_check.h"
#include "vk_mesh_path.h"
#include "vk_mesh_fb.h"

PFN_vkGetDeviceProcAddr vk_dpa = vkGetDeviceProcAddr;

static uint32_t g_gfx_queues = 1;
#define QFAM_MAX 16
static uint32_t g_nqfam;
static VkQueueFlags g_qfam_flags[QFAM_MAX];
static uint32_t g_qfam_count[QFAM_MAX];

static VkCtx g_vk;
static int g_ready;
static PlatMutex g_lock = PLAT_MUTEX_INIT;
static VkFence lost_fence;
static PlatMutex lost_fence_lock = PLAT_MUTEX_INIT;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    int err = (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    if (err) plat_inc32(&g_vk.validation_errors);
    static volatile int32_t logged;
    if (plat_inc32(&logged) <= 500) rt_log("vk validation %s: %s", err ? "error" : "warning", d->pMessage);
    return VK_FALSE;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int layer_present(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *l = calloc(n ? n : 1, sizeof *l);
    vkEnumerateInstanceLayerProperties(&n, l);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(l[i].layerName, name)) found = 1;
    free(l);
    return found;
}

static int instance_ext_present(const char *name) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateInstanceExtensionProperties(NULL, &n, e);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) found = 1;
    free(e);
    return found;
}

#define MESA_CACHE_SIZE "8G"
static void mesa_cache_beside_exe(void) {
    char dir[PLAT_PATH_MAX * 2], size[32];
    if (plat_getenv("MESA_SHADER_CACHE_DIR", dir, sizeof dir)) return;
    int len = plat_exe_path(dir, sizeof dir - 16);
    char *cut = len ? plat_last_sep(dir) : NULL;
    if (!cut) return;
    strcpy(cut + 1, "mesa-cache");
    plat_setenv("MESA_SHADER_CACHE_DIR", dir);
    if (!plat_getenv("MESA_SHADER_CACHE_MAX_SIZE", size, sizeof size)) plat_setenv("MESA_SHADER_CACHE_MAX_SIZE", MESA_CACHE_SIZE);
}

#if defined(_WIN32) || defined(__linux__)
static int nvidia_driver_present(void) {
#ifdef _WIN32
    char root[PLAT_PATH_MAX], file[PLAT_PATH_MAX + 32];
    if (!plat_getenv("SystemRoot", root, sizeof root)) return 0;
    snprintf(file, sizeof file, "%s\\System32\\nvapi64.dll", root);
    return plat_fs_kind(file) == PLAT_FS_FILE;
#else
    return plat_fs_kind("/proc/driver/nvidia/version") == PLAT_FS_FILE;
#endif
}

static int folder_error(const char *dir) {
#ifdef _WIN32
    char probe[PLAT_PATH_MAX * 2 + 32];
    snprintf(probe, sizeof probe, "%s\\.write-test-%u", dir, (unsigned)plat_process_id());
    plat_fs_unlink(probe);
    PlatFile *f = plat_fs_create(probe, NULL);
    if (!f) return plat_fs_os_error() ? plat_fs_os_error() : 1;
    plat_file_close(f);
    plat_fs_unlink(probe);
    return 0;
#else
    return access(dir, W_OK | X_OK) ? errno : 0;
#endif
}

static void nvidia_cache_beside_exe(void) {
    char dir[PLAT_PATH_MAX * 2], size[64], skip[64], mark[16], line[PLAT_PATH_MAX * 2 + 400], why[64] = "";
    const char *what = "";
#ifdef _WIN32
    const int windows = 1;
#else
    const int windows = 0;
#endif
    int ours = plat_getenv(NVC_MARK_VAR, mark, sizeof mark) ? nvc_mark_parse(mark) : 0;
    NvcIn in = {.windows = windows, .enabled = cfg_bool("NX2SARIA_NVIDIA_CACHE_DIR"), .driver = nvidia_driver_present()};
    in.path_set = plat_getenv(NVC_PATH_VAR, dir, sizeof dir) != 0 && !(ours & NVC_SET_PATH);
    in.size_set = plat_getenv(NVC_SIZE_VAR, size, sizeof size) != 0 && !(ours & NVC_SET_SIZE);
    in.skip_set = plat_getenv(NVC_SKIP_VAR, skip, sizeof skip) != 0 && !(ours & NVC_SET_SKIP);
    if (nvc_need_folder(&in)) {
        int len = plat_exe_path(dir, sizeof dir - 16);
        char *cut = len ? plat_last_sep(dir) : NULL;
        if (!cut) return;
        strcpy(cut + 1, NVC_FOLDER);
        int made = plat_fs_mkdir(dir), there = made == PLAT_FS_OK || made == PLAT_FS_EXISTS;
        int err = !there ? plat_fs_os_error() : folder_error(dir);
        what = there ? "is not writable" : "cannot be made";
        in.folder_ok = !err;
        if (err) {
#ifdef _WIN32
            snprintf(why, sizeof why, "Windows error %d", err);
#else
            snprintf(why, sizeof why, "%s", strerror(err));
#endif
        }
    }
    NvcPlan plan = nvc_plan(&in);
    int stale = nvc_stale(&plan, ours);
    if (stale & NVC_SET_PATH) plat_setenv(NVC_PATH_VAR, NULL);
    if (stale & NVC_SET_SIZE) plat_setenv(NVC_SIZE_VAR, NULL);
    if (stale & NVC_SET_SKIP) plat_setenv(NVC_SKIP_VAR, NULL);
    if (ours && plan.verdict != NVC_USE) plat_setenv(NVC_MARK_VAR, NULL);
    if (plan.verdict == NVC_OFF) {
        nvc_off_line(line, sizeof line);
        rt_log("%s", line);
        return;
    }
    if (plan.verdict == NVC_NO_FOLDER) {
        nvc_stays_line(line, sizeof line, dir, what, why);
        rt_log("%s", line);
        return;
    }
    if (plan.verdict != NVC_USE) return;
    int refused = 0;
    if ((plan.set & NVC_SET_PATH) && !plat_setenv(NVC_PATH_VAR, dir)) refused |= NVC_SET_PATH;
    if (plan.set & NVC_SET_SIZE) {
        if (!plat_setenv(NVC_SIZE_VAR, NVC_SIZE_VALUE)) refused |= NVC_SET_SIZE;
        snprintf(size, sizeof size, "%s", NVC_SIZE_VALUE);
    }
    if (plan.set & NVC_SET_SKIP) {
        if (!plat_setenv(NVC_SKIP_VAR, NVC_SKIP_VALUE)) refused |= NVC_SET_SKIP;
        snprintf(skip, sizeof skip, "%s", NVC_SKIP_VALUE);
    }
    nvc_mark_text(plan.set & ~refused, mark, sizeof mark);
    if (plan.set & ~refused) plat_setenv(NVC_MARK_VAR, mark);
    if (refused) {
        nvc_refused_line(line, sizeof line, refused);
        rt_log("%s", line);
        return;
    }
    nvc_use_line(line, sizeof line, windows, plan.set, dir, size, skip);
    rt_log("%s", line);
}
#endif

static const char *submit_thread_why;

static void mesa_submit_thread(void) {
    char cur[16];
    if (plat_getenv("MESA_VK_ENABLE_SUBMIT_THREAD", cur, sizeof cur)) {
        submit_thread_why = "MESA_VK_ENABLE_SUBMIT_THREAD is set in the environment";
        return;
    }
    if (!cfg_bool("NX2SARIA_SUBMIT_THREAD")) {
        submit_thread_why = "NX2SARIA_SUBMIT_THREAD=0: the driver's own choice";
        return;
    }
    plat_setenv("MESA_VK_ENABLE_SUBMIT_THREAD", "1");
    submit_thread_why = "every one on the queue's own thread (NX2SARIA_SUBMIT_THREAD=0 leaves the driver's own choice)";
}

void vk_driver_cache_env(void) {
    static int done;
    if (done) return;
    done = 1;
    mesa_cache_beside_exe();
    mesa_submit_thread();
#if defined(_WIN32) || defined(__linux__)
    nvidia_cache_beside_exe();
#endif
}

static int create_instance(void) {
    vk_driver_cache_env();
    vk_dlssg_early();
    const char *ext[12];
    uint32_t n_ext = (uint32_t)gfx_window_vk_extensions(ext, 7);
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    int validate = cfg_bool("NX2SARIA_VK_VALIDATION");
    if (validate && !layer_present(layers[0])) {
        rt_log("vk: NX2SARIA_VK_VALIDATION set but %s is not installed; continuing without", layers[0]);
        validate = 0;
    }
    if (validate || (vk_dlssg_wanted() && instance_ext_present(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)))
        ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (vk_hdr_armed() && instance_ext_present(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
        ext[n_ext++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
    vk_dlaa_instance_exts(ext, &n_ext, 12);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "NX2Saria",
                             .pEngineName = "NX2Saria", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    rdoc_init();
    VkResult r = vk_dlssg_create_instance(&ci, &g_vk.instance);
    if (r != VK_SUCCESS) { rt_log("vk: vkCreateInstance failed (%d); is a Vulkan 1.3 driver installed?", (int)r); return 0; }
    g_vk.validation = validate;
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.instance, &mi, NULL, &g_vk.messenger);
    }
    return 1;
}

static const char *const k_required_ext[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};

static const char *missing_required(VkPhysicalDevice pd) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    const char *miss = NULL;
    for (size_t i = 0; i < sizeof k_required_ext / sizeof *k_required_ext && !miss; i++)
        if (!has_ext(e, n, k_required_ext[i])) miss = k_required_ext[i];
    free(e);
    return miss;
}

static uint32_t find_gfx_family(VkPhysicalDevice pd, const VkQueueFamilyProperties *qp, uint32_t nq) {
    for (uint32_t i = 0; i < nq; i++)
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            gfx_window_vk_presentable(g_vk.instance, pd, i))
            return i;
    return UINT32_MAX;
}

static int name_has(const char *s, const char *sub) {
    for (size_t n = strlen(sub); *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return 0;
}

static int card_usable(VkPhysicalDevice pd) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    if (p.apiVersion < VK_API_VERSION_1_3 || missing_required(pd)) return 0;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t fam = find_gfx_family(pd, qp, nq);
    free(qp);
    return fam != UINT32_MAX;
}

_Static_assert(GPUC_OTHER == VK_PHYSICAL_DEVICE_TYPE_OTHER && GPUC_INTEGRATED == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
                   GPUC_DISCRETE == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && GPUC_VIRTUAL == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU &&
                   GPUC_CPU == VK_PHYSICAL_DEVICE_TYPE_CPU,
               "gpu_choice.h GPUC_* are VkPhysicalDeviceType's values");

static char g_gpu_choice_note[200];

static char g_prepare_text[1024];

const char *vk_card_prepare_text(void) { return g_prepare_text; }

int vk_card_usable(VkPhysicalDevice pd) { return card_usable(pd); }

static void cards_usable(const VkPhysicalDevice *pd, GpucCard *cards, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) cards[i].usable = cards[i].type == GPUC_DISCRETE && card_usable(pd[i]);
}

static int pick_device(void) {
    uint32_t n = 0;
    vk_dlssg_enumerate(g_vk.instance, &n, NULL);
    if (!n) { rt_log("vk: no Vulkan device"); return 0; }
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vk_dlssg_enumerate(g_vk.instance, &n, pd);
    const char *e = cfg_get("NX2SARIA_GPU_INDEX");
    int want = e && *e ? atoi(e) : -1;
    const char *name = cfg_get("NX2SARIA_GPU_NAME");
    int best = -1, best_score = -1, listed = 0;
    uint32_t best_driver = 0;
    GpucCard *cards = calloc(n, sizeof *cards);
    VkPhysicalDeviceProperties *props = calloc(n, sizeof *props);
    for (uint32_t i = 0; i < n; i++) {
        vkGetPhysicalDeviceProperties(pd[i], &props[i]);
        const VkPhysicalDeviceProperties *p = &props[i];
        rt_log("vk: gpu %u: %s (type %d, api %u.%u.%u)", i, p->deviceName, (int)p->deviceType,
               VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion), VK_API_VERSION_PATCH(p->apiVersion));
        cards[i].name = p->deviceName;
        cards[i].type = (int)p->deviceType;
    }
    char line[800];
    const int asked = want;
    const int from_env = cfg_from_env("NX2SARIA_GPU_INDEX");
    int how = name && *name ? GPUC_BY_NAME : want >= 0 ? GPUC_BY_POSITION : GPUC_BY_AUTO;
    int usable_known = 0;
    {
        if (gpuc_legacy_applies(cards, (int)n, want, name, from_env)) cards_usable(pd, cards, n), usable_known = 1;
        gpuc_setting_line(cards, (int)n, want, name, from_env, line, sizeof line);
        rt_log("%s", line);
        int dropped, give_way;
        want = gpuc_resolve(cards, (int)n, want, name, from_env, &dropped, &give_way);
        if (dropped) {
            gpuc_dropped_line(cards, (int)n, asked, name, line, sizeof line);
            rt_log("%s", line);
        }
        if (give_way >= 0) {
            gpuc_legacy_line(cards, asked, give_way, line, sizeof line);
            rt_log("%s", line);
            how = GPUC_BY_LEGACY;
        }
    }
retry:
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (want >= 0 && (int)i != want) continue;
        if (name && *name && !name_has(p.deviceName, name)) continue;
        if (dlssg_card_skipped(vk_dlssg_interposed(), want >= 0 || (name && *name), p.vendorID)) {
            rt_log("vk: gpu %u skipped: DLSS Frame Generation is on (framegen = dlss), which only an NVIDIA card runs", i);
            continue;
        }
        if (p.apiVersion < VK_API_VERSION_1_3) { rt_log("vk: gpu %u skipped: Vulkan below 1.3", i); continue; }
        const char *miss = missing_required(pd[i]);
        if (miss) { rt_log("vk: gpu %u skipped: missing extension %s", i, miss); continue; }
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, NULL);
        VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, qp);
        uint32_t fam = find_gfx_family(pd[i], qp, nq);
        free(qp);
        if (fam == UINT32_MAX) { rt_log("vk: gpu %u skipped: no graphics+compute+present queue family", i); continue; }
        int score = (int)i == want ? 100 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) { best_score = score; best = (int)i; best_driver = p.driverVersion; continue; }
        if (score == best_score && best >= 0) {
            VkPhysicalDeviceProperties b;
            vkGetPhysicalDeviceProperties(pd[best], &b);
            if (b.vendorID == p.vendorID && b.deviceID == p.deviceID && p.driverVersion > best_driver) {
                if (!listed) rt_log("vk: gpu %u is gpu %d listed by a newer driver (%#x over %#x): gpu %u", i, best,
                                    p.driverVersion, best_driver, i);
                best = (int)i, best_driver = p.driverVersion;
            }
        }
    }
    if (best < 0 && (want >= 0 || (name && *name))) {
        if (want >= 0) rt_log("vk: NX2SARIA_GPU_INDEX=%d is not usable (see above): the best usable gpu instead", want);
        else rt_log("vk: no usable gpu matches NX2SARIA_GPU_NAME=%s (see above): the best usable gpu instead", name);
        want = -1, name = NULL, listed = 1, how = GPUC_BY_FALLBACK;
        goto retry;
    }
    if (best < 0) {
        rt_log("vk: no device with Vulkan 1.3 and the required extensions (see above)");
        free(pd), free(cards), free(props);
        return 0;
    }
    int alt = -1;
    g_gpu_choice_note[0] = 0;
    if (cards[best].type != GPUC_DISCRETE && !usable_known) cards_usable(pd, cards, n), usable_known = 1;
    if (cards[best].type != GPUC_DISCRETE && want >= 0 && !(name && *name))
        alt = gpuc_choice_line(cards, (int)n, best, want, name, line, sizeof line, g_gpu_choice_note,
                               sizeof g_gpu_choice_note);
    gpuc_prepare_line(cards, (int)n, best, how, asked, name, g_prepare_text, sizeof g_prepare_text);
    g_vk.phys = pd[best];
    g_vk.device_index = best;
    free(pd), free(cards), free(props);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_vk.mem_props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq ? nq : 1, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.gfx_family = find_gfx_family(g_vk.phys, qp, nq);
    g_gfx_queues = qp[g_vk.gfx_family].queueCount;
    g_nqfam = nq < QFAM_MAX ? nq : QFAM_MAX;
    for (uint32_t i = 0; i < g_nqfam; i++) g_qfam_flags[i] = qp[i].queueFlags, g_qfam_count[i] = qp[i].queueCount;
    g_vk.xfer_family = g_vk.gfx_family;
    for (uint32_t i = 0; i < nq; i++)
        if ((qp[i].queueFlags & VK_QUEUE_TRANSFER_BIT) && !(qp[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            g_vk.xfer_family = i;
            break;
        }
    free(qp);
    rt_log("vk: using gpu %d: %s", best, g_vk.props.deviceName);
    if (alt >= 0) rt_log("%s", line);
    hw_detect_device_check(g_vk.props.vendorID, g_vk.props.deviceID, g_vk.props.driverVersion, g_vk.props.deviceName);
    vk_caps_driver(g_vk.phys);
    {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
        vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
        vk_mem_dmabuf_caps(has_ext(ep, ne, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                           has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME));
        free(ep);
    }
    vk_mem_class_log(&g_vk.props, &g_vk.mem_props);
    return 1;
}
static int vram_heap_limits(VkDeviceSize *limit) {
    const char *e = cfg_get("NX2SARIA_VRAM_HEAP_LIMIT");
    long mb = !e || !*e ? 0 : strcmp(e, "auto") ? atol(e) : -1;
    if (!mb) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                            .pNext = g_vk.has_memory_budget ? &b : NULL};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &p2);
    const VkPhysicalDeviceMemoryProperties *mp = &p2.memoryProperties;
    int nlocal = 0;
    uint32_t largest = 0;
    for (uint32_t h = 0; h < mp->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
        if (mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            if (!nlocal++ || mp->memoryHeaps[h].size > mp->memoryHeaps[largest].size) largest = h;
        }
    if (mb < 0 && (nlocal < 2 || !g_vk.has_memory_budget)) return 0;
    for (uint32_t h = 0; h < VK_MAX_MEMORY_HEAPS; h++) limit[h] = VK_WHOLE_SIZE;
    for (uint32_t h = 0; h < mp->memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
        if (!(mp->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        VkDeviceSize lim;
        if (mb > 0) {
            if (h != largest) continue;
            lim = (VkDeviceSize)mb << 20;
        } else {
            VkDeviceSize budget = b.heapBudget[h] && b.heapBudget[h] < mp->memoryHeaps[h].size ? b.heapBudget[h]
                                                                                                 : mp->memoryHeaps[h].size;
            VkDeviceSize margin = budget / 10 > (256ull << 20) ? budget / 10 : (256ull << 20);
            if (budget <= margin * 2) continue;
            lim = budget - margin;
        }
        if (lim >= mp->memoryHeaps[h].size) continue;
        limit[h] = lim;
        rt_log("vk: memory heap %u (%llu MiB, budget %llu MiB): the allocator uses at most %llu MiB, then the next heap "
               "(NX2SARIA_VRAM_HEAP_LIMIT)", h, (unsigned long long)(mp->memoryHeaps[h].size >> 20),
               (unsigned long long)(b.heapBudget[h] >> 20), (unsigned long long)(lim >> 20));
    }
    return 1;
}


typedef struct {
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids;
    VkPhysicalDeviceConditionalRenderingFeaturesEXT cond;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb;
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv;
    VkPhysicalDeviceRobustness2FeaturesEXT rob2;
    VkPhysicalDeviceFaultFeaturesEXT fault;
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT fsi;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
    VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT fbl;
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr;
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh;
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs;
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR mrc;
    VkPhysicalDeviceMaintenance5FeaturesKHR m5;
    VkPhysicalDevicePipelineBinaryFeaturesKHR pbin;
    VkPhysicalDevicePresentIdFeaturesKHR pid;
    VkPhysicalDeviceAntiLagFeaturesAMD antilag;
    VkPhysicalDeviceMemoryPriorityFeaturesEXT mprio;
    VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT pageable;
} FeatureChain;

static const char *g_mesh_path = "not decided yet";

static void fatal_note(void) {
    char mem[512];
    vk_mem_fatal_note(mem, sizeof mem);
    rt_log("fatal: gpu %s; %s; texture detail %s; mesh draws %s%s%s", vk_caps_summary(), mem, vk_texture_detail_name(),
           g_mesh_path, g_gpu_choice_note[0] ? "; " : "", g_gpu_choice_note);
}

static VkResult VKAPI_PTR timed_alloc(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *cb,
                                      VkDeviceMemory *m) {
    uint64_t t0 = perf_mark_begin();
    VkResult r = vkAllocateMemory(d, ai, cb, m);
    perf_mark_end(PERF_M_DEVMEM, t0, r == VK_SUCCESS ? ai->allocationSize : 0);
    return r;
}

static int create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
    const char *ext[64];
    uint32_t n_ext = 0;
    for (size_t i = 0; i < sizeof k_required_ext / sizeof *k_required_ext; i++) ext[n_ext++] = k_required_ext[i];

    FeatureChain *c = calloc(1, sizeof *c);
    void **tail = &c->f2.pNext;
#define CHAIN(member, stype) do { c->member.sType = stype; *tail = &c->member; tail = &c->member.pNext; } while (0)
    c->f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    CHAIN(f11, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    CHAIN(f12, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    CHAIN(f13, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    CHAIN(clip, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT);
#define OPTIONAL_EXT(flag, name, member, stype) do { \
        if (has_ext(ep, ne, name)) { g_vk.flag = 1; ext[n_ext++] = name; CHAIN(member, stype); } } while (0)
    OPTIONAL_EXT(has_custom_border_color, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, cbc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT);
    OPTIONAL_EXT(has_eds, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, eds, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);
    OPTIONAL_EXT(has_eds2, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, eds2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT);
    OPTIONAL_EXT(has_eds3, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
    OPTIONAL_EXT(has_vertex_input_dynamic_state, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, vids, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT);
    OPTIONAL_EXT(has_conditional_rendering, VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, cond, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT);
    OPTIONAL_EXT(has_transform_feedback, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, xfb, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT);
    OPTIONAL_EXT(has_provoking_vertex, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, pv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT);
    OPTIONAL_EXT(has_robustness2, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, rob2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
    OPTIONAL_EXT(has_device_fault, VK_EXT_DEVICE_FAULT_EXTENSION_NAME, fault, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT);
    if (cfg_bool("NX2SARIA_VRAM_PAGEABLE") && has_ext(ep, ne, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME) &&
        has_ext(ep, ne, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME)) {
        OPTIONAL_EXT(has_memory_priority, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, mprio,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT);
        OPTIONAL_EXT(has_pageable, VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME, pageable,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT);
    }
    OPTIONAL_EXT(has_fragment_shader_interlock, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, fsi,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT);
    OPTIONAL_EXT(has_mesh_shader, VK_EXT_MESH_SHADER_EXTENSION_NAME, mesh, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT);
    OPTIONAL_EXT(has_vrs, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, vrs, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR);
    OPTIONAL_EXT(has_maximal_reconvergence, VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME, mrc,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR);
    if (cfg_bool("NX2SARIA_PIPE_BINARIES") && has_ext(ep, ne, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) &&
        has_ext(ep, ne, VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
        g_vk.has_pipeline_binary = 1;
        ext[n_ext++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_PIPELINE_BINARY_EXTENSION_NAME;
        CHAIN(m5, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR);
        CHAIN(pbin, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR);
    }
    int has_fbl = 0, has_lr = 0;
    if (has_ext(ep, ne, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
        has_ext(ep, ne, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME)) {
        has_fbl = has_lr = 1;
        ext[n_ext++] = VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME;
        ext[n_ext++] = VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
        CHAIN(fbl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT);
        CHAIN(lr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR);
    }
    vk_dlaa_device_exts(ep, ne, ext, &n_ext, 40);
    if (has_ext(ep, ne, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME))
        ext[n_ext++] = VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME;
    if (vk_dlssg_wanted()) {
        static const char *const promoted[] = {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                                               VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
                                               VK_KHR_BIND_MEMORY_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                                               VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_1_EXTENSION_NAME,
                                               VK_KHR_MAINTENANCE_2_EXTENSION_NAME, VK_KHR_MAINTENANCE_3_EXTENSION_NAME};
        for (size_t i = 0; i < sizeof promoted / sizeof *promoted && n_ext < 56; i++)
            if (has_ext(ep, ne, promoted[i])) ext[n_ext++] = promoted[i];
    }
    if (has_ext(ep, ne, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) { g_vk.has_memory_budget = 1; ext[n_ext++] = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME; }
    if (has_ext(ep, ne, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) { g_vk.has_calibrated = 2; ext[n_ext++] = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME; }
    else if (has_ext(ep, ne, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) { g_vk.has_calibrated = 1; ext[n_ext++] = VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME; }
    if (vk_hdr_armed() && has_ext(ep, ne, VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
        g_vk.has_hdr_metadata = 1;
        ext[n_ext++] = VK_EXT_HDR_METADATA_EXTENSION_NAME;
    }
    if (has_ext(ep, ne, VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) { g_vk.has_buffer_marker = 1; ext[n_ext++] = VK_AMD_BUFFER_MARKER_EXTENSION_NAME; }
    if (vk_pass_marks_want_nv() && has_ext(ep, ne, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) {
        g_vk.has_nv_checkpoints = 1;
        ext[n_ext++] = VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME;
    }
    if (vk_mem_dmabuf_on() && n_ext + 4 <= sizeof ext / sizeof *ext) {
        ext[n_ext++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    }
    g_vk.lists_low_latency = 0;
    g_vk.low_latency2_rev = 0;
    if (!vk_dlssg_dpa() && n_ext + 2 <= sizeof ext / sizeof *ext) {
        int nv = has_ext(ep, ne, VK_NV_LOW_LATENCY_2_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_PRESENT_ID_EXTENSION_NAME);
        int amd = !nv && has_ext(ep, ne, VK_AMD_ANTI_LAG_EXTENSION_NAME);
        if (cfg_int("NX2SARIA_LOW_LATENCY") <= 0) {
            g_vk.lists_low_latency = nv || amd;
        } else if (nv) {
            g_vk.has_low_latency2 = 1;
            g_vk.low_latency2_rev = vk_lowlat_revision(ep, ne);
            ext[n_ext++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
            ext[n_ext++] = VK_NV_LOW_LATENCY_2_EXTENSION_NAME;
            CHAIN(pid, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR);
        } else if (amd) {
            g_vk.has_anti_lag = 1;
            ext[n_ext++] = VK_AMD_ANTI_LAG_EXTENSION_NAME;
            CHAIN(antilag, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD);
        }
    }
    void **gpl_link = NULL;
    if (has_ext(ep, ne, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(ep, ne, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)) {
        ext[n_ext++] = VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME;
        gpl_link = tail;
        CHAIN(gpl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);
    }
    free(ep);

    vkGetPhysicalDeviceFeatures2(g_vk.phys, &c->f2);
    vk_caps_features(&c->f2.features);
    if (gpl_link) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        if (!vk_caps_gpl(&c->gpl, &gp)) { *gpl_link = NULL; n_ext -= 2; }
    }
    const VkPhysicalDeviceFeatures *h = &c->f2.features;
    const char *miss = NULL;
    if (!c->f12.timelineSemaphore) miss = "timelineSemaphore";
    else if (!c->f13.dynamicRendering) miss = "dynamicRendering";
    else if (!c->f13.synchronization2) miss = "synchronization2";
    else if (!h->shaderStorageImageWriteWithoutFormat) miss = "shaderStorageImageWriteWithoutFormat";
    else if (!c->clip.depthClipControl) miss = "depthClipControl";
    else if (!h->shaderInt64) miss = "shaderInt64";
    else if (!h->samplerAnisotropy) miss = "samplerAnisotropy";
    else if (!h->independentBlend) miss = "independentBlend";
    else if (!h->geometryShader) miss = "geometryShader";
    else if (!h->textureCompressionBC) miss = "textureCompressionBC";
    if (miss) { rt_log("vk: required feature missing on %s: %s", g_vk.props.deviceName, miss); free(c); return 0; }
    if (!c->fault.deviceFault) g_vk.has_device_fault = 0;
    if (!c->mprio.memoryPriority) g_vk.has_memory_priority = 0;
    if (!c->pageable.pageableDeviceLocalMemory) g_vk.has_pageable = 0;
    rt_log("vk: pageable video memory with priorities %s (NX2SARIA_VRAM_PAGEABLE)", g_vk.has_pageable && g_vk.has_memory_priority ? "on" : "off");
    if (g_vk.has_vrs) {
        c->vrs.attachmentFragmentShadingRate = c->vrs.primitiveFragmentShadingRate = VK_FALSE;
        if (!c->vrs.pipelineFragmentShadingRate) g_vk.has_vrs = 0;
        rt_log("vk: fragment shading rate: %s", g_vk.has_vrs ? "per draw" : "not supported");
    }
    if (g_vk.has_mesh_shader) {
        c->mesh.multiviewMeshShader = c->mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
        const char *gp = cfg_get("NX2SARIA_GPU_PASSES"), *dmp = cfg_get("NX2SARIA_DUMP");
        int want = (gp && *gp && strcmp(gp, "0")) || (dmp && *dmp);
        g_vk.has_mesh_queries = want && c->mesh.meshShaderQueries && h->pipelineStatisticsQuery && c->f12.hostQueryReset;
        c->mesh.meshShaderQueries = g_vk.has_mesh_queries ? VK_TRUE : VK_FALSE;
        if (!c->mesh.meshShader || !c->mesh.taskShader) g_vk.has_mesh_shader = 0;
        rt_log("vk: mesh shaders: %s", g_vk.has_mesh_shader ? "VK_EXT_mesh_shader" : "the extension is there, its task or mesh feature is not");
    }
    {
        const char *mf = cfg_value("NX2SARIA_MESH_FALLBACK"), *me = cfg_value("NX2SARIA_MESH_EMULATION"), *why = "";
        unsigned radv_fixed = MESH_RADV_NO_FIX;
        const uint32_t dv = g_vk.props.driverVersion;
        int radv_bug = mesh_radv_task_regression(vk_caps.driver_id, dv, &radv_fixed);
        int path = mesh_path_decide(g_vk.has_mesh_shader, vk_caps.amd_sc, g_vk.props.vendorID, radv_bug, mf, me, &why);
        g_vk.mesh_fallback = path == MESH_PATH_FALLBACK;
        g_vk.mesh_fallback_default = mesh_path_fallback_default(path, g_vk.has_mesh_shader, vk_caps.amd_sc,
                                                                g_vk.props.vendorID, radv_bug, me);
        if (vk_caps.driver_id == MESH_DRIVER_RADV && g_vk.has_mesh_shader) {
            char until[64] = "";
            if (radv_bug && radv_fixed == MESH_RADV_NO_FIX)
                snprintf(until, sizeof until, "; no release of this series has the fix yet");
            else if (radv_bug)
                snprintf(until, sizeof until, "; this series has it from %u.%u.%u", dv >> 22, dv >> 12 & 0x3ff, radv_fixed);
            rt_log("vk: mesh path: RADV %u.%u.%u is %s the releases whose task shaders lose the device (Mesa 26.2 and 26.3 "
                   "before their fix%s): mesh_fallback = auto %s", dv >> 22, dv >> 12 & 0x3ff, dv & 0xfff,
                   radv_bug ? "inside" : "outside", until,
                   radv_bug ? "takes the mesh fallback" : "draws mesh programs natively");
            if (radv_bug && path == MESH_PATH_NATIVE)
                rt_log("vk: mesh path: WARNING: mesh_fallback = 0 draws mesh programs natively on this RADV release all "
                       "the same: the device can be lost within seconds");
        }
        g_vk.mesh_emulation = path == MESH_PATH_EMULATION;
        g_mesh_path = path == MESH_PATH_EMULATION ? "emulated (mesh_emulation)"
                    : path == MESH_PATH_FALLBACK ? (g_vk.has_mesh_shader ? "through the mesh fallback on a device with mesh "
                                                                           "shaders (compute passes)"
                                                                         : "through the mesh fallback (compute passes)")
                    : path == MESH_PATH_NATIVE ? "native (VK_EXT_mesh_shader)" : "skipped (no mesh shaders, mesh_fallback = 0)";
        if (path == MESH_PATH_EMULATION) rt_log("vk: mesh shaders: emulated with compute (mesh_emulation = 1); mesh_fallback is not used");
        else if (path == MESH_PATH_FALLBACK) {
            rt_log("vk: mesh fallback: task and mesh programs run as compute passes and are drawn from their buffers (%s)",
                   why);
        }
        else if (path == MESH_PATH_NONE)
            rt_log("vk: mesh shaders: not supported and mesh_fallback = 0 (task/mesh draws are skipped)");
        vk_exec_mfb_batch_start(g_vk.mesh_fallback, g_vk.mesh_fallback_default, g_mesh_path);
        mesh_pipeline_check_start(path == MESH_PATH_EMULATION);
    }
    if (g_vk.has_maximal_reconvergence && !c->mrc.shaderMaximalReconvergence) g_vk.has_maximal_reconvergence = 0;
    if (g_vk.has_pipeline_binary && !(c->pbin.pipelineBinaries && c->m5.maintenance5)) g_vk.has_pipeline_binary = 0;
    g_vk.has_present_id = g_vk.has_low_latency2 && c->pid.presentId;
    if (g_vk.has_anti_lag && !c->antilag.antiLag) g_vk.has_anti_lag = 0;
    g_vk.has_buffer_device_address = c->f12.bufferDeviceAddress;
    g_vk.has_astc_ldr = h->textureCompressionASTC_LDR;
    {
        const char *e = cfg_get("NX2SARIA_FEEDBACK");
        int layout_ok = has_fbl && has_lr && c->fbl.attachmentFeedbackLoopLayout && c->lr.dynamicRenderingLocalRead;
        g_vk.feedback = e && !strcmp(e, "0") ? VK_FEEDBACK_OFF
                      : layout_ok && !(e && !strcmp(e, "restart")) ? VK_FEEDBACK_LAYOUT : VK_FEEDBACK_RESTART;
        rt_log("vk: feedback loops: %s (attachmentFeedbackLoopLayout %d, dynamicRenderingLocalRead %d)",
               g_vk.feedback == VK_FEEDBACK_LAYOUT ? "feedback layout + by-region barrier"
               : g_vk.feedback == VK_FEEDBACK_RESTART ? "pass restart + full barrier" : "off (NX2SARIA_FEEDBACK=0)",
               has_fbl && c->fbl.attachmentFeedbackLoopLayout, has_lr && c->lr.dynamicRenderingLocalRead);
    }

    static const float prio[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t nq_gfx = vk_fg_queue_count() < g_gfx_queues ? vk_fg_queue_count() : g_gfx_queues;
    uint32_t used[QFAM_MAX] = {0};
    if (g_vk.gfx_family < QFAM_MAX) used[g_vk.gfx_family] = nq_gfx;
    if (g_vk.xfer_family != g_vk.gfx_family && g_vk.xfer_family < QFAM_MAX) used[g_vk.xfer_family] = 1;
    uint32_t fg_pf = UINT32_MAX, fg_pi = 0, fg_af = UINT32_MAX, fg_ai = 0;
    if (nq_gfx >= 2) {
        fg_pf = fg_af = g_vk.gfx_family, fg_pi = 1, fg_ai = nq_gfx - 1;
    } else {
        for (int pass = 0; pass < 2 && fg_pf == UINT32_MAX; pass++)
            for (uint32_t f = 0; f < g_nqfam && fg_pf == UINT32_MAX; f++) {
                if (f == g_vk.gfx_family || used[f] >= g_qfam_count[f]) continue;
                if (!(g_qfam_flags[f] & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) continue;
                if (!pass != !!(g_qfam_flags[f] & VK_QUEUE_COMPUTE_BIT)) continue;
                if (!gfx_window_vk_presentable(g_vk.instance, g_vk.phys, f)) continue;
                fg_pf = f, fg_pi = used[f]++;
            }
        if (fg_pf != UINT32_MAX) {
            if (used[fg_pf] < g_qfam_count[fg_pf] && used[fg_pf] < 4) fg_af = fg_pf, fg_ai = used[fg_pf]++;
            for (uint32_t f = 0; f < g_nqfam && fg_af == UINT32_MAX; f++)
                if (f != g_vk.gfx_family && used[f] < g_qfam_count[f] && used[f] < 4) fg_af = f, fg_ai = used[f]++;
            if (fg_af == UINT32_MAX) fg_af = fg_pf, fg_ai = fg_pi;
        }
    }
    VkDeviceQueueCreateInfo qi[QFAM_MAX];
    uint32_t nqi = 0;
    qi[nqi++] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                          .queueFamilyIndex = g_vk.gfx_family, .queueCount = nq_gfx, .pQueuePriorities = prio};
    for (uint32_t f = 0; f < g_nqfam; f++)
        if (f != g_vk.gfx_family && used[f])
            qi[nqi++] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                  .queueFamilyIndex = f, .queueCount = used[f], .pQueuePriorities = prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &c->f2, .queueCreateInfoCount = nqi,
                             .pQueueCreateInfos = qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VkResult r = vk_dlssg_create_device(g_vk.instance, g_vk.phys, &di, &g_vk.device);
    free(c);
    if (r != VK_SUCCESS) { rt_log("vk: vkCreateDevice failed (%d)", (int)r); return 0; }
    if (submit_thread_why && vk_caps.mesa) rt_log("vk: queue submissions: %s", submit_thread_why);
    vk_dlssg_device_ready(g_vk.phys, g_vk.device);
    if (vk_dlssg_dpa()) {
        vk_dpa = vk_dlssg_dpa();
        rt_log("vk: device entry points come through Streamline's interposer (DLSS Frame Generation)");
    }
    vkGetDeviceQueue(g_vk.device, g_vk.gfx_family, 0, &g_vk.gfx_queue);
    if (fg_pf != UINT32_MAX) {
        VkQueue present, acquire;
        vkGetDeviceQueue(g_vk.device, fg_pf, fg_pi, &present);
        vkGetDeviceQueue(g_vk.device, fg_af, fg_ai, &acquire);
        vk_fg_queues(present, fg_pf, acquire, fg_af);
        if (fg_pf != g_vk.gfx_family)
            rt_log("vk: frame generation queues: the graphics family %u has %u queue%s; presents on family %u queue %u, "
                   "image acquires on family %u queue %u", g_vk.gfx_family, g_gfx_queues, g_gfx_queues == 1 ? "" : "s",
                   fg_pf, fg_pi, fg_af, fg_ai);
    } else {
        rt_log("vk: frame generation queues: none (the graphics family %u has %u queue%s and no other family with a free "
               "queue can present): AMD FSR 3.1 frame generation cannot run", g_vk.gfx_family, g_gfx_queues,
               g_gfx_queues == 1 ? "" : "s");
    }
    if (g_vk.xfer_family != g_vk.gfx_family) vkGetDeviceQueue(g_vk.device, g_vk.xfer_family, 0, &g_vk.xfer_queue);
    else { g_vk.xfer_queue = g_vk.gfx_queue; g_vk.xfer_shared = 1; }

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &emh};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    g_vk.min_imported_host_alignment = (uint32_t)emh.minImportedHostPointerAlignment;
    vk_caps_init(&g_vk);
    session_summary_header("the Vulkan device");
    vk_mem_dmabuf_probe(g_vk.device, &g_vk.mem_props, vk_gpa_wanted_ctx(&g_vk));
    vk_mem_lazy_decide();
    rt_fatal_note_add(fatal_note);

    static const VmaVulkanFunctions vf = {.vkAllocateMemory = timed_alloc};
    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.device, .instance = g_vk.instance,
                                 .vulkanApiVersion = VK_API_VERSION_1_3,
                                 .flags = (g_vk.has_buffer_device_address ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0) |
                                          (g_vk.has_memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0) |
                                          (g_vk.has_memory_priority ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT : 0),
                                 .preferredLargeHeapBlockSize = vk_vma_block_size(),
                                 .pDeviceMemoryCallbacks = vk_headroom_callbacks(), .pVulkanFunctions = &vf};
    VkDeviceSize heap_limit[VK_MAX_MEMORY_HEAPS];
    if (vram_heap_limits(heap_limit)) ai.pHeapSizeLimit = heap_limit;
    if ((r = vmaCreateAllocator(&ai, &g_vk.vma)) != VK_SUCCESS) { rt_log("vk: vmaCreateAllocator failed (%d)", (int)r); return 0; }
    vk_mem_ballast(g_vk.vma);

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    if ((r = vkCreateSemaphore(g_vk.device, &si, NULL, &g_vk.timeline)) != VK_SUCCESS) {
        rt_log("vk: timeline semaphore creation failed (%d)", (int)r);
        return 0;
    }
    vk_crumbs_init(&g_vk);
    vk_lowlat_init(&g_vk);
    return 1;
}

static long g_errors_total;

enum { VMA_LEAK_LINES = 16 };
static uint32_t vma_leak_lines, vma_leak_found;

void vk_vma_leak_line(const char *fmt, ...) {
    if (++vma_leak_lines > VMA_LEAK_LINES) return;
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("vk: teardown: %s", line);
}

void vk_vma_leak_found(void) { vma_leak_found++; }

static void vma_destroy_logged(VmaAllocator vma) {
    VmaTotalStatistics st;
    vmaCalculateStatistics(vma, &st);
    uint32_t n = st.total.statistics.allocationCount;
    if (n)
        rt_log("vk: teardown: %u allocations (%llu bytes) were still in use when the memory allocator was destroyed: their "
               "memory goes with it, and the first %d are listed below", n,
               (unsigned long long)st.total.statistics.allocationBytes, VMA_LEAK_LINES);
    vma_leak_lines = vma_leak_found = 0;
    vmaDestroyAllocator(vma);
    if (n || vma_leak_found)
        rt_log("vk: teardown: the allocator found %u memory blocks or lists not empty and listed %u allocations", vma_leak_found,
               vma_leak_lines);
}

static void destroy_all(void) {
    gfx_window_release_vk();
    if (g_vk.device) vkDeviceWaitIdle(g_vk.device);
    if (g_vk.device) vk_dlaa_release();
    if (g_vk.device) vk_fsr3_release();
    if (g_vk.device) vk_astc_release();
    if (g_vk.device) vk_crumbs_release();
    if (g_vk.device) vk_lowlat_release();
    if (g_vk.timeline) vkDestroySemaphore(g_vk.device, g_vk.timeline, NULL);
    if (lost_fence) vkDestroyFence(g_vk.device, lost_fence, NULL);
    lost_fence = VK_NULL_HANDLE;
    vk_headroom_stop();
    if (g_vk.vma) vk_mem_ballast_release(g_vk.vma);
    if (g_vk.vma) vma_destroy_logged(g_vk.vma);
    if (g_vk.device) vkDestroyDevice(g_vk.device, NULL);
    if (g_vk.messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT f =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (f) f(g_vk.instance, g_vk.messenger, NULL);
    }
    if (g_vk.instance) vkDestroyInstance(g_vk.instance, NULL);
    g_errors_total += g_vk.validation_errors;
    memset(&g_vk, 0, sizeof g_vk);
}

long vk_validation_errors_total(void) { return g_errors_total + (g_ready ? g_vk.validation_errors : 0); }

int vk_init(void) {
    plat_mutex_lock(&g_lock);
    if (!g_ready) {
        memset(&g_vk, 0, sizeof g_vk);
        if (create_instance() && pick_device() && create_device())
            g_ready = 1, vk_headroom_start(), vk_dlaa_init(), mem_budget_vram(1);
        else destroy_all();
    }
    int ok = g_ready;
    plat_mutex_unlock(&g_lock);
    return ok;
}

void vk_shutdown(void) {
    plat_mutex_lock(&g_lock);
    if (g_ready) { mem_budget_vram(0); destroy_all(); g_ready = 0; }
    plat_mutex_unlock(&g_lock);
}

VkCtx *vk_ctx(void) { return g_ready ? &g_vk : NULL; }

static const char *fault_address_type(VkDeviceFaultAddressTypeEXT t) {
    switch (t) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execute invalid";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "instruction pointer (unknown)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "instruction pointer (invalid)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "instruction pointer (fault)";
    default: return "unknown type";
    }
}

static void device_fault_report(void) {
    vk_sync_lost_report();
    vk_crumbs_report();
    vk_pass_marks_report();
    vk_mem_lost_report();
    VkCtx *c = vk_ctx();
    PFN_vkGetDeviceFaultInfoEXT get =
        c && c->has_device_fault ? (PFN_vkGetDeviceFaultInfoEXT)vk_dpa(c->device, "vkGetDeviceFaultInfoEXT")
                                 : NULL;
    if (!get) {
        rt_log("vk: device lost; no fault report (VK_EXT_device_fault %s)", c && c->has_device_fault ? "has no entry point"
                                                                                                    : "not enabled");
        return;
    }
    VkDeviceFaultCountsEXT n = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult q = get(c->device, &n, NULL);
    VkDeviceFaultAddressInfoEXT *ai = calloc(n.addressInfoCount + 1, sizeof *ai);
    VkDeviceFaultVendorInfoEXT *vi = calloc(n.vendorInfoCount + 1, sizeof *vi);
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = ai, .pVendorInfos = vi};
    uint64_t binary = n.vendorBinarySize;
    n.vendorBinarySize = 0;
    if (q == VK_SUCCESS && ai && vi) q = get(c->device, &n, &info);
    if (q != VK_SUCCESS && q != VK_INCOMPLETE) {
        rt_log("vk: device lost; vkGetDeviceFaultInfoEXT failed (%d)", (int)q);
    } else {
        rt_log("vk: device fault: \"%s\" (%u address infos, %u vendor infos, vendor binary %llu bytes)", info.description,
               n.addressInfoCount, n.vendorInfoCount, (unsigned long long)binary);
        for (uint32_t i = 0; i < n.addressInfoCount; i++)
            rt_log("vk: device fault address %u: %s at %#llx, precision %#llx", i, fault_address_type(ai[i].addressType),
                   (unsigned long long)ai[i].reportedAddress, (unsigned long long)ai[i].addressPrecision);
        for (uint32_t i = 0; i < n.vendorInfoCount; i++)
            rt_log("vk: device fault vendor info %u: \"%s\" code %#llx data %#llx", i, vi[i].description,
                   (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
    }
    free(ai);
    free(vi);
}

#define LOST_EXIT_MS 10000
volatile int32_t vk_lost;

static void lost_wait_exit(void *arg) {
    int watchdog = arg != NULL;
    for (int t = 0; t < LOST_EXIT_MS + (watchdog ? 0 : 2000); t += 50) plat_sleep_ms(50);
    if (watchdog) {
        rt_log_ending();
        rt_log_sync();
        rt_log("vk: the end of the run did not finish in %d s; ending it", LOST_EXIT_MS / 1000);
    }
    plat_terminate(RT_LOST_EXIT);
}

void vk_device_lost_check(VkResult r, const char *where) {
    if (r != VK_ERROR_DEVICE_LOST) return;
    if (plat_xchg32(&vk_lost, 1)) lost_wait_exit(NULL);
    plat_thread_spawn(lost_wait_exit, (void *)1, "vk lost exit", 0);
    char at[16];
    log_stamp(at, sizeof at, 0);
    rt_log("vk: device lost at %s local time (seen at: %s)", at, where);
    device_fault_report();
    rt_fatal_code(RT_LOST_EXIT, "vk: the GPU device was lost (seen at: %s). The run ends here; the lines above name what "
                                "the GPU was doing", where);
}


void vk_device_lost_poll(void) {
    VkCtx *c = vk_ctx();
    if (!c || vk_device_is_lost()) return;
    VkFence f = __atomic_load_n(&lost_fence, __ATOMIC_ACQUIRE);
    if (!f) {
        plat_mutex_lock(&lost_fence_lock);
        if (!lost_fence) {
            VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence n = VK_NULL_HANDLE;
            VkResult r = vkCreateFence(c->device, &fi, NULL, &n);
            if (r == VK_SUCCESS) __atomic_store_n(&lost_fence, n, __ATOMIC_RELEASE);
            else vk_device_lost_check(r, "the fence that asks whether the device lives");
        }
        f = lost_fence;
        plat_mutex_unlock(&lost_fence_lock);
        if (!f) return;
    }
    vk_device_lost_check(vkGetFenceStatus(c->device, f), "a poll of the device");
}

int vk_selftest(void) {
    if (!vk_init()) return 1;
    VkCtx *c = &g_vk;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64 << 10,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT};
    VkBuffer buf;
    VmaAllocation alloc;
    VkResult r = vmaCreateBuffer(c->vma, &bi, &aci, &buf, &alloc, NULL);
    if (r != VK_SUCCESS) { rt_log("vk selftest: vmaCreateBuffer failed (%d)", (int)r); return 2; }
    vmaDestroyBuffer(c->vma, buf, alloc);

    VkSemaphoreSignalInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = c->timeline, .value = 1};
    vk_sync_signal_note(1);
    if ((r = vkSignalSemaphore(c->device, &sig)) != VK_SUCCESS) { rt_log("vk selftest: vkSignalSemaphore failed (%d)", (int)r); return 3; }
    uint64_t value = 1;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &c->timeline,
                              .pValues = &value};
    if ((r = vkWaitSemaphores(c->device, &wi, 1000000000ull)) != VK_SUCCESS) { rt_log("vk selftest: vkWaitSemaphores failed (%d)", (int)r); return 4; }
    uint64_t now = 0;
    vkGetSemaphoreCounterValue(c->device, c->timeline, &now);
    if (now != 1) { rt_log("vk selftest: timeline value %llu, expected 1", (unsigned long long)now); return 5; }
    return 0;
}

int probe_vkdevice(void) {
    int rc = vk_selftest();
    VkCtx *c = vk_ctx();
    if (rc || !c) { printf("probe-vkdevice: FAILED (selftest %d)\n", rc ? rc : 1); vk_shutdown(); return 1; }
    uint32_t v = c->props.apiVersion;
    printf("probe-vkdevice: device %d \"%s\" api %u.%u.%u, gfx queue family %u, transfer family %u%s, host import alignment %#x\n",
           c->device_index, c->props.deviceName, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v),
           c->gfx_family, c->xfer_family, c->xfer_shared ? " (shared queue)" : "", c->min_imported_host_alignment);
    printf("probe-vkdevice: optional: custom_border=%d eds=%d eds2=%d eds3=%d vertex_input=%d cond_render=%d xfb=%d provoking=%d "
           "robustness2=%d fault=%d budget=%d bda=%d interlock=%d buffer_marker=%d nv_checkpoints=%d validation=%d\n",
           c->has_custom_border_color, c->has_eds, c->has_eds2, c->has_eds3, c->has_vertex_input_dynamic_state,
           c->has_conditional_rendering, c->has_transform_feedback, c->has_provoking_vertex, c->has_robustness2,
           c->has_device_fault, c->has_memory_budget, c->has_buffer_device_address, c->has_fragment_shader_interlock,
           c->has_buffer_marker, c->has_nv_checkpoints, c->validation);
    fatal_note();
    vk_shutdown();
    printf("probe-vkdevice: %s (validation errors %ld)\n", g_errors_total ? "FAILED" : "passed", g_errors_total);
    return g_errors_total ? 1 : 0;
}

int vk_mesh_shader_native(void) { return g_vk.has_mesh_shader; }

