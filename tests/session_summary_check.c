#include <math.h>
#include <stdio.h>
#include <string.h>

#include "session_summary.h"

static int fails;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL: " __VA_ARGS__);                                                                              \
            printf("\n");                                                                                              \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

static const char *dir;

static void write_case(const char *name, const SessionSummary *s) {
    static char json[8192];
    int n = ss_format(s, json, sizeof json);
    CHECK(n > 0, "%s: ss_format returned %d", name, n);
    if (n <= 0) return;
    CHECK((size_t)n == strlen(json), "%s: length %d, string %zu", name, n, strlen(json));
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.json", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        CHECK(0, "%s: cannot write %s", name, path);
        return;
    }
    fwrite(json, 1, (size_t)n, f);
    fclose(f);
}

static const char *const full_patches[] = {"fps60", "wide_hud", "Not A Set", "", "mod_1a2b3c4d", "zstd"};

static SessionSummary full(void) {
    return (SessionSummary){
        .version = "0.19.0-rc.2", .game_version = "1.1.1",
        .os_family = "windows", .os_name = "Windows 11 (build 26100)", .wine = 0,
        .cpu_name = "AMD Ryzen 7 7800X3D 8-Core Processor", .cpu_threads = 16, .ram_mb = 32768,
        .gpu_name = "NVIDIA GeForce RTX 4070", .gpu_vendor = 0x10de, .gpu_device = 0x2786, .gpu_driver = "NVIDIA 581.57",
        .vram_mb = 12282,
        .output = "2560x1440", .render_scale = "game", .fullscreen = 1, .game_aspect = "16:10",
        .picture = "2304x1440", .preset = "high", .upscaler = "fsr", .taa = "dlss", .dlss = "dlaa", .framegen = "off",
        .fps_limit = 60, .vk_present = "fifo",
        .duration_s = 1834, .frames = 109800, .end = "normal",
        .has_frametime = 1, .fps_mean = 59.94, .fps_low_1pct = 52.06, .worst_ms = 88.44, .over_25ms = 14, .over_100ms = 0,
        .shader_prewarm_ms = 5400, .shaders_translated = 120, .pipelines_built = 900, .pipeline_build_ms = 3100,
        .patches = full_patches, .n_patches = 6, .mods_count = 2, .mods_code = 1,
        .fatal = NULL,
    };
}

static SessionSummary all_null(void) {
    return (SessionSummary){
        .version = "0.19.0", .game_version = "1.1.1", .end = "crash", .wine = -1, .fullscreen = -1,
        .cpu_threads = -1, .ram_mb = -1, .gpu_vendor = -1, .gpu_device = -1, .vram_mb = -1, .fps_limit = -1,
        .duration_s = -1, .frames = -1, .over_25ms = -1, .over_100ms = -1,
        .shader_prewarm_ms = -1, .shaders_translated = -1, .pipelines_built = -1, .pipeline_build_ms = -1,
        .n_patches = -1, .mods_count = -1, .mods_code = -1,
    };
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    dir = argv[1];

    SessionSummary s = full();
    write_case("full", &s);
    s = all_null();
    write_case("all_null", &s);

    static char long_name[400], long_fatal[2000];
    memset(long_name, 'N', sizeof long_name - 1);
    for (size_t i = 0; i + 1 < sizeof long_fatal; i++) long_fatal[i] = "a\"\\\n\x01\xc3\xa9"[i % 7];
    s = full();
    s.os_name = "Ubuntu \"24.04\" \\ LTS\n\r\t\x01\x1f\x7f";
    s.cpu_name = "Caf\xc3\xa9 \xe2\x80\x9c" "CPU\xe2\x80\x9d \xff\xfe \xc2\x85 \xed\xa0\x80 \xf0\x9f\x8e\xae";
    s.gpu_name = long_name;
    s.gpu_driver = "  padded  ";
    s.output = "2560x1440;rm -rf";
    s.render_scale = "\"quoted\"";
    s.game_aspect = "this-value-is-far-longer-than-thirty-two-characters";
    s.upscaler = "other";
    s.end = "fatal";
    s.fatal = long_fatal;
    s.cpu_threads = 5000, s.ram_mb = 99999999, s.vram_mb = 99999999, s.duration_s = 99999999;
    s.gpu_vendor = 0x10005, s.gpu_device = 0x1;
    s.fps_mean = 99999, s.fps_low_1pct = NAN, s.worst_ms = 1e12;
    write_case("hostile", &s);

    s = full();
    s.end = "device_lost";
    s.fatal = "vk: the GPU device was lost (seen at: queue submit). The run ends here; ~/Games/NX2Saria";
    s.has_frametime = 0;
    write_case("device_lost", &s);

    char small[64];
    s = full();
    CHECK(ss_format(&s, small, sizeof small) == -1 && strlen(small) < sizeof small, "a small buffer is refused");

    char out[40];
    const char *r;
    r = ss_setting("FIFO", "mailbox|fifo|immediate", out, sizeof out);
    CHECK(r && !strcmp(r, "fifo"), "FIFO in mailbox|fifo|immediate: %s", r ? r : "(null)");
    r = ss_setting("vsync", "mailbox|fifo|immediate", out, sizeof out);
    CHECK(r && !strcmp(r, "other"), "vsync: %s", r ? r : "(null)");
    r = ss_setting("144", "30..240|display|unlimited", out, sizeof out);
    CHECK(r && !strcmp(r, "144"), "144 in 30..240: %s", r ? r : "(null)");
    r = ss_setting("0144", "30..240|display|unlimited", out, sizeof out);
    CHECK(r && !strcmp(r, "144"), "0144 in 30..240: %s", r ? r : "(null)");
    r = ss_setting("500", "30..240|display|unlimited", out, sizeof out);
    CHECK(r && !strcmp(r, "other"), "500 outside 30..240: %s", r ? r : "(null)");
    r = ss_setting("Display", "30..240|display|unlimited", out, sizeof out);
    CHECK(r && !strcmp(r, "display"), "Display: %s", r ? r : "(null)");
    r = ss_setting("-5", "30..240|display|unlimited", out, sizeof out);
    CHECK(r && !strcmp(r, "other"), "-5: %s", r ? r : "(null)");
    r = ss_setting("1920x1080", NULL, out, sizeof out);
    CHECK(r && !strcmp(r, "1920x1080"), "free text: %s", r ? r : "(null)");
    CHECK(!ss_setting("", "a|b", out, sizeof out) && !ss_setting(NULL, "a|b", out, sizeof out), "empty and NULL are NULL");
    r = ss_setting("fsr", "fsr|bilinear", out, sizeof out);
    CHECK(r && !strcmp(r, "fsr"), "fsr: %s", r ? r : "(null)");
    r = ss_setting("fs", "fsr|bilinear", out, sizeof out);
    CHECK(r && !strcmp(r, "other"), "a prefix of an item is not the item: %s", r ? r : "(null)");

    char os[128];
    ss_os_name("Windows 10.0.26100.4061", os, sizeof os);
    CHECK(!strcmp(os, "Windows 11 (build 26100)"), "Windows 11: %s", os);
    ss_os_name("Windows 10.0.19045", os, sizeof os);
    CHECK(!strcmp(os, "Windows 10 (build 19045)"), "Windows 10: %s", os);
    ss_os_name("Windows 6.1.7601", os, sizeof os);
    CHECK(!strcmp(os, "Windows 6.1 (build 7601)"), "Windows 7: %s", os);
    ss_os_name("Ubuntu 24.04.1 LTS (Linux 6.8.0-45-generic)", os, sizeof os);
    CHECK(!strcmp(os, "Ubuntu 24.04.1 LTS"), "Linux: %s", os);
    ss_os_name("Linux", os, sizeof os);
    CHECK(!strcmp(os, "Linux"), "bare Linux: %s", os);

    static uint32_t hist[1024];
    hist[66] = 985, hist[132] = 10, hist[480] = 5;
    uint64_t sum = 985ull * 16600000 + 10ull * 33000000 + 5ull * 120000000;
    double mean, low, worst;
    ss_frametime(hist, 1024, 250000, 1000, sum, 120000000, &mean, &low, &worst);
    CHECK(fabs(mean - 1000 * 1e9 / sum) < 1e-9, "mean %f", mean);
    CHECK(fabs(low - 1e9 / 33250000.0) < 1e-9, "1%% low %f, want %f", low, 1e9 / 33250000.0);
    CHECK(fabs(worst - 120.0) < 1e-9, "worst %f", worst);
    memset(hist, 0, sizeof hist);
    hist[66] = 49, hist[200] = 1;
    ss_frametime(hist, 1024, 250000, 50, 49ull * 16600000 + 50000000, 50000000, &mean, &low, &worst);
    CHECK(fabs(low - 1e9 / 50250000.0) < 1e-9, "50 frames: 1%% low %f", low);
    memset(hist, 0, sizeof hist);
    hist[1023] = 1;
    ss_frametime(hist, 1024, 250000, 1, 900000000, 900000000, &mean, &low, &worst);
    CHECK(fabs(low - 1e9 / 256000000.0) < 1e-9 && fabs(worst - 900.0) < 1e-9, "slow frame: low %f worst %f", low, worst);
    ss_frametime(hist, 1024, 250000, 0, 0, 0, &mean, &low, &worst);
    CHECK(mean == 0 && low == 0 && worst == 0, "no frames: all 0");

    printf("session_summary_check (C side): %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
