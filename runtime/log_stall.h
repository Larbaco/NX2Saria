#pragma once
#include <stddef.h>
#include <stdint.h>

void log_stamp(char *out, size_t cap, uint64_t mono_ns);

typedef enum { LS_ACQUIRE_FAILED, LS_FG_WATCHDOG, LS_MAP_PHYS_FAILED, LS_UNMAP_PHYS_FAILED, LS_N } LsEvent;
int log_event(LsEvent e);
int log_events_text(char *out, size_t cap, int whole);

void log_pressure_baseline(void);
int log_pressure_text(char *out, size_t cap);

#ifndef LOG_STALL_IMPL
#include <stdio.h>
__attribute__((weak)) void log_stamp(char *out, size_t cap, uint64_t mono_ns) {
    (void)mono_ns;
    if (cap) snprintf(out, cap, "--:--:--");
}
__attribute__((weak)) int log_event(LsEvent e) { return (void)e, 1; }
__attribute__((weak)) int log_events_text(char *out, size_t cap, int whole) {
    (void)whole;
    if (cap) out[0] = 0;
    return 0;
}
__attribute__((weak)) void log_pressure_baseline(void) {}
__attribute__((weak)) int log_pressure_text(char *out, size_t cap) {
    if (cap) out[0] = 0;
    return 0;
}
#endif
