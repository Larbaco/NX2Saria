// Quality presets: which tier this PC gets, and what each tier sets. Header only and free of the settings code, so that
// tests/preset_check.c checks every decision with made-up hardware and no device.
//
// Design. A preset is a layer of defaults in cfg.c: what the player sets (environment, nx2saria.ini) wins, then the
// device profile (cfg.c DECK, the Steam Deck), then the preset in use, then the settings table. cfg.c cfg_start reads
// the hardware once (hw_detect.c hw_detect), asks preset_pick for the tier this PC should get and puts the tier in
// use in that layer before any restart-only setting is read. "High" is the game as shipped and sets nothing; the other
// tiers set the keys of PRESET_ROWS below. A preset never turns frame generation on, lowers the frame-rate limit only on
// integrated GPUs and handhelds, and asks for DLSS or FSR 4 only below High, on the cards that run them.
//
// The tier: a handheld, an integrated GPU, a software renderer on the CPU or a GPU without mesh shaders (which the
// game's renderer then emulates) get Low; any other card goes by its video memory, up to High, then the PC's memory
// and processor count cap it. Without anything known about the GPU, the game as shipped (High). Ultra is never
// recommended: it only adds to the game as shipped, and on a large monitor it multiplies the pixels drawn (a 16 GB card
// on a 3840x2160 monitor would go from the 1920x1080 scene to a 3840x2160 one, about four times as many), so it stays
// the player's choice. Video memory is compared as the size the card is sold with: an 8 GB card reports 7.8 to 8.0 GiB
// of device-local memory, so it is rounded to the nearest GiB first (preset_gib). RAM goes by thresholds below the sold
// sizes (PRESET_RAM_*_MIB).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { PRESET_LOW, PRESET_MEDIUM, PRESET_HIGH, PRESET_ULTRA, PRESET_TIERS };
static const char *const preset_tier_names[PRESET_TIERS] = {"low", "medium", "high", "ultra"};

// PRESET_GPU_CPU: a Vulkan device that renders on the processor (llvmpipe); OTHER: a virtual GPU or one of no known type
enum { PRESET_GPU_OTHER, PRESET_GPU_INTEGRATED, PRESET_GPU_DISCRETE, PRESET_GPU_CPU };

// What the tier is chosen from. have_gpu 0: nothing is known of the GPU (no Vulkan 1.3 device, or the probe could not
// run and no earlier result was kept); the GPU fields are then 0. Any number 0 means unknown.
typedef struct {
    int have_gpu;
    char gpu_name[256];
    uint32_t vendor, device, driver;   // VkPhysicalDeviceProperties vendorID, deviceID, driverVersion
    int gpu_type;                      // PRESET_GPU_*
    uint32_t vram_mib;                 // the largest device-local memory heap
    int mesh;                          // VK_EXT_mesh_shader with both mesh and task shaders
    int dlss;                          // runs DLSS (preset_gpu_caps)
    int fsr4;                          // runs FSR 4 (preset_gpu_caps)
    int cores, threads;                // physical cores and logical processors
    uint32_t ram_mib;                  // the memory the system reports installed
    char handheld[32];                 // the handheld's name (preset_handheld), "" for none
} PresetHw;

// Tier boundaries, in GiB as sold (preset_gib): video memory under LOW gives Low, under MEDIUM Medium, from MEDIUM
// on High. RAM caps go by the MiB the system reports, below the sizes PCs are sold with, because a PC
// with integrated graphics hands part of its RAM to them: under RAM_LOW_MIB (an 8 GB PC) caps at Low, under
// RAM_MEDIUM_MIB (a 12 GB one) at Medium, and a 16 GB PC that reports 14 GiB or more is not capped. Up to CPU_MEDIUM
// logical processors cap at Medium.
#define PRESET_VRAM_LOW_GIB 6
#define PRESET_VRAM_MEDIUM_GIB 8
#define PRESET_RAM_LOW_MIB 10240
#define PRESET_RAM_MEDIUM_MIB 14336
#define PRESET_CPU_MEDIUM_THREADS 4

#define PRESET_VENDOR_NVIDIA 0x10de
#define PRESET_VENDOR_AMD 0x1002

static inline unsigned preset_gib(uint32_t mib) { return (mib + 512) / 1024; }

static inline int preset_has(const char *s, const char *sub) {
    if (!s) return 0;
    for (size_t n = strlen(sub); *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return 0;
}

// DLSS: an NVIDIA card whose name says RTX (the GeForce RTX cards, the professional RTX ones). FSR 4: an AMD RDNA 4
// card (Radeon RX 9070, 9060 and the others of the RX 90x0 series), on Windows only, where the FSR 4 library runs.
static inline void preset_gpu_caps(PresetHw *hw, int windows) {
    hw->dlss = hw->have_gpu && hw->vendor == PRESET_VENDOR_NVIDIA && preset_has(hw->gpu_name, "RTX");
    hw->fsr4 = hw->have_gpu && windows && hw->vendor == PRESET_VENDOR_AMD && preset_has(hw->gpu_name, "RX 90");
}

// The handheld PC the firmware names, NULL for none. vendor, product, family, board_vendor, board: the system's
// manufacturer, product name, product family, baseboard manufacturer and baseboard product, from Linux's
// /sys/class/dmi/id (sys_vendor, product_name, product_family, board_vendor, board_name) or Windows' registry
// (HKLM\HARDWARE\DESCRIPTION\System\BIOS: SystemManufacturer, SystemProductName, SystemFamily,
// BaseBoardManufacturer, BaseBoardProduct). Any of them may be NULL or empty.
static inline const char *preset_handheld(const char *vendor, const char *product, const char *family,
                                          const char *board_vendor, const char *board) {
    // Valve's Steam Deck: the LCD model Jupiter, the OLED model Galileo
    if (preset_has(vendor, "Valve") || preset_has(board_vendor, "Valve"))
        if (preset_has(product, "Jupiter") || preset_has(product, "Galileo") || preset_has(board, "Jupiter") ||
            preset_has(board, "Galileo"))
            return "Steam Deck";
    // ASUS ROG Ally (RC71L) and Ally X (RC72LA); the product name says "ROG Ally" and the board its model
    if (preset_has(vendor, "ASUS") || preset_has(board_vendor, "ASUS"))
        if (preset_has(product, "ROG Ally") || preset_has(product, "RC71L") || preset_has(product, "RC72LA") ||
            preset_has(board, "RC71L") || preset_has(board, "RC72LA") || preset_has(family, "ROG Ally"))
            return "ROG Ally";
    // Lenovo Legion Go: the family names it, the product name is the machine type (83E1 the first, 83L3 the Go S,
    // 83N0 and 83N1 the Go 2)
    if (preset_has(vendor, "LENOVO") || preset_has(board_vendor, "LENOVO")) {
        static const char *const types[] = {"83E1", "83L3", "83N0", "83N1"};
        if (preset_has(family, "Legion Go")) return "Legion Go";
        for (size_t i = 0; i < sizeof types / sizeof *types; i++)
            if (product && !strncasecmp(product, types[i], 4)) return "Legion Go";
    }
    // MSI Claw
    if (preset_has(vendor, "Micro-Star") || preset_has(vendor, "MSI") || preset_has(board_vendor, "Micro-Star"))
        if (preset_has(product, "Claw") || preset_has(family, "Claw")) return "MSI Claw";
    return NULL;
}

static inline int preset_cap(int tier, int most) { return tier > most ? most : tier; }

// The tier this PC gets, and why in a few English words (into why, cap bytes).
static inline int preset_pick(const PresetHw *hw, char *why, size_t cap) {
    char buf[160];
    int tier;
    if (hw->handheld[0]) {
        snprintf(why, cap, "a handheld PC (%s)", hw->handheld);
        return PRESET_LOW;
    }
    if (!hw->have_gpu) {
        tier = PRESET_HIGH;
        snprintf(buf, sizeof buf, "nothing is known of the GPU: the game as shipped");
    } else if (hw->gpu_type == PRESET_GPU_INTEGRATED) {
        snprintf(why, cap, "an integrated GPU");
        return PRESET_LOW;
    } else if (hw->gpu_type == PRESET_GPU_CPU) {
        snprintf(why, cap, "a software renderer on the processor");
        return PRESET_LOW;
    } else if (!hw->mesh) {
        snprintf(why, cap, "the GPU has no mesh shaders, which the game's renderer then emulates");
        return PRESET_LOW;
    } else {
        unsigned gib = preset_gib(hw->vram_mib);
        tier = gib < PRESET_VRAM_LOW_GIB ? PRESET_LOW : gib < PRESET_VRAM_MEDIUM_GIB ? PRESET_MEDIUM : PRESET_HIGH;
        snprintf(buf, sizeof buf, "%u GiB of video memory", gib);
    }
    size_t n = strlen(buf);
    unsigned ram = preset_gib(hw->ram_mib);
    if (hw->ram_mib && hw->ram_mib < PRESET_RAM_LOW_MIB && tier > PRESET_LOW) {
        tier = PRESET_LOW;
        n += (size_t)snprintf(buf + n, sizeof buf - n, "; %u GiB of RAM: at most low", ram);
    } else if (hw->ram_mib && hw->ram_mib < PRESET_RAM_MEDIUM_MIB && tier > PRESET_MEDIUM) {
        tier = PRESET_MEDIUM;
        n += (size_t)snprintf(buf + n, sizeof buf - n, "; %u GiB of RAM: at most medium", ram);
    }
    if (hw->threads && hw->threads <= PRESET_CPU_MEDIUM_THREADS && tier > PRESET_MEDIUM && n < sizeof buf) {
        tier = PRESET_MEDIUM;
        snprintf(buf + n, sizeof buf - n, "; %d logical processors: at most medium", hw->threads);
    }
    snprintf(why, cap, "%s", buf);
    return tier;
}

// ---- what each tier sets. One row per key and hardware; for one key, a later row that applies replaces an earlier
// one. Recalibrate from bench numbers here; tests/preset_check.c checks every value against its setting's choices.
// The rows keep to what costs GPU time measurably in this game version (docs/presets.md, Measurements): the scene's
// share, the picture's height and the filter that enlarges the scene. Anisotropic filtering, texture detail, foliage,
// anti-aliasing and the soft-shadow settings cost no time a desktop card shows, so High's and Medium's stay as
// shipped. Low lowers anisotropic filtering and foliage only on an integrated GPU or a handheld, whose memory bandwidth
// they may still ease; a card of its own that falls to Low (no mesh shaders, little video memory) keeps them, and no
// tier sets texture detail: its auto already picks smaller textures by the budget the driver grants. shadow_res has no
// effect on 1.1.1 (cfg_supported) and is in no tier.
enum {
    PRESET_ANY,        // every PC
    PRESET_SMALL,      // an integrated GPU or a handheld
    PRESET_IGPU_DESK,  // an integrated GPU that is no handheld (a laptop or a desktop's processor graphics)
    PRESET_HANDHELD,   // a handheld
    PRESET_DLSS,       // a card that runs DLSS (preset_gpu_caps)
    PRESET_FSR4,       // a card that runs FSR 4 and not DLSS
};
typedef struct { uint8_t tier, when; const char *key, *value; } PresetRow;

static const PresetRow PRESET_ROWS[] = {
    // ---- Ultra: more than the game as shipped; only the player chooses it (preset_pick never recommends it)
    {PRESET_ULTRA, PRESET_ANY, "SCENE_SHARE", "1"},       // the 3D scene at the picture's full size (shipped: 0.75)
    {PRESET_ULTRA, PRESET_ANY, "GAME_RES", "window"},     // the picture and HUD at the window's height (shipped: 1440)
    {PRESET_ULTRA, PRESET_ANY, "SHADOW_PCF", "1"},        // shadow edges filtered over several samples
    // ---- High: the game as shipped. No rows.
    // ---- Medium
    {PRESET_MEDIUM, PRESET_ANY, "SCENE_SHARE", "0.67"},   // the 3D scene at two thirds of the picture
    {PRESET_MEDIUM, PRESET_ANY, "GAME_RES", "1080"},      // the picture and HUD 1080 lines high
    {PRESET_MEDIUM, PRESET_DLSS, "DLAA", "1"},            // the scene through DLSS ...
    {PRESET_MEDIUM, PRESET_DLSS, "TAA", "dlss"},
    {PRESET_MEDIUM, PRESET_DLSS, "DLSS", "quality"},      // ... in its quality mode
    {PRESET_MEDIUM, PRESET_FSR4, "DLAA", "1"},            // the scene through FSR 4 ...
    {PRESET_MEDIUM, PRESET_FSR4, "TAA", "fsr3"},
    {PRESET_MEDIUM, PRESET_FSR4, "FSR4", "1"},
    {PRESET_MEDIUM, PRESET_FSR4, "DLSS", "quality"},      // ... in its quality mode
    // ---- Low
    {PRESET_LOW, PRESET_ANY, "SCENE_SHARE", "0.5"},       // the 3D scene at half the picture
    {PRESET_LOW, PRESET_SMALL, "SCENE_SHARE", "0.4"},     // and less on an integrated GPU or a handheld
    {PRESET_LOW, PRESET_ANY, "GAME_RES", "1080"},         // the picture and HUD 1080 lines high
    {PRESET_LOW, PRESET_SMALL, "ANISO", "4"},             // 4x anisotropic filtering on an integrated GPU or a handheld
    {PRESET_LOW, PRESET_SMALL, "FOLIAGE", "reduced"},     // fewer grass and bush draws there
    {PRESET_LOW, PRESET_ANY, "STANDIN_FILTER", "bilinear"},   // the scene enlarged with the plain filter (shipped: easu)
    {PRESET_LOW, PRESET_IGPU_DESK, "FPS_LIMIT", "30"},    // 30 fps, which an integrated GPU holds
    {PRESET_LOW, PRESET_HANDHELD, "FPS_LIMIT", "40"},     // 40 fps, which divides a handheld's 40, 80 and 120 Hz
    {PRESET_LOW, PRESET_DLSS, "DLAA", "1"},               // the scene through DLSS ...
    {PRESET_LOW, PRESET_DLSS, "TAA", "dlss"},
    {PRESET_LOW, PRESET_DLSS, "DLSS", "performance"},     // ... in its performance mode
    {PRESET_LOW, PRESET_FSR4, "DLAA", "1"},               // the scene through FSR 4 ...
    {PRESET_LOW, PRESET_FSR4, "TAA", "fsr3"},
    {PRESET_LOW, PRESET_FSR4, "FSR4", "1"},
    {PRESET_LOW, PRESET_FSR4, "DLSS", "performance"},     // ... in its performance mode
};
#define PRESET_ROW_COUNT (sizeof PRESET_ROWS / sizeof *PRESET_ROWS)

static inline int preset_applies(int when, const PresetHw *hw) {
    int igpu = hw->have_gpu && hw->gpu_type == PRESET_GPU_INTEGRATED, hand = hw->handheld[0] != 0;
    switch (when) {
    case PRESET_ANY: return 1;
    case PRESET_SMALL: return igpu || hand;
    case PRESET_IGPU_DESK: return igpu && !hand;
    case PRESET_HANDHELD: return hand;
    case PRESET_DLSS: return hw->dlss;
    case PRESET_FSR4: return hw->fsr4 && !hw->dlss;
    }
    return 0;
}

// The value tier gives key (upper case, no prefix) on hw; NULL when the tier leaves it alone.
static inline const char *preset_value(int tier, const PresetHw *hw, const char *key) {
    const char *v = NULL;
    for (size_t i = 0; i < PRESET_ROW_COUNT; i++)
        if (PRESET_ROWS[i].tier == tier && !strcasecmp(PRESET_ROWS[i].key, key) && preset_applies(PRESET_ROWS[i].when, hw))
            v = PRESET_ROWS[i].value;
    return v;
}

// The hardware as one line: vendor:device:driver:video GiB:logical processors:RAM GiB:handheld, the IDs in hex, both
// sizes rounded to the GiB as sold (preset_gib) and the handheld's name with '-' for blanks ("none" for none). The
// state file (cfg.c) keeps it. Video memory in whole GiB, as resizable BAR (Smart Access Memory) turned on or off or a
// driver update moves the heap the card reports by a few hundred MiB on the same card.
static inline void preset_fingerprint(const PresetHw *hw, char *out, size_t cap) {
    char hand[32];
    snprintf(hand, sizeof hand, "%s", hw->handheld[0] ? hw->handheld : "none");
    for (char *p = hand; *p; p++)
        if (*p == ' ' || *p == ':') *p = '-';
    snprintf(out, cap, "%04x:%04x:%x:%u:%d:%u:%s", hw->vendor, hw->device, hw->driver, preset_gib(hw->vram_mib),
             hw->threads, preset_gib(hw->ram_mib), hand);
}

// The video memory field of a fingerprint in GiB. A state file written before the fingerprint rounded it holds MiB
// there; a number of 256 or more is taken as MiB and rounded as preset_gib rounds (no card that runs the game has
// less than 256 MiB, and none has 256 GiB).
static inline unsigned preset_fp_vram_gib(const char *field, size_t n) {
    char num[16];
    if (!n || n >= sizeof num) return 0xffffffffu;
    memcpy(num, field, n);
    num[n] = 0;
    char *end;
    unsigned long v = strtoul(num, &end, 10);
    if (*end) return 0xffffffffu;   // not a number: equal only to itself (compared as text below)
    return v >= 256 ? preset_gib((uint32_t)v) : (unsigned)v;
}

// Whether two fingerprints name the same hardware: every field but the driver version, which a driver update
// changes on the same card; the video memory by its GiB (preset_fp_vram_gib), so a fingerprint of the older form in MiB
// still matches.
static inline int preset_same_hardware(const char *a, const char *b) {
    for (int field = 0;; field++) {
        size_t na = strcspn(a, ":"), nb = strcspn(b, ":");
        if (field == 3) {
            unsigned ga = preset_fp_vram_gib(a, na), gb = preset_fp_vram_gib(b, nb);
            if (ga != gb || (ga == 0xffffffffu && (na != nb || strncmp(a, b, na)))) return 0;
        } else if (field != 2 && (na != nb || strncmp(a, b, na))) {
            return 0;
        }
        if (!a[na] || !b[nb]) return !a[na] && !b[nb];
        a += na + 1, b += nb + 1;
    }
}
