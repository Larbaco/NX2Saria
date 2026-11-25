#pragma once
#include <stdint.h>

#define UPD_SLOTS 48

typedef struct {
    uint64_t fence;
    uint64_t presents;
    int kind;
    void *handle, *aux;
} UpdEntry;

typedef struct {
    UpdEntry e[UPD_SLOTS];
    int n;
    uint64_t put, released;
} UpdList;

typedef void (*UpdRelease)(const UpdEntry *e, void *arg);

static inline int upd_put(UpdList *l, int kind, void *handle, void *aux, uint64_t fence, uint64_t presents) {
    if (!handle) return 1;
    if (l->n >= UPD_SLOTS) return 0;
    UpdEntry *e = &l->e[l->n++];
    e->fence = fence, e->presents = presents, e->kind = kind, e->handle = handle, e->aux = aux;
    l->put++;
    return 1;
}

static inline int upd_ready(const UpdEntry *e, uint64_t done, uint64_t presents) {
    return e->fence <= done && e->presents <= presents;
}

static inline int upd_take(UpdList *l, uint64_t done, uint64_t presents, UpdRelease release, void *arg) {
    UpdEntry go[UPD_SLOTS];
    int k = 0, ng = 0;
    for (int i = 0; i < l->n; i++) {
        if (upd_ready(&l->e[i], done, presents)) go[ng++] = l->e[i];
        else l->e[k++] = l->e[i];
    }
    l->n = k;
    for (int i = 0; i < ng; i++) release(&go[i], arg);
    l->released += (uint64_t)ng;
    return ng;
}

static inline int upd_take_all(UpdList *l, UpdRelease release, void *arg) {
    return upd_take(l, UINT64_MAX, UINT64_MAX, release, arg);
}

static inline uint64_t upd_max_fence(const UpdList *l) {
    uint64_t m = 0;
    for (int i = 0; i < l->n; i++)
        if (l->e[i].fence > m) m = l->e[i].fence;
    return m;
}

static inline int upd_count(const UpdList *l, int kind) {
    int n = 0;
    for (int i = 0; i < l->n; i++) n += kind < 0 || l->e[i].kind == kind;
    return n;
}
