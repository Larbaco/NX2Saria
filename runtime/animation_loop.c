// Single-frame loops in the 1.1.1 animation clock and pose update (recomp/patches-v111.txt, ozora).
#include "host.h"
#include "layout.h"
#include "simd.h"
#include <stdatomic.h>

// The descriptor and container identify the same animation clock entry in the update and draw paths.
// Each clock has 0x4c bytes. A signed descriptor index outside the container's list is a fallback in
// the game, so it does not identify the intended animation.
static uint64_t loop_entry(uint64_t desc, uint64_t container) {
    if (!desc || !container || !(LD16(desc + 0x18) & 0x40) || (int8_t)LD8(desc + 0x1c) < 0) return 0;
    unsigned index = LD16(desc + 0x1a);
    if (index >= 0x8000 || index >= LD32(container + 0x180)) return 0;
    uint64_t entries = LD64(container + 0x188);
    return entries ? entries + 0x4c * index : 0;
}

static int single_frame(uint64_t entry) {
    // The clock wraps only with flag bit 1. The frozen pose loops over [0, 1].
    return entry && (LD32(entry) & 2) && LD32(entry + 0x10) == 0 && LD32(entry + 0x14) == 0x3f800000u;
}

static void clock_advance(Cpu *c, unsigned out, unsigned speed, uint64_t desc, uint64_t container) {
    float step = VSF32(8);
    uint64_t entry = c->x[20];
    if (step > 0.0f && step < 1.0f && single_frame(entry) &&
        loop_entry(desc, container) == entry && frame_step_frames() < 1.0f) {
        uint32_t whole = frame_rate_animation_frames(entry, step);
        FP_SETS(out, op_s(c, FOP_MUL, (float)whole, VSF32(speed), 0.0f));
        static _Atomic unsigned logged;
        if (atomic_load_explicit(&logged, memory_order_relaxed) < 4) {
            unsigned n = atomic_fetch_add_explicit(&logged, 1, memory_order_relaxed);
            if (n < 4) rt_log("frame rate: single-frame loop clock #%u: step %.6f -> %u", n + 1, step, whole);
        }
        return;
    }
    fp_op_s(c, FOP_MUL, out, 8, speed);
}

static void pose_store(Cpu *c) {
    uint64_t pose = c->x[22];
    uint32_t time = VS32(0);
    float phase = VSF32(0);
    // The pose pass has its own clock. Confirm its range and the looping source animation before
    // setting a fractional pose phase to the first frame. The guest also still uses s0 after this store.
    if (phase > 0.0f && phase < 1.0f && LD32(pose + 4) == 0 && LD32(pose + 8) == 0x3f800000u &&
        frame_step_frames() < 1.0f && single_frame(loop_entry(c->x[26], c->x[27]))) {
        time = 0;
        static _Atomic unsigned logged;
        if (atomic_load_explicit(&logged, memory_order_relaxed) < 4) {
            unsigned n = atomic_fetch_add_explicit(&logged, 1, memory_order_relaxed);
            if (n < 4) rt_log("frame rate: single-frame loop pose #%u: phase %.6f -> 0", n + 1, phase);
        }
    }
    ST32(pose, time);
}

void animation_loop_host(Cpu *c) {
    switch (c->x[30] - MOD_MAIN_BASE) {
    case 0x2afe08: clock_advance(c, 2, 1, c->x[22], c->x[24]); break;
    case 0x2b01d4: clock_advance(c, 1, 0, c->x[18], c->x[17]); break;
    case 0x924e54: pose_store(c); break;
    }
}
