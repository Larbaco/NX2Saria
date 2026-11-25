#pragma once
#include <stdint.h>
#include <string.h>

enum { TCP_STAY, TCP_MOVE, TCP_DROP };

typedef struct {
    uint32_t block;
    uint8_t what;
    uint64_t bytes;
} TcpAlloc;

typedef struct {
    uint64_t id;
    uint64_t size;
    uint8_t held;
    uint64_t move, drop, stay;
    uint32_t n_move, n_drop, n_stay;
    uint8_t victim;
} TcpBlock;

typedef struct {
    uint64_t free_bytes;
    uint32_t sparse_pct;
    uint32_t room_pct;
    uint64_t min_gain;
    uint64_t max_move;
    uint32_t max_blocks;
} TcpParams;

typedef struct {
    uint32_t blocks, sparse, emptiable;
    uint64_t block_bytes, listed_bytes;
    uint64_t move_bytes, drop_bytes, stay_bytes;
    uint64_t sparse_bytes, sparse_listed;
    uint64_t emptiable_bytes, emptiable_move, emptiable_drop;
} TcpStats;

typedef struct {
    uint32_t n;
    uint64_t gain;
    uint64_t move, drop;
    uint32_t n_move, n_drop;
} TcpPlan;

static inline void tcp_tally(TcpBlock *b, int nb, const TcpAlloc *a, int na) {
    for (int i = 0; i < nb; i++) {
        b[i].move = b[i].drop = b[i].stay = 0;
        b[i].n_move = b[i].n_drop = b[i].n_stay = 0;
        b[i].victim = 0;
    }
    for (int i = 0; i < na; i++) {
        if (a[i].block >= (uint32_t)nb) continue;
        TcpBlock *k = &b[a[i].block];
        if (a[i].what == TCP_MOVE) k->move += a[i].bytes, k->n_move++;
        else if (a[i].what == TCP_DROP) k->drop += a[i].bytes, k->n_drop++;
        else k->stay += a[i].bytes, k->n_stay++;
    }
}

static inline uint64_t tcp_listed(const TcpBlock *k) { return k->move + k->drop + k->stay; }

static inline int tcp_sparse(const TcpBlock *k, uint32_t pct) {
    uint64_t l = tcp_listed(k);
    return k->size && l <= k->size && l * 100 <= k->size * (uint64_t)pct;
}

static inline int tcp_emptiable(const TcpBlock *k, uint32_t pct) {
    return !k->held && !k->n_stay && (k->n_move || k->n_drop) && tcp_sparse(k, pct);
}

static inline void tcp_stats(const TcpBlock *b, int nb, uint32_t sparse_pct, TcpStats *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < nb; i++) {
        const TcpBlock *k = &b[i];
        s->blocks++, s->block_bytes += k->size, s->listed_bytes += tcp_listed(k);
        s->move_bytes += k->move, s->drop_bytes += k->drop, s->stay_bytes += k->stay;
        if (!tcp_sparse(k, sparse_pct)) continue;
        s->sparse++, s->sparse_bytes += k->size, s->sparse_listed += tcp_listed(k);
        if (!tcp_emptiable(k, sparse_pct)) continue;
        s->emptiable++, s->emptiable_bytes += k->size, s->emptiable_move += k->move, s->emptiable_drop += k->drop;
    }
}

static inline int tcp_plan(TcpBlock *b, int nb, const TcpParams *p, uint32_t *order, int max_order, TcpPlan *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < nb; i++) b[i].victim = 0;
    uint64_t inside = 0;
    uint32_t most = p->max_blocks < (uint32_t)(max_order > 0 ? max_order : 0) ? p->max_blocks : (uint32_t)(max_order > 0 ? max_order : 0);
    while (out->n < most) {
        int best = -1;
        for (int i = 0; i < nb; i++) {
            if (b[i].victim || !tcp_emptiable(&b[i], p->sparse_pct)) continue;
            if (best < 0 || b[i].move < b[best].move || (b[i].move == b[best].move && b[i].size > b[best].size)) best = i;
        }
        if (best < 0) break;
        TcpBlock *k = &b[best];
        uint64_t in = inside + (k->size - tcp_listed(k)), need = out->move + k->move;
        uint64_t outside = p->free_bytes > in ? p->free_bytes - in : 0;
        if (need > p->max_move || need * 100 > outside * (uint64_t)p->room_pct) {
            k->victim = 2;
            continue;
        }
        k->victim = 1;
        inside = in;
        order[out->n++] = (uint32_t)best;
        out->gain += k->size, out->move += k->move, out->drop += k->drop;
        out->n_move += k->n_move, out->n_drop += k->n_drop;
    }
    for (int i = 0; i < nb; i++)
        if (b[i].victim == 2 || out->gain < p->min_gain) b[i].victim = 0;
    if (out->gain < p->min_gain) memset(out, 0, sizeof *out);
    return (int)out->n;
}
