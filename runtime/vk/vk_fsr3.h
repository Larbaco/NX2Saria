#pragma once
#include "vk_ffx.h"

int vk_fsr3_init(void);
int vk_fsr3_available(void);
int vk_fsr3_upscale(VkCommandBuffer cb, const VkFfxImage *color, const VkFfxImage *depth, const VkFfxImage *mv,
                    const VkFfxImage *out, float jx, float jy, int reset, float dt_ms, const VkFfxCamera *cam,
                    int sharpen, float sharpness);
void vk_fsr3_release(void);
int vk_fsr3_holds(void);
void vk_fsr3_release_idle(void);
int vk_fsr3_dead_take(uint64_t done);
uint64_t vk_fsr3_dead_fence(void);
int vk_fsr3_contexts_made(void);
