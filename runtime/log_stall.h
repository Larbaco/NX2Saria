// What the log keeps so that a stall, a freeze or a lost device can be traced to the minute afterwards (log_stall.c;
// docs/performance.md, "Tracing a stall in the log"): the local time of the lines that mark an event, counts for the
// events whose own lines are limited, and the machine's memory and I/O pressure once a minute.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The local time as "hh:mm:ss", the clock the system's journal and the kernel log use, of a moment on plat_now_ns's
// clock (0: now). "--:--:--" when the zone cannot say.
void log_stamp(char *out, size_t cap, uint64_t mono_ns);

// Events whose own lines the log limits or leaves out, counted for the frame-time summary's "events" line.
typedef enum { LS_ACQUIRE_FAILED, LS_FG_WATCHDOG, LS_MAP_PHYS_FAILED, LS_UNMAP_PHYS_FAILED, LS_N } LsEvent;
// Counts one (any thread, no lock). 1 while the event's own line is to be written: its first few times (5 acquire
// failures, 3 watchdog firings, the first MapPhysicalMemory and UnmapPhysicalMemory failure), 0 after them.
int log_event(LsEvent e);
// The events as the text of one line: those counted since the previous call without whole, each with its total and
// the limit of its own lines ("presenter acquire failed 2 (7 in all, only the first 5 logged)"); whole: every event
// of the run, the previous call's moment left alone. 0 (and an empty string) when there is none.
int log_events_text(char *out, size_t cap, int whole);

// The machine's memory and I/O pressure as the text of one line, against the sample of the previous call (the
// baseline, or the line before it), so the stall time of the minute is a difference of the system's totals. 0 and an
// empty string when the platform gives nothing or the sampling is busy on another thread (never waited for).
void log_pressure_baseline(void);   // the sample the first line is made against: the start of the first minute
int log_pressure_text(char *out, size_t cap);

#ifndef LOG_STALL_IMPL
// A program that compiles a file of the runtime without log_stall.c (the unit checks in tests/ do, with stand-ins for
// the rest) links these: no time ("--:--:--"), every event has its line, no counts, no pressure. log_stall.c replaces
// them as it defines LOG_STALL_IMPL first; the game's link takes the whole runtime library, so its definitions win.
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
