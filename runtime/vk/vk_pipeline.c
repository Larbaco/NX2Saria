// Contains code Copyright (c) 2026 Peiikai, Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "mesh_emu_spirv.h"
#include "gfx.h"
#include "host.h"
#include "log_stall.h"
#include "boot_warm.h"
#include "mem_need.h"
#include "nvidia_cache.h"
#include "nvn_hmap.h"
#include "platform/platform.h"
#include "prp_file.h"
#include "session_summary.h"
#include "spirv_water.h"
#include "stutter_map.h"
#include "tex.h"
#include "vk_build_gate.h"
#include "vk_cache_file.h"
#include "vk_cache_stamp.h"
#include "vk_play_gate.h"
#include "vk_exec.h"
#include "vk_save_retry.h"
#include "vk_warm_hint.h"

#define LOG_ONCE(...) do { static volatile int32_t once_; if (!plat_xchg32(&once_, 1)) rt_log(__VA_ARGS__); } while (0)

enum { SPV_OP_ENTRY_POINT = 15, SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_CONSTANT = 43, SPV_OP_CONSTANT_COMPOSITE = 44,
       SPV_OP_CONSTANT_NULL = 46, SPV_OP_FUNCTION = 54, SPV_OP_VARIABLE = 59,
       SPV_OP_DECORATE = 71, SPV_DEC_BUILTIN = 11, SPV_DEC_LOCATION = 30, SPV_DEC_INDEX = 32, SPV_DEC_BINDING = 33, SPV_DEC_DESCRIPTOR_SET = 34,
       SPV_SC_OUTPUT = 3 };

#define MAX_BINDINGS 128
#define PIPE_BUCKETS 4096
#define LAYOUT_BUCKETS 256

enum { NP_16 = 1, NP_32 = 2, NP_64 = 4 };

static struct {
    PlatMutex lock;
    volatile int32_t ready;
    VkDevice device;
    VkPipelineCache cache;
    uint32_t max_push;
    int indep;
    int push_stage;
    int gpl;
    int mesh_lib;
    struct { uint64_t links, fb_links, wlinks, flinks, dlinks, pr_added, lib_slow[4], ready_n[6], ready_nofast, ready_mesh, ready_recorded, ready_fresh, ready_logged, lib_logged, hints, hint_libs, requeued, link_fail, defer, lib_n[4], lib_fail, swaps, slow, pre_used[4]; double pr_added_ms, lib_slow_ms[4], ready_ms, ready_max, link_ms, link_max, lib_ms[4], lib_max[4], dlink_ms, dlink_max;
             uint64_t m_links, m_ready_fast, m_lib_n[2]; double m_link_ms, m_link_max, m_ready_ms, m_ready_max, m_lib_ms[2], m_lib_max[2]; } g;
    int vs_outs_all;
    int dyn_vi, dyn_clamp, dyn_poly, dyn_blend_en, dyn_blend_eq, dyn_wmask, dyn_a2c;
    int f_logic_op, f_depth_clamp, f_fill_non_solid, f_dual_src, f_full_subgroups;
    int wide_lines;
    float line_w[2], line_gran;
    int cache_control;
    int feedback;
    uint64_t cache_loaded;
    VkPipelineCache play;
    uint64_t play_loaded;
    uint64_t saved[2];
    volatile uint64_t held[2];
    int fold;
    int base_at_exit;
    volatile uint64_t play_made;
    int pv_last;
    VkShaderStageFlags sg32_stages;
    int reconverge;
    uint32_t nan_preserve;
    int nan_bits;
    volatile uint32_t nan_bits_tests, nan_bits_selects, nan_bits_modules;
    int mesh_invariant;
    volatile uint32_t mesh_invariant_modules;
    int mesh_sg;
    volatile uint32_t sg_task_pinned, sg_task_full, sg_mesh_full;
    VkDynamicState dyn[32];
    uint32_t n_dyn;
    PFN_vkCmdPushDescriptorSetKHR push;
    int tpl;
    PFN_vkCreateDescriptorUpdateTemplate tpl_create;
    PFN_vkDestroyDescriptorUpdateTemplate tpl_destroy;
    PFN_vkUpdateDescriptorSetWithTemplate tpl_update;
    PFN_vkCmdPushDescriptorSetWithTemplateKHR tpl_push;
    PFN_vkCmdSetVertexInputEXT set_vi;
    PFN_vkCmdSetDepthClampEnableEXT set_clamp;
    PFN_vkCmdSetPolygonModeEXT set_poly;
    PFN_vkCmdSetColorBlendEnableEXT set_blend_en;
    PFN_vkCmdSetColorBlendEquationEXT set_blend_eq;
    PFN_vkCmdSetColorWriteMaskEXT set_wmask;
    PFN_vkCmdSetAlphaToCoverageEnableEXT set_a2c;
    char cache_path[PLAT_PATH_MAX * 2];
    char play_path[PLAT_PATH_MAX * 2 + 8];
    VkPipeStats stats;
} P = {.lock = PLAT_MUTEX_INIT};

typedef struct Layout {
    struct Layout *next;
    uint64_t hash;
    VkDescriptorSetLayout set;
    VkPipelineLayout layout;
    int push;
    uint32_t n;
    VkDescriptorSetLayout set1;
    int push1;
    uint32_t n0;
    VkPipelineLayout half[2];
    VkDescriptorUpdateTemplate tpl[2];
    uint8_t emu;
    VkDescriptorUpdateTemplate tpl_c;
    VkDescriptorSetLayoutBinding b[];
} Layout;

typedef struct Pipe {
    struct Pipe *next;
    uint64_t hash;
    VkPipeInfo info;
    VkPipeBinding *bindings;
    int building;
    struct Job *job;
    const struct Layout *lay;
    VkPipeline fast;
    uint8_t fast_tried;
    uint8_t link_queued;
    uint8_t used;
    uint8_t pre;
    uint8_t optimised;
    uint32_t used_min;
    uint64_t t_miss;
    uint64_t rel_fence;
    size_t key_size;
    uint8_t key[];
} Pipe;

static volatile uint32_t pipe_minute;
static uint32_t minute_now(void) { return (uint32_t)(plat_now_ns() / 60000000000ull) + 1; }
static inline void pipe_used(Pipe *p) {
    uint32_t m = __atomic_load_n(&pipe_minute, __ATOMIC_RELAXED);
    if (p->used_min != m) p->used_min = m;
}

static Layout *layouts[LAYOUT_BUCKETS];
static Pipe *pipes[PIPE_BUCKETS];

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
#define FNV0 0xcbf29ce484222325ull
static __thread uint64_t mm_hash;

static uint64_t key_hash(const void *k, size_t n) { return nvn_hm_hash(k, (uint32_t)n); }

static void cache_path_init(void) {
    const char *cfg_path = cfg_get("NX2SARIA_PIPELINE_CACHE");
    if (cfg_path && *cfg_path && strlen(cfg_path) < sizeof P.cache_path) {
        strcpy(P.cache_path, cfg_path);
        return;
    }
    P.cache_path[0] = 0;
    char exe[PLAT_PATH_MAX * 2];
    int n = plat_exe_path(exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    char *cut = plat_last_sep(exe);
    if (!cut) return;
    *cut = 0;
    snprintf(P.cache_path, sizeof P.cache_path, "%s/pipeline-cache.bin", exe);
}

#define PB_VERSION 2
#ifdef _WIN32
#define pb_seek _fseeki64
#define pb_tell _ftelli64
#else
#define pb_seek fseeko
#define pb_tell ftello
#endif
#define PB_MAX_BIN 8
#define PB_KEY VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR
enum { PB_BLOB = 1, PB_PIPE = 2 };
#define PB_HEAD (4 + 4 + PB_KEY)
typedef struct { char magic[8]; uint32_t version, gkey_size; uint8_t gkey[PB_KEY]; } PbHeader;
typedef struct { uint64_t h, off; uint32_t size; } PbSlot;
static struct {
    int on;
    PFN_vkCreatePipelineBinariesKHR create;
    PFN_vkDestroyPipelineBinaryKHR destroy;
    PFN_vkGetPipelineKeyKHR key;
    PFN_vkGetPipelineBinaryDataKHR data;
    PFN_vkReleaseCapturedPipelineDataKHR release;
    char path[PLAT_PATH_MAX * 2 + 32];
    PlatMutex lock;
    FILE *rf;
    PbSlot *slot;
    uint32_t cap, n, pipes, blobs;
    uint8_t *pend;
    size_t pend_n, pend_cap;
    uint64_t file_size;
    int no_capture[3];
    uint64_t from_file, load_fail, compiled, captured, capture_fail, blob_new, blob_shared;
    double from_ms, compile_ms;
} PB = {.lock = PLAT_MUTEX_INIT};
static __thread int pb_last;
static __thread int cc_hit;

static uint64_t pb_hash(uint32_t type, const uint8_t *key, uint32_t n) {
    uint64_t h = fnv(fnv(FNV0, &type, 4), key, n);
    return h ? h : 1;
}

static PbSlot *pb_find(uint64_t h) {
    if (!PB.cap) return NULL;
    for (uint32_t j = (uint32_t)h & (PB.cap - 1);; j = (j + 1) & (PB.cap - 1)) {
        if (!PB.slot[j].h) return NULL;
        if (PB.slot[j].h == h) return &PB.slot[j];
    }
}

static int pb_insert(uint64_t h, uint64_t off, uint32_t size) {
    if ((PB.n + 1) * 2 > PB.cap) {
        uint32_t cap = PB.cap ? PB.cap * 2 : 4096;
        PbSlot *t = calloc(cap, sizeof *t);
        if (!t) return 0;
        for (uint32_t i = 0; i < PB.cap; i++) {
            if (!PB.slot[i].h) continue;
            uint32_t j = (uint32_t)PB.slot[i].h & (cap - 1);
            while (t[j].h) j = (j + 1) & (cap - 1);
            t[j] = PB.slot[i];
        }
        free(PB.slot);
        PB.slot = t, PB.cap = cap;
    }
    if (pb_find(h)) return 0;
    uint32_t j = (uint32_t)h & (PB.cap - 1);
    while (PB.slot[j].h) j = (j + 1) & (PB.cap - 1);
    PB.slot[j] = (PbSlot){h, off, size};
    PB.n++;
    return 1;
}

static void pb_rewrite(const PbHeader *hd) {
    FILE *f = fopen(PB.path, "wb");
    if (f) fwrite(hd, sizeof *hd, 1, f), fclose(f);
    PB.file_size = f ? sizeof *hd : 0;
}

static uint64_t pb_index(FILE *f, uint64_t len) {
    uint64_t good = sizeof(PbHeader);
    for (;;) {
        uint32_t head[3];
        uint8_t key[PB_KEY];
        if (pb_seek(f, (int64_t)good, SEEK_SET) || fread(head, sizeof head, 1, f) != 1 || head[2] > PB_KEY ||
            head[0] < PB_HEAD || (head[1] != PB_BLOB && head[1] != PB_PIPE) || fread(key, PB_KEY, 1, f) != 1 ||
            good + 4 + head[0] > len)
            break;
        if (pb_insert(pb_hash(head[1], key, head[2]), good, head[0])) {
            if (head[1] == PB_PIPE) PB.pipes++;
            else PB.blobs++;
        }
        good += 4 + (uint64_t)head[0];
    }
    return good;
}

static uint64_t pb_keep(const char *path, uint64_t good, uint64_t len) {
    if (good == len) return len;
    if (!plat_file_truncate(path, good)) return 0;
    rt_log("vk_pipeline: pipeline binaries: %s ends inside a record or holds a malformed one at byte %llu; %llu bytes "
           "from there dropped", path, (unsigned long long)good, (unsigned long long)(len - good));
    return good;
}

static void pb_init(VkCtx *c) {
    if (!c->has_pipeline_binary || !P.cache_path[0]) return;
#define PBPROC(field, name) (PB.field = (void *)vk_dpa(c->device, name))
    if (!PBPROC(create, "vkCreatePipelineBinariesKHR") || !PBPROC(destroy, "vkDestroyPipelineBinaryKHR") ||
        !PBPROC(key, "vkGetPipelineKeyKHR") || !PBPROC(data, "vkGetPipelineBinaryDataKHR") ||
        !PBPROC(release, "vkReleaseCapturedPipelineDataKHR")) {
        rt_log("vk_pipeline: pipeline binaries: the driver lacks a VK_KHR_pipeline_binary function; using pipeline-cache.bin");
        return;
    }
#undef PBPROC
    PbHeader want = {{'N', 'X', '2', 'N', 'P', 'B', 'I', 'N'}, PB_VERSION, 0, {0}};
    VkPipelineBinaryKeyKHR gk = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    if (PB.key(c->device, NULL, &gk) != VK_SUCCESS || !gk.keySize || gk.keySize > PB_KEY) {
        rt_log("vk_pipeline: pipeline binaries: no global key from the driver; using pipeline-cache.bin");
        return;
    }
    want.gkey_size = gk.keySize;
    memcpy(want.gkey, gk.key, gk.keySize);
    snprintf(PB.path, sizeof PB.path, "%s", P.cache_path);
    char *cut = plat_last_sep(PB.path);
    snprintf(cut ? cut + 1 : PB.path, sizeof PB.path - (size_t)(cut ? cut + 1 - PB.path : 0), "pipeline-binaries.bin");
    uint64_t cap = (uint64_t)strtoull(cfg_value("NX2SARIA_PIPE_BINARIES_MAX_MB"), NULL, 10) << 20;
    FILE *f = fopen(PB.path, "rb");
    PbHeader hd;
    const char *why = NULL;
    uint64_t good = 0, len = 0;
    if (f) {
        pb_seek(f, 0, SEEK_END);
        len = (uint64_t)pb_tell(f);
        pb_seek(f, 0, SEEK_SET);
        if (fread(&hd, sizeof hd, 1, f) != 1 || memcmp(&hd, &want, sizeof hd)) why = "written by another driver, GPU or version";
        else if (cap && len > cap) why = "over NX2SARIA_PIPE_BINARIES_MAX_MB";
        else good = pb_index(f, len);
        fclose(f);
    }
    if (f && !why) {
        uint64_t kept = pb_keep(PB.path, good, len);
        if (kept) len = kept;
        else why = "ends inside a record or holds a malformed one, and it cannot be cut back to its whole records";
    }
    if (why || !f) {
        if (f) rt_log("vk_pipeline: pipeline binaries: %s %s (%.0f MB); starting empty", PB.path, why, len / 1048576.0);
        free(PB.slot), PB.slot = NULL, PB.cap = PB.n = PB.pipes = PB.blobs = 0;
        pb_rewrite(&want);
    } else {
        PB.file_size = len;
    }
    PB.rf = fopen(PB.path, "rb");
    PB.on = PB.rf != NULL && PB.file_size >= sizeof want;
    rt_log("vk_pipeline: pipeline binaries %s: %s, %u pipelines, %u binaries, %.1f MB (NX2SARIA_PIPE_BINARIES=0 goes back "
           "to pipeline-cache.bin)", PB.on ? "on" : "off (cannot open the file)", PB.path, PB.pipes, PB.blobs,
           PB.file_size / 1048576.0);
}

static uint8_t *pb_read(uint32_t type, const uint8_t *key, uint32_t ks, uint32_t *size) {
    PbSlot *s = pb_find(pb_hash(type, key, ks));
    if (!s || s->size < PB_HEAD) return NULL;
    uint32_t n = s->size - PB_HEAD;
    uint8_t *head = malloc(PB_HEAD + (size_t)n), *out = NULL;
    if (!head) return NULL;
    int ok;
    if (s->off >= PB.file_size) {
        size_t at = (size_t)(s->off - PB.file_size) + 4;
        ok = at + s->size <= PB.pend_n;
        if (ok) memcpy(head, PB.pend + at, s->size);
    } else {
        ok = PB.rf && !pb_seek(PB.rf, (int64_t)(s->off + 4), SEEK_SET) && fread(head, s->size, 1, PB.rf) == 1;
    }
    uint32_t t, k;
    if (ok) memcpy(&t, head, 4), memcpy(&k, head + 4, 4);
    if (ok && t == type && k == ks && !memcmp(head + 8, key, ks)) {
        memmove(head, head + PB_HEAD, n);
        out = head, *size = n;
    } else {
        free(head);
    }
    return out;
}

static int pb_load(const VkPipelineBinaryKeyKHR *k, VkPipelineBinaryKHR bins[PB_MAX_BIN], uint32_t *n) {
    VkPipelineBinaryKeyKHR keys[PB_MAX_BIN];
    VkPipelineBinaryDataKHR data[PB_MAX_BIN];
    uint8_t *blob[PB_MAX_BIN] = {0};
    uint32_t nb = 0, size = 0;
    plat_mutex_lock(&PB.lock);
    uint8_t *pipe = pb_read(PB_PIPE, k->key, k->keySize, &size);
    int ok = pipe && size >= 4;
    if (ok) memcpy(&nb, pipe, 4);
    if (!nb || nb > PB_MAX_BIN || size < 4 + nb * (4 + PB_KEY)) ok = 0;
    for (uint32_t i = 0; ok && i < nb; i++) {
        const uint8_t *e = pipe + 4 + i * (4 + PB_KEY);
        uint32_t ks, ds = 0;
        memcpy(&ks, e, 4);
        keys[i] = (VkPipelineBinaryKeyKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR, .keySize = ks};
        memcpy(keys[i].key, e + 4, PB_KEY);
        if (ks > PB_KEY || !(blob[i] = pb_read(PB_BLOB, keys[i].key, ks, &ds))) ok = 0;
        else data[i] = (VkPipelineBinaryDataKHR){.dataSize = ds, .pData = blob[i]};
    }
    plat_mutex_unlock(&PB.lock);
    if (ok) {
        VkPipelineBinaryKeysAndDataKHR kd = {.binaryCount = nb, .pPipelineBinaryKeys = keys, .pPipelineBinaryData = data};
        VkPipelineBinaryCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pKeysAndDataInfo = &kd};
        VkPipelineBinaryHandlesInfoKHR hi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
                                             .pipelineBinaryCount = nb, .pPipelineBinaries = bins};
        ok = PB.create(P.device, &ci, NULL, &hi) == VK_SUCCESS;
        *n = nb;
    }
    for (uint32_t i = 0; i < PB_MAX_BIN; i++) free(blob[i]);
    free(pipe);
    return ok;
}

static int pb_put(uint32_t type, const uint8_t *key, uint32_t ks, const void *a, size_t an, const void *b, size_t bn) {
    size_t size = PB_HEAD + an + bn;
    if (size >= (1u << 31)) return 0;
    if (PB.pend_n + 4 + size > PB.pend_cap) {
        size_t cap = PB.pend_cap ? PB.pend_cap * 2 : (size_t)4 << 20;
        while (cap < PB.pend_n + 4 + size) cap *= 2;
        uint8_t *t = realloc(PB.pend, cap);
        if (!t) return 0;
        PB.pend = t, PB.pend_cap = cap;
    }
    if (!pb_insert(pb_hash(type, key, ks), PB.file_size + PB.pend_n, (uint32_t)size)) return 0;
    uint8_t *o = PB.pend + PB.pend_n;
    uint32_t s32 = (uint32_t)size;
    memcpy(o, &s32, 4), memcpy(o + 4, &type, 4), memcpy(o + 8, &ks, 4);
    memset(o + 12, 0, PB_KEY), memcpy(o + 12, key, ks);
    if (an) memcpy(o + 4 + PB_HEAD, a, an);
    if (bn) memcpy(o + 4 + PB_HEAD + an, b, bn);
    PB.pend_n += 4 + size;
    if (type == PB_PIPE) PB.pipes++;
    else PB.blobs++;
    return 1;
}

static void pb_flush(void);

static void pb_capture(VkPipeline p, const VkPipelineBinaryKeyKHR *k) {
    VkPipelineBinaryCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pipeline = p};
    VkPipelineBinaryKHR bins[PB_MAX_BIN];
    VkPipelineBinaryHandlesInfoKHR hi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    uint32_t nb = 0;
    int ok = PB.create(P.device, &ci, NULL, &hi) == VK_SUCCESS && hi.pipelineBinaryCount &&
             hi.pipelineBinaryCount <= PB_MAX_BIN;
    if (ok) {
        hi.pPipelineBinaries = bins;
        ok = PB.create(P.device, &ci, NULL, &hi) == VK_SUCCESS;
        nb = ok ? hi.pipelineBinaryCount : 0;
    }
    VkPipelineBinaryKeyKHR keys[PB_MAX_BIN];
    uint8_t list[4 + PB_MAX_BIN * (4 + PB_KEY)] = {0};
    memcpy(list, &nb, 4);
    for (uint32_t i = 0; ok && i < nb; i++) {
        VkPipelineBinaryDataInfoKHR di = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, .pipelineBinary = bins[i]};
        keys[i] = (VkPipelineBinaryKeyKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        size_t ds = 0;
        ok = PB.data(P.device, &di, &keys[i], &ds, NULL) == VK_SUCCESS && keys[i].keySize && keys[i].keySize <= PB_KEY;
        if (!ok) break;
        memcpy(list + 4 + i * (4 + PB_KEY), &keys[i].keySize, 4), memcpy(list + 8 + i * (4 + PB_KEY), keys[i].key, keys[i].keySize);
        plat_mutex_lock(&PB.lock);
        int have = pb_find(pb_hash(PB_BLOB, keys[i].key, keys[i].keySize)) != NULL;
        plat_mutex_unlock(&PB.lock);
        if (have) {
            PB.blob_shared++;
            continue;
        }
        uint8_t *d = malloc(ds ? ds : 1);
        ok = d && PB.data(P.device, &di, &keys[i], &ds, d) == VK_SUCCESS;
        if (ok) {
            plat_mutex_lock(&PB.lock);
            if (pb_put(PB_BLOB, keys[i].key, keys[i].keySize, d, ds, NULL, 0)) PB.blob_new++;
            plat_mutex_unlock(&PB.lock);
        }
        free(d);
    }
    for (uint32_t i = 0; i < nb; i++) PB.destroy(P.device, bins[i], NULL);
    VkReleaseCapturedPipelineDataInfoKHR ri = {.sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, .pipeline = p};
    PB.release(P.device, &ri, NULL);
    plat_mutex_lock(&PB.lock);
    if (ok && pb_put(PB_PIPE, k->key, k->keySize, list, 4 + nb * (4 + PB_KEY), NULL, 0)) PB.captured++;
    else if (!ok) PB.capture_fail++;
    int full = PB.pend_n >= ((size_t)64 << 20);
    plat_mutex_unlock(&PB.lock);
    if (full) pb_flush();
}

static void pb_flush(void) {
    if (!PB.on) return;
    plat_mutex_lock(&PB.lock);
    if (PB.pend_n) {
        FILE *f = fopen(PB.path, "ab");
        int ok = f && fwrite(PB.pend, PB.pend_n, 1, f) == 1;
        if (f) ok = !fclose(f) && ok;
        if (ok) PB.file_size += PB.pend_n;
        if (ok) PB.pend_n = 0;
        if (!ok) {
            if (PB.rf) fclose(PB.rf);
            int cut = plat_file_truncate(PB.path, PB.file_size);
            PB.rf = fopen(PB.path, "rb");
            rt_log("vk_pipeline: pipeline binaries: cannot append to %s; this run's binaries stay in memory%s", PB.path,
                   cut ? "" : " (and the file cannot be cut back to its whole records)");
        }
    }
    plat_mutex_unlock(&PB.lock);
}

static void pb_log(const char *when) {
    if (!PB.on) return;
    plat_mutex_lock(&PB.lock);
    rt_log("vk_pipeline: pipeline binaries %s: %llu made from pipeline-binaries.bin (avg %.2f ms), %llu compiled (avg %.1f "
           "ms), %llu captured, %llu could not be stored, %llu stored ones failed; binaries %llu new, %llu shared; %u pipelines and "
           "%u binaries in the file, %.1f MB", when,
           (unsigned long long)PB.from_file, PB.from_file ? PB.from_ms / PB.from_file : 0.0,
           (unsigned long long)PB.compiled, PB.compiled ? PB.compile_ms / PB.compiled : 0.0,
           (unsigned long long)PB.captured, (unsigned long long)PB.capture_fail, (unsigned long long)PB.load_fail,
           (unsigned long long)PB.blob_new, (unsigned long long)PB.blob_shared, PB.pipes, PB.blobs,
           (PB.file_size + PB.pend_n) / 1048576.0);
    plat_mutex_unlock(&PB.lock);
}

typedef struct { VkStructureType sType; const void *pNext; VkPipelineCreateFlags flags; } PipeCiHead;

static PlatMutex cache_rw = PLAT_MUTEX_INIT;
static volatile int32_t cache_saving_at_exit;

static void cache_build_enter(void) {
    for (;;) {
        while (__atomic_load_n(&cache_saving_at_exit, __ATOMIC_ACQUIRE)) plat_sleep_ms(1);
        plat_mutex_lock_shared(&cache_rw);
        if (!__atomic_load_n(&cache_saving_at_exit, __ATOMIC_ACQUIRE)) return;
        plat_mutex_unlock_shared(&cache_rw);
    }
}

static __thread VkPipelineCache own_cache;
static __thread int no_cache;
static volatile uint32_t cache_merges;

static int prep_mode;

static VkResult pipe_create_one(int kind, void *ci, VkPipeline *pipe, VkPipelineCache cache) {
    return kind == 2 ? vkCreateComputePipelines(P.device, cache, 1, ci, NULL, pipe)
                     : vkCreateGraphicsPipelines(P.device, cache, 1, ci, NULL, pipe);
}

static VkResult pipe_create_in(int kind, void *ci, VkPipeline *pipe, VkPipelineCache cache) {
    if (no_cache) cache = VK_NULL_HANDLE;
    int shared = cache && !own_cache;
    if (cache && own_cache) cache = own_cache;
    uint64_t t0 = perf_mark_begin();
    if (shared) cache_build_enter();
    VkResult r;
    if (shared && P.play) {
        PipeCiHead *hd = ci;
        VkPipelineCreateFlags flags = hd->flags;
        hd->flags = flags | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        r = pipe_create_one(kind, ci, pipe, cache);
        hd->flags = flags;
        if (r == VK_PIPELINE_COMPILE_REQUIRED) {
            r = pipe_create_one(kind, ci, pipe, P.play);
            if (r == VK_SUCCESS && *pipe) __atomic_fetch_add(&P.play_made, 1, __ATOMIC_RELAXED);
        }
    } else {
        r = pipe_create_one(kind, ci, pipe, cache);
    }
    if (shared) plat_mutex_unlock_shared(&cache_rw);
    perf_mark_end(PERF_M_PIPE, t0, 0);
    return r;
}

static struct { volatile uint64_t made, from_cache, compiled, uncached; } FP;

void vk_pipeline_foreign_counts(VkForeignPipes *o) {
    o->made = __atomic_load_n(&FP.made, __ATOMIC_RELAXED), o->from_cache = __atomic_load_n(&FP.from_cache, __ATOMIC_RELAXED);
    o->compiled = __atomic_load_n(&FP.compiled, __ATOMIC_RELAXED), o->uncached = __atomic_load_n(&FP.uncached, __ATOMIC_RELAXED);
}

static VkResult foreign_one(VkDevice dev, int compute, VkPipelineCache cache, uint32_t n, const void *ci,
                            const VkAllocationCallbacks *alloc, VkPipeline *out) {
    return compute ? vkCreateComputePipelines(dev, cache, n, ci, alloc, out)
                   : vkCreateGraphicsPipelines(dev, cache, n, ci, alloc, out);
}

VkResult vk_pipeline_create_foreign(VkDevice dev, int compute, uint32_t n, const void *infos,
                                    const VkAllocationCallbacks *alloc, VkPipeline *out) {
    VkPipelineCache cache = __atomic_load_n(&P.ready, __ATOMIC_ACQUIRE) && dev == P.device ? P.cache : VK_NULL_HANDLE;
    if (!cache) {
        VkResult r = foreign_one(dev, compute, VK_NULL_HANDLE, n, infos, alloc, out);
        if (r == VK_SUCCESS) __atomic_fetch_add(&FP.made, n, __ATOMIC_RELAXED), __atomic_fetch_add(&FP.uncached, n, __ATOMIC_RELAXED);
        return r;
    }
    VkResult r = VK_SUCCESS;
    cache_build_enter();
    if (!P.play) {
        r = foreign_one(dev, compute, cache, n, infos, alloc, out);
        if (r == VK_SUCCESS) __atomic_fetch_add(&FP.made, n, __ATOMIC_RELAXED), __atomic_fetch_add(&cache_merges, n, __ATOMIC_RELAXED);
    }
    for (uint32_t i = 0; P.play && i < n; i++) {
        VkComputePipelineCreateInfo cc;
        VkGraphicsPipelineCreateInfo gc;
        void *ci;
        if (compute) cc = ((const VkComputePipelineCreateInfo *)infos)[i], ci = &cc;
        else gc = ((const VkGraphicsPipelineCreateInfo *)infos)[i], ci = &gc;
        PipeCiHead *hd = ci;
        int flags2 = 0;
        for (const VkBaseInStructure *b = hd->pNext; b && !flags2; b = b->pNext)
            flags2 = b->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR;
        VkPipelineCreateFlags flags = hd->flags;
        VkResult ri = VK_PIPELINE_COMPILE_REQUIRED;
        out[i] = VK_NULL_HANDLE;
        if (!flags2) {
            hd->flags = flags | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
            ri = foreign_one(dev, compute, cache, 1, ci, alloc, &out[i]);
            hd->flags = flags;
            if (ri == VK_SUCCESS) __atomic_fetch_add(&FP.from_cache, 1, __ATOMIC_RELAXED);
        }
        if (ri == VK_PIPELINE_COMPILE_REQUIRED) {
            ri = foreign_one(dev, compute, P.play, 1, ci, alloc, &out[i]);
            if (ri == VK_SUCCESS) {
                __atomic_fetch_add(&FP.compiled, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&P.play_made, 1, __ATOMIC_RELAXED);
                __atomic_fetch_add(&cache_merges, 1, __ATOMIC_RELAXED);
            }
        }
        if (ri == VK_SUCCESS) __atomic_fetch_add(&FP.made, 1, __ATOMIC_RELAXED);
        else if (r == VK_SUCCESS) r = ri;
        if (ri != VK_SUCCESS) out[i] = VK_NULL_HANDLE;
    }
    plat_mutex_unlock_shared(&cache_rw);
    return r;
}

static VkResult pipe_create(int kind, void *ci, VkPipeline *pipe) {
    PipeCiHead *hd = ci;
    VkResult r;
    pb_last = 0;
#define PIPE_CREATE(cache) pipe_create_in(kind, ci, pipe, cache)
    if (!PB.on) return PIPE_CREATE(P.cache);
    VkPipelineCreateInfoKHR kci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, .pNext = ci};
    VkPipelineBinaryKeyKHR key = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    int keyed = PB.key(P.device, &kci, &key) == VK_SUCCESS && key.keySize && key.keySize <= PB_KEY;
    const void *next = hd->pNext;
    if (keyed) {
        VkPipelineBinaryKHR bins[PB_MAX_BIN];
        uint32_t nb = 0;
        uint64_t t0 = plat_now_ns();
        if (pb_load(&key, bins, &nb)) {
            VkPipelineBinaryInfoKHR bi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, .pNext = next,
                                          .binaryCount = nb, .pPipelineBinaries = bins};
            hd->pNext = &bi;
            r = PIPE_CREATE(VK_NULL_HANDLE);
            hd->pNext = next;
            for (uint32_t i = 0; i < nb; i++) PB.destroy(P.device, bins[i], NULL);
            plat_mutex_lock(&PB.lock);
            if (r == VK_SUCCESS) PB.from_file++, PB.from_ms += (plat_now_ns() - t0) / 1e6;
            else PB.load_fail++;
            plat_mutex_unlock(&PB.lock);
            if (r == VK_SUCCESS) return pb_last = 1, r;
        }
    }
    int capture = keyed && !PB.no_capture[kind];
    VkPipelineCreateFlags2CreateInfoKHR f2 = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, .pNext = next,
                                              .flags = (VkPipelineCreateFlags2KHR)hd->flags | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
    uint64_t t0 = plat_now_ns();
    if (capture) hd->pNext = &f2;
    r = PIPE_CREATE(P.cache);
    hd->pNext = next;
    if (capture && r < 0) {
        PB.no_capture[kind] = 1;
        rt_log("vk_pipeline: pipeline binaries: a %s with its data captured failed (%d); such pipelines are not stored",
               kind == 2 ? "compute pipeline" : kind ? "pipeline library" : "graphics pipeline", (int)r);
        capture = 0;
        r = PIPE_CREATE(P.cache);
    }
#undef PIPE_CREATE
    if (r == VK_SUCCESS && *pipe) {
        plat_mutex_lock(&PB.lock);
        PB.compiled++, PB.compile_ms += (plat_now_ns() - t0) / 1e6;
        plat_mutex_unlock(&PB.lock);
        if (capture) pb_capture(*pipe, &key);
    }
    return r;
}

static uint64_t cache_cap(void) {
    static uint64_t cap = UINT64_MAX;
    if (cap != UINT64_MAX) return cap;
    const char *set = cfg_get("NX2SARIA_PIPELINE_CACHE_MAX_MB");
    float used = 0, total = 0, proc = 0;
    uint64_t mb;
    if (set && *set) mb = strtoull(set, NULL, 10);
    else {
        plat_mem_info(&used, &total, &proc);
        mb = total > 0 ? (uint64_t)total / 32 : 512;
        mb = mb < 1024 ? 1024 : mb > 4096 ? 4096 : mb;
    }
    if (set && *set)
        rt_log("vk_pipeline: pipeline cache files up to %llu MB%s (NX2SARIA_PIPELINE_CACHE_MAX_MB)",
               (unsigned long long)mb, mb ? "" : ": no limit");
    else
        rt_log("vk_pipeline: pipeline cache files up to %llu MB (from %.0f GB of RAM; NX2SARIA_PIPELINE_CACHE_MAX_MB "
               "sets it)", (unsigned long long)mb, total / 1024.0);
    return cap = mb << 20;
}

static int cache_over_cap;
static int cache_foreign;
static void *cache_data_read(VkCtx *c, const char *path, size_t *size) {
    *size = 0;
    cache_over_cap = 0;
    if (!path[0]) return NULL;
    uint64_t cap = cache_cap(), len = 0;
    int why = CACHE_FILE_NONE;
    void *data = cache_file_read(path, sizeof(VkPipelineCacheHeaderVersionOne), cap, &len, &why);
    if (!data) {
        if (why == CACHE_FILE_OVER_CAP)
            cache_over_cap = 1,
            rt_log("vk_pipeline: %s holds %.0f MB, over NX2SARIA_PIPELINE_CACHE_MAX_MB %llu: starting empty; it is "
                   "replaced when the play file is next folded in, at the next start that finds one", path, len / 1048576.0, (unsigned long long)(cap >> 20));
        else if (why != CACHE_FILE_NONE)
            rt_log("vk_pipeline: %s (%llu bytes, %.1f MB) is not read: %s; starting empty", path,
                   (unsigned long long)len, len / 1048576.0, cache_file_why(why));
        return NULL;
    }
    VkPipelineCacheHeaderVersionOne h;
    memcpy(&h, data, sizeof h);
    if (h.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || h.vendorID != c->props.vendorID ||
        h.deviceID != c->props.deviceID || memcmp(h.pipelineCacheUUID, c->props.pipelineCacheUUID, VK_UUID_SIZE)) {
        rt_log("vk_pipeline: %s (%.1f MB) was written by another device or driver; starting empty", path,
               len / 1048576.0);
        if (path == P.cache_path) cache_foreign = 1;
        free(data);
        return NULL;
    }
    *size = (size_t)len;
    return data;
}

static VkPipelineCache cache_make(VkCtx *c, const void *data, size_t size, size_t *kept) {
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = size,
                                    .pInitialData = size ? data : NULL};
    if (vkCreatePipelineCache(c->device, &ci, NULL, &cache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = NULL;
        if (vkCreatePipelineCache(c->device, &ci, NULL, &cache) != VK_SUCCESS) cache = VK_NULL_HANDLE;
    }
    *kept = 0;
    if (cache && vkGetPipelineCacheData(c->device, cache, kept, NULL) != VK_SUCCESS) *kept = 0;
    return cache;
}

static int pg_draws_min(void);

static void cache_stamp_check(void) {
    if (!P.cache_path[0]) return;
    if (!cfg_bool("NX2SARIA_PIPELINE_CACHE_STAMP")) {
        rt_log("vk: pipeline cache stamp off (NX2SARIA_PIPELINE_CACHE_STAMP 0): the files are read whichever "
               "translator wrote them");
        return;
    }
    static const uint8_t zero[32];
    const uint8_t *cur = gfx_shader_identity();
    if (!memcmp(cur, zero, 32)) {
        rt_log("vk: pipeline cache stamp none: no translator identity, so the files are read and not stamped");
        return;
    }
    char stamp[sizeof P.cache_path + 8], play[sizeof P.play_path];
    cache_stamp_sibling(P.cache_path, ".id", stamp, sizeof stamp);
    cache_stamp_sibling(P.cache_path, "-play.bin", play, sizeof play);
    uint64_t n = 0;
    char *text = (char *)plat_file_read(stamp, &n);
    uint8_t was[32] = {0};
    int state = cache_stamp_state(text, text ? (size_t)n : 0, cur, was);
    free(text);
    int act = cache_stamp_action(state, state == CACHE_STAMP_DIFFERS && gfx_shader_carries(was), prep_mode);
    int64_t s0 = plat_file_size(P.cache_path), s1 = plat_file_size(play);
    double mb = ((s0 > 0 ? s0 : 0) + (s1 > 0 ? s1 : 0)) / 1048576.0;
    const char *left = "";
    if (act == CACHE_STAMP_DROP) {
        plat_file_delete(P.cache_path);
        plat_file_delete(play);
        if (plat_file_exists(P.cache_path) || plat_file_exists(play))
            act = CACHE_STAMP_READ, left = " (not deleted: a file is in use; read, the stamp kept)";
    }
    if (act == CACHE_STAMP_DROP || act == CACHE_STAMP_READ_STAMP) {
        char line[66];
        cache_stamp_format(cur, line);
        if (!plat_file_write_atomic(stamp, line, 65)) left = " (the stamp could not be written)";
    }
    char was_text[40] = "";
    if (state == CACHE_STAMP_DIFFERS)
        snprintf(was_text, sizeof was_text, "the files' translator %02x%02x%02x%02x; ", was[0], was[1], was[2], was[3]);
    int at_exit = cache_base_at_exit(prep_mode, plat_file_size(P.cache_path));
    rt_log("vk: pipeline cache stamp %02x%02x%02x%02x %s: %s%s, %.1f MB (%s%s%s)", cur[0], cur[1], cur[2], cur[3],
           cache_stamp_state_name(state), cache_stamp_action_name(act), left, mb, was_text,
           at_exit ? "pipeline-cache.bin is written at this run's exit; " : "", stamp);
}

static void cache_load(VkCtx *c) {
    cache_path_init();
    pb_init(c);
    if (PB.on) {
        rt_log("vk_pipeline: %s is not read or written while pipeline binaries are on", P.cache_path);
        return;
    }
    cache_stamp_check();
    P.base_at_exit = P.cache_path[0] && cache_base_at_exit(prep_mode, plat_file_size(P.cache_path));
    size_t size = 0, kept = 0;
    void *data = cache_data_read(c, P.cache_path, &size);
    int base_over = cache_over_cap;
    P.cache = cache_make(c, data, size, &kept);
    free(data);
    P.cache_loaded = kept;
    P.saved[0] = !size || kept == size ? kept : 0;
    if (size)
        rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it", P.cache_path, size / 1048576.0,
               kept / 1048576.0);
    if (!P.cache || !P.cache_path[0]) return;

    cache_stamp_sibling(P.cache_path, "-play.bin", P.play_path, sizeof P.play_path);
    int want = cfg_bool("NX2SARIA_PIPELINE_CACHE_PLAY") && !prep_mode, on = want && P.cache_control;
    size_t psize = 0, pkept = 0;
    void *pdata = cache_data_read(c, P.play_path, &psize);
    if (base_over && cache_over_cap)
        rt_log("vk_pipeline: both pipeline cache files are over NX2SARIA_PIPELINE_CACHE_MAX_MB, so what one run builds does "
               "not fit it: this start builds every pipeline again, and so will each one after it until the limit is "
               "raised above the play file's size (pipeline_cache_max_mb in nx2saria.ini (or an existing nx2navi.ini); 0 = no limit)");
    VkPipelineCache play = psize || on ? cache_make(c, pdata, psize, &pkept) : VK_NULL_HANDLE;
    free(pdata);
    uint64_t fold_at = (uint64_t)strtoull(cfg_value("NX2SARIA_PIPELINE_CACHE_PLAY_MB"), NULL, 10) << 20;
    int fold = play && psize && (!on || psize >= fold_at || base_over);
    if (fold && on && !base_over && cache_cap()) {
        VkResult r = vkMergePipelineCaches(c->device, P.cache, 1, &play);
        size_t merged = 0;
        if (r == VK_SUCCESS && vkGetPipelineCacheData(c->device, P.cache, &merged, NULL) == VK_SUCCESS &&
            merged + fold_at > cache_cap()) {
            rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it; folding it in would make %s %.1f MB, "
                   "with less than %llu MB of room left under the limit of %llu MB: both files stay as they are, and "
                   "this run's pipelines go into the play file as long as it stays under the limit", P.play_path,
                   psize / 1048576.0, pkept / 1048576.0, P.cache_path, merged / 1048576.0,
                   (unsigned long long)(fold_at >> 20), (unsigned long long)(cache_cap() >> 20));
            fold = 0, psize = 0;
        } else if (r != VK_SUCCESS) {
            rt_log("vk_pipeline: %s did not merge into the cache of %s (%d); it is left as it is and not used in this run",
                   P.play_path, P.cache_path, (int)r);
            vkDestroyPipelineCache(c->device, play, NULL);
            play = VK_NULL_HANDLE, fold = 0, on = 0, psize = 0;
        } else {
            rt_log("vk_pipeline: folding %s in makes %s %.1f MB (the limit is %llu MB)", P.play_path, P.cache_path,
                   merged / 1048576.0, (unsigned long long)(cache_cap() >> 20));
            vkDestroyPipelineCache(c->device, play, NULL);
            play = VK_NULL_HANDLE;
            P.fold = 1, P.saved[0] = 0;
            rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it; merged into the cache of %s, which is "
                   "written once now (it reached the size at which it is folded in, before play)", P.play_path,
                   psize / 1048576.0, pkept / 1048576.0, P.cache_path);
            play = cache_make(c, NULL, 0, &pkept), psize = 0;
            fold = 0;
        }
    }
    if (fold) {
        VkResult r = vkMergePipelineCaches(c->device, P.cache, 1, &play);
        vkDestroyPipelineCache(c->device, play, NULL);
        play = VK_NULL_HANDLE;
        if (r == VK_SUCCESS) {
            P.fold = 1, P.saved[0] = 0;
            rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it; merged into the cache of %s, which is "
                   "written once now (%s)", P.play_path, psize / 1048576.0, pkept / 1048576.0, P.cache_path,
                   prep_mode ? "a preparation builds into that file alone"
                   : !want ? "NX2SARIA_PIPELINE_CACHE_PLAY=0"
                   : !P.cache_control ? "no pipelineCreationCacheControl on this device"
                   : "it reached the size at which it is folded in, before play");
            if (on) play = cache_make(c, NULL, 0, &pkept), psize = 0;
        } else {
            rt_log("vk_pipeline: %s did not merge into the cache of %s (%d); it is left as it is and not used in this run",
                   P.play_path, P.cache_path, (int)r);
            on = 0;
        }
    } else if (psize) {
        VkResult r = vkMergePipelineCaches(c->device, P.cache, 1, &play);
        rt_log("vk_pipeline: %s: %.1f MB read, the driver holds %.1f MB of it%s", P.play_path, psize / 1048576.0,
               pkept / 1048576.0, r == VK_SUCCESS ? "" : "; it did not merge into the other cache in memory");
    }
    P.play = on ? play : VK_NULL_HANDLE;
    P.play_loaded = P.play ? pkept : 0;
    P.saved[1] = P.play && (!psize || pkept == psize) ? pkept : 0;
    if (P.play)
        rt_log("vk_pipeline: what this run compiles goes into %s; %s %s", P.play_path, P.cache_path,
               P.base_at_exit ? "was not there at the start, so the save at this run's exit writes it once, with what "
                                "the run takes from the driver's own cache"
                              : "is not written in play");
    else if (want && !P.cache_control)
        rt_log("vk_pipeline: no play cache (no pipelineCreationCacheControl on this device): a new pipeline has %s "
               "written whole", P.cache_path);
}

static void mesh_stages_init(uint32_t sg_min, uint32_t sg_max) {
    P.mesh_sg = rt_knob("VK_MESH_SUBGROUPS", "1") == 1;
    P.mesh_invariant = rt_knob("VK_MESH_INVARIANT", "1") == 1;
    const VkShaderStageFlags task = VK_SHADER_STAGE_TASK_BIT_EXT, mesh = VK_SHADER_STAGE_MESH_BIT_EXT;
    const char *why = !P.mesh_sg ? "off (NX2SARIA_VK_MESH_SUBGROUPS=0)"
                    : !sg_max ? "not available (no subgroup size control)"
                    : !(P.sg32_stages & task) ? "the task stage cannot be pinned here (not in requiredSubgroupSizeStages, "
                                                "32 outside the device's range, or NX2SARIA_VK_SUBGROUP_PIN=0)"
                    : sg_min == 32 && sg_max == 32 ? "on (subgroups are always 32 lanes here: no change)" : "on";
    rt_log("vk_pipeline: task and mesh stages: subgroup size 32 on the task stage and full subgroups %s; device %u-%u lanes, "
           "pinnable stages task %s mesh %s, full subgroups %s; mesh positions Invariant %s",
           why, sg_min, sg_max, P.sg32_stages & task ? "yes" : "no", P.sg32_stages & mesh ? "yes" : "no",
           P.f_full_subgroups ? "yes" : "no", P.mesh_invariant ? "on" : "off (NX2SARIA_VK_MESH_INVARIANT=0)");
}

static void nan_preserve_init(VkCtx *c) {
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceFloatControlsProperties fc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES,
                                                  .pNext = &drv};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &fc};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    const char *e = cfg_get("NX2SARIA_VK_NAN_PRESERVE");
    int set = e && *e, want = set ? strcmp(e, "0") != 0 : drv.driverID != VK_DRIVER_ID_NVIDIA_PROPRIETARY;
    uint32_t can = (fc.shaderSignedZeroInfNanPreserveFloat16 ? NP_16 : 0) |
                   (fc.shaderSignedZeroInfNanPreserveFloat32 ? NP_32 : 0) |
                   (fc.shaderSignedZeroInfNanPreserveFloat64 ? NP_64 : 0);
    P.nan_preserve = want && (can & NP_32) ? can : 0;
    const char *nb = cfg_get("NX2SARIA_VK_NAN_BITS");
    P.nan_bits = nb && *nb ? strcmp(nb, "0") != 0 : vk_caps.amd_sc;
    if (P.nan_bits)
        rt_log("vk_pipeline: game shaders test for NaN on the bits of the number, not as a float (%s)",
               nb && *nb ? "NX2SARIA_VK_NAN_BITS=1" : "AMD's driver for RDNA 1 and 2");
    if (P.nan_preserve)
        rt_log("vk_pipeline: game shaders keep NaN, infinity and signed zero (SignedZeroInfNanPreserve for %s32%s-bit "
               "floats%s)", can & NP_16 ? "16, " : "", can & NP_64 ? ", 64" : "", set ? ", NX2SARIA_VK_NAN_PRESERVE=1" : "");
    else
        rt_log("vk_pipeline: game shaders without SignedZeroInfNanPreserve (%s)",
               !want ? (set ? "NX2SARIA_VK_NAN_PRESERVE=0" : "NVIDIA's driver") : "the driver cannot keep them in 32-bit floats");
}

static void keys_open(void);
static void cu_open(void);
static void prebuild_start(void);

static VkPipelineCreateFlags noopt_flag_for(int mesh);

static void mesh_lib_init(const VkCtx *c) {
    const char *why = !c->has_mesh_shader ? "this GPU has no mesh shaders"
                    : c->mesh_fallback ? "mesh draws go through the mesh fallback (vk_device.c, mesh_fallback)"
                    : !vk_caps.gpl ? "the graphics pipeline library is off"
                    : !cfg_bool("NX2SARIA_GPL_MESH") ? "NX2SARIA_GPL_MESH=0"
                    : noopt_flag_for(1) != noopt_flag_for(0) ? "they alone are built without the driver's optimiser "
                                                               "(NX2SARIA_VK_NOOPT), which a shared library would not be"
                    : NULL;
    P.mesh_lib = !why;
    if (why) rt_log("vk_pipeline: mesh pipelines are built whole, without stage libraries: %s", why);
    else rt_log("vk_pipeline: mesh pipelines are linked from stage libraries like vertex pipelines: the task and mesh "
                "stages in one pre-rasterization library, a fast link at the first draw, the whole build behind it "
                "(NX2SARIA_GPL_MESH=0 builds each whole, as before)");
}

static int ensure_init(void) {
    if (P.ready) return 1;
    if (!vk_init()) return 0;
    plat_mutex_lock(&P.lock);
    if (P.ready) {
        plat_mutex_unlock(&P.lock);
        return 1;
    }
    VkCtx *c = vk_ctx();
    P.device = c->device;
    __atomic_store_n(&pipe_minute, minute_now(), __ATOMIC_RELAXED);
    VkPhysicalDevicePushDescriptorPropertiesKHR pd = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pd};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    P.max_push = pd.maxPushDescriptors;
    P.indep = vk_caps.gpl;
    P.gpl = vk_caps.gpl;
    rt_log(P.gpl ? "vk_pipeline: graphics pipeline library on: a draw whose pipeline is building gets a fast-linked one, "
                   "the optimised build replaces it when done"
                 : "vk_pipeline: graphics pipeline library off: draws wait for or skip pipelines that are building");
    const char *ps = cfg_get("NX2SARIA_PUSH_STAGE"), *vo = cfg_get("NX2SARIA_VS_OUTS");
    P.push_stage = ps && !strcmp(ps, "fs") ? GFX_STAGE_FS : GFX_STAGE_VS;
    P.vs_outs_all = vo && !strcmp(vo, "all");
    if (P.indep) rt_log("vk_pipeline: stage-independent layouts: vertex set 0, fragment set 1, %s set pushed; vertex outputs: %s",
                        P.push_stage == GFX_STAGE_VS ? "vertex" : "fragment", P.vs_outs_all ? "all 32 locations" : "union of paired fragment inputs");
    mesh_lib_init(c);
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT e3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pvf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,
                                                      .pNext = c->has_eds3 ? &e3 : NULL};
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = c->has_provoking_vertex ? (void *)&pvf : (c->has_eds3 ? (void *)&e3 : NULL)};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    P.pv_last = c->has_provoking_vertex && pvf.provokingVertexLast;
    if (!P.pv_last) rt_log("vk_pipeline: provokingVertexLast unavailable: flat outputs come from the first vertex (NVN: last)");
    uint32_t sg_min = 0, sg_max = 0;
    if (v13.subgroupSizeControl) {
        VkPhysicalDeviceVulkan13Properties p13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceProperties2 q2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p13};
        vkGetPhysicalDeviceProperties2(c->phys, &q2);
        P.sg32_stages = vk_caps.sg_pin_stages;
        sg_min = p13.minSubgroupSize, sg_max = p13.maxSubgroupSize;
        rt_log("vk_pipeline: subgroup size %u-%u, requiredSubgroupSizeStages %#x, pinning %u on %#x", p13.minSubgroupSize,
               p13.maxSubgroupSize, (unsigned)p13.requiredSubgroupSizeStages, vk_caps.sg_size, (unsigned)P.sg32_stages);
    }
    P.reconverge = c->has_maximal_reconvergence && rt_knob("VK_RECONVERGE", "1") == 1;
    rt_log("vk_pipeline: maximal reconvergence for shaders with subgroup ops: %s",
           P.reconverge ? "on" : c->has_maximal_reconvergence ? "off (NX2SARIA_VK_RECONVERGE=0)" : "not supported by the driver");
    nan_preserve_init(c);
    P.f_full_subgroups = v13.computeFullSubgroups;
    mesh_stages_init(sg_min, sg_max);
    P.cache_control = v13.pipelineCreationCacheControl;
    P.feedback = c->props.apiVersion >= VK_API_VERSION_1_3 ? atoi(cfg_value("NX2SARIA_PIPE_FEEDBACK")) : 0;
    P.f_logic_op = f2.features.logicOp;
    P.f_depth_clamp = f2.features.depthClamp;
    P.f_fill_non_solid = f2.features.fillModeNonSolid;
    P.f_dual_src = f2.features.dualSrcBlend;
    P.wide_lines = f2.features.wideLines && cfg_bool("NX2SARIA_LINE_WIDTH");
    P.line_w[0] = c->props.limits.lineWidthRange[0], P.line_w[1] = c->props.limits.lineWidthRange[1];
    P.line_gran = c->props.limits.lineWidthGranularity;
    if (!P.wide_lines)
        rt_log("vk_pipeline: lines are drawn 1 pixel wide whatever width the game sets (%s)",
               f2.features.wideLines ? "NX2SARIA_LINE_WIDTH=0" : "this device has no wide lines");
#define PROC(field, name) (P.field = (void *)vk_dpa(c->device, name))
    PROC(push, "vkCmdPushDescriptorSetKHR");
    if (!cfg_bool("NX2SARIA_DESC_TEMPLATE")) {
        rt_log("vk_pipeline: descriptor update templates off (NX2SARIA_DESC_TEMPLATE=0)");
    } else if (c->props.apiVersion >= VK_API_VERSION_1_1 && PROC(tpl_create, "vkCreateDescriptorUpdateTemplate") &&
               PROC(tpl_destroy, "vkDestroyDescriptorUpdateTemplate") && PROC(tpl_update, "vkUpdateDescriptorSetWithTemplate")) {
        P.tpl = 1;
        if (!PROC(tpl_push, "vkCmdPushDescriptorSetWithTemplateKHR"))
            rt_log("vk_pipeline: vkCmdPushDescriptorSetWithTemplateKHR missing: push sets are written with write structs");
    } else {
        rt_log("vk_pipeline: descriptor update templates unavailable: sets are written with write structs");
    }
    const char *dv = cfg_get("NX2SARIA_VK_DYN_VERTEX");
    int intel_legacy = c->props.vendorID == 0x8086 && c->props.apiVersion <= VK_MAKE_API_VERSION(0, 1, 3, 215);
    int dv_on = dv && *dv ? strcmp(dv, "0") != 0 : !intel_legacy && !vk_caps.amd_llpc;
    P.dyn_vi = dv_on && c->has_vertex_input_dynamic_state && PROC(set_vi, "vkCmdSetVertexInputEXT");
    if (!dv_on && c->has_vertex_input_dynamic_state)
        rt_log("vk_pipeline: dynamic vertex input off (%s)",
               dv && *dv ? "NX2SARIA_VK_DYN_VERTEX=0"
               : intel_legacy ? "Intel's older Windows driver draws nothing with it"
               : "AMD's driver compiles pipelines that have it again at every boot");
    if (c->has_eds3) {
        P.dyn_clamp = e3.extendedDynamicState3DepthClampEnable && P.f_depth_clamp && PROC(set_clamp, "vkCmdSetDepthClampEnableEXT");
        P.dyn_poly = e3.extendedDynamicState3PolygonMode && PROC(set_poly, "vkCmdSetPolygonModeEXT");
        P.dyn_blend_en = e3.extendedDynamicState3ColorBlendEnable && PROC(set_blend_en, "vkCmdSetColorBlendEnableEXT");
        P.dyn_blend_eq = e3.extendedDynamicState3ColorBlendEquation && PROC(set_blend_eq, "vkCmdSetColorBlendEquationEXT");
        P.dyn_wmask = e3.extendedDynamicState3ColorWriteMask && PROC(set_wmask, "vkCmdSetColorWriteMaskEXT");
        P.dyn_a2c = e3.extendedDynamicState3AlphaToCoverageEnable && PROC(set_a2c, "vkCmdSetAlphaToCoverageEnableEXT");
    }
    const char *da = cfg_get("NX2SARIA_VK_DYN_A2C");
    int marks = vk_pool_marks_enabled();
    int da_on = !marks && (da && *da ? strcmp(da, "0") != 0 : !vk_caps.amd_sc);
    if (!da_on && P.dyn_a2c) {
        P.dyn_a2c = 0;
        rt_log("vk_pipeline: dynamic alpha to coverage off (%s)", marks ? "the texture pool marks, NX2SARIA_TEX_DISUSE"
                                                                  : da && *da ? "NX2SARIA_VK_DYN_A2C=0"
                                                                              : "AMD's driver for RDNA 2 and older");
    }
#undef PROC
    static const VkDynamicState core[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
    P.n_dyn = 0;
    for (size_t i = 0; i < sizeof core / sizeof *core; i++) P.dyn[P.n_dyn++] = core[i];
    P.dyn[P.n_dyn++] = P.dyn_vi ? VK_DYNAMIC_STATE_VERTEX_INPUT_EXT : VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    if (P.dyn_clamp) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    if (P.dyn_poly) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
    if (P.dyn_blend_en) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
    if (P.dyn_blend_eq) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
    if (P.dyn_wmask) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
    if (P.dyn_a2c) P.dyn[P.n_dyn++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT;
    vk_mem_stage_log("device creation");
    cache_load(c);
    vk_mem_stage_log("the pipeline cache load");
    pg_draws_min();
    rt_log("vk_pipeline: max push descriptors %u; dynamic vertex input %d, depth clamp %d, polygon mode %d, blend enable %d, "
           "blend equation %d, write mask %d, alpha to coverage %d; cache %s",
           P.max_push, P.dyn_vi, P.dyn_clamp, P.dyn_poly, P.dyn_blend_en, P.dyn_blend_eq, P.dyn_wmask, P.dyn_a2c,
           P.cache_path[0] ? P.cache_path : "<memory only>");
    keys_open();
    cu_open();
    plat_xchg32(&P.ready, 1);
    plat_mutex_unlock(&P.lock);
    prebuild_start();
    return 1;
}

static VkBlendFactor blend_factor(unsigned f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: LOG_ONCE("vk_pipeline: unknown NVN blend factor %#x, using ONE", f); return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(unsigned e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_op(unsigned f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp stencil_op(unsigned o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }
static VkPolygonMode polygon_mode(unsigned m) {
    if (!P.f_fill_non_solid || m > 2) return VK_POLYGON_MODE_FILL;
    return (VkPolygonMode)(2 - m);
}

VkPrimitiveTopology vk_pipeline_topology(int prim) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_LOOP:
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:
    case NVN_DRAW_PRIMITIVE_QUAD_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:
    case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case NVN_DRAW_PRIMITIVE_PATCHES: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static int topology_class(VkPrimitiveTopology t) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return 1;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return 3;
    default: return 2;
    }
}

int vk_pipeline_tex_type(int target) {
    switch (target) {
    case NVN_TEXTURE_TARGET_1D: return GFX_TEX_1D;
    case NVN_TEXTURE_TARGET_3D: return GFX_TEX_3D;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return GFX_TEX_1D_ARRAY;
    case NVN_TEXTURE_TARGET_2D_ARRAY: return GFX_TEX_2D_ARRAY;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE: return GFX_TEX_2D_MS;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return GFX_TEX_2D_MS_ARRAY;
    case NVN_TEXTURE_TARGET_CUBEMAP: return GFX_TEX_CUBE;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return GFX_TEX_CUBE_ARRAY;
    case NVN_TEXTURE_TARGET_BUFFER: return GFX_TEX_BUFFER;
    default: return GFX_TEX_2D;
    }
}

static int format_type(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_S8_UINT:
        return GFX_TYPE_UINT;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_B8G8R8A8_SINT:
        return GFX_TYPE_SINT;
    default: return GFX_TYPE_FLOAT;
    }
}

static VkFormatFeatureFlags format_features(VkFormat f, int optimal) {
    static VkFormatFeatureFlags known[2][256];
    static uint8_t have[2][256];
    VkCtx *c = vk_ctx();
    if (!c) return 0;
    if ((unsigned)f < 256 && have[optimal][f]) return known[optimal][f];
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(c->phys, f, &p);
    VkFormatFeatureFlags r = optimal ? p.optimalTilingFeatures : p.bufferFeatures;
    if ((unsigned)f < 256) known[optimal][f] = r, have[optimal][f] = 1;
    return r;
}

typedef struct { VkFormat vk; uint8_t type; } VtxFmt;
#define F(v) {VK_FORMAT_##v, GFX_TYPE_FLOAT}
#define U(v) {VK_FORMAT_##v, GFX_TYPE_UINT}
#define I(v) {VK_FORMAT_##v, GFX_TYPE_SINT}
static const VtxFmt vtx_formats[0x9B] = {
    [0x01] = F(R8_UNORM), [0x02] = F(R8_SNORM), [0x03] = U(R8_UINT), [0x04] = I(R8_SINT), [0x05] = F(R16_SFLOAT),
    [0x06] = F(R16_UNORM), [0x07] = F(R16_SNORM), [0x08] = U(R16_UINT), [0x09] = I(R16_SINT), [0x0A] = F(R32_SFLOAT),
    [0x0B] = U(R32_UINT), [0x0C] = I(R32_SINT), [0x0D] = F(R8G8_UNORM), [0x0E] = F(R8G8_SNORM), [0x0F] = U(R8G8_UINT),
    [0x10] = I(R8G8_SINT), [0x11] = F(R16G16_SFLOAT), [0x12] = F(R16G16_UNORM), [0x13] = F(R16G16_SNORM),
    [0x14] = U(R16G16_UINT), [0x15] = I(R16G16_SINT), [0x16] = F(R32G32_SFLOAT), [0x17] = U(R32G32_UINT),
    [0x18] = I(R32G32_SINT), [0x19] = F(R8G8B8_UNORM), [0x1A] = F(R8G8B8_SNORM), [0x1B] = U(R8G8B8_UINT),
    [0x1C] = I(R8G8B8_SINT), [0x1D] = F(R16G16B16_SFLOAT), [0x1E] = F(R16G16B16_UNORM), [0x1F] = F(R16G16B16_SNORM),
    [0x20] = U(R16G16B16_UINT), [0x21] = I(R16G16B16_SINT), [0x22] = F(R32G32B32_SFLOAT), [0x23] = U(R32G32B32_UINT),
    [0x24] = I(R32G32B32_SINT), [0x25] = F(R8G8B8A8_UNORM), [0x26] = F(R8G8B8A8_SNORM), [0x27] = U(R8G8B8A8_UINT),
    [0x28] = I(R8G8B8A8_SINT), [0x29] = F(R16G16B16A16_SFLOAT), [0x2A] = F(R16G16B16A16_UNORM),
    [0x2B] = F(R16G16B16A16_SNORM), [0x2C] = U(R16G16B16A16_UINT), [0x2D] = I(R16G16B16A16_SINT),
    [0x2E] = F(R32G32B32A32_SFLOAT), [0x2F] = U(R32G32B32A32_UINT), [0x30] = I(R32G32B32A32_SINT),
    [0x3D] = F(A2B10G10R10_UNORM_PACK32), [0x3E] = U(A2B10G10R10_UINT_PACK32), [0x3F] = F(B10G11R11_UFLOAT_PACK32),
    [0x51] = F(R8_USCALED), [0x52] = F(R8_SSCALED), [0x53] = F(R16_USCALED), [0x54] = F(R16_SSCALED),
    [0x57] = F(R8G8_USCALED), [0x58] = F(R8G8_SSCALED), [0x59] = F(R16G16_USCALED), [0x5A] = F(R16G16_SSCALED),
    [0x5D] = F(R8G8B8_USCALED), [0x5E] = F(R8G8B8_SSCALED), [0x5F] = F(R16G16B16_USCALED), [0x60] = F(R16G16B16_SSCALED),
    [0x63] = F(R8G8B8A8_USCALED), [0x64] = F(R8G8B8A8_SSCALED), [0x65] = F(R16G16B16A16_USCALED),
    [0x66] = F(R16G16B16A16_SSCALED),
    [0x55] = U(R32_UINT), [0x56] = I(R32_SINT), [0x5B] = U(R32G32_UINT), [0x5C] = I(R32G32_SINT),
    [0x61] = U(R32G32B32_UINT), [0x62] = I(R32G32B32_SINT), [0x67] = U(R32G32B32A32_UINT), [0x68] = I(R32G32B32A32_SINT),
    [0x69] = F(A2B10G10R10_SNORM_PACK32), [0x6A] = I(A2B10G10R10_SINT_PACK32), [0x6B] = F(A2B10G10R10_USCALED_PACK32),
    [0x6C] = F(A2B10G10R10_SSCALED_PACK32), [0x9A] = F(B8G8R8A8_UNORM),
};
#undef F
#undef U
#undef I

static VkFormat widen(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R8G8B8_USCALED: return VK_FORMAT_R8G8B8A8_USCALED;
    case VK_FORMAT_R8G8B8_SSCALED: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case VK_FORMAT_R16G16B16_USCALED: return VK_FORMAT_R16G16B16A16_USCALED;
    case VK_FORMAT_R16G16B16_SSCALED: return VK_FORMAT_R16G16B16A16_SSCALED;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat vk_vertex_format(int nvn_format, int *type) {
    if (type) *type = GFX_TYPE_FLOAT;
    if (nvn_format <= 0 || nvn_format >= (int)(sizeof vtx_formats / sizeof *vtx_formats)) return VK_FORMAT_UNDEFINED;
    const VtxFmt *v = &vtx_formats[nvn_format];
    if (v->vk == VK_FORMAT_UNDEFINED) return VK_FORMAT_UNDEFINED;
    if (type) *type = v->type;
    if ((nvn_format >= 0x55 && nvn_format <= 0x68) && v->type != GFX_TYPE_FLOAT)
        LOG_ONCE("vk_pipeline: 32-bit *_UI2F/*_I2F vertex formats are read as integers (first: %#x)", nvn_format);
    VkFormat f = v->vk;
    if (vk_ctx() && !(format_features(f, 0) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
        VkFormat w = widen(f);
        if (w == VK_FORMAT_UNDEFINED || !(format_features(w, 0) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            LOG_ONCE("vk_pipeline: vertex format %d (NVN %#x) unsupported", (int)f, nvn_format);
            return VK_FORMAT_UNDEFINED;
        }
        LOG_ONCE("vk_pipeline: vertex format %d unsupported, read as 4 components (first: NVN %#x)", (int)f, nvn_format);
        f = w;
    }
    return f;
}

static VkFormat target_format(int nvn_format, int *depth, int *stencil) {
    TexVkFormat t;
    if (depth) *depth = 0;
    if (stencil) *stencil = 0;
    if (!nvn_format || !tex_vk_format(nvn_format, &t)) return VK_FORMAT_UNDEFINED;
    if (depth) *depth = t.depth;
    if (stencil) *stencil = t.stencil;
    return t.vk;
}

void vk_pipe_state_defaults(VkPipeState *s) {
    memset(s, 0, sizeof *s);
    s->samples = 1;
    s->prim = NVN_DRAW_PRIMITIVE_TRIANGLES;
    for (int i = 0; i < 8; i++)
        s->blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE,
                                 NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    s->color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS};
    s->chmask.mask = 0xffffffffu;
    s->ms.enable = 1;
    s->poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    s->ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                    NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP,
                    NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}

void vk_pipeline_answers(const VkPipeState *s, GfxShaderAnswers *vs, GfxShaderAnswers *fs) {
    if (vs)
        for (int i = 0; i < 16; i++) {
            va_st a;
            memcpy(&a, &s->vattr[i], sizeof a);
            int type = GFX_TYPE_FLOAT;
            if (a.fmt) vk_vertex_format((int)a.fmt, &type);
            vs->attr_type[i] = (uint8_t)type;
        }
    if (fs)
        for (int i = 0; i < 8; i++) {
            VkFormat f = target_format(s->color_fmt[i], NULL, NULL);
            fs->frag_out_type[i] = (uint8_t)(f == VK_FORMAT_UNDEFINED ? GFX_TYPE_FLOAT : format_type(f));
        }
}

typedef VkVtxInput VtxInput;

static VkFormat zero_format(int type) {
    return type == GFX_TYPE_UINT ? VK_FORMAT_R32_UINT : type == GFX_TYPE_SINT ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_SFLOAT;
}

static void vertex_input(const VkPipeState *s, VtxInput *v) {
    GfxShaderIo io[32];
    uint32_t used = gfx_shader_inputs(s->vs, io), streams = 0;
    int zero = 0;
    memset(v, 0, sizeof *v);
    for (int l = 0; l < 32; l++) {
        if (!(used >> l & 1)) continue;
        VkVertexInputAttributeDescription2EXT *a = &v->a[v->n_attr++];
        *a = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                                                     .location = (uint32_t)l};
        va_st va = {0};
        if (l < 16) memcpy(&va, &s->vattr[l], sizeof va);
        VkFormat f = va.fmt ? vk_vertex_format((int)va.fmt, NULL) : VK_FORMAT_UNDEFINED;
        if (f != VK_FORMAT_UNDEFINED && va.stream < 16) {
            a->binding = va.stream, a->format = f, a->offset = va.offset;
            streams |= 1u << va.stream;
        } else {
            a->binding = VK_PIPE_ZERO_BINDING, a->format = zero_format(io[l].type);
            zero = 1;
        }
    }
    for (uint32_t b = 0; b < 16; b++) {
        if (!(streams >> b & 1)) continue;
        uint32_t div = s->vstream[b].divisor;
        if (div > 1) LOG_ONCE("vk_pipeline: instance divisor %u treated as 1 (no VK_EXT_vertex_attribute_divisor)", div);
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = b, .stride = s->vstream[b].stride,
            .inputRate = div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
    }
    if (zero)
        v->b[v->n_bind++] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = VK_PIPE_ZERO_BINDING,
            .stride = 0, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1};
}

int vk_pipeline_vertex_input_dynamic(void) { return ensure_init() && P.dyn_vi; }

PFN_vkCmdPushDescriptorSetKHR vk_pipeline_push_fn(void) { return ensure_init() ? P.push : NULL; }
PFN_vkCmdPushDescriptorSetWithTemplateKHR vk_pipeline_push_tpl_fn(void) { return ensure_init() && P.tpl ? P.tpl_push : NULL; }
PFN_vkUpdateDescriptorSetWithTemplate vk_pipeline_update_tpl_fn(void) { return ensure_init() && P.tpl ? P.tpl_update : NULL; }

int vk_pipeline_set_indep(int on) {
    if (!ensure_init()) return 0;
    plat_mutex_lock(&P.lock);
    int was = P.indep;
    P.indep = on && vk_caps.gpl;
    plat_mutex_unlock(&P.lock);
    return was;
}

static uint32_t *spirv_rebind(const uint32_t *w, uint64_t n, uint32_t off) {
    uint32_t *o = malloc(n * 4);
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
        if (!len || i + len > n || op == SPV_OP_FUNCTION) break;
        if (op == SPV_OP_DECORATE && len >= 4 && o[i + 2] == SPV_DEC_BINDING && o[i + 3] >= 1) o[i + 3] += off;
        i += len;
    }
    return o;
}

static uint32_t *spirv_set(const uint32_t *w, uint64_t n, uint32_t set) {
    uint32_t *o = malloc(n * 4);
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
        if (!len || i + len > n || op == SPV_OP_FUNCTION) break;
        if (op == SPV_OP_DECORATE && len >= 4 && o[i + 2] == SPV_DEC_DESCRIPTOR_SET) o[i + 3] = set;
        i += len;
    }
    return o;
}

static uint32_t *spirv_dual_source(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    uint32_t outs[64];
    int nout = 0;
    uint64_t at = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        if (op == SPV_OP_VARIABLE && len >= 4 && w[i + 3] == SPV_SC_OUTPUT && nout < 64) outs[nout++] = w[i + 2];
        i += len;
    }
    for (uint64_t i = 5; i < n && !at;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == SPV_OP_DECORATE && len == 4 && w[i + 2] == SPV_DEC_LOCATION && w[i + 3] == 1)
            for (int k = 0; k < nout; k++)
                if (outs[k] == w[i + 1]) at = i;
        i += len;
    }
    if (!at) return NULL;
    uint32_t *o = malloc((n + 4) * 4);
    if (!o) return NULL;
    memcpy(o, w, (at + 4) * 4);
    o[at + 3] = 0;
    o[at + 4] = 4u << 16 | SPV_OP_DECORATE, o[at + 5] = w[at + 1], o[at + 6] = SPV_DEC_INDEX, o[at + 7] = 1;
    memcpy(o + at + 8, w + at + 4, (n - at - 4) * 4);
    *out_n = n + 4;
    return o;
}

static uint32_t *spirv_add_outputs(const uint32_t *w, uint64_t n, uint32_t missing, const GfxShaderIo io[32],
                                   uint64_t *out_n) {
    uint64_t ep_at = 0, ann_end = 0, fn_at = 0;
    uint32_t bound = w[3], scalar[3] = {0, 0, 0};
    struct { uint32_t id, comp, count; } vec[64];
    struct { uint32_t id, type; } ptr[64];
    int nvec = 0, nptr = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_ENTRY_POINT && !ep_at) ep_at = i;
        else if (op == SPV_OP_DECORATE || op == 72 || op == 73 || op == 74 || op == 75 || op == 332 || op == 5632 || op == 5633)
            ann_end = i + len;
        else if (op == SPV_OP_TYPE_FLOAT && len >= 3 && a[1] == 32) scalar[GFX_TYPE_FLOAT] = a[0];
        else if (op == SPV_OP_TYPE_INT && len >= 4 && a[1] == 32) scalar[a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT] = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len >= 4 && nvec < 64) vec[nvec].id = a[0], vec[nvec].comp = a[1], vec[nvec++].count = a[2];
        else if (op == SPV_OP_TYPE_POINTER && len >= 4 && a[1] == SPV_SC_OUTPUT && nptr < 64) ptr[nptr].id = a[0], ptr[nptr++].type = a[2];
        else if (op == SPV_OP_FUNCTION) { fn_at = i; break; }
        i += len;
    }
    if (!ep_at || !ann_end || !fn_at || ann_end < ep_at || fn_at < ann_end) return NULL;
    uint32_t types[32 * 40], decs[32 * 4], vars[32], zero[3] = {0, 0, 0}, one[3] = {0, 0, 0};
    int nt = 0, nd = 0, nv = 0;
    for (int l = 0; l < 32; l++) {
        if (!(missing >> l & 1)) continue;
        int t = io[l].type <= GFX_TYPE_UINT ? io[l].type : GFX_TYPE_FLOAT;
        uint32_t comps = io[l].comps ? io[l].comps : 4;
        if (!scalar[t]) {
            scalar[t] = bound++;
            if (t == GFX_TYPE_FLOAT) types[nt++] = 3u << 16 | SPV_OP_TYPE_FLOAT, types[nt++] = scalar[t], types[nt++] = 32;
            else types[nt++] = 4u << 16 | SPV_OP_TYPE_INT, types[nt++] = scalar[t], types[nt++] = 32, types[nt++] = t == GFX_TYPE_SINT;
        }
        uint32_t type = scalar[t];
        if (comps > 1) {
            int k = 0;
            while (k < nvec && !(vec[k].comp == type && vec[k].count == comps)) k++;
            if (k == nvec) {
                if (nvec == 64) return NULL;
                vec[nvec].id = bound++, vec[nvec].comp = type, vec[nvec++].count = comps;
                types[nt++] = 4u << 16 | SPV_OP_TYPE_VECTOR, types[nt++] = vec[k].id, types[nt++] = type, types[nt++] = comps;
            }
            type = vec[k].id;
        }
        int k = 0;
        while (k < nptr && ptr[k].type != type) k++;
        if (k == nptr) {
            if (nptr == 64) return NULL;
            ptr[nptr].id = bound++, ptr[nptr++].type = type;
            types[nt++] = 4u << 16 | SPV_OP_TYPE_POINTER, types[nt++] = ptr[k].id, types[nt++] = SPV_SC_OUTPUT, types[nt++] = type;
        }
        uint32_t init = bound++, var = bound++;
        if (comps == 4) {
            if (!zero[t]) {
                zero[t] = bound++, one[t] = bound++;
                types[nt++] = 4u << 16 | SPV_OP_CONSTANT, types[nt++] = scalar[t], types[nt++] = zero[t], types[nt++] = 0;
                types[nt++] = 4u << 16 | SPV_OP_CONSTANT, types[nt++] = scalar[t], types[nt++] = one[t], types[nt++] = 0x3f800000;
            }
            types[nt++] = 7u << 16 | SPV_OP_CONSTANT_COMPOSITE, types[nt++] = type, types[nt++] = init;
            types[nt++] = zero[t], types[nt++] = zero[t], types[nt++] = zero[t], types[nt++] = one[t];
        } else {
            types[nt++] = 3u << 16 | SPV_OP_CONSTANT_NULL, types[nt++] = type, types[nt++] = init;
        }
        types[nt++] = 5u << 16 | SPV_OP_VARIABLE, types[nt++] = ptr[k].id, types[nt++] = var, types[nt++] = SPV_SC_OUTPUT,
        types[nt++] = init;
        decs[nd++] = 4u << 16 | SPV_OP_DECORATE, decs[nd++] = var, decs[nd++] = SPV_DEC_LOCATION, decs[nd++] = (uint32_t)l;
        vars[nv++] = var;
    }
    uint64_t total = n + (uint64_t)(nt + nd + nv);
    uint32_t *o = malloc(total * 4), *p = o;
    if (!o) return NULL;
    uint32_t ep_len = w[ep_at] >> 16;
    memcpy(p, w, 5 * 4), p[3] = bound, p += 5;
    memcpy(p, w + 5, (ep_at - 5) * 4), p += ep_at - 5;
    *p++ = (ep_len + (uint32_t)nv) << 16 | SPV_OP_ENTRY_POINT;
    memcpy(p, w + ep_at + 1, (ep_len - 1) * 4), p += ep_len - 1;
    memcpy(p, vars, (size_t)nv * 4), p += nv;
    memcpy(p, w + ep_at + ep_len, (ann_end - ep_at - ep_len) * 4), p += ann_end - ep_at - ep_len;
    memcpy(p, decs, (size_t)nd * 4), p += nd;
    memcpy(p, w + ann_end, (fn_at - ann_end) * 4), p += fn_at - ann_end;
    memcpy(p, types, (size_t)nt * 4), p += nt;
    memcpy(p, w + fn_at, (n - fn_at) * 4), p += n - fn_at;
    *out_n = (uint64_t)(p - o);
    return o;
}

static uint32_t spirv_mesh_outputs(const uint32_t *w, uint64_t n, int *points) {
    enum { OP_EXECUTION_MODE = 16, MODE_OUTPUT_POINTS = 27 };
    uint32_t bound = n >= 5 && w[3] < (1u << 22) ? w[3] : 0, mask = 0;
    uint8_t *out = bound ? calloc(bound, 1) : NULL;
    int8_t *loc = bound ? malloc(bound) : NULL;
    *points = 0;
    if (!out || !loc) return free(out), free(loc), 0;
    memset(loc, -1, bound);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        if (op == OP_EXECUTION_MODE && len >= 3 && a[1] == MODE_OUTPUT_POINTS) *points = 1;
        else if (op == SPV_OP_DECORATE && len >= 4 && a[1] == SPV_DEC_LOCATION && a[0] < bound && a[2] < 32) loc[a[0]] = (int8_t)a[2];
        else if (op == SPV_OP_VARIABLE && len >= 4 && a[2] == SPV_SC_OUTPUT && a[1] < bound) out[a[1]] = 1;
        else if (op == SPV_OP_FUNCTION) break;
        i += len;
    }
    for (uint32_t id = 0; id < bound; id++)
        if (out[id] && loc[id] >= 0) mask |= 1u << loc[id];
    free(out), free(loc);
    return mask;
}

static uint32_t *spirv_fs_fill_inputs(const uint32_t *w, uint64_t n, uint32_t missing, int points, uint64_t *out_n) {
    enum { SC_INPUT = 1, SC_PRIVATE = 6, OP_EXECUTION_MODE = 16, OP_TYPE_POINTER = 32, OP_LABEL = 248, OP_LOAD = 61,
           OP_STORE = 62, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_COMPOSITE_CONSTRUCT = 80,
           OP_COMPOSITE_EXTRACT = 81, OP_DECORATE_ID = 332, BUILTIN_POINT_COORD = 16, MAX_FILL = 32, MAX_PRIV = 64 };
    if (!missing || n < 5 || w[3] >= (1u << 22)) return NULL;
    uint32_t old_bound = w[3], bound = old_bound;
    uint32_t *pt_type = calloc(old_bound, 4), *vec_comp = calloc(old_bound, 4), *vec_n = calloc(old_bound, 4);
    int8_t *loc = malloc(old_bound);
    uint8_t *fill = calloc(old_bound, 1);
    uint32_t *o = NULL;
    if (!pt_type || !vec_comp || !vec_n || !loc || !fill) goto done;
    memset(loc, -1, old_bound);
    uint32_t f32 = 0, v2 = 0, entry = 0;
    uint64_t ep_at = 0, ann_end = 0, fn_at = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto done;
        const uint32_t *a = w + i + 1;
        if (op == SPV_OP_ENTRY_POINT && !ep_at) ep_at = i, entry = a[1];
        else if (op == SPV_OP_DECORATE || op == 72 || op == OP_DECORATE_ID) {
            ann_end = i + len;
            if (op == SPV_OP_DECORATE && len >= 4 && a[1] == SPV_DEC_LOCATION && a[0] < old_bound && a[2] < 32)
                loc[a[0]] = (int8_t)a[2];
        } else if (op == SPV_OP_TYPE_FLOAT && len >= 3 && a[1] == 32) f32 = a[0];
        else if (op == SPV_OP_TYPE_VECTOR && len >= 4 && a[0] < old_bound) {
            vec_comp[a[0]] = a[1], vec_n[a[0]] = a[2];
            if (a[1] == f32 && a[2] == 2) v2 = a[0];
        } else if (op == OP_TYPE_POINTER && len >= 4 && a[0] < old_bound) pt_type[a[0]] = a[2];
        else if (op == SPV_OP_FUNCTION) { fn_at = i; break; }
        i += len;
    }
    if (!ep_at || !ann_end || !fn_at || !f32 || !entry || ann_end > fn_at) goto done;
    uint32_t vars[MAX_FILL], vtype[MAX_FILL], nvars = 0, priv[MAX_PRIV][2], npriv = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        uint32_t ptr = 0;
        if (op == SPV_OP_VARIABLE && len >= 4 && a[2] == SC_INPUT && a[1] < old_bound && a[0] < old_bound && i < fn_at) {
            int l = loc[a[1]];
            uint32_t t = pt_type[a[0]];
            int floaty = t == f32 || (t < old_bound && vec_comp[t] == f32 && vec_n[t] >= 2 && vec_n[t] <= 4);
            if (l >= 0 && (missing >> l & 1) && floaty && nvars < MAX_FILL) {
                fill[a[1]] = 1, vars[nvars] = a[1], vtype[nvars++] = t, ptr = a[0];
            }
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 && a[2] < old_bound && fill[a[2]] &&
                   a[1] < old_bound && a[0] < old_bound) {
            fill[a[1]] = 2, ptr = a[0];
        }
        if (ptr) {
            uint32_t k = 0;
            while (k < npriv && priv[k][0] != ptr) k++;
            if (k == npriv) {
                if (npriv == MAX_PRIV) goto done;
                priv[npriv][0] = ptr, priv[npriv++][1] = bound++;
            }
        }
        i += len;
    }
    if (!nvars) goto done;
    uint32_t extra[MAX_PRIV * 4 + 32], ne = 0, c0 = bound++, c1 = bound++, pc_ptr = 0, pc_var = 0;
    uint8_t declared[MAX_PRIV] = {0};
    extra[ne++] = 4u << 16 | SPV_OP_CONSTANT, extra[ne++] = f32, extra[ne++] = c0, extra[ne++] = 0;
    extra[ne++] = 4u << 16 | SPV_OP_CONSTANT, extra[ne++] = f32, extra[ne++] = c1, extra[ne++] = 0x3f800000;
    if (points) {
        if (!v2) v2 = bound++, extra[ne++] = 4u << 16 | SPV_OP_TYPE_VECTOR, extra[ne++] = v2, extra[ne++] = f32, extra[ne++] = 2;
        pc_ptr = bound++, pc_var = bound++;
        extra[ne++] = 4u << 16 | OP_TYPE_POINTER, extra[ne++] = pc_ptr, extra[ne++] = SC_INPUT, extra[ne++] = v2;
        extra[ne++] = 4u << 16 | SPV_OP_VARIABLE, extra[ne++] = pc_ptr, extra[ne++] = pc_var, extra[ne++] = SC_INPUT;
    }
    o = malloc((n + ne + (uint64_t)npriv * 4 + 16 + (uint64_t)nvars * 12 + 32) * 4);
    if (!o) goto done;
    uint32_t *p = o;
    memcpy(p, w, 5 * 4), p += 5;
    int in_entry = 0, stored = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        if (i == fn_at) {
            for (uint32_t k = 0; k < npriv; k++)
                if (!declared[k]) *p++ = 4u << 16 | OP_TYPE_POINTER, *p++ = priv[k][1], *p++ = SC_PRIVATE, *p++ = pt_type[priv[k][0]];
            memcpy(p, extra, ne * 4), p += ne;
        }
        if (i == ep_at) {
            uint32_t *head = p, name_words = (uint32_t)(strnlen((const char *)(a + 2), (len - 3) * 4) / 4 + 1);
            for (uint32_t k = 0; k < len; k++) {
                uint32_t v = w[i + k];
                if (k >= 3 + name_words && v < old_bound && fill[v] == 1 && w[1] < 0x00010400) continue;
                *p++ = v;
            }
            if (pc_var) *p++ = pc_var;
            *head = (uint32_t)(p - head) << 16 | SPV_OP_ENTRY_POINT;
            i += len;
            continue;
        }
        if ((op == SPV_OP_DECORATE || op == OP_DECORATE_ID) && len >= 3 && a[0] < old_bound && fill[a[0]] == 1) {
            i += len;
            if (i == ann_end && pc_var)
                *p++ = 4u << 16 | SPV_OP_DECORATE, *p++ = pc_var, *p++ = SPV_DEC_BUILTIN, *p++ = BUILTIN_POINT_COORD;
            continue;
        }
        if (op == SPV_OP_VARIABLE && len >= 4 && a[1] < old_bound && fill[a[1]] == 1)
            for (uint32_t k = 0; k < npriv; k++)
                if (priv[k][0] == a[0] && !declared[k])
                    declared[k] = 1, *p++ = 4u << 16 | OP_TYPE_POINTER, *p++ = priv[k][1], *p++ = SC_PRIVATE, *p++ = pt_type[priv[k][0]];
        memcpy(p, w + i, len * 4);
        if ((op == SPV_OP_VARIABLE || op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 &&
            a[1] < old_bound && fill[a[1]]) {
            for (uint32_t k = 0; k < npriv; k++)
                if (priv[k][0] == a[0]) p[1] = priv[k][1];
            if (op == SPV_OP_VARIABLE) p[3] = SC_PRIVATE;
        }
        p += len;
        if (i + len == ann_end && pc_var)
            *p++ = 4u << 16 | SPV_OP_DECORATE, *p++ = pc_var, *p++ = SPV_DEC_BUILTIN, *p++ = BUILTIN_POINT_COORD;
        if (op == SPV_OP_FUNCTION && len >= 5) in_entry = a[1] == entry;
        i += len;
        if (op == OP_LABEL && in_entry && !stored) {
            while (i < n && (w[i] & 0xffff) == SPV_OP_VARIABLE) memcpy(p, w + i, (w[i] >> 16) * 4), p += w[i] >> 16, i += w[i] >> 16;
            uint32_t sx = c0, sy = c0;
            if (pc_var) {
                uint32_t pc = bound++;
                sx = bound++, sy = bound++;
                *p++ = 4u << 16 | OP_LOAD, *p++ = v2, *p++ = pc, *p++ = pc_var;
                *p++ = 5u << 16 | OP_COMPOSITE_EXTRACT, *p++ = f32, *p++ = sx, *p++ = pc, *p++ = 0;
                *p++ = 5u << 16 | OP_COMPOSITE_EXTRACT, *p++ = f32, *p++ = sy, *p++ = pc, *p++ = 1;
            }
            for (uint32_t k = 0; k < nvars; k++) {
                uint32_t t = vtype[k], c = t == f32 ? 1 : vec_n[t], comp[4] = {sx, sy, c0, c1}, val = comp[0];
                if (c > 1) {
                    val = bound++;
                    *p++ = (3 + c) << 16 | OP_COMPOSITE_CONSTRUCT, *p++ = t, *p++ = val;
                    for (uint32_t q = 0; q < c; q++) *p++ = comp[q];
                }
                *p++ = 3u << 16 | OP_STORE, *p++ = vars[k], *p++ = val;
            }
            stored = 1;
        }
    }
    if (!stored) { free(o), o = NULL; goto done; }
    o[3] = bound;
    *out_n = (uint64_t)(p - o);
done:
    free(pt_type), free(vec_comp), free(vec_n), free(loc), free(fill);
    return o;
}

static uint32_t *spirv_fixup(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    enum { OP_EXTENSION = 10, OP_CAPABILITY = 17, OP_MEMBER_NAME = 6, OP_EXECUTION_MODE = 16, OP_TYPE_ARRAY = 28,
           OP_TYPE_STRUCT = 30, OP_CONSTANT = 43, OP_MEMBER_DECORATE = 72, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
           OP_BEGIN_INTERLOCK = 5364, OP_END_INTERLOCK = 5365, CAP_SAMPLE_INTERLOCK = 5363, CAP_PIXEL_INTERLOCK = 5378,
           CAP_RATE_INTERLOCK = 5372, BUILTIN_CLIP = 3, BUILTIN_CULL = 4 };
    if (n < 5) return NULL;
    uint32_t bound = w[3] < (1u << 22) ? w[3] : (1u << 22);
    uint32_t *val = calloc(bound, sizeof *val), *elem = calloc(bound, sizeof *elem), *len = calloc(bound, sizeof *len),
             *pointee = calloc(bound, sizeof *pointee), *var_type = calloc(bound, sizeof *var_type);
    uint32_t *o = NULL;
    int interlock_used = 0, interlock_declared = 0, cull_ok = 0;
    uint32_t cull_struct = 0, cull_member = 0, clip_member = ~0u, members = 0, member_types[8] = {0};
    if (!val || !elem || !len || !pointee || !var_type) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) goto out;
        const uint32_t *a = w + i + 1;
        if (op == OP_BEGIN_INTERLOCK || op == OP_END_INTERLOCK) interlock_used = 1;
        else if (op == OP_EXECUTION_MODE && l >= 3 && a[1] >= 5366 && a[1] <= 5371) interlock_used = 1;
        else if (op == OP_CAPABILITY && l >= 2 && (a[0] == CAP_PIXEL_INTERLOCK || a[0] == CAP_SAMPLE_INTERLOCK || a[0] == CAP_RATE_INTERLOCK))
            interlock_declared = 1;
        else if (op == OP_CONSTANT && l == 4 && a[1] < bound) val[a[1]] = a[2];
        else if (op == OP_TYPE_ARRAY && l >= 4 && a[0] < bound) elem[a[0]] = a[1], len[a[0]] = a[2] < bound ? val[a[2]] : 0;
        else if (op == SPV_OP_TYPE_POINTER && l >= 4 && a[0] < bound) pointee[a[0]] = a[2];
        else if (op == SPV_OP_VARIABLE && l >= 4 && a[1] < bound) var_type[a[1]] = a[0] < bound ? pointee[a[0]] : 0;
        else if (op == OP_MEMBER_DECORATE && l >= 5 && a[2] == SPV_DEC_BUILTIN && a[3] == BUILTIN_CULL) cull_struct = a[0], cull_member = a[1];
        else if (op == OP_MEMBER_DECORATE && l >= 5 && a[2] == SPV_DEC_BUILTIN && a[3] == BUILTIN_CLIP) clip_member = a[1];
        else if (op == OP_TYPE_STRUCT && cull_struct && a[0] == cull_struct && l >= 2) {
            members = l - 2;
            for (uint32_t m = 0; m < members && m < 8; m++) member_types[m] = a[1 + m];
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && l >= 5 && cull_struct && a[2] < bound) {
            uint32_t t = var_type[a[2]], k = 3;
            if (t < bound && t && elem[t] == cull_struct) k = 4;
            if ((t == cull_struct || k == 4) && k + 1 < l && a[k] < bound && val[a[k]] == cull_member) cull_struct = 0;
        }
        i += l;
    }
    VkCtx *c = vk_ctx();
    uint32_t limit = c ? c->props.limits.maxCombinedClipAndCullDistances : 8;
    if (cull_struct && members && members <= 8 && cull_member == members - 1 && clip_member < members) {
        uint32_t clip_t = member_types[clip_member], cull_t = member_types[cull_member];
        uint32_t clip_n = clip_t < bound ? len[clip_t] : 0, cull_n = cull_t < bound ? len[cull_t] : 0;
        cull_ok = clip_n + cull_n > limit && clip_n <= limit;
    }
    int strip = interlock_declared && !interlock_used;
    if (!strip && !cull_ok) goto out;
    o = malloc(n * 4);
    if (!o) goto out;
    uint64_t on = 5;
    memcpy(o, w, 5 * 4);
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        int drop = 0;
        if (strip && op == OP_CAPABILITY && (a[0] == CAP_PIXEL_INTERLOCK || a[0] == CAP_SAMPLE_INTERLOCK || a[0] == CAP_RATE_INTERLOCK)) drop = 1;
        if (strip && op == OP_EXTENSION && l >= 2 && !strncmp((const char *)a, "SPV_EXT_fragment_shader_interlock", (l - 1) * 4)) drop = 1;
        if (cull_ok && (op == OP_MEMBER_DECORATE || op == OP_MEMBER_NAME) && l >= 3 && a[0] == cull_struct && a[1] == cull_member) drop = 1;
        if (!drop) {
            memcpy(o + on, w + i, l * 4);
            if (cull_ok && op == OP_TYPE_STRUCT && a[0] == cull_struct) o[on] = (l - 1) << 16 | op;
            on += op == OP_TYPE_STRUCT && cull_ok && a[0] == cull_struct ? l - 1 : l;
        }
        i += l;
    }
    *out_n = on;
out:
    free(val), free(elem), free(len), free(pointee), free(var_type);
    return o;
}

static int uses_subgroup_ops(const uint32_t *w, uint64_t words) {
    for (uint64_t i = 5; i < words;) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > words) return 0;
        if (op != 17) return 0;
        uint32_t cap = w[i + 1];
        if ((cap >= 61 && cap <= 68) || cap == 4423 || cap == 4431) return 1;
        i += n;
    }
    return 0;
}

static uint32_t *spirv_reconverge(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    enum { OP_EXTENSION = 10, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, MODE_MAXIMALLY_RECONVERGES = 6023 };
    static const char ext[] = "SPV_KHR_maximal_reconvergence";
    const uint32_t ext_words = (uint32_t)(sizeof ext + 3) / 4;
    if (n < 5 || !uses_subgroup_ops(w, n)) return NULL;
    uint64_t caps_end = 0, eps_end = 0;
    uint32_t eps[16], neps = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) return NULL;
        if (op == OP_CAPABILITY) caps_end = i + l;
        else if (op == SPV_OP_ENTRY_POINT && l >= 3) {
            if (neps == 16) return NULL;
            eps[neps++] = w[i + 2], eps_end = i + l;
        } else if (op == OP_EXECUTION_MODE && l >= 3 && w[i + 2] == MODE_MAXIMALLY_RECONVERGES) return NULL;
        else if (op == SPV_OP_FUNCTION) break;
        i += l;
    }
    if (!caps_end || !neps || eps_end < caps_end) return NULL;
    uint64_t on = n + 1 + ext_words + 3ull * neps;
    uint32_t *o = malloc(on * 4);
    if (!o) return NULL;
    uint64_t p = 0;
    memcpy(o, w, caps_end * 4), p = caps_end;
    o[p++] = (1 + ext_words) << 16 | OP_EXTENSION;
    memset(o + p, 0, ext_words * 4);
    memcpy(o + p, ext, sizeof ext), p += ext_words;
    memcpy(o + p, w + caps_end, (eps_end - caps_end) * 4), p += eps_end - caps_end;
    for (uint32_t k = 0; k < neps; k++) o[p++] = 3u << 16 | OP_EXECUTION_MODE, o[p++] = eps[k], o[p++] = MODE_MAXIMALLY_RECONVERGES;
    memcpy(o + p, w + eps_end, (n - eps_end) * 4), p += n - eps_end;
    *out_n = p;
    return o;
}

static uint32_t *spirv_mesh_invariant(const uint32_t *w, uint64_t n, uint64_t *out_n) {
    enum { OP_MEMBER_DECORATE = 72, DEC_INVARIANT = 18, BUILTIN_POSITION = 0, MODEL_MESH = 5365 };
    if (n < 5) return NULL;
    int mesh = 0;
    uint64_t at = 0;
    uint32_t target = 0, member = ~0u;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) return NULL;
        if (op == SPV_OP_ENTRY_POINT && l >= 2 && w[i + 1] == MODEL_MESH) mesh = 1;
        else if (op == SPV_OP_DECORATE && l >= 4 && w[i + 2] == SPV_DEC_BUILTIN && w[i + 3] == BUILTIN_POSITION && !at)
            at = i + l, target = w[i + 1];
        else if (op == OP_MEMBER_DECORATE && l >= 5 && w[i + 3] == SPV_DEC_BUILTIN && w[i + 4] == BUILTIN_POSITION && !at)
            at = i + l, target = w[i + 1], member = w[i + 2];
        else if (op == SPV_OP_FUNCTION) break;
        i += l;
    }
    if (!mesh || !at) return NULL;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (op == SPV_OP_FUNCTION) break;
        if (member == ~0u && op == SPV_OP_DECORATE && l >= 3 && w[i + 1] == target && w[i + 2] == DEC_INVARIANT) return NULL;
        if (member != ~0u && op == OP_MEMBER_DECORATE && l >= 4 && w[i + 1] == target && w[i + 2] == member &&
            w[i + 3] == DEC_INVARIANT)
            return NULL;
        i += l;
    }
    uint32_t add = member == ~0u ? 3 : 4;
    uint32_t *o = malloc((n + add) * 4);
    if (!o) return NULL;
    memcpy(o, w, at * 4);
    uint64_t p = at;
    if (member == ~0u) o[p++] = 3u << 16 | SPV_OP_DECORATE, o[p++] = target, o[p++] = DEC_INVARIANT;
    else o[p++] = 4u << 16 | OP_MEMBER_DECORATE, o[p++] = target, o[p++] = member, o[p++] = DEC_INVARIANT;
    memcpy(o + p, w + at, (n - at) * 4), p += n - at;
    *out_n = p;
    return o;
}

static uint32_t *spirv_nan_preserve(const uint32_t *w, uint64_t n, uint32_t widths, uint64_t *out_n) {
    enum { OP_EXTENSION = 10, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, CAP_FLOAT16 = 9, CAP_FLOAT64 = 10,
           CAP_PRESERVE = 4466, MODE_PRESERVE = 4461 };
    static const char ext[] = "SPV_KHR_float_controls";
    const uint32_t ext_words = (uint32_t)(sizeof ext + 3) / 4;
    if (n < 5 || !(widths & NP_32)) return NULL;
    uint64_t caps_end = 0, eps_end = 0;
    uint32_t eps[16], neps = 0, use = NP_32;
    int has_cap = 0, has_ext = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) return NULL;
        if (op == OP_CAPABILITY && l >= 2) {
            caps_end = i + l;
            if (w[i + 1] == CAP_FLOAT16) use |= NP_16;
            else if (w[i + 1] == CAP_FLOAT64) use |= NP_64;
            else if (w[i + 1] == CAP_PRESERVE) has_cap = 1;
        } else if (op == OP_EXTENSION && (l - 1) * 4 >= sizeof ext && !memcmp(w + i + 1, ext, sizeof ext)) {
            has_ext = 1;
        } else if (op == SPV_OP_ENTRY_POINT && l >= 3) {
            if (neps == 16) return NULL;
            eps[neps++] = w[i + 2], eps_end = i + l;
        } else if (op == OP_EXECUTION_MODE && l >= 3 && w[i + 2] == MODE_PRESERVE) {
            return NULL;
        } else if (op == SPV_OP_FUNCTION) {
            break;
        }
        i += l;
    }
    if (!caps_end || !neps || eps_end < caps_end) return NULL;
    use &= widths;
    uint32_t nw = 0, width[3];
    if (use & NP_16) width[nw++] = 16;
    width[nw++] = 32;
    if (use & NP_64) width[nw++] = 64;
    uint64_t on = n + (has_cap ? 0 : 2) + (has_ext ? 0 : 1 + ext_words) + 4ull * nw * neps;
    uint32_t *o = malloc(on * 4);
    if (!o) return NULL;
    uint64_t p = 0;
    memcpy(o, w, caps_end * 4), p = caps_end;
    if (!has_cap) o[p++] = 2u << 16 | OP_CAPABILITY, o[p++] = CAP_PRESERVE;
    if (!has_ext) {
        o[p++] = (1 + ext_words) << 16 | OP_EXTENSION;
        memset(o + p, 0, ext_words * 4);
        memcpy(o + p, ext, sizeof ext), p += ext_words;
    }
    memcpy(o + p, w + caps_end, (eps_end - caps_end) * 4), p += eps_end - caps_end;
    for (uint32_t k = 0; k < neps; k++)
        for (uint32_t j = 0; j < nw; j++)
            o[p++] = 4u << 16 | OP_EXECUTION_MODE, o[p++] = eps[k], o[p++] = MODE_PRESERVE, o[p++] = width[j];
    memcpy(o + p, w + eps_end, (n - eps_end) * 4), p += n - eps_end;
    *out_n = p;
    return o;
}

static uint32_t *spirv_nan_bits(const uint32_t *w, uint64_t n, uint64_t *out_n, uint32_t *tests, uint32_t *selects) {
    enum { OP_UNDEF = 1, OP_TYPE_BOOL = 20, OP_SPEC_CONSTANT = 50, OP_SPEC_CONSTANT_COMPOSITE = 51, OP_STORE = 62,
           OP_BITCAST = 124, OP_IS_NAN = 156, OP_SELECT = 169, OP_UGREATER = 172, OP_BITWISE_AND = 199 };
    *tests = *selects = 0;
    if (n < 5 || w[3] >= (1u << 22)) return NULL;
    uint32_t bound = w[3];
    uint32_t *rtype = calloc(bound, sizeof *rtype), *vec_comp = calloc(bound, sizeof *vec_comp);
    uint8_t *f32 = calloc(bound, 1), *vec_n = calloc(bound, 1), *nan_const = calloc(bound, 1);
    uint32_t *o = NULL;
    if (!rtype || !vec_comp || !f32 || !vec_n || !nan_const) goto out;
    uint32_t tu = 0, uvec[5] = {0};
    uint64_t fn_at = 0;
    int vec_used[5] = {0};
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) goto out;
        const uint32_t *a = w + i + 1;
        if (!fn_at) {
            if (op == SPV_OP_FUNCTION) fn_at = i;
            else if (op == SPV_OP_TYPE_FLOAT && l >= 3 && a[0] < bound && a[1] == 32) f32[a[0]] = 1;
            else if (op == SPV_OP_TYPE_INT && l >= 4 && a[1] == 32 && a[2] == 0 && !tu) tu = a[0];
            else if (op == SPV_OP_TYPE_VECTOR && l >= 4 && a[0] < bound && a[2] >= 2 && a[2] <= 4) {
                vec_comp[a[0]] = a[1], vec_n[a[0]] = (uint8_t)a[2];
                if (tu && a[1] == tu && !uvec[a[2]]) uvec[a[2]] = a[0];
            } else if ((op == SPV_OP_CONSTANT || op == SPV_OP_CONSTANT_COMPOSITE || op == SPV_OP_CONSTANT_NULL ||
                        op == OP_UNDEF || op == SPV_OP_VARIABLE || op == OP_SPEC_CONSTANT ||
                        op == OP_SPEC_CONSTANT_COMPOSITE) && l >= 3 && a[1] < bound) {
                rtype[a[1]] = a[0];
                if (op == SPV_OP_CONSTANT && l == 4 && a[0] < bound && f32[a[0]] && (a[2] & 0x7fffffffu) > 0x7f800000u)
                    nan_const[a[1]] = 1;
            }
        }
        if (fn_at) {
            int no_result = op == OP_STORE || op == 63 || op == 64 || op == 8 || op == 317 || (op >= 246 && op <= 255) ||
                            op == 99 || op == 224 || op == 225 || op == 228 || op == 56 || op == 218 || op == 219 ||
                            op == 5294 || op == 5295 || op == 4416 || op == 5380 || op == 5364 || op == 5365;
            if (!no_result && op != SPV_OP_FUNCTION && l >= 3 && a[1] < bound && !rtype[a[1]]) rtype[a[1]] = a[0];
            if (op == OP_IS_NAN && l == 4 && a[2] < bound) {
                uint32_t t = rtype[a[2]];
                if (t < bound && f32[t]) ++*tests;
                else if (t < bound && vec_n[t] && vec_comp[t] < bound && f32[vec_comp[t]]) ++*tests, vec_used[vec_n[t]] = 1;
            } else if (op == OP_SELECT && l == 6 && a[0] < bound && f32[a[0]] && a[3] < bound && a[4] < bound &&
                       (nan_const[a[3]] || nan_const[a[4]])) {
                ++*selects;
            }
        }
        i += l;
    }
    if (!fn_at || (!*tests && !*selects)) goto out;
    uint32_t next = bound, decl[64], nd = 0;
    if (!tu) tu = next++, decl[nd++] = 4u << 16 | SPV_OP_TYPE_INT, decl[nd++] = tu, decl[nd++] = 32, decl[nd++] = 0;
    uint32_t c_mag = next++, c_inf = next++, cv_mag[5] = {0}, cv_inf[5] = {0};
    for (int k = 2; k <= 4; k++)
        if (vec_used[k] && !uvec[k])
            uvec[k] = next++, decl[nd++] = 4u << 16 | SPV_OP_TYPE_VECTOR, decl[nd++] = uvec[k], decl[nd++] = tu, decl[nd++] = (uint32_t)k;
    decl[nd++] = 4u << 16 | SPV_OP_CONSTANT, decl[nd++] = tu, decl[nd++] = c_mag, decl[nd++] = 0x7fffffffu;
    decl[nd++] = 4u << 16 | SPV_OP_CONSTANT, decl[nd++] = tu, decl[nd++] = c_inf, decl[nd++] = 0x7f800000u;
    for (int k = 2; k <= 4; k++) {
        if (!vec_used[k]) continue;
        cv_mag[k] = next++, cv_inf[k] = next++;
        decl[nd++] = (uint32_t)(3 + k) << 16 | SPV_OP_CONSTANT_COMPOSITE, decl[nd++] = uvec[k], decl[nd++] = cv_mag[k];
        for (int j = 0; j < k; j++) decl[nd++] = c_mag;
        decl[nd++] = (uint32_t)(3 + k) << 16 | SPV_OP_CONSTANT_COMPOSITE, decl[nd++] = uvec[k], decl[nd++] = cv_inf[k];
        for (int j = 0; j < k; j++) decl[nd++] = c_inf;
    }
    uint64_t cap = n + nd + 10ull * *tests + 12ull * *selects;
    o = malloc(cap * 4);
    if (!o) goto out;
    memcpy(o, w, fn_at * 4);
    uint64_t p = fn_at;
    memcpy(o + p, decl, nd * 4), p += nd;
    for (uint64_t i = fn_at; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        const uint32_t *a = w + i + 1;
        uint32_t t = op == OP_IS_NAN && l == 4 && a[2] < bound ? rtype[a[2]] : 0;
        int k = t && t < bound ? (f32[t] ? 1 : vec_n[t] && vec_comp[t] < bound && f32[vec_comp[t]] ? vec_n[t] : 0) : 0;
        if (k) {
            uint32_t ut = k == 1 ? tu : uvec[k], mag = k == 1 ? c_mag : cv_mag[k], inf = k == 1 ? c_inf : cv_inf[k];
            uint32_t bits = next++, low = next++;
            o[p++] = 4u << 16 | OP_BITCAST, o[p++] = ut, o[p++] = bits, o[p++] = a[2];
            o[p++] = 5u << 16 | OP_BITWISE_AND, o[p++] = ut, o[p++] = low, o[p++] = bits, o[p++] = mag;
            o[p++] = 5u << 16 | OP_UGREATER, o[p++] = a[0], o[p++] = a[1], o[p++] = low, o[p++] = inf;
        } else if (op == OP_SELECT && l == 6 && a[0] < bound && f32[a[0]] && a[3] < bound && a[4] < bound &&
                   (nan_const[a[3]] || nan_const[a[4]])) {
            uint32_t x = next++, y = next++, s = next++;
            o[p++] = 4u << 16 | OP_BITCAST, o[p++] = tu, o[p++] = x, o[p++] = a[3];
            o[p++] = 4u << 16 | OP_BITCAST, o[p++] = tu, o[p++] = y, o[p++] = a[4];
            o[p++] = 6u << 16 | OP_SELECT, o[p++] = tu, o[p++] = s, o[p++] = a[2], o[p++] = x, o[p++] = y;
            o[p++] = 4u << 16 | OP_BITCAST, o[p++] = a[0], o[p++] = a[1], o[p++] = s;
        } else {
            memcpy(o + p, w + i, l * 4), p += l;
        }
        i += l;
    }
    o[3] = next;
    *out_n = p;
out:
    free(rtype), free(vec_comp), free(f32), free(vec_n), free(nan_const);
    return o;
}

static int spirv_mark_list_test(const char *list);

int vk_pipeline_spirv_selftest(void) {
    enum { CAP = 2u << 16 | 17, MODE = 4u << 16 | 16 };
    static const uint32_t head[] = {0x07230203, 0x00010300, 0, 5, 0, CAP, 1, CAP, 10};
    static const uint32_t body[] = {
        3u << 16 | 14, 0, 1,
        5u << 16 | 15, 4, 1, 0x6e69616d, 0,
        3u << 16 | 16, 1, 7,
        2u << 16 | 19, 2,
        3u << 16 | 33, 3, 2,
        5u << 16 | 54, 2, 1, 0, 3,
        2u << 16 | 248, 4, 1u << 16 | 253, 1u << 16 | 56};
    enum { NH = sizeof head / 4, NB = sizeof body / 4, EP_END = 8 };
    uint32_t in[NH + NB], want[NH + NB + 32];
    memcpy(in, head, sizeof head), memcpy(in + NH, body, sizeof body);
    int failures = 0;
    for (int t = 0; t < 3; t++) {
        uint32_t widths = t == 0 ? NP_16 | NP_32 | NP_64 : NP_32, n = NH + NB, p = 0;
        int f64 = t != 2, w64 = t == 0;
        if (!f64) in[NH - 1] = 0;
        memcpy(want, in, NH * 4), p = NH;
        want[p++] = CAP, want[p++] = 4466;
        want[p++] = 7u << 16 | 10;
        memset(want + p, 0, 24), memcpy(want + p, "SPV_KHR_float_controls", 23), p += 6;
        memcpy(want + p, body, EP_END * 4), p += EP_END;
        want[p++] = MODE, want[p++] = 1, want[p++] = 4461, want[p++] = 32;
        if (w64) want[p++] = MODE, want[p++] = 1, want[p++] = 4461, want[p++] = 64;
        memcpy(want + p, body + EP_END, (NB - EP_END) * 4), p += NB - EP_END;
        uint64_t on = 0, again_n = 0;
        uint32_t *o = spirv_nan_preserve(in, n, widths, &on);
        uint32_t *again = o ? spirv_nan_preserve(o, on, widths, &again_n) : NULL;
        if (!o || on != p || memcmp(o, want, p * 4) || again) {
            printf("spirv: SignedZeroInfNanPreserve rewrite, case %d: wrong output\n", t);
            failures++;
        }
        free(o), free(again);
    }
    if (spirv_nan_preserve(in, NH + NB, NP_16 | NP_64, &(uint64_t){0})) {
        printf("spirv: SignedZeroInfNanPreserve rewrite: a module changed without the 32-bit width\n");
        failures++;
    }
    printf("spirv: SignedZeroInfNanPreserve rewrite: %s\n", failures ? "FAIL" : "ok");
    {
        static const uint32_t m[] = {
            0x07230203, 0x00010300, 0, 20, 0,
            2u << 16 | 17, 1,
            3u << 16 | 14, 0, 1,
            5u << 16 | 15, 5, 1, 0x6e69616d, 0,
            2u << 16 | 19, 2,
            3u << 16 | 33, 3, 2,
            3u << 16 | 22, 4, 32,
            2u << 16 | 20, 5,
            4u << 16 | 23, 6, 4, 2,
            4u << 16 | 23, 7, 5, 2,
            4u << 16 | 43, 4, 8, 0xffffffffu,
            4u << 16 | 43, 4, 9, 0x3f800000u,
            5u << 16 | 44, 6, 10, 8, 9,
            3u << 16 | 41, 5, 11,
            5u << 16 | 54, 2, 1, 0, 3,
            2u << 16 | 248, 12,
            4u << 16 | 156, 5, 13, 9,
            4u << 16 | 156, 7, 14, 10,
            6u << 16 | 169, 4, 15, 11, 9, 8,
            1u << 16 | 253, 1u << 16 | 56};
        uint64_t on = 0;
        uint32_t tests = 0, sels = 0, isnan_left = 0, ugt = 0, int32 = 0, bitcasts = 0;
        uint32_t *o = spirv_nan_bits(m, sizeof m / 4, &on, &tests, &sels);
        for (uint64_t i = 5; o && i < on;) {
            uint32_t op = o[i] & 0xffff, l = o[i] >> 16;
            if (!l) break;
            isnan_left += op == 156, ugt += op == 172, int32 += op == 21 && o[i + 2] == 32, bitcasts += op == 124;
            i += l;
        }
        int bad = !o || tests != 2 || sels != 1 || isnan_left || ugt != 2 || int32 != 1 || bitcasts != 5 || o[3] <= 20;
        printf("spirv: integer NaN tests rewrite: %s\n", bad ? "FAIL" : "ok");
        failures += bad;
        free(o);
    }
    const char *list = cfg_get("NX2SARIA_SPV_MARK_TEST");
    if (list && *list) failures += spirv_mark_list_test(list);
    return failures;
}

static void pin_subgroup32(VkPipelineShaderStageCreateInfo *st, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *rq,
                           const uint32_t *w, uint64_t words) {
    if (!(P.sg32_stages & st->stage) || !w || !uses_subgroup_ops(w, words)) return;
    *rq = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = vk_caps.sg_size};
    st->pNext = rq;
}

static uint32_t local_size_x(const uint32_t *w, uint64_t words) {
    for (uint64_t i = 5; w && i < words;) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (!n || i + n > words) return 0;
        if (op == 16 && n >= 6 && w[i + 2] == 17) return w[i + 3];
        if (op == 54) return 0;
        i += n;
    }
    return 0;
}

typedef struct { uint8_t pin, full; } StageSg;
static StageSg stage_sg(VkShaderStageFlagBits stage, const uint32_t *w, uint64_t words) {
    StageSg g = {0, 0};
    if (!P.mesh_sg || !(P.sg32_stages & stage) || !w || !uses_subgroup_ops(w, words)) return g;
    uint32_t lx = local_size_x(w, words);
    g.pin = stage == VK_SHADER_STAGE_TASK_BIT_EXT;
    g.full = P.f_full_subgroups && vk_caps.sg_size && lx && lx % vk_caps.sg_size == 0;
    return g;
}

static void stage_sg_apply(VkPipelineShaderStageCreateInfo *st, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *rq,
                           StageSg g) {
    if (g.pin) {
        *rq = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
            .requiredSubgroupSize = vk_caps.sg_size};
        st->pNext = rq;
        __atomic_add_fetch(&P.sg_task_pinned, 1, __ATOMIC_RELAXED);
    }
    if (g.full && st->pNext) {
        st->flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        __atomic_add_fetch(st->stage == VK_SHADER_STAGE_TASK_BIT_EXT ? &P.sg_task_full : &P.sg_mesh_full, 1,
                           __ATOMIC_RELAXED);
    }
    if (!g.pin && !g.full) return;
    uint32_t n = __atomic_load_n(&P.sg_task_pinned, __ATOMIC_RELAXED) + __atomic_load_n(&P.sg_mesh_full, __ATOMIC_RELAXED);
    if (n == 1 || n == 10 || n == 100 || n == 1000)
        rt_log("vk_pipeline: task and mesh subgroups so far: %u task stages pinned to 32 (%u with full subgroups), %u mesh "
               "stages with full subgroups (NX2SARIA_VK_MESH_SUBGROUPS)", __atomic_load_n(&P.sg_task_pinned, __ATOMIC_RELAXED),
               __atomic_load_n(&P.sg_task_full, __ATOMIC_RELAXED), __atomic_load_n(&P.sg_mesh_full, __ATOMIC_RELAXED));
}

static uint32_t *spirv_pool_rebind(const uint32_t *w, uint64_t n) {
    enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_ARRAY_ = 28 };
    uint32_t bound = n >= 5 && w[3] < (1u << 22) ? w[3] : 0;
    if (!bound) return NULL;
    int32_t *kind = malloc(bound * sizeof *kind);
    uint32_t *cval = calloc(bound, sizeof *cval);
    if (!kind || !cval) return free(kind), free(cval), NULL;
    memset(kind, 0xff, bound * sizeof *kind);
    int any = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n || op == SPV_OP_FUNCTION) break;
        if (op == OP_TYPE_IMAGE && len >= 9 && w[i + 1] < bound && w[i + 7] == 1) {
            uint32_t dim = w[i + 3], depth = w[i + 4], arrayed = w[i + 5], ms = w[i + 6];
            if (dim <= 3 && !ms) kind[w[i + 1]] = (int32_t)((depth == 1 ? 8 : 0) + dim * 2 + (arrayed ? 1 : 0));
        } else if (op == OP_TYPE_SAMPLER && len >= 2 && w[i + 1] < bound) {
            kind[w[i + 1]] = 100;
        } else if (op == SPV_OP_CONSTANT && len >= 4 && w[i + 2] < bound) {
            cval[w[i + 2]] = w[i + 3];
        } else if (op == OP_TYPE_ARRAY_ && len >= 4 && w[i + 1] < bound && w[i + 2] < bound && w[i + 3] < bound) {
            int32_t k = kind[w[i + 2]];
            uint32_t want = k == 100 ? VK_POOL_SAMP : VK_POOL_TEX;
            int pool = k >= 0 && (cval[w[i + 3]] == want || (k != 100 && cval[w[i + 3]] == VK_POOL_TEX_CARRIED));
            kind[w[i + 1]] = pool ? k + 1000 : -1;
        } else if (op == SPV_OP_TYPE_POINTER && len >= 4 && w[i + 1] < bound && w[i + 3] < bound) {
            kind[w[i + 1]] = kind[w[i + 3]] >= 1000 ? kind[w[i + 3]] : -1;
        } else if (op == SPV_OP_VARIABLE && len >= 4 && w[i + 1] < bound && w[i + 2] < bound) {
            kind[w[i + 2]] = kind[w[i + 1]] >= 1000 ? kind[w[i + 1]] + 1000 : -1;
            if (kind[w[i + 2]] >= 2000) any = 1;
        }
        i += len;
    }
    uint32_t *o = NULL;
    if (any && (o = malloc(n * 4))) {
        memcpy(o, w, n * 4);
        uint32_t used = 0;
        for (uint64_t i = 5; i < n;) {
            uint32_t op = o[i] & 0xffff, len = o[i] >> 16;
            if (!len || i + len > n || op == SPV_OP_FUNCTION) break;
            if (op == SPV_OP_DECORATE && len >= 4 && o[i + 1] < bound && kind[o[i + 1]] >= 2000) {
                int32_t k = kind[o[i + 1]] - 2000;
                if (o[i + 2] == SPV_DEC_DESCRIPTOR_SET) o[i + 3] = 2;
                else if (o[i + 2] == SPV_DEC_BINDING) o[i + 3] = k == 100 ? VK_POOL_KINDS : (uint32_t)k;
                if (k != 100) used |= 1u << k;
            }
            i += len;
        }
        __atomic_fetch_or(&vk_pool_kinds_used, used, __ATOMIC_RELAXED);
    }
    free(kind), free(cval);
    return o;
}

volatile int32_t vk_pool_marks_missed;
static int marks_on = -1, marks_vs_ok, marks_fs_ok;

int vk_pool_marks_enabled(void) {
    if (marks_on < 0) {
        marks_on = rt_knob("TEX_DISUSE", "1") >= 1;
        VkCtx *c = vk_ctx();
        if (c) {
            VkPhysicalDeviceFeatures f;
            vkGetPhysicalDeviceFeatures(c->phys, &f);
            marks_vs_ok = f.vertexPipelineStoresAndAtomics, marks_fs_ok = f.fragmentStoresAndAtomics;
        }
    }
    return marks_on;
}

static uint32_t *spirv_pool_mark(const uint32_t *w, uint64_t n, int early_ok, uint64_t *out_n, int *status,
                                 const char **why) {
    enum { OP_TYPE_RUNTIME_ARRAY = 29, OP_TYPE_STRUCT_ = 30, OP_ACCESS_CHAIN_ = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
           OP_STORE_ = 62, OP_MEMBER_DECORATE_ = 72, OP_DECORATION_GROUP = 73, OP_GROUP_DECORATE = 74,
           OP_GROUP_MEMBER_DECORATE = 75, OP_DECORATE_ID = 332, OP_DECORATE_STRING = 5632, OP_MEMBER_DECORATE_STRING = 5633,
           OP_EXECUTION_MODE = 16, OP_EXECUTION_MODE_ID = 331, OP_IMAGE_WRITE = 99, OP_ATOMIC_FIRST = 227,
           OP_ATOMIC_LAST = 242, OP_KILL = 252, OP_TERMINATE_INVOCATION = 4416, OP_DEMOTE_TO_HELPER = 5380,
           EM_EARLY_FRAGMENT_TESTS = 9, BI_SAMPLE_MASK = 20, BI_FRAG_DEPTH = 22, BI_FRAG_STENCIL_REF = 5014,
           SC_STORAGE_BUFFER = 12, DEC_BLOCK = 2, DEC_ARRAY_STRIDE = 6, DEC_OFFSET_ = 35, OP_TYPE_BOOL = 20,
           OP_LOAD_ = 61, OP_BITCAST = 124, OP_SELECT_ = 169, OP_INOT_EQUAL = 171, OP_LOOP_MERGE = 246, OP_LABEL_ = 248,
           OP_IEQUAL_ = 170, OP_ISUB_ = 130, OP_UGREATER_THAN_ = 172,
           OP_RETURN = 253, OP_RETURN_VALUE = 254, SC_PRIVATE = 6, LOOP_DEPTH = 64, OP_TYPE_ARRAY_ = 28, OP_IADD = 128,
           OP_BITWISE_AND = 199, OP_ULESS_THAN = 176, OP_BRANCH = 249, OP_BRANCH_CONDITIONAL = 250, OP_PHI = 245 };
    *status = 0, *why = NULL;
    if (n < 5) return NULL;
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return NULL;
    uint32_t *bind = calloc(bound, sizeof *bind);
    uint8_t *set2 = calloc(bound, 1);
    if (!bind || !set2) return free(bind), free(set2), *status = -1, *why = "no memory", NULL;
    uint64_t ann_end = 0, fn_start = 0, em_end = 0;
    uint32_t uint_t = 0, c0 = 0, c1 = 0, ptr_u = 0, fs_entry = 0, int_t = 0, bool_t = 0;
    int vertex_stage = 0, fragment_stage = 0, entries = 0, early = early_ok;
    uint32_t *rtype = calloc(bound, sizeof *rtype);
    uint8_t *is_type = calloc(bound, 1);
    if (!rtype || !is_type)
        return free(bind), free(set2), free(rtype), free(is_type), *status = -1, *why = "no memory", NULL;
#define POOL_MARK_FREE() (free(bind), free(set2), free(rtype), free(is_type), free(site_def))
    uint8_t *site_def = NULL;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) return POOL_MARK_FREE(), *status = -1, *why = "an instruction past the module's end", NULL;
        if (op == SPV_OP_FUNCTION) { fn_start = i; break; }
        if (op == SPV_OP_ENTRY_POINT || op == OP_EXECUTION_MODE || op == OP_EXECUTION_MODE_ID) em_end = i + l;
        if (op == SPV_OP_ENTRY_POINT && l >= 3) {
            entries++;
            if (w[i + 1] <= 3) vertex_stage = 1;
            else if (w[i + 1] == 4) fragment_stage = 1, fs_entry = w[i + 2];
        } else if (op == OP_EXECUTION_MODE && l >= 3 && w[i + 2] == EM_EARLY_FRAGMENT_TESTS) {
            early = 0;
        } else if (op == SPV_OP_DECORATE && l >= 4 && w[i + 1] < bound) {
            if (w[i + 2] == SPV_DEC_DESCRIPTOR_SET && w[i + 3] == 2) set2[w[i + 1]] = 1;
            else if (w[i + 2] == SPV_DEC_BINDING) bind[w[i + 1]] = w[i + 3] + 1;
            else if (w[i + 2] == SPV_DEC_BUILTIN &&
                     (w[i + 3] == BI_SAMPLE_MASK || w[i + 3] == BI_FRAG_DEPTH || w[i + 3] == BI_FRAG_STENCIL_REF))
                early = 0;
        } else if (op == SPV_OP_VARIABLE && l >= 4 && w[i + 3] == SC_STORAGE_BUFFER) {
            early = 0;
        } else if (op == SPV_OP_TYPE_INT && l == 4 && w[i + 2] == 32 && w[i + 3] == 0) {
            uint_t = w[i + 1];
        } else if (op == SPV_OP_TYPE_INT && l == 4 && w[i + 2] == 32 && w[i + 3] == 1) {
            int_t = w[i + 1];
        } else if (op == OP_TYPE_BOOL && l == 2) {
            bool_t = w[i + 1];
        } else if (op == SPV_OP_CONSTANT && l == 4 && uint_t && w[i + 1] == uint_t) {
            if (w[i + 3] == 0 && !c0) c0 = w[i + 2];
            if (w[i + 3] == 1 && !c1) c1 = w[i + 2];
        } else if (op == SPV_OP_TYPE_POINTER && l == 4 && w[i + 2] == SC_STORAGE_BUFFER && uint_t && w[i + 3] == uint_t) {
            ptr_u = w[i + 1];
        }
        if (op == SPV_OP_DECORATE || op == OP_MEMBER_DECORATE_ || op == OP_DECORATION_GROUP || op == OP_GROUP_DECORATE ||
            op == OP_GROUP_MEMBER_DECORATE || op == OP_DECORATE_ID || op == OP_DECORATE_STRING || op == OP_MEMBER_DECORATE_STRING)
            ann_end = i + l;
        if (op >= 19 && op <= 39 && l >= 2 && w[i + 1] < bound) is_type[w[i + 1]] = 1;
        else if (op == SPV_OP_CONSTANT && l >= 4 && w[i + 2] < bound) rtype[w[i + 2]] = w[i + 1];
        i += l;
    }
    enum { RING = 8 };
    uint64_t sites = 0, deferred = 0, rings = 0, exits = 0;
    int marked = 0;
    for (uint32_t id = 0; id < bound; id++)
        if (set2[id] && bind[id] == VK_POOL_MARK_BINDING + 1) marked = 1;
    site_def = fn_start && !marked ? calloc((n - fn_start) / 5 + 1, 1) : NULL;
    if (fn_start && !marked && !site_def) return POOL_MARK_FREE(), *status = -1, *why = "no memory", NULL;
    uint32_t loops[LOOP_DEPTH], fn = 0;
    int depth = 0;
    for (uint64_t i = fn_start; fn_start && !marked && i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (!l || i + l > n) break;
        if (l >= 3 && w[i + 1] < bound && is_type[w[i + 1]] && w[i + 2] < bound) rtype[w[i + 2]] = w[i + 1];
        if (op == SPV_OP_FUNCTION && l >= 3) fn = w[i + 2], depth = 0;
        else if (op == OP_LOOP_MERGE && l >= 3) depth < LOOP_DEPTH ? (void)(loops[depth++] = w[i + 1]) : (void)depth++;
        else if (op == OP_LABEL_ && l >= 2 && depth > 0 && depth <= LOOP_DEPTH && loops[depth - 1] == w[i + 1]) depth--;
        if ((op == OP_ACCESS_CHAIN_ || op == OP_IN_BOUNDS_ACCESS_CHAIN) && l >= 5 && w[i + 3] < bound && set2[w[i + 3]] &&
            bind[w[i + 3]] && bind[w[i + 3]] - 1 < VK_POOL_KINDS) {
            uint32_t ix = w[i + 4], t = ix < bound ? rtype[ix] : 0;
            if (fragment_stage && entries == 1 && fs_entry && t && (t == uint_t || t == int_t)) {
                int ring = fn != fs_entry || depth;
                site_def[sites] = (uint8_t)(1 | (t == int_t ? 2 : 0) | (ring ? 4 : 0));
                ring ? rings++ : deferred++;
            }
            sites++;
        } else if (op == OP_KILL || op == OP_TERMINATE_INVOCATION || op == OP_DEMOTE_TO_HELPER || op == OP_IMAGE_WRITE ||
                   (op >= OP_ATOMIC_FIRST && op <= OP_ATOMIC_LAST))
            early = 0;
        if (op == OP_KILL || op == OP_TERMINATE_INVOCATION || op == OP_DEMOTE_TO_HELPER ||
            (fn == fs_entry && (op == OP_RETURN || op == OP_RETURN_VALUE)))
            exits++;
        i += l;
    }
    if (marked || !sites) return POOL_MARK_FREE(), NULL;
    static int shared = -1;
    if (shared < 0) shared = rt_knob("POOL_MARK_SHARED", "1") != 0;
    uint64_t ring_sites = rings;
    if (shared && rings > 1) rings = 1;
    uint64_t stores = deferred + rings;
    if (stores && (!exits || exits * (deferred + 3 * rings) > 8192))
        memset(site_def, 0, sites), deferred = rings = stores = 0;
    *why = w[1] < 0x00010300 || w[1] > 0x00010600 ? "a SPIR-V version before 1.3 or after 1.6"
         : !ann_end || !fn_start              ? "no decorations or no function"
         : vertex_stage && !marks_vs_ok       ? "the GPU allows no stores in vertex-pipeline shaders"
         : fragment_stage && !marks_fs_ok     ? "the GPU allows no stores in fragment shaders"
                                              : NULL;
    if (*why) return POOL_MARK_FREE(), *status = -1, NULL;
    early = early && fragment_stage && entries == 1 && em_end && em_end < ann_end;
    int iface = w[1] >= 0x00010400;
    uint64_t on = n + 3 + (uint64_t)entries * (1 + deferred + 2 * rings) + 20 + 30 + 9 * sites + 80 + deferred * 12 +
                  ring_sites * 64 + 8 + (deferred * 29 + rings * (RING * 29 + 25)) * exits;
    uint32_t *o = malloc(on * 4);
    if (!o) return POOL_MARK_FREE(), *status = -1, *why = "no memory", NULL;
    uint32_t next = bound;
    uint32_t new_uint = uint_t ? 0 : (uint_t = next++), new_c0 = c0 ? 0 : (c0 = next++), new_c1 = c1 ? 0 : (c1 = next++);
    uint32_t rta = next++, st = next++, ptr_st = next++, var = next++, new_ptr_u = ptr_u ? 0 : (ptr_u = next++);
    uint32_t new_bool = 0, ptr_pv = 0, c_none = 0, c_spare = 0, pv0 = 0, ring_t = 0, ptr_ring = 0, ring_none = 0,
             cr[RING] = {0}, c_ring = 0, c_over = 0, ra0 = 0, rc0 = 0;
    if (stores) {
        new_bool = bool_t ? 0 : (bool_t = next++);
        ptr_pv = next++, c_none = next++, c_spare = next++, pv0 = next, next += (uint32_t)deferred;
        if (rings) {
            ring_t = next++, ptr_ring = next++, ring_none = next++, c_ring = next++, c_over = next++;
            for (int j = 0; j < RING; j++) cr[j] = next++;
            ra0 = next, next += (uint32_t)rings, rc0 = next, next += (uint32_t)rings;
        }
    }
    uint64_t p = 5;
    memcpy(o, w, 5 * 4);
    for (uint64_t i = 5; i < ann_end;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (early && i == em_end) o[p++] = 3u << 16 | OP_EXECUTION_MODE, o[p++] = fs_entry, o[p++] = EM_EARLY_FRAGMENT_TESTS;
        memcpy(o + p, w + i, l * 4);
        if (iface && op == SPV_OP_ENTRY_POINT && l >= 3) {
            uint32_t nv = 0, *at = o + p + l + 1;
            o[p] = (uint32_t)(l + 1 + deferred + 2 * rings) << 16 | op, o[p + l] = var, p++;
            for (uint32_t k = 0; k < deferred; k++) at[nv++] = pv0 + k;
            for (uint32_t k = 0; k < rings; k++) at[nv++] = ra0 + k;
            for (uint32_t k = 0; k < rings; k++) at[nv++] = rc0 + k;
            p += nv;
        }
        p += l;
        i += l;
    }
    o[p++] = 4u << 16 | SPV_OP_DECORATE, o[p++] = rta, o[p++] = DEC_ARRAY_STRIDE, o[p++] = 4;
    o[p++] = 5u << 16 | OP_MEMBER_DECORATE_, o[p++] = st, o[p++] = 0, o[p++] = DEC_OFFSET_, o[p++] = 0;
    o[p++] = 3u << 16 | SPV_OP_DECORATE, o[p++] = st, o[p++] = DEC_BLOCK;
    o[p++] = 4u << 16 | SPV_OP_DECORATE, o[p++] = var, o[p++] = SPV_DEC_DESCRIPTOR_SET, o[p++] = 2;
    o[p++] = 4u << 16 | SPV_OP_DECORATE, o[p++] = var, o[p++] = SPV_DEC_BINDING, o[p++] = VK_POOL_MARK_BINDING;
    memcpy(o + p, w + ann_end, (fn_start - ann_end) * 4), p += fn_start - ann_end;
    if (new_uint) o[p++] = 4u << 16 | SPV_OP_TYPE_INT, o[p++] = uint_t, o[p++] = 32, o[p++] = 0;
    if (new_c0) o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c0, o[p++] = 0;
    if (new_c1) o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c1, o[p++] = 1;
    o[p++] = 3u << 16 | OP_TYPE_RUNTIME_ARRAY, o[p++] = rta, o[p++] = uint_t;
    o[p++] = 3u << 16 | OP_TYPE_STRUCT_, o[p++] = st, o[p++] = rta;
    o[p++] = 4u << 16 | SPV_OP_TYPE_POINTER, o[p++] = ptr_st, o[p++] = SC_STORAGE_BUFFER, o[p++] = st;
    if (new_ptr_u) o[p++] = 4u << 16 | SPV_OP_TYPE_POINTER, o[p++] = ptr_u, o[p++] = SC_STORAGE_BUFFER, o[p++] = uint_t;
    o[p++] = 4u << 16 | SPV_OP_VARIABLE, o[p++] = ptr_st, o[p++] = var, o[p++] = SC_STORAGE_BUFFER;
    if (stores) {
        if (new_bool) o[p++] = 2u << 16 | OP_TYPE_BOOL, o[p++] = bool_t;
        o[p++] = 4u << 16 | SPV_OP_TYPE_POINTER, o[p++] = ptr_pv, o[p++] = SC_PRIVATE, o[p++] = uint_t;
        o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c_none, o[p++] = 0xffffffffu;
        o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c_spare, o[p++] = VK_POOL_TEX;
        if (rings) {
            for (int j = 0; j < RING; j++) o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = cr[j], o[p++] = (uint32_t)j;
            o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c_ring, o[p++] = RING;
            o[p++] = 4u << 16 | SPV_OP_CONSTANT, o[p++] = uint_t, o[p++] = c_over, o[p++] = VK_POOL_MARK_OVERFLOW;
            o[p++] = 4u << 16 | OP_TYPE_ARRAY_, o[p++] = ring_t, o[p++] = uint_t, o[p++] = c_ring;
            o[p++] = 4u << 16 | SPV_OP_TYPE_POINTER, o[p++] = ptr_ring, o[p++] = SC_PRIVATE, o[p++] = ring_t;
            o[p++] = (3u + RING) << 16 | SPV_OP_CONSTANT_COMPOSITE, o[p++] = ring_t, o[p++] = ring_none;
            for (int j = 0; j < RING; j++) o[p++] = c_none;
        }
        for (uint32_t k = 0; k < deferred; k++)
            o[p++] = 5u << 16 | SPV_OP_VARIABLE, o[p++] = ptr_pv, o[p++] = pv0 + k, o[p++] = SC_PRIVATE, o[p++] = c_none;
        for (uint32_t k = 0; k < rings; k++)
            o[p++] = 5u << 16 | SPV_OP_VARIABLE, o[p++] = ptr_ring, o[p++] = ra0 + k, o[p++] = SC_PRIVATE, o[p++] = ring_none;
        for (uint32_t k = 0; k < rings; k++)
            o[p++] = 5u << 16 | SPV_OP_VARIABLE, o[p++] = ptr_pv, o[p++] = rc0 + k, o[p++] = SC_PRIVATE, o[p++] = c0;
    }
    uint64_t site = 0;
    uint32_t k_def = 0, k_ring = 0, cur = 0;
    fn = 0;
    for (uint64_t i = fn_start; i < n;) {
        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
        if (op == SPV_OP_FUNCTION && l >= 3) fn = w[i + 2];
        if (op == OP_LABEL_ && l >= 2) cur = w[i + 1];
        if (stores && (op == OP_KILL || op == OP_TERMINATE_INVOCATION || op == OP_DEMOTE_TO_HELPER ||
                       (fn == fs_entry && (op == OP_RETURN || op == OP_RETURN_VALUE)))) {
#define POOL_MARK_STORE(from)                                                                                         \
    do {                                                                                                              \
        uint32_t v_ = next++, ok_ = next++, ix_ = next++, at_ = next++;                                              \
        o[p++] = 4u << 16 | OP_LOAD_, o[p++] = uint_t, o[p++] = v_, o[p++] = (from);                                 \
        o[p++] = 5u << 16 | OP_INOT_EQUAL, o[p++] = bool_t, o[p++] = ok_, o[p++] = v_, o[p++] = c_none;              \
        o[p++] = 6u << 16 | OP_SELECT_, o[p++] = uint_t, o[p++] = ix_, o[p++] = ok_, o[p++] = v_, o[p++] = c_spare;  \
        o[p++] = 6u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_u, o[p++] = at_, o[p++] = var, o[p++] = c0, o[p++] = ix_; \
        o[p++] = 3u << 16 | OP_STORE_, o[p++] = at_, o[p++] = c1;                                                    \
    } while (0)
            for (uint32_t k = 0; k < deferred; k++) POOL_MARK_STORE(pv0 + k);
            for (uint32_t r = 0; r < rings; r++) {
                {
                    uint32_t oc = next++, og = next++, oi = next++, oa = next++;
                    o[p++] = 4u << 16 | OP_LOAD_, o[p++] = uint_t, o[p++] = oc, o[p++] = rc0 + r;
                    o[p++] = 5u << 16 | OP_UGREATER_THAN_, o[p++] = bool_t, o[p++] = og, o[p++] = oc, o[p++] = c_ring;
                    o[p++] = 6u << 16 | OP_SELECT_, o[p++] = uint_t, o[p++] = oi, o[p++] = og, o[p++] = c_over, o[p++] = c_spare;
                    o[p++] = 6u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_u, o[p++] = oa, o[p++] = var, o[p++] = c0, o[p++] = oi;
                    o[p++] = 3u << 16 | OP_STORE_, o[p++] = oa, o[p++] = c1;
                }
                if (op == OP_DEMOTE_TO_HELPER) {
                    for (int j = 0; j < RING; j++) {
                        uint32_t from = next++;
                        o[p++] = 5u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_pv, o[p++] = from, o[p++] = ra0 + r, o[p++] = cr[j];
                        POOL_MARK_STORE(from);
                    }
                    continue;
                }
                uint32_t cnt = next++, lt = next++, lim = next++, hd = next++, j = next++, mg = next++, ct = next++,
                         cond = next++, body = next++, from = next++, jn = next++;
                o[p++] = 4u << 16 | OP_LOAD_, o[p++] = uint_t, o[p++] = cnt, o[p++] = rc0 + r;
                o[p++] = 5u << 16 | OP_ULESS_THAN, o[p++] = bool_t, o[p++] = lt, o[p++] = cnt, o[p++] = c_ring;
                o[p++] = 6u << 16 | OP_SELECT_, o[p++] = uint_t, o[p++] = lim, o[p++] = lt, o[p++] = cnt, o[p++] = c_ring;
                o[p++] = 2u << 16 | OP_BRANCH, o[p++] = hd;
                o[p++] = 2u << 16 | OP_LABEL_, o[p++] = hd;
                o[p++] = 7u << 16 | OP_PHI, o[p++] = uint_t, o[p++] = j, o[p++] = c0, o[p++] = cur, o[p++] = jn, o[p++] = ct;
                o[p++] = 5u << 16 | OP_ULESS_THAN, o[p++] = bool_t, o[p++] = cond, o[p++] = j, o[p++] = lim;
                o[p++] = 4u << 16 | OP_LOOP_MERGE, o[p++] = mg, o[p++] = ct, o[p++] = 0;
                o[p++] = 4u << 16 | OP_BRANCH_CONDITIONAL, o[p++] = cond, o[p++] = body, o[p++] = mg;
                o[p++] = 2u << 16 | OP_LABEL_, o[p++] = body;
                o[p++] = 5u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_pv, o[p++] = from, o[p++] = ra0 + r, o[p++] = j;
                POOL_MARK_STORE(from);
                o[p++] = 2u << 16 | OP_BRANCH, o[p++] = ct;
                o[p++] = 2u << 16 | OP_LABEL_, o[p++] = ct;
                o[p++] = 5u << 16 | OP_IADD, o[p++] = uint_t, o[p++] = jn, o[p++] = j, o[p++] = c1;
                o[p++] = 2u << 16 | OP_BRANCH, o[p++] = hd;
                o[p++] = 2u << 16 | OP_LABEL_, o[p++] = mg;
                cur = mg;
            }
#undef POOL_MARK_STORE
        }
        memcpy(o + p, w + i, l * 4), p += l;
        if ((op == OP_ACCESS_CHAIN_ || op == OP_IN_BOUNDS_ACCESS_CHAIN) && l >= 5 && w[i + 3] < bound && set2[w[i + 3]] &&
            bind[w[i + 3]] && bind[w[i + 3]] - 1 < VK_POOL_KINDS) {
            uint8_t d = site_def[site++];
            if (d) {
                uint32_t ix = w[i + 4];
                if (d & 2) o[p++] = 4u << 16 | OP_BITCAST, o[p++] = uint_t, o[p++] = next, o[p++] = ix, ix = next++;
                if ((d & 4) && shared) {
                    uint32_t ra = ra0, rc = rc0, cnt = next++, cm1 = next++, jl = next++, pl = next++, last = next++,
                             same = next++, jn = next++, js = next++, at = next++, inc = next++, c2 = next++;
                    o[p++] = 4u << 16 | OP_LOAD_, o[p++] = uint_t, o[p++] = cnt, o[p++] = rc;
                    o[p++] = 5u << 16 | OP_ISUB_, o[p++] = uint_t, o[p++] = cm1, o[p++] = cnt, o[p++] = c1;
                    o[p++] = 5u << 16 | OP_BITWISE_AND, o[p++] = uint_t, o[p++] = jl, o[p++] = cm1, o[p++] = cr[RING - 1];
                    o[p++] = 5u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_pv, o[p++] = pl, o[p++] = ra, o[p++] = jl;
                    o[p++] = 4u << 16 | OP_LOAD_, o[p++] = uint_t, o[p++] = last, o[p++] = pl;
                    o[p++] = 5u << 16 | OP_IEQUAL_, o[p++] = bool_t, o[p++] = same, o[p++] = last, o[p++] = ix;
                    o[p++] = 5u << 16 | OP_BITWISE_AND, o[p++] = uint_t, o[p++] = jn, o[p++] = cnt, o[p++] = cr[RING - 1];
                    o[p++] = 6u << 16 | OP_SELECT_, o[p++] = uint_t, o[p++] = js, o[p++] = same, o[p++] = jl, o[p++] = jn;
                    o[p++] = 5u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_pv, o[p++] = at, o[p++] = ra, o[p++] = js;
                    o[p++] = 3u << 16 | OP_STORE_, o[p++] = at, o[p++] = ix;
                    o[p++] = 5u << 16 | OP_IADD, o[p++] = uint_t, o[p++] = inc, o[p++] = cnt, o[p++] = c1;
                    o[p++] = 6u << 16 | OP_SELECT_, o[p++] = uint_t, o[p++] = c2, o[p++] = same, o[p++] = cnt, o[p++] = inc;
                    o[p++] = 3u << 16 | OP_STORE_, o[p++] = rc, o[p++] = c2;
                } else if (d & 4) {
                    uint32_t ra = ra0 + k_ring, rc = rc0 + k_ring++, cnt = next++, j = next++, at = next++, inc = next++;
                    o[p++] = 4u << 16 | OP_LOAD_, o[p++] = uint_t, o[p++] = cnt, o[p++] = rc;
                    o[p++] = 5u << 16 | OP_BITWISE_AND, o[p++] = uint_t, o[p++] = j, o[p++] = cnt, o[p++] = cr[RING - 1];
                    o[p++] = 5u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_pv, o[p++] = at, o[p++] = ra, o[p++] = j;
                    o[p++] = 3u << 16 | OP_STORE_, o[p++] = at, o[p++] = ix;
                    o[p++] = 5u << 16 | OP_IADD, o[p++] = uint_t, o[p++] = inc, o[p++] = cnt, o[p++] = c1;
                    o[p++] = 3u << 16 | OP_STORE_, o[p++] = rc, o[p++] = inc;
                } else {
                    o[p++] = 3u << 16 | OP_STORE_, o[p++] = pv0 + k_def++, o[p++] = ix;
                }
            } else {
                uint32_t at = next++;
                o[p++] = 6u << 16 | OP_ACCESS_CHAIN_, o[p++] = ptr_u, o[p++] = at, o[p++] = var, o[p++] = c0, o[p++] = w[i + 4];
                o[p++] = 3u << 16 | OP_STORE_, o[p++] = at, o[p++] = c1;
            }
        }
        i += l;
    }
    o[3] = next;
    POOL_MARK_FREE();
#undef POOL_MARK_FREE
    *out_n = p;
    *status = 1;
    return o;
}

static int spirv_mark_list_test(const char *list) {
    marks_vs_ok = marks_fs_ok = 1;
    FILE *f = fopen(list, "rb");
    if (!f) return printf("spirv: pool marks test: %s cannot be read\n", list), 1;
    char line[2048];
    int marked = 0, none = 0, missed = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char in[1024], out[1024];
        if (sscanf(line, "%1023s %1023s", in, out) != 2) continue;
        FILE *g = fopen(in, "rb");
        if (!g) { bad++; continue; }
        fseek(g, 0, SEEK_END);
        long sz = ftell(g);
        fseek(g, 0, SEEK_SET);
        uint32_t *w = sz > 20 ? malloc((size_t)sz) : NULL;
        if (!w || fread(w, 1, (size_t)sz, g) != (size_t)sz) { fclose(g), free(w), bad++; continue; }
        fclose(g);
        uint64_t n = (uint64_t)sz / 4, mn = 0;
        uint32_t *pooled = spirv_pool_rebind(w, n);
        int st;
        const char *why;
        uint32_t *m = spirv_pool_mark(pooled ? pooled : w, n, 1, &mn, &st, &why);
        if (st > 0) {
            marked++;
            if (!plat_file_write_atomic(out, m, (size_t)mn * 4)) {
                if (!bad) printf("spirv: pool marks test: %s cannot be written\n", out);
                bad++;
            }
        } else if (st < 0) {
            missed++;
            printf("spirv: pool marks test: %s not markable: %s\n", in, why);
        } else {
            none++;
        }
        free(m), free(pooled), free(w);
    }
    fclose(f);
    printf("spirv: pool marks test: %d marked, %d without pool texture accesses, %d not markable, %d file errors\n", marked,
           none, missed, bad);
    return bad;
}

static VkShaderModule make_module(const uint32_t *w, uint64_t words, int early_ok) {
    const uint32_t *orig_w = w;
    uint64_t orig_words = words, fixed_n = 0;
    uint32_t *fixed = spirv_fixup(w, words, &fixed_n);
    if (fixed) w = fixed, words = fixed_n;
    uint32_t *pooled = spirv_pool_rebind(w, words);
    if (pooled) {
        free(fixed);
        w = fixed = pooled;
        if (vk_pool_marks_enabled()) {
            uint64_t mk_n = 0;
            int st;
            const char *why;
            uint32_t *mk = spirv_pool_mark(w, words, early_ok, &mk_n, &st, &why);
            if (mk) {
                free(fixed);
                w = fixed = mk, words = mk_n;
            } else if (st < 0) {
                int nth = plat_inc32(&vk_pool_marks_missed);
                if (nth <= 8) {
                    uint32_t model = ~0u;
                    for (uint64_t i = 5; i < words;) {
                        uint32_t op = w[i] & 0xffff, l = w[i] >> 16;
                        if (!l) break;
                        if (op == SPV_OP_ENTRY_POINT && l >= 2) { model = w[i + 1]; break; }
                        i += l;
                    }
                    const char *stage = model == 0 ? "vertex" : model <= 3 ? "tessellation or geometry" : model == 4 ? "fragment"
                                      : model == 5 ? "compute" : model == 5364 ? "task" : model == 5365 ? "mesh" : "other";
                    rt_log("vk_pipeline: a %s shader that indexes the texture pool could not be marked (SPIR-V %u.%u, %llu "
                           "words, hash %016llx): %s%s", stage, w[1] >> 16 & 0xff, w[1] >> 8 & 0xff,
                           (unsigned long long)words, (unsigned long long)fnv(FNV0, orig_w, (size_t)orig_words * 4), why,
                           nth == 1 ? "; textures are not evicted by disuse this run (NX2SARIA_TEX_DISUSE)" : "");
                }
            }
        }
    }
    uint64_t rc_n = 0;
    uint32_t *rc = P.reconverge ? spirv_reconverge(w, words, &rc_n) : NULL;
    if (rc) {
        free(fixed);
        w = fixed = rc, words = rc_n;
    }
    uint64_t np_n = 0;
    uint32_t *np = P.nan_preserve ? spirv_nan_preserve(w, words, P.nan_preserve, &np_n) : NULL;
    if (np) {
        free(fixed);
        w = fixed = np, words = np_n;
    }
    uint64_t nb_n = 0;
    uint32_t nb_tests = 0, nb_sels = 0;
    uint32_t *nb = P.nan_bits ? spirv_nan_bits(w, words, &nb_n, &nb_tests, &nb_sels) : NULL;
    if (nb) {
        free(fixed);
        w = fixed = nb, words = nb_n;
        __atomic_fetch_add(&P.nan_bits_tests, nb_tests, __ATOMIC_RELAXED);
        __atomic_fetch_add(&P.nan_bits_selects, nb_sels, __ATOMIC_RELAXED);
        if (__atomic_add_fetch(&P.nan_bits_modules, 1, __ATOMIC_RELAXED) == 1)
            rt_log("vk_pipeline: first module with NaN tests made integer: %u tests, %u selects of NaN bits", nb_tests,
                   nb_sels);
    }
    uint64_t mi_n = 0;
    uint32_t *mi = P.mesh_invariant ? spirv_mesh_invariant(w, words, &mi_n) : NULL;
    if (mi) {
        free(fixed);
        w = fixed = mi, words = mi_n;
        uint32_t k = __atomic_add_fetch(&P.mesh_invariant_modules, 1, __ATOMIC_RELAXED);
        if (k == 1 || k == 10 || k == 100 || k == 1000)
            rt_log("vk_pipeline: mesh positions made Invariant: %u mesh modules so far (NX2SARIA_VK_MESH_INVARIANT)", k);
    }
    if (P.feedback >= 2) mm_hash = fnv(FNV0, w, (size_t)words * 4);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)words * 4, .pCode = w};
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(P.device, &ci, NULL, &m);
    free(fixed);
    return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

typedef struct {
    uint32_t n;
    VkDescriptorSetLayoutBinding b[MAX_BINDINGS];
    VkPipeBinding pb[MAX_BINDINGS];
    int pool;
} Bindings;

static VkDescriptorType descriptor_type(int kind, const GfxShaderRes *r) {
    int buffer = (r->type & 0xff) == 2;
    switch (kind) {
    case GFX_RES_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case GFX_RES_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case GFX_RES_TEX:
        if (buffer) return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        if (r->separate) LOG_ONCE("vk_pipeline: a separate texture handle: bound as SAMPLED_IMAGE");
        return r->separate ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default: return buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}

static VkShaderStageFlags stage_bit(int stage) {
    return stage == GFX_STAGE_VS ? VK_SHADER_STAGE_VERTEX_BIT : stage == GFX_STAGE_FS ? VK_SHADER_STAGE_FRAGMENT_BIT
         : stage == GFX_STAGE_TS ? VK_SHADER_STAGE_TASK_BIT_EXT : stage == GFX_STAGE_MS ? VK_SHADER_STAGE_MESH_BIT_EXT
                                                                 : VK_SHADER_STAGE_COMPUTE_BIT;
}

static int is_mesh(const VkPipeState *s) { return s->vs && gfx_shader_stage(s->vs) == GFX_STAGE_MS; }

static int mesh_libs(void) { return P.mesh_lib && P.indep; }

int vk_pipeline_mesh_libs(void) { return ensure_init() && mesh_libs(); }

static int no_libs(const VkPipeState *s, const Layout *l) { return is_mesh(s) && !(l && l->set1); }

static const char *origin_name(const GfxShader *s) {
    static const char *const name[3] = {"from the shader cache", "a previous translator's", "translated in this session"};
    return s ? name[gfx_shader_origin(s)] : "none";
}

static void crash_note(const char *what, const GfxShader *a, const GfxShader *b, const GfxShader *c) {
    static __thread char buf[320];
    char *o = buf;
    o += snprintf(o, 160, "building %s", what);
    const GfxShader *sh[3] = {a, b, c};
    for (int i = 0; i < 3; i++)
        if (sh[i]) {
            const uint8_t *k = gfx_shader_key(sh[i]);
            o += snprintf(o, 40, " stage %d %02x%02x%02x%02x%02x%02x%02x%02x", gfx_shader_stage(sh[i]), k[0], k[1], k[2], k[3],
                          k[4], k[5], k[6], k[7]);
        }
    plat_crash_context(buf);
}

static uint32_t binding_span(const GfxShader *s) {
    uint32_t span = 1;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        const GfxShaderRes *r;
        int n = gfx_shader_resources(s, k, &r);
        for (int i = 0; i < n; i++)
            if (r[i].binding + (uint32_t)(r[i].count ? r[i].count : 1) > span) span = r[i].binding + (r[i].count ? r[i].count : 1);
    }
    return span;
}

static int add_stage(Bindings *B, const GfxShader *s, uint32_t off) {
    int stage = gfx_shader_stage(s);
    VkShaderStageFlags bit = stage_bit(stage);
    if (!B->n) {
        B->b[0] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL};
        B->pb[0] = (VkPipeBinding){0, (uint8_t)stage, VK_PIPE_SUPPORT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, 0, NULL};
        B->n = 1;
    }
    B->b[0].stageFlags |= bit;
    B->pb[0].stages |= bit;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        const GfxShaderRes *r;
        int n = gfx_shader_resources(s, k, &r);
        for (int i = 0; i < n; i++) {
            if (r[i].pool) {
                B->pool = 1;
                continue;
            }
            if (B->n == MAX_BINDINGS || !r[i].binding) return 0;
            VkDescriptorType t = descriptor_type(k, &r[i]);
            uint32_t binding = r[i].binding + off, count = r[i].count ? r[i].count : 1;
            B->b[B->n] = (VkDescriptorSetLayoutBinding){binding, t, count, bit, NULL};
            B->pb[B->n] = (VkPipeBinding){(uint16_t)binding, (uint8_t)stage, (uint8_t)k, t, (uint16_t)count, bit, &r[i]};
            B->n++;
        }
    }
    return 1;
}

static int module_binding_missing(const GfxShader *s, const VkDescriptorSetLayoutBinding *b, uint32_t n, uint32_t off) {
    const uint32_t *bind;
    uint32_t nb = gfx_shader_bindings(s, &bind);
    const GfxShaderRes *tr;
    int nt = gfx_shader_resources(s, GFX_RES_TEX, &tr);
    for (uint32_t i = 0; i < nb; i++) {
        int pooled = 0;
        for (int t = 0; t < nt; t++)
            if (tr[t].pool && tr[t].binding == (bind[i] & 0xffff)) pooled = 1;
        if (pooled) continue;
        uint32_t want = bind[i] + (bind[i] >= 1 ? off : 0), j = 0;
        while (j < n && b[j].binding != want) j++;
        if (j == n) return (int)bind[i];
    }
    return -1;
}

static int layout_covers(const VkPipeState *s, const Layout *l, uint32_t fs_off, char *why, size_t cap) {
    uint32_t nv = l->set1 ? l->n0 : l->n;
    int m = module_binding_missing(s->vs, l->b, nv, 0);
    const char *stage = "vertex";
    if (m < 0 && s->fs) {
        m = l->set1 ? module_binding_missing(s->fs, l->b + l->n0, l->n - l->n0, 0) : module_binding_missing(s->fs, l->b, l->n, fs_off);
        stage = "fragment";
    }
    if (m >= 0) snprintf(why, cap, "the %s shader uses binding %d, which its reflection and layout lack", stage, m);
    return m < 0;
}

static VkDescriptorUpdateTemplate make_tpl_bp(const VkDescriptorSetLayoutBinding *b, uint32_t n, VkDescriptorSetLayout sl,
                                              int push, VkPipelineLayout pl, uint32_t set, int bp) {
    if (!P.tpl || !n || (push && !P.tpl_push)) return VK_NULL_HANDLE;
    VkDescriptorUpdateTemplateEntry e[MAX_BINDINGS];
    size_t off = 0;
    VkShaderStageFlags stages = 0;
    for (uint32_t i = 0; i < n; i++) {
        e[i] = (VkDescriptorUpdateTemplateEntry){b[i].binding, 0, b[i].descriptorCount, b[i].descriptorType, off, sizeof(VkDescSlot)};
        off += sizeof(VkDescSlot) * b[i].descriptorCount;
        stages |= b[i].stageFlags;
    }
    VkDescriptorUpdateTemplateCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, .descriptorUpdateEntryCount = n,
        .pDescriptorUpdateEntries = e,
        .templateType = push ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = sl,
        .pipelineBindPoint = bp >= 0 ? (VkPipelineBindPoint)bp
                             : stages & VK_SHADER_STAGE_COMPUTE_BIT ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
        .pipelineLayout = pl, .set = set};
    VkDescriptorUpdateTemplate t;
    if (P.tpl_create(P.device, &ci, NULL, &t) != VK_SUCCESS) {
        LOG_ONCE("vk_pipeline: descriptor update template creation failed: that set is written with write structs");
        return VK_NULL_HANDLE;
    }
    return t;
}
static VkDescriptorUpdateTemplate make_tpl(const VkDescriptorSetLayoutBinding *b, uint32_t n, VkDescriptorSetLayout sl,
                                           int push, VkPipelineLayout pl, uint32_t set) {
    return make_tpl_bp(b, n, sl, push, pl, set, -1);
}

static int64_t bindings_sort(Bindings *B, uint64_t *h) {
    for (uint32_t i = 1; i < B->n; i++)
        for (uint32_t j = i; j > 0 && B->b[j - 1].binding > B->b[j].binding; j--) {
            VkDescriptorSetLayoutBinding t = B->b[j]; B->b[j] = B->b[j - 1]; B->b[j - 1] = t;
            VkPipeBinding u = B->pb[j]; B->pb[j] = B->pb[j - 1]; B->pb[j - 1] = u;
        }
    int64_t total = 0;
    for (uint32_t i = 0; i < B->n; i++) {
        if (i && B->b[i].binding == B->b[i - 1].binding) return -1;
        uint32_t k[4] = {B->b[i].binding, B->b[i].descriptorType, B->b[i].descriptorCount, B->b[i].stageFlags};
        *h = fnv(*h, k, sizeof k);
        total += B->b[i].descriptorCount;
    }
    return total;
}

static Layout *layout_get(Bindings *B) {
    uint64_t h = FNV0;
    int64_t total = bindings_sort(B, &h);
    if (total < 0) return NULL;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->n == B->n && !memcmp(l->b, B->b, B->n * sizeof *B->b)) return l;
    Layout *l = calloc(1, sizeof *l + B->n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = B->n;
    memcpy(l->b, B->b, B->n * sizeof *B->b);
    l->push = total <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = l->n, .pBindings = l->b};
    if (vkCreateDescriptorSetLayout(P.device, &si, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    VkDescriptorSetLayout sets3[3] = {l->set, vk_empty_set_layout(), vk_pool_set_layout()};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
                                     .pSetLayouts = sets3};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl(l->b, l->n, l->set, l->push, l->layout, 0);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    P.stats.push_layouts += (uint64_t)l->push;
    return l;
}

static int mesh_emu_on(void) {
    const VkCtx *c = vk_ctx();
    return c && c->mesh_emulation;
}

static Layout *layout_get_emu(Bindings *B) {
    for (uint32_t i = 0; i < B->n; i++) {
        VkShaderStageFlags f = B->b[i].stageFlags;
        if (f & (VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT))
            f = (f & ~(VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT)) | VK_SHADER_STAGE_COMPUTE_BIT;
        B->b[i].stageFlags = f;
        B->pb[i].stages = f;
    }
    uint64_t h = fnv(FNV0, "mesh-emu", 8);
    int64_t total = bindings_sort(B, &h);
    if (total < 0) return NULL;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->emu && l->n == B->n && !memcmp(l->b, B->b, B->n * sizeof *B->b)) return l;
    Layout *l = calloc(1, sizeof *l + B->n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = B->n, l->emu = 1;
    memcpy(l->b, B->b, B->n * sizeof *B->b);
    l->push = total <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = l->n, .pBindings = l->b};
    if (vkCreateDescriptorSetLayout(P.device, &si, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    VkDescriptorSetLayout sets3[3] = {l->set, vk_empty_set_layout(), vk_pool_set_layout()};
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, 0, MESH_EMU_PC_BYTES};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
                                     .pSetLayouts = sets3, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl_bp(l->b, l->n, l->set, l->push, l->layout, 0, VK_PIPELINE_BIND_POINT_GRAPHICS);
    l->tpl_c = make_tpl_bp(l->b, l->n, l->set, l->push, l->layout, 0, VK_PIPELINE_BIND_POINT_COMPUTE);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    return l;
}

typedef struct MeshEmuEnt {
    struct MeshEmuEnt *next;
    const GfxShader *ms, *ts;
    VkPipelineLayout layout;
    int ok;
    VkPipeline mesh, task;
    MeshEmuInfo mi, ti;
} MeshEmuEnt;
#define EMU_BUCKETS 256
static MeshEmuEnt *emu_ents[EMU_BUCKETS];
static PlatMutex emu_lock = PLAT_MUTEX_INIT;

static uint32_t emu_bucket(const GfxShader *ms, const GfxShader *ts, VkPipelineLayout layout) {
    uint64_t h = (uint64_t)(uintptr_t)ms * 0x9e3779b97f4a7c15ull ^ (uint64_t)(uintptr_t)ts * 0xc2b2ae3d27d4eb4full ^
                 (uint64_t)(uintptr_t)layout * 0x165667b19e3779f9ull;
    return (uint32_t)(h >> 40) % EMU_BUCKETS;
}

static MeshEmuEnt *emu_find(MeshEmuEnt *e, const GfxShader *ms, const GfxShader *ts, VkPipelineLayout layout) {
    while (e && !(e->ms == ms && e->ts == ts && e->layout == layout)) e = __atomic_load_n(&e->next, __ATOMIC_ACQUIRE);
    return e;
}

static VkPipeline emu_compute(const uint32_t *w, size_t n, VkPipelineLayout layout) {
    VkShaderModule m = make_module(w, n, 0);
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(P.device, P.cache, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(P.device, m, NULL);
    return p;
}

static const MeshEmuEnt *emu_get(const GfxShader *ms, const GfxShader *ts, VkPipelineLayout layout, uint32_t toff, int create) {
    MeshEmuEnt **head = &emu_ents[emu_bucket(ms, ts, layout)];
    MeshEmuEnt *e = emu_find(__atomic_load_n(head, __ATOMIC_ACQUIRE), ms, ts, layout);
    if (e || !create) return e;
    e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->ms = ms, e->ts = ts, e->layout = layout;
    const char *why = NULL;
    uint64_t n = 0;
    size_t cn = 0;
    const uint32_t *w = gfx_shader_spirv_pin(ms, &n);
    uint32_t *c = w ? mesh_emu_compute(w, (size_t)n, &e->mi, &cn, &why) : NULL;
    gfx_shader_spirv_unpin(ms);
    if (c && (e->mesh = emu_compute(c, cn, layout))) e->ok = 1;
    else if (!why) why = "the mesh stage's compute pipeline was not created";
    free(c);
    if (e->ok && e->mi.payload_words && !ts) e->ok = 0, why = "a mesh stage that reads a task payload without a task stage";
    if (e->ok && ts) {
        w = gfx_shader_spirv_pin(ts, &n);
        c = w ? mesh_emu_compute(w, (size_t)n, &e->ti, &cn, &why) : NULL;
        gfx_shader_spirv_unpin(ts);
        uint32_t *r = c && toff ? spirv_rebind(c, cn, toff) : NULL;
        if (c && (e->task = emu_compute(r ? r : c, cn, layout))) {
            if (e->ti.payload_words != e->mi.payload_words) e->ok = 0, why = "task and mesh payload sizes differ";
        } else {
            e->ok = 0;
            if (!why) why = "the task stage's compute pipeline was not created";
        }
        free(r), free(c);
    }
    plat_mutex_lock(&emu_lock);
    MeshEmuEnt *had = emu_find(*head, ms, ts, layout);
    if (!had) {
        static int logged;
        if (!e->ok && logged++ < 16) rt_log("vk_pipeline: mesh emulation: a mesh program is not emulated: %s", why ? why : "?");
        e->next = *head;
        __atomic_store_n(head, e, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&emu_lock);
    if (had) {
        if (e->mesh) vkDestroyPipeline(P.device, e->mesh, NULL);
        if (e->task) vkDestroyPipeline(P.device, e->task, NULL);
        free(e);
        return had;
    }
    return e;
}

int vk_pipeline_mesh_emu(const GfxShader *ms, const GfxShader *ts, VkPipelineLayout layout, VkPipeline *mesh,
                         VkPipeline *task, MeshEmuInfo *mi) {
    const MeshEmuEnt *e = emu_get(ms, ts, layout, 0, 0);
    if (!e || !e->ok) return 0;
    *mesh = e->mesh, *task = e->task;
    *mi = e->mi;
    if (ts) mi->max_mesh = e->ti.max_mesh;
    return 1;
}

static Layout *layout_pair(Bindings *B, Bindings *F) {
    uint64_t h = fnv(FNV0, "indep", 5);
    int64_t tv = bindings_sort(B, &h);
    h = fnv(h, "|", 1);
    int64_t tf = bindings_sort(F, &h);
    if (tv < 0 || tf < 0 || B->n + F->n > MAX_BINDINGS) return NULL;
    uint32_t n0 = B->n, n = B->n + F->n;
    memcpy(B->b + n0, F->b, F->n * sizeof *F->b);
    memcpy(B->pb + n0, F->pb, F->n * sizeof *F->pb);
    for (uint32_t i = n0; i < n; i++) B->pb[i].set = 1;
    B->n = n;
    Layout **head = &layouts[h % LAYOUT_BUCKETS];
    for (Layout *l = *head; l; l = l->next)
        if (l->hash == h && l->set1 && l->n == n && l->n0 == n0 && !memcmp(l->b, B->b, n * sizeof *B->b)) return l;
    Layout *l = calloc(1, sizeof *l + n * sizeof *B->b);
    if (!l) return NULL;
    l->hash = h, l->n = n, l->n0 = n0;
    memcpy(l->b, B->b, n * sizeof *B->b);
    l->push = P.push_stage == GFX_STAGE_VS && tv <= (int64_t)P.max_push;
    l->push1 = P.push_stage == GFX_STAGE_FS && F->n && tf <= (int64_t)P.max_push;
    VkDescriptorSetLayoutCreateInfo s0 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = n0, .pBindings = l->b};
    VkDescriptorSetLayoutCreateInfo s1 = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = l->push1 ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0,
                                          .bindingCount = n - n0, .pBindings = l->b + n0};
    if (vkCreateDescriptorSetLayout(P.device, &s0, NULL, &l->set) != VK_SUCCESS) {
        free(l);
        return NULL;
    }
    if (vkCreateDescriptorSetLayout(P.device, &s1, NULL, &l->set1) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        free(l);
        return NULL;
    }
    VkDescriptorSetLayout sets[3] = {l->set, l->set1, vk_pool_set_layout()};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 3,
                                     .pSetLayouts = sets};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->layout) != VK_SUCCESS) {
        vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
        vkDestroyDescriptorSetLayout(P.device, l->set1, NULL);
        free(l);
        return NULL;
    }
    l->tpl[0] = make_tpl(l->b, n0, l->set, l->push, l->layout, 0);
    l->tpl[1] = make_tpl(l->b + n0, n - n0, l->set1, l->push1, l->layout, 1);
    l->next = *head;
    *head = l;
    P.stats.layouts++;
    P.stats.push_layouts += (uint64_t)(l->push | l->push1);
    return l;
}

#define VS_OUTS_CAP 8192
static struct {
    PlatMutex lock;
    const GfxShader *vs[VS_OUTS_CAP];
    uint32_t mask[VS_OUTS_CAP];
    GfxShaderIo io[VS_OUTS_CAP][32];
    uint32_t n, full;
} VO = {.lock = PLAT_MUTEX_INIT};

static uint32_t vs_outs(const GfxShader *vs, const GfxShader *fs, GfxShaderIo io[32]) {
    GfxShaderIo fin[32];
    uint32_t need = fs ? gfx_shader_inputs(fs, fin) : 0, mask;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && fin[i].packed) need &= ~(1u << i);
    plat_mutex_lock(&VO.lock);
    uint32_t j = (uint32_t)(((uintptr_t)vs >> 4) * 0x9e3779b1u) & (VS_OUTS_CAP - 1);
    while (VO.vs[j] && VO.vs[j] != vs) j = (j + 1) & (VS_OUTS_CAP - 1);
    if (!VO.vs[j] && VO.n + 1 >= VS_OUTS_CAP * 3 / 4) {
        VO.full++;
        plat_mutex_unlock(&VO.lock);
        LOG_ONCE("vk_pipeline: vertex output union table full: outputs from the pair");
        memset(io, 0, 32 * sizeof *io);
        for (int i = 0; i < 32; i++)
            if (need >> i & 1) io[i] = fin[i];
        return need;
    }
    if (!VO.vs[j]) VO.vs[j] = vs, VO.n++;
    for (int i = 0; i < 32; i++)
        if (!(need >> i & 1)) continue;
        else if (!(VO.mask[j] >> i & 1)) VO.io[j][i] = fin[i], VO.mask[j] |= 1u << i;
        else if (VO.io[j][i].type == fin[i].type && VO.io[j][i].comps < fin[i].comps) VO.io[j][i].comps = fin[i].comps;
    mask = VO.mask[j];
    memcpy(io, VO.io[j], 32 * sizeof *io);
    plat_mutex_unlock(&VO.lock);
    return mask;
}

#define MESH_OUTS_CAP 1024
static struct {
    PlatMutex lock;
    const GfxShader *ms[MESH_OUTS_CAP];
    uint32_t mask[MESH_OUTS_CAP];
    uint8_t points[MESH_OUTS_CAP];
    uint32_t n;
} MSO = {.lock = PLAT_MUTEX_INIT};

static uint32_t mesh_outs(const GfxShader *ms, int *points) {
    uint32_t j = (uint32_t)(((uintptr_t)ms >> 4) * 0x9e3779b1u) & (MESH_OUTS_CAP - 1), mask;
    plat_mutex_lock(&MSO.lock);
    while (MSO.ms[j] && MSO.ms[j] != ms) j = (j + 1) & (MESH_OUTS_CAP - 1);
    if (MSO.ms[j]) {
        mask = MSO.mask[j], *points = MSO.points[j];
        plat_mutex_unlock(&MSO.lock);
        return mask;
    }
    plat_mutex_unlock(&MSO.lock);
    uint64_t n = 0;
    const uint32_t *w = gfx_shader_spirv_pin(ms, &n);
    *points = 0;
    mask = w ? spirv_mesh_outputs(w, n, points) : ~0u;
    gfx_shader_spirv_unpin(ms);
    plat_mutex_lock(&MSO.lock);
    for (j = (uint32_t)(((uintptr_t)ms >> 4) * 0x9e3779b1u) & (MESH_OUTS_CAP - 1); MSO.ms[j] && MSO.ms[j] != ms;)
        j = (j + 1) & (MESH_OUTS_CAP - 1);
    if (!MSO.ms[j] && MSO.n + 1 < MESH_OUTS_CAP * 3 / 4)
        MSO.ms[j] = ms, MSO.mask[j] = mask, MSO.points[j] = (uint8_t)*points, MSO.n++;
    plat_mutex_unlock(&MSO.lock);
    return mask;
}

static uint32_t mesh_fill(const GfxShader *ms, const GfxShader *fs, int *points) {
    GfxShaderIo fin[32];
    *points = 0;
    if (!fs) return 0;
    int pts = 0;
    uint32_t need = gfx_shader_inputs(fs, fin), have = mesh_outs(ms, &pts), missing = 0;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && !fin[i].packed && !(have >> i & 1)) missing |= 1u << i;
    if (missing) *points = pts;
    return missing;
}

static Pipe *pipe_find(const void *key, size_t size, uint64_t h) {
    for (Pipe *p = pipes[h % PIPE_BUCKETS]; p; p = p->next)
        if (p->hash == h && p->key_size == size && !memcmp(p->key, key, size)) return p;
    return NULL;
}

static Pipe *pipe_add(const void *key, size_t size, uint64_t h, VkPipeline pipeline, const Layout *l, const Bindings *B,
                      VkPipelineBindPoint bp, uint32_t vs_inputs) {
    Pipe *p = calloc(1, sizeof *p + size);
    if (!p) return NULL;
    p->hash = h, p->key_size = size;
    memcpy(p->key, key, size);
    if (l && B) {
        p->bindings = malloc(B->n * sizeof *p->bindings);
        if (p->bindings) memcpy(p->bindings, B->pb, B->n * sizeof *p->bindings);
    }
    p->info = (VkPipeInfo){pipeline, l ? l->layout : VK_NULL_HANDLE, l ? l->set : VK_NULL_HANDLE, l ? l->push : 0, bp,
                           p->bindings ? B->n : 0, p->bindings, vs_inputs};
    p->info.set_layout1 = l ? l->set1 : VK_NULL_HANDLE;
    p->info.push1 = l ? l->push1 : 0;
    p->info.n_sets = l && l->set1 ? 2 : 1;
    p->info.pool = B ? B->pool : 0;
    if (l) p->info.tpl[0] = l->tpl[0], p->info.tpl[1] = l->tpl[1];
    if (l && l->emu) p->info.emu = 1, p->info.emu_tpl_c = l->tpl_c;
    p->lay = l;
    Pipe **head = &pipes[h % PIPE_BUCKETS];
    p->next = *head;
    *head = p;
    return p;
}

typedef struct {
    const GfxShader *vs, *fs, *ts;
    uint32_t color[8], depth, stencil;
    uint8_t n_color, samples, topo_class, minus_w;
    uint8_t clamp, poly_mode, a2c, logic_op;
    uint8_t blend_en, dsb, fbl, pad;
    uint32_t wmask;
    uint8_t blend[8][6];
    uint32_t attr_fmt[32], attr_off[32];
    uint8_t attr_bind[32];
    uint8_t rate[17];
    uint8_t indep;
    uint8_t pad2[2];
} GKey;

static int blendable(VkFormat f) {
    return f != VK_FORMAT_UNDEFINED && (format_features(f, 1) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
}

static int target_count(const VkPipeState *s) {
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (s->color_fmt[i]) n = i + 1;
    return n;
}

static int is_src1(unsigned f) { return f >= NVN_BLEND_FUNC_SRC1_COLOR && f <= NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA; }
static VkPipelineCreateFlags noopt_flag_for(int mesh) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("NX2SARIA_VK_NOOPT");
        int by_driver = !v || !*v;
        on = by_driver ? (vk_caps.amd_sc ? 2 : 0) : !strcmp(v, "mesh") ? 2 : strcmp(v, "0") != 0;
        if (on) rt_log("vk_pipeline: %s built without the driver's optimiser (%s)",
                       on == 2 ? "pipelines with task and mesh stages" : "pipelines",
                       by_driver ? "the default on AMD's driver for RDNA 1 and 2, where the device is lost in passes "
                                   "that start with such a pipeline; NX2SARIA_VK_NOOPT=0 builds them optimised"
                                 : "NX2SARIA_VK_NOOPT");
    }
    return on == 1 || (on == 2 && mesh) ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0;
}
static VkPipelineCreateFlags noopt_flag(void) { return noopt_flag_for(0); }

enum { FB_GRAPHICS, FB_LIBRARY, FB_COMPUTE, FB_KINDS };
static struct {
    PlatMutex lock;
    uint64_t n[FB_KINDS], hit[FB_KINDS];
    double hit_ms[FB_KINDS], miss_ms[FB_KINDS];
} FB = {.lock = PLAT_MUTEX_INIT};

static const void *fb_chain(VkPipelineCreationFeedbackCreateInfo *ci, VkPipelineCreationFeedback *fb, const void *next) {
    if (!P.feedback) return next;
    memset(fb, 0, sizeof *fb);
    *ci = (VkPipelineCreationFeedbackCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                                 .pNext = next, .pPipelineCreationFeedback = fb};
    return ci;
}

static void fb_note(int kind, const VkPipelineCreationFeedback *fb) {
    if (!P.feedback || !P.cache || !(fb->flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT)) return;
    int hit = (fb->flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    double ms = (double)fb->duration / 1e6;
    plat_mutex_lock(&FB.lock);
    FB.n[kind]++, FB.hit[kind] += (uint64_t)hit;
    if (hit) FB.hit_ms[kind] += ms;
    else FB.miss_ms[kind] += ms;
    plat_mutex_unlock(&FB.lock);
}

static void fb_log(const char *when) {
    pb_log(when);
    if (!P.feedback || !P.cache) return;
    static const char *const kn[FB_KINDS] = {"pipelines", "libraries", "compute"};
    char line[512];
    int o = snprintf(line, sizeof line, "vk_pipeline: pipeline-cache.bin hits %s:", when);
    plat_mutex_lock(&FB.lock);
    for (int k = 0; k < FB_KINDS && o < (int)sizeof line - 96; k++) {
        uint64_t miss = FB.n[k] - FB.hit[k];
        o += snprintf(line + o, sizeof line - o, " %s %llu of %llu (%.0f%%; avg %.2f ms from the file, %.1f ms otherwise)%s", kn[k],
                      (unsigned long long)FB.hit[k], (unsigned long long)FB.n[k], FB.n[k] ? 100.0 * FB.hit[k] / FB.n[k] : 0.0,
                      FB.hit[k] ? FB.hit_ms[k] / FB.hit[k] : 0.0, miss ? FB.miss_ms[k] / miss : 0.0, k + 1 < FB_KINDS ? ";" : "");
    }
    plat_mutex_unlock(&FB.lock);
    rt_log("%s", line);
}

static int uses_src1(const VkPipeState *s) {
    for (uint32_t m = s->color.blend_en & 0xffu; m; m &= m - 1) {
        int i = __builtin_ctz(m);
        const blend_st *b = &s->blend[i];
        if ((is_src1(b->src_rgb) || is_src1(b->dst_rgb) || is_src1(b->src_a) || is_src1(b->dst_a)) &&
            i < target_count(s) && blendable(target_format(s->color_fmt[i], NULL, NULL)))
            return 1;
    }
    return 0;
}

static uint32_t target_wmask(const VkPipeState *s, int i) {
    uint32_t m = s->chmask.mask >> (4 * i) & 0xf;
    if (s->fs && !(gfx_shader_outputs(s->fs, NULL) >> i & 1)) m = 0;
    if (s->fs) {
        uint32_t c = gfx_shader_omap(s->fs) >> (4 * i) & 0xf;
        if (c) m &= c;
    }
    if (i == 1 && s->fs && P.f_dual_src && uses_src1(s)) m = 0;
    return m;
}

uint32_t vk_pipeline_target_wmask(const VkPipeState *s, int i) { return target_wmask(s, i); }

static void gkey_fill(const VkPipeState *s, GKey *k) {
    memset(k, 0, sizeof *k);
    k->vs = s->vs, k->fs = s->fs, k->ts = s->ts;
    k->indep = (uint8_t)P.indep;
    k->n_color = (uint8_t)target_count(s);
    for (int i = 0; i < k->n_color; i++) k->color[i] = target_format(s->color_fmt[i], NULL, NULL);
    int d, st;
    VkFormat df = target_format(s->depth_fmt, &d, &st);
    k->depth = d ? df : VK_FORMAT_UNDEFINED;
    k->stencil = st ? df : VK_FORMAT_UNDEFINED;
    k->samples = s->samples > 1 ? s->samples : 1;
    k->topo_class = (uint8_t)topology_class(vk_pipeline_topology(s->prim));
    k->minus_w = s->depth_minus_w != 0;
    k->dsb = s->fs && P.f_dual_src && uses_src1(s);
    k->fbl = s->fs ? s->feedback : 0;
    if (!P.dyn_clamp) k->clamp = s->depth_clamp && P.f_depth_clamp;
    if (!P.dyn_poly) k->poly_mode = (uint8_t)polygon_mode(s->poly.mode);
    if (!P.dyn_a2c) k->a2c = s->ms.a2c != 0;
    if (P.f_logic_op && s->color.logic_op != NVN_LOGIC_OP_COPY) k->logic_op = (uint8_t)(s->color.logic_op + 1);
    for (int i = 0; i < k->n_color; i++) {
        int on = (s->color.blend_en >> i & 1) && blendable(k->color[i]);
        if (!P.dyn_blend_en && on) k->blend_en |= (uint8_t)(1u << i);
        if (!P.dyn_wmask) k->wmask |= target_wmask(s, i) << (4 * i);
        if (!P.dyn_blend_eq && on) {
            const blend_st *b = &s->blend[i];
            uint8_t *o = k->blend[i];
            o[0] = (uint8_t)blend_factor(b->src_rgb), o[1] = (uint8_t)blend_factor(b->dst_rgb), o[2] = (uint8_t)blend_op(b->eq_rgb);
            o[3] = (uint8_t)blend_factor(b->src_a), o[4] = (uint8_t)blend_factor(b->dst_a), o[5] = (uint8_t)blend_op(b->eq_a);
        }
    }
    if (is_mesh(s)) k->topo_class = s->prim == NVN_DRAW_PRIMITIVE_POINTS ? 0 : 2;
    if (!P.dyn_vi && !is_mesh(s)) {
        VtxInput v;
        vertex_input(s, &v);
        for (uint32_t i = 0; i < v.n_attr; i++) {
            uint32_t l = v.a[i].location;
            k->attr_fmt[l] = v.a[i].format, k->attr_off[l] = v.a[i].offset, k->attr_bind[l] = (uint8_t)v.a[i].binding;
        }
        for (uint32_t i = 0; i < v.n_bind; i++)
            k->rate[v.b[i].binding] = v.b[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? 2 : 1;
    }
}

static uint32_t vs_union_missing(const VkPipeState *s, GfxShaderIo fin[32]) {
    GfxShaderIo vout[32];
    uint32_t need = vs_outs(s->vs, s->fs, fin), have = gfx_shader_outputs(s->vs, vout), missing = 0;
    if (s->fs) {
        GfxShaderIo fi[32];
        uint32_t fneed = gfx_shader_inputs(s->fs, fi);
        for (int i = 0; i < 32; i++)
            if (fneed >> i & 1 && !fi[i].packed &&
                (!(need >> i & 1) || fin[i].type != fi[i].type || fin[i].comps < fi[i].comps)) {
                if (need >> i & 1 && fin[i].type != fi[i].type)
                    LOG_ONCE("vk_pipeline: varying %d read as type %d and %d by fragment shaders of one vertex shader", i, fin[i].type, fi[i].type);
                fin[i] = fi[i], need |= 1u << i;
            }
    }
    if (P.vs_outs_all)
        for (int i = 0; i < 32; i++)
            if (!(need >> i & 1)) fin[i] = (GfxShaderIo){GFX_TYPE_FLOAT, 4, 0}, need |= 1u << i;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && !(have >> i & 1)) missing |= 1u << i;
    return missing;
}


typedef struct {
    uint64_t vs, fs, fs_set1;
    uint64_t mat, mat_set1;
    uint32_t mat_pointer, mat_distance;
} WaterProfile;
static const WaterProfile water_profiles[] = {
    {0xf6d753852c47b0deull, 0xa39c3d09241441faull, 0x3d3a316ab4da3bbaull, 0xe73dfc0f4c556df8ull, 0xabfccea256824a78ull, 1965, 8490},
    {0x8d83e3705d790fedull, 0x3c20cb98dbaeb257ull, 0x216bd323c6b48ee2ull, 0x061318799d885bcfull, 0xb3cec6d0d78ac25aull, 1969, 8826},
};
enum { WATER_VERTEX, WATER_FRAGMENT, WATER_ABSORPTION };

static struct { uint64_t key; uint32_t *code; uint64_t words; int state; } water_mods[8];
static PlatMutex water_lock = PLAT_MUTEX_INIT;

static const uint32_t *water_module(uint64_t key, int kind, const WaterProfile *p, const GfxShader *src, uint64_t *words) {
    const uint32_t *code = NULL;
    plat_mutex_lock(&water_lock);
    int i = 0;
    while (i < 8 && water_mods[i].state && water_mods[i].key != key) i++;
    if (i < 8 && !water_mods[i].state && src) {
        uint64_t wn = 0, n = 0;
        const uint32_t *w = gfx_shader_spirv_pin(src, &n);
        uint32_t *c = NULL, *set1 = kind == WATER_VERTEX ? NULL : spirv_set(w, n, 1);
        if (kind == WATER_VERTEX) c = spirv_water_vertex(w, n, &wn);
        else if (set1 && fnv(FNV0, set1, (size_t)n * 4) == key)
            c = kind == WATER_FRAGMENT ? spirv_water_fragment(set1, n, &wn)
                                       : spirv_water_absorption(set1, n, p->mat_pointer, p->mat_distance, &wn);
        free(set1);
        gfx_shader_spirv_unpin(src);
        water_mods[i].key = key, water_mods[i].code = c, water_mods[i].words = wn, water_mods[i].state = c ? 1 : -1;
        static const char *const names[] = {"vertex stage of the coordinate pair", "fragment stage of the coordinate pair",
                                            "absorption material"};
        if (c) rt_log("vk_pipeline: water: the %s (module %016llx) is built from its corrected form "
                      "(NX2SARIA_WATER_PROJECTIVE)", names[kind], (unsigned long long)key);
        else rt_log("vk_pipeline: water: the %s (module %016llx) is not of the shape its correction expects; built as "
                    "it is", names[kind], (unsigned long long)key);
    }
    if (i < 8 && water_mods[i].state == 1) code = water_mods[i].code, *words = water_mods[i].words;
    plat_mutex_unlock(&water_lock);
    return code;
}

static int water_projective_pair(const VkPipeState *s) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2SARIA_WATER_PROJECTIVE");
    if (!on || !P.indep || !s->fs) return 0;
    uint64_t wn, fh = gfx_shader_spirv_fnv(s->fs);
    for (size_t i = 0; i < sizeof water_profiles / sizeof *water_profiles; i++) {
        const WaterProfile *p = &water_profiles[i];
        if (fh == p->mat) return water_module(p->mat_set1, WATER_ABSORPTION, p, s->fs, &wn) ? 2 : 0;
        if (fh != p->fs) continue;
        if (!s->vs || gfx_shader_spirv_fnv(s->vs) != p->vs) return 0;
        return water_module(p->vs, WATER_VERTEX, p, s->vs, &wn) && water_module(p->fs_set1, WATER_FRAGMENT, p, s->fs, &wn);
    }
    return 0;
}

static VkShaderModule water_pair_module(const uint32_t *w, uint64_t n, int early, int mode) {
    uint64_t words = 0;
    const uint32_t *code = mode ? water_module(fnv(FNV0, w, (size_t)n * 4), 0, NULL, NULL, &words) : NULL;
    if (mode == 1 && !code) LOG_ONCE("vk_pipeline: water: a coordinate pair's fragment module is not in its known form; built as it is");
    return code ? make_module(code, words, early) : make_module(w, n, early);
}

static const uint32_t *water_pair_vertex(const uint32_t *v, uint64_t n, int mode, uint64_t *words) {
    return mode == 1 && v ? water_module(fnv(FNV0, v, (size_t)n * 4), 0, NULL, NULL, words) : NULL;
}

static int water_mode(const VkPipeState *s, const GKey *k) {
    int mode = water_projective_pair(s);
    return mode == 1 && k->dsb ? 0 : mode;
}

static int key_lines(const GKey *k) { return P.wide_lines && k->topo_class == 1; }

void vk_pipeline_line_width(VkCommandBuffer cb, const VkPipeState *s, float width) {
    if (!P.wide_lines || is_mesh(s) || topology_class(vk_pipeline_topology(s->prim)) != 1) return;
    float w = width > 0 ? width : 1.0f;
    if (P.line_gran > 0) w = P.line_w[0] + P.line_gran * (float)(int)((w - P.line_w[0]) / P.line_gran + 0.5f);
    w = w < P.line_w[0] ? P.line_w[0] : w > P.line_w[1] ? P.line_w[1] : w;
    static float logged;
    if (w != 1.0f && w != logged) {
        logged = w;
        rt_log("vk_pipeline: lines drawn %g pixels wide (the game set %g; the device draws %g to %g in steps of %g)",
               (double)w, (double)width, (double)P.line_w[0], (double)P.line_w[1], (double)P.line_gran);
    }
    vkCmdSetLineWidth(cb, w);
}

static uint32_t task_off(const VkPipeState *s, const Layout *l, uint32_t fs_off) {
    return l->set1 ? binding_span(s->vs) - 1 : fs_off + (s->fs ? binding_span(s->fs) - 1 : 0);
}

static uint32_t mesh_dyn(VkDynamicState *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < P.n_dyn; i++)
        if (P.dyn[i] != VK_DYNAMIC_STATE_VERTEX_INPUT_EXT && P.dyn[i] != VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE &&
            P.dyn[i] != VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY && P.dyn[i] != VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE)
            out[n++] = P.dyn[i];
    if (vk_ctx()->has_vrs) out[n++] = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;
    return n;
}

static VkPipeline create_graphics(const VkPipeState *s, const GKey *k, const Layout *l, uint32_t fs_off) {
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    uint64_t vw, fw, vm_hash = 0, fm_hash = 0;
    uint64_t vw0 = 0, fw0 = 0;
    const uint32_t *vplain = gfx_shader_spirv_pin(s->vs, &vw0), *fplain = s->fs ? gfx_shader_spirv_pin(s->fs, &fw0) : NULL;
    const uint32_t *vcode = vplain;
    vw = vw0;
    uint32_t *vpatched = NULL, *fpatched = NULL;
    int mesh = is_mesh(s), emu = mesh && l->emu;
    int wmode = mesh || !l->set1 ? 0 : water_mode(s, k);
    uint64_t wn = 0;
    const uint32_t *vwater = water_pair_vertex(vcode, vw, wmode, &wn);
    if (vwater) vcode = vwater, vw = wn;
    if (emu) {
        uint32_t toff = fs_off + (s->fs ? binding_span(s->fs) - 1 : 0);
        const MeshEmuEnt *me = emu_get(s->vs, s->ts, l->layout, toff, 1);
        if (!me || !me->ok) {
            gfx_shader_spirv_unpin(s->vs);
            if (s->fs) gfx_shader_spirv_unpin(s->fs);
            return VK_NULL_HANDLE;
        }
        GfxShaderIo fin[32];
        uint8_t ty[32], cp[32];
        uint32_t need = s->fs ? gfx_shader_inputs(s->fs, fin) : 0;
        for (int i = 0; i < 32; i++) {
            ty[i] = need >> i & 1 && !fin[i].packed && fin[i].type <= GFX_TYPE_UINT ? (uint8_t)fin[i].type : 0;
            cp[i] = need >> i & 1 && !fin[i].packed && fin[i].comps ? (uint8_t)fin[i].comps : 4;
        }
        size_t vn = 0;
        vpatched = mesh_emu_vs(&me->mi, need, ty, cp, fplain, (size_t)fw0, &vn);
        if (!vpatched) {
            gfx_shader_spirv_unpin(s->vs);
            if (s->fs) gfx_shader_spirv_unpin(s->fs);
            return VK_NULL_HANDLE;
        }
        vcode = vpatched, vw = vn;
        mesh = 0;
    }
    VkShaderModule tm = VK_NULL_HANDLE;
    StageSg tsg = {0, 0};
    if (mesh && s->ts) {
        uint64_t tw;
        const uint32_t *tcode = gfx_shader_spirv_pin(s->ts, &tw);
        uint32_t toff = task_off(s, l, fs_off);
        uint32_t *t = tcode && toff ? spirv_rebind(tcode, tw, toff) : NULL;
        tsg = stage_sg(VK_SHADER_STAGE_TASK_BIT_EXT, tcode, tw);
        tm = make_module(t ? t : tcode, tw, 0);
        free(t);
        gfx_shader_spirv_unpin(s->ts);
    }
    if (l->set1 && !mesh) {
        GfxShaderIo fin[32];
        uint32_t missing = vs_union_missing(s, fin);
        if (missing) {
            uint64_t n2 = 0;
            vpatched = spirv_add_outputs(vcode, vw, missing, fin, &n2);
            if (!vpatched) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", missing);
            else vcode = vpatched, vw = n2;
        }
    }
    if (s->fs) {
        GfxShaderIo fin[32], vout[32];
        int points = 0;
        uint32_t need = gfx_shader_inputs(s->fs, fin), missing = 0,
                 have = mesh ? (vcode ? spirv_mesh_outputs(vcode, vw, &points) : ~0u) : emu ? ~0u : gfx_shader_outputs(s->vs, vout);
        for (int i = 0; i < 32; i++) {
            if (!(need >> i & 1) || fin[i].packed) continue;
            if (!(have >> i & 1)) missing |= 1u << i;
            else if (!mesh && !emu && vout[i].type != fin[i].type)
                LOG_ONCE("vk_pipeline: varying %d is type %d in the vertex stage, %d in the fragment stage", i, vout[i].type, fin[i].type);
        }
        if (missing && !l->set1 && !mesh) {
            uint64_t n2 = 0;
            vpatched = spirv_add_outputs(vcode, vw, missing, fin, &n2);
            if (!vpatched) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", missing);
            else vcode = vpatched, vw = n2;
        }
        const uint32_t *fcode = fplain;
        fw = fw0;
        if (missing && mesh && fcode) {
            uint64_t n2 = 0;
            uint32_t *f = spirv_fs_fill_inputs(fcode, fw, missing, points, &n2);
            if (f) fcode = fpatched = f, fw = n2;
            else LOG_ONCE("vk_pipeline: cannot fill fragment inputs the mesh stage lacks (mask %#x)", missing);
        }
        if (k->dsb) {
            uint64_t n2 = 0;
            uint32_t *d = spirv_dual_source(fcode, fw, &n2);
            if (d) free(fpatched), fcode = fpatched = d, fw = n2;
            else LOG_ONCE("vk_pipeline: SRC1 blend factors but the fragment shader writes no colour output 1");
        }
        if (l->set1 || fs_off) {
            uint32_t *r = !fcode ? NULL : l->set1 ? spirv_set(fcode, fw, 1) : spirv_rebind(fcode, fw, fs_off);
            free(fpatched);
            fcode = fpatched = r;
        }
        if (fcode) fm = water_pair_module(fcode, fw, !k->a2c, wmode), fm_hash = mm_hash;
    }
    vm = make_module(vcode, vw, 0), vm_hash = mm_hash;
    free(vpatched), free(fpatched);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!vm || (s->fs && !fm) || (mesh && s->ts && !tm)) goto out;

    VkPipelineShaderStageCreateInfo stages[3] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,
         .module = vm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_TASK_BIT_EXT, .module = tm, .pName = "main"}};
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rqv, rqf, rqt;
    if (!emu) pin_subgroup32(&stages[0], &rqv, vplain, vw0);
    if (mesh) stage_sg_apply(&stages[0], &rqv, stage_sg(VK_SHADER_STAGE_MESH_BIT_EXT, vplain, vw0));
    if (tm) stage_sg_apply(&stages[2], &rqt, tsg);
    uint32_t n_stages = 1;
    if (s->fs) n_stages = 2;
    if (tm) {
        if (!s->fs) stages[1] = stages[2];
        n_stages++;
    }
    if (s->fs) pin_subgroup32(&stages[1], &rqf, fplain, fw0);
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    uint32_t na = 0, nb = 0;
    if (!P.dyn_vi) {
        VtxInput v;
        vertex_input(s, &v);
        for (uint32_t i = 0; i < v.n_attr; i++) va[na++] = (VkVertexInputAttributeDescription){v.a[i].location, v.a[i].binding, v.a[i].format, v.a[i].offset};
        for (uint32_t i = 0; i < v.n_bind; i++) vb[nb++] = (VkVertexInputBindingDescription){v.b[i].binding, v.b[i].stride, v.b[i].inputRate};
    }
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    static const VkPrimitiveTopology class_base[4] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = class_base[k->topo_class]};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                                                            .negativeOneToOne = k->minus_w};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = &clip};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = P.pv_last ? &pvs : NULL,
                                                 .depthClampEnable = k->clamp, .polygonMode = (VkPolygonMode)k->poly_mode,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState cba[8];
    memset(cba, 0, sizeof cba);
    for (int i = 0; i < k->n_color; i++) {
        const uint8_t *b = k->blend[i];
        cba[i].blendEnable = k->blend_en >> i & 1;
        cba[i].srcColorBlendFactor = (VkBlendFactor)b[0], cba[i].dstColorBlendFactor = (VkBlendFactor)b[1];
        cba[i].colorBlendOp = (VkBlendOp)b[2], cba[i].srcAlphaBlendFactor = (VkBlendFactor)b[3];
        cba[i].dstAlphaBlendFactor = (VkBlendFactor)b[4], cba[i].alphaBlendOp = (VkBlendOp)b[5];
        cba[i].colorWriteMask = k->wmask >> (4 * i) & 0xf;
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .logicOpEnable = k->logic_op != 0,
                                              .logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY,
                                              .attachmentCount = k->n_color, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = P.n_dyn, .pDynamicStates = P.dyn};
    VkDynamicState mdyn[sizeof P.dyn / sizeof P.dyn[0] + 1];
    if (!mesh && key_lines(k)) {
        memcpy(mdyn, P.dyn, P.n_dyn * sizeof *mdyn);
        mdyn[P.n_dyn] = VK_DYNAMIC_STATE_LINE_WIDTH;
        dy.dynamicStateCount = P.n_dyn + 1, dy.pDynamicStates = mdyn;
    }
    if (mesh) dy.dynamicStateCount = mesh_dyn(mdyn), dy.pDynamicStates = mdyn;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->n_color,
                                        .pColorAttachmentFormats = (const VkFormat *)k->color,
                                        .depthAttachmentFormat = (VkFormat)k->depth, .stencilAttachmentFormat = (VkFormat)k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .stageCount = n_stages, .pStages = stages, .pVertexInputState = mesh ? NULL : &vi,
                                       .pInputAssemblyState = mesh ? NULL : &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
                                       .pDynamicState = &dy, .layout = l->layout, .flags = noopt_flag_for(mesh)};
    if (k->fbl & 1) pi.flags |= VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    if (k->fbl & 2) pi.flags |= VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkPipelineCreationFeedbackCreateInfo fci;
    VkPipelineCreationFeedback fb;
    pi.pNext = fb_chain(&fci, &fb, pi.pNext);
    uint64_t p0 = perf_begin();
    crash_note("a graphics pipeline", s->vs, s->fs, s->ts);
    VkResult r = pipe_create(0, &pi, &pipe);
    if (r == VK_ERROR_UNKNOWN && !(pi.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT)) {
        static int told;
        if (!told++) rt_log("vk_pipeline: the driver could not build a graphics pipeline (VK_ERROR_UNKNOWN): built again "
                            "without its optimiser (logged once)");
        pi.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        r = pipe_create(0, &pi, &pipe);
    }
    plat_crash_context(NULL);
    perf_end(PERF_PIPE, p0);
    if (r != VK_SUCCESS) {
        rt_log("vk_pipeline: vkCreateGraphicsPipelines failed (%d)", (int)r);
        pipe = VK_NULL_HANDLE;
    } else {
        perf_count(PERF_CNT_PIPE_BUILT);
        fb_note(FB_GRAPHICS, &fb);
        if (P.feedback >= 2) {
            uint64_t st = fnv(fnv(FNV0, cba, sizeof cba), k->color, sizeof k->color);
            if (!P.dyn_vi)
                st = fnv(fnv(fnv(fnv(st, k->attr_fmt, sizeof k->attr_fmt), k->attr_off, sizeof k->attr_off),
                             k->attr_bind, sizeof k->attr_bind), k->rate, sizeof k->rate);
            uint32_t bits[] = {(uint32_t)mesh, k->topo_class, k->minus_w, k->clamp, k->poly_mode, k->samples, k->a2c,
                               k->logic_op, k->n_color, k->fbl, k->dsb, l->set1 != 0, fs_off, pi.flags, k->depth, k->stencil};
            rt_log("vk_pipeline: graphics: modules %016llx %016llx, state %016llx (mesh %d, topology class %u, samples %u, "
                   "colors %u, feedback loop %u, depth format %u, stage-independent layout %d, flags 0x%x), %s, %.1f ms",
                   (unsigned long long)vm_hash, (unsigned long long)fm_hash, (unsigned long long)fnv(st, bits, sizeof bits),
                   mesh, (unsigned)k->topo_class, (unsigned)k->samples, (unsigned)k->n_color, (unsigned)k->fbl,
                   (unsigned)k->depth, l->set1 != 0, (unsigned)pi.flags,
                   pb_last ? "from pipeline-binaries.bin" : fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT ? "from pipeline-cache.bin" : "compiled",
                   (double)fb.duration / 1e6);
        }
    }
out:
    if (vm) vkDestroyShaderModule(P.device, vm, NULL);
    if (fm) vkDestroyShaderModule(P.device, fm, NULL);
    if (tm) vkDestroyShaderModule(P.device, tm, NULL);
    gfx_shader_spirv_unpin(s->vs);
    if (s->fs) gfx_shader_spirv_unpin(s->fs);
    return pipe;
}

static PlatCond work_cv = PLAT_COND_INIT, done_cv = PLAT_COND_INIT;
static PlatCond lib_cv = PLAT_COND_INIT;
#define LIB_BUCKETS 1024
enum { LIB_VI, LIB_PR, LIB_FS, LIB_FO };
typedef struct Lib { struct Lib *next; uint64_t hash; VkPipeline pipe; uint32_t size; uint8_t pre, used, building, pad;
                     struct LibJob *job; uint8_t key[]; } Lib;
static Lib *libs[LIB_BUCKETS];

typedef struct { uint32_t kind, topo_class, attr_fmt[32], attr_off[32]; uint8_t attr_bind[32], rate[17], pad[3]; } ViKey;
typedef struct { uint32_t kind, missing; const GfxShader *vs; uint8_t clamp, poly_mode, minus_w, water_projective;
                 uint8_t lines;
                 const GfxShader *ts;
                 GfxShaderIo fin[32]; } PrKey;
typedef struct { uint32_t kind; uint8_t dsb, samples, a2c, water_projective; const GfxShader *fs;
                 uint32_t fill; uint8_t points, vrs, pad[2]; } FsKey;
typedef struct { uint32_t kind; uint8_t n_color, samples, a2c, logic_op, blend_en, fbl, pad[2]; uint32_t color[8], depth, stencil, wmask;
                 uint8_t blend[8][6]; } FoKey;

static uint64_t qpc(void) { return plat_now_ns(); }
static double ms_since(uint64_t t0) { return (double)(qpc() - t0) / 1e6; }

static VkPipelineLayout half_layout(Layout *l, int h) {
    if (l->half[h]) return l->half[h];
    VkDescriptorSetLayout sets[3] = {h ? VK_NULL_HANDLE : l->set, h ? l->set1 : VK_NULL_HANDLE, vk_pool_set_layout()};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 3,
                                     .pSetLayouts = sets};
    if (vkCreatePipelineLayout(P.device, &pi, NULL, &l->half[h]) != VK_SUCCESS) l->half[h] = VK_NULL_HANDLE;
    return l->half[h];
}

static VkPipeline lib_create(int kind, const VkPipeState *s, const GKey *k, VkPipelineLayout lay, const void *key) {
    static const VkGraphicsPipelineLibraryFlagsEXT part[4] = {
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .flags = part[kind]};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = &gl};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = P.n_dyn, .pDynamicStates = P.dyn};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | noopt_flag(), .pDynamicState = &dy};
    VkDynamicState ldyn[sizeof P.dyn / sizeof P.dyn[0] + 1];
    int mesh = is_mesh(s);
    if (kind == LIB_PR && mesh) {
        dy.dynamicStateCount = mesh_dyn(ldyn), dy.pDynamicStates = ldyn;
        pi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | noopt_flag_for(1);
    } else if (kind == LIB_PR && ((const PrKey *)key)->lines) {
        memcpy(ldyn, P.dyn, P.n_dyn * sizeof *ldyn);
        ldyn[P.n_dyn] = VK_DYNAMIC_STATE_LINE_WIDTH;
        dy.dynamicStateCount = P.n_dyn + 1, dy.pDynamicStates = ldyn;
    } else if (kind == LIB_FS && ((const FsKey *)key)->vrs) {
        dy.dynamicStateCount = mesh_dyn(ldyn), dy.pDynamicStates = ldyn;
    }
    VkShaderModule m = VK_NULL_HANDLE, tm = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pName = "main"};
    VkPipelineShaderStageCreateInfo mst[2];
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rq, rqt;
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    static const VkPrimitiveTopology class_base[4] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = class_base[k->topo_class]};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
                                                            .negativeOneToOne = k->minus_w};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .pNext = &clip};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .pNext = P.pv_last ? &pvs : NULL, .depthClampEnable = k->clamp,
                                                 .polygonMode = (VkPolygonMode)k->poly_mode, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .maxDepthBounds = 1.0f};
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    uint32_t *patched = NULL;
    VkPipeline pipe = VK_NULL_HANDLE;
    const GfxShader *pinned[2] = {NULL, NULL};
    if (kind == LIB_VI) {
        if (!P.dyn_vi) {
            VtxInput v;
            vertex_input(s, &v);
            for (uint32_t i = 0; i < v.n_attr; i++)
                va[i] = (VkVertexInputAttributeDescription){v.a[i].location, v.a[i].binding, v.a[i].format, v.a[i].offset};
            for (uint32_t i = 0; i < v.n_bind; i++)
                vb[i] = (VkVertexInputBindingDescription){v.b[i].binding, v.b[i].stride, v.b[i].inputRate};
            vi.vertexAttributeDescriptionCount = v.n_attr, vi.pVertexAttributeDescriptions = va;
            vi.vertexBindingDescriptionCount = v.n_bind, vi.pVertexBindingDescriptions = vb;
        }
        pi.pVertexInputState = &vi, pi.pInputAssemblyState = &ia;
    } else if (kind == LIB_PR && mesh) {
        uint64_t vw, tw = 0;
        const uint32_t *vcode = gfx_shader_spirv_pin(pinned[0] = s->vs, &vw);
        if (!vcode || !(m = make_module(vcode, vw, 0))) goto out;
        mst[0] = st, mst[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT, mst[0].module = m;
        pin_subgroup32(&mst[0], &rq, vcode, vw);
        stage_sg_apply(&mst[0], &rq, stage_sg(VK_SHADER_STAGE_MESH_BIT_EXT, vcode, vw));
        pi.stageCount = 1;
        if (s->ts) {
            const uint32_t *tcode = gfx_shader_spirv_pin(pinned[1] = s->ts, &tw);
            uint32_t toff = binding_span(s->vs) - 1;
            patched = tcode && toff ? spirv_rebind(tcode, tw, toff) : NULL;
            if (!tcode || !(tm = make_module(patched ? patched : tcode, tw, 0))) goto out;
            mst[1] = st, mst[1].stage = VK_SHADER_STAGE_TASK_BIT_EXT, mst[1].module = tm;
            stage_sg_apply(&mst[1], &rqt, stage_sg(VK_SHADER_STAGE_TASK_BIT_EXT, tcode, tw));
            pi.stageCount = 2;
        }
        pi.pStages = mst, pi.pViewportState = &vp, pi.pRasterizationState = &rs;
        pi.layout = lay;
    } else if (kind == LIB_PR) {
        const PrKey *pk = key;
        uint64_t vw, sw, n2 = 0;
        const uint32_t *vcode = gfx_shader_spirv_pin(pinned[0] = s->vs, &vw), *scode = vcode;
        sw = vw;
        uint64_t wn = 0;
        const uint32_t *vwater = water_pair_vertex(vcode, vw, pk->water_projective, &wn);
        if (vwater) vcode = vwater, vw = wn;
        if (pk->missing && (patched = spirv_add_outputs(vcode, vw, pk->missing, pk->fin, &n2))) vcode = patched, vw = n2;
        else if (pk->missing) LOG_ONCE("vk_pipeline: cannot add missing vertex outputs (mask %#x)", pk->missing);
        if (!(m = make_module(vcode, vw, 0))) goto out;
        st.stage = VK_SHADER_STAGE_VERTEX_BIT, st.module = m;
        pin_subgroup32(&st, &rq, scode, sw);
        pi.stageCount = 1, pi.pStages = &st, pi.pViewportState = &vp, pi.pRasterizationState = &rs;
        pi.layout = lay;
    } else if (kind == LIB_FS) {
        if (s->fs) {
            const FsKey *fk = key;
            uint64_t fw, sw, n2 = 0;
            const uint32_t *fcode = gfx_shader_spirv_pin(pinned[0] = s->fs, &fw), *scode = fcode;
            sw = fw;
            uint32_t *d = NULL, *filled = NULL;
            if (fk->fill && fcode) {
                if ((filled = spirv_fs_fill_inputs(fcode, fw, fk->fill, fk->points, &n2))) fcode = filled, fw = n2;
                else LOG_ONCE("vk_pipeline: cannot fill fragment inputs the mesh stage lacks (mask %#x)", fk->fill);
            }
            if (k->dsb && fcode) {
                if ((d = spirv_dual_source(fcode, fw, &n2))) fcode = d, fw = n2;
                else LOG_ONCE("vk_pipeline: SRC1 blend factors but the fragment shader writes no colour output 1");
            }
            patched = fcode ? spirv_set(fcode, fw, 1) : NULL;
            free(d), free(filled);
            if (!patched || !(m = water_pair_module(patched, fw, !k->a2c, fk->water_projective))) goto out;
            st.stage = VK_SHADER_STAGE_FRAGMENT_BIT, st.module = m;
            pin_subgroup32(&st, &rq, scode, sw);
            pi.stageCount = 1, pi.pStages = &st;
        }
        pi.pMultisampleState = &ms, pi.pDepthStencilState = &ds;
        pi.layout = lay;
    } else {
        memset(cba, 0, sizeof cba);
        for (int i = 0; i < k->n_color; i++) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = k->blend_en >> i & 1;
            cba[i].srcColorBlendFactor = (VkBlendFactor)b[0], cba[i].dstColorBlendFactor = (VkBlendFactor)b[1];
            cba[i].colorBlendOp = (VkBlendOp)b[2], cba[i].srcAlphaBlendFactor = (VkBlendFactor)b[3];
            cba[i].dstAlphaBlendFactor = (VkBlendFactor)b[4], cba[i].alphaBlendOp = (VkBlendOp)b[5];
            cba[i].colorWriteMask = k->wmask >> (4 * i) & 0xf;
        }
        cb.logicOpEnable = k->logic_op != 0, cb.logicOp = k->logic_op ? (VkLogicOp)(k->logic_op - 1) : VK_LOGIC_OP_COPY;
        cb.attachmentCount = k->n_color, cb.pAttachments = cba;
        ri.colorAttachmentCount = k->n_color, ri.pColorAttachmentFormats = (const VkFormat *)k->color;
        ri.depthAttachmentFormat = (VkFormat)k->depth, ri.stencilAttachmentFormat = (VkFormat)k->stencil;
        pi.pColorBlendState = &cb, pi.pMultisampleState = &ms;
        const FoKey *ok = key;
        if (ok->fbl & 1) pi.flags |= VK_PIPELINE_CREATE_COLOR_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
        if (ok->fbl & 2) pi.flags |= VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    }
    VkPipelineCreationFeedbackCreateInfo fci;
    VkPipelineCreationFeedback fb;
    pi.pNext = fb_chain(&fci, &fb, pi.pNext);
    static const char *const kind_name[4] = {"a vertex input library", "a pre-rasterization library",
                                             "a fragment shader library", "a fragment output library"};
    crash_note(kind == LIB_PR && mesh ? "a pre-rasterization library of task and mesh stages" : kind_name[kind], s->vs,
               s->fs, mesh ? s->ts : NULL);
    if (pipe_create(1, &pi, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    plat_crash_context(NULL);
    if (pipe && (kind == LIB_PR || kind == LIB_FS)) fb_note(FB_LIBRARY, &fb);
out:
    if (m) vkDestroyShaderModule(P.device, m, NULL);
    if (tm) vkDestroyShaderModule(P.device, tm, NULL);
    free(patched);
    for (int i = 0; i < 2; i++)
        if (pinned[i]) gfx_shader_spirv_unpin(pinned[i]);
    return pipe;
}

static Lib *lib_find(uint64_t h, const void *key, uint32_t size) {
    for (Lib *b = libs[h % LIB_BUCKETS]; b; b = b->next)
        if (b->hash == h && b->size == size && !memcmp(b->key, key, size)) return b;
    return NULL;
}

static VkPipeline lib_put(uint64_t h, const void *key, uint32_t size, VkPipeline pipe) {
    Lib *b = lib_find(h, key, size);
    if (b && b->building) {
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return VK_NULL_HANDLE;
    }
    if (b) {
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return b->pipe;
    }
    if (!(b = calloc(1, sizeof *b + size))) return pipe;
    b->hash = h, b->size = size, b->pipe = pipe;
    memcpy(b->key, key, size);
    b->next = libs[h % LIB_BUCKETS];
    libs[h % LIB_BUCKETS] = b;
    return pipe;
}

typedef struct LibJob { struct LibJob *next; Lib *b; int kind; Layout *l; VkPipeState s; GKey k; Pipe *p; } LibJob;
static struct {
    LibJob *head, *tail;
    int idle;
    int running;
    int on;
    int lib_workers;
    uint64_t waits, queued, by_worker, stolen, timeouts, links;
} U;

static Lib *lib_new(uint64_t h, const void *key, uint32_t size) {
    Lib *b = calloc(1, sizeof *b + size);
    if (!b) return NULL;
    b->hash = h, b->size = size, b->building = 1;
    memcpy(b->key, key, size);
    b->next = libs[h % LIB_BUCKETS];
    libs[h % LIB_BUCKETS] = b;
    return b;
}

static VkPipeline lib_build(Lib *b, int kind, const VkPipeState *s, const GKey *k, Layout *l) {
    VkPipelineLayout lay = kind == LIB_PR ? half_layout(l, 0) : kind == LIB_FS ? half_layout(l, 1) : VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (lay || kind == LIB_VI || kind == LIB_FO) {
        plat_mutex_unlock(&P.lock);
        uint64_t t0 = qpc();
        pipe = lib_create(kind, s, k, lay, b->key);
        double ms = ms_since(t0);
        plat_mutex_lock(&P.lock);
        P.g.lib_n[kind]++, P.g.lib_ms[kind] += ms;
        if (ms > P.g.lib_max[kind]) P.g.lib_max[kind] = ms;
        int mesh = is_mesh(s) && (kind == LIB_PR || kind == LIB_FS);
        if (mesh) {
            int i = kind == LIB_FS;
            P.g.m_lib_n[i]++, P.g.m_lib_ms[i] += ms;
            if (ms > P.g.m_lib_max[i]) P.g.m_lib_max[i] = ms;
        }
        int added = kind == LIB_PR && ((const PrKey *)(const void *)b->key)->missing;
        if (added) P.g.pr_added++, P.g.pr_added_ms += ms;
        if (ms >= 25) P.g.lib_slow[kind]++, P.g.lib_slow_ms[kind] += ms;
        if (ms >= 25 && (kind == LIB_PR || kind == LIB_FS) && P.g.lib_logged < 32) {
            const GfxShader *sh = kind == LIB_PR ? s->vs : s->fs;
            const uint8_t *id = sh ? gfx_shader_input_id(sh) : NULL;
            P.g.lib_logged++;
            rt_log("vk_pipeline: gpl: a %s library took %.0f ms: shader %02x%02x%02x%02x, %s%s",
                   kind == LIB_FS ? (mesh ? "fragment shader (of a mesh pipeline)" : "fragment shader")
                                  : mesh ? "pre-rasterization (task and mesh stages)" : "pre-rasterization",
                   ms, id ? id[0] : 0, id ? id[1] : 0,
                   id ? id[2] : 0, id ? id[3] : 0, origin_name(sh),
                   added ? "; outputs were added for its fragment shader, a module the preparation did not build"
                   : sh && gfx_shader_origin(sh) == 2 ? "; nothing was prepared from a shader first translated now"
                   : "; the driver's cache did not have it, or the build waited behind others in the driver");
        }
    }
    if (!pipe && ++P.g.lib_fail <= 8) rt_log("vk_pipeline: gpl: stage library %d not built", kind);
    b->pipe = pipe, b->building = 0;
    plat_cond_broadcast(&done_cv);
    return pipe;
}

static void lib_steal(Lib *b) {
    LibJob *j = b->job, **pp = &U.head, *prev = NULL;
    while (*pp && *pp != j) prev = *pp, pp = &(*pp)->next;
    if (*pp) *pp = j->next;
    if (U.tail == j) U.tail = prev;
    b->job = NULL;
    lib_build(b, j->kind, &j->s, &j->k, j->l);
    free(j);
}

static VkPipeline lib_get(int kind, const VkPipeState *s, const GKey *k, Layout *l, const void *key, uint32_t size,
                          int build, int *miss) {
    uint64_t h = key_hash(key, size);
    Lib *have = lib_find(h, key, size);
    while (have && have->building) {
        if (!build) {
            *miss = 1;
            return VK_NULL_HANDLE;
        }
        if (have->job) U.stolen++, lib_steal(have);
        else plat_cond_wait(&done_cv, &P.lock);
    }
    if (have) {
        if (have->pre && !have->used) have->used = 1, P.g.pre_used[kind]++;
        return have->pipe;
    }
    VkPipelineLayout lay = kind == LIB_PR ? half_layout(l, 0) : kind == LIB_FS ? half_layout(l, 1) : VK_NULL_HANDLE;
    if (!build || ((kind == LIB_PR || kind == LIB_FS) && !lay)) {
        *miss = 1;
        return VK_NULL_HANDLE;
    }
    Lib *b = lib_new(h, key, size);
    if (b) return lib_build(b, kind, s, k, l);
    plat_mutex_unlock(&P.lock);
    uint64_t t0 = qpc();
    VkPipeline pipe = lib_create(kind, s, k, lay, key);
    double ms = ms_since(t0);
    plat_mutex_lock(&P.lock);
    if ((have = lib_find(h, key, size)) && !have->building) {
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        return have->pipe;
    }
    P.g.lib_n[kind]++, P.g.lib_ms[kind] += ms;
    if (ms > P.g.lib_max[kind]) P.g.lib_max[kind] = ms;
    if (!pipe && ++P.g.lib_fail <= 8) rt_log("vk_pipeline: gpl: stage library %d not built", kind);
    return lib_put(h, key, size, pipe);
}

static void libs_destroy(void) {
    for (int i = 0; i < LIB_BUCKETS; i++) {
        for (Lib *b = libs[i], *n; b; b = n) {
            n = b->next;
            if (b->pipe) vkDestroyPipeline(P.device, b->pipe, NULL);
            free(b);
        }
        libs[i] = NULL;
    }
}

static void lib_keys(const VkPipeState *s, const GKey *k, ViKey *v, PrKey *pk, FsKey *fk, FoKey *ok) {
    int mesh = is_mesh(s);
    memset(v, 0, sizeof *v);
    v->kind = LIB_VI, v->topo_class = k->topo_class;
    memcpy(v->attr_fmt, k->attr_fmt, sizeof v->attr_fmt), memcpy(v->attr_off, k->attr_off, sizeof v->attr_off);
    memcpy(v->attr_bind, k->attr_bind, sizeof v->attr_bind), memcpy(v->rate, k->rate, sizeof v->rate);
    memset(pk, 0, sizeof *pk);
    GfxShaderIo fin[32];
    pk->kind = LIB_PR, pk->vs = s->vs, pk->clamp = k->clamp, pk->poly_mode = k->poly_mode, pk->minus_w = k->minus_w;
    pk->lines = (uint8_t)key_lines(k);
    if (mesh) pk->ts = s->ts;
    else pk->missing = vs_union_missing(s, fin);
    for (int i = 0; i < 32; i++)
        if (pk->missing >> i & 1) pk->fin[i] = fin[i];
    memset(fk, 0, sizeof *fk);
    fk->kind = LIB_FS, fk->fs = s->fs, fk->dsb = k->dsb, fk->samples = k->samples, fk->a2c = k->a2c;
    if (mesh) {
        int points = 0;
        fk->fill = mesh_fill(s->vs, s->fs, &points);
        fk->points = (uint8_t)points, fk->vrs = vk_ctx()->has_vrs != 0;
    }
    pk->water_projective = fk->water_projective = mesh ? 0 : (uint8_t)water_mode(s, k);
    memset(ok, 0, sizeof *ok);
    ok->kind = LIB_FO, ok->n_color = k->n_color, ok->samples = k->samples, ok->a2c = k->a2c, ok->logic_op = k->logic_op;
    ok->blend_en = k->blend_en, ok->fbl = k->fbl, ok->depth = k->depth, ok->stencil = k->stencil, ok->wmask = k->wmask;
    memcpy(ok->color, k->color, sizeof ok->color), memcpy(ok->blend, k->blend, sizeof ok->blend);
}

static int gpl_feedback(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_GPL_FEEDBACK");
        if (!on) rt_log("vk_pipeline: gpl: pipelines with a feedback loop get no fast link (NX2SARIA_GPL_FEEDBACK=0)");
    }
    return on;
}

static VkPipeline gpl_link(const VkPipeState *s, const GKey *k, const Layout *lc, int build, int *final) {
    Layout *l = (Layout *)lc;
    *final = 1;
    if (!l->set1 || (k->fbl && !gpl_feedback())) return VK_NULL_HANDLE;
    int mesh = is_mesh(s);
    uint64_t t0 = qpc();
    ViKey vk_;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &vk_, &pk, &fk, &ok);
    int miss = 0;
    VkPipeline lib[4];
    lib[1] = lib_get(LIB_PR, s, k, l, &pk, sizeof pk, build, &miss);
    lib[2] = miss ? VK_NULL_HANDLE : lib_get(LIB_FS, s, k, l, &fk, sizeof fk, build, &miss);
    if (miss) {
        if (!build) *final = 0, P.g.defer++;
        return VK_NULL_HANDLE;
    }
    lib[0] = mesh ? VK_NULL_HANDLE : lib_get(LIB_VI, s, k, l, &vk_, sizeof vk_, 1, &miss);
    lib[3] = lib_get(LIB_FO, s, k, l, &ok, sizeof ok, 1, &miss);
    VkPipeline pipe = VK_NULL_HANDLE;
    double link_ms = 0;
    if ((mesh || lib[0]) && lib[1] && lib[2] && lib[3]) {
        VkPipelineLibraryCreateInfoKHR li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                                             .libraryCount = mesh ? 3 : 4, .pLibraries = mesh ? lib + 1 : lib};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &li,
                                           .layout = l->layout};
        if (build) plat_mutex_unlock(&P.lock);
        uint64_t t1 = qpc();
        char what[160];
        snprintf(what, sizeof what,
                 "a library link (%sfragment shader %s, %u colour targets, depth format %u, stencil format %u, %u samples, "
                 "vertex outputs added %#x)", mesh ? "task and mesh stages, " : "", s->fs ? "yes" : "none", k->n_color,
                 k->depth, k->stencil, k->samples, pk.missing);
        crash_note(what, s->vs, s->fs, mesh ? s->ts : NULL);
        if (vkCreateGraphicsPipelines(P.device, VK_NULL_HANDLE, 1, &pi, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
        plat_crash_context(NULL);
        double ms = ms_since(t1);
        if (build) plat_mutex_lock(&P.lock);
        P.g.link_ms += ms, link_ms = ms;
        if (ms > P.g.link_max) P.g.link_max = ms;
    }
    if (pipe) P.g.links++, P.g.fb_links += k->fbl != 0;
    else if (!P.g.link_fail++) rt_log("vk_pipeline: gpl: fast link failed; such draws wait or skip as without GPL");
    if (pipe && mesh) {
        P.g.m_links++, P.g.m_link_ms += link_ms;
        if (link_ms > P.g.m_link_max) P.g.m_link_max = link_ms;
    }
    double total = ms_since(t0);
    if (!build && total > 5.0 && P.g.slow++ < 16)
        rt_log("vk_pipeline: gpl: fast link on the drawing thread took %.1f ms", total);
    return pipe;
}

typedef struct Job {
    struct Job *next, *prev;
    Pipe *p;
    const Layout *l;
    uint32_t fs_off;
    uint8_t low;
    uint8_t prebuild, demanded;
    uint8_t rested;
    int worker;
    VkPipeState s;
    GKey k;
} Job;

#define MAX_WORKERS 4
#define JOB_ON_DRAW (-2)

static struct {
    int on;
    int threads;
    PlatThread *thread[MAX_WORKERS];
    uint8_t prio_low[MAX_WORKERS];
    Job *head, *tail;
    Job *low, *low_tail;
    uint32_t inflight;
    int low_running;
    int gate_shut;
    uint64_t first, taken;
    PlatCond idle;
} A = {.idle = PLAT_COND_INIT};

#define KEYS_VERSION 4
typedef struct { char magic[8]; uint32_t version, rec_size; } KeysHeader;
typedef struct {
    uint8_t vs[32], fs[32];
    uint32_t has_fs, pad;
    VkPipeState s;
} KeyRecV3;
typedef struct {
    uint8_t vs[32], fs[32], ts[32];
    uint32_t has_fs, has_ts;
    VkPipeState s;
} KeyRec;
static struct {
    char path[PLAT_PATH_MAX * 2];
    int header_ok;
    uint64_t *seen;
    uint32_t seen_cap, seen_n;
    KeyRec *pend;
    uint32_t pend_n, pend_cap;
    uint8_t *file;
    size_t file_n;
    size_t own_n;
    PlatMutex io;
    int loading;
    int warm;
    uint32_t left;
    uint32_t built, failed, promoted;
    uint32_t gate_ended;
    double t0;
    int prep;
    double until;
} K = {.io = PLAT_MUTEX_INIT};

static uint64_t rec_hash(const KeyRec *r) {
    uint64_t h = fnv(FNV0, r, sizeof *r);
    return h ? h : 1;
}

static int seen_add(uint64_t h) {
    if ((K.seen_n + 1) * 2 > K.seen_cap) {
        uint32_t cap = K.seen_cap ? K.seen_cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        if (!t) return 1;
        for (uint32_t i = 0; i < K.seen_cap; i++) {
            if (!K.seen[i]) continue;
            uint32_t j = (uint32_t)K.seen[i] & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = K.seen[i];
        }
        free(K.seen);
        K.seen = t, K.seen_cap = cap;
    }
    uint32_t j = (uint32_t)h & (K.seen_cap - 1);
    while (K.seen[j]) {
        if (K.seen[j] == h) return 0;
        j = (j + 1) & (K.seen_cap - 1);
    }
    K.seen[j] = h;
    K.seen_n++;
    return 1;
}

static void rec_fill(KeyRec *r, const VkPipeState *s) {
    memset(r, 0, sizeof *r);
    memcpy(r->vs, gfx_shader_input_id(s->vs), 32);
    if (s->fs) memcpy(r->fs, gfx_shader_input_id(s->fs), 32), r->has_fs = 1;
    if (s->ts) memcpy(r->ts, gfx_shader_input_id(s->ts), 32), r->has_ts = 1;
    r->s = *s;
    r->s.vs = r->s.fs = r->s.ts = NULL;
    r->s.prim_restart = 0;
}

static void keys_note(const VkPipeState *s) {
    if (!A.on || !K.path[0]) return;
    static const uint8_t none[32];
    if (s->fs && !memcmp(gfx_shader_input_id(s->fs), none, 32)) return;
    if (s->ts && !memcmp(gfx_shader_input_id(s->ts), none, 32)) return;
    if (!memcmp(gfx_shader_input_id(s->vs), none, 32)) return;
    KeyRec r;
    rec_fill(&r, s);
    if (!seen_add(rec_hash(&r))) return;
    if (K.pend_n == K.pend_cap) {
        uint32_t cap = K.pend_cap ? K.pend_cap * 2 : 64;
        KeyRec *t = realloc(K.pend, cap * sizeof *t);
        if (!t) return;
        K.pend = t, K.pend_cap = cap;
    }
    K.pend[K.pend_n++] = r;
}

static void keys_flush(void) {
    if (!K.path[0]) return;
    plat_mutex_lock(&K.io);
    plat_mutex_lock(&P.lock);
    KeyRec *r = K.pend;
    uint32_t n = K.pend_n;
    K.pend = NULL, K.pend_n = K.pend_cap = 0;
    plat_mutex_unlock(&P.lock);
    if (n) {
        FILE *f = fopen(K.path, K.header_ok ? "ab" : "wb");
        int ok = f != NULL;
        if (ok && !K.header_ok) {
            KeysHeader h = {{'N', 'X', '2', 'N', 'P', 'K', 'E', 'Y'}, KEYS_VERSION, sizeof(KeyRec)};
            ok = fwrite(&h, sizeof h, 1, f) == 1;
        }
        if (ok) ok = fwrite(r, sizeof *r, n, f) == n;
        if (ok) ok = plat_file_sync(f);
        if (f) ok = !fclose(f) && ok;
        if (ok) K.header_ok = 1;
        else rt_log("vk_pipeline: cannot write %s", K.path);
    }
    free(r);
    plat_mutex_unlock(&K.io);
}

static void keys_open_seed(void);

static void keys_open_own(void);
static void keys_open(void) {
    keys_open_own();
    K.own_n = K.file_n;
    if (K.path[0]) keys_open_seed();
}

static int seen_has(uint64_t h) {
    if (!K.seen_cap) return 0;
    for (uint32_t j = (uint32_t)h & (K.seen_cap - 1); K.seen[j]; j = (j + 1) & (K.seen_cap - 1))
        if (K.seen[j] == h) return 1;
    return 0;
}

static void keys_open_seed(void) {
    if (!cfg_bool("NX2SARIA_PIPE_SEED")) return;
    char path[sizeof K.path];
    snprintf(path, sizeof path, "%s", K.path);
    char *slash = strrchr(path, '\\'), *fslash = strrchr(path, '/');
    if (fslash > slash) slash = fslash;
    snprintf(slash ? slash + 1 : path, sizeof path - (size_t)(slash ? slash + 1 - path : 0), "seed-pipeline-keys.bin");
    FILE *f = fopen(path, "rb");
    if (!f) return;
    KeysHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = len > (long)sizeof h ? ((size_t)len - sizeof h) / sizeof(KeyRec) : 0;
    KeyRec *seed = NULL;
    int ok = len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "NX2NPKEY", 8) &&
             h.version == KEYS_VERSION && h.rec_size == sizeof(KeyRec) && n && (seed = malloc(n * sizeof *seed)) &&
             fread(seed, sizeof *seed, n, f) == n;
    fclose(f);
    if (!ok) {
        if (n) rt_log("vk_pipeline: %s is a list for another runtime version (its record layout differs); left out", path);
        free(seed);
        return;
    }
    KeyRec *all = realloc(K.file, (K.file_n + n) * sizeof(KeyRec));
    if (!all) {
        free(seed);
        return;
    }
    K.file = (uint8_t *)all;
    size_t added = 0;
    for (size_t i = 0; i < n; i++)
        if (!seen_has(rec_hash(&seed[i]))) all[K.file_n + added++] = seed[i];
    K.file_n += added;
    free(seed);
    rt_log("vk_pipeline: %s: %zu listed pipelines, %zu of them not recorded by this install; built with its own "
           "(NX2SARIA_PIPE_SEED)", path, n, added);
}

static void keys_open_own(void) {
    if (!A.on || !P.cache_path[0]) return;
    snprintf(K.path, sizeof K.path, "%s", P.cache_path);
    char *slash = strrchr(K.path, '\\'), *fslash = strrchr(K.path, '/');
    if (fslash > slash) slash = fslash;
    snprintf(slash ? slash + 1 : K.path, sizeof K.path - (size_t)(slash ? slash + 1 - K.path : 0), "pipeline-keys.bin");
    gfx_shader_record_at(K.path);
    FILE *f = fopen(K.path, "rb");
    if (!f) return;
    KeysHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    int head = len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "NX2NPKEY", 8);
    if (head && h.version == KEYS_VERSION && h.rec_size == sizeof(KeyRec)) {
        K.header_ok = 1;
        size_t n = ((size_t)len - sizeof h) / sizeof(KeyRec);
        if (n && (K.file = malloc(n * sizeof(KeyRec))) && fread(K.file, sizeof(KeyRec), n, f) == n) K.file_n = n;
        else free(K.file), K.file = NULL;
        for (size_t i = 0; i < K.file_n; i++) seen_add(rec_hash((const KeyRec *)K.file + i));
        if (len != (long)(sizeof h + n * sizeof(KeyRec)))
            K.header_ok = 0;
    } else if (head && h.version == 3 && h.rec_size == sizeof(KeyRecV3)) {
        size_t n = ((size_t)len - sizeof h) / sizeof(KeyRecV3);
        KeyRecV3 *old = n ? malloc(n * sizeof *old) : NULL;
        if (old && fread(old, sizeof *old, n, f) == n && (K.file = calloc(n, sizeof(KeyRec)))) {
            KeyRec *r = (KeyRec *)K.file;
            for (size_t i = 0; i < n; i++) {
                memcpy(r[i].vs, old[i].vs, 32), memcpy(r[i].fs, old[i].fs, 32);
                r[i].has_fs = old[i].has_fs, r[i].s = old[i].s;
                seen_add(rec_hash(&r[i]));
            }
            K.file_n = n;
            rt_log("vk_pipeline: %s: %zu version 3 records converted; the file is rewritten", K.path, n);
        }
        free(old);
    } else {
        rt_log("vk_pipeline: %s has another format or version; it is rewritten", K.path);
    }
    fclose(f);
    if (!K.header_ok && K.file_n) {
        KeyRec *t = malloc(K.file_n * sizeof *t);
        if (t) memcpy(t, K.file, K.file_n * sizeof *t), K.pend = t, K.pend_n = K.pend_cap = (uint32_t)K.file_n;
    }
}

static double now_ms(void) { return (double)plat_now_ns() / 1e6; }

#define READY_SLOW_MS 50.0
#define READY_LOGS 48
static void ready_note(Pipe *p, int fast, const VkPipeState *s) {
    if (fast && (p->job ? !p->job->prebuild : 1)) keys_note(p->job ? &p->job->s : s);
    if (!p->t_miss) return;
    double ms = (double)(plat_now_ns() - p->t_miss) / 1e6;
    p->t_miss = 0;
    P.g.ready_n[ms < 4 ? 0 : ms < 8 ? 1 : ms < 17 ? 2 : ms < 50 ? 3 : ms < 150 ? 4 : 5]++;
    P.g.ready_ms += ms;
    if (ms > P.g.ready_max) P.g.ready_max = ms;
    P.g.ready_nofast += !fast;
    const Job *j = p->job;
    if (!j) return;
    int mesh = is_mesh(&j->s);
    int fresh = gfx_shader_origin(j->s.vs) == 2 || (j->s.fs && gfx_shader_origin(j->s.fs) == 2) ||
                (j->s.ts && gfx_shader_origin(j->s.ts) == 2);
    P.g.ready_mesh += (uint64_t)mesh, P.g.ready_recorded += j->prebuild, P.g.ready_fresh += (uint64_t)fresh;
    stutter_ready(mesh, gfx_shader_input_id(j->s.vs), j->s.fs ? gfx_shader_input_id(j->s.fs) : NULL, ms, j->prebuild,
                  fresh, fast);
    if (mesh) {
        P.g.m_ready_fast += fast != 0, P.g.m_ready_ms += ms;
        if (ms > P.g.m_ready_max) P.g.m_ready_max = ms;
    }
    if (ms < READY_SLOW_MS || P.g.ready_logged >= READY_LOGS) return;
    P.g.ready_logged++;
    const uint8_t *vi = gfx_shader_input_id(j->s.vs), *fi = j->s.fs ? gfx_shader_input_id(j->s.fs) : NULL;
    char fid[16] = "none";
    if (fi) snprintf(fid, sizeof fid, "%02x%02x%02x%02x", fi[0], fi[1], fi[2], fi[3]);
    rt_log("vk_pipeline: a pipeline a draw missed was usable after %.0f ms: a %s pipeline, %s; %s; %s shader "
           "%02x%02x%02x%02x (%s), fragment shader %s (%s)%s%s",
           ms, mesh ? "mesh" : "vertex",
           fast ? "fast-linked, so the time went to its stage libraries (the lines \"a ... library took\" name the slow ones)"
                : no_libs(&j->s, p->lay) ? "which has no stage libraries: this is its whole build"
                                         : "which got no fast link: this is its whole build",
           j->prebuild ? "pipeline-keys.bin names it and its prebuild had not come to it" : "not in pipeline-keys.bin before",
           mesh ? "mesh" : "vertex", vi[0], vi[1], vi[2], vi[3], origin_name(j->s.vs), fid, origin_name(j->s.fs),
           j->k.fbl ? "; feedback loop" : "", P.g.ready_logged == READY_LOGS ? " (the last such line of this run)" : "");
}

static void prebuild_report(void) {
    rt_log("vk_pipeline: prebuild done: %u built, %u failed, %u asked for by a draw first; %.0f ms after %s", K.built,
           K.failed, K.promoted, now_ms() - K.t0, K.prep ? "the preparation started" : "the device was ready");
}

static void job_lower(Job *j);

static void u_run(LibJob *lj) {
    U.by_worker++, U.running++;
    if (lj->p) {
        Pipe *p = lj->p;
        if (!p->info.pipeline) {
            int final = 0;
            VkPipeline f = gpl_link(&lj->s, &lj->k, lj->l, 1, &final);
            if (f && !p->info.pipeline) {
                p->fast = p->info.pipeline = f;
                ready_note(p, 1, &lj->s);
                job_lower(p->job);
            } else if (f) {
                vkDestroyPipeline(P.device, f, NULL);
            }
        }
        p->link_queued = 0;
        U.links++;
        plat_cond_broadcast(&done_cv);
    } else {
        lib_build(lj->b, lj->kind, &lj->s, &lj->k, lj->l);
    }
    U.running--;
    free(lj);
}

static int fast_workers(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2SARIA_PIPE_FAST_WORKERS");
    return on;
}

static Job *fast_take(void) {
    if (!P.gpl || !fast_workers()) return NULL;
    for (Job *j = A.head, *prev = NULL; j; prev = j, j = j->next) {
        Pipe *p = j->p;
        if (p->info.pipeline || p->fast_tried || p->link_queued || !p->lay || no_libs(&j->s, p->lay)) continue;
        if (prev) prev->next = j->next;
        else A.head = j->next;
        if (A.tail == j) A.tail = prev;
        j->next = j->prev = NULL;
        return j;
    }
    return NULL;
}

static void lib_worker(void *arg) {
    (void)arg;
    plat_mutex_lock(&P.lock);
    for (;;) {
        Job *j = NULL;
        while (!U.head && !(j = fast_take())) plat_cond_wait(&lib_cv, &P.lock);
        if (!j) {
            LibJob *lj = U.head;
            if (!(U.head = lj->next)) U.tail = NULL;
            if (lj->b) lj->b->job = NULL;
            u_run(lj);
            continue;
        }
        Pipe *p = j->p;
        int final;
        p->fast_tried = 1;
        VkPipeline f = gpl_link(&j->s, &j->k, p->lay, 1, &final);
        if (f && !p->info.pipeline) {
            p->fast = p->info.pipeline = f, P.g.wlinks++, P.g.flinks++;
            ready_note(p, 1, &j->s);
            plat_cond_broadcast(&done_cv);
        } else if (f) {
            vkDestroyPipeline(P.device, f, NULL);
        }
        if (p->info.pipeline) {
            j->low = 1, P.g.requeued++;
            j->next = A.low;
            if (A.low) A.low->prev = j;
            else A.low_tail = j;
            A.low = j;
        } else {
            if (!(j->next = A.head)) A.tail = j;
            A.head = j;
        }
        plat_cond_signal(&work_cv);
    }
}

#define GATE_BIG_MB 4096.0
#define GATE_SAMPLE_MS 100.0
#define GATE_WAIT_MS 30000.0
static struct {
    PlatMutex lock;
    BuildGate g;
    double est, used, limit, at;
    int init;
    int off;
    uint64_t refused;
} GATE = {.lock = PLAT_MUTEX_INIT};

static int gate_enter(double est, double *took) {
    plat_mutex_lock(&GATE.lock);
    if (!GATE.init) {
        GATE.init = 1;
        GATE.g.free_mb = strtod(cfg_value("NX2SARIA_BUILD_FREE_MB"), NULL);
        GATE.est = strtod(cfg_value("NX2SARIA_BUILD_RESERVE_MB"), NULL);
        if (GATE.est < 0) GATE.est = 0;
        GATE.at = -GATE_SAMPLE_MS;
    }
    if (GATE.off) GATE.g.free_mb = 0;
    double now = now_ms();
    if (now - GATE.at >= GATE_SAMPLE_MS) {
        double proc;
        plat_commit_info(&proc, &GATE.used, &GATE.limit);
        GATE.at = now;
    }
    if (est <= 0) est = GATE.est;
    int ok = build_gate_enter(&GATE.g, GATE.used, GATE.limit, est);
    if (!ok) GATE.refused++;
    plat_mutex_unlock(&GATE.lock);
    if (took) *took = est;
    return ok;
}

static void gate_leave(double est) {
    plat_mutex_lock(&GATE.lock);
    build_gate_leave(&GATE.g, est);
    plat_mutex_unlock(&GATE.lock);
}

static const char *gate_text(char *buf, size_t n) {
    plat_mutex_lock(&GATE.lock);
    snprintf(buf, n, "system commit %.0f of %.0f MB, %.0f MB reserved for builds under way, %.0f MB kept free "
                     "(NX2SARIA_BUILD_FREE_MB), %.0f MB a build (NX2SARIA_BUILD_RESERVE_MB)", GATE.used, GATE.limit,
             GATE.g.reserved_mb, GATE.g.free_mb, GATE.est);
    plat_mutex_unlock(&GATE.lock);
    return buf;
}

static int gate_wait(const char *who, volatile int32_t *stop, double *took) {
    double w0 = now_ms();
    int waited = 0;
    while (!gate_enter(0, took)) {
        char g[256];
        if (*stop) return 0;
        if (!waited++) rt_log("vk_pipeline: %s: waiting for commit headroom (%s)", who, gate_text(g, sizeof g));
        if (now_ms() - w0 > GATE_WAIT_MS) return 0;
        for (int i = 0; i < 20 && !*stop; i++) plat_sleep_ms(100);
    }
    return 1;
}

static int bf_waiting(void);
static struct {
    PlatMutex lock;
    PlayGateCount n, logged;
    volatile int32_t draws_min;
    volatile uint32_t draws_last;
    volatile uint64_t shown_ns;
} PG = {.lock = PLAT_MUTEX_INIT, .draws_min = -1};

static int pg_draws_min(void) {
    int m = __atomic_load_n(&PG.draws_min, __ATOMIC_ACQUIRE);
    if (m >= 0) return m;
    plat_mutex_lock(&PG.lock);
    if ((m = PG.draws_min) < 0) {
        m = cfg_int("NX2SARIA_BG_BUILD_PLAY_DRAWS");
        if (m < 0) m = 0;
        if (m)
            rt_log("vk: background builds rest in play: while the frame shown has %d draws or more "
                   "(NX2SARIA_BG_BUILD_PLAY_DRAWS; 0: they build in every frame); a pipeline a draw waits for is always "
                   "built", m);
        else
            rt_log("vk: background builds rest in play: off (NX2SARIA_BG_BUILD_PLAY_DRAWS 0): they build in "
                   "every frame");
        __atomic_store_n(&PG.draws_min, m, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&PG.lock);
    return m;
}

static int play_rest(void) {
    int m = pg_draws_min();
    if (!m) return 0;
    uint64_t shown = __atomic_load_n(&PG.shown_ns, __ATOMIC_ACQUIRE);
    int light = prep_mode || bf_waiting() || vk_exec_pool_loading();
    return play_gate_rest(m, __atomic_load_n(&PG.draws_last, __ATOMIC_RELAXED), now_ms(),
                          shown ? (double)shown / 1e6 : -1.0, light);
}

void vk_pipeline_frame_note(uint32_t draws) {
    __atomic_store_n(&PG.draws_last, draws, __ATOMIC_RELAXED);
    __atomic_store_n(&PG.shown_ns, plat_now_ns(), __ATOMIC_RELEASE);
    if (!pg_draws_min()) return;
    int rest = play_rest();
    plat_mutex_lock(&PG.lock);
    play_gate_frame(&PG.n, rest);
    plat_mutex_unlock(&PG.lock);
}

static void pg_rest_begin(uint64_t held) {
    plat_mutex_lock(&PG.lock);
    play_gate_rest_begin(&PG.n, held);
    plat_mutex_unlock(&PG.lock);
}

static void pg_rest_end(double ms) {
    plat_mutex_lock(&PG.lock);
    play_gate_rest_end(&PG.n, ms);
    plat_mutex_unlock(&PG.lock);
}

static uint64_t low_held(void) {
    uint64_t n = 0;
    for (Job *j = A.low; j; j = j->next)
        if (!j->rested) j->rested = 1, n++;
    return n;
}

static void play_rest_wait(volatile int32_t *stop) {
    if (*stop || !play_rest()) return;
    double t0 = now_ms();
    pg_rest_begin(1);
    while (!*stop && play_rest()) plat_sleep_ms(100);
    pg_rest_end(now_ms() - t0);
}

static void pg_log(void) {
    if (pg_draws_min() <= 0) return;
    plat_mutex_lock(&PG.lock);
    if (play_gate_changed(&PG.n, &PG.logged))
        rt_log("vk: background builds paused in play: %llu frames, %llu builds deferred (%llu rests, %.1f s of "
               "builder time at rest; NX2SARIA_BG_BUILD_PLAY_DRAWS %d)", (unsigned long long)PG.n.frames,
               (unsigned long long)PG.n.deferred, (unsigned long long)PG.n.rests, PG.n.rest_ms / 1000.0,
               (int)PG.draws_min);
    plat_mutex_unlock(&PG.lock);
}

int vk_pipeline_probe_buildgate(void) {
    double took = 0, big = 0;
    int fails = 0, in = 0, want = 0;
    int first = gate_enter(0, &took);
    double free_mb = GATE.g.free_mb, used = GATE.used, limit = GATE.limit, est = took;
    if (first) gate_leave(took);
    printf("probe-buildgate: system commit %.0f of %.0f MB, %.0f MB kept free (NX2SARIA_BUILD_FREE_MB), %.0f MB a build "
           "(NX2SARIA_BUILD_RESERVE_MB)\n", used, limit, free_mb, est);
    double room = limit - used - free_mb;
    for (int i = 0; i < 16; i++) {
        want += free_mb <= 0 || limit < 0 || room - i * est >= est;
        in += gate_enter(0, &took);
    }
    double reserved = GATE.g.reserved_mb;
    int big_in = gate_enter(GATE_BIG_MB, &big);
    int big_want = free_mb <= 0 || limit < 0 || room - reserved >= GATE_BIG_MB;
    printf("probe-buildgate: %d of 16 ordinary builds admitted (%d fit), %.0f MB reserved; a large compute build beside "
           "them %s\n", in, want, reserved, big_in ? "admitted" : "refused");
    if (in != want) fails++, printf("probe-buildgate: FAIL: %d admitted, %d fit\n", in, want);
    if (reserved != in * est) fails++, printf("probe-buildgate: FAIL: %.0f MB reserved for %d builds\n", reserved, in);
    if (big_in != big_want) fails++, printf("probe-buildgate: FAIL: the large build was %s\n", big_in ? "admitted" : "refused");
    if (big_in) gate_leave(big);
    for (int i = 0; i < in; i++) gate_leave(est);
    if (GATE.g.reserved_mb != 0) fails++, printf("probe-buildgate: FAIL: %.0f MB still reserved\n", GATE.g.reserved_mb);
    volatile int32_t stop = 1;
    double t0 = now_ms();
    int got = gate_wait("probe", &stop, &took);
    if (got) gate_leave(took);
    if (got != first || now_ms() - t0 > 1000)
        fails++, printf("probe-buildgate: FAIL: a stopped builder's wait gave %d after %.0f ms\n", got, now_ms() - t0);
    printf("probe-buildgate: an ordinary build is %s now; %s\n", first ? "admitted" : "refused", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}

static uint64_t fast_retired;

static void fast_destroy_cb(void *arg) {
    vkDestroyPipeline(P.device, (VkPipeline)(uintptr_t)arg, NULL);
}

static void fast_retire(Pipe *p) {
    VkPipeline old = p->fast;
    if (!old || old == p->info.pipeline) return;
    p->fast = VK_NULL_HANDLE;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t rf = vk_exec_recording_fence(), at = vk_sync_issued() + 2;
    if (rf + 1 > at) at = rf + 1;
    vk_completion_add(at, fast_destroy_cb, (void *)(uintptr_t)old);
    fast_retired++;
}

#define RELEASE_EVERY_MS 5000
#define RELEASE_BATCH 256
#define RELEASE_DRAIN 64
static uint64_t rel_total, rel_batches;
static uint64_t rel_used0;
static Pipe *rel_head, *rel_tail;

static void release_drain(void) {
    if (!rel_head || vk_device_is_lost()) return;
    uint64_t done = vk_sync_fence_done();
    for (int i = 0; i < RELEASE_DRAIN && rel_head && rel_head->rel_fence <= done; i++) {
        Pipe *p = rel_head;
        if (!(rel_head = p->next)) rel_tail = NULL;
        if (p->info.pipeline) vkDestroyPipeline(P.device, p->info.pipeline, NULL);
        free(p->bindings);
        free(p);
    }
}

static int releasable(const Pipe *p) {
    if (p->used_min || !p->info.pipeline || p->building || p->job || p->link_queued || p->fast || p->t_miss || !p->lay)
        return 0;
    if (p->info.bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS || p->key_size != sizeof(GKey)) return 0;
    const GKey *k = (const GKey *)p->key;
    return k->vs && gfx_shader_stage(k->vs) != GFX_STAGE_MS;
}

static void pressure_release(uint64_t now) {
    static int on = -1, pct;
    static uint64_t last;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_PIPE_RELEASE");
        pct = cfg_int("NX2SARIA_TEX_VRAM_PRESSURE");
        if (pct < 0 || pct > 100) pct = 80;
        if (!on) rt_log("vk_pipeline: every pipeline stays for the run, also while video memory is short (NX2SARIA_PIPE_RELEASE=0)");
        else if (!pct) {
            on = 0;
            rt_log("vk_pipeline: every pipeline stays for the run, also while video memory is short "
                   "(NX2SARIA_TEX_VRAM_PRESSURE=0)");
        }
    }
    if (!on || now - last < RELEASE_EVERY_MS) return;
    last = now;
    uint64_t used = 0, budget = 0;
    if (!vk_mem_vram_live(&used, &budget) || !budget) return;
    if (rel_used0) {
        rt_log("vk_pipeline: device-local memory in use (free space inside the allocator's blocks not counted) %llu MiB "
               "when the last batch of unused pipelines went, %llu MiB now", (unsigned long long)rel_used0,
               (unsigned long long)(used >> 20));
        rel_used0 = 0;
    }
    if (used * 100 < budget * (uint64_t)pct) return;
    Pipe *gone[RELEASE_BATCH];
    int n = 0;
    plat_mutex_lock(&P.lock);
    for (int b = 0; b < PIPE_BUCKETS && n < RELEASE_BATCH; b++)
        for (Pipe **pp = &pipes[b]; *pp && n < RELEASE_BATCH;) {
            Pipe *p = *pp;
            if (releasable(p)) *pp = p->next, gone[n++] = p;
            else pp = &p->next;
        }
    uint64_t at = vk_sync_issued() + 2;
    plat_mutex_unlock(&P.lock);
    if (!n) return;
    for (int i = 0; i < n; i++) {
        gone[i]->rel_fence = at, gone[i]->next = NULL;
        if (rel_tail) rel_tail->next = gone[i];
        else rel_head = gone[i];
        rel_tail = gone[i];
    }
    rel_total += (uint64_t)n, rel_batches++;
    rel_used0 = used >> 20;
    if (rel_batches <= 16 || !(rel_batches & (rel_batches - 1)))
        rt_log("vk_pipeline: video memory in use %llu of %llu MiB (over %d%%, free space inside the allocator's blocks not "
               "counted): %d graphics pipelines no draw used in this run given back, %llu in %llu batches so far; a draw "
               "that needs one builds it again from the pipeline cache (NX2SARIA_PIPE_RELEASE)",
               (unsigned long long)(used >> 20), (unsigned long long)(budget >> 20), pct, n,
               (unsigned long long)rel_total, (unsigned long long)rel_batches);
}

static void job_build(Job *j, int lowrun) {
    plat_mutex_unlock(&P.lock);
    VkPipeline pipe = create_graphics(&j->s, &j->k, j->l, j->fs_off);
    plat_mutex_lock(&P.lock);
    if (pipe || !j->p->fast) __atomic_store_n(&j->p->info.pipeline, pipe, __ATOMIC_SEQ_CST);
    ready_note(j->p, 0, &j->s);
    if (pipe && j->p->fast) P.g.swaps++, j->p->optimised = 1, fast_retire(j->p);
    j->p->building = 0;
    j->p->job = NULL;
    if (pipe) {
        P.stats.graphics++, P.stats.async_built++;
        if (!j->prebuild) keys_note(&j->s);
    } else {
        P.stats.failures++;
        rt_log("vk_pipeline: graphics pipeline not built: pipeline creation failed");
    }
    if (j->prebuild) {
        if (pipe) K.built++;
        else K.failed++;
        K.promoted += j->demanded;
        if (!--K.left && !K.loading) prebuild_report();
    }
    A.low_running -= lowrun;
    if (!--A.inflight) plat_cond_broadcast(&A.idle);
    plat_cond_broadcast(&done_cv);
    if (pipe && (!j->prebuild || j->demanded)) perf_count(PERF_CNT_PIPE_ASYNC);
    free(j);
}

static void pipe_worker(void *arg) {
    int me = (int)(intptr_t)arg;
    int resting = 0;
    double rest_t0 = 0;
    plat_mutex_lock(&P.lock);
    for (;;) {
        while (!U.head && !A.head && !(A.low && (A.threads < 3 || !U.on || A.low_running < A.threads - 1))) {
            if (resting) resting = 0, pg_rest_end(now_ms() - rest_t0);
            U.idle++;
            plat_cond_wait(&work_cv, &P.lock);
            U.idle--;
        }
        if (U.head) {
            if (resting) resting = 0, pg_rest_end(now_ms() - rest_t0);
            LibJob *lj = U.head;
            if (!(U.head = lj->next)) U.tail = NULL;
            if (lj->b) lj->b->job = NULL;
            if (A.prio_low[me]) A.prio_low[me] = 0, plat_thread_priority(PLAT_PRIO_NORMAL);
            u_run(lj);
            continue;
        }
        Job *j = A.head;
        int lowrun = !j;
        double held = 0;
        if (!j && P.gpl && play_rest()) {
            if (!resting) resting = 1, rest_t0 = now_ms(), pg_rest_begin(low_held());
            U.idle++;
            plat_cond_wait_ns(&work_cv, &P.lock, 100000000ull);
            U.idle--;
            continue;
        }
        if (resting) resting = 0, pg_rest_end(now_ms() - rest_t0);
        if (!j && !gate_enter(0, &held)) {
            if (!A.gate_shut++) {
                char g[256];
                rt_log("vk_pipeline: background pipeline builds wait for commit headroom (%s); a pipeline a draw needs "
                       "is still built at once", gate_text(g, sizeof g));
            }
            plat_cond_wait_ns(&work_cv, &P.lock, 2000000000ull);
            continue;
        }
        if (!j && A.gate_shut) {
            A.gate_shut = 0;
            rt_log("vk_pipeline: background pipeline builds go on: the commit headroom is back");
        }
        if (j) {
            if (!(A.head = j->next)) A.tail = NULL;
        } else {
            j = A.low;
            if ((A.low = j->next)) A.low->prev = NULL;
            else A.low_tail = NULL;
            A.low_running++;
        }
        j->next = j->prev = NULL;
        j->worker = me;
        if (j->low != A.prio_low[me]) {
            A.prio_low[me] = j->low;
            plat_thread_priority(j->low ? PLAT_PRIO_BELOW_NORMAL : PLAT_PRIO_NORMAL);
        }
        if (P.gpl && (!j->prebuild || j->demanded) && !j->p->info.pipeline && !j->p->fast_tried && j->p->lay &&
            !no_libs(&j->s, j->p->lay)) {
            int final;
            VkPipeline f = gpl_link(&j->s, &j->k, j->p->lay, 1, &final);
            j->p->fast_tried = 1;
            if (f && !j->p->info.pipeline) {
                j->p->fast = j->p->info.pipeline = f, P.g.wlinks++;
                ready_note(j->p, 1, &j->s);
                plat_cond_broadcast(&done_cv);
                j->low = 1, j->worker = -1, P.g.requeued++;
                j->prev = NULL, j->next = A.low;
                if (A.low) A.low->prev = j;
                else A.low_tail = j;
                A.low = j;
                A.low_running -= lowrun;
                if (lowrun) gate_leave(held);
                continue;
            } else if (f) {
                vkDestroyPipeline(P.device, f, NULL);
            }
        }
        job_build(j, lowrun);
        if (lowrun) gate_leave(held);
    }
    return;
}

static int hint_on(void);

static int workers_start(void) {
    if (A.threads) return 1;
    uint32_t cpus = plat_cpu_usable();
    int n = cpus / 4 < 2 ? 2 : cpus / 4 > MAX_WORKERS ? MAX_WORKERS : (int)(cpus / 4);
    for (int i = 0; i < n; i++) {
        PlatThread *t = plat_thread_new(pipe_worker, (void *)(intptr_t)A.threads, "pipe worker");
        if (!t) break;
        A.thread[A.threads++] = t;
    }
    int nlib = hint_on() || fast_workers() ? n : 2;
    for (int i = 0; A.threads && U.on && P.gpl && i < nlib; i++)
        if (plat_thread_new(lib_worker, NULL, "pipe lib worker")) U.lib_workers++;
    if (A.threads)
        rt_log("vk_pipeline: %d background graphics pipeline workers, %d for waiting draws' libraries", A.threads,
               U.lib_workers);
    else rt_log("vk_pipeline: cannot start pipeline workers; building synchronously");
    return A.threads != 0;
}

static void workers_drain(void) {
    plat_mutex_lock(&P.lock);
    K.loading = 0;
    for (Job *j = A.low, *n; j; j = n) {
        n = j->next;
        j->p->building = 0, j->p->job = NULL;
        A.inflight--, K.left--;
        free(j);
    }
    A.low = A.low_tail = NULL;
    for (LibJob *j = U.head, *n; j; j = n) {
        n = j->next;
        if (j->b) j->b->job = NULL, j->b->building = 0;
        if (j->p) j->p->link_queued = 0;
        free(j);
    }
    U.head = U.tail = NULL;
    while (A.inflight) plat_cond_wait(&A.idle, &P.lock);
    while (U.running) plat_cond_wait(&done_cv, &P.lock);
    plat_mutex_unlock(&P.lock);
}

void vk_pipeline_async_enable(void) {
    U.on = strcmp(cfg_value("NX2SARIA_PIPE_WAIT"), "draw") != 0;
    const char *e = cfg_get("NX2SARIA_PIPE_SYNC");
    if (e && *e && strcmp(e, "0")) return;
    A.on = 1;
}

int vk_pipeline_async_on(void) { return A.on; }

static void promote(Pipe *p) {
    Job *j = p->job;
    if (!j || !j->low) return;
    if (j->prebuild && !j->demanded && !p->info.pipeline) p->t_miss = plat_now_ns();
    j->low = 0, j->demanded = 1;
    if (j->worker == -1) {
        if (j->prev) j->prev->next = j->next;
        else A.low = j->next;
        if (j->next) j->next->prev = j->prev;
        else A.low_tail = j->prev;
        j->next = j->prev = NULL;
        if (A.tail) A.tail->next = j;
        else A.head = j;
        A.tail = j;
        plat_cond_signal(&work_cv);
        if (U.lib_workers) plat_cond_signal(&lib_cv);
    } else if (j->worker >= 0 && A.prio_low[j->worker]) {
        A.prio_low[j->worker] = 0;
        plat_thread_set_priority(A.thread[j->worker], PLAT_PRIO_NORMAL);
    }
}

#define TAKE_WAIT_MS 500u
static void job_wanted(Job *j, uint32_t wait_ms) {
    if (!j || j->worker != -1 || j->low) return;
    Job *q = NULL;
    if (A.head != j) {
        for (q = A.head; q && q->next != j; q = q->next) {}
        if (!q) return;
    }
    if (wait_ms > TAKE_WAIT_MS && !U.idle) {
        if (q) {
            if (!(q->next = j->next)) A.tail = q;
        } else if (!(A.head = j->next)) {
            A.tail = NULL;
        }
        j->next = j->prev = NULL;
        j->worker = JOB_ON_DRAW;
        A.taken++;
        job_build(j, 0);
        return;
    }
    if (q) {
        if (!(q->next = j->next)) A.tail = q;
        j->next = A.head, A.head = j;
        A.first++;
    }
    plat_cond_signal(&work_cv);
}

static Layout *mesh_pair_layout(const VkPipeState *s, Bindings *B, const char **why) {
    Bindings F = {0};
    Layout *l = NULL;
    if (!add_stage(B, s->vs, 0) || (s->ts && !add_stage(B, s->ts, binding_span(s->vs) - 1)) ||
        (s->fs && !add_stage(&F, s->fs, 0))) *why = "too many bindings";
    else if (!(l = layout_pair(B, &F))) *why = "descriptor layout creation failed";
    B->pool |= F.pool;
    return l;
}

static Pipe *pipe_miss(const VkPipeState *s, const GKey *k, uint64_t h, int mode, int *queued) {
    *queued = 0;
    const char *why = NULL;
    Bindings B = {0};
    Layout *l = NULL;
    uint32_t span = binding_span(s->vs);
    int mesh = is_mesh(s);
    if (mesh) {
        uint32_t fspan = s->fs ? binding_span(s->fs) : 1;
        int emu = !vk_ctx()->has_mesh_shader && mesh_emu_on();
        if (!vk_ctx()->has_mesh_shader && !emu) why = "the GPU has no mesh shaders (mesh_emulation = 1 emulates them)";
        else if ((s->fs && gfx_shader_stage(s->fs) != GFX_STAGE_FS) || (s->ts && gfx_shader_stage(s->ts) != GFX_STAGE_TS)) why = "stage mismatch";
        else if (!emu && mesh_libs()) {
            l = mesh_pair_layout(s, &B, &why);
            span = 1;
        } else if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&B, s->fs, span - 1)) ||
                 (s->ts && !add_stage(&B, s->ts, span - 1 + fspan - 1))) why = "too many bindings";
        else if (!(l = emu ? layout_get_emu(&B) : layout_get(&B))) why = "descriptor layout creation failed";
    } else if (gfx_shader_stage(s->vs) != GFX_STAGE_VS || (s->fs && gfx_shader_stage(s->fs) != GFX_STAGE_FS)) why = "stage mismatch";
    else if (k->topo_class == 3) why = "patches need tessellation stages, which are not translated";
    else if (P.indep) {
        Bindings F = {0};
        if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&F, s->fs, 0))) why = "too many bindings";
        else if (!(l = layout_pair(&B, &F))) why = "descriptor layout creation failed";
        B.pool |= F.pool;
        span = 1;
    } else if (!add_stage(&B, s->vs, 0) || (s->fs && !add_stage(&B, s->fs, span - 1))) why = "too many bindings";
    else if (!(l = layout_get(&B))) why = "descriptor layout creation failed";
    char lack[128];
    if (!why && !layout_covers(s, l, span - 1, lack, sizeof lack)) why = lack, l = NULL;
    VkPipeline pipe = VK_NULL_HANDLE;
    Job *j = NULL;
    if (!why && mode && workers_start() && (j = calloc(1, sizeof *j))) {
        *j = (Job){.l = l, .fs_off = span - 1, .low = mode == 2, .prebuild = mode == 2, .worker = -1, .s = *s, .k = *k};
    } else if (!why && !(pipe = create_graphics(s, k, l, span - 1))) {
        why = "pipeline creation failed";
    }
    if (why) {
        P.stats.failures++;
        rt_log("vk_pipeline: graphics pipeline not built: %s", why);
    } else if (!j) {
        P.stats.graphics++;
        if (mode != 2) keys_note(s);
    }
    Pipe *p = pipe_add(k, sizeof *k, h, pipe, l, l ? &B : NULL, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_shader_inputs(s->vs, NULL));
    if (!j || !p) {
        free(j);
        return p;
    }
    j->p = p;
    p->building = 1, p->job = j;
    if (mode != 2) p->t_miss = plat_now_ns();
    if (mode == 2) {
        j->prev = A.low_tail;
        if (A.low_tail) A.low_tail->next = j;
        else A.low = j;
        A.low_tail = j;
        K.left++;
    } else {
        if (A.tail) A.tail->next = j;
        else A.head = j;
        A.tail = j;
        if (U.lib_workers && fast_workers()) plat_cond_signal(&lib_cv);
    }
    A.inflight++;
    plat_cond_signal(&work_cv);
    *queued = 1;
    return p;
}

static void job_lower(Job *j) {
    if (!j || j->worker != -1 || j->low) return;
    if (A.head == j) {
        if (!(A.head = j->next)) A.tail = NULL;
    } else {
        Job *q = A.head;
        while (q && q->next != j) q = q->next;
        if (!q) return;
        if (!(q->next = j->next)) A.tail = q;
    }
    j->low = 1, P.g.requeued++;
    j->prev = NULL, j->next = A.low;
    if (A.low) A.low->prev = j;
    else A.low_tail = j;
    A.low = j;
}

static void u_push(LibJob *j) {
    if (U.tail) U.tail->next = j;
    else U.head = j;
    U.tail = j;
}

static int lib_queue(const VkPipeState *s, const GKey *k, Layout *l, Lib *b[2]) {
    ViKey vk_;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &vk_, &pk, &fk, &ok);
    U.waits++;
    const void *key[2] = {&pk, &fk};
    uint32_t size[2] = {sizeof pk, sizeof fk};
    int queued = 0;
    for (int i = 0; i < 2; i++) {
        uint64_t h = key_hash(key[i], size[i]);
        if ((b[i] = lib_find(h, key[i], size[i]))) continue;
        if (!half_layout(l, i)) return 0;
        LibJob *j = calloc(1, sizeof *j);
        if (!j || !(b[i] = lib_new(h, key[i], size[i]))) {
            free(j);
            return 0;
        }
        *j = (LibJob){.b = b[i], .kind = i ? LIB_FS : LIB_PR, .l = l, .s = *s, .k = *k};
        b[i]->job = j;
        u_push(j);
        U.queued++, queued++;
    }
    if (queued) plat_cond_broadcast(&work_cv), plat_cond_broadcast(&lib_cv);
    return 1;
}

static int link_queue(Pipe *p, const VkPipeState *s, const GKey *k, Layout *l) {
    Lib *b[2];
    if (!U.lib_workers || !lib_queue(s, k, l, b)) return 0;
    LibJob *j = calloc(1, sizeof *j);
    if (!j) return 0;
    *j = (LibJob){.l = l, .s = *s, .k = *k, .p = p};
    p->link_queued = 1;
    u_push(j);
    plat_cond_broadcast(&work_cv), plat_cond_broadcast(&lib_cv);
    return 1;
}

static void lib_wait(const VkPipeState *s, const GKey *k, Layout *l, double end) {
    Lib *b[2];
    if (!lib_queue(s, k, l, b)) return;
    double steal_at = now_ms() + 1.0;
    for (;;) {
        if (!b[0]->building && !b[1]->building) return;
        double now = now_ms();
        if (now >= end) {
            U.timeouts++;
            return;
        }
        Lib *q = b[0]->job ? b[0] : b[1]->job ? b[1] : NULL;
        if (q && !U.lib_workers && (now >= steal_at || !U.idle)) {
            U.stolen++;
            lib_steal(q);
            continue;
        }
        double until = q && steal_at < end ? steal_at : end;
        plat_cond_wait_ms(&done_cv, &P.lock, (uint32_t)(until - now) + 1);
    }
}

static int hint_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2SARIA_PIPE_AHEAD");
    return on;
}

int vk_pipeline_hint(const VkPipeState *s) {
    if (!A.on || !U.on || !hint_on() || !s->vs || !ensure_init() || !P.gpl || !P.indep) return 0;
    int mesh = is_mesh(s);
    if (mesh ? !mesh_libs() || (s->ts && gfx_shader_stage(s->ts) != GFX_STAGE_TS) : gfx_shader_stage(s->vs) != GFX_STAGE_VS)
        return 0;
    if (s->fs && gfx_shader_stage(s->fs) != GFX_STAGE_FS) return 0;
    GKey k;
    gkey_fill(s, &k);
    if (k.topo_class == 3) return 0;
    uint64_t h = key_hash(&k, sizeof k);
    int queued = 0;
    plat_mutex_lock(&P.lock);
    if (!pipe_find(&k, sizeof k, h)) {
        Bindings B = {0}, F = {0};
        const char *why = NULL;
        Layout *l = mesh ? mesh_pair_layout(s, &B, &why)
                  : add_stage(&B, s->vs, 0) && (!s->fs || add_stage(&F, s->fs, 0)) ? layout_pair(&B, &F) : NULL;
        Lib *b[2];
        uint64_t q0 = U.queued, w0 = U.waits;
        if (l && l->set1 && workers_start() && lib_queue(s, &k, l, b)) queued = (int)(U.queued - q0);
        U.waits = w0, U.queued = q0;
        P.g.hints++, P.g.hint_libs += (uint64_t)queued;
    }
    plat_mutex_unlock(&P.lock);
    return queued;
}

static VkPipeline graphics(const VkPipeState *s, const VkPipeInfo **info, int async, int *pending, uint32_t wait_ms) {
    if (info) *info = NULL;
    if (pending) *pending = 0;
    if (!s->vs || !ensure_init()) return VK_NULL_HANDLE;
    GKey k;
    gkey_fill(s, &k);
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    Pipe *p = pipe_find(&k, sizeof k, h);
    int queued;
    if (p) {
        if (!p->building) P.stats.hits++;
    } else {
        p = pipe_miss(s, &k, h, async, &queued);
    }
    if (p) pipe_used(p);
    static int draw_build = -1;
    if (draw_build < 0) {
        const char *e = cfg_get("NX2SARIA_GPL_DRAW_BUILD");
        draw_build = !(e && !strcmp(e, "0"));
        rt_log("vk_pipeline: a draw that waits for its pipeline %s", !draw_build ? "leaves its libraries to its job"
               : U.on ? "has its libraries built in parallel on the workers, ahead of other jobs"
                      : "builds its libraries itself (NX2SARIA_PIPE_WAIT=draw)");
    }
    double end = wait_ms ? now_ms() + wait_ms : 0;
    if (p && p->building && P.gpl && !p->info.pipeline && !p->fast_tried && !p->link_queued && p->lay &&
        !no_libs(s, p->lay) && wait_ms && draw_build && U.on && link_queue(p, s, &k, (Layout *)p->lay)) {
        p->fast_tried = 1;
        job_lower(p->job);
    } else if (p && p->building && P.gpl && !p->info.pipeline && !p->fast_tried && !p->link_queued && p->lay &&
               !no_libs(s, p->lay)) {
        int final = 0, build = wait_ms != 0 && draw_build;
        uint64_t p0 = perf_begin(), t0 = qpc();
        if (build && U.on) {
            job_lower(p->job);
            lib_wait(s, &k, (Layout *)p->lay, end);
            build = 0;
        } else if (build) {
            p->fast_tried = 1;
        }
        VkPipeline f = p->info.pipeline ? VK_NULL_HANDLE : gpl_link(s, &k, p->lay, build, &final);
        perf_end(PERF_PIPE, p0);
        if (wait_ms && draw_build) {
            double ms = ms_since(t0);
            P.g.dlinks++, P.g.dlink_ms += ms;
            if (ms > P.g.dlink_max) P.g.dlink_max = ms;
            if (ms > 5.0 && P.g.slow++ < 16)
                rt_log("vk_pipeline: gpl: a waiting draw %s its libraries in %.1f ms", U.on ? "got" : "built", ms);
        }
        if (final) p->fast_tried = 1;
        if (f && !p->info.pipeline) {
            p->fast = p->info.pipeline = f;
            ready_note(p, 1, s);
            job_lower(p->job);
            plat_cond_broadcast(&done_cv);
        } else if (f) {
            vkDestroyPipeline(P.device, f, NULL);
        }
    }
    if (p && p->building && !p->info.pipeline) {
        promote(p);
        uint64_t p0 = wait_ms ? perf_begin() : 0;
        double now;
        if (wait_ms) job_wanted(p->job, wait_ms);
        while (p->building && !p->info.pipeline && wait_ms && (now = now_ms()) < end)
            plat_cond_wait_ms(&done_cv, &P.lock, (uint32_t)(end - now) + 1);
        if (wait_ms) perf_end(PERF_PIPE, p0);
        if (wait_ms && !p->building) perf_count(PERF_CNT_PIPE_WAIT);
    }
    int wait = p && p->building && !p->info.pipeline;
    if (wait) P.stats.pending_skips++;
    VkPipeline pipe = p ? p->info.pipeline : VK_NULL_HANDLE;
    plat_mutex_unlock(&P.lock);
    if (wait) {
        if (pending) *pending = 1;
        return VK_NULL_HANDLE;
    }
    if (!p) return VK_NULL_HANDLE;
    if (info) *info = &p->info;
    return pipe;
}

VkPipeline vk_pipeline_graphics(const VkPipeState *s, const VkPipeInfo **info) {
    return graphics(s, info, 0, NULL, 0);
}

VkPipeline vk_pipeline_graphics_async(const VkPipeState *s, const VkPipeInfo **info, int *pending, uint32_t wait_ms) {
    return graphics(s, info, A.on, pending, wait_ms);
}

int vk_pipeline_graphics_key_matches(const VkPipeState *s, const VkPipeInfo *info) {
    if (!s->vs || !info) return -1;
    GKey k;
    gkey_fill(s, &k);
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    const Pipe *p = pipe_find(&k, sizeof k, h);
    int matches = p ? &p->info == info : -1;
    plat_mutex_unlock(&P.lock);
    return matches;
}

double vk_pipeline_building_ms(const VkPipeState *s) {
    if (!s->vs || !P.ready) return 0;
    GKey k;
    gkey_fill(s, &k);
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    const Pipe *p = pipe_find(&k, sizeof k, h);
    double ms = p && p->building && !p->info.pipeline && p->t_miss ? (double)(plat_now_ns() - p->t_miss) / 1e6 : 0;
    plat_mutex_unlock(&P.lock);
    return ms;
}

static int id_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

static void warm_builder(void *arg) {
    int one = arg != NULL;
    plat_mutex_lock(&P.lock);
    while (A.low && !(K.until && now_ms() >= K.until)) {
        double held;
        if (!gate_enter(0, &held)) {
            if (!K.gate_ended++) {
                char g[256];
                rt_log("vk_pipeline: prewarm: the boot builds fewer recorded pipelines side by side, or none: the commit "
                       "headroom is short (%s). The rest are built in the background when there is room, or at their "
                       "first draw", gate_text(g, sizeof g));
            }
            break;
        }
        Job *j = A.low;
        if ((A.low = j->next)) A.low->prev = NULL;
        else A.low_tail = NULL;
        j->next = j->prev = NULL;
        j->low = 0;
        plat_mutex_unlock(&P.lock);
        VkPipeline pipe = create_graphics(&j->s, &j->k, j->l, j->fs_off);
        plat_mutex_lock(&P.lock);
        if (pipe || !j->p->fast) __atomic_store_n(&j->p->info.pipeline, pipe, __ATOMIC_SEQ_CST);
        ready_note(j->p, 0, &j->s);
        if (pipe && j->p->fast) P.g.swaps++, j->p->optimised = 1, fast_retire(j->p);
        j->p->building = 0;
        j->p->job = NULL;
        if (pipe) {
            P.stats.graphics++, P.stats.async_built++, K.built++;
        } else {
            P.stats.failures++, K.failed++;
            rt_log("vk_pipeline: graphics pipeline not built: pipeline creation failed");
        }
        K.promoted += j->demanded;
        K.left--;
        if (!--A.inflight) plat_cond_broadcast(&A.idle);
        plat_cond_broadcast(&done_cv);
        free(j);
        gate_leave(held);
        if (one) break;
    }
    plat_mutex_unlock(&P.lock);
    return;
}

static void lb_start(const uint32_t pr_hist[16], const uint32_t fs_hist[4]);
static int pm_skipped(void);
static void pm_set(void);

static int32_t pw_phase;
static int64_t pw_pipe_done, pw_pipe_total;
static int64_t pw_boot_built = -1, pw_boot_ms = -1;

int vk_pipeline_boot_stats(int64_t *built, int64_t *ms) {
    int64_t t = __atomic_load_n(&pw_boot_ms, __ATOMIC_ACQUIRE);
    if (t < 0) return 0;
    *built = __atomic_load_n(&pw_boot_built, __ATOMIC_RELAXED);
    *ms = t;
    return 1;
}

void vk_pipeline_prewarm_progress(VkPrewarmProgress *p) {
    p->phase = __atomic_load_n(&pw_phase, __ATOMIC_ACQUIRE);
    gfx_shader_prewarm_progress(&p->sh_done, &p->sh_total, &p->sh_translated);
    p->pipe_done = __atomic_load_n(&pw_pipe_done, __ATOMIC_RELAXED);
    p->pipe_total = __atomic_load_n(&pw_pipe_total, __ATOMIC_RELAXED);
}

static void prewarm_run(void);

static struct {
    int on;
    int lw;
    volatile int32_t game;
    double t_end;
    char owed[PLAT_PATH_MAX * 2 + 48];
} BF;

static int bf_waiting(void) {
    return BF.on && !__atomic_load_n(&BF.game, __ATOMIC_ACQUIRE);
}

void vk_pipeline_prewarm_detach(void) {
    __atomic_store_n(&BF.game, 1, __ATOMIC_RELEASE);
}

static void bf_decide(void);
static void bf_wait(void);

#define PREWARM_PIPE_MS 20000.0

void vk_pipeline_prewarm(void) {
    prewarm_run();
    __atomic_store_n(&pw_phase, 5, __ATOMIC_RELEASE);
    session_summary_header("the boot prewarm");
}

typedef struct {
    VkPipeState s;
    GKey k;
    int kind;
    uint64_t h;
    uint32_t size;
    union { PrKey pr; FsKey fs; } key;
} MeshPre;
static struct {
    MeshPre *e;
    uint32_t n, cap, n_pr;
    volatile int64_t next;
    volatile int32_t stop;
    int boot;
    PlatThread *th;
    PlatMutex stat;
    uint64_t built, have, fail;
    double ms, max, t0;
} MP = {.stat = PLAT_MUTEX_INIT};

static void mesh_pre_note(const VkPipeState *s, const GKey *k) {
    ViKey v;
    PrKey pk;
    FsKey fk;
    FoKey ok;
    lib_keys(s, k, &v, &pk, &fk, &ok);
    const void *key[2] = {&pk, &fk};
    const uint32_t size[2] = {sizeof pk, sizeof fk};
    for (int i = 0; i < 2; i++) {
        int kind = i ? LIB_FS : LIB_PR;
        uint64_t h = key_hash(key[i], size[i]);
        uint32_t j = 0;
        while (j < MP.n && !(MP.e[j].kind == kind && MP.e[j].h == h && !memcmp(&MP.e[j].key, key[i], size[i]))) j++;
        if (j < MP.n) continue;
        if (MP.n == MP.cap) {
            uint32_t cap = MP.cap ? MP.cap * 2 : 64;
            MeshPre *t = realloc(MP.e, cap * sizeof *t);
            if (!t) return;
            MP.e = t, MP.cap = cap;
        }
        MeshPre *e = &MP.e[MP.n++];
        memset(e, 0, sizeof *e);
        e->s = *s, e->k = *k, e->kind = kind, e->h = h, e->size = size[i];
        memcpy(&e->key, key[i], size[i]);
        MP.n_pr += kind == LIB_PR;
    }
}

static void mesh_pre_worker(void *arg) {
    (void)arg;
    for (;;) {
        int idle = MP.boot && !bf_waiting();
        plat_thread_priority(idle ? PLAT_PRIO_IDLE : PLAT_PRIO_NORMAL);
        int64_t i = plat_inc64(&MP.next) - 1;
        if (i >= (int64_t)MP.n || MP.stop) break;
        MeshPre *e = &MP.e[i];
        double held = 0;
        if (MP.boot && !gate_wait("mesh library prebuild", &MP.stop, &held)) {
            char g[256];
            if (!MP.stop)
                rt_log("vk_pipeline: mesh library prebuild: ends here, at %lld of %u libraries: no commit headroom for "
                       "another build (%s). The others are built when a draw first needs them", (long long)i, MP.n,
                       gate_text(g, sizeof g));
            break;
        }
        const char *why = NULL;
        Bindings B = {0};
        plat_thread_priority(PLAT_PRIO_NORMAL);
        plat_mutex_lock(&P.lock);
        int have = lib_find(e->h, &e->key, e->size) != NULL;
        Layout *l = have ? NULL : mesh_pair_layout(&e->s, &B, &why);
        VkPipelineLayout lay = l ? half_layout(l, e->kind == LIB_FS) : VK_NULL_HANDLE;
        plat_mutex_unlock(&P.lock);
        if (idle) plat_thread_priority(PLAT_PRIO_IDLE);
        VkPipeline pipe = VK_NULL_HANDLE;
        double t0 = now_ms(), ms = 0;
        if (lay) pipe = lib_create(e->kind, &e->s, &e->k, lay, &e->key);
        if (pipe) {
            ms = now_ms() - t0;
            vkDestroyPipeline(P.device, pipe, NULL);
            __atomic_fetch_add(&cache_merges, 1, __ATOMIC_RELAXED);
        }
        if (MP.boot) gate_leave(held);
        plat_mutex_lock(&MP.stat);
        if (have) MP.have++;
        else if (!pipe) MP.fail++;
        else MP.built++, MP.ms += ms, MP.max = ms > MP.max ? ms : MP.max;
        plat_mutex_unlock(&MP.stat);
    }
    if (MP.boot)
        rt_log("vk_pipeline: mesh library prebuild %s: of the %u shader libraries of the recorded mesh pipelines (%u of "
               "task and mesh stages, %u fragment shader) %llu built into the caches (avg %.1f ms, longest %.0f ms; a "
               "few milliseconds each when the caches held them), %llu made by a draw first, %llu failed; %.0f ms",
               MP.stop ? "stopped" : "done", MP.n, MP.n_pr, MP.n - MP.n_pr, (unsigned long long)MP.built,
               MP.built ? MP.ms / (double)MP.built : 0.0, MP.max, (unsigned long long)MP.have,
               (unsigned long long)MP.fail, now_ms() - MP.t0);
}

static void mesh_pre_start(void) {
    if (!MP.n || MP.th || MP.next) return;
    MP.boot = 1, MP.t0 = now_ms();
    MP.th = plat_thread_new(mesh_pre_worker, NULL, "mesh lib prebuild");
}

static void mesh_pre_run(int threads) {
    if (!MP.n) return;
    PlatThread *t[16];
    int n = 0;
    MP.boot = 0, MP.t0 = now_ms();
    for (int i = 0; i < threads && i < 16 && (uint32_t)i < MP.n; i++)
        if ((t[n] = plat_thread_new(mesh_pre_worker, NULL, "mesh lib prebuild"))) n++;
    if (!n) mesh_pre_worker(NULL);
    for (int i = 0; i < n; i++) plat_thread_join(t[i]);
}

static int seed_boot(void) {
    static int on = -1;
    if (on >= 0) return on;
    const char *e = cfg_get("NX2SARIA_PIPE_SEED_BOOT");
    if (e && (!strcmp(e, "0") || !strcmp(e, "1"))) return on = e[0] == '1';
    double proc, used, limit, need = strtod(cfg_value("NX2SARIA_MEM_NEED_MB"), NULL);
    plat_commit_info(&proc, &used, &limit);
    return on = !mem_need_short(proc, used, limit, need);
}

static void prewarm_run(void) {
    const char *gpu = rt_config()->gpu;
    uint32_t pr_hist[16] = {0}, fs_hist[4] = {0};
    if (!A.on || (gpu && !strcmp(gpu, "null"))) return;
    if (!cfg_bool("NX2SARIA_PIPE_PREBUILD")) {
        if (ensure_init()) lb_start(pr_hist, fs_hist);
        return;
    }
    double t0 = now_ms();
    K.warm = 1;
    if (!ensure_init() || !K.file_n) {
        rt_log("vk_pipeline: prewarm: no pipeline keys (%s)", K.path[0] ? K.path : "<none>");
        if (P.ready) lb_start(pr_hist, fs_hist);
        return;
    }
    const KeyRec *r = (const KeyRec *)K.file;
    size_t n = K.file_n, m = 0;
    uint8_t (*ids)[32] = malloc(3 * n * 32);
    const GfxShader **sh = calloc(3 * n, sizeof *sh);
    if (!ids || !sh) {
        free(ids), free(sh);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        memcpy(ids[m++], r[i].vs, 32);
        if (r[i].has_fs) memcpy(ids[m++], r[i].fs, 32);
        if (r[i].has_ts) memcpy(ids[m++], r[i].ts, 32);
    }
    qsort(ids, m, 32, id_cmp);
    size_t u = 0;
    for (size_t i = 0; i < m; i++)
        if (!u || memcmp(ids[u - 1], ids[i], 32)) memcpy(ids[u++], ids[i], 32);
    int cpus = (int)plat_cpu_usable();
    const char *pe = cfg_get("NX2SARIA_PREWARM_XLATE");
    int procs = pe && *pe ? atoi(pe) : cpus / 2 > 16 ? 16 : cpus / 2;
    __atomic_store_n(&pw_phase, 1, __ATOMIC_RELEASE);
    gfx_shader_prewarm((const uint8_t(*)[32])ids, u, sh, cpus, procs < 1 ? 1 : procs);
    double t1 = now_ms();
    size_t carried = 0, fresh = 0;
    for (size_t i = 0; i < u; i++)
        if (sh[i]) carried += gfx_shader_origin(sh[i]) == 1, fresh += gfx_shader_origin(sh[i]) == 2;
    vk_mem_stage_log("the shader prewarm");

    if (P.indep) {
        uint64_t pair = 0, uni = 0, pairs = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
            const uint8_t (*f)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
            if (!v || !f || !sh[v - ids] || !sh[f - ids] || gfx_shader_stage(sh[v - ids]) != GFX_STAGE_VS) continue;
            GfxShaderIo io[32];
            vs_outs(sh[v - ids], sh[f - ids], io);
            pair += (uint64_t)__builtin_popcount(gfx_shader_inputs(sh[f - ids], NULL)), pairs++;
        }
        for (uint32_t j = 0; j < VS_OUTS_CAP; j++) uni += VO.vs[j] ? (uint64_t)__builtin_popcount(VO.mask[j]) : 0;
        rt_log("vk_pipeline: vertex output unions: %u vertex shaders, %.2f locations each; %llu pairs read %.2f each",
               VO.n, VO.n ? (double)uni / VO.n : 0.0, (unsigned long long)pairs, pairs ? (double)pair / pairs : 0.0);
    }
    if (pm_skipped()) {
        plat_mutex_lock(&P.lock);
        free(K.file);
        K.file = NULL;
        plat_mutex_unlock(&P.lock);
        free(ids), free(sh);
        lb_start(pr_hist, fs_hist);
        return;
    }
    pm_set();
    bf_decide();
    uint32_t queued = 0, dropped = 0, known = 0, listed = 0;
    int at_boot = seed_boot();
    plat_mutex_lock(&P.lock);
    K.t0 = t1;
    K.loading = 1;
    for (size_t i = 0; i < n; i++) {
        const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
        const uint8_t (*f)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
        const uint8_t (*t)[32] = r[i].has_ts ? bsearch(r[i].ts, ids, u, 32, id_cmp) : NULL;
        const GfxShader *vs = v ? sh[v - ids] : NULL, *fs = f ? sh[f - ids] : NULL, *ts = t ? sh[t - ids] : NULL;
        if (!vs || (r[i].has_fs && !fs) || (r[i].has_ts && !ts)) {
            dropped++;
            continue;
        }
        VkPipeState s = r[i].s;
        s.vs = vs, s.fs = fs, s.ts = ts;
        if (is_mesh(&s) && (vk_ctx()->mesh_fallback || (!vk_ctx()->has_mesh_shader && !mesh_emu_on()))) {
            dropped++;
            continue;
        }
        GKey k;
        gkey_fill(&s, &k);
        if (!is_mesh(&s)) {
            pr_hist[(k.clamp & 1) | (k.poly_mode & 3) << 1 | (k.minus_w & 1) << 3]++;
            fs_hist[k.samples >= 8 ? 3 : k.samples >= 4 ? 2 : k.samples >= 2 ? 1 : 0]++;
        } else if (mesh_libs()) {
            mesh_pre_note(&s, &k);
        }
        if (!at_boot && i >= K.own_n && !is_mesh(&s)) {
            listed++;
            continue;
        }
        uint64_t h = key_hash(&k, sizeof k);
        int q = 0;
        if (pipe_find(&k, sizeof k, h)) known++;
        else pipe_miss(&s, &k, h, 2, &q), queued += (uint32_t)q;
    }
    K.loading = 0;
    plat_mutex_unlock(&P.lock);
    if (listed) {
        const char *e = cfg_get("NX2SARIA_PIPE_SEED_BOOT");
        rt_log("vk_pipeline: prewarm: %u records of the shared pipeline list that this install has not drawn are left to "
               "their first draw, when pipeline-cache.bin gives them (NX2SARIA_PIPE_SEED_BOOT=%s%s)", listed,
               e && !strcmp(e, "0") ? "0" : "auto",
               e && !strcmp(e, "0") ? "" : ": this machine started with less commit free than the game takes in play");
    }
    free(ids), free(sh);

    __atomic_store_n(&pw_pipe_done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&pw_pipe_total, (int64_t)queued, __ATOMIC_RELAXED);
    __atomic_store_n(&pw_phase, 2, __ATOMIC_RELEASE);
    const char *full = cfg_get("NX2SARIA_PREWARM_FULL");
    int all = (full && !strcmp(full, "1")) || (BF.on && queued);
    if (P.gpl && all && BF.on && !(full && !strcmp(full, "1")))
        rt_log("vk_pipeline: prewarm: the start waits for the %u recorded pipelines as long as the whole warm after a "
               "driver change may take, and not %d s", queued, (int)(PREWARM_PIPE_MS / 1000));
    K.until = P.gpl && !all ? now_ms() + PREWARM_PIPE_MS : 0;
    if (P.gpl && BF.on && BF.t_end && !(full && !strcmp(full, "1"))) K.until = BF.t_end;
    PlatThread *t[64];
    int nt = 0;
    if (queued) warm_builder((void *)1);
    for (int i = 0; i < cpus && i < 64 && queued; i++)
        if ((t[nt] = plat_thread_new(warm_builder, NULL, "pipe prewarm"))) nt++;
    if (!nt) warm_builder(NULL);
    double logged = now_ms();
    for (int i = 0; i < nt; i++)
        while (!plat_thread_join_ns(t[i], 100000000u)) {
            uint32_t fin = __atomic_load_n(&K.built, __ATOMIC_RELAXED) + __atomic_load_n(&K.failed, __ATOMIC_RELAXED);
            __atomic_store_n(&pw_pipe_done, (int64_t)fin, __ATOMIC_RELAXED);
            if (now_ms() - logged < 2000) continue;
            logged = now_ms();
            rt_log("vk_pipeline: prewarm: %u of %u pipelines, %.0f ms", fin, queued, now_ms() - t1);
        }
    __atomic_store_n(&pw_pipe_done, (int64_t)queued, __ATOMIC_RELAXED);
    plat_mutex_lock(&P.lock);
    uint32_t later = 0;
    for (Job *j = A.low; j; j = j->next) later += j->prebuild;
    if (!later)
        while (K.left) plat_cond_wait(&done_cv, &P.lock);
    else
        plat_cond_broadcast(&work_cv);
    rt_log("vk_pipeline: prewarm: %zu records in %s: %u pipelines built, %u left to build in the background, %u "
           "failed, %u already built, %u dropped (shader neither cached nor stored); shaders %.0f ms, pipelines %.0f "
           "ms on %d threads, total %.0f ms", n, K.path, K.built, later, K.failed, known, dropped, t1 - t0,
           now_ms() - t1, nt, now_ms() - t0);
    __atomic_store_n(&pw_boot_built, (int64_t)K.built, __ATOMIC_RELAXED);
    __atomic_store_n(&pw_boot_ms, (int64_t)(now_ms() - t1), __ATOMIC_RELEASE);
    free(K.file);
    K.file = NULL;
    plat_mutex_unlock(&P.lock);
    if (carried || fresh)
        rt_log("vk_pipeline: prewarm: of the %zu shaders the recorded pipelines name, %zu are a previous translator's "
               "translations and %zu were translated now; a recorded pipeline is compiled again only where its shaders' "
               "translation changed, which the pipeline-cache.bin hits below show", u, carried, fresh);
    fb_log("after the boot prewarm");
    vk_mem_stage_log("the recorded pipelines' prebuild");
    vk_pipeline_prewarm_marker_clear();
    lb_start(pr_hist, fs_hist);
    if (BF.on) bf_wait();
}

static int cache_dir(char *dir, size_t n);
static void gpu_key(char *out, size_t n);
static char PM_run[PLAT_PATH_MAX * 2 + 48], PM_crash[PLAT_PATH_MAX * 2 + 48];
static volatile int32_t pm_on;
static volatile int32_t jr_boot_on;
static int pm_quiet;

static void crash_path_init(const char *dir) {
    if (!PM_crash[0]) snprintf(PM_crash, sizeof PM_crash, "%s/boot-build-crash.txt", dir);
}

void vk_pipeline_crash_note(void) {
    static const char text[] = "the process faulted while the boot built pipelines or stage libraries "
                               "(runtime/vk_pipeline.c vk_pipeline_crash_note)\n";
    if (!PM_crash[0] || (!__atomic_load_n(&pm_on, __ATOMIC_ACQUIRE) && !__atomic_load_n(&jr_boot_on, __ATOMIC_ACQUIRE)))
        return;
    plat_file_write(PM_crash, text, sizeof text - 1);
}

static int crash_noted(void) {
    static int was = -1;
    if (was < 0) {
        was = PM_crash[0] && plat_file_exists(PM_crash);
        if (was) plat_file_delete(PM_crash);
    }
    return was;
}

static int pm_skipped(void) {
    char dir[PLAT_PATH_MAX * 2], sp[sizeof PM_run], key[32];
    if (!cache_dir(dir, sizeof dir)) return 0;
    snprintf(PM_run, sizeof PM_run, "%s/pipeline-prewarm-running.txt", dir);
    snprintf(sp, sizeof sp, "%s/pipeline-prewarm-skip.txt", dir);
    crash_path_init(dir);
    gpu_key(key, sizeof key);
    uint64_t n = 0;
    char *left = (char *)plat_file_read(PM_run, &n);
    if (left) {
        char head[64];
        snprintf(head, sizeof head, "%.*s", (int)(n < sizeof head - 1 ? n : sizeof head - 1), left);
        int crashed = crash_noted(), quiet = 0, what = boot_end_decide(head, key, crashed, &quiet);
        if (what == BOOT_END_OTHER) {
            plat_file_delete(PM_run);
        } else if (what == BOOT_END_SKIP) {
            char text[256];
            int len = snprintf(text, sizeof text,
                               "# The boot's pipeline prebuild did not finish on this GPU and driver (runtime/vk_pipeline.c "
                               "pm_skipped): starts skip it\n# while this line matches. Delete this file to build ahead of play "
                               "again.\n%s\n", key);
            if (plat_file_write_atomic(sp, text, (size_t)len)) plat_file_delete(PM_run);
            if (crashed)
                rt_log("vk_pipeline: the last start crashed while the boot built pipelines (%s was left, with the crash "
                       "handler's note); %s now skips that prebuild on this GPU and driver (%s)", PM_run, sp, key);
            else
                rt_log("vk_pipeline: %d starts in a row ended while the boot built pipelines, none with a crash report "
                       "(%s was left each time); %s now skips that prebuild on this GPU and driver (%s)", quiet, PM_run,
                       sp, key);
        } else {
            pm_quiet = quiet;
            plat_file_delete(PM_run);
            rt_log("vk_pipeline: the last start ended while the boot built pipelines and reported no crash: taken for a "
                   "start that was closed or stopped, and the pipelines are built again (%d in a row; at %d the prebuild "
                   "is skipped on this GPU and driver)", pm_quiet, BOOT_QUIET_ENDS);
        }
        free(left);
    }
    uint8_t *skip = plat_file_read(sp, &n);
    int on = 0;
    for (char *line = (char *)skip, *next; line && *line && !on; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
        on = line[0] != '#' && !strcmp(line, key);
    }
    free(skip);
    if (on)
        rt_log("vk_pipeline: prewarm: %zu recorded pipelines not built ahead of play: an earlier start died while building "
               "them on this GPU and driver (%s lists %s); each builds at its first draw", K.file_n, sp, key);
    return on;
}

static void pm_set(void) {
    if (!PM_run[0]) return;
    char key[32], text[64];
    gpu_key(key, sizeof key);
    int len = snprintf(text, sizeof text, "%s %d\n", key, pm_quiet);
    if (plat_file_write_atomic(PM_run, text, (size_t)len)) plat_xchg32(&pm_on, 1);
}

void vk_pipeline_prewarm_marker_clear(void) {
    if (plat_xchg32(&pm_on, 0)) plat_file_delete(PM_run);
}

static void bf_decide(void) {
    const char *full = cfg_get("NX2SARIA_PREWARM_FULL");
    char dir[PLAT_PATH_MAX * 2], key[32], line[100];
    if ((full && !strcmp(full, "0")) || rt_config()->shot || !cache_dir(dir, sizeof dir)) return;
    snprintf(BF.owed, sizeof BF.owed, "%s/boot-warm-owed.txt", dir);
    gpu_key(key, sizeof key);
    uint64_t n = 0;
    char *f = (char *)plat_file_read(BF.owed, &n);
    if (f) snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), f);
    int tries = 0, what = boot_warm_decide(f ? line : NULL, key, cache_foreign, &tries);
    if (what == BOOT_WARM_NO) {
        if (f) plat_file_delete(BF.owed);
    } else if (what == BOOT_WARM_DONE) {
        if (cache_foreign)
            rt_log("vk_pipeline: the whole warm for this GPU and driver was done by an earlier start (%s); the "
                   "pipeline cache's file keeps the earlier driver's header until the play cache is folded into it",
                   BF.owed);
    } else if (what == BOOT_WARM_GAVE_UP) {
        rt_log("vk_pipeline: the whole warm after a driver change did not end in %d starts on this GPU and driver (%s): "
               "this start builds in the background like any other. Delete %s to have it tried again", BOOT_WARM_TRIES,
               key, BF.owed);
    } else {
        int len = snprintf(line, sizeof line, "%s %d\n", key, tries);
        if (!plat_file_write_atomic(BF.owed, line, (size_t)len)) rt_log("vk_pipeline: cannot write %s", BF.owed);
        BF.on = 1;
        const char *se = cfg_get("NX2SARIA_PREWARM_FULL_S");
        double secs = se && *se ? atof(se) : 600;
        BF.t_end = secs > 0 ? now_ms() + secs * 1000 : 0;
        rt_log("vk_pipeline: the whole warm on the boot screen (%s, start %d of %d at most): the start waits for the "
               "recorded pipelines, the compute pipelines and the driver's cache of stage libraries, for %.0f s at most "
               "(NX2SARIA_PREWARM_FULL_S; 0 = no limit). NX2SARIA_PREWARM_FULL=0 leaves them to the background",
               cache_foreign ? "the pipeline cache was written by another device or driver" : "an earlier start began it",
               tries, BOOT_WARM_TRIES, secs > 0 ? secs : 0.0);
    }
    free(f);
}

static void prebuild_start(void) {
    plat_mutex_lock(&P.lock);
    if (K.file && !K.warm) {
        rt_log("vk_pipeline: prebuild off%s; %zu records not built",
               !cfg_bool("NX2SARIA_PIPE_PREBUILD") ? " (NX2SARIA_PIPE_PREBUILD=0)" : " (no boot prewarm)", K.file_n);
        free(K.file);
        K.file = NULL;
    }
    plat_mutex_unlock(&P.lock);
}

static uint32_t *spv_override(const uint32_t *code, uint64_t words, uint64_t *out) {
    const char *dir = cfg_get("NX2SARIA_SPV_OVERRIDE");
    if (!dir || !*dir) return NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < words * 4; i++) h = (h ^ ((const uint8_t *)code)[i]) * 0x100000001b3ull;
    char path[600];
    snprintf(path, sizeof path, "%s/%016llx.spv", dir, (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (w && fread(w, 1, (size_t)n, f) != (size_t)n) free(w), w = NULL;
    fclose(f);
    if (w) *out = (uint64_t)n / 4, rt_log("vk_pipeline: compute shader %016llx replaced by %s", (unsigned long long)h, path);
    return w;
}

static int fog_skip_block(const uint32_t *w, uint64_t n, uint64_t at, const uint64_t *def, uint32_t bound) {
    int writes = 0;
    for (uint64_t i = at + (w[at] >> 16); i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        if (op == 253) return writes == 5;
        if (op == 248 || op == 249 || op == 250) return 0;
        if (op == 99) {
            if (len < 4 || w[i + 3] >= bound || ++writes > 5) return 0;
            uint64_t c = def[w[i + 3]];
            if (!c || (w[c] & 0xffff) != 80 || w[c] >> 16 != 7) return 0;
            float v[4];
            for (int k = 0; k < 4; k++) {
                uint32_t id = w[c + 3 + k];
                uint64_t f = id < bound ? def[id] : 0;
                if (!f || (w[f] & 0xffff) != 43 || w[f] >> 16 != 4) return 0;
                memcpy(&v[k], &w[f + 3], 4);
            }
            if (writes == 5 ? !(v[0] > 0.0039f && v[0] < 0.0040f && v[1] == 0 && v[2] == 0 && v[3] == 0)
                            : !(v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 1)) return 0;
        }
        i += len;
    }
    return 0;
}

static uint32_t *spirv_fog_keep_froxels(const uint32_t *w, uint64_t n) {
    const char *e = cfg_get("NX2SARIA_FOG_CULL");
    if (n < 6 || (e && !strcmp(e, "1"))) return NULL;
    uint32_t bound = w[3];
    int writes = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t len = w[i] >> 16;
        if (!len || i + len > n) return NULL;
        writes += (w[i] & 0xffff) == 99;
        i += len;
    }
    if (writes < 5) return NULL;
    uint64_t *def = calloc(bound, sizeof *def);
    if (!def) return NULL;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == 248 && len >= 2 && w[i + 1] < bound) def[w[i + 1]] = i;
        else if ((op == 43 || op == 80 || op == 166 || op == 168 || op == 186) && len >= 3 && w[i + 2] < bound) def[w[i + 2]] = i;
        i += len;
    }
    uint64_t cmp = 0, prev = 0;
    int found = 0;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == 250 && len >= 4 && prev && (w[prev] & 0xffff) == 247 && w[prev + 1] == w[i + 3] && w[i + 1] < bound &&
            w[i + 3] < bound && def[w[i + 3]] && fog_skip_block(w, n, def[w[i + 3]], def, bound)) {
            uint64_t c = def[w[i + 1]];
            for (int k = 0; c && (w[c] & 0xffff) == 166 && k < 2; k++) {
                uint32_t a = w[c + 3 + k];
                uint64_t no = a < bound ? def[a] : 0;
                if (!no || (w[no] & 0xffff) != 168 || w[no + 3] >= bound) continue;
                uint64_t gt = def[w[no + 3]];
                if (gt && (w[gt] & 0xffff) == 186) cmp = gt, found++;
            }
        }
        prev = i;
        i += len;
    }
    free(def);
    uint32_t *o = found == 1 ? malloc(n * 4) : NULL;
    if (!o) return NULL;
    memcpy(o, w, n * 4);
    o[cmp + 4] = o[cmp + 3];
    LOG_ONCE("vk_pipeline: fog: froxels behind last frame's depth are computed too");
    return o;
}

typedef struct {
    uint8_t code[32];
    VkPipelineLayout layout;
    uint64_t words;
    VkPipelineCreateFlags flags;
    VkPipelineShaderStageCreateFlags stage_flags;
} ComputeWarmKey;
typedef struct ComputeWarmEntry {
    ComputeWarmKey key;
    struct ComputeWarmEntry *next;
} ComputeWarmEntry;
#define COMPUTE_WARM_BUCKETS 1024
static ComputeWarmEntry *compute_warm[COMPUTE_WARM_BUCKETS];
static PlatMutex compute_warm_lock = PLAT_MUTEX_INIT;
static uint64_t compute_warm_reused;

static int compute_warm_remember(const ComputeWarmKey *key, int completed) {
    uint64_t h = fnv(FNV0, key, sizeof *key);
    plat_mutex_lock(&compute_warm_lock);
    ComputeWarmEntry **head = &compute_warm[h % COMPUTE_WARM_BUCKETS], *e = *head;
    while (e && memcmp(&e->key, key, sizeof *key)) e = e->next;
    int have = e != NULL;
    if (!have && completed && (e = malloc(sizeof *e))) e->key = *key, e->next = *head, *head = e;
    plat_mutex_unlock(&compute_warm_lock);
    return have;
}

static void compute_warm_clear(void) {
    plat_mutex_lock(&compute_warm_lock);
    for (size_t i = 0; i < COMPUTE_WARM_BUCKETS; i++) {
        for (ComputeWarmEntry *e = compute_warm[i], *n; e; e = n) n = e->next, free(e);
        compute_warm[i] = NULL;
    }
    plat_mutex_unlock(&compute_warm_lock);
}

static VkPipeline compute_create(const GfxShader *cs, const Layout *l, int cached_only, int *already_warm) {
    if (already_warm) *already_warm = 0;
    uint64_t words;
    const uint32_t *code = gfx_shader_spirv_pin(cs, &words);
    uint64_t ow = 0;
    uint32_t *over = spv_override(code, words, &ow);
    if (over) code = over, words = ow;
    uint32_t *fog = spirv_fog_keep_froxels(code, words);
    if (fog) free(over), over = fog, code = fog;
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rqc;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main"},
                                      .layout = l->layout, .flags = noopt_flag()};
    if (cached_only) ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    pin_subgroup32(&ci.stage, &rqc, code, words);
    uint32_t lx = local_size_x(code, words);
    if (P.f_full_subgroups && vk_caps.sg_size && lx && lx % vk_caps.sg_size == 0 && uses_subgroup_ops(code, words))
        ci.stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    ComputeWarmKey warm_key = {0};
    const char *reuse = already_warm ? cfg_get("NX2SARIA_COMPUTE_WARM_REUSE") : NULL;
    int remember = already_warm && (!reuse || strcmp(reuse, "0"));
    if (remember) {
        rt_sha256(code, (size_t)words * 4, warm_key.code);
        warm_key.layout = l->layout, warm_key.words = words;
        warm_key.flags = ci.flags, warm_key.stage_flags = ci.stage.flags;
        if (compute_warm_remember(&warm_key, 0)) {
            *already_warm = 1;
            __atomic_fetch_add(&compute_warm_reused, 1, __ATOMIC_RELAXED);
            free(over);
            gfx_shader_spirv_unpin(cs);
            return VK_NULL_HANDLE;
        }
    }
    VkShaderModule m = make_module(code, words, 0);
    if (!m) return free(over), gfx_shader_spirv_unpin(cs), VK_NULL_HANDLE;
    ci.stage.module = m;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineCreationFeedbackCreateInfo fci;
    VkPipelineCreationFeedback fb;
    ci.pNext = fb_chain(&fci, &fb, ci.pNext);
    crash_note("a compute pipeline", cs, NULL, NULL);
    if (pipe_create(2, &ci, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    if (pipe && remember) compute_warm_remember(&warm_key, 1);
    plat_crash_context(NULL);
    if (pipe) fb_note(FB_COMPUTE, &fb);
    cc_hit = !pipe ? -1 : pb_last ? 1
           : !P.feedback || !P.cache || !(fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? -1
           : (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    if (P.feedback >= 2 && (pipe || !cached_only))
        rt_log("vk_pipeline: compute %016llx: %llu words, module %016llx, %s, %.1f ms", (unsigned long long)fnv(FNV0, code, words * 4),
               (unsigned long long)words, (unsigned long long)mm_hash,
               !pipe ? "failed" : pb_last ? "from pipeline-binaries.bin" : fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT ? "from pipeline-cache.bin" : "compiled",
               (double)fb.duration / 1e6);
    vkDestroyShaderModule(P.device, m, NULL);
    free(over);
    gfx_shader_spirv_unpin(cs);
    return pipe;
}

static Layout *compute_layout(const GfxShader *cs, Bindings *B, const char **why) {
    Layout *l = NULL;
    if (gfx_shader_stage(cs) != GFX_STAGE_CS) *why = "not a compute shader";
    else if (!add_stage(B, cs, 0)) *why = "too many bindings";
    else if (!(l = layout_get(B))) *why = "descriptor layout creation failed";
    return l;
}

#define LB_MAX_THREADS 16
#define LB_BIG_WORDS (512u * 1024)

static struct {
    GfxWarmEnt *e;
    size_t n;
    size_t n_keep;
    volatile int64_t next;
    volatile int32_t stop, running;
    volatile int32_t short_mem;
    int threads;
    PlatThread *th[LB_MAX_THREADS];
    uint8_t clamp, poly, minus_w, samples;
    double t0, ms[3];
    uint64_t built[3], have[3], fail[3], warmed[3], noshader, nolib, skipped, done, cs_draw;
    uint64_t big_words, deferred;
    uint64_t big_unused;
    double warm_ms[3];
    volatile int32_t kept;
    int32_t keep_n;
    int idle;
    int tail_one;
    int bg_threads;
    PlatMutex stat;
} LB = {.stat = PLAT_MUTEX_INIT};
static __thread int lb_me = -1;

static int lb_idle(void) {
    return LB.idle && !bf_waiting();
}

static size_t lb_next(int worker) {
    int64_t next = __atomic_load_n(&LB.next, __ATOMIC_RELAXED);
    for (;;) {
        int bg = lb_idle();
        if (LB.stop || (size_t)next >= LB.n || (bg && worker >= LB.bg_threads) ||
            (bg && LB.tail_one && worker > 0 && (size_t)next >= LB.n_keep)) return LB.n;
        if (__atomic_compare_exchange_n(&LB.next, &next, next + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return (size_t)next;
    }
}

#define JR_SLOT 64
#define JR_SLOTS 64

static struct {
    PlatFile *journal;
    PlatMutex jlock;
    char jpath[PLAT_PATH_MAX * 2 + 48];
    uint8_t (*iid)[32];
    size_t n;
    int loaded;
} SK = {.jlock = PLAT_MUTEX_INIT};

static int cache_dir(char *dir, size_t n) {
    snprintf(dir, n, "%s", P.cache_path);
    char *cut = plat_last_sep(dir);
    if (!cut) return 0;
    *cut = 0;
    return 1;
}

static void gpu_key(char *out, size_t n) {
    const VkPhysicalDeviceProperties *p = &vk_ctx()->props;
    snprintf(out, n, "%04x:%04x:%08x", p->vendorID, p->deviceID, p->driverVersion);
}

static const char *stage_name(int st) {
    return st == GFX_STAGE_VS ? "vertex" : st == GFX_STAGE_FS ? "fragment" : st == GFX_STAGE_CS ? "compute" : "other";
}

static int hex_digit(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int journal_fold(const uint8_t *j, uint64_t jn, const char *jp, const char *sp, const char *key, char **s,
                        uint64_t *sn) {
    static const char head[] =
        "# Shader variants the graphics driver was building when the process died during preparation\n"
        "# (runtime/vk_pipeline.c skip_init): input id, stage, GPU vendor:device:driver version. Preparation skips\n"
        "# them on that GPU and driver. Delete this file to build them again.\n";
    size_t cap = (*s ? *sn : sizeof head) + (jn / JR_SLOT + 1) * 128, len = 0;
    char *out = malloc(cap);
    int added = 0;
    if (!out) return 1;
    len = *s ? *sn : sizeof head - 1;
    memcpy(out, *s ? *s : head, len);
    if (len && out[len - 1] != '\n') out[len++] = '\n';
    for (uint64_t o = 0; o + 33 <= jn; o += JR_SLOT) {
        int set = 0;
        for (int b = 0; b < 32; b++) set |= j[o + b];
        if (!set) continue;
        char hex[65];
        for (int b = 0; b < 32; b++) snprintf(hex + 2 * b, 3, "%02x", j[o + b]);
        len += (size_t)snprintf(out + len, cap - len, "%s %s %s\n", hex, stage_name(j[o + 32]), key);
        added++;
    }
    out[len] = 0;
    if (added && plat_file_write_atomic(sp, out, len)) {
        rt_log("vk_pipeline: the last shader preparation, or the last start's warm on the boot screen, died while "
               "the driver built %d variants; %s now lists them and they are skipped ahead of play on this GPU and "
               "driver (%s)", added, sp, key);
        free(*s), *s = out, *sn = len;
        return 1;
    }
    if (added) rt_log("vk_pipeline: cannot write %s; %s stays for the next run", sp, jp);
    free(out);
    return !added;
}

static void skip_init(const char *dir) {
    if (SK.loaded) return;
    SK.loaded = 1;
    char jp[PLAT_PATH_MAX * 2 + 48], bj[PLAT_PATH_MAX * 2 + 48], sp[PLAT_PATH_MAX * 2 + 48], key[32];
    snprintf(jp, sizeof jp, "%s/shader-prepare-journal.bin", dir);
    snprintf(bj, sizeof bj, "%s/shader-warm-journal.bin", dir);
    snprintf(sp, sizeof sp, "%s/shader-prepare-skip.txt", dir);
    crash_path_init(dir);
    gpu_key(key, sizeof key);
    uint64_t jn = 0, bn = 0, sn = 0;
    uint8_t *j = plat_file_read(jp, &jn), *b = plat_file_read(bj, &bn);
    char *s = (char *)plat_file_read(sp, &sn);
    if (!j || journal_fold(j, jn, jp, sp, key, &s, &sn)) plat_file_delete(jp);
    if (b) {
        int marked = 0;
        for (uint64_t o = 0; o + 33 <= bn; o += JR_SLOT)
            for (int k = 0; k < 32; k++)
                if (b[o + k]) { marked++; break; }
        if (marked && !crash_noted()) {
            rt_log("vk_pipeline: the last start ended during the library warm on the boot screen, with %d variants in "
                   "the driver's hands, and reported no crash: taken for a start that was closed or stopped; no variant "
                   "is skipped for it", marked);
            plat_file_delete(bj);
        } else if (journal_fold(b, bn, bj, sp, key, &s, &sn)) {
            plat_file_delete(bj);
        }
    }
    free(j), free(b);
    for (char *line = s, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char hex[65], st[16], k[32];
        uint8_t iid[32];
        int ok = line[0] != '#' && sscanf(line, "%64s %15s %31s", hex, st, k) == 3 && strlen(hex) == 64 &&
                 !strcmp(k, key);
        for (int b2 = 0; ok && b2 < 32; b2++) {
            int hi = hex_digit(hex[2 * b2]), lo = hex_digit(hex[2 * b2 + 1]);
            if (hi < 0 || lo < 0) ok = 0;
            else iid[b2] = (uint8_t)(hi << 4 | lo);
        }
        uint8_t (*grown)[32] = ok ? realloc(SK.iid, (SK.n + 1) * sizeof *SK.iid) : NULL;
        if (grown) memcpy(grown[SK.n++], iid, 32), SK.iid = grown;
    }
    free(s);
    if (SK.n) rt_log("vk_pipeline: %zu shader variants skipped ahead of play on this GPU and driver (%s)", SK.n, sp);
}

static int skip_listed(const uint8_t iid[32]) {
    for (size_t i = 0; i < SK.n; i++)
        if (!memcmp(SK.iid[i], iid, 32)) return 1;
    return 0;
}

static void skip_count(void) {
    plat_mutex_lock(&LB.stat);
    LB.skipped++;
    plat_mutex_unlock(&LB.stat);
}

static void journal_mark(int slot, const GfxWarmEnt *e) {
    if (slot < 0 || slot >= JR_SLOTS) return;
    uint8_t rec[JR_SLOT] = {0};
    if (e) memcpy(rec, e->iid, 32), rec[32] = e->stage;
    plat_mutex_lock(&SK.jlock);
    if (SK.journal) plat_file_pwrite(SK.journal, rec, sizeof rec, (uint64_t)slot * JR_SLOT);
    plat_mutex_unlock(&SK.jlock);
}

static void journal_open(void) {
    char dir[PLAT_PATH_MAX * 2];
    if (!cache_dir(dir, sizeof dir)) return;
    crash_path_init(dir);
    snprintf(SK.jpath, sizeof SK.jpath, "%s/shader-warm-journal.bin", dir);
    PlatFile *j = plat_fs_create(SK.jpath, NULL);
    if (!j) {
        rt_log("vk_pipeline: cannot create %s: a driver crash in this start's library warm will not be recorded", SK.jpath);
        SK.jpath[0] = 0;
        return;
    }
    plat_mutex_lock(&SK.jlock);
    SK.journal = j;
    plat_mutex_unlock(&SK.jlock);
    __atomic_store_n(&jr_boot_on, 1, __ATOMIC_RELEASE);
}

static void journal_close(void) {
    if (!SK.jpath[0]) return;
    __atomic_store_n(&jr_boot_on, 0, __ATOMIC_RELEASE);
    plat_mutex_lock(&SK.jlock);
    PlatFile *j = SK.journal;
    SK.journal = NULL;
    plat_mutex_unlock(&SK.jlock);
    if (!j) return;
    plat_file_close(j);
    plat_file_delete(SK.jpath);
}

#define CS_FLIGHT 32
static const GfxShader *cs_flight[CS_FLIGHT];

static int cs_flight_has(const GfxShader *cs) {
    for (int i = 0; i < CS_FLIGHT; i++)
        if (cs_flight[i] == cs) return 1;
    return 0;
}

static volatile uint8_t cs_flight_want[CS_FLIGHT];
static PlatThread *cs_flight_th[CS_FLIGHT];

static int cs_flight_add(const GfxShader *cs, PlatThread *th) {
    for (int i = 0; i < CS_FLIGHT; i++)
        if (!cs_flight[i]) return cs_flight[i] = cs, cs_flight_want[i] = 0, cs_flight_th[i] = th, 1;
    return 0;
}

static PlatThread *cs_flight_wanted(const GfxShader *cs) {
    PlatThread *th = NULL;
    for (int i = 0; i < CS_FLIGHT; i++)
        if (cs_flight[i] == cs && !cs_flight_want[i]) cs_flight_want[i] = 1, th = cs_flight_th[i];
    return th;
}

static int cs_flight_is_wanted(const GfxShader *cs) {
    for (int i = 0; i < CS_FLIGHT; i++)
        if (__atomic_load_n(&cs_flight[i], __ATOMIC_RELAXED) == cs) return cs_flight_want[i];
    return 0;
}

static int cs_flight_remove(const GfxShader *cs) {
    int want = 0;
    for (int i = 0; i < CS_FLIGHT; i++)
        if (cs_flight[i] == cs) cs_flight[i] = NULL, want |= cs_flight_want[i];
    return want;
}

#define CU_VERSION 1
typedef struct { char magic[8]; uint32_t version, rec_size, run, pad; } CuHeader;
typedef struct { uint8_t iid[32]; uint32_t run, pad; } CuRec;
static struct {
    PlatMutex lock;
    int opened, dirty;
    char path[PLAT_PATH_MAX * 2 + 32];
    CuRec *rec;
    uint8_t *keep;
    uint32_t n, cap, n_file;
    uint32_t *slot;
    uint32_t slot_cap;
    uint32_t run;
    int quiet;
    int32_t keep_n;
    uint32_t kept;
} CU = {.lock = PLAT_MUTEX_INIT};

#define CF_WINDOW_MS 33.0
static struct {
    uint64_t n, hit, pre_used;
    double ms, max_ms, hit_ms;
    double win_t0, win_ms, worst_ms;
    uint32_t win_n, worst_n;
} CF;

static uint32_t *cu_slot(const uint8_t iid[32]) {
    uint64_t h;
    memcpy(&h, iid, 8);
    for (uint32_t j = (uint32_t)(h * 0x9e3779b97f4a7c15ull >> 32) & (CU.slot_cap - 1);; j = (j + 1) & (CU.slot_cap - 1))
        if (!CU.slot[j] || !memcmp(CU.rec[CU.slot[j] - 1].iid, iid, 32)) return &CU.slot[j];
}

static int cu_room(void) {
    if (CU.n == CU.cap) {
        uint32_t cap = CU.cap ? CU.cap * 2 : 1024;
        CuRec *t = realloc(CU.rec, cap * sizeof *t);
        if (!t) return 0;
        CU.rec = t, CU.cap = cap;
    }
    if ((CU.n + 1) * 2 > CU.slot_cap) {
        uint32_t cap = CU.slot_cap ? CU.slot_cap * 2 : 4096;
        uint32_t *t = calloc(cap, sizeof *t), *old = CU.slot;
        if (!t) return 0;
        CU.slot = t, CU.slot_cap = cap;
        for (uint32_t i = 0; i < CU.n; i++) *cu_slot(CU.rec[i].iid) = i + 1;
        free(old);
    }
    return 1;
}

static int cu_run_cmp(const void *a, const void *b) {
    const uint32_t *x = a, *y = b;
    return x[0] != y[0] ? (x[0] < y[0] ? 1 : -1) : (x[1] > y[1]) - (x[1] < y[1]);
}

static long cu_parse(const uint8_t *f, uint64_t size, CuHeader *h) {
    if (!f || size < sizeof *h) return -1;
    memcpy(h, f, sizeof *h);
    if (memcmp(h->magic, "NX2NCUSE", 8) || h->version != CU_VERSION || h->rec_size != sizeof(CuRec)) return -1;
    return (long)((size - sizeof *h) / sizeof(CuRec));
}

static void cu_read(void) {
    const char *e = cfg_value("NX2SARIA_COMPUTE_KEEP");
    plat_mutex_lock(&CU.lock);
    int count = cfg_int("NX2SARIA_COMPUTE_KEEP");
    CU.keep_n = !strcmp(e, "all") ? -1 : count < 0 ? 0 : count;
    uint64_t size = 0;
    uint8_t *f = plat_file_read(CU.path, &size);
    CuHeader h;
    long n = cu_parse(f, size, &h);
    if (n >= 0) {
        for (long i = 0; i < n && cu_room(); i++) {
            CuRec r;
            memcpy(&r, f + sizeof h + (size_t)i * sizeof r, sizeof r);
            uint32_t *s = cu_slot(r.iid);
            if (*s) continue;
            CU.rec[CU.n] = r, *s = ++CU.n;
        }
        CU.run = h.run;
    } else if (f) {
        rt_log("vk_pipeline: %s has another format or version; it is rewritten", CU.path);
    }
    free(f);
    CU.n_file = CU.n, CU.run++;
    uint32_t (*order)[2] = CU.n ? malloc(CU.n * sizeof *order) : NULL;
    CU.keep = CU.n ? calloc(CU.n, 1) : NULL;
    if (order && CU.keep) {
        for (uint32_t i = 0; i < CU.n; i++) order[i][0] = CU.rec[i].run, order[i][1] = i;
        qsort(order, CU.n, sizeof *order, cu_run_cmp);
        for (uint32_t i = 0; i < CU.n && (CU.keep_n < 0 || i < (uint32_t)CU.keep_n); i++) CU.keep[order[i][1]] = 1, CU.kept++;
    }
    free(order);
    CU.opened = 1;
    rt_log("vk_pipeline: %s: %u compute shaders dispatched in earlier runs; the boot keeps the pipelines of %s%u "
           "dispatched most recently (NX2SARIA_COMPUTE_KEEP %s)", CU.path, CU.n,
           CU.keep_n < 0 ? "every recorded compute shader, among them the " : "the ", CU.kept, e);
    plat_mutex_unlock(&CU.lock);
}

static void cu_open(void) {
    if (!A.on || !K.path[0]) return;
    snprintf(CU.path, sizeof CU.path, "%s", K.path);
    char *cut = plat_last_sep(CU.path);
    snprintf(cut ? cut + 1 : CU.path, sizeof CU.path - (size_t)(cut ? cut + 1 - CU.path : 0), "compute-used.bin");
    cu_read();
}

static void cu_open_prep(const char *dir) {
    if (CU.opened) return;
    CU.quiet = 1;
    snprintf(CU.path, sizeof CU.path, "%s/compute-used.bin", dir);
    cu_read();
}

static int cu_keep(const uint8_t iid[32]) {
    plat_mutex_lock(&CU.lock);
    int keep = CU.keep_n < 0 || !CU.opened;
    if (!keep && CU.slot_cap) {
        uint32_t s = *cu_slot(iid);
        keep = s && s <= CU.n_file && CU.keep && CU.keep[s - 1];
    }
    plat_mutex_unlock(&CU.lock);
    return keep;
}

static void cu_note(const GfxShader *cs) {
    static const uint8_t none[32];
    const uint8_t *iid = gfx_shader_input_id(cs);
    if (!CU.opened || !memcmp(iid, none, 32)) return;
    plat_mutex_lock(&CU.lock);
    if (cu_room()) {
        uint32_t *s = cu_slot(iid);
        if (!*s) {
            memset(&CU.rec[CU.n], 0, sizeof *CU.rec);
            memcpy(CU.rec[CU.n].iid, iid, 32);
            *s = ++CU.n;
        }
        if (CU.rec[*s - 1].run != CU.run) CU.rec[*s - 1].run = CU.run, CU.dirty = 1;
    }
    plat_mutex_unlock(&CU.lock);
}

static PlatMutex cu_io = PLAT_MUTEX_INIT;

static int cu_write(const char *path) {
    plat_mutex_lock(&cu_io);
    plat_mutex_lock(&CU.lock);
    uint32_t n = CU.n;
    plat_mutex_unlock(&CU.lock);
    size_t cap = sizeof(CuHeader) + ((size_t)n + 256) * sizeof(CuRec), size = 0;
    uint8_t *out = malloc(cap);
    plat_mutex_lock(&CU.lock);
    CuHeader h = {{'N', 'X', '2', 'N', 'C', 'U', 'S', 'E'}, CU_VERSION, sizeof(CuRec), CU.run, 0};
    if (out && sizeof h + CU.n * sizeof(CuRec) <= cap) {
        size = sizeof h + CU.n * sizeof(CuRec);
        memcpy(out, &h, sizeof h);
        if (CU.n) memcpy(out + sizeof h, CU.rec, CU.n * sizeof(CuRec));
        CU.dirty = 0;
    }
    plat_mutex_unlock(&CU.lock);
    int ok = size && plat_file_write_atomic(path, out, size);
    if (!ok) {
        plat_mutex_lock(&CU.lock);
        CU.dirty = 1;
        plat_mutex_unlock(&CU.lock);
    }
    free(out);
    plat_mutex_unlock(&cu_io);
    return ok;
}

static void cu_flush(void) {
    if (!CU.opened || CU.quiet || !*(volatile int *)&CU.dirty) return;
    if (!cu_write(CU.path)) LOG_ONCE("vk_pipeline: cannot write %s; the next saves try again", CU.path);
}

static void cf_note(double ms, int hit) {
    double now = now_ms();
    CF.n++, CF.ms += ms;
    if (ms > CF.max_ms) CF.max_ms = ms;
    if (hit == 1) CF.hit++, CF.hit_ms += ms;
    if (now - CF.win_t0 > CF_WINDOW_MS) CF.win_t0 = now, CF.win_n = 0, CF.win_ms = 0;
    CF.win_n++, CF.win_ms += ms;
    if (CF.win_ms > CF.worst_ms) CF.worst_ms = CF.win_ms, CF.worst_n = CF.win_n;
}

static void cf_log(void) {
    plat_mutex_lock_shared(&P.lock);
    typeof(CF) c = CF;
    plat_mutex_unlock_shared(&P.lock);
    if (c.n || c.pre_used)
        rt_log("vk_pipeline: compute pipelines created at their first dispatch: %llu (avg %.2f ms, longest %.2f ms; the "
               "pipeline cache held %llu of them, avg %.2f ms); the %.0f ms that held the most of it: %u pipelines, "
               "%.1f ms; of the compute pipelines kept from the boot, %llu were dispatched",
               (unsigned long long)c.n, c.n ? c.ms / c.n : 0.0, c.max_ms, (unsigned long long)c.hit,
               c.hit ? c.hit_ms / c.hit : 0.0, CF_WINDOW_MS, c.worst_n, c.worst_ms, (unsigned long long)c.pre_used);
}

static struct {
    const GfxShader **cs;
    uint8_t (*iid)[32];
    uint64_t *words;
    size_t n, cap;
    PlatThread *th;
    const GfxShader *volatile building;
    volatile int32_t raised;
    uint64_t built, failed, there;
    double ms;
} BG;

VkPipeline vk_pipeline_compute(const GfxShader *cs, const VkPipeInfo **info) {
    if (info) *info = NULL;
    if (!cs || !ensure_init()) return VK_NULL_HANDLE;
    struct { const GfxShader *cs; uint64_t compute; } k = {cs, 1};
    uint64_t h = key_hash(&k, sizeof k);
    plat_mutex_lock(&P.lock);
    Pipe *p = pipe_find(&k, sizeof k, h);
    double w0 = !p && cs_flight_has(cs) ? now_ms() : 0;
    int bg = 0;
    while (!p && cs_flight_has(cs)) {
        PlatThread *builder = cs_flight_wanted(cs);
        if (builder) plat_thread_set_priority(builder, PLAT_PRIO_NORMAL);
        if (BG.building == cs && BG.th && !plat_xchg32(&BG.raised, 1))
            plat_thread_set_priority(BG.th, PLAT_PRIO_NORMAL), bg = 1;
        plat_mutex_unlock(&P.lock);
        plat_sleep_ms(1);
        plat_mutex_lock(&P.lock);
        p = pipe_find(&k, sizeof k, h);
    }
    if (w0 && now_ms() - w0 >= 2.0) {
        static int logs;
        if (logs++ < 16)
            rt_log("vk_exec: a dispatch waited %.0f ms for its compute pipeline (%s)", now_ms() - w0,
                   bg ? "built on the background thread" : "built by another thread");
    }
    if (p) {
        P.stats.hits++;
    } else {
        const char *why = NULL;
        Bindings B = {0};
        Layout *l = compute_layout(cs, &B, &why);
        VkPipeline pipe = VK_NULL_HANDLE;
        if (!why) {
            int flight = cs_flight_add(cs, NULL);
            plat_mutex_unlock(&P.lock);
            uint64_t p0 = perf_begin();
            double c0 = now_ms();
            pipe = compute_create(cs, l, 0, NULL);
            double cms = now_ms() - c0;
            int hit = cc_hit;
            perf_end(PERF_PIPE, p0);
            plat_mutex_lock(&P.lock);
            if (flight) cs_flight_remove(cs);
            if (!pipe) why = "pipeline creation failed";
            else cf_note(cms, hit);
        }
        if ((p = pipe_find(&k, sizeof k, h))) {
            if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        } else {
            if (why) {
                P.stats.failures++;
                rt_log("vk_pipeline: compute pipeline not built: %s", why);
            } else {
                P.stats.compute++;
                LB.cs_draw++;
            }
            p = pipe_add(&k, sizeof k, h, pipe, l, l ? &B : NULL, VK_PIPELINE_BIND_POINT_COMPUTE, 0);
        }
    }
    if (p && !p->used) {
        p->used = 1;
        CF.pre_used += p->pre;
        if (p->info.pipeline) cu_note(cs);
    }
    if (p) pipe_used(p);
    plat_mutex_unlock(&P.lock);
    if (!p) return VK_NULL_HANDLE;
    if (info) *info = &p->info;
    return p->info.pipeline;
}

static void lb_lock(void) {
    plat_thread_priority(PLAT_PRIO_NORMAL);
    plat_mutex_lock(&P.lock);
}

static void lb_unlock(void) {
    plat_mutex_unlock(&P.lock);
    if (lb_idle()) plat_thread_priority(PLAT_PRIO_IDLE);
}

static void lb_count(int kind, int what, double ms) {
    plat_mutex_lock(&LB.stat);
    if (what == 3) LB.warmed[kind]++, LB.warm_ms[kind] += ms;
    else (what == 0 ? LB.built : what == 1 ? LB.have : LB.fail)[kind]++, LB.ms[kind] += ms;
    plat_mutex_unlock(&LB.stat);
}

static uint32_t vs_union_peek(const GfxShader *vs, GfxShaderIo fin[32]) {
    GfxShaderIo vout[32];
    uint32_t need = 0, have = gfx_shader_outputs(vs, vout), missing = 0;
    memset(fin, 0, 32 * sizeof *fin);
    plat_mutex_lock_shared(&VO.lock);
    uint32_t j = (uint32_t)(((uintptr_t)vs >> 4) * 0x9e3779b1u) & (VS_OUTS_CAP - 1);
    while (VO.vs[j] && VO.vs[j] != vs) j = (j + 1) & (VS_OUTS_CAP - 1);
    if (VO.vs[j]) need = VO.mask[j], memcpy(fin, VO.io[j], 32 * sizeof *fin);
    plat_mutex_unlock_shared(&VO.lock);
    if (P.vs_outs_all)
        for (int i = 0; i < 32; i++)
            if (!(need >> i & 1)) fin[i] = (GfxShaderIo){GFX_TYPE_FLOAT, 4, 0}, need |= 1u << i;
    for (int i = 0; i < 32; i++)
        if (need >> i & 1 && !(have >> i & 1)) missing |= 1u << i;
    return missing;
}

static void lb_defer(const uint8_t iid[32], uint64_t words) {
    plat_mutex_lock(&LB.stat);
    LB.deferred++;
    plat_mutex_unlock(&LB.stat);
    rt_log("vk_pipeline: stage library prebuild: compute shader %02x%02x%02x%02x%02x%02x%02x%02x (%llu SPIR-V words) %s: "
           "left to its first dispatch (NX2SARIA_LIB_PREBUILD_BIG %llu)", iid[0], iid[1], iid[2], iid[3], iid[4], iid[5],
           iid[6], iid[7], (unsigned long long)words,
           P.cache_control ? "not in the pipeline caches" : "too large to build without a cache probe (no pipelineCreationCacheControl)",
           (unsigned long long)LB.big_words);
}

static int bg_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("NX2SARIA_LIB_PREBUILD_BIG_BG");
    return on;
}

static void bg_queue(const GfxShader *cs, const uint8_t iid[32], uint64_t words) {
    plat_mutex_lock(&LB.stat);
    if (BG.n == BG.cap) {
        size_t cap = BG.cap ? BG.cap * 2 : 16;
        const GfxShader **c = realloc(BG.cs, cap * sizeof *c);
        if (c) BG.cs = c;
        uint8_t (*i)[32] = realloc(BG.iid, cap * sizeof *i);
        if (i) BG.iid = i;
        uint64_t *w = realloc(BG.words, cap * sizeof *w);
        if (w) BG.words = w;
        if (c && i && w) BG.cap = cap;
    }
    int ok = BG.n < BG.cap;
    if (ok) BG.cs[BG.n] = cs, memcpy(BG.iid[BG.n], iid, 32), BG.words[BG.n] = words, BG.n++;
    plat_mutex_unlock(&LB.stat);
    if (!ok) {
        lb_defer(iid, words);
        return;
    }
    rt_log("vk_pipeline: stage library prebuild: compute shader %02x%02x%02x%02x%02x%02x%02x%02x (%llu SPIR-V words) not in "
           "the pipeline caches: built on the background thread after the prebuild (NX2SARIA_LIB_PREBUILD_BIG_BG)",
           iid[0], iid[1], iid[2], iid[3], iid[4], iid[5], iid[6], iid[7], (unsigned long long)words);
}

static int bg_room(double *used, double *limit, double *held) {
    static double need = -1;
    if (need < 0) need = strtod(cfg_value("NX2SARIA_BIG_BUILD_FREE_MB"), NULL);
    double proc;
    plat_commit_info(&proc, used, limit);
    if (!(need <= 0 || *limit < 0 || *limit - *used >= need)) return 0;
    return gate_enter(GATE_BIG_MB, held);
}

static double bg_growth_cap(void) {
    static double cap = -1;
    if (cap < 0) cap = strtod(cfg_value("NX2SARIA_BIG_BUILD_MAX_MB"), NULL);
    return cap;
}

static void bg_merge(VkPipelineCache own, int built) {
    int locked = 0;
    while (built && !LB.stop && !(locked = plat_mutex_trylock(&cache_rw))) plat_sleep_ms(2);
    if (locked) {
        VkResult r = vkMergePipelineCaches(P.device, P.play ? P.play : P.cache, 1, &own);
        plat_mutex_unlock(&cache_rw);
        if (r == VK_SUCCESS) __atomic_fetch_add(&cache_merges, 1, __ATOMIC_RELAXED);
        else rt_log("vk_pipeline: background compute build: its cache did not merge into %s (%d)",
                    P.play ? P.play_path : P.cache_path, (int)r);
    }
    vkDestroyPipelineCache(P.device, own, NULL);
}

static void bg_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    plat_mutex_lock(&LB.stat);
    for (size_t i = 1; i < BG.n; i++)
        for (size_t j = i; j > 0 && BG.words[j] < BG.words[j - 1]; j--) {
            const GfxShader *c = BG.cs[j];
            BG.cs[j] = BG.cs[j - 1], BG.cs[j - 1] = c;
            uint64_t w = BG.words[j];
            BG.words[j] = BG.words[j - 1], BG.words[j - 1] = w;
            uint8_t t[32];
            memcpy(t, BG.iid[j], 32), memcpy(BG.iid[j], BG.iid[j - 1], 32), memcpy(BG.iid[j - 1], t, 32);
        }
    plat_mutex_unlock(&LB.stat);
    double t_all = now_ms();
    double c_start, c_used0, c_limit0;
    plat_commit_info(&c_start, &c_used0, &c_limit0);
    for (size_t i = 0; !LB.stop; i++) {
        uint8_t idbuf[32];
        uint64_t words = 0;
        const GfxShader *cs;
        plat_mutex_lock(&LB.stat);
        int have = i < BG.n;
        if (have) {
            cs = BG.cs[i];
            memcpy(idbuf, BG.iid[i], 32);
            words = BG.words[i];
        }
        plat_mutex_unlock(&LB.stat);
        if (!have) break;
        const uint8_t *iid = idbuf;
        struct { const GfxShader *cs; uint64_t compute; } k = {cs, 1};
        uint64_t h = key_hash(&k, sizeof k);
        Bindings B = {0};
        const char *why = NULL;
        double used, limit, c_now;
        plat_commit_info(&c_now, &used, &limit);
        double cap = bg_growth_cap();
        if (cap > 0 && c_now - c_start >= cap) {
            rt_log("vk_pipeline: background compute build: stopping, the process grew %.0f MB in all (NX2SARIA_BIG_BUILD_MAX_MB "
                   "%.0f); the shaders left build at their first dispatch", c_now - c_start, cap);
            break;
        }
        int waited = 0, no_room = 0;
        double w0 = now_ms(), held = -1;
        while (!LB.stop && !bg_room(&used, &limit, &held)) {
            held = -1;
            if (!waited++)
                rt_log("vk_pipeline: background compute build: waiting for commit headroom (system %.0f of %.0f MB, "
                       "NX2SARIA_BIG_BUILD_FREE_MB %s)", used, limit, cfg_value("NX2SARIA_BIG_BUILD_FREE_MB"));
            if (now_ms() - w0 > 30000.0) {
                rt_log("vk_pipeline: background compute build: no commit headroom after %.0f s; the shaders left build at "
                       "their first dispatch", (now_ms() - w0) / 1000.0);
                no_room = 1;
                break;
            }
            plat_sleep_ms(2000);
        }
        if (LB.stop || no_room) {
            if (held >= 0 && !no_room) gate_leave(held);
            break;
        }
        plat_mutex_lock(&P.lock);
        int there = cs_flight_has(cs) || pipe_find(&k, sizeof k, h);
        Layout *l = there ? NULL : compute_layout(cs, &B, &why);
        int flight = l ? cs_flight_add(cs, NULL) : 0;
        if (l) BG.building = cs;
        plat_mutex_unlock(&P.lock);
        if (!l) {
            if (held >= 0) gate_leave(held);
            if (there) BG.there++;
            else BG.failed++, rt_log("vk_pipeline: background compute build: %s", why ? why : "no layout");
            continue;
        }
        VkPipelineCache own = VK_NULL_HANDLE;
        VkPipelineCacheCreateInfo oci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (P.cache && vkCreatePipelineCache(P.device, &oci, NULL, &own) != VK_SUCCESS) own = VK_NULL_HANDLE;
        double t0 = now_ms();
        own_cache = own;
        VkPipeline pipe = compute_create(cs, l, 0, NULL);
        own_cache = VK_NULL_HANDLE;
        double ms = now_ms() - t0;
        if (held >= 0) gate_leave(held);
        plat_mutex_lock(&P.lock);
        if (flight) cs_flight_remove(cs);
        BG.building = NULL;
        Pipe *e = NULL;
        if (pipe && !pipe_find(&k, sizeof k, h) &&
            (e = pipe_add(&k, sizeof k, h, pipe, l, &B, VK_PIPELINE_BIND_POINT_COMPUTE, 0))) {
            e->pre = 1;
            P.stats.compute++;
        } else if (pipe) {
            vkDestroyPipeline(P.device, pipe, NULL);
        }
        plat_mutex_unlock(&P.lock);
        if (own) bg_merge(own, pipe != VK_NULL_HANDLE);
        if (plat_xchg32(&BG.raised, 0)) plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
        if (pipe) BG.built++, BG.ms += ms;
        else BG.failed++;
        double c_proc, c_used, c_limit;
        plat_commit_info(&c_proc, &c_used, &c_limit);
        rt_log("vk_pipeline: background compute build: %02x%02x%02x%02x%02x%02x%02x%02x (%llu SPIR-V words) %s in %.0f ms; "
               "commit: process %.0f MB, system %.0f of %.0f MB", iid[0], iid[1], iid[2], iid[3], iid[4], iid[5], iid[6],
               iid[7], (unsigned long long)words, pipe ? "built" : "failed", ms, c_proc, c_used, c_limit);
    }
    rt_log("vk_pipeline: background compute builds %s: %llu built (%.0f ms in all), %llu failed, %llu dispatched before "
           "their turn; %.0f ms after the prebuild", LB.stop ? "stopped" : "done", (unsigned long long)BG.built, BG.ms,
           (unsigned long long)BG.failed, (unsigned long long)BG.there, now_ms() - t_all);
}

static void bg_start(void) {
    if (!BG.n || LB.stop || BG.th) return;
    if (!(BG.th = plat_thread_new(bg_main, NULL, "big compute")))
        rt_log("vk_pipeline: cannot start the background compute builder; %zu large compute shaders build at their first "
               "dispatch", BG.n);
}

static void lb_big_words_init(void) {
    const char *be = cfg_get("NX2SARIA_LIB_PREBUILD_BIG");
    LB.big_words = be && *be ? strtoull(be, NULL, 10) : LB_BIG_WORDS;
}

static int lb_big_left_out(uint64_t words, int keep) {
    return LB.big_words && words > LB.big_words && !keep;
}

static void lb_compute(const GfxShader *cs, const uint8_t iid[32], int keep, int private_shader) {
    struct { const GfxShader *cs; uint64_t compute; } k = {cs, 1};
    uint64_t h = key_hash(&k, sizeof k);
    Bindings B = {0};
    const char *why = NULL;
    uint64_t words = gfx_shader_words(cs);
    int big = LB.big_words && words > LB.big_words;
    if (lb_big_left_out(words, keep)) {
        plat_mutex_lock(&LB.stat);
        LB.big_unused++;
        plat_mutex_unlock(&LB.stat);
        return;
    }
    if (big && !P.cache_control) {
        if (bg_on()) bg_queue(cs, iid, words);
        else lb_defer(iid, words);
        return;
    }
    lb_lock();
    int busy = cs_flight_has(cs);
    Layout *l = busy || pipe_find(&k, sizeof k, h) ? NULL : compute_layout(cs, &B, &why);
    PlatThread *me = LB.idle && lb_me >= 0 ? LB.th[lb_me] : NULL;
    int have = !l && !why, flight = l ? cs_flight_add(cs, me) : 0;
    lb_unlock();
    if (!l) {
        lb_count(2, have ? 1 : 2, 0);
        return;
    }
    double t0 = now_ms();
    if (lb_idle() && flight && me) {
        plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
        if (cs_flight_is_wanted(cs)) plat_thread_priority(PLAT_PRIO_NORMAL);
    } else if (lb_idle()) {
        plat_thread_priority(PLAT_PRIO_NORMAL);
    }
    int reused = 0;
    VkPipeline pipe = compute_create(cs, l, big, !keep && private_shader ? &reused : NULL);
    double ms = now_ms() - t0;
    int compiled = pipe && cc_hit != 1, kept = 0;
    lb_lock();
    if (flight && cs_flight_remove(cs)) keep = 1;
    Pipe *e = NULL;
    if (pipe && pipe_find(&k, sizeof k, h)) {
        vkDestroyPipeline(P.device, pipe, NULL);
        have = 1;
    } else if (pipe && keep && (e = pipe_add(&k, sizeof k, h, pipe, l, &B, VK_PIPELINE_BIND_POINT_COMPUTE, 0))) {
        e->pre = 1, kept = 1;
        P.stats.compute++;
    } else if (pipe) {
        vkDestroyPipeline(P.device, pipe, NULL);
        if (compiled) __atomic_fetch_add(&cache_merges, 1, __ATOMIC_RELAXED);
    }
    lb_unlock();
    if (!pipe && big && bg_on()) bg_queue(cs, iid, words);
    else if (!pipe && big) lb_defer(iid, words);
    else lb_count(2, reused ? 1 : !pipe ? 2 : have ? 1 : kept ? 0 : 3, ms);
}

static void lb_library(const GfxShader *sh, int vertex, int keep) {
    int kind = vertex ? LIB_PR : LIB_FS, stat = vertex ? 0 : 1;
    VkPipeState s;
    vk_pipe_state_defaults(&s);
    GKey k;
    memset(&k, 0, sizeof k);
    k.clamp = LB.clamp, k.poly_mode = LB.poly, k.minus_w = LB.minus_w, k.samples = LB.samples;
    PrKey pk;
    FsKey fk;
    memset(&pk, 0, sizeof pk), memset(&fk, 0, sizeof fk);
    const void *key;
    uint32_t size;
    if (vertex) {
        GfxShaderIo fin[32];
        s.vs = sh;
        pk.kind = LIB_PR, pk.vs = sh, pk.clamp = k.clamp, pk.poly_mode = k.poly_mode, pk.minus_w = k.minus_w;
        pk.missing = vs_union_peek(sh, fin);
        for (int i = 0; i < 32; i++)
            if (pk.missing >> i & 1) pk.fin[i] = fin[i];
        key = &pk, size = sizeof pk;
    } else {
        s.fs = sh;
        fk.kind = LIB_FS, fk.fs = sh, fk.samples = k.samples;
        key = &fk, size = sizeof fk;
    }
    uint64_t h = key_hash(key, size);
    Bindings B = {0}, F = {0};
    VkPipelineLayout lay = VK_NULL_HANDLE;
    lb_lock();
    int have = keep && lib_find(h, key, size) != NULL;
    if (!have && add_stage(vertex ? &B : &F, sh, 0)) {
        Layout *l = layout_pair(&B, &F);
        if (l) lay = half_layout(l, vertex ? 0 : 1);
    }
    lb_unlock();
    if (have || !lay) {
        lb_count(stat, have ? 1 : 2, 0);
        return;
    }
    double t0 = now_ms();
    VkPipeline pipe = lib_create(kind, &s, &k, lay, key);
    double ms = now_ms() - t0;
    if (!keep) {
        if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
        lb_count(stat, pipe ? 3 : 2, ms);
        return;
    }
    lb_lock();
    have = lib_find(h, key, size) != NULL;
    VkPipeline got = lib_put(h, key, size, pipe);
    if (!have && pipe) {
        Lib *b = lib_find(h, key, size);
        if (b) b->pre = 1;
    }
    lb_unlock();
    lb_count(stat, !pipe ? 2 : have || got != pipe ? 1 : 0, ms);
}

static void lb_skip_lib(void) {
    plat_mutex_lock(&LB.stat);
    LB.nolib++;
    plat_mutex_unlock(&LB.stat);
}

static void lb_report(const char *how) {
    float mem_used, mem_total, mem_proc;
    plat_mem_info(&mem_used, &mem_total, &mem_proc);
    plat_mutex_lock_shared(&LB.stat);
    rt_log("vk_pipeline: stage library prebuild %s: %llu of %zu variants in %.0f ms; kept: vertex %llu built (avg %.1f ms), "
           "%llu there; fragment %llu built (avg %.1f ms), %llu there; compute %llu built (avg %.1f ms), %llu there; driver "
           "cache only: vertex %llu (avg %.1f ms), fragment %llu (avg %.1f ms), compute %llu (avg %.1f ms); %llu failed, %llu not in the shader cache, "
           "%llu vertex/fragment skipped (graphics pipeline library off), %llu skipped (the driver crashed on them before, "
           "shader-prepare-skip.txt), %llu large compute shaders left to their first dispatch (NX2SARIA_LIB_PREBUILD_BIG), %zu "
           "queued for the background builder (NX2SARIA_LIB_PREBUILD_BIG_BG), %llu large compute shaders not built because "
           "no run has dispatched them; %llu compute requests the same as one built before, not built again "
           "(NX2SARIA_COMPUTE_WARM_REUSE); process working set %.0f MB", how, (unsigned long long)LB.done, LB.n,
           now_ms() - LB.t0, (unsigned long long)LB.built[0], LB.built[0] ? LB.ms[0] / LB.built[0] : 0.0,
           (unsigned long long)LB.have[0], (unsigned long long)LB.built[1], LB.built[1] ? LB.ms[1] / LB.built[1] : 0.0,
           (unsigned long long)LB.have[1], (unsigned long long)LB.built[2], LB.built[2] ? LB.ms[2] / LB.built[2] : 0.0,
           (unsigned long long)LB.have[2], (unsigned long long)LB.warmed[0], LB.warmed[0] ? LB.warm_ms[0] / LB.warmed[0] : 0.0,
           (unsigned long long)LB.warmed[1], LB.warmed[1] ? LB.warm_ms[1] / LB.warmed[1] : 0.0,
           (unsigned long long)LB.warmed[2], LB.warmed[2] ? LB.warm_ms[2] / LB.warmed[2] : 0.0,
           (unsigned long long)(LB.fail[0] + LB.fail[1] + LB.fail[2]),
           (unsigned long long)LB.noshader, (unsigned long long)LB.nolib, (unsigned long long)LB.skipped,
           (unsigned long long)LB.deferred, BG.n, (unsigned long long)LB.big_unused,
           (unsigned long long)__atomic_load_n(&compute_warm_reused, __ATOMIC_RELAXED), (double)mem_proc);
    plat_mutex_unlock_shared(&LB.stat);
}

static void lb_worker(void *arg) {
    lb_me = (int)(intptr_t)arg;
    if (lb_idle()) plat_thread_priority(PLAT_PRIO_IDLE);
    for (;;) {
        size_t i = lb_next(lb_me);
        if (i >= LB.n) break;
        if (skip_listed(LB.e[i].iid)) {
            skip_count();
            plat_mutex_lock(&LB.stat);
            LB.done++;
            plat_mutex_unlock(&LB.stat);
            continue;
        }
        double held;
        if (LB.short_mem || !gate_wait("stage library prebuild", &LB.stop, &held)) {
            if (!LB.stop && !plat_xchg32(&LB.short_mem, 1)) {
                char g[256];
                rt_log("vk_pipeline: stage library prebuild: ends here, at %zu of %zu variants: no commit headroom for "
                       "another build (%s). The others are built when the game first uses them", i, LB.n,
                       gate_text(g, sizeof g));
            }
            break;
        }
        int keep = i < LB.n_keep, owned = 0;
        plat_thread_priority(PLAT_PRIO_NORMAL);
        const GfxShader *sh = keep || LB.e[i].stage != GFX_STAGE_CS ? gfx_shader_load_input(LB.e[i].iid, 0)
                                                                    : gfx_shader_load_private(LB.e[i].iid, &owned);
        if (lb_idle()) plat_thread_priority(PLAT_PRIO_IDLE);
        if (!sh) {
            plat_mutex_lock(&LB.stat);
            LB.noshader++;
            plat_mutex_unlock(&LB.stat);
        } else if (gfx_shader_stage(sh) == GFX_STAGE_CS) {
            lb_compute(sh, LB.e[i].iid, keep, owned);
        } else if (!owned && (gfx_shader_stage(sh) == GFX_STAGE_VS || gfx_shader_stage(sh) == GFX_STAGE_FS)) {
            if (P.gpl) lb_library(sh, gfx_shader_stage(sh) == GFX_STAGE_VS, 1);
            else lb_skip_lib();
        }
        if (owned) gfx_shader_release_private(sh);
        gate_leave(held);
        plat_mutex_lock(&LB.stat);
        LB.done++;
        plat_mutex_unlock(&LB.stat);
    }
    if (!plat_dec32(&LB.running)) {
        lb_report(LB.stop ? "stopped" : LB.short_mem ? "ended for want of commit headroom" : "done");
        fb_log("after the stage library prebuild");
        vk_mem_stage_log("the compute pipeline prebuild");
        bg_start();
    }
    return;
}

static void lb_state(const uint32_t pr_hist[16], const uint32_t fs_hist[4]) {
    uint32_t best = 0, bf = 0;
    for (uint32_t i = 1; i < 16; i++) best = pr_hist[i] > pr_hist[best] ? i : best;
    for (uint32_t i = 1; i < 4; i++) bf = fs_hist[i] > fs_hist[bf] ? i : bf;
    if (!pr_hist[best]) best = 8;
    LB.clamp = (uint8_t)(best & 1), LB.poly = (uint8_t)(best >> 1 & 3), LB.minus_w = (uint8_t)(best >> 3 & 1);
    LB.samples = (uint8_t)(1u << bf);
}

#define LW_SAMPLE 12
#define LW_WARM_SHARE 0.75
#define LW_COLD_MS 15.0
#define LW_MAX_THREADS 6
#define LW_BOOT_THREADS 32
static struct {
    GfxWarmEnt *e;
    size_t n;
    volatile int64_t next, done;
    volatile int32_t running, stop, started;
    volatile int32_t short_mem;
    volatile int32_t checked;
    size_t from;
    PlatThread *th[LW_BOOT_THREADS + 1];
    int threads, strikes;
    int bg_threads;
    uint32_t vendor;
    double t0;
    char stamp[PLAT_PATH_MAX * 2 + 32], id[64];
} LW;

static void lw_stamp_at(const char *state, int strikes, size_t at) {
    char line[160];
    int n = snprintf(line, sizeof line, "%s %s %d %zu\n", state, LW.id, strikes, at);
    if (!plat_file_write_atomic(LW.stamp, line, (size_t)n)) rt_log("vk_pipeline: cannot write %s", LW.stamp);
}

static void lw_stamp(const char *state, int strikes) { lw_stamp_at(state, strikes, 0); }

static double lw_build(const GfxWarmEnt *e) {
    if (e->stage != GFX_STAGE_VS && e->stage != GFX_STAGE_FS) return -1;
    if (skip_listed(e->iid)) return -1;
    int owned = 0;
    const GfxShader *sh = gfx_shader_load_private(e->iid, &owned);
    if (!sh) return -1;
    double t0 = now_ms();
    int st = gfx_shader_stage(sh);
    if (st == GFX_STAGE_VS || st == GFX_STAGE_FS) lb_library(sh, st == GFX_STAGE_VS, 0);
    double ms = now_ms() - t0;
    if (owned) gfx_shader_release_private(sh);
    return ms;
}

static void lw_keep_progress(void) {
    int64_t d = LW.done;
    size_t at = LW.from + (size_t)(d > LW.threads ? d - LW.threads : 0);
    if (at <= LW.from) return;
    lw_stamp_at("warming", LW.strikes, at);
    rt_log("vk_pipeline: library warm: %lld prepared variants built in this run; the next start goes on from variant "
           "%zu of %zu", (long long)d, at, LW.n);
}

static void lw_worker(void *arg) {
    int me = (int)(intptr_t)arg, idle = 0;
    no_cache = 1;
    for (;;) {
        if (!bf_waiting()) {
            if (me >= LW.bg_threads) break;
            if (!idle) plat_thread_priority(PLAT_PRIO_IDLE), idle = 1;
        }
        size_t i = (size_t)plat_inc64(&LW.next) - 1;
        if (i >= LW.n || LW.stop) break;
        play_rest_wait(&LW.stop);
        double held;
        if (LW.short_mem || !gate_wait("library warm", &LW.stop, &held)) {
            if (!LW.stop && !plat_xchg32(&LW.short_mem, 1)) {
                char g[256];
                rt_log("vk_pipeline: library warm: ends for this run at %lld of %zu prepared variants: no commit headroom "
                       "for another build (%s). The next start goes on with it", (long long)LW.done, LW.n,
                       gate_text(g, sizeof g));
            }
            break;
        }
        journal_mark(me, &LW.e[i]);
        lw_build(&LW.e[i]);
        journal_mark(me, NULL);
        gate_leave(held);
        int64_t d = plat_inc64(&LW.done);
        if (d % 8000 == 0) {
            size_t at = LW.from + (size_t)(d > LW.threads ? d - LW.threads : 0);
            lw_stamp_at("warming", LW.strikes, at);
            rt_log("vk_pipeline: library warm: %lld of %zu prepared variants in %.0f s (up to variant %zu)", (long long)d,
                   LW.n - LW.from, (now_ms() - LW.t0) / 1000, at);
        }
    }
    if (plat_dec32(&LW.running)) return;
    if (!LW.stop && !LW.short_mem) {
        lw_stamp("done", LW.strikes);
        rt_log("vk_pipeline: library warm: done, %zu prepared variants in %.0f s on %d threads; new pipelines "
               "link from the driver's cache from here on", LW.n, (now_ms() - LW.t0) / 1000, LW.threads);
        return;
    }
    lw_keep_progress();
}

static double lw_time_sample(int *fast, int *got_out) {
    double ms[LW_SAMPLE];
    int got = 0;
    size_t step = LW.n / LW_SAMPLE ? LW.n / LW_SAMPLE : 1, at = (size_t)(plat_now_ns() / 1000) % step;
    for (size_t i = at; i < LW.n && got < LW_SAMPLE && !LW.stop; i += step)
        for (size_t j = i; j < i + step && j < LW.n; j++) {
            if (LW.e[j].stage != GFX_STAGE_FS) continue;
            double t = lw_build(&LW.e[j]);
            if (t >= 0) ms[got++] = t;
            break;
        }
    *fast = 0, *got_out = got;
    if (got < LW_SAMPLE / 2) return -1;
    for (int i = 0; i < got; i++) *fast += ms[i] <= LW_COLD_MS;
    for (int i = 1; i < got; i++)
        for (int j = i; j > 0 && ms[j] < ms[j - 1]; j--) {
            double t = ms[j];
            ms[j] = ms[j - 1], ms[j - 1] = t;
        }
    return ms[got / 2];
}

static int lw_warm_sample(int fast, int got) {
    return got > 0 && fast >= (int)(got * LW_WARM_SHARE + 0.999);
}

static double lw_probe(int *fast, int *got) {
    plat_thread_priority(PLAT_PRIO_NORMAL);
    double t = lw_time_sample(fast, got);
    if (t >= 0 && !lw_warm_sample(*fast, *got) && !LW.stop && !bf_waiting()) {
        for (int i = 0; i < 100 && !LW.stop; i++) plat_sleep_ms(100);
        int f2 = 0, g2 = 0;
        double t2 = lw_time_sample(&f2, &g2);
        if (t2 >= 0 && lw_warm_sample(f2, g2) >= lw_warm_sample(*fast, *got) && f2 * (*got) > (*fast) * g2)
            t = t2, *fast = f2, *got = g2;
    }
    if (!bf_waiting()) plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    return t;
}

_Static_assert(NVC_KIND_FILE == PLAT_FS_FILE && NVC_KIND_DIR == PLAT_FS_DIR && NVC_KIND_LINK == PLAT_FS_LINK &&
                   NVC_LISTED == PLAT_FS_OK && NVC_MISSING == PLAT_FS_NOT_FOUND,
               "nvidia_cache.h mirrors platform.h's file kinds and listing results");

static void lw_say_cache_folder(void) {
    char dir[PLAT_PATH_MAX * 2], work[PLAT_PATH_MAX * 2 + 64], text[PLAT_PATH_MAX * 2 + 160];
    if (!plat_getenv(NVC_PATH_VAR, dir, sizeof dir)) return;
    NvcSize s;
    nvc_measure(plat_fs_list, dir, work, sizeof work, &s);
    nvc_size_text(text, sizeof text, dir, &s);
    rt_log("vk_pipeline: library warm: %s", text);
}

static void lw_check(void) {
    if (!bf_waiting()) plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    no_cache = 1;
    lw_say_cache_folder();
    char was[16] = "", id[64] = "";
    int strikes = 0;
    unsigned long long at = 0;
    uint64_t size = 0;
    char *f = (char *)plat_file_read(LW.stamp, &size);
    if (f && size < 200) {
        char line[200];
        memcpy(line, f, size), line[size] = 0;
        if (sscanf(line, "%15s %63s %d %llu", was, id, &strikes, &at) < 2) was[0] = 0;
    }
    free(f);
    int same = !strcmp(id, LW.id), resume = same && !strcmp(was, "warming");
    const char *mode = cfg_value("NX2SARIA_LIB_WARM");
    int fast = 0, got = 0;
    double median = resume ? 0 : lw_probe(&fast, &got);
    if (LW.stop) return;
    int cold = (median >= 0 && !lw_warm_sample(fast, got)) || !strcmp(mode, "force");
    LW.from = resume && at < LW.n ? (size_t)at : 0;
    LW.next = (int64_t)LW.from;
    if (!resume && !cold) {
        if (same && strikes) lw_stamp("done", 0);
        rt_log("vk_pipeline: library warm: the driver's cache has the prepared libraries (%d of %d sampled built in "
               "under %.0f ms, median %.1f ms)", fast, got, LW_COLD_MS, median);
        return;
    }
    if (!resume && same && strikes >= 2 && strcmp(mode, "force")) {
        rt_log("vk_pipeline: library warm: the prepared libraries are cold again (%d of %d sampled fast, median %.0f "
               "ms) after two finished warms on this driver: it does not keep them, so pipelines compile when a draw "
               "first needs them (NX2SARIA_LIB_WARM=force warms anyway).%s", fast, got, median,
               lw_cold_hint(LW.vendor, LW_COLD_GAVE_UP, LW_ON_WINDOWS));
        return;
    }
    LW.strikes = resume ? strikes : same && !strcmp(was, "done") ? strikes + 1 : 0;
    lw_stamp_at("warming", LW.strikes, LW.from);
    int cpus = (int)plat_cpu_usable(), t = cpus / 4, boot = bf_waiting();
    t = t < 2 ? 2 : t > LW_MAX_THREADS ? LW_MAX_THREADS : t;
    LW.bg_threads = t;
    if (boot) t = cpus < 1 ? 1 : cpus > LW_BOOT_THREADS ? LW_BOOT_THREADS : cpus;
    if (boot) {
        char why[64];
        if (resume) snprintf(why, sizeof why, "an earlier run began the warm, which goes on from variant %zu", LW.from);
        else snprintf(why, sizeof why, "%d of %d sampled fast, median %.0f ms", fast, got, median);
        rt_log("vk_pipeline: library warm: the driver's cache does not have the prepared libraries (%s): building %zu "
               "prepared variants into it on %d threads while the start waits (the whole warm after a driver change)",
               why, LW.n - LW.from, t);
    } else if (resume)
        rt_log("vk_pipeline: library warm: going on with the warm an earlier run began, from variant %zu of %zu, on %d "
               "idle threads", LW.from, LW.n, t);
    else
        rt_log("vk_pipeline: library warm: the driver's cache does not have the prepared libraries (%d of %d sampled "
               "fast, median %.0f ms; a driver update or a change of what they are built with): building %zu prepared "
               "variants into it on %d idle threads while the game runs. Until it is done, a pipeline a draw first "
               "needs may compile, which skips that draw for some frames or makes it wait.%s", fast, got, median, LW.n,
               t,
               lw_cold_hint(LW.vendor,
                            lw_warm_sample(fast, got) ? LW_WARM_FORCED
                            : same && !strcmp(was, "done") ? LW_COLD_AFTER_WARM : LW_COLD_FIRST,
                            LW_ON_WINDOWS));
    LW.t0 = now_ms();
    LW.running = t;
    int started = 0;
    for (int i = 0; i < t; i++)
        if ((LW.th[1 + started] = plat_thread_new(lw_worker, (void *)(intptr_t)i, "lib warm"))) started++;
    LW.threads = started;
    if (plat_add32(&LW.running, started - t) == 0 && !started) rt_log("vk_pipeline: library warm: no thread started");
}

static void lw_main(void *arg) {
    (void)arg;
    lw_check();
    plat_xchg32(&LW.checked, 1);
}

static void lw_start(void) {
    const char *mode = cfg_value("NX2SARIA_LIB_WARM");
    if (!P.gpl || !strcmp(mode, "0") || plat_xchg32(&LW.started, 1)) return;
    char dir[PLAT_PATH_MAX * 2], path[PLAT_PATH_MAX * 2 + 32];
    if (!cache_dir(dir, sizeof dir)) return;
    snprintf(path, sizeof path, "%s/shader-prepared.bin", dir);
    snprintf(LW.stamp, sizeof LW.stamp, "%s/shader-warm.txt", dir);
    size_t n = 0;
    uint8_t *f = gfx_shader_prepared_read(path, &n, NULL, NULL);
    LW.e = n ? calloc(n, sizeof *LW.e) : NULL;
    for (size_t i = 0; LW.e && i < n; i++)
        memcpy(LW.e[i].iid, f + PRP_REC * i + 4, 32), LW.e[i].stage = f[PRP_REC * i], LW.e[i].prio = f[PRP_REC * i + 1];
    free(f);
    if (!LW.e) return;
    LW.n = n;
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(vk_ctx()->phys, &pp);
    snprintf(LW.id, sizeof LW.id, "%04x:%04x:%08x", pp.vendorID, pp.deviceID, pp.driverVersion);
    LW.vendor = pp.vendorID;
    LW.th[0] = plat_thread_new(lw_main, NULL, "lib warm check");
}

static void lw_stop(void) {
    plat_xchg32(&LW.stop, 1);
    for (int i = 0; i <= LW_BOOT_THREADS; i++)
        if (LW.th[i]) plat_thread_join(LW.th[i]), LW.th[i] = NULL;
}

static void lb_start(const uint32_t pr_hist[16], const uint32_t fs_hist[4]) {
    const char *e = cfg_value("NX2SARIA_LIB_PREBUILD"), *ke = cfg_get("NX2SARIA_LIB_PREBUILD_KEEP");
    const char *te = cfg_get("NX2SARIA_LIB_PREBUILD_THREADS");
    if (!A.on || !cfg_bool("NX2SARIA_LIB_PREBUILD")) return;
    mesh_pre_start();
    lb_state(pr_hist, fs_hist);
    if (BF.on) BF.lw = 1;
    else lw_start();
    lb_big_words_init();
    char dir[PLAT_PATH_MAX * 2];
    if (cache_dir(dir, sizeof dir)) skip_init(dir);
    GfxWarmEnt *all = NULL;
    size_t total = gfx_shader_warm_list(&all), n = 0, n_cs = 0;
    LB.keep_n = ke && *ke ? atoi(ke) : 0;
    LB.e = malloc((total + 1) * sizeof *LB.e);
    uint8_t *kf = total ? malloc(total) : NULL;
    for (size_t i = 0; LB.e && kf && i < total; i++)
        if (all[i].stage == GFX_STAGE_CS && (kf[i] = (uint8_t)cu_keep(all[i].iid))) LB.e[n++] = all[i];
    LB.n_keep = n;
    for (size_t i = 0; LB.e && i < total; i++)
        if (all[i].stage == GFX_STAGE_CS && !(kf && kf[i])) LB.e[n++] = all[i];
    free(kf);
    n_cs = n;
    for (size_t i = 0; LB.e && i < total && (int32_t)(n - n_cs) < LB.keep_n; i++)
        if (all[i].stage != GFX_STAGE_CS && all[i].prio == 0) LB.e[n++] = all[i];
    free(all);
    if (atoi(e) > 1 && (size_t)atoi(e) < n) n = (size_t)atoi(e);
    if (LB.n_keep > n) LB.n_keep = n;
    if (!n) {
        free(LB.e), LB.e = NULL;
        return;
    }
    LB.n = n;
    LB.idle = 1;
    int cpus = (int)plat_cpu_usable();
    int t = te && *te ? atoi(te) : (cpus < 4 ? 1 : 2);
    LB.tail_one = !(te && *te);
    t = t < 1 ? 1 : t > LB_MAX_THREADS ? LB_MAX_THREADS : t;
    LB.bg_threads = t;
    if (bf_waiting() && LB.tail_one) t = cpus < 1 ? 1 : cpus > LB_MAX_THREADS ? LB_MAX_THREADS : cpus;
    if ((size_t)t > n) t = (int)n;
    LB.t0 = now_ms();
    LB.running = t;
    int started = 0;
    for (int i = 0; i < t; i++)
        if ((LB.th[started] = plat_thread_new(lb_worker, (void *)(intptr_t)started, "lib prebuild"))) started++;
    plat_add32(&LB.running, started - t);
    LB.threads = started;
    char big[128];
    if (!LB.big_words) snprintf(big, sizeof big, "compute shaders of every size");
    else snprintf(big, sizeof big, "compute shaders over %llu SPIR-V words %s", (unsigned long long)LB.big_words,
                  P.cache_control ? "only from the pipeline caches" : "left to their first dispatch");
    rt_log("vk_pipeline: stage library prebuild: %zu compute pipelines, of which %zu stay (the shaders dispatched most "
           "recently, NX2SARIA_COMPUTE_KEEP) and the others are built into the pipeline cache and destroyed, to be created "
           "at their first dispatch; %zu recorded variants' libraries (NX2SARIA_LIB_PREBUILD_KEEP %d); on %d %s "
           "threads; library state clamp %u, polygon mode %u, depth -w..w %u, samples %u; %s (NX2SARIA_LIB_PREBUILD_BIG)",
           n_cs < n ? n_cs : n, LB.n_keep, n > n_cs ? n - n_cs : 0, LB.keep_n, started,
           bf_waiting() ? "normal-priority (the start waits for them)" : "idle-priority", LB.clamp, LB.poly, LB.minus_w,
           LB.samples, big);
    if (LB.tail_one && started > 1 && !bf_waiting())
        rt_log("vk_pipeline: stage library prebuild: after the %zu recent compute pipelines, one worker continues the "
               "remaining list; NX2SARIA_LIB_PREBUILD_THREADS overrides this background limit", LB.n_keep);
}

static void lb_stop(void) {
    lw_stop();
    plat_xchg32(&MP.stop, 1);
    if (MP.th) plat_thread_join(MP.th), MP.th = NULL;
    plat_xchg32(&LB.stop, 1);
    for (int i = 0; i < LB.threads; i++)
        if (LB.th[i]) plat_thread_join(LB.th[i]), LB.th[i] = NULL;
    LB.threads = 0;
    if (BG.th) plat_thread_join(BG.th), BG.th = NULL;
}

static int bf_time_up(void) {
    if (!BF.t_end || !bf_waiting() || now_ms() < BF.t_end) return 0;
    vk_pipeline_prewarm_detach();
    return 1;
}

static void bf_wait(void) {
    double t0 = now_ms();
    int up = bf_time_up();
    if (LB.n && LB.threads) {
        __atomic_store_n(&pw_pipe_done, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&pw_pipe_total, (int64_t)LB.n, __ATOMIC_RELAXED);
        __atomic_store_n(&pw_phase, 3, __ATOMIC_RELEASE);
        while (__atomic_load_n(&LB.running, __ATOMIC_ACQUIRE) > 0 && !LB.stop && bf_waiting()) {
            plat_sleep_ms(100);
            plat_mutex_lock_shared(&LB.stat);
            uint64_t d = LB.done;
            plat_mutex_unlock_shared(&LB.stat);
            __atomic_store_n(&pw_pipe_done, (int64_t)d, __ATOMIC_RELAXED);
            up |= bf_time_up();
        }
    }
    double t1 = now_ms();
    int libs = 0;
    if (BF.lw && !LB.stop) {
        if (bf_waiting()) journal_open();
        lw_start();
        if (LW.th[0]) {
            libs = 1;
            __atomic_store_n(&pw_pipe_done, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&pw_pipe_total, (int64_t)LW.n, __ATOMIC_RELAXED);
            __atomic_store_n(&pw_phase, 4, __ATOMIC_RELEASE);
            while ((!LW.checked || LW.running > 0) && !LW.stop && bf_waiting()) {
                plat_sleep_ms(100);
                __atomic_store_n(&pw_pipe_done, (int64_t)LW.done, __ATOMIC_RELAXED);
                __atomic_store_n(&pw_pipe_total, (int64_t)(LW.n - LW.from), __ATOMIC_RELAXED);
                up |= bf_time_up();
            }
        }
        journal_close();
    }
    int stopped = LB.stop || LW.stop;
    int over = __atomic_load_n(&LB.running, __ATOMIC_ACQUIRE) <= 0 && (!libs || (LW.checked && LW.running <= 0));
    rt_log("vk_pipeline: the whole warm %s: the compute pipelines (%zu) took %.0f ms of the wait, the driver's cache of "
           "stage libraries %.0f ms (%s; %lld of %zu prepared variants built so far)",
           stopped ? "stopped with the process" : over ? "is done"
           : up ? "goes on beside the game: its time on the boot screen is up (NX2SARIA_PREWARM_FULL_S)"
                : "goes on beside the game: the boot stopped waiting", LB.n, t1 - t0, now_ms() - t1,
           !libs ? "not warmed: no prepared list, or the graphics pipeline library or NX2SARIA_LIB_WARM is off"
                 : LW.done ? "being warmed" : "left as it is so far: the check found it warm, gave the warm up, or still runs",
           (long long)LW.done, LW.n);
    if (!stopped) {
        char key[32], line[64];
        gpu_key(key, sizeof key);
        int len = snprintf(line, sizeof line, "%s done\n", key);
        if (!plat_file_write_atomic(BF.owed, line, (size_t)len)) rt_log("vk_pipeline: cannot write %s", BF.owed);
    }
    vk_pipeline_prewarm_detach();
}

#define PW_OUT 32
static struct {
    size_t at[JR_SLOTS];
    double t0[JR_SLOTS];
    uint64_t words[JR_SLOTS];
    uint8_t out_iid[PW_OUT][32];
    uint64_t out_words[PW_OUT];
    int n_out;
    int test_ms;
} PW;

static void pw_mark(int slot, size_t at, uint64_t words) {
    if (slot < 0 || slot >= JR_SLOTS) return;
    plat_mutex_lock(&LB.stat);
    PW.at[slot] = at, PW.t0[slot] = now_ms(), PW.words[slot] = words;
    plat_mutex_unlock(&LB.stat);
}

static void lb_warm_worker(void *arg) {
    int slot = (int)(intptr_t)arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&LB.next) - 1;
        if (i >= LB.n) return;
        pw_mark(slot, i + 1, 0);
        if (PW.test_ms > 0 && i + 1 == LB.n) plat_sleep_ms((uint32_t)PW.test_ms);
        int owned = 0, skip = skip_listed(LB.e[i].iid);
        const GfxShader *sh = skip ? NULL : gfx_shader_load_private(LB.e[i].iid, &owned);
        int st = sh ? gfx_shader_stage(sh) : -1;
        if (sh) journal_mark(slot, &LB.e[i]);
        if (sh) pw_mark(slot, i + 1, gfx_shader_words(sh));
        if (skip) {
            skip_count();
        } else if (!sh) {
            plat_mutex_lock(&LB.stat);
            LB.noshader++;
            plat_mutex_unlock(&LB.stat);
        } else if (st == GFX_STAGE_CS && lb_big_left_out(gfx_shader_words(sh), cu_keep(LB.e[i].iid))) {
            plat_mutex_lock(&LB.stat);
            LB.big_unused++;
            if (PW.n_out < PW_OUT)
                memcpy(PW.out_iid[PW.n_out], LB.e[i].iid, 32), PW.out_words[PW.n_out++] = gfx_shader_words(sh);
            plat_mutex_unlock(&LB.stat);
        } else if (st == GFX_STAGE_CS) {
            Bindings B = {0};
            const char *why = NULL;
            lb_lock();
            Layout *l = compute_layout(sh, &B, &why);
            lb_unlock();
            double t0 = now_ms();
            int reused = 0;
            VkPipeline pipe = l ? compute_create(sh, l, 0, &reused) : VK_NULL_HANDLE;
            if (pipe) vkDestroyPipeline(P.device, pipe, NULL);
            lb_count(2, reused ? 1 : pipe ? 3 : 2, now_ms() - t0);
        } else if (st == GFX_STAGE_VS || st == GFX_STAGE_FS) {
            if (P.gpl) lb_library(sh, st == GFX_STAGE_VS, 0);
            else lb_skip_lib();
        }
        pw_mark(slot, 0, 0);
        if (sh) journal_mark(slot, NULL);
        if (owned) gfx_shader_release_private(sh);
        plat_mutex_lock(&LB.stat);
        uint64_t d = ++LB.done;
        plat_mutex_unlock(&LB.stat);
        if (d % 4000 == 0) lb_report("preparation progress");
    }
}

static void prep_stalled(double stall_ms, int k, const char *jp) {
    if (__atomic_load_n(&LB.next, __ATOMIC_RELAXED) < (int64_t)LB.n) return;
    double now = now_ms();
    int busy = 0, stuck = 0, lib = 0;
    size_t first = 0;
    plat_mutex_lock(&LB.stat);
    for (int s = 0; s < k && s < JR_SLOTS; s++) {
        if (!PW.at[s]) continue;
        busy++;
        if (now - PW.t0[s] < stall_ms) continue;
        stuck++;
        size_t at = PW.at[s] - 1;
        if (!first || at + 1 < first) first = at + 1;
        lib |= LB.e[at].stage == GFX_STAGE_VS || LB.e[at].stage == GFX_STAGE_FS;
    }
    if (!busy || stuck < busy) {
        plat_mutex_unlock(&LB.stat);
        return;
    }
    uint64_t d = LB.done, built = d - LB.skipped - LB.noshader - LB.nolib - LB.big_unused;
    for (int s = 0; s < k && s < JR_SLOTS; s++) {
        if (!PW.at[s]) continue;
        const GfxWarmEnt *e = &LB.e[PW.at[s] - 1];
        char hex[65];
        for (int b = 0; b < 32; b++) snprintf(hex + 2 * b, 3, "%02x", e->iid[b]);
        char size[48] = "";
        if (PW.words[s]) snprintf(size, sizeof size, ", %llu SPIR-V words", (unsigned long long)PW.words[s]);
        printf("prepare-shaders: variant %s (%s%s) still building after %.0f s\n", hex, stage_name(e->stage), size,
               (now - PW.t0[s]) / 1000);
    }
    plat_mutex_unlock(&LB.stat);
    plat_mutex_lock(&SK.jlock);
    if (SK.journal) plat_file_close(SK.journal), SK.journal = NULL;
    plat_mutex_unlock(&SK.jlock);
    plat_file_delete(jp);
    if (P.gpl && !LB.nolib) {
        if (lib) lw_stamp_at("warming", 0, first - 1);
        else lw_stamp("done", 0);
    }
    printf("prepare-shaders: progress build %llu %zu\n", (unsigned long long)d, LB.n);
    printf("prepare-shaders: %llu of %zu variants built into the driver cache on %d threads in %.0f ms; %d still "
           "building after %.0f s each are left to the game, which goes on with them in the background "
           "(NX2SARIA_PREPARE_STALL_S)\n", (unsigned long long)built, LB.n, k, now - LB.t0, stuck, stall_ms / 1000);
    fflush(stdout);
    rt_log("vk_pipeline: the preparation stopped waiting for %d builds the driver had held for %.0f s or more; %s", stuck,
           stall_ms / 1000, lib ? "the game's library warm goes on from the first of them" : "all compute shaders, which the boot builds");
    plat_exit(0);
}

static int prep_sample_warm(int *fast, int *got) {
    double ms[LW_SAMPLE];
    *fast = *got = 0;
    size_t step = LB.n / LW_SAMPLE ? LB.n / LW_SAMPLE : 1;
    int was = no_cache;
    no_cache = 1;
    for (size_t i = step / 2; i < LB.n && *got < LW_SAMPLE; i += step)
        for (size_t j = i; j < i + step && j < LB.n; j++) {
            if (LB.e[j].stage != GFX_STAGE_FS) continue;
            journal_mark(0, &LB.e[j]);
            double t = lw_build(&LB.e[j]);
            journal_mark(0, NULL);
            if (t >= 0) ms[(*got)++] = t;
            break;
        }
    no_cache = was;
    for (int i = 0; i < *got; i++) *fast += ms[i] <= LW_COLD_MS;
    return *got >= LW_SAMPLE / 2 && lw_warm_sample(*fast, *got);
}

static void lb_warm_prepared(const char *dir, int sample) {
    char path[PLAT_PATH_MAX * 2 + 32];
    snprintf(path, sizeof path, "%s/shader-prepared.bin", dir);
    size_t n = 0;
    uint8_t *f = gfx_shader_prepared_read(path, &n, NULL, NULL);
    LB.e = n ? calloc(n, sizeof *LB.e) : NULL;
    for (size_t i = 0; LB.e && i < n; i++)
        memcpy(LB.e[i].iid, f + PRP_REC * i + 4, 32), LB.e[i].stage = f[PRP_REC * i], LB.e[i].prio = f[PRP_REC * i + 1];
    free(f);
    if (!LB.e) return;
    skip_init(dir);
    lb_big_words_init();
    cu_open_prep(dir);
    PW.test_ms = cfg_int("NX2SARIA_PREPARE_STALL_TEST_MS");
    char jp[PLAT_PATH_MAX * 2 + 48];
    snprintf(jp, sizeof jp, "%s/shader-prepare-journal.bin", dir);
    SK.journal = plat_fs_create(jp, NULL);
    if (!SK.journal) rt_log("vk_pipeline: cannot create %s: a driver crash in this run will not be recorded", jp);
    LB.n = n, LB.idle = 0, LB.t0 = now_ms();
    static const uint32_t no_pr[16], no_fs[4];
    lb_state(no_pr, no_fs);
    snprintf(LW.stamp, sizeof LW.stamp, "%s/shader-warm.txt", dir);
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(vk_ctx()->phys, &pp);
    snprintf(LW.id, sizeof LW.id, "%04x:%04x:%08x", pp.vendorID, pp.deviceID, pp.driverVersion);
    char was[16] = "", sid[64] = "";
    uint64_t ssize = 0;
    char *sf = (char *)plat_file_read(LW.stamp, &ssize);
    if (sf && ssize < 200) {
        char line[200];
        memcpy(line, sf, ssize), line[ssize] = 0;
        if (sscanf(line, "%15s %63s", was, sid) < 2) was[0] = 0;
    }
    free(sf);
    int unfinished = P.gpl && !strcmp(was, "warming") && !strcmp(sid, LW.id);
    if (sample && unfinished)
        printf("prepare-shaders: a warm of the graphics driver's cache that the game began did not finish "
               "(shader-warm.txt): all %zu variants are built into it\n", n);
    int fast = 0, got = 0;
    if (sample && !unfinished && P.gpl && strcmp(cfg_value("NX2SARIA_LIB_WARM"), "force") &&
        prep_sample_warm(&fast, &got)) {
        if (SK.journal) plat_file_close(SK.journal), SK.journal = NULL, plat_file_delete(jp);
        printf("prepare-shaders: progress build %zu %zu\n", n, n);
        printf("prepare-shaders: the graphics driver's cache still has the prepared shaders (%d of %d sampled came from "
               "it in under %.0f ms each): the %zu variants are not built again (%.0f ms)\n", fast, got, LW_COLD_MS, n,
               now_ms() - LB.t0);
        fflush(stdout);
        free(LB.e), LB.e = NULL, LB.n = 0, LB.next = 0, LB.done = 0, LB.skipped = 0;
        return;
    }
    if (sample && got)
        printf("prepare-shaders: the graphics driver's cache does not have the prepared shaders (%d of %d sampled came "
               "from it): all %zu variants are built into it again\n", fast, got, n);
    int cpus = (int)plat_cpu_usable(), k = 0;
    PlatThread *t[JR_SLOTS];
    for (int i = 0; i < cpus && i < JR_SLOTS; i++)
        if ((t[k] = plat_thread_new(lb_warm_worker, (void *)(intptr_t)k, "lib warm"))) k++;
    if (!k) lb_warm_worker((void *)(intptr_t)0);
    double stall_ms = cfg_int("NX2SARIA_PREPARE_STALL_S") * 1000.0;
    for (int i = 0; i < k; i++)
        while (!plat_thread_join_ns(t[i], 2000000000u)) {
            plat_mutex_lock(&LB.stat);
            uint64_t d = LB.done;
            plat_mutex_unlock(&LB.stat);
            printf("prepare-shaders: progress build %llu %zu\n", (unsigned long long)d, n);
            fflush(stdout);
            if (stall_ms > 0) prep_stalled(stall_ms, k, jp);
        }
    printf("prepare-shaders: progress build %zu %zu\n", n, n);
    fflush(stdout);
    if (SK.journal) plat_file_close(SK.journal), SK.journal = NULL, plat_file_delete(jp);
    lb_report("preparation done");
    if (P.gpl && !LB.nolib) lw_stamp("done", 0);
    unsigned long long skipped = LB.skipped, big = LB.big_unused;
    size_t tried = n - (size_t)skipped - (size_t)big;
    if (LB.nolib)
        printf("prepare-shaders: %zu variants on %d threads in %.0f ms, %llu vertex/fragment variants skipped (graphics "
               "pipeline library off, they build at their first draw)\n", tried, k, now_ms() - LB.t0,
               (unsigned long long)LB.nolib);
    else
        printf("prepare-shaders: %zu variants built into the driver cache on %d threads in %.0f ms\n", tried, k,
               now_ms() - LB.t0);
    if (skipped)
        printf("prepare-shaders: %llu variants skipped: the graphics driver crashed while building them in an earlier "
               "run (shader-prepare-skip.txt); they build at their first draw\n", skipped);
    if (big)
        printf("prepare-shaders: %llu large compute shaders left out (never dispatched): over %llu SPIR-V words, as the "
               "game's boot leaves them (NX2SARIA_LIB_PREBUILD_BIG, compute-used.bin); one builds at its first "
               "dispatch\n", big, (unsigned long long)LB.big_words);
    for (int j = 0; j < PW.n_out; j++) {
        char hex[65];
        for (int b = 0; b < 32; b++) snprintf(hex + 2 * b, 3, "%02x", PW.out_iid[j][b]);
        printf("prepare-shaders: left out %s (%llu SPIR-V words)\n", hex, (unsigned long long)PW.out_words[j]);
    }
    if (big > (unsigned long long)PW.n_out) printf("prepare-shaders: and %llu more left out\n", big - PW.n_out);
    PW.n_out = 0;
    fflush(stdout);
    free(LB.e), LB.e = NULL, LB.n = 0, LB.next = 0, LB.done = 0, LB.skipped = 0, LB.big_unused = 0;
}

static int cache_save(uint32_t wait_ms, int exiting);

static int seed_pipelines(const char *dir) {
    const char *mode = cfg_value("NX2SARIA_PREPARE_PIPELINES");
    int all = !strcmp(mode, "all") || (!strcmp(mode, "mesh") && !P.gpl);
    char path[PLAT_PATH_MAX * 2 + 48], mark[PLAT_PATH_MAX * 2 + 48];
    snprintf(path, sizeof path, "%s/seed-pipeline-keys.bin", dir);
    snprintf(mark, sizeof mark, "%s/seed-pipelines-running.txt", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (!strcmp(mode, "0")) {
        fclose(f);
        printf("prepare-pipelines: off (NX2SARIA_PREPARE_PIPELINES=0)\n");
        return 0;
    }
    if (plat_file_exists(mark)) {
        fclose(f);
        printf("prepare-pipelines: skipped: an earlier preparation ended while it built the listed pipelines (%s); they "
               "are built at their first draw\n", mark);
        return 0;
    }
    KeysHeader h;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = 0;
    KeyRec *r = NULL;
    if (len >= (long)sizeof h && fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "NX2NPKEY", 8) &&
        h.version == KEYS_VERSION && h.rec_size == sizeof(KeyRec)) {
        n = ((size_t)len - sizeof h) / sizeof(KeyRec);
        r = malloc(n * sizeof *r + 1);
        if (r && fread(r, sizeof *r, n, f) != n) n = 0;
    } else {
        printf("prepare-pipelines: %s is a list for another version of the game's runtime (its record layout differs); "
               "no pipeline is built from it\n", path);
    }
    fclose(f);
    uint8_t (*ids)[32] = n ? malloc(3 * n * 32) : NULL;
    const GfxShader **sh = n ? calloc(3 * n, sizeof *sh) : NULL;
    if (!r || !ids || !sh) {
        free(r), free(ids), free(sh);
        return 0;
    }
    double t0 = now_ms();
    size_t m = 0, u = 0;
    for (size_t i = 0; i < n; i++) {
        memcpy(ids[m++], r[i].vs, 32);
        if (r[i].has_fs) memcpy(ids[m++], r[i].fs, 32);
        if (r[i].has_ts) memcpy(ids[m++], r[i].ts, 32);
    }
    qsort(ids, m, 32, id_cmp);
    for (size_t i = 0; i < m; i++)
        if (!u || memcmp(ids[u - 1], ids[i], 32)) memcpy(ids[u++], ids[i], 32);
    int cpus = (int)plat_cpu_usable();
    const char *pe = cfg_get("NX2SARIA_PREWARM_XLATE");
    int procs = pe && *pe ? atoi(pe) : cpus / 2 > 16 ? 16 : cpus / 2;
    gfx_shader_prewarm((const uint8_t(*)[32])ids, u, sh, cpus, procs < 1 ? 1 : procs);
    static const char note[] = "the listed pipelines are being built; a run that finds this file skips them\n";
    plat_file_write_atomic(mark, note, sizeof note - 1);
    uint32_t queued = 0, dropped = 0, known = 0, other = 0, mesh = 0, no_mesh = 0;
    plat_mutex_lock(&P.lock);
    uint32_t built0 = K.built, failed0 = K.failed;
    K.t0 = t0, K.prep = 1;
    K.loading = 1;
    for (size_t i = 0; i < n; i++) {
        const uint8_t (*v)[32] = bsearch(r[i].vs, ids, u, 32, id_cmp);
        const uint8_t (*fi)[32] = r[i].has_fs ? bsearch(r[i].fs, ids, u, 32, id_cmp) : NULL;
        const uint8_t (*t)[32] = r[i].has_ts ? bsearch(r[i].ts, ids, u, 32, id_cmp) : NULL;
        const GfxShader *vs = v ? sh[v - ids] : NULL, *fs = fi ? sh[fi - ids] : NULL, *ts = t ? sh[t - ids] : NULL;
        if (!vs || (r[i].has_fs && !fs) || (r[i].has_ts && !ts)) {
            dropped++;
            continue;
        }
        VkPipeState s = r[i].s;
        s.vs = vs, s.fs = fs, s.ts = ts;
        if (!is_mesh(&s) && !all) {
            other++;
            continue;
        }
        if (is_mesh(&s) && (!vk_ctx()->has_mesh_shader || vk_ctx()->mesh_fallback)) {
            no_mesh++;
            continue;
        }
        if (!is_mesh(&s) && P.indep && fs && gfx_shader_stage(vs) == GFX_STAGE_VS) {
            GfxShaderIo io[32];
            vs_outs(vs, fs, io);
        }
        GKey k;
        gkey_fill(&s, &k);
        if (is_mesh(&s) && mesh_libs()) mesh_pre_note(&s, &k);
        uint64_t hk = key_hash(&k, sizeof k);
        int q = 0;
        if (pipe_find(&k, sizeof k, hk)) known++;
        else pipe_miss(&s, &k, hk, 2, &q), queued += (uint32_t)q, mesh += q && is_mesh(&s);
    }
    K.loading = 0;
    K.until = 0;
    GATE.off = 1;
    plat_mutex_unlock(&P.lock);
    free(ids), free(sh), free(r);
    PlatThread *th[64];
    int nt = 0;
    if (queued) warm_builder((void *)1);
    for (int i = 0; i < cpus && i < 64 && queued; i++)
        if ((th[nt] = plat_thread_new(warm_builder, NULL, "pipe list"))) nt++;
    if (!nt && queued) warm_builder(NULL);
    for (int i = 0; i < nt; i++)
        while (!plat_thread_join_ns(th[i], 2000000000u)) {
            printf("prepare-pipelines: progress %u %u\n",
                   __atomic_load_n(&K.built, __ATOMIC_RELAXED) + __atomic_load_n(&K.failed, __ATOMIC_RELAXED) - built0 - failed0,
                   queued);
            fflush(stdout);
        }
    plat_mutex_lock(&P.lock);
    while (K.left) plat_cond_wait(&done_cv, &P.lock);
    uint32_t built = K.built - built0, failed = K.failed - failed0;
    plat_mutex_unlock(&P.lock);
    mesh_pre_run(cpus);
    if (MP.n) {
        char ml[320];
        snprintf(ml, sizeof ml, "prepare-pipelines: %u stage libraries of the listed mesh pipelines (%u of task and mesh "
                 "stages, %u fragment shader): %llu built (avg %.1f ms, longest %.0f ms), %llu failed; %.0f ms", MP.n,
                 MP.n_pr, MP.n - MP.n_pr, (unsigned long long)MP.built, MP.built ? MP.ms / (double)MP.built : 0.0, MP.max,
                 (unsigned long long)MP.fail, now_ms() - MP.t0);
        printf("%s\n", ml);
        fflush(stdout);
        rt_log("vk_pipeline: %s", ml);
    }
    int saved = cache_save(5000, 0) > 0;
    plat_file_delete(mark);
    char msg[640];
    snprintf(msg, sizeof msg, "prepare-pipelines: %zu listed pipelines: %u built (%u mesh), %u failed, %u the same as one "
             "built before, %u without a shader this game version or its shader cache has, %u vertex pipelines left to "
             "their first draw (NX2SARIA_PREPARE_PIPELINES=%s), %u mesh pipelines this GPU cannot draw; on %d threads in "
             "%.0f ms; pipeline cache %s", n, built, mesh, failed, known, dropped, other,
             !strcmp(mode, "all") ? "all" : all ? "mesh, every record without the graphics pipeline library" : "mesh", no_mesh,
             nt ? nt : 1, now_ms() - t0,
             saved ? "saved" : "not saved (builds kept running, or its file could not be written)");
    printf("%s\n", msg);
    fflush(stdout);
    rt_log("vk_pipeline: %s", msg);
    fb_log("after the listed pipelines");
    return 0;
}

static void prep_card_line(const char *mode) {
    const char *text = vk_card_prepare_text();
    if (!text || !*text) return;
    printf("%s: card: %s\n", mode, text);
    fflush(stdout);
}

int vk_pipeline_prepare_pipelines(void) {
    prep_mode = 1;
    if (!ensure_init()) {
        fprintf(stderr, "prepare-pipelines: no Vulkan device\n");
        return 1;
    }
    prep_card_line("prepare-pipelines");
    char dir[PLAT_PATH_MAX * 2];
    if (!cache_dir(dir, sizeof dir)) return 1;
    char path[PLAT_PATH_MAX * 2 + 48];
    snprintf(path, sizeof path, "%s/seed-pipeline-keys.bin", dir);
    if (!plat_file_exists(path)) {
        fprintf(stderr, "prepare-pipelines: no %s (tools/prepare_shaders.py writes it from the pipeline list)\n", path);
        return 1;
    }
    return seed_pipelines(dir);
}

int vk_pipeline_prepare_shaders(void) {
    prep_mode = 1;
    if (!ensure_init()) {
        fprintf(stderr, "prepare-shaders: no Vulkan device\n");
        return 1;
    }
    prep_card_line("prepare-shaders");
    char dir[PLAT_PATH_MAX * 2];
    if (!cache_dir(dir, sizeof dir)) return 1;
    int cpus = (int)plat_cpu_usable();
    const char *pe = cfg_get("NX2SARIA_PREWARM_XLATE");
    int procs = pe && *pe ? atoi(pe) : cpus / 2 > 16 ? 16 : cpus / 2;
    int rc = gfx_shader_prepare_seed(dir, cpus, procs < 1 ? 1 : procs);
    if (!rc) seed_pipelines(dir);
    if (!rc) lb_warm_prepared(dir, 0);
    return rc;
}

int vk_pipeline_warm_shaders(void) {
    prep_mode = 1;
    if (!ensure_init()) {
        fprintf(stderr, "warm-shaders: no Vulkan device\n");
        return 1;
    }
    prep_card_line("prepare-shaders");
    char dir[PLAT_PATH_MAX * 2];
    if (!cache_dir(dir, sizeof dir)) return 1;
    char prp[PLAT_PATH_MAX * 2 + 32];
    snprintf(prp, sizeof prp, "%s/shader-prepared.bin", dir);
    if (!plat_file_exists(prp)) {
        fprintf(stderr, "warm-shaders: no %s (tools/prepare_shaders.py writes it)\n", prp);
        return 1;
    }
    seed_pipelines(dir);
    lb_warm_prepared(dir, 1);
    return 0;
}

#define PROBE_DISPATCHES 256
int vk_pipeline_probe_prewarm(void) {
    vk_pipeline_async_enable();
    CU.quiet = 1;
    vk_pipeline_prewarm();
    if (!P.ready) {
        printf("probe-prewarm: SKIP (the boot prewarm brought no Vulkan device up: none on this machine, NX2SARIA_GPU=null "
               "or NX2SARIA_PIPE_SYNC=1)\n");
        return 0;
    }
    while (__atomic_load_n(&LB.running, __ATOMIC_ACQUIRE) > 0) plat_sleep_ms(50);
    if (!LB.n) {
        printf("probe-prewarm: SKIP (no recorded compute shaders next to the executable: %s)\n", P.cache_path);
        return 0;
    }
    VkPipeStats st;
    vk_pipeline_stats(&st);
    plat_mutex_lock_shared(&LB.stat);
    uint64_t kept = LB.built[2], cache_only = LB.warmed[2], failed = LB.fail[2], there = LB.have[2];
    uint64_t big_unused = LB.big_unused;
    size_t queued = BG.n;
    plat_mutex_unlock_shared(&LB.stat);
    printf("probe-prewarm: %zu compute shaders, %zu to keep: %llu kept, %llu built into the pipeline cache only, %llu "
           "there already, %llu failed, %llu large ones not built (no run dispatched them), %zu large ones for the "
           "background builder; %llu graphics pipelines\n", LB.n, LB.n_keep, (unsigned long long)kept,
           (unsigned long long)cache_only, (unsigned long long)there, (unsigned long long)failed,
           (unsigned long long)big_unused, queued, (unsigned long long)st.graphics);
    int fails = kept > LB.n_keep;
    if (fails) printf("probe-prewarm: FAIL: %llu pipelines kept, %zu expected at most\n", (unsigned long long)kept, LB.n_keep);
    if (queued > LB.n_keep)
        fails++, printf("probe-prewarm: FAIL: %zu large compute shaders queued for the background builder, %zu kept "
                        "shaders at most could be among them\n", queued, LB.n_keep);
    uint32_t asked = 0, got = 0;
    double t_all = 0, t_max = 0, t_load = 0;
    for (size_t i = LB.n_keep; i < LB.n && asked < PROBE_DISPATCHES; i++) {
        if (LB.e[i].stage != GFX_STAGE_CS) continue;
        double l0 = now_ms();
        const GfxShader *sh = gfx_shader_load_input(LB.e[i].iid, 0);
        double load = now_ms() - l0;
        if (!sh || gfx_shader_stage(sh) != GFX_STAGE_CS) continue;
        uint64_t words = gfx_shader_words(sh);
        if (LB.big_words && words > LB.big_words) continue;
        const VkPipeInfo *info = NULL;
        double t0 = now_ms();
        VkPipeline p = vk_pipeline_compute(sh, &info);
        double ms = now_ms() - t0;
        asked++, got += p != VK_NULL_HANDLE;
        t_all += ms, t_load += load;
        if (ms > t_max) t_max = ms;
    }
    vk_mem_stage_log("the probe's first dispatches");
    plat_mutex_lock_shared(&P.lock);
    uint64_t hit = CF.hit;
    plat_mutex_unlock_shared(&P.lock);
    printf("probe-prewarm: %u first dispatches of pipelines the boot did not keep: %u created, avg %.2f ms, longest %.2f "
           "ms, the pipeline cache held %llu of them; reading their shaders took avg %.2f ms\n", asked, got,
           asked ? t_all / asked : 0.0, t_max, (unsigned long long)hit, asked ? t_load / asked : 0.0);
    if (got != asked) fails++, printf("probe-prewarm: FAIL: %u pipelines were not created\n", asked - got);
    if (CU.opened) {
        char rp[sizeof CU.path + 8];
        snprintf(rp, sizeof rp, "%s.probe", CU.path);
        uint64_t rsize = 0;
        CuHeader rh = {0};
        uint32_t mine = 0;
        int wrote = cu_write(rp);
        uint8_t *rf = wrote ? plat_file_read(rp, &rsize) : NULL;
        long rn = cu_parse(rf, rsize, &rh);
        for (long i = 0; i < rn; i++) {
            CuRec r;
            memcpy(&r, rf + sizeof rh + (size_t)i * sizeof r, sizeof r);
            mine += r.run == CU.run;
        }
        free(rf);
        plat_file_delete(rp);
        printf("probe-prewarm: the record of dispatched compute shaders: %ld entries, %u of this run (run %u)\n", rn, mine,
               CU.run);
        if (!wrote || rn < 0 || rh.run != CU.run || mine != got)
            fails++, printf("probe-prewarm: FAIL: the record does not hold the %u shaders dispatched here\n", got);
    }
    lb_stop();
    vk_pipeline_save();
    printf("probe-prewarm: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}

void vk_pipeline_set_dynamic(VkCommandBuffer cb, const VkPipeState *s) {
    VkDynShadow sh;
    uint64_t skipped = 0;
    sh.have = 0;
    vk_pipeline_set_dynamic_delta(cb, s, &sh, 0, &skipped);
}

int vk_pipeline_set_dynamic_delta(VkCommandBuffer cb, const VkPipeState *s, VkDynShadow *sh, int mesh, uint64_t *skipped) {
    if (!ensure_init()) return 0;
    const poly_st *pl = &s->poly;
    const ds_st *d = &s->ds;
    int core = (sh->have & VK_DYN_CORE) != 0, vtx = !mesh && (sh->have & VK_DYN_VTX), blend = (sh->have & VK_DYN_BLEND) != 0;
    int n = 0, left = 0;
#define DYN(known, field, val, call)                                         \
    do {                                                                     \
        if ((known) && sh->field == (val)) left++;                           \
        else sh->field = (val), call, n++;                                   \
    } while (0)
    int front_cw = pl->front == NVN_FRONT_FACE_CW;
    VkCullModeFlags cull = (VkCullModeFlags)(pl->cull & 3);
    VkFrontFace front = front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    DYN(core, cull, cull, vkCmdSetCullMode(cb, cull));
    DYN(core, front, front, vkCmdSetFrontFace(cb, front));
    if (!mesh) {
        VkPrimitiveTopology topo = vk_pipeline_topology(s->prim);
        DYN(vtx, topo, topo, vkCmdSetPrimitiveTopology(cb, topo));
    }
    DYN(core, dt, (VkBool32)d->dt, vkCmdSetDepthTestEnable(cb, d->dt));
    DYN(core, dw, (VkBool32)d->dw, vkCmdSetDepthWriteEnable(cb, d->dw));
    DYN(core, dop, compare_op(d->df), vkCmdSetDepthCompareOp(cb, compare_op(d->df)));
    DYN(core, st, (VkBool32)d->st, vkCmdSetStencilTestEnable(cb, d->st));
    for (int f = 0; f < 2; f++) {
        VkStencilOp o[3] = {stencil_op(f ? d->fail_b : d->fail_f), stencil_op(f ? d->zpass_b : d->zpass_f),
                            stencil_op(f ? d->zfail_b : d->zfail_f)};
        VkCompareOp c = compare_op(f ? d->sf_b : d->sf_f);
        if (core && sh->scmp[f] == c && !memcmp(sh->sop[f], o, sizeof o)) {
            left++;
            continue;
        }
        vkCmdSetStencilOp(cb, f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT, o[0], o[1], o[2], c);
        sh->scmp[f] = c, memcpy(sh->sop[f], o, sizeof o), n++;
    }
    unsigned mode = pl->mode <= NVN_POLYGON_MODE_FILL ? pl->mode : NVN_POLYGON_MODE_FILL;
    VkBool32 bias_en = (pl->offset_en >> mode & 1) != 0, discard = s->raster_discard != 0, restart = s->prim_restart != 0;
    DYN(core, bias_en, bias_en, vkCmdSetDepthBiasEnable(cb, bias_en));
    DYN(core, discard, discard, vkCmdSetRasterizerDiscardEnable(cb, discard));
    if (!mesh) DYN(vtx, restart, restart, vkCmdSetPrimitiveRestartEnable(cb, restart));
    if (!mesh && P.dyn_vi && s->vs) {
        VtxInput v;
        vertex_input(s, &v);
        if (vtx && sh->vi.n_attr == v.n_attr && sh->vi.n_bind == v.n_bind &&
            !memcmp(sh->vi.a, v.a, sizeof v.a[0] * v.n_attr) && !memcmp(sh->vi.b, v.b, sizeof v.b[0] * v.n_bind)) {
            left++;
        } else {
            rec_vertex_input(P.set_vi, cb, v.n_bind, v.b, v.n_attr, v.a);
            sh->vi = v, n++;
        }
    } else if (!mesh) {
        sh->vi.n_attr = UINT32_MAX;
    }
    if (P.dyn_clamp) {
        VkBool32 clamp = s->depth_clamp != 0;
        DYN(core, clamp, clamp, rec_bool(P.set_clamp, cb, clamp));
    }
    if (P.dyn_poly) {
        VkPolygonMode pm = polygon_mode(pl->mode);
        DYN(core, poly, pm, rec_polygon_mode(P.set_poly, cb, pm));
    }
    if (P.dyn_a2c) {
        VkBool32 a2c = s->ms.a2c != 0;
        DYN(core, a2c, a2c, rec_bool((PFN_vkCmdSetDepthClampEnableEXT)P.set_a2c, cb, a2c));
    }
#undef DYN
    sh->have |= VK_DYN_CORE;
    if (mesh) sh->have &= ~(uint32_t)VK_DYN_VTX;
    else sh->have |= VK_DYN_VTX;
    int nt = target_count(s);
    if (nt) {
        VkBool32 en[8];
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8];
        for (int i = 0; i < nt; i++) {
            const blend_st *b = &s->blend[i];
            en[i] = (s->color.blend_en >> i & 1) && blendable(target_format(s->color_fmt[i], NULL, NULL));
            eq[i] = (VkColorBlendEquationEXT){blend_factor(b->src_rgb), blend_factor(b->dst_rgb), blend_op(b->eq_rgb),
                                              blend_factor(b->src_a), blend_factor(b->dst_a), blend_op(b->eq_a)};
            wm[i] = target_wmask(s, i);
        }
        int known = blend && (uint32_t)nt <= sh->nt;
        if (P.dyn_blend_en) {
            if (known && !memcmp(sh->en, en, sizeof en[0] * nt)) left++;
            else rec_blend_enable(P.set_blend_en, cb, 0, (uint32_t)nt, en), n++;
        }
        if (P.dyn_blend_eq) {
            if (known && !memcmp(sh->eq, eq, sizeof eq[0] * nt)) left++;
            else rec_blend_equation(P.set_blend_eq, cb, 0, (uint32_t)nt, eq), n++;
        }
        if (P.dyn_wmask) {
            if (known && !memcmp(sh->wm, wm, sizeof wm[0] * nt)) left++;
            else rec_write_mask(P.set_wmask, cb, 0, (uint32_t)nt, wm), n++;
        }
        memcpy(sh->en, en, sizeof en[0] * nt), memcpy(sh->eq, eq, sizeof eq[0] * nt), memcpy(sh->wm, wm, sizeof wm[0] * nt);
        sh->nt = blend && sh->nt > (uint32_t)nt ? sh->nt : (uint32_t)nt;
        sh->have |= VK_DYN_BLEND;
    }
    *skipped += (uint64_t)left;
    return n;
}

void vk_pipeline_stats(VkPipeStats *s) {
    plat_mutex_lock_shared(&P.lock);
    *s = P.stats;
    plat_mutex_unlock_shared(&P.lock);
}

static PlatMutex save_lock = PLAT_MUTEX_INIT;
#define SAVE_SLOW_MS 1000

static int cache_save_one(int which, uint32_t wait_ms, int exiting) {
    VkPipelineCache cache = which ? P.play : P.cache;
    const char *path = which ? P.play_path : P.cache_path;
    if (!cache) return 1;
    uint64_t until = plat_now_ms() + wait_ms;
    if (exiting) __atomic_store_n(&cache_saving_at_exit, 1, __ATOMIC_RELEASE);
    while (!plat_mutex_trylock(&cache_rw)) {
        if (plat_now_ms() >= until) {
            if (exiting) __atomic_store_n(&cache_saving_at_exit, 0, __ATOMIC_RELEASE);
            return 0;
        }
        plat_sleep_ms(2);
    }
    size_t size = 0, room = 0;
    PlatFileSwap *swap = NULL;
    void *data = NULL;
    int sized = vkGetPipelineCacheData(P.device, cache, &size, NULL) == VK_SUCCESS && size;
    if (sized) __atomic_store_n(&P.held[which], (uint64_t)size, __ATOMIC_RELAXED);
    static int over_said[2];
    uint64_t cap = cache_cap();
    if (sized && cap && size > cap && !prep_mode) {
        plat_mutex_unlock(&cache_rw);
        if (exiting) __atomic_store_n(&cache_saving_at_exit, 0, __ATOMIC_RELEASE);
        if (!over_said[which]++)
            rt_log("vk_pipeline: %s not written past its last save: the cache holds %.1f MB, over the limit of %llu MB; "
                   "pipelines made from here on compile again at the next start (NX2SARIA_PIPELINE_CACHE_MAX_MB raises "
                   "the limit)", path, size / 1048576.0, (unsigned long long)(cap >> 20));
        return 1;
    }
    if (sized && cap && size > cap)
        rt_log("vk_pipeline: %s holds %.1f MB, over the limit of %llu MB: written for the preparation, and dropped at "
               "the next start unless NX2SARIA_PIPELINE_CACHE_MAX_MB is raised", path, size / 1048576.0,
               (unsigned long long)(cap >> 20));
    int same = sized && size == P.saved[which];
    if (sized && !same) room = size, data = plat_file_swap_begin(path, room, &swap);
    uint64_t copy_t0 = plat_now_ms();
    int got = data && vkGetPipelineCacheData(P.device, cache, &size, data) == VK_SUCCESS && size <= room;
    uint64_t copy_ms = plat_now_ms() - copy_t0;
    plat_mutex_unlock(&cache_rw);
    if (exiting) __atomic_store_n(&cache_saving_at_exit, 0, __ATOMIC_RELEASE);
    static uint64_t skipped[2];
    int failed = !sized || (!same && !got);
    if (same) skipped[which]++;
    if (failed) rt_log("vk_pipeline: %s not saved: no copy of its %.1f MB", path, (room ? room : size) / 1048576.0);
    if (data) {
        uint64_t t0 = plat_now_ms();
        if (!plat_file_swap_end(swap, size, got)) {
            rt_log("vk_pipeline: cannot write %s (%.1f MB)", path, size / 1048576.0);
            failed = 1;
        } else {
            uint64_t loaded = which ? P.play_loaded : P.cache_loaded, write_ms = plat_now_ms() - t0;
            char slow[120] = "";
            if (write_ms >= SAVE_SLOW_MS || copy_ms >= SAVE_SLOW_MS) {
                char at[16];
                log_stamp(at, sizeof at, 0);
                snprintf(slow, sizeof slow, "; slow: the driver's data was copied in %llu ms, the save ended at %s",
                         (unsigned long long)copy_ms, at);
            }
            rt_log("vk_pipeline: %s saved: %.1f MB (%+.1f MB since the boot) written in %llu ms; %llu saves before it were "
                   "skipped with the data unchanged%s", path, size / 1048576.0, ((double)size - (double)loaded) / 1048576.0,
                   (unsigned long long)write_ms, (unsigned long long)skipped[which], slow);
            P.saved[which] = size, skipped[which] = 0;
            if (!which && P.fold) {
                P.fold = 0;
                plat_file_delete(P.play_path);
                rt_log("vk_pipeline: %s removed: %s holds its pipelines", P.play_path, P.cache_path);
            }
            if (P.feedback) {
                char when[96];
                snprintf(when, sizeof when, "at a save of %.1f MB (%+.1f MB since the boot)", size / 1048576.0,
                         ((double)size - (double)loaded) / 1048576.0);
                fb_log(when);
            }
        }
    }
    return failed ? -1 : 1;
}

static int cache_save(uint32_t wait_ms, int exiting) {
    if (!P.ready || !P.cache || !P.cache_path[0]) return 1;
    plat_mutex_lock(&save_lock);
    if (!P.cache) {
        plat_mutex_unlock(&save_lock);
        return 1;
    }
    int ok = 1, r;
    if (cache_save_writes_base(P.play != VK_NULL_HANDLE, P.fold, P.base_at_exit, exiting))
        ok = cache_save_one(0, wait_ms, exiting);
    static int fold_kept_said;
    if (P.play && P.fold) {
        if (!fold_kept_said++)
            rt_log("vk_pipeline: %s is kept as it is until %s holds what was folded from it; this run's new pipelines "
                   "are saved once that write is done", P.play_path, P.cache_path);
    } else if (P.play && (r = cache_save_one(1, wait_ms, exiting)) < ok) {
        ok = r;
    }
    plat_mutex_unlock(&save_lock);
    return ok;
}

#define SAVE_WAIT_MS 5000

static int save_with(uint32_t wait_ms, int exiting) {
    if (P.ready) keys_flush(), pb_flush(), cu_flush(), cf_log();
    if (P.ready && P.gpl) {
        rt_log("vk_pipeline: gpl: %llu fast links (%llu by workers, %llu of those by the library workers for skipped draws; "
               "%llu of pipelines with a feedback loop; avg %.2f ms, "
               "max %.2f ms, %llu failed), %llu lookups by a draw found a stage library not made in this process yet and "
               "left it to a worker (whether the driver's cache has it shows in the library times below), %llu optimised "
               "pipelines swapped in (%llu queued "
               "low after their fast link; %llu replaced fast links given back), %llu drawing-thread links over 5 ms; %llu "
               "waiting draws built their libraries "
               "(avg %.2f ms, max %.2f ms)",
               (unsigned long long)P.g.links, (unsigned long long)P.g.wlinks, (unsigned long long)P.g.flinks,
               (unsigned long long)P.g.fb_links,
               P.g.links ? P.g.link_ms / P.g.links : 0.0,
               P.g.link_max, (unsigned long long)P.g.link_fail, (unsigned long long)P.g.defer,
               (unsigned long long)P.g.swaps, (unsigned long long)P.g.requeued, (unsigned long long)fast_retired,
               (unsigned long long)P.g.slow,
               (unsigned long long)P.g.dlinks, P.g.dlinks ? P.g.dlink_ms / P.g.dlinks : 0.0, P.g.dlink_max);
        uint64_t rn = P.g.ready_n[0] + P.g.ready_n[1] + P.g.ready_n[2] + P.g.ready_n[3] + P.g.ready_n[4] + P.g.ready_n[5];
        rt_log("vk_pipeline: gpl: pipelines a draw missed were usable after under 4 ms %llu, 4-8 ms %llu, 8-17 ms %llu, "
               "17-50 ms %llu, 50-150 ms %llu, longer %llu (avg %.1f ms, longest %.0f ms); %llu of them had no fast link; "
               "%llu were mesh pipelines (counted by themselves below), %llu were named in pipeline-keys.bin and not "
               "prebuilt yet (the others were new to this install), %llu had a shader translated in this session",
               (unsigned long long)P.g.ready_n[0], (unsigned long long)P.g.ready_n[1], (unsigned long long)P.g.ready_n[2],
               (unsigned long long)P.g.ready_n[3], (unsigned long long)P.g.ready_n[4], (unsigned long long)P.g.ready_n[5],
               rn ? P.g.ready_ms / (double)rn : 0.0, P.g.ready_max, (unsigned long long)P.g.ready_nofast,
               (unsigned long long)P.g.ready_mesh, (unsigned long long)P.g.ready_recorded,
               (unsigned long long)P.g.ready_fresh);
        if (vk_ctx()->has_mesh_shader && !vk_ctx()->mesh_fallback)
            rt_log("vk_pipeline: gpl: mesh pipelines (%s): %llu fast links (avg %.2f ms, max %.2f ms); the %llu a draw "
                   "missed were usable after avg %.1f ms, longest %.0f ms, %llu of them through a fast link (the others "
                   "after their whole build); their libraries made in this run: %llu of task and mesh stages (avg %.1f "
                   "ms, max %.0f ms), %llu fragment shader (avg %.1f ms, max %.0f ms)",
                   mesh_libs() ? "linked from stage libraries" : "built whole, without stage libraries",
                   (unsigned long long)P.g.m_links, P.g.m_links ? P.g.m_link_ms / (double)P.g.m_links : 0.0,
                   P.g.m_link_max, (unsigned long long)P.g.ready_mesh,
                   P.g.ready_mesh ? P.g.m_ready_ms / (double)P.g.ready_mesh : 0.0, P.g.m_ready_max,
                   (unsigned long long)P.g.m_ready_fast, (unsigned long long)P.g.m_lib_n[0],
                   P.g.m_lib_n[0] ? P.g.m_lib_ms[0] / (double)P.g.m_lib_n[0] : 0.0, P.g.m_lib_max[0],
                   (unsigned long long)P.g.m_lib_n[1], P.g.m_lib_n[1] ? P.g.m_lib_ms[1] / (double)P.g.m_lib_n[1] : 0.0,
                   P.g.m_lib_max[1]);
        if (P.g.hints)
            rt_log("vk_pipeline: gpl: looking ahead named %llu pipeline states not known yet and queued %llu stage libraries "
                   "for them", (unsigned long long)P.g.hints, (unsigned long long)P.g.hint_libs);
        if (A.first || A.taken)
            rt_log("vk_pipeline: whole builds a draw waited for while they were still queued: %llu moved to the head of "
                   "the queue, %llu built by the waiting thread (a wait outside the frame's budget, no worker idle)",
                   (unsigned long long)A.first, (unsigned long long)A.taken);
        if (U.waits || U.queued)
            rt_log("vk_pipeline: gpl: library jobs for waiting draws: %llu queued, %llu built by a worker, %llu by the "
                   "thread that needed them (no worker free), %llu waits ended at their limit; %llu fast links on the %d "
                   "library workers",
                   (unsigned long long)U.queued, (unsigned long long)U.by_worker, (unsigned long long)U.stolen,
                   (unsigned long long)U.timeouts, (unsigned long long)U.links, U.lib_workers);
        static const char *const kn[4] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};
        for (int i = 0; i < 4; i++)
            rt_log("vk_pipeline: gpl: %s libraries %llu (avg %.2f ms, max %.2f ms; %llu of 25 ms or more, %.0f ms in all)",
                   kn[i], (unsigned long long)P.g.lib_n[i], P.g.lib_n[i] ? P.g.lib_ms[i] / P.g.lib_n[i] : 0.0, P.g.lib_max[i],
                   (unsigned long long)P.g.lib_slow[i], P.g.lib_slow_ms[i]);
        if (P.g.pr_added)
            rt_log("vk_pipeline: gpl: %llu of the pre-rasterization libraries had outputs added for their fragment shaders "
                   "(avg %.1f ms): another module than the one the preparation built, so the driver's cache has none of them",
                   (unsigned long long)P.g.pr_added, P.g.pr_added_ms / (double)P.g.pr_added);
        if (LB.n)
            rt_log("vk_pipeline: gpl: prebuilt libraries a draw used: pre-rasterization %llu, fragment shader %llu; "
                   "compute pipelines built on the replay thread %llu", (unsigned long long)P.g.pre_used[LIB_PR],
                   (unsigned long long)P.g.pre_used[LIB_FS], (unsigned long long)LB.cs_draw);
    }
    pg_log();
    gfx_shader_record_flush(1);
    if (P.ready && !P.cache) pb_log("at a save");
    int saved = cache_save(wait_ms, exiting);
    if (!saved && wait_ms)
        rt_log("vk_pipeline: %s not saved: pipeline builds kept running for %u ms", P.play ? P.play_path : P.cache_path,
               (unsigned)wait_ms);
    return saved;
}

void vk_pipeline_save(void) {
    save_with(SAVE_WAIT_MS, 0);
}

void vk_pipeline_save_exit(void) {
    journal_close();
    if (plat_cas32(&LW.running, 0, 0) > 0 && !plat_xchg32(&LW.stop, 1)) lw_keep_progress();
    save_with(SAVE_WAIT_MS, 1);
}

#define SIZES_EVERY_MS 60000
static void cache_sizes_ask(void) {
    if (!plat_mutex_trylock(&save_lock)) return;
    if (plat_mutex_trylock(&cache_rw)) {
        for (int w = 0; w < 2; w++) {
            VkPipelineCache cache = w ? P.play : P.cache;
            size_t size = 0;
            if (cache && vkGetPipelineCacheData(P.device, cache, &size, NULL) == VK_SUCCESS && size)
                __atomic_store_n(&P.held[w], (uint64_t)size, __ATOMIC_RELAXED);
        }
        plat_mutex_unlock(&cache_rw);
    }
    plat_mutex_unlock(&save_lock);
}

static void autosave_once(void) {
    static uint64_t last_keys, last_sizes;
    static SaveRetry sr;
    if (!P.ready) return;
    uint64_t now = plat_now_ms();
    __atomic_store_n(&pipe_minute, minute_now(), __ATOMIC_RELAXED);
    release_drain();
    pressure_release(now);
    if (now - last_sizes >= SIZES_EVERY_MS && (P.cache || P.play)) last_sizes = now, cache_sizes_ask();
    if (now - last_keys >= 2000) {
        last_keys = now;
        if (*(volatile uint32_t *)&K.pend_n) keys_flush();
        pb_flush();
        cu_flush();
    }
    gfx_shader_record_flush(0);
    if (now - sr.at_ms < SAVE_EVERY_MS) return;
    VkPipeStats s;
    vk_pipeline_stats(&s);
    uint64_t n = s.graphics + s.compute + cache_merges;
    if (!save_due(&sr, n, now, P.fold)) return;
    if (P.cache && P.cache_path[0]) {
        if (!plat_mutex_trylock(&cache_rw)) return;
        plat_mutex_unlock(&cache_rw);
    }
    if (save_tried(&sr, n, now, save_with(0, 0) > 0))
        rt_log("vk_pipeline: the pipeline cache was not saved in %d tries in a row; the next try is made when a new "
               "pipeline is built, or at exit", SAVE_RETRIES + 1);
}

static void autosave_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    for (;;) {
        plat_sleep_ms(500);
        autosave_once();
    }
}

void vk_pipeline_autosave(void) {
    static int32_t started;
    if (!P.ready || plat_xchg32(&started, 1)) return;
    if (!plat_thread_spawn(autosave_main, NULL, "pipe autosave", 0))
        rt_log("vk_pipeline: cannot start the autosave thread; the cache is saved at exit only");
}

void vk_pipeline_shutdown(void) {
    if (!P.ready) return;
    lb_stop();
    compute_warm_clear();
    workers_drain();
    vk_pipeline_save();
    plat_mutex_lock(&P.lock);
    vkDeviceWaitIdle(P.device);
    for (int i = 0; i < PIPE_BUCKETS; i++) {
        for (Pipe *p = pipes[i], *n; p; p = n) {
            n = p->next;
            if (p->info.pipeline) vkDestroyPipeline(P.device, p->info.pipeline, NULL);
            if (p->fast && p->fast != p->info.pipeline) vkDestroyPipeline(P.device, p->fast, NULL);
            free(p->bindings), free(p);
        }
        pipes[i] = NULL;
    }
    libs_destroy();
    for (int i = 0; i < LAYOUT_BUCKETS; i++) {
        for (Layout *l = layouts[i], *n; l; l = n) {
            n = l->next;
            for (int k = 0; k < 2; k++)
                if (l->tpl[k]) P.tpl_destroy(P.device, l->tpl[k], NULL);
            vkDestroyPipelineLayout(P.device, l->layout, NULL);
            for (int h = 0; h < 2; h++)
                if (l->half[h]) vkDestroyPipelineLayout(P.device, l->half[h], NULL);
            vkDestroyDescriptorSetLayout(P.device, l->set, NULL);
            if (l->set1) vkDestroyDescriptorSetLayout(P.device, l->set1, NULL);
            free(l);
        }
        layouts[i] = NULL;
    }
    plat_mutex_lock(&save_lock);
    if (P.cache) vkDestroyPipelineCache(P.device, P.cache, NULL);
    if (P.play) vkDestroyPipelineCache(P.device, P.play, NULL);
    P.cache = P.play = VK_NULL_HANDLE;
    plat_mutex_unlock(&save_lock);
    memset(&P.stats, 0, sizeof P.stats);
    plat_xchg32(&P.ready, 0);
    plat_mutex_unlock(&P.lock);
}

typedef struct { const char *key, *c1, *src; size_t c1_len, src_len; int stage; uint32_t local[3]; } CensusEntry;

static const char *st_field(const char *from, const char *to, const char *name, size_t *len) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\": \"", name);
    const char *p = strstr(from, pat);
    if (!p || p >= to) return NULL;
    p += strlen(pat);
    const char *e = memchr(p, '"', (size_t)(to - p));
    if (!e) return NULL;
    *len = (size_t)(e - p);
    return p;
}

static int census_cmp(const void *x, const void *y) {
    const CensusEntry *a = x, *b = y;
    size_t n = a->src_len < b->src_len ? a->src_len : b->src_len;
    int c = memcmp(a->src, b->src, n);
    if (c) return c;
    if (a->src_len != b->src_len) return a->src_len < b->src_len ? -1 : 1;
    return a->stage - b->stage;
}

static char *census_read(const char *root, size_t *size) {
    char path[PLAT_PATH_MAX * 2 + 64];
    snprintf(path, sizeof path, "%s/jobs.json", root);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *size = (size_t)n;
    return b;
}

static int census_parse(const char *jobs, CensusEntry *out, int max) {
    int n = 0;
    for (const char *p = jobs; n < max && (p = strstr(p, "\"job\": {")); p += 8) {
        const char *end = strstr(p, "\"origin\": ");
        if (!end) break;
        size_t kl, sl, cl, rl;
        const char *k = st_field(p, end, "key", &kl), *s = st_field(p, end, "stage", &sl), *c = st_field(p, end, "c1", &cl),
                   *r = st_field(p, end, "source", &rl);
        if (!k || !s || !c || !r || kl != 64) continue;
        CensusEntry *e = &out[n++];
        e->key = k, e->c1 = c, e->c1_len = cl / 2, e->src = r, e->src_len = rl;
        e->stage = !strncmp(s, "vs", sl) ? GFX_STAGE_VS : !strncmp(s, "ps", sl) ? GFX_STAGE_FS : GFX_STAGE_CS;
        e->local[0] = e->local[1] = e->local[2] = 1;
        const char *l = strstr(p, "\"local\": [");
        if (l && l < end) {
            l += 10;
            for (int i = 0; i < 3; i++) {
                while (*l == ' ' || *l == '\n' || *l == '\r' || *l == ',') l++;
                e->local[i] = (uint32_t)strtoul(l, (char **)&l, 10);
            }
        }
    }
    return n;
}

static int hexv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

static const GfxShader *census_shader(const char *root, const CensusEntry *e) {
    char path[PLAT_PATH_MAX * 2 + 128];
    static uint8_t c1[8192];
    uint8_t control[0x880] = {0};
    if (e->c1_len > sizeof c1) return NULL;
    for (size_t b = 0; b < e->c1_len; b++) c1[b] = (uint8_t)(hexv(e->c1[b * 2]) << 4 | hexv(e->c1[b * 2 + 1]));
    snprintf(path, sizeof path, "%s/code/%.64s.code", root, e->key);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long body = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t skip = e->stage == GFX_STAGE_CS ? 0 : 0x30, total = skip + (size_t)body;
    uint8_t *code = calloc(1, total);
    int ok = code && fread(code + skip, 1, (size_t)body, f) == (size_t)body;
    fclose(f);
    if (!ok) return free(code), NULL;
    uint32_t c1_off = (uint32_t)(total - e->c1_len), length = (uint32_t)total;
    memcpy(control + 0x700, &c1_off, 4), memcpy(control + 0x704, &length, 4);
    GfxShaderAnswers a = {0};
    if (e->stage == GFX_STAGE_CS) for (int k = 0; k < 3; k++) a.local[k] = (uint16_t)e->local[k];
    const GfxShader *s = gfx_shader_get(e->stage, control, code, total, c1, &a);
    free(code);
    return s;
}

static void selftest_state(VkPipeState *s, const GfxShader *vs, const GfxShader *fs) {
    vk_pipe_state_defaults(s);
    s->vs = vs, s->fs = fs;
    GfxShaderIo io[32];
    uint32_t outs = gfx_shader_outputs(fs, io);
    for (int i = 0; i < 8; i++)
        if (outs >> i & 1)
            s->color_fmt[i] = io[i].type == GFX_TYPE_UINT ? NVN_FORMAT_RGBA8UI : io[i].type == GFX_TYPE_SINT ? NVN_FORMAT_RGBA8I
                                                                                                            : NVN_FORMAT_RGBA8;
    s->depth_fmt = NVN_FORMAT_DEPTH32F_STENCIL8;
    s->ds.dt = 1;
    s->color.blend_en = 1;
    s->blend[0].src_rgb = NVN_BLEND_FUNC_SRC_ALPHA, s->blend[0].dst_rgb = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA;
    s->poly.cull = NVN_FACE_BACK;
    uint32_t ins = gfx_shader_inputs(vs, io), off = 0;
    for (int l = 0; l < 16; l++)
        if (ins >> l & 1) {
            va_st a = {io[l].type == GFX_TYPE_UINT ? NVN_FORMAT_RGBA32UI : io[l].type == GFX_TYPE_SINT ? NVN_FORMAT_RGBA32I
                                                                                                      : NVN_FORMAT_RGBA32F,
                       off, 0};
            memcpy(&s->vattr[l], &a, sizeof a);
            off += 16;
        }
    s->vstream[0].stride = off ? off : 16;
}

static int selftest_record(const VkPipeState *s, VkPipeline pipe) {
    VkCtx *c = vk_ctx();
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = c->gfx_family};
    VkCommandPool pool;
    if (vkCreateCommandPool(c->device, &pi, NULL, &pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    int ok = vkAllocateCommandBuffers(c->device, &ai, &cb) == VK_SUCCESS;
    if (ok) {
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vk_pipeline_set_dynamic(cb, s);
        ok = vkEndCommandBuffer(cb) == VK_SUCCESS;
    }
    vkDestroyCommandPool(c->device, pool, NULL);
    return ok;
}

int vk_pipeline_selftest(void) {
    enum { MAXN = 20000, WANT_PAIRS = 64, WANT_CS = 16 };
    char root[PLAT_PATH_MAX * 2];
    if (!vk_init() || !ensure_init()) { printf("vk_pipeline selftest: FAILED: no Vulkan device\n"); return 1; }
    if (!probe_census_dir(root, sizeof root)) {
        printf("vk_pipeline selftest: FAILED: no build/v%d/shaders/jobs.json above the exe\n", rt_game_version);
        return 1;
    }
    size_t size;
    char *jobs = census_read(root, &size);
    CensusEntry *e = malloc(sizeof *e * MAXN);
    int n = jobs && e ? census_parse(jobs, e, MAXN) : 0;
    if (!n) { printf("vk_pipeline selftest: FAILED: no census entries\n"); free(jobs), free(e); return 1; }
    qsort(e, (size_t)n, sizeof *e, census_cmp);
    long err0 = vk_validation_errors_total();
    VkPipeStats st0;
    vk_pipeline_stats(&st0);

    int (*pairs)[2] = malloc(sizeof *pairs * (size_t)n), np = 0, *cs = malloc(sizeof *cs * (size_t)n), nc = 0;
    for (int i = 0; i < n;) {
        int j = i, v0 = -1, nv = 0, nf = 0;
        for (; j < n && e[j].src_len == e[i].src_len && !memcmp(e[j].src, e[i].src, e[i].src_len); j++) {
            if (e[j].stage == GFX_STAGE_VS && v0 < 0) v0 = j;
            nv += e[j].stage == GFX_STAGE_VS;
            if (e[j].stage == GFX_STAGE_CS) cs[nc++] = j;
        }
        for (int k = i; k < j && nv; k++)
            if (e[k].stage == GFX_STAGE_FS) pairs[np][0] = v0 + nf++ % nv, pairs[np][1] = k, np++;
        i = j;
    }
    int tried = 0, built = 0, cached = 1, shader_fail = 0, ctried = 0, cbuilt = 0, recorded = 0, varying_fix = 0;
    for (int t = 0; t < WANT_PAIRS && t < np; t++) {
        const int *pr = pairs[(long long)t * np / (np < WANT_PAIRS ? np : WANT_PAIRS)];
        tried++;
        const GfxShader *vs = census_shader(root, &e[pr[0]]), *fs = census_shader(root, &e[pr[1]]);
        if (!vs || !fs) { shader_fail++; continue; }
        if (gfx_shader_inputs(fs, NULL) & ~gfx_shader_outputs(vs, NULL)) varying_fix++;
        VkPipeState s;
        selftest_state(&s, vs, fs);
        GfxShaderAnswers fa = {0};
        vk_pipeline_answers(&s, NULL, &fa);
        const VkPipeInfo *info;
        VkPipeline p = vk_pipeline_graphics(&s, &info);
        if (!p) continue;
        built++;
        const VkPipeInfo *again;
        if (vk_pipeline_graphics(&s, &again) != p || again != info) cached = 0;
        if (!recorded) recorded = selftest_record(&s, p);
    }
    for (int t = 0; t < WANT_CS && t < nc; t++) {
        const CensusEntry *c = &e[cs[(long long)t * nc / (nc < WANT_CS ? nc : WANT_CS)]];
        ctried++;
        const GfxShader *s = census_shader(root, c);
        if (!s) { shader_fail++; continue; }
        const VkPipeInfo *info;
        VkPipeline p = vk_pipeline_compute(s, &info);
        if (!p) continue;
        cbuilt++;
        const VkPipeInfo *again;
        if (vk_pipeline_compute(s, &again) != p || again != info) cached = 0;
    }
    vk_pipeline_save();
    VkPipeStats st;
    vk_pipeline_stats(&st);
    long errors = vk_validation_errors_total() - err0;
    printf("vk_pipeline selftest: graphics %d/%d census pairs (%d needed vertex outputs added), compute %d/%d, "
           "%d shaders failed to translate; layouts %llu (%llu push); cache hits %llu; dynamic state recorded %d; "
           "validation errors %ld\n",
           built, tried, varying_fix, cbuilt, ctried, shader_fail, (unsigned long long)(st.layouts - st0.layouts),
           (unsigned long long)(st.push_layouts - st0.push_layouts), (unsigned long long)(st.hits - st0.hits), recorded,
           errors);
    free(pairs), free(cs), free(e), free(jobs);
    int ok = built > 0 && cbuilt > 0 && cached && recorded && errors == 0;
    printf("vk_pipeline selftest: %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}

void vk_pipeline_sizes(PerfSizes *s) {
    uint64_t ent = 0, built = 0, fast = 0, libn = 0, libb = 0, lq = 0;
    plat_mutex_lock_shared(&P.lock);
    for (int i = 0; i < PIPE_BUCKETS; i++)
        for (const Pipe *p = pipes[i]; p; p = p->next) ent++, built += p->info.pipeline != VK_NULL_HANDLE, fast += p->fast != VK_NULL_HANDLE;
    for (int i = 0; i < LIB_BUCKETS; i++)
        for (const Lib *b = libs[i]; b; b = b->next) libn++, libb += b->pipe != VK_NULL_HANDLE;
    for (const LibJob *j = U.head; j; j = j->next) lq++;
    uint64_t lay = P.stats.layouts, fails = P.stats.failures, jobs = A.inflight, seen = K.seen_n, seen_b = (uint64_t)K.seen_cap * 8;
    plat_mutex_unlock_shared(&P.lock);
    perf_size(s, "pipeline entries", ent, 0, PERF_SIZE_NONE);
    perf_size(s, "with a pipeline", built, 0, PERF_SIZE_NONE);
    perf_size(s, "fast links", fast, 0, PERF_SIZE_NONE);
    perf_size(s, "pipeline failures", fails, 0, PERF_SIZE_NONE);
    perf_size(s, "pipeline layouts", lay, 0, PERF_SIZE_NONE);
    perf_size(s, "stage library entries", libn, 0, PERF_SIZE_NONE);
    perf_size(s, "built stage libraries", libb, 0, PERF_SIZE_NONE);
    perf_size(s, "library jobs queued", lq, 0, PERF_SIZE_NONE);
    perf_size(s, "pipeline jobs queued or running", jobs, 0, PERF_SIZE_NONE);
    perf_size(s, "pipeline keys seen", seen, 0, seen_b);
}

void vk_pipeline_mem(VkPipeMem *m, uint32_t idle_min) {
    memset(m, 0, sizeof *m);
    m->idle_min = idle_min;
    m->cache[0] = __atomic_load_n(&P.held[0], __ATOMIC_RELAXED);
    m->cache[1] = __atomic_load_n(&P.held[1], __ATOMIC_RELAXED);
    m->cache_asked = m->cache[0] || m->cache[1];
    if (!m->cache[0]) m->cache[0] = P.cache ? P.cache_loaded : 0;
    if (!m->cache[1]) m->cache[1] = P.play ? P.play_loaded : 0;
    if (!P.ready) return;
    uint32_t now = __atomic_load_n(&pipe_minute, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&P.lock);
    for (int i = 0; i < PIPE_BUCKETS; i++)
        for (const Pipe *p = pipes[i]; p; p = p->next) {
            m->ram_pipes += sizeof *p + p->key_size + (p->bindings ? p->info.n_bindings * sizeof *p->bindings : 0);
            int comp = p->info.bind_point == VK_PIPELINE_BIND_POINT_COMPUTE;
            VkPipeline pipe = p->info.pipeline;
            if (!pipe) {
                if (comp) m->comp_none++;
                else m->gfx_none++;
                continue;
            }
            int never = !p->used_min, idle = !never && now - p->used_min >= idle_min;
            if (comp) {
                m->comp++, m->comp_never += never, m->comp_idle += idle;
                continue;
            }
            m->gfx++, m->gfx_never += never, m->gfx_idle += idle;
            if (p->fast && pipe == p->fast) m->gfx_fast++;
            else if (p->optimised) m->gfx_opt++;
            else m->gfx_whole++;
        }
    for (int i = 0; i < LIB_BUCKETS; i++)
        for (const Lib *b = libs[i]; b; b = b->next) {
            m->ram_pipes += sizeof *b + b->size;
            uint32_t kind;
            memcpy(&kind, b->key, sizeof kind);
            if (!b->pipe) m->lib_none++;
            else if (kind < 4) m->lib[kind]++;
        }
    for (int i = 0; i < LAYOUT_BUCKETS; i++)
        for (const Layout *l = layouts[i]; l; l = l->next) m->layouts++, m->ram_pipes += sizeof *l + l->n * sizeof *l->b;
    m->ram_keys = (uint64_t)K.seen_cap * sizeof *K.seen + (uint64_t)K.pend_cap * sizeof(KeyRec) +
                  (K.file ? K.own_n * sizeof(KeyRec) : 0);
    m->ram_seed = K.file && K.file_n > K.own_n ? (K.file_n - K.own_n) * sizeof(KeyRec) : 0;
    plat_mutex_unlock_shared(&P.lock);
    plat_mutex_lock(&PB.lock);
    m->ram_binaries = (uint64_t)PB.cap * sizeof *PB.slot + PB.pend_cap;
    plat_mutex_unlock(&PB.lock);
}
