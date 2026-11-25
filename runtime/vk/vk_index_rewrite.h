#pragma once
#include <stdint.h>
#include <string.h>

static inline uint32_t idx_read(const uint8_t *src, uint32_t unit, uint32_t i) {
    if (unit == 1) return src[i];
    if (unit == 2) {
        uint16_t v;
        memcpy(&v, src + 2 * (size_t)i, 2);
        return v;
    }
    uint32_t v;
    memcpy(&v, src + 4 * (size_t)i, 4);
    return v;
}

static inline uint32_t idx_quads(const uint8_t *src, uint32_t unit, uint32_t count, uint32_t *out) {
    static const uint8_t k[6] = {0, 1, 2, 0, 2, 3};
    uint32_t q = count / 4;
    for (uint32_t i = 0; i < q; i++)
        for (int j = 0; j < 6; j++) out[6 * i + (uint32_t)j] = idx_read(src, unit, 4 * i + k[j]);
    return 6 * q;
}

static inline uint32_t idx_loop(const uint8_t *src, uint32_t unit, uint32_t count, uint32_t *out) {
    for (uint32_t i = 0; i < count; i++) out[i] = idx_read(src, unit, i);
    out[count] = out[0];
    return count + 1;
}
