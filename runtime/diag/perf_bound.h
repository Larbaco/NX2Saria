#pragma once

#define PERF_PACE_SHARE 0.15
#define PERF_GPU_BUSY_SHARE 0.8
#define PERF_FENCE_SHARE 0.5
#define PERF_STARVED_SHARE 0.2
#define PERF_SERIAL_WAIT_SHARE 0.25

typedef struct {
    double interval_ms;
    double pace_share;
    int have_gpu;
    double gpu_ms;
    int have_idle;
    double starved_ms;
    double wait_ms;
} PerfBoundIn;

static inline const char *perf_bound(const PerfBoundIn *in) {
    double iv = in->interval_ms;
    if (in->pace_share >= PERF_PACE_SHARE) return "pace";
    if (!in->have_gpu) return in->wait_ms >= PERF_FENCE_SHARE * iv ? "gpu" : "cpu";
    if (in->gpu_ms >= PERF_GPU_BUSY_SHARE * iv) return "gpu";
    if (in->have_idle && iv > 0 && in->starved_ms >= PERF_STARVED_SHARE * iv && in->wait_ms >= PERF_SERIAL_WAIT_SHARE * iv)
        return "serial";
    return "cpu";
}
