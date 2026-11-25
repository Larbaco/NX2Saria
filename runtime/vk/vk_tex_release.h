#pragma once
#include <stdint.h>

enum { TRL_STORAGE, TRL_GPU };

#define TRL_HOLD_S 60
#define TRL_HOLD_MAX_S 900
#define TRL_ROOM_TIMES 2
#define TRL_ROOM_MARGIN 10
#define TRL_CAP_SHARE 32
#define TRL_CAP_MIN_MB 384
#define TRL_CAP_MAX_MB 1024
#define TRL_CAP_UNKNOWN_MB 512

static inline uint32_t trl_cap_mb(uint64_t budget_mb) {
    if (!budget_mb) return TRL_CAP_UNKNOWN_MB;
    uint64_t mb = budget_mb / TRL_CAP_SHARE;
    return (uint32_t)(mb < TRL_CAP_MIN_MB ? TRL_CAP_MIN_MB : mb > TRL_CAP_MAX_MB ? TRL_CAP_MAX_MB : mb);
}

static inline uint32_t trl_hold(uint32_t hold_s, int cls) { return cls == TRL_GPU ? hold_s * 2 : hold_s; }

static inline int trl_aged(uint32_t hold_s, int cls, uint32_t unused_s) {
    return hold_s && unused_s >= trl_hold(hold_s, cls);
}

static inline int trl_room(uint64_t used, uint64_t budget, int pressure_pct, double commit_free_mb,
                           double commit_low_mb) {
    if (!budget || pressure_pct <= TRL_ROOM_MARGIN || commit_free_mb < 0) return 0;
    return used <= budget / 100 * (uint64_t)(pressure_pct - TRL_ROOM_MARGIN) && commit_free_mb >= 2 * commit_low_mb;
}

static inline uint64_t trl_keep(uint32_t hold_s, uint64_t cap, int room) {
    if (cap == UINT64_MAX || !hold_s || !room) return cap;
    return cap > UINT64_MAX / TRL_ROOM_TIMES ? UINT64_MAX : cap * TRL_ROOM_TIMES;
}

enum { TRL_HC_OVER, TRL_HC_INTACT_OLD, TRL_HC_INTACT, TRL_HC_GPU, TRL_HC_LATELY, TRL_HC_N };

static inline int trl_hard_class(int overwritten, int sampled_lately, int intact, int past_gap) {
    if (overwritten) return TRL_HC_OVER;
    if (sampled_lately) return TRL_HC_LATELY;
    if (!intact) return TRL_HC_GPU;
    return past_gap ? TRL_HC_INTACT_OLD : TRL_HC_INTACT;
}

static inline int trl_hard_pick(const uint8_t *cls, int n, int need, int *out) {
    int k = 0;
    for (int c = 0; c < TRL_HC_LATELY && k < need; c++)
        for (int i = 0; i < n && k < need; i++)
            if (cls[i] == c) out[k++] = i;
    return k;
}

#define TRL_CLOCK_N 2048
typedef struct {
    uint64_t frame[TRL_CLOCK_N];
    uint32_t sec[TRL_CLOCK_N];
    uint32_t n;
} TrlClock;

static inline void trl_clock_tick(TrlClock *c, uint64_t frame, uint32_t sec) {
    c->frame[c->n % TRL_CLOCK_N] = frame, c->sec[c->n % TRL_CLOCK_N] = sec;
    c->n++;
}

static inline uint32_t trl_clock_age(const TrlClock *c, uint64_t frame, uint32_t now_sec) {
    uint32_t kept = c->n < TRL_CLOCK_N ? c->n : TRL_CLOCK_N, first = c->n - kept;
    if (!kept || c->frame[(c->n - 1) % TRL_CLOCK_N] < frame) return 0;
    uint32_t lo = first, hi = c->n - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (c->frame[mid % TRL_CLOCK_N] >= frame) hi = mid;
        else lo = mid + 1;
    }
    uint32_t s = c->sec[lo % TRL_CLOCK_N];
    return now_sec > s ? now_sec - s : 0;
}

#define TRL_RATE_60 256u
#define TRL_RATE_MIN 64u
#define TRL_RATE_MAX 2048u
static inline uint32_t trl_rate_sample(uint64_t presents, uint64_t ns, int low) {
    if (!ns) return TRL_RATE_60;
    uint64_t r = presents * 256ull * 1000000000ull / (ns * 60);
    uint32_t least = low ? TRL_RATE_MIN : TRL_RATE_60;
    return r < least ? least : r > TRL_RATE_MAX ? TRL_RATE_MAX : (uint32_t)r;
}

static inline uint32_t trl_rate(const uint32_t *ring, int n) {
    uint32_t m = TRL_RATE_MIN;
    for (int i = 0; i < n; i++) {
        uint32_t v = ring[i] ? ring[i] : TRL_RATE_60;
        if (v > m) m = v;
    }
    return m;
}

static inline uint64_t trl_presents(uint64_t n, uint32_t rate) { return n * rate >> 8; }
