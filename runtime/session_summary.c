#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "host.h"
#include "log_privacy.h"
#include "platform/platform.h"
#include "session_summary.h"
#include "sysinfo.h"

__attribute__((weak)) int vk_caps_gpu(SsGpu *out) { (void)out; return 0; }
__attribute__((weak)) int gfx_shader_prewarm_stats(int64_t *ms, int64_t *translated) { (void)ms, (void)translated; return 0; }
__attribute__((weak)) int vk_pipeline_boot_stats(int64_t *built, int64_t *ms) { (void)built, (void)ms; return 0; }
__attribute__((weak)) int wide_picture_known(unsigned *w, unsigned *h) { (void)w, (void)h; return 0; }
__attribute__((weak)) int rt_patches_on(const char **names, int cap) { (void)names, (void)cap; return -1; }
__attribute__((weak)) int mods_in_use(int *count, int *code) { (void)count, (void)code; return 0; }

#define FILE_NAME "session-summary.json"

static volatile int32_t armed, written;
static uint64_t start_ns;
static char path[PLAT_PATH_MAX * 3 + 32];
static char version[64], game_version[16], os_name[256], cpu_name[128];
static int64_t cpu_threads = -1, ram_mb = -1;
static int wine = -1;

void session_summary_start(void) {
    start_ns = plat_now_ns();
    const char *log = rt_log_path();
    char exe[PLAT_PATH_MAX * 3];
    int beside_exe = 0;
    if (!log && plat_exe_path(exe, sizeof exe) && strlen(exe) < sizeof path - sizeof FILE_NAME - 8)
        log = exe, beside_exe = 1;
    if (log && strlen(log) < sizeof path - sizeof FILE_NAME - 8) {
        snprintf(path, sizeof path, "%s", log);
        char *s = strrchr(path, '\\'), *f = strrchr(path, '/');
        if (f > s) s = f;
        if (s && beside_exe) {
            snprintf(s, sizeof path - (size_t)(s - path), "/logs");
            plat_file_mkdir_p(path);
            s += 5;
            *s = '/';
        }
        if (s) snprintf(s + 1, sizeof path - (size_t)(s + 1 - path), "%s", FILE_NAME);
        else path[0] = 0;
    }
    if (path[0]) plat_file_delete(path);
    snprintf(version, sizeof version, "%.*s", (int)strcspn(rt_port_version, "+"), rt_port_version);
    snprintf(game_version, sizeof game_version, "%d.%d.%d", rt_game_version / 100, rt_game_version / 10 % 10,
             rt_game_version % 10);
    char raw[256];
    if (plat_os_version(raw, sizeof raw)) ss_os_name(raw, os_name, sizeof os_name);
#ifdef _WIN32
    wine = plat_is_wine() != 0;
#else
    wine = 0;
#endif
    sysinfo_cpu_name(cpu_name, sizeof cpu_name);
    unsigned n = plat_cpu_count();
    cpu_threads = n ? (int64_t)n : -1;
    float used, total, proc;
    plat_mem_info(&used, &total, &proc);
    ram_mb = total > 0 ? (int64_t)total : -1;
    (void)cfg_preset_custom();
    __atomic_store_n(&armed, 1, __ATOMIC_RELEASE);
    if (path[0])
        rt_log("telemetry: a session summary goes to logs/%s at the end; the game sends nothing (the launcher sends it "
               "only while session reports are on)", FILE_NAME);
    else
        rt_log("telemetry: no session summary this run: no folder for it");
}

static const char *setting(const char *key, char *out, size_t cap) {
    const CfgMeta *m = cfg_meta(key);
    if (!m) return NULL;
    char name[64];
    snprintf(name, sizeof name, "NX2SARIA_%s", key);
    return ss_setting(cfg_value(name), m->values, out, cap);
}

static const char *priv(const char *s, char *out, size_t cap) { return s && *s ? log_privacy_str_always(s, out, cap) : NULL; }

static void write_summary(const char *end, const char *fatal_line) {
    if (!__atomic_load_n(&armed, __ATOMIC_ACQUIRE) || plat_xchg32(&written, 1)) return;
    static char json[8192], fatal[1024], os_p[256], cpu_p[256], gpu_p[256], drv_p[288];
    static char out_v[40], scale_v[40], aspect_v[40], picture_v[40], up_v[40], taa_v[40], dlss_v[40], fg_v[40],
        fps_v[40], present_v[40];
    static SsGpu gpu;
    static const char *patch_names[96];
    SessionSummary s = {
        .version = version, .game_version = game_version,
#ifdef _WIN32
        .os_family = "windows",
#else
        .os_family = "linux",
#endif
        .wine = wine, .cpu_threads = cpu_threads, .ram_mb = ram_mb, .end = end,
        .gpu_vendor = -1, .gpu_device = -1, .vram_mb = -1, .fps_limit = -1,
        .shader_prewarm_ms = -1, .shaders_translated = -1, .pipelines_built = -1, .pipeline_build_ms = -1,
        .n_patches = -1, .mods_count = -1, .mods_code = -1,
    };
    s.patches = patch_names;
    s.n_patches = rt_patches_on(patch_names, (int)(sizeof patch_names / sizeof *patch_names));
    int mods_n, mods_c;
    if (mods_in_use(&mods_n, &mods_c)) s.mods_count = mods_n, s.mods_code = mods_c;
    s.os_name = priv(os_name, os_p, sizeof os_p);
    s.cpu_name = priv(cpu_name, cpu_p, sizeof cpu_p);
    if (vk_caps_gpu(&gpu)) {
        s.gpu_name = priv(gpu.name, gpu_p, sizeof gpu_p);
        s.gpu_driver = priv(gpu.driver, drv_p, sizeof drv_p);
        s.gpu_vendor = gpu.vendor, s.gpu_device = gpu.device;
        s.vram_mb = gpu.vram_mb ? (int64_t)gpu.vram_mb : -1;
    }
    s.output = setting("OUTPUT_RES", out_v, sizeof out_v);
    s.render_scale = setting("RENDER_SCALE", scale_v, sizeof scale_v);
    s.fullscreen = cfg_meta("FULLSCREEN") ? cfg_bool("NX2SARIA_FULLSCREEN") : -1;
    s.game_aspect = setting("GAME_ASPECT", aspect_v, sizeof aspect_v);
    unsigned pw, ph;
    if (wide_picture_known(&pw, &ph)) {
        snprintf(picture_v, sizeof picture_v, "%ux%u", pw, ph);
        s.picture = picture_v;
    }
    int tier = cfg_preset_in_use();
    if (tier >= 0) s.preset = cfg_preset_custom() ? "custom" : cfg_preset_name(tier);
    s.upscaler = setting("UPSCALER", up_v, sizeof up_v);
    s.taa = setting("TAA", taa_v, sizeof taa_v);
    s.dlss = setting("DLSS", dlss_v, sizeof dlss_v);
    s.framegen = setting("FRAMEGEN", fg_v, sizeof fg_v);
    s.vk_present = setting("VK_PRESENT", present_v, sizeof present_v);
    const char *fps = setting("FPS_LIMIT", fps_v, sizeof fps_v);
    if (fps && fps[0] >= '0' && fps[0] <= '9') {
        int64_t v = 0;
        for (const char *p = fps; *p >= '0' && *p <= '9' && v < 100000; p++) v = v * 10 + (*p - '0');
        s.fps_limit = v;
    }
    uint64_t now = plat_now_ns();
    s.duration_s = now > start_ns ? (int64_t)((now - start_ns + 500000000ull) / 1000000000ull) : 0;
    PerfSession ps;
    if (perf_session_stats(&ps)) {
        s.frames = (int64_t)ps.frames;
        s.has_frametime = 1;
        s.fps_mean = ps.fps_mean, s.fps_low_1pct = ps.fps_low_1pct, s.worst_ms = ps.worst_ms;
        s.over_25ms = (int64_t)ps.over25, s.over_100ms = (int64_t)ps.over100;
    }
    gfx_shader_prewarm_stats(&s.shader_prewarm_ms, &s.shaders_translated);
    vk_pipeline_boot_stats(&s.pipelines_built, &s.pipeline_build_ms);
    if (fatal_line) {
        const char *f = priv(fatal_line, fatal, sizeof fatal);
        s.fatal = f ? f : "";
    }
    if (!path[0]) {
        rt_log("telemetry: session summary not written: no folder for it");
        return;
    }
    int n = ss_format(&s, json, sizeof json);
    if (n < 0) {
        rt_log("telemetry: session summary not written: it does not fit its buffer");
        return;
    }
    if (!plat_file_write_atomic(path, json, (size_t)n)) {
        rt_log("telemetry: session summary not written: the file could not be written");
        return;
    }
    rt_log("telemetry: session summary written to logs/%s (%d bytes, end %s)", FILE_NAME, n, end);
}

void session_summary_exit(void) { write_summary("normal", NULL); }

void session_summary_fatal(int code, const char *line) {
    write_summary(code == RT_LOST_EXIT ? "device_lost" : "fatal", line ? line : "");
}
