#pragma once
#include <vulkan/vulkan.h>

typedef struct {
    VkImage image;
    VkFormat format;
    uint32_t width, height;
    int storage;
    VkImageView view;
} VkFfxImage;

typedef struct { float fov_y, znear, zfar, meters; } VkFfxCamera;
float vk_dlaa_fsr3_boost(uint32_t w, uint32_t ow);
void vk_upscaler_device_idle(void);
int vk_upscaler_idle_wait(void);
void vk_upscaler_let_go(void);
void vk_upscaler_settle(void);
uint64_t vk_upscaler_gpu_wait(uint64_t v);

int vk_ffx_load(void);
int vk_ffx_available(void);
int vk_ffx_framegen_available(void);
int vk_ffx_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                   const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, const VkFfxCamera *cam);
void vk_ffx_release(void);
int vk_ffx_holds(void);
void vk_ffx_release_idle(void);
int vk_ffx_trim_idle(uint64_t recent);
int vk_ffx_dead_take(uint64_t done);
uint64_t vk_ffx_dead_fence(void);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_ffx_proc_addr(VkDevice dev, const char *name);

int vk_ffx4_available(void);
const char *vk_ffx4_name(void);
int vk_ffx4_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                    const VkFfxImage *out, float jx, float jy, int reset, float dt_ms,
                    const VkFfxCamera *cam);
void vk_ffx4_release(void);
int vk_ffx4_holds(void);
void vk_ffx4_release_idle(void);
int vk_ffx4_dead_take(uint64_t done);
uint64_t vk_ffx4_dead_fence(void);
int vk_ffx4_failed(void);
void vk_ffx4_retry(void);
void vk_ffx4_trim(uint64_t now_ns);
uint64_t vk_ffx4_vram(int *contexts);
int vk_ffx_fsr4(void);

#if defined(__cplusplus) && !defined(VK_FFX_NO_API)
#include "third_party/ffx_api/ffx_api.h"
extern "C" {
ffxReturnCode_t vk_ffx_create(ffxContext *ctx, ffxCreateContextDescHeader *desc);
ffxReturnCode_t vk_ffx_destroy(ffxContext *ctx);
ffxReturnCode_t vk_ffx_configure(ffxContext *ctx, const ffxConfigureDescHeader *desc);
ffxReturnCode_t vk_ffx_query(ffxContext *ctx, ffxQueryDescHeader *desc);
ffxReturnCode_t vk_ffx_dispatch(ffxContext *ctx, const ffxDispatchDescHeader *desc);
}
#endif
