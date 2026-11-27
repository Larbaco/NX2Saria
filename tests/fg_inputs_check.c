#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "vk_fg_inputs.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static pthread_mutex_t real = PTHREAD_MUTEX_INITIALIZER;
void plat_mutex_lock(PlatMutex *m) { (void)m, pthread_mutex_lock(&real); }
void plat_mutex_unlock(PlatMutex *m) { (void)m, pthread_mutex_unlock(&real); }

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void spin_ns(uint64_t ns) {
    const uint64_t t0 = now_ns();
    while (now_ns() - t0 < ns) {}
}

static VkDlaaCamera stamp_cam(uint64_t f) {
    VkDlaaCamera c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 16; i++) c.view_to_clip[i] = c.clip_to_prev_clip[i] = (float)f;
    c.pos[0] = c.fwd[2] = c.fov_y = c.znear = (float)f;
    return c;
}

static VkDlaaFgSet stamp_set(uint64_t f, uint64_t made) {
    VkDlaaFgSet s;
    memset(&s, 0, sizeof s);
    s.depth.width = s.depth.height = s.mv.width = s.mv.height = (uint32_t)f;
    s.depth_view = (VkImageView)(uintptr_t)f, s.mv_view = (VkImageView)(uintptr_t)(f + 1000000);
    s.jx = s.jy = (float)f;
    s.cam = stamp_cam(f), s.cam_ok = 1, s.made = made;
    return s;
}

static VkDlaaFgHudless stamp_hl(uint64_t f, uint64_t made) {
    VkDlaaFgHudless h = {(VkImage)(uintptr_t)f, (VkImageView)(uintptr_t)f, (uint32_t)f, (uint32_t)f, VK_FORMAT_R8G8B8A8_UNORM,
                         made, 0};
    return h;
}

static int mixed(const VkDlaaFgSet *s) {
    const float f = (float)s->depth.width;
    if (s->depth.height != s->depth.width || s->mv.width != s->depth.width || s->mv.height != s->depth.width) return 1;
    if ((uintptr_t)s->depth_view != s->depth.width || (uintptr_t)s->mv_view != s->depth.width + 1000000ull) return 1;
    if (s->jx != f || s->jy != f || s->cam.pos[0] != f || s->cam.fwd[2] != f || s->cam.fov_y != f || s->cam.znear != f)
        return 1;
    for (int i = 0; i < 16; i++)
        if (s->cam.view_to_clip[i] != f || s->cam.clip_to_prev_clip[i] != f) return 1;
    return 0;
}

static struct {
    VkDlaaCamera cam;
    int cam_ok;
    VkFfxImage depth, mv;
    VkImageView depth_view, mv_view;
    float jx, jy;
    int fresh;
    uint64_t at;
    VkDlaaFgHudless hl;
    int hl_fresh;
    uint64_t hl_at;
} O;

static void old_camera(uint64_t f) {
    const VkDlaaCamera c = stamp_cam(f);
    volatile float *d = (volatile float *)&O.cam;
    const float *src = (const float *)&c;
    *(volatile int *)&O.cam_ok = 0;
    for (size_t i = 0; i < sizeof c / sizeof(float); i++) d[i] = src[i];
    *(volatile int *)&O.cam_ok = 1;
}

static void old_publish(uint64_t f, uint64_t presents) {
    const VkDlaaFgSet s = stamp_set(f, presents);
    *(volatile VkDlaaFgHudless *)&O.hl = stamp_hl(f, presents);
    *(volatile uint64_t *)&O.hl_at = presents, *(volatile int *)&O.hl_fresh = 1;
    *(volatile VkFfxImage *)&O.depth = s.depth, *(volatile VkFfxImage *)&O.mv = s.mv;
    *(volatile VkImageView *)&O.depth_view = s.depth_view, *(volatile VkImageView *)&O.mv_view = s.mv_view;
    *(volatile float *)&O.jx = s.jx, *(volatile float *)&O.jy = s.jy;
    *(volatile uint64_t *)&O.at = presents, *(volatile int *)&O.fresh = 1;
}

static int old_take(uint64_t presents, VkDlaaFgSet *out) {
    if (!*(volatile int *)&O.fresh) return 0;
    *(volatile int *)&O.fresh = 0;
    if (presents - *(volatile uint64_t *)&O.at > FGI_MAX_AGE) return 0;
    memset(out, 0, sizeof *out);
    out->depth = *(volatile VkFfxImage *)&O.depth, out->mv = *(volatile VkFfxImage *)&O.mv;
    out->depth_view = *(volatile VkImageView *)&O.depth_view, out->mv_view = *(volatile VkImageView *)&O.mv_view;
    out->jx = *(volatile float *)&O.jx, out->jy = *(volatile float *)&O.jy;
    if (!*(volatile int *)&O.cam_ok) return 1;
    volatile const float *s = (volatile const float *)&O.cam;
    float *d = (float *)&out->cam;
    for (size_t i = 0; i < sizeof out->cam / sizeof(float); i++) d[i] = s[i];
    out->cam_ok = 1;
    return 1;
}

static int old_take_hudless(uint64_t presents, VkDlaaFgHudless *out) {
    if (!*(volatile int *)&O.hl_fresh) return 0;
    *(volatile int *)&O.hl_fresh = 0;
    if (presents - *(volatile uint64_t *)&O.hl_at > FGI_MAX_AGE) return 0;
    *out = *(volatile VkDlaaFgHudless *)&O.hl;
    return 1;
}

#define ROUNDS 2000
static FgiBox B;
static int snapshot, two_threads;
static pthread_mutex_t pl = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcv = PTHREAD_COND_INITIALIZER;
static uint64_t job, handed, replayed, presents;
static int pending, busy, done, reading;
static struct {
    int got, lost, mixed, other_frame, hl_got, hl_other, overlaps;
} R;

static void drain(void) {
    pthread_mutex_lock(&pl);
    while (pending || busy) pthread_cond_wait(&pcv, &pl);
    pthread_mutex_unlock(&pl);
}

static void overlap(void) {
    if (__atomic_load_n(&reading, __ATOMIC_ACQUIRE)) R.overlaps++;
}

static void present_call(uint64_t f) {
    __atomic_store_n(&presents, f, __ATOMIC_RELAXED);
    pthread_mutex_lock(&pl);
    while (pending) pthread_cond_wait(&pcv, &pl);
    job = f, pending = 1, handed = f;
    pthread_cond_broadcast(&pcv);
    pthread_mutex_unlock(&pl);
}

static void replay_frame(uint64_t f) {
    pthread_mutex_lock(&pl);
    while (handed + 1 < f - 1 && f > 1) pthread_cond_wait(&pcv, &pl);
    pthread_mutex_unlock(&pl);
    drain();
    spin_ns(100000);
    overlap();
    VkDlaaCamera cam = stamp_cam(f);
    if (!snapshot) old_camera(f);
    spin_ns(150000);
    overlap();
    const uint64_t made = __atomic_load_n(&presents, __ATOMIC_RELAXED);
    if (snapshot) {
        VkDlaaFgSet s = stamp_set(f, made);
        s.cam = cam;
        const VkDlaaFgHudless h = stamp_hl(f, made);
        fgi_put_hudless(&B, &h);
        fgi_put(&B, &s);
    } else {
        old_publish(f, made);
    }
}

static void *game(void *arg) {
    (void)arg;
    for (uint64_t f = 1; f <= ROUNDS; f++) {
        replay_frame(f);
        if (!two_threads) {
            present_call(f);
            continue;
        }
        pthread_mutex_lock(&pl);
        replayed = f;
        pthread_cond_broadcast(&pcv);
        pthread_mutex_unlock(&pl);
    }
    return NULL;
}

static void *game_presents(void *arg) {
    (void)arg;
    uint32_t rnd = 777;
    for (uint64_t f = 1; f <= ROUNDS; f++) {
        pthread_mutex_lock(&pl);
        while (replayed < f) pthread_cond_wait(&pcv, &pl);
        pthread_mutex_unlock(&pl);
        rnd = rnd * 1103515245u + 12345u;
        spin_ns((rnd >> 8) % 200000);
        present_call(f);
    }
    return NULL;
}

static void *present_thread(void *arg) {
    (void)arg;
    uint32_t rnd = 12345;
    for (;;) {
        pthread_mutex_lock(&pl);
        while (!pending && !done) pthread_cond_wait(&pcv, &pl);
        if (!pending) {
            pthread_mutex_unlock(&pl);
            return NULL;
        }
        const uint64_t f = job;
        pending = 0, busy = 1;
        pthread_cond_broadcast(&pcv);
        pthread_mutex_unlock(&pl);
        __atomic_store_n(&reading, 1, __ATOMIC_RELEASE);
        rnd = rnd * 1103515245u + 12345u;
        spin_ns((rnd >> 8) % 350000);
        const uint64_t now = __atomic_load_n(&presents, __ATOMIC_RELAXED);
        VkDlaaFgHudless h;
        if (snapshot ? fgi_take_hudless(&B, now, &h) : old_take_hudless(now, &h))
            R.hl_got++, R.hl_other += (uintptr_t)h.img != f;
        spin_ns(5000);
        VkDlaaFgSet s;
        if (snapshot ? fgi_take(&B, now, &s) : old_take(now, &s)) {
            R.got++;
            R.mixed += mixed(&s);
            R.other_frame += s.depth.width != f;
        } else {
            R.lost++;
        }
        spin_ns(20000);
        __atomic_store_n(&reading, 0, __ATOMIC_RELEASE);
        pthread_mutex_lock(&pl);
        busy = 0;
        pthread_cond_broadcast(&pcv);
        pthread_mutex_unlock(&pl);
    }
}

static void run(int with_snapshot, int threads) {
    memset(&O, 0, sizeof O), memset(&B, 0, sizeof B), memset(&R, 0, sizeof R);
    snapshot = with_snapshot, two_threads = threads == 2;
    job = handed = replayed = presents = 0, pending = busy = done = reading = 0;
    pthread_t g, gp = 0, p;
    pthread_create(&p, NULL, present_thread, NULL);
    pthread_create(&g, NULL, game, NULL);
    if (two_threads) pthread_create(&gp, NULL, game_presents, NULL);
    pthread_join(g, NULL);
    if (two_threads) pthread_join(gp, NULL);
    drain();
    pthread_mutex_lock(&pl);
    done = 1;
    pthread_cond_broadcast(&pcv);
    pthread_mutex_unlock(&pl);
    pthread_join(p, NULL);
}

static void report(const char *how) {
    printf("fg_inputs_check: %s, %d presents: %d took inputs, %d none; %d sets mixed two frames, %d another frame's than "
           "the one presented; %d HUD-less pictures, %d another frame's; %d replay writes while the present read\n",
           how, ROUNDS, R.got, R.lost, R.mixed, R.other_frame, R.hl_got, R.hl_other, R.overlaps);
}

static void unit(void) {
    FgiBox b;
    memset(&b, 0, sizeof b);
    VkDlaaFgSet s;
    VkDlaaFgHudless h;
    VkDlaaCamera c;
    CHECK(!fgi_take(&b, 5, &s) && !fgi_take_hudless(&b, 5, &h) && !fgi_camera(&b, &c), "an empty box hands nothing out");
    const VkDlaaFgSet a = stamp_set(3, 5);
    fgi_put(&b, &a);
    CHECK(fgi_take(&b, 6, &s) && s.made == 5 && s.age == 0 && !mixed(&s) && s.depth.width == 3,
          "a set made before this present is this frame's (age 0)");
    CHECK(!fgi_take(&b, 6, &s), "a set is handed out once");
    CHECK(fgi_camera(&b, &c) && c.pos[0] == 3.0f, "the camera stays readable after the set was taken");
    const VkDlaaFgSet old = stamp_set(4, 5);
    fgi_put(&b, &old);
    CHECK(!fgi_take(&b, 5 + FGI_MAX_AGE + 1, &s), "a set more than %d presents old is not handed out", FGI_MAX_AGE);
    CHECK(!fgi_take(&b, 6, &s), "and counts as taken");
    const VkDlaaFgSet s7 = stamp_set(7, 10), s8 = stamp_set(8, 11);
    fgi_put(&b, &s7), fgi_put(&b, &s8);
    CHECK(fgi_take(&b, 12, &s) && s.depth.width == 8 && !mixed(&s), "the newest of two sets put before a present");
    const VkDlaaFgSet s9 = stamp_set(9, 20);
    fgi_put(&b, &s9);
    CHECK(fgi_take(&b, 23, &s) && s.age == 2, "a set made two presents before the last one is 2 presents back");
    const VkDlaaFgSet s10 = stamp_set(10, 30);
    const VkDlaaFgHudless h10 = stamp_hl(10, 30);
    fgi_put(&b, &s10), fgi_put_hudless(&b, &h10);
    fgi_withdraw(&b, 1, 0);
    CHECK(!fgi_take(&b, 31, &s) && !fgi_camera(&b, &c), "a withdrawn set is not handed out, nor its camera");
    CHECK(fgi_take_hudless(&b, 31, &h) && (uintptr_t)h.img == 10 && h.age == 0, "the HUD-less picture is withdrawn apart");
    CHECK(!fgi_take_hudless(&b, 31, &h), "a HUD-less picture is handed out once");
    const VkDlaaFgHudless h11 = stamp_hl(11, 40);
    fgi_put_hudless(&b, &h11);
    fgi_withdraw(&b, 0, 1);
    CHECK(!fgi_take_hudless(&b, 41, &h), "a withdrawn HUD-less picture is not handed out");
    const VkDlaaFgSet s12 = stamp_set(12, 50);
    fgi_put(&b, &s12);
    CHECK(fgi_take(&b, 51, &s) && s.depth.width == 12, "a set put after a withdrawal is handed out");
}

int main(void) {
    unit();
    run(0, 1);
    report("one game thread, fields as the replay wrote them up to 0.28.0");
    CHECK(!R.mixed && !R.other_frame && !R.lost && !R.hl_other && !R.overlaps,
          "the drain before each submission keeps the replay out of the present's reads");
    run(1, 1);
    report("one game thread, one snapshot a frame");
    CHECK(!R.mixed && !R.other_frame && !R.hl_other && R.got == ROUNDS && R.hl_got == ROUNDS && !R.overlaps,
          "every present took its own frame's set and picture");
    run(0, 2);
    report("two game threads, fields as the replay wrote them up to 0.28.0");
    const int old_mixed = R.mixed;
    CHECK(R.overlaps > 0 && R.mixed > 0, "the detector must see the replay write while the present reads (it works)");
    run(1, 2);
    report("two game threads, one snapshot a frame");
    CHECK(R.overlaps > 0, "the replay still writes while the present reads");
    CHECK(!R.mixed, "%d sets mixed two frames", R.mixed);
    printf("fg_inputs_check: sets that mixed two frames with two game threads: %d without the snapshot, %d with it\n",
           old_mixed, R.mixed);
    printf(fails ? "fg_inputs_check: %d FAILED\n" : "fg_inputs_check: OK\n", fails);
    return fails != 0;
}
