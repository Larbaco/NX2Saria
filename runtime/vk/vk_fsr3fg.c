// AMD FSR 3.1 frame generation recorded by the runtime (vk_fsr3fg.h): the optical flow passes (runtime/shaders/
// fsr3of_*.comp) and the frame interpolation passes (fsr3fi_*.comp), over the headers in runtime/shaders/ffx3, built
// into runtime/fsr3of_*_spv.h and fsr3fi_*_spv.h by tools/build_shaders.py. No library is loaded: this is what makes
// NX2SARIA_FRAMEGEN = fsr3 run outside Windows, on any GPU the passes run on.
//
// One call (vk_fsr3fg_record), at the end of a present's command buffer, records in this order:
// - optical flow on the picture without the HUD (the current one against the one before): its luma, the luma pyramid,
//   the scene change detection (a histogram and its divergence from the frame before's), then from the coarsest of
//   seven levels down to the finest a block search, a filter and (but at the finest) the scale of the vectors into the
//   next level; the finest level's filter writes the vectors frame interpolation reads (8x8 blocks of the picture);
// - frame interpolation's preparation from the frame's depth and motion vectors: the dilated depth and vectors and the
//   previous frame's nearest depth reconstructed;
// - frame interpolation: setup (clears the vector fields and the disocclusion mask, counts frames since the last
//   reset), then, but on a reset, the interpolated frame's depth reconstructed, the game's vectors and the optical
//   flow's as fields at the interpolated frame, the game field's pyramid (to fill its holes) and the disocclusion mask;
//   the interpolation itself, the colour pyramid of its output, and the inpainting of the holes, which also puts the HUD
//   of the current picture back where the two pictures differ;
// - the copy of the picture without the HUD into the one the next frame interpolates from.
// A global compute barrier separates the passes. The interpolation is at the middle, so one generated frame per
// rendered frame (2x). After a reset the passes run as AMD's do, priming the histories, and no frame is shown.
//
// The context (the images) is made for the swapchain's size and format and for render sizes up to the larger of that
// size and the game's picture (widescreen.c wide_picture), as vk_fg.cpp makes AMD's; another size or format makes it
// again after a wait for the device. A context that cannot be made, or passes that cannot be made, turn frame
// generation off for the run (vk_fsr3fg_failed), with a line in the log; the frames then go out the plain way.
//
// The constants live in a host-visible ring of RING slots, one per call: a call's slot is read by the present it was
// recorded into, and gfx_present.c waits for the present FRAMES (2) back before it records, so a slot RING calls old is
// done. The same ring index picks the call's timestamp queries, read back when the slot comes round again.
#include "vk.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk_fg.h"
#include "vk_fg_stats.h"
#include "vk_fsr3fg.h"
#include "vk_rec.h"

#include "fsr3fi_disocclusion_mask_spv.h"
#include "fsr3fi_game_motion_vector_field_spv.h"
#include "fsr3fi_game_vector_field_inpainting_pyramid_spv.h"
#include "fsr3fi_inpainting_pyramid_spv.h"
#include "fsr3fi_inpainting_spv.h"
#include "fsr3fi_interpolate_spv.h"
#include "fsr3fi_optical_flow_vector_field_spv.h"
#include "fsr3fi_reconstruct_and_dilate_spv.h"
#include "fsr3fi_reconstruct_previous_depth_spv.h"
#include "fsr3fi_setup_spv.h"
#include "fsr3of_filter_spv.h"
#include "fsr3of_luminance_pyramid_spv.h"
#include "fsr3of_prepare_luma_spv.h"
#include "fsr3of_scale_spv.h"
#include "fsr3of_scd_divergence_spv.h"
#include "fsr3of_scd_histogram_spv.h"
#include "fsr3of_search_spv.h"

int vk_fsr3fg_build(void) {
#ifdef _WIN32
    return 0;
#else
    return 1;
#endif
}

// ---- the images of a context

#define OF_LEVELS 7        // the optical flow's pyramid levels
#define OF_BLOCK 8         // pixels per side of its blocks at the finest level
#define PYR_VIEWS 13       // the inpainting pyramid passes write mips 0-12 through views of their own
#define SCD_BINS (256 * 9) // the scene change histograms: 256 bins in each of 3 x 3 parts of the picture

enum { SZ_RENDER, SZ_DISPLAY, SZ_HALF, SZ_LUMA, SZ_FLOW, SZ_FIXED };
enum {
    I_DDEPTH, I_DMV, I_RPREV, I_RINTERP, I_GMX, I_GMY, I_OMX, I_OMY, I_DISOCC,   // render size (the largest)
    I_PREV_SRC,                                                                  // display size, the swapchain's format
    I_PYR,                                                                       // half the display size, every mip
    I_LUMA_A, I_LUMA_B = I_LUMA_A + OF_LEVELS,                                   // luma pyramids, two frames
    I_FLOW_A = I_LUMA_B + OF_LEVELS, I_FLOW_B = I_FLOW_A + OF_LEVELS,            // flow pyramids, ping-pong
    I_OFVEC = I_FLOW_B + OF_LEVELS,                                              // the finest flow, for interpolation
    I_SCD_OUT, I_SCD_HIST, I_SCD_PREV, I_SCD_TEMP, I_DIST, I_ZERO_U,
    I_N
};
typedef struct {
    VkFormat fmt;   // VK_FORMAT_UNDEFINED: the swapchain's
    uint8_t size;
    uint8_t level;   // SZ_LUMA, SZ_FLOW: the pyramid level
    uint16_t w;      // SZ_FIXED: the width (height 1)
} ImgDesc;
static ImgDesc img_desc(int i) {
    if (i >= I_LUMA_A && i < I_LUMA_A + 2 * OF_LEVELS)
        return (ImgDesc){VK_FORMAT_R8_UINT, SZ_LUMA, (uint8_t)((i - I_LUMA_A) % OF_LEVELS), 0};
    if (i >= I_FLOW_A && i < I_FLOW_A + 2 * OF_LEVELS)
        return (ImgDesc){VK_FORMAT_R16G16_SINT, SZ_FLOW, (uint8_t)((i - I_FLOW_A) % OF_LEVELS), 0};
    switch (i) {
    case I_DDEPTH: return (ImgDesc){VK_FORMAT_R32_SFLOAT, SZ_RENDER, 0, 0};
    case I_DMV: return (ImgDesc){VK_FORMAT_R16G16_SFLOAT, SZ_RENDER, 0, 0};
    case I_RPREV: case I_RINTERP: case I_GMX: case I_GMY: case I_OMX: case I_OMY:
        return (ImgDesc){VK_FORMAT_R32_UINT, SZ_RENDER, 0, 0};
    case I_DISOCC: return (ImgDesc){VK_FORMAT_R8G8_UNORM, SZ_RENDER, 0, 0};
    case I_PREV_SRC: return (ImgDesc){VK_FORMAT_UNDEFINED, SZ_DISPLAY, 0, 0};
    case I_PYR: return (ImgDesc){VK_FORMAT_R16G16B16A16_SFLOAT, SZ_HALF, 0, 0};
    case I_OFVEC: return (ImgDesc){VK_FORMAT_R16G16_SINT, SZ_FLOW, 0, 0};
    case I_SCD_OUT: return (ImgDesc){VK_FORMAT_R32_UINT, SZ_FIXED, 0, 3};
    case I_SCD_HIST: return (ImgDesc){VK_FORMAT_R32_UINT, SZ_FIXED, 0, SCD_BINS};
    case I_SCD_PREV: return (ImgDesc){VK_FORMAT_R32_SFLOAT, SZ_FIXED, 0, SCD_BINS};
    case I_SCD_TEMP: return (ImgDesc){VK_FORMAT_R32_UINT, SZ_FIXED, 0, 3};
    case I_DIST: return (ImgDesc){VK_FORMAT_R8G8_UNORM, SZ_FIXED, 0, 1};    // no distortion: a vector of 0
    default: return (ImgDesc){VK_FORMAT_R32_UINT, SZ_FIXED, 0, 1};          // I_ZERO_U: the confidence the passes ignore
    }
}

// ---- what the passes bind (the binding numbers of runtime/shaders/fsr3fi_*.comp and fsr3of_*.comp)

enum {
    // sampled images
    S_IN_DEPTH, S_IN_MV, S_DMV, S_DDEPTH, S_RPREV, S_RINTERP, S_PREV_SRC, S_CUR_SRC, S_PRESENT, S_DISOCC, S_GMX, S_GMY,
    S_OMX, S_OMY, S_OFVEC, S_OF_CONF, S_SCD, S_OUT, S_PYR, S_DIST, S_COLOR, S_LUMA, S_LUMA_PREV, S_FLOW, S_FLOW_PREV,
    // storage images
    U_FIRST, U_RPREV = U_FIRST, U_DMV, U_DDEPTH, U_RINTERP, U_GMX, U_GMY, U_OMX, U_OMY, U_DISOCC, U_OUT,
    U_PYR0, U_LUMA0 = U_PYR0 + PYR_VIEWS, U_FLOW = U_LUMA0 + OF_LEVELS, U_FLOW_NEXT, U_SCD_HIST, U_SCD_PREV,
    U_SCD_TEMP, U_SCD_OUT,
    // storage buffers (the counters)
    B_FIRST, B_CNT_RO = B_FIRST, B_CNT,
    // uniform buffers
    C_FIRST, C_FI = C_FIRST, C_PYR, C_OF, C_OFSPD,
    R_N
};
typedef struct {
    uint8_t binding, res;
} Bind;
#define B(...) ((const Bind[]){__VA_ARGS__}), (sizeof((const Bind[]){__VA_ARGS__}) / sizeof(Bind))
#define PYR_BINDS(first)                                                                                            \
    {first, U_PYR0}, {first + 1, U_PYR0 + 1}, {first + 2, U_PYR0 + 2}, {first + 3, U_PYR0 + 3},                     \
        {first + 4, U_PYR0 + 4}, {first + 5, U_PYR0 + 5}, {first + 6, U_PYR0 + 6}, {first + 7, U_PYR0 + 7},          \
        {first + 8, U_PYR0 + 8}, {first + 9, U_PYR0 + 9}, {first + 10, U_PYR0 + 10}, {first + 11, U_PYR0 + 11},      \
        {first + 12, U_PYR0 + 12}

enum {
    P_OF_LUMA, P_OF_LPYR, P_OF_HIST, P_OF_DIV, P_OF_SEARCH, P_OF_FILTER, P_OF_SCALE,
    P_FI_DILATE, P_FI_SETUP, P_FI_RPREV, P_FI_GAME, P_FI_GPYR, P_FI_OFFIELD, P_FI_DISOCC, P_FI_INTERP, P_FI_PYR,
    P_FI_INPAINT, P_N
};
static const struct {
    const char *name;
    const uint32_t *code;
    size_t size;
    const Bind *b;
    uint32_t n;
} PASS[P_N] = {
    [P_OF_LUMA] = {"optical flow luma", fsr3of_prepare_luma_spv, sizeof fsr3of_prepare_luma_spv,
                   B({0, S_COLOR}, {1, U_LUMA0}, {2, C_OF})},
    [P_OF_LPYR] = {"optical flow luma pyramid", fsr3of_luminance_pyramid_spv, sizeof fsr3of_luminance_pyramid_spv,
                   B({0, U_LUMA0}, {1, U_LUMA0 + 1}, {2, U_LUMA0 + 2}, {3, U_LUMA0 + 3}, {4, U_LUMA0 + 4},
                     {5, U_LUMA0 + 5}, {6, U_LUMA0 + 6}, {7, C_OF}, {8, C_OFSPD})},
    [P_OF_HIST] = {"scene change histogram", fsr3of_scd_histogram_spv, sizeof fsr3of_scd_histogram_spv,
                   B({0, S_LUMA}, {1, U_SCD_HIST}, {2, C_OF})},
    [P_OF_DIV] = {"scene change divergence", fsr3of_scd_divergence_spv, sizeof fsr3of_scd_divergence_spv,
                  B({0, U_SCD_HIST}, {1, U_SCD_PREV}, {2, U_SCD_TEMP}, {3, U_SCD_OUT}, {4, C_OF})},
    [P_OF_SEARCH] = {"optical flow search", fsr3of_search_spv, sizeof fsr3of_search_spv,
                     B({0, S_LUMA}, {1, S_LUMA_PREV}, {2, U_FLOW}, {3, U_SCD_OUT}, {4, C_OF})},
    [P_OF_FILTER] = {"optical flow filter", fsr3of_filter_spv, sizeof fsr3of_filter_spv,
                     B({0, S_FLOW_PREV}, {1, U_FLOW}, {2, C_OF})},
    [P_OF_SCALE] = {"optical flow scale", fsr3of_scale_spv, sizeof fsr3of_scale_spv,
                    B({0, S_LUMA}, {1, S_LUMA_PREV}, {2, S_FLOW}, {3, U_FLOW_NEXT}, {4, U_SCD_OUT}, {5, C_OF})},
    [P_FI_DILATE] = {"reconstruct and dilate", fsr3fi_reconstruct_and_dilate_spv,
                     sizeof fsr3fi_reconstruct_and_dilate_spv,
                     B({0, S_IN_MV}, {1, S_IN_DEPTH}, {2, U_RPREV}, {3, U_DMV}, {4, U_DDEPTH}, {5, C_FI})},
    [P_FI_SETUP] = {"setup", fsr3fi_setup_spv, sizeof fsr3fi_setup_spv,
                    B({0, S_SCD}, {1, U_GMX}, {2, U_GMY}, {3, U_OMX}, {4, U_OMY}, {5, U_DISOCC}, {6, B_CNT},
                      {7, C_FI})},
    [P_FI_RPREV] = {"reconstruct previous depth", fsr3fi_reconstruct_previous_depth_spv,
                    sizeof fsr3fi_reconstruct_previous_depth_spv,
                    B({0, S_DMV}, {1, S_DDEPTH}, {2, S_CUR_SRC}, {3, S_DIST}, {4, U_RINTERP}, {5, C_FI})},
    [P_FI_GAME] = {"game motion vector field", fsr3fi_game_motion_vector_field_spv,
                   sizeof fsr3fi_game_motion_vector_field_spv,
                   B({0, S_DMV}, {1, S_DDEPTH}, {2, S_PREV_SRC}, {3, S_CUR_SRC}, {4, S_DIST}, {5, U_GMX}, {6, U_GMY},
                     {7, C_FI})},
    [P_FI_GPYR] = {"game vector field pyramid", fsr3fi_game_vector_field_inpainting_pyramid_spv,
                   sizeof fsr3fi_game_vector_field_inpainting_pyramid_spv,
                   B({0, S_GMX}, {1, S_GMY}, {2, B_CNT}, PYR_BINDS(3), {16, C_FI}, {17, C_PYR})},
    [P_FI_OFFIELD] = {"optical flow vector field", fsr3fi_optical_flow_vector_field_spv,
                      sizeof fsr3fi_optical_flow_vector_field_spv,
                      B({0, S_OFVEC}, {1, S_OF_CONF}, {2, S_DDEPTH}, {3, S_PREV_SRC}, {4, S_CUR_SRC}, {6, U_OMX},
                        {7, U_OMY}, {8, C_FI})},
    [P_FI_DISOCC] = {"disocclusion mask", fsr3fi_disocclusion_mask_spv, sizeof fsr3fi_disocclusion_mask_spv,
                     B({0, S_GMX}, {1, S_GMY}, {2, S_RPREV}, {3, S_DDEPTH}, {4, S_RINTERP}, {5, S_PYR}, {6, S_DIST},
                       {7, U_DISOCC}, {8, C_FI})},
    [P_FI_INTERP] = {"interpolate", fsr3fi_interpolate_spv, sizeof fsr3fi_interpolate_spv,
                     B({0, S_GMX}, {1, S_GMY}, {2, S_OMX}, {3, S_OMY}, {4, S_PREV_SRC}, {5, S_CUR_SRC}, {6, S_DISOCC},
                       {7, S_PYR}, {8, B_CNT_RO}, {9, U_OUT}, {10, C_FI})},
    [P_FI_PYR] = {"inpainting pyramid", fsr3fi_inpainting_pyramid_spv, sizeof fsr3fi_inpainting_pyramid_spv,
                  B({0, S_OUT}, {1, B_CNT}, PYR_BINDS(2), {15, C_FI}, {16, C_PYR})},
    [P_FI_INPAINT] = {"inpainting", fsr3fi_inpainting_spv, sizeof fsr3fi_inpainting_spv,
                      B({0, S_SCD}, {1, S_PYR}, {2, S_PRESENT}, {3, S_CUR_SRC}, {7, U_OUT}, {8, C_FI})},
};
#undef B
#undef PYR_BINDS
#define MAX_BINDS 20

static VkDescriptorType desc_type(int r) {
    return r >= C_FIRST ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
         : r >= B_FIRST ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
         : r >= U_FIRST ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                        : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
}

// The constants of frame interpolation (std140 cbFI: 4-byte members, the vec4 and the vec3 at 16-byte boundaries).
typedef struct {
    int32_t render[2], display[2];
    float display_rcp[2], cam_near, cam_far;
    int32_t upscaler_target[2], mode, reset;
    float device_to_view[4];
    float delta_time;
    int32_t hudless_attached, distortion_size[2];
    float of_scale[2];
    int32_t of_block;
    uint32_t dispatch_flags;
    int32_t max_render[2], of_half_res, instances;
    int32_t rect_base[2], rect_size[2];
    float debug_bar[3];
    uint32_t transfer_function;
    float min_max_lum[2], tan_half_fov;
    int32_t pad;
    float jitter[2], mv_scale[2];
} FiConsts;
typedef struct {
    uint32_t mips, groups, offset[2];
} PyrConsts;
typedef struct {
    int32_t luma_res[2];
    uint32_t level, levels;
    uint32_t frame_index, transfer_function;
    float min_max_lum[2];
} OfConsts;
typedef struct {
    uint32_t mips, groups, offset[2], groups_pyramid;
} OfSpdConsts;
_Static_assert(sizeof(FiConsts) == 176, "cbFI is 176 bytes");
_Static_assert(__builtin_offsetof(FiConsts, device_to_view) == 48, "a vec4 at a 16-byte boundary in std140");
_Static_assert(__builtin_offsetof(FiConsts, debug_bar) == 128, "a vec3 at a 16-byte boundary in std140");
_Static_assert(sizeof(OfConsts) == 32, "cbOF is 32 bytes");

// The blocks of one ring slot: the interpolation's, the two pyramids', the optical flow's base and one per level, its
// SPD's.
enum { K_FI, K_PYR_RENDER, K_PYR_DISPLAY, K_OF, K_OF_LEVEL0, K_OFSPD = K_OF_LEVEL0 + OF_LEVELS, K_N };
#define RING 8
#define TS_PER 3   // timestamps per call: start, after the optical flow, end
// NX2SARIA_FSR3FG_PASS_TIMES=1 (diagnosis): a timestamp after every dispatch too, summed by pass (vk_fsr3fg_pass_ms)
#define PT_MAX 40

typedef struct {
    uint32_t w, h, mw, mh;   // the display size (the swapchain's) and the largest render size
    VkFormat fmt;            // the swapchain's format
    VkImage img[I_N];
    VmaAllocation mem[I_N];
    VkImageView view[I_N];
    VkImageView pyr_mip[PYR_VIEWS];
    uint32_t pyr_levels;
    VkBuffer cnt;            // the counters (two words)
    VmaAllocation cnt_mem;
    uint64_t vram;
    int fresh;               // never recorded into: transition and clear everything first
    int of_odd;              // which luma pyramid is the current frame's
    uint32_t of_frame;       // optical flow frames since its last reset
    int have_prev;           // I_PREV_SRC holds the last frame's picture
} Ctx;

static struct {
    volatile int32_t state;   // -1: not tried; 0: off; 1: ready
    volatile int32_t failed;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkSampler linear;
    VkDescriptorSetLayout dsl[P_N];
    VkPipelineLayout layout[P_N];
    VkPipeline pipe[P_N];
    VkBuffer ring;
    VmaAllocation ring_mem;
    uint8_t *ring_map;
    uint32_t block;   // one constant block (the uniform buffer offset alignment)
    uint32_t ring_next;
    VkQueryPool ts;
    int ts_on;
    uint8_t ts_written[RING];
    double ts_period_ns;
    Ctx ctx;
    int want_reset;
    VkFsr3fgStats st;
    double gpu_of_sum, gpu_fi_sum;
    uint64_t last_ns;
    volatile uint64_t made_ns;   // when the last generated frame was recorded (vk_fsr3fg_generating)
    VkQueryPool pt;                      // the per-dispatch timestamps (NX2SARIA_FSR3FG_PASS_TIMES)
    uint8_t pt_pass[RING][PT_MAX];       // the pass of each dispatch of a slot's call
    uint8_t pt_n[RING];                  // its dispatches (0: nothing to read)
    double pt_sum[P_N];
    uint64_t pt_count[P_N];
    // NX2SARIA_FSR3FG_DUMP=<dir> (diagnosis): every NX2SARIA_FSR3FG_DUMP_EVERY seconds (10) the picture before (without
    // the HUD), the generated frame and the picture after (with it) are copied into dump, and written as PPM images
    // once the slot comes round again (the GPU is done with it then); at most 20 dumps a run
    VkBuffer dump;
    VmaAllocation dump_mem;
    void *dump_map;
    uint32_t dump_w, dump_h, dump_slot;
    VkFormat dump_fmt;
    int dump_pending, dumps;
    uint64_t dump_next_ns;
} G = {.state = -1};

// ---- setup

static VkImageView make_view(VkImage img, VkFormat fmt, uint32_t base, uint32_t levels) {
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, levels, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(vk_ctx()->device, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

static int make_image(uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, VkImageUsageFlags usage, VkImage *img,
                      VmaAllocation *mem, uint64_t *bytes) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VmaAllocationInfo info;
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, img, mem, &info) != VK_SUCCESS) {
        *img = VK_NULL_HANDLE, *mem = NULL;
        return 0;
    }
    *bytes += info.size;
    return 1;
}

// What the device lacks for the passes, or NULL.
static const char *unsupported(const VkCtx *c) {
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(c->phys, &f);
    if (!f.shaderStorageImageExtendedFormats) return "no shaderStorageImageExtendedFormats";
    if (!f.shaderStorageImageReadWithoutFormat) return "no shaderStorageImageReadWithoutFormat";
    if (!f.shaderStorageImageWriteWithoutFormat) return "no shaderStorageImageWriteWithoutFormat";
    VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                       VK_SUBGROUP_FEATURE_QUAD_BIT;
    if (!(sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || (sg.supportedOperations & ops) != ops)
        return "no basic, arithmetic and quad subgroup operations in compute shaders";
    static const VkFormat storage[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_UINT,
                                       VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8_UINT,
                                       VK_FORMAT_R16G16_SINT};
    for (size_t i = 0; i < sizeof storage / sizeof storage[0]; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(c->phys, storage[i], &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) != need) return "a storage image format it needs is missing";
    }
    return NULL;
}

static int make_pass(const VkCtx *c, int p) {
    VkDescriptorSetLayoutBinding b[MAX_BINDS + 1];
    uint32_t n = 0;
    for (uint32_t i = 0; i < PASS[p].n; i++)
        b[n++] = (VkDescriptorSetLayoutBinding){.binding = PASS[p].b[i].binding, .descriptorCount = 1,
                                                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .descriptorType = desc_type(PASS[p].b[i].res)};
    // s_LinearClamp of the interpolation's headers, fixed in the layout: nothing to push for it
    b[n++] = (VkDescriptorSetLayoutBinding){.binding = 1000, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                                            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                            .pImmutableSamplers = &G.linear};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = n, .pBindings = b};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl[p]) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl[p]};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &G.layout[p]) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = PASS[p].size,
                                   .pCode = PASS[p].code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = G.layout[p]};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &G.pipe[p]);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) G.pipe[p] = VK_NULL_HANDLE;
    return r == VK_SUCCESS;
}

static void destroy_global(void) {
    const VkCtx *c = vk_ctx();
    if (!c) return;
    for (int p = 0; p < P_N; p++) {
        if (G.pipe[p]) vkDestroyPipeline(c->device, G.pipe[p], NULL);
        if (G.layout[p]) vkDestroyPipelineLayout(c->device, G.layout[p], NULL);
        if (G.dsl[p]) vkDestroyDescriptorSetLayout(c->device, G.dsl[p], NULL);
        G.pipe[p] = VK_NULL_HANDLE, G.layout[p] = VK_NULL_HANDLE, G.dsl[p] = VK_NULL_HANDLE;
    }
    if (G.linear) vkDestroySampler(c->device, G.linear, NULL);
    if (G.ring) vmaDestroyBuffer(c->vma, G.ring, G.ring_mem);
    if (G.ts) vkDestroyQueryPool(c->device, G.ts, NULL);
    if (G.pt) vkDestroyQueryPool(c->device, G.pt, NULL);
    G.pt = VK_NULL_HANDLE;
    if (G.dump) vmaDestroyBuffer(c->vma, G.dump, G.dump_mem);
    G.dump = VK_NULL_HANDLE, G.dump_map = NULL, G.dump_pending = 0;
    G.linear = VK_NULL_HANDLE, G.ring = VK_NULL_HANDLE, G.ring_map = NULL, G.ts = VK_NULL_HANDLE, G.ts_on = 0;
}

static int make_global(const VkCtx *c) {
    uint32_t align = (uint32_t)c->props.limits.minUniformBufferOffsetAlignment;
    if (align < 256) align = 256;   // the largest block (cbFI, 176 bytes) fits
    G.block = align;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)G.block * K_N * RING,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &G.ring, &G.ring_mem, &info) != VK_SUCCESS) {
        G.ring = VK_NULL_HANDLE;
        return 0;
    }
    G.ring_map = info.pMappedData;
    // GPU timestamps, where the graphics queue has them
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    if (c->gfx_family < nq && qp[c->gfx_family].timestampValidBits && c->props.limits.timestampPeriod > 0) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = RING * TS_PER};
        if (vkCreateQueryPool(c->device, &qi, NULL, &G.ts) == VK_SUCCESS) G.ts_on = 1;
        G.ts_period_ns = c->props.limits.timestampPeriod;
    }
    memset(G.ts_written, 0, sizeof G.ts_written);
    if (G.ts_on && cfg_get("NX2SARIA_FSR3FG_PASS_TIMES") && !strcmp(cfg_get("NX2SARIA_FSR3FG_PASS_TIMES"), "1")) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = RING * (PT_MAX + 1)};
        if (vkCreateQueryPool(c->device, &qi, NULL, &G.pt) != VK_SUCCESS) G.pt = VK_NULL_HANDLE;
    }
    memset(G.pt_n, 0, sizeof G.pt_n);
    G.ring_next = 0;
    return 1;
}

int vk_fsr3fg_init(void) {
    int s = __atomic_load_n(&G.state, __ATOMIC_ACQUIRE);
    if (s >= 0) return s;
    const VkCtx *c = vk_ctx();
    if (!c || !vk_fsr3fg_build()) return 0;
    const char *why = unsupported(c);
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    if (!why && !G.push) why = "no vkCmdPushDescriptorSetKHR";
    if (!why) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                                  .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE};
        if (vkCreateSampler(c->device, &si, NULL, &G.linear) != VK_SUCCESS) why = "sampler creation failed";
    }
    uint64_t t0 = plat_now_ns();
    for (int p = 0; !why && p < P_N; p++)
        if (!make_pass(c, p)) why = PASS[p].name;
    if (!why && !make_global(c)) why = "its constant buffer could not be made";
    if (why) {
        rt_log("fsr3fg: AMD FSR 3.1 frame generation (the runtime's passes) cannot run here: %s", why);
        destroy_global();
        __atomic_store_n(&G.state, 0, __ATOMIC_RELEASE);
        return 0;
    }
    rt_log("fsr3fg: AMD FSR 3.1 frame generation with the runtime's passes, no AMD library (%d pipelines in %.0f ms, "
           "GPU timestamps %s)", P_N, (plat_now_ns() - t0) / 1e6, G.ts_on ? "on" : "not on this queue");
    __atomic_store_n(&G.state, 1, __ATOMIC_RELEASE);
    return 1;
}

int vk_fsr3fg_available(void) {
    return vk_fsr3fg_build() && __atomic_load_n(&G.state, __ATOMIC_ACQUIRE) != 0 &&
           !__atomic_load_n(&G.failed, __ATOMIC_RELAXED);
}

int vk_fsr3fg_failed(void) { return __atomic_load_n(&G.failed, __ATOMIC_RELAXED) != 0; }

int vk_fsr3fg_on(void) { return vk_fsr3fg_available() && !strcmp(cfg_value("NX2SARIA_FRAMEGEN"), "fsr3"); }

void vk_fsr3fg_reset(void) { G.want_reset = 1; }

int vk_fsr3fg_generating(void) {
    uint64_t t = __atomic_load_n(&G.made_ns, __ATOMIC_RELAXED);
    return t && plat_now_ns() - t < 250000000ull;
}

static void fail(const char *what) {
    rt_log("fsr3fg: %s: frame generation off for this run, the frames go out without it", what);
    __atomic_store_n(&G.failed, 1, __ATOMIC_RELAXED);
}

// ---- the context

static void free_ctx(Ctx *x) {
    const VkCtx *c = vk_ctx();
    for (int i = 0; i < PYR_VIEWS; i++)
        if (x->pyr_mip[i]) vkDestroyImageView(c->device, x->pyr_mip[i], NULL);
    for (int i = 0; i < I_N; i++) {
        if (x->view[i]) vkDestroyImageView(c->device, x->view[i], NULL);
        if (x->img[i]) vmaDestroyImage(c->vma, x->img[i], x->mem[i]);
    }
    if (x->cnt) vmaDestroyBuffer(c->vma, x->cnt, x->cnt_mem);
    memset(x, 0, sizeof *x);
}

static uint32_t at_least_1(uint32_t v) { return v ? v : 1; }

static void flow_size(uint32_t w, uint32_t h, int level, uint32_t *fw, uint32_t *fh) {
    *fw = (w + OF_BLOCK - 1) / OF_BLOCK, *fh = (h + OF_BLOCK - 1) / OF_BLOCK;
    for (int i = 0; i < level; i++) *fw = (*fw + 1) / 2, *fh = (*fh + 1) / 2;
}

static int make_ctx(Ctx *x, uint32_t w, uint32_t h, VkFormat fmt, uint32_t mw, uint32_t mh) {
    memset(x, 0, sizeof *x);
    x->w = w, x->h = h, x->fmt = fmt, x->mw = mw, x->mh = mh;
    uint32_t hw = at_least_1(w / 2), hh = at_least_1(h / 2), big = hw > hh ? hw : hh;
    x->pyr_levels = 1;
    while (big >>= 1) x->pyr_levels++;
    const VkImageUsageFlags su = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int i = 0; i < I_N; i++) {
        ImgDesc d = img_desc(i);
        uint32_t iw = 1, ih = 1, levels = i == I_PYR ? x->pyr_levels : 1;
        VkFormat f = d.fmt ? d.fmt : fmt;
        VkImageUsageFlags u = i == I_PREV_SRC ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT : su;
        switch (d.size) {
        case SZ_RENDER: iw = mw, ih = mh; break;
        case SZ_DISPLAY: iw = w, ih = h; break;
        case SZ_HALF: iw = hw, ih = hh; break;
        case SZ_LUMA: iw = at_least_1(w >> d.level), ih = at_least_1(h >> d.level); break;
        case SZ_FLOW: flow_size(w, h, d.level, &iw, &ih); break;
        case SZ_FIXED: iw = d.w, ih = 1; break;
        }
        if (!make_image(iw, ih, levels, f, u, &x->img[i], &x->mem[i], &x->vram) ||
            !(x->view[i] = make_view(x->img[i], f, 0, levels))) {
            free_ctx(x);   // half made: no submission names it
            return 0;
        }
    }
    for (uint32_t m = 0; m < PYR_VIEWS; m++) {
        uint32_t level = m < x->pyr_levels ? m : x->pyr_levels - 1;   // the passes name more mips than a small one has
        if (!(x->pyr_mip[m] = make_view(x->img[I_PYR], VK_FORMAT_R16G16B16A16_SFLOAT, level, 1))) {
            free_ctx(x);
            return 0;
        }
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &x->cnt, &x->cnt_mem, NULL) != VK_SUCCESS) {
        x->cnt = VK_NULL_HANDLE;
        free_ctx(x);
        return 0;
    }
    x->fresh = 1;
    return 1;
}

static void idle_device(void) {
    vk_queue_lock();
    vkDeviceWaitIdle(vk_ctx()->device);
    vk_queue_unlock();
}

// The context for this display size and format, with render sizes up to the larger of rw x rh and the picture's.
static Ctx *ctx_for(uint32_t w, uint32_t h, VkFormat fmt, uint32_t rw, uint32_t rh) {
    Ctx *x = &G.ctx;
    if (x->img[0] && x->w == w && x->h == h && x->fmt == fmt && rw <= x->mw && rh <= x->mh) return x;
    if (x->img[0]) {   // another size: what a present in flight records into goes first
        idle_device();
        free_ctx(x);
    }
    unsigned pw = 0, ph = 0;
    wide_picture(&pw, &ph);
    uint32_t mw = rw > pw ? rw : pw, mh = rh > ph ? rh : ph;
    if (mw < w) mw = w;
    if (mh < h) mh = h;
    // NX2SARIA_FSR3FG_TEST_FAIL=1 (test, live): the images are not made, as when video memory runs out
    const char *tf = cfg_get("NX2SARIA_FSR3FG_TEST_FAIL");
    const int test_fail = tf && !strcmp(tf, "1");
    if (test_fail || !make_ctx(x, w, h, fmt, mw, mh)) {
        // another try after the GPU is idle (images it still held may have gone), then off
        idle_device();
        if (test_fail || !make_ctx(x, w, h, fmt, mw, mh)) {
            char what[160];
            snprintf(what, sizeof what, "its images for %ux%u (render sizes up to %ux%u) could not be made", w, h, mw, mh);
            fail(what);
            return NULL;
        }
    }
    G.st.vram = x->vram;
    rt_log("fsr3fg: context for %ux%u (format %d), render sizes up to %ux%u, %u pyramid mips: %.1f MiB of video memory",
           w, h, (int)fmt, mw, mh, x->pyr_levels, x->vram / 1048576.0);
    return x;
}

void vk_fsr3fg_idle(void) {
    if (!G.ctx.img[0]) return;
    idle_device();
    rt_log("fsr3fg: frame generation off: its images (%.1f MiB) are freed", G.ctx.vram / 1048576.0);
    free_ctx(&G.ctx);
    G.st.vram = 0;
}

void vk_fsr3fg_release(void) {
    if (!vk_ctx()) return;
    free_ctx(&G.ctx);
    destroy_global();
    G.st.vram = 0;
    __atomic_store_n(&G.state, -1, __ATOMIC_RELEASE);   // a new device makes everything again
}

void vk_fsr3fg_stats(VkFsr3fgStats *s) {
    *s = G.st;
    if (G.st.gpu_samples) s->gpu_ms_of = G.gpu_of_sum / G.st.gpu_samples, s->gpu_ms_fi = G.gpu_fi_sum / G.st.gpu_samples;
}

// ---- recording

static const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
static const VkAccessFlags2 CS_RW = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                          .dstStageMask = ds, .dstAccessMask = da};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m};
    vkCmdPipelineBarrier2(cb, &d);
}

static void clear_u(VkCommandBuffer cb, VkImage img, uint32_t v) {
    VkClearColorValue c = {.uint32 = {v, v, v, v}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &all);
}

static void to_general(VkCommandBuffer cb, const VkImage *img, int n) {
    VkImageMemoryBarrier2 b[I_N];
    for (int i = 0; i < n; i++)
        b[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = XF | CS,
                                       .dstStageMask = XF | CS,
                                       .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | CS_RW,
                                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img[i],
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = (uint32_t)n,
                          .pImageMemoryBarriers = b};
    vkCmdPipelineBarrier2(cb, &d);
}

// SPD's work groups (64 x 64 pixels each) and mips over w x h, as AMD's ffxSpdSetup gives them (mips < 0: from the size)
static void spd_setup(uint32_t w, uint32_t h, int mips, uint32_t groups[2], uint32_t *count, uint32_t *nmips) {
    groups[0] = (w - 1) / 64 + 1, groups[1] = (h - 1) / 64 + 1;
    *count = groups[0] * groups[1];
    if (mips >= 0) {
        *nmips = (uint32_t)mips;
    } else {
        uint32_t big = w > h ? w : h, m = 0;
        while (big >>= 1) m++;   // floor(log2(max(w, h)))
        *nmips = m > 12 ? 12 : m;
    }
}

typedef struct {
    VkCommandBuffer cb;
    uint32_t slot;
    VkImageView v[U_FIRST + (B_FIRST - U_FIRST)];   // the image of each slot this dispatch
    VkBuffer cnt;
    VkDeviceSize base;   // this call's slot of the ring
    int k_of;            // which optical flow block C_OF names (K_OF or a level's)
    int k_pyr;           // which pyramid block C_PYR names
    int n;               // dispatches so far
} Rec;

static void dispatch(Rec *r, int p, uint32_t gx, uint32_t gy, uint32_t gz) {
    VkWriteDescriptorSet wr[MAX_BINDS];
    VkDescriptorImageInfo ii[MAX_BINDS];
    VkDescriptorBufferInfo bi[MAX_BINDS];
    for (uint32_t j = 0; j < PASS[p].n; j++) {
        int s = PASS[p].b[j].res;
        wr[j] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstBinding = PASS[p].b[j].binding, .descriptorCount = 1,
                                       .descriptorType = desc_type(s)};
        if (s >= C_FIRST) {
            int k = s == C_FI ? K_FI : s == C_PYR ? r->k_pyr : s == C_OF ? r->k_of : K_OFSPD;
            bi[j] = (VkDescriptorBufferInfo){G.ring, r->base + (VkDeviceSize)k * G.block,
                                             s == C_FI ? sizeof(FiConsts) : s == C_OF ? sizeof(OfConsts)
                                             : s == C_PYR ? sizeof(PyrConsts) : 32};
            wr[j].pBufferInfo = &bi[j];
        } else if (s >= B_FIRST) {
            bi[j] = (VkDescriptorBufferInfo){r->cnt, 0, 16};
            wr[j].pBufferInfo = &bi[j];
        } else {
            ii[j] = (VkDescriptorImageInfo){VK_NULL_HANDLE, r->v[s], VK_IMAGE_LAYOUT_GENERAL};
            wr[j].pImageInfo = &ii[j];
        }
    }
    if (r->n++) barrier(r->cb, CS, VK_ACCESS_2_SHADER_WRITE_BIT, CS, CS_RW);
    vkCmdBindPipeline(r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe[p]);
    rec_push(G.push, r->cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout[p], 0, PASS[p].n, wr);
    vkCmdDispatch(r->cb, gx ? gx : 1, gy ? gy : 1, gz ? gz : 1);
    if (G.pt && G.pt_n[r->slot] < PT_MAX) {
        G.pt_pass[r->slot][G.pt_n[r->slot]] = (uint8_t)p;
        vkCmdWriteTimestamp2(r->cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, G.pt,
                             r->slot * (PT_MAX + 1) + ++G.pt_n[r->slot]);
    }
}

// The per-dispatch timestamps of the call RING calls ago in this slot (NX2SARIA_FSR3FG_PASS_TIMES)
static void pt_collect(uint32_t slot) {
    uint32_t n = G.pt_n[slot];
    G.pt_n[slot] = 0;
    if (!G.pt || !n) return;
    uint64_t t[2 * (PT_MAX + 1)];
    if (vkGetQueryPoolResults(vk_ctx()->device, G.pt, slot * (PT_MAX + 1), n + 1, sizeof t, t, 2 * sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS)
        return;
    for (uint32_t i = 1; i <= n; i++) {
        if (!t[2 * i + 1] || !t[2 * i - 1] || t[2 * i] < t[2 * i - 2]) return;
        int p = G.pt_pass[slot][i - 1];
        G.pt_sum[p] += (double)(t[2 * i] - t[2 * i - 2]) * G.ts_period_ns / 1e6, G.pt_count[p]++;
    }
}

// Writes the dump the slot holds, if any (NX2SARIA_FSR3FG_DUMP): its three pictures as PPM images.
static void dump_collect(uint32_t slot) {
    if (!G.dump_pending || G.dump_slot != slot) return;
    G.dump_pending = 0;
    const char *dir = cfg_get("NX2SARIA_FSR3FG_DUMP");
    const uint32_t w = G.dump_w, h = G.dump_h;
    const int bgra = G.dump_fmt == VK_FORMAT_B8G8R8A8_UNORM || G.dump_fmt == VK_FORMAT_B8G8R8A8_SRGB;
    vmaInvalidateAllocation(vk_ctx()->vma, G.dump_mem, 0, VK_WHOLE_SIZE);
    static const char *const what[3] = {"before", "generated", "after"};
    for (int k = 0; k < 3 && dir; k++) {
        char path[1100];
        snprintf(path, sizeof path, "%s/fsr3fg_%02d_%s.ppm", dir, G.dumps, what[k]);
        FILE *fp = fopen(path, "wb");
        if (!fp) continue;
        fprintf(fp, "P6\n%u %u\n255\n", w, h);
        const uint8_t *px = (const uint8_t *)G.dump_map + (size_t)k * w * h * 4;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            uint8_t rgb[3] = {px[4 * i + (bgra ? 2 : 0)], px[4 * i + 1], px[4 * i + (bgra ? 0 : 2)]};
            fwrite(rgb, 1, 3, fp);
        }
        fclose(fp);
    }
    rt_log("fsr3fg: dump %d written to %s (the picture before, the generated frame, the picture after)", G.dumps, dir);
    G.dumps++;
}

// Records this call's dump copies when one is due (NX2SARIA_FSR3FG_DUMP, 8-bit pictures only).
static void dump_record(VkCommandBuffer cb, uint32_t slot, const Ctx *x, const VkNgxImage *bb, const VkNgxImage *out) {
    const char *dir = cfg_get("NX2SARIA_FSR3FG_DUMP");
    if (!dir || G.dump_pending || G.dumps >= 20) return;
    if (bb->fmt != VK_FORMAT_B8G8R8A8_UNORM && bb->fmt != VK_FORMAT_R8G8B8A8_UNORM) return;
    uint64_t now = plat_now_ns();
    if (now < G.dump_next_ns) return;
    const char *e = cfg_get("NX2SARIA_FSR3FG_DUMP_EVERY");
    double every = e ? atof(e) : 10.0;
    G.dump_next_ns = now + (uint64_t)((every > 0.5 ? every : 0.5) * 1e9);
    const VkDeviceSize one = (VkDeviceSize)bb->w * bb->h * 4;
    if (G.dump && (G.dump_w != bb->w || G.dump_h != bb->h)) vmaDestroyBuffer(vk_ctx()->vma, G.dump, G.dump_mem), G.dump = 0;
    if (!G.dump) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = one * 3,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                      .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                               VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &G.dump, &G.dump_mem, &info) != VK_SUCCESS) {
            G.dump = VK_NULL_HANDLE;
            return;
        }
        G.dump_map = info.pMappedData;
    }
    G.dump_w = bb->w, G.dump_h = bb->h, G.dump_fmt = bb->fmt, G.dump_slot = slot, G.dump_pending = 1;
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {bb->w, bb->h, 1}};
    VkImage src[3] = {x->img[I_PREV_SRC], out->img, bb->img};
    for (int k = 0; k < 3; k++) {
        r.bufferOffset = one * (VkDeviceSize)k;
        vkCmdCopyImageToBuffer(cb, src[k], VK_IMAGE_LAYOUT_GENERAL, G.dump, 1, &r);
    }
}

int vk_fsr3fg_pass_ms(int i, const char **name, double *ms, uint64_t *count) {
    if (i < 0 || i >= P_N) return 0;
    *name = PASS[i].name, *count = G.pt_count[i], *ms = G.pt_count[i] ? G.pt_sum[i] / (double)G.pt_count[i] : 0;
    return 1;
}

// Each frame by the game's state for the periodic line (vk_fg_stats.h), the one the Windows paths write too: counted on
// the present path (gfx_present.c ng_record) as vk_fsr3fg_evaluate and vk_fsr3fg_paused see the frames.
static FgStats S;
static uint64_t S_presents;   // the presenter's presents so far (vk_fsr3fg_presents)

static void stats_frame(FgsOutcome o) {
    uint64_t now = plat_now_ns();
    fgs_count(&S, o, S_presents, now);
    char line[1024];
    if (fgs_line(&S, "fsr3fg", S_presents, now, line, sizeof line) == 1) rt_log("%s", line);
}

void vk_fsr3fg_presents(uint64_t n) { S_presents = n; }

void vk_fsr3fg_paused(int menu) { stats_frame(menu ? FGS_MENU : FGS_NO_SCENE); }

// The timestamps of the call RING calls ago in this slot, if the GPU wrote them: the averages of the stats.
static void ts_collect(uint32_t slot) {
    if (!G.ts_on || !G.ts_written[slot]) return;
    uint64_t t[TS_PER * 2];   // value and availability
    VkResult r = vkGetQueryPoolResults(vk_ctx()->device, G.ts, slot * TS_PER, TS_PER, sizeof t, t, 2 * sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    G.ts_written[slot] = 0;
    if (r != VK_SUCCESS || !t[1] || !t[3] || !t[5] || t[2] < t[0] || t[4] < t[2]) return;
    double of = (double)(t[2] - t[0]) * G.ts_period_ns / 1e6, fi = (double)(t[4] - t[2]) * G.ts_period_ns / 1e6;
    if (of > 1000 || fi > 1000) return;   // a wrapped counter
    G.gpu_of_sum += of, G.gpu_fi_sum += fi, G.st.gpu_samples++;
    fgs_gpu(&S, of + fi);
}

int vk_fsr3fg_record(VkCommandBuffer cb, const VkNgxImage *bb, const VkNgxImage *hudless, const VkNgxImage *out,
                     const int rect[4], const VkFsr3fgInputs *in) {
    if (__atomic_load_n(&G.failed, __ATOMIC_RELAXED) || vk_fsr3fg_init() != 1) return 0;
    if (!bb->view || !out->view || !in->depth.view || !in->mv.view || bb->w != out->w || bb->h != out->h ||
        bb->fmt != out->fmt) {
        static int told;
        if (!told++) rt_log("fsr3fg: an input has no view, or the output is not of the picture's size and format");
        return 0;
    }
    if (hudless && (!hudless->view || hudless->w != bb->w || hudless->h != bb->h || hudless->fmt != bb->fmt))
        hudless = NULL;   // the interpolation compares it with bb pixel by pixel
    const uint32_t W = bb->w, H = bb->h, rw = in->depth.width, rh = in->depth.height;
    if (!W || !H || !rw || !rh) return 0;
    Ctx *x = ctx_for(W, H, bb->fmt, rw, rh);
    if (!x) return 0;
    G.st.frames++;
    const VkCtx *c = vk_ctx();
    int reset = in->reset || G.want_reset || x->fresh || !x->have_prev;
    G.want_reset = 0;

    // the picture's rectangle, inside the image
    int rx = 0, ry = 0, rwid = (int)W, rhei = (int)H;
    if (rect && rect[2] > 0 && rect[3] > 0 && rect[0] >= 0 && rect[1] >= 0 && rect[0] + rect[2] <= (int)W &&
        rect[1] + rect[3] <= (int)H)
        rx = rect[0], ry = rect[1], rwid = rect[2], rhei = rect[3];

    // the ring slot: its timestamps of RING calls ago first
    const uint32_t slot = G.ring_next;
    G.ring_next = (slot + 1) % RING;
    ts_collect(slot);
    pt_collect(slot);
    dump_collect(slot);
    if (G.pt) {
        vkCmdResetQueryPool(cb, G.pt, slot * (PT_MAX + 1), PT_MAX + 1);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.pt, slot * (PT_MAX + 1));
    }
    if (G.ts_on) {
        vkCmdResetQueryPool(cb, G.ts, slot * TS_PER, TS_PER);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.ts, slot * TS_PER);
    }

    // what drew the inputs and the picture, and the last call's reads of what this one writes, come first
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, XF | CS,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT | CS_RW);
    if (x->fresh) {   // every image cleared once, so nothing reads what memory held before
        to_general(cb, x->img, I_N);
        for (int i = 0; i < I_N; i++) clear_u(cb, x->img[i], 0);
        vkCmdFillBuffer(cb, x->cnt, 0, VK_WHOLE_SIZE, 0);
        x->fresh = 0;
    }
    // the optical flow starts again with the interpolation: its pictures, histograms and scene change result cleared
    if (reset) {
        for (int i = I_LUMA_A; i < I_LUMA_A + 2 * OF_LEVELS; i++) clear_u(cb, x->img[i], 0);
        clear_u(cb, x->img[I_SCD_OUT], 0), clear_u(cb, x->img[I_SCD_HIST], 0), clear_u(cb, x->img[I_SCD_PREV], 0);
        clear_u(cb, x->img[I_SCD_TEMP], 0);
        x->of_frame = 0;
    } else {
        x->of_frame++;
    }
    // the reconstructed depths start at the far value (the bits of 1.0f; depth not inverted)
    clear_u(cb, x->img[I_RPREV], 0x3f800000u);
    clear_u(cb, x->img[I_RINTERP], 0x3f800000u);
    barrier(cb, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, CS_RW);

    // ---- constants
    const VkDeviceSize base = (VkDeviceSize)slot * K_N * G.block;
    uint8_t *at = G.ring_map + base;
    const VkFfxCamera *cam = &in->cam;
    const float fov_y = cam->fov_y > 0 && cam->fov_y < 3.14159f ? cam->fov_y : 1.0f;
    const float znear = cam->znear > 0 ? cam->znear : 0.1f, zfar = cam->zfar > 0 ? cam->zfar : 1e8f;
    const float meters = cam->meters > 0 ? cam->meters : 1.0f;
    FiConsts k = {0};
    k.render[0] = (int32_t)rw, k.render[1] = (int32_t)rh;
    k.display[0] = (int32_t)W, k.display[1] = (int32_t)H;
    k.display_rcp[0] = 1.0f / (float)W, k.display_rcp[1] = 1.0f / (float)H;
    k.cam_near = znear, k.cam_far = zfar;
    k.upscaler_target[0] = rwid, k.upscaler_target[1] = rhei;
    k.reset = reset;
    {   // view depth from device depth (not inverted, finite far plane), in metres, and the projection's scales
        float lo = fminf(znear, zfar), hi = fmaxf(znear, zfar), q = hi / (lo - hi);
        float aspect = (float)rw / (float)rh, cot = cosf(0.5f * fov_y) / sinf(0.5f * fov_y);
        k.device_to_view[0] = -q;
        k.device_to_view[1] = q * lo * meters;
        k.device_to_view[2] = aspect / cot;
        k.device_to_view[3] = 1.0f / cot;
        k.tan_half_fov = tanf(fov_y / 2) * aspect;   // tan of half the horizontal angle
    }
    float dt = in->dt_ms;
    k.delta_time = dt > 0 && dt < 1000 ? dt : 16.67f;
    k.hudless_attached = hudless != NULL;
    k.distortion_size[0] = k.distortion_size[1] = 1;
    k.of_scale[0] = 1.0f / (float)W, k.of_scale[1] = 1.0f / (float)H;
    k.of_block = OF_BLOCK;
    k.max_render[0] = (int32_t)x->mw, k.max_render[1] = (int32_t)x->mh;
    k.rect_base[0] = rx, k.rect_base[1] = ry, k.rect_size[0] = rwid, k.rect_size[1] = rhei;
    k.transfer_function = 0;   // sRGB-encoded SDR
    k.jitter[0] = in->jx, k.jitter[1] = in->jy;
    k.mv_scale[0] = 1.0f / (float)rw, k.mv_scale[1] = 1.0f / (float)rh;   // render pixels to UV
    memcpy(at + K_FI * G.block, &k, sizeof k);
    uint32_t gpyr_groups[2], dpyr_groups[2], lpyr_groups[2];
    {
        PyrConsts p = {0};
        spd_setup(rw, rh, -1, gpyr_groups, &p.groups, &p.mips);
        memcpy(at + K_PYR_RENDER * G.block, &p, sizeof p);
        spd_setup(W, H, -1, dpyr_groups, &p.groups, &p.mips);
        memcpy(at + K_PYR_DISPLAY * G.block, &p, sizeof p);
    }
    {
        OfConsts o = {{(int32_t)W, (int32_t)H}, 0, OF_LEVELS, x->of_frame, 0, {0, 0}};
        memcpy(at + K_OF * G.block, &o, sizeof o);
        for (uint32_t l = 0; l < OF_LEVELS; l++) {
            o.level = l;
            memcpy(at + (K_OF_LEVEL0 + l) * G.block, &o, sizeof o);
        }
        OfSpdConsts s = {0};
        spd_setup(W, H, 4, lpyr_groups, &s.groups, &s.mips);
        s.groups_pyramid = s.groups;
        memcpy(at + K_OFSPD * G.block, &s, sizeof s);
    }
    vmaFlushAllocation(c->vma, G.ring_mem, base, (VkDeviceSize)K_N * G.block);

    Rec r = {.cb = cb, .slot = slot, .cnt = x->cnt, .base = base, .k_of = K_OF, .k_pyr = K_PYR_RENDER};
    VkImageView *v = r.v;
    const VkImageView cur_src = hudless ? hudless->view : bb->view;

    // ---- optical flow, on the pictures without the HUD
    const int odd = x->of_odd;
    x->of_odd = !odd;
    const int lumA = odd ? I_LUMA_B : I_LUMA_A, lumB = odd ? I_LUMA_A : I_LUMA_B;   // this frame's, the last one's
    v[S_COLOR] = cur_src;
    for (int l = 0; l < OF_LEVELS; l++) v[U_LUMA0 + l] = x->view[lumA + l];
    dispatch(&r, P_OF_LUMA, (((W + 1) / 2) + 15) / 16, (((H + 1) / 2) + 15) / 16, 1);
    dispatch(&r, P_OF_LPYR, lpyr_groups[0], lpyr_groups[1], 1);
    v[S_LUMA] = x->view[lumA];
    v[U_SCD_HIST] = x->view[I_SCD_HIST], v[U_SCD_PREV] = x->view[I_SCD_PREV], v[U_SCD_TEMP] = x->view[I_SCD_TEMP];
    v[U_SCD_OUT] = x->view[I_SCD_OUT];
    dispatch(&r, P_OF_HIST, (((W / 4) / 3) + 31) / 32, 16, 9);
    dispatch(&r, P_OF_DIV, 9, 3, 1);
    for (int level = OF_LEVELS - 1; level >= 0; level--) {
        const int odd_level = level & 1;
        const int flowA = odd != odd_level ? I_FLOW_B : I_FLOW_A, flowB = odd != odd_level ? I_FLOW_A : I_FLOW_B;
        r.k_of = K_OF_LEVEL0 + level;
        v[S_LUMA] = x->view[lumA + level], v[S_LUMA_PREV] = x->view[lumB + level];
        v[U_FLOW] = x->view[flowA + level];
        const uint32_t lw = at_least_1(W >> level), lh = at_least_1(H >> level);
        dispatch(&r, P_OF_SEARCH, ((lw + 3) / 4 * 16 + 63) / 64, (lh + 15) / 16, 1);
        v[S_FLOW_PREV] = x->view[flowA + level];
        v[U_FLOW] = level ? x->view[flowB + level] : x->view[I_OFVEC];
        uint32_t fw, fh;
        flow_size(W, H, level, &fw, &fh);
        dispatch(&r, P_OF_FILTER, (fw + 15) / 16, (fh + 3) / 4, 1);
        if (level) {
            v[S_FLOW] = x->view[flowB + level], v[U_FLOW_NEXT] = x->view[flowB + level - 1];
            uint32_t nw, nh;
            flow_size(W, H, level - 1, &nw, &nh);
            dispatch(&r, P_OF_SCALE, (nw + 3) / 4, (nh + 3) / 4, 1);
        }
    }
    if (G.ts_on) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, G.ts, slot * TS_PER + 1);

    // ---- frame interpolation: the preparation from this frame's depth and motion
    v[S_IN_DEPTH] = in->depth.view, v[S_IN_MV] = in->mv.view;
    v[S_DMV] = v[U_DMV] = x->view[I_DMV];
    v[S_DDEPTH] = v[U_DDEPTH] = x->view[I_DDEPTH];
    v[S_RPREV] = v[U_RPREV] = x->view[I_RPREV];
    v[S_RINTERP] = v[U_RINTERP] = x->view[I_RINTERP];
    v[S_GMX] = v[U_GMX] = x->view[I_GMX], v[S_GMY] = v[U_GMY] = x->view[I_GMY];
    v[S_OMX] = v[U_OMX] = x->view[I_OMX], v[S_OMY] = v[U_OMY] = x->view[I_OMY];
    v[S_DISOCC] = v[U_DISOCC] = x->view[I_DISOCC];
    v[S_PREV_SRC] = x->view[I_PREV_SRC], v[S_CUR_SRC] = cur_src, v[S_PRESENT] = bb->view;
    v[S_OFVEC] = x->view[I_OFVEC], v[S_OF_CONF] = x->view[I_ZERO_U], v[S_SCD] = x->view[I_SCD_OUT];
    v[S_OUT] = v[U_OUT] = out->view;
    v[S_PYR] = x->view[I_PYR];
    for (int m = 0; m < PYR_VIEWS; m++) v[U_PYR0 + m] = x->pyr_mip[m];
    v[S_DIST] = x->view[I_DIST];
    const uint32_t rgx = (rw + 7) / 8, rgy = (rh + 7) / 8, dgx = (W + 7) / 8, dgy = (H + 7) / 8;
    dispatch(&r, P_FI_DILATE, rgx, rgy, 1);
    // ---- the interpolation
    dispatch(&r, P_FI_SETUP, rgx, rgy, 1);
    if (!reset) {
        dispatch(&r, P_FI_RPREV, rgx, rgy, 1);
        dispatch(&r, P_FI_GAME, rgx, rgy, 1);
        r.k_pyr = K_PYR_RENDER;
        dispatch(&r, P_FI_GPYR, gpyr_groups[0], gpyr_groups[1], 1);
        dispatch(&r, P_FI_OFFIELD, ((uint32_t)((float)W / OF_BLOCK) + 7) / 8, ((uint32_t)((float)H / OF_BLOCK) + 7) / 8,
                 1);
        dispatch(&r, P_FI_DISOCC, rgx, rgy, 1);
    }
    dispatch(&r, P_FI_INTERP, dgx, dgy, 1);
    r.k_pyr = K_PYR_DISPLAY;
    dispatch(&r, P_FI_PYR, dpyr_groups[0], dpyr_groups[1], 1);
    dispatch(&r, P_FI_INPAINT, dgx, dgy, 1);

    // ---- the picture the next frame interpolates from
    barrier(cb, CS, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT, XF,
            VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!reset) dump_record(cb, slot, x, bb, out);
    barrier(cb, XF, VK_ACCESS_2_TRANSFER_READ_BIT, XF, VK_ACCESS_2_TRANSFER_WRITE_BIT);   // the dump read it first
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {W, H, 1}};
    vkCmdCopyImage(cb, hudless ? hudless->img : bb->img, VK_IMAGE_LAYOUT_GENERAL, x->img[I_PREV_SRC],
                   VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    x->have_prev = 1;
    // the presenter's copy of out, and the next present's writes of bb, come after these
    barrier(cb, CS | XF, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    if (G.ts_on) {
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, G.ts, slot * TS_PER + 2);
        G.ts_written[slot] = 1;
    }
    if (!hudless) G.st.no_hudless++;
    if (reset) {
        G.st.cuts++;
    } else {
        G.st.generated++;
        __atomic_store_n(&G.made_ns, plat_now_ns(), __ATOMIC_RELAXED);
    }
    if (G.st.frames == 1)
        rt_log("fsr3fg: first frame recorded: %ux%u picture (rectangle %d,%d %dx%d), %ux%u depth and motion, %d "
               "dispatches, %s the picture without the HUD", W, H, rx, ry, rwid, rhei, rw, rh, r.n,
               hudless ? "with" : "without");
    return !reset;
}

int vk_fsr3fg_evaluate(VkCommandBuffer cb, const VkNgxImage *bb, const VkNgxImage *hudless, const VkNgxImage *out,
                       const int rect[4]) {
    VkFsr3fgInputs in = {0};
    VkImageView dv, mvv;
    uint64_t now = plat_now_ns();
    in.dt_ms = G.last_ns ? (float)((now - G.last_ns) / 1e6) : 16.67f;
    G.last_ns = now;
    if (!vk_dlaa_fg_inputs(&in.depth, &in.mv, &in.jx, &in.jy, &in.reset) || !vk_dlaa_fg_views(&dv, &mvv)) {
        G.st.no_inputs++;
        G.want_reset = 1;   // the next frame has no valid frame before it
        stats_frame(fgs_classify(0, vk_dlaa_fg_scene(), 0, 1, 1, 0));
        return 0;
    }
    in.depth.view = dv, in.mv.view = mvv;
    if (!vk_dlaa_ffx_camera(&in.cam)) memset(&in.cam, 0, sizeof in.cam);
    const uint64_t cuts = G.st.cuts;
    int made = vk_fsr3fg_record(cb, bb, hudless, out, rect, &in);
    // these passes generate without the picture without the HUD too (bb stands in, counted in vk_fsr3fg_stats): a frame
    // that made none started a history, or failed as logged (counted as one without its inputs)
    stats_frame(made ? FGS_GENERATED : G.st.cuts != cuts ? FGS_CUT : FGS_NO_INPUTS);
    return made;
}
