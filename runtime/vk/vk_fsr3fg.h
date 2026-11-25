#pragma once
#include "vk_dlaa.h"

int vk_fsr3fg_build(void);
int vk_fsr3fg_init(void);
int vk_fsr3fg_available(void);
int vk_fsr3fg_failed(void);
int vk_fsr3fg_on(void);
void vk_fsr3fg_reset(void);

typedef struct {
    VkFfxImage depth, mv;
    float jx, jy;
    int reset;
    VkFfxCamera cam;
    float dt_ms;
} VkFsr3fgInputs;

int vk_fsr3fg_record(VkCommandBuffer cb, const VkNgxImage *bb, const VkNgxImage *hudless, const VkNgxImage *out,
                     const int rect[4], const VkFsr3fgInputs *in);
int vk_fsr3fg_evaluate(VkCommandBuffer cb, const VkNgxImage *bb, const VkNgxImage *hudless, const VkNgxImage *out,
                       const int rect[4]);
void vk_fsr3fg_presents(uint64_t n);
void vk_fsr3fg_paused(int menu);
void vk_fsr3fg_idle(void);
void vk_fsr3fg_release(void);

typedef struct {
    uint64_t frames;
    uint64_t generated;
    uint64_t cuts;
    uint64_t no_inputs;
    uint64_t no_hudless;
    double gpu_ms_of, gpu_ms_fi;
    uint64_t gpu_samples;
    uint64_t vram;
} VkFsr3fgStats;
void vk_fsr3fg_stats(VkFsr3fgStats *s);
int vk_fsr3fg_pass_ms(int i, const char **name, double *ms, uint64_t *count);
