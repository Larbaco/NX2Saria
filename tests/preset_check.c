// The quality presets: runtime/preset_pick.h (the tier made-up hardware gets, the handheld names, what each tier sets)
// and runtime/cfg.c's preset layer (cfg_start's choice, the state file, the menu's calls), with stand-ins for what cfg.c
// links against. Built and run by tests/preset_check.py.
//   pure            preset_pick.h alone: prints "ok:" or "FAIL:" per check
//   values <dir>    every value any tier gives any key on any hardware, read by cfg.c as the settings file's and as a
//                   Ctrl+F5 reload's: prints "FAIL:" for a value outside its setting's choices
//   run <dir> ...   cfg_init and cfg_start with the settings file and state file in <dir> and the hardware in
//                   NX2SARIA_TEST_HW (a tool mode's start with NX2SARIA_TEST_TOOL set); then
//                   the actions after it (use=<tier>, chosen, set=<KEY>=<value>, save, ini=<text>,
//                   reload, notice, handoff=<tier>); prints every line cfg.c logs ("log: ...") and what the
//                   readers get ("got: ...")
// The stand-ins for what cfg.c links against are in preset_check_stubs.h.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "hw_detect.h"
#include "platform/platform.h"

#include "preset_check_stubs.h"

// ---- pure: preset_pick.h
static int fails;

static void check(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf(ok ? "  ok: " : "FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fails += !ok;
}

static PresetHw gpu(const char *name, uint32_t vendor, int type, uint32_t vram, int mesh, int cores, int threads,
                    uint32_t ram, const char *hand, int windows) {
    PresetHw hw = {0};
    hw.have_gpu = name != NULL;
    if (name) snprintf(hw.gpu_name, sizeof hw.gpu_name, "%s", name);
    hw.vendor = vendor, hw.device = 0x1234, hw.driver = 0x100;
    hw.gpu_type = type, hw.vram_mib = vram, hw.mesh = mesh;
    hw.cores = cores, hw.threads = threads, hw.ram_mib = ram;
    if (hand) snprintf(hw.handheld, sizeof hw.handheld, "%s", hand);
    preset_gpu_caps(&hw, windows);
    return hw;
}

static void tier_is(const char *what, PresetHw hw, int want) {
    char why[192];
    int got = preset_pick(&hw, why, sizeof why);
    check(got == want, "%-44s -> %-6s (%s)%s%s", what, preset_tier_names[got], why, got == want ? "" : ", wanted ",
          got == want ? "" : preset_tier_names[want]);
}

static void handheld_is(const char *what, const char *v, const char *p, const char *f, const char *bv, const char *b,
                        const char *want) {
    const char *got = preset_handheld(v, p, f, bv, b);
    int ok = want ? got && !strcmp(got, want) : !got;
    check(ok, "handheld %-40s -> %s%s%s", what, got ? got : "none", ok ? "" : ", wanted ", ok ? "" : want ? want : "none");
}

#define D PRESET_GPU_DISCRETE
#define I PRESET_GPU_INTEGRATED
#define NV PRESET_VENDOR_NVIDIA
#define AMD PRESET_VENDOR_AMD
#define INTEL 0x8086

static void pure(void) {
    // ---- the tier
    tier_is("bench PC: RX 6700 10 GB, 16 threads, 32 GB", gpu("AMD Radeon RX 6700 (RADV NAVI22)", AMD, D, 10224, 1, 8, 16, 31900, NULL, 0), PRESET_HIGH);
    tier_is("RTX 4090 24 GB: high, ultra by choice only", gpu("NVIDIA GeForce RTX 4090", NV, D, 24564, 1, 16, 32, 64000, NULL, 1), PRESET_HIGH);
    tier_is("RTX 3060 12 GB", gpu("NVIDIA GeForce RTX 3060", NV, D, 12288, 1, 6, 12, 32000, NULL, 1), PRESET_HIGH);
    tier_is("GTX 1650 4 GB", gpu("NVIDIA GeForce GTX 1650", NV, D, 4096, 1, 6, 12, 16000, NULL, 1), PRESET_LOW);
    tier_is("RTX 4060 8 GB (8,188 MiB)", gpu("NVIDIA GeForce RTX 4060", NV, D, 8188, 1, 8, 16, 32000, NULL, 1), PRESET_HIGH);
    tier_is("RX 6600 8 GB", gpu("AMD Radeon RX 6600", AMD, D, 8176, 1, 6, 12, 16000, NULL, 1), PRESET_HIGH);
    tier_is("GTX 1060 6 GB, no mesh shaders", gpu("NVIDIA GeForce GTX 1060 6GB", NV, D, 6144, 0, 4, 8, 16000, NULL, 1), PRESET_LOW);
    tier_is("RTX 2060 6 GB", gpu("NVIDIA GeForce RTX 2060", NV, D, 5980, 1, 6, 12, 16000, NULL, 1), PRESET_MEDIUM);
    tier_is("Steam Deck", gpu("AMD Custom GPU 0405 (RADV VANGOGH)", AMD, I, 1024, 1, 4, 8, 14800, "Steam Deck", 0), PRESET_LOW);
    tier_is("ROG Ally, Radeon 780M", gpu("AMD Radeon 780M Graphics", AMD, I, 8192, 1, 8, 16, 15400, "ROG Ally", 1), PRESET_LOW);
    tier_is("Intel Iris Xe", gpu("Intel(R) Iris(R) Xe Graphics", INTEL, I, 8034, 1, 4, 8, 16000, NULL, 1), PRESET_LOW);
    tier_is("RTX 3070 with 8 GB of RAM", gpu("NVIDIA GeForce RTX 3070", NV, D, 8192, 1, 6, 12, 7800, NULL, 1), PRESET_LOW);
    tier_is("4 threads and an RX 6800 16 GB", gpu("AMD Radeon RX 6800", AMD, D, 16368, 1, 4, 4, 32000, NULL, 1), PRESET_MEDIUM);
    tier_is("RTX 4090 with 12 GB of RAM", gpu("NVIDIA GeForce RTX 4090", NV, D, 24564, 1, 16, 32, 11900, NULL, 1), PRESET_MEDIUM);
    tier_is("RTX 4090 with 16 GB of RAM (15.6 GiB)", gpu("NVIDIA GeForce RTX 4090", NV, D, 24564, 1, 16, 32, 15980, NULL, 1), PRESET_HIGH);
    tier_is("RX 7900 XTX 24 GB", gpu("AMD Radeon RX 7900 XTX", AMD, D, 24560, 1, 16, 32, 64000, NULL, 1), PRESET_HIGH);
    tier_is("llvmpipe: a software renderer", gpu("llvmpipe (LLVM 19.1.7, 256 bits)", 0x10005, PRESET_GPU_CPU, 31900, 1, 8, 16, 31900, NULL, 0), PRESET_LOW);
    tier_is("GTX 1080 Ti 11 GB, no mesh shaders", gpu("NVIDIA GeForce GTX 1080 Ti", NV, D, 11264, 0, 8, 16, 32000, NULL, 1), PRESET_LOW);
    // the three PCs docs/presets.md names: the bench, the owner's laptop (its log: 6017 MiB, RAM 16087 MiB) and a
    // player's RX 6650 XT with a Ryzen 5 4650G, whose integrated graphics keep part of the 16 GB
    tier_is("the bench: RX 6700 10 GB, 16 threads, 31 GiB", gpu("AMD Radeon RX 6700 (RADV NAVI22)", 0x1002, D, 10224, 1, 8, 16, 31932, NULL, 0), PRESET_HIGH);
    tier_is("the owner's RTX 3050 6GB Laptop GPU, 16 GB", gpu("NVIDIA GeForce RTX 3050 6GB Laptop GPU", NV, D, 6017, 1, 8, 16, 16087, NULL, 1), PRESET_MEDIUM);
    tier_is("RX 6650 XT 8 GB, Ryzen 5 4650G, 16 GB (15.3 GiB)", gpu("AMD Radeon RX 6650 XT", 0x1002, D, 8176, 1, 6, 12, 15667, NULL, 0), PRESET_HIGH);
    tier_is("no data", gpu(NULL, 0, 0, 0, 0, 0, 0, 0, NULL, 0), PRESET_HIGH);
    tier_is("no GPU data, a handheld", gpu(NULL, 0, 0, 0, 0, 4, 8, 14800, "Steam Deck", 0), PRESET_LOW);
    // ultra is never recommended: any type, mesh shaders or not, up to 96 GiB of video memory, any RAM and processors
    int ultra = 0, tried = 0;
    static const int types[] = {PRESET_GPU_OTHER, PRESET_GPU_INTEGRATED, PRESET_GPU_DISCRETE, PRESET_GPU_CPU};
    for (int ty = 0; ty < 4; ty++)
        for (uint32_t vram = 0; vram <= 96 * 1024; vram += 512)
            for (int mesh = 0; mesh < 2; mesh++)
                for (uint32_t ram = 0; ram <= 256 * 1024; ram += 16 * 1024)
                    for (int threads = 0; threads <= 64; threads += 16) {
                        PresetHw hw = gpu("NVIDIA GeForce RTX 5090", NV, types[ty], vram, mesh, threads / 2, threads, ram,
                                          NULL, 1);
                        char why[192];
                        ultra += preset_pick(&hw, why, sizeof why) == PRESET_ULTRA, tried++;
                    }
    check(!ultra, "no hardware is recommended ultra (%d of %d made-up PCs)", ultra, tried);

    // ---- what the GPU runs
    PresetHw h = gpu("NVIDIA GeForce RTX 4060", NV, D, 8188, 1, 8, 16, 32000, NULL, 0);
    check(h.dlss && !h.fsr4, "an RTX card runs DLSS");
    h = gpu("NVIDIA GeForce GTX 1660 SUPER", NV, D, 6144, 1, 8, 16, 32000, NULL, 1);
    check(!h.dlss, "a GTX card does not");
    h = gpu("AMD Radeon RX 9070 XT", AMD, D, 16304, 1, 8, 16, 32000, NULL, 1);
    check(h.fsr4 && !h.dlss, "an RX 9070 XT on Windows runs FSR 4");
    h = gpu("AMD Radeon RX 9060 XT (RADV GFX1200)", AMD, D, 16304, 1, 8, 16, 32000, NULL, 0);
    check(!h.fsr4, "an RX 9060 XT on Linux does not");
    h = gpu("AMD Radeon RX 7900 XTX", AMD, D, 24560, 1, 8, 16, 32000, NULL, 1);
    check(!h.fsr4, "an RX 7900 XTX does not");
    h = gpu("NVIDIA RTX 9000 (made up)", AMD, D, 24560, 1, 8, 16, 32000, NULL, 1);
    check(!h.dlss, "an AMD vendor ID with RTX in the name does not run DLSS");

    // ---- handheld names, as Linux's DMI tables and Windows' registry give them
    handheld_is("Steam Deck LCD (DMI)", "Valve", "Jupiter", "Aerith", "Valve", "Jupiter", "Steam Deck");
    handheld_is("Steam Deck OLED (DMI)", "Valve", "Galileo", "Sephiroth", "Valve", "Galileo", "Steam Deck");
    handheld_is("ROG Ally (DMI)", "ASUSTeK COMPUTER INC.", "ROG Ally RC71L_RC71L", "ROG Ally", "ASUSTeK COMPUTER INC.", "RC71L", "ROG Ally");
    handheld_is("ROG Ally X (DMI)", "ASUSTeK COMPUTER INC.", "ROG Ally X RC72LA_RC72LA", "ROG Ally X", "ASUSTeK COMPUTER INC.", "RC72LA", "ROG Ally");
    handheld_is("ROG Ally (registry)", "ASUSTeK COMPUTER INC.", "ROG Ally RC71L", "ROG Ally", "ASUSTeK COMPUTER INC.", "RC71L", "ROG Ally");
    handheld_is("ROG Ally, board only (registry)", "ASUSTeK COMPUTER INC.", "", "", "", "RC71L", "ROG Ally");
    handheld_is("Legion Go (DMI)", "LENOVO", "83E1", "Legion Go 8APU1", "LENOVO", "LNVNB161216", "Legion Go");
    handheld_is("Legion Go S (registry)", "LENOVO", "83L3", "", "LENOVO", "LNVNB161216", "Legion Go");
    handheld_is("Legion Go 2 (DMI)", "LENOVO", "83N1", "", "LENOVO", "", "Legion Go");
    handheld_is("MSI Claw (DMI)", "Micro-Star International Co., Ltd.", "Claw A1M", "Claw", "Micro-Star International Co., Ltd.", "MS-1T41", "MSI Claw");
    handheld_is("MSI Claw 8 AI+ (registry)", "Micro-Star International Co., Ltd.", "Claw 8 AI+ A2VM", "", "", "MS-1T52", "MSI Claw");
    handheld_is("ASUS desktop board", "ASUSTeK COMPUTER INC.", "System Product Name", "To be filled by O.E.M.", "ASUSTeK COMPUTER INC.", "ROG STRIX B550-F GAMING", NULL);
    handheld_is("Lenovo Legion laptop", "LENOVO", "82JW", "Legion 5 15ACH6H", "LENOVO", "LNVNB161216", NULL);
    handheld_is("MSI desktop board", "Micro-Star International Co., Ltd.", "MS-7C56", "", "Micro-Star International Co., Ltd.", "B550-A PRO (MS-7C56)", NULL);
    handheld_is("Valve Index host (not a Deck)", "Valve", "Something", "", "Valve", "Other", NULL);
    handheld_is("nothing known", NULL, NULL, NULL, NULL, NULL, NULL);

    // ---- what each tier sets, over a spread of hardware
    static const struct { const char *name; uint32_t vendor; int type; uint32_t vram; const char *hand; int win; } kinds[] = {
        {"NVIDIA GeForce RTX 4090", NV, D, 24564, NULL, 1}, {"NVIDIA GeForce RTX 3060", NV, D, 12288, NULL, 0},
        {"NVIDIA GeForce GTX 1650", NV, D, 4096, NULL, 1}, {"AMD Radeon RX 9070 XT", AMD, D, 16304, NULL, 1},
        {"AMD Radeon RX 9070 XT", AMD, D, 16304, NULL, 0}, {"AMD Radeon RX 6700", AMD, D, 10224, NULL, 1},
        {"AMD Radeon 780M Graphics", AMD, I, 8192, NULL, 1}, {"AMD Radeon 780M Graphics", AMD, I, 8192, "ROG Ally", 1},
        {"AMD Custom GPU 0405", AMD, I, 1024, "Steam Deck", 0}, {"Intel(R) Arc(TM) Graphics", INTEL, I, 16000, "MSI Claw", 1},
        {"Microsoft Basic Render Driver", 0x1414, PRESET_GPU_OTHER, 0, NULL, 1}, {NULL, 0, 0, 0, NULL, 1},
    };
    int bad_high = 0, bad_up = 0, bad_fps = 0, bad_fg = 0, bad_fsr4 = 0, bad_dlss = 0, bad_ultra = 0;
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++) {
        PresetHw hw = gpu(kinds[k].name, kinds[k].vendor, kinds[k].type, kinds[k].vram, 1, 8, 16, 32000, kinds[k].hand,
                          kinds[k].win);
        int small = (hw.have_gpu && hw.gpu_type == I) || hw.handheld[0];
        for (int t = 0; t < PRESET_TIERS; t++)
            for (size_t r = 0; r < PRESET_ROW_COUNT; r++) {
                const char *key = PRESET_ROWS[r].key, *v = preset_value(t, &hw, key);
                if (!v) continue;
                if (t == PRESET_HIGH) bad_high++;
                if (!strcmp(key, "FRAMEGEN") || !strcmp(key, "FRAMEGEN_MULTIPLIER") || !strcmp(key, "FG_MULT") ||
                    !strcmp(key, "FSR4_FG"))
                    bad_fg++;
                if (!strcmp(key, "FPS_LIMIT") && (!small || atoi(v) < 30 || atoi(v) > 60)) bad_fps++;
                int upscaler = !strcmp(key, "DLAA") || !strcmp(key, "TAA") || !strcmp(key, "DLSS") || !strcmp(key, "FSR4");
                if (upscaler && (t >= PRESET_HIGH || !(hw.dlss || hw.fsr4))) bad_up++;
                if ((!strcmp(key, "FSR4") || (!strcmp(key, "TAA") && !strcmp(v, "fsr3"))) && !(hw.fsr4 && !hw.dlss)) bad_fsr4++;
                if (!strcmp(key, "TAA") && !strcmp(v, "dlss") && !hw.dlss) bad_dlss++;
                if (t == PRESET_ULTRA && upscaler) bad_ultra++;
            }
    }
    check(!bad_high, "high sets nothing on any hardware (%d values)", bad_high);
    check(!bad_fg, "no tier sets frame generation (%d values)", bad_fg);
    check(!bad_up, "DLSS and FSR 4 rows only below high, only on cards that run them (%d values)", bad_up);
    check(!bad_dlss, "TAA = dlss only on an RTX card (%d values)", bad_dlss);
    check(!bad_fsr4, "FSR 4 only on RDNA 4 on Windows (%d values)", bad_fsr4);
    check(!bad_ultra, "ultra never forces an upscaler (%d values)", bad_ultra);
    check(!bad_fps, "fps_limit only on an integrated GPU or a handheld, 30..60 (%d values)", bad_fps);
    PresetHw ig = gpu("Intel(R) Iris(R) Xe Graphics", INTEL, I, 8034, 1, 4, 8, 16000, NULL, 1);
    PresetHw deck = gpu("AMD Custom GPU 0405", AMD, I, 1024, 1, 4, 8, 14800, "Steam Deck", 0);
    PresetHw rtx = gpu("NVIDIA GeForce RTX 4060", NV, D, 8188, 1, 8, 16, 32000, NULL, 1);
    PresetHw rdna4 = gpu("AMD Radeon RX 9070 XT", AMD, D, 16304, 1, 8, 16, 32000, NULL, 1);
    const char *v;
    check((v = preset_value(PRESET_LOW, &ig, "FPS_LIMIT")) && !strcmp(v, "30"), "low on an integrated GPU: 30 fps");
    check((v = preset_value(PRESET_LOW, &deck, "FPS_LIMIT")) && !strcmp(v, "40"), "low on a handheld: 40 fps");
    check(!preset_value(PRESET_MEDIUM, &ig, "FPS_LIMIT") && !preset_value(PRESET_MEDIUM, &deck, "FPS_LIMIT"),
          "medium on an integrated GPU or a handheld: no limit");
    check(!preset_value(PRESET_LOW, &rtx, "FPS_LIMIT"), "low on a card of its own: no limit");
    check((v = preset_value(PRESET_LOW, &ig, "SCENE_SHARE")) && !strcmp(v, "0.4"), "low on an integrated GPU: scene 0.4");
    check((v = preset_value(PRESET_LOW, &rtx, "SCENE_SHARE")) && !strcmp(v, "0.5"), "low on a card: scene 0.5");
    check((v = preset_value(PRESET_MEDIUM, &rtx, "DLSS")) && !strcmp(v, "quality") &&
              (v = preset_value(PRESET_MEDIUM, &rtx, "TAA")) && !strcmp(v, "dlss"),
          "medium on RTX: DLSS quality");
    check((v = preset_value(PRESET_LOW, &rtx, "DLSS")) && !strcmp(v, "performance"), "low on RTX: DLSS performance");
    check((v = preset_value(PRESET_MEDIUM, &rdna4, "FSR4")) && !strcmp(v, "1") &&
              (v = preset_value(PRESET_MEDIUM, &rdna4, "TAA")) && !strcmp(v, "fsr3"),
          "medium on RDNA 4 on Windows: FSR 4");
    check(!preset_value(PRESET_ULTRA, &rtx, "DLAA") && !preset_value(PRESET_HIGH, &rtx, "DLAA"),
          "ultra and high leave DLSS alone on RTX");
    PresetHw small_card = gpu("NVIDIA GeForce GTX 1650", NV, D, 4096, 1, 6, 12, 16000, NULL, 1);
    PresetHw no_mesh = gpu("NVIDIA GeForce GTX 1080 Ti", NV, D, 11264, 0, 8, 16, 32000, NULL, 1);
    PresetHw rx580 = gpu("AMD Radeon RX 580 Series", AMD, D, 8192, 0, 6, 12, 16000, NULL, 1);
    int tex = 0;
    for (size_t r = 0; r < PRESET_ROW_COUNT; r++) tex += !strcmp(PRESET_ROWS[r].key, "TEXTURE_DETAIL");
    check(!tex, "no tier sets texture_detail: its auto picks smaller textures by the driver's budget (%d rows)", tex);
    check(!preset_value(PRESET_LOW, &no_mesh, "ANISO") && !preset_value(PRESET_LOW, &no_mesh, "FOLIAGE") &&
              !preset_value(PRESET_LOW, &rx580, "ANISO") && !preset_value(PRESET_LOW, &rx580, "FOLIAGE") &&
              !preset_value(PRESET_LOW, &small_card, "ANISO") && !preset_value(PRESET_LOW, &rtx, "FOLIAGE"),
          "low on a card of its own (GTX 1080 Ti, RX 580 8 GB, GTX 1650, RTX 4060) keeps filtering and foliage");
    check((v = preset_value(PRESET_LOW, &ig, "ANISO")) && !strcmp(v, "4") && (v = preset_value(PRESET_LOW, &deck, "ANISO")) &&
              !strcmp(v, "4") && (v = preset_value(PRESET_LOW, &ig, "FOLIAGE")) && !strcmp(v, "reduced") &&
              (v = preset_value(PRESET_LOW, &deck, "FOLIAGE")) && !strcmp(v, "reduced"),
          "low on an integrated GPU or a handheld: aniso 4, foliage reduced");
    check((v = preset_value(PRESET_LOW, &no_mesh, "SCENE_SHARE")) && !strcmp(v, "0.5") &&
              (v = preset_value(PRESET_LOW, &no_mesh, "STANDIN_FILTER")) && !strcmp(v, "bilinear"),
          "low on a card without mesh shaders: the smaller scene and the plain filter");

    // ---- fingerprints
    char a[160], b[160];
    PresetHw x = rtx;
    preset_fingerprint(&x, a, sizeof a);
    check(!strcmp(a, "10de:1234:100:8:16:31:none"), "fingerprint %s (video memory in GiB)", a);
    x.driver = 0x200;
    preset_fingerprint(&x, b, sizeof b);
    check(preset_same_hardware(a, b), "a new driver is the same hardware");
    x.vram_mib = 8188 - 256;
    preset_fingerprint(&x, b, sizeof b);
    check(preset_same_hardware(a, b), "the heap 256 MiB smaller (resizable BAR, a driver update) is the same: %s", b);
    x.vram_mib = 8188 + 256;
    preset_fingerprint(&x, b, sizeof b);
    check(preset_same_hardware(a, b), "and 256 MiB larger: %s", b);
    check(preset_same_hardware(a, "10de:1234:100:8188:16:31:none") && preset_same_hardware("10de:1234:100:7932:16:31:none", a),
          "a fingerprint of the older form, video memory in MiB, matches its GiB");
    check(!preset_same_hardware(a, "10de:1234:100:12288:16:31:none"), "an older one of another size does not");
    check(preset_same_hardware("1002:163f:1:1024:8:14:Steam-Deck", "1002:163f:1:1:8:14:Steam-Deck"),
          "the Steam Deck's 1024 MiB in the older form is its 1 GiB");
    x.vram_mib = 12288;
    preset_fingerprint(&x, b, sizeof b);
    check(!preset_same_hardware(a, b), "another card is not");
    x = rtx, x.threads = 32;
    preset_fingerprint(&x, b, sizeof b);
    check(!preset_same_hardware(a, b), "another processor is not");
    preset_fingerprint(&deck, b, sizeof b);
    check(strstr(b, ":Steam-Deck") != NULL, "a handheld's name in the fingerprint without blanks: %s", b);
    check(!preset_same_hardware(a, "10de:1234:100:8188:16:31"), "a cut fingerprint is not the same");
}

// ---- values: every value cfg.c reads as the file's and as a reload's
static void values(const char *dir) {
    snprintf(exe_path, sizeof exe_path, "%s/nx2saria", dir);
    char ini[1100];
    snprintf(ini, sizeof ini, "%s/nx2saria.ini", dir);
    FILE *f = fopen(ini, "w");
    if (f) fclose(f);
    cfg_init();
    cfg_start(1);
    int n = 0;
    for (size_t r = 0; r < PRESET_ROW_COUNT; r++) {   // the values do not depend on the hardware, only whether a row applies
        const char *key = PRESET_ROWS[r].key, *v = PRESET_ROWS[r].value;
        if (!cfg_meta(key)) {
            printf("FAIL: %s has no row in the settings table\n", key);
            continue;
        }
        f = fopen(ini, "w");
        fprintf(f, "%s = %s\n", key, v);
        fclose(f);
        printf("value: %s = %s\n", key, v);
        if (!cfg_load(ini)) printf("FAIL: cannot read %s\n", ini);   // logs a value outside its choices
        f = fopen(ini, "w");   // a reload of a file without it, then with it: what a reload accepts (cfg.c value_ok)
        fclose(f);
        CfgReloadResult res;
        cfg_reload_file(&res);
        f = fopen(ini, "w");
        fprintf(f, "%s = %s\n", key, v);
        fclose(f);
        cfg_reload_file(&res);
        if (res.rejected) printf("FAIL: a reload rejects %s = %s\n", key, v);
        n++;
    }
    printf("values: %d checked\n", n);
}

static void got(const char *what, const char *v) { printf("got: %s %s\n", what, v ? v : "(unset)"); }

static void report(void) {
    const char *why;
    int rec = cfg_preset_recommended(&why);
    printf("got: in_use %s\n", cfg_preset_name(cfg_preset_in_use()));
    printf("got: recommended %s (%s)\n", cfg_preset_name(rec), why);
    printf("got: hw_changed %d\n", cfg_preset_hw_changed());
    printf("got: custom %d\n", cfg_preset_custom());
    printf("got: notice_due %d\n", cfg_preset_notice_due());
    int n;
    const char *const *keys = cfg_preset_keys(&n);
    // the keys choosing the tier in use in the menu hands back (runtime/ui/option_rows.cpp opt_preset_handback, outside
    // a draft): the keys it sets that the player sets to another value, the environment's aside
    printf("got: handback");
    for (int i = 0, first = 1; i < n; i++) {
        char name[80];
        snprintf(name, sizeof name, "NX2SARIA_%s", keys[i]);
        if (cfg_is_set(name) && !cfg_from_env(name) && cfg_preset_differs(cfg_preset_in_use(), name, cfg_get(name)))
            printf("%s%s", first ? " " : ",", keys[i]), first = 0;
    }
    printf("\n");
    for (int i = 0; i < n; i++) {
        char name[80], what[96];
        snprintf(name, sizeof name, "NX2SARIA_%s", keys[i]);
        snprintf(what, sizeof what, "get %s", keys[i]);
        got(what, cfg_get(name));
        snprintf(what, sizeof what, "default %s", keys[i]);
        got(what, cfg_default(keys[i]));
        snprintf(what, sizeof what, "pending %s", keys[i]);
        printf("got: %s %d\n", what, cfg_pending(keys[i]));
    }
    for (int t = 0; t < PRESET_TIERS; t++) {
        char what[64];
        snprintf(what, sizeof what, "value %s SCENE_SHARE", cfg_preset_name(t));
        got(what, cfg_preset_value(t, "scene_share"));
        static const char *const tier_keys[] = {"SCENE_SHARE", "STANDIN_FILTER", "SHADOW_PCF"};
        for (int k = 0; k < 3; k++) {
            char name[64];
            snprintf(name, sizeof name, "NX2SARIA_%s", tier_keys[k]);
            snprintf(what, sizeof what, "tier_default %s %s", cfg_preset_name(t), tier_keys[k]);
            got(what, cfg_default_tier(name, t));
        }
        snprintf(what, sizeof what, "differs %s SCENE_SHARE", cfg_preset_name(t));
        printf("got: %s %d\n", what, cfg_preset_differs(t, "NX2SARIA_SCENE_SHARE", cfg_get("NX2SARIA_SCENE_SHARE")));
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 2 && !strcmp(argv[1], "pure")) {
        pure();
        printf("preset_check pure: %s\n", fails ? "FAILED" : "passed");
        return fails ? 1 : 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "values")) {
        values(argv[2]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "run")) {
        snprintf(exe_path, sizeof exe_path, "%s/nx2saria", argv[2]);
        cfg_init();
        cfg_start(!getenv("NX2SARIA_TEST_TOOL"));   // NX2SARIA_TEST_TOOL: as a tool mode starts (main.c game_run)
        report();
        for (int i = 3; i < argc; i++) {
            if (!strncmp(argv[i], "use=", 4)) {
                unsigned before = cfg_version();
                int t = -1;
                for (int k = 0; k < PRESET_TIERS; k++)
                    if (!strcmp(argv[i] + 4, cfg_preset_name(k))) t = k;
                cfg_preset_use(t);
                printf("got: version bumped %d\n", cfg_version() != before);
            } else if (!strcmp(argv[i], "chosen")) {
                cfg_preset_chosen();
            } else if (!strncmp(argv[i], "set=", 4)) {   // set=<KEY>=<value>: cfg_set, as the menu's Apply does
                char name[160];
                snprintf(name, sizeof name, "NX2SARIA_%s", argv[i] + 4);
                char *eq = strchr(name, '=');
                if (eq) *eq = 0, cfg_set(name, eq + 1);
            } else if (!strcmp(argv[i], "save")) {   // the menu's save after its Apply
                cfg_save();
            } else if (!strncmp(argv[i], "ini=", 4)) {   // the settings file's new text, for a reload after it
                char ini[1100];
                snprintf(ini, sizeof ini, "%s/nx2saria.ini", argv[2]);
                FILE *f = fopen(ini, "w");
                if (f) fprintf(f, "%s\n", argv[i] + 4), fclose(f);
            } else if (!strcmp(argv[i], "notice")) {   // the menu shows the notice when it is due
                if (cfg_preset_notice_due()) cfg_preset_notice_done();
            } else if (!strncmp(argv[i], "handoff=", 8)) {
                // the menu's Apply of a preset choice: the values before, the keys set by hand reset, the switch
                int t = -1, n;
                for (int k = 0; k < PRESET_TIERS; k++)
                    if (!strcmp(argv[i] + 8, cfg_preset_name(k))) t = k;
                const char *const *keys = cfg_preset_keys(&n);
                static char before[32][64];
                const char *from[32];
                for (int k = 0; k < n && k < 32; k++) {
                    char name[80];
                    snprintf(name, sizeof name, "NX2SARIA_%s", keys[k]);
                    snprintf(before[k], sizeof before[k], "%s", cfg_value(name));
                    from[k] = before[k];
                    if (cfg_is_set(name)) {
                        const char *one = name;
                        cfg_reset(&one, 1);
                    }
                }
                cfg_preset_use_from(t, from);
            } else if (!strcmp(argv[i], "reload")) {
                CfgReloadResult r;
                cfg_reload_file(&r);
            }
            printf("--- after %s\n", argv[i]);
            report();
        }
        return 0;
    }
    return 2;
}
