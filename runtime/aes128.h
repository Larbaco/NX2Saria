#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t rk[176];
} Aes128;

void aes128_init(Aes128 *a, const uint8_t key[16]);
void aes128_encrypt_block(const Aes128 *a, const uint8_t in[16], uint8_t out[16]);
void aes128_ctr(const Aes128 *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n);

#ifdef __cplusplus
}
#endif
