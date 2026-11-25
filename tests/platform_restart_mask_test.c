#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "platform/platform.h"

#define OUT "RESTART_MASK_TEST_OUT"

static int allowed(void) {
    cpu_set_t set;
    return sched_getaffinity(0, sizeof set, &set) ? -1 : CPU_COUNT(&set);
}

static int restarted_count(const char *path) {
    for (int i = 0; i < 500; i++) {
        FILE *f = fopen(path, "r");
        int n = -1;
        if (f) {
            int got = fscanf(f, "%d", &n);
            fclose(f);
            if (got == 1) {
                remove(path);
                return n;
            }
        }
        usleep(10000);
    }
    return -1;
}

static atomic_int go, done;

static void *other_thread(void *arg) {
    (void)arg;
    while (!atomic_load(&go)) usleep(1000);
    plat_restart();
    atomic_store(&done, 1);
    return NULL;
}

int main(void) {
    const char *out = getenv(OUT);
    if (getenv("NX2SARIA_RESTART_WAIT") && out) {
        FILE *f = fopen(out, "w");
        if (f) fprintf(f, "%d\n", allowed()), fclose(f);
        return 0;
    }
    char path[64];
    snprintf(path, sizeof path, "/tmp/restart-mask-%d.txt", (int)getpid());
    setenv(OUT, path, 1);
    remove(path);
    int before = allowed(), fails = 0;
    pthread_t th;
    pthread_create(&th, NULL, other_thread, NULL);
    char which[128];
    int core = plat_cpu_reserve(which, sizeof which);
    if (!core) { puts("SKIP: no core to reserve on this machine"); return 77; }
    atomic_store(&go, 1);
    while (!atomic_load(&done)) usleep(1000);
    pthread_join(th, NULL);
    int from_other = restarted_count(path);
    plat_restart();
    int from_holder = restarted_count(path);
    printf("%d processors before; the core taken: %s; a restart from another thread starts with %d, from the holder with %d\n",
           before, which, from_other, from_holder);
    if (from_other != before) puts("FAIL: the restarted process lacks processors (asked by another thread)"), fails++;
    if (from_holder != before) puts("FAIL: the restarted process lacks processors (asked by the holder)"), fails++;
    if (!fails) puts("restart mask: the restarted process has every processor");
    return fails != 0;
}
