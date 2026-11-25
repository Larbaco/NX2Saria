#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"

typedef struct RtModule {
    const char *name;
    uint64_t base, size;
    const uint64_t *addrs;
    const GuestFn *fns;
    uint64_t count;
} RtModule;

typedef struct RtLayoutEntry { const char *name; uint64_t base, size; } RtLayoutEntry;
extern const RtLayoutEntry rt_layout[];
extern const int rt_layout_count;
int rt_linked_modules(RtModule *out, int max);

void rt_register_host(uint64_t addr, GuestFn fn);

void rt_dispatch_init(const RtModule *mods, int count);

GuestFn rt_dispatch_lookup(uint64_t pc);

const char *rt_guest_name(uint64_t pc, char *buf, int size);

typedef struct RtDispatchStats {
    uint64_t lo, hi;
    uint64_t slots;
    uint64_t table_bytes;
    uint64_t functions;
    uint64_t host_fns;
} RtDispatchStats;
RtDispatchStats rt_dispatch_stats(void);

void rt_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#define RT_FATAL_EXIT 70
#define RT_LOST_EXIT 72
void rt_fatal_code(int code, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void rt_fatal_note_add(void (*fn)(void));
void rt_fatal_notes(void);
int rt_ending_enter(int code);
void rt_rumble_stop_bounded(void);
