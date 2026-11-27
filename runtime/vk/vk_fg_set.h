#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "vk_ffx.h"

#define VK_DLAA_FAR_INFINITE 1e8f
typedef struct {
    float view_to_clip[16], clip_to_view[16], clip_to_prev_clip[16], prev_clip_to_clip[16];
    float pos[3], up[3], right[3], fwd[3];
    float fov_y, aspect;
    float znear, zfar;
    int infinite, inverted;
} VkDlaaCamera;
typedef struct {
    VkFfxImage depth, mv;
    VkImageView depth_view, mv_view;
    float jx, jy;
    int reset;
    VkDlaaCamera cam;
    int cam_ok;
    uint64_t made, age;
} VkDlaaFgSet;
typedef struct {
    VkImage img;
    VkImageView view;
    uint32_t w, h;
    VkFormat fmt;
    uint64_t made, age;
} VkDlaaFgHudless;
