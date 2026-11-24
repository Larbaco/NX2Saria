#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t sz_crc32(uint32_t crc, const void *data, size_t n);

uint8_t *sz_deflate(const uint8_t *src, size_t n, size_t *out_n);
int sz_inflate(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_n);

typedef struct {
    char *name;
    uint32_t crc, csize, usize, offset;
    uint16_t method;
} SzMember;
typedef struct {
    uint8_t *buf;
    size_t n, cap;
    SzMember *m;
    int count, cap_m;
    int failed;
    uint16_t dos_time, dos_date;
} SzWriter;
void sz_writer_init(SzWriter *w, int year, int month, int day, int hour, int minute, int second);
void sz_writer_add(SzWriter *w, const char *name, const void *data, size_t n);
uint8_t *sz_writer_finish(SzWriter *w, size_t *n);

enum { SZ_OK, SZ_E_NOT_ZIP, SZ_E_DAMAGED, SZ_E_UNSUPPORTED, SZ_E_MEMORY };
typedef struct {
    char name[256];
    uint32_t crc;
    uint64_t csize, usize, data;
    uint16_t method;
    int is_dir, is_link;
} SzEntry;
int sz_read_dir(const uint8_t *zip, size_t n, SzEntry **out, int *count);
uint8_t *sz_extract(const uint8_t *zip, size_t n, const SzEntry *e, int *err);

int sz_json_str(const char *json, const char *key, char *out, size_t cap);
int sz_json_int(const char *json, const char *key, long long *out);
char *sz_json_escape(const char *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
