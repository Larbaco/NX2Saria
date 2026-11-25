#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *version, *game_version;
    const char *os_family, *os_name;
    int wine;
    const char *cpu_name;
    int64_t cpu_threads, ram_mb;
    const char *gpu_name, *gpu_driver;
    int64_t gpu_vendor, gpu_device, vram_mb;
    const char *output, *render_scale, *game_aspect, *picture;
    int fullscreen;
    const char *preset, *upscaler, *taa, *dlss, *framegen, *vk_present;
    int64_t fps_limit;
    int64_t duration_s, frames;
    const char *end;
    int has_frametime;
    double fps_mean, fps_low_1pct, worst_ms;
    int64_t over_25ms, over_100ms;
    int64_t shader_prewarm_ms, shaders_translated, pipelines_built, pipeline_build_ms;
    const char *const *patches;
    int n_patches;
    int64_t mods_count, mods_code;
    const char *fatal;
} SessionSummary;

int ss_format(const SessionSummary *s, char *out, size_t cap);

const char *ss_setting(const char *value, const char *allowed, char *out, size_t cap);

void ss_os_name(const char *raw, char *out, size_t cap);

void ss_frametime(const uint32_t *hist, int bins, uint64_t bin_ns, uint64_t frames, uint64_t sum_ns, uint64_t max_ns,
                  double *fps_mean, double *fps_low_1pct, double *worst_ms);

typedef struct {
    char name[256], driver[288];
    uint32_t vendor, device;
    uint64_t vram_mb;
} SsGpu;
int vk_caps_gpu(SsGpu *out);
int gfx_shader_prewarm_stats(int64_t *ms, int64_t *translated);
int vk_pipeline_boot_stats(int64_t *built, int64_t *ms);
int wide_picture_known(unsigned *w, unsigned *h);
int rt_patches_on(const char **names, int cap);
int mods_in_use(int *count, int *code);

void session_summary_start(void);
void session_summary_header(const char *at);
void session_summary_exit(void);
void session_summary_fatal(int code, const char *line);

#ifdef __cplusplus
}
#endif
