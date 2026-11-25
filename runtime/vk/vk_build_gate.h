#pragma once

typedef struct {
    double free_mb;
    double reserved_mb;
} BuildGate;

static inline int build_gate_admits(const BuildGate *g, double used_mb, double limit_mb, double est_mb) {
    if (g->free_mb <= 0 || limit_mb < 0) return 1;
    return limit_mb - used_mb - g->reserved_mb >= g->free_mb + est_mb;
}

static inline int build_gate_enter(BuildGate *g, double used_mb, double limit_mb, double est_mb) {
    if (!build_gate_admits(g, used_mb, limit_mb, est_mb)) return 0;
    g->reserved_mb += est_mb;
    return 1;
}

static inline void build_gate_leave(BuildGate *g, double est_mb) {
    g->reserved_mb -= est_mb;
    if (g->reserved_mb < 0) g->reserved_mb = 0;
}
