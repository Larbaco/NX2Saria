#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "mem_budget.h"
#include "platform/platform.h"
#include "prof.h"
#include "prp_file.h"
#include "session_summary.h"
#include "spv_store.h"

#define CONTROL_SIZE 0x880
#define CODE_SKIP 0x30
#define BUCKETS 4096
#define LINE_MAX_LEN 4096
#define KEY_VERSION 5
#define IN_VERSION 1
#define USE_MAGIC 0x4d554b54u
#define USE_VERSION 1
#define IN_MAGIC 0x49534b54u
#define MAX_SVC 32

typedef struct {
    uint32_t slots;
    uint32_t attrs;
    uint32_t fouts;
} UseMask;

typedef struct UseEnt {
    struct UseEnt *next;
    uint8_t code_hash[32];
    int known;
    UseMask m;
} UseEnt;

struct GfxShader {
    struct GfxShader *next;
    uint8_t key[32];
    uint8_t iid[32];
    SpvStore spv;
    uint32_t *bind;
    uint32_t nbind;
    char *reflection;
    uint64_t reflection_size;
    int stage;
    GfxShaderRes *res[GFX_RES_KINDS];
    int nres[GFX_RES_KINDS];
    uint32_t in_mask, out_mask;
    GfxShaderIo in[32], out[32];
    uint32_t local[3];
    uint32_t mvprev_banks;
    uint8_t pal_stride, pal_storage;
    int16_t pal_slot;
    uint32_t pal_joints;
    uint32_t omap;
    uint8_t carried;
    uint8_t fresh;
};

static PlatMutex cache_lock = PLAT_MUTEX_INIT;
static PlatMutex spv_lock = PLAT_MUTEX_INIT;
static GfxShader *buckets[BUCKETS];
static UseEnt *use_buckets[BUCKETS];
static volatile int64_t n_requests, n_hits, n_failures, n_keys, n_xlated;

typedef struct {
    PlatChild *child;
    int busy;
    size_t in_len;
    char inbuf[LINE_MAX_LEN * 2];
} Svc;

static struct {
    PlatMutex lock;
    PlatCond freed;
    int limit;
    Svc s[MAX_SVC];
} S = {PLAT_MUTEX_INIT, PLAT_COND_INIT};

static struct {
    PlatMutex lock;
    PlatCond done;
    uint8_t id[2 * MAX_SVC][32];
    int n;
} F = {PLAT_MUTEX_INIT, PLAT_COND_INIT};

#define PATH_CAP 1024
static char cache_dir[PATH_CAP];
static char xlate_exe[PATH_CAP];
static uint8_t xlate_id[32];
static uint8_t xlate_base[32];
static int host_f64 = 1, host_zero_locals, host_bda, host_set;
static uint32_t host_sbuf, host_sg[3] = {32, 32, 32}, host_gather;
static PlatOnce paths_once = PLAT_ONCE_INIT;

#define IDS_MAX 8
static struct {
    int on;
    int n_prev;
    uint8_t prev[IDS_MAX][32];
    uint8_t prev_eff[IDS_MAX][32];
    volatile int32_t ready;
    volatile int32_t suspended;
    volatile int32_t noted;
    uint8_t dropped[IDS_MAX][32];
    int n_dropped;
    volatile int64_t served;
    int64_t served_logged;
    volatile int64_t refused;
    int64_t refused_logged;
} C;
static PlatMutex ids_lock = PLAT_MUTEX_INIT;

static int carry_on(void) {
    return C.n_prev && __atomic_load_n(&C.ready, __ATOMIC_ACQUIRE) && !__atomic_load_n(&C.suspended, __ATOMIC_ACQUIRE);
}

#define file_exists plat_file_exists
#define mkdir_p plat_file_mkdir_p
#define read_file plat_file_read
#define write_file plat_file_write

enum { PACK_SPV = 1, PACK_RES = 2, PACK_USE = 3, PACK_IN = 4 };
typedef struct { uint8_t name[32]; uint32_t kind, size; uint64_t offset; } PackEnt;
_Static_assert(sizeof(PackEnt) == 48, "shader-cache.pack entry layout");
static PlatFile *pack_file;
static PackEnt *pack_ix;
static uint32_t pack_n;

static int pack_cmp(const PackEnt *a, uint32_t kind, const uint8_t name[32]) {
    if (a->kind != kind) return a->kind < kind ? -1 : 1;
    return memcmp(a->name, name, 32);
}

static int pack_read_at(uint64_t offset, void *out, uint32_t n) {
    return plat_file_pread(pack_file, out, n, offset) == (int64_t)n;
}

static void pack_open(void) {
    char p[PATH_CAP + 32];
    snprintf(p, sizeof p, "%s/shader-cache.pack", cache_dir);
    PlatFile *h = plat_file_open(p);
    if (!h) return;
    PlatFileId id;
    uint8_t head[16];
    uint32_t n = 0;
    const char *why = NULL;
    PackEnt *ix = NULL;
    pack_file = h;
    if (!plat_file_id(h, &id) || !pack_read_at(0, head, sizeof head) || memcmp(head, "TKSPACK1", 8)) why = "header";
    if (!why) {
        memcpy(&n, head + 8, 4);
        if (n > (1u << 24) || (uint64_t)n * sizeof *ix + sizeof head > id.size ||
            !(ix = malloc((size_t)n * sizeof *ix + 1)))
            why = "index size";
        else if (!pack_read_at(sizeof head, ix, n * (uint32_t)sizeof *ix)) why = "index read";
    }
    for (uint32_t i = 0; !why && i < n; i++) {
        if (ix[i].offset + ix[i].size > id.size) why = "entry out of bounds";
        else if (i && pack_cmp(&ix[i - 1], ix[i].kind, ix[i].name) >= 0) why = "index order";
    }
    if (why) {
        rt_log("gfx_shader: %s is not a usable cache pack (%s); reading the loose files", p, why);
        free(ix);
        plat_file_close(h);
        pack_file = NULL;
        return;
    }
    pack_ix = ix, pack_n = n;
    rt_log("gfx_shader: cache pack %s: %u files", p, n);
}

static const PackEnt *pack_find(uint32_t kind, const uint8_t name[32]) {
    size_t lo = 0, hi = pack_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = pack_cmp(&pack_ix[mid], kind, name);
        if (!c) return &pack_ix[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

static uint8_t *pack_read(uint32_t kind, const uint8_t name[32], uint64_t *size) {
    const PackEnt *e = pack_find(kind, name);
    uint8_t *buf = e && e->size ? malloc((size_t)e->size + 1) : NULL;
    if (!buf) return NULL;
    if (!pack_read_at(e->offset, buf, e->size)) {
        free(buf);
        return NULL;
    }
    buf[e->size] = 0;
    *size = e->size;
    return buf;
}

static void json_esc(char **o, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        if (c >= 0x80) {
            int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            c &= 0x3f >> more;
            while (more-- && (*p & 0xc0) == 0x80) c = c << 6 | (*p++ & 0x3f);
        }
        if (c == '"' || c == '\\') *(*o)++ = '\\', *(*o)++ = (char)c;
        else if (c >= 0x20 && c < 0x7f) *(*o)++ = (char)c;
        else if (c >= 0x10000) {
            c -= 0x10000;
            *o += sprintf(*o, "\\u%04x\\u%04x", 0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff));
        } else *o += sprintf(*o, "\\u%04x", c);
    }
}

static int hex32(const char *h, uint8_t out[32]) {
    for (int i = 0; i < 64; i++) {
        char c = h[i] >= 'A' && h[i] <= 'F' ? (char)(h[i] | 32) : h[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return 0;
        out[i / 2] = (uint8_t)(i & 1 ? out[i / 2] | v : v << 4);
    }
    for (const char *p = h + 64; *p; p++)
        if (*p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') return 0;
    return 1;
}

static void xlate_identity(void) {
    char text[256] = "";
    const char *how = "NX2SARIA_XLATE_ID";
    const char *id_env = cfg_get("NX2SARIA_XLATE_ID");
    if (id_env && *id_env && strlen(id_env) < sizeof text) {
        strcpy(text, id_env);
        if (!hex32(text, xlate_id)) rt_sha256(text, strlen(text), xlate_id);
    } else {
        char p[PATH_CAP + 8];
        strcpy(p, xlate_exe);
        char *dot = strrchr(p, '.'), *sep = plat_last_sep(p);
        if (dot && dot > sep) *dot = 0;
        strcat(p, ".id");
        uint64_t size = 0;
        char *f = (char *)read_file(p, &size);
        how = "nx2navi_xlate.id";
        if (!f || !hex32(f, xlate_id)) {
            uint8_t *exe = read_file(xlate_exe, &size);
            how = exe ? "sha256 of the exe; tools/build_xlate.py writes nx2navi_xlate.id" : "none: no translator";
            if (exe) rt_sha256(exe, (size_t)size, xlate_id);
            free(exe);
        }
        free(f);
    }
    memcpy(xlate_base, xlate_id, 32);
    rt_log("gfx_shader: translator identity %02x%02x%02x%02x%02x%02x%02x%02x (%s)", xlate_id[0], xlate_id[1], xlate_id[2],
           xlate_id[3], xlate_id[4], xlate_id[5], xlate_id[6], xlate_id[7], how);
}

static void ids_load(void);
static void pack_swap_pruned(void);

static void use_preload_start(void);

static void init_paths(void *param) {
    (void)param;
    char exe[PATH_CAP], root[PATH_CAP] = "", exe_dir[PATH_CAP] = "";
    if (plat_exe_path(exe, sizeof exe)) {
        char *last = plat_last_sep(exe);
        if (last) snprintf(exe_dir, sizeof exe_dir, "%.*s", (int)(last - exe), exe);
        for (char *cut = plat_last_sep(exe); cut; cut = plat_last_sep(exe)) {
            *cut = 0;
            char probe[PATH_CAP + 32];
            snprintf(probe, sizeof probe, "%s/build/xlate/nx2navi_xlate" PLAT_EXE_SUFFIX, exe);
            if (file_exists(probe)) {
                strcpy(root, exe);
                break;
            }
        }
    }
    const char *xlate_cfg = cfg_get("NX2SARIA_XLATE"), *cache_cfg = cfg_get("NX2SARIA_SHADER_CACHE");
    char beside[PATH_CAP + 32];
    if (exe_dir[0]) snprintf(beside, sizeof beside, "%s/nx2navi_xlate" PLAT_EXE_SUFFIX, exe_dir);
    if (xlate_cfg && *xlate_cfg && strlen(xlate_cfg) < sizeof xlate_exe) strcpy(xlate_exe, xlate_cfg);
    else if (exe_dir[0] && file_exists(beside)) strcpy(xlate_exe, beside);
    else if (root[0]) snprintf(xlate_exe, sizeof xlate_exe, "%s/build/xlate/nx2navi_xlate" PLAT_EXE_SUFFIX, root);
    if (cache_cfg && *cache_cfg && strlen(cache_cfg) < sizeof cache_dir) strcpy(cache_dir, cache_cfg);
    else if (exe_dir[0]) snprintf(cache_dir, sizeof cache_dir, "%s/shader-cache", exe_dir);
    xlate_identity();
    C.on = cfg_bool("NX2SARIA_SHADER_CARRY");
    if (cache_dir[0] && C.on) ids_load(), pack_swap_pruned();
    if (cache_dir[0]) pack_open();
    use_preload_start();
    return;
}

static void paths(void) { plat_once(&paths_once, init_paths, NULL); }

static int host_text(char *t) {
    if (host_f64 && !host_sbuf && host_sg[0] == 32 && host_sg[1] == 32 && host_sg[2] == 32 && !host_gather &&
        !host_zero_locals && !host_bda)
        return 0;
    int n = snprintf(t, 96, "TKHO f64=%d sbuf=%u sg=%u,%u,%u", host_f64, host_sbuf, host_sg[0], host_sg[1], host_sg[2]);
    if (host_gather) n += snprintf(t + n, (size_t)(96 - n), " gather=%u", host_gather);
    if (host_zero_locals) n += snprintf(t + n, (size_t)(96 - n), " zero_locals");
    if (host_bda) n += snprintf(t + n, (size_t)(96 - n), " bda");
    return n;
}

static void host_mix(const uint8_t base[32], uint8_t out[32]) {
    uint8_t buf[32 + 96];
    int n = host_text((char *)buf + 32);
    if (!n) {
        memmove(out, base, 32);
        return;
    }
    memcpy(buf, base, 32);
    rt_sha256(buf, 32 + (size_t)n, out);
}

void gfx_shader_set_host(int float64, uint32_t sbuf_align, const uint32_t subgroup[3], uint32_t gather_bits, int zero_locals,
                         int bda) {
    paths();
    if (host_set++) return;
    host_f64 = float64, host_sbuf = sbuf_align, host_gather = gather_bits, host_zero_locals = zero_locals, host_bda = bda;
    memcpy(host_sg, subgroup, sizeof host_sg);
    char t[96];
    int n = host_text(t);
    host_mix(xlate_base, xlate_id);
    for (int i = 0; i < C.n_prev; i++) host_mix(C.prev[i], C.prev_eff[i]);
    __atomic_store_n(&C.ready, 1, __ATOMIC_RELEASE);
    if (n) rt_log("gfx_shader: host options %s: own shader cache keys", t + 5);
}

static void key_hex(const uint8_t key[32], char out[65]);

static void ids_path(char *p) { snprintf(p, PATH_CAP + 32, "%s/identities.txt", cache_dir); }

static int ids_parse(const char *f, uint8_t out[IDS_MAX][32]) {
    int n = 0;
    while (f && *f && n < IDS_MAX) {
        const char *e = strchr(f, '\n');
        size_t len = e ? (size_t)(e - f) : strlen(f);
        char line[128];
        uint8_t id[32];
        if (len < sizeof line && *f != '#') {
            memcpy(line, f, len), line[len] = 0;
            int ok = hex32(line, id);
            for (int i = 0; ok && i < n; i++) ok = memcmp(out[i], id, 32) != 0;
            if (ok) memcpy(out[n++], id, 32);
        }
        f += len + (e != NULL);
    }
    return n;
}

static int id_zero(const uint8_t id[32]) {
    static const uint8_t zero[32];
    return !memcmp(id, zero, 32);
}

static void ids_load(void) {
    char p[PATH_CAP + 32];
    uint8_t ids[IDS_MAX][32];
    uint64_t size = 0;
    ids_path(p);
    char *f = (char *)read_file(p, &size);
    int n = ids_parse(f, ids);
    free(f);
    for (int i = 0; i < n; i++)
        if (memcmp(ids[i], xlate_base, 32)) memcpy(C.prev[C.n_prev++], ids[i], 32);
    C.noted = id_zero(xlate_base) || (n && !memcmp(ids[0], xlate_base, 32));
    if (C.n_prev)
        rt_log("gfx_shader: carry: the cache holds translations of %d previous translators (newest %02x%02x%02x%02x); "
               "shaders the current one has not translated yet use theirs", C.n_prev, C.prev[0][0], C.prev[0][1],
               C.prev[0][2], C.prev[0][3]);
}

static void ids_write(void) {
    char p[PATH_CAP + 32], text[128 + IDS_MAX * 66];
    uint8_t ids[IDS_MAX][32], out[IDS_MAX][32];
    uint64_t size = 0;
    plat_mutex_lock(&ids_lock);
    ids_path(p);
    char *f = (char *)read_file(p, &size);
    int n = ids_parse(f, ids), k = 0;
    free(f);
    if (__atomic_load_n(&C.noted, __ATOMIC_ACQUIRE) && !id_zero(xlate_base)) memcpy(out[k++], xlate_base, 32);
    for (int i = 0; i < n && k < IDS_MAX; i++) {
        int skip = !memcmp(ids[i], xlate_base, 32);
        for (int j = 0; j < C.n_dropped && !skip; j++) skip = !memcmp(ids[i], C.dropped[j], 32);
        if (!skip) memcpy(out[k++], ids[i], 32);
    }
    int len = snprintf(text, sizeof text, "# Translator identities this shader cache holds translations of, newest first\n");
    for (int i = 0; i < k; i++) {
        key_hex(out[i], text + len);
        len += 64;
        text[len++] = '\n';
    }
    mkdir_p(cache_dir);
    if (!plat_file_write_atomic(p, text, (size_t)len)) rt_log("gfx_shader: carry: cannot write %s", p);
    plat_mutex_unlock(&ids_lock);
}

static void ids_write_main(void *arg) {
    (void)arg;
    ids_write();
}

static void ids_note(void) {
    if (!C.on || plat_cas32(&C.noted, 1, 0)) return;
    if (!plat_thread_spawn(ids_write_main, NULL, "shader ids", 0)) ids_write();
}

static void child_stop(Svc *v) {
    plat_child_stop(v->child);
    v->child = NULL;
    v->in_len = 0;
}

static int child_start(Svc *v) {
    paths();
    if (!xlate_exe[0] || !file_exists(xlate_exe)) {
        rt_log("gfx_shader: translator not found (%s); set NX2SARIA_XLATE", xlate_exe[0] ? xlate_exe : "<none>");
        return 0;
    }
    uint32_t err = 0;
    v->child = plat_child_start(xlate_exe, "--serve", &err);
    if (!v->child) {
        rt_log("gfx_shader: cannot start %s (error %lu)", xlate_exe, (unsigned long)err);
        return 0;
    }
    v->in_len = 0;
    return 1;
}

static int child_exchange_raw(Svc *v, const char *line, char *reply, size_t reply_size) {
    if (!v->child && !child_start(v)) return 0;
    plat_inc64(&n_requests);
    size_t n = strlen(line);
    if (!plat_child_write(v->child, line, n)) return 0;
    if (!plat_child_write(v->child, "\n", 1)) return 0;
    for (;;) {
        char *nl = memchr(v->inbuf, '\n', v->in_len);
        if (nl) {
            size_t len = (size_t)(nl - v->inbuf), keep = len < reply_size - 1 ? len : reply_size - 1;
            memcpy(reply, v->inbuf, keep);
            while (keep && reply[keep - 1] == '\r') keep--;
            reply[keep] = 0;
            memmove(v->inbuf, nl + 1, v->in_len - len - 1);
            v->in_len -= len + 1;
            return 1;
        }
        if (v->in_len == sizeof v->inbuf) v->in_len = 0;
        long got = plat_child_read(v->child, v->inbuf + v->in_len, sizeof v->inbuf - v->in_len);
        if (got == -2) return -1;
        if (got <= 0) return 0;
        v->in_len += (size_t)got;
    }
}

static int child_exchange(Svc *v, const char *line, char *reply, size_t reply_size) {
    uint64_t p0 = perf_begin();
    int r = child_exchange_raw(v, line, reply, reply_size);
    perf_end(PERF_XLATE, p0);
    return r;
}

static int svc_runtime_limit(void) {
    static volatile int32_t n;
    if (!n) {
        const char *e = cfg_get("NX2SARIA_XLATE_PROCS");
        int v = e && *e ? atoi(e) : 2;
        plat_xchg32(&n, v < 1 ? 1 : v > MAX_SVC ? MAX_SVC : v);
    }
    return (int)n;
}

static int svc_used(void) {
    int used = 0;
    for (int i = 0; i < MAX_SVC; i++) used += S.s[i].busy || S.s[i].child;
    return used;
}

static Svc *svc_get(void) {
    plat_mutex_lock(&S.lock);
    if (!S.limit) S.limit = svc_runtime_limit();
    Svc *v;
    for (;;) {
        Svc *empty = NULL;
        v = NULL;
        for (int i = 0; i < MAX_SVC && !v; i++) {
            Svc *c = &S.s[i];
            if (!c->busy && c->child) v = c;
            else if (!c->busy && !empty) empty = c;
        }
        if (!v && empty && svc_used() < S.limit) v = empty;
        if (v) break;
        plat_cond_wait(&S.freed, &S.lock);
    }
    v->busy = 1;
    plat_mutex_unlock(&S.lock);
    return v;
}

static void svc_put(Svc *v) {
    plat_mutex_lock(&S.lock);
    int stop = v->child && svc_used() > S.limit;
    if (!stop) v->busy = 0;
    plat_mutex_unlock(&S.lock);
    if (stop) {
        child_stop(v);
        plat_mutex_lock(&S.lock);
        v->busy = 0;
        plat_mutex_unlock(&S.lock);
    }
    plat_cond_broadcast(&S.freed);
}

static void svc_set_limit(int n) {
    Svc *stop[MAX_SVC];
    int k = 0;
    plat_mutex_lock(&S.lock);
    S.limit = n < 1 ? 1 : n > MAX_SVC ? MAX_SVC : n;
    for (int i = 0, used = svc_used(); i < MAX_SVC && used > S.limit; i++)
        if (!S.s[i].busy && S.s[i].child) S.s[i].busy = 1, stop[k++] = &S.s[i], used--;
    plat_mutex_unlock(&S.lock);
    for (int i = 0; i < k; i++) child_stop(stop[i]);
    plat_mutex_lock(&S.lock);
    for (int i = 0; i < k; i++) stop[i]->busy = 0;
    plat_mutex_unlock(&S.lock);
    plat_cond_broadcast(&S.freed);
}

static int svc_request(const char *line, char *reply, size_t reply_size) {
    Svc *v = svc_get();
    int ok = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        int r = child_exchange(v, line, reply, reply_size);
        if (r > 0) {
            ok = 1;
            break;
        }
        child_stop(v);
        if (r < 0) {
            ok = -1;
            break;
        }
    }
    svc_put(v);
    return ok;
}

static void flight_enter(const uint8_t id[32]) {
    plat_mutex_lock(&F.lock);
    for (;;) {
        int busy = F.n == 2 * MAX_SVC;
        for (int i = 0; i < F.n && !busy; i++) busy = !memcmp(F.id[i], id, 32);
        if (!busy) break;
        plat_cond_wait(&F.done, &F.lock);
    }
    memcpy(F.id[F.n++], id, 32);
    plat_mutex_unlock(&F.lock);
}

static void flight_leave(const uint8_t id[32]) {
    plat_mutex_lock(&F.lock);
    for (int i = 0; i < F.n; i++)
        if (!memcmp(F.id[i], id, 32)) {
            memcpy(F.id[i], F.id[--F.n], 32);
            break;
        }
    plat_mutex_unlock(&F.lock);
    plat_cond_broadcast(&F.done);
}

static GfxShader *mem_find(const uint8_t key[32]) {
    plat_mutex_lock_shared(&cache_lock);
    GfxShader *s = buckets[(key[0] | key[1] << 8) % BUCKETS];
    while (s && memcmp(s->key, key, 32)) s = s->next;
    plat_mutex_unlock_shared(&cache_lock);
    return s;
}

static void shader_free(GfxShader *s) {
    for (int k = 0; k < GFX_RES_KINDS; k++) free(s->res[k]);
    spv_store_free(&s->spv);
    free(s->bind), free(s->reflection), free(s);
}

static void reflect(GfxShader *s);

static void carry_served(const GfxShader *s) {
    if (plat_inc64(&C.served) != 1) return;
    const uint8_t *id = C.prev[s->carried - 1];
    rt_log("gfx_shader: carry: serving translations of a previous translator (%02x%02x%02x%02x) where the current one has "
           "none yet; each keeps its translation for this session, and a background pass translates them again for the "
           "next start", id[0], id[1], id[2], id[3]);
}

static void zpass_kick(void);
static void pool_short_note(const GfxShader *s);

static GfxShader *mem_insert(GfxShader *s) {
    int added = 0;
    plat_mutex_lock(&cache_lock);
    GfxShader **head = &buckets[(s->key[0] | s->key[1] << 8) % BUCKETS], *r = *head;
    while (r && memcmp(r->key, s->key, 32)) r = r->next;
    if (r) {
        shader_free(s);
        s = r;
    } else {
        s->next = *head;
        *head = s;
        n_keys++;
        added = 1;
    }
    plat_mutex_unlock(&cache_lock);
    if (added && s->carried) carry_served(s);
    else if (added) ids_note();
    if (added) pool_short_note(s), zpass_kick();
    return s;
}

static void key_hex(const uint8_t key[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[i * 2] = hex[key[i] >> 4], out[i * 2 + 1] = hex[key[i] & 15];
    out[64] = 0;
}

static int reflection_valid(const char *res, uint64_t size) {
    if (strlen(res) != size) return 0;
    const char *b = res, *e = res + size;
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (e - b < 2 || *b != '{' || e[-1] != '}') return 0;
    const char *st = strstr(b, "\"stage\"");
    if (!st) return 0;
    st = strchr(st + 7, '"');
    return st && (!strncmp(st, "\"vs\"", 4) || !strncmp(st, "\"ps\"", 4) || !strncmp(st, "\"cs\"", 4) ||
                  !strncmp(st, "\"ts\"", 4) || !strncmp(st, "\"ms\"", 4));
}

static int spirv_complete(const uint32_t *w, uint64_t n) {
    uint64_t i = 5;
    while (i < n) {
        uint32_t len = w[i] >> 16;
        if (!len) return 0;
        i += len;
    }
    return i == n;
}

static void reflect_spirv(GfxShader *s);
static void reflect_stage(GfxShader *s);

static void spirv_seal(GfxShader *s) {
    const uint32_t *w = s->spv.plain;
    uint64_t n = s->spv.words;
    uint32_t cap = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        if (op == 71 && len >= 4 && w[i + 2] == 33) {
            if (s->nbind == cap) {
                uint32_t *b = realloc(s->bind, (cap = cap ? cap * 2 : 16) * sizeof *b);
                if (!b) rt_fatal("gfx_shader: out of memory");
                s->bind = b;
            }
            s->bind[s->nbind++] = w[i + 3];
        }
        i += len;
    }
    s->spv.used_ms = plat_now_ms();
}

static GfxShader *disk_load_from(const uint8_t key[32], const uint8_t *iid, int packed, int full) {
    static volatile int32_t n_bad;
    char hex[65], p[PATH_CAP + 80];
    key_hex(key, hex);
    uint64_t spv_size = 0, res_size = 0;
    snprintf(p, PATH_CAP + 80, "%s/%s.res.json", cache_dir, hex);
    uint8_t *res = packed ? pack_read(PACK_RES, key, &res_size) : read_file(p, &res_size);
    if (!res) return NULL;
    if (!reflection_valid((const char *)res, res_size)) {
        int32_t n = plat_inc32(&n_bad);
        if (n <= 3 || !(n & (n - 1))) {
            const char *st = strstr((const char *)res, "\"stage\"");
            char what[24] = "no stage field";
            if (st) snprintf(what, sizeof what, "%.16s", st + 8);
            rt_log("gfx_shader: %s.res.json is malformed (stage %s), translating again (%d so far)", hex, what, n);
        }
        free(res);
        return NULL;
    }
    snprintf(p, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
    uint8_t *spv = packed ? pack_read(PACK_SPV, key, &spv_size) : read_file(p, &spv_size);
    uint32_t magic = 0;
    if (spv && spv_size >= 20) memcpy(&magic, spv, 4);
    if (!spv || spv_size % 4 || magic != 0x07230203 || !spirv_complete((const uint32_t *)spv, spv_size / 4)) {
        free(res), free(spv);
        return NULL;
    }
    GfxShader *s = calloc(1, sizeof *s);
    memcpy(s->key, key, 32);
    if (iid) memcpy(s->iid, iid, 32);
    spv_store_fill(&s->spv, (uint32_t *)spv, spv_size / 4, 0);
    s->reflection = (char *)res;
    s->reflection_size = res_size;
    if (!full) {
        reflect_stage(s), reflect_spirv(s);
        return s;
    }
    reflect(s);
    if (s->stage == GFX_STAGE_FS) vk_hdr_shader_swap(&s->spv.plain, &s->spv.words);
    spirv_seal(s);
    return s;
}

const GfxShader *gfx_shader_own(const uint32_t *spirv, uint64_t words, const char *res_json) {
    if (words < 5 || spirv[0] != 0x07230203 || !spirv_complete(spirv, words) ||
        !reflection_valid(res_json, strlen(res_json)))
        return NULL;
    GfxShader *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    size_t rn = strlen(res_json);
    uint8_t *buf = malloc(words * 4 + rn);
    spv_store_fill(&s->spv, malloc(words * 4), words, 0);
    s->reflection = malloc(rn + 1);
    if (!buf || !s->spv.plain || !s->reflection) rt_fatal("gfx_shader: out of memory");
    memcpy(buf, spirv, words * 4), memcpy(buf + words * 4, res_json, rn);
    rt_sha256(buf, words * 4 + rn, s->key);
    free(buf);
    memcpy(s->spv.plain, spirv, words * 4), memcpy(s->reflection, res_json, rn + 1);
    s->reflection_size = rn;
    reflect(s);
    spirv_seal(s);
    return s;
}

static GfxShader *disk_load(const uint8_t key[32], const uint8_t *iid) {
    GfxShader *s = pack_n ? disk_load_from(key, iid, 1, 1) : NULL;
    return s ? s : disk_load_from(key, iid, 0, 1);
}

static int entry_present(const uint8_t key[32]) {
    if (pack_find(PACK_SPV, key) && pack_find(PACK_RES, key)) return 1;
    char hex[65], p[PATH_CAP + 80];
    key_hex(key, hex);
    snprintf(p, sizeof p, "%s/%s.spv", cache_dir, hex);
    if (!file_exists(p)) return 0;
    snprintf(p, sizeof p, "%s/%s.res.json", cache_dir, hex);
    return file_exists(p);
}

static const char *const tex_names[] = {NULL, "1d", "1darray", "buffer", "2d", "2darray", "2dms", "2dmsarray", "3d",
                                        "cube", "cubearray"};
static const char *const fmt_names[] = {
    NULL, "R8Unorm", "R8Snorm", "R8Uint", "R8Sint", "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint", "R32Float",
    "R32Uint", "R32Sint", "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint", "R16G16Float", "R16G16Unorm", "R16G16Snorm",
    "R16G16Uint", "R16G16Sint", "R32G32Float", "R32G32Uint", "R32G32Sint", "R8G8B8A8Unorm", "R8G8B8A8Snorm",
    "R8G8B8A8Uint", "R8G8B8A8Sint", "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm", "R16G16B16A16Uint",
    "R16G16B16A16Sint", "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint", "R10G10B10A2Unorm",
    "R10G10B10A2Uint", "R11G11B10Float"};
#define COUNT(a) ((int)(sizeof a / sizeof *a))

static const char *attr_name(uint8_t t) { return t == 1 ? "sint" : t == 2 ? "uint" : "float"; }

static const char *json_key(const char *o, const char *e, const char *name) {
    char pat[40];
    int n = snprintf(pat, sizeof pat, "\"%s\":", name);
    for (const char *p = o; p + n <= e; p++)
        if (*p == '"' && !memcmp(p, pat, (size_t)n)) {
            p += n;
            while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
            return p;
        }
    return NULL;
}

static long json_int(const char *o, const char *e, const char *name, long def) {
    const char *p = json_key(o, e, name);
    return p ? strtol(p, NULL, 10) : def;
}

static void json_str(const char *o, const char *e, const char *name, char *buf, size_t n) {
    const char *p = json_key(o, e, name);
    size_t k = 0;
    if (p && *p == '"')
        for (p++; p < e && *p != '"' && k + 1 < n; p++) buf[k++] = *p;
    buf[k] = 0;
}

static int json_objects(const char *js, const char *name, void (*fn)(GfxShader *, int, const char *, const char *),
                        GfxShader *s, int kind) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    if (!p || *p != '[') return 0;
    int n = 0;
    for (p++; p < end && *p != ']'; p++) {
        if (*p != '{') continue;
        const char *q = memchr(p, '}', (size_t)(end - p));
        if (!q) break;
        if (fn) fn(s, kind, p, q);
        n++;
        p = q;
    }
    return n;
}

static int fmt_code(const char *name) {
    for (int i = 1; i < COUNT(fmt_names); i++)
        if (!strcmp(fmt_names[i], name)) return i;
    return 0;
}

static int handle_slot(int kind, int cbuf_slot, int handle, int separate) {
    if (cbuf_slot >= 0 && cbuf_slot != 2) return -1;
    int bytes = (handle & 0x3fff) * 4;
    if (kind == GFX_RES_IMG) return bytes >= 0x120 && bytes < 0x168 ? (bytes - 0x120) / 8 : -1;
    if (separate) return bytes >= 0x168 && bytes < 0x568 ? (bytes - 0x168) / 8 : -1;
    return bytes >= 0x20 && bytes < 0x120 ? (bytes - 0x20) / 8 : -1;
}

static void add_res(GfxShader *s, int kind, const char *o, const char *e) {
    GfxShaderRes *r = &s->res[kind][s->nres[kind]++];
    r->set = 0;
    r->binding = (uint16_t)json_int(o, e, "binding", 0);
    if (kind == GFX_RES_UBO || kind == GFX_RES_SSBO) {
        r->slot = (int16_t)json_int(o, e, "slot", -1);
        r->count = 1;
        if (kind == GFX_RES_SSBO) {
            r->cbuf_slot = (int8_t)json_int(o, e, "cb_slot", -1);
            r->handle = (uint16_t)json_int(o, e, "cb_offset", 0);
            r->flags = (uint8_t)json_int(o, e, "flags", 0);
            r->ptr = (uint16_t)(json_int(o, e, "ptr", -1) + 1);
        }
        return;
    }
    char buf[32];
    json_str(o, e, "format", buf, sizeof buf);
    const char *sep = json_key(o, e, "separate");
    r->type = (uint16_t)json_int(o, e, "type", 0);
    r->format = (uint8_t)fmt_code(buf);
    r->flags = (uint8_t)json_int(o, e, "flags", 0);
    r->cbuf_slot = (int8_t)json_int(o, e, "cbuf_slot", -1);
    int handle = (int)json_int(o, e, "handle", 0);
    r->handle = (uint16_t)handle;
    r->count = (uint16_t)json_int(o, e, "array", 1);
    r->separate = sep && *sep == 't';
    r->pool = (uint8_t)json_int(o, e, "pool", 0);
    r->slot = (int16_t)handle_slot(kind, r->cbuf_slot, handle, r->separate);
}

enum { OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
       OP_TYPE_ARRAY = 28, OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_VARIABLE = 59, OP_DECORATE = 71,
       DEC_BUILTIN = 11, DEC_LOCATION = 30, DEC_COMPONENT = 31, SC_INPUT = 1, SC_OUTPUT = 3, EM_LOCAL_SIZE = 17 };

static void reflect_spirv(GfxShader *s) {
    const uint32_t *w = s->spv.plain;
    uint64_t n = s->spv.words;
    s->local[0] = s->local[1] = s->local[2] = 1;
    if (n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    int32_t *loc = calloc(bound, sizeof *loc);
    uint8_t *packed = calloc(bound, 1), *ttype = calloc(bound, 1), *tcomps = calloc(bound, 1), *tlocs = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof *pointee), *value = calloc(bound, sizeof *value);
    if (!loc || !packed || !ttype || !tcomps || !tlocs || !pointee || !value) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        uint32_t id = len > 1 ? a[0] : 0;
        if (op == OP_EXECUTION_MODE && len >= 6 && a[1] == EM_LOCAL_SIZE) {
            s->local[0] = a[2], s->local[1] = a[3], s->local[2] = a[4];
        } else if (op == OP_DECORATE && len >= 4 && id < bound) {
            if (a[1] == DEC_LOCATION) loc[id] = (int32_t)a[2] + 1;
            else if (a[1] == DEC_COMPONENT) packed[id] = 1;
        } else if (op == OP_TYPE_INT && len >= 4 && id < bound) {
            ttype[id] = a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT, tcomps[id] = 1, tlocs[id] = 1;
        } else if (op == OP_TYPE_FLOAT && len >= 3 && id < bound) {
            ttype[id] = GFX_TYPE_FLOAT, tcomps[id] = 1, tlocs[id] = 1;
        } else if (op == OP_TYPE_VECTOR && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = (uint8_t)a[2], tlocs[id] = 1;
        } else if (op == OP_CONSTANT && len == 4 && a[1] < bound) {
            value[a[1]] = a[2];
        } else if (op == OP_TYPE_ARRAY && len >= 4 && id < bound && a[1] < bound && a[2] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = tcomps[a[1]];
            uint64_t slots = (uint64_t)tlocs[a[1]] * value[a[2]];
            tlocs[id] = (uint8_t)(slots < 32 ? slots : 32);
        } else if (op == OP_TYPE_POINTER && len >= 4 && id < bound) {
            pointee[id] = a[2];
        } else if (op == OP_VARIABLE && len >= 4) {
            uint32_t type = a[0], var = a[1], sc = a[2];
            if ((sc == SC_INPUT || sc == SC_OUTPUT) && var < bound && type < bound && loc[var] > 0 && loc[var] <= 32) {
                int l = loc[var] - 1;
                uint32_t t = pointee[type] < bound ? pointee[type] : 0;
                GfxShaderIo io = {ttype[t], tcomps[t] ? tcomps[t] : 4, packed[var]};
                uint32_t slots = ((s->stage == GFX_STAGE_VS && sc == SC_OUTPUT) ||
                                  (s->stage == GFX_STAGE_FS && sc == SC_INPUT)) && tlocs[t] ? tlocs[t] : 1;
                for (uint32_t j = 0; j < slots && l + j < 32; j++) {
                    if (sc == SC_INPUT) s->in_mask |= 1u << (l + j), s->in[l + j] = io;
                    else s->out_mask |= 1u << (l + j), s->out[l + j] = io;
                }
            }
        }
        i += len;
    }
out:
    free(loc), free(packed), free(ttype), free(tcomps), free(tlocs), free(pointee), free(value);
}

enum { OP_COMPOSITE_CONSTRUCT = 80, OP_SAMPLE_EXPLICIT = 88, IO_GRAD = 4 };

static void patch_txd_transpose(GfxShader *s) {
    static int on = -1;
    static volatile int32_t n_logged;
    if (on < 0) {
        const char *e = cfg_get("NX2SARIA_TXD_TRANSPOSE");
        on = e && *e ? *e != '0' : rt_nvn_api_version[0] < 200;
    }
    uint32_t *w = s->spv.plain;
    uint64_t n = s->spv.words;
    if (!on || n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    uint64_t *cc = calloc(bound, sizeof *cc);
    int found = 0;
    if (!cc) return;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto out;
        if (op == OP_COMPOSITE_CONSTRUCT && len == 5 && w[i + 2] < bound) cc[w[i + 2]] = i;
        if (op == OP_SAMPLE_EXPLICIT && len >= 8 && w[i + 5] == IO_GRAD && w[i + 6] < bound && w[i + 7] < bound &&
            cc[w[i + 6]] && cc[w[i + 7]] && w[cc[w[i + 6]] + 1] == w[cc[w[i + 7]] + 1])
            found++;
        i += len;
    }
    if (!found) goto out;
    uint32_t *o = malloc((n + (uint64_t)found * 10) * sizeof *o);
    if (!o) goto out;
    uint64_t k = 0;
    uint32_t next = w[3];
    for (uint64_t i = 0; i < 5; i++) o[k++] = w[i];
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OP_SAMPLE_EXPLICIT && len >= 8 && w[i + 5] == IO_GRAD && w[i + 6] < bound && w[i + 7] < bound &&
            cc[w[i + 6]] && cc[w[i + 7]] && w[cc[w[i + 6]] + 1] == w[cc[w[i + 7]] + 1]) {
            const uint32_t *dx = w + cc[w[i + 6]], *dy = w + cc[w[i + 7]];
            uint32_t ndx = next++, ndy = next++;
            o[k++] = 5u << 16 | OP_COMPOSITE_CONSTRUCT, o[k++] = dx[1], o[k++] = ndx, o[k++] = dx[3], o[k++] = dy[3];
            o[k++] = 5u << 16 | OP_COMPOSITE_CONSTRUCT, o[k++] = dx[1], o[k++] = ndy, o[k++] = dx[4], o[k++] = dy[4];
            for (uint32_t j = 0; j < len; j++) o[k++] = w[i + j];
            o[k - len + 6] = ndx, o[k - len + 7] = ndy;
        } else {
            for (uint32_t j = 0; j < len; j++) o[k++] = w[i + j];
        }
        i += len;
    }
    o[3] = next;
    free(s->spv.plain);
    s->spv.plain = o, s->spv.words = k;
    if (plat_inc32(&n_logged) <= 4) rt_log("gfx_shader: %d gradient samples now read their derivatives per coordinate", found);
out:
    free(cc);
}

static volatile int32_t n_exact_shaders;
uint32_t gfx_shader_exact_seen(void) { return (uint32_t)__atomic_load_n(&n_exact_shaders, __ATOMIC_RELAXED); }

static void reflect_exact(GfxShader *s) {
    enum { X_FUNCTION = 54, X_TYPE_IMAGE = 25, X_TYPE_SAMPLED_IMAGE = 27, X_LOAD = 61, X_ACCESS_CHAIN = 65,
           X_IN_BOUNDS_ACCESS_CHAIN = 66, X_COMPOSITE_EXTRACT = 81, X_COPY_OBJECT = 83, X_SAMPLED_IMAGE = 86, X_FETCH = 95,
           X_IMAGE = 100, X_QUERY_SIZE_LOD = 103, X_QUERY_SIZE = 104, X_QUERY_LEVELS = 106, X_DEC_BINDING = 33 };
    static volatile int32_t n_pool_logged;
    const uint32_t *w = s->spv.plain;
    uint64_t n = s->spv.words, fn = 0;
    if (!s->nres[GFX_RES_TEX] || n < 5) return;
    int any = 0;
    for (uint64_t i = 5; i < n && !any;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len) break;
        any = op == X_FETCH || op == X_QUERY_SIZE || op == X_QUERY_LEVELS || op == X_QUERY_SIZE_LOD;
        i += len;
    }
    if (!any) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    uint8_t *arrayed = calloc(bound, 1), *need = calloc(bound, 1);
    uint32_t *bind = calloc(bound, sizeof *bind), *org = calloc(bound, sizeof *org), *query = calloc(bound, sizeof *query);
    int all = 0, queries = 0;
    if (!arrayed || !need || !bind || !org || !query) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i;
        if (op == X_FUNCTION && !fn) fn = i;
        #define ID(x) ((x) < bound)
        if (op == OP_DECORATE && len >= 4 && ID(a[1]) && a[2] == X_DEC_BINDING) bind[a[1]] = a[3] + 1;
        else if (op == X_TYPE_IMAGE && len >= 9 && ID(a[1])) arrayed[a[1]] = a[5] != 0;
        else if (op == X_TYPE_SAMPLED_IMAGE && len >= 3 && ID(a[1]) && ID(a[2])) arrayed[a[1]] = arrayed[a[2]];
        else if (op == OP_TYPE_ARRAY && len >= 4 && ID(a[1]) && ID(a[2])) arrayed[a[1]] = arrayed[a[2]];
        else if (op == OP_TYPE_POINTER && len >= 4 && ID(a[1]) && ID(a[3])) arrayed[a[1]] = arrayed[a[3]];
        else if (op == OP_VARIABLE && len >= 4 && ID(a[1]) && ID(a[2])) {
            arrayed[a[2]] = arrayed[a[1]];
            if (bind[a[2]]) org[a[2]] = a[2];
        } else if ((op == X_ACCESS_CHAIN || op == X_IN_BOUNDS_ACCESS_CHAIN || op == X_LOAD || op == X_COPY_OBJECT ||
                    op == X_SAMPLED_IMAGE || op == X_IMAGE) && len >= 4 && ID(a[2]) && ID(a[3])) {
            org[a[2]] = org[a[3]];
        } else if ((op == X_FETCH || op == X_QUERY_SIZE || op == X_QUERY_LEVELS || op == X_QUERY_SIZE_LOD) && len >= 4) {
            uint32_t var = ID(a[3]) ? org[a[3]] : 0;
            if (!var) all = 1;
            else if (op == X_QUERY_SIZE_LOD && arrayed[var] && ID(a[2])) query[a[2]] = var + 1, queries++;
            else need[var] = 1;
        }
        #undef ID
        i += len;
    }
    for (uint64_t i = fn; queries && fn && i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        int layer_count = op == X_COMPOSITE_EXTRACT && len == 5 && w[i + 4] == 2;
        int defines = op == X_QUERY_SIZE_LOD && len >= 4;
        for (uint32_t j = 1; j < len; j++) {
            uint32_t x = w[i + j];
            if (x >= bound || !query[x]) continue;
            if ((defines && j == 2) || (layer_count && j == 3)) continue;
            need[query[x] - 1] = 1;
        }
        i += len;
    }
    int pooled = 0, some = 0;
    for (int k = 0; k < s->nres[GFX_RES_TEX]; k++) {
        GfxShaderRes *r = &s->res[GFX_RES_TEX][k];
        int hit = 0;
        for (uint32_t id = 0; id < bound && !hit; id++) hit = need[id] && bind[id] == (uint32_t)r->binding + 1;
        if (r->pool) pooled |= hit;
        else r->exact = (uint8_t)(hit || all), some |= r->exact;
    }
    if (some) plat_inc32(&n_exact_shaders);
    if (pooled && plat_inc32(&n_pool_logged) <= 4)
        rt_log("gfx_shader: a shader reads a texture of the bindless pool by texel position or uses its size; with "
               "texture_detail below full such a texture may be read at the wrong level (vk_texture.c, texture detail)");
out:
    free(arrayed), free(need), free(bind), free(org), free(query);
}

int vk_caps_vec_index_bug(void);

enum { VX_TYPE_BOOL = 20, VX_TYPE_INT = 21, VX_TYPE_FLOAT = 22, VX_TYPE_VECTOR = 23, VX_TYPE_ARRAY = 28,
       VX_TYPE_RTARRAY = 29, VX_TYPE_STRUCT = 30, VX_TYPE_POINTER = 32, VX_CONSTANT = 43, VX_FUNCTION = 54,
       VX_VARIABLE = 59, VX_LOAD = 61, VX_ACCESS_CHAIN = 65, VX_IN_BOUNDS_ACCESS_CHAIN = 66, VX_COMPOSITE_EXTRACT = 81,
       VX_SELECT = 169, VX_IEQUAL = 170, VX_SC_UNIFORM = 2 };

static void patch_vec_index(GfxShader *s) {
    static int on = -1;
    static volatile int32_t n_logged;
    if (on < 0) {
        const char *e = cfg_get("NX2SARIA_VEC_INDEX_FIX");
        on = e && *e ? (strcmp(e, "auto") ? *e != '0' : vk_caps_vec_index_bug()) : 0;
        if (on) rt_log("gfx_shader: uniform vector components at computed indices are read whole and selected "
                       "(NX2SARIA_VEC_INDEX_FIX=0 turns it off)");
    }
    uint32_t *w = s->spv.plain;
    uint64_t n = s->spv.words;
    if (!on || n < 5) return;
    uint32_t bound = w[3];
    if (bound > (1u << 22)) return;
    uint64_t *def = calloc(bound, sizeof *def);
    uint32_t *ptr_v = calloc(bound, sizeof *ptr_v);
    uint32_t *target = calloc(bound, sizeof *target);
    uint32_t *acv = calloc(bound, sizeof *acv);
    if (!def || !ptr_v || !target || !acv) goto out;
    uint64_t first_fn = 0;
    uint32_t bool_t = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto out;
        if (op == VX_FUNCTION && !first_fn) first_fn = i;
        uint32_t rid = 0;
        switch (op) {
        case 3: case 4: case 5: case 6: case 8: case 10: case 14: case 15: case 16: case 17: case 56: case 62: case 63:
        case 71: case 72: case 73: case 74: case 75: case 99: case 224: case 225: case 228: case 246: case 247: case 248:
        case 249: case 250: case 251: case 252: case 253: case 254: case 255: case 317: case 330: case 331: case 332:
        case 4416: case 5294: case 5295: case 5632: case 5633: break;
        case 7: case 11:
        case VX_TYPE_BOOL: case VX_TYPE_INT: case VX_TYPE_FLOAT: case VX_TYPE_VECTOR: case VX_TYPE_ARRAY:
        case VX_TYPE_RTARRAY: case VX_TYPE_STRUCT: case VX_TYPE_POINTER: rid = len >= 2 ? w[i + 1] : 0; break;
        default:
            if (op >= 19 && op <= 39) rid = len >= 2 ? w[i + 1] : 0;
            else if (len >= 3) rid = w[i + 2];
        }
        if (op == VX_TYPE_BOOL && !bool_t) bool_t = w[i + 1];
        if (op == VX_TYPE_POINTER && len == 4 && w[i + 2] == VX_SC_UNIFORM && w[i + 3] < bound && !ptr_v[w[i + 3]])
            ptr_v[w[i + 3]] = w[i + 1];
        if (rid && rid < bound && !def[rid]) def[rid] = i;
        i += len;
    }
    if (!first_fn) goto out;
#define VX_OP(id) ((id) < bound && def[id] ? w[def[id]] & 0xffff : 0)
    int found = 0;
    uint32_t next = bound;
    uint32_t need_ptr[64], need_ptr_v[64], nneed = 0;
    uint32_t cint[8][4] = {{0}}, cint_t[8] = {0}, ncint = 0;
    for (uint64_t i = first_fn; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if ((op == VX_ACCESS_CHAIN || op == VX_IN_BOUNDS_ACCESS_CHAIN) && len >= 6) {
            uint32_t rt = w[i + 1], id = w[i + 2], base = w[i + 3], nidx = len - 4, last = w[i + len - 1];
            uint32_t vt = 0;
            if (VX_OP(rt) == VX_TYPE_POINTER && w[def[rt] + 2] == VX_SC_UNIFORM && VX_OP(base) == VX_VARIABLE &&
                VX_OP(last) != VX_CONSTANT && VX_OP(w[def[base] + 1]) == VX_TYPE_POINTER) {
                uint32_t t = w[def[w[def[base] + 1]] + 3];
                int ok = 1;
                for (uint32_t k = 0; k + 1 < nidx && ok; k++) {
                    uint32_t top = VX_OP(t), ix = w[i + 4 + k];
                    if (top == VX_TYPE_STRUCT) {
                        if (VX_OP(ix) != VX_CONSTANT) { ok = 0; break; }
                        uint32_t m = w[def[ix] + 3], slen = w[def[t]] >> 16;
                        if (2 + m >= slen) { ok = 0; break; }
                        t = w[def[t] + 2 + m];
                    } else if (top == VX_TYPE_ARRAY || top == VX_TYPE_RTARRAY) {
                        t = w[def[t] + 2];
                    } else ok = 0;
                }
                uint32_t sc = w[def[rt] + 3];
                if (ok && VX_OP(t) == VX_TYPE_VECTOR && w[def[t] + 2] == sc && w[def[t] + 3] >= 2 && w[def[t] + 3] <= 4 &&
                    def[last] && (w[def[last]] >> 16) >= 3) {
                    uint32_t it = w[def[last] + 1];
                    if (VX_OP(it) == VX_TYPE_INT) vt = t;
                    if (vt && !ptr_v[vt]) {
                        if (nneed == 64) vt = 0;
                        else need_ptr[nneed] = next++, need_ptr_v[nneed++] = vt, ptr_v[vt] = need_ptr[nneed - 1];
                    }
                    if (vt) {
                        uint32_t c = 0;
                        while (c < ncint && cint_t[c] != it) c++;
                        if (c == ncint) {
                            if (ncint == 8) vt = 0;
                            else cint_t[ncint++] = it;
                        }
                    }
                }
            }
            if (vt && id < bound) target[id] = next++, acv[id] = vt, found++;
        }
        i += len;
    }
    if (!found) goto out;
    for (uint64_t i = 5; i < first_fn;) {
        uint32_t len = w[i] >> 16;
        if ((w[i] & 0xffff) == VX_CONSTANT && len == 4 && w[i + 3] < 4)
            for (uint32_t c = 0; c < ncint; c++)
                if (cint_t[c] == w[i + 1] && !cint[c][w[i + 3]]) cint[c][w[i + 3]] = w[i + 2];
        i += len;
    }
    uint32_t newc[8][4] = {{0}};
    for (uint32_t c = 0; c < ncint; c++)
        for (int v = 0; v < 4; v++)
            if (!cint[c][v]) cint[c][v] = newc[c][v] = next++;
    uint32_t new_bool = bool_t ? 0 : next++;
    if (!bool_t) bool_t = new_bool;
    uint64_t loads = 0;
    for (uint64_t i = first_fn; i < n;) {
        uint32_t len = w[i] >> 16;
        if ((w[i] & 0xffff) == VX_LOAD && len >= 4 && w[i + 3] < bound && target[w[i + 3]]) loads++;
        i += len;
    }
    uint32_t *o = malloc((n + 64 * 4 + 32 * 4 + 4 + (uint64_t)found * 16 + loads * 64) * sizeof *o);
    if (!o) goto out;
    uint64_t k = 0;
    for (uint64_t i = 0; i < 5; i++) o[k++] = w[i];
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (i == first_fn) {
            for (uint32_t j = 0; j < nneed; j++)
                o[k++] = 4u << 16 | VX_TYPE_POINTER, o[k++] = need_ptr[j], o[k++] = VX_SC_UNIFORM, o[k++] = need_ptr_v[j];
            if (new_bool) o[k++] = 2u << 16 | VX_TYPE_BOOL, o[k++] = new_bool;
            for (uint32_t c = 0; c < ncint; c++)
                for (uint32_t v = 0; v < 4; v++)
                    if (newc[c][v]) o[k++] = 4u << 16 | VX_CONSTANT, o[k++] = cint_t[c], o[k++] = newc[c][v], o[k++] = v;
        }
        for (uint32_t j = 0; j < len; j++) o[k++] = w[i + j];
        if ((op == VX_ACCESS_CHAIN || op == VX_IN_BOUNDS_ACCESS_CHAIN) && len >= 6 && w[i + 2] < bound && target[w[i + 2]]) {
            o[k++] = (len - 1) << 16 | op, o[k++] = ptr_v[acv[w[i + 2]]], o[k++] = target[w[i + 2]];
            for (uint32_t j = 3; j < len - 1; j++) o[k++] = w[i + j];
        } else if (op == VX_LOAD && len >= 4 && w[i + 3] < bound && target[w[i + 3]]) {
            k -= len;
            uint32_t st = w[i + 1], res = w[i + 2], ac = w[i + 3], vt = acv[ac], nc = w[def[vt] + 3];
            uint32_t idx = w[def[ac] + (w[def[ac]] >> 16) - 1], it = w[def[idx] + 1], c = 0;
            while (cint_t[c] != it) c++;
            uint32_t vec = next++, e[4], sel = 0;
            o[k++] = 4u << 16 | VX_LOAD, o[k++] = vt, o[k++] = vec, o[k++] = target[ac];
            for (uint32_t m = 0; m < nc; m++)
                e[m] = next++, o[k++] = 5u << 16 | VX_COMPOSITE_EXTRACT, o[k++] = st, o[k++] = e[m], o[k++] = vec, o[k++] = m;
            sel = e[0];
            for (uint32_t m = 1; m < nc; m++) {
                uint32_t eq = next++, r = m + 1 == nc ? res : next++;
                o[k++] = 5u << 16 | VX_IEQUAL, o[k++] = bool_t, o[k++] = eq, o[k++] = idx, o[k++] = cint[c][m];
                o[k++] = 6u << 16 | VX_SELECT, o[k++] = st, o[k++] = r, o[k++] = eq, o[k++] = e[m], o[k++] = sel;
                sel = r;
            }
        }
        i += len;
    }
    o[3] = next;
    free(s->spv.plain);
    s->spv.plain = o, s->spv.words = k;
    if (plat_inc32(&n_logged) <= 4)
        rt_log("gfx_shader: %d uniform vector component reads at computed indices now select the component", found);
#undef VX_OP
out:
    free(def), free(ptr_v), free(target), free(acv);
}

static void reflect_palette(GfxShader *s) {
    enum { P_COMPOSITE_EXTRACT = 81, P_COPY_OBJECT = 83, P_LOAD = 61, P_STORE = 62, P_ACCESS_CHAIN = 65,
           P_IN_BOUNDS_ACCESS_CHAIN = 66, P_UCONVERT = 113, P_SCONVERT = 114, P_BITCAST = 124, P_IADD = 128,
           P_ISUB = 130, P_IMUL = 132, P_SHR = 194, P_SAR = 195, P_SHL = 196, P_OR = 197, P_AND = 199,
           P_FUNCTION = 54, P_DEC_BINDING = 33, P_SC_UNIFORM = 2, P_SC_STORAGE = 12 };
    const uint32_t *w = s->spv.plain;
    uint64_t n = s->spv.words, fn = 0;
    s->pal_stride = 0, s->pal_slot = -1;
    if (s->stage != GFX_STAGE_VS || n < 5) return;
    int any = 0;
    for (int l = 0; l < 32 && !any; l++) any = (s->in_mask >> l & 1) && s->in[l].type != GFX_TYPE_FLOAT;
    if (!any) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    uint8_t *isint = calloc(bound, 1), *con = calloc(bound, 1), *loc = calloc(bound, 1), *block = calloc(bound, 1);
    uint8_t *joint = calloc(bound, 1), *scaled = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof *pointee), *bind = calloc(bound, sizeof *bind);
    uint32_t found = 0, found_bind = 0, found_joints = 0;
    int two = 0;
    if (!isint || !con || !loc || !block || !joint || !scaled || !pointee || !bind) goto out;
    #define PID(x) ((x) < bound)
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto out;
        const uint32_t *a = w + i;
        if (op == P_FUNCTION) {
            fn = i;
            break;
        }
        if (op == OP_DECORATE && len >= 4 && PID(a[1])) {
            if (a[2] == DEC_LOCATION && a[3] < 32) loc[a[1]] = (uint8_t)(a[3] + 1);
            else if (a[2] == P_DEC_BINDING) bind[a[1]] = a[3] + 1;
        } else if (op == OP_TYPE_INT && len >= 4 && PID(a[1])) {
            isint[a[1]] = 1;
        } else if (op == OP_TYPE_VECTOR && len >= 4 && PID(a[1]) && PID(a[2])) {
            isint[a[1]] = isint[a[2]];
        } else if (op == OP_TYPE_POINTER && len >= 4 && PID(a[1])) {
            pointee[a[1]] = a[3];
        } else if (op == OP_CONSTANT && len == 4 && PID(a[1]) && PID(a[2]) && isint[a[1]] &&
                   (a[3] == 6 || a[3] == 48 || a[3] == 64)) {
            con[a[2]] = (uint8_t)(a[3] + 1);
        } else if (op == OP_VARIABLE && len >= 4 && PID(a[1]) && PID(a[2])) {
            uint32_t t = pointee[a[1]];
            if (a[3] == SC_INPUT && loc[a[2]] && PID(t) && isint[t]) joint[a[2]] = loc[a[2]];
            else if ((a[3] == P_SC_UNIFORM || a[3] == P_SC_STORAGE) && bind[a[2]] > 1) block[a[2]] = 1;
        }
        i += len;
    }
    if (!fn) goto out;
    #define PJOINT(to, from) do { if (!joint[to] && joint[from]) joint[to] = joint[from], changed = 1; } while (0)
    #define PSCALED(to, from) \
        do { if (!scaled[to] && scaled[from]) scaled[to] = scaled[from], joint[to] = joint[from], changed = 1; } while (0)
    for (int pass = 0, changed = 1; pass < 4 && changed; pass++) {
        changed = 0;
        for (uint64_t i = fn; i < n;) {
            uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
            if (!len || i + len > n) goto out;
            const uint32_t *a = w + i;
            if ((op == P_ACCESS_CHAIN || op == P_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 && PID(a[2]) && PID(a[3])) {
                PJOINT(a[2], a[3]);
                for (uint32_t k = 4; block[a[3]] && k < len; k++) {
                    if (!PID(a[k]) || !scaled[a[k]]) continue;
                    if (found && found_bind != bind[a[3]]) two = 1;
                    found = scaled[a[k]], found_bind = bind[a[3]];
                    if (joint[a[k]]) found_joints |= 1u << (joint[a[k]] - 1);
                }
            } else if ((op == P_LOAD || op == P_BITCAST || op == P_UCONVERT || op == P_SCONVERT || op == P_COPY_OBJECT ||
                        op == P_COMPOSITE_EXTRACT) && len >= 4 && PID(a[2]) && PID(a[3])) {
                PJOINT(a[2], a[3]);
                PSCALED(a[2], a[3]);
            } else if (op == P_STORE && len >= 3 && PID(a[1]) && PID(a[2]) && !block[a[1]]) {
                PJOINT(a[1], a[2]);
                PSCALED(a[1], a[2]);
            } else if (op == P_IMUL && len >= 5 && PID(a[2]) && PID(a[3]) && PID(a[4])) {
                for (int k = 0; k < 2; k++) {
                    uint32_t x = a[3 + k], c = a[4 - k];
                    if (!scaled[a[2]] && joint[x] && !scaled[x] && (con[c] == 49 || con[c] == 65))
                        scaled[a[2]] = (uint8_t)(con[c] - 1), joint[a[2]] = joint[x], changed = 1;
                }
            } else if (op == P_SHL && len >= 5 && PID(a[2]) && PID(a[3]) && PID(a[4])) {
                if (!scaled[a[2]] && joint[a[3]] && !scaled[a[3]] && con[a[4]] == 7)
                    scaled[a[2]] = 64, joint[a[2]] = joint[a[3]], changed = 1;
            } else if ((op == P_IADD || op == P_ISUB || op == P_SHR || op == P_SAR || op == P_AND || op == P_OR) &&
                       len >= 5 && PID(a[2]) && PID(a[3]) && PID(a[4])) {
                PSCALED(a[2], a[3]);
                PSCALED(a[2], a[4]);
            }
            i += len;
        }
    }
    #undef PJOINT
    #undef PSCALED
    #undef PID
    if (!found || two || !found_joints) goto out;
    for (int kind = GFX_RES_UBO; kind <= GFX_RES_SSBO; kind++)
        for (int i = 0; i < s->nres[kind]; i++)
            if ((uint32_t)s->res[kind][i].binding + 1 == found_bind && s->res[kind][i].slot >= 0) {
                s->pal_stride = (uint8_t)found, s->pal_storage = kind == GFX_RES_SSBO;
                s->pal_slot = s->res[kind][i].slot, s->pal_joints = found_joints;
            }
out:
    free(isint), free(con), free(loc), free(block), free(joint), free(scaled), free(pointee), free(bind);
}

static void reflect_stage(GfxShader *s) {
    const char *js = s->reflection;
    char stage[8];
    json_str(js, js + strlen(js), "stage", stage, sizeof stage);
    s->stage = !strcmp(stage, "cs") ? GFX_STAGE_CS : !strcmp(stage, "ps") ? GFX_STAGE_FS : !strcmp(stage, "ts") ? GFX_STAGE_TS
             : !strcmp(stage, "ms") ? GFX_STAGE_MS : GFX_STAGE_VS;
}

static void reflect(GfxShader *s) {
    static const char *const kinds[GFX_RES_KINDS] = {"cbufs", "sbufs", "textures", "images"};
    const char *js = s->reflection;
    reflect_stage(s);
    s->mvprev_banks = (uint32_t)json_int(js, js + strlen(js), "mvprev_banks", 0);
    s->omap = s->stage == GFX_STAGE_FS ? (uint32_t)json_int(js, js + strlen(js), "frag_out_map", 0) : 0;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        int count = json_objects(js, kinds[k], NULL, s, k);
        s->res[k] = calloc(count ? count : 1, sizeof *s->res[k]);
        json_objects(js, kinds[k], add_res, s, k);
    }
    reflect_spirv(s);
    reflect_exact(s);
    reflect_palette(s);
    patch_txd_transpose(s);
    patch_vec_index(s);
}

static uint32_t json_slot_mask(const char *js, const char *name) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    uint32_t m = 0;
    if (!p || *p != '[') return 0;
    for (p++; p < end && *p != ']';) {
        char *q;
        long v = strtol(p, &q, 10);
        if (q == p) {
            p++;
            continue;
        }
        if (v >= 0 && v < 32) m |= 1u << v;
        p = q;
    }
    return m;
}

static UseMask use_of(const GfxShader *s) {
    UseMask m = {json_slot_mask(s->reflection, "tex_queried") | json_slot_mask(s->reflection, "img_queried"), 0, 0};
    if (s->stage == GFX_STAGE_VS) m.attrs = s->in_mask;
    else if (s->stage == GFX_STAGE_FS) m.fouts = s->out_mask & 0xff;
    return m;
}

static void use_name_id(const uint8_t id[32], const uint8_t h[32], uint8_t name[32]) {
    uint8_t buf[4 + 32 + 32];
    memcpy(buf, "TKSU", 4), memcpy(buf + 4, id, 32), memcpy(buf + 36, h, 32);
    rt_sha256(buf, sizeof buf, name);
}

static void use_name(const uint8_t h[32], uint8_t name[32]) { use_name_id(xlate_id, h, name); }

static void use_path_of(const uint8_t name[32], char *p) {
    char hex[65];
    key_hex(name, hex);
    snprintf(p, PATH_CAP + 80, "%s/%s.use", cache_dir, hex);
}

static void use_path(const uint8_t h[32], char *p) {
    uint8_t name[32];
    use_name(h, name);
    use_path_of(name, p);
}

#define USE_FILE_BUCKETS 65536
typedef struct UseFile { struct UseFile *next; uint8_t name[32]; UseMask m; } UseFile;
static UseFile *use_files[USE_FILE_BUCKETS];
static volatile int32_t use_files_ready;
static volatile int64_t use_files_n;

static UseFile **use_file_slot(const uint8_t name[32]) {
    UseFile **e = &use_files[(name[0] | name[1] << 8) % USE_FILE_BUCKETS];
    while (*e && memcmp((*e)->name, name, 32)) e = &(*e)->next;
    return e;
}

static int hex_byte(const char *c) {
    int v = 0;
    for (int i = 0; i < 2; i++) {
        int d = isdigit((unsigned char)c[i]) ? c[i] - '0' : c[i] >= 'a' && c[i] <= 'f' ? c[i] - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

typedef struct { int n, bad; } UsePreload;

static int use_preload_file(const char *name, void *ctx) {
    UsePreload *st = ctx;
    char p[PATH_CAP + 80];
    UseFile e = {0};
    int ok = strlen(name) == 68;
    for (int i = 0; ok && i < 32; i++) {
        int v = hex_byte(name + 2 * i);
        ok = v >= 0, e.name[i] = (uint8_t)v;
    }
    uint64_t len = 0;
    uint32_t *u = NULL;
    if (ok) {
        snprintf(p, sizeof p, "%s/%s", cache_dir, name);
        u = (uint32_t *)read_file(p, &len);
        ok = u && len == 20 && u[0] == USE_MAGIC && u[1] == USE_VERSION;
    }
    UseFile **slot = ok ? use_file_slot(e.name) : NULL, *x;
    if (slot && !*slot && (x = malloc(sizeof *x))) {
        e.m.slots = u[2], e.m.attrs = u[3], e.m.fouts = u[4];
        *x = e, *slot = x, st->n++;
    } else if (!ok) {
        st->bad++;
    }
    free(u);
    return 1;
}

static void use_preload(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint64_t t0 = plat_now_ns();
    int n = 0, bad = 0;
    for (uint32_t i = 0; i < pack_n; i++) {
        uint32_t u[5];
        UseFile **slot, *x;
        if (pack_ix[i].kind != PACK_USE) continue;
        if (pack_ix[i].size != sizeof u || !pack_read_at(pack_ix[i].offset, u, sizeof u) || u[0] != USE_MAGIC ||
            u[1] != USE_VERSION) {
            bad++;
            continue;
        }
        if (!*(slot = use_file_slot(pack_ix[i].name)) && (x = malloc(sizeof *x))) {
            memcpy(x->name, pack_ix[i].name, 32);
            x->next = NULL, x->m.slots = u[2], x->m.attrs = u[3], x->m.fouts = u[4];
            *slot = x, n++;
        }
    }
    UsePreload st = {0};
    plat_file_list(cache_dir, ".use", use_preload_file, &st);
    n += st.n, bad += st.bad;
    __atomic_store_n(&use_files_n, (int64_t)n, __ATOMIC_RELAXED);
    plat_xchg32(&use_files_ready, 1);
    rt_log("gfx_shader: %d use masks read in %.0f ms on a background thread (%d unreadable)", n,
           (plat_now_ns() - t0) / 1e6, bad);
    return;
}

static void use_preload_start(void) {
    if (!cache_dir[0]) return;
    plat_thread_spawn(use_preload, NULL, "use preload", 0);
}

static UseEnt **use_slot(const uint8_t h[32]) {
    UseEnt **e = &use_buckets[(h[0] | h[1] << 8) % BUCKETS];
    while (*e && memcmp((*e)->code_hash, h, 32)) e = &(*e)->next;
    return e;
}

static void use_put(const uint8_t h[32], int known, const UseMask *m) {
    UseEnt **e = use_slot(h);
    if (!*e && !(*e = calloc(1, sizeof **e))) return;
    memcpy((*e)->code_hash, h, 32);
    if (known) (*e)->known = 1, (*e)->m = *m;
}

static int use_find(const uint8_t h[32], UseMask *m) {
    plat_mutex_lock_shared(&cache_lock);
    UseEnt *e = *use_slot(h);
    int seen = e != NULL, known = e && e->known;
    if (known) *m = e->m;
    plat_mutex_unlock_shared(&cache_lock);
    if (seen) return known;
    uint8_t name[32];
    use_name(h, name);
    if (plat_cas32(&use_files_ready, 0, 0)) {
        UseFile *u = *use_file_slot(name);
        if ((known = u != NULL)) *m = u->m;
    } else {
        char p[PATH_CAP + 80];
        use_path_of(name, p);
        uint64_t n = 0;
        uint32_t *f = (uint32_t *)pack_read(PACK_USE, name, &n);
        if (!f) f = (uint32_t *)read_file(p, &n);
        known = f && n == 20 && f[0] == USE_MAGIC && f[1] == USE_VERSION;
        if (known) m->slots = f[2], m->attrs = f[3], m->fouts = f[4];
        free(f);
    }
    plat_mutex_lock(&cache_lock);
    use_put(h, known, m);
    plat_mutex_unlock(&cache_lock);
    return known;
}

static int cache_flush(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("SHADER_CACHE_FLUSH", "0") == 1;
    return on;
}

static void use_store(const uint8_t h[32], const UseMask *m) {
    char p[PATH_CAP + 80];
    use_path(h, p);
    uint32_t f[5] = {USE_MAGIC, USE_VERSION, m->slots, m->attrs, m->fouts};
    if (cache_flush()) plat_file_write_atomic(p, f, sizeof f);
    else plat_file_write_cached(p, f, sizeof f);
    plat_mutex_lock(&cache_lock);
    use_put(h, 1, m);
    plat_mutex_unlock(&cache_lock);
}

static void put_u32(uint8_t **p, uint32_t v) { memcpy(*p, &v, 4), *p += 4; }

static size_t pack_answers(uint8_t *o, const GfxShaderAnswers *a) {
    uint8_t *p = o;
    GfxShaderAnswers zero = {0};
    if (!a) a = &zero;
    memcpy(p, a->tex_type, 32), p += 32;
    memcpy(p, a->img_type, 32), p += 32;
    memcpy(p, a->img_format, 32), p += 32;
    memcpy(p, a->attr_type, 32), p += 32;
    memcpy(p, a->frag_out_type, 8), p += 8;
    for (int i = 0; i < 3; i++) *p++ = (uint8_t)a->local[i], *p++ = (uint8_t)(a->local[i] >> 8);
    put_u32(&p, a->shared_bytes);
    *p++ = (uint8_t)(a->depth_minus_w | (a->mvprev ? 0x80 : 0));
    return (size_t)(p - o);
}

static int build_code_hash(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                           const uint8_t *c1, uint32_t c1_len, uint8_t hash[32]) {
    size_t total = 4 + CONTROL_SIZE + 8 + (size_t)code_size + 4 + c1_len;
    uint8_t *buf = malloc(total), *p = buf;
    if (!buf) return 0;
    put_u32(&p, (uint32_t)stage);
    memcpy(p, control, CONTROL_SIZE), p += CONTROL_SIZE;
    memcpy(p, &code_size, 8), p += 8;
    memcpy(p, code, (size_t)code_size), p += code_size;
    put_u32(&p, c1_len);
    if (c1_len) memcpy(p, c1, c1_len);
    rt_sha256(buf, total, hash);
    free(buf);
    return 1;
}

static void mask_answers(const GfxShaderAnswers *a, const UseMask *m, GfxShaderAnswers *o) {
    memset(o, 0, sizeof *o);
    if (!a) return;
    for (int i = 0; i < 32; i++) {
        if (m->slots >> i & 1) o->tex_type[i] = a->tex_type[i], o->img_type[i] = a->img_type[i], o->img_format[i] = a->img_format[i];
        if (m->attrs >> i & 1) o->attr_type[i] = a->attr_type[i];
    }
    for (int i = 0; i < 8; i++)
        if (m->fouts >> i & 1) o->frag_out_type[i] = a->frag_out_type[i];
    memcpy(o->local, a->local, sizeof o->local);
    o->shared_bytes = a->shared_bytes;
    o->mvprev = a->mvprev;
}

static void key_of_input_id(const uint8_t id[32], const uint8_t iid[32], uint8_t key[32]) {
    uint8_t buf[8 + 32 + 32], *p = buf;
    memcpy(p, "TKSK", 4), p += 4;
    put_u32(&p, KEY_VERSION);
    memcpy(p, id, 32), p += 32;
    memcpy(p, iid, 32), p += 32;
    rt_sha256(buf, (size_t)(p - buf), key);
}

static void key_of_input(const uint8_t iid[32], uint8_t key[32]) { key_of_input_id(xlate_id, iid, key); }

static void iid_of(const uint8_t code_hash[32], const GfxShaderAnswers *a, const UseMask *m, uint8_t iid[32]) {
    GfxShaderAnswers masked;
    uint8_t buf[8 + 32 + 256], *p = buf;
    memcpy(p, "TKSI", 4), p += 4;
    put_u32(&p, IN_VERSION);
    memcpy(p, code_hash, 32), p += 32;
    mask_answers(a, m, &masked);
    p += pack_answers(p, &masked);
    rt_sha256(buf, (size_t)(p - buf), iid);
}

static void build_key(const uint8_t code_hash[32], const GfxShaderAnswers *a, const UseMask *m, uint8_t key[32],
                      uint8_t iid[32]) {
    uint8_t buf[8 + 32 + 32 + 256], *p = buf;
    if (m) {
        iid_of(code_hash, a, m, iid);
        key_of_input(iid, key);
        return;
    }
    memcpy(p, "TKSP", 4), p += 4;
    put_u32(&p, KEY_VERSION);
    memcpy(p, xlate_id, 32), p += 32;
    memcpy(p, code_hash, 32), p += 32;
    p += pack_answers(p, a);
    rt_sha256(buf, (size_t)(p - buf), key);
}

static size_t build_request(char *o0, const char *hex, const char *stage, const char *code_path, const uint8_t *c1,
                            uint32_t c1_len, const GfxShaderAnswers *a, const uint8_t *control, uint8_t *options_hash) {
    char *o = o0;
    o += sprintf(o, "{\"job\":{\"key\":\"");
    json_esc(&o, hex);
    o += sprintf(o, "\",\"code\":\"");
    json_esc(&o, code_path);
    o += sprintf(o, "\",");
    const char *options = o;
    o += sprintf(o, "\"stage\":\"%s\",\"debug\":false,\"c1\":\"", stage);
    for (uint32_t i = 0; i < c1_len; i++) o += sprintf(o, "%02x", c1[i]);
    o += sprintf(o, "\"");
    if (a) {
        const char *sep = "";
        o += sprintf(o, ",\"tex\":{");
        for (int i = 0; i < 32; i++)
            if (a->tex_type[i] && a->tex_type[i] < COUNT(tex_names)) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, tex_names[a->tex_type[i]]), sep = ",";
        sep = "";
        o += sprintf(o, "},\"img\":{");
        for (int i = 0; i < 32; i++)
            if (a->img_type[i] && a->img_type[i] < COUNT(tex_names)) {
                const char *f = a->img_format[i] && a->img_format[i] < COUNT(fmt_names) ? fmt_names[a->img_format[i]] : "R8G8B8A8Unorm";
                o += sprintf(o, "%s\"%d\":{\"type\":\"%s\",\"fmt\":\"%s\"}", sep, i, tex_names[a->img_type[i]], f), sep = ",";
            }
        sep = "";
        o += sprintf(o, "},\"attr\":{");
        for (int i = 0; i < 32; i++)
            if (a->attr_type[i]) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, attr_name(a->attr_type[i])), sep = ",";
        sep = "";
        o += sprintf(o, "},\"fout\":{");
        for (int i = 0; i < 8; i++)
            if (a->frag_out_type[i]) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, attr_name(a->frag_out_type[i])), sep = ",";
        uint32_t shared = a->shared_bytes, magic0;
        memcpy(&magic0, control, 4);
        if (!shared && magic0 == 0x98761234u && !strcmp(stage, "cs")) memcpy(&shared, control + 0x64, 4);
        o += sprintf(o, "},\"shared\":%u", shared);
        uint32_t local = 0;
        if (magic0 == 0x98761234u && !strcmp(stage, "cs")) memcpy(&local, control + 0x68, 4);
        if (local && local <= (1u << 20)) o += sprintf(o, ",\"local_mem\":%u", local);
    }
    if (!strcmp(stage, "cs")) {
        uint32_t l[3] = {1, 1, 1};
        if (a && (a->local[0] | a->local[1] | a->local[2])) l[0] = a->local[0], l[1] = a->local[1], l[2] = a->local[2];
        else memcpy(l, control + (*(const uint32_t *)control == 0x98761234u ? 0x58 : 0x748), 12);
        if (!(l[0] | l[1] | l[2])) l[0] = l[1] = l[2] = 1;
        o += sprintf(o, ",\"local\":[%u,%u,%u]", l[0], l[1], l[2]);
    }
    if (!strcmp(stage, "ps") && (control[0x718] & 1)) o += sprintf(o, ",\"early_fragment_tests\":true");
    int si = !strcmp(stage, "vs") ? 0 : !strcmp(stage, "cs") || !strcmp(stage, "ts") || !strcmp(stage, "ms") ? 2 : 1;
    if (!host_f64) o += sprintf(o, ",\"softfloat64\":true");
    if (a && a->mvprev && !strcmp(stage, "vs")) o += sprintf(o, ",\"mvprev\":true");
    if (host_sbuf) o += sprintf(o, ",\"sbuf_align\":%u", host_sbuf);
    if (host_sg[si] != 32) o += sprintf(o, ",\"subgroup\":%u", host_sg[si]);
    if (host_gather) o += sprintf(o, ",\"gather_bias\":%u", host_gather);
    if (host_zero_locals) o += sprintf(o, ",\"zero_locals\":true");
    if (host_bda) o += sprintf(o, ",\"bda\":true");
    uint32_t magic, nvn2_size;
    memcpy(&magic, control, 4), memcpy(&nvn2_size, control + 0x38, 4);
    if (magic == 0x98761234u) {
        o += sprintf(o, ",\"sm86\":true,\"size\":%u", nvn2_size);
        if (!strcmp(stage, "ts") || !strcmp(stage, "ms")) {
            uint32_t w58, w5c, w60;
            memcpy(&w58, control + 0x58, 4), memcpy(&w5c, control + 0x5c, 4), memcpy(&w60, control + 0x60, 4);
            o += sprintf(o, ",\"mesh\":{\"stage\":%d,\"threads\":%u,\"payload\":%u,\"maxv\":%u,\"maxp\":%u,\"topo\":%u,\"task\":%s}",
                         !strcmp(stage, "ts") ? 6 : 7, w58 & 0xffff, (w58 >> 24) * 256, w60 & 0xffff, w60 >> 16, w5c >> 24,
                         (w5c >> 16 & 0xff) ? "true" : "false");
        }
        if (!strcmp(stage, "cs")) {
            uint16_t end;
            memcpy(&end, control + 0xba, 2);
            int ntex = control[0xb3], nimg = control[0xb6];
            if (end >= 8 * (ntex + nimg)) o += sprintf(o, ",\"nvn2_table\":[%d,%d,%d]", end - 8 * (ntex + nimg), ntex, nimg);
        }
    }
    if (options_hash) rt_sha256(options, (size_t)(o - options), options_hash);
    o += sprintf(o, "},\"out\":\"");
    json_esc(&o, cache_dir);
    o += sprintf(o, "\"}");
    return (size_t)(o - o0);
}

static int use_lookup(const uint8_t id[32], const uint8_t h[32], UseMask *m) {
    uint8_t name[32];
    use_name_id(id, h, name);
    if (plat_cas32(&use_files_ready, 0, 0)) {
        UseFile *u = *use_file_slot(name);
        if (u) *m = u->m;
        return u != NULL;
    }
    char p[PATH_CAP + 80];
    uint64_t n = 0;
    uint32_t *f = (uint32_t *)pack_read(PACK_USE, name, &n);
    if (!f) use_path_of(name, p), f = (uint32_t *)read_file(p, &n);
    int known = f && n == 20 && f[0] == USE_MAGIC && f[1] == USE_VERSION;
    if (known) m->slots = f[2], m->attrs = f[3], m->fouts = f[4];
    free(f);
    return known;
}

#define POOL_TEX_SHORT 32768

static int pool_short(const GfxShader *s) {
    for (int k = 0; k < s->nres[GFX_RES_TEX]; k++)
        if (s->res[GFX_RES_TEX][k].pool == 1 && s->res[GFX_RES_TEX][k].count == POOL_TEX_SHORT) return 1;
    return 0;
}

static volatile int32_t n_pool_short, pool_short_said;
static void pool_short_note(const GfxShader *s) {
    if (!pool_short(s)) return;
    plat_inc32(&n_pool_short);
    if (s->carried || plat_inc32(&pool_short_said) != 1) return;
    rt_log("gfx_shader: a shader of the translator in use indexes the texture pool with %d entries: it reads slot %d "
           "for every texture numbered from %d up; the translator is older than this runtime (tools/build_xlate.py "
           "builds the current one)", POOL_TEX_SHORT, POOL_TEX_SHORT - 1, POOL_TEX_SHORT);
}

uint32_t gfx_shader_pool_short_seen(void) { return (uint32_t)__atomic_load_n(&n_pool_short, __ATOMIC_RELAXED); }

static int carry_usable(const GfxShader *s, int j) {
    if (!pool_short(s)) return 1;
    if (plat_inc64(&C.refused) == 1) {
        const uint8_t *id = C.prev[j];
        rt_log("gfx_shader: carry: translations of a previous translator (%02x%02x%02x%02x) that index the texture pool "
               "with %d entries are not used (textures numbered from %d up would read slot %d); those shaders are "
               "translated again, and one the current translator fails keeps it", id[0], id[1], id[2], id[3],
               POOL_TEX_SHORT, POOL_TEX_SHORT, POOL_TEX_SHORT - 1);
    }
    return 0;
}

static GfxShader *carry_load(const uint8_t key[32], const uint8_t iid[32], int stage, int j, int short_ok) {
    GfxShader *s = disk_load(key, iid);
    if (!s) return NULL;
    if (stage >= 0 && s->stage != stage) {
        static volatile int32_t n_logged;
        if (plat_inc32(&n_logged) <= 4)
            rt_log("gfx_shader: carry: a previous translation of stage %d is not used for a stage %d request", s->stage, stage);
        shader_free(s);
        return NULL;
    }
    if (!short_ok && !carry_usable(s, j)) {
        shader_free(s);
        return NULL;
    }
    s->carried = (uint8_t)(j + 1);
    return mem_insert(s);
}

static GfxShader *carry_find(const uint8_t h[32], const GfxShaderAnswers *a, int stage, int disk, int short_ok) {
    if (!carry_on()) return NULL;
    for (int j = 0; j < C.n_prev; j++) {
        UseMask m;
        uint8_t iid[32], key[32];
        if (!use_lookup(C.prev_eff[j], h, &m)) continue;
        iid_of(h, a, &m, iid);
        key_of_input_id(C.prev_eff[j], iid, key);
        GfxShader *s = mem_find(key);
        if (s || (disk && (s = carry_load(key, iid, stage, j, short_ok)))) return s;
    }
    return NULL;
}

static GfxShader *carry_find_iid(const uint8_t iid[32], int disk, int *owned) {
    if (!carry_on()) return NULL;
    for (int j = 0; j < C.n_prev; j++) {
        uint8_t key[32];
        key_of_input_id(C.prev_eff[j], iid, key);
        GfxShader *s = mem_find(key);
        if (s || !disk) {
            if (s) return s;
            continue;
        }
        if (!owned) s = carry_load(key, iid, -1, j, 0);
        else if ((s = disk_load(key, iid))) {
            if (carry_usable(s, j)) s->carried = (uint8_t)(j + 1), *owned = 1;
            else shader_free(s), s = NULL;
        }
        if (s) return s;
    }
    return NULL;
}

static GfxShader *carry_last(const uint8_t key[32], const uint8_t h[32], const GfxShaderAnswers *a, int stage) {
    static volatile int32_t n_logged;
    GfxShader *s = carry_find(h, a, stage, 1, 1);
    if (s && pool_short(s) && plat_inc32(&n_logged) <= 4) {
        char hex[65];
        key_hex(key, hex);
        rt_log("gfx_shader: carry: the current translator failed %s; its previous translation (texture pool of %d "
               "entries) serves", hex, POOL_TEX_SHORT);
    }
    return s;
}

static int carry_has_iid(const uint8_t iid[32]) {
    if (!carry_on()) return 0;
    for (int j = 0; j < C.n_prev; j++) {
        uint8_t key[32];
        key_of_input_id(C.prev_eff[j], iid, key);
        if (entry_present(key)) return 1;
    }
    return 0;
}

#define ZPASS_PERIOD_MS 2000
#define ZPASS_LEVEL 1

static struct {
    PlatOnce once;
    PlatMutex pass;
    volatile int32_t state;
    uint32_t idle_s;
    volatile int64_t frames, frame_fail;
    volatile int64_t dec, dec_ns;
    volatile int64_t dec_draw, dec_draw_ns, dec_draw_max_ns;
} Z = {PLAT_ONCE_INIT, PLAT_MUTEX_INIT};

static void zpass_count_decode(uint64_t ns) {
    if (!ns) return;
    plat_inc64(&Z.dec), plat_add64(&Z.dec_ns, (int64_t)ns);
    uint32_t tid = plat_thread_id();
    if (tid != prof_render_tid && tid != prof_replay_tid) return;
    plat_inc64(&Z.dec_draw), plat_add64(&Z.dec_draw_ns, (int64_t)ns);
    int64_t m = __atomic_load_n(&Z.dec_draw_max_ns, __ATOMIC_RELAXED);
    while ((int64_t)ns > m && !__atomic_compare_exchange_n(&Z.dec_draw_max_ns, &m, (int64_t)ns, 0, __ATOMIC_RELAXED,
                                                           __ATOMIC_RELAXED)) {}
}

static void spv_held(const GfxShader *g, uint64_t *plain, uint64_t *z) {
    *plain = __atomic_load_n(&g->spv.plain, __ATOMIC_RELAXED) ? g->spv.words * 4 : 0;
    *z = __atomic_load_n(&g->spv.zsize, __ATOMIC_RELAXED);
}

static uint64_t zpass_idle_ms(void) {
    uint64_t ms = (uint64_t)Z.idle_s * 1000;
    int level = mem_budget_level(MEM_RAM);
    uint64_t cap = level == MEM_CRITICAL ? 1000 : level == MEM_TIGHT ? 5000 : ms;
    return ms < cap ? ms : cap;
}

static void zpass_main(void *arg) {
    (void)arg;
    SpvPacker pk = {0};
    int logged = 0;
    for (;;) {
        plat_sleep_ms(ZPASS_PERIOD_MS);
        uint64_t idle_ms = zpass_idle_ms(), t0 = plat_now_ns(), raw = 0, packed = 0, freed = 0;
        uint32_t made = 0, dropped = 0;
        plat_mutex_lock(&Z.pass);
        for (int i = 0; i < BUCKETS; i++) {
            plat_mutex_lock_shared(&cache_lock);
            GfxShader *g = buckets[i];
            plat_mutex_unlock_shared(&cache_lock);
            for (; g; g = g->next) {
                uint64_t now = plat_now_ms();
                int step = spv_pack_begin(&g->spv, &spv_lock, now, idle_ms);
                if (step == SPV_PACK_NONE) continue;
                void *z = NULL;
                uint32_t zn = 0;
                if (step == SPV_PACK_ENCODE) {
                    plat_thread_priority(PLAT_PRIO_LOWEST);
                    z = spv_pack_encode(&g->spv, &pk, ZPASS_LEVEL, &zn);
                    plat_thread_priority(PLAT_PRIO_NORMAL);
                    if (z) {
                        plat_inc64(&Z.frames), made++, raw += g->spv.words * 4, packed += zn;
                    } else if (plat_inc64(&Z.frame_fail) <= 4) {
                        rt_log("gfx_shader: SPIR-V at rest: a shader's SPIR-V (%llu words) was not compressed (the encoder "
                               "failed or its frame did not decode to the same words); it stays as it is",
                               (unsigned long long)g->spv.words);
                    }
                }
                uint64_t f = spv_pack_end(&g->spv, &spv_lock, step, z, zn, now, idle_ms);
                freed += f, dropped += f != 0;
            }
        }
        plat_mutex_unlock(&Z.pass);
        if (made && !logged) {
            logged = 1;
            rt_log("gfx_shader: SPIR-V at rest: the first pass compressed %u shaders unused for %u s, %.1f MiB to %.1f MiB "
                   "(%.1f%%), and freed %.1f MiB of plain SPIR-V in %u shaders, in %.0f ms (NX2SARIA_SHADER_COMPRESS=0 keeps "
                   "it plain)", made, (unsigned)(idle_ms / 1000), raw / 1048576.0, packed / 1048576.0,
                   raw ? 100.0 * (double)packed / (double)raw : 0.0, freed / 1048576.0, dropped,
                   (double)(plat_now_ns() - t0) / 1e6);
        }
    }
}

static void zpass_start(void *arg) {
    (void)arg;
    if (!cfg_bool("NX2SARIA_SHADER_COMPRESS")) {
        __atomic_store_n(&Z.state, -1, __ATOMIC_RELEASE);
        rt_log("gfx_shader: SPIR-V at rest: off (NX2SARIA_SHADER_COMPRESS=0): every shader keeps its SPIR-V plain");
        return;
    }
    int idle = cfg_int("NX2SARIA_SHADER_COMPRESS_IDLE_S");
    Z.idle_s = idle < 1 ? 1 : (uint32_t)idle;
    if (!plat_thread_spawn(zpass_main, NULL, "shader compress", 0)) {
        __atomic_store_n(&Z.state, -1, __ATOMIC_RELEASE);
        rt_log("gfx_shader: SPIR-V at rest: cannot start its thread; every shader keeps its SPIR-V plain");
        return;
    }
    __atomic_store_n(&Z.state, 1, __ATOMIC_RELEASE);
}

static void zpass_kick(void) { plat_once(&Z.once, zpass_start, NULL); }

int gfx_shader_raw_request(const char *line, char *reply, size_t reply_size) {
    int away = kern_core_away();
    Svc *v = svc_get();
    int ok = child_exchange(v, line, reply, reply_size) > 0;
    if (!ok) child_stop(v);
    svc_put(v);
    kern_core_back(&away);
    return ok;
}

uint64_t gfx_shader_failures(void) { return (uint64_t)n_failures; }
uint64_t gfx_shader_requests(void) { return (uint64_t)n_requests; }
uint64_t gfx_shader_cache_hits(void) { return (uint64_t)n_hits; }

void gfx_shader_drop_memory(void) {
    plat_mutex_lock(&Z.pass);
    plat_mutex_lock(&cache_lock);
    for (int i = 0; i < BUCKETS; i++) {
        for (GfxShader *s = buckets[i], *n; s; s = n) {
            n = s->next;
            shader_free(s);
        }
        buckets[i] = NULL;
        n_keys = 0;
        for (UseEnt *e = use_buckets[i], *n; e; e = n) n = e->next, free(e);
        use_buckets[i] = NULL;
    }
    plat_xchg32(&use_files_ready, 0);
    for (int i = 0; i < USE_FILE_BUCKETS; i++) {
        for (UseFile *e = use_files[i], *n; e; e = n) n = e->next, free(e);
        use_files[i] = NULL;
    }
    plat_mutex_unlock(&cache_lock);
    plat_mutex_unlock(&Z.pass);
}

static const GfxShader *fail(const char *why, const char *hex) {
    plat_inc64(&n_failures);
    rt_log("gfx_shader: translation failed (%s): %s", hex ? hex : "-", why);
    return NULL;
}

typedef struct {
    const char *stage_name;
    uint32_t length, c1_len;
    uint32_t skip;
    int sm86;
    const uint8_t *c1;
    uint8_t code_hash[32];
    uint8_t key[32];
    uint8_t iid[32];
    int final;
} Req;

#define NVN2_CONTROL_MAGIC 0x98761234u
#define NVN2_CONTROL_SIZE 0xe0
static volatile int64_t n_sm86;
uint64_t gfx_shader_sm86_refused(void) { return (uint64_t)n_sm86; }

static int is_nvn2(const uint8_t *control) {
    uint32_t magic;
    memcpy(&magic, control, 4);
    return magic == NVN2_CONTROL_MAGIC;
}

static const uint8_t *nvn2_control(const uint8_t *control, uint8_t buf[CONTROL_SIZE]) {
    if (!control || !is_nvn2(control)) return control;
    memset(buf, 0, CONTROL_SIZE);
    memcpy(buf, control, NVN2_CONTROL_SIZE);
    return buf;
}

static int req_init(Req *r, int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size, const uint8_t *c1,
                    const GfxShaderAnswers *a) {
    r->stage_name = stage == GFX_STAGE_VS ? "vs" : stage == GFX_STAGE_FS ? "ps" : stage == GFX_STAGE_CS ? "cs"
                  : stage == GFX_STAGE_TS ? "ts" : stage == GFX_STAGE_MS ? "ms" : NULL;
    if (!r->stage_name) return fail("stage is not translated", NULL), 0;
    uint32_t c1_off;
    r->sm86 = is_nvn2(control);
    if (r->sm86) {
        uint32_t size, c1_start, c1_end;
        memcpy(&size, control + 0x38, 4), memcpy(&c1_start, control + 0x40, 4), memcpy(&c1_end, control + 0x44, 4);
        r->length = size, c1_off = size;
        if (c1_start >= size && c1_end >= c1_start && c1_end <= code_size) r->length = c1_end, c1_off = c1_start;
        r->skip = 0;
    } else {
        memcpy(&c1_off, control + 0x700, 4), memcpy(&r->length, control + 0x704, 4);
        r->skip = stage == GFX_STAGE_CS ? 0 : CODE_SKIP;
    }
    uint64_t skip = r->skip;
    if (!code || r->length > code_size || c1_off > r->length || skip >= r->length) return fail("bad control section", NULL), 0;
    r->c1_len = r->length - c1_off;
    r->c1 = c1 ? c1 : code + c1_off;
    if (!build_code_hash(stage, control, code, r->length, r->c1, r->c1_len, r->code_hash))
        return fail("out of memory", NULL), 0;
    paths();
    UseMask m;
    r->final = use_find(r->code_hash, &m);
    build_key(r->code_hash, a, r->final ? &m : NULL, r->key, r->iid);
    return 1;
}

static const GfxShader *translate(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                  const GfxShaderAnswers *a);

static void ans_note(const Req *r, int stage, const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a,
                     const GfxShader *s);
static const GfxShader *get_async_one(Req r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a, int *pending);

#define FAILED_N 4096
static uint8_t failed_keys[FAILED_N][32];
static uint8_t failed_used[FAILED_N];
static PlatMutex failed_lock = PLAT_MUTEX_INIT;

static int failed_slot(const uint8_t key[32], int insert) {
    uint32_t h = (uint32_t)key[0] | (uint32_t)key[1] << 8 | (uint32_t)key[2] << 16;
    int found = 0;
    plat_mutex_lock(&failed_lock);
    for (uint32_t k = 0; k < 64; k++) {
        uint32_t i = (h + k) % FAILED_N;
        if (!failed_used[i]) {
            if (insert) memcpy(failed_keys[i], key, 32), failed_used[i] = 1;
            break;
        }
        if (!memcmp(failed_keys[i], key, 32)) { found = 1; break; }
    }
    plat_mutex_unlock(&failed_lock);
    return found;
}

static const GfxShader *get_one(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                const GfxShaderAnswers *a) {
    GfxShader *s = mem_find(r->key);
    if (s || (s = carry_find(r->code_hash, a, stage, 0, 0))) return plat_inc64(&n_hits), s;
    paths();
    if (!cache_dir[0]) return fail("no cache directory", NULL);
    if (r->final && (s = disk_load(r->key, r->iid))) return plat_inc64(&n_hits), mem_insert(s);
    if (failed_slot(r->key, 0)) return NULL;
    if ((s = carry_find(r->code_hash, a, stage, 1, 0))) return plat_inc64(&n_hits), s;
    const GfxShader *t = translate(r, stage, control, code, a);
    if (!t && !(t = carry_last(r->key, r->code_hash, a, stage))) failed_slot(r->key, 1);
    return t;
}

const GfxShader *gfx_shader_get(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                const uint8_t *c1, const GfxShaderAnswers *a) {
    uint8_t ctl2[CONTROL_SIZE];
    control = nvn2_control(control, ctl2);
    Req r;
    if (!req_init(&r, stage, control, code, code_size, c1, a)) return NULL;
    const GfxShader *s = get_one(&r, stage, control, code, a);
    if (s) ans_note(&r, stage, control, code, a, s);
    return s;
}

typedef struct {
    uint32_t magic, version, stage, length, c1_len, answers_size;
} InHeader;

typedef struct {
    uint8_t *buf;
    int stage;
    uint32_t length, c1_len;
    const uint8_t *control, *code, *c1;
    GfxShaderAnswers a;
} Inputs;

static void in_path(const uint8_t iid[32], char *p) {
    char hex[65];
    key_hex(iid, hex);
    snprintf(p, PATH_CAP + 80, "%s/in/%s.in", cache_dir, hex);
}

static void in_store(const uint8_t iid[32], int stage, const uint8_t *control, const uint8_t *code, const Req *r,
                     const GfxShaderAnswers *a) {
    char p[PATH_CAP + 80], tmp[PATH_CAP + 120];
    in_path(iid, p);
    if (pack_find(PACK_IN, iid) || file_exists(p)) return;
    GfxShaderAnswers zero = {0};
    InHeader h = {IN_MAGIC, IN_VERSION, (uint32_t)stage, r->length, r->c1_len, sizeof(GfxShaderAnswers)};
    size_t n = sizeof h + sizeof zero + CONTROL_SIZE + r->length + r->c1_len;
    uint8_t *buf = malloc(n), *o = buf;
    if (!buf) return;
    memcpy(o, &h, sizeof h), o += sizeof h;
    memcpy(o, a ? a : &zero, sizeof zero), o += sizeof zero;
    memcpy(o, control, CONTROL_SIZE), o += CONTROL_SIZE;
    memcpy(o, code, r->length), o += r->length;
    if (r->c1_len) memcpy(o, r->c1, r->c1_len);
    snprintf(tmp, PATH_CAP + 120, "%s/in", cache_dir);
    mkdir_p(tmp);
    plat_file_write_atomic(p, buf, n);
    free(buf);
}

static int in_load(const uint8_t iid[32], Inputs *in) {
    char p[PATH_CAP + 80];
    in_path(iid, p);
    uint64_t n = 0;
    uint8_t *f = pack_read(PACK_IN, iid, &n);
    if (!f) f = read_file(p, &n);
    InHeader h;
    if (!f || n < sizeof h) return free(f), 0;
    memcpy(&h, f, sizeof h);
    if (h.magic != IN_MAGIC || h.version != IN_VERSION || h.answers_size != sizeof(GfxShaderAnswers) ||
        n != sizeof h + sizeof(GfxShaderAnswers) + CONTROL_SIZE + (uint64_t)h.length + h.c1_len)
        return free(f), 0;
    const uint8_t *o = f + sizeof h;
    memcpy(&in->a, o, sizeof in->a), o += sizeof in->a;
    in->control = o, o += CONTROL_SIZE;
    in->code = o, o += h.length;
    in->c1 = o;
    in->buf = f, in->stage = (int)h.stage, in->length = h.length, in->c1_len = h.c1_len;
    return 1;
}

typedef struct PreparedJobs PreparedJobs;
static _Thread_local PreparedJobs *preparing;

typedef struct PreparedJob {
    struct PreparedJob *next;
    uint8_t digest[32];
    PlatCond done;
    int busy, ready;
    char cache_key[65], reflection_key[65];
} PreparedJob;

struct PreparedJobs {
    PlatMutex lock;
    PreparedJob *buckets[BUCKETS];
    volatile int64_t reused;
};

static PreparedJob *prepared_enter(const uint8_t options_hash[32], const uint8_t *bytes, size_t n, PreparedJob **owner) {
    uint8_t parts[3][32], digest[32];
    memcpy(parts[0], xlate_id, 32);
    rt_sha256(bytes, n, parts[1]);
    memcpy(parts[2], options_hash, 32);
    rt_sha256(parts, sizeof parts, digest);
    uint32_t slot;
    memcpy(&slot, digest, sizeof slot);
    PreparedJob **bucket = &preparing->buckets[slot % BUCKETS];
    plat_mutex_lock(&preparing->lock);
    PreparedJob *j = *bucket;
    while (j && memcmp(j->digest, digest, 32)) j = j->next;
    if (!j && (j = calloc(1, sizeof *j))) {
        memcpy(j->digest, digest, 32);
        plat_cond_init(&j->done);
        j->next = *bucket;
        *bucket = j;
    }
    if (j) {
        while (j->busy) plat_cond_wait(&j->done, &preparing->lock);
        if (!j->ready) {
            j->busy = 1;
            *owner = j;
        }
    }
    plat_mutex_unlock(&preparing->lock);
    return *owner ? NULL : j;
}

static void prepared_finish(PreparedJob *owner, const GfxShader *s, const uint8_t request_key[32]) {
    plat_mutex_lock(&preparing->lock);
    if (s) {
        key_hex(s->key, owner->cache_key);
        key_hex(request_key, owner->reflection_key);
        owner->ready = 1;
    }
    owner->busy = 0;
    plat_cond_broadcast(&owner->done);
    plat_mutex_unlock(&preparing->lock);
}

static GfxShader *prepared_copy(const PreparedJob *j, const Req *r, const char *hex, const char *spv, const char *res) {
    char path[PATH_CAP + 80];
    uint64_t ns = 0, nr = 0;
    snprintf(path, sizeof path, "%s/%s.res.json", cache_dir, j->cache_key);
    uint8_t *reflection = read_file(path, &nr);
    if (!reflection) return NULL;
    const char *end = (char *)reflection + nr;
    const char *id = json_key((char *)reflection, end, "key");
    if (!id || end - id < 66 || *id != '"' || id[65] != '"' || memcmp(id + 1, j->reflection_key, 64))
        return free(reflection), NULL;
    memcpy((char *)id + 1, hex, 64);
    snprintf(path, sizeof path, "%s/%s.spv", cache_dir, j->cache_key);
    uint8_t *raw = read_file(path, &ns);
    int ok = raw && plat_file_write_cached(spv, raw, (size_t)ns) && plat_file_write_cached(res, reflection, (size_t)nr);
    free(raw);
    free(reflection);
    return ok ? disk_load_from(r->key, r->iid, 0, 1) : NULL;
}

static const GfxShader *translate_one(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a, PreparedJob **owner, int *published) {
    const uint8_t *key = r->key;
    GfxShader *s;
    UseMask m;
    uint8_t mk[32], miid[32];
    const uint8_t *look = r->final ? key : NULL, *look_iid = r->iid;
    if (!r->final && use_find(r->code_hash, &m)) build_key(r->code_hash, a, &m, mk, miid), look = mk, look_iid = miid;
    if (look && (s = mem_find(look))) return plat_inc64(&n_hits), s;
    if (look && (s = disk_load(look, look_iid))) return plat_inc64(&n_hits), mem_insert(s);

    uint64_t skip = r->skip;
    char hex[65], code_path[PATH_CAP + 80], spv_path[PATH_CAP + 80], res_path[PATH_CAP + 80];
    key_hex(key, hex);
    mkdir_p(cache_dir);
    snprintf(code_path, PATH_CAP + 80, "%s/%s.code", cache_dir, hex);
    snprintf(spv_path, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
    snprintf(res_path, PATH_CAP + 80, "%s/%s.res.json", cache_dir, hex);
    size_t cap = 2 * PATH_CAP * 6 + 2 * (size_t)r->c1_len + 8192;
    char *req = malloc(cap), reply[LINE_MAX_LEN];
    if (!req) return fail("out of memory", hex);
    const char *k = cfg_get("NX2SARIA_XLATE_KEEP");
    int keep = k && *k ? atoi(k) : 0;
    uint8_t options[32];
    int share = preparing && !keep;
    build_request(req, hex, r->stage_name, code_path, r->c1, r->c1_len, a, control, share ? options : NULL);
    size_t code_len = (size_t)(r->length - skip);
    PreparedJob *prior = share ? prepared_enter(options, code + skip, code_len, owner) : NULL;
    s = prior ? prepared_copy(prior, r, hex, spv_path, res_path) : NULL;
    int reused = s != NULL;
    if (s) {
        free(req);
    } else {
        if (!write_file(code_path, code + skip, code_len)) return free(req), fail("cannot write the code file", hex);
        int ok = svc_request(req, reply, sizeof reply);
        int failed = ok > 0 && strncmp(reply, "ok ", 3);
        int kept = keep >= 2 || (keep && a && a->mvprev) || failed || ok < 0;
        if (kept) {
            char job_path[PATH_CAP + 80];
            snprintf(job_path, sizeof job_path, "%s/%s.job", cache_dir, hex);
            write_file(job_path, req, strlen(req));
        }
        free(req);
        if (!kept) plat_file_delete(code_path);
        if (ok < 0) {
            plat_file_delete(spv_path), plat_file_delete(res_path);
            char why[96];
            snprintf(why, sizeof why, "the translator gave no answer in %d s; its child was stopped",
                     PLAT_CHILD_READ_MS / 1000);
            return fail(why, hex);
        }
        if (!ok) return fail("translator unavailable", hex);
        if (failed) {
            plat_file_delete(spv_path), plat_file_delete(res_path);
            if (r->sm86) plat_inc64(&n_sm86);
            return fail(reply, hex);
        }
        if (!(s = disk_load_from(key, r->iid, 0, 1))) return fail("translator output missing or malformed", hex);
    }
    if (cache_flush()) plat_file_flush(spv_path), plat_file_flush(res_path);
    *published = 1;
    if (!r->final) {
        m = use_of(s);
        build_key(r->code_hash, a, &m, mk, miid);
        char mhex[65], to[PATH_CAP + 80];
        key_hex(mk, mhex);
        snprintf(to, PATH_CAP + 80, "%s/%s.spv", cache_dir, mhex);
        int moved = plat_file_replace(spv_path, to, cache_flush());
        snprintf(to, PATH_CAP + 80, "%s/%s.res.json", cache_dir, mhex);
        moved = moved && plat_file_replace(res_path, to, cache_flush());
        if (moved) {
            use_store(r->code_hash, &m);
        } else {
            *published = 0;
            plat_file_delete(spv_path), plat_file_delete(res_path), rt_log("gfx_shader: cannot rename %s", hex);
            plat_mutex_lock(&cache_lock);
            use_put(r->code_hash, 1, &m);
            plat_mutex_unlock(&cache_lock);
        }
        memcpy(s->key, mk, 32), memcpy(s->iid, miid, 32);
    }
    in_store(s->iid, stage, control, code, r, a);
    if (reused) plat_inc64(&preparing->reused);
    else plat_inc64(&n_xlated);
    s->fresh = 1;
    return mem_insert(s);
}

static const GfxShader *translate(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                  const GfxShaderAnswers *a) {
    int away = kern_core_away();
    const uint8_t *id = r->final ? r->key : r->code_hash;
    flight_enter(id);
    PreparedJob *owner = NULL;
    int published = 0;
    const GfxShader *s = translate_one(r, stage, control, code, a, &owner, &published);
    if (owner) prepared_finish(owner, published ? s : NULL, r->key);
    flight_leave(id);
    kern_core_back(&away);
    return s;
}

typedef struct XJob {
    struct XJob *next;
    int stage;
    Req r;
    uint8_t *control, *code;
    GfxShaderAnswers a;
} XJob;

typedef struct XWait {
    struct XWait *next;
    uint8_t key[32];
    int failed;
} XWait;

static struct {
    PlatMutex lock;
    PlatCond work, done;
    PlatCond lwork;
    XJob *head, *tail;
    XJob *lhead, *ltail;
    XWait *waits[256];
    int started;
    int loaders;
} X = {PLAT_MUTEX_INIT, PLAT_COND_INIT, PLAT_COND_INIT, PLAT_COND_INIT};

static volatile int64_t n_async;
static volatile int64_t n_disk_sync, n_disk_sync_ns, n_disk_bg;

#define LOAD_BURST_NS 2000000ll
#define LOAD_SHARE 8
#define LOADERS 3
static PlatMutex load_lock = PLAT_MUTEX_INIT;
static int64_t load_tokens = LOAD_BURST_NS;
static uint64_t load_last;

static int load_budget(void) {
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&load_lock);
    if (load_last) load_tokens += (int64_t)(now - load_last) / LOAD_SHARE;
    if (load_tokens > LOAD_BURST_NS) load_tokens = LOAD_BURST_NS;
    load_last = now;
    int ok = load_tokens > 0;
    plat_mutex_unlock(&load_lock);
    return ok;
}

static void load_spent(uint64_t ns) {
    plat_mutex_lock(&load_lock);
    load_tokens -= (int64_t)ns;
    plat_mutex_unlock(&load_lock);
    plat_inc64(&n_disk_sync);
    __atomic_add_fetch(&n_disk_sync_ns, (int64_t)ns, __ATOMIC_RELAXED);
}

void gfx_shader_disk_counts(uint64_t *sync, uint64_t *sync_ns, uint64_t *bg) {
    *sync = (uint64_t)n_disk_sync, *sync_ns = (uint64_t)n_disk_sync_ns, *bg = (uint64_t)n_disk_bg;
}

static XWait **wait_slot(const uint8_t key[32]) {
    XWait **w = &X.waits[key[0]];
    while (*w && memcmp((*w)->key, key, 32)) w = &(*w)->next;
    return w;
}

static void xlate_worker(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&X.lock);
        while (!X.head) plat_cond_wait(&X.work, &X.lock);
        XJob *j = X.head;
        if (!(X.head = j->next)) X.tail = NULL;
        plat_mutex_unlock(&X.lock);
        const GfxShader *t = translate(&j->r, j->stage, j->control, j->code, &j->a), *s = t;
        if (!s) s = carry_last(j->r.key, j->r.code_hash, &j->a, j->stage);
        plat_mutex_lock(&X.lock);
        XWait **w = wait_slot(j->r.key), *e = *w;
        if (e && s) *w = e->next, free(e);
        else if (e) e->failed = 1;
        plat_mutex_unlock(&X.lock);
        plat_cond_broadcast(&X.done);
        if (t) plat_inc64(&n_async);
        free(j->control), free(j->code), free(j);
    }
    return;
}

static void load_worker(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&X.lock);
        while (!X.lhead) plat_cond_wait(&X.lwork, &X.lock);
        XJob *j = X.lhead;
        if (!(X.lhead = j->next)) X.ltail = NULL;
        plat_mutex_unlock(&X.lock);
        GfxShader *d = j->r.final ? disk_load(j->r.key, j->r.iid) : NULL;
        const GfxShader *s = d ? mem_insert(d) : carry_find(j->r.code_hash, &j->a, j->stage, 1, 0);
        plat_mutex_lock(&X.lock);
        if (s) {
            XWait **w = wait_slot(j->r.key), *e = *w;
            if (e) *w = e->next, free(e);
        } else {
            j->next = NULL;
            if (X.tail) X.tail->next = j;
            else X.head = j;
            X.tail = j;
            plat_cond_signal(&X.work);
        }
        plat_mutex_unlock(&X.lock);
        if (!s) continue;
        plat_cond_broadcast(&X.done);
        plat_inc64(&n_hits), plat_inc64(&n_disk_bg);
        free(j->control), free(j->code), free(j);
    }
}

static void start_workers(void) {
    if (!X.started) {
        int n = 0;
        for (int i = 0; i < svc_runtime_limit(); i++) {
            if (plat_thread_spawn(xlate_worker, NULL, "xlate", 0)) n++;
        }
        X.started = n ? 1 : -1;
        if (n) rt_log("gfx_shader: graphics shaders are translated by %d background workers (NX2SARIA_XLATE_PROCS)", n);
        else rt_log("gfx_shader: cannot start the translation workers; translating synchronously");
        for (int i = 0; n && i < LOADERS; i++) {
            if (plat_thread_spawn(load_worker, NULL, "shload", 0)) __atomic_add_fetch(&X.loaders, 1, __ATOMIC_RELAXED);
        }
    }
}

static void to_front(XJob **head, XJob **tail, const uint8_t key[32]) {
    for (XJob *j = *head, *prev = NULL; j; prev = j, j = j->next)
        if (!memcmp(j->r.key, key, 32)) {
            if (prev) {
                prev->next = j->next;
                if (*tail == j) *tail = prev;
                j->next = *head, *head = j;
            }
            return;
        }
}

const GfxShader *gfx_shader_get_async(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                      const GfxShaderAnswers *a, int *pending) {
    *pending = 0;
    uint8_t ctl2[CONTROL_SIZE];
    control = nvn2_control(control, ctl2);
    Req r;
    if (!req_init(&r, stage, control, code, code_size, NULL, a)) return NULL;
    const GfxShader *s = get_async_one(r, stage, control, code, a, pending);
    if (s) ans_note(&r, stage, control, code, a, s);
    return s;
}

static const GfxShader *get_async_one(Req r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a, int *pending) {
    GfxShader *s = mem_find(r.key);
    if (s || (s = carry_find(r.code_hash, a, stage, 0, 0))) return plat_inc64(&n_hits), s;
    plat_mutex_lock_shared(&X.lock);
    XWait *w = *wait_slot(r.key);
    int failed = w && w->failed;
    plat_mutex_unlock_shared(&X.lock);
    if (w) return *pending = !failed, NULL;
    paths();
    if (!cache_dir[0]) return fail("no cache directory", NULL);
    int read = load_budget() || __atomic_load_n(&X.loaders, __ATOMIC_RELAXED) == 0;
    if (read) {
        uint64_t t0 = plat_now_ns();
        GfxShader *d = r.final ? disk_load(r.key, r.iid) : NULL;
        s = d ? mem_insert(d) : carry_find(r.code_hash, a, stage, 1, 0);
        load_spent(plat_now_ns() - t0);
        if (s) return plat_inc64(&n_hits), s;
    }

    XJob *j = calloc(1, sizeof *j);
    XWait *e = calloc(1, sizeof *e);
    uint8_t *ctl = malloc(CONTROL_SIZE), *cd = malloc(r.length);
    plat_mutex_lock(&X.lock);
    start_workers();
    XWait **slot = wait_slot(r.key);
    int raced = *slot != NULL;
    int queued = !raced && X.started == 1 && j && e && ctl && cd;
    int load = queued && !read && X.loaders;
    if (queued) {
        memcpy(ctl, control, CONTROL_SIZE), memcpy(cd, code, r.length);
        *j = (XJob){.stage = stage, .r = r, .control = ctl, .code = cd, .a = *a};
        j->r.c1 = cd + (r.length - r.c1_len);
        memcpy(e->key, r.key, 32);
        *slot = e;
        XJob **head = load ? &X.lhead : &X.head, **tail = load ? &X.ltail : &X.tail;
        if (*tail) (*tail)->next = j;
        else *head = j;
        *tail = j;
        plat_cond_signal(load ? &X.lwork : &X.work);
    }
    plat_mutex_unlock(&X.lock);
    if (queued) return *pending = 1, NULL;
    free(j), free(e), free(ctl), free(cd);
    if (raced) return *pending = 1, NULL;
    if (!read) {
        GfxShader *d = r.final ? disk_load(r.key, r.iid) : NULL;
        if ((s = d ? mem_insert(d) : carry_find(r.code_hash, a, stage, 1, 0))) return plat_inc64(&n_hits), s;
    }
    const GfxShader *t = translate(&r, stage, control, code, a);
    return t ? t : carry_last(r.key, r.code_hash, a, stage);
}

const GfxShader *gfx_shader_await(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                  const GfxShaderAnswers *a, uint32_t wait_ms, int *pending) {
    uint8_t ctl2[CONTROL_SIZE];
    control = nvn2_control(control, ctl2);
    const GfxShader *s = gfx_shader_get_async(stage, control, code, code_size, a, pending);
    Req r;
    if (!*pending) return s;
    *pending = 0;
    if (!req_init(&r, stage, control, code, code_size, NULL, a)) return s;
    uint64_t p0 = perf_begin();
    uint64_t end = wait_ms == UINT32_MAX ? 0 : plat_now_ms() + wait_ms, now = 0;
    plat_mutex_lock(&X.lock);
    to_front(&X.lhead, &X.ltail, r.key);
    to_front(&X.head, &X.tail, r.key);
    XWait *w;
    while ((w = *wait_slot(r.key)) && !w->failed && (!end || (now = plat_now_ms()) < end)) {
        if (end) plat_cond_wait_ms(&X.done, &X.lock, (uint32_t)(end - now));
        else plat_cond_wait(&X.done, &X.lock);
    }
    int failed = w && w->failed;
    plat_mutex_unlock(&X.lock);
    perf_end(PERF_XLATE, p0);
    if (failed) return NULL;
    if (w) return *pending = 1, NULL;
    s = gfx_shader_get_async(stage, control, code, code_size, a, pending);
    return *pending ? NULL : s;
}

uint64_t gfx_shader_async_done(void) { return (uint64_t)n_async; }

void gfx_shader_counts(uint64_t *keys, uint64_t *translated) { *keys = (uint64_t)n_keys, *translated = (uint64_t)n_xlated; }

const uint8_t *gfx_shader_key(const GfxShader *s) { return s->key; }

int gfx_shader_origin(const GfxShader *s) { return s->fresh ? 2 : s->carried ? 1 : 0; }

const uint8_t *gfx_shader_identity(void) {
    paths();
    return xlate_base;
}

int gfx_shader_carries(const uint8_t id[32]) {
    paths();
    if (!C.on) return 0;
    for (int i = 0; i < C.n_prev; i++)
        if (!memcmp(C.prev[i], id, 32)) return 1;
    return 0;
}

const uint8_t *gfx_shader_input_id(const GfxShader *s) { return s->iid; }

const GfxShader *gfx_shader_load_input(const uint8_t iid[32], int xlate) {
    paths();
    if (!cache_dir[0]) return NULL;
    uint8_t key[32];
    key_of_input(iid, key);
    GfxShader *s = mem_find(key);
    if (s || (s = carry_find_iid(iid, 0, NULL))) return s;
    if ((s = disk_load(key, iid))) return mem_insert(s);
    if ((s = carry_find_iid(iid, 1, NULL))) return s;
    Inputs in;
    if (!xlate || !in_load(iid, &in)) return NULL;
    const GfxShader *t = gfx_shader_get(in.stage, in.control, in.code, in.length, in.c1, &in.a);
    free(in.buf);
    return t;
}

const GfxShader *gfx_shader_load_private(const uint8_t iid[32], int *owned) {
    *owned = 0;
    paths();
    if (!cache_dir[0]) return NULL;
    uint8_t key[32];
    key_of_input(iid, key);
    GfxShader *s = mem_find(key);
    if (s || (s = carry_find_iid(iid, 0, NULL))) return s;
    if ((s = disk_load(key, iid))) *owned = 1, ids_note();
    if (!s) s = carry_find_iid(iid, 1, owned);
    return s;
}

void gfx_shader_release_private(const GfxShader *s) {
    if (s) shader_free((GfxShader *)s);
}

typedef struct {
    const uint8_t (*named)[32];
    size_t n_named;
    const GfxShader **out;
    uint8_t (*other)[32];
    size_t n_other;
    volatile int64_t next, done;
    volatile int32_t missing, other_cached, other_carried;
} Warm;

static int64_t pw_done, pw_total, pw_xlated;
static int64_t pw_ms = -1;

int gfx_shader_prewarm_stats(int64_t *ms, int64_t *translated) {
    int64_t t = __atomic_load_n(&pw_ms, __ATOMIC_ACQUIRE);
    if (t < 0) return 0;
    *ms = t;
    *translated = __atomic_load_n(&pw_xlated, __ATOMIC_RELAXED);
    return 1;
}

void gfx_shader_prewarm_progress(int64_t *done, int64_t *total, int64_t *translated) {
    *done = __atomic_load_n(&pw_done, __ATOMIC_RELAXED);
    *total = __atomic_load_n(&pw_total, __ATOMIC_RELAXED);
    *translated = __atomic_load_n(&pw_xlated, __ATOMIC_RELAXED);
}

static void warm_worker(void *arg) {
    Warm *w = arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&w->next) - 1;
        if (i >= w->n_named + w->n_other) return;
        if (i < w->n_named) {
            if (!(w->out[i] = gfx_shader_load_input(w->named[i], 1))) plat_inc32(&w->missing);
        } else {
            const uint8_t *iid = w->other[i - w->n_named];
            uint8_t key[32];
            char hex[65], p[PATH_CAP + 80];
            key_of_input(iid, key);
            key_hex(key, hex);
            snprintf(p, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
            if (pack_find(PACK_SPV, key) || file_exists(p)) plat_inc32(&w->other_cached);
            else if (carry_has_iid(iid)) plat_inc32(&w->other_carried);
            else gfx_shader_load_input(iid, 1);
        }
        plat_inc64(&w->done);
    }
}

static int iid_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

typedef struct {
    const uint8_t (*named)[32];
    size_t n_named, n, cap;
    uint8_t (*v)[32];
} InList;

static int in_collect(const char *file, void *ctx) {
    InList *l = ctx;
    char name[80];
    uint8_t iid[32];
    if (strlen(file) != 67) return 1;
    memcpy(name, file, 64);
    name[64] = 0;
    if (!hex32(name, iid) || (l->n_named && bsearch(iid, l->named, l->n_named, 32, iid_cmp))) return 1;
    if (l->n == l->cap) {
        size_t c = l->cap ? l->cap * 2 : 1024;
        uint8_t (*t)[32] = realloc(l->v, c * 32);
        if (!t) return 0;
        l->v = t, l->cap = c;
    }
    memcpy(l->v[l->n++], iid, 32);
    return 1;
}

static size_t other_inputs(const uint8_t (*named)[32], size_t n_named, uint8_t (**out)[32]) {
    char dir[PATH_CAP + 80];
    snprintf(dir, sizeof dir, "%s/in", cache_dir);
    InList l = {.named = named, .n_named = n_named};
    char name[68];
    for (uint32_t i = 0; i < pack_n; i++) {
        if (pack_ix[i].kind != PACK_IN) continue;
        key_hex(pack_ix[i].name, name);
        memcpy(name + 64, ".in", 4);
        if (!in_collect(name, &l)) break;
    }
    plat_file_list(dir, ".in", in_collect, &l);
    return *out = l.v, l.n;
}

void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out, int threads, int procs) {
    paths();
    double t0 = (double)plat_now_ms();
    memset(out, 0, n * sizeof *out);
    if (!cache_dir[0]) return;
    uint64_t xl0 = (uint64_t)n_xlated, fail0 = (uint64_t)n_failures;
    uint8_t (*sorted)[32] = n ? malloc(n * 32) : NULL;
    if (sorted) memcpy(sorted, iids, n * 32), qsort(sorted, n, 32, iid_cmp);
    Warm w = {.named = iids, .n_named = n, .out = out};
    w.n_other = other_inputs(sorted, sorted ? n : 0, &w.other);
    free(sorted);
    size_t total = w.n_named + w.n_other;
    __atomic_store_n(&pw_done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pw_xlated, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pw_total, (int64_t)total, __ATOMIC_RELAXED);
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    if ((size_t)threads > total) threads = (int)(total ? total : 1);
    svc_set_limit(procs);
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; i < threads; i++)
        if ((t[k] = plat_thread_new(warm_worker, &w, "shader prewarm"))) k++;
    if (!k) warm_worker(&w);
    double logged = (double)plat_now_ms();
    for (int m = 0; m < k; m++)
        while (!plat_thread_join_ns(t[m], 100000000u)) {
            __atomic_store_n(&pw_done, (int64_t)w.done, __ATOMIC_RELAXED);
            __atomic_store_n(&pw_xlated, (int64_t)((uint64_t)n_xlated - xl0), __ATOMIC_RELAXED);
            if ((double)plat_now_ms() - logged < 2000) continue;
            logged = (double)plat_now_ms();
            rt_log("gfx_shader: prewarm: %lld of %zu shaders, %llu translated, %.0f ms", (long long)w.done, total,
                   (unsigned long long)((uint64_t)n_xlated - xl0), (double)plat_now_ms() - t0);
        }
    __atomic_store_n(&pw_done, (int64_t)total, __ATOMIC_RELAXED);
    __atomic_store_n(&pw_xlated, (int64_t)((uint64_t)n_xlated - xl0), __ATOMIC_RELAXED);
    svc_set_limit(svc_runtime_limit());
    rt_log("gfx_shader: prewarm: %zu shaders named by pipeline keys (%ld not available), %zu other stored inputs (%ld "
           "cached, %ld by a previous translator only); %llu translated, %llu failed, with up to %d translators and %d "
           "threads, in %.0f ms", w.n_named, (long)w.missing, w.n_other, (long)w.other_cached, (long)w.other_carried,
           (unsigned long long)((uint64_t)n_xlated - xl0), (unsigned long long)((uint64_t)n_failures - fail0), procs, k,
           (double)plat_now_ms() - t0);
    free(w.other);
    __atomic_store_n(&pw_ms, (int64_t)((double)plat_now_ms() - t0), __ATOMIC_RELEASE);
}
const uint32_t *gfx_shader_spirv_pin(const GfxShader *cs, uint64_t *words) {
    GfxShader *s = (GfxShader *)cs;
    uint64_t ns;
    const uint32_t *w = spv_pin(&s->spv, &spv_lock, &ns);
    if (!w) rt_fatal("gfx_shader: a shader's compressed SPIR-V (%llu words) cannot be decoded", (unsigned long long)s->spv.words);
    zpass_count_decode(ns);
    if (words) *words = s->spv.words;
    return w;
}

void gfx_shader_spirv_unpin(const GfxShader *s) { spv_unpin(&((GfxShader *)s)->spv, &spv_lock, plat_now_ms()); }

uint64_t gfx_shader_words(const GfxShader *s) { return s->spv.words; }

uint64_t gfx_shader_spirv_fnv(const GfxShader *s) {
    uint64_t ns;
    uint64_t h = spv_fnv(&((GfxShader *)s)->spv, &spv_lock, plat_now_ms(), &ns);
    zpass_count_decode(ns);
    return h;
}

uint32_t gfx_shader_bindings(const GfxShader *s, const uint32_t **b) {
    *b = s->bind;
    return s->nbind;
}

const char *gfx_shader_res_json(const GfxShader *s) { return s->reflection; }

int gfx_shader_write_code(const GfxShader *s, const char *path) {
    Inputs in;
    paths();
    if (!s || !cache_dir[0] || !in_load(s->iid, &in)) return 0;
    int ok = write_file(path, in.code, (size_t)in.length);
    const char *st = in.stage == GFX_STAGE_VS ? "vs" : in.stage == GFX_STAGE_FS ? "ps" : in.stage == GFX_STAGE_CS ? "cs"
                   : in.stage == GFX_STAGE_TS ? "ts" : in.stage == GFX_STAGE_MS ? "ms" : NULL;
    size_t cap = 2 * PATH_CAP * 6 + 2 * (size_t)in.c1_len + 8192;
    char *req = st ? malloc(cap) : NULL, job[PATH_CAP + 80], hex[65];
    if (ok && req) {
        key_hex(s->key, hex);
        build_request(req, hex, st, path, in.c1, in.c1_len, &in.a, in.control, NULL);
        snprintf(job, sizeof job, "%s.job", path);
        write_file(job, req, strlen(req));
    }
    free(req);
    free(in.buf);
    return ok;
}

int gfx_shader_stage(const GfxShader *s) { return s->stage; }

int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res) {
    if (kind < 0 || kind >= GFX_RES_KINDS) return 0;
    if (res) *res = s->res[kind];
    return s->nres[kind];
}

uint32_t gfx_shader_slot_mask(const GfxShader *s) { return use_of(s).slots; }

uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->in, sizeof s->in);
    return s->in_mask;
}

uint32_t gfx_shader_mvprev_banks(const GfxShader *s) { return s->mvprev_banks; }

int gfx_shader_palette(const GfxShader *s, int *slot, int *storage, uint32_t *joints) {
    if (!s->pal_stride) return 0;
    *slot = s->pal_slot, *storage = s->pal_storage, *joints = s->pal_joints;
    return s->pal_stride;
}

uint32_t gfx_shader_omap(const GfxShader *s) { return s->omap; }

uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->out, sizeof s->out);
    return s->out_mask;
}

void gfx_shader_local_size(const GfxShader *s, uint32_t local[3]) { memcpy(local, s->local, sizeof s->local); }

#define ANS_VERSION 1
#define ANS_PACKED 147
#define ANS_REC (4 + 32 + 32 + ANS_PACKED + 1)
#define PRG_VERSION 1
#define PRG_HEAD (12 + 32 + CONTROL_SIZE)
typedef struct { char magic[8]; uint32_t version, rec_size; } RecHeader;
typedef struct { uint64_t *v; uint32_t cap, n; } Set64;
typedef struct { uint8_t *p; size_t n, cap; } Bytes;

static struct {
    PlatMutex lock;
    volatile int32_t on;
    char ans_path[PATH_CAP], prg_path[PATH_CAP];
    Set64 pairs, progs;
    Bytes ans, prg;
    uint64_t pairs_new;
} RC = {PLAT_MUTEX_INIT};

static int set_add(Set64 *s, uint64_t h) {
    h = h ? h : 1;
    if ((s->n + 1) * 2 > s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        if (!t) return 0;
        for (uint32_t i = 0; i < s->cap; i++)
            if (s->v[i]) {
                uint32_t j = (uint32_t)s->v[i] & (cap - 1);
                while (t[j]) j = (j + 1) & (cap - 1);
                t[j] = s->v[i];
            }
        free(s->v), s->v = t, s->cap = cap;
    }
    uint32_t j = (uint32_t)h & (s->cap - 1);
    for (; s->v[j]; j = (j + 1) & (s->cap - 1))
        if (s->v[j] == h) return 0;
    s->v[j] = h, s->n++;
    return 1;
}

static int bytes_put(Bytes *b, const void *p, size_t n) {
    if (!n) return 1;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->n + n) cap *= 2;
        uint8_t *t = realloc(b->p, cap);
        if (!t) return 0;
        b->p = t, b->cap = cap;
    }
    memcpy(b->p + b->n, p, n), b->n += n;
    return 1;
}

static uint64_t hash8(const uint8_t *sha) {
    uint64_t v;
    memcpy(&v, sha, 8);
    return v;
}

static void unpack_answers(const uint8_t *p, GfxShaderAnswers *a) {
    memset(a, 0, sizeof *a);
    memcpy(a->tex_type, p, 32), p += 32;
    memcpy(a->img_type, p, 32), p += 32;
    memcpy(a->img_format, p, 32), p += 32;
    memcpy(a->attr_type, p, 32), p += 32;
    memcpy(a->frag_out_type, p, 8), p += 8;
    for (int i = 0; i < 3; i++) a->local[i] = (uint16_t)(p[0] | p[1] << 8), p += 2;
    memcpy(&a->shared_bytes, p, 4), p += 4;
    a->depth_minus_w = *p & 0x7f, a->mvprev = *p >> 7;
}

static volatile int32_t rec_kept;

static int rec_read(const char *path, const char *magic, uint32_t version, uint32_t rec_size, uint8_t **body, size_t *n) {
    *body = NULL, *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    RecHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    int head = len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1;
    int format = head && !memcmp(h.magic, magic, 8) && h.version == version && h.rec_size == rec_size;
    int ok = format;
    if (ok && len > (long)sizeof h) {
        *n = (size_t)len - sizeof h;
        ok = (*body = malloc(*n)) && fread(*body, 1, *n, f) == *n;
    }
    fclose(f);
    if (!ok) {
        free(*body), *body = NULL, *n = 0;
        if ((head && !format) || (len > 0 && len < (long)sizeof h)) rt_log("gfx_shader: %s has another format or version; it is rewritten", path), remove(path);
        else if (len > 0) rt_log("gfx_shader: cannot read %s (%ld bytes); kept as it is", path, len), plat_xchg32(&rec_kept, 1);
    }
    return ok;
}

static size_t prg_walk(const uint8_t *b, size_t n, void (*fn)(const uint8_t *rec, void *ctx), void *ctx) {
    size_t o = 0;
    while (n - o >= PRG_HEAD) {
        uint32_t len, c1;
        memcpy(&len, b + o + 4, 4), memcpy(&c1, b + o + 8, 4);
        size_t size = PRG_HEAD + (size_t)len + c1;
        if (len > (64u << 20) || c1 > len || size > n - o) break;
        if (fn) fn(b + o, ctx);
        o += size;
    }
    return o;
}

static void prg_seen(const uint8_t *rec, void *ctx) { set_add(ctx, hash8(rec + 12)); }

static void truncate_to(const char *path, size_t size) {
    if (!plat_file_truncate(path, size)) return;
    rt_log("gfx_shader: %s had a torn tail; cut to %zu bytes", path, size);
}

void gfx_shader_record_at(const char *keys_path) {
    if (!keys_path || !keys_path[0] || RC.on) return;
    plat_mutex_lock(&RC.lock);
    const char *slash = strrchr(keys_path, '\\'), *fs = strrchr(keys_path, '/');
    if (fs > slash) slash = fs;
    int dir = slash ? (int)(slash + 1 - keys_path) : 0;
    snprintf(RC.ans_path, sizeof RC.ans_path, "%.*sshader-answers.bin", dir, keys_path);
    snprintf(RC.prg_path, sizeof RC.prg_path, "%.*sshader-programs.bin", dir, keys_path);
    uint8_t *b;
    size_t n;
    plat_xchg32(&rec_kept, 0);
    if (rec_read(RC.ans_path, "NX2NSANS", ANS_VERSION, ANS_REC, &b, &n)) {
        for (size_t o = 0; o + ANS_REC <= n; o += ANS_REC) set_add(&RC.pairs, hash8(b + o + 36));
        if (n % ANS_REC) truncate_to(RC.ans_path, sizeof(RecHeader) + n / ANS_REC * ANS_REC);
    }
    free(b);
    if (rec_read(RC.prg_path, "NX2NSPRG", PRG_VERSION, 0, &b, &n)) {
        size_t whole = prg_walk(b, n, prg_seen, &RC.progs);
        if (whole != n) truncate_to(RC.prg_path, sizeof(RecHeader) + whole);
    }
    free(b);
    if (plat_xchg32(&rec_kept, 0)) {
        rt_log("gfx_shader: answers met in play are not recorded this run (%s or %s could not be read)", RC.ans_path,
               RC.prg_path);
        plat_mutex_unlock(&RC.lock);
        return;
    }
    rt_log("gfx_shader: recording answers met in play to %s (%u records, %u programs recorded before)", RC.ans_path,
           RC.pairs.n, RC.progs.n);
    plat_xchg32(&RC.on, 1);
    plat_mutex_unlock(&RC.lock);
}

static void ans_note(const Req *r, int stage, const uint8_t *control, const uint8_t *code, const GfxShaderAnswers *a,
                     const GfxShader *s) {
    if (!RC.on) return;
    uint32_t st = (uint32_t)stage;
    plat_mutex_lock(&RC.lock);
    if (set_add(&RC.pairs, hash8(s->iid))) {
        uint8_t rec[ANS_REC] = {0};
        memcpy(rec, &st, 4), memcpy(rec + 4, r->code_hash, 32), memcpy(rec + 36, s->iid, 32);
        pack_answers(rec + 68, a);
        if (set_add(&RC.progs, hash8(r->code_hash))) {
            uint32_t head[3] = {st, r->length, r->c1_len};
            bytes_put(&RC.prg, head, 12), bytes_put(&RC.prg, r->code_hash, 32);
            bytes_put(&RC.prg, control, CONTROL_SIZE), bytes_put(&RC.prg, code, r->length);
            bytes_put(&RC.prg, r->c1, r->c1_len);
        }
        bytes_put(&RC.ans, rec, sizeof rec);
        RC.pairs_new++;
    }
    plat_mutex_unlock(&RC.lock);
}

static int rec_append(const char *path, const char *magic, uint32_t version, uint32_t rec_size, const Bytes *b) {
    FILE *f = fopen(path, "ab");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    int ok = 1;
    if (ftell(f) == 0) {
        RecHeader h = {{0}, version, rec_size};
        memcpy(h.magic, magic, 8);
        ok = fwrite(&h, sizeof h, 1, f) == 1;
    }
    ok = ok && fwrite(b->p, 1, b->n, f) == b->n;
    return !fclose(f) && ok;
}

static PlatMutex rec_io = PLAT_MUTEX_INIT;

void gfx_shader_record_flush(int force) {
    static volatile int64_t last;
    int64_t now = (int64_t)plat_now_ms();
    int64_t served = __atomic_load_n(&C.served, __ATOMIC_RELAXED), refused = __atomic_load_n(&C.refused, __ATOMIC_RELAXED);
    if (force && served != __atomic_exchange_n(&C.served_logged, served, __ATOMIC_RELAXED))
        rt_log("gfx_shader: carry: %lld shaders served from a previous translator this session", (long long)served);
    if (force && refused != __atomic_exchange_n(&C.refused_logged, refused, __ATOMIC_RELAXED))
        rt_log("gfx_shader: carry: a previous translator's translation was passed over %lld times this session "
               "(texture pool of %d entries; it serves only where the current translator failed)", (long long)refused,
               POOL_TEX_SHORT);
    if (!RC.on || (!force && now - last < 2000)) return;
    last = now;
    plat_mutex_lock(&rec_io);
    plat_mutex_lock(&RC.lock);
    Bytes ans = RC.ans, prg = RC.prg;
    RC.ans = (Bytes){0}, RC.prg = (Bytes){0};
    uint64_t total = RC.pairs.n, added = RC.pairs_new;
    plat_mutex_unlock(&RC.lock);
    if (prg.n && !rec_append(RC.prg_path, "NX2NSPRG", PRG_VERSION, 0, &prg)) rt_log("gfx_shader: cannot write %s", RC.prg_path);
    if (ans.n && !rec_append(RC.ans_path, "NX2NSANS", ANS_VERSION, ANS_REC, &ans)) rt_log("gfx_shader: cannot write %s", RC.ans_path);
    plat_mutex_unlock(&rec_io);
    if (ans.n) rt_log("gfx_shader: answers: %zu new records appended (%llu new this run, %llu recorded)", ans.n / ANS_REC,
                      (unsigned long long)added, (unsigned long long)total);
    free(ans.p), free(prg.p);
}

#define SEED_DEFAULT 0x80
typedef struct { const uint8_t *rec; } PrgIdx;
typedef struct { PrgIdx *idx; size_t n; } PrgList;
typedef struct {
    PrgList progs;
    const uint8_t *ans;
    size_t n_ans;
    PreparedJobs jobs;
    uint8_t (*iid)[32];
    volatile int64_t next, done, missing, failed, refined;
} Prep;

static int prg_cmp(const void *x, const void *y) { return memcmp(((const PrgIdx *)x)->rec + 12, ((const PrgIdx *)y)->rec + 12, 32); }

static void prg_collect(const uint8_t *rec, void *ctx) {
    PrgList *l = ctx;
    l->idx[l->n++].rec = rec;
}

static void prep_worker(void *arg) {
    Prep *p = arg;
    preparing = &p->jobs;
    for (;;) {
        size_t i = (size_t)plat_inc64(&p->next) - 1;
        if (i >= p->n_ans) break;
        const uint8_t *r = p->ans + i * ANS_REC;
        uint8_t key_rec[12 + 32];
        memcpy(key_rec + 12, r + 4, 32);
        PrgIdx k = {key_rec}, *e = bsearch(&k, p->progs.idx, p->progs.n, sizeof k, prg_cmp);
        if (!e) {
            plat_inc64(&p->missing), plat_inc64(&p->done);
            continue;
        }
        uint32_t st, len;
        memcpy(&st, e->rec, 4), memcpy(&len, e->rec + 4, 4);
        const uint8_t *control = e->rec + 44, *code = control + CONTROL_SIZE, *c1 = code + len;
        GfxShaderAnswers a, b;
        unpack_answers(r + 68, &a);
        const GfxShader *s = gfx_shader_get((int)st, control, code, len, c1, &a), *t;
        if (s && r[ANS_REC - 1] & SEED_DEFAULT && gfx_shader_guess_refine(s, (int)st, &a, &b) &&
            (t = gfx_shader_get((int)st, control, code, len, c1, &b)))
            s = t, plat_inc64(&p->refined);
        if (!s) plat_inc64(&p->failed);
        else if (p->iid) memcpy(p->iid[i], s->iid, 32);
        plat_inc64(&p->done);
    }
    preparing = NULL;
}

static long long prepare_files(const char *ans_path, const char *prg_path, int threads, int procs, const char *what,
                               uint8_t **ans_out, uint8_t (**iid_out)[32]) {
    paths();
    double t0 = (double)plat_now_ms();
    uint8_t *ab = NULL, *pb = NULL;
    size_t an = 0, pn = 0;
    if (!rec_read(ans_path, "NX2NSANS", ANS_VERSION, ANS_REC, &ab, &an) ||
        !rec_read(prg_path, "NX2NSPRG", PRG_VERSION, 0, &pb, &pn)) {
        rt_log("gfx_shader: prepare: no %s (%s)", what, ans_path);
        free(ab), free(pb);
        return -1;
    }
    Prep p = {.progs = {calloc(pn / PRG_HEAD + 1, sizeof(PrgIdx)), 0}, .ans = ab, .n_ans = an / ANS_REC};
    if (iid_out) p.iid = calloc(p.n_ans + 1, 32);
    if (!p.progs.idx || (iid_out && !p.iid)) return free(p.progs.idx), free(p.iid), free(ab), free(pb), -1;
    prg_walk(pb, pn, prg_collect, &p.progs);
    qsort(p.progs.idx, p.progs.n, sizeof *p.progs.idx, prg_cmp);
    plat_inc32(&C.suspended);
    plat_mutex_init(&p.jobs.lock);
    uint64_t xl0 = (uint64_t)n_xlated;
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    svc_set_limit(procs);
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; i < threads; i++)
        if ((t[k] = plat_thread_new(prep_worker, &p, "shader prepare"))) k++;
    if (!k) prep_worker(&p);
    for (int m = 0, polls = 0; m < k; m++)
        while (!plat_thread_join_ns(t[m], 2000000000u)) {
            printf("prepare-shaders: progress translate %lld %zu\n", (long long)p.done, p.n_ans);
            fflush(stdout);
            if (++polls % 5 == 0)
                rt_log("gfx_shader: prepare: %lld of %zu pairs, %llu translated, %.0f ms", (long long)p.done, p.n_ans,
                       (unsigned long long)((uint64_t)n_xlated - xl0), (double)plat_now_ms() - t0);
        }
    printf("prepare-shaders: progress translate %zu %zu\n", p.n_ans, p.n_ans);
    fflush(stdout);
    plat_dec32(&C.suspended);
    svc_set_limit(svc_runtime_limit());
    rt_log("gfx_shader: prepare: %zu %s of %zu programs: %llu translated, %lld failed, %lld without a program, the rest "
           "cached; %lld default variants with refined answers; %.0f ms", p.n_ans, what, p.progs.n,
           (unsigned long long)((uint64_t)n_xlated - xl0), (long long)p.failed, (long long)p.missing, (long long)p.refined,
           (double)plat_now_ms() - t0);
    rt_log("gfx_shader: prepare: %lld equivalent translator requests reused", (long long)p.jobs.reused);
    for (int b = 0; b < BUCKETS; b++) {
        PreparedJob *j = p.jobs.buckets[b];
        while (j) {
            PreparedJob *next = j->next;
            free(j);
            j = next;
        }
    }
    free(p.progs.idx), free(pb);
    if (ans_out) *ans_out = ab;
    else free(ab);
    if (iid_out) *iid_out = p.iid;
    return (long long)p.n_ans;
}

void gfx_shader_prepare_recorded(int threads, int procs) {
    if (!RC.on) {
        rt_log("gfx_shader: prepare: no recorded answers (not recording)");
        return;
    }
    prepare_files(RC.ans_path, RC.prg_path, threads, procs, "recorded pairs", NULL, NULL);
}

static int file_sha256(const char *path, uint8_t out[32]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = len > 0 ? malloc((size_t)len) : NULL;
    int ok = b && fread(b, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (ok) rt_sha256(b, (size_t)len, out);
    free(b);
    return ok;
}

uint8_t *gfx_shader_prepared_read(const char *path, size_t *n, PrpFrom *from, int *has_from) {
    uint64_t size = 0;
    uint8_t *f = plat_file_read(path, &size), *out = NULL;
    PrpFrom fr = {{0}};
    int has = 0;
    const uint8_t *recs = prp_parse(f, f ? size : 0, n, &fr, &has);
    if (recs && (out = malloc(*n * PRP_REC))) memcpy(out, recs, *n * PRP_REC);
    if (!out) *n = 0;
    if (from) *from = fr;
    if (has_from) *has_from = has;
    free(f);
    return out;
}

int gfx_shader_prepare_seed(const char *dir, int threads, int procs) {
    char ans[PATH_CAP + 32], prg[PATH_CAP + 32], out[PATH_CAP + 32];
    snprintf(ans, sizeof ans, "%s/seed-answers.bin", dir);
    snprintf(prg, sizeof prg, "%s/seed-programs.bin", dir);
    snprintf(out, sizeof out, "%s/shader-prepared.bin", dir);
    double t0 = (double)plat_now_ms();
    uint64_t fail0 = (uint64_t)n_failures;
    uint8_t *ab = NULL, (*iid)[32] = NULL;
    paths();
    PrpFrom from = {{0}}, was;
    int seeded = file_sha256(prg, from.programs) && file_sha256(ans, from.answers), had = 0;
    memcpy(from.translator, xlate_base, 32);
    size_t n_was = 0;
    free(gfx_shader_prepared_read(out, &n_was, &was, &had));
    if (seeded && n_was) {
        int same_seed = had && !memcmp(was.programs, from.programs, 32) && !memcmp(was.answers, from.answers, 32);
        int same_xl = had && !memcmp(was.translator, from.translator, 32);
        rt_log("gfx_shader: prepare: %s holds %zu variants %s; it is replaced by this preparation", out, n_was,
               !had ? "and does not say what they were prepared from"
               : !same_seed ? "prepared from another seed"
               : !same_xl ? "prepared from this seed by another translator" : "prepared from this seed and translator");
    }
    plat_inc32(&C.suspended);
    long long n = prepare_files(ans, prg, threads, procs, "seed variants", &ab, &iid);
    if (n < 0) return 1;
    uint8_t *recs = calloc((size_t)n + 1, PRP_REC);
    static const uint8_t zero[32];
    size_t u = 0, by[3] = {0}, stages[8] = {0};
    Set64 seen = {0};
    for (long long i = 0; recs && i < n; i++) {
        if (!memcmp(iid[i], zero, 32) || !set_add(&seen, hash8(iid[i]))) continue;
        uint8_t *r = recs + u * PRP_REC;
        const uint8_t *a = ab + (size_t)i * ANS_REC;
        uint8_t prio = a[ANS_REC - 1] & ~SEED_DEFAULT;
        r[0] = a[0] < 6 ? a[0] : 0, r[1] = prio > 2 ? 2 : prio;
        memcpy(r + 4, iid[i], 32);
        by[r[1]]++, stages[r[0]]++, u++;
    }
    free(seen.v);
    uint8_t *file = recs ? malloc(PRP_HEAD + PRP_FROM_SIZE + u * PRP_REC + 1) : NULL;
    int ok = file != NULL;
    if (ok) {
        size_t head = prp_head_write(file, &from);
        memcpy(file + head, recs, u * PRP_REC);
        ok = plat_file_write_atomic(out, file, head + u * PRP_REC);
    }
    char msg[768];
    snprintf(msg, sizeof msg, "gfx_shader: prepared %zu shader variants (vertex %zu, fragment %zu, compute %zu; title %zu, "
             "warm-up %zu, other %zu) from %lld seed records (seed programs %02x%02x%02x%02x, answers %02x%02x%02x%02x, "
             "translator %02x%02x%02x%02x), %llu translations failed, in %.0f ms; %s %s", u,
             stages[GFX_STAGE_VS], stages[GFX_STAGE_FS], stages[GFX_STAGE_CS], by[0], by[1], by[2], n,
             from.programs[0], from.programs[1], from.programs[2], from.programs[3], from.answers[0], from.answers[1],
             from.answers[2], from.answers[3], from.translator[0], from.translator[1], from.translator[2],
             from.translator[3], (unsigned long long)((uint64_t)n_failures - fail0), (double)plat_now_ms() - t0,
             ok ? "wrote" : "cannot write", out);
    rt_log("%s", msg);
    printf("%s\n", msg);
    free(file), free(recs), free(iid), free(ab);
    return ok ? 0 : 1;
}

size_t gfx_shader_warm_list(GfxWarmEnt **out) {
    *out = NULL;
    if (!RC.on) return 0;
    char prp[PATH_CAP + 32];
    snprintf(prp, sizeof prp, "%.*sshader-prepared.bin", (int)(strlen(RC.ans_path) - strlen("shader-answers.bin")),
             RC.ans_path);
    uint8_t *ab = NULL, *pb = NULL;
    size_t an = 0, pn = 0;
    plat_mutex_lock(&rec_io);
    rec_read(RC.ans_path, "NX2NSANS", ANS_VERSION, ANS_REC, &ab, &an);
    plat_mutex_unlock(&rec_io);
    PrpFrom from;
    int has_from = 0;
    pb = gfx_shader_prepared_read(prp, &pn, &from, &has_from), pn *= PRP_REC;
    if (pb && has_from)
        rt_log("gfx_shader: %s was prepared from seed programs %02x%02x%02x%02x, answers %02x%02x%02x%02x, by translator "
               "%02x%02x%02x%02x%s", prp, from.programs[0], from.programs[1], from.programs[2], from.programs[3],
               from.answers[0], from.answers[1], from.answers[2], from.answers[3], from.translator[0], from.translator[1],
               from.translator[2], from.translator[3],
               memcmp(from.translator, xlate_base, 32) ? " (not this one: its translations are carried over)" : "");
    else if (pb)
        rt_log("gfx_shader: %s does not say what it was prepared from (written before the list recorded it)", prp);
    size_t na = an / ANS_REC, np = pn / PRP_REC, n = 0;
    GfxWarmEnt *e = calloc(na + np + 1, sizeof *e);
    Set64 seen = {0};
    for (size_t i = 0; e && i < na; i++) {
        const uint8_t *r = ab + i * ANS_REC;
        if (!set_add(&seen, hash8(r + 36))) continue;
        memcpy(e[n].iid, r + 36, 32), e[n].stage = r[0], e[n].prio = 0, n++;
    }
    for (size_t i = 0; e && i < np; i++) {
        const uint8_t *r = pb + i * PRP_REC;
        if (!set_add(&seen, hash8(r + 4))) continue;
        memcpy(e[n].iid, r + 4, 32), e[n].stage = r[0], e[n].prio = (uint8_t)(1 + r[1]), n++;
    }
    free(seen.v), free(ab), free(pb);
    rt_log("gfx_shader: warm list: %zu variants (%zu met in play, %zu prepared)", n, na, np);
    *out = e;
    return n;
}

static int carry_lock(void) {
    char p[PATH_CAP + 32];
    snprintf(p, sizeof p, "%s/carry.lock", cache_dir);
    return plat_lock_file(p);
}

#define PACK_SRC_MAGIC "TKSPSRC1"
#define PACK_TRAILER 48

static void file_id_put(uint8_t *o, const PlatFileId *id) {
    uint64_t v[5] = {id->volume, id->file_id, id->size, id->write_time, id->create_time};
    memcpy(o, v, sizeof v);
}

static int pruned_of(const PlatFileId *src, uint64_t *size) {
    char p[PATH_CAP + 48];
    snprintf(p, sizeof p, "%s/shader-cache.pack.pruned", cache_dir);
    PlatFile *f = plat_file_open(p);
    PlatFileId id;
    uint8_t tr[PACK_TRAILER], want[40];
    int same = f && plat_file_id(f, &id) && id.size >= 16 + PACK_TRAILER &&
               plat_file_pread(f, tr, PACK_TRAILER, id.size - PACK_TRAILER) == PACK_TRAILER && !memcmp(tr, PACK_SRC_MAGIC, 8);
    if (f) plat_file_close(f);
    if (same) file_id_put(want, src), same = !memcmp(tr + 8, want, sizeof want), *size = id.size;
    return same;
}

static void pack_swap_pruned(void) {
    char pend[PATH_CAP + 48], pack[PATH_CAP + 32];
    snprintf(pend, sizeof pend, "%s/shader-cache.pack.pruned", cache_dir);
    if (!file_exists(pend) || carry_lock() != 1) return;
    snprintf(pack, sizeof pack, "%s/shader-cache.pack", cache_dir);
    PlatFile *b = plat_file_open(pack);
    PlatFileId ib;
    uint64_t size = 0;
    int same = b && plat_file_id(b, &ib);
    if (b) plat_file_close(b);
    same = same && pruned_of(&ib, &size);
    if (!same) {
        plat_file_delete(pend);
        rt_log("gfx_shader: carry: the cache pack changed after its pruned copy was made; the copy is deleted");
    } else if (plat_file_replace(pend, pack, 1)) {
        rt_log("gfx_shader: carry: the cache pack is now its copy without previous translators' files (%.0f MiB)",
               (double)size / 1048576.0);
    } else {
        rt_log("gfx_shader: carry: cannot put the pruned copy in place of %s now; a later start tries again", pack);
    }
}

typedef struct {
    uint8_t iid[32], h[32];
    uint8_t has;
    uint8_t state, h_known;
} CarryItem;
enum { CI_TODO, CI_COVERED, CI_FAILED, CI_SKIPPED };

static Svc carry_svc;

static int carry_busy(void) {
    return __atomic_load_n(&F.n, __ATOMIC_RELAXED) > 0 || __atomic_load_n(&X.head, __ATOMIC_RELAXED) != NULL;
}

static void carry_wait(void) {
    while (carry_busy()) plat_sleep_ms(50);
}

static int carry_exchange(const char *line, char *reply, size_t n) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!carry_svc.child) {
            if (!child_start(&carry_svc)) return 0;
            plat_child_lower(carry_svc.child);
        }
        int r = child_exchange_raw(&carry_svc, line, reply, n);
        if (r > 0) return 1;
        child_stop(&carry_svc);
        if (r < 0) return -1;
    }
    return 0;
}

static int carry_translate(const Req *r, const Inputs *in, char *why, size_t nwhy) {
    uint8_t t[32], tb[36];
    memcpy(tb, "TKSC", 4), memcpy(tb + 4, r->key, 32);
    rt_sha256(tb, sizeof tb, t);
    char hex[65], code_path[PATH_CAP + 80], spv_path[PATH_CAP + 80], res_path[PATH_CAP + 80];
    key_hex(t, hex);
    mkdir_p(cache_dir);
    snprintf(code_path, sizeof code_path, "%s/%s.code", cache_dir, hex);
    snprintf(spv_path, sizeof spv_path, "%s/%s.spv", cache_dir, hex);
    snprintf(res_path, sizeof res_path, "%s/%s.res.json", cache_dir, hex);
    if (!write_file(code_path, in->code + r->skip, (size_t)(r->length - r->skip)))
        return snprintf(why, nwhy, "cannot write the code file"), -1;
    size_t cap = 2 * PATH_CAP * 6 + 2 * (size_t)r->c1_len + 8192;
    char *req = malloc(cap), reply[LINE_MAX_LEN];
    if (!req) return plat_file_delete(code_path), snprintf(why, nwhy, "out of memory"), -1;
    build_request(req, hex, r->stage_name, code_path, r->c1, r->c1_len, &in->a, in->control, NULL);
    int ok = carry_exchange(req, reply, sizeof reply), refused = ok > 0 && strncmp(reply, "ok ", 3);
    if (refused || ok < 0) {
        char job_path[PATH_CAP + 80];
        snprintf(job_path, sizeof job_path, "%s/%s.job", cache_dir, hex);
        write_file(job_path, req, strlen(req));
    } else {
        plat_file_delete(code_path);
    }
    free(req);
    if (!ok) return snprintf(why, nwhy, "translator unavailable"), -1;
    if (ok < 0) {
        plat_file_delete(spv_path), plat_file_delete(res_path);
        return snprintf(why, nwhy, "the translator gave no answer in %d s", PLAT_CHILD_READ_MS / 1000), 0;
    }
    GfxShader *s = refused ? NULL : disk_load_from(t, NULL, 0, 0);
    if (!s || s->stage != in->stage) {
        snprintf(why, nwhy, "%s", refused ? reply : s ? "translated to another stage" : "translator output missing or malformed");
        plat_file_delete(spv_path), plat_file_delete(res_path);
        if (s) shader_free(s);
        return 0;
    }
    UseMask m = {0};
    uint8_t key[32], iid[32];
    if (r->final) memcpy(key, r->key, 32), memcpy(iid, r->iid, 32);
    else m = use_of(s), build_key(r->code_hash, &in->a, &m, key, iid);
    shader_free(s);
    char khex[65], to[PATH_CAP + 80];
    key_hex(key, khex);
    snprintf(to, sizeof to, "%s/%s.spv", cache_dir, khex);
    int moved = plat_file_replace(spv_path, to, cache_flush());
    snprintf(to, sizeof to, "%s/%s.res.json", cache_dir, khex);
    moved = moved && plat_file_replace(res_path, to, cache_flush());
    if (!moved) {
        plat_file_delete(spv_path), plat_file_delete(res_path);
        if (!entry_present(key)) return snprintf(why, nwhy, "cannot rename %s", hex), -1;
    }
    if (!r->final) {
        plat_thread_priority(PLAT_PRIO_NORMAL);
        use_store(r->code_hash, &m);
        plat_thread_priority(PLAT_PRIO_LOWEST);
    }
    in_store(iid, in->stage, in->control, in->code, r, &in->a);
    ids_note();
    return 1;
}

static int set_has(const Set64 *s, uint64_t h) {
    h = h ? h : 1;
    for (uint32_t j = s->cap ? (uint32_t)h & (s->cap - 1) : 0; s->cap && s->v[j]; j = (j + 1) & (s->cap - 1))
        if (s->v[j] == h) return 1;
    return 0;
}

typedef struct { uint32_t kind; uint8_t name[32]; } PackDrop;

static int drop_cmp(const void *x, const void *y) {
    const PackDrop *a = x, *b = y;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    return memcmp(a->name, b->name, 32);
}

typedef struct {
    PackDrop *v;
    size_t n, cap;
    size_t deleted;
    size_t left;
} Drops;

static void drop_file(Drops *d, uint32_t kind, const uint8_t name[32]) {
    char hex[65], p[PATH_CAP + 80];
    key_hex(name, hex);
    snprintf(p, sizeof p, "%s/%s%s", cache_dir, hex, kind == PACK_USE ? ".use" : kind == PACK_SPV ? ".spv" : ".res.json");
    if (file_exists(p)) {
        plat_file_delete(p);
        if (file_exists(p)) d->left++;
        else d->deleted++;
    }
    if (!pack_find(kind, name)) return;
    d->left++;
    if (d->n == d->cap) {
        size_t c = d->cap ? d->cap * 2 : 4096;
        PackDrop *t = realloc(d->v, c * sizeof *t);
        if (!t) return;
        d->v = t, d->cap = c;
    }
    d->v[d->n].kind = kind, memcpy(d->v[d->n].name, name, 32), d->n++;
}

static size_t carry_prune_one(const CarryItem *it, size_t m, int j, Drops *d) {
    Set64 keep = {0}, done = {0};
    size_t left0 = d->left, kept = 0;
    for (size_t i = 0; i < m; i++)
        if ((it[i].has >> j & 1) && it[i].state != CI_COVERED) {
            kept++;
            if (it[i].h_known) set_add(&keep, hash8(it[i].h));
        }
    for (size_t i = 0; i < m; i++) {
        if (!(it[i].has >> j & 1) || it[i].state != CI_COVERED) continue;
        uint8_t key[32], name[32];
        key_of_input_id(C.prev_eff[j], it[i].iid, key);
        drop_file(d, PACK_SPV, key), drop_file(d, PACK_RES, key);
        if (!set_add(&done, hash8(it[i].h)) || set_has(&keep, hash8(it[i].h))) continue;
        use_name_id(C.prev_eff[j], it[i].h, name);
        drop_file(d, PACK_USE, name);
    }
    free(keep.v), free(done.v);
    return kept + (d->left - left0);
}

static int carry_repack(Drops *d) {
    char tmp[PATH_CAP + 48], out[PATH_CAP + 48];
    qsort(d->v, d->n, sizeof *d->v, drop_cmp);
    PackEnt *nix = malloc(((size_t)pack_n + 1) * sizeof *nix);
    uint64_t *src = malloc(((size_t)pack_n + 1) * sizeof *src), at;
    uint32_t k = 0;
    PlatFileId id;
    uint64_t have = 0;
    if (!nix || !src || !plat_file_id(pack_file, &id) || pruned_of(&id, &have)) {
        if (have) rt_log("gfx_shader: carry: a pruned copy of the cache pack waits for the next start already");
        free(nix), free(src);
        return 0;
    }
    for (uint32_t i = 0; i < pack_n; i++) {
        PackDrop key = {pack_ix[i].kind, {0}};
        memcpy(key.name, pack_ix[i].name, 32);
        if (bsearch(&key, d->v, d->n, sizeof *d->v, drop_cmp)) continue;
        src[k] = pack_ix[i].offset, nix[k] = pack_ix[i], k++;
    }
    at = 16 + (uint64_t)k * sizeof *nix;
    for (uint32_t i = 0; i < k; i++) nix[i].offset = at, at += nix[i].size;
    snprintf(tmp, sizeof tmp, "%s/shader-cache.pack.pruned.tmp", cache_dir);
    snprintf(out, sizeof out, "%s/shader-cache.pack.pruned", cache_dir);
    plat_file_delete(tmp);
    int err = 0;
    PlatFile *f = plat_fs_create(tmp, &err);
    uint8_t head[16] = "TKSPACK1", trailer[PACK_TRAILER] = PACK_SRC_MAGIC;
    memcpy(head + 8, &k, 4);
    file_id_put(trailer + 8, &id);
    size_t cap = 4u << 20, fill = 0;
    uint8_t *buf = malloc(cap);
    at = 16 + (uint64_t)k * sizeof *nix;
    int ok = f && buf && plat_file_pwrite(f, head, 16, 0) == 16 &&
             plat_file_pwrite(f, nix, k * (uint32_t)sizeof *nix, 16) == (int64_t)k * (int64_t)sizeof *nix;
    for (uint32_t i = 0; ok && i <= k; i++) {
        uint32_t size = i < k ? nix[i].size : 0;
        if (i == k || fill + size > cap) {
            ok = !fill || plat_file_pwrite(f, buf, (uint32_t)fill, at) == (int64_t)fill;
            at += fill, fill = 0;
            carry_wait();
        }
        if (i == k || !ok) continue;
        if (size > cap) {
            uint8_t *big = malloc(size);
            ok = big && pack_read_at(src[i], big, size) && plat_file_pwrite(f, big, size, at) == (int64_t)size;
            at += size;
            free(big);
        } else {
            ok = pack_read_at(src[i], buf + fill, size);
            fill += size;
        }
    }
    ok = ok && plat_file_pwrite(f, trailer, PACK_TRAILER, at) == PACK_TRAILER && plat_file_fsync(f);
    if (f) plat_file_close(f);
    ok = ok && plat_file_replace(tmp, out, 1);
    if (!ok) plat_file_delete(tmp);
    if (ok)
        rt_log("gfx_shader: carry: wrote %s: %u of the pack's %u files (%.0f MiB), without %zu of previous translators; "
               "it takes the pack's place at the next start", out, k, pack_n, (double)(at + PACK_TRAILER) / 1048576.0, d->n);
    else rt_log("gfx_shader: carry: cannot write %s; the pack keeps previous translators' files for now", out);
    free(buf), free(nix), free(src);
    return ok;
}

static void carry_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_LOWEST);
    double t0 = (double)plat_now_ms(), logged = t0;
    char tmp[PATH_CAP + 48];
    snprintf(tmp, sizeof tmp, "%s/shader-cache.pack.pruned.tmp", cache_dir);
    plat_file_delete(tmp);
    uint8_t (*ids)[32] = NULL;
    size_t n = other_inputs(NULL, 0, &ids), m = 0, per[IDS_MAX] = {0};
    CarryItem *it = calloc(n + 1, sizeof *it);
    for (size_t i = 0; it && i < n; i++) {
        uint8_t has = 0;
        for (int j = 0; j < C.n_prev; j++) {
            uint8_t key[32];
            key_of_input_id(C.prev_eff[j], ids[i], key);
            if (entry_present(key)) has |= (uint8_t)(1u << j), per[j]++;
        }
        if (has) memcpy(it[m].iid, ids[i], 32), it[m++].has = has;
    }
    free(ids);
    if (!it) {
        rt_log("gfx_shader: carry: out of memory; no background pass");
        return;
    }
    char list[IDS_MAX * 32] = "";
    for (int j = 0, k = 0; j < C.n_prev && k < (int)sizeof list; j++)
        k += snprintf(list + k, sizeof list - (size_t)k, "%s%02x%02x%02x%02x: %zu", j ? ", " : "", C.prev[j][0],
                      C.prev[j][1], C.prev[j][2], C.prev[j][3], per[j]);
    rt_log("gfx_shader: carry: background pass started: %zu of %zu stored inputs have a previous translator's "
           "translation (%s); they are translated again at idle priority for the next start", m, n, list);
    size_t had = 0, xlated = 0, failed = 0, skipped = 0;
    int unavailable = 0;
    for (size_t i = 0; i < m && unavailable < 3; i++) {
        carry_wait();
        CarryItem *c = &it[i];
        Inputs in;
        Req r;
        plat_thread_priority(PLAT_PRIO_NORMAL);
        int ok = in_load(c->iid, &in);
        if (ok && !req_init(&r, in.stage, in.control, in.code, in.length, in.c1, &in.a)) free(in.buf), ok = 0;
        plat_thread_priority(PLAT_PRIO_LOWEST);
        if (!ok) {
            c->state = CI_SKIPPED, skipped++;
            continue;
        }
        memcpy(c->h, r.code_hash, 32), c->h_known = 1;
        if (r.final && entry_present(r.key)) {
            c->state = CI_COVERED, had++;
        } else {
            char why[512], hex[65];
            int k = carry_translate(&r, &in, why, sizeof why);
            if (k > 0) {
                c->state = CI_COVERED, xlated++, unavailable = 0;
            } else if (!k) {
                c->state = CI_FAILED, failed++;
                key_hex(c->iid, hex);
                if (failed <= 64)
                    rt_log("gfx_shader: carry: stored input %s does not translate with the current translator (%s); "
                           "its previous translation stays in use", hex, why);
            } else {
                c->state = CI_SKIPPED, skipped++, unavailable++;
                if (unavailable == 3) rt_log("gfx_shader: carry: %s; the background pass stops", why);
            }
        }
        free(in.buf);
        if ((double)plat_now_ms() - logged >= 10000) {
            logged = (double)plat_now_ms();
            rt_log("gfx_shader: carry: %zu of %zu done: %zu translated, %zu had the current translator's already, %zu "
                   "failed, %zu skipped; %lld shaders served from a previous translator so far (%.0f s)", i + 1, m, xlated,
                   had, failed, skipped, (long long)C.served, (logged - t0) / 1000);
        }
    }
    child_stop(&carry_svc);
    rt_log("gfx_shader: carry: background pass finished in %.0f s: %zu stored inputs, %zu translated again, %zu had the "
           "current translator's already, %zu failed (their previous translations stay in use), %zu skipped or not "
           "reached; %lld shaders served from a previous translator so far", ((double)plat_now_ms() - t0) / 1000, m,
           xlated, had, failed, m - xlated - had - failed, (long long)C.served);
    Drops d = {0};
    uint8_t gone[IDS_MAX][32];
    int n_gone = 0;
    for (int j = 0; j < C.n_prev; j++)
        if (!carry_prune_one(it, m, j, &d)) memcpy(gone[n_gone++], C.prev[j], 32);
    free(it);
    int repacked = d.n && carry_repack(&d);
    plat_mutex_lock(&ids_lock);
    for (int j = 0; j < n_gone && C.n_dropped < IDS_MAX; j++) memcpy(C.dropped[C.n_dropped++], gone[j], 32);
    plat_mutex_unlock(&ids_lock);
    if (had || xlated) plat_xchg32(&C.noted, 1);
    ids_write();
    rt_log("gfx_shader: carry: %zu files of previous translators deleted, %zu packed ones %s; %d of %d previous "
           "translators stay in identities.txt", d.deleted, d.n,
           repacked ? "left out of the pruned pack" : "stay in the pack for now", C.n_prev - n_gone, C.n_prev);
    free(d.v);
}

void gfx_shader_carry_start(void) {
    static volatile int32_t started;
    paths();
    if (!carry_on() || !cache_dir[0] || plat_xchg32(&started, 1)) return;
    mkdir_p(cache_dir);
    int lk = carry_lock();
    if (lk != 1) {
        rt_log("gfx_shader: carry: %s; no background pass in this process",
               lk ? "cannot make carry.lock in the shader cache" : "another process uses this shader cache");
        return;
    }
    if (!plat_thread_spawn(carry_main, NULL, "shader carry", 0)) rt_log("gfx_shader: carry: cannot start the background pass");
}

void gfx_shader_ram(GfxShaderRam *r) {
    memset(r, 0, sizeof *r);
    plat_mutex_lock_shared(&cache_lock);
    for (int i = 0; i < BUCKETS; i++) {
        for (const GfxShader *g = buckets[i]; g; g = g->next) {
            uint64_t plain, z;
            spv_held(g, &plain, &z);
            r->shaders++;
            r->cache += sizeof *g + plain + z + (uint64_t)g->nbind * sizeof *g->bind + g->reflection_size + 1;
            for (int k = 0; k < GFX_RES_KINDS; k++) r->cache += (uint64_t)g->nres[k] * sizeof *g->res[k];
            r->spv_held += plain + z, r->spv_full += g->spv.words * 4, r->spv_packed += !plain && z;
        }
        for (const UseEnt *u = use_buckets[i]; u; u = u->next) r->use += sizeof *u;
    }
    plat_mutex_unlock_shared(&cache_lock);
    r->compress = __atomic_load_n(&Z.state, __ATOMIC_ACQUIRE);
    r->decodes = (uint64_t)__atomic_load_n(&Z.dec, __ATOMIC_RELAXED);
    r->decode_ns = (uint64_t)__atomic_load_n(&Z.dec_ns, __ATOMIC_RELAXED);
    r->draw_decodes = (uint64_t)__atomic_load_n(&Z.dec_draw, __ATOMIC_RELAXED);
    r->draw_decode_ns = (uint64_t)__atomic_load_n(&Z.dec_draw_ns, __ATOMIC_RELAXED);
    r->draw_decode_max_ns = (uint64_t)__atomic_load_n(&Z.dec_draw_max_ns, __ATOMIC_RELAXED);
    r->use += (uint64_t)__atomic_load_n(&use_files_n, __ATOMIC_RELAXED) * sizeof(UseFile) + sizeof use_files;
    r->pack = (uint64_t)pack_n * sizeof(PackEnt);
    plat_mutex_lock(&RC.lock);
    r->records = ((uint64_t)RC.pairs.cap + RC.progs.cap) * sizeof(uint64_t) + RC.ans.cap + RC.prg.cap;
    plat_mutex_unlock(&RC.lock);
}

void gfx_shader_sizes(PerfSizes *s) {
    uint64_t n = 0, spv = 0, refl = 0, failed = 0, xq = 0, lq = 0, procs = 0, busy = 0;
    plat_mutex_lock_shared(&cache_lock);
    for (int i = 0; i < BUCKETS; i++)
        for (const GfxShader *g = buckets[i]; g; g = g->next) {
            uint64_t plain, z;
            spv_held(g, &plain, &z);
            n++, spv += plain + z, refl += g->reflection_size;
        }
    plat_mutex_unlock_shared(&cache_lock);
    plat_mutex_lock(&failed_lock);
    for (int i = 0; i < FAILED_N; i++) failed += failed_used[i] != 0;
    plat_mutex_unlock(&failed_lock);
    plat_mutex_lock(&X.lock);
    for (const XJob *j = X.head; j; j = j->next) xq++;
    for (const XJob *j = X.lhead; j; j = j->next) lq++;
    plat_mutex_unlock(&X.lock);
    plat_mutex_lock(&S.lock);
    for (int i = 0; i < MAX_SVC; i++) procs += S.s[i].child != NULL, busy += S.s[i].busy != 0;
    plat_mutex_unlock(&S.lock);
    perf_size(s, "shader memory cache", n, 0, spv + refl);
    perf_size(s, "of it reflection", PERF_SIZE_NONE, 0, refl);
    perf_size(s, "failed shader keys", failed, FAILED_N, PERF_SIZE_NONE);
    perf_size(s, "translations queued", xq, 0, PERF_SIZE_NONE);
    perf_size(s, "shader loads queued", lq, 0, PERF_SIZE_NONE);
    perf_size(s, "translator processes", procs, MAX_SVC, PERF_SIZE_NONE);
    perf_size(s, "busy", busy, 0, PERF_SIZE_NONE);
}
