// runtime/cfg.c's reading of the settings file and environment, with stand-ins for what cfg.c links against. Built and
// run by tests/cfg_parse_check.py: `cfg_parse_check <case> <ini>` loads the file the script wrote and prints every line
// cfg.c logged ("log: ...") and what the case reads back ("got: ...").
//   load     cfg_load on the file; then cfg_init's check of the environment (the variables the script set)
//   full     cfg_load on the file, then two cfg_set of keys the file does not have
//   read     cfg_load on the file and cfg_init's check of the environment; then what cfg_get, cfg_bool and cfg_int give
//            for a list of keys
//   lowend   cfg_load on the file and cfg_init; then what cfg_bool gives for the switch of the low-end GPU mode,
//            which is gone (cfg.c lowend_set), what two of the mode's keys read and the default of one of them
//   norow    cfg_int and cfg_double of a key without a row whose value (the environment's) does not parse
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "hw_detect.h"
#include "platform/platform.h"

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
int cfg_supported(const char *key) { (void)key; return 1; }   // cfg_support.c: no patch sets here, every key counts
void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes) {
    (void)s, (void)name, (void)count, (void)limit, (void)bytes;
}
static const char *ini;
int plat_exe_path(char *out, size_t n) {
    const char *p = getenv("NX2SARIA_TEST_EXE");
    if (!p || strlen(p) >= n) return 0;
    strcpy(out, p);
    return 1;
}
// cfg.c find_ini renames an nx2navi.ini without an nx2saria.ini; NX2SARIA_TEST_RENAME_FAIL=1 refuses it, as a file in
// use would be refused
int plat_fs_rename(const char *from, const char *to, int durable) {
    (void)durable;
    const char *fail = getenv("NX2SARIA_TEST_RENAME_FAIL");
    if (fail && *fail == '1') return PLAT_FS_BUSY;
    FILE *f = fopen(to, "rb");
    if (f) return fclose(f), PLAT_FS_EXISTS;
    return rename(from, to) ? PLAT_FS_ERROR : PLAT_FS_OK;
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
    uint8_t *d = malloc((size_t)n + 1);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) n = 0;
    d[n] = 0;
    fclose(f);
    *size = (uint64_t)n;
    return d;
}
int64_t plat_file_size(const char *p) { (void)p; return -1; }
int plat_file_write_atomic(const char *p, const void *d, size_t n) { (void)p, (void)d, (void)n; return 1; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int plat_setenv(const char *n, const char *v) { (void)n, (void)v; return 1; }
// hw_detect.c: nothing known of the hardware, so the recommended preset is high, which sets nothing
void hw_detect(PresetHw *hw, int probe, char *skipped, size_t skipped_cap, char *note, size_t note_cap,
               double *probe_ms) {
    (void)probe;
    memset(hw, 0, sizeof *hw);
    snprintf(skipped, skipped_cap, "a test");
    note[0] = 0, (void)note_cap;
    *probe_ms = 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) return 2;
    ini = argv[2];
    if (!strcmp(argv[1], "load")) {
        if (!cfg_load(ini)) return printf("cfg_load failed\n"), 2;
        printf("got: pace_smooth '%s'\n", cfg_get("NX2NAVI_PACE_SMOOTH") ? cfg_get("NX2NAVI_PACE_SMOOTH") : "(unset)");
        printf("--- environment\n");
        cfg_init();   // finds no settings file next to the stand-in exe; checks the environment's values
        cfg_start(0);
        return 0;
    }
    if (!strcmp(argv[1], "full")) {
        if (!cfg_load(ini)) return printf("cfg_load failed\n"), 2;
        const char *ll = cfg_get("NX2NAVI_LOW_LATENCY");
        printf("got: low_latency %s\n", ll ? ll : "(unset)");
        cfg_set("NX2NAVI_VK_PRESENT", "fifo");
        cfg_set("NX2NAVI_FULLSCREEN", "1");
        printf("got: vk_present %s\n", cfg_get("NX2NAVI_VK_PRESENT") ? cfg_get("NX2NAVI_VK_PRESENT") : "(unset)");
        return 0;
    }
    if (!strcmp(argv[1], "read")) {
        if (!cfg_load(ini)) return printf("cfg_load failed\n"), 2;
        printf("--- environment\n");
        cfg_init();
        cfg_start(0);
        static const char *const keys[] = {"PACE_SMOOTH", "FULLSCREEN", "FPS_LIMIT", "VK_PRESENT", "ASPECT", "RTSS",
                                           "HDR_LIVE", "CLOTH_REALTIME", "WINDOW_SIZE", "DLSS_PRESET", "MODS_DIR",
                                           "DYN_RES", "SHADOW_PCF"};
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
            char name[64];
            snprintf(name, sizeof name, "NX2NAVI_%s", keys[i]);
            const char *v = cfg_get(name);
            printf("got: %s '%s'", keys[i], v ? v : "(unset)");
            const CfgMeta *m = cfg_meta(name);
            if (m && m->values && !strcmp(m->values, "0|1")) printf(" bool %d int %d", cfg_bool(name), cfg_int(name));
            printf("\n");
        }
        return 0;
    }
    if (!strcmp(argv[1], "lowend")) {
        if (!cfg_load(ini)) return printf("cfg_load failed\n"), 2;
        cfg_init();
        cfg_start(0);
        const char *mesh = cfg_get("NX2NAVI_MESH_EMULATION"), *res = cfg_get("NX2NAVI_VRAM_RESERVE_MB");
        printf("got: bool %d mesh_emulation '%s' vram_reserve_mb '%s' default '%s'\n", cfg_bool("NX2NAVI_LOW_END_GPU"),
               mesh ? mesh : "(unset)", res ? res : "(unset)", cfg_default("NX2NAVI_VRAM_RESERVE_MB"));
        return 0;
    }
    if (!strcmp(argv[1], "norow")) {
        if (!cfg_load(ini)) return printf("cfg_load failed\n"), 2;
        printf("got: cfg_int %d cfg_double %g\n", cfg_int("NX2NAVI_NO_ROW_CHECK"), cfg_double("NX2NAVI_NO_ROW_CHECK"));
        return 0;
    }
    if (!strcmp(argv[1], "nearby")) {
        cfg_init();
        cfg_start(0);
        const char *v = cfg_get("NX2SARIA_SHARPNESS");
        printf("nearby: sharpness '%s'\n", v ? v : "(unset)");
        return 0;
    }
    return 2;
}
