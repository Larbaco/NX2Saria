#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MG_MAX 32
#define MG_NOISE (1ull << 20)

typedef struct {
    const char *name;
    uint64_t first;
    uint64_t prev, last;
    uint32_t ups, downs;
} MgCat;

typedef struct {
    MgCat c[MG_MAX];
    int n;
    uint32_t lines;
    uint32_t play_lines;
    int play;
} MgBook;

static inline int mg_cat(MgBook *b, const char *name) {
    if (b->n >= MG_MAX) return -1;
    memset(&b->c[b->n], 0, sizeof b->c[b->n]);
    b->c[b->n].name = name;
    return b->n++;
}

static inline void mg_take(MgBook *b, const uint64_t *v, int in_play) {
    int base = !b->play && in_play;
    for (int i = 0; i < b->n; i++) {
        MgCat *c = &b->c[i];
        c->prev = b->lines ? c->last : v[i];
        c->last = v[i];
        if (base) c->first = v[i];
        else if (b->play && c->last >= c->prev + MG_NOISE) c->ups++;
        else if (b->play && c->prev >= c->last + MG_NOISE) c->downs++;
    }
    if (base) b->play = 1;
    b->lines++;
    if (b->play) b->play_lines++;
}

static inline int64_t mg_since_prev(const MgCat *c) { return (int64_t)(c->last - c->prev); }
static inline int64_t mg_since_first(const MgBook *b, const MgCat *c) { return b->play ? (int64_t)(c->last - c->first) : 0; }
static inline int mg_only_climbs(const MgBook *b, const MgCat *c) { return b->play && c->ups && !c->downs; }

__attribute__((format(printf, 4, 5))) static inline void mg_put(char *out, size_t cap, size_t *k, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*k < cap ? out + *k : NULL, *k < cap ? cap - *k : 0, fmt, ap);
    va_end(ap);
    if (n > 0) *k += (size_t)n;
}

static inline int mg_grew(const MgBook *b, uint64_t step, char *out, size_t cap) {
    size_t k = 0;
    int named = 0, any;
    if (cap) out[0] = 0;
    any = 0;
    for (int i = 0; i < b->n; i++) {
        const MgCat *c = &b->c[i];
        int64_t d = mg_since_prev(c);
        if (b->lines < 2 || d <= (int64_t)step) continue;
        mg_put(out, cap, &k, "%s%s %+.0f MiB", any ? ", " : "since the previous report: ", c->name, d / 1048576.0);
        any = 1, named++;
    }
    int listed = any;
    any = 0;
    for (int i = 0; i < b->n; i++) {
        const MgCat *c = &b->c[i];
        int64_t d = mg_since_first(b, c);
        if (b->play_lines < 2 || d <= (int64_t)step) continue;
        mg_put(out, cap, &k, "%s%s %+.0f MiB%s", any ? ", " : listed ? "; since play began: " : "since play began: ",
               c->name, d / 1048576.0, mg_only_climbs(b, c) ? " (only climbs)" : "");
        any = 1, named++;
    }
    if (cap && k >= cap) out[cap - 1] = 0;
    return named;
}

static inline int mg_rank(const MgBook *b, char *out, size_t cap) {
    size_t k = 0;
    if (cap) out[0] = 0;
    if (!b->play) return 0;
    int order[MG_MAX], n = 0;
    for (int i = 0; i < b->n; i++) {
        int64_t d = mg_since_first(b, &b->c[i]);
        int j = n++;
        while (j > 0 && mg_since_first(b, &b->c[order[j - 1]]) < d) order[j] = order[j - 1], j--;
        order[j] = i;
    }
    int grew = 0, fell = 0, flat = 0;
    for (int j = 0; j < n; j++) {
        const MgCat *c = &b->c[order[j]];
        int64_t d = mg_since_first(b, c);
        if (d < (int64_t)MG_NOISE) continue;
        mg_put(out, cap, &k, "%s%s %+.0f MiB%s", grew ? ", " : "", c->name, d / 1048576.0,
               mg_only_climbs(b, c) ? " (only climbs)" : "");
        grew++;
    }
    if (!grew) mg_put(out, cap, &k, "nothing grew");
    for (int j = n - 1; j >= 0; j--) {
        const MgCat *c = &b->c[order[j]];
        int64_t d = mg_since_first(b, c);
        if (d > -(int64_t)MG_NOISE) continue;
        mg_put(out, cap, &k, "%s%s %+.0f MiB", fell ? ", " : "; fell: ", c->name, d / 1048576.0);
        fell++;
    }
    for (int j = 0; j < n; j++) {
        int64_t d = mg_since_first(b, &b->c[order[j]]);
        flat += d < (int64_t)MG_NOISE && d > -(int64_t)MG_NOISE;
    }
    if (flat) mg_put(out, cap, &k, "; within 1 MiB of the base: %d more", flat);
    if (cap && k >= cap) out[cap - 1] = 0;
    return grew;
}
