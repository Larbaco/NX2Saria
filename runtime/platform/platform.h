#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#if defined(__x86_64__) || defined(_M_X64)
#include "fp_x86.h"
#include <immintrin.h>
#endif

typedef struct PlatThread PlatThread;
typedef void (*PlatThreadFn)(void *arg);

#define PLAT_STACK_GUARANTEE 32768
void plat_stack_guarantee(void);
PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name);
const char *plat_thread_name(void);
#define PLAT_STACK_DEFAULT (16u << 20)
PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack);
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack);
void plat_thread_join(PlatThread *t);
int plat_thread_join_ns(PlatThread *t, uint64_t ns);
void plat_thread_detach(PlatThread *t);
#define PLAT_THREAD_END_HOOKS 4
void plat_thread_end_hook(void (*fn)(void));
void plat_thread_end_run(void);
uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask);
int plat_cpu_reserve(char *which, size_t size);
int plat_cpu_reserve_sweep(void);
uint32_t plat_cpu_count(void);
uint32_t plat_cpu_usable(void);
uint64_t plat_thread_cpu_ns(uint32_t tid);
int plat_cpu_cores(uint64_t *all, uint64_t *first);
uint64_t plat_cpu_cache_die(uint32_t *l3_kb);
int plat_process_affinity(uint64_t mask);

typedef struct PlatIntr PlatIntr;
PlatIntr *plat_intr_self(void);
void plat_intr_free(PlatIntr *t);
int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp);
int plat_intr_resume(PlatIntr *t, uint64_t to);

#define PLAT_WATCH_DEPTH 4
typedef int (*PlatWatchHit)(int slot, const uint64_t *frames, int depth);
int plat_watch_set(const uint64_t *addr, const int *len, int n, PlatWatchHit hit);

typedef struct PlatFiber PlatFiber;
PlatFiber *plat_fiber_self(void);
void plat_fiber_unself(void);
PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg);
void plat_fiber_switch(PlatFiber *to);
void plat_fiber_free(PlatFiber *f);

#ifdef _WIN32
typedef struct { uint64_t gpr[10]; uint32_t mxcsr, fpcw; uint64_t pad; uint8_t xmm[160]; } PlatJmp;
int plat_jmp_save(PlatJmp *j) __attribute__((returns_twice, nothrow));
_Noreturn void plat_jmp_go(PlatJmp *j);
#elif defined(__linux__) && defined(__x86_64__)
typedef struct { uint64_t gpr[8]; uint32_t mxcsr, fpcw; } PlatJmp;
int plat_jmp_save(PlatJmp *j) __attribute__((returns_twice, nothrow));
_Noreturn void plat_jmp_go(PlatJmp *j);
#else
#include <setjmp.h>
typedef struct { jmp_buf b; } PlatJmp;
#define plat_jmp_save(j) setjmp((j)->b)
#define plat_jmp_go(j) longjmp((j)->b, 1)
#endif
int plat_frame_capture(PlatJmp *out, int (*want)(uintptr_t rip, void *arg), void *arg);
enum { PLAT_PRIO_IDLE = -15, PLAT_PRIO_LOWEST = -2, PLAT_PRIO_BELOW_NORMAL = -1, PLAT_PRIO_NORMAL = 0,
       PLAT_PRIO_ABOVE_NORMAL = 1, PLAT_PRIO_HIGHEST = 2, PLAT_PRIO_TIME_CRITICAL = 15 };
void plat_thread_priority(int prio);
void plat_thread_set_priority(PlatThread *t, int prio);
int plat_thread_priority_get(void);
void plat_yield(void);
static inline void plat_pause(void) {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
}

uint64_t plat_now_ns(void);
uint64_t plat_counter(void);
uint64_t plat_counter_hz(void);
void plat_timer_1ms(void);
int plat_power_full(void);
typedef struct { uint64_t q, f; } PlatScale;
static inline PlatScale plat_scale(uint64_t num, uint64_t den) {
    return (PlatScale){num / den, (uint64_t)(((unsigned __int128)(num % den) << 64) / den)};
}
static inline uint64_t plat_scaled(PlatScale s, uint64_t t) {
    return t * s.q + (uint64_t)(((unsigned __int128)t * s.f) >> 64);
}
void plat_sleep_ns(uint64_t ns);
void plat_wait_ns(uint64_t ns);
#ifdef _WIN32
#define PLAT_WAIT_LATE_NS 500000u
#else
#define PLAT_WAIT_LATE_NS 60000u
#endif
static inline uint64_t plat_now_ms(void) { return plat_now_ns() / 1000000u; }
void plat_sleep_ms(uint32_t ms);

typedef struct { void *p; } PlatMutex;
typedef struct { void *p; } PlatCond;
#define PLAT_MUTEX_INIT {0}
#define PLAT_COND_INIT {0}
void plat_mutex_init(PlatMutex *m);
void plat_mutex_lock(PlatMutex *m);
int plat_mutex_trylock(PlatMutex *m);
void plat_mutex_unlock(PlatMutex *m);
void plat_mutex_lock_shared(PlatMutex *m);
int plat_mutex_trylock_shared(PlatMutex *m);
void plat_mutex_unlock_shared(PlatMutex *m);
void plat_cond_init(PlatCond *c);
void plat_cond_wait(PlatCond *c, PlatMutex *m);
int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns);
static inline int plat_cond_wait_ms(PlatCond *c, PlatMutex *m, uint32_t ms) {
    return plat_cond_wait_ns(c, m, (uint64_t)ms * 1000000u);
}
void plat_cond_signal(PlatCond *c);
void plat_cond_broadcast(PlatCond *c);

typedef struct { PlatMutex m; uint32_t owner, depth; } PlatRMutex;
#define PLAT_RMUTEX_INIT {PLAT_MUTEX_INIT, 0, 0}
void plat_rmutex_lock(PlatRMutex *m);
int plat_rmutex_trylock(PlatRMutex *m);
void plat_rmutex_unlock(PlatRMutex *m);

typedef void (*PlatLockNote)(void *site, uint64_t ns);
void plat_lock_timing(PlatLockNote fn);
void plat_mutex_lock_at(PlatMutex *m, void *site);
void plat_rmutex_lock_at(PlatRMutex *m, void *site);

typedef struct { volatile int32_t state; } PlatOnce;
#define PLAT_ONCE_INIT {0}
void plat_once(PlatOnce *o, void (*fn)(void *arg), void *arg);

typedef struct { PlatMutex m; PlatCond c; int set, manual; } PlatEvent;
void plat_event_init(PlatEvent *e, int manual, int initially_set);
void plat_event_set(PlatEvent *e);
void plat_event_reset(PlatEvent *e);
void plat_event_wait(PlatEvent *e);
int plat_event_wait_ns(PlatEvent *e, uint64_t ns);

#define PLAT_SC __ATOMIC_SEQ_CST
static inline int32_t plat_inc32(volatile int32_t *p) { return __atomic_add_fetch(p, 1, PLAT_SC); }
static inline int32_t plat_dec32(volatile int32_t *p) { return __atomic_sub_fetch(p, 1, PLAT_SC); }
static inline int32_t plat_add32(volatile int32_t *p, int32_t v) { return __atomic_add_fetch(p, v, PLAT_SC); }
static inline int32_t plat_xadd32(volatile int32_t *p, int32_t v) { return __atomic_fetch_add(p, v, PLAT_SC); }
static inline int32_t plat_xchg32(volatile int32_t *p, int32_t v) { return __atomic_exchange_n(p, v, PLAT_SC); }
static inline int32_t plat_cas32(volatile int32_t *p, int32_t xchg, int32_t cmp) {
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, PLAT_SC, PLAT_SC);
    return cmp;
}
static inline int64_t plat_inc64(volatile int64_t *p) { return __atomic_add_fetch(p, 1, PLAT_SC); }
static inline int64_t plat_dec64(volatile int64_t *p) { return __atomic_sub_fetch(p, 1, PLAT_SC); }
static inline int64_t plat_add64(volatile int64_t *p, int64_t v) { return __atomic_add_fetch(p, v, PLAT_SC); }
static inline int64_t plat_xadd64(volatile int64_t *p, int64_t v) { return __atomic_fetch_add(p, v, PLAT_SC); }
static inline int64_t plat_xchg64(volatile int64_t *p, int64_t v) { return __atomic_exchange_n(p, v, PLAT_SC); }
static inline int64_t plat_or64(volatile int64_t *p, int64_t v) { return __atomic_fetch_or(p, v, PLAT_SC); }
static inline int64_t plat_cas64(volatile int64_t *p, int64_t xchg, int64_t cmp) {
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, PLAT_SC, PLAT_SC);
    return cmp;
}
static inline void *plat_xchgp(void *volatile *p, void *v) { return __atomic_exchange_n(p, v, PLAT_SC); }
static inline void *plat_casp(void *volatile *p, void *xchg, void *cmp) {
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, PLAT_SC, PLAT_SC);
    return cmp;
}

enum { PLAT_PROT_NONE, PLAT_PROT_READ, PLAT_PROT_RW };
enum { PLAT_MEM_FREE = -2, PLAT_MEM_RESERVED = -1 };
int plat_mem_reserve(uint64_t addr, uint64_t n, int watch);
void *plat_mem_alloc(size_t n);
int plat_mem_commit(uint64_t addr, size_t n, uint32_t *error);
void plat_mem_commit_error(uint32_t error, char *out, size_t cap);
int plat_write_watch_note(char *out, size_t cap);
int plat_mem_decommit(uint64_t addr, uint64_t n);
int plat_mem_protect(uint64_t addr, uint64_t n, int prot);
int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end);
int plat_mem_census(uint64_t *priv, uint64_t *wc, uint64_t *wc_allocs);
uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset);
int plat_mem_prepare_write(uint64_t addr, uint64_t n);
int plat_mem_dmabuf_ok(uint64_t *limit, uint64_t *any, const char **why);
int plat_mem_dmabuf(uint64_t addr, uint64_t n);
void plat_mem_dmabuf_done(uint64_t addr, uint64_t n);
void plat_mem_fault_hook(int (*fn)(uint64_t addr));
void plat_mem_access_hook(int (*fn)(uint64_t addr, int write, uint64_t ip));
_Noreturn void plat_reexec(void);

typedef struct {
    const char *what;
    uint32_t code;
    uintptr_t rip, offset;
    int access;
    uint64_t addr;
    int nframes;
    uintptr_t frames[16];
    const char *module;
} PlatCrash;
void plat_crash_hook(void (*fn)(const PlatCrash *c));
void plat_crash_context(const char *s);
int plat_crash_filter_reassert(char *who, size_t n);
const char *plat_crash_context_get(void);
int plat_mem_read(uint64_t addr, void *out, size_t n);
int plat_mem_readable(uint64_t addr, uint64_t n);
int plat_mem_resident(uint64_t addr, uint64_t n, uint8_t *vec);
void *plat_aligned_alloc(size_t size, size_t align);
void plat_aligned_free(void *p);

_Noreturn void plat_exit(int code);
extern void (*plat_exit_hook)(void);
_Noreturn void plat_terminate(int code);
int plat_setenv(const char *name, const char *value);
uint32_t plat_process_id(void);
uint64_t plat_process_cpu_ns(void);
uint64_t plat_process_uptime_ns(void);
uint64_t plat_unix_time_ns(void);
void plat_local_time(struct tm *out);
int plat_localtime(int64_t unix_s, struct tm *out);
int plat_gmtime(int64_t unix_s, struct tm *out);
long plat_tz_west(void);

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms);

int plat_random(void *out, size_t n);

int plat_file_exists(const char *path);
int plat_dir_exists(const char *path);
int plat_full_path(const char *path, char *out, size_t cap);
void plat_file_mkdir_p(const char *path);
uint8_t *plat_file_read(const char *path, uint64_t *size);
int plat_file_write(const char *path, const void *data, size_t n);
int plat_file_replace(const char *from, const char *to, int durable);
int plat_file_write_atomic(const char *path, const void *data, size_t n);
int plat_file_write_cached(const char *path, const void *data, size_t n);
typedef struct PlatFileSwap PlatFileSwap;
void *plat_file_swap_begin(const char *path, size_t n, PlatFileSwap **swap);
int plat_file_swap_end(PlatFileSwap *swap, size_t used, int ok);
int plat_lock_file(const char *path);
void plat_unlock_file(const char *path);
int plat_file_flush(const char *path);
int plat_file_sync(FILE *f);
#ifdef _WIN32
static inline int plat_fseek64(FILE *f, int64_t off, int whence) { return _fseeki64(f, off, whence); }
static inline int64_t plat_ftell64(FILE *f) { return _ftelli64(f); }
#else
static inline int plat_fseek64(FILE *f, int64_t off, int whence) { return fseeko(f, (off_t)off, whence); }
static inline int64_t plat_ftell64(FILE *f) { return (int64_t)ftello(f); }
#endif
void plat_file_delete(const char *path);
int plat_file_truncate(const char *path, uint64_t size);
int64_t plat_file_size(const char *path);
int plat_file_set_readonly(const char *path, int on);
int plat_file_private(const char *path);
typedef struct PlatAppend PlatAppend;
PlatAppend *plat_append_open(const char *path);
int plat_append_write(PlatAppend *f, const void *data, size_t n);
void plat_exit_note_setup(PlatAppend *log, void (*queued)(const char **p, size_t *n));
void plat_exit_note_modules(void);
void plat_exit_orderly(void);
int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx);
typedef struct PlatFile PlatFile;
PlatFile *plat_file_open(const char *path);
PlatFile *plat_file_open_unbuffered(const char *path);
void plat_file_close(PlatFile *f);
int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off);
typedef struct { uint64_t volume, file_id, size, write_time, create_time; } PlatFileId;
int plat_file_id(PlatFile *f, PlatFileId *out);
const void *plat_file_map(PlatFile *f);

enum { PLAT_FS_OK, PLAT_FS_NOT_FOUND, PLAT_FS_EXISTS, PLAT_FS_NOT_EMPTY, PLAT_FS_BUSY, PLAT_FS_ERROR };
enum { PLAT_FS_MISSING = -1, PLAT_FS_FILE, PLAT_FS_DIR, PLAT_FS_LINK };
int plat_fs_kind(const char *path);
int plat_fs_os_error(void);
int plat_fs_mkdir(const char *path);
int plat_fs_rmdir(const char *path);
int plat_fs_unlink(const char *path);
int plat_fs_rename(const char *from, const char *to, int durable);
int plat_fs_link(const char *existing, const char *path);
int plat_fs_links(const char *path);
void plat_fs_sync_dir(const char *path);
void plat_fs_busy_reason(const char *dir, char *out, size_t cap);
int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx);
PlatFile *plat_fs_open(const char *path, int write, int *err);
PlatFile *plat_fs_create(const char *path, int *err);
int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off);
int plat_file_set_size(PlatFile *f, uint64_t size);
int plat_file_fsync(PlatFile *f);
int plat_temp_dir(char *out, size_t cap);
int plat_exe_path(char *out, size_t cap);
uintptr_t plat_exe_base(void);
int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base);
int plat_library_path(const char *name, char *out, size_t cap);
#define PLAT_PATH_MAX 1024
#ifdef _WIN32
#define PLAT_EXE_SUFFIX ".exe"
#else
#define PLAT_EXE_SUFFIX ""
#endif
static inline char *plat_last_sep(const char *path) {
    char *last = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last = (char *)p;
    return last;
}
size_t plat_getenv(const char *name, char *out, size_t cap);
const char *plat_ui_language(void);
const char *plat_user_locale(void);
uint32_t plat_thread_id(void);
int plat_font_path(const char *lang, char *out, size_t cap);
int plat_os_version(char *out, size_t cap);
int plat_is_wine(void);
int plat_stderr_path(char *out, size_t cap);
int plat_stderr_write(const void *p, size_t n);
int plat_console_setup(void (*on_close)(const char *what));

int plat_clipboard_set(const char *utf8);
int plat_clipboard_get(char *out, size_t cap);
int plat_open_folder(const char *path);

void *plat_lib_open(const char *name);
void *plat_lib_sym(void *lib, const char *name);

typedef struct PlatLoad PlatLoad;
PlatLoad *plat_load_open(const uint8_t *gpu_luid, int pci_bus);
void plat_load_close(PlatLoad *l);
void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c);
void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb);
void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb);
void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb);
int plat_commit_enforced(void);
int plat_commit_note(char *out, size_t cap);
int plat_commit_counts_vram(void);
int plat_page_file_managed(void);
typedef struct {
    double mem_some10, mem_full10, io_some10, io_full10;
    uint64_t mem_some_us, mem_full_us, io_some_us, io_full_us;
    double avail_mb, total_mb;
    double swap_free_mb, swap_total_mb;
    double commit_used_mb, commit_limit_mb;
} PlatPressure;
void plat_pressure(PlatPressure *p);
static inline void plat_pressure_clear(PlatPressure *p) {
    p->mem_some10 = p->mem_full10 = p->io_some10 = p->io_full10 = -1;
    p->mem_some_us = p->mem_full_us = p->io_some_us = p->io_full_us = 0;
    p->avail_mb = p->total_mb = p->swap_free_mb = p->swap_total_mb = p->commit_used_mb = p->commit_limit_mb = -1;
}
void plat_process_memory(uint64_t *resident, uint64_t *private_bytes);
uint32_t plat_handle_count(void);
typedef struct { char name[32]; uint32_t n; } PlatHandleKind;
int plat_handle_census(PlatHandleKind *out, int max);
typedef struct { uint64_t committed, used; uint32_t other_heaps; } PlatHeapInfo;
void plat_heap_info(PlatHeapInfo *h);

typedef struct PlatChild PlatChild;
PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err);
void plat_child_stop(PlatChild *c);
void plat_child_lower(PlatChild *c);
int plat_child_write(PlatChild *c, const void *data, size_t n);
#ifndef PLAT_CHILD_READ_MS
#define PLAT_CHILD_READ_MS 120000
#endif
long plat_child_read(PlatChild *c, void *buf, size_t cap);

typedef struct PlatIpc PlatIpc;
PlatIpc *plat_ipc_connect(const char *name);
int plat_ipc_read(PlatIpc *p, void *buf, size_t n, uint32_t ms);
int plat_ipc_write(PlatIpc *p, const void *buf, size_t n);
void plat_ipc_close(PlatIpc *p);

typedef struct PlatUdp PlatUdp;
typedef struct { uint8_t b[32]; } PlatAddr;
int plat_net_resolve(const char *host, uint16_t port, PlatAddr *out);
PlatUdp *plat_udp_open(uint16_t port, int dual_stack);
uint16_t plat_udp_port(const PlatUdp *u);
int plat_udp_send(PlatUdp *u, const PlatAddr *to, const void *p, size_t n);
int plat_udp_recv(PlatUdp *u, void *buf, size_t cap, PlatAddr *from, uint32_t ms);
void plat_udp_close(PlatUdp *u);
void plat_addr_set(PlatAddr *a, const uint8_t ip[16], uint16_t port, uint32_t scope);
void plat_addr_get(const PlatAddr *a, uint8_t ip[16], uint32_t *scope);
uint16_t plat_addr_port(const PlatAddr *a);
int plat_addr_eq(const PlatAddr *a, const PlatAddr *b);
int plat_addr_str(const PlatAddr *a, char *out, size_t cap);

typedef struct PlatTcp PlatTcp;
PlatTcp *plat_tcp_listen_loopback(uint16_t port, int *err);
PlatTcp *plat_tcp_accept(PlatTcp *listener, uint32_t ms, PlatAddr *peer, int *err);
int plat_tcp_recv(PlatTcp *c, void *buf, size_t cap, uint32_t ms);
int plat_tcp_send(PlatTcp *c, const void *buf, size_t n, uint32_t ms);
void plat_tcp_close(PlatTcp *c);

int plat_restart(void);
int plat_restart_wait(uint32_t timeout_ms);
