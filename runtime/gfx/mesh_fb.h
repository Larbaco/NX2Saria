#pragma once
#include <stddef.h>
#include <stdint.h>

#define MESHFB_MAX_LINES 136

typedef struct {
    int stage;
    uint32_t threads;
    uint32_t maxv, maxp;
    uint32_t ipp;
    uint32_t payload_bytes;
    int has_task;
    uint32_t nlines;
    uint16_t lines[MESHFB_MAX_LINES];
    uint32_t attr, map, pay, shared_bytes;
} MeshFbInfo;

enum { MESHFB_CB_TASK = 0x41, MESHFB_CB_MESH = 0x42, MESHFB_CB_LIST = 0x43 };
#define MESHFB_LIST_TASK_BITS 24

int meshfb_info(const uint8_t *control, const uint8_t *header, int stage, MeshFbInfo *out, char *err, size_t cap);
static inline uint32_t meshfb_slot_words(const MeshFbInfo *m) { return m->pay / 4; }

uint32_t *meshfb_task_cs(const uint32_t *spirv, uint64_t words, const MeshFbInfo *m, uint32_t binding, uint64_t *out_words,
                         char *err, size_t cap);
uint32_t *meshfb_mesh_cs(const uint32_t *spirv, uint64_t words, const MeshFbInfo *m, uint32_t binding,
                         uint32_t task_payload_words, uint64_t *out_words, char *err, size_t cap);
uint32_t *meshfb_vs(const MeshFbInfo *m, uint32_t out_mask, const uint8_t out_type[32], uint64_t *out_words);
char *meshfb_res_json(const char *res, uint32_t binding, const int *cb_slots, int n);
