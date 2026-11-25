#pragma once
#include <stdint.h>

enum { MEM_ROOMY, MEM_NORMAL, MEM_TIGHT, MEM_CRITICAL, MEM_LEVELS };
enum { MEM_VRAM, MEM_RAM, MEM_SIDES };

static const uint32_t mem_level_line[MEM_LEVELS - 1] = {400, 200, 80};
#define MEM_HYST_PM 50
#define MEM_FLOOR_MB 256

static const char *const mem_level_name[MEM_LEVELS] = {"roomy", "normal", "tight", "critical"};

typedef struct {
    uint32_t free_pm;
    uint64_t free_mb;
} MemRoom;

static inline MemRoom mem_room(uint64_t used, uint64_t limit) {
    MemRoom r = {0, 0};
    if (!limit || used >= limit) return r;
    r.free_mb = limit - used;
    r.free_pm = (uint32_t)(r.free_mb * 1000 / limit);
    return r;
}

static inline MemRoom mem_room_min(MemRoom a, MemRoom b) {
    MemRoom r = {a.free_pm < b.free_pm ? a.free_pm : b.free_pm, a.free_mb < b.free_mb ? a.free_mb : b.free_mb};
    return r;
}

static inline int mem_level_next(int level, MemRoom r) {
    int raw = 0;
    while (raw < MEM_LEVELS - 1 && r.free_pm <= mem_level_line[raw]) raw++;
    if (r.free_mb < MEM_FLOOR_MB) raw = MEM_CRITICAL;
    if (raw >= level) return raw;
    int n = level;
    while (n > raw && r.free_pm > mem_level_line[n - 1] + MEM_HYST_PM) n--;
    return n;
}

#define MEM_BUDGET_PERIOD_MS 2000
void mem_budget_vram(int on);
int mem_budget_level(int side);
int mem_budget_vram_mb(uint64_t *used_mb, uint64_t *limit_mb);
void mem_budget_text(char *out, unsigned cap);
