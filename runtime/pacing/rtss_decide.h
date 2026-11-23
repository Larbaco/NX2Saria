#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    RTSS_UNSET = -2,
    RTSS_OUT_SETTING = -1,
    RTSS_OUT_FRAMEGEN = -3,
    RTSS_OUT_LOWLAT = -4,
};

static inline const char *rtss_out_text(int64_t want) {
    return want == RTSS_OUT_FRAMEGEN ? "frame generation"
           : want == RTSS_OUT_LOWLAT ? "the driver's low latency"
                                     : "NX2SARIA_RTSS=0";
}

static inline const char *rtss_out_limiter(int64_t want) {
    return want == RTSS_OUT_FRAMEGEN ? "frame generation and Reflex or the game's limiter"
           : want == RTSS_OUT_LOWLAT ? "the driver's low latency"
                                     : "the game's limiter";
}

#define RTSS_SIGNATURE 0x52545353u
#define RTSS_API_MASK 0xFFFFu
#define RTSS_API_VULKAN 0xAu
#define RTSS_UPDATE_ASKED 0x10000000u
enum { RTSS_FLAGS_AT = 4 + 260, RTSS_PERIOD_AT = RTSS_FLAGS_AT + 4, RTSS_ENTRY_MIN = RTSS_PERIOD_AT + 12,
       RTSS_APPS_MAX = 4096 };

typedef struct {
    int running;
    int listed;
    uint32_t flags;
    uint32_t t0, t1, frames;
} RtssApp;

static inline void rtss_parse(const uint8_t *p, size_t size, uint32_t pid, RtssApp *a) {
    memset(a, 0, sizeof *a);
    if (!p || size < 20) return;
    uint32_t h[5];
    memcpy(h, p, sizeof h);
    a->running = h[0] == RTSS_SIGNATURE && h[1] >= 0x20000;
    for (uint32_t i = 0; a->running && h[2] >= RTSS_ENTRY_MIN && i < h[4] && i < RTSS_APPS_MAX; i++) {
        uint64_t at = (uint64_t)h[3] + (uint64_t)i * h[2];
        if (at + RTSS_ENTRY_MIN > size) break;
        uint32_t id, t[3];
        memcpy(&id, p + at, 4);
        if (id != pid) continue;
        a->listed = 1;
        memcpy(&a->flags, p + at + RTSS_FLAGS_AT, 4);
        memcpy(t, p + at + RTSS_PERIOD_AT, sizeof t);
        a->t0 = t[0], a->t1 = t[1], a->frames = t[2];
        break;
    }
}

static inline double rtss_fps(const RtssApp *a) {
    return a->listed && a->t1 > a->t0 ? 1000.0 * a->frames / (a->t1 - a->t0) : 0;
}

#define RTSS_OVER_PERIODS 3
#define RTSS_OVER_SLACK 1.05
#define RTSS_FRESH_NS 2500000000ull
#define RTSS_HOLD_IDLE_NS 200000000ull
#define RTSS_SETTLE_POLLS 2
#define RTSS_UNPROVEN_POLLS 60

enum {
    RTSS_FAIL_NONE,
    RTSS_FAIL_LIBRARY,
    RTSS_FAIL_READONLY,
    RTSS_FAIL_NOT_LIMITING,
    RTSS_FAIL_UNPROVEN,
};
enum { RTSS_DO_NOTHING, RTSS_DO_APPLY, RTSS_DO_PUT_BACK };
enum {
    RTSS_WHY_HOLDS,
    RTSS_WHY_NOT_LISTED,
    RTSS_WHY_OUT,
    RTSS_WHY_FAILED,
    RTSS_WHY_NOT_RUNNING,
    RTSS_WHY_REFUSED,
    RTSS_WHY_NEWER,
    RTSS_WHY_UNSET,
    RTSS_WHY_MOVIE,
    RTSS_WHY_NOT_VULKAN,
    RTSS_WHY_UPDATE,
    RTSS_WHY_NOT_COUNTING,
    RTSS_WHY_IDLE,
    RTSS_WHY_SETTLING,
    RTSS_WHY_NO_PRESENT,
};

typedef struct {
    int64_t applied;
    int64_t refused;
    int changed;
    int over;
    int fail;
    int held;
    uint32_t t1, span;
    uint64_t fresh_ns;
    int64_t seen;
    int polls;
    int unproven;
} RtssState;
#define RTSS_STATE_INIT {-1, -1, 0, 0, RTSS_FAIL_NONE, 0, 0, 0, 0, RTSS_UNSET, 0, 0}

typedef struct {
    int64_t want;
    int current;
    int hooks;
    int64_t movie_mhz;
    uint64_t now_ns;
    uint64_t hosted_ns;
    RtssApp app;
} RtssIn;

typedef struct {
    int act;
    int64_t mhz;
    int holds;
    int why;
    double fps;
} RtssOut;

static inline int64_t rtss_target(int64_t w, int64_t movie_mhz) { return w > 0 && movie_mhz > w ? 0 : w; }

static inline void rtss_limit_value(int64_t mhz, int has_den, uint32_t *limit, uint32_t *den) {
    *den = 1;
    if (mhz % 1000 == 0) *limit = (uint32_t)(mhz / 1000);
    else if (has_den) *limit = (uint32_t)mhz, *den = 1000;
    else *limit = (uint32_t)(mhz / 1000);
}

static inline int rtss_wants_hooks(const RtssState *s, const RtssIn *in) {
    return in->want >= 0 && !s->fail && in->app.running;
}

static inline RtssOut rtss_decide(RtssState *s, const RtssIn *in) {
    int64_t w = in->want;
    RtssOut o = {RTSS_DO_NOTHING, w, 0, RTSS_WHY_NOT_LISTED, 0};
    if (w == RTSS_UNSET) return o.why = RTSS_WHY_UNSET, o;
    const RtssApp *a = &in->app;
    int fresh = a->listed && a->t1 > a->t0 && a->frames && a->t1 != s->t1;
    if (fresh) s->t1 = a->t1, s->span = a->t1 - a->t0, s->fresh_ns = in->now_ns;
    int held = s->held, unproven = s->unproven;
    s->held = s->unproven = 0;
    s->polls = w == s->seen ? s->polls + 1 : 1;
    s->seen = w;
    int settled = s->polls >= RTSS_SETTLE_POLLS;
    if (w < 0) {
        if (settled && in->hooks == 1 && s->changed) o.act = RTSS_DO_PUT_BACK;
        s->over = 0;
        return o.why = RTSS_WHY_OUT, o;
    }
    if (s->fail) return o.why = RTSS_WHY_FAILED, o;
    if (!a->running) return o.why = RTSS_WHY_NOT_RUNNING, o;
    if (in->hooks == 0) return s->fail = RTSS_FAIL_LIBRARY, o.why = RTSS_WHY_FAILED, o;
    if (!in->hosted_ns) return o.why = RTSS_WHY_NO_PRESENT, o;
    int64_t t = o.mhz = rtss_target(w, in->movie_mhz);
    if (t == s->refused) return o.why = RTSS_WHY_REFUSED, o;
    if (t != s->applied && !settled) return o.why = RTSS_WHY_SETTLING, o;
    if (t != s->applied) {
        o.act = RTSS_DO_APPLY;
        o.why = in->movie_mhz ? RTSS_WHY_MOVIE : a->listed ? RTSS_WHY_UPDATE : RTSS_WHY_NOT_LISTED;
        s->over = 0;
        return o;
    }
    if (!in->current) return o.why = RTSS_WHY_NEWER, o;
    uint64_t idle = in->now_ns > in->hosted_ns ? in->now_ns - in->hosted_ns : 0;
    int shown = idle < RTSS_HOLD_IDLE_NS;
    uint64_t stale = (uint64_t)s->span * 2500000ull > RTSS_FRESH_NS ? (uint64_t)s->span * 2500000ull : RTSS_FRESH_NS;
    int unseen = !a->listed                                       ? RTSS_WHY_NOT_LISTED
                 : (a->flags & RTSS_API_MASK) != RTSS_API_VULKAN   ? RTSS_WHY_NOT_VULKAN
                 : a->flags & RTSS_UPDATE_ASKED                    ? RTSS_WHY_UPDATE
                 : !s->fresh_ns || in->now_ns - s->fresh_ns > stale ? RTSS_WHY_NOT_COUNTING
                                                                   : RTSS_WHY_HOLDS;
    if (unseen != RTSS_WHY_HOLDS) {
        s->unproven = a->listed && t > 0 && shown ? unproven + 1 : 0;
        if (s->unproven < RTSS_UNPROVEN_POLLS) return o.why = unseen, o;
        s->fail = RTSS_FAIL_UNPROVEN;
        o.act = RTSS_DO_PUT_BACK, o.why = RTSS_WHY_FAILED;
        return o;
    }
    if (fresh && held && t > 0) {
        double fps = rtss_fps(a);
        s->over = fps > t / 1000.0 * RTSS_OVER_SLACK ? s->over + 1 : 0;
        if (s->over == RTSS_OVER_PERIODS) {
            s->fail = RTSS_FAIL_NOT_LIMITING;
            o.act = RTSS_DO_PUT_BACK, o.why = RTSS_WHY_FAILED, o.fps = fps;
            return o;
        }
    }
    if (in->movie_mhz) return o.why = RTSS_WHY_MOVIE, o;
    if (!shown) return o.why = RTSS_WHY_IDLE, o;
    s->held = o.holds = 1;
    return o.why = RTSS_WHY_HOLDS, o;
}

enum {
    RTSS_WROTE_READONLY = -1,
    RTSS_WROTE_REFUSED = 0,
    RTSS_WROTE_HELD = 1,
    RTSS_WROTE_REFUSED_CLEARED = 2,
};

static inline void rtss_written(RtssState *s, RtssOut *o, int r) {
    if (r == RTSS_WROTE_READONLY) {
        s->fail = RTSS_FAIL_READONLY, o->holds = 0, o->why = RTSS_WHY_FAILED;
        return;
    }
    s->changed = 1;
    if (r == RTSS_WROTE_HELD) {
        s->applied = o->mhz;
        return;
    }
    if (r == RTSS_WROTE_REFUSED_CLEARED) s->applied = 0;
    s->refused = o->mhz, o->holds = 0, o->why = RTSS_WHY_REFUSED;
}

static inline void rtss_put_back_done(RtssState *s) { s->changed = 0, s->applied = -1; }

typedef struct {
    void *ctx;
    int (*apply)(void *ctx, int64_t mhz, int first, int clear);
    void (*put_back)(void *ctx, const RtssOut *o);
} RtssOps;

static inline RtssOut rtss_poll(RtssState *s, const RtssIn *in, const RtssOps *ops) {
    RtssOut o = rtss_decide(s, in);
    if (o.act == RTSS_DO_APPLY) {
        rtss_written(s, &o, ops->apply(ops->ctx, o.mhz, !s->changed, s->applied != 0));
    } else if (o.act == RTSS_DO_PUT_BACK) {
        ops->put_back(ops->ctx, &o);
        rtss_put_back_done(s);
    }
    return o;
}

static inline const char *rtss_fail_text(int fail) {
    return fail == RTSS_FAIL_READONLY       ? "RivaTuner Statistics Server's profile cannot be changed"
           : fail == RTSS_FAIL_NOT_LIMITING ? "RivaTuner Statistics Server does not hold the game to the limit"
           : fail == RTSS_FAIL_UNPROVEN     ? "RivaTuner Statistics Server never showed it holds the game"
                                            : "RivaTuner Statistics Server's hooks library did not load";
}

static inline void rtss_why_text(char *out, size_t n, const RtssOut *o, int fail) {
    if (o->why == RTSS_WHY_OUT) snprintf(out, n, "RivaTuner Statistics Server kept out: %s", rtss_out_text(o->mhz));
    else if (o->why == RTSS_WHY_FAILED) snprintf(out, n, "%s", rtss_fail_text(fail));
    else if (o->why == RTSS_WHY_NOT_RUNNING) snprintf(out, n, "RivaTuner Statistics Server is not running");
    else if (o->why == RTSS_WHY_REFUSED) snprintf(out, n, "RivaTuner Statistics Server refused this limit");
    else if (o->why == RTSS_WHY_MOVIE) snprintf(out, n, "a movie plays: the game keeps its pictures on time");
    else if (o->why == RTSS_WHY_NOT_VULKAN)
        snprintf(out, n, "RivaTuner Statistics Server lists the game but has not seen its Vulkan presents yet");
    else if (o->why == RTSS_WHY_UPDATE)
        snprintf(out, n, "RivaTuner Statistics Server has not read the game's limit from its profile yet");
    else if (o->why == RTSS_WHY_NOT_COUNTING)
        snprintf(out, n, "RivaTuner Statistics Server has measured no frames of the game lately");
    else if (o->why == RTSS_WHY_IDLE) snprintf(out, n, "no picture went out lately: RivaTuner has none to hold");
    else if (o->why == RTSS_WHY_SETTLING) snprintf(out, n, "a new limit: RivaTuner Statistics Server gets it once it stays");
    else if (o->why == RTSS_WHY_NO_PRESENT) snprintf(out, n, "no picture has gone out yet");
    else snprintf(out, n, "RivaTuner Statistics Server has not hooked the game yet");
}

static inline uint64_t rtss_pace_period(int holds, int movie, uint64_t now_ns, uint64_t hosted_ns, uint64_t period,
                                        uint64_t ceiling) {
    uint64_t idle = now_ns > hosted_ns ? now_ns - hosted_ns : 0;
    if (!holds || movie || !hosted_ns || idle >= RTSS_HOLD_IDLE_NS) return period;
    return ceiling < period ? ceiling : period;
}

#define RTSS_SAVED_HEADER                                                                                              \
    "# RivaTuner Statistics Server's profiles as they were before the game changed them (rtss_limit.c): existed, old\n" \
    "# limit and denominator, the game's limit and denominator, the game's previous ones, profile. The game puts a\n"   \
    "# profile back and removes its line when it closes; a start that finds a line puts that profile back first.\n"
enum { RTSS_NAME_MAX = 260 };

typedef struct {
    int existed;
    uint32_t old_limit, old_den;
    uint32_t limit, den;
    uint32_t prev_limit, prev_den;
    char name[RTSS_NAME_MAX];
} RtssSaved;

static inline size_t rtss_saved_format(char *out, size_t n, const RtssSaved *r) {
    int k = snprintf(out, n, "rtss1 %d %u %u %u %u %u %u %s\n", r->existed ? 1 : 0, r->old_limit, r->old_den,
                     r->limit, r->den, r->prev_limit, r->prev_den, r->name);
    return k > 0 && (size_t)k < n ? (size_t)k : 0;
}

static inline int rtss_saved_parse(const char *line, size_t len, RtssSaved *r) {
    char buf[RTSS_NAME_MAX + 96];
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
    if (len >= sizeof buf) return 0;
    memcpy(buf, line, len);
    buf[len] = 0;
    int existed = -1, at = 0;
    unsigned v[6];
    if (sscanf(buf, "rtss1 %d %u %u %u %u %u %u %n", &existed, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &at) != 7 ||
        !at)
        return 0;
    size_t name = len - (size_t)at;
    if ((existed != 0 && existed != 1) || !v[1] || !v[3] || !v[5] || !name || name >= RTSS_NAME_MAX) return 0;
    r->existed = existed, r->old_limit = v[0], r->old_den = v[1], r->limit = v[2], r->den = v[3];
    r->prev_limit = v[4], r->prev_den = v[5];
    memcpy(r->name, buf + at, name + 1);
    return 1;
}

static inline int rtss_name_eq(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static inline int rtss_saved_find(const char *text, const char *name, RtssSaved *r) {
    for (const char *p = text; p && *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (*p != '#' && rtss_saved_parse(p, len, r) && rtss_name_eq(r->name, name)) return 1;
        p = end ? end + 1 : NULL;
    }
    return 0;
}

static inline size_t rtss_saved_update(const char *text, const char *name, const RtssSaved *r, char *out, size_t cap,
                                       int *records) {
    size_t at = (size_t)snprintf(out, cap, "%s", RTSS_SAVED_HEADER);
    *records = 0;
    if (at >= cap) return 0;
    for (const char *p = text; p && *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        RtssSaved other;
        if (*p != '#' && rtss_saved_parse(p, len, &other) && !rtss_name_eq(other.name, name)) {
            size_t k = rtss_saved_format(out + at, cap - at, &other);
            if (!k) return 0;
            at += k, ++*records;
        }
        p = end ? end + 1 : NULL;
    }
    if (r) {
        size_t k = rtss_saved_format(out + at, cap - at, r);
        if (!k) return 0;
        at += k, ++*records;
    }
    return at;
}

enum {
    RTSS_PLAN_NONE,
    RTSS_PLAN_LEAVE,
    RTSS_PLAN_WRITE,
    RTSS_PLAN_DELETE,
};
static inline int rtss_restore_plan(const RtssSaved *r, int exists, uint32_t limit, uint32_t den) {
    if (!exists) return r->existed ? RTSS_PLAN_LEAVE : RTSS_PLAN_NONE;
    den = den ? den : 1;
    if ((limit != r->limit || den != r->den) && (limit != r->prev_limit || den != r->prev_den)) return RTSS_PLAN_LEAVE;
    return r->existed ? RTSS_PLAN_WRITE : RTSS_PLAN_DELETE;
}

typedef struct {
    int limit, limit_den, time, time_den, scan0, scan1;
    int sync_display, reflex_sleep, passive_wait;
} RtssSettings;

static inline int rtss_settings_limit(const RtssSettings *s) { return s->limit || s->time || s->scan0 || s->scan1; }

static inline int rtss_out_of_limit(int64_t want, int fail) {
    return want != RTSS_UNSET &&
           (want < 0 || fail == RTSS_FAIL_READONLY || fail == RTSS_FAIL_LIBRARY || fail == RTSS_FAIL_UNPROVEN);
}

static inline int rtss_two(int64_t want, int fail, int listed, int own_limit) {
    return rtss_out_of_limit(want, fail) && listed && own_limit;
}

static inline void rtss_settings_text(char *out, size_t n, const RtssSettings *s, int modes) {
    int k = snprintf(out, n, "limit %d/%d, frame time limit %d/%d, scanline sync %d/%d", s->limit, s->limit_den, s->time,
                     s->time_den, s->scan0, s->scan1);
    if (modes && k >= 0 && (size_t)k < n)
        snprintf(out + k, n - (size_t)k, ", display sync %d, Reflex sleep %d, passive wait %d", s->sync_display,
                 s->reflex_sleep, s->passive_wait);
}
