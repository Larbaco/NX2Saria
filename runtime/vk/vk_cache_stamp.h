#pragma once

#include <stdint.h>
#include <string.h>

enum {
    CACHE_STAMP_NONE = 0,
    CACHE_STAMP_MATCHES = 1,
    CACHE_STAMP_DIFFERS = 2,
};

enum {
    CACHE_STAMP_READ = 0,
    CACHE_STAMP_READ_STAMP = 1,
    CACHE_STAMP_CARRY = 2,
    CACHE_STAMP_DROP = 3,
};

static inline int cache_stamp_hexval(char c, int *v) {
    if (c >= '0' && c <= '9') return *v = c - '0', 1;
    if (c >= 'a' && c <= 'f') return *v = c - 'a' + 10, 1;
    if (c >= 'A' && c <= 'F') return *v = c - 'A' + 10, 1;
    return 0;
}

static inline int cache_stamp_parse(const char *text, size_t n, uint8_t id[32]) {
    if (!text || n < 64) return 0;
    for (int i = 0; i < 32; i++) {
        int hi, lo;
        if (!cache_stamp_hexval(text[2 * i], &hi) || !cache_stamp_hexval(text[2 * i + 1], &lo)) return 0;
        id[i] = (uint8_t)(hi << 4 | lo);
    }
    for (size_t k = 64; k < n; k++)
        if (text[k] != '\n' && text[k] != '\r' && text[k] != ' ' && text[k] != '\t') return 0;
    return 1;
}

static inline void cache_stamp_format(const uint8_t id[32], char out[66]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[2 * i] = hex[id[i] >> 4], out[2 * i + 1] = hex[id[i] & 15];
    out[64] = '\n', out[65] = 0;
}

static inline int cache_stamp_state(const char *text, size_t n, const uint8_t cur[32], uint8_t stamped[32]) {
    if (!cache_stamp_parse(text, n, stamped)) return CACHE_STAMP_NONE;
    return memcmp(stamped, cur, 32) ? CACHE_STAMP_DIFFERS : CACHE_STAMP_MATCHES;
}

static inline int cache_stamp_action(int state, int carried, int prep) {
    switch (state) {
    case CACHE_STAMP_MATCHES: return CACHE_STAMP_READ;
    case CACHE_STAMP_DIFFERS: return carried && !prep ? CACHE_STAMP_CARRY : CACHE_STAMP_DROP;
    default: return CACHE_STAMP_READ_STAMP;
    }
}

static inline int cache_base_at_exit(int prep, int64_t base_size) { return !prep && base_size <= 0; }

static inline int cache_save_writes_base(int play, int fold, int base_at_exit, int exiting) {
    return !play || fold || (base_at_exit && exiting);
}

static inline void cache_stamp_sibling(const char *path, const char *suffix, char *out, size_t cap) {
    if (!cap) return;
    size_t n = strlen(path), k = strlen(suffix);
    if (n > 4 && !strcmp(path + n - 4, ".bin")) n -= 4;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, path, n);
    if (k > cap - 1 - n) k = cap - 1 - n;
    memcpy(out + n, suffix, k);
    out[n + k] = 0;
}

static inline const char *cache_stamp_state_name(int state) {
    return state == CACHE_STAMP_MATCHES ? "matches" : state == CACHE_STAMP_DIFFERS ? "differs" : "none";
}

static inline const char *cache_stamp_action_name(int action) {
    switch (action) {
    case CACHE_STAMP_READ: return "read";
    case CACHE_STAMP_READ_STAMP: return "read, stamped for the next start";
    case CACHE_STAMP_CARRY: return "read for the carried shaders, stamp kept";
    default: return "deleted, starting empty";
    }
}
