#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx_wsys.h"
#include "host.h"
#include "layout.h"
#include "session_summary.h"
#include "ui/menu.h"
#include "vk_dlaa.h"
#include "widescreen_math.h"

#define GAME_W 2560
#define GAME_H 1440
#define SCENE_H 1080
#define GAME_DTA_KIB 312320.0
#define SAVE_W 543
#define SAVE_H 306

static int screen_size(int *w, int *h) {
    *w = *h = 0;
    if (gfx_cfg_fullscreen()) return gfx_window_monitor_size(w, h) && *h > 0;
    gfx_cfg_window_size(w, h);
    return *h > 0;
}

static double wanted_ratio(void) {
    const char *s = cfg_value("NX2SARIA_GAME_ASPECT");
    if (!strcmp(s, "16:10")) return 16.0 / 10.0;
    if (!strcmp(s, "3:2")) return 3.0 / 2.0;
    if (!strcmp(s, "21:9")) return 64.0 / 27.0;
    if (!strcmp(s, "32:9")) return 32.0 / 9.0;
    if (strcmp(s, "window")) return 0;
    int w, h;
    if (!screen_size(&w, &h)) {
        rt_log("widescreen: the screen's size is unknown; the game's own 16:9");
        return 0;
    }
    return (double)w / h;
}

#define PICTURE_MIN_H 1080
static unsigned picture_height(void) {
    const char *s = cfg_value("NX2SARIA_GAME_RES");
    int w = 0, h = GAME_H, min = PICTURE_MIN_H;
    if (!strcmp(s, "window")) {
        if (!screen_size(&w, &h)) h = GAME_H;
        min = GAME_H;
    } else {
        h = atoi(s);
    }
    return h < min ? (unsigned)min : h > 4320 ? 4320 : (unsigned)h;
}

#define RES_FIXED_AFTER 120
static double dlss_share(void);
double wide_dlss_share(void) { return dlss_share(); }

#define PLAIN_SHARE 0.75

static double heap_share(void);

static int heap_unserved;

static double fit_share(void) {
    return heap_unserved ? PLAIN_SHARE : heap_share();
}

static double mode_share(void) {
    const char *m = dynres_override();
    if (!m) m = cfg_value("NX2SARIA_DLSS");
    return !strcmp(m, "quality") ? 2.0 / 3 : !strcmp(m, "balanced") ? 0.58 : !strcmp(m, "performance") ? 0.5
         : !strcmp(m, "ultra_performance") ? 1.0 / 3 : 1.0;
}

static int upscaler_runs(void) {
    return vk_dlaa_wanted() && cfg_bool("NX2SARIA_DLAA") && (vk_dlaa_fsr3() || vk_dlaa_available());
}

static double dlss_share(void) {
    if (!upscaler_runs()) return 0;
    double s = mode_share();
    if (s > fit_share()) {
        static int said;
        if (!said++)
            rt_log("widescreen: the scene is %.0f%% of the picture, not the mode's %.0f%%: the game's render-target heap "
                   "%s. A mode chosen in the menu gets its size after a restart, unless the start's log says the heap "
                   "is at its limit", fit_share() * 100, s * 100,
                   heap_unserved ? "is the shipped one (the start's log says why the grown one was not served)"
                                 : "was sized at the start for that much");
        s = fit_share();
    }
    return s;
}

static double plain_share(void) {
    const char *e = cfg_get("NX2SARIA_SCENE_SHARE");
    double s = e && *e ? atof(e) : PLAIN_SHARE;
    return s > 1 ? 1 : s;
}

static double scene_share(void) {
    double s = dlss_share(), plain = plain_share(), dyn = s > 0 ? 0 : dynres_share();
    if (dyn > 0 && dyn < plain) plain = dyn;
    return s > 0 ? s : plain > fit_share() ? fit_share() : plain;
}

double wide_dynres_base(void) {
    static int set = -2;
    if (set == -2) set = rt_patch_set("res_fixed");
    if (upscaler_runs()) return -1;
    double s = plain_share(), fit = fit_share();
    if (s != s) s = 0;
    return set < 0 || s <= 0 ? 0 : s > fit ? fit : s;
}

static double wanted_share(void) {
    return upscaler_runs() ? mode_share() : plain_share();
}

static int picture_size(unsigned *ow, unsigned *oh, int quiet);

static double picture_ratio(void) {
    unsigned ow, oh;
    if (!wide_available() || !picture_size(&ow, &oh, 1)) ow = GAME_W, oh = GAME_H;
    return (double)ow * oh / ((double)GAME_W * GAME_H);
}

static double share_most(void) {
    double room = (double)mem_heap_extra_max_kib() / GAME_DTA_KIB + 1;
    return sqrt(room / picture_ratio()) * PLAIN_SHARE;
}

static double heap_share(void) {
    static double share = -1;
    if (share < 0) {
        double s = plain_share();
        if (s < PLAIN_SHARE) s = PLAIN_SHARE;
        if (rt_patch_set("res_fixed") >= 0 && cfg_bool("NX2SARIA_DLAA") && mode_share() > s) s = mode_share();
        double most = share_most();
        if (s > most) {
            if (most < PLAIN_SHARE) most = PLAIN_SHARE;
            rt_log("widescreen: the scene can be %.0f%% of the picture at most, not %.0f%%: the game's render-target "
                   "heap for more does not fit the application pool (%llu MiB of growth at most)", most * 100, s * 100,
                   (unsigned long long)(mem_heap_extra_max_kib() >> 10));
            s = most;
        }
        share = s;
    }
    return share;
}

static double scale_k = 1.0;

static void scene_log(double s, int first, int dyn) {
    static double last_s = -1;
    static int last_how = -1;
    int how = dlss_share() > 0 ? (vk_dlaa_fsr3() ? 2 : 1) : s >= 1 ? 3
            : !strcmp(cfg_value("NX2SARIA_STANDIN_FILTER"), "easu") ? 4 : 5;
    if (!first && how == last_how && fabs(s - last_s) < 0.001) return;
    last_s = s, last_how = how;
    static int dyn_lines;
    if (dyn && !first && dyn_lines++ >= 40) return;
    static const char *const hows[] = {
        "", "DLSS scales it to the picture", "AMD FSR scales it to the picture",
        "native: no upscaling, the upscaler's stand-in only decodes it 1:1",
        "the upscaler's stand-in enlarges it with the edge-adaptive filter (no DLSS or FSR)",
        "the upscaler's stand-in enlarges it with the linear filter (no DLSS or FSR)"};
    unsigned pw, ph;
    wide_picture(&pw, &ph);
    rt_log("widescreen: scene %ux%u, picture %ux%u (%.0f%% of it): %s%s", (unsigned)lround(pw * s),
           (unsigned)lround(ph * s), pw, ph, s * 100, hows[how], first ? " (the game's dynamic resolution off)" : "");
    double want = wanted_share();
    if (how >= 3 && want > fit_share() + 0.001)
        rt_log("widescreen: scene_share asks for %.0f%% of the picture; the game's render-target heap %s", want * 100,
               heap_unserved ? "is the shipped one (the start's log says why the grown one was not served)"
               : wide_scene_restart() ? "was sized at the start for less: the next start gives it"
                                      : "is at its limit for this picture (the start's log says so)");
}

void wide_present(void) {
    static int set = -2;
    static uint64_t presents;
    static unsigned seen = ~0u;
    if (set == -2) set = rt_patch_set("res_fixed");
    if (set < 0) return;
    static const char *dyn_seen;
    static double dyn_share_seen;
    unsigned v = cfg_version();
    const char *dyn = dynres_override();
    double dyn_share = dynres_share();
    int at = ++presents == RES_FIXED_AFTER, due = presents >= RES_FIXED_AFTER;
    int dyn_only = v == seen && !at;
    if (dyn_only && dyn == dyn_seen && dyn_share == dyn_share_seen) return;
    seen = v, dyn_seen = dyn, dyn_share_seen = dyn_share;
    double s = scene_share();
    float page_s = (float)(s * scale_k);
    if (s > 0) memcpy((uint8_t *)rt_patch_page() + 240, &page_s, 4);
    int on = rt_patch_enabled(set);
    if (s <= 0) {
        if (on) rt_patch_switch(set, 0);
        return;
    }
    if (!due) return;
    if (!on) rt_patch_switch(set, 1);
    scene_log(s, !on, dyn_only);
}

int wide_scene_settable(void) { return rt_patch_set("res_fixed") >= 0; }

double wide_scene_share(void) { return scene_share(); }

double wide_scene_wanted(void) { return wanted_share(); }

int wide_scene_restart(void) {
    double want = wanted_share(), now = fit_share(), next = share_most();
    if (want > 1) want = 1;
    return !heap_unserved && want > now + 0.001 && next > now + 0.001;
}

static int picture_size(unsigned *ow, unsigned *oh, int quiet) {
    double r = wanted_ratio(), game = (double)GAME_W / GAME_H;
    *oh = picture_height();
    if (r <= 0 || fabs(r / game - 1.0) < 0.01) {
        if (r > 0 && !quiet && r != game) rt_log("widescreen: a %.4f picture is within 1%% of 16:9; the game's 16:9", r);
        r = game;
    }
    if (r == game && *oh == GAME_H) return 0;
    if (r > 32.0 / 9.0) r = 32.0 / 9.0;
    *ow = (unsigned)lround(*oh * r / 8) * 8;
    return 1;
}

uint64_t wide_heap_extra_kib(void) {
    unsigned ow, oh;
    if (!picture_size(&ow, &oh, 1)) return 0;
    uint64_t kib = wide_aspect_heap_growth_kib(ow, oh, heap_share()), most = mem_heap_extra_max_kib();
    if (kib > most) {
        static int said;
        if (!said++)
            rt_log("widescreen: WARNING: a picture of %.1f times the game's own needs %llu MiB more render-target heap "
                   "and the application pool has room for %llu: the game may end when a screen needs more targets "
                   "than fit. A smaller game_res or a narrower game_aspect fits", picture_ratio(),
                   (unsigned long long)(kib >> 10), (unsigned long long)(most >> 10));
        kib = most;
    }
    return kib;
}

static void save_extra(unsigned ow, unsigned oh, int32_t extra[4]) {
    double ratio = (double)ow / oh, game = (double)GAME_W / GAME_H;
    int32_t x = ratio > game ? 2 * (int32_t)lround(SAVE_W * (ratio / game - 1) / 2) : 0;
    int32_t y = ratio < game ? 2 * (int32_t)lround(SAVE_H * (game / ratio - 1) / 2) : 0;
    extra[0] = -x / 2, extra[1] = x;
    extra[2] = -y / 2, extra[3] = y;
}

static unsigned start_w, start_h;
static int start_wide, started;

static float balloon_ui_scale = 1.0f;

static void wide_balloon_init(float scale) {
    int set = rt_patch_set("wide_balloon");
    if (set < 0) return;
    if (!rt_patch_enabled(set)) {
        rt_log("balloon: off, patch disabled or replaced by an ExeFS mod (UI horizontal scale %.4f)", scale);
        return;
    }
    balloon_ui_scale = scale;
    rt_patch_switch(set, scale != 1.0f);
    rt_log("balloon: %s, UI horizontal scale %.4f", scale == 1.0f ? "off" : "on", scale);
}

void wide_balloon_host(Cpu *c) {
    uint32_t bits = (uint32_t)c->v[0];
    float x;
    memcpy(&x, &bits, sizeof x);
    float corrected = x / balloon_ui_scale;
    if (!isfinite(corrected) || corrected == x) return;
    memcpy(&bits, &corrected, sizeof bits);
    c->v[0] = bits;
    static atomic_flag logged = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set_explicit(&logged, memory_order_relaxed))
        rt_log("balloon: anchor x %.2f -> %.2f (UI horizontal scale %.4f)", x, corrected, balloon_ui_scale);
}

void wide_init(void) {
    int so = rt_patch_set("wide_out"), ss = rt_patch_set("wide_scene"), sa = rt_patch_set("wide_aspect"),
        sh = rt_patch_set("wide_hud"), sm = rt_patch_set("wide_movie"), sv = rt_patch_set("wide_save");
    double game = (double)GAME_W / GAME_H, f = 0.75;
    unsigned ow, oh;
    if (wide_heap_extra_kib()) {
        fs_init();
        if (!res_heapdef_ok()) {
            heap_unserved = 1;
            rt_log("widescreen: the game's render-target heap stays the shipped one (the grown table was not served, see "
                   "the RomFS and resolution lines above): the scene is %.0f%% of the picture at most and the picture "
                   "the game's own 2560x1440", PLAIN_SHARE * 100);
        }
    }
    int center = !strcmp(cfg_value("NX2SARIA_WIDE_MENUS"), "center");
    const char *menus = center ? "center" : "full";
    if (!picture_size(&ow, &oh, 0) || heap_unserved) {
        if (wide_available())
            rt_log("widescreen: menus %s: the picture is the game's own 2560x1440, nothing to keep in it", menus);
        start_w = GAME_W, start_h = GAME_H, start_wide = 0;
        __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
        wide_balloon_init(1.0f);
        return;
    }
    if (so < 0 || ss < 0 || sa < 0 || sh < 0 || sm < 0) {
        rt_log("widescreen: the generated C has no widescreen patch sets (gen.py --patches recomp/patches-v111.txt); "
               "the game's own 2560x1440 picture");
        wide_balloon_init(1.0f);
        return;
    }
    unsigned sw, sch, ignored_w;
    wide_aspect_dimensions(oh, (double)ow / oh, &ignored_w, &sw, &sch);
    uint64_t out = (uint64_t)oh << 32 | ow, scene = (uint64_t)sch << 32 | sw;
    float match = (float)WIDE_BASE_ASPECT, ratio = (float)ow / oh, tolerance = 0.01f;
    int wider = ratio > match * (1 + tolerance), taller = ratio < match * (1 - tolerance);
    float x_squeeze = ratio > match ? match / ratio : 1.0f;
    float y_squeeze = (float)wide_ui_vertical_scale(ratio);
    float ui[2] = {2.0f / 3840.0f, 2.0f / 2160.0f};
    float squeeze4[4] = {x_squeeze, x_squeeze, x_squeeze, x_squeeze};
    float vertical4[4] = {y_squeeze, y_squeeze, y_squeeze, y_squeeze};
    float margin_x = (1.0f - x_squeeze) / 2, margin_y = (1.0f - y_squeeze) / 2;
    uint64_t keep[2] = {MOD_MAIN_BASE + 0x4bed098, MOD_MAIN_BASE + 0x4be4fb0},
             keep2[2] = {MOD_MAIN_BASE + 0x4be61c8, MOD_MAIN_BASE + 0x4be7b30};
    uint8_t *page = rt_patch_page();
    memcpy(page + 96, &out, 8);
    memcpy(page + 104, &scene, 8);
    memcpy(page + 112, &match, 4);
    memcpy(page + 116, &ratio, 4);
    memcpy(page + 120, &tolerance, 4);
    memcpy(page + 128, vertical4, 16);
    memcpy(page + 144, ui, 8);
    memcpy(page + 160, keep, 16);
    memcpy(page + 176, squeeze4, 16);
    memcpy(page + 192, &margin_x, 4);
    float movie_margin_y = taller ? 0.0f : margin_y;
    memcpy(page + 196, &movie_margin_y, 4);
    memcpy(page + 208, keep2, 16);
    int32_t save[4];
    save_extra(ow, oh, save);
    memcpy(page + 224, save, sizeof save);
    rt_patch_switch(so, 1);
    rt_patch_switch(ss, 1);
    rt_patch_switch(sa, 1);
    rt_patch_switch(sh, 1);
    rt_patch_switch(sm, 1);
    wide_balloon_init(x_squeeze);
    if (save[1] || save[3]) rt_patch_switch(sv, 1);
    start_w = ow, start_h = oh, start_wide = 1;
    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    if (taller)
        rt_log("widescreen: tall picture: full-height fades, backgrounds and movies; ordinary UI stays in the middle 16:9");
    else if (wider)
        rt_log("widescreen: menus %s: %s", menus,
               center ? "black bars: the picture's sides black while a pause or system menu, a loading screen or a "
                        "movie is up, the loading icon in the middle 16:9's corner"
                      : "fill the screen: the pause, system and loading screens over the whole picture, with "
                        "their layouts laid out for its width");
    if ((center && wider) || taller) wide_bars_init(margin_x, taller ? 0.0 : margin_y);
    scale_k = oh > GAME_H ? (double)oh / GAME_H : 1.0;
    if (taller)
        rt_log("widescreen: picture %ux%u, scene %ux%u (%.0f%% of it), camera Vert+: vertical angle v -> "
               "2 atan(tan(v/2) * %.4f); ordinary UI centered at %.1f%% height; whole-picture screens and movies fill height, saves keep middle 16:9",
               ow, oh, sw, sch, f * 100, match / ratio, y_squeeze * 100);
    else
        rt_log("widescreen: picture %ux%u, scene %ux%u (%.0f%% of it), camera aspect ratio %.4f (16:9 is %.4f), UI and movies "
               "in the middle %.1f%%", ow, oh, sw, sch, f * 100, ratio, game, x_squeeze * 100);
    if ((save[1] || save[3]) && sv >= 0)
        rt_log("widescreen: a save's picture holds the middle 16:9 (viewport adds %d x and %d y pixels)", save[1], save[3]);
    else if (save[1] || save[3])
        rt_log("widescreen: the generated C has no set wide_save; a save's picture holds the whole picture, squeezed");
}

int wide_start_picture(unsigned *w, unsigned *h) {
    static unsigned kw, kh;
    static int kept = -1;
    if (started) {
        *w = start_w, *h = start_h;
        return start_wide;
    }
    if (kept < 0) {
        unsigned ow, oh;
        kw = GAME_W, kh = GAME_H, kept = 0;
        if (wide_available() && picture_size(&ow, &oh, 1) && !(wide_heap_extra_kib() && !res_heapdef_ok()))
            kw = ow, kh = oh, kept = 1;
    }
    *w = kw, *h = kh;
    return kept;
}

int wide_picture_known(unsigned *w, unsigned *h) {
    if (!__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return 0;
    *w = start_w, *h = start_h;
    return 1;
}

void wide_picture(unsigned *w, unsigned *h) {
    static unsigned pw, ph;
    if (!pw) {
        if (!wide_available() || heap_unserved || !picture_size(&pw, &ph, 1)) pw = GAME_W, ph = GAME_H;
    }
    *w = pw, *h = ph;
}

int wide_available(void) {
    static const char *const names[] = {"wide_out", "wide_scene", "wide_aspect", "wide_hud", "wide_movie"};
    for (int i = 0; i < 5; i++)
        if (rt_patch_set(names[i]) < 0) return 0;
    return 1;
}
