#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_tex_release.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MB(n) ((uint64_t)(n) << 20)

static uint64_t rng_state = 0x243f6a8885a308d3ull;
static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state << 13, rng_state ^= rng_state >> 7, rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 33) % n;
}

static TrlClock clk;

static void clock_checks(void) {
    memset(&clk, 0, sizeof clk);
    uint64_t frame = 0, at[4000];
    for (uint32_t s = 0; s < 4000; s++) {
        at[s] = frame;
        trl_clock_tick(&clk, frame, s);
        frame += 60 + (s / 7 % 4) * 60;
    }
    uint32_t now = 3999;
    for (uint32_t s = 0; s < 4000; s += 13) {
        uint32_t age = trl_clock_age(&clk, at[s] + 1, now), real = now - s;
        if (now - s < TRL_CLOCK_N - 1) {
            CHECK(age <= real && age + 1 >= real, "present after second %u: %u s old, %u by the clock", s, real, age);
        } else {
            CHECK(age >= TRL_CLOCK_N - 2, "present after second %u, before the ticks kept: %u by the clock", s, age);
        }
    }
    CHECK(trl_clock_age(&clk, frame + 5, now) == 0, "a present after the last tick is 0 s old");
    CHECK(trl_clock_age(&clk, at[3999], now) == 0, "the last tick's present is 0 s old");
    uint32_t s60 = 2500, s240 = 0;
    for (uint32_t s = 2500; s < 4000; s++)
        if ((s / 7 % 4) == 3) { s240 = s; break; }
    CHECK(trl_clock_age(&clk, at[s60], s60 + 180) == 180 && trl_clock_age(&clk, at[s240], s240 + 180) == 180,
          "180 s are 180 s at any rate");
    TrlClock c2 = {0};
    CHECK(trl_clock_age(&c2, 100, 50) == 0, "no tick yet: 0 s");
    trl_clock_tick(&c2, 0, 0), trl_clock_tick(&c2, 100, 1), trl_clock_tick(&c2, 250, 2);
    CHECK(trl_clock_age(&c2, 50, 10) == 9 && trl_clock_age(&c2, 0, 10) == 10 && trl_clock_age(&c2, 251, 10) == 0,
          "short run: %u %u %u", trl_clock_age(&c2, 50, 10), trl_clock_age(&c2, 0, 10), trl_clock_age(&c2, 251, 10));
}

static void rate_checks(void) {
    const uint64_t S = 1000000000ull;
    CHECK(trl_rate_sample(60, S, 1) == 256 && trl_rate_sample(60, S, 0) == 256, "60 presents a second");
    CHECK(trl_rate_sample(240, S, 1) == 1024 && trl_rate_sample(240, S, 0) == 1024, "240 a second");
    CHECK(trl_rate_sample(1000, S, 1) == TRL_RATE_MAX, "1000 a second counts as 480");
    CHECK(trl_rate_sample(30, S, 1) == 128, "30 a second: %u", trl_rate_sample(30, S, 1));
    CHECK(trl_rate_sample(21, S, 1) == 89, "21 a second: %u", trl_rate_sample(21, S, 1));
    CHECK(trl_rate_sample(5, S, 1) == TRL_RATE_MIN && trl_rate_sample(0, S, 1) == TRL_RATE_MIN, "under 15 a second: 15");
    CHECK(trl_rate_sample(30, S, 0) == 256 && trl_rate_sample(0, S, 0) == 256, "the rule before: never under 60");
    CHECK(trl_rate_sample(100, 0, 1) == 256, "no time passed: 60");
    CHECK(trl_rate_sample(63, 3 * S, 1) == 89, "21 a second over three seconds: %u", trl_rate_sample(63, 3 * S, 1));
    enum { N = 8 };
    uint32_t ring[N] = {0};
    CHECK(trl_rate(ring, N) == 256, "no sample yet: %u", trl_rate(ring, N));
    for (int i = 0; i < N - 1; i++) ring[i] = trl_rate_sample(21, S, 1);
    CHECK(trl_rate(ring, N) == 256, "seven slow seconds and one not sampled: %u", trl_rate(ring, N));
    ring[N - 1] = trl_rate_sample(21, S, 1);
    CHECK(trl_rate(ring, N) == 89, "eight slow seconds: %u", trl_rate(ring, N));
    ring[3] = trl_rate_sample(60, S, 1);
    CHECK(trl_rate(ring, N) == 256, "one second at 60 among them: %u", trl_rate(ring, N));
    ring[3] = trl_rate_sample(144, S, 1);
    CHECK(trl_rate(ring, N) == trl_rate_sample(144, S, 1), "one second at 144 among them");
    for (int i = 0; i < N; i++) ring[i] = trl_rate_sample(120, S, 1);
    ring[5] = trl_rate_sample(2, S, 1);
    CHECK(trl_rate(ring, N) == 512, "a slow second among fast ones shortens nothing: %u", trl_rate(ring, N));
    static const uint32_t fps[] = {15, 21, 30, 45, 60, 90, 144, 240};
    printf("the wait after a release (120 presents at 60 a second), by the presents a second:\n");
    printf("  presents/s   before: presents, seconds   now: presents, seconds\n");
    for (unsigned i = 0; i < sizeof fps / sizeof *fps; i++) {
        uint64_t was = trl_presents(120, trl_rate_sample(fps[i], S, 0)), now = trl_presents(120, trl_rate_sample(fps[i], S, 1));
        double was_s = (double)was / fps[i], now_s = (double)now / fps[i];
        printf("  %10u   %16llu  %7.2f   %13llu  %7.2f\n", fps[i], (unsigned long long)was, was_s,
               (unsigned long long)now, now_s);
        CHECK(now_s > 1.9 && now_s <= 2.0, "%u presents a second: %.2f s", fps[i], now_s);
        if (fps[i] >= 60) CHECK(was == now, "%u presents a second: %llu presents, %llu before", fps[i],
                                (unsigned long long)now, (unsigned long long)was);
    }
    CHECK(trl_presents(120, trl_rate_sample(10, S, 1)) == 30, "10 presents a second: 30 presents");
    double before = (double)trl_presents(120, trl_rate_sample(21, S, 0)) / 21;
    CHECK(before > 5.7 && before < 5.72, "the rule before at 21 a second: %.2f s", before);
    CHECK(trl_presents(1800, trl_rate_sample(30, S, 1)) == 900, "30 s at 30 presents a second: 900 presents");
}

static void rule_checks(void) {
    CHECK(trl_hold(TRL_HOLD_S, TRL_STORAGE) == 60 && trl_hold(TRL_HOLD_S, TRL_GPU) == 120, "holds 60 and 120 s");
    CHECK(!trl_aged(0, TRL_STORAGE, 100000), "no hold: nothing ages out");
    CHECK(!trl_aged(180, TRL_STORAGE, 179) && trl_aged(180, TRL_STORAGE, 180), "the hold's edge");
    CHECK(!trl_aged(180, TRL_GPU, 359) && trl_aged(180, TRL_GPU, 360), "GPU content waits twice the hold");
    CHECK(2 * TRL_HOLD_MAX_S < TRL_CLOCK_N - 1, "the clock covers the longest hold of GPU content");
    uint64_t budget = MB(31 * 1024);
    CHECK(trl_room(MB(11786), budget, 80, 70981 - 43683, 7098), "RTX 5090 session: room");
    CHECK(!trl_room(MB(23000), budget, 80, 27000, 7098), "card above 70%% of its budget: no room");
    CHECK(!trl_room(MB(5000), budget, 80, 10000, 7098), "commit under twice its low mark: no room");
    CHECK(!trl_room(MB(5000), budget, 80, -1, 7098), "commit not tracked: no room");
    CHECK(!trl_room(MB(1000), 0, 80, 30000, 4096), "no budget known: no room");
    CHECK(!trl_room(MB(1), budget, 5, 30000, 4096), "pressure under the margin: no room");
    CHECK(!trl_room(MB(6450), MB(7680), 80, 20000, 4096), "8 GiB card in play: no room");
    CHECK(trl_keep(180, MB(4008), 1) == MB(8016) && trl_keep(180, MB(4008), 0) == MB(4008), "keep: twice with room");
    CHECK(trl_keep(0, MB(4008), 1) == MB(4008), "keep: the bound without the hold, as before");
    CHECK(trl_keep(180, UINT64_MAX, 1) == UINT64_MAX, "keep: no bound stays none");
    CHECK(trl_cap_mb(0) == TRL_CAP_UNKNOWN_MB, "no budget known: %u", trl_cap_mb(0));
    CHECK(trl_cap_mb(31419) == 981, "RTX 5090 (budget 31419 MiB): %u", trl_cap_mb(31419));
    CHECK(trl_cap_mb(15450) == 482, "a 16 GiB card (budget 15450 MiB): %u", trl_cap_mb(15450));
    CHECK(trl_cap_mb(7680) == TRL_CAP_MIN_MB && trl_cap_mb(3900) == TRL_CAP_MIN_MB, "8 and 4 GiB cards: the least");
    CHECK(trl_cap_mb(48000) == TRL_CAP_MAX_MB && trl_cap_mb(UINT64_MAX) == TRL_CAP_MAX_MB, "a larger card: the most");
    uint32_t last = 0;
    for (uint64_t b = 1; b <= 65536; b += 97) {
        uint32_t c = trl_cap_mb(b);
        CHECK(c >= last && c >= TRL_CAP_MIN_MB && c <= TRL_CAP_MAX_MB, "budget %llu MiB: bound %u after %u",
              (unsigned long long)b, c, last);
        last = c;
    }
    CHECK(trl_keep(TRL_HOLD_S, MB(trl_cap_mb(31419)), 1) < MB(2048), "the most on a 32 GiB card");
    CHECK(trl_keep(TRL_HOLD_S, MB(trl_cap_mb(7680)), 0) == MB(384), "the most on an 8 GiB card in play");
}

static const struct { const char *session; uint64_t n[5]; } TAKEN[] = {
    {"low, frame 3600", {46626, 18, 0, 0, 0}},     {"low, frame 7200", {115111, 126, 69, 0, 0}},
    {"low, frame 10800", {88575, 79, 156, 0, 0}}, {"balanced, frame 3600", {22034, 1, 0, 0, 0}},
    {"balanced, frame 7200", {120779, 91, 1, 0, 0}},
};
static const uint32_t EDGE[5] = {2, 10, 60, 300, UINT32_MAX};
static const uint64_t FIELD_BACK_MB[3] = {1494, 171, 10};
static const uint64_t FIELD_BACK_N[3] = {941, 331, 28}, FIELD_UPLOADS = 33118;

static void taken_back_checks(void) {
    uint32_t longest = 0;
    for (size_t i = 0; i < sizeof TAKEN / sizeof *TAKEN; i++)
        for (int b = 0; b < 5; b++)
            if (TAKEN[i].n[b] && EDGE[b] > longest) longest = EDGE[b];
    CHECK(longest == 60, "the longest take-back of the logs lies under %u s", longest);
    for (int cls = TRL_STORAGE; cls <= TRL_GPU; cls++)
        CHECK(!trl_aged(TRL_HOLD_S, cls, longest - 1) && trl_hold(TRL_HOLD_S, cls) >= longest,
              "class %d: hold %u s against take-backs under %u s", cls, trl_hold(TRL_HOLD_S, cls), longest);
    uint64_t all_mb = FIELD_BACK_MB[0] + FIELD_BACK_MB[1] + FIELD_BACK_MB[2];
    uint64_t all_n = FIELD_BACK_N[0] + FIELD_BACK_N[1] + FIELD_BACK_N[2];
    CHECK(FIELD_BACK_MB[2] * 100 < all_mb, "field run: %llu of %llu MiB came back past a minute",
          (unsigned long long)FIELD_BACK_MB[2], (unsigned long long)all_mb);
    CHECK(all_n * 20 < FIELD_UPLOADS, "field run: %llu taken back beside %llu loaded", (unsigned long long)all_n,
          (unsigned long long)FIELD_UPLOADS);
}

typedef struct {
    uint32_t rel;
    uint32_t back;
    uint32_t bytes;
    uint8_t cls, gone, taken;
} Ent;

typedef struct {
    const char *name;
    uint64_t cap;
    uint32_t hold;
} Policy;

typedef struct {
    uint64_t remade, remade_bytes, remade_quick, dropped_age, dropped_bound, peak, end, at_burst_end;
} Result;

#define MAXE 400000
static Ent ents[MAXE];
static uint32_t nents;
static uint32_t backs[MAXE], nbacks;

static int back_cmp(const void *a, const void *b) {
    uint32_t x = ents[*(const uint32_t *)a].back, y = ents[*(const uint32_t *)b].back;
    return x < y ? -1 : x > y;
}

static void make_stream(uint32_t burst_s, double burst_mbs, uint32_t end_s, double calm_mbs, uint32_t back_pml,
                        uint32_t late_s) {
    nents = 0;
    double carry = 0;
    for (uint32_t s = 1; s < end_s; s++) {
        carry += (s <= burst_s ? burst_mbs : calm_mbs) * 1048576.0;
        while (carry > 0 && nents < MAXE) {
            Ent *e = &ents[nents++];
            memset(e, 0, sizeof *e);
            e->rel = s, e->bytes = (uint32_t)(MB(1) / 10 + rnd((uint32_t)MB(18) / 10));
            e->cls = rnd(10) == 0 ? TRL_GPU : TRL_STORAGE;
            if (rnd(1000) < back_pml) e->back = s + (rnd(2) ? 2 + rnd(8) : 10 + rnd(50));
            if (late_s && rnd(1000) < back_pml) e->back = s + late_s;
            carry -= e->bytes;
        }
    }
    nbacks = 0;
    for (uint32_t i = 0; i < nents; i++)
        if (ents[i].back) backs[nbacks++] = i;
    qsort(backs, nbacks, sizeof *backs, back_cmp);
}

static Result run(const Policy *p, int room, uint32_t burst_s, uint32_t end_s) {
    Result r = {0};
    for (uint32_t i = 0; i < nents; i++) ents[i].gone = ents[i].taken = 0;
    uint64_t held = 0;
    uint32_t next = 0, head = 0;
    uint32_t nb = 0;
    for (uint32_t s = 1; s < end_s; s++) {
        while (next < nents && ents[next].rel <= s) held += ents[next++].bytes;
        for (; nb < nbacks && ents[backs[nb]].back <= s; nb++) {
            Ent *e = &ents[backs[nb]];
            if (e->gone) r.remade++, r.remade_bytes += e->bytes, r.remade_quick += e->back - e->rel < 10;
            else held -= e->bytes;
            e->taken = 1;
        }
        for (uint32_t i = head; p->hold && i < next; i++) {
            Ent *e = &ents[i];
            if (e->gone || e->taken) continue;
            if (s - e->rel < p->hold) break;
            if (trl_aged(p->hold, e->cls, s - e->rel)) e->gone = 1, held -= e->bytes, r.dropped_age++;
        }
        uint64_t keep = trl_keep(p->hold, p->cap, room);
        for (uint32_t i = head; i < next && held > keep; i++) {
            Ent *e = &ents[i];
            if (e->gone || e->taken || s - e->rel < 2) continue;
            e->gone = 1, held -= e->bytes, r.dropped_bound++;
        }
        while (head < next && (ents[head].gone || ents[head].taken)) head++;
        if (held > r.peak) r.peak = held;
        if (s == burst_s) r.at_burst_end = held;
    }
    r.end = held;
    return r;
}

static void print_result(const char *scenario, const Policy *p, int room, const Result *r) {
    printf("  %-34s %-12s room %d: made again %5llu (%6.0f MiB), dropped by age %6llu, by the bound %6llu; held at "
           "the burst's end %5.0f MiB, peak %5.0f MiB, at the end %5.0f MiB\n",
           scenario, p->name, room, (unsigned long long)r->remade, r->remade_bytes / 1048576.0,
           (unsigned long long)r->dropped_age, (unsigned long long)r->dropped_bound, r->at_burst_end / 1048576.0,
           r->peak / 1048576.0, r->end / 1048576.0);
}

static void session_checks(void) {
    const Policy was = {"until 0.11.0", MB(4008), 180}, now = {"0.11.0", MB(trl_cap_mb(31419)), TRL_HOLD_S};
    const Policy was8 = {"until 0.11.0", MB(1920), 180}, now8 = {"0.11.0", MB(trl_cap_mb(7680)), TRL_HOLD_S};
    struct { const char *name; uint32_t burst_s; double burst_mbs; uint32_t end_s; double calm_mbs; } sc[] = {
        {"area changes 70 s, calm 10 min", 70, 93.0, 670, 1.0},
        {"steady 20 MiB/s for 20 min", 1200, 20.0, 1500, 0.5},
        {"teleports 4 min, calm 10 min", 240, 93.0, 840, 1.0},
        {"field run 3 min, calm 2 min", 170, 130.0, 290, 1.0},
    };
    printf("simulated sessions (made again: taken back after its drop; MiB: own memory of released textures)\n");
    for (size_t i = 0; i < sizeof sc / sizeof *sc; i++) {
        rng_state = 0x9e3779b97f4a7c15ull + i;
        make_stream(sc[i].burst_s, sc[i].burst_mbs, sc[i].end_s, sc[i].calm_mbs, 20, 0);
        Result rn = run(&now, 1, sc[i].burst_s, sc[i].end_s), rw = run(&was, 1, sc[i].burst_s, sc[i].end_s);
        Result rn8 = run(&now8, 0, sc[i].burst_s, sc[i].end_s), rw8 = run(&was8, 0, sc[i].burst_s, sc[i].end_s);
        print_result(sc[i].name, &was, 1, &rw), print_result("", &now, 1, &rn);
        print_result("  same on an 8 GiB card", &was8, 0, &rw8), print_result("", &now8, 0, &rn8);
        uint64_t total = 0, came_back = 0;
        for (uint32_t k = 0; k < nents; k++) total += ents[k].bytes, came_back += ents[k].back ? ents[k].bytes : 0;
        uint64_t slack = MB((uint64_t)(2 * sc[i].burst_mbs) + 4);
        CHECK(rn.peak <= trl_keep(now.hold, now.cap, 1) + slack, "%s: holds %.0f MiB at its fullest", sc[i].name,
              rn.peak / 1048576.0);
        CHECK(rn8.peak <= trl_keep(now8.hold, now8.cap, 0) + slack, "%s, 8 GiB card: holds %.0f MiB at its fullest",
              sc[i].name, rn8.peak / 1048576.0);
        CHECK(rn.peak <= rw.peak && rn8.peak <= rw8.peak, "%s: peak %.0f/%.0f MiB against %.0f/%.0f before", sc[i].name,
              rn.peak / 1048576.0, rn8.peak / 1048576.0, rw.peak / 1048576.0, rw8.peak / 1048576.0);
        if (rw.peak > MB(4096))
            CHECK(rn.peak * 2 < rw.peak, "%s: peak %.0f MiB, %.0f before", sc[i].name, rn.peak / 1048576.0,
                  rw.peak / 1048576.0);
        CHECK(rn.end <= rw.end && rn.end <= MB((uint64_t)(sc[i].calm_mbs * 2 * TRL_HOLD_S) + 64),
              "%s: holds %.0f MiB at the end, %.0f before", sc[i].name, rn.end / 1048576.0, rw.end / 1048576.0);
        if ((uint64_t)(sc[i].burst_mbs * 12) < (trl_keep(now.hold, now.cap, 1) >> 20))
            CHECK(rn.remade_quick == 0, "%s: %llu quick take-backs made again", sc[i].name,
                  (unsigned long long)rn.remade_quick);
        if ((uint64_t)(sc[i].burst_mbs * 12) < (trl_keep(now8.hold, now8.cap, 0) >> 20))
            CHECK(rn8.remade_quick == 0, "%s, 8 GiB card: %llu quick take-backs made again", sc[i].name,
                  (unsigned long long)rn8.remade_quick);
        CHECK(rn.remade_bytes <= came_back && rn8.remade_bytes <= came_back && came_back * 40 < total,
              "%s: made again %.0f/%.0f MiB of %.0f MiB that came back", sc[i].name, rn.remade_bytes / 1048576.0,
              rn8.remade_bytes / 1048576.0, came_back / 1048576.0);
    }
    rng_state = 7;
    make_stream(0, 0.0, 400, 5.0, 20, 50);
    Result r50 = run(&now, 1, 0, 400);
    CHECK(r50.remade == 0 && r50.dropped_bound == 0, "take-backs at 50 s: made %llu again, %llu dropped by the bound",
          (unsigned long long)r50.remade, (unsigned long long)r50.dropped_bound);
    rng_state = 7;
    make_stream(0, 0.0, 400, 5.0, 20, 90);
    Result r90 = run(&now, 1, 0, 400);
    CHECK(r90.remade > 0, "take-backs at 90 s: the hold should have dropped some first");
    printf("  late take-backs in calm play: at 50 s made again %llu, at 90 s %llu (the hold is %u s)\n",
           (unsigned long long)r50.remade, (unsigned long long)r90.remade, TRL_HOLD_S);
}

static void hard_checks(void) {
    CHECK(trl_hard_class(1, 1, 0, 0) == TRL_HC_OVER, "an overwritten entry goes first, sampled or not");
    CHECK(trl_hard_class(0, 1, 1, 1) == TRL_HC_LATELY, "a sampled entry is kept");
    CHECK(trl_hard_class(0, 0, 0, 0) == TRL_HC_GPU, "GPU-made content");
    CHECK(trl_hard_class(0, 0, 1, 1) == TRL_HC_INTACT_OLD && trl_hard_class(0, 0, 1, 0) == TRL_HC_INTACT, "intact");
    enum { N = 64 };
    uint8_t cls[N];
    int out[N];
    for (int i = 0; i < N; i++)
        cls[i] = i < 16 ? TRL_HC_GPU : i % 9 == 0 ? TRL_HC_OVER : i % 3 == 0 ? TRL_HC_LATELY : i < 40 ? TRL_HC_INTACT_OLD
               : TRL_HC_INTACT;
    int k = trl_hard_pick(cls, N, 20, out);
    CHECK(k == 20, "picked %d of 20", k);
    int last = -1, gpu = 0, prev_i = -1;
    for (int j = 0; j < k; j++) {
        int c = cls[out[j]];
        CHECK(c != TRL_HC_LATELY, "a lately sampled entry was picked (%d)", out[j]);
        CHECK(c >= last, "class order: %d after %d", c, last);
        if (c == last) CHECK(out[j] > prev_i, "release order within a class");
        gpu += c == TRL_HC_GPU;
        last = c, prev_i = out[j];
    }
    CHECK(gpu == 0, "%d render targets went while intact entries were left", gpu);
    for (int i = 0; i < N; i++) cls[i] = i % 2 ? TRL_HC_GPU : TRL_HC_LATELY;
    k = trl_hard_pick(cls, N, 5, out);
    CHECK(k == 5 && out[0] == 1 && out[4] == 9, "render targets in release order: %d, %d..%d", k, out[0], out[4]);
    for (int i = 0; i < N; i++) cls[i] = TRL_HC_LATELY;
    CHECK(trl_hard_pick(cls, N, 5, out) == 0, "all sampled lately: none picked");
}

int main(void) {
    clock_checks();
    rate_checks();
    rule_checks();
    taken_back_checks();
    session_checks();
    hard_checks();
    if (fails) printf("%d checks failed\n", fails);
    else printf("tex_release_check: all checks passed\n");
    return fails ? 1 : 0;
}
