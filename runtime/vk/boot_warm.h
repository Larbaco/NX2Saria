#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_WARM_TRIES 3

enum { BOOT_WARM_NO, BOOT_WARM_YES, BOOT_WARM_GAVE_UP, BOOT_WARM_DONE };

static inline int boot_warm_decide(const char *text, const char *key, int foreign, int *tries) {
    char was[64] = "", tok[16] = "", *end = tok;
    long n = -1;
    *tries = 0;
    if (text && sscanf(text, "%63s %15s", was, tok) == 2 && !strcmp(was, key)) {
        if (!strcmp(tok, "done")) return BOOT_WARM_DONE;
        n = strtol(tok, &end, 10);
        if (end == tok || *end || n < 0) n = -1;
    }
    if (n < 0 && !foreign) return BOOT_WARM_NO;
    if (n < 0) n = 0;
    if (n >= BOOT_WARM_TRIES) return BOOT_WARM_GAVE_UP;
    *tries = (int)n + 1;
    return BOOT_WARM_YES;
}

#define BOOT_QUIET_ENDS 3

enum { BOOT_END_OTHER, BOOT_END_SKIP, BOOT_END_COUNTED };

static inline int boot_end_decide(const char *text, const char *key, int crashed, int *quiet) {
    char was[64] = "";
    int n = 0;
    if (sscanf(text, "%63s %d", was, &n) < 2 || n < 0) n = 0;
    *quiet = 0;
    if (strcmp(was, key)) return BOOT_END_OTHER;
    *quiet = n + 1;
    return crashed || n + 1 >= BOOT_QUIET_ENDS ? BOOT_END_SKIP : BOOT_END_COUNTED;
}
