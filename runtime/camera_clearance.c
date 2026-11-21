// The camera's clearance from walls on pictures wider or narrower than 16:9 (Ocarina of Time 1.1.1, the patch set
// camera_clearance, recomp/patches-v111.txt).
//
// The camera's collision step (main+0x71a4a0) sizes the sphere it keeps clear of walls from its near plane:
// h = tan(fov / 2) * near, w = h * ratio and sqrt(w * w + h * h), plus a margin, at least a minimum (main+0x71a84c..
// and its second path at main+0x71b620..). The ratio is a camera parameter it loads from its object's +0x51c at
// main+0x71a844 (into s10) and main+0x71b624 (into s14), copied there from the camera's parameters (main+0x478798).
// Where the game's camera works with the picture's own ratio, a 32:9 picture makes the sphere about 1.8 times its 16:9
// size, and the camera cannot follow Link into a crawl passage.
//
// The sites keep their load and then call the page's +0xe00 (camera_clearance_host). When the picture is not 16:9
// (the page's +116, widescreen.c) and the loaded value is that picture's ratio (within 0.1%), it is replaced by 16:9
// (the page's +112) for this sum alone; any other value stays. The register written is the load's own, which the code
// after it reads; nothing reads the flags between the sites and the call that follows each (main+0x71a878,
// main+0x71b64c), and x30 is the function's own, saved in its prologue.
//
// The log counts the loads by what they held (16:9, the picture's ratio, other), so a session shows whether the game's
// camera ever sees the wide ratio here: the fork's own widescreen keeps the camera's parameters at 16:9 and widens
// only the projection (the set wide_aspect), so with it the sites are expected to see 16:9.
//
// NX2SARIA_CAMERA_CLEARANCE_FIX=0 turns the set off.
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

#define SITE_A 0x71a844ull     // ldr s10, [x19, #0x51c]
#define SITE_B 0x71b624ull     // ldr s14, [x19, #0x51c]
#define WORD_A 0xbd451e6au
#define WORD_B 0xbd451e6eu
#define HOST_SLOT 0xe00        // patch page slot (runtime/patches.c lists the others)

enum { K_BASE, K_PICTURE, K_OTHER, NK };
static int set_index = -1;
static _Atomic uint64_t seen[2][NK], replaced[2];
static _Atomic int told[2];

static float page_f32(int off) {   // 0 without the page (no patch table: the sites are not there either)
    const uint8_t *page = rt_patch_page();
    float f = 0;
    if (page) memcpy(&f, page + off, 4);
    return f;
}

// 1 when a and b differ by at most rel of b
static int near_ratio(float a, float b, float rel) { return isfinite(a) && b > 0 && fabsf(a - b) <= rel * b; }

static void camera_clearance_host(Cpu *c) {
    const uint64_t at = c->x[30] - MOD_MAIN_BASE - 4;
    const int site = at == SITE_A ? 0 : at == SITE_B ? 1 : -1;
    if (site < 0) return;   // (only the set's two sites call the slot)
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
    c->v[reg] = bits;   // (the upper lanes of a 32-bit load are zero, as the load leaves them)
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
