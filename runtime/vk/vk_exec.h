// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
#pragma once
#include <stdint.h>

#include "nvn_api.h"
#include "vk.h"

enum {
    VK_EXEC_DIRTY_VIEWPORT = 1 << 0,
    VK_EXEC_DIRTY_SCISSOR = 1 << 1,
    VK_EXEC_DIRTY_DEPTH_BIAS = 1 << 2,
    VK_EXEC_DIRTY_BLEND_CONST = 1 << 3,
    VK_EXEC_DIRTY_STENCIL = 1 << 4,
    VK_EXEC_DIRTY_ALL = (1 << 5) - 1,
};

typedef struct {
    const tex_t *tex;
    VkImage image;
    VkFormat format;
    VkImageViewType view_type;
    VkImageAspectFlags aspect;
    uint32_t level, base_layer, layers;
    uint8_t is3d;
    uint8_t blocks;
    uint32_t width, height, samples;
    int32_t nvn_format;
    const VkTexImage *img;
    uint8_t scale;
} VkExecTarget;

void vk_exec_cond_frame_end(uint64_t frame);
extern unsigned vk_cond_clears_run, vk_cond_clears_skip, vk_cond_draws_skip, vk_cond_depth_run;
void vk_exec_cond_why(const p_render_cond *c, char *buf, size_t cap);

typedef struct VkExec {
    struct DrawCtx *dc;
    VkCommandBuffer cb;
    uint64_t fence;
    int recorded;
    int in_pass;
    uint32_t dirty;
    VkPassMark *mark;
    uint64_t mark_draws0;
    uint32_t mark_passes, mark_dispatches;

    p_set_rt rt;
    int rt_resolved;
    VkExecTarget color[8], depth;
    uint32_t pass_w, pass_h, pass_layers;
    uint32_t pass_scale;
    int pass_noatt;
    VkFormat pass_color[8], pass_depth, pass_stencil;
    uint32_t pass_ncolor, pass_samples;

    float vp[16][6];
    int has_vp;
    int vp_jittered;
    int pass_jit;
    p_scissor scissor;
    int has_scissor;
    float depth_range[2];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    float point_size;
    float line_width;
    p_poly_offset poly_offset;
    p_prim_restart prim_restart;
    float blend_color[4];
    int64_t copy_stride;
    const dpool_t *tex_pool, *samp_pool;
    VkPipeState pipe;

    const prog_t *prog;
    int32_t prog_stages;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[8][18], ssbo[8][16];
    uint8_t cs_push[1024];
    uint64_t tex[8][128], samp[8][128], img[8][32];

    int render_disabled;
    p_render_cond render_cond;
    int has_render_cond;

    void *views;
    int nviews, cap_views;
    int barrier_at;
    int bar_dirty, bar_passes, bar_dispatches;

    VkImage fbl_img[9];
    VkImageAspectFlags fbl_aspect[9];
    int nfbl;
    uint32_t fbl_mask;
    uint32_t fb_dirty;
    uint32_t fb_read;
    const void *fb_fs;
    uint32_t fb_serial, fb_pass, fb_sampled;
    uint32_t tex_serial, pass_serial;
    uint32_t pre_pass;
} VkExec;

void vk_exec_pass_begin(VkExec *ex);
void vk_exec_pass_end(VkExec *ex);
void vk_exec_barrier(VkExec *ex);
void vk_exec_barrier_dispatch(VkExec *ex);
void vk_exec_barrier_log(uint64_t frame);
int vk_exec_split_now(VkExec *ex);
VkCommandBuffer vk_exec_inline_begin(int *reopen);
void vk_exec_inline_end(int reopen);
void vk_exec_probe_inline(VkCommandBuffer cb, uint64_t fence);
void *vk_exec_draw_ctx_new(void);
void vk_exec_draw_ctx_use(void *ctx);
uint32_t vk_exec_draw_group_begin(VkExec *ex);
uint32_t vk_exec_upd_top(uint32_t all);
void vk_exec_xfer_group(int group);
int vk_exec_xfer_front(VkExec *fx, uint32_t op, const void *payload);
uint64_t vk_exec_xfer_chunk_state(void);
void vk_exec_xfer_worker(int on, const uint8_t *cond, uint32_t ncond, uint64_t state, uint32_t slot0, uint32_t nslots,
                         int chunk, int group);
void vk_exec_xfer_group_merge(int group, int nchunks);
int vk_exec_xfer_counting(void);
uint32_t vk_exec_xfer_slots(uint32_t n);
void vk_exec_draw_new_cb(void);
int vk_exec_pool_split(uint64_t fence);
void vk_exec_draw_shaders(const GfxShader **vs, const GfxShader **fs);
void vk_exec_pool_marks(VkExec *ex);
VkBuffer vk_exec_inline_buffer(VkDeviceSize size);
void vk_exec_bc_gone(VkImage image);
extern int vk_hdr_track;
void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info);
VkRect2D vk_exec_scissor(const VkExec *ex);
void vk_exec_feedback_war(VkExec *ex, uint32_t written);
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h);
int vk_exec_targets_recent(VkExec *ex, int any);
extern uint64_t vk_exec_steady_passes;
void vk_exec_ahead(VkExec *ex);
void vk_exec_ahead_begin(void);
void vk_exec_ahead_op(VkExec *sx, uint32_t op, const void *payload);
extern uint64_t vk_exec_ahead_draws, vk_exec_ahead_lookups;
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap);

typedef struct {
    uint64_t passes, draws_seen, draws, clears, copies, dispatches, submits;
    uint64_t fb_draws, fb_barriers, fb_restarts, fb_layouts;
    uint64_t fb_war_barriers;
    uint64_t ub_updates, ub_pass_breaks;
    uint64_t bind_calls, bind_ns;
    uint64_t bind_tex_ns;
    uint64_t bind_buf_ns;
    uint64_t bind_sig_ns;
    uint64_t bind_push_ns;
    uint64_t dyn_calls, dyn_left;
} VkExecFrame;
extern VkExecFrame vk_exec_frame;
void vk_exec_bind_count(int reused);
void vk_exec_frame_written(const tex_t *t);
uint64_t vk_exec_recording_fence(void);
void vk_exec_frame_log(const tex_t *presented, uint64_t frame);
uint32_t vk_exec_pending_draws_take(void);
int vk_frame_hold_left(void);
int vk_exec_pool_loading(void);

extern int vk_dump_armed;
void vk_dump_init(void);
void vk_dump_pass_end(VkExec *ex);
void vk_dump_draw(VkExec *ex, const VkPipeState *s, VkPipeline p, const VkPipeInfo *info, int mesh);
enum { SK_DISABLED, SK_SCISSOR, SK_NO_PROGRAM, SK_NO_VERTEX_STAGE, SK_SHADER_PENDING, SK_SHADER_FAILED, SK_DLAA,
       SK_PIPE_PENDING, SK_PIPE_FAILED, SK_POOL_OFF, SK_PROG_FILTER, SK_SHADER_FILTER, SK_COUNT };
extern const char *const vk_draw_skip_names[SK_COUNT];
void vk_dump_draw_skipped(const VkExec *ex, int mesh, int why);
int vk_dump_mesh_begin(VkExec *ex);
void vk_dump_mesh_end(VkExec *ex, int q);
void vk_dump_dispatch(VkExec *ex, const GfxShader *cs, VkPipeline p, const VkPipeInfo *info, const p_dispatch *d);
void vk_dump_copy(VkExec *ex, uint32_t op, const void *payload);
void vk_dump_present(uint64_t frame);

#define VK_C0_BYTES 0x800u
void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance);

void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload);
void vk_exec_phase_worker(void);
void vk_exec_phases_frame(uint64_t frame);
int vk_exec_phases_on(void);
void vk_exec_phase_op(uint32_t op, uint64_t ns);
void vk_exec_phase_record(uint64_t ns);
enum { SEG_PROLOGUE, SEG_WALK, SEG_DRAIN, SEG_MIRROR_END, SEG_TAIL,
       SEG_PINS, SEG_EVSYNC, SEG_MIRROR_BEGIN, SEG_USNAP_BEGIN,
       SEG_PEEK, SEG_GBEGIN, SEG_FRONT, SEG_HANDOUT, SEG_OWN, SEG_N };
void vk_exec_phase_seg(int seg, uint64_t ns);
VkImageAspectFlags vk_exec_view_aspect(const VkTexImage *img, const view_t *v, int hv);
VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat format, VkImageViewType type, uint32_t aspect,
                                uint32_t level, uint32_t base, uint32_t layers, int blocks);
void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload);
int vk_exec_wait_event(const void *payload);
int vk_exec_event_met(const void *payload);
int vk_exec_event_met_now(const void *payload);
extern int vk_exec_signalled;
void vk_exec_event_cpu(const void *event);
void vk_exec_event_sync(int begin);
int vk_exec_event_cpu_known(const void *event);
int vk_exec_event_holds_now(const void *payload);
void vk_exec_gate_wait(const uint64_t *handles, int n);
int vk_exec_event_at_sync(void);
uint64_t vk_exec_event_late_take(void);
void vk_exec_event_stats(uint64_t *waits, uint64_t *unmet, uint64_t *late, uint64_t *late_subs);
extern uint64_t vk_exec_submit_seq;
static inline uint64_t vk_exec_submit_seq_now(void) { return __atomic_load_n(&vk_exec_submit_seq, __ATOMIC_RELAXED); }
extern uint32_t vk_exec_brk;
uint32_t vk_exec_brk_now(void);
int vk_exec_mesh_run(const void *cur, const void **out, int max);
void vk_exec_mesh_run_stops(char *out, size_t n);
void vk_exec_evt(char what, const void *event, uint32_t value);
void vk_exec_evt_frame(uint64_t frame);
typedef struct { uint64_t pools, spare, sets_each, tex_sets; } VkDescMem;
void vk_exec_desc_mem(VkDescMem *d);
int vk_exec_held(void);
int vk_exec_held_ready(void);
extern const VkTexImage *vk_dlaa_swap_img;
extern VkImageView vk_dlaa_swap_view;
enum { VK_AA_GAME, VK_AA_OFF, VK_AA_FXAA, VK_AA_SMAA };
enum { VK_AA_ROLE_NONE, VK_AA_ROLE_EDGES, VK_AA_ROLE_WEIGHTS, VK_AA_ROLE_COMPOSE };
int vk_aa_mode(void);
int vk_aa_role(uint64_t guest_hash);
int vk_aa_skip_ok(void);
int vk_aa_record(VkCommandBuffer cb, int mode, VkImageView src, VkImageView dst, uint32_t w, uint32_t h);
void vk_aa_composed(int ok);
void vk_contact_pass_begin(VkExec *ex);
void vk_contact_flush(VkExec *ex);
int vk_contact_pending(void);
void vk_exec_oq_draw(VkExec *ex);
void vk_exec_oq_pass_end(VkExec *ex);
