#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    exit(3);
}
const char *bugcap_replay_dir(void) { return NULL; }
int cfg_supported(const char *key) { (void)key; return 1; }
void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes) {
    (void)s, (void)name, (void)count, (void)limit, (void)bytes;
}
static char exe_path[1024];
int plat_exe_path(char *out, size_t n) {
    if (!exe_path[0] || strlen(exe_path) >= n) return 0;
    strcpy(out, exe_path);
    return 1;
}
int plat_fs_rename(const char *from, const char *to, int durable) {
    (void)from, (void)to, (void)durable;
    return PLAT_FS_ERROR;
}
int plat_file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n + 1);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) n = 0;
    d[n] = 0;
    fclose(f);
    *size = (uint64_t)n;
    return d;
}
int64_t plat_file_size(const char *p) { (void)p; return -1; }
int plat_file_write_atomic(const char *p, const void *d, size_t n) {
    printf("write: %s\n", p);
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int plat_setenv(const char *n, const char *v) { (void)n, (void)v; return 1; }
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    (void)name, (void)stack;
    fn(arg);
    return 1;
}

void hw_detect(PresetHw *hw, const char *no_probe, void (*before_probe)(void), char *skipped, size_t skipped_cap,
               char *note, size_t note_cap, double *probe_ms) {
    memset(hw, 0, sizeof *hw);
    skipped[0] = note[0] = 0;
    *probe_ms = 1.5;
    const char *spec = getenv("NX2SARIA_TEST_HW");
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec ? spec : "");
    int win = 0, crash = 0;
    for (char *f = strtok(buf, ","); f; f = strtok(NULL, ",")) {
        char *eq = strchr(f, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(f, "name")) hw->have_gpu = 1, snprintf(hw->gpu_name, sizeof hw->gpu_name, "%s", v);
        else if (!strcmp(f, "vendor")) hw->vendor = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(f, "device")) hw->device = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(f, "driver")) hw->driver = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(f, "type"))
            hw->gpu_type = !strcmp(v, "discrete") ? PRESET_GPU_DISCRETE : !strcmp(v, "integrated") ? PRESET_GPU_INTEGRATED
                         : !strcmp(v, "cpu") ? PRESET_GPU_CPU : PRESET_GPU_OTHER;
        else if (!strcmp(f, "vram")) hw->vram_mib = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(f, "mesh")) hw->mesh = atoi(v);
        else if (!strcmp(f, "cores")) hw->cores = atoi(v);
        else if (!strcmp(f, "threads")) hw->threads = atoi(v);
        else if (!strcmp(f, "ram")) hw->ram_mib = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(f, "hand")) snprintf(hw->handheld, sizeof hw->handheld, "%s", v);
        else if (!strcmp(f, "skip")) snprintf(skipped, skipped_cap, "%s", v);
        else if (!strcmp(f, "win")) win = atoi(v);
        else if (!strcmp(f, "crash")) crash = atoi(v);
    }
    if (no_probe && *no_probe) snprintf(skipped, skipped_cap, "%s", no_probe);
    if (skipped[0]) {
        PresetHw keep = *hw;
        memset(hw, 0, sizeof *hw);
        hw->cores = keep.cores, hw->threads = keep.threads, hw->ram_mib = keep.ram_mib;
        memcpy(hw->handheld, keep.handheld, sizeof hw->handheld);
        *probe_ms = 0;
        return;
    }
    if (before_probe) before_probe();
    if (crash) {
        printf("crash: in the probe\n");
        fflush(stdout);
        _Exit(9);
    }
    if (!hw->have_gpu) snprintf(note, note_cap, "no Vulkan device");
    preset_gpu_caps(hw, win);
}

#ifdef __cplusplus
}
#endif
