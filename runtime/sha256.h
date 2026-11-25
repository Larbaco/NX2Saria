#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t buf[64];
    uint32_t used;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t n);
void sha256_final(Sha256 *s, uint8_t out[32]);
void sha256(const void *data, size_t n, uint8_t out[32]);
int sha256_ni(void);
void hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len, uint8_t out[32]);
