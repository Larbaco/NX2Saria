#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "platform/platform.h"

typedef struct {
    uint32_t *plain;
    uint64_t words;
    void *z;
    uint32_t zsize;
    uint32_t pins;
    uint64_t used_ms;
    uint64_t fnv;
    volatile int32_t fnv_ok;
} SpvStore;

typedef struct {
    ZSTD_CCtx *cc;
    void *out;
    size_t out_cap;
    uint32_t *back;
    size_t back_cap;
} SpvPacker;

static inline uint64_t spv_fnv_words(const uint32_t *w, uint64_t words) {
    const uint8_t *b = (const uint8_t *)w;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < words * 4; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static inline void spv_store_fill(SpvStore *s, uint32_t *plain, uint64_t words, uint64_t now_ms) {
    memset(s, 0, sizeof *s);
    s->plain = plain, s->words = words, s->used_ms = now_ms;
}

static inline void spv_store_free(SpvStore *s) {
    free(s->plain), free(s->z);
    s->plain = NULL, s->z = NULL, s->zsize = 0;
}

static inline const uint32_t *spv_pin(SpvStore *s, PlatMutex *lk, uint64_t *decode_ns) {
    *decode_ns = 0;
    plat_mutex_lock(lk);
    uint32_t *p = s->plain;
    if (p) s->pins++;
    plat_mutex_unlock(lk);
    if (p) return p;
    uint64_t t0 = plat_now_ns();
    size_t bytes = (size_t)s->words * 4;
    uint32_t *buf = malloc(bytes ? bytes : 4);
    if (!buf) return NULL;
    size_t got = ZSTD_decompress(buf, bytes, s->z, s->zsize);
    if (ZSTD_isError(got) || got != bytes) {
        free(buf);
        return NULL;
    }
    plat_mutex_lock(lk);
    if (s->plain) p = s->plain;
    else __atomic_store_n(&s->plain, buf, __ATOMIC_RELAXED), p = buf, buf = NULL;
    s->pins++;
    plat_mutex_unlock(lk);
    free(buf);
    *decode_ns = plat_now_ns() - t0;
    return p;
}

static inline void spv_unpin(SpvStore *s, PlatMutex *lk, uint64_t now_ms) {
    plat_mutex_lock(lk);
    s->pins--;
    if (now_ms > s->used_ms) s->used_ms = now_ms;
    plat_mutex_unlock(lk);
}

static inline uint64_t spv_fnv(SpvStore *s, PlatMutex *lk, uint64_t now_ms, uint64_t *decode_ns) {
    *decode_ns = 0;
    if (__atomic_load_n(&s->fnv_ok, __ATOMIC_ACQUIRE)) return __atomic_load_n(&s->fnv, __ATOMIC_RELAXED);
    const uint32_t *w = spv_pin(s, lk, decode_ns);
    if (!w) return 0;
    uint64_t h = spv_fnv_words(w, s->words);
    spv_unpin(s, lk, now_ms);
    __atomic_store_n(&s->fnv, h, __ATOMIC_RELAXED);
    __atomic_store_n(&s->fnv_ok, 1, __ATOMIC_RELEASE);
    return h;
}

static inline int spv_idle_locked(const SpvStore *s, uint64_t now_ms, uint64_t idle_ms) {
    return !s->pins && now_ms >= s->used_ms && now_ms - s->used_ms >= idle_ms;
}

enum { SPV_PACK_NONE, SPV_PACK_ENCODE, SPV_PACK_DROP };

static inline int spv_pack_begin(SpvStore *s, PlatMutex *lk, uint64_t now_ms, uint64_t idle_ms) {
    plat_mutex_lock(lk);
    int r = SPV_PACK_NONE;
    if (s->plain && spv_idle_locked(s, now_ms, idle_ms)) {
        r = s->z ? SPV_PACK_DROP : SPV_PACK_ENCODE;
        if (r == SPV_PACK_ENCODE) s->pins++;
    }
    plat_mutex_unlock(lk);
    return r;
}

static inline void *spv_pack_encode(SpvStore *s, SpvPacker *pk, int level, uint32_t *zsize) {
    const uint32_t *w = s->plain;
    size_t bytes = (size_t)s->words * 4;
    if (!__atomic_load_n(&s->fnv_ok, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s->fnv, spv_fnv_words(w, s->words), __ATOMIC_RELAXED);
        __atomic_store_n(&s->fnv_ok, 1, __ATOMIC_RELEASE);
    }
    if (!pk->cc && !(pk->cc = ZSTD_createCCtx())) return NULL;
    size_t cap = ZSTD_compressBound(bytes);
    if (pk->out_cap < cap) {
        void *o = realloc(pk->out, cap);
        if (!o) return NULL;
        pk->out = o, pk->out_cap = cap;
    }
    if (pk->back_cap < bytes + 4) {
        void *b = realloc(pk->back, bytes + 4);
        if (!b) return NULL;
        pk->back = b, pk->back_cap = bytes + 4;
    }
    size_t n = ZSTD_compressCCtx(pk->cc, pk->out, cap, w, bytes, level);
    if (ZSTD_isError(n) || n > UINT32_MAX) return NULL;
    size_t got = ZSTD_decompress(pk->back, bytes + 4, pk->out, n);
    if (ZSTD_isError(got) || got != bytes || memcmp(pk->back, w, bytes)) return NULL;
    void *z = malloc(n ? n : 1);
    if (!z) return NULL;
    memcpy(z, pk->out, n);
    *zsize = (uint32_t)n;
    return z;
}

static inline uint64_t spv_pack_end(SpvStore *s, PlatMutex *lk, int step, void *z, uint32_t zsize, uint64_t now_ms,
                                    uint64_t idle_ms) {
    uint32_t *drop = NULL;
    plat_mutex_lock(lk);
    if (step == SPV_PACK_ENCODE) {
        s->pins--;
        if (z && !s->z) __atomic_store_n(&s->zsize, zsize, __ATOMIC_RELAXED), __atomic_store_n(&s->z, z, __ATOMIC_RELAXED), z = NULL;
    }
    if (s->plain && s->z && spv_idle_locked(s, now_ms, idle_ms)) drop = s->plain, __atomic_store_n(&s->plain, NULL, __ATOMIC_RELAXED);
    plat_mutex_unlock(lk);
    free(z);
    free(drop);
    return drop ? (uint64_t)s->words * 4 : 0;
}

static inline void spv_packer_free(SpvPacker *pk) {
    if (pk->cc) ZSTD_freeCCtx(pk->cc);
    free(pk->out), free(pk->back);
    memset(pk, 0, sizeof *pk);
}
