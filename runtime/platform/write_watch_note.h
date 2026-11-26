#pragma once
#include <stddef.h>
#include <stdio.h>

enum { WW_ON, WW_OLD_KERNEL, WW_REFUSED };
enum { WW_MIN_MAJOR = 6, WW_MIN_MINOR = 7 };

static inline int ww_read_number(const char **p) {
    int v = 0, n = 0;
    while (**p >= '0' && **p <= '9' && n < 6) v = v * 10 + (*(*p)++ - '0'), n++;
    return n ? v : -1;
}

static inline int ww_kernel_old(const char *release) {
    const char *p = release ? release : "";
    int major = ww_read_number(&p), minor = 0;
    if (major < 0) return -1;
    if (*p == '.') {
        p++;
        minor = ww_read_number(&p);
        if (minor < 0) minor = 0;
    }
    return major < WW_MIN_MAJOR || (major == WW_MIN_MAJOR && minor < WW_MIN_MINOR);
}

static inline int ww_verdict(const char *release, int probe_err) {
    if (!probe_err) return WW_ON;
    return ww_kernel_old(release) == 1 ? WW_OLD_KERNEL : WW_REFUSED;
}

static inline void ww_note(int verdict, const char *release, const char *err, char *out, size_t cap) {
    const char *k = release && *release ? release : "(version unknown)";
    if (verdict == WW_ON)
        snprintf(out, cap, "mem: write watch on: asynchronous userfaultfd write protection tracks written guest memory "
                 "(kernel %s)", k);
    else if (verdict == WW_OLD_KERNEL)
        snprintf(out, cap, "mem: write watch off: kernel %s is older than %d.%d and has no asynchronous userfaultfd write "
                 "protection: written memory is not tracked, the game uploads all of it every frame and runs at about "
                 "1 fps; update to kernel %d.%d or later (%s)", k, WW_MIN_MAJOR, WW_MIN_MINOR, WW_MIN_MAJOR,
                 WW_MIN_MINOR, err && *err ? err : "no error text");
    else
        snprintf(out, cap, "mem: write watch off: kernel %s refused asynchronous userfaultfd write protection (%s): written "
                 "memory is not tracked, the game uploads all of it every frame and runs at about 1 fps; it needs a "
                 "kernel %d.%d or later built with userfaultfd, and no sandbox that blocks the userfaultfd call", k,
                 err && *err ? err : "no error text", WW_MIN_MAJOR, WW_MIN_MINOR);
}
