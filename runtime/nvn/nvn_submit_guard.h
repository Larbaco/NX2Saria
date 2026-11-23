// Contains code Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#pragma once
#include <stdint.h>
#include <string.h>

#include "nvn_obj.h"

static inline int nvn_copy_enter(uint32_t *busy) { return __atomic_fetch_add(busy, 1, __ATOMIC_ACQ_REL) == 0; }
static inline void nvn_copy_leave(uint32_t *busy) { __atomic_fetch_sub(busy, 1, __ATOMIC_RELEASE); }

static inline int nvn_copy_again(const uint64_t *handles, const uint64_t *out, int i) {
    for (int j = 0; j < i; j++)
        if (handles[j] == handles[i] && out[j] == handles[j]) return 1;
    return 0;
}

typedef void (*NvnBindNote)(const p_bind_handles *b, const uint64_t *now, uint64_t nth);

static inline int nvn_resolve_binds(const uint8_t *p, int depth, uint64_t *reads, uint64_t *changed, NvnBindNote note,
                                    const uint8_t **bad) {
    int nd = 0;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) return *bad = p, -1;
        if (h.op == OP_END) return nd;
        if (h.op == OP_JUMP) {
            memcpy(&p, p + 8, 8);
            continue;
        }
        if (h.op == OP_CALL) {
            uint64_t t;
            memcpy(&t, p + 8, 8);
            if (depth >= 16) return *bad = p, -1;
            int k = nvn_resolve_binds((const uint8_t *)(uintptr_t)t, depth + 1, reads, changed, note, bad);
            if (k < 0) return k;
            nd += k;
        } else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) {
            p_bind_handles *b = (p_bind_handles *)(uintptr_t)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                nd++;
                uint64_t cur[32];
                memcpy(cur, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
                if (reads) __atomic_fetch_add(reads, 1, __ATOMIC_RELAXED);
                if (memcmp(cur, b->h, 8 * (size_t)b->n)) {
                    uint64_t nth = changed ? __atomic_fetch_add(changed, 1, __ATOMIC_RELAXED) : 0;
                    if (note) note(b, cur, nth);
                    memcpy(b->h, cur, 8 * (size_t)b->n);
                }
            }
        }
        p += h.size;
    }
}
