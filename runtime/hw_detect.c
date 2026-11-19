// The hardware the quality presets are chosen by (cfg.c cfg_start; the choice: preset_pick.h).
//
// Design. The preset is decided inside cfg_start, before any restart-only setting is read and long before the game
// makes its Vulkan device, so the GPU is read by a probe of its own: a Vulkan instance without surface extensions,
// which lists the devices, picks the one vk_device.c pick_device would pick (NX2SARIA_GPU_INDEX or NX2SARIA_GPU_NAME,
// else a discrete GPU before an integrated one before any other, a software renderer on the processor among the
// others; Vulkan 1.3 and the extensions the game needs; of one GPU listed by two drivers, the newer driver's listing), reads its name, IDs, driver version, type, largest
// device-local heap and mesh shader support, and is destroyed again. The window's present support cannot be asked
// without a window, so a GPU the game later refuses for that is the one case the probe and the game disagree on;
// vk_device.c says so in the log (hw_detect_device_check).
//
// The probe is the process's first Vulkan call, so it first puts the drivers' cache variables in the environment
// (vk_device.c vk_driver_cache_env, which a driver reads when it loads). It does not run where something else must
// make the first Vulkan call: DLSS Frame Generation on Windows (framegen = dlss: Streamline's interposer, vk_dlssg.c
// vk_dlssg_early) and a RenderDoc capture (rdoc); nor in the tool modes (--selftest, --prepare-shaders and the others,
// cfg_start's game 0), which have no use for an early driver load; nor with preset_probe = 0, nor after a start whose
// probe never returned (cfg.c preset_init: the state file's probing mark). cfg.c then goes by the last probe's result,
// which it keeps in its state file. cfg_start runs once memory.c mem_init has claimed the whole guest address space, so the
// drivers' libraries and allocations land outside it, as they do for the game's own device later. The probe's time, the drivers' first load included, goes to the log each start.
//
// The rest: logical processors (plat_cpu_count), physical cores (plat_cpu_cores), installed memory (plat_mem_info),
// and the handheld the firmware names (preset_pick.h preset_handheld; Linux's DMI tables, Windows' registry copy of the
// firmware's SMBIOS strings; also Steam's SteamDeck=1).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <vulkan/vulkan.h>

#include "cfg.h"
#include "host.h"
#include "hw_detect.h"
#include "platform/platform.h"
#include "vk_dlssg.h"

#ifdef _WIN32
#include <windows.h>
#endif

// The device extensions vk_device.c pick_device requires (its k_required_ext): a GPU without one is not the game's.
static const char *const required_ext[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};

static int ext_listed(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

// 1 when pd has every required extension; *mesh: VK_EXT_mesh_shader is listed.
static int usable_exts(VkPhysicalDevice pd, int *mesh) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
    if (!e) return 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int ok = 1;
    for (size_t i = 0; i < sizeof required_ext / sizeof *required_ext; i++) ok &= ext_listed(e, n, required_ext[i]);
    *mesh = ext_listed(e, n, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    free(e);
    return ok;
}

static void probe_gpu(PresetHw *hw, char *note, size_t note_cap) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "NX2Saria",
                             .pEngineName = "NX2Saria", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    VkResult r = vkCreateInstance(&ci, NULL, &inst);
    if (r != VK_SUCCESS) {
        snprintf(note, note_cap, "no Vulkan 1.3 instance (%d)", (int)r);
        return;
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice *pd = calloc(n ? n : 1, sizeof *pd);
    if (pd) vkEnumeratePhysicalDevices(inst, &n, pd);
    else n = 0;
    const char *e = cfg_get("NX2SARIA_GPU_INDEX");   // as vk_device.c pick_device: the best device when unset
    int want = e && *e ? atoi(e) : -1;
    const char *name = cfg_get("NX2SARIA_GPU_NAME");
    int best = -1, best_score = -1, best_mesh = 0;
    VkPhysicalDeviceProperties bp = {0};
retry:
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (want >= 0 && (int)i != want) continue;
        if (name && *name && !preset_has(p.deviceName, name)) continue;
        if (p.apiVersion < VK_API_VERSION_1_3) continue;   // a CPU device counts, ranked with the others (as pick_device)
        int mesh = 0;
        if (!usable_exts(pd[i], &mesh)) continue;
        int score = (int)i == want ? 100 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        // of one GPU listed by two drivers, the newer driver's listing (as pick_device)
        int newer = score == best_score && bp.vendorID == p.vendorID && bp.deviceID == p.deviceID &&
                    p.driverVersion > bp.driverVersion;
        if (score > best_score || newer) best = (int)i, best_score = score, bp = p, best_mesh = mesh;
    }
    if (best < 0 && (want >= 0 || (name && *name))) {   // a choice that is not usable: the best usable one, as the game does
        want = -1, name = NULL;
        goto retry;
    }
    if (best < 0) {
        snprintf(note, note_cap, n ? "no device with Vulkan 1.3 and the extensions the game needs" : "no Vulkan device");
    } else {
        hw->have_gpu = 1;
        snprintf(hw->gpu_name, sizeof hw->gpu_name, "%s", bp.deviceName);
        hw->vendor = bp.vendorID, hw->device = bp.deviceID, hw->driver = bp.driverVersion;
        hw->gpu_type = bp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? PRESET_GPU_DISCRETE
                     : bp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? PRESET_GPU_INTEGRATED
                     : bp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? PRESET_GPU_CPU : PRESET_GPU_OTHER;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd[best], &mp);
        VkDeviceSize largest = 0;
        for (uint32_t h = 0; h < mp.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++)
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[h].size > largest)
                largest = mp.memoryHeaps[h].size;
        hw->vram_mib = (uint32_t)(largest >> 20);
        if (best_mesh) {
            VkPhysicalDeviceMeshShaderFeaturesEXT ms = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &ms};
            vkGetPhysicalDeviceFeatures2(pd[best], &f2);
            hw->mesh = ms.meshShader && ms.taskShader;
        }
    }
    free(pd);
    vkDestroyInstance(inst, NULL);
}

// ---- the handheld the firmware names
#ifdef _WIN32
// One string of the firmware's SMBIOS tables as Windows keeps them (HKLM\HARDWARE\DESCRIPTION\System\BIOS); "" when absent.
static void bios_string(const char *value, char *out, DWORD cap) {
    DWORD n = cap;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", value, RRF_RT_REG_SZ, NULL, out, &n) !=
        ERROR_SUCCESS)
        out[0] = 0;
}
#else
static void dmi_string(const char *file, char *out, size_t cap) {
    char path[96];
    snprintf(path, sizeof path, "/sys/class/dmi/id/%s", file);
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (!fgets(out, (int)cap, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}
#endif

static const char *handheld(void) {
    const char *env = getenv("SteamDeck");   // Steam sets it on the Steam Deck
    if (env && !strcmp(env, "1")) return "Steam Deck";
    char vendor[128], product[128], family[128], board_vendor[128], board[128];
#ifdef _WIN32
    bios_string("SystemManufacturer", vendor, sizeof vendor);
    bios_string("SystemProductName", product, sizeof product);
    bios_string("SystemFamily", family, sizeof family);
    bios_string("BaseBoardManufacturer", board_vendor, sizeof board_vendor);
    bios_string("BaseBoardProduct", board, sizeof board);
#else
    dmi_string("sys_vendor", vendor, sizeof vendor);
    dmi_string("product_name", product, sizeof product);
    dmi_string("product_family", family, sizeof family);
    dmi_string("board_vendor", board_vendor, sizeof board_vendor);
    dmi_string("board_name", board, sizeof board);
#endif
    return preset_handheld(vendor, product, family, board_vendor, board);
}

void hw_detect(PresetHw *hw, const char *no_probe, void (*before_probe)(void), char *skipped, size_t skipped_cap,
               char *note, size_t note_cap, double *probe_ms) {
    memset(hw, 0, sizeof *hw);
    skipped[0] = note[0] = 0;
    *probe_ms = 0;
    hw->threads = (int)plat_cpu_count();
    uint64_t all, first;
    hw->cores = plat_cpu_cores(&all, &first);
    float used, total, proc;
    plat_mem_info(&used, &total, &proc);
    hw->ram_mib = total > 0 ? (uint32_t)total : 0;
    const char *hand = handheld();
    if (hand) snprintf(hw->handheld, sizeof hw->handheld, "%s", hand);
    if (no_probe && *no_probe) snprintf(skipped, skipped_cap, "%s", no_probe);
#ifdef _WIN32
    if (!skipped[0] && vk_dlssg_wanted())
        snprintf(skipped, skipped_cap, "DLSS Frame Generation (framegen = dlss) makes the process's first Vulkan call");
#endif
    const char *rdoc = cfg_get("NX2SARIA_RDOC");
    if (!skipped[0] && rdoc && *rdoc) snprintf(skipped, skipped_cap, "a RenderDoc capture (rdoc) is set up");
    if (skipped[0]) return;
    uint64_t t0 = plat_now_ns();
    vk_driver_cache_env();   // before the drivers first load: they read these variables then
    if (before_probe) before_probe();
    probe_gpu(hw, note, note_cap);
    *probe_ms = (double)(plat_now_ns() - t0) / 1e6;
#ifdef _WIN32
    preset_gpu_caps(hw, 1);
#else
    preset_gpu_caps(hw, 0);
#endif
}

void hw_detect_device_check(uint32_t vendor, uint32_t device, uint32_t driver, const char *name) {
    cfg_preset_device_started();   // the drivers load: a probing mark of an earlier start can go
    const PresetHw *hw = cfg_preset_hw();
    if (!hw || !hw->have_gpu) {
        rt_log("preset: the Vulkan device is \"%s\"; the preset had no GPU to go by", name);
        return;
    }
    if (hw->vendor != vendor || hw->device != device) {
        rt_log("preset: the Vulkan device differs from the probe: the game uses \"%s\" (vendor 0x%04x device 0x%04x), "
               "the preset was chosen for \"%s\" (vendor 0x%04x device 0x%04x)",
               name, vendor, device, hw->gpu_name, hw->vendor, hw->device);
        return;
    }
    if (hw->driver != driver)
        rt_log("preset: the Vulkan device matches the probe; its driver is 0x%x, the probe's result was from 0x%x", driver,
               hw->driver);
    else
        rt_log("preset: the Vulkan device matches the probe");
}
