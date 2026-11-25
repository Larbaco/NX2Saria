#pragma once
#include "vk.h"
#include "vk_ffx.h"

#define DLAA_SCENE_MIN_W 320u

int vk_dlaa_wanted(void);
typedef struct { VkImage img; VkImageView view; VkFormat fmt; uint32_t w, h; } VkNgxImage;
int vk_ngxfg_wanted(void);
int vk_ngxfg_max(void);
int vk_ngxfg_on(void);
void vk_ngxfg_reset(void);
int vk_ngxfg_evaluate(VkCommandBuffer cb, const VkNgxImage *bb, const VkNgxImage *hudless, const VkNgxImage *out, int count,
                      const VkNgxImage *real);
int vk_dlaa_fsr3(void);
int vk_dlaa_needs_restart(void);
int vk_dlaa_fg_inputs(VkFfxImage *depth, VkFfxImage *mv, float *jx, float *jy, int *reset);
int vk_dlaa_fg_only(void);
#define VK_DLAA_FAR_INFINITE 1e8f
typedef struct {
    float view_to_clip[16], clip_to_view[16], clip_to_prev_clip[16], prev_clip_to_clip[16];
    float pos[3], up[3], right[3], fwd[3];
    float fov_y, aspect;
    float znear, zfar;
    int infinite, inverted;
} VkDlaaCamera;
int vk_dlaa_fg_views(VkImageView *depth, VkImageView *mv);
extern int vk_dlaa_hudless_wanted;
int vk_dlaa_fg_hudless(VkImage *img, VkImageView *view, uint32_t *w, uint32_t *h, VkFormat *fmt);
int vk_dlaa_fg_scene(void);
int vk_dlaa_fg_camera(VkDlaaCamera *c);
int vk_dlaa_ffx_camera(VkFfxCamera *c);
void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_init(void);
void vk_dlaa_release(void);
int vk_dlaa_available(void);
int64_t vk_dlaa_vram(void);
typedef struct VkExec VkExec;
void vk_dlaa_pass(VkExec *ex);
void vk_dlaa_retarget(VkExec *ex);
int vk_dlaa_target_scale(const VkExec *ex, float *sx, float *sy);
void vk_dlaa_upscaler(VkExec *ex, VkImage out, VkFormat fmt, uint32_t w, uint32_t h);
int vk_dlaa_active(void);
void vk_dlaa_frame(void);
int vk_dlaa_resized_take(void);
void vk_dlaa_draw(VkExec *ex);
void vk_dlaa_viewport(VkExec *ex, VkViewport *v);
void vk_dlaa_pass_end(VkExec *ex);
void vk_dlaa_jitter_check(VkExec *ex);
void vk_mvsurvey_draw(VkExec *ex);
void vk_mvsurvey_frame(void);
void vk_mvprev_draw(VkExec *ex);
int vk_mvobj_execute(VkExec *ex, uint64_t cam, VkImageView mv, VkImageView depth, VkFormat depth_fmt, uint32_t w,
                     uint32_t h);
int vk_dlaa_prev_camera(VkDescriptorBufferInfo *out, float jitter[2]);
void vk_mvcheck_camera(VkCommandBuffer cb, VkImage mv, uint32_t w, uint32_t h, float jx, float jy);
void vk_mvcheck_final(VkCommandBuffer cb, uint64_t fence, VkImage mv, VkImage cimg, VkFormat cfmt, uint32_t level,
                      uint32_t layer, uint32_t w, uint32_t h, float jx, float jy);
const char *vk_mvobj_frame_masks(void);
void vk_mvcheck_output(VkCommandBuffer cb, VkImage out, uint32_t ow, uint32_t oh);
int vk_dlaa_skip_draw(VkExec *ex, const GfxShader *fs);

int vk_dlaa_pose_depth(uint64_t frame, uint32_t w, uint32_t h, VkDescriptorImageInfo *image, float history[4]);
