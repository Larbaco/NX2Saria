#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <locale.h>
#include <pthread.h>
#endif

#include "log_stall.c"
#include "platform/proc_pressure.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SAME(got, want) CHECK(!strcmp(got, want), "got \"%s\", wanted \"%s\"", got, want)

static uint64_t now_ns, wall_ns;
static long zone_west = 3 * 3600;
static int zone_ok = 1;
static int lock_busy;
static PlatPressure next_sample;
static int samples_taken;

uint64_t plat_now_ns(void) { return now_ns; }
uint64_t plat_unix_time_ns(void) { return wall_ns; }
int plat_localtime(int64_t unix_s, struct tm *out) {
    if (!zone_ok) return 0;
    time_t t = (time_t)(unix_s - zone_west);
#ifdef _WIN32
    return !gmtime_s(out, &t);
#else
    return gmtime_r(&t, out) != NULL;
#endif
}
int plat_mutex_trylock(PlatMutex *m) {
    (void)m;
    return !lock_busy;
}
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void plat_pressure(PlatPressure *p) {
    *p = next_sample;
    samples_taken++;
}

#define T_2026_11_21_155955 1795276795ll
#define T_2026_11_21_030001 1795230001ll
static void at_utc(int64_t unix_s, uint64_t frac_ns) {
    wall_ns = (uint64_t)unix_s * 1000000000ull + frac_ns;
    now_ns = 5000ull * 1000000000ull;
}

static void test_psi(void) {
    double avg = -1;
    uint64_t us = 77;
    const char *both = "some avg10=1.23 avg60=0.50 avg300=0.10 total=123456\nfull avg10=0.04 avg60=0.01 avg300=0.00 total=7890\n";
    CHECK(psi_line(both, "some", &avg, &us) && avg > 1.2299 && avg < 1.2301 && us == 123456, "some: %f %llu", avg, (unsigned long long)us);
    CHECK(psi_line(both, "full", &avg, &us) && avg > 0.0399 && avg < 0.0401 && us == 7890, "full: %f %llu", avg, (unsigned long long)us);
    CHECK(psi_line("some avg10=100.00 avg60=99.50 avg300=40.10 total=9999999999\n", "some", &avg, &us) && avg == 100.0 &&
          us == 9999999999ull, "full scale and a total over 32 bits: %f %llu", avg, (unsigned long long)us);
    CHECK(psi_line("some avg10=0.5 avg60=0 avg300=0 total=1\n", "some", &avg, &us) && avg == 0.5, "one decimal: %f", avg);
    CHECK(psi_line("some avg10=7 avg60=0 avg300=0 total=1\n", "some", &avg, &us) && avg == 7.0, "no decimals: %f", avg);
    CHECK(psi_line("some avg10=0.00 avg60=0.00 avg300=0.00 total=0", "some", &avg, &us) && avg == 0 && us == 0, "no newline at the end");

    avg = -1, us = 77;
    CHECK(!psi_line("some avg10=1.00 avg60=0.00 avg300=0.00 total=5\n", "full", &avg, &us) && avg == -1 && us == 77,
          "a cpu file of an older kernel has no full line");
    CHECK(!psi_line("", "some", &avg, &us) && !psi_line("\n\n", "some", &avg, &us), "empty text");
    CHECK(!psi_line("some avg10= avg60=0 total=5\n", "some", &avg, &us), "no digits in avg10");
    CHECK(!psi_line("some avg10=1.00 avg60=0 total=\n", "some", &avg, &us), "no digits in total");
    CHECK(!psi_line("some avg10=1.00 avg60=0\nfull avg10=0.00 total=5\n", "some", &avg, &us),
          "the total of the next line is not this line's");
    CHECK(!psi_line("somewhat avg10=1.00 total=5\n", "some", &avg, &us), "a longer word is not the kind");
    CHECK(!psi_line("some\n", "some", &avg, &us) && avg == -1 && us == 77, "the kind alone");
    CHECK(!psi_line("xsome avg10=1.00 total=5\n", "some", &avg, &us), "the kind in the middle of a line");
#ifndef _WIN32
    if (setlocale(LC_NUMERIC, "de_DE.UTF-8") || setlocale(LC_NUMERIC, "de_DE") || setlocale(LC_NUMERIC, "pt_BR.UTF-8")) {
        CHECK(psi_line(both, "some", &avg, &us) && avg > 1.2299 && avg < 1.2301, "a comma locale: %f", avg);
        setlocale(LC_NUMERIC, "C");
    }
#endif
}

static void test_meminfo(void) {
    const char *text = "MemTotal:       32698492 kB\nMemFree:         1054220 kB\nMemAvailable:   20835692 kB\n"
                       "Buffers:          912308 kB\nSwapCached:         1224 kB\nSwapTotal:       2097148 kB\nSwapFree:        1835012 kB\n"
                       "HugePages_Free:        0\n";
    uint64_t kb = 0;
    CHECK(meminfo_kb(text, "MemTotal:", &kb) && kb == 32698492, "MemTotal %llu", (unsigned long long)kb);
    CHECK(meminfo_kb(text, "MemAvailable:", &kb) && kb == 20835692, "MemAvailable %llu", (unsigned long long)kb);
    CHECK(meminfo_kb(text, "SwapTotal:", &kb) && kb == 2097148, "SwapTotal %llu", (unsigned long long)kb);
    CHECK(meminfo_kb(text, "SwapFree:", &kb) && kb == 1835012, "SwapFree %llu, not SwapCached's", (unsigned long long)kb);
    CHECK(meminfo_kb(text, "HugePages_Free:", &kb) && kb == 0, "a figure without a unit");
    kb = 99;
    CHECK(!meminfo_kb(text, "Free:", &kb) && !meminfo_kb(text, "Available:", &kb) && kb == 99, "a key matches from the start of a line only");
    CHECK(!meminfo_kb("MemTotal:\n", "MemTotal:", &kb) && !meminfo_kb("", "MemTotal:", &kb), "no figure, no text");
    CHECK(meminfo_kb("MemTotal:\t 5 kB", "MemTotal:", &kb) && kb == 5, "a tab, no newline at the end");
}

static void test_stamp(void) {
    char out[16];
    at_utc(T_2026_11_21_155955, 400000000ull);
    log_stamp(out, sizeof out, 0);
    SAME(out, "12:59:55");
    log_stamp(out, sizeof out, now_ns);
    SAME(out, "12:59:55");
    log_stamp(out, sizeof out, now_ns - 4500000000ull);
    SAME(out, "12:59:50");
    log_stamp(out, sizeof out, now_ns - 65ull * 1000000000ull);
    SAME(out, "12:58:50");
    log_stamp(out, sizeof out, now_ns + 1000000000ull);
    SAME(out, "12:59:55");
    at_utc(T_2026_11_21_030001, 0);
    log_stamp(out, sizeof out, 0);
    SAME(out, "00:00:01");
    log_stamp(out, sizeof out, now_ns - 2000000000ull);
    SAME(out, "23:59:59");
    zone_ok = 0;
    log_stamp(out, sizeof out, 0);
    SAME(out, "--:--:--");
    zone_ok = 1;
    at_utc(T_2026_11_21_155955, 0);
    char small[4] = "abc";
    log_stamp(small, sizeof small, 0);
    CHECK(small[3] == 0 && !strncmp(small, "12:", 3), "a buffer of 4: \"%s\"", small);
    log_stamp(small, 0, 0);
    zone_west = -(5 * 3600 + 1800);
    log_stamp(out, sizeof out, 0);
    SAME(out, "21:29:55");
    zone_west = 3 * 3600;
}

#ifndef _WIN32
#define THREADS 8
#define PER_THREAD 20000
static void *hammer(void *arg) {
    int wrote = 0;
    for (int i = 0; i < PER_THREAD; i++) wrote += log_event(LS_ACQUIRE_FAILED);
    *(int *)arg = wrote;
    return NULL;
}
#endif

static void test_events(void) {
    char text[512];
    CHECK(log_events_text(text, sizeof text, 0) == 0 && !text[0], "nothing counted yet");
    CHECK(log_events_text(text, sizeof text, 1) == 0 && !text[0], "nothing in the whole run yet");

    int wrote = 0;
    for (int i = 0; i < 7; i++) wrote += log_event(LS_ACQUIRE_FAILED);
    CHECK(wrote == 5, "acquire failures: %d of 7 have a line, 5 expected", wrote);
    wrote = 0;
    for (int i = 0; i < 4; i++) wrote += log_event(LS_FG_WATCHDOG);
    CHECK(wrote == 3, "watchdog firings: %d of 4 have a line, 3 expected", wrote);
    wrote = 0;
    for (int i = 0; i < 132; i++) wrote += log_event(LS_MAP_PHYS_FAILED);
    CHECK(wrote == 1, "MapPhysicalMemory failures: %d of 132 have a line, 1 expected", wrote);
    CHECK(log_event(LS_UNMAP_PHYS_FAILED) == 1 && log_event(LS_UNMAP_PHYS_FAILED) == 0, "the first unmap failure has its line, the second none");
    CHECK(!log_event((LsEvent)99) && !log_event((LsEvent)-1), "an event that does not exist is not counted");

    int n = log_events_text(text, sizeof text, 0);
    SAME(text, "presenter acquire failed 7 (only the first 5 logged), frame generation watchdog fired 4 (only the first 3 logged), "
               "MapPhysicalMemory failed 132 (only the first logged), UnmapPhysicalMemory failed 2 (only the first logged)");
    CHECK(n == (int)strlen(text), "the length returned (%d) is the text's (%zu)", n, strlen(text));

    CHECK(log_events_text(text, sizeof text, 0) == 0 && !text[0], "no events since the last window: \"%s\"", text);
    for (int i = 0; i < 3; i++) log_event(LS_ACQUIRE_FAILED);
    for (int i = 0; i < 12; i++) log_event(LS_MAP_PHYS_FAILED);
    log_events_text(text, sizeof text, 0);
    SAME(text, "presenter acquire failed 3 (10 in all, only the first 5 logged), MapPhysicalMemory failed 12 (144 in all, only the first logged)");
    log_event(LS_FG_WATCHDOG);
    log_events_text(text, sizeof text, 1);
    SAME(text, "presenter acquire failed 10 (only the first 5 logged), frame generation watchdog fired 5 (only the first 3 logged), "
               "MapPhysicalMemory failed 144 (only the first logged), UnmapPhysicalMemory failed 2 (only the first logged)");
    log_events_text(text, sizeof text, 0);
    SAME(text, "frame generation watchdog fired 1 (5 in all, only the first 3 logged)");

    char small[24];
    n = log_events_text(small, sizeof small, 1);
    CHECK(n < (int)sizeof small && n == (int)strlen(small), "a text cut to the buffer: %d, \"%s\"", n, small);
    CHECK(log_events_text(small, 0, 1) == 0, "no buffer");

    memset(ev_total, 0, sizeof ev_total);
    memset(ev_seen, 0, sizeof ev_seen);
    log_event(LS_ACQUIRE_FAILED), log_event(LS_ACQUIRE_FAILED);
    log_events_text(text, sizeof text, 0);
    SAME(text, "presenter acquire failed 2");
    log_event(LS_ACQUIRE_FAILED);
    log_events_text(text, sizeof text, 0);
    SAME(text, "presenter acquire failed 1 (3 in all)");
    for (int i = 0; i < 3; i++) log_event(LS_ACQUIRE_FAILED);
    log_events_text(text, sizeof text, 0);
    SAME(text, "presenter acquire failed 3 (6 in all, only the first 5 logged)");
    log_events_text(text, sizeof text, 1);
    SAME(text, "presenter acquire failed 6 (only the first 5 logged)");
    memset(ev_total, 0, sizeof ev_total);
    memset(ev_seen, 0, sizeof ev_seen);

#ifndef _WIN32
    pthread_t th[THREADS];
    int wrote_by[THREADS], lines = 0;
    uint64_t before = __atomic_load_n(&ev_total[LS_ACQUIRE_FAILED], __ATOMIC_RELAXED);
    for (int i = 0; i < THREADS; i++) pthread_create(&th[i], NULL, hammer, &wrote_by[i]);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL), lines += wrote_by[i];
    uint64_t total = __atomic_load_n(&ev_total[LS_ACQUIRE_FAILED], __ATOMIC_RELAXED);
    CHECK(total == before + THREADS * PER_THREAD, "%d threads counted %llu of %d", THREADS, (unsigned long long)(total - before),
          THREADS * PER_THREAD);
    CHECK(lines == 5, "the threads were told to write %d lines between them, the limit is 5", lines);
#endif
}

static PlatPressure linux_sample(double mem_some, double mem_full, uint64_t mem_some_us, uint64_t mem_full_us, double io_some,
                                 double io_full, uint64_t io_some_us, uint64_t io_full_us) {
    PlatPressure p;
    plat_pressure_clear(&p);
    p.mem_some10 = mem_some, p.mem_full10 = mem_full, p.mem_some_us = mem_some_us, p.mem_full_us = mem_full_us;
    p.io_some10 = io_some, p.io_full10 = io_full, p.io_some_us = io_some_us, p.io_full_us = io_full_us;
    p.avail_mb = 20345.4, p.total_mb = 31932.0, p.swap_free_mb = 1802.0, p.swap_total_mb = 2048.0;
    return p;
}

static void test_pressure(void) {
    char text[700];
    PlatPressure before = linux_sample(0.0, 0.0, 1000000, 500000, 0.5, 0.1, 40000000, 20000000);
    PlatPressure now = linux_sample(3.25, 1.1, 1052000, 500000, 12.0, 8.5, 41840000, 21210000);
    int n = pressure_format(text, sizeof text, &now, &before, 60.04, "12:59:55");
    SAME(text, "system at 12:59:55: PSI memory avg10 some 3.2% full 1.1% (stalled in the last 60 s: some 52 ms, full 0 ms); "
               "PSI io avg10 some 12.0% full 8.5% (stalled in the last 60 s: some 1840 ms, full 1210 ms); "
               "MemAvailable 20345 of 31932 MB; SwapFree 1802 of 2048 MB");
    CHECK(n == (int)strlen(text), "the length returned (%d) is the text's (%zu)", n, strlen(text));

    pressure_format(text, sizeof text, &now, NULL, 0, "12:59:55");
    SAME(text, "system at 12:59:55: PSI memory avg10 some 3.2% full 1.1%; PSI io avg10 some 12.0% full 8.5%; "
               "MemAvailable 20345 of 31932 MB; SwapFree 1802 of 2048 MB");

    PlatPressure odd = linux_sample(0.0, 0.0, 5, 5, 0.0, 0.0, 5, 5);
    pressure_format(text, sizeof text, &odd, &now, 60, "12:59:55");
    CHECK(strstr(text, "some 0 ms, full 0 ms") != NULL && strstr(text, "-") == NULL, "totals that fell: %s", text);

    PlatPressure nopsi = linux_sample(-1, -1, 0, 0, -1, -1, 0, 0);
    pressure_format(text, sizeof text, &nopsi, &before, 60, "12:59:55");
    SAME(text, "system at 12:59:55: PSI not available; MemAvailable 20345 of 31932 MB; SwapFree 1802 of 2048 MB");
    PlatPressure memonly = linux_sample(1.0, 0.5, 2000000, 600000, -1, -1, 0, 0);
    pressure_format(text, sizeof text, &memonly, &before, 60, "12:59:55");
    SAME(text, "system at 12:59:55: PSI memory avg10 some 1.0% full 0.5% (stalled in the last 60 s: some 1000 ms, full 100 ms); "
               "MemAvailable 20345 of 31932 MB; SwapFree 1802 of 2048 MB");

    PlatPressure win;
    plat_pressure_clear(&win);
    win.avail_mb = 9800, win.total_mb = 16384, win.commit_used_mb = 21000, win.commit_limit_mb = 27000;
    pressure_format(text, sizeof text, &win, &before, 60, "09:01:02");
    SAME(text, "system at 09:01:02: physical memory available 9800 of 16384 MB; commit 21000 of 27000 MB");

    PlatPressure none;
    plat_pressure_clear(&none);
    CHECK(pressure_format(text, sizeof text, &none, NULL, 0, "12:59:55") == 0 && !text[0], "no figures, no line: \"%s\"", text);
    char small[40];
    n = pressure_format(small, sizeof small, &now, &before, 60, "12:59:55");
    CHECK(n < (int)sizeof small && n == (int)strlen(small), "a text cut to the buffer: %d", n);
    CHECK(pressure_format(small, 0, &now, &before, 60, "12:59:55") == 0, "no buffer");

    at_utc(T_2026_11_21_155955, 0);
    samples_taken = 0;
    next_sample = before;
    now_ns = 100ull * 1000000000ull;
    log_pressure_baseline();
    CHECK(samples_taken == 1, "the baseline takes one sample (%d)", samples_taken);
    next_sample = now;
    now_ns = 160ull * 1000000000ull;
    n = log_pressure_text(text, sizeof text);
    CHECK(n > 0 && strstr(text, "system at 12:59:55: PSI memory avg10 some 3.2% full 1.1% (stalled in the last 60 s: some 52 ms, full 0 ms)") == text,
          "the line against the baseline: %s", text);
    next_sample = now;
    now_ns = 220ull * 1000000000ull;
    log_pressure_text(text, sizeof text);
    CHECK(strstr(text, "(stalled in the last 60 s: some 0 ms, full 0 ms)") != NULL, "the next line counts from the last one: %s", text);
    lock_busy = 1;
    int taken = samples_taken;
    CHECK(log_pressure_text(text, sizeof text) == 0 && !text[0] && samples_taken == taken, "a busy lock: no line, no sample");
    log_pressure_baseline();
    CHECK(samples_taken == taken, "a busy lock: no baseline sample");
    lock_busy = 0;
}

int main(void) {
    test_psi();
    test_meminfo();
    test_stamp();
    test_events();
    test_pressure();
    if (fails) {
        printf("log_stall_check: %d checks failed\n", fails);
        return 1;
    }
    printf("log_stall_check: passed\n");
    return 0;
}
