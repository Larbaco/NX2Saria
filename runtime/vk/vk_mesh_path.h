// How the game's task and mesh programs are drawn (vk_device.c create_device). Header only, so that
// tests/mesh_path_check.c checks the decision without a device.
//
//   native     VK_EXT_mesh_shader pipelines
//   fallback   the mesh fallback: the programs run as compute passes and an ordinary draw reads what they wrote
//              (vk_exec_draw.c "mesh fallback", vk_mesh_fb.c)
//   emulation  the mesh emulation (mesh_emulation = 1 on a device without mesh shaders, vk_exec_draw.c); never on an
//              NVIDIA card (below)
//   none       a device without mesh shaders with both off: mesh draws are skipped
//
// mesh_fallback = auto (the default) takes the fallback on a device without mesh shaders, and on AMD's Windows driver
// for RDNA 1 and 2 (vk_caps.amd_sc), which has them on RDNA 2. That driver lost the device ("No fault detected", a
// timeout) minutes into play on RX 6600 to RX 6950 XT cards in every version from 0.6.0 to 0.9.2 (0.10.x built its mesh
// pipelines without the driver's optimiser, NX2SARIA_VK_NOOPT, which no reporter has confirmed), with the pipeline
// library, the replay workers, the asynchronous replay, the lazy write-back and the low-end GPU mode each on and off.
// Where the pass marks named the pass the GPU was in, it was one that starts with a native mesh draw. The same driver
// on RDNA 1 has no mesh shaders and draws through this fallback, and AMD's driver for RDNA 3 and 4 (LLPC) draws them
// natively; neither has such a report. That the native mesh path is the cause is not shown: the fallback is what the
// marks leave to try, at the cost of a compute pass and a pass break per mesh draw, and of mesh programs that are
// left out of the picture until their compute passes are built, at their first sight in a run. mesh_fallback = 0
// draws them natively again; 1 takes the fallback on every device.
//
// auto takes the fallback on Intel's cards with mesh shaders too (vendor 0x8086, Arc and the integrated GPUs that
// have them), on every Intel driver. On an Arc B390 (integrated) and an Arc B580 with Intel's Windows driver
// 32.0.101.9150 the fog over Kokiri Forest's ground showed small holes the grass shows through (issue #13): the fog's
// fragment programs drop a pixel whose depth is past the depth its own depth pass wrote, and there the two differ.
// 0.18.2 pinned the task stage to 32 lanes and made the mesh programs' positions invariant; the player saw the holes
// with both, and they went only with mesh_fallback = 1. Neither change was a fix of a shader compiler's, so what is
// left is how the native mesh path writes and compares depth on this hardware, which Mesa's Intel driver (ANV) shares:
// there it is taken as a precaution, with no report from it either way. mesh_fallback = 0 draws them natively again.
//
// The mesh emulation is the older of the two ways for a card without mesh shaders, and the low-end GPU mode turned it
// on up to 0.10.1 (cfg.c lowend_set). On NVIDIA's cards without mesh shaders (GTX 10 and older) it ended the game
// inside the driver at the first mesh draws of an area: an access violation at nvoglv64.dll+0x10cdffe on a GTX 1050 Ti
// and on a GTX 1070, both on 0.10.1, and a Titan X (Pascal) report of the same mode. The GTX 1050 Ti ran on once
// mesh_emulation was set to 0, through the fallback, with the rest of that mode still on. So an NVIDIA card never
// takes the emulation: with it asked for, the card is drawn for as if it were not.
#pragma once
#include <string.h>

enum { MESH_PATH_NATIVE, MESH_PATH_FALLBACK, MESH_PATH_EMULATION, MESH_PATH_NONE };

#define MESH_VENDOR_NVIDIA 0x10de
#define MESH_VENDOR_INTEL 0x8086

// has_mesh: the device has VK_EXT_mesh_shader with task and mesh shaders; amd_sc: AMD's Windows driver for RDNA 1 and
// 2; vendor: the device's vendorID; fallback: the mesh_fallback setting ("auto", "0", "1"; anything else reads as
// auto); emulation: mesh_emulation ("0" off). *why: the reason, for the boot line.
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

// 1 when path is the fallback and mesh_fallback = auto would choose it on this device too (AMD's Windows driver for RDNA
// 1 and 2, Intel, a device without mesh shaders): the fallback is the device's default, whether or not mesh_fallback = 1
// also asks for it; 0 where mesh_fallback = 1 forces it on a device that draws mesh programs natively.
// NX2SARIA_MESH_FALLBACK_BATCH = auto batches where this is 1.
static inline int mesh_path_fallback_default(int path, int has_mesh, int amd_sc, unsigned vendor, int radv_task_bug,
                                             const char *emulation) {
    return path == MESH_PATH_FALLBACK &&
           mesh_path_decide(has_mesh, amd_sc, vendor, radv_task_bug, "auto", emulation, NULL) == MESH_PATH_FALLBACK;
}
