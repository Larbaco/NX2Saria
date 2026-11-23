// Contains code Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#pragma once
#include <stdint.h>

#include "gfx.h"
#include "host.h"
#include "nvn_obj.h"

#define NVN_API(n)                                                                                                   \
    static void nvn_impl_##n(Cpu *c);                                                                                \
    void nvnp_##n(Cpu *c) {                                                                                          \
        static uint8_t seen;                                                                                         \
        nvn_called(&seen, #n);                                                                                       \
        nvn_impl_##n(c);                                                                                             \
    }                                                                                                                \
    static void nvn_impl_##n(Cpu *c)
void nvn_called(uint8_t *seen, const char *name);
#define NVN_LOG(...) do { if (rt_trace & RT_TRACE_GFX) rt_log("nvn: " __VA_ARGS__); } while (0)
#define NVN_BLOCKING() __attribute__((cleanup(kern_core_back))) int nvn_away_ = kern_core_away()
static inline int nvn_arg_bool(const Cpu *c, int i) { return (int)(arg_x(c, i) & 0xff); }
int nvn_no_object(Cpu *c, const char *what);

#define NVN_POOL_MAGIC MAGIC('P', 'O', 'O', 'L')
#define NVN_VPOOL_PAGE 0x10000ull
static inline int nvn_gpu_only(uint64_t a) { return a - RT_GPU_VA_BASE < RT_GPU_VA_SIZE; }

typedef struct { NVNdevice *dev; pool_t *pool; int64_t off; uint64_t size; } buf_builder_t;   FITS(buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; pool_t *pool; int64_t off; uint64_t size; } buf_t;           FITS(buf_t, NVNbuffer);
typedef struct {
    const NVNmemoryPool *physical_pool;
    int64_t physical_offset, virtual_offset;
    uint64_t size;
    int32_t storage_class;
    uint32_t padding;
} nvn_mapping_t;
_Static_assert(sizeof(nvn_mapping_t) == 0x28, "NVNmappingRequest stride");

const pool_t *nvn_pool_find(uint64_t addr, uint64_t len);
uint64_t nvn_prog_made_ns(uint64_t serial);
uint64_t nvn_new_serial(void);
void nvn_call_note(int present, uintptr_t q, int enter);
uint64_t nvn_fence_last(void);
int nvn_depth_minus_w(void);
extern int nvn_submits_in_frame;
void nvn_guest_written(uint64_t addr, uint64_t size);
void nvn_flush_stats(uint64_t out[2][3][2]);
void nvn_submit_prepare(const uint64_t *handles, uint64_t *out, int n);
void nvn_submit_done(const uint64_t *handles, const uint64_t *out, int n);
void nvn_submit_prepare_async(const uint64_t *handles, uint64_t *out, int n);
int gfx_replay_async(void);
uint64_t gfx_replayed_submits(void);
void gfx_event_signalling(const void *value, uint32_t to);
#define NVN_HANDLE_TEX(h) ((int)((h) & 0xfffff))
#define NVN_HANDLE_SAMP(h) ((int)(((h) >> 20) & 0xfff))
extern GfxStats gfx_counters;

#define NVN_TEX_MAGIC MAGIC('T', 'E', 'X', 0)
#define NVN_SAMP_MAGIC MAGIC('S', 'M', 'P', 0)
#define NVN_PROG_MAGIC MAGIC('P', 'R', 'O', 'G')
#define NVN_WIN_MAGIC MAGIC('W', 'I', 'N', 'D')
int nvn_tex_live_at(const void *mem, tex_t *out);
uint64_t nvn_tex_view_offset(const tex_t *t, const view_t *v);

typedef struct { volatile uint32_t *val; uint32_t mode, value; } p_signal_event;
typedef struct { volatile uint32_t *val; uint32_t mode, value; } p_wait_event;
typedef struct { uint64_t type, addr; } p_report_counter;
typedef struct { uint64_t type; } p_reset_counter;
typedef struct { uint64_t src, dst, size; } p_copy_b2b;
typedef struct { uint64_t dst, size; uint8_t data[]; } p_update_ub;
typedef struct { int32_t bits, pad; } p_barrier;
typedef struct { uint64_t target; } p_jump;
typedef struct { int32_t prim, itype; uint64_t ibuf, ind; } p_draw_idx_ind;
typedef struct { uint64_t ind, caller; } p_dispatch_ind;
typedef struct { uint64_t addr, size; uint32_t value, pad; } p_clear_buf;
_Static_assert(sizeof(p_signal_event) == 16 && sizeof(p_report_counter) == 16 && sizeof(p_copy_b2b) == 24 &&
               sizeof(p_update_ub) == 16, "op payload layouts of hle_nvn.c");
static inline const op_hdr *nvn_op_first(uint64_t handle) { return (const op_hdr *)(uintptr_t)handle; }
static inline const op_hdr *nvn_op_next(const op_hdr *h) { return (const op_hdr *)(const void *)((const uint8_t *)h + h->size); }
static inline const void *nvn_op_payload(const op_hdr *h) { return h + 1; }
int nvn_handle_live(uint64_t handle);
uint64_t nvn_handle_bytes(uint64_t handle);
uint64_t nvn_submit_count(void);
#define NVN_PIN_AGE 1200
void nvn_pins_sweep(uint64_t now, const tex_t *const *keep, int nkeep);
void nvn_pins_stats(uint64_t *live, uint64_t *freed);
void nvn_copies_stats(uint64_t *n, uint64_t *bytes, uint64_t *idle_n, uint64_t *idle_bytes);
const char *nvn_op_name(uint32_t op);
void nvn_pool_invalidate(const pool_t *p, uint64_t off, uint64_t size);
void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size);
