#include <assert.h>
#include <stdio.h>

#include "host.h"
#include "layout.h"
#include "simd.h"

void animation_loop_host(Cpu *c);

static float render_step = 0.5f;
static unsigned clock_calls, clock_logs, pose_logs;
static uint8_t pool[0x4c * 2], container[0x190], primary[0x20], secondary[0x20], pose[12];
static uint64_t addr(void *p) { return (uint64_t)(uintptr_t)p; }
float frame_step_frames(void) { return render_step; }
uint32_t frame_rate_animation_frames(uint64_t entry, float step) {
    assert(entry == addr(pool + 0x4c) && step == 0.5f);
    return ++clock_calls % 2 == 0;
}

void rt_log(const char *fmt, ...) {
    if (strstr(fmt, "loop clock")) clock_logs++;
    if (strstr(fmt, "loop pose")) pose_logs++;
}

static void desc_init(uint64_t desc) {
    ST16(desc + 0x18, 0x40);
    ST16(desc + 0x1a, 1);
    ST8(desc + 0x1c, 0);
}

static void verify_pose(Cpu *c, float phase, float expected) {
    FP_SETS(0, phase);
    animation_loop_host(c);
    assert(LD32(addr(pose)) == fb_s(expected) && c->v[0] == fb_s(phase));
}

int main(void) {
    desc_init(addr(primary));
    desc_init(addr(secondary));
    ST32(addr(container) + 0x180, 2);
    ST64(addr(container) + 0x188, addr(pool));
    uint64_t entry = addr(pool + 0x4c);
    ST32(entry, 2);
    ST32(entry + 0x14, fb_s(1.0f));
    ST32(addr(pose) + 8, fb_s(1.0f));

    Cpu cpu = {0};
    Cpu *c = &cpu;
    c->x[20] = entry;
    c->x[22] = addr(primary);
    c->x[24] = addr(container);
    c->x[18] = addr(secondary);
    c->x[17] = addr(container);
    FP_SETS(8, 0.5f);
    FP_SETS(1, 2.0f);
    c->x[30] = MOD_MAIN_BASE + 0x2afe08;
    animation_loop_host(c);
    assert(c->v[2] == fb_s(0.0f) && c->v[1] == fb_s(2.0f));
    FP_SETS(0, 3.0f);
    c->x[30] = MOD_MAIN_BASE + 0x2b01d4;
    animation_loop_host(c);
    assert(c->v[1] == fb_s(3.0f) && c->v[0] == fb_s(3.0f));

    ST16(addr(secondary) + 0x1a, 2);
    animation_loop_host(c);
    assert(c->v[1] == fb_s(1.5f) && clock_calls == 2);
    desc_init(addr(secondary));
    ST32(entry, 0);
    animation_loop_host(c);
    assert(c->v[1] == fb_s(1.5f) && clock_calls == 2);
    ST32(entry, 2);
    render_step = 1.0f;
    animation_loop_host(c);
    assert(c->v[1] == fb_s(1.5f) && clock_calls == 2);
    render_step = 0.5f;
    for (int i = 0; i < 12; i++) animation_loop_host(c);
    assert(clock_calls == 14 && clock_logs == 4);

    c->x[22] = addr(pose);
    c->x[26] = addr(primary);
    c->x[27] = addr(container);
    c->x[30] = MOD_MAIN_BASE + 0x924e54;
    for (int i = 0; i < 12; i++) verify_pose(c, 0.75f, 0.0f);
    assert(pose_logs == 4);
    ST32(entry, 0);
    verify_pose(c, 0.75f, 0.75f);
    ST32(entry, 2);
    ST32(entry + 0x14, fb_s(2.0f));
    verify_pose(c, 0.75f, 0.75f);
    ST32(entry + 0x14, fb_s(1.0f));
    ST16(addr(primary) + 0x18, 0);
    verify_pose(c, 0.75f, 0.75f);
    desc_init(addr(primary));
    verify_pose(c, 1.0f, 1.0f);
    render_step = 1.0f;
    verify_pose(c, 0.75f, 0.75f);
    puts("animation_loop_check: clock and pose keep whole updates for verified single-frame loops");
    return 0;
}
