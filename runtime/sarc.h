// SARC archives and the zstd dictionaries of Pack/ZsDic.pack.zs (sarc.c).
#ifndef SARC_H
#define SARC_H

#include <stddef.h>
#include <stdint.h>

// The next member of the SARC p[0..n) from member *next on that is named `name` (any member when name is NULL): 1 with
// its start and size, *next moved past it; 0 when no such member is left; -1 when the archive is damaged (its header,
// or the found member's range).
int sarc_next(const uint8_t *p, size_t n, const char *name, unsigned *next, size_t *at, size_t *size);

// The first member named `name`: 1 with its start and size; 0 when there is none or the archive is damaged.
int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size);

// The number of member entries of the SARC p[0..n); -1 when its header is damaged.
int sarc_count(const uint8_t *p, size_t n);

// The SARC p[0..n) with the data of some members replaced: entry i (of sarc_count's) takes with[i][0..with_n[i]) in
// place of its own bytes, or keeps them where with[i] is NULL. The header, the entries' order, hashes and names, and
// SFNT stay; the members' data is laid out again in the order the archive has it, each member at the first offset past
// the one before that keeps its start's alignment (the largest power of two dividing it, up to 8 KiB), and the entries'
// ranges and the archive's size follow. Entries that shared one range share it still unless one of them is replaced;
// a replaced entry gets a range of its own. malloc'd, *out_n its size; NULL when the archive is damaged, two entries'
// ranges overlap without being the same, or out of memory.
uint8_t *sarc_rebuild(const uint8_t *p, size_t n, const uint8_t *const *with, const size_t *with_n, size_t *out_n);

// Pack/ZsDic.pack.zs's archive (z[0..zn), a frame compressed without a dictionary) unpacked: malloc'd, *n its size;
// NULL when it is not a zstd frame that says its size, or that size is over max.
uint8_t *zsdic_unpack(const void *z, size_t zn, size_t max, size_t *n);

// The dictionary with id `id` among the members of that archive (dics[0..n)): 1 with its start and size; 0 when none
// has it (and always for id 0, the id of a frame without a dictionary).
int zsdic_find(const uint8_t *dics, size_t n, unsigned id, size_t *at, size_t *size);

#endif
