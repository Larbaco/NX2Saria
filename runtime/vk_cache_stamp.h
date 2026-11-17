// The translator stamp of the pipeline cache files (vk_pipeline.c cache_stamp_check): what a start does with
// pipeline-cache.bin and pipeline-cache-play.bin given the translator that wrote them. Own header so
// tests/cache_stamp_check.c checks it without Vulkan or files.
//
// A pipeline is made from the SPIR-V of a translation, and every translator release makes other SPIR-V, so the
// entries a cache holds from an earlier translator are never asked for again once nothing serves that translator's
// translations any more. The files only grow (vk_pipeline.c cache_save_one, and the fold in cache_load): without a
// stamp a 650 MB file read and held twice at every boot is mostly dead entries. pipeline-cache.id names the translator
// whose entries the files hold (its base identity, gfx_shader.c xlate_identity, as in the shader cache's
// identities.txt; the host options mixed into the shader keys are not part of it, so a change of those alone keeps the
// files):
// - the same translator: the files are read;
// - another translator whose translations the shader cache still serves (gfx_shader.c "Carry": a shader the current
//   translator has not translated yet uses the previous one's for the whole session): the files are read, since the
//   pipelines of those carried shaders are in them, and the stamp keeps naming the earlier translator, so the files are
//   dropped at the first start after the carry pass has removed its translations;
// - another translator, not carried, or any other translator in a preparation (--prepare-shaders and the like, which
//   translate everything under the current translator and carry nothing): the files are deleted before they are read
//   and the stamp names the current one;
// - no stamp (files from before the stamp, or none at all): the files are read and the stamp names the current one.
// The cost: what the runs under a carry compiled for the current translator went into the same files, and goes with
// them at the deletion, to be compiled once more. The carry pass usually ends in the first session after an update,
// so that is what that one session compiled. A deletion is not a driver change: the start that follows builds in the
// background as any other (vk_pipeline.c bf_decide), as it did when the files were read and none of their pipelines
// matched the new translator's SPIR-V.
#pragma once

#include <stdint.h>
#include <string.h>

enum {
    CACHE_STAMP_NONE = 0,     // no stamp file, or one that names no identity
    CACHE_STAMP_MATCHES = 1,  // the stamp names the current translator
    CACHE_STAMP_DIFFERS = 2,  // it names another one
};

enum {
    CACHE_STAMP_READ = 0,       // read the files; the stamp stays as it is
    CACHE_STAMP_READ_STAMP = 1, // read the files, then write the current translator into the stamp
    CACHE_STAMP_CARRY = 2,      // read the files for the carried shaders; the stamp keeps the earlier translator
    CACHE_STAMP_DROP = 3,       // delete the files before reading them, then write the current translator into the stamp
};

// 1 when c is a hex digit; its value in *v.
static inline int cache_stamp_hexval(char c, int *v) {
    if (c >= '0' && c <= '9') return *v = c - '0', 1;
    if (c >= 'a' && c <= 'f') return *v = c - 'a' + 10, 1;
    if (c >= 'A' && c <= 'F') return *v = c - 'A' + 10, 1;
    return 0;
}

// The identity a stamp file's text names (n bytes, not terminated): 64 hex digits at its start, then nothing but
// whitespace. 1 and the 32 bytes in id when it names one; 0 for any other text (a cut write, an empty file).
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

// The text a stamp file holds for id: 64 lowercase hex digits and a line end, 65 bytes, terminated (out[66]).
static inline void cache_stamp_format(const uint8_t id[32], char out[66]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[2 * i] = hex[id[i] >> 4], out[2 * i + 1] = hex[id[i] & 15];
    out[64] = '\n', out[65] = 0;
}

// The stamp file's state against the current identity cur. text NULL: no file.
static inline int cache_stamp_state(const char *text, size_t n, const uint8_t cur[32], uint8_t stamped[32]) {
    if (!cache_stamp_parse(text, n, stamped)) return CACHE_STAMP_NONE;
    return memcmp(stamped, cur, 32) ? CACHE_STAMP_DIFFERS : CACHE_STAMP_MATCHES;
}

// What the start does. carried: the stamped translator's translations are still served (the shader cache's carry
// is on and lists it among the previous translators). prep: the start is a preparation, which carries nothing.
static inline int cache_stamp_action(int state, int carried, int prep) {
    switch (state) {
    case CACHE_STAMP_MATCHES: return CACHE_STAMP_READ;
    case CACHE_STAMP_DIFFERS: return carried && !prep ? CACHE_STAMP_CARRY : CACHE_STAMP_DROP;
    default: return CACHE_STAMP_READ_STAMP;
    }
}

// path with its ".bin" replaced by suffix into out (cap bytes): pipeline-cache.bin gives pipeline-cache.id and
// pipeline-cache-play.bin. A path without ".bin" at its end gets suffix after the whole of it. Cut to cap.
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

// The words the log line gives a state and an action.
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
