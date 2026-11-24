#pragma once

#include <stdio.h>

#include "platform/platform.h"

#define PART_FILE_SUFFIX ".part"

static inline int part_file_name(const char *path, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s" PART_FILE_SUFFIX, path);
    return n > 0 && (size_t)n < cap;
}

static inline FILE *part_file_open(const char *path, const char *mode) {
    char part[PLAT_PATH_MAX + 64];
    return part_file_name(path, part, sizeof part) ? fopen(part, mode) : NULL;
}

static inline int part_file_close(FILE *f, const char *path, int ok) {
    char part[PLAT_PATH_MAX + 64];
    if (!f) return 0;
    ok = ok && !ferror(f);
    ok = !fclose(f) && ok;
    if (!part_file_name(path, part, sizeof part)) return 0;
    if (ok && plat_file_replace(part, path, 0)) return 1;
    plat_file_delete(part);
    return 0;
}
