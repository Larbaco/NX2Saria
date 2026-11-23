#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#define NX2SL_VERSION 3

typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
    VkImageLayout layout;
    VkImageUsageFlags usage;
} Nx2SlImage;

typedef struct {
    float view_to_clip[16], clip_to_view[16], clip_to_prev_clip[16], prev_clip_to_clip[16];
    float pos[3], up[3], right[3], fwd[3];
    float znear, zfar, fov_y, aspect;
    float jitter[2];
    float mv_scale[2];
    int reset;
} Nx2SlCamera;

typedef void (*Nx2SlLog)(const char *line);

#ifdef __cplusplus
extern "C" {
#endif
#ifdef NX2SL_BUILD
#define NX2SL_API __declspec(dllexport)
#else
#define NX2SL_API
#endif
typedef int (*PFN_nx2sl_version)(void);
typedef int (*PFN_nx2sl_init)(const wchar_t *dir, Nx2SlLog log);
typedef PFN_vkGetInstanceProcAddr (*PFN_nx2sl_instance_proc)(void);
typedef PFN_vkGetDeviceProcAddr (*PFN_nx2sl_device_proc)(void);
typedef int (*PFN_nx2sl_available)(void);
typedef void (*PFN_nx2sl_log_unavailable)(VkPhysicalDevice physical);
typedef int (*PFN_nx2sl_support)(const char **name);
typedef void (*PFN_nx2sl_reflex)(int mode);
typedef int (*PFN_nx2sl_reflex2)(int mode, uint32_t frame_limit_us);
typedef void (*PFN_nx2sl_sleep)(uint32_t frame, int hold);
typedef void (*PFN_nx2sl_submit_end)(uint32_t frame);
typedef int (*PFN_nx2sl_frame)(VkCommandBuffer cb, const Nx2SlImage *depth, const Nx2SlImage *mv, const Nx2SlImage *hudless,
                               const Nx2SlCamera *cam, uint32_t out_w, uint32_t out_h, int enable, uint32_t frame);
typedef uint32_t (*PFN_nx2sl_frames_max)(void);
typedef void (*PFN_nx2sl_present_marker)(int after, uint32_t frame);
typedef uint64_t (*PFN_nx2sl_shown)(void);
typedef void (*PFN_nx2sl_shutdown)(void);
#ifdef __cplusplus
}
#endif
