#pragma once
#include <stddef.h>
#include <stdint.h>

#define MESH_EMU_DOFF 256u
#define MESH_EMU_DWORDS 16u
#define MESH_EMU_DRECS 4096u
#define MESH_EMU_TOFF (MESH_EMU_DOFF + MESH_EMU_DRECS * MESH_EMU_DWORDS * 4u)
#define MESH_EMU_R_GROUPS 9u
#define MESH_EMU_R_FIRST 10u
#define MESH_EMU_R_ICUR 11u
#define MESH_EMU_R_ICAP 12u
#define MESH_EMU_R_PER 13u
#define MESH_EMU_R_TASK 14u
#define MESH_EMU_TWORDS 780u
#define MESH_EMU_PC_BYTES 32u

const uint32_t *mesh_emu_alloc_spirv(size_t *n);

typedef struct MeshEmuInfo {
    int task;
    uint32_t threads;
    uint32_t max_v, max_p;
    uint32_t prim;
    uint32_t psize_slot;
    uint32_t payload_words;
    uint32_t max_mesh;
    uint32_t slots;
    uint32_t n_generic;
    uint8_t generic_loc[32];
} MeshEmuInfo;

uint32_t *mesh_emu_compute(const uint32_t *w, size_t n, MeshEmuInfo *info, size_t *out_n, const char **why);

int mesh_emu_prim(const uint32_t *w, size_t n);

uint32_t *mesh_emu_vs(const MeshEmuInfo *m, uint32_t need, const uint8_t types[32], const uint8_t comps[32],
                      const uint32_t *fsw, size_t fsn, size_t *out_n);
