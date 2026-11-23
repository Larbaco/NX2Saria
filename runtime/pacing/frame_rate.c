// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_tools.h"
#include "cfg.h"
#include "floor_watch.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "layout.h"
#include "platform/platform.h"
#include "rtss_decide.h"
#include "vk_dlssg.h"
#include "vk_fg.h"
#include "vk_lowlat.h"

#define STEP_SAMPLES 5
#define STEP_MAX_S (1.0 / 15.0)
#define STEP_RESET_S 0.25
#define STEP_FIRST_S (1.0 / 60.0)
#define STEP_MIN_S (1.0 / 240.0)

static _Atomic uint64_t *step_word;
static double step_now = STEP_FIRST_S;
static _Atomic uint32_t *step_interval;
static _Atomic uint32_t *ostep_page;
static _Atomic uint32_t *carry_page;

static void floor_carry_store(float frames) {
    if (!carry_page || !(frames > 0)) return;
    float cut = frames < 1.0f ? 100.0f / frames : 100.0f;
    uint32_t bits;
    memcpy(&bits, &cut, 4);
    atomic_store_explicit(carry_page, bits, memory_order_release);
}
static uint64_t last_ns;
static double samples[STEP_SAMPLES];
static unsigned sample_count, sample_next;

static void step_store(float seconds) {
    step_now = seconds;
    float ratio = seconds * 30.0f, interval = seconds * 60.0f;
    uint32_t lo, hi, iv;
    memcpy(&lo, &ratio, 4);
    memcpy(&hi, &seconds, 4);
    memcpy(&iv, &interval, 4);
    atomic_store_explicit(step_word, (uint64_t)lo | (uint64_t)hi << 32, memory_order_release);
    atomic_store_explicit(step_interval, iv, memory_order_release);
}

static void frame_limit_live(void);

#define OCARINA_QUIET_NS 2000000000ull
#define OCARINA_CLOCK_WORD 264
#define OCARINA_STAFF_WORD 776
static _Atomic uint32_t *oc_clock_word, *oc_staff_word;
static struct {
    uint32_t clock, staff;
    int on;
    uint64_t start_ns, last_ns, presents, last_presents, clock_moves, notes;
} oc;

static void ocarina_log_init(void) {
    int rec = rt_patch_set("orecord"), staff = rt_patch_set("ostaff");
    if ((rec < 0 && staff < 0) || !rt_patch_page()) return;
    if (rec >= 0 && rt_patch_enabled(rec)) {
        oc_clock_word = (_Atomic uint32_t *)((uint8_t *)rt_patch_page() + OCARINA_CLOCK_WORD);
        oc.clock = atomic_load_explicit(oc_clock_word, memory_order_relaxed);
    }
    if (staff >= 0 && rt_patch_enabled(staff)) {
        oc_staff_word = (_Atomic uint32_t *)((uint8_t *)rt_patch_page() + OCARINA_STAFF_WORD);
        oc.staff = atomic_load_explicit(oc_staff_word, memory_order_relaxed);
    }
    rt_log("ocarina: the log follows the recorder clock (set orecord %s) and a played-back song's staff (set ostaff %s): "
           "one line per stretch of ocarina use",
           rec < 0 ? "absent" : oc_clock_word ? "on" : "off", staff < 0 ? "absent" : oc_staff_word ? "on" : "off");
}

static void ocarina_end(void) {
    double s = (double)(oc.last_ns - oc.start_ns) * 1e-9;
    rt_log("ocarina: %.1f s of ocarina use, %llu presents (%.0f fps): the recorder clock carried a fraction on %llu of "
           "them%s; a played-back song's staff moved %llu notes%s", s, (unsigned long long)oc.last_presents,
           s > 0 ? (double)(oc.last_presents - 1) / s : 0.0, (unsigned long long)oc.clock_moves,
           oc_clock_word ? "" : " (set orecord off: not watched)", (unsigned long long)oc.notes,
           oc_staff_word ? "" : " (set ostaff off: not watched)");
    oc.on = 0;
}

static void ocarina_watch(uint64_t now_ns) {
    if (!oc_clock_word && !oc_staff_word) return;
    uint32_t c = oc_clock_word ? atomic_load_explicit(oc_clock_word, memory_order_relaxed) : 0;
    uint32_t s = oc_staff_word ? atomic_load_explicit(oc_staff_word, memory_order_relaxed) : 0;
    int clock = c != oc.clock, staff = s != oc.staff, rise = s && !oc.staff;
    oc.clock = c, oc.staff = s;
    if (clock || staff) {
        if (!oc.on) oc.on = 1, oc.start_ns = now_ns, oc.presents = oc.clock_moves = oc.notes = 0;
        oc.last_ns = now_ns;
        oc.clock_moves += clock, oc.notes += rise;
    }
    if (!oc.on) return;
    oc.presents++;
    if (clock || staff) oc.last_presents = oc.presents;
    else if (now_ns - oc.last_ns > OCARINA_QUIET_NS) ocarina_end();
}

void frame_step_init(void) {
    int fps60 = rt_patch_set("fps60"), realtime = rt_patch_set("realtime"), cloth = rt_patch_set("cloth");
    int ostep = rt_patch_set("ostep");
    if (ostep >= 0 && rt_patch_enabled(ostep)) {
        ostep_page = (_Atomic uint32_t *)((uint8_t *)rt_patch_page() + 256);
        float one = 1.0f;
        uint32_t bits;
        memcpy(&bits, &one, 4);
        atomic_store_explicit(ostep_page, bits, memory_order_release);
        rt_log("frame rate: the game's frames per update follow the display interval (set ostep)");
    }
    ocarina_log_init();
    int follow = rt_patch_set("ofollow");
    if (follow >= 0) {
        carry_page = (_Atomic uint32_t *)((uint8_t *)rt_patch_page() + 260);
        floor_carry_store(1.0f);
        rt_log("frame rate: a moving floor carries a character as at 60 fps: %s",
               rt_patch_enabled(follow) ? "on (set ofollow)" : "off, the game's cut above 100 fps (floor_carry = 0)");
    }
    int solver = rt_patch_set("osolver"), solver_full = rt_patch_set("osolverfull");
    if (solver_full >= 0) {
        const char *e = cfg_get("NX2SARIA_PHYSICS_STEPS");
        int full = (e && !strcmp(e, "2")) || rt_patch_enabled(solver_full);
        rt_patch_switch(solver_full, full);
        if (full && solver >= 0) rt_patch_disable(solver);
        rt_log("frame rate: physics solver steps an update: %s",
               full ? "rounded up and at least a 60 fps frame's 4 (physics_steps = 2)"
               : solver >= 0 && rt_patch_enabled(solver) ? "rounded up (physics_steps = 1)"
                                                          : "the game's own count (physics_steps = 0)");
    }
    if (realtime < 0) {
        rt_log("frame rate: no patch table, shipping 30 fps whole-vsync steps");
        return;
    }
    if (!rt_patch_enabled(realtime) && rt_patch_enabled(cloth)) {
        rt_patch_disable(cloth);
        rt_log("frame rate: cloth clock off, it needs the real-time step (NX2SARIA_REALTIME=0)");
    }
    if (rt_patch_enabled(realtime)) {
        step_word = (_Atomic uint64_t *)rt_patch_page();
        step_interval = (_Atomic uint32_t *)((uint8_t *)rt_patch_page() + 8);
        step_store((float)STEP_FIRST_S);
    }
    frame_limit_live();
    rt_log("frame rate: present interval %s, %s", rt_patch_enabled(fps60) ? "1..3 (above 30 fps)" : "2..3 (shipping 30 fps)",
           step_word ? "real-time step (5-frame average of display intervals, at most 1/15 s)" : "whole-vsync steps");
}

int frame_step_realtime(void) { return step_word != NULL; }

#define LIMIT_MIN_HZ 30
#define LIMIT_MAX_HZ 240
static int fps60_set = -2;
static int game_interval = 1;

static double shown_hz(double hz) { return hz <= 0 ? 60 : hz < LIMIT_MIN_HZ ? LIMIT_MIN_HZ : hz; }

#define LIMIT_FREE_MAX_HZ 240
int frame_free_running(void) { return rt_patch_set("fps60") < 0 && rt_patch_set("realtime") < 0; }
int frame_limit_adjustable(void) { return 1; }

uint64_t frame_step_ceiling_ns(void) {
    return 1000000000ull / (frame_free_running() ? LIMIT_FREE_MAX_HZ : LIMIT_MAX_HZ);
}
static unsigned limit_seen;
static int limit_read;
static int limit_hz = -1;

static int limit_parse(void) {
    const char *e = cfg_get("NX2SARIA_FPS_LIMIT"), *old = cfg_get("NX2SARIA_FPS60");
    if (!e || !*e) return old && !strcmp(old, "0") ? 30 : 60;
    if (!strcmp(e, "display")) return 0;
    int free_run = frame_free_running();
    if (!strcmp(e, "unlimited")) {
        if (free_run) return -2;
        rt_log("frame rate: NX2SARIA_FPS_LIMIT=unlimited needs a game that steps by the clock; %d", LIMIT_MAX_HZ);
        return LIMIT_MAX_HZ;
    }
    char *end;
    long n = strtol(e, &end, 10);
    if (!*end && free_run && n >= LIMIT_MIN_HZ) return n > LIMIT_FREE_MAX_HZ ? LIMIT_FREE_MAX_HZ : (int)n;
    if (!*end && n > LIMIT_MAX_HZ) {
        rt_log("frame rate: NX2SARIA_FPS_LIMIT=%s is above %d, where the game's physics runs out of memory; %d", e,
               LIMIT_MAX_HZ, LIMIT_MAX_HZ);
        return LIMIT_MAX_HZ;
    }
    if (!*end && n >= LIMIT_MIN_HZ) return (int)n;
    rt_log("frame rate: NX2SARIA_FPS_LIMIT=%s is not %d..%d, display or unlimited; 60", e, LIMIT_MIN_HZ, LIMIT_MAX_HZ);
    return 60;
}

static void frame_limit_live(void) {
    unsigned v = cfg_version();
    if (limit_read && v == limit_seen) return;
    limit_read = 1;
    limit_seen = v;
    int hz = limit_parse();
    if (hz == limit_hz) return;
    limit_hz = hz;
    if (frame_free_running()) {
        if (hz == -2) rt_log("frame rate: unlimited (the game steps by the clock), at most %d game frames a second",
                             LIMIT_FREE_MAX_HZ);
        else if (hz) rt_log("frame rate: limit %d fps (the game steps by the clock)", hz);
        else rt_log("frame rate: limit at the monitor's refresh rate (the game steps by the clock)");
        return;
    }
    if (fps60_set == -2) fps60_set = rt_patch_set("fps60");
    if (fps60_set >= 0) rt_patch_switch(fps60_set, hz != 30);
    if ((hz > 60 || hz == 0) && !step_word)
        rt_log("frame rate: limits above 60 need the real-time step (NX2SARIA_REALTIME=1); 60 fps");
    else if (hz) rt_log("frame rate: limit %d fps", hz);
    else if (gfx_window_refresh_hz() > 0) rt_log("frame rate: limit at the monitor's refresh rate (%.3f Hz now)", gfx_window_refresh_hz());
    else rt_log("frame rate: limit at the monitor's refresh rate (60 until the window measures it)");
}

static int movie_cap_hz(void) {
    static int hz = -1;
    if (hz < 0) {
        const char *e = cfg_get("NX2SARIA_MOVIE_FPS");
        hz = e && *e ? atoi(e) : 60;
        if (hz && hz < LIMIT_MIN_HZ) hz = 60;
    }
    return hz && ov_movie_live() ? hz : 0;
}

static int generated_factor(void) { return vk_fg_generating() || vk_fsr3fg_generating() ? 2 : vk_dlssg_multiplier(); }

static double limit_game_hz(int limit, int gen) {
    if (limit) return shown_hz(limit);
    double hz = shown_hz(gfx_window_refresh_hz());
    return gen > 1 ? hz / gen : hz;
}

double frame_step_game_hz(void) {
    if (frame_step_limit_hz() <= 0) return 0;
    double hz = limit_game_hz(limit_hz, generated_factor());
    return !frame_free_running() && !step_word && hz > 60 ? 60 : hz > LIMIT_MAX_HZ ? LIMIT_MAX_HZ : hz;
}

static double paced_hz;
static int movie_paced;
int frame_step_movie_hz(void) { return movie_paced; }
uint64_t frame_step_cap_ns(void) {
    frame_limit_live();
    static int rtss_allowed = -1;
    if (rtss_allowed < 0) {
        const char *e = cfg_get("NX2SARIA_RTSS");
        rtss_allowed = !(e && !strcmp(e, "0"));
    }
    int64_t rtss_out = !rtss_allowed                          ? RTSS_OUT_SETTING
                       : vk_fg_wanted() || vk_dlssg_active() ? RTSS_OUT_FRAMEGEN
                       : vk_lowlat_on()                      ? RTSS_OUT_LOWLAT
                                                             : 0;
    int gen = generated_factor();
    int movie_hz = movie_cap_hz();
    static int movie_clock = -1;
    if (movie_clock < 0) {
        const char *e = cfg_get("NX2SARIA_MOVIE_CLOCK");
        movie_clock = !(e && !strcmp(e, "0"));
    }
    movie_paced = movie_clock ? movie_hz : 0;
    static int movie_was, movie_logs;
    if (!!movie_hz != movie_was) {
        movie_was = !!movie_hz;
        if (movie_logs++ < 40) {
            if (movie_hz) rt_log("frame rate: a movie plays: %d fps until it ends (NX2SARIA_MOVIE_FPS)", movie_hz);
            else rt_log("frame rate: the movie ended: the frame limit again");
        }
    }
    rtss_limit_movie(movie_hz);
    if (limit_hz == -2 && !movie_hz && !frame_free_running()) {
        rtss_limit_want(rtss_out);
        paced_hz = 0;
        return 0;
    }
    double hz = limit_hz == -2 ? LIMIT_FREE_MAX_HZ : limit_game_hz(limit_hz, gen);
    double max = frame_free_running() ? LIMIT_FREE_MAX_HZ : LIMIT_MAX_HZ;
    if (hz > max) hz = max;
    static double logged_hz;
    static int logged_gen, gen_logs;
    if (gen > 1 && (hz != logged_hz || gen != logged_gen) && gen_logs < 40) {
        gen_logs++, logged_hz = hz, logged_gen = gen;
        rt_log("frame rate: the game draws up to %.0f frames a second and frame generation shows %d for each (%.0f on "
               "screen)", hz, gen, hz * gen);
    }
    int64_t play_mhz = (int64_t)(hz * 1000 + 0.5);
    if (movie_hz) hz = movie_hz;
    paced_hz = hz;
    rtss_limit_want(rtss_out ? rtss_out : play_mhz);
    return rtss_pace_period(rtss_limit_active(), movie_hz, kern_now_ns(), gfx_window_hosted_ns(),
                            (uint64_t)(1e9 / hz + 0.5), frame_step_ceiling_ns());
}

double frame_step_limit_hz(void) {
    if (frame_free_running()) {
        frame_limit_live();
        int limit = limit_hz;
        if (limit == -2 || limit == -1) return 0;
        return shown_hz(limit ? limit : gfx_window_refresh_hz());
    }
    int hz = limit_hz;
    if (hz < 0) return 0;
    double r = shown_hz(hz ? hz : gfx_window_refresh_hz());
    return !step_word && r > 60 ? 60 : r > LIMIT_MAX_HZ ? LIMIT_MAX_HZ : r;
}

static uint64_t win_t0, win_frames, win_hist[4];
static double win_game;
static int probe = -1;
static float last_pos[3];
static uint64_t last_pos_ns;

static int read_guest(uint64_t a, void *out, size_t n) {
    if (!a) return 0;
    mem_kernel_access(a, n);
    return plat_mem_read(a, out, n);
}
static uint64_t read_ptr(uint64_t a) {
    uint64_t p = 0;
    return read_guest(a, &p, 8) ? p : 0;
}
static int player_name(uint64_t name) {
    char text[7];
    return name && read_guest(name, text, 7) && !memcmp(text, "Player", 7);
}

static int player_pos(float xyz[3]) {
    if (RT_V111) {
        uint64_t manager = read_ptr(MOD_MAIN_BASE + 0x4e371d0), actor = manager ? read_ptr(manager + 0x480) : 0;
        return actor && player_name(read_ptr(actor + 0x20)) && read_guest(actor + 0x31c, xyz, 12) && isfinite(xyz[0]) &&
               isfinite(xyz[2]);
    }
    uint64_t module = read_ptr(MOD_MAIN_BASE + (RT_V143 ? 0x3add238 : RT_V110 ? 0x4630a68 : 0x4728538)),
             scene = module ? read_ptr(module + 0x1e8) : 0;
    int32_t count = 0;
    if (!scene || !read_guest(scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uint64_t comps = read_ptr(scene + 0x58), resident = comps ? read_ptr(comps + 13 * 8) : 0;
    if (!resident || !read_guest(resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uint64_t links = read_ptr(resident + 0x28);
    for (int i = 0; links && i < count; i++) {
        uint64_t data = read_ptr(links + (uint64_t)i * 0x70 + 8), actor = data ? read_ptr(data + 0x40) : 0;
        if (actor && player_name(read_ptr(actor + 0x218)))
            return read_guest(actor + 0x2b4, xyz, 12) && isfinite(xyz[0]) && isfinite(xyz[2]);
    }
    return 0;
}

static double ocarina_step_s(void) {
    uint64_t slot = read_ptr(MOD_MAIN_BASE + 0x4c7e890), mgr = slot ? read_ptr(slot) : 0;
    float base = 0, speed = 0;
    if (!mgr || !read_guest(mgr + 0x1c0, &base, 4) || !read_guest(mgr + 0x10, &speed, 4) || !isfinite(base) || !isfinite(speed))
        return -1;
    return base * speed / 60.0;
}

static _Atomic uint64_t guest_presents;

uint64_t frame_step_presents(void) { return atomic_load_explicit(&guest_presents, memory_order_relaxed); }

void frame_step_flight(float *page, float *game, uint64_t *presents) {
    *page = ostep_page ? frame_step_frames() : -1.0f;
    double s = frame_free_running() ? ocarina_step_s() : -1;
    *game = s > 0 ? (float)(s * 60.0) : -1.0f;
    *presents = atomic_load_explicit(&guest_presents, memory_order_relaxed);
}

static uint64_t ostep_writes, ostep_changed;
static float ostep_last = -1;

#define OSTEP_WINDOW 12
#define OSTEP_SNAP 0.03
#define OSTEP_REPAY_MAX 0.10
#define OSTEP_MIN_S (1.0 / 480.0)
static double os_ring[OSTEP_WINDOW], os_owed;
static unsigned os_count, os_next;
static double os_step_lo, os_step_hi;

static void ocarina_step_reset(void) {
    os_count = os_next = 0;
    os_owed = 0;
}

static double ocarina_step(double dt) {
    static int steady = -1, movie_was;
    if (steady < 0) {
        const char *e = cfg_get("NX2SARIA_STEP_STEADY");
        steady = !(e && !strcmp(e, "0"));
    }
    if (movie_paced != movie_was) {
        movie_was = movie_paced;
        ocarina_step_reset();
    }
    if (movie_paced > 0) {
        double step = 1.0 / movie_paced;
        if (os_step_lo <= 0 || step < os_step_lo) os_step_lo = step;
        if (step > os_step_hi) os_step_hi = step;
        return step;
    }
    if (dt > STEP_MAX_S) dt = STEP_MAX_S;
    os_ring[os_next++ % OSTEP_WINDOW] = dt;
    if (os_count < OSTEP_WINDOW) os_count++;
    double step;
    if (!steady) {
        unsigned n = os_count < STEP_SAMPLES ? os_count : STEP_SAMPLES;
        double sum = 0;
        for (unsigned i = 1; i <= n; i++) sum += os_ring[(os_next - i) % OSTEP_WINDOW];
        step = sum / n;
        if (step < 1.0 / 1000) step = 1.0 / 1000;
    } else {
        double sum = 0, lo = os_ring[0], hi = os_ring[0];
        for (unsigned i = 0; i < os_count; i++) {
            sum += os_ring[i];
            if (os_ring[i] < lo) lo = os_ring[i];
            if (os_ring[i] > hi) hi = os_ring[i];
        }
        double base = os_count >= 4 ? (sum - lo - hi) / (os_count - 2) : sum / os_count;
        double period = paced_hz > 0 ? 1.0 / paced_hz : 0;
        if (period > 0 && fabs(base - period) <= OSTEP_SNAP * period) base = period;
        double over = fabs(os_owed) - base * 0.5, repay = 0;
        if (over > 0) {
            repay = over / 64 < OSTEP_REPAY_MAX * base ? over / 64 : OSTEP_REPAY_MAX * base;
            if (os_owed < 0) repay = -repay;
        }
        step = base + repay;
        if (step < OSTEP_MIN_S) step = OSTEP_MIN_S;
        if (step > STEP_MAX_S) step = STEP_MAX_S;
        os_owed += dt - step;
        if (os_owed > STEP_RESET_S) os_owed = STEP_RESET_S;
        if (os_owed < -STEP_RESET_S) os_owed = -STEP_RESET_S;
    }
    if (os_step_lo <= 0 || step < os_step_lo) os_step_lo = step;
    if (step > os_step_hi) os_step_hi = step;
    return step;
}

#define WHOLE_SLOTS 8
#define WATER_SLOTS 64
typedef struct { uint64_t at; float rem; uint64_t used; } WholeSlot;
static WholeSlot whole_slot[WHOLE_SLOTS], water_slot[WATER_SLOTS];
static uint64_t whole_calls;
static PlatMutex whole_lock = PLAT_MUTEX_INIT;

static uint32_t cut_fcvtzs(float f) {
    if (f != f) return 0;
    if (f >= 2147483648.0f) return 0x7fffffffu;
    if (f <= -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}

static uint32_t whole_take(WholeSlot *slot, int n, uint64_t at, float amount) {
    if (!(amount > 0.0f) || amount >= 2147483648.0f) return cut_fcvtzs(amount);
    plat_mutex_lock(&whole_lock);
    int k = -1, oldest = 0;
    for (int i = 0; i < n; i++) {
        if (slot[i].at == at) { k = i; break; }
        if (slot[i].used < slot[oldest].used) oldest = i;
    }
    if (k < 0) k = oldest, slot[k].at = at, slot[k].rem = 0.0f;
    slot[k].used = ++whole_calls;
    float sum = slot[k].rem + amount;
    uint32_t whole = cut_fcvtzs(sum);
    slot[k].rem = sum - (float)whole;
    plat_mutex_unlock(&whole_lock);
    return whole;
}

static uint32_t whole_frames(uint64_t at, float amount) { return whole_take(whole_slot, WHOLE_SLOTS, at, amount); }

uint32_t frame_rate_animation_frames(uint64_t entry, float step) {
    static WholeSlot loops[64];
    return whole_take(loops, sizeof loops / sizeof *loops, entry, step);
}

void frame_rate_day_host(Cpu *c) {
    uint32_t bits = (uint32_t)c->v[0];
    float secs;
    memcpy(&secs, &bits, 4);
    uint32_t whole = whole_frames(c->x[19] + 0x3c, secs);
    c->x[8] = whole;
    if (rt_trace & RT_TRACE_PERF) {
        static uint64_t t0, given, cut;
        static double exact;
        uint64_t now = plat_now_ns();
        given += whole, cut += cut_fcvtzs(secs), exact += secs;
        if (!t0) t0 = now;
        if (now - t0 >= 10000000000ull) {
            rt_log("frame rate: the time of day got %llu game seconds in %.1f s (the frames' step makes %.1f; cut "
                   "to whole seconds each frame, as the game cuts them, %llu)", (unsigned long long)given,
                   (now - t0) / 1e9, exact, (unsigned long long)cut);
            t0 = now, given = cut = 0, exact = 0;
        }
    }
}

void frame_rate_water_host(Cpu *c) {
    uint32_t bits = (uint32_t)c->v[0];
    float step;
    memcpy(&step, &bits, 4);
    uint32_t whole = whole_take(water_slot, WATER_SLOTS, c->x[21], step);
    c->x[9] = whole;
    if (rt_trace & RT_TRACE_PERF) {
        static uint64_t t0, asked, given, cut;
        uint64_t now = plat_now_ns(), n = 0, g = 0, k = 0, t = 0;
        plat_mutex_lock(&whole_lock);
        asked++, given += whole, cut += cut_fcvtzs(step);
        if (!t0) t0 = now;
        if (now - t0 >= 10000000000ull) n = asked, g = given, k = cut, t = now - t0, t0 = now, asked = given = cut = 0;
        plat_mutex_unlock(&whole_lock);
        if (n)
            rt_log("frame rate: the water's ripples got %llu steps in %.1f s, asked %llu times (cut to whole steps "
                   "each time, as the game cuts them, %llu)", (unsigned long long)g, t / 1e9, (unsigned long long)n,
                   (unsigned long long)k);
    }
}

#define SIM_SLOTS 64
static WholeSlot sim_slot[SIM_SLOTS];

void frame_rate_sim_host(Cpu *c) {
    uint32_t bits = (uint32_t)c->v[0];
    float steps;
    memcpy(&steps, &bits, 4);
    uint32_t whole = whole_take(sim_slot, SIM_SLOTS, c->x[19], steps);
    if (steps > 0.0f && steps < 2147483648.0f) {
        float f = (float)whole;
        memcpy(&bits, &f, 4);
        c->v[0] = bits;
    }
    if (rt_trace & RT_TRACE_PERF) {
        static uint64_t t0, asked, given, cut;
        uint64_t now = plat_now_ns(), n = 0, g = 0, k = 0, t = 0;
        plat_mutex_lock(&whole_lock);
        asked++, given += whole, cut += cut_fcvtzs(steps);
        if (!t0) t0 = now;
        if (now - t0 >= 10000000000ull) n = asked, g = given, k = cut, t = now - t0, t0 = now, asked = given = cut = 0;
        plat_mutex_unlock(&whole_lock);
        if (n)
            rt_log("frame rate: the water and floor surfaces got %llu fixed steps in %.1f s, asked %llu times (cut to "
                   "whole steps each time, as the game cuts them, %llu)", (unsigned long long)g, t / 1e9,
                   (unsigned long long)n, (unsigned long long)k);
    }
}

typedef struct {
    float owed;
    int run;
} SoundGate;
static SoundGate sound_gate = {0.0f, 1};

static int sound_gate_take(SoundGate *g, uint32_t phase, float step) {
    if (phase != 0) return g->run;
    if (!(step > 0.0f)) {
        g->owed = 0.0f;
        return g->run = 1;
    }
    if (step >= 1.0f) return g->run = 1;
    g->owed += step;
    g->run = g->owed >= 1.0f;
    if (g->run) g->owed -= 1.0f;
    return g->run;
}

static float game_frame_step(void) {
    uint64_t slot = read_ptr(MOD_MAIN_BASE + 0x4c7e890), mgr = slot ? read_ptr(slot) : 0;
    float step = 1.0f;
    if (mgr && !read_guest(mgr + 0x10, &step, 4)) step = 1.0f;
    return step;
}

void frame_rate_sound_gate_host(Cpu *c) {
    uint32_t phase = (uint32_t)c->x[2];
    float step = phase == 0 ? game_frame_step() : 1.0f;
    uint64_t asked = 0, ran = 0, t = 0;
    plat_mutex_lock(&whole_lock);
    int run = sound_gate_take(&sound_gate, phase, step);
    if (phase == 0) battle_music_gate(run);
    if (phase == 0 && (rt_trace & RT_TRACE_PERF)) {
        static uint64_t t0, n, r;
        uint64_t now = plat_now_ns();
        n++, r += (uint64_t)run;
        if (!t0) t0 = now;
        if (now - t0 >= 10000000000ull) asked = n, ran = r, t = now - t0, t0 = now, n = r = 0;
    }
    plat_mutex_unlock(&whole_lock);
    if (!run) c->x[2] = 3;
    if (asked)
        rt_log("frame rate: the sound job ran %llu of %llu frames in %.1f s (step %.3f)", (unsigned long long)ran,
               (unsigned long long)asked, t / 1e9, step);
}

void frame_rate_sound_step_host(Cpu *c) {
    uint32_t bits = (uint32_t)c->v[0];
    float step;
    memcpy(&step, &bits, 4);
    if (step > 0.0f && step < 1.0f) c->v[0] = 0x3f800000u;
}

float frame_step_frames(void) {
    if (!ostep_page) return 1.0f;
    uint32_t bits = atomic_load_explicit(ostep_page, memory_order_acquire);
    float f;
    memcpy(&f, &bits, 4);
    return f > 0 ? f : 1.0f;
}

void frame_rate_horse_jump_host(Cpu *c) {
    if (frame_step_frames() >= 1.0f) return;
    uint32_t bits = (uint32_t)c->v[10];
    float jump_step;
    memcpy(&jump_step, &bits, sizeof jump_step);
    if (!(jump_step > 0.0f && jump_step < 1.0f)) return;

    float frame = game_frame_step();
    if (!(frame > 0.0f && frame < 1.0f && jump_step >= frame)) return;
    uint32_t threshold;
    memcpy(&threshold, &frame, sizeof threshold);
    c->v[9] = threshold;

    static _Atomic unsigned logged;
    if (atomic_load_explicit(&logged, memory_order_relaxed) < 4) {
        unsigned n = atomic_fetch_add_explicit(&logged, 1, memory_order_relaxed);
        if (n < 4) rt_log("frame rate: horse jump blend #%u: step %.6f, threshold 1 -> %.6f", n + 1, jump_step, frame);
    }
}

void frame_rate_gohma_turn_host(Cpu *c) {
    if (frame_step_frames() >= 1.0f) return;
    uint32_t bits = (uint32_t)c->v[8];
    float step;
    memcpy(&step, &bits, sizeof step);
    if (!(step > 0.0f && step < 1.0f)) return;

    float old_bound;
    bits = (uint32_t)c->v[0];
    memcpy(&old_bound, &bits, sizeof old_bound);
    float turn = old_bound / step;
    float rate = turn / step;
    if (!(rate > 0.0f && isfinite(rate))) return;
    memcpy(&bits, &rate, sizeof bits);
    c->v[0] = bits;
    c->v[1] = bits | 0x80000000u;

    static _Atomic unsigned logged;
    if (atomic_load_explicit(&logged, memory_order_relaxed) < 4) {
        unsigned n = atomic_fetch_add_explicit(&logged, 1, memory_order_relaxed);
        if (n < 4)
            rt_log("frame rate: Gohma steering clamp #%u: step %.6f, turn magnitude %.6f, rate limit %.6f -> %.6f",
                   n + 1, step, turn, old_bound, rate);
    }
}

static void ocarina_step_write(double dt) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("NX2SARIA_OCARINA_STEP");
        on = !(e && !strcmp(e, "0"));
    }
    if (!on || dt <= 0) return;
    if (ostep_page) {
        if (dt < 1.0 / 1000) dt = 1.0 / 1000;
        if (dt > STEP_MAX_S) dt = STEP_MAX_S;
        float f = (float)(dt * 60.0);
        uint32_t bits;
        memcpy(&bits, &f, 4);
        atomic_store_explicit(ostep_page, bits, memory_order_release);
        floor_carry_store(f);
        ostep_last = f;
        return;
    }
    uint64_t slot = read_ptr(MOD_MAIN_BASE + 0x4c7e890), mgr = slot ? read_ptr(slot) : 0;
    if (!mgr || !mem_host_readable(mgr + 0x1c0, 4)) return;
    _Atomic uint32_t *word = (_Atomic uint32_t *)(uintptr_t)(mgr + 0x1c0);
    uint32_t now_bits = atomic_load_explicit(word, memory_order_relaxed);
    float now_v;
    memcpy(&now_v, &now_bits, 4);
    if (ostep_last >= 0 && now_v != ostep_last) ostep_changed++;
    if (dt < 1.0 / 1000) dt = 1.0 / 1000;
    if (dt > STEP_MAX_S) dt = STEP_MAX_S;
    float v = (float)(dt * 60.0);
    uint32_t bits;
    memcpy(&bits, &v, 4);
    atomic_store_explicit(word, bits, memory_order_relaxed);
    ostep_last = v;

    if (++ostep_writes % 1200 == 0)
        rt_log("frame step: Ocarina base step %.3f frames (%.2f ms); the game changed it %llu times in the last 1200 presents",
               v, dt * 1000, (unsigned long long)ostep_changed), ostep_changed = 0;
}

static void time_dump(void) {
    static int done;
    const char *e = cfg_get("NX2SARIA_TIME_DUMP");
    if (done || !e || strcmp(e, "1")) return;
    uint64_t guest_start = __atomic_load_n(&rt_guest_start_ns, __ATOMIC_ACQUIRE);
    if (!guest_start || kern_now_ns() - guest_start < 30000000000ull) return;
    done = 1;
    uint64_t slot = read_ptr(MOD_MAIN_BASE + 0x4c7e890), mgr = slot ? read_ptr(slot) : 0;
    uint32_t w[160];
    if (!mgr || !read_guest(mgr, w, sizeof w)) return;
    for (int i = 0; i < 160; i += 8) {
        char line[512];
        int o = snprintf(line, sizeof line, "time dump +%03x:", i * 4);
        for (int k = 0; k < 8; k++) {
            float f;
            memcpy(&f, &w[i + k], 4);
            o += snprintf(line + o, sizeof line - o, " %08x(%g)", w[i + k], (double)f);
        }
        rt_log("%s", line);
    }
}

static void speed_report(uint64_t now, double step, int interval) {
    if (!win_t0) {
        win_t0 = now;
        return;
    }
    win_frames++;
    win_game += step;
    win_hist[interval < 1 ? 1 : interval > 3 ? 3 : interval]++;
    if (win_frames < 120) return;
    if (probe < 0) {
        const char *e = cfg_get("NX2SARIA_SPEED_PROBE");
        probe = e && !strcmp(e, "1");
    }
    double wall = (double)(now - win_t0) * 1e-9;
    char pos[240] = "";
    float p[3];
    if (probe && player_pos(p)) {
        double d = last_pos_ns ? hypot(p[0] - last_pos[0], p[2] - last_pos[2]) : 0,
               s = last_pos_ns ? (double)(now - last_pos_ns) * 1e-9 : 0;
        snprintf(pos, sizeof pos, "; player %.1f %.1f %.1f, %.2f m in %.3f s = %.2f m/s", p[0], p[1], p[2], d, s,
                 s > 0 ? d / s : 0);
        memcpy(last_pos, p, sizeof p);
        last_pos_ns = now;
    }
    float global_step;
    if (probe && ostep_page && read_guest(MOD_MAIN_BASE + 0x4d41fc8, &global_step, 4)) {
        size_t o = strlen(pos);
        snprintf(pos + o, sizeof pos - o, "; global step %.3f frames", (double)global_step);
    }
    if (os_step_hi > 0) {
        size_t o = strlen(pos);
        snprintf(pos + o, sizeof pos - o, "; step %.3f..%.3f frames, %.1f ms owed", os_step_lo * 60, os_step_hi * 60,
                 os_owed * 1000);
        os_step_lo = os_step_hi = 0;
    }
    if (step_word || win_game > 0)
        rt_log("fps: %llu frames in %.3f s (%.1f fps), %.3f s game stepped, speed %.3f; interval 1/2/3+ %llu/%llu/%llu%s",
               (unsigned long long)win_frames, wall, (double)win_frames / wall, win_game, win_game / wall,
               (unsigned long long)win_hist[1], (unsigned long long)win_hist[2], (unsigned long long)win_hist[3], pos);
    else
        rt_log("fps: %llu frames in %.3f s (%.1f fps), whole-vsync steps; interval 1/2/3+ %llu/%llu/%llu%s",
               (unsigned long long)win_frames, wall, (double)win_frames / wall, (unsigned long long)win_hist[1],
               (unsigned long long)win_hist[2], (unsigned long long)win_hist[3], pos);
    win_t0 = now;
    win_frames = 0;
    win_game = 0;
    memset(win_hist, 0, sizeof win_hist);
}

static _Atomic int64_t world_us = -1;
static _Atomic uint64_t game_us;
static uint64_t presents;

int64_t frame_step_world_us(void) { return atomic_load_explicit(&world_us, memory_order_relaxed); }
uint64_t frame_step_ticks(void) { return atomic_load_explicit(&game_us, memory_order_relaxed) * 60 / 1000000; }

static void world_clock(double dt, double step) {
    presents++;
    int64_t w = atomic_load_explicit(&world_us, memory_order_relaxed);
    if (w < 0) {
        float p[3];
        if (!player_pos(p) || fabsf(p[0]) + fabsf(p[2]) < 1.0f) return;
        uint64_t guest_start = __atomic_load_n(&rt_guest_start_ns, __ATOMIC_ACQUIRE);
        rt_log("frame step: world clock starts at present %llu, %.2f s after the guest started (the player at %.1f %.1f %.1f)",
               (unsigned long long)presents, guest_start ? (kern_now_ns() - guest_start) / 1e9 : 0.0, p[0],
               p[1], p[2]);
        atomic_store_explicit(&world_us, 0, memory_order_relaxed);
        return;
    }
    if (!step_word) step = dt > STEP_MAX_S ? STEP_MAX_S : dt;
    atomic_store_explicit(&world_us, w + (int64_t)(step * 1e6 + 0.5), memory_order_relaxed);
}

#define FLOOR_PROBE_RADIUS 12.0f
#define FLOOR_PROBE_NEAR 6
#define FLOOR_PROBE_GROUPS 16384
static int floor_probe_left = -1;
static float *floor_last;
static uint32_t floor_last_n;
static float floor_link[3];
static int floor_link_ok;

static int direct_read(uint64_t a, void *out, size_t n) {
    if (!a || !mem_host_readable(a, n)) return 0;
    memcpy(out, (const void *)(uintptr_t)a, n);
    return 1;
}
static uint64_t direct_ptr(uint64_t a) {
    uint64_t v = 0;
    return direct_read(a, &v, 8) ? v : 0;
}

static void floor_probe(void) {
    if (floor_probe_left < 0) {
        const char *e = cfg_get("NX2SARIA_FLOOR_PROBE");
        int n = e && *e ? atoi(e) : 0;
        floor_probe_left = !RT_V111 || n <= 0 ? 0 : n == 1 ? 4000 : n;
        if (floor_probe_left) rt_log("floor probe: on for %d lines", floor_probe_left);
    }
    if (!floor_probe_left) return;
    float link[3];
    int link_ok = player_pos(link);
    if (!link_ok) {
        floor_link_ok = 0;
        return;
    }
    uint64_t g = direct_ptr(MOD_MAIN_BASE + 0x4c7df00), p = direct_ptr(g), q = p ? direct_ptr(p + 0xc0) : 0,
             mgr = q ? direct_ptr(q + 0x130) : 0;
    uint32_t count = 0;
    uint64_t array = 0;
    if (!mgr || !direct_read(mgr + 0x28, &count, 4) || !(array = direct_ptr(mgr + 0x30))) return;
    if (count > FLOOR_PROBE_GROUPS) {
        static int told;
        if (!told++) rt_log("floor probe: a ground group count of %u is not plausible: skipped until it is", count);
        return;
    }
    static uint64_t *entry;
    static float *rows;
    static uint64_t (*src)[2];
    static uint8_t **dst, *ok;
    if (!floor_last) {
        floor_last = calloc(FLOOR_PROBE_GROUPS * 3, sizeof(float));
        entry = calloc(FLOOR_PROBE_GROUPS * 4, sizeof(uint64_t));
        rows = calloc(FLOOR_PROBE_GROUPS * 12, sizeof(float));
        src = calloc(FLOOR_PROBE_GROUPS, sizeof *src);
        dst = calloc(FLOOR_PROBE_GROUPS, sizeof *dst);
        ok = calloc(FLOOR_PROBE_GROUPS, 1);
    }
    if (!floor_last || !entry || !rows || !src || !dst || !ok || !count) return;
    uint64_t heap = 0, heap_size = 0;
    if (!mem_info(4, &heap) || !mem_info(5, &heap_size) || !mem_host_readable(array, 32ull * count)) return;
    uint8_t *edst = (uint8_t *)entry, eok = 0;
    uint64_t esrc[1][2] = {{array, 32ull * count}};
    if (mem_copy_readable(&edst, (const uint64_t (*)[2])esrc, 1, &eok) != 1) return;
    int n = 0;
    static uint32_t *which;
    if (!which) which = calloc(FLOOR_PROBE_GROUPS, sizeof *which);
    if (!which) return;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t grp = entry[4 * i];
        if (grp < heap || grp + 0x88 + 48 > heap + heap_size) continue;
        src[n][0] = grp + 0x88, src[n][1] = 48;
        dst[n] = (uint8_t *)(rows + 12 * n);
        which[n++] = i;
    }
    mem_copy_readable(dst, (const uint64_t (*)[2])src, n, ok);
    int near[FLOOR_PROBE_NEAR], nn = 0;
    float near_d2[FLOOR_PROBE_NEAR], near_t[FLOOR_PROBE_NEAR][3], near_m[FLOOR_PROBE_NEAR][3];
    for (int k = 0; k < n; k++) {
        if (!ok[k]) continue;
        uint32_t i = which[k];
        const float *r = rows + 12 * k;
        float t[3] = {r[3], r[7], r[11]}, *last = floor_last + 3 * i;
        if (!isfinite(t[0]) || !isfinite(t[1]) || !isfinite(t[2])) continue;
        float m[3] = {0, 0, 0};
        if (i < floor_last_n) m[0] = t[0] - last[0], m[1] = t[1] - last[1], m[2] = t[2] - last[2];
        memcpy(last, t, sizeof t);
        float d2 = (t[0] - link[0]) * (t[0] - link[0]) + (t[2] - link[2]) * (t[2] - link[2]);
        if (d2 >= FLOOR_PROBE_RADIUS * FLOOR_PROBE_RADIUS) continue;
        if (m[0] == 0 && m[1] == 0 && m[2] == 0) d2 += FLOOR_PROBE_RADIUS * FLOOR_PROBE_RADIUS;
        if (nn == FLOOR_PROBE_NEAR && d2 >= near_d2[nn - 1]) continue;
        int at = nn < FLOOR_PROBE_NEAR ? nn++ : nn - 1;
        while (at > 0 && near_d2[at - 1] > d2) {
            near[at] = near[at - 1], near_d2[at] = near_d2[at - 1];
            memcpy(near_t[at], near_t[at - 1], sizeof near_t[at]);
            memcpy(near_m[at], near_m[at - 1], sizeof near_m[at]);
            at--;
        }
        near[at] = (int)i, near_d2[at] = d2, memcpy(near_t[at], t, sizeof t), memcpy(near_m[at], m, sizeof m);
    }
    floor_last_n = count;
    static unsigned quiet;
    if (floor_link_ok && (nn > 0 || ++quiet >= 30)) {
        quiet = 0;
        char line[320 + FLOOR_PROBE_NEAR * 120];
        int o = snprintf(line, sizeof line, "floor probe: step %.4f frames; link %.4f %.4f %.4f moved %.5f %.5f %.5f; "
                         "%d of %u groups within %.0f m", (double)frame_step_frames(), link[0], link[1], link[2],
                         link[0] - floor_link[0], link[1] - floor_link[1], link[2] - floor_link[2], nn, count,
                         (double)FLOOR_PROBE_RADIUS);
        uint64_t tslot = direct_ptr(MOD_MAIN_BASE + 0x4c7e890), tm = tslot ? direct_ptr(tslot) : 0;
        float tb[2] = {0}, tc[8] = {0};
        if (tm && direct_read(tm + 0x1c0, &tb[0], 4) && direct_read(tm + 0x10, &tb[1], 4) &&
            direct_read(tm + 0x20, tc, 16) && direct_read(tm + 0x50, tc + 4, 16) && o > 0 && o < (int)sizeof line)
            o += snprintf(line + o, sizeof line - o,
                          "; time base %.4f speed %.4f cat0 %.4f %.4f %.6f %.3f cat1 %.4f %.4f %.6f %.3f", tb[0], tb[1],
                          tc[0], tc[1], tc[2], tc[3], tc[4], tc[5], tc[6], tc[7]);
        for (int k = 0; k < nn && o > 0 && o < (int)sizeof line; k++)
            o += snprintf(line + o, sizeof line - o, "; group %d at %.4f %.4f %.4f moved %.5f %.5f %.5f", near[k],
                          near_t[k][0], near_t[k][1], near_t[k][2], near_m[k][0], near_m[k][1], near_m[k][2]);
        rt_log("%s", line);
        floor_probe_left--;
    }
    if (link_ok) memcpy(floor_link, link, sizeof link);
    floor_link_ok = link_ok;
}

#define FLOOR_SCAN_ARM 30
#define FLOOR_SCAN_MAX 2000000
#define FLOOR_SCAN_SHOW 64
static int floor_scan_left = -1;
static int floor_watching;
static struct {
    int state, run, tries;
    float link0[3], last[3], start[3], path, sec_link[3];
    int have_last;
    uint64_t *addr, sec_ns;
    float *val;
    int n;
} fs;

static void floor_scan_snapshot(const float link[3]) {
    enum { PIECE = 16 << 20 };
    if (!fs.addr) fs.addr = malloc(sizeof(uint64_t) * FLOOR_SCAN_MAX);
    if (!fs.val) fs.val = malloc(sizeof(float) * 3 * FLOOR_SCAN_MAX);
    if (!fs.addr || !fs.val) return;
    uint64_t t0 = plat_now_ns(), bytes = 0;
    int over = 0;
    fs.n = 0;
    const RtGuestLayout *l = mem_layout();
    const RtRange ranges[2] = {l->heap, l->alias};
    for (int r = 0; r < 2; r++) {
        uint64_t a = ranges[r].base, end = ranges[r].base + ranges[r].size;
        while (a < end) {
            RtMemoryInfo mi;
            if (mem_query(a, &mi) || !mi.size) break;
            uint64_t stop = mi.base + mi.size < end ? mi.base + mi.size : end;
            if (!mi.state || (mi.perm & 3) != 3) {
                a = stop;
                continue;
            }
            while (a < stop) {
                uint64_t n = stop - a < PIECE ? stop - a : PIECE;
                if (n >= 12 && mem_read_hold(a, n)) {
                    const float *f = (const float *)(uintptr_t)a;
                    uint64_t words = n / 4;
                    bytes += n;
                    for (uint64_t i = 0; i + 2 < words; i++) {
                        float x = f[i];
                        if (!(fabsf(x - link[0]) < 6.0f)) continue;
                        float y = f[i + 1] - link[1], z = f[i + 2];
                        if (!(fabsf(z - link[2]) < 6.0f) || !(y > -4.0f && y < 0.5f)) continue;
                        if (fs.n < FLOOR_SCAN_MAX) {
                            fs.addr[fs.n] = a + 4 * i;
                            memcpy(fs.val + 3 * fs.n, f + i, 12);
                            fs.n++;
                        } else
                            over = 1;
                    }
                    mem_read_release();
                }
                a += n;
            }
        }
    }
    memcpy(fs.link0, link, 12);
    rt_log("floor scan: snapshot of %.0f MB in %.0f ms: %d float triplets around and below Link at %.3f %.3f %.3f%s",
           bytes / 1e6, (plat_now_ns() - t0) / 1e6, fs.n, link[0], link[1], link[2],
           over ? " (more were left out)" : "");
}

static void floor_scan_read(int n, float *out) {
    static uint64_t (*src)[2];
    static uint8_t **dst, *ok;
    if (!src) src = malloc(sizeof *src * FLOOR_SCAN_MAX);
    if (!dst) dst = malloc(sizeof *dst * FLOOR_SCAN_MAX);
    if (!ok) ok = malloc(FLOOR_SCAN_MAX);
    if (!src || !dst || !ok) return;
    for (int i = 0; i < n; i++) src[i][0] = fs.addr[i], src[i][1] = 12, dst[i] = (uint8_t *)(out + 3 * i);
    mem_copy_readable(dst, (const uint64_t (*)[2])src, n, ok);
    for (int i = 0; i < n; i++)
        if (!ok[i]) out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = NAN;
}

static float hdist(const float a[3], const float b[3]) { return hypotf(a[0] - b[0], a[2] - b[2]); }

#define FLOOR_SCAN_BASES 4
#define FLOOR_SCAN_HOLDERS 24
static int main_data_word(uint64_t w) {
    return w >= MOD_MAIN_BASE + 0x4800000 && w < MOD_MAIN_BASE + MOD_MAIN_SIZE && !(w & 7);
}

static uint64_t vtable_below(uint64_t a, uint64_t *where) {
    uint64_t buf[0x80];
    uint8_t ok = 0, *dst = (uint8_t *)buf;
    uint64_t lo = (a & ~7ull) - 0x3f8, src[1][2] = {{lo, sizeof buf}};
    *where = 0;
    if (mem_copy_readable(&dst, (const uint64_t (*)[2])src, 1, &ok) != 1) return 0;
    for (int i = 0x7f; i >= 0; i--)
        if (main_data_word(buf[i])) {
            *where = lo + 8ull * i;
            return buf[i];
        }
    return 0;
}

static void floor_scan_dump(uint64_t a) {
    enum { PIECE = 16 << 20 };
    uint64_t base = (a & ~7ull) - 0x400, words[0xa0];
    uint8_t ok = 0, *dst = (uint8_t *)words;
    uint64_t src[1][2] = {{base, sizeof words}};
    if (mem_copy_readable(&dst, (const uint64_t (*)[2])src, 1, &ok) != 1) {
        rt_log("floor scan: the words around %#llx cannot be read", (unsigned long long)a);
        return;
    }
    rt_log("floor scan: the words from %#llx to %#llx around the floor point %#llx:", (unsigned long long)base,
           (unsigned long long)(base + sizeof words), (unsigned long long)a);
    for (int i = 0; i < 0xa0; i += 4)
        rt_log("floor scan:   %#llx: %016llx %016llx %016llx %016llx", (unsigned long long)(base + 8ull * i),
               (unsigned long long)words[i], (unsigned long long)words[i + 1], (unsigned long long)words[i + 2],
               (unsigned long long)words[i + 3]);
    uint64_t starts[FLOOR_SCAN_BASES];
    int nb = 0;
    for (int i = 0x9f; i >= 0; i--) {
        if (!main_data_word(words[i])) continue;
        uint64_t at = base + 8ull * i;
        rt_log("floor scan:   %#llx (point %+lld) holds main+%#llx", (unsigned long long)at, (long long)(at - a),
               (unsigned long long)(words[i] - MOD_MAIN_BASE));
        if (at <= a && nb < FLOOR_SCAN_BASES) starts[nb++] = at;
    }
    if (!nb) return;
    uint64_t t0 = plat_now_ns(), holder[FLOOR_SCAN_HOLDERS], held[FLOOR_SCAN_HOLDERS];
    int nh = 0;
    const RtGuestLayout *l = mem_layout();
    const RtRange ranges[2] = {l->heap, l->alias};
    for (int r = 0; r < 2 && nh < FLOOR_SCAN_HOLDERS; r++) {
        uint64_t x = ranges[r].base, end = ranges[r].base + ranges[r].size;
        while (x < end && nh < FLOOR_SCAN_HOLDERS) {
            RtMemoryInfo mi;
            if (mem_query(x, &mi) || !mi.size) break;
            uint64_t stop = mi.base + mi.size < end ? mi.base + mi.size : end;
            if (!mi.state || (mi.perm & 3) != 3) {
                x = stop;
                continue;
            }
            while (x < stop && nh < FLOOR_SCAN_HOLDERS) {
                uint64_t n = stop - x < PIECE ? stop - x : PIECE;
                if (n >= 8 && mem_read_hold(x, n)) {
                    const uint64_t *w = (const uint64_t *)(uintptr_t)x;
                    for (uint64_t i = 0; i < n / 8 && nh < FLOOR_SCAN_HOLDERS; i++)
                        for (int b = 0; b < nb; b++)
                            if (w[i] == starts[b]) holder[nh] = x + 8 * i, held[nh++] = starts[b];
                    mem_read_release();
                }
                x += n;
            }
        }
    }
    rt_log("floor scan: %d holders of the %d object starts below the point found in %.0f ms", nh, nb,
           (plat_now_ns() - t0) / 1e6);
    for (int h = 0; h < nh; h++) {
        uint64_t at = 0, vt = vtable_below(holder[h], &at);
        rt_log("floor scan:   %#llx holds %#llx; its object's vtable main+%#llx at %#llx (holder %+lld)",
               (unsigned long long)holder[h], (unsigned long long)held[h],
               (unsigned long long)(vt ? vt - MOD_MAIN_BASE : 0), (unsigned long long)at,
               (long long)(at ? holder[h] - at : 0));
    }
}

static void floor_scan(const float link[3], int link_ok) {
    if (floor_scan_left < 0) {
        const char *e = cfg_get("NX2SARIA_FLOOR_SCAN");
        int n = e && *e ? atoi(e) : 0;
        if (n <= 0 && floor_watch_wanted()) n = 1;
        floor_scan_left = !RT_V111 || n <= 0 ? 0 : n == 1 ? 20000 : n;
        if (floor_scan_left) rt_log("floor scan: on for %d lines", floor_scan_left);
    }
    if (!floor_scan_left) return;
    if (!link_ok) {
        fs.have_last = 0, fs.run = 0;
        return;
    }
    float move = fs.have_last ? hdist(link, fs.last) : 0, rise = fs.have_last ? fabsf(link[1] - fs.last[1]) : 0;
    int had = fs.have_last;
    memcpy(fs.last, link, 12);
    fs.have_last = 1;
    if (fs.state == 0) {
        double pct[2] = {0, 0};
        int still = !input_pad_stick_percent(pct) || pct[0] < 20.0;
        int drift = had && still && fabsf(frame_step_frames() - 1.0f) < 0.05f && move > 0.002f && move < 0.03f &&
                    rise < 0.005f;
        if (!drift) {
            fs.run = 0;
            return;
        }
        if (!fs.run) memcpy(fs.start, link, 12), fs.path = 0;
        fs.run++, fs.path += move;
        if (fs.run >= FLOOR_SCAN_ARM) {
            int straight = hdist(link, fs.start) >= 0.8f * fs.path;
            fs.run = 0;
            if (straight && fs.tries < 3) {
                fs.tries++;
                floor_scan_snapshot(link);
                if (fs.n) fs.state = 1;
            }
        }
        return;
    }
    if (fs.state == 1) {
        if (++fs.run < FLOOR_SCAN_ARM) return;
        fs.run = 0;
        float d_link = hdist(link, fs.link0);
        static float *now;
        if (!now) now = malloc(sizeof(float) * 3 * FLOOR_SCAN_MAX);
        if (!now) return;
        floor_scan_read(fs.n, now);
        int kept = 0, moved = 0;
        float tol = d_link * 0.2f > 0.02f ? d_link * 0.2f : 0.02f;
        for (int i = 0; i < fs.n; i++) {
            const float *v = now + 3 * i, *w = fs.val + 3 * i;
            if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) continue;
            float d = hypotf(v[0] - w[0], v[2] - w[2]);
            if (!(fabsf(d - d_link) <= tol) || d < 0.01f) continue;
            moved++;
            if (!(v[1] < link[1] - 0.3f || hdist(v, link) > 0.5f)) continue;
            if (kept < FLOOR_SCAN_SHOW) fs.addr[kept] = fs.addr[i], memcpy(fs.val + 3 * kept, v, 12);
            kept++;
        }
        rt_log("floor scan: %d of %d triplets moved with Link (%.3f m in %d presents); %d of those lie below him or "
               "away from his feet%s", moved, fs.n, d_link, FLOOR_SCAN_ARM, kept,
               kept > FLOOR_SCAN_SHOW ? " (the first of them are logged)" : "");
        if (!kept) {
            fs.state = 0, fs.n = 0;
            return;
        }
        fs.n = kept < FLOOR_SCAN_SHOW ? kept : FLOOR_SCAN_SHOW;
        int deep = 0;
        for (int k = 1; k < fs.n; k++)
            if (fs.val[3 * k + 1] < fs.val[3 * deep + 1]) deep = k;
        floor_scan_dump(fs.addr[deep]);
        if (floor_watch_wanted()) {
            int unit[FLOOR_SCAN_SHOW];
            for (int k = 0; k < fs.n; k++) {
                uint64_t at = 0;
                unit[k] = vtable_below(fs.addr[k], &at) == MOD_MAIN_BASE + 0x4bf8a88;
            }
            floor_watching = floor_watch_found(fs.addr, fs.val, unit, fs.n, link);
        }
        memcpy(fs.sec_link, link, 12);
        fs.sec_ns = plat_now_ns();
        fs.state = 2;
        return;
    }
    uint64_t t = plat_now_ns();
    if (t - fs.sec_ns < 1000000000ull) return;
    double secs = (t - fs.sec_ns) / 1e9;
    fs.sec_ns = t;
    float v[FLOOR_SCAN_SHOW * 3];
    floor_scan_read(fs.n, v);
    rt_log("floor scan: step %.4f frames; link %.4f %.4f %.4f moved %.4f %.4f %.4f in %.3f s (%.3f m/s)",
           (double)frame_step_frames(), link[0], link[1], link[2], link[0] - fs.sec_link[0], link[1] - fs.sec_link[1],
           link[2] - fs.sec_link[2], secs, hdist(link, fs.sec_link) / secs);
    memcpy(fs.sec_link, link, 12);
    floor_scan_left--;
    for (int k = 0; k < fs.n && floor_scan_left > 0; k++) {
        float *w = fs.val + 3 * k, *u = v + 3 * k;
        rt_log("floor scan:   %#llx at %.4f %.4f %.4f moved %.4f %.4f %.4f (%.3f m/s)", (unsigned long long)fs.addr[k],
               u[0], u[1], u[2], u[0] - w[0], u[1] - w[1], u[2] - w[2], hdist(u, w) / secs);
        memcpy(w, u, 12);
        floor_scan_left--;
    }
    if (floor_scan_left < 0) floor_scan_left = 0;
}

void frame_step_present(uint64_t display_ns, int interval) {
    atomic_fetch_add_explicit(&guest_presents, 1, memory_order_relaxed);
    game_interval = interval > 0 ? interval : 1;
    frame_limit_live();
    double step = 0, dt = last_ns ? (double)(display_ns - last_ns) * 1e-9 : 0;
    if (step_word && last_ns) {
        if (dt > STEP_RESET_S || dt < 0.001) {
            sample_count = sample_next = 0;
        } else {
            if (dt > STEP_MAX_S) dt = STEP_MAX_S;
            samples[sample_next++ % STEP_SAMPLES] = dt;
            if (sample_count < STEP_SAMPLES) sample_count++;
            double sum = 0;
            for (unsigned i = 0; i < sample_count; i++) sum += samples[i];
            step = sum / sample_count;
            if (step < STEP_MIN_S) step = STEP_MIN_S;
            step_store((float)step);
        }
    }
    if (frame_free_running() && last_ns) {
        if (dt > 0 && dt < STEP_RESET_S) ocarina_step_write(ocarina_step(dt) * (double)dbg_speed_factor());
        else ocarina_step_reset();
    }
    last_ns = display_ns;
    double game = step_word ? step_now : ostep_last > 0 ? ostep_last / 60.0 : (interval > 0 ? interval : 1) / 60.0;
    atomic_store_explicit(&game_us, atomic_load_explicit(&game_us, memory_order_relaxed) + (uint64_t)(game * 1e6 + 0.5),
                          memory_order_relaxed);
    world_clock(dt, step);
    ocarina_watch(display_ns);
    floor_probe();
    if (floor_scan_left || floor_watching) {
        float link[3];
        int ok = floor_scan_left > 0 || floor_watching ? player_pos(link) : 0;
        if (floor_scan_left) floor_scan(link, ok);
        if (floor_watching) floor_watching = floor_watch_present(link, ok, frame_step_frames());
    }
    if (rt_trace & RT_TRACE_PERF) {
        time_dump();
        if (frame_free_running()) {
            double s = ocarina_step_s();
            step = s > 0 ? s : 0;
        }
        speed_report(display_ns, step, interval);
    }
}
