#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PRP_MAGIC "NX2NSPRP"
#define PRP_VERSION 2
#define PRP_REC 36
#define PRP_HEAD 16
#define PRP_FROM_SIZE 96

typedef struct {
    uint8_t programs[32];
    uint8_t answers[32];
    uint8_t translator[32];
} PrpFrom;

static inline size_t prp_head_write(uint8_t *out, const PrpFrom *from) {
    uint32_t v = PRP_VERSION, r = PRP_REC;
    memcpy(out, PRP_MAGIC, 8), memcpy(out + 8, &v, 4), memcpy(out + 12, &r, 4);
    memcpy(out + PRP_HEAD, from->programs, 32), memcpy(out + PRP_HEAD + 32, from->answers, 32);
    memcpy(out + PRP_HEAD + 64, from->translator, 32);
    return PRP_HEAD + PRP_FROM_SIZE;
}

static inline const uint8_t *prp_parse(const uint8_t *f, uint64_t size, size_t *n, PrpFrom *from, int *has_from) {
    uint32_t v = 0, r = 0;
    *n = 0, *has_from = 0;
    if (!f || size < PRP_HEAD || memcmp(f, PRP_MAGIC, 8)) return NULL;
    memcpy(&v, f + 8, 4), memcpy(&r, f + 12, 4);
    if (r != PRP_REC || (v != 1 && v != 2)) return NULL;
    uint64_t at = PRP_HEAD;
    if (v == 2) {
        if (size < PRP_HEAD + PRP_FROM_SIZE) return NULL;
        memcpy(from->programs, f + PRP_HEAD, 32), memcpy(from->answers, f + PRP_HEAD + 32, 32);
        memcpy(from->translator, f + PRP_HEAD + 64, 32);
        *has_from = 1, at += PRP_FROM_SIZE;
    }
    *n = (size_t)((size - at) / PRP_REC);
    return *n ? f + at : NULL;
}

uint8_t *gfx_shader_prepared_read(const char *path, size_t *n, PrpFrom *from, int *has_from);
