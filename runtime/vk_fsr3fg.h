// AMD FSR 3.1 frame generation recorded by the runtime itself (vk_fsr3fg.c): the optical flow and frame interpolation
// compute passes (runtime/shaders/fsr3of_*.comp, fsr3fi_*.comp, built into runtime/*_spv.h), without AMD's library.
// gfx_present.c's frame generation presenter (the one DLSS Frame Generation through NGX uses) records them into each
// present's command buffer and shows the generated frame between the frame before and this one. Every build but the
// Windows one, where NX2SARIA_FRAMEGEN = fsr3 goes through AMD's library (vk_fg.cpp).
#pragma once
#include "vk_dlaa.h"

// 1 in the builds where NX2SARIA_FRAMEGEN = fsr3 runs through these passes (all but Windows).
int vk_fsr3fg_build(void);
// Makes the pipelines once the device exists (0 before it, and asked again then). 1 when the device runs them; the
// first call logs which, and why not.
int vk_fsr3fg_init(void);
// For the settings menu, any thread: 1 before vk_fsr3fg_init ran (the build has the passes), then what it found, and 0
// after a failure in this run (vk_fsr3fg_failed).
int vk_fsr3fg_available(void);
int vk_fsr3fg_failed(void);   // a context or a recording failed in this run: frame generation stays off, as logged
// NX2SARIA_FRAMEGEN = fsr3 now (the menu switches it live), in a build of vk_fsr3fg_build, where it is available.
int vk_fsr3fg_on(void);
void vk_fsr3fg_reset(void);   // the next frame starts without history (a resize, the menu, a pause)

// One frame's inputs: the scene's depth (D32F, 0 near) and motion vectors (R16G16F, render pixels, current to
// previous, without the jitter), both of the render size and with views, the jitter in render pixels, whether the
// history broke at this frame (a camera cut, another scene size or target), the camera and the time since the last
// frame.
typedef struct {
    VkFfxImage depth, mv;
    float jx, jy;
    int reset;
    VkFfxCamera cam;
    float dt_ms;
} VkFsr3fgInputs;

// Records the generation of the frame between the previous call's picture and this one into out (the presenter's
// image, bb's size and format, storage, GENERAL). bb: this frame's picture with the HUD; hudless: the same without it
// (NULL: none this frame, bb stands in for it); rect: the picture's rectangle in bb {x, y, w, h} (black bars around it);
// in: the frame's depth and motion. Every image in GENERAL. Returns 1 when out holds a frame to show, 0 when not (a
// history that starts at this frame shows no generated frame; a failure, as logged).
int vk_fsr3fg_record(VkCommandBuffer cb, const VkNgxImage *bb, const VkNgxImage *hudless, const VkNgxImage *out,
                     const int rect[4], const VkFsr3fgInputs *in);
// The same with this frame's inputs from vk_dlaa.c (vk_dlaa_fg_inputs, vk_dlaa_fg_views, vk_dlaa_ffx_camera); a frame
// without them generates nothing and the next one starts a new history.
int vk_fsr3fg_evaluate(VkCommandBuffer cb, const VkNgxImage *bb, const VkNgxImage *hudless, const VkNgxImage *out,
                       const int rect[4]);
// Frame generation went off (the menu): its images go once the GPU is idle; the pipelines stay. g_pl held.
void vk_fsr3fg_idle(void);
void vk_fsr3fg_release(void);   // before the device goes away (the GPU idle): everything, and init runs again after

// What it did so far, for the presenter's log line and the probe.
typedef struct {
    uint64_t frames;      // calls of vk_fsr3fg_record
    uint64_t generated;   // ... that left a frame to show
    uint64_t cuts;        // ... that started a new history (no frame shown)
    uint64_t no_inputs;   // vk_fsr3fg_evaluate calls without the frame's depth and motion
    uint64_t no_hudless;  // frames generated without the picture without the HUD (bb stood in)
    double gpu_ms_of, gpu_ms_fi;   // average GPU time of the optical flow and of the interpolation (0: not measured)
    uint64_t gpu_samples;          // frames those averages are over
    uint64_t vram;                 // bytes of the context's images
} VkFsr3fgStats;
void vk_fsr3fg_stats(VkFsr3fgStats *s);
