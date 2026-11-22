// runtime/diag/perf_bound.h: which side holds the frame back in the 60-frame window line (runtime/diag/perf.c perf_gpu_line).
// Built and run by tests/perf_bound_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "perf_bound.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *v(double iv, double pace, int have_gpu, double gpu, int have_idle, double starved, double wait) {
    PerfBoundIn in = {.interval_ms = iv, .pace_share = pace, .have_gpu = have_gpu, .gpu_ms = gpu,
                      .have_idle = have_idle, .starved_ms = starved, .wait_ms = wait};
    return perf_bound(&in);
}
#define IS(got, want, ...) CHECK(!strcmp(got, want), __VA_ARGS__)

int main(void) {
    // Kokiri Forest on an RX 6700 (RADV, 2026-11-19, 0.19.0-rc.2): 33.6 ms frames, GPU busy 22.5 ms, idle waiting for
    // the next submission 9.6 ms, the game's threads waiting 17 ms for GPU fences. The old rule said cpu.
    IS(v(33.6, 0.0, 1, 22.5, 1, 9.6, 17.0), "serial", "the measured Kokiri window is serial");
    // The same window without the idle split (no calibrated timestamps): the rule cannot see the turns, cpu as before.
    IS(v(33.6, 0.0, 1, 22.5, 0, 0.0, 17.0), "cpu", "without the idle split it stays cpu");
    // A full GPU wins over serial.
    IS(v(20.0, 0.0, 1, 17.0, 1, 4.5, 15.0), "gpu", "GPU busy 85%% is gpu even with turns");
    // Pacing wins over everything.
    IS(v(16.7, 0.2, 1, 15.0, 1, 5.0, 10.0), "pace", "20%% pacing is pace");
    // The CPU alone: the GPU starves but nothing waits for it.
    IS(v(30.0, 0.0, 1, 10.0, 1, 18.0, 2.0), "cpu", "GPU starved, threads hardly wait: cpu");
    // Threads wait for the GPU but the GPU always had work queued: not taking turns, and not full: cpu.
    IS(v(30.0, 0.0, 1, 22.0, 1, 2.0, 15.0), "cpu", "fence waits without a starved GPU stay cpu");
    // The edges are inclusive: a fifth starved and a quarter waiting.
    IS(v(40.0, 0.0, 1, 20.0, 1, 8.0, 10.0), "serial", "8 of 40 starved and 10 of 40 waiting is serial");
    IS(v(40.0, 0.0, 1, 20.0, 1, 7.9, 10.0), "cpu", "just under a fifth starved is cpu");
    IS(v(40.0, 0.0, 1, 20.0, 1, 8.0, 9.9), "cpu", "just under a quarter waiting is cpu");
    IS(v(40.0, 0.0, 1, 32.0, 1, 8.0, 10.0), "gpu", "80%% busy is gpu");
    // Without timestamps the fence waits decide, as before.
    IS(v(30.0, 0.0, 0, 0.0, 0, 0.0, 15.0), "gpu", "no timestamps, waits half the interval: gpu");
    IS(v(30.0, 0.0, 0, 0.0, 0, 0.0, 14.9), "cpu", "no timestamps, waits under half: cpu");
    // An idle split with no interval (the first window of a run): no division by zero, cpu.
    IS(v(0.0, 0.0, 1, 0.0, 1, 0.0, 0.0), "gpu", "an empty window with timestamps reads gpu (0 >= 0), as before");
    if (fails) printf("%d checks failed\n", fails);
    else printf("perf_bound_check: all checks passed\n");
    return fails != 0;
}
