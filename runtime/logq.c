#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "logq.h"
#include "platform/platform.h"

#define LOGQ_CAP (1u << 20)
#define LOGQ_GATHER_NS 250000000ull
#define LOGQ_GATHER_BYTES (LOGQ_CAP / 2)
#define LOGQ_SLOW_NS 2000000ll
#define LOGQ_SAY_NS 10000000000ull

static struct {
    PlatMutex q, io;
    PlatCond more;
    char *buf, *out;
    size_t n;
    LogqSink sink;
    volatile int32_t on, saying, now;
    volatile int64_t slow, said, worst_ns, full;
    volatile uint64_t said_at;
} Q = {.q = PLAT_MUTEX_INIT, .io = PLAT_MUTEX_INIT, .more = PLAT_COND_INIT};

void logq_init(LogqSink sink) { Q.sink = sink; }

size_t logq_bytes(void) { return __atomic_load_n(&Q.buf, __ATOMIC_RELAXED) ? 2 * (size_t)LOGQ_CAP : 0; }

static void put_out(const char *p, size_t n) {
    uint64_t t0 = plat_now_ns();
    Q.sink(p, n);
    int64_t d = (int64_t)(plat_now_ns() - t0);
    if (d < LOGQ_SLOW_NS) return;
    __atomic_add_fetch(&Q.slow, 1, __ATOMIC_RELAXED);
    int64_t w = __atomic_load_n(&Q.worst_ns, __ATOMIC_RELAXED);
    while (d > w && !__atomic_compare_exchange_n(&Q.worst_ns, &w, d, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void say_slow(void) {
    int64_t slow = __atomic_load_n(&Q.slow, __ATOMIC_RELAXED);
    if (slow == __atomic_load_n(&Q.said, __ATOMIC_RELAXED)) return;
    uint64_t now = plat_now_ns(), at = __atomic_load_n(&Q.said_at, __ATOMIC_RELAXED);
    if ((at && now - at < LOGQ_SAY_NS) || plat_xchg32(&Q.saying, 1)) return;
    __atomic_store_n(&Q.said, slow, __ATOMIC_RELAXED);
    __atomic_store_n(&Q.said_at, now ? now : 1, __ATOMIC_RELAXED);
    rt_log("log: %lld writes of the log took over %d ms so far, the longest %.1f ms, %s (NX2SARIA_LOG_ASYNC); %lld lines "
           "found the queue full and waited for its write", (long long)slow, (int)(LOGQ_SLOW_NS / 1000000),
           __atomic_load_n(&Q.worst_ns, __ATOMIC_RELAXED) / 1e6,
           __atomic_load_n(&Q.on, __ATOMIC_ACQUIRE) ? "on the log thread" : "on the threads that logged",
           (long long)__atomic_load_n(&Q.full, __ATOMIC_RELAXED));
    plat_xchg32(&Q.saying, 0);
}

static void drain(void) {
    plat_mutex_lock(&Q.io);
    plat_mutex_lock(&Q.q);
    char *b = Q.buf;
    size_t n = Q.n;
    Q.buf = Q.out, Q.out = b, Q.n = 0;
    plat_mutex_unlock(&Q.q);
    if (n) put_out(b, n);
    plat_mutex_unlock(&Q.io);
}

static int put(const char *p, size_t n) {
    if (!__atomic_load_n(&Q.on, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock(&Q.q);
    int on = __atomic_load_n(&Q.on, __ATOMIC_ACQUIRE);
    if (!on || Q.n + n > LOGQ_CAP) {
        plat_mutex_unlock(&Q.q);
        if (on) {
            __atomic_add_fetch(&Q.full, 1, __ATOMIC_RELAXED);
            drain();
        }
        return 0;
    }
    int wake = !Q.n || (Q.n < LOGQ_GATHER_BYTES && Q.n + n >= LOGQ_GATHER_BYTES);
    memcpy(Q.buf + Q.n, p, n);
    Q.n += n;
    plat_mutex_unlock(&Q.q);
    if (wake) plat_cond_signal(&Q.more);
    return 1;
}

void logq_write(const char *p, size_t n) {
    if (put(p, n)) return;
    put_out(p, n);
    say_slow();
}

static void log_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&Q.q);
        while (!Q.n) plat_cond_wait(&Q.more, &Q.q);
        for (uint64_t t0 = plat_now_ns(), spent = 0; Q.n && Q.n < LOGQ_GATHER_BYTES && spent < LOGQ_GATHER_NS && !Q.now;
             spent = plat_now_ns() - t0)
            plat_cond_wait_ns(&Q.more, &Q.q, LOGQ_GATHER_NS - spent);
        Q.now = 0;
        plat_mutex_unlock(&Q.q);
        drain();
        say_slow();
    }
}

int logq_async(void) {
    if (!Q.sink || __atomic_load_n(&Q.on, __ATOMIC_ACQUIRE)) return 0;
    if (!Q.buf) Q.buf = malloc(LOGQ_CAP), Q.out = malloc(LOGQ_CAP);
    if (!Q.buf || !Q.out || !plat_thread_spawn(log_main, NULL, "log", 0)) return 0;
    __atomic_store_n(&Q.on, 1, __ATOMIC_RELEASE);
    return 1;
}

void logq_flush(void) {
    if (!__atomic_load_n(&Q.on, __ATOMIC_ACQUIRE)) return;
    int io = 0, q = 0;
    for (int i = 0; i < 100 && !(io = plat_mutex_trylock(&Q.io)); i++) plat_sleep_ms(1);
    if (!io) return;
    for (int i = 0; i < 100 && !(q = plat_mutex_trylock(&Q.q)); i++) plat_sleep_ms(1);
    if (!q) {
        plat_mutex_unlock(&Q.io);
        return;
    }
    char *b = Q.buf;
    size_t n = Q.n;
    Q.buf = Q.out, Q.out = b, Q.n = 0;
    plat_mutex_unlock(&Q.q);
    if (n) put_out(b, n);
    plat_mutex_unlock(&Q.io);
}

void logq_nudge(void) {
    if (!__atomic_load_n(&Q.on, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&Q.q);
    Q.now = 1;
    plat_mutex_unlock(&Q.q);
    plat_cond_signal(&Q.more);
}

void logq_peek(const char **p, size_t *n) {
    size_t k = __atomic_load_n(&Q.n, __ATOMIC_RELAXED);
    *p = __atomic_load_n(&Q.buf, __ATOMIC_RELAXED);
    *n = __atomic_load_n(&Q.on, __ATOMIC_RELAXED) && k <= LOGQ_CAP ? k : 0;
}

void logq_sync(void) {
    if (!plat_xchg32(&Q.on, 0)) return;
    int io = 0, q = 0;
    for (int i = 0; i < 500 && !(io = plat_mutex_trylock(&Q.io)); i++) plat_sleep_ms(1);
    for (int i = 0; i < 500 && !(q = plat_mutex_trylock(&Q.q)); i++) plat_sleep_ms(1);
    size_t n = Q.n;
    Q.n = 0;
    if (n && n <= LOGQ_CAP) put_out(Q.buf, n);
    if (q) plat_mutex_unlock(&Q.q);
    if (io) plat_mutex_unlock(&Q.io);
}
