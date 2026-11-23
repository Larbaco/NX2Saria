#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { PRESET_LOW, PRESET_MEDIUM, PRESET_HIGH, PRESET_ULTRA, PRESET_TIERS };
static const char *const preset_tier_names[PRESET_TIERS] = {"low", "medium", "high", "ultra"};

enum { PRESET_GPU_OTHER, PRESET_GPU_INTEGRATED, PRESET_GPU_DISCRETE, PRESET_GPU_CPU };

typedef struct {
    int have_gpu;
    char gpu_name[256];
    uint32_t vendor, device, driver;
    int gpu_type;
    uint32_t vram_mib;
    int mesh;
    int dlss;
    int fsr4;
    int cores, threads;
    uint32_t ram_mib;
    char handheld[32];
} PresetHw;

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

static inline void preset_gpu_caps(PresetHw *hw, int windows) {
    hw->dlss = hw->have_gpu && hw->vendor == PRESET_VENDOR_NVIDIA && preset_has(hw->gpu_name, "RTX");
    hw->fsr4 = hw->have_gpu && windows && hw->vendor == PRESET_VENDOR_AMD && preset_has(hw->gpu_name, "RX 90");
}

static inline const char *preset_handheld(const char *vendor, const char *product, const char *family,
                                          const char *board_vendor, const char *board) {
    if (preset_has(vendor, "Valve") || preset_has(board_vendor, "Valve"))
        if (preset_has(product, "Jupiter") || preset_has(product, "Galileo") || preset_has(board, "Jupiter") ||
            preset_has(board, "Galileo"))
            return "Steam Deck";
    if (preset_has(vendor, "ASUS") || preset_has(board_vendor, "ASUS"))
        if (preset_has(product, "ROG Ally") || preset_has(product, "RC71L") || preset_has(product, "RC72LA") ||
            preset_has(board, "RC71L") || preset_has(board, "RC72LA") || preset_has(family, "ROG Ally"))
            return "ROG Ally";
    if (preset_has(vendor, "LENOVO") || preset_has(board_vendor, "LENOVO")) {
        static const char *const types[] = {"83E1", "83L3", "83N0", "83N1"};
        if (preset_has(family, "Legion Go")) return "Legion Go";
        for (size_t i = 0; i < sizeof types / sizeof *types; i++)
            if (product && !strncasecmp(product, types[i], 4)) return "Legion Go";
    }
    if (preset_has(vendor, "Micro-Star") || preset_has(vendor, "MSI") || preset_has(board_vendor, "Micro-Star"))
        if (preset_has(product, "Claw") || preset_has(family, "Claw")) return "MSI Claw";
    return NULL;
}

static inline int preset_cap(int tier, int most) { return tier > most ? most : tier; }

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

enum {
    PRESET_ANY,
    PRESET_SMALL,
    PRESET_IGPU_DESK,
    PRESET_HANDHELD,
    PRESET_DLSS,
    PRESET_FSR4,
};
typedef struct { uint8_t tier, when; const char *key, *value; } PresetRow;

static const PresetRow PRESET_ROWS[] = {
    {PRESET_ULTRA, PRESET_ANY, "SCENE_SHARE", "1"},
    {PRESET_ULTRA, PRESET_ANY, "GAME_RES", "window"},
    {PRESET_ULTRA, PRESET_ANY, "SHADOW_PCF", "1"},
    {PRESET_MEDIUM, PRESET_ANY, "SCENE_SHARE", "0.67"},
    {PRESET_MEDIUM, PRESET_ANY, "GAME_RES", "1080"},
    {PRESET_MEDIUM, PRESET_DLSS, "DLAA", "1"},
    {PRESET_MEDIUM, PRESET_DLSS, "TAA", "dlss"},
    {PRESET_MEDIUM, PRESET_DLSS, "DLSS", "quality"},
    {PRESET_MEDIUM, PRESET_FSR4, "DLAA", "1"},
    {PRESET_MEDIUM, PRESET_FSR4, "TAA", "fsr3"},
    {PRESET_MEDIUM, PRESET_FSR4, "FSR4", "1"},
    {PRESET_MEDIUM, PRESET_FSR4, "DLSS", "quality"},
    {PRESET_LOW, PRESET_ANY, "SCENE_SHARE", "0.5"},
    {PRESET_LOW, PRESET_SMALL, "SCENE_SHARE", "0.4"},
    {PRESET_LOW, PRESET_ANY, "GAME_RES", "1080"},
    {PRESET_LOW, PRESET_SMALL, "ANISO", "4"},
    {PRESET_LOW, PRESET_SMALL, "FOLIAGE", "reduced"},
    {PRESET_LOW, PRESET_ANY, "STANDIN_FILTER", "bilinear"},
    {PRESET_LOW, PRESET_IGPU_DESK, "FPS_LIMIT", "30"},
    {PRESET_LOW, PRESET_HANDHELD, "FPS_LIMIT", "40"},
    {PRESET_LOW, PRESET_DLSS, "DLAA", "1"},
    {PRESET_LOW, PRESET_DLSS, "TAA", "dlss"},
    {PRESET_LOW, PRESET_DLSS, "DLSS", "performance"},
    {PRESET_LOW, PRESET_FSR4, "DLAA", "1"},
    {PRESET_LOW, PRESET_FSR4, "TAA", "fsr3"},
    {PRESET_LOW, PRESET_FSR4, "FSR4", "1"},
    {PRESET_LOW, PRESET_FSR4, "DLSS", "performance"},
};
#define PRESET_ROW_COUNT (sizeof PRESET_ROWS / sizeof *PRESET_ROWS)

static inline int preset_applies(int when, const PresetHw *hw) {
    int igpu = hw->have_gpu && (hw->gpu_type == PRESET_GPU_INTEGRATED || hw->gpu_type == PRESET_GPU_CPU);
    int hand = hw->handheld[0] != 0;
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

static inline const char *preset_value(int tier, const PresetHw *hw, const char *key) {
    const char *v = NULL;
    for (size_t i = 0; i < PRESET_ROW_COUNT; i++)
        if (PRESET_ROWS[i].tier == tier && !strcasecmp(PRESET_ROWS[i].key, key) && preset_applies(PRESET_ROWS[i].when, hw))
            v = PRESET_ROWS[i].value;
    return v;
}

static inline void preset_fingerprint(const PresetHw *hw, char *out, size_t cap) {
    char hand[32];
    snprintf(hand, sizeof hand, "%s", hw->handheld[0] ? hw->handheld : "none");
    for (char *p = hand; *p; p++)
        if (*p == ' ' || *p == ':') *p = '-';
    snprintf(out, cap, "%04x:%04x:%x:%u:%d:%u:%s", hw->vendor, hw->device, hw->driver, preset_gib(hw->vram_mib),
             hw->threads, preset_gib(hw->ram_mib), hand);
}

static inline unsigned preset_fp_vram_gib(const char *field, size_t n) {
    char num[16];
    if (!n || n >= sizeof num) return 0xffffffffu;
    memcpy(num, field, n);
    num[n] = 0;
    char *end;
    unsigned long v = strtoul(num, &end, 10);
    if (*end) return 0xffffffffu;
    return v >= 256 ? preset_gib((uint32_t)v) : (unsigned)v;
}

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
