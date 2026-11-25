#pragma once
#include <vulkan/vulkan.h>
#include <stdint.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop
VkResult vk_vma_create_image(VmaAllocator vma, const VkImageCreateInfo *ici, const VmaAllocationCreateInfo *ai, VkImage *img,
                             VmaAllocation *a, VmaAllocationInfo *info, const char *file);
VkResult vk_vma_create_buffer(VmaAllocator vma, const VkBufferCreateInfo *bci, const VmaAllocationCreateInfo *ai,
                              VkBuffer *buf, VmaAllocation *a, VmaAllocationInfo *info, const char *file);
VkResult vk_vma_allocate_memory(VmaAllocator vma, const VkMemoryRequirements *req, const VmaAllocationCreateInfo *ai,
                                VmaAllocation *a, VmaAllocationInfo *info, const char *file);
void vk_vma_destroy_image(VmaAllocator vma, VkImage img, VmaAllocation a);
void vk_vma_destroy_buffer(VmaAllocator vma, VkBuffer buf, VmaAllocation a);
void vk_vma_free_memory(VmaAllocator vma, VmaAllocation a);
uint64_t vk_vma_moved(void);
enum { VK_OWN_TEXTURES, VK_OWN_POOLS, VK_OWN_UPSCALER, VK_OWN_REPLAY, VK_OWN_USNAP, VK_OWN_GPA, VK_OWN_ASTC,
       VK_OWN_PRESENT, VK_OWN_HEADROOM, VK_OWN_OTHER, VK_OWN_N };
uint64_t vk_vma_owner_bytes(int owner, int local);
#ifndef VK_VMA_BUDGET_IMPL
#define vmaCreateImage(v, ici, ai, img, a, info) vk_vma_create_image(v, ici, ai, img, a, info, __FILE__)
#define vmaCreateBuffer(v, bci, ai, buf, a, info) vk_vma_create_buffer(v, bci, ai, buf, a, info, __FILE__)
#define vmaAllocateMemory(v, req, ai, a, info) vk_vma_allocate_memory(v, req, ai, a, info, __FILE__)
#define vmaDestroyImage vk_vma_destroy_image
#define vmaDestroyBuffer vk_vma_destroy_buffer
#define vmaFreeMemory vk_vma_free_memory
#endif
#include "nvn_obj.h"
#include "gfx.h"

typedef struct VkCtx {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem_props;
    uint32_t gfx_family;
    uint32_t xfer_family;
    VkQueue gfx_queue;
    VkQueue xfer_queue;
    int xfer_shared;
    VmaAllocator vma;
    VkSemaphore timeline;
    VkDebugUtilsMessengerEXT messenger;
    volatile int32_t validation_errors;
    int validation;
    int has_custom_border_color, has_eds, has_eds2, has_eds3, has_vertex_input_dynamic_state,
        has_conditional_rendering, has_transform_feedback, has_provoking_vertex, has_device_fault, has_memory_budget,
        has_memory_priority, has_pageable,
        has_robustness2, has_buffer_device_address, has_fragment_shader_interlock, has_buffer_marker,
        has_nv_checkpoints,
        has_astc_ldr,
        has_calibrated,
        has_hdr_metadata,
        has_mesh_shader, has_vrs,
        has_maximal_reconvergence,
        has_pipeline_binary,
        has_low_latency2,
        has_present_id,
        has_anti_lag,
        lists_low_latency,
        has_mesh_queries;
    uint32_t low_latency2_rev;
    int mesh_fallback;
    int mesh_fallback_default;
    int mesh_emulation;
    int feedback;
    uint32_t min_imported_host_alignment;
    int device_index;
} VkCtx;
enum { VK_FEEDBACK_OFF, VK_FEEDBACK_RESTART, VK_FEEDBACK_LAYOUT };

typedef struct {
    int float64;
    uint32_t sbuf_align;
    uint32_t sg_size;
    VkShaderStageFlags sg_pin_stages;
    int gpl;
    int amd;
    int amd_sc;
    int amd_llpc;
    int mesa;
    uint32_t driver_id;
} VkCaps;
extern VkCaps vk_caps;
void vk_caps_driver(VkPhysicalDevice p);
const char *vk_caps_summary(void);
void vk_caps_features(VkPhysicalDeviceFeatures *f);
int vk_caps_gpl(const VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT *f,
                const VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT *p);
void vk_caps_init(VkCtx *c);

VkCtx *vk_ctx(void);
const char *vk_card_prepare_text(void);
int vk_card_usable(VkPhysicalDevice pd);
int vk_init(void);
void vk_shutdown(void);
int vk_selftest(void);
void vk_device_lost_check(VkResult r, const char *where);
extern volatile int32_t vk_lost;
static inline int vk_device_is_lost(void) { return __atomic_load_n(&vk_lost, __ATOMIC_RELAXED); }
void vk_device_lost_poll(void);
void vk_sync_signal_note(uint64_t value);
void vk_sync_check_timeline(const char *where);
void vk_sync_state_text(char *o, size_t n);
void vk_sync_lost_report(void);

enum { VK_CRUMB_SUBMIT = 1, VK_CRUMB_SUBMIT_END, VK_CRUMB_UPLOAD, VK_CRUMB_UPLOAD_END, VK_CRUMB_ASTC, VK_CRUMB_PRESENT,
       VK_CRUMB_PRESENT_END };
void vk_crumbs_init(VkCtx *c);
void vk_crumbs_release(void);
void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n);
void vk_crumbs_report(void);

#define VK_PASS_MARKS 4096
enum { VK_MARK_PASS = 1, VK_MARK_DISPATCH };
typedef struct {
    uint32_t id;
    uint8_t kind;
    uint8_t ncolor;
    uint32_t step;
    uint32_t w, h, d;
    uint32_t draws;
    uint32_t vw, vh;
    uint64_t fence;
    VkFormat color[8], depth;
    const GfxShader *first[2], *last[2];
    uint32_t gp;
    uint32_t layer;
    uint32_t gap;
    uint16_t mesh, mesh_ind;
} VkPassMark;
void vk_gpu_passes_frame(uint64_t frame);
enum {
    VK_GAP_COPY_B2B, VK_GAP_COPY_B2T, VK_GAP_COPY_T2B, VK_GAP_COPY_T2T, VK_GAP_CLEAR_BUF, VK_GAP_CLEAR_TEX,
    VK_GAP_CLEAR_TARGET, VK_GAP_UPSCALER, VK_GAP_WB_MERGE, VK_GAP_WB_COPY, VK_GAP_TWIN, VK_GAP_POOL_MARKS,
    VK_GAP_MESH_RING, VK_GAP_BC_BACK,
    VK_GAP_GROUP,
    VK_GAP_N
};
void vk_gpu_passes_part(VkCommandBuffer cb, int end);
void vk_gpu_passes_fence(uint64_t fence);
void vk_gpu_passes_result(int kinds);
void vk_gpu_passes_waited(uint64_t need);
int vk_sync_result_mask(void);
void vk_gpu_gap_note(int kind, uint64_t units);
extern int vk_pass_marks_on;
int vk_pass_marks_want_nv(void);
VkPassMark *vk_pass_mark_begin(VkCommandBuffer cb, int kind, uint64_t fence);
void vk_pass_mark_end(VkCommandBuffer cb, const VkPassMark *m);
void vk_pass_mark_cmd(VkCommandBuffer cb, uint32_t id, int end);
int vk_pass_marks_read(uint32_t *started, uint32_t *finished);
const VkPassMark *vk_pass_mark_record(uint32_t id);
void vk_pass_marks_format(const char *source, uint32_t started, uint32_t finished, char *o, size_t size);
void vk_pass_marks_report(void);
extern int vk_draw_marks_on;
void vk_draw_mark(VkCommandBuffer cb, const VkPassMark *m, const GfxShader *vs, const GfxShader *fs, uint32_t draw);

void vk_present_image(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]);
void vk_present_blit(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]);
int vk_fsr_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, int sw, int sh, const int crop[4],
                  VkImage dst, const int out[4]);
void vk_fsr_release(void);
VkImage vk_color_record(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]);
void vk_color_release(void);
const char *const *vk_color_preset(const char *name);
int vk_hdr_armed(void);
int vk_hdr_wanted(void);
int vk_hdr_needs_restart(void);
int vk_hdr_restart_for(const char *value);
void vk_hdr_off(void);
int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out);
void vk_hdr_metadata(VkSwapchainKHR sc, int new_sc);
void vk_hdr_set_active(int on);
int vk_hdr_active(void);
VkImage vk_hdr_encode(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]);
int vk_hdr_native_picture(VkFormat f);
VkImageView vk_hdr_ui_target(VkCommandBuffer cb, uint32_t w, uint32_t h);
void vk_hdr_ui_compose(VkCommandBuffer cb, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h);
void vk_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r);
void vk_hdr_luminance_stats(const uint8_t *in, int stride, const int rect[4], int a2r, float out[7]);
void vk_hdr_release(void);
void vk_hdr_images_release(void);
void vk_hdr_support(float out[20]);
int vk_contact_strength(void);
void vk_contact_release(void);
void vk_present_flip(void);
void vk_present_swapchain_mem(uint32_t *n, uint32_t *w, uint32_t *h, uint64_t *bytes);
uint64_t vk_ffx_vram(int *contexts, uint64_t *fg);
void vk_queue_lock(void);
void vk_queue_unlock(void);
void vk_queue_lock_raw(void);
int vk_queue_trylock_raw(void);
void vk_sync_flush_held(void);
void gfx_fit_rect(int cw, int ch, int dw, int dh, int out[4]);
void vk_present_capture(int on);
typedef struct {
    uint8_t *px;
    int w, h;
    int bgra, hdr, a2r;
    int image_w, image_h;
    int scaler;
    float pic[4];
} VkGrab;
int vk_present_grab(void (*cb)(VkGrab *g, void *ctx), void *ctx);
void vk_present_grab_cancel(void);
void vk_present_grab_rgba(const VkGrab *g, uint8_t *out);
void vk_present_grab_free(VkGrab *g);
int vk_present_captured(const uint8_t **px, int *w, int *h, VkFormat *fmt);

int gfx_window_vk_extensions(const char **out, int max);
int gfx_window_vk_presentable(VkInstance inst, VkPhysicalDevice pd, uint32_t family);
void gfx_window_resize(int client_w, int client_h);
void gfx_vsync_bind_counter(volatile int32_t *counter);
unsigned gfx_window_focus_messages(void);
void gfx_window_release_vk(void);
long vk_validation_errors_total(void);

int  vk_pool_init(pool_t *p);
void vk_pool_fini(pool_t *p);
int  vk_pool_map_virtual(pool_t *v, int n, const void *requests);
void vk_texture_register(int id, const tex_t *t, const view_t *v);
void vk_sampler_register(int id, const samp_par *s);
void vk_reg_replay_begin(void);
void vk_reg_replay_end(uint64_t fence);
void vk_texture_release(const tex_t *t);
void vk_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img);
void vk_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img);
void vk_guest_written(uint64_t addr, uint64_t size);

typedef struct {
    VkBuffer buffer;
    VkDeviceSize offset, avail;
    VmaAllocation alloc;
    VkDeviceSize alloc_offset;
    int cls;
    void *pool;
} VkMemSpan;
enum { VK_MCLS_NONE, VK_MCLS_HOST_NOACC, VK_MCLS_HOST_UNC, VK_MCLS_HOST_CACHED, VK_MCLS_DEVICE, VK_MCLS_SCRATCH, VK_MCLS_N };
enum { VK_MUSE_VB, VK_MUSE_IB, VK_MUSE_UBO, VK_MUSE_SSBO, VK_MUSE_IND, VK_MUSE_COPY_SRC, VK_MUSE_GPU_WRITE,
       VK_MUSE_CPU_READ, VK_MUSE_N };
void vk_mem_count(int cls, int use, uint64_t bytes);
void vk_mem_bound_note(void *pool);

extern PFN_vkGetDeviceProcAddr vk_dpa;

uint64_t vk_gpu_busy_all(void);
#define VK_MEM_BOUND(sp) do { if ((rt_trace & RT_TRACE_PERF) && (sp).pool) vk_mem_bound_note((sp).pool); } while (0)
void vk_mem_dirty_submit(void);
extern int vk_mirror_on;
void vk_mem_use(VkMemSpan *sp);
void vk_mem_pick(VkMemSpan *sp);
VkBuffer vk_mem_guest_twin(const VkMemSpan *sp);
#define VK_MEM_USE(sp) do { if (vk_mirror_on && (sp).pool) vk_mem_use(&(sp)); } while (0)
#define VK_MEM_PICK(sp) do { if (vk_mirror_on && (sp).pool) vk_mem_pick(&(sp)); } while (0)
void vk_mem_gpu_wrote(const VkMemSpan *sp, uint64_t size);
void vk_mem_gpu_wrote_shader(const VkMemSpan *sp, uint64_t size);
void vk_usnap_begin(const uint64_t *handles, int n, uint64_t fence);
void vk_usnap_take(const uint64_t *handles, int n);
void vk_usnap_end(uint64_t fence);
int vk_usnap_find(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out);
void vk_exec_usnap_hit(void);
void vk_usnap_hits_add(uint64_t n);
const uint8_t *vk_usnap_map(void);
const uint8_t *vk_usnap_bytes(uint64_t addr, uint64_t n);
void vk_usnap_drop(uint64_t addr, uint64_t n);
uint64_t vk_usnap_gen(void);
int vk_usnap_read(uint64_t addr, uint64_t n, void *dst);
enum { VK_USNAP_FOUND, VK_USNAP_NO_SNAPSHOT, VK_USNAP_DROPPED, VK_USNAP_IN_FLIGHT, VK_USNAP_NOT_CPU, VK_USNAP_NOT_BOUND };
int vk_usnap_why(uint64_t addr, uint64_t n);
void vk_usnap_shutdown(void);
enum { MVPREV_NONE, MVPREV_SELF, MVPREV_SIDE, MVPREV_NEARER };
uint64_t vk_mvprev_step(int stage, int index, uint64_t size);
int vk_mvprev_on(void);
void vk_mvprev_want(uint32_t index_mask);
int vk_mvprev_find(int index, uint64_t addr, uint64_t size, uint64_t *prev, int *how);
void vk_mvprev_frame(void);
void vk_mem_mirror_begin(void);
int vk_mem_mirror_end(VkCommandBuffer cb, uint64_t fence);
int vk_mem_mirror_pending(void);
int vk_mem_gpa_late(void);
void vk_mem_mirror_prologue(VkCommandBuffer cb);
void vk_mem_note_live(void);
int vk_mem_take_live(void);
enum { VK_RES_EVENT = 1, VK_RES_COUNTER = 2, VK_RES_OQ = 4, VK_RES_WRITE = 8, VK_RES_WRITEBACK = 16, VK_RES_KINDS = 5 };
void vk_exec_note_results(int kind);
struct VkExec;
void vk_exec_tail_begin(struct VkExec *ex);
int vk_exec_tail(void);
void vk_exec_tail_ub(uint64_t fence, uint64_t addr, const void *data, uint32_t size);
int vk_sync_tail_ok(void);
VkCommandBuffer vk_sync_tail_hold(void);
VkCommandBuffer vk_sync_tail_release(void);
int vk_sync_game_wait(uint64_t value, uint64_t timeout_ns);
uint64_t vk_sync_tail_pending_ns(uint64_t value);
int vk_mem_live_swap(int v);
uint64_t vk_mem_gpu_wrote_count(void);
int vk_mem_wb_lazy(void);
int vk_exec_take_results(void);
void vk_mem_lazy_part_end(void);
void vk_mem_lazy_handed(void);
void vk_mem_lazy_decide(void);
void vk_mem_fatal_note(char *out, size_t n);
uint64_t vk_sync_rec_fence(void);
void vk_sync_release_held(uint64_t value);
void vk_mem_merge_cpu_wait(uint64_t value);
void vk_mem_shadow_sync(uint64_t fence);
void vk_mem_shadow_table_read(void);
int vk_mem_shadow_storage(void);
int vk_mem_shadow_table_stores(void);
void vk_mem_gpu_pending(uint64_t addr, uint64_t size);
void vk_gpu_written(uint64_t addr, uint64_t size);
void vk_gpu_written_back(uint64_t addr, uint64_t size);
int vk_mem_mirrored(uint64_t addr, uint64_t size);
int vk_mem_flush_add_probe(void);
uint64_t vk_mem_mirror_uploaded(void);
int vk_mem_mirror_probe(uint64_t addr);
void vk_mem_dmabuf_caps(int exts);
int vk_mem_dmabuf_on(void);
void vk_mem_dmabuf_probe(VkDevice dev, const VkPhysicalDeviceMemoryProperties *mp, int bda);
void vk_mem_class_log(const VkPhysicalDeviceProperties *p, const VkPhysicalDeviceMemoryProperties *m);
int vk_mem_class_of(uint64_t addr);
void vk_mem_census(uint64_t frame);
void vk_mem_lost_report(void);
float vk_mem_device_used_mb(void);
void vk_mem_stage_log(const char *stage);
void vk_mem_rank_log(const char *when);
void vk_mem_exit_log(void);
int vk_mem_vram(uint64_t *used, uint64_t *budget, int *exact);
const VmaDeviceMemoryCallbacks *vk_headroom_callbacks(void);
void vk_headroom_start(void);
void vk_headroom_stop(void);
uint64_t vk_headroom_held(void);
uint64_t vk_headroom_release(void);
uint64_t vk_mem_give_back(void);
void vk_mem_ballast(VmaAllocator vma);
void vk_mem_ballast_release(VmaAllocator vma);
VkDeviceSize vk_vma_block_size(void);
int vk_mem_resolve(uint64_t addr, uint64_t size, VkMemSpan *out);
int vk_mem_lookup(uint64_t addr, uint64_t size, VkMemSpan *out);
uint64_t vk_mem_gen(void);
int vk_mem_changed(uint64_t addr, uint64_t size, uint64_t since, uint64_t *now);
int vk_mem_changed_snap(uint64_t addr, uint64_t size, uint64_t since, uint32_t epoch, uint64_t *now);
void vk_mem_snap_stats(uint64_t *checks, uint64_t *calls, uint64_t *untracked);

int vk_fix_zero(void);
VkCommandBuffer vk_upload_begin(void);
void vk_upload_end(void);
void *vk_staging(VkDeviceSize size, VkBuffer *buf, VmaAllocation *alloc);
void *vk_staging_at(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off);
uint64_t vk_upload_flush(int wait);
enum { VK_TS_CREATE, VK_TS_UPLOAD, VK_TS_STAGE, VK_TS_STG_NEW, VK_TS_COPY, VK_TS_ASTC, VK_TS_SCRATCH, VK_TS_DEVMEM,
       VK_TS_FLUSH, VK_TS_N };
void vk_texstat_time(int kind, uint64_t t0);
void vk_texstat_add(int kind, uint64_t n, uint64_t bytes);
void vk_defer_image(VkImage img, VmaAllocation alloc);
void vk_defer_view(VkImageView view);
void vk_defer_free(void *p);
int vk_exec_image_gone(VkImage image);
void vk_defer_buffer(VkBuffer buf, VmaAllocation alloc);
int vk_defer_buffer_try(VkBuffer buf, VmaAllocation alloc);
enum { VK_ASTC_CPU, VK_ASTC_GPU, VK_ASTC_NATIVE, VK_ASTC_CHECK };
int vk_astc_mode(void);
int vk_astc_native(VkFormat astc_fmt);
int vk_astc_gpu(void);
int vk_astc_bc(void);
int vk_astc_bc1(void);
typedef struct { VkDeviceSize src; uint32_t w, h, slices, level, layer; VkOffset3D offset; uint8_t bl, th, td; } VkAstcRegion;
int vk_astc_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_size, const VkAstcRegion *rg, int n, uint32_t bw,
                   uint32_t bh, int srgb, int bc, VkImage img, VkImageAspectFlags aspect, VkBuffer readback);
void vk_astc_scratch_fail(int n);
uint64_t vk_texture_upload_retries(void);
void vk_texture_split_fill_set(int mode);
void vk_texture_split_counts(uint64_t *copies, uint64_t *fills);
VkDeviceSize vk_astc_region_bytes(const VkAstcRegion *r, uint32_t bw, uint32_t bh);
void vk_astc_stats(uint64_t *dispatches, uint64_t *texels);
uint64_t vk_astc_scratch_bytes(void);
void vk_astc_scratch_trim(void);
void vk_astc_time_enable(int on);
int vk_astc_time_get(uint32_t bw, uint32_t bh, uint64_t *texels, uint64_t *calls, double *us);
void vk_astc_time_summary(void);
void vk_astc_release(void);
void vk_mem_note_submit(uint64_t timeline_value);
void vk_mem_oom_report(const char *what, VkResult r);
void vk_mem_collect(void);
void vk_mem_collect_try(void);
typedef struct { uint64_t imported_pools, physical_pools, virtual_pools, import_failures, upload_batches; } VkMemStats;
void vk_mem_stats(VkMemStats *s);
void vk_mem_shutdown(void);

typedef struct {
    VkImage image;
    VkFormat format;
    VkImageType type;
    VkImageViewType view_type;
    VkImageAspectFlags aspect;
    VkImageUsageFlags usage;
    uint32_t width, height, depth, levels, layers, samples;
    uint64_t addr;
    uint8_t alpha_one;
    uint8_t chan[4];
    uint8_t mirrored, gpu_owned;
    uint8_t bound;
    uint8_t block_views;
    uint8_t swz[4];
    uint8_t fb_usage;
    uint8_t fb_learned;
    uint8_t scale;
    uint8_t skip;
    uint8_t gone;
} VkTexImage;
const VkTexImage *vk_texture_flat3d_locked(const VkTexImage *img, uint32_t layer);
void vk_hdr_present_source(const VkTexImage *win, VkImage *image, VkFormat *format);
void vk_hdr_image_gone(VkImage image);
const VkTexImage *vk_texture_get(const tex_t *t);
const VkTexImage *vk_texture_by_id(int id, view_t *view, int *has_view);
void vk_texture_lock(void);
void vk_texture_unlock(void);
const VkTexImage *vk_texture_by_id_locked(int id, view_t *view, int *has_view);
int vk_texture_deferred(void);
int vk_texture_deferred_take(void);
void vk_texture_deferred_stats(uint64_t out[3]);
int vk_exec_turn_due(void);
int vk_exec_in_chunk(void);
int vk_exec_shared(void);
int vk_exec_behind(void);
void vk_exec_par(void (*fn)(void *arg, int lo, int hi), void *arg, int n, int grain);
void vk_exec_hooks_wait(void);
void vk_exec_hook_turn(void);
uint64_t vk_exec_stream_pos(void);
uint64_t vk_exec_epoch(int write);
void vk_exec_turn(void);
VkSampler vk_sampler_by_id_locked(int id);
VkSampler vk_sampler_point_by_id_locked(int id);
VkSampler vk_sampler_compare_by_id_locked(int id);
int vk_texture_detail_auto(void);
const char *vk_texture_detail_name(void);
VkSampler vk_sampler_detail_by_id_locked(int id, uint32_t skip);
const VkTexImage *vk_texture_exact_by_id_locked(int id, view_t *view, int *has_view);
uint32_t vk_texture_slots_gen(void);
int vk_texture_slot_count_locked(void);
void vk_texture_create_budget_locked(int n);
uint32_t vk_texture_slot_why_locked(int id, uint64_t *addr, uint32_t *format, uint32_t *w, uint32_t *h);
void vk_texture_pool_shown_locked(int shown);
uint64_t vk_texture_pool_make_ns_take(void);
int vk_texture_create_deferred_locked(void);
int vk_sampler_slot_count_locked(void);
#define VK_POOL_TEX 40960
#define VK_POOL_TEX_CARRIED 32768
#define VK_POOL_SAMP 4096
#define VK_POOL_KINDS 16
extern volatile uint32_t vk_pool_kinds_used;
#define VK_POOL_MARK_BINDING (VK_POOL_KINDS + 1)
#define VK_POOL_MARK_OVERFLOW (VK_POOL_TEX + 1)
int vk_pool_marks_enabled(void);
extern volatile int32_t vk_pool_marks_missed;
void vk_pool_marks_read(const uint32_t *marks, uint32_t n);
#define VK_POOL_SETS 8
uint64_t vk_pool_takes(void);
void vk_texture_pool_ctx(int on);
VkDescriptorSetLayout vk_pool_set_layout(void);
VkDescriptorSetLayout vk_empty_set_layout(void);
VkImage vk_tonemap_record(VkCommandBuffer cb, VkImage src, VkFormat sf, uint32_t sw, uint32_t sh, uint32_t dw,
                          uint32_t dh, int edge);
void vk_tonemap_forget(VkImage src);
uint64_t vk_texture_slot_token(int id);
uint64_t vk_texture_same_regs(void);
void vk_texture_bucket_stats(char *out, size_t n);
uint64_t vk_texture_slot_khash_locked(int id);
uint32_t vk_texture_entry_gen(void);
int vk_texture_dirty_slots_locked(uint64_t *pos, void (*fn)(int id, void *arg), void (*gfn)(int g, void *arg), void *arg);
typedef struct { uint64_t kh; int32_t head, n; uint32_t stamp, fmask; } VkSlotGroup;
VkSlotGroup *vk_texture_group_locked(int g);
int vk_texture_group_next_locked(int id);
void vk_texture_groups_unstamp_locked(void);
uint64_t vk_texture_dirty_pos_locked(void);
int vk_texture_pool_note_locked(int id, uint32_t *ent);
int vk_texture_pool_fresh_locked(void);
void vk_texture_pool_hold_locked(uint32_t old, uint32_t ent);
int vk_texture_slot_retry_locked(int id);
void vk_mem_gen_lock_shared(void);
void vk_mem_gen_unlock_shared(void);
int vk_mem_unchanged_locked(uint64_t addr, uint64_t size, uint64_t since, uint32_t epoch);
int vk_mem_unchanged_why_locked(uint64_t addr, uint64_t size, uint64_t since, uint32_t epoch);
void vk_mem_gen_lock(void);
void vk_mem_gen_unlock(void);
uint64_t vk_mem_snap_range_locked(uint64_t addr, uint64_t size, uint32_t epoch);
void vk_mem_snap_note_locked(uint64_t addr, uint64_t size, uint32_t epoch);
uint64_t vk_mem_snap_noted_locked(uint32_t epoch, int *left);
void vk_sampler_dlss_bias(float bias);
void vk_sampler_aniso_live(void);
uint32_t vk_sampler_gen(void);
void vk_texture_gpu_written(const tex_t *t);
const VkTexImage *vk_texture_at_locked(const VkTexImage *img, VkFormat format);
void vk_texture_storage_bound_locked(const VkTexImage *img);
uint64_t vk_texture_last_write(const tex_t *t);
size_t vk_texture_texel_span(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img);
int vk_texture_small_dirty(void);
void vk_texture_write_back_small(uint64_t fence);
void vk_texture_debug(const VkTexImage *img, uint32_t *uploads, int *last_change, int *pages_written);
int vk_texture_slot_swz(int id, uint8_t swz[4]);
int vk_texture_read_image(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                          uint64_t img);
int vk_texture_cpu_stale(uint64_t addr, uint64_t size);
uint64_t vk_texture_dirty_fence(uint64_t addr, uint64_t size);
void vk_texture_write_back(uint64_t addr, uint64_t size);
void vk_texture_range_gone(uint64_t lo, uint64_t size, VmaAllocation alloc);
void vk_texture_stats(uint64_t *images, uint64_t *uploads, uint64_t *reuploads);
typedef struct {
    uint64_t live, released, own_live, own_released, bytes_live, bytes_released;
    uint64_t bytes_live_gpu, bytes_live_idle, bytes_released_young;
} VkTexCensus;
void vk_texture_census(VkTexCensus *s);
uint64_t *vk_texture_cpu_ranges(int *n);
int vk_texture_host_room(uint64_t bytes);
void vk_mem_tex_pools(uint64_t frame);
typedef struct { uint64_t entries, bytes, tmp, tmp_peak; } VkTexHost;
void vk_texture_host(VkTexHost *h);
void vk_texture_census_log(uint64_t frame);
void vk_texture_reuse_log(uint64_t frame);
typedef struct { uint64_t mem, block_size, bytes; uint32_t type; } VkBlockRow;
#define VK_HOLES_KINDS 4
typedef struct {
    uint32_t type, marks, blocks, sparse, emptiable;
    uint64_t block_bytes, tex_bytes, other_bytes, move_bytes, drop_bytes;
    uint64_t sparse_bytes, sparse_listed, emptiable_bytes, emptiable_move, emptiable_drop;
    uint32_t kept, kept_with[VK_HOLES_KINDS], kept_only[VK_HOLES_KINDS];
    uint64_t kept_bytes, kept_only_bytes[VK_HOLES_KINDS];
} VkTexHoles;
void vk_texture_holes(const VkBlockRow *other, int nother, VkTexHoles *h);
int vk_texture_compact_on(void);
int vk_texture_compact_due(void);
void vk_texture_compact_plan(const VkBlockRow *other, int nother);
void vk_texture_block_freed(uint64_t mem);
void vk_texture_want_room(uint64_t bytes);
int vk_texture_commit_low(void);
int vk_mem_vram_live(uint64_t *used, uint64_t *budget);
void vk_texture_shutdown(void);
void vk_texture_new_submit(void);
uint32_t vk_texture_epoch(void);
VkSampler vk_sampler_by_id(int id);
VkSampler vk_sampler_for(const samp_par *s);
int vk_sampler_params(int id, samp_par *out);

void rt_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);

void vk_sync_start(void);
uint64_t vk_sync_submit(const uint64_t *handles, int n);
uint64_t vk_sync_submit_own(void (*record)(VkCommandBuffer, void *), void *arg);
int vk_sync_try_locked(void (*fn)(void));
void vk_sync_locked(void (*fn)(void));
void vk_present_frames_owed(void);
uint64_t vk_sync_reserve(void);
int vk_sync_fence_wait(uint64_t value, uint64_t timeout_ns);
int vk_sync_exec_wait(uint64_t value, uint64_t timeout_ns);
int vk_sync_recorded_wait(uint64_t value, uint64_t timeout_ns, int (*gpu_wait)(uint64_t, uint64_t));
int vk_sync_wb_split(void);
void vk_sync_exec_stats(uint64_t *waits, uint64_t *early, uint64_t *lag_ns, uint64_t *lag_max_ns);
int vk_sync_effect_late(void);
uint64_t vk_sync_cb_value(void);
void vk_sync_cb_stats(char *out, size_t n);
void vk_exec_late_stats(uint64_t *events, uint64_t *counters, uint64_t *counts);
void vk_exec_clear_stats(char *out, size_t n);
VkCommandBuffer vk_sync_split_exec(void);
int vk_mem_mirror_wb_pending(void);
uint64_t vk_sync_fence_done(void);
uint64_t vk_sync_issued(void);
void vk_sync_finish(void);
void vk_sync_shutdown(void);
int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence);
VkCommandBuffer vk_sync_split(void);
int vk_sync_append_room(int n);
VkCommandBuffer vk_sync_append_later(int n, int *at);
void vk_sync_append_fill(int at, const VkCommandBuffer *cbs, int n);
VkCommandBuffer vk_sync_worker_begin(int w);
void vk_sync_worker_end(VkCommandBuffer cb);
VkCommandBuffer vk_sync_pre(void);
void vk_exec_mesh_flush(VkCommandBuffer cb);
VkCommandBuffer vk_sync_cut(void);
enum { GTL_PART, GTL_MIRROR, GTL_PRE, GTL_UPLOAD, GTL_PRESENT, GTL_KINDS };
void vk_gpu_parts_log(uint64_t frame, uint64_t t0, uint64_t t1, double fence_ms);
void vk_gtl_note(int kind, uint64_t sub_ns, uint64_t start_tick, uint64_t end_tick);
void vk_gtl_log(uint64_t frame, int frames);
extern int vk_sub_in_frame;
void vk_sub_in_frame_publish(void);
void vk_completion_add(uint64_t fence, void (*fn)(void *), void *arg);
void vk_completion_add_result(uint64_t fence, void (*fn)(void *), void *arg);
void vk_completion_add_first(uint64_t fence, void (*fn)(void *), void *arg);
void rdoc_init(void);
int rdoc_active(void);
void rdoc_after_present(uint64_t frame);
void vk_present_drain(void);
int vk_replay_async(void);
void vk_program_release(const prog_t *p);
void vk_backend_shutdown(void);

typedef struct VkPipeState {
    const GfxShader *vs, *fs;
    const GfxShader *ts;
    int32_t color_fmt[8];
    int32_t depth_fmt;
    uint8_t samples;
    uint8_t prim;
    uint8_t depth_minus_w;
    uint8_t depth_clamp, raster_discard;
    uint8_t y_flip;
    uint8_t prim_restart;
    uint8_t feedback;
    blend_st blend[8];
    color_st color;
    chmask_st chmask;
    ms_st ms;
    poly_st poly;
    ds_st ds;
    uint32_t vattr[16];
    vs_st vstream[16];
} VkPipeState;
void vk_pipe_state_defaults(VkPipeState *s);
enum { VK_PIPE_SUPPORT = GFX_RES_KINDS };
#define VK_PIPE_ZERO_BINDING 16
typedef struct {
    uint16_t binding;
    uint8_t stage;
    uint8_t kind;
    VkDescriptorType type;
    uint16_t count;
    VkShaderStageFlags stages;
    const GfxShaderRes *res;
    uint8_t set;
} VkPipeBinding;
typedef struct {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set_layout;
    int push;
    VkPipelineBindPoint bind_point;
    uint32_t n_bindings;
    const VkPipeBinding *bindings;
    uint32_t vs_inputs;
    VkDescriptorSetLayout set_layout1;
    int push1;
    uint32_t n_sets;
    VkDescriptorUpdateTemplate tpl[2];
    int pool;
    int emu;
    VkDescriptorUpdateTemplate emu_tpl_c;
    int8_t gpa;
} VkPipeInfo;
typedef union { VkDescriptorImageInfo image; VkDescriptorBufferInfo buffer; VkBufferView view; uint64_t w[3]; } VkDescSlot;
VkPipeline vk_pipeline_graphics(const VkPipeState *s, const VkPipeInfo **info);
VkPipeline vk_pipeline_graphics_async(const VkPipeState *s, const VkPipeInfo **info, int *pending, uint32_t wait_ms);
int vk_pipeline_graphics_key_matches(const VkPipeState *s, const VkPipeInfo *info);
double vk_pipeline_building_ms(const VkPipeState *s);
int vk_pipeline_hint(const VkPipeState *s);
int vk_pipeline_mesh_libs(void);
void vk_pipeline_async_enable(void);
int vk_pipeline_async_on(void);
void vk_pipeline_prewarm(void);
void vk_pipeline_prewarm_marker_clear(void);
void vk_pipeline_prewarm_detach(void);
void vk_pipeline_frame_note(uint32_t draws);
typedef struct {
    int phase;
    int64_t sh_done, sh_total, sh_translated;
    int64_t pipe_done, pipe_total;
} VkPrewarmProgress;
void vk_pipeline_prewarm_progress(VkPrewarmProgress *p);
VkPipeline vk_pipeline_compute(const GfxShader *cs, const VkPipeInfo **info);
struct MeshEmuInfo;
int vk_pipeline_mesh_emu(const GfxShader *ms, const GfxShader *ts, VkPipelineLayout layout, VkPipeline *mesh,
                         VkPipeline *task, struct MeshEmuInfo *mi);
void vk_pipeline_set_dynamic(VkCommandBuffer cb, const VkPipeState *s);
void vk_pipeline_line_width(VkCommandBuffer cb, const VkPipeState *s, float width);
typedef struct {
    uint32_t n_attr, n_bind;
    VkVertexInputAttributeDescription2EXT a[32];
    VkVertexInputBindingDescription2EXT b[17];
} VkVtxInput;
enum { VK_DYN_CORE = 1, VK_DYN_VTX = 2, VK_DYN_BLEND = 4 };
typedef struct {
    uint32_t have;
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topo;
    VkBool32 dt, dw, st, bias_en, discard, restart, clamp, a2c;
    VkCompareOp dop, scmp[2];
    VkStencilOp sop[2][3];
    VkPolygonMode poly;
    uint32_t nt;
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
    VkVtxInput vi;
} VkDynShadow;
int vk_pipeline_set_dynamic_delta(VkCommandBuffer cb, const VkPipeState *s, VkDynShadow *sh, int mesh, uint64_t *skipped);
int vk_pipeline_vertex_input_dynamic(void);
void vk_pipeline_answers(const VkPipeState *s, GfxShaderAnswers *vs, GfxShaderAnswers *fs);
int vk_pipeline_tex_type(int nvn_target);
uint32_t vk_pipeline_target_wmask(const VkPipeState *s, int i);
VkPrimitiveTopology vk_pipeline_topology(int nvn_prim);
VkFormat vk_vertex_format(int nvn_format, int *type);
PFN_vkCmdPushDescriptorSetKHR vk_pipeline_push_fn(void);
PFN_vkCmdPushDescriptorSetWithTemplateKHR vk_pipeline_push_tpl_fn(void);
PFN_vkUpdateDescriptorSetWithTemplate vk_pipeline_update_tpl_fn(void);
int vk_pipeline_set_indep(int on);
typedef struct { uint64_t graphics, compute, failures, hits, layouts, push_layouts, async_built, pending_skips; } VkPipeStats;
void vk_pipeline_stats(VkPipeStats *s);
typedef struct {
    uint64_t gfx, gfx_whole, gfx_fast, gfx_opt, gfx_none, gfx_idle, gfx_never;
    uint64_t lib[4], lib_none;
    uint64_t comp, comp_none, comp_idle, comp_never;
    uint64_t layouts;
    uint64_t ram_pipes, ram_keys, ram_seed, ram_binaries;
    uint64_t cache[2];
    int cache_asked;
    uint32_t idle_min;
} VkPipeMem;
void vk_pipeline_mem(VkPipeMem *m, uint32_t idle_min);
void vk_pipeline_save(void);
void vk_pipeline_save_exit(void);
void vk_pipeline_autosave(void);
void vk_pipeline_shutdown(void);
int vk_pipeline_selftest(void);
VkResult vk_pipeline_create_foreign(VkDevice dev, int compute, uint32_t n, const void *infos,
                                    const VkAllocationCallbacks *alloc, VkPipeline *out);
typedef struct { uint64_t made, from_cache, compiled, uncached; } VkForeignPipes;
void vk_pipeline_foreign_counts(VkForeignPipes *o);

#include "vk_rec.h"

struct VkExec;
int vk_fg_pose_prepare(struct VkExec *ex, uint64_t cam, VkImage depth, uint32_t w, uint32_t h);
