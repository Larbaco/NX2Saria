// SARC archives and the zstd dictionaries of Pack/ZsDic.pack.zs. A SARC is a little-endian header ("SARC", its
// length at +4, the byte-order mark 0xfeff at +6, the archive's size at +8, the start of the members' data at +12),
// then SFAT: its member count at +6 and from +12 one 16-byte entry per member (name hash, attributes, start and end
// relative to the data). An entry's attributes hold a nonzero top byte when the member has a name, and in the low 24
// bits the name's offset in SFNT's string table divided by 4; SFNT's 8-byte header follows the entries. The game's
// archives may hold several members under one name (UI/Layout2Archive/Common...blarc.zs holds each layout four times),
// so members are walked one by one. sarc_rebuild lays the members out again when some of them change size.
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "sarc.h"

// The header's fields: SFAT's start, its entry count and the start of the members' data. 0 when it is damaged.
static int sarc_head(const uint8_t *p, size_t n, uint16_t *head, uint16_t *count, uint32_t *data) {
    uint16_t bom;
    if (n < 0x14 || memcmp(p, "SARC", 4)) return 0;
    memcpy(head, p + 4, 2);
    memcpy(&bom, p + 6, 2);
    memcpy(data, p + 12, 4);
    if (bom != 0xfeff || (size_t)*head + 12 > n || memcmp(p + *head, "SFAT", 4)) return 0;
    memcpy(count, p + *head + 6, 2);
    size_t names = (size_t)*head + 12 + (size_t)*count * 16;   // the entries end where SFNT starts
    return names + 8 <= n && !memcmp(p + names, "SFNT", 4);
}

int sarc_count(const uint8_t *p, size_t n) {
    uint32_t data;
    uint16_t head, count;
    return sarc_head(p, n, &head, &count, &data) ? count : -1;
}

int sarc_next(const uint8_t *p, size_t n, const char *name, unsigned *next, size_t *at, size_t *size) {
    uint32_t data;
    uint16_t head, count;
    if (!sarc_head(p, n, &head, &count, &data)) return -1;
    size_t names = (size_t)head + 12 + (size_t)count * 16 + 8;
    size_t len = name ? strlen(name) : 0;
    for (unsigned i = *next; i < count; i++) {
        uint32_t attr, lo, hi;
        const uint8_t *e = p + head + 12 + (size_t)i * 16;
        memcpy(&attr, e + 4, 4);
        memcpy(&lo, e + 8, 4);
        memcpy(&hi, e + 12, 4);
        if (name) {
            size_t s = names + (size_t)(attr & 0xffffff) * 4;
            if (!(attr >> 24) || s > n || n - s < len + 1 || memcmp(p + s, name, len + 1)) continue;
        }
        if (lo > hi || (size_t)data + hi > n) return -1;
        *at = (size_t)data + lo;
        *size = hi - lo;
        *next = i + 1;
        return 1;
    }
    *next = count;
    return 0;
}

int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    unsigned next = 0;
    return sarc_next(p, n, name, &next, at, size) == 1;
}

// An entry's range of the members' data, and the entry's number.
typedef struct {
    uint32_t lo, hi;
    unsigned entry;
} Range;

static int range_order(const void *a, const void *b) {
    const Range *x = a, *y = b;
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    if (x->hi != y->hi) return x->hi < y->hi ? -1 : 1;
    return x->entry < y->entry ? -1 : x->entry > y->entry;
}

uint8_t *sarc_rebuild(const uint8_t *p, size_t n, const uint8_t *const *with, const size_t *with_n, size_t *out_n) {
    enum { MAX_ALIGN = 0x2000 };
    uint32_t data;
    uint16_t head, count;
    if (!sarc_head(p, n, &head, &count, &data) || data > n) return NULL;
    Range *r = malloc(((size_t)count + 1) * sizeof *r);
    uint64_t *at = malloc(((size_t)count + 1) * sizeof *at);   // each entry's new start, from the data's
    uint8_t *out = NULL;
    if (!r || !at) goto fail;
    for (unsigned i = 0; i < count; i++) {
        const uint8_t *e = p + head + 12 + (size_t)i * 16;
        memcpy(&r[i].lo, e + 8, 4);
        memcpy(&r[i].hi, e + 12, 4);
        r[i].entry = i;
        if (r[i].lo > r[i].hi || (size_t)data + r[i].hi > n) goto fail;
        if (with[i] && with_n[i] > UINT32_MAX) goto fail;
    }
    qsort(r, count, sizeof *r, range_order);
    // Lay the ranges out in the archive's order: a kept range that is the same as the last kept one shares its place.
    uint64_t pos = data, old_end = data;
    int kept = -1;
    for (unsigned k = 0; k < count; k++) {
        const Range *e = &r[k];
        int same = k && r[k - 1].lo == e->lo && r[k - 1].hi == e->hi;
        if (!with[e->entry] && kept >= 0 && r[kept].lo == e->lo && r[kept].hi == e->hi) {
            at[e->entry] = at[r[kept].entry];
            continue;
        }
        if (!same && data + (uint64_t)e->lo < old_end) goto fail;   // inside the range before, not the same one
        uint64_t start = data + (uint64_t)e->lo, align = start ? start & (~start + 1) : MAX_ALIGN;
        if (align > MAX_ALIGN) align = MAX_ALIGN;
        pos = (pos + align - 1) & ~(align - 1);
        at[e->entry] = pos - data;
        pos += with[e->entry] ? with_n[e->entry] : e->hi - e->lo;
        if (data + (uint64_t)e->hi > old_end) old_end = data + (uint64_t)e->hi;
        if (!with[e->entry]) kept = (int)k;
    }
    uint64_t total = pos + (n - old_end);   // what followed the last member follows it still
    if (total > UINT32_MAX) goto fail;
    if (!(out = calloc((size_t)total, 1))) goto fail;
    memcpy(out, p, data);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *e = out + head + 12 + (size_t)i * 16;
        uint32_t lo, hi;
        memcpy(&lo, e + 8, 4);
        memcpy(&hi, e + 12, 4);
        size_t size = with[i] ? with_n[i] : hi - lo;
        memcpy(out + data + at[i], with[i] ? with[i] : p + data + lo, size);
        uint32_t nlo = (uint32_t)at[i], nhi = (uint32_t)(at[i] + size);
        memcpy(e + 8, &nlo, 4);
        memcpy(e + 12, &nhi, 4);
    }
    memcpy(out + pos, p + old_end, n - old_end);
    uint32_t t32 = (uint32_t)total;
    memcpy(out + 8, &t32, 4);
    *out_n = (size_t)total;
    free(r);
    free(at);
    return out;
fail:
    free(r);
    free(at);
    free(out);
    return NULL;
}

uint8_t *zsdic_unpack(const void *z, size_t zn, size_t max, size_t *n) {
    unsigned long long rn = ZSTD_getFrameContentSize(z, zn);
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || !rn || rn > max) return NULL;
    uint8_t *raw = malloc((size_t)rn);
    size_t got = raw ? ZSTD_decompress(raw, (size_t)rn, z, zn) : 0;
    if (!raw || ZSTD_isError(got) || got != rn) {
        free(raw);
        return NULL;
    }
    *n = (size_t)rn;
    return raw;
}

int zsdic_find(const uint8_t *dics, size_t n, unsigned id, size_t *at, size_t *size) {
    unsigned next = 0;
    if (!id) return 0;
    while (sarc_next(dics, n, NULL, &next, at, size) == 1)
        if (ZSTD_getDictID_fromDict(dics + *at, *size) == id) return 1;
    return 0;
}
