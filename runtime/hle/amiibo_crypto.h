#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "amiibo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMIIBO_KEYS_SIZE 160

bool amiibo_keys_load(const char *path);
bool amiibo_keys_loaded(void);
void amiibo_keys_clear(void);

typedef struct {
    uint8_t b[AMIIBO_DUMP_SIZE];
} AmiiboPlain;

bool amiibo_decrypt(const uint8_t tag[AMIIBO_DUMP_SIZE], AmiiboPlain *out);
bool amiibo_encrypt(const AmiiboPlain *plain, uint8_t tag[AMIIBO_DUMP_SIZE]);

typedef struct {
    uint8_t flags;
    uint8_t country_code;
    uint16_t write_counter;
    uint16_t setup_year;
    uint8_t setup_month, setup_day;
    uint16_t write_year;
    uint8_t write_month, write_day;
    char nickname[41];
    uint8_t mii[0x60];
    uint64_t title_id;
    uint16_t app_write_counter;
    uint32_t access_id;
    uint8_t app_area[AMIIBO_APP_AREA_SIZE];
} AmiiboFields;

#define AMIIBO_FLAG_REGISTERED 0x10u
#define AMIIBO_FLAG_APP_AREA   0x20u

void amiibo_plain_read(const AmiiboPlain *p, AmiiboFields *out);
void amiibo_plain_write(AmiiboPlain *p, const AmiiboFields *in);

bool amiibo_crypto_selftest(void);

#ifdef __cplusplus
}
#endif
