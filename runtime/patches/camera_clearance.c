#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "camera_clearance.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#define SITE_A 0x71a844ull
#define SITE_B 0x71b624ull
#define WORD_A 0xbd451e6au
#define WORD_B 0xbd451e6eu
#define HOST_SLOT 0xe00

enum { K_BASE, K_PICTURE, K_OTHER, NK };
static int set_index = -1;
static _Atomic uint64_t seen[2][NK], replaced[2];
static _Atomic int told[2];

static float page_f32(int off) {
    const uint8_t *page = rt_patch_page();
    float f = 0;
    if (page) memcpy(&f, page + off, 4);
    return f;
}

static int near_ratio(float a, float b, float rel) { return isfinite(a) && b > 0 && fabsf(a - b) <= rel * b; }

static void camera_clearance_host(Cpu *c) {
    const uint64_t at = c->x[30] - MOD_MAIN_BASE - 4;
    const int site = at == SITE_A ? 0 : at == SITE_B ? 1 : -1;
    if (site < 0) return;
    const int reg = site ? 14 : 10;
    uint32_t bits = (uint32_t)c->v[reg];
    float v;
    memcpy(&v, &bits, 4);
    const float base = page_f32(112), picture = page_f32(116);
    const int wide = !near_ratio(picture, base, 0.01f);
    int k = near_ratio(v, base, 0.001f) ? K_BASE : near_ratio(v, picture, 0.001f) ? K_PICTURE : K_OTHER;
    seen[site][k]++;
    if (!atomic_exchange(&told[site], 1))
        rt_log("camera clearance: the first ratio at main+%#llx is %.5f (the picture's %.5f, 16:9 %.5f)%s",
               (unsigned long long)at, v, picture, base,
               k == K_PICTURE && wide ? ": the picture's ratio, made 16:9 for the clearance" : "");
    if (k != K_PICTURE || !wide) return;
    memcpy(&bits, &base, 4);
    c->v[reg] = bits;
    replaced[site]++;
}

void camera_clearance_start(void) {
    set_index = rt_patch_set("camera_clearance");
    if (set_index < 0) return;
    static const struct { uint64_t off; uint32_t word; } sites[] = {{SITE_A, WORD_A}, {SITE_B, WORD_B}};
    for (int i = 0; i < 2; i++) {
        const uint64_t a = MOD_MAIN_BASE + sites[i].off;
        if (mem_readable(a, 4) && *(const uint32_t *)(uintptr_t)a != sites[i].word && rt_patch_enabled(set_index)) {
            rt_patch_switch(set_index, 0);
            rt_log("camera clearance: main+%#llx holds %08x where 1.1.1 has %08x: the set camera_clearance stays off",
                   (unsigned long long)sites[i].off, *(const uint32_t *)(uintptr_t)a, sites[i].word);
        }
    }
    rt_log("camera clearance: %s", rt_patch_enabled(set_index)
                                       ? "on: a camera clearance sized from a wide picture's ratio is sized from 16:9 "
                                         "(patch set camera_clearance; NX2SARIA_CAMERA_CLEARANCE_FIX=0 turns it off)"
                                       : "off (NX2SARIA_CAMERA_CLEARANCE_FIX=0): the camera's clearance as shipped");
}

void camera_clearance_exit_log(void) {
    if (set_index < 0) return;
    for (int s = 0; s < 2; s++)
        rt_log("camera clearance: this session at main+%#llx: %llu loads of 16:9, %llu of the picture's ratio (%llu made "
               "16:9), %llu of other ratios", (unsigned long long)(s ? SITE_B : SITE_A),
               (unsigned long long)seen[s][K_BASE], (unsigned long long)seen[s][K_PICTURE],
               (unsigned long long)replaced[s], (unsigned long long)seen[s][K_OTHER]);
}

#ifdef RT_PATCH_PAGE
__attribute__((constructor)) static void camera_clearance_register(void) {
    if (!RT_V111) return;
    rt_register_host(RT_PATCH_PAGE + HOST_SLOT, camera_clearance_host);
}
#endif
