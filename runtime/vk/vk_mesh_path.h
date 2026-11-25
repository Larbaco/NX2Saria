#pragma once
#include <string.h>

enum { MESH_PATH_NATIVE, MESH_PATH_FALLBACK, MESH_PATH_EMULATION, MESH_PATH_NONE };

#define MESH_VENDOR_NVIDIA 0x10de
#define MESH_VENDOR_INTEL 0x8086

#define MESH_DRIVER_RADV 3
#define MESH_RADV_NO_FIX 0xffffffffu

typedef struct {
    unsigned major, minor, fixed;
} MeshRadvSeries;

static const MeshRadvSeries mesh_radv_task_series[] = {{26, 2, MESH_RADV_NO_FIX}, {26, 3, MESH_RADV_NO_FIX}};

static inline int mesh_radv_task_in(const MeshRadvSeries *series, int n, unsigned driver_id, unsigned version,
                                    unsigned *fixed) {
    if (driver_id != MESH_DRIVER_RADV) return 0;
    const unsigned major = version >> 22, minor = version >> 12 & 0x3ff, patch = version & 0xfff;
    for (int i = 0; i < n; i++) {
        if (series[i].major != major || series[i].minor != minor) continue;
        if (series[i].fixed != MESH_RADV_NO_FIX && patch >= series[i].fixed) return 0;
        if (fixed) *fixed = series[i].fixed;
        return 1;
    }
    return 0;
}

static inline int mesh_radv_task_regression(unsigned driver_id, unsigned version, unsigned *fixed) {
    return mesh_radv_task_in(mesh_radv_task_series, (int)(sizeof mesh_radv_task_series / sizeof *mesh_radv_task_series),
                             driver_id, version, fixed);
}

static inline int mesh_path_decide(int has_mesh, int amd_sc, unsigned vendor, int radv_task_bug, const char *fallback,
                                   const char *emulation, const char **why) {
    const int nvidia = vendor == MESH_VENDOR_NVIDIA, intel = vendor == MESH_VENDOR_INTEL;
    const char *w;
    int path;
    if (!has_mesh && strcmp(emulation, "0") && !nvidia)
        path = MESH_PATH_EMULATION, w = "mesh_emulation = 1 on a device without mesh shaders";
    else if (!strcmp(fallback, "1"))
        path = MESH_PATH_FALLBACK, w = "mesh_fallback = 1";
    else if (!strcmp(fallback, "0"))
        path = has_mesh ? MESH_PATH_NATIVE : MESH_PATH_NONE, w = "mesh_fallback = 0";
    else if (!has_mesh)
        path = MESH_PATH_FALLBACK, w = strcmp(emulation, "0") ? "this device has no mesh shaders; mesh_emulation is "
                                                                "not used on an NVIDIA card, where it ended the game "
                                                                "inside the driver"
                                                              : "this device has no mesh shaders";
    else if (amd_sc)
        path = MESH_PATH_FALLBACK, w = "the default on AMD's driver for RDNA 1 and 2, where native mesh pipelines lost "
                                       "the device in play; mesh_fallback = 0 draws them natively";
    else if (intel)
        path = MESH_PATH_FALLBACK, w = "the default on Intel's drivers, where native mesh pipelines left holes in the "
                                       "fog on Arc cards; mesh_fallback = 0 draws them natively";
    else if (radv_task_bug)
        path = MESH_PATH_FALLBACK, w = "the default on the RADV releases whose task shaders lose the device, Mesa 26.2.0 "
                                       "and later until a release with the fix; mesh_fallback = 0 draws them natively";
    else
        path = MESH_PATH_NATIVE, w = "VK_EXT_mesh_shader";
    if (why) *why = w;
    return path;
}

static inline int mesh_path_fallback_default(int path, int has_mesh, int amd_sc, unsigned vendor, int radv_task_bug,
                                             const char *emulation) {
    return path == MESH_PATH_FALLBACK &&
           mesh_path_decide(has_mesh, amd_sc, vendor, radv_task_bug, "auto", emulation, NULL) == MESH_PATH_FALLBACK;
}
