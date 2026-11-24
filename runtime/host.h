#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "dispatch.h"

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_vlogf(const char *fmt, va_list ap);
void rt_log_raw(const char *p, size_t n);
const char *rt_log_path(void);
void rt_log_sync(void);
void rt_log_ending(void);
void rt_log_focus_lost(void);
void hang_watch_start(void);
void kern_wait_report(void);
void vk_sync_progress(uint64_t *issued, uint64_t *done);
enum { RT_TRACE_SVC = 1, RT_TRACE_HOSTCALL = 2, RT_TRACE_IPC = 4, RT_TRACE_FS = 8, RT_TRACE_GFX = 16, RT_TRACE_PERF = 32,
       RT_TRACE_SCHED = 64, RT_TRACE_AUDIO = 128, RT_TRACE_PRESENT = 256 };
extern unsigned rt_trace;
enum { PERF_PACE, PERF_FENCE, PERF_DRAIN, PERF_PIPE, PERF_XLATE, PERF_TEX, PERF_ASTC, PERF_HANDOFF, PERF_N };
void prof_start(void);
void prof_write(void);
uint64_t perf_begin(void);
void perf_end(int kind, uint64_t t0);
void perf_end_any(int kind, uint64_t t0);
void perf_replay(uint64_t t0);
void perf_replay_thread(void);
void perf_main_thread(void);
enum { PERF_CNT_PIPE_ASYNC, PERF_CNT_PIPE_SKIP, PERF_CNT_TEX_UP, PERF_CNT_TEX_REUP, PERF_CNT_SHADER_SKIP,
       PERF_CNT_PIPE_WAIT, PERF_CNT_ONESHOT_WAIT, PERF_CNT_PIPE_BUILT, PERF_CNT_TEX_NEW, PERF_CNT_TEX_ALIAS,
       PERF_CNT_WB, PERF_CNT_WB_SKIP, PERF_CNT_N };
void perf_count(int kind);
enum { PERF_M_FILE, PERF_M_ZSTD, PERF_M_PIPE, PERF_M_GMEM, PERF_M_POOL, PERF_M_DEVMEM, PERF_M_NGX, PERF_M_UPWAIT,
       PERF_M_UPREL, PERF_M_N };
uint64_t perf_mark_begin(void);
void perf_mark_end(int kind, uint64_t t0, uint64_t bytes);
void perf_mark_add(int kind, uint64_t ns, uint64_t bytes);
void perf_segment(const char *name);
void perf_scene(const char *name);
uint64_t perf_area_load_ns(void);
typedef struct { char line[1800]; int n; uint64_t frame; const char *group; } PerfSizes;
void perf_size(PerfSizes *s, const char *name, uint64_t count, uint64_t limit, uint64_t bytes);
#define PERF_SIZE_NONE UINT64_MAX
void mem_sizes(PerfSizes *s);
void kern_sizes(PerfSizes *s);
void setjmp_sizes(PerfSizes *s);
void fs_host_sizes(PerfSizes *s);
void romfs_sizes(PerfSizes *s);
void zstd_sizes(PerfSizes *s);
void cfg_sizes(PerfSizes *s);
void vk_mem_sizes(PerfSizes *s);
void vk_gpa_sizes(PerfSizes *s);
void vk_texture_sizes(PerfSizes *s);
void vk_exec_sizes(PerfSizes *s);
void vk_sync_sizes(PerfSizes *s);
void vk_usnap_sizes(PerfSizes *s);
void nvn_cmd_sizes(PerfSizes *s);
void vk_pipeline_sizes(PerfSizes *s);
void gfx_shader_sizes(PerfSizes *s);
enum { PERF_RP_DRAW, PERF_RP_BIND, PERF_RP_XFER, PERF_RP_OTHER, PERF_RP_N };
void perf_replay_op(int cat, uint64_t ns);
void perf_replay_op_at(int cat, uint32_t op, uint64_t ns);
void perf_replay_cpu(uint64_t ns);
void perf_lock_timing_here(void);
int perf_lock_holds_on(void);
void perf_lock_hold(void *site, uint64_t ns);
void perf_qlock_hold(void *site, uint64_t ns);
extern void (*perf_rec_waits)(uint64_t *full_ns, uint64_t *drain_ns);
void perf_present(uint64_t frame);
void perf_cpu_start(void);
int perf_cpu_on_cache_die(void);
void perf_summary_flush(void);
void perf_summary_flush_try(void);
typedef struct {
    uint64_t frames, over25, over100;
    double fps_mean, fps_low_1pct, worst_ms;
} PerfSession;
int perf_session_stats(PerfSession *out);
void perf_focus_lost(void);
void gfx_exit_log(void);
void gfx_window_closed(void);
int rt_exit_begin(void);
int rt_exit_started(void);
enum { RT_EXIT_SETTINGS = 1 };
void rt_exit_steps(unsigned what);
void perf_gpu_wait(uint64_t t0);
typedef struct {
    int have_gpu, have_vram, vram_exact;
    uint64_t gpu_ns, gpu_parts, gpu_missed;
    uint64_t vram_used, vram_budget;
    int have_idle;
    uint64_t idle_ns, starved_ns, queued_ns;
    uint64_t idle_gaps, max_gap_ns;
    uint64_t mirror_ns;
} PerfGpu;
extern void (*perf_gpu_stats)(PerfGpu *out);
void perf_note_wc(uint64_t wc, uint64_t vram_used);
int perf_wc_recent(uint64_t max_age_ns, uint64_t *wc, uint64_t *vram_used);
void perf_census_walk(int begin);
void perf_report_part(uint64_t ns);
uint64_t perf_census_walked_frame(void);
void perf_set_overlay(int mode);
int perf_gpu_timing(void);
void perf_gpu_timing_want(int on);
void perf_gpu_busy(uint64_t ns);
uint64_t perf_gpu_busy_total(void);
#define RT_RESULT(module, desc) ((uint32_t)(module) | ((uint32_t)(desc) << 9))
void rt_sha256(const void *p, size_t n, uint8_t out[32]);

#define RT_ADDRESS_LIMIT (1ull << 39)
#define RT_KERNEL_BASE 0x7010000000ull
#define RT_KERNEL_SIZE 0x10000000ull
#define RT_HOSTFN_BASE 0x7020000000ull
#define RT_HOSTFN_SIZE 0x100000ull

static inline uint64_t arg_x(const Cpu *c, int i) { return i < 8 ? c->x[i] : LD64(c->sp + 8 * (uint64_t)(i - 8)); }
static inline void *arg_p(const Cpu *c, int i) { return (void *)(uintptr_t)arg_x(c, i); }
static inline float arg_s(const Cpu *c, int i) { uint32_t b = (uint32_t)c->v[i]; float f; memcpy(&f, &b, 4); return f; }
static inline double arg_d(const Cpu *c, int i) { uint64_t b = (uint64_t)c->v[i]; double d; memcpy(&d, &b, 8); return d; }
static inline void ret_x(Cpu *c, uint64_t v) { c->x[0] = v; }
static inline void ret_s(Cpu *c, float f) { uint32_t b; memcpy(&b, &f, 4); c->v[0] = b; }
static inline void ret_d(Cpu *c, double d) { uint64_t b; memcpy(&b, &d, 8); c->v[0] = b; }

typedef struct { const char *name; GuestFn fn; } RtHostEntry;
#define RT_CAT2(a, b) a##b
#define RT_CAT(a, b) RT_CAT2(a, b)
#ifdef _WIN32
#define RT_HOST_SECTION(kind) section("." #kind "$m")
#else
#define RT_HOST_SECTION(kind) section("nx2navi_" #kind)
#endif
#define HOST_OVERRIDE(sym, fn) \
    __attribute__((used, RT_HOST_SECTION(ovr))) static const RtHostEntry RT_CAT(rt_ovr_, __COUNTER__) = {sym, fn}
#define HOST_OBSERVER(sym, fn) \
    __attribute__((used, RT_HOST_SECTION(obs))) static const RtHostEntry RT_CAT(rt_obs_, __COUNTER__) = {sym, fn}
uint64_t hostfn_register(const char *name, GuestFn fn);
GuestFn hostfn_override(const char *name);
GuestFn hostfn_observer(const char *name);
void hostfn_add_override(const char *name, GuestFn fn);
const char *hostfn_name(uint64_t addr);
void hostfn_seal(void);
uint64_t hostfn_calls(void);
void hostfn_report(void);
int hostfn_table_size(int observers);
const char *hostfn_table_name(int observers, int i);
uint64_t rt_original(Cpu *c);
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...);
#define RT_RET_HOST 0x70200ff000ull

typedef struct { uint64_t base, size; } RtRange;
typedef struct { RtRange aslr, stack, heap, alias, kernel, hostfn; uint64_t pool; } RtGuestLayout;
typedef struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } RtMemoryInfo;
void mem_init(int game);
const RtGuestLayout *mem_layout(void);
void mem_commit_checked(uint64_t addr, size_t size, const char *what);
int mem_start_note(int probe);
void mem_guest_census(uint64_t frame);
int mem_guest_ranges(const char **name, RtRange *r, int max);
void mem_guest_census_log(uint64_t frame, const char *const *name, const uint64_t *committed, int n);
void mem_lazy_heap_census(uint64_t frame);
void mem_lazy_heap_report(uint64_t frame);
void mem_guest_usage(uint64_t *heap, uint64_t *heap_committed, uint64_t *phys_maps);
uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr);
uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm);
uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at);
uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n);
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n);
uint32_t mem_query(uint64_t a, RtMemoryInfo *out);
int mem_readable(uint64_t a, uint64_t n);
uint32_t mem_map_gen(void);
int mem_read_hold(uint64_t a, uint64_t n);
void mem_read_release(void);
uint32_t mem_map_physical(uint64_t a, uint64_t n);
uint32_t mem_unmap_physical(uint64_t a, uint64_t n);
int mem_info(uint32_t id, uint64_t *out);
void *mem_kernel_alloc(uint64_t size, uint64_t align);
uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max);
uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max);
int mem_watched(uint64_t addr, uint64_t size);
int mem_freeze(uint64_t a);
int mem_frozen(uint64_t a);
void mem_thaw(uint64_t a, uint64_t n);
void mem_kernel_access(uint64_t a, uint64_t n);
uint64_t mem_untouched_run(uint64_t a, uint64_t end);
int mem_host_readable(uint64_t a, uint64_t n);
void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws,
                      uint64_t *hot_holds);
typedef struct { uint64_t h0, h1, off, size; } MemLazyCopy;
int mem_lazy_enable(void (*fetch)(const MemLazyCopy *c, int n), int (*clean)(uint64_t lo, uint64_t hi, void *ctx));
uint64_t mem_lazy_arm(uint64_t a, uint64_t n, uint64_t h0, uint64_t h1, uint64_t off, void *ctx);
int mem_lazy_skip(uint64_t a, uint64_t n, uint64_t h0, int64_t delta, uint64_t out[][2], int max);
int mem_lazy_armed(uint64_t a, uint64_t n);
int mem_copy_readable(uint8_t *const *dst, const uint64_t (*src)[2], int n, uint8_t *ok);
int mem_lazy_touches(uint64_t a, uint64_t n);
void mem_lazy_window(int open);
void mem_lazy_release(const uint64_t (*rg)[3], int n);
void mem_lazy_report(uint64_t frame, uint64_t skipped);
void mem_lazy_note(char *out, size_t n);
int mem_lazy_pick(uint64_t *addr, int max);
int mem_probe(uint64_t a, uint64_t n);
int mem_probing(void);
void mem_probe_report(uint64_t frame);
void mem_kernel_free(void *p, uint64_t size);

typedef struct {
    const char *name; uint64_t base, size, text_end, data_off;
    uint64_t init, init_array, init_array_size, ctor_flag;
} RtLoaded;
void ldr_load(const char *exefs_dir);
int ldr_count(void);
const RtLoaded *ldr_module(int i);
uint64_t ldr_symbol(const char *name);
uint64_t ldr_plt_stub(const char *module, const char *name);
const char *ldr_describe(uint64_t a, char *buf, int n);
uint8_t *ldr_read_verified(const char *dir, const char *file, size_t *len);

typedef struct { const char *file, *sha256; } RtFileHash;
extern const RtFileHash rt_exefs_sha256[];
extern const int rt_exefs_sha256_count;
extern const char rt_default_exefs[], rt_default_romfs[], rt_default_profile[];
extern const unsigned long long rt_app_pool_bytes;
extern const int rt_nvn_api_version[2];
extern const int rt_game_version;
extern const char rt_port_version[];
#define RT_V110 (rt_game_version == 110)
#define RT_V111 (rt_game_version == 111)
#define RT_V143 (rt_game_version >= 143)

typedef struct RtThread RtThread;
Cpu *kern_cpu(void);
void fs_save_crash_wait(void);
void kern_free_cores_note(void);
uint32_t kern_main_thread(Cpu **cpu);
void kern_main_stack(uint64_t *base, uint64_t *size);
void kern_run_on_guest_main(void (*fn)(Cpu *c));
int kern_core_away(void);
void kern_core_back(int *away);
void kern_checkpoint(void);
static inline int kern_preempt_pending(const void *thread) { return __atomic_load_n((const int *)thread, __ATOMIC_RELAXED); }
extern int kern_host_track;
static inline int *kern_host_depth(void *thread) { return (int *)thread + 1; }
void kern_host_left(void *thread);
int kern_core_diag_on(void);
void kern_core_diag_frame(uint64_t frame, uint64_t interval_ns);
void kern_sched_report(uint64_t frame);
uint32_t kern_handle_new(int kind, void *obj);
void *kern_handle_get(uint32_t h, int kind);
void *kern_handle_hold(uint32_t h, int kind, void (*hold)(void *obj));
int kern_handle_copy(uint32_t h, int kind, void *out, size_t n);
void kern_handle_close(uint32_t h);
enum { KERN_SESSION = 4, KERN_TMEM = 5 };
uint32_t kern_event_new(void);
uint32_t kern_event_readable(uint32_t writable);
void kern_event_signal(uint32_t writable);
void kern_event_clear(uint32_t writable);
void kern_bind_system_event(Cpu *c, uint64_t ev, int autoclear);
void kern_signal_system_event(uint64_t ev);
void kern_clear_system_event(uint64_t ev);
void kern_unbind_system_event(Cpu *c, uint64_t ev);
int kern_system_event_live(uint64_t ev);
int kern_svc(Cpu *c, uint32_t imm);
uint64_t kern_now_ns(void);
extern uint64_t rt_guest_start_ns;
typedef struct { uint64_t svcs, threads, host_calls; } RtStats;
RtStats rt_stats(void);

int ipc_svc(Cpu *c, uint32_t imm);
void ipc_handle_closed(int kind, void *obj);

void fs_init(void);

void account_uid(uint64_t out[2]);

void movie_tick(Cpu *c);
float cam_speed_wanted(void);
float cam_speed_applied(void);
int cam_pack_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, void **out, size_t *out_n);
void cam_pack_opened(void);
void cam_speed_tick(void);
void cam_pack_served(void);
uint64_t res_heap_extra_bytes(void);
uint64_t res_heap_extra_kib(void);
int res_heapdef_build(const void *in, size_t n, void **out, size_t *out_n);
void res_heapdef_served(void);
int res_heapdef_ok(void);
void wide_init(void);
void wide_present(void);
void wide_lens_host(Cpu *c);
int wide_available(void);
void wide_picture(unsigned *w, unsigned *h);
int wide_start_picture(unsigned *w, unsigned *h);
void wide_bars_init(double x_margin, double y_margin);
void wide_bars_screen_host(Cpu *c);
int wide_menu_open(void);
int wide_loading_open(void);
void menu_mouse_screen_host(Cpu *c);
void wide_menu_bars(double *x, double *y);
int wide_scene_settable(void);
double wide_scene_share(void);
double wide_scene_wanted(void);
int wide_scene_restart(void);
double wide_dynres_base(void);
uint64_t wide_heap_extra_kib(void);
uint64_t mem_heap_extra_max_kib(void);
void res_init(void);
float res_dlss_lod_bias(void);
int res_dlss_mode(const char *mode);
void dynres_frame(void);
const char *dynres_override(void);
double dynres_share(void);
void res_fn_hit(uint64_t addr, uint64_t x0);
void res_present(Cpu *c);
void res_sizes(unsigned out[2], unsigned scene[2]);
int res_adjustable(void);
int res_is_16x9(unsigned w, unsigned h);
int res_wants_wide_overlay(const unsigned out[2]);
void res_wanted(unsigned out[2]);
void res_fov_live(void);
float res_fov_factor(void);
void rt_patches_init(void);
int rt_patch_set(const char *name);
int rt_patch_enabled(int set);
void rt_patch_disable(int set);
void rt_patch_switch(int set, int on);
void *rt_patch_page(void);
void frame_step_init(void);
int frame_step_realtime(void);
uint64_t frame_step_cap_ns(void);
int frame_step_movie_hz(void);
uint64_t frame_step_ceiling_ns(void);
int ov_movie_live(void);
int frame_limit_adjustable(void);
int frame_free_running(void);
double frame_step_limit_hz(void);
double frame_step_game_hz(void);
void frame_rate_day_host(Cpu *c);
void frame_rate_water_host(Cpu *c);
void frame_rate_sim_host(Cpu *c);
void frame_rate_sound_gate_host(Cpu *c);
void frame_rate_horse_jump_host(Cpu *c);
void frame_rate_gohma_turn_host(Cpu *c);
uint32_t frame_rate_animation_frames(uint64_t entry, float step);
void animation_loop_host(Cpu *c);
void battle_music_gate(int run);
void battle_music_flags_host(Cpu *c);
void voice_language_host(Cpu *c);
void wide_balloon_host(Cpu *c);
void frame_rate_sound_step_host(Cpu *c);
float frame_step_frames(void);
void frame_step_flight(float *page, float *game, uint64_t *presents);
uint64_t frame_step_presents(void);
void rtss_limit_want(int64_t mhz);
void rtss_limit_movie(int hz);
int rtss_limit_active(void);
int rtss_limit_state(void);
void rtss_limit_restore(void);
void rtss_limit_crash(void);
int64_t frame_step_world_us(void);
uint64_t frame_step_ticks(void);
int rt_ab(void);
int rt_knob(const char *name, const char *def);
#define RT_KNOB_ON(mode) ((mode) == 2 ? rt_ab() : (mode))
void frame_step_present(uint64_t display_ns, int interval);
typedef struct { const char *exefs, *romfs, *profile; int boot; double seconds; const char *shot, *gpu; } RtConfig;
const RtConfig *rt_config(void);
void rt_boot_event(const char *what, Cpu *c, const char *detail);
void rt_boot(void);
int list_audio_devices(void);
int probe_cfg(void);
int probe_dpi(void);
int list_monitors(void);
int probe_mods(void);
void discord_init(void);
void tracker_api_init(void);
int tracker_api_on(void);
int probe_discord(void);
int probe_loader(void), probe_kernel(void), probe_ipc(void), probe_services(void), probe_fs(void);
int probe_gfx_abi(void);
int probe_vkdevice(void);
int vk_sync_probe_devlost(void);
int probe_nvn(void), probe_nvn_tex(void);
int probe_window(void);
int probe_shader(void);
int probe_census_dir(char *dir, size_t n);
int probe_vkmem(void), probe_vkexec(void), probe_astc(void);
int probe_meshfb(void);
int probe_fsr3(void);
int probe_fsr3fg(void);
int probe_zstd(void);
int probe_gamepad(void);
int probe_movie(void);
int vk_pipeline_prepare_shaders(void);
int vk_pipeline_warm_shaders(void);
int vk_pipeline_prepare_pipelines(void);
int vk_pipeline_probe_prewarm(void);
int vk_pipeline_probe_buildgate(void);
void vk_pipeline_crash_note(void);
int vk_ffx_probe_cache(void);
void vk_driver_cache_env(void);
int vk_pipeline_spirv_selftest(void);
