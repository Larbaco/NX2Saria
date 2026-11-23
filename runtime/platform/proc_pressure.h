#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline int psi_line(const char *text, const char *kind, double *avg10, uint64_t *total_us) {
    size_t k = strlen(kind);
    for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (strncmp(line, kind, k) || line[k] != ' ') continue;
        const char *end = strchr(line, '\n');
        const char *a = strstr(line, "avg10="), *t = strstr(line, "total=");
        if (!a || !t || (end && (a > end || t > end))) return 0;
        a += 6, t += 6;
        uint64_t whole = 0, frac = 0, scale = 1;
        int digits = 0;
        for (; *a >= '0' && *a <= '9'; a++) whole = whole * 10 + (uint64_t)(*a - '0'), digits++;
        if (*a == '.')
            for (a++; *a >= '0' && *a <= '9'; a++)
                if (scale < 1000000) frac = frac * 10 + (uint64_t)(*a - '0'), scale *= 10, digits++;
        uint64_t us = 0;
        int tdigits = 0;
        for (; *t >= '0' && *t <= '9'; t++) us = us * 10 + (uint64_t)(*t - '0'), tdigits++;
        if (!digits || !tdigits) return 0;
        *avg10 = (double)whole + (double)frac / (double)scale;
        *total_us = us;
        return 1;
    }
    return 0;
}

static inline int meminfo_kb(const char *text, const char *key, uint64_t *kb) {
    size_t k = strlen(key);
    for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (strncmp(line, key, k)) continue;
        const char *p = line + k;
        while (*p == ' ' || *p == '\t') p++;
        if (*p < '0' || *p > '9') return 0;
        uint64_t v = 0;
        for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint64_t)(*p - '0');
        *kb = v;
        return 1;
    }
    return 0;
}
