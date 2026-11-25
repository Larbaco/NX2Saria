#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SKIN_PAIRED, SKIN_NEW, SKIN_COUNT, SKIN_SIZE, SKIN_ADDRESS, SKIN_PAIR_WHYS };

typedef struct {
    uint64_t key;
    uint64_t order;
    uint64_t palette;
    uint32_t bytes;
    uint32_t tag;
    uint32_t nth, of;
    uint64_t before;
    int32_t prev;
    uint8_t why;
} SkinEntry;

static inline uint64_t skin_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static int skin_entry_cmp(const void *a, const void *b) {
    const SkinEntry *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static inline void skin_number(SkinEntry *e, int n) {
    qsort(e, (size_t)n, sizeof *e, skin_entry_cmp);
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && e[j].key == e[i].key) j++;
        for (int k = i; k < j; k++) e[k].nth = (uint32_t)(k - i), e[k].of = (uint32_t)(j - i);
        i = j;
    }
}

static inline int skin_find(const SkinEntry *e, int n, uint64_t key) {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (e[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && e[lo].key == key ? lo : -1;
}

static inline void skin_pair(SkinEntry *cur, int ncur, const SkinEntry *last, int nlast, int consecutive) {
    for (int i = 0; i < ncur; i++) {
        SkinEntry *c = &cur[i];
        c->prev = -1, c->before = 0, c->why = SKIN_NEW;
        int p = skin_find(last, nlast, c->key);
        if (p < 0) continue;
        if (last[p].of != c->of) {
            if (consecutive) c->why = SKIN_COUNT;
            continue;
        }
        const SkinEntry *l = &last[p + (int)c->nth];
        if (l->bytes != c->bytes) {
            if (consecutive) c->why = SKIN_SIZE;
            continue;
        }
        c->before = c->palette != l->palette ? l->palette : l->before;
        if (!consecutive) continue;
        if (c->palette != l->palette && c->palette != l->before) {
            c->why = SKIN_ADDRESS;
            continue;
        }
        c->prev = p + (int)c->nth, c->why = SKIN_PAIRED;
    }
}

static inline int skin_near(const void *now, const void *then, uint32_t bytes, uint32_t stride, float limit) {
    static const uint8_t at[6] = {3, 7, 11, 12, 13, 14};
    int places = stride >= 64 ? 6 : 3;
    if (stride < 48) return 0;
    for (uint32_t o = 0; o + stride <= bytes; o += stride) {
        float a[16], b[16];
        memcpy(a, (const uint8_t *)now + o, stride < 64 ? 48 : 64);
        memcpy(b, (const uint8_t *)then + o, stride < 64 ? 48 : 64);
        for (int k = 0; k < places; k++) {
            float d = a[at[k]] - b[at[k]];
            if (!(d <= limit && d >= -limit)) return 0;
        }
    }
    return 1;
}
