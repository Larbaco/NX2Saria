#ifndef SARC_H
#define SARC_H

#include <stddef.h>
#include <stdint.h>

int sarc_next(const uint8_t *p, size_t n, const char *name, unsigned *next, size_t *at, size_t *size);

int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size);

int sarc_count(const uint8_t *p, size_t n);

uint8_t *sarc_rebuild(const uint8_t *p, size_t n, const uint8_t *const *with, const size_t *with_n, size_t *out_n);

uint8_t *zsdic_unpack(const void *z, size_t zn, size_t max, size_t *n);

int zsdic_find(const uint8_t *dics, size_t n, unsigned id, size_t *at, size_t *size);

#endif
