// The session summary: logs/session-summary.json, written once at the game's end (session_summary.c) for the
// launcher, which may send it when the player has said yes. The game sends nothing itself.
//
// Two parts. session_summary_fmt.c turns a SessionSummary into the JSON text and needs nothing of the runtime (the unit
// check tests/session_summary_check.py builds it alone). session_summary.c gathers the values and writes the file.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- the values, as the formatter takes them. A string NULL, an integer below 0 and a number below 0 are written as
// null. version, game_version and end are always written (an empty string when NULL).
typedef struct {
    const char *version, *game_version;
    const char *os_family, *os_name;   // os_family: "windows" or "linux"
    int wine;                          // 1, 0, or -1 for null
    const char *cpu_name;
    int64_t cpu_threads, ram_mb;
    const char *gpu_name, *gpu_driver;
    int64_t gpu_vendor, gpu_device, vram_mb;   // vendor/device above 0xffff are written as null
    // Tokens: the display's and the settings' values. A value with a character outside [A-Za-z0-9_.:x+-] or longer than
    // 32 is written as "other"; ss_setting maps a value outside the setting's own list to "other" before.
    // picture: the size of the picture the game draws, "2560x1440" or a widescreen one (widescreen.c), as "WxH".
    const char *output, *render_scale, *game_aspect, *picture;
    int fullscreen;                    // 1, 0, or -1 for null
    const char *preset, *upscaler, *taa, *dlss, *framegen, *vk_present;
    int64_t fps_limit;
    int64_t duration_s, frames;
    const char *end;                   // "normal", "fatal" or "device_lost"
    int has_frametime;                 // 0: "frametime" is null
    double fps_mean, fps_low_1pct, worst_ms;
    int64_t over_25ms, over_100ms;
    int64_t shader_prewarm_ms, shaders_translated, pipelines_built, pipeline_build_ms;
    const char *fatal;                 // the FATAL line without "FATAL: ", personal folders already short
} SessionSummary;

// The JSON text into out (cap bytes, NUL-terminated); its length, or -1 when it does not fit. Escapes every string,
// drops control characters and invalid UTF-8, cuts os.name, cpu.name, gpu.name and gpu.driver at 64 characters and fatal
// at 300, and writes the three frame-time numbers with at most one decimal. No allocation.
int ss_format(const SessionSummary *s, char *out, size_t cap);

// value as the summary writes a setting: the list's own spelling when value (any case) is one of allowed's items
// ("a|b|c"; an item "lo..hi" takes a whole number in that range), "other" when it is not; value itself when allowed is
// NULL (free text). NULL for a NULL or empty value. The result is value, an item copied into out (cap bytes), or a
// static string.
const char *ss_setting(const char *value, const char *allowed, char *out, size_t cap);

// The OS line of platform.h plat_os_version as the summary names the OS: "Windows 10.0.26100.1234" becomes
// "Windows 11 (build 26100)" (build 22000 and later are Windows 11), "Ubuntu 24.04.1 LTS (Linux 6.8.0)" loses the kernel
// part. Anything else is copied. out gets cap bytes.
void ss_os_name(const char *raw, char *out, size_t cap);

// The session's frame times from a histogram of `bins` bins of bin_ns each (the last also takes every slower frame),
// as perf.c ft_report computes them: mean fps over the summed intervals, the 1% low as the upper edge of the bin where
// the slowest 1% of frames begins, the slowest frame in ms. All 0 when frames or sum_ns is 0.
void ss_frametime(const uint32_t *hist, int bins, uint64_t bin_ns, uint64_t frames, uint64_t sum_ns, uint64_t max_ns,
                  double *fps_mean, double *fps_low_1pct, double *worst_ms);

// ---- what the back end hands over (strong definitions in vk_caps.c, gfx_shader.c, vk_pipeline.c; session_summary.c
// has weak ones returning 0, for builds without the Vulkan back end)
typedef struct {
    char name[256], driver[288];
    uint32_t vendor, device;
    uint64_t vram_mb;   // the largest device-local heap
} SsGpu;
int vk_caps_gpu(SsGpu *out);   // 1 once vk_caps_init ran; reads values stored then, no driver call. Any thread.
// The boot's shader prewarm (gfx_shader.c gfx_shader_prewarm): its time and the shaders it translated. 1 once it ended.
int gfx_shader_prewarm_stats(int64_t *ms, int64_t *translated);
// The boot's pipeline prewarm (vk_pipeline.c): pipelines it built and the time of its pipeline stage. 1 once it ended.
int vk_pipeline_boot_stats(int64_t *built, int64_t *ms);
// The picture widescreen.c wide_init set up: 1 and its size once wide_init has decided, 0 before (no computing: the
// fatal end may call it on any thread).
int wide_picture_known(unsigned *w, unsigned *h);

// ---- the game's side (session_summary.c)
// main.c main_run, a game run only, after the log is open: notes the start, the OS, CPU and RAM, and removes a summary
// an earlier run left, so that the launcher never takes it for this run's. Before this the other calls do nothing.
void session_summary_start(void);
// The orderly end (gfx_present.c rt_exit_steps), after the frame-time summary: end "normal".
void session_summary_exit(void);
// A fatal error's end (dispatch.c fatal_end), after its frame-time summary, on the failing thread: end "fatal", or
// "device_lost" for code 72 (dispatch.h RT_LOST_EXIT). line: the FATAL line's text. Static buffers, no allocation.
void session_summary_fatal(int code, const char *line);

#ifdef __cplusplus
}
#endif
