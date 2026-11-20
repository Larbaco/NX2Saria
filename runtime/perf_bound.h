// Which side holds the frame back, for the second line of the 60-frame window ("perf: frame N: ... bound: X",
// perf.c perf_gpu_line). Header only, so that tests/perf_bound_check.c checks the rule without a game run.
//
//   pace    the frame waited for pacing (vsync, the frame limiter) at least 15% of the window
//   gpu     the GPU was busy for 80% of the frame interval or more (without timestamps: threads waited for fences for
//           half the interval or more)
//   serial  the GPU and the CPU took turns: the GPU sat idle waiting for the next submission for a fifth of the
//           interval or more, and threads waited for GPU fences for a quarter of it or more. Neither side is full,
//           and the frame is close to the sum of the two. A cheaper GPU frame or a cheaper replay both shorten it.
//   cpu     otherwise
//
// Why serial is its own verdict: standing in Kokiri Forest on an RX 6700 (RADV, 2026-11-19) the window line said cpu
// in every window while, per frame of 33 ms, the GPU was busy 22.5 ms and idle 11 ms (9.6 ms of it waiting for the
// next submission) and the game's threads waited 17 ms for GPU fences. The game waits after a frame's first
// submission (vk_sync.c, "the executed timeline"), and that small submission is queued behind the frame before's large
// one, so the CPU's work and the GPU's do not overlap. Calling that frame cpu-bound sends a reader to the replay alone,
// and calling it gpu-bound to the resolution alone.
#pragma once

#define PERF_PACE_SHARE 0.15       // pacing share of the window for pace
#define PERF_GPU_BUSY_SHARE 0.8    // GPU busy share of the interval for gpu
#define PERF_FENCE_SHARE 0.5       // without timestamps: fence waits for gpu
#define PERF_STARVED_SHARE 0.2     // GPU idle waiting for the next submission, for serial
#define PERF_SERIAL_WAIT_SHARE 0.25   // threads' GPU fence waits, for serial

typedef struct {
    double interval_ms;   // the frame interval over the window
    double pace_share;    // the share of the window spent in pacing waits
    int have_gpu;         // GPU busy time from timestamps
    double gpu_ms;        // GPU busy per frame
    int have_idle;        // the GPU's idle time split by calibrated timestamps
    double starved_ms;    // GPU idle per frame with no submission queued (waiting for the CPU)
    double wait_ms;       // the render thread's and the other threads' GPU fence waits per frame
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
