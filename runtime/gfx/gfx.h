#pragma once
#include <stdint.h>
#include "host.h"
#include "nvn_obj.h"

enum { OE_MSG_FOCUS_STATE_CHANGED = 15, OE_MSG_OPERATION_MODE_CHANGED = 30, OE_MSG_PERFORMANCE_MODE_CHANGED = 31 };
void oe_push_message(uint32_t message);
void oe_set_focus(int in_focus);
typedef struct { uint64_t alloc, free, realloc, user; } OvNvAllocator;
typedef struct { OvNvAllocator graphics, devtools; uint64_t memory, memory_size; int initialized; } OvNvState;
const OvNvState *ov_nv_state(void);

#define RT_GPU_VA_BASE 0x8000000000ull
#define RT_GPU_VA_SIZE (64ull << 30)

void gfx_window_start(void);
void gfx_tl_note(int kind, uint64_t t, uint64_t a, uint64_t b);
void gfx_tl_present(void);
void gfx_vsync_bind(uint64_t system_event);
uint64_t gfx_vsync_ticks(void);
uint64_t gfx_vsync_time_ns(uint64_t tick);
double gfx_window_refresh_hz(void);
uint64_t gfx_window_hosted_ns(void);
int gfx_window_focused(void);
typedef struct { uint32_t present, present_depth, queue, queue_depth, start; } GfxLockHolders;
void gfx_lock_holders(GfxLockHolders *h);
int gfx_window_in_front(void);
int vk_hdr_native(void);
int gfx_picture_rect(float out[4]);
int gfx_surface_hdr10(void);

enum { GFX_STAGE_VS, GFX_STAGE_TCS, GFX_STAGE_TES, GFX_STAGE_GS, GFX_STAGE_FS, GFX_STAGE_CS,
       GFX_STAGE_TS, GFX_STAGE_MS };
enum { GFX_TEX_1D = 1, GFX_TEX_1D_ARRAY, GFX_TEX_BUFFER, GFX_TEX_2D, GFX_TEX_2D_ARRAY, GFX_TEX_2D_MS,
       GFX_TEX_2D_MS_ARRAY, GFX_TEX_3D, GFX_TEX_CUBE, GFX_TEX_CUBE_ARRAY };
enum { GFX_TYPE_FLOAT, GFX_TYPE_SINT, GFX_TYPE_UINT };
typedef struct {
    uint8_t tex_type[32], img_type[32], img_format[32];
    uint8_t attr_type[32], frag_out_type[8];
    uint16_t local[3]; uint32_t shared_bytes;
    uint8_t depth_minus_w;
    uint8_t mvprev;
} GfxShaderAnswers;
typedef struct GfxShader GfxShader;
const GfxShader *gfx_shader_get(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                const uint8_t *c1, const GfxShaderAnswers *a);
const GfxShader *gfx_shader_get_async(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                      const GfxShaderAnswers *a, int *pending);
const GfxShader *gfx_shader_await(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                  const GfxShaderAnswers *a, uint32_t wait_ms, int *pending);
uint64_t gfx_shader_async_done(void);
void gfx_shader_counts(uint64_t *keys, uint64_t *translated);
void gfx_shader_disk_counts(uint64_t *sync, uint64_t *sync_ns, uint64_t *bg);
typedef struct {
    uint64_t shaders, cache, use, pack, records;
    uint64_t spv_held, spv_full, spv_packed;
    uint64_t decodes, decode_ns, draw_decodes, draw_decode_ns, draw_decode_max_ns;
    int compress;
} GfxShaderRam;
void gfx_shader_ram(GfxShaderRam *r);
void gfx_shader_set_host(int float64, uint32_t sbuf_align, const uint32_t subgroup[3], uint32_t gather_bits, int zero_locals,
                         int bda);
const uint8_t *gfx_shader_key(const GfxShader *s);
const uint8_t *gfx_shader_input_id(const GfxShader *s);
int gfx_shader_origin(const GfxShader *s);
const uint8_t *gfx_shader_identity(void);
int gfx_shader_carries(const uint8_t id[32]);
const GfxShader *gfx_shader_load_input(const uint8_t iid[32], int xlate);
void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out, int threads, int procs);
void gfx_shader_carry_start(void);
void gfx_shader_prewarm_progress(int64_t *done, int64_t *total, int64_t *translated);
void gfx_present_boot(void);
void gfx_shader_record_at(const char *keys_path);
void gfx_shader_record_flush(int force);
void gfx_shader_prepare_recorded(int threads, int procs);
int gfx_shader_prepare_seed(const char *dir, int threads, int procs);
int gfx_shader_guess_refine(const GfxShader *s, int stage, const GfxShaderAnswers *a, GfxShaderAnswers *out);
typedef struct { uint8_t iid[32]; uint8_t stage, prio; } GfxWarmEnt;
size_t gfx_shader_warm_list(GfxWarmEnt **out);
const GfxShader *gfx_shader_load_private(const uint8_t iid[32], int *owned);
void gfx_shader_release_private(const GfxShader *s);
const uint32_t *gfx_shader_spirv_pin(const GfxShader *s, uint64_t *words);
void gfx_shader_spirv_unpin(const GfxShader *s);
uint64_t gfx_shader_words(const GfxShader *s);
uint64_t gfx_shader_spirv_fnv(const GfxShader *s);
uint32_t gfx_shader_bindings(const GfxShader *s, const uint32_t **b);
const char *gfx_shader_res_json(const GfxShader *s);
int gfx_shader_write_code(const GfxShader *s, const char *path);
const GfxShader *gfx_shader_own(const uint32_t *spirv, uint64_t words, const char *res_json);
void vk_hdr_shader_swap(uint32_t **spirv, uint64_t *words);
uint64_t gfx_shader_failures(void);
enum { GFX_RES_UBO, GFX_RES_SSBO, GFX_RES_TEX, GFX_RES_IMG, GFX_RES_KINDS };
typedef struct {
    uint16_t set, binding;
    int16_t slot;
    uint16_t type;
    uint8_t format;
    uint8_t flags;
    int8_t cbuf_slot;
    uint16_t handle;
    uint16_t count;
    uint8_t separate;
    uint8_t pool;
    uint16_t ptr;
    uint8_t exact;
} GfxShaderRes;
typedef struct { uint8_t type, comps, packed; } GfxShaderIo;
int gfx_shader_stage(const GfxShader *s);
int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res);
uint32_t gfx_shader_exact_seen(void);
uint32_t gfx_shader_pool_short_seen(void);
uint32_t gfx_shader_slot_mask(const GfxShader *s);
uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]);
uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]);
uint32_t gfx_shader_omap(const GfxShader *s);
uint32_t gfx_shader_mvprev_banks(const GfxShader *s);
int gfx_shader_palette(const GfxShader *s, int *slot, int *storage, uint32_t *joints);
void gfx_shader_local_size(const GfxShader *s, uint32_t local[3]);

typedef struct {
    const char *name;
    int  (*pool_init)(pool_t *p);
    void (*pool_fini)(pool_t *p);
    int  (*pool_map_virtual)(pool_t *v, int n, const void *requests);
    void (*texture_register)(int id, const tex_t *t, const view_t *v);
    void (*sampler_register)(int id, const samp_par *s);
    void (*texture_release)(const tex_t *t);
    void (*program_release)(const prog_t *p);
    uint64_t (*submit)(const uint64_t *handles, int n);
    int  (*fence_wait)(uint64_t value, uint64_t timeout_ns);
    int  (*exec_wait)(uint64_t value, uint64_t timeout_ns);
    int  (*game_wait)(uint64_t value, uint64_t timeout_ns);
    uint64_t (*fence_done)(void);
    void (*present)(const tex_t *t, const int crop[4], uint64_t at_ns);
    void (*frame_start)(void);
    void (*input_read)(void);
    uint64_t (*frame_paced)(uint64_t period_ns);
    void (*finish)(void);
    void (*read_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img);
    void (*write_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img);
    void (*guest_written)(uint64_t addr, uint64_t size);
    void (*guest_read)(uint64_t addr, uint64_t size);
    void (*cpu_synced)(uint64_t fence);
} GfxBackend;
extern const GfxBackend gfx_null_backend, gfx_vulkan_backend;
const GfxBackend *gfx_backend(void);

void nvn_procs_register(void);
uint64_t nvn_proc_address(const char *name);

typedef struct { uint64_t frames, nvn_calls, nvn_distinct, unimplemented, validation_errors, shader_failures,
                 draws_skipped; } GfxStats;
GfxStats gfx_stats(void);
void rt_frame_presented(const tex_t *t, uint64_t n);

void gfx_movie_frame(const uint8_t *rgba, int w, int h);
void gfx_movie_frame_own(uint8_t *rgba, int w, int h);
void gfx_movie_stop(void);

extern _Thread_local uintptr_t gfx_submit_queue;
