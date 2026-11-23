// Contains code Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors, Copyright (c) 2026 Peiikai (MIT; see
// THIRD_PARTY_NOTICES.md).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PERF_HISTORY 120

typedef struct {
    float fps, frame_ms_avg, frame_ms_max;
    float low1_fps, low01_fps;
    float frame_ms[PERF_HISTORY];
    char cpu_name[64];
    float cpu_total_pct, cpu_game_pct, cpu_temp_c;
    float ram_used_mb, ram_total_mb, ram_game_mb;
    char gpu_name[96];
    float gpu_pct, gpu_temp_c, gpu_power_w, gpu_clock_mhz, vram_used_mb, vram_total_mb;
    float vram_game_mb;
    float replay_ms, gpu_ms, pipe_wait_ms, xlate_ms, tex_ms, tex_uploads;
} PerfStats;

void perfmon_mode(int mode);
void perfmon_set_gpu(const char *name, const uint8_t *luid, int pci_bus, float vram_total_mb);
void perfmon_set_vram_game(float (*used_mb)(void));
void perfmon_shown(uint64_t now_ns);
void perfmon_frame(uint64_t now_ns, uint64_t interval_ns, const uint64_t *wait_ns, uint64_t replay_ns,
                   int64_t tex_uploads);
void perfmon_snapshot(PerfStats *out);

#ifdef __cplusplus
}
#endif
