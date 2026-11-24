#include "lib_origin.h"

#include <stdio.h>
#include <string.h>

static int dir_len(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *back = strrchr(path, '\\');
    if (!slash || (back && back > slash)) slash = back;
#endif
    return slash ? (int)(slash - path) : -1;
}

static int same_dir(const char *a, const char *b) {
    int n = dir_len(a);
    if (n < 0 || n != dir_len(b)) return 0;
#ifdef _WIN32
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y && !((x == '/' || x == '\\') && (y == '/' || y == '\\'))) return 0;
    }
    return 1;
#else
    return !memcmp(a, b, (size_t)n);
#endif
}

int lib_origin_line(char *out, size_t cap, int loaded, int built, const char *sdl_path, const char *exe_path,
                    int sha_ni) {
    const char *sha = sha_ni ? "SHA-256 on the SHA extensions (SHA-NI)"
                             : "SHA-256 in portable C (no SHA extensions on this CPU)";
    const char *where = !sdl_path || !*sdl_path        ? NULL
                      : !exe_path || !*exe_path         ? "the game's folder unknown"
                      : same_dir(sdl_path, exe_path)    ? "beside the game"
                                                        : "not beside the game";
    int n = snprintf(out, cap, "%s; SDL %d.%d.%d (built against %d.%d.%d), ", sha, loaded / 1000000,
                     loaded / 1000 % 1000, loaded % 1000, built / 1000000, built / 1000 % 1000, built % 1000);
    size_t used = n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap ? cap - 1 : 0;
    int m = where ? snprintf(out + used, cap - used, "%s: %s", where, sdl_path)
                  : snprintf(out + used, cap - used, "its file not found among the loaded modules");
    return n < 0 || m < 0 ? -1 : n + m;
}
