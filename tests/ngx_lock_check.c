#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ngx_lock.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static pthread_mutex_t real = PTHREAD_MUTEX_INITIALIZER;
void plat_mutex_lock(PlatMutex *m) { (void)m, pthread_mutex_lock(&real); }
int plat_mutex_trylock(PlatMutex *m) { return (void)m, pthread_mutex_trylock(&real) == 0; }
void plat_mutex_unlock(PlatMutex *m) { (void)m, pthread_mutex_unlock(&real); }
uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int inside, overlaps, entries;

static void spin_ns(uint64_t ns) {
    const uint64_t t0 = plat_now_ns();
    while (plat_now_ns() - t0 < ns) {}
}

static int fake_ngx(uint64_t ns) {
    if (__atomic_add_fetch(&inside, 1, __ATOMIC_ACQ_REL) > 1) __atomic_add_fetch(&overlaps, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&entries, 1, __ATOMIC_RELAXED);
    spin_ns(ns);
    __atomic_sub_fetch(&inside, 1, __ATOMIC_ACQ_REL);
    return 1;
}

static NgxLock L;
static int locked;
static uint64_t replay_ns = 200000, create_ns = 30000000, present_ns = 200000;
#define ROUNDS 150

static int call(int at, uint64_t ns) { return locked ? NGX_CALL(&L, at, fake_ngx(ns)) : fake_ngx(ns); }

static void *replay(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        call(NGX_AT_REPLAY, i == ROUNDS / 2 ? create_ns : replay_ns);
        spin_ns(50000);
    }
    return NULL;
}

static void *present(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        call(NGX_AT_PRESENT, present_ns);
        spin_ns(50000);
    }
    return NULL;
}

static void *frame(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS / 10; i++) {
        call(NGX_AT_FRAME, 20000);
        spin_ns(400000);
    }
    return NULL;
}

static void run(int with_lock) {
    memset(&L, 0, sizeof L);
    locked = with_lock, overlaps = entries = 0;
    pthread_t t[3];
    pthread_create(&t[0], NULL, replay, NULL);
    pthread_create(&t[1], NULL, present, NULL);
    pthread_create(&t[2], NULL, frame, NULL);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
}

int main(void) {
    run(0);
    printf("ngx_lock_check: without the lock, %d of %d calls entered the fake NGX while another thread was in it\n",
           overlaps, entries);
    CHECK(overlaps > 0, "the fake NGX must see the threads overlap without the lock (the detector works)");
    run(1);
    printf("ngx_lock_check: with the lock, %d of %d calls overlapped\n", overlaps, entries);
    CHECK(!overlaps, "two threads were inside NGX at once through the lock");
    const int expect = ROUNDS * 2 + ROUNDS / 10;
    CHECK(entries == expect && L.calls[NGX_AT_REPLAY] == ROUNDS && L.calls[NGX_AT_PRESENT] == ROUNDS &&
              L.calls[NGX_AT_FRAME] == ROUNDS / 10,
          "calls counted: %d entries, replay %llu present %llu frame %llu", entries,
          (unsigned long long)L.calls[NGX_AT_REPLAY], (unsigned long long)L.calls[NGX_AT_PRESENT],
          (unsigned long long)L.calls[NGX_AT_FRAME]);
    CHECK(L.hold_max >= create_ns && L.hold_max_at == NGX_AT_REPLAY, "the longest hold is the replay's create (%.2f ms at %s)",
          L.hold_max / 1e6, ngx_at_name(L.hold_max_at));
    CHECK(L.wait_max[NGX_AT_PRESENT] <= L.hold_max + 5000000ull,
          "the present thread waited %.2f ms, longer than any hold (%.2f ms)", L.wait_max[NGX_AT_PRESENT] / 1e6,
          L.hold_max / 1e6);
    char line[512];
    ngx_lock_line(&L, line, sizeof line);
    printf("ngx_lock_check: %s\n", line);
    CHECK(strstr(line, "longest hold") && strstr(line, "(replay)"), "the log line names the longest hold");
    printf("ngx_lock_check: worst present wait %.2f ms with a %.0f ms create on the replay thread\n",
           L.wait_max[NGX_AT_PRESENT] / 1e6, create_ns / 1e6);

    memset(&L, 0, sizeof L);
    enum { N = 1000000 };
    const uint64_t t0 = plat_now_ns();
    int sum = 0;
    for (int i = 0; i < N; i++) sum += NGX_CALL(&L, NGX_AT_REPLAY, i & 1);
    const double per = (double)(plat_now_ns() - t0) / N;
    printf("ngx_lock_check: an uncontended call costs %.0f ns for the lock and its two clock reads (%d)\n", per, sum);
    CHECK(!L.waits[NGX_AT_REPLAY] && L.calls[NGX_AT_REPLAY] == N, "one thread alone never waits");
    CHECK(per < 2000, "an uncontended call costs %.0f ns", per);
    printf(fails ? "ngx_lock_check: %d FAILED\n" : "ngx_lock_check: OK\n", fails);
    return fails != 0;
}
