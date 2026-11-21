// What the log keeps so that a stall, a freeze or a lost device can be traced to the minute afterwards.
//
// The log had one wall-clock line, the window's focus change with its date (gfx_present.c). A report of a stall, or of
// a GPU that stopped, was matched against the system's journal and kernel log by an estimate (about 2 s off at best), and
// three things hid how often an event happened:
// the lines of the presenter's failed acquires and of its watchdog stop after 5 and 3, and the physical memory map's
// refusals (a session had 132 of them) wrote one line each. Nothing said whether the machine was short of memory or
// waiting for its disk while the frames stopped. This file gives each of those one place:
// - log_stamp: the local time, "hh:mm:ss", of a line that marks an event;
// - log_event / log_events_text: a count for each limited event, and the one line the frame-time summary writes with
//   the counts of its minute and, at the end of the run, of the whole run;
// - log_pressure_text: the memory and I/O stall times of the minute (Linux pressure stall information), the memory
//   available and the swap free, or on Windows the memory available and the commit.
#define LOG_STALL_IMPL   // log_stall.h: this file defines what its stand-ins stand for
#include "log_stall.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "platform/platform.h"

void log_stamp(char *out, size_t cap, uint64_t mono_ns) {
    uint64_t now = plat_now_ns(), wall = plat_unix_time_ns();
    uint64_t age = mono_ns && mono_ns < now ? now - mono_ns : 0;
    struct tm t;
    if (cap && plat_localtime((int64_t)((wall > age ? wall - age : 0) / 1000000000ull), &t))
        snprintf(out, cap, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    else if (cap)
        snprintf(out, cap, "--:--:--");
}

// ---- events
static const struct { const char *name; uint64_t lines; } ev_info[LS_N] = {
    [LS_ACQUIRE_FAILED] = {"presenter acquire failed", 5},
    [LS_FG_WATCHDOG] = {"frame generation watchdog fired", 3},
    [LS_MAP_PHYS_FAILED] = {"MapPhysicalMemory failed", 1},
    [LS_UNMAP_PHYS_FAILED] = {"UnmapPhysicalMemory failed", 1},
};
static uint64_t ev_total[LS_N];   // every count of the run
static uint64_t ev_seen[LS_N];    // the totals the previous window line counted from

int log_event(LsEvent e) {
    if ((unsigned)e >= LS_N) return 0;
    return __atomic_add_fetch(&ev_total[e], 1, __ATOMIC_RELAXED) <= ev_info[e].lines;
}

// Appends to out at *n, never past cap.
static void put(char *out, size_t cap, size_t *n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(char *out, size_t cap, size_t *n, const char *fmt, ...) {
    if (*n >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out + *n, cap - *n, fmt, ap);
    va_end(ap);
    if (k > 0) *n = (size_t)k < cap - *n ? *n + (size_t)k : cap - 1;
}

int log_events_text(char *out, size_t cap, int whole) {
    size_t n = 0;
    if (!cap) return 0;
    out[0] = 0;
    for (int e = 0; e < LS_N; e++) {
        uint64_t total = __atomic_load_n(&ev_total[e], __ATOMIC_RELAXED);
        uint64_t since = whole ? total : total - __atomic_exchange_n(&ev_seen[e], total, __ATOMIC_RELAXED);
        if (!since) continue;
        char limit[48] = "";
        if (total > ev_info[e].lines)
            snprintf(limit, sizeof limit, ev_info[e].lines == 1 ? "only the first logged" : "only the first %llu logged",
                     (unsigned long long)ev_info[e].lines);
        put(out, cap, &n, "%s%s %llu", n ? ", " : "", ev_info[e].name, (unsigned long long)since);
        if (since != total)
            put(out, cap, &n, " (%llu in all%s%s)", (unsigned long long)total, *limit ? ", " : "", limit);
        else if (*limit)
            put(out, cap, &n, " (%s)", limit);
    }
    return (int)n;
}

// ---- pressure
static PlatMutex pr_lock = PLAT_MUTEX_INIT;
static PlatPressure pr_prev;   // pr_lock: the sample the next line is made against
static uint64_t pr_prev_ns;
static int pr_have;

static double ms_between(uint64_t from_us, uint64_t to_us) { return to_us > from_us ? (double)(to_us - from_us) / 1000.0 : 0.0; }

typedef struct { double some10, full10; uint64_t some_us, full_us; } Psi;

// One resource's text: the average of the last 10 s and, with a sample to compare (prev), what the totals gained since it.
static void psi_text(char *out, size_t cap, size_t *n, const char *name, const Psi *now, const Psi *prev, double secs) {
    put(out, cap, n, "%sPSI %s avg10 some %.1f%% full %.1f%%", *n ? "; " : "", name, now->some10, now->full10);
    if (prev)
        put(out, cap, n, " (stalled in the last %.0f s: some %.0f ms, full %.0f ms)", secs,
            ms_between(prev->some_us, now->some_us), ms_between(prev->full_us, now->full_us));
}

// The text from a sample, the sample before it (NULL: none) and the seconds between them; hms is the time of the sample.
// Linux: PSI memory and io, MemAvailable and SwapFree. Windows: the memory available and the commit.
static int pressure_format(char *out, size_t cap, const PlatPressure *now, const PlatPressure *prev, double secs,
                           const char *hms) {
    size_t n = 0;
    if (!cap) return 0;
    out[0] = 0;
    char body[600];
    size_t b = 0;
    body[0] = 0;
    Psi mem = {now->mem_some10, now->mem_full10, now->mem_some_us, now->mem_full_us},
        io = {now->io_some10, now->io_full10, now->io_some_us, now->io_full_us};
    Psi pmem = {0}, pio = {0};
    if (prev) {
        pmem = (Psi){prev->mem_some10, prev->mem_full10, prev->mem_some_us, prev->mem_full_us};
        pio = (Psi){prev->io_some10, prev->io_full10, prev->io_some_us, prev->io_full_us};
    }
    if (mem.some10 >= 0 && mem.full10 >= 0) psi_text(body, sizeof body, &b, "memory", &mem, prev ? &pmem : NULL, secs);
    if (io.some10 >= 0 && io.full10 >= 0) psi_text(body, sizeof body, &b, "io", &io, prev ? &pio : NULL, secs);
    int linux_like = now->swap_total_mb >= 0 && now->avail_mb >= 0;
    if (linux_like && mem.some10 < 0 && io.some10 < 0) put(body, sizeof body, &b, "PSI not available");
    if (linux_like)
        put(body, sizeof body, &b, "%sMemAvailable %.0f of %.0f MB; SwapFree %.0f of %.0f MB", b ? "; " : "", now->avail_mb,
            now->total_mb, now->swap_free_mb, now->swap_total_mb);
    else if (now->avail_mb >= 0)   // Windows
        put(body, sizeof body, &b, "%sphysical memory available %.0f of %.0f MB; commit %.0f of %.0f MB", b ? "; " : "",
            now->avail_mb, now->total_mb, now->commit_used_mb, now->commit_limit_mb);
    if (!b) return 0;
    put(out, cap, &n, "system at %s: %s", hms, body);
    return (int)n;
}

void log_pressure_baseline(void) {
    if (!plat_mutex_trylock(&pr_lock)) return;
    plat_pressure(&pr_prev);
    pr_prev_ns = plat_now_ns();
    pr_have = 1;
    plat_mutex_unlock(&pr_lock);
}

int log_pressure_text(char *out, size_t cap) {
    if (cap) out[0] = 0;
    if (!plat_mutex_trylock(&pr_lock)) return 0;
    PlatPressure now;
    plat_pressure(&now);
    uint64_t t = plat_now_ns();
    char hms[16];
    log_stamp(hms, sizeof hms, 0);
    int n = pressure_format(out, cap, &now, pr_have ? &pr_prev : NULL, pr_have ? (double)(t - pr_prev_ns) / 1e9 : 0.0, hms);
    pr_prev = now, pr_prev_ns = t, pr_have = 1;
    plat_mutex_unlock(&pr_lock);
    return n;
}
