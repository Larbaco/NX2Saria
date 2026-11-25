#ifdef _WIN32
#include <windows.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "ngx_lock.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_dlssg.h"
#include "vk_exec.h"
#include "vk_fg.h"
#include "vk_fsr3fg.h"
#include "vk_ffx.h"
#include "vk_rec.h"
#include "vk_up_defer.h"
#include "dlaa_mv_spv.h"
#include "dlaa_sharpen_spv.h"

typedef int NgxResult;
#define NGX_OK(r) (((unsigned)(r) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x15
#define NGX_FEATURE_SUPERSAMPLING 1
#define NGX_PERFQ_DLAA 5
typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
typedef struct { void *slot[17]; } NgxParamVtbl;
struct NgxParam { const NgxParamVtbl *vt; };
typedef struct {
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned width, height;
} NgxImageView;
typedef struct {
    union { NgxImageView img; struct { VkBuffer buffer; unsigned bytes; } buf; } r;
    int type;
    _Bool rw;
} NgxResourceVK;

typedef NgxResult (*PFN_Init)(unsigned long long app, const wchar_t *path, VkInstance, VkPhysicalDevice, VkDevice,
                              PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const void *info);
typedef NgxResult (*PFN_ReqExt)(unsigned *ni, const char ***ie, unsigned *nd, const char ***de);
typedef NgxResult (*PFN_Params)(NgxParam **);
typedef NgxResult (*PFN_DestroyParams)(NgxParam *);
typedef NgxResult (*PFN_Create)(VkCommandBuffer, int feature, NgxParam *, NgxHandle **);
typedef NgxResult (*PFN_Evaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *callback);
typedef NgxResult (*PFN_Release)(NgxHandle *);
#ifndef _WIN32
typedef NgxResult (*PFN_Shutdown1)(VkDevice, unsigned *remaining);
#endif

static struct {
    void *core;
    PFN_Init init;
    PFN_ReqExt req_ext;
    PFN_Params caps, alloc_params;
    PFN_DestroyParams destroy_params;
    PFN_Create create;
    PFN_Evaluate evaluate;
    PFN_Release release;
    void *feat_inst_req;
    int tried, loaded, available;
    int ngx_up;
    NgxParam *capsp;
    int wanted;
#ifndef _WIN32
    PFN_Shutdown1 shutdown1;
    VkDevice device;
    NgxParam *caps_map;
#endif
} N = {.wanted = -1};

static struct {
    int max;
    NgxHandle *feat;
    NgxParam *p;
    uint32_t w, h;
    VkFormat fmt;
    int failed, reset, evals;
} FG;

static NgxLock NL;

static void ngx_lock_log(void) {
    if (!N.ngx_up) return;
    char line[512];
    ngx_lock_line(&NL, line, sizeof line);
    rt_log("dlaa: %s", line);
}

int vk_ngxfg_wanted(void) {
#ifdef _WIN32
    return 0;
#else
    static int w = -1;
    if (w < 0) w = cfg_bool("NX2SARIA_DLAA") && strcmp(cfg_value("NX2SARIA_TAA"), "fsr3");
    return w;
#endif
}

typedef void (*SetI)(NgxParam *, const char *, int);
typedef void (*SetU)(NgxParam *, const char *, unsigned);
typedef void (*SetF)(NgxParam *, const char *, float);
typedef void (*SetP)(NgxParam *, const char *, void *);
typedef NgxResult (*GetI)(const NgxParam *, const char *, int *);
typedef NgxResult (*GetU)(const NgxParam *, const char *, unsigned *);
typedef NgxResult (*GetF)(const NgxParam *, const char *, float *);
typedef NgxResult (*GetP)(const NgxParam *, const char *, void **);
typedef NgxResult (*GetU64)(const NgxParam *, const char *, unsigned long long *);
#ifdef _WIN32
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64 };
#else
enum { S_U64 = 0, S_F, S_D, S_U32, S_I, S_D11, S_D12, S_P, G_U64, G_F, G_D, G_U32, G_I, G_D11, G_D12, G_P };
#endif
static void p_seti(NgxParam *p, const char *n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
static void p_setu(NgxParam *p, const char *n, unsigned v) { ((SetU)p->vt->slot[S_U32])(p, n, v); }
static void p_setf(NgxParam *p, const char *n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
static void p_setp(NgxParam *p, const char *n, void *v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
static int p_geti(const NgxParam *p, const char *n, int *v) { return NGX_OK(((GetI)p->vt->slot[G_I])(p, n, v)); }
static int p_getu(const NgxParam *p, const char *n, unsigned *v) { return NGX_OK(((GetU)p->vt->slot[G_U32])(p, n, v)); }
static int p_getf(const NgxParam *p, const char *n, float *v) { return NGX_OK(((GetF)p->vt->slot[G_F])(p, n, v)); }
static int p_getp(const NgxParam *p, const char *n, void **v) { return NGX_OK(((GetP)p->vt->slot[G_P])(p, n, v)); }
static int p_getull(const NgxParam *p, const char *n, unsigned long long *v) {
    return NGX_OK(((GetU64)p->vt->slot[G_U64])(p, n, v));
}

#ifdef _WIN32
#define DLSS_MODEL "nvngx_dlss.dll"
#else
#define DLSS_MODEL "libnvidia-ngx-dlss.so.<version>"
#endif

int vk_dlaa_wanted(void) {
    static int at_start = -1;
    if (at_start < 0) at_start = cfg_bool("NX2SARIA_DLAA");
    return at_start || (!N.ngx_up && cfg_bool("NX2SARIA_DLAA") && !strcmp(cfg_value("NX2SARIA_TAA"), "fsr3"));
}

static int fsr3(void) { return !N.ngx_up && !strcmp(cfg_value("NX2SARIA_TAA"), "fsr3"); }
static int fsr_frame;
int vk_dlaa_fsr3(void) { return vk_dlaa_wanted() && fsr3() && vk_ffx_available(); }

int vk_dlaa_needs_restart(void) {
    if (!cfg_bool("NX2SARIA_DLAA")) return 0;
    return strcmp(cfg_value("NX2SARIA_TAA"), "fsr3") ? !N.ngx_up && !N.tried : N.ngx_up;
}

#ifdef _WIN32
static int ngx_dir_from_adapter(char *dir, DWORD size) {
    HKEY cls;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                      0, KEY_READ, &cls) != ERROR_SUCCESS)
        return 0;
    int found = 0;
    for (DWORD i = 0; !found; i++) {
        char sub[64], val[MAX_PATH + 1] = {0}, probe[MAX_PATH + 32];
        DWORD sn = sizeof sub, vn = MAX_PATH;
        if (RegEnumKeyExA(cls, i, sub, &sn, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegGetValueA(cls, sub, "VulkanDriverName", RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ, NULL, val, &vn) != ERROR_SUCCESS)
            continue;
        char *slash = strrchr(val, '\\');
        if (!slash) continue;
        *slash = 0;
        snprintf(probe, sizeof probe, "%s\\_nvngx.dll", val);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) continue;
        snprintf(dir, size, "%s", val);
        found = 1;
    }
    RegCloseKey(cls);
    return found;
}
#endif

static int core_load(void) {
    if (N.tried) return N.loaded;
    N.tried = 1;
#ifdef _WIN32
    char dir[MAX_PATH] = {0}, path[MAX_PATH + 32];
    DWORD n = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath", RRF_RT_REG_SZ,
                     NULL, dir, &n) != ERROR_SUCCESS && !ngx_dir_from_adapter(dir, sizeof dir)) {
        rt_log("dlaa: the driver has no NGX core (registry NGXCore missing, none beside its Vulkan driver): DLAA off");
        return 0;
    }
    snprintf(path, sizeof path, "%s\\_nvngx.dll", dir);
    N.core = LoadLibraryA(path);
    if (!N.core) {
        rt_log("dlaa: cannot load %s (error %lu): DLAA off", path, GetLastError());
        return 0;
    }
#define FN(f, name) N.f = (void *)GetProcAddress((HMODULE)N.core, name)
#else
    const char *path = "libnvidia-ngx.so.1";
    N.core = plat_lib_open(path);
    if (!N.core) {
        rt_log("dlaa: cannot load %s (not an NVIDIA driver, or one without NGX): DLAA off", path);
        return 0;
    }
#define FN(f, name) N.f = plat_lib_sym(N.core, name)
#endif
    FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
#ifndef _WIN32
    FN(shutdown1, "NVSDK_NGX_VULKAN_Shutdown1");
#endif
    FN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
    FN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    FN(alloc_params, "NVSDK_NGX_VULKAN_AllocateParameters");
    FN(destroy_params, "NVSDK_NGX_VULKAN_DestroyParameters");
    FN(create, "NVSDK_NGX_VULKAN_CreateFeature");
    FN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
    FN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
    FN(feat_inst_req, "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements");
#undef FN
    if (!N.init || !N.req_ext || !N.caps || !N.alloc_params || !N.create || !N.evaluate || !N.release) {
        rt_log("dlaa: the NGX core %s lacks Vulkan entry points: DLAA off", path);
        return 0;
    }
#ifndef _WIN32
    if (!N.shutdown1 || !N.destroy_params) {
        rt_log("dlaa: the NGX core %s lacks NVSDK_NGX_VULKAN_Shutdown1 or DestroyParameters, which its teardown needs: "
               "DLAA off", path);
        return 0;
    }
#endif
    N.loaded = 1;
    rt_log("dlaa: NGX core %s", path);
    return 1;
}

static int listed(const char *const *list, uint32_t n, const char *s) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}

typedef struct {
    int sdk, feature;
    struct { int type; union { struct { const char *id; int engine; const char *ver; } project; unsigned long long app; } v; } ident;
    const wchar_t *path;
    const void *info;
} NgxDiscovery;
typedef NgxResult (*PFN_FeatInstReq)(const NgxDiscovery *, uint32_t *, VkExtensionProperties **);
#define NGX_FEATURE_FRAME_GENERATION 11
static char fg_names[16][VK_MAX_EXTENSION_NAME_SIZE];
static int fg_nnames;
static const char *fg_keep(const char *name) {
    for (int i = 0; i < fg_nnames; i++)
        if (!strcmp(fg_names[i], name)) return fg_names[i];
    if (fg_nnames >= 16) return NULL;
    snprintf(fg_names[fg_nnames], sizeof fg_names[0], "%s", name);
    return fg_names[fg_nnames++];
}

void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || fsr3() || !core_load() ||
        !NGX_OK(NGX_CALL(&NL, NGX_AT_SETUP, N.req_ext(&ni, &ie, &nd, &de))))
        return;
    for (unsigned i = 0; i < ni; i++)
        if (!listed(ext, *n, ie[i]) && *n < max) ext[(*n)++] = ie[i];
    if (vk_ngxfg_wanted() && N.feat_inst_req) {
        static const wchar_t here[] = L".";
        NgxDiscovery d;
        memset(&d, 0, sizeof d);
        d.sdk = NGX_VERSION_API, d.feature = NGX_FEATURE_FRAME_GENERATION;
        d.ident.type = 0, d.ident.v.app = 231313132ull;
        d.path = here;
        uint32_t k = 0;
        VkExtensionProperties *pr = NULL;
        if (NGX_OK(NGX_CALL(&NL, NGX_AT_SETUP, ((PFN_FeatInstReq)N.feat_inst_req)(&d, &k, &pr))) && pr)
            for (uint32_t i = 0; i < k; i++) {
                const char *nm = fg_keep(pr[i].extensionName);
                if (nm && !listed(ext, *n, nm) && *n < max) ext[(*n)++] = nm, rt_log("ngxfg: instance extension %s", nm);
            }
    }
}

void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n,
                         uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || fsr3() || !core_load() ||
        !NGX_OK(NGX_CALL(&NL, NGX_AT_SETUP, N.req_ext(&ni, &ie, &nd, &de))))
        return;
    for (unsigned i = 0; i < nd; i++) {
        int ok = 0;
        for (uint32_t k = 0; k < nhave && !ok; k++) ok = !strcmp(have[k].extensionName, de[i]);
        if (!ok) rt_log("dlaa: the device lacks %s, which NGX asks for", de[i]);
        else if (!listed(ext, *n, de[i]) && *n < max) ext[(*n)++] = de[i];
    }
}

static int vtable_ok(void) {
    NgxParam *p = NULL;
    if (!NGX_OK(NGX_CALL(&NL, NGX_AT_SETUP, N.alloc_params(&p))) || !p) return 0;
    int i = 0;
    unsigned u = 0;
    float f = 0;
    void *v = NULL;
    p_seti(p, "NX2Saria.Test.I", -12345);
    p_setu(p, "NX2Saria.Test.U", 4000000000u);
    p_setf(p, "NX2Saria.Test.F", 1.25f);
    p_setp(p, "NX2Saria.Test.P", (void *)p);
    int ok = p_geti(p, "NX2Saria.Test.I", &i) && i == -12345 && p_getu(p, "NX2Saria.Test.U", &u) && u == 4000000000u &&
             p_getf(p, "NX2Saria.Test.F", &f) && f == 1.25f && p_getp(p, "NX2Saria.Test.P", &v) && v == (void *)p;
    if (N.destroy_params) NGX_CALL(&NL, NGX_AT_SETUP, N.destroy_params(p));
    if (!ok) rt_log("dlaa: NGX parameter layout check failed (i=%d u=%u f=%g p=%p): DLAA off", i, u, f, v);
    return ok;
}

#ifndef _WIN32
static int utf8_to_wide(const char *in, wchar_t *out, size_t cap) {
    const unsigned char *u = (const unsigned char *)in;
    size_t n = 0;
    while (*u) {
        unsigned c = *u++, more = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 4;
        if (more == 4) return 0;
        if (more) c &= 0x3F >> more;
        for (unsigned k = 0; k < more; k++, u++) {
            if ((*u & 0xC0) != 0x80) return 0;
            c = c << 6 | (*u & 0x3F);
        }
        if (n + 1 >= cap) return 0;
        out[n++] = (wchar_t)c;
    }
    out[n] = 0;
    return 1;
}
#endif

void vk_dlaa_init(void) {
    if (vk_dlaa_wanted() && fsr3()) {
        fsr_frame = 1;
        N.available = vk_ffx_load();
        rt_log("dlaa: the temporal upscaler is AMD FSR 3.1%s", N.available ? "" : ", which is not available: the game's own scaling");
        return;
    }
    if (!vk_dlaa_wanted() || !core_load()) return;
#ifdef _WIN32
    wchar_t wdir[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wdir, MAX_PATH);
    while (len && wdir[len - 1] != L'\\') len--;
    wdir[len] = 0;
#else
    char dir[PLAT_PATH_MAX];
    wchar_t wdir[PLAT_PATH_MAX];
    char *cut = plat_exe_path(dir, sizeof dir) ? plat_last_sep(dir) : NULL;
    if (!cut || !utf8_to_wide((cut[1] = 0, dir), wdir, PLAT_PATH_MAX)) {
        rt_log("dlaa: cannot name the executable's folder for NGX: DLAA off");
        return;
    }
#endif
    const VkCtx *c = vk_ctx();
    NgxResult r = NGX_CALL(&NL, NGX_AT_SETUP, N.init(231313132ull, wdir, c->instance, c->phys, c->device,
                                                      vkGetInstanceProcAddr, vkGetDeviceProcAddr, NGX_VERSION_API, NULL));
    if (!NGX_OK(r)) {
        rt_log("dlaa: NGX init failed (%08x): DLAA off", (unsigned)r);
        return;
    }
#ifndef _WIN32
    N.device = c->device;
#endif
    if (!vtable_ok()) return;
    NgxParam *caps = NULL;
    int avail = 0, driver = 0;
    if (NGX_OK(NGX_CALL(&NL, NGX_AT_SETUP, N.caps(&caps))) && caps) {
#ifndef _WIN32
        N.caps_map = caps;
#endif
        p_geti(caps, "SuperSampling.Available", &avail);
        p_geti(caps, "SuperSampling.NeedsUpdatedDriver", &driver);
    }
    N.available = avail != 0;
    N.capsp = caps;
    N.ngx_up = 1;
    rt_log("dlaa: NGX up; DLSS %s%s", avail ? "available" : "NOT available (is " DLSS_MODEL " next to the executable?)",
           driver ? "; the driver needs an update" : "");
    if (vk_ngxfg_wanted() && caps) {
        int fga = 0;
        unsigned mx = 0;
        p_geti(caps, "FrameGeneration.Available", &fga);
        if (!p_getu(caps, "DLSSG.MultiFrameCountMax", &mx)) mx = 1;
        FG.max = fga ? (mx >= 3 ? 5 : mx < 1 ? 1 : (int)mx) : 0;
        rt_log("ngxfg: DLSS Frame Generation %s (up to %dx)",
               fga ? "available" : "NOT available (is libnvidia-ngx-dlssg.so.<version> next to the executable?)", FG.max + 1);
    }
#ifdef _WIN32
    const char *fg_why = "it is Linux only";
#else
    const char *fg_why = !vk_ngxfg_wanted() ? "not wanted in this run" : "not available";
#endif
    if (FG.max)
        rt_log("dlaa: NGX calls go through one lock (NGX is not thread safe): DLSS's from the replay thread, its idle "
               "releases from the frame's bookkeeping, DLSS Frame Generation's from the present thread, which can wait "
               "there for a DLSS call (the NGX lock line says how long)");
    else
        rt_log("dlaa: NGX calls go through one lock (NGX is not thread safe): DLSS's from the replay thread and its "
               "idle releases from the frame's bookkeeping; no DLSS Frame Generation through NGX (%s), so the lock is "
               "almost never waited for",
               fg_why);
}

int vk_dlaa_available(void) { return vk_dlaa_wanted() && N.available; }

#define OCAM_BLOCK 0x3500u
#define OCAM_BYTES 0xb0u
#define DLSS_SIZES 5
#define SIZE_STALE 3600
static struct {
    VkExecTarget color, depth;
    int armed, done, gbuf;
    int ended, up;
    struct Size {
        uint32_t w, h, ow, oh;
        VkImage out, mv;
        VmaAllocation out_mem, mv_mem;
        VkImageView out_view, mv_view;
        VkImage sharp;
        VmaAllocation sharp_mem;
        VkImageView sharp_view;
        VkImage mask, chars[2];
        VmaAllocation mask_mem, chars_mem[2];
        VkImageView mask_view, chars_view[2];
        int chars_cur, chars_ok;
        VkImage dist[2];
        VmaAllocation dist_mem[2];
        VkImageView dist_view[2];
        uint64_t pose_distance_frame;
        float pose_history[4];
        int pose_distance_valid;
        int dist_cur, dist_ok;
        NgxHandle *feat;
        int q;
        int ranged;
        uint32_t min_w, min_h;
        uint64_t used;
        uint64_t feat_used;
        uint64_t feat_fence;
    } sizes[DLSS_SIZES], *last;
    struct Size *host;
    int stepped;
    uint32_t ev_w, ev_h, ev_ow, ev_oh;
    uint64_t steps_kept, steps_made;
    VkImage last_color;
    NgxParam *params;
    int failed, reset;
    uint64_t evals, frames_missed;
    int share_off;
    int shape_logged;
    VkImage jdepth;
    uint32_t jw, jh;
    float jx, jy;
    float ux, uy;
    uint32_t jframe;
    int jit_ok, jit_used, hold;
    uint64_t holds, jittered;
    uint64_t jdraws, jfixed;
    uint64_t cam_addr;
    uint32_t cam_size, cam_vp;
    struct {
        uint64_t addr, seq;
        uint32_t n;
        int got;
        float cam[OCAM_BYTES / 4];
    } views[8];
    int nviews;
    VkBuffer camb;
    VmaAllocation camb_mem;
    VkBuffer mvm;
    VmaAllocation mvm_mem;
    uint8_t *mvm_map;
    uint32_t mvm_slot;
    double cam_prev[16];
    int cam_slot, cam_prev_ok;
    VkDescriptorSetLayout mv_dsl;
    VkPipelineLayout mv_layout;
    VkPipeline mv_pipe;
    VkSampler mv_samp;
    int mv_ok;
    uint64_t mv_frames, mv_none, masked;
    uint64_t why[3];
    uint64_t target_kept;
    uint64_t cam_none_draw, cam_none_snap;
    uint64_t cam_at_draw;
    uint64_t cam_kept, cam_kept_max;
    float cut_pos[3], cut_fwd[3];
    uint64_t cut_ns, cuts;
    int cut_ok, cut_logged;
    int keep_run;
    uint64_t gap_logged, miss_logged, target_logged, second_logged;
    uint64_t seq_present;
    uint64_t seconds;
    int on;
    int fgo;
    int preset, preset_dirty;
    VkExecTarget ids;
    int ids_ok;
    unsigned cfg_seen;
    uint64_t last_eval_ns;
    VkFfxImage fg_depth, fg_mv;
    VkImageView fg_depth_view, fg_mv_view;
    VkDlaaCamera fg_cam;
    int fg_cam_ok;
    float fg_jx, fg_jy;
    int fg_fresh, fg_reset;
    int scene_last;
    uint64_t presents;
    uint64_t fg_at, hl_at;
    VkImage hl_srgb, hl;
    VmaAllocation hl_srgb_mem, hl_mem;
    VkImageView hl_view;
    uint32_t hl_w, hl_h;
    VkFormat hl_fmt;
    int hl_fresh;
    int pre_on;
    int pre;
    int pre_at;
    int comp;
    VkImage comp_game;
    uint32_t pic_w, pic_h;
    uint64_t pre_misses;
} D = {.mv_ok = -1, .cfg_seen = ~0u};

static struct {
    uint32_t zdraws, zother;
    uint32_t zother_w, zother_h;
    uint32_t ocam, ocam_off;
    VkFormat ocam_off_fmt;
    uint32_t c3_other, c3_size;
    uint64_t seq0;
    uint32_t one_n, one_w, one_h;
    VkFormat one_fmt;
    uint32_t second;
    VkImage second_img;
} F;

#define FG_INPUT_PRESENTS 4
static int fg_current(uint64_t made) { return D.presents - made <= FG_INPUT_PRESENTS; }

int vk_dlaa_fg_hudless(VkImage *img, VkImageView *view, uint32_t *w, uint32_t *h, VkFormat *fmt) {
    if (!D.hl_fresh) return 0;
    D.hl_fresh = 0;
    if (!fg_current(D.hl_at)) return 0;
    *img = D.hl, *view = D.hl_view, *w = D.hl_w, *h = D.hl_h, *fmt = D.hl_fmt;
    return 1;
}

int vk_dlaa_fg_views(VkImageView *depth, VkImageView *mv) {
    *depth = D.fg_depth_view, *mv = D.fg_mv_view;
    return D.fg_depth_view && D.fg_mv_view;
}

int vk_dlaa_pose_depth(uint64_t frame, uint32_t w, uint32_t h, VkDescriptorImageInfo *image, float history[4]) {
    const struct Size *last = D.last;
    if (!last || !last->pose_distance_valid || !last->dist_ok || last->w != w || last->h != h ||
        !frame || last->pose_distance_frame != frame - 1 || fabsf(last->pose_history[2]) < 1e-6f) return 0;
    *image = (VkDescriptorImageInfo){D.mv_samp, last->dist_view[last->dist_cur ^ 1], VK_IMAGE_LAYOUT_GENERAL};
    memcpy(history, last->pose_history, sizeof last->pose_history);
    return 1;
}

int vk_dlaa_fg_scene(void) { return __atomic_load_n(&D.scene_last, __ATOMIC_RELAXED); }

int vk_dlaa_fg_camera(VkDlaaCamera *c) {
    if (!D.fg_cam_ok) return 0;
    *c = D.fg_cam;
    return 1;
}

#define VIEW_METERS 1.0f

int vk_dlaa_ffx_camera(VkFfxCamera *c) {
    if (!D.fg_cam_ok) return 0;
    *c = (VkFfxCamera){D.fg_cam.fov_y, D.fg_cam.znear, D.fg_cam.zfar, VIEW_METERS};
    return 1;
}

int vk_dlaa_fg_inputs(VkFfxImage *depth, VkFfxImage *mv, float *jx, float *jy, int *reset) {
    if (!D.fg_fresh || D.failed) return 0;
    D.fg_fresh = 0;
    if (!fg_current(D.fg_at)) return 0;
    *depth = D.fg_depth, *mv = D.fg_mv, *jx = D.fg_jx, *jy = D.fg_jy, *reset = D.fg_reset;
    return 1;
}

const VkTexImage *vk_dlaa_swap_img;
VkImageView vk_dlaa_swap_view;

static int dl_active(void) { return N.available && !D.failed && D.on; }
int vk_dlaa_active(void) { return dl_active(); }
static int inputs_on(void) { return dl_active() || D.fgo; }
int vk_dlaa_fg_only(void) { return D.fgo; }
static int use_fsr(void) { return !N.ngx_up && fsr_frame; }

static void dl_fail(const char *what, NgxResult r) {
    rt_log("dlaa: %s failed (%08x): DLAA off for this run", what, (unsigned)r);
    D.failed = 1;
}

static int image2d(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkImage *img, VmaAllocation *mem,
                   VkImageView *view) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, view) == VK_SUCCESS;
}

static int feat_released;

static int idle_free(void) {
    static int m = -1;
    if (m < 0) m = cfg_int("NX2SARIA_DLSS_IDLE_FREE");
    return m;
}

void vk_upscaler_device_idle(void) {
    uint64_t t0 = perf_mark_begin();
    vk_queue_lock(), vkDeviceWaitIdle(vk_ctx()->device), vk_queue_unlock();
    perf_mark_end(PERF_M_UPWAIT, t0, 0);
}

static void feat_release(NgxHandle *f, int at) {
    uint64_t t0 = perf_mark_begin();
    NGX_CALL(&NL, at, N.release(f));
    perf_mark_end(PERF_M_UPREL, t0, 0);
}

enum { UPD_FEAT = 1, UPD_IMG, UPD_VIEW };
static UpdList dead;
#define FG_LATE_PRESENTS 8

static uint64_t presents_now(void) { return __atomic_load_n(&D.presents, __ATOMIC_RELAXED); }

int vk_upscaler_idle_wait(void) {
    static int m = -1;
    if (m < 0) m = cfg_int("NX2SARIA_UPSCALER_IDLE_WAIT") == 1;
    return m;
}

static void dead_release(const UpdEntry *e, void *teardown) {
    const VkCtx *c = vk_ctx();
    uint64_t t0 = perf_mark_begin();
    if (e->kind == UPD_FEAT) {
        if (N.release) NGX_CALL(&NL, teardown ? NGX_AT_SETUP : NGX_AT_REPLAY, N.release((NgxHandle *)e->handle));
    } else if (e->kind == UPD_VIEW) {
        if (teardown) vkDestroyImageView(c->device, (VkImageView)e->handle, NULL);
        else vk_defer_view((VkImageView)e->handle);
    } else if (e->kind == UPD_IMG) {
        if (teardown) vmaDestroyImage(c->vma, (VkImage)e->handle, (VmaAllocation)e->aux);
        else vk_defer_image((VkImage)e->handle, (VmaAllocation)e->aux);
    }
    perf_mark_end(PERF_M_UPREL, t0, 0);
}

static void upscalers_drain(void) {
    const uint64_t done = vk_sync_fence_done();
    if (dead.n) upd_take(&dead, done, presents_now(), dead_release, NULL);
    vk_ffx_dead_take(done);
}

void vk_upscaler_settle(void) {
    const uint64_t issued = vk_sync_issued();
    vk_upscaler_device_idle();
    upd_take(&dead, issued, presents_now(), dead_release, NULL);
    vk_ffx_dead_take(issued);
}

void vk_upscaler_let_go(void) {
    if (vk_upscaler_idle_wait()) vk_upscaler_settle();
    else upscalers_drain();
}

uint64_t vk_upscaler_gpu_wait(uint64_t v) {
    const uint64_t issued = vk_sync_issued();
    if (vk_upscaler_idle_wait()) {
        vk_upscaler_device_idle();
        return issued;
    }
    if (v > issued) v = issued;
    if (v <= vk_sync_fence_done()) return v;
    uint64_t t0 = perf_mark_begin();
    vk_queue_lock(), vk_queue_unlock();
    const VkCtx *c = vk_ctx();
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &c->timeline, .pValues = &v};
    VkResult r = vkWaitSemaphores(c->device, &wi, 2000000000ull);
    perf_mark_end(PERF_M_UPWAIT, t0, 0);
    if (r == VK_SUCCESS) return v;
    vk_device_lost_check(r, "the upscaler's wait for the GPU");
    rt_log("dlaa: the wait for the GPU to pass submission %llu failed (%d): waiting for the device instead",
           (unsigned long long)v, (int)r);
    vk_upscaler_device_idle();
    return issued;
}

static void dead_put(int kind, void *h, void *aux, uint64_t fence, uint64_t presents) {
    if (upd_put(&dead, kind, h, aux, fence, presents)) return;
    static int told;
    if (told++ < 4) rt_log("dlaa: %d things DLSS let go of still wait for the GPU or a present: a wait for the device",
                           UPD_SLOTS);
    vk_upscaler_settle();
    if (upd_put(&dead, kind, h, aux, fence, presents)) return;
    upd_take_all(&dead, dead_release, NULL);
    upd_put(&dead, kind, h, aux, fence, presents);
}

static void retire_size(struct Size *e) {
    vk_defer_view(e->out_view), vk_defer_image(e->out, e->out_mem);
    vk_defer_view(e->sharp_view), vk_defer_image(e->sharp, e->sharp_mem);
    vk_defer_view(e->mask_view), vk_defer_image(e->mask, e->mask_mem);
    for (int i = 0; i < 2; i++) {
        vk_defer_view(e->chars_view[i]), vk_defer_image(e->chars[i], e->chars_mem[i]);
        vk_defer_view(e->dist_view[i]), vk_defer_image(e->dist[i], e->dist_mem[i]);
    }
    const uint64_t gate = presents_now() + FG_LATE_PRESENTS;
    dead_put(UPD_VIEW, e->mv_view, NULL, 0, gate);
    dead_put(UPD_IMG, e->mv, e->mv_mem, 0, gate);
    if (e->feat) dead_put(UPD_FEAT, e->feat, NULL, e->feat_fence, 0), feat_released = 1;
    if (D.host == e) D.host = NULL;
    memset(e, 0, sizeof *e);
}

static void retire_feat(struct Size *s, int count) {
    dead_put(UPD_FEAT, s->feat, NULL, s->feat_fence, 0);
    s->feat = NULL;
    if (count) feat_released = 1;
}

static void destroy_size(struct Size *e, int at) {
    const VkCtx *c = vk_ctx();
    if (e->out_view) vkDestroyImageView(c->device, e->out_view, NULL);
    if (e->mv_view) vkDestroyImageView(c->device, e->mv_view, NULL);
    if (e->out) vmaDestroyImage(c->vma, e->out, e->out_mem);
    if (e->mv) vmaDestroyImage(c->vma, e->mv, e->mv_mem);
    if (e->sharp_view) vkDestroyImageView(c->device, e->sharp_view, NULL);
    if (e->sharp) vmaDestroyImage(c->vma, e->sharp, e->sharp_mem);
    if (e->mask_view) vkDestroyImageView(c->device, e->mask_view, NULL);
    if (e->mask) vmaDestroyImage(c->vma, e->mask, e->mask_mem);
    for (int i = 0; i < 2; i++) {
        if (e->chars_view[i]) vkDestroyImageView(c->device, e->chars_view[i], NULL);
        if (e->chars[i]) vmaDestroyImage(c->vma, e->chars[i], e->chars_mem[i]);
        if (e->dist_view[i]) vkDestroyImageView(c->device, e->dist_view[i], NULL);
        if (e->dist[i]) vmaDestroyImage(c->vma, e->dist[i], e->dist_mem[i]);
    }
    if (e->feat && N.release) feat_release(e->feat, at), feat_released = 1;
    if (D.host == e) D.host = NULL;
    memset(e, 0, sizeof *e);
}

static void free_size(struct Size *e) {
    retire_size(e);
    vk_upscaler_let_go();
}

static struct Size *size_for(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    struct Size *e = NULL, *lru = &D.sizes[0];
    for (int i = 0; i < DLSS_SIZES; i++) {
        if (D.sizes[i].w == w && D.sizes[i].h == h && D.sizes[i].ow == ow && D.sizes[i].oh == oh) return &D.sizes[i];
        if (!D.sizes[i].out && !e) e = &D.sizes[i];
        if (D.sizes[i].used < lru->used) lru = &D.sizes[i];
    }
    if (!e) {
        if (D.last == lru) D.last = NULL;
        free_size(e = lru);
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageUsageFlags u8 = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!image2d(ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, u, &e->out, &e->out_mem, &e->out_view) ||
        !image2d(w, h, VK_FORMAT_R16G16_SFLOAT, u | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &e->mv, &e->mv_mem, &e->mv_view) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u8, &e->mask, &e->mask_mem, &e->mask_view) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u8, &e->chars[0], &e->chars_mem[0], &e->chars_view[0]) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u8, &e->chars[1], &e->chars_mem[1], &e->chars_view[1]) ||
        !image2d(w, h, VK_FORMAT_R32_SFLOAT, u8, &e->dist[0], &e->dist_mem[0], &e->dist_view[0]) ||
        !image2d(w, h, VK_FORMAT_R32_SFLOAT, u8, &e->dist[1], &e->dist_mem[1], &e->dist_view[1]))
        return NULL;
    VkImageMemoryBarrier2 ib[7];
    VkImage im[7] = {e->out, e->mv, e->mask, e->chars[0], e->chars[1], e->dist[0], e->dist[1]};
    for (int i = 0; i < 7; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 7,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    e->w = w, e->h = h, e->ow = ow, e->oh = oh;
    return e;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static NgxResourceVK res_image(VkImageView v, VkImage img, VkFormat f, uint32_t w, uint32_t h, VkImageAspectFlags a,
                               int rw) {
    NgxResourceVK r;
    memset(&r, 0, sizeof r);
    r.r.img = (NgxImageView){v, img, {a, 0, 1, 0, 1}, f, w, h};
    r.type = 0;
    r.rw = rw;
    return r;
}

enum { T_MV0, T_MV1, T_OBJ, T_DL0, T_DL1, T_N };
#define TIMES_SLOTS 8u
#define TIMES_SUMMARY 600
static struct {
    int on;
    VkQueryPool pool;
    uint32_t slot;
    int used[TIMES_SLOTS];
    double gpu[3];
    uint64_t frames, cpu_drain, cpu_ngx, cpu_mv;
} T = {.on = -1};

static int times_on(void) {
    if (T.on < 0) {
        T.on = rt_knob("DLAA_TIMES", "0");
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = TIMES_SLOTS * T_N};
        if (T.on && vkCreateQueryPool(vk_ctx()->device, &qi, NULL, &T.pool) != VK_SUCCESS) T.on = 0;
    }
    return T.on;
}

static void times_mark(VkCommandBuffer cb, int which) {
    if (times_on()) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, T.pool, T.slot * T_N + (uint32_t)which);
}

static void times_begin(VkCommandBuffer cb) {
    if (!times_on()) return;
    T.slot = (T.slot + 1) % TIMES_SLOTS;
    if (T.used[T.slot]) {
        uint64_t q[T_N];
        if (vkGetQueryPoolResults(vk_ctx()->device, T.pool, T.slot * T_N, T_N, sizeof q, q, sizeof q[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double ns = vk_ctx()->props.limits.timestampPeriod / 1e6;
            T.gpu[0] += (double)(q[T_MV1] - q[T_MV0]) * ns, T.gpu[1] += (double)(q[T_OBJ] - q[T_MV1]) * ns;
            T.gpu[2] += (double)(q[T_DL1] - q[T_DL0]) * ns;
            if (++T.frames % TIMES_SUMMARY == 0) {
                rt_log("dlaa times: gpu camera motion %.3f ms, object motion %.3f ms, DLSS %.3f ms (%llu frames timed); "
                       "cpu: recorder drain %.3f ms, NGX evaluate %.3f ms, motion pass recording %.3f ms",
                       T.gpu[0] / TIMES_SUMMARY, T.gpu[1] / TIMES_SUMMARY, T.gpu[2] / TIMES_SUMMARY,
                       (unsigned long long)T.frames, T.cpu_drain / 1e6 / TIMES_SUMMARY, T.cpu_ngx / 1e6 / TIMES_SUMMARY,
                       T.cpu_mv / 1e6 / TIMES_SUMMARY);
                T.gpu[0] = T.gpu[1] = T.gpu[2] = 0, T.cpu_drain = T.cpu_ngx = T.cpu_mv = 0;
            }
        }
    }
    T.used[T.slot] = 0;
    vkCmdResetQueryPool(cb, T.pool, T.slot * T_N, T_N);
}

#define CAM_BYTES 256u
#define CAM_BLOCK 0x940u
#define CAM_SLOT 0xa00u
#define MVM_SLOTS 16u

typedef struct {
    float size[2], jitter[2], bias, prev_ok, zfit, zprev_ok;
    float dz[4];
    float zmask;
} MvPush;

static int mv_init(void) {
    if (D.mv_ok >= 0) return D.mv_ok;
    D.mv_ok = 0;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[10] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 10, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MvPush)};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_mv_spv,
                                   .pCode = dlaa_mv_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2 * CAM_SLOT,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &D.mv_dsl) != VK_SUCCESS) goto fail;
    VkBufferCreateInfo mbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MVM_SLOTS * 2 * CAM_BYTES,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VmaAllocationCreateInfo mai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo minfo;
    if (vmaCreateBuffer(c->vma, &mbi, &mai, &D.mvm, &D.mvm_mem, &minfo) != VK_SUCCESS) goto fail;
    D.mvm_map = minfo.pMappedData;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &D.mv_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &D.mv_layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &D.mv_samp) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS ||
        vmaCreateBuffer(c->vma, &bi, &ai, &D.camb, &D.camb_mem, NULL) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = D.mv_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &D.mv_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    m = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) goto fail;
    return D.mv_ok = 1;
fail:
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    rt_log("dlaa: the motion-vector pass could not be set up: DLAA runs with zero motion");
    return 0;
}

static int inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r);
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-300) return 0;
        for (int k = 0; k < 8; k++) { double t = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) {
                double f = a[r][col];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r][k + 4];
    return 1;
}

static void ocam_vp(const float *v, double out[16]) {
    const float *P = v + 7 * 4;
    double V[16] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], 0, 0, 0, 1};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            double acc = 0;
            for (int j = 0; j < 4; j++) acc += (double)P[r * 4 + j] * V[j * 4 + k];
            out[r * 4 + k] = acc;
        }
}

static void fg_store(float out[16], const double m[16]) {
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) out[r * 4 + k] = (float)m[k * 4 + r];
}
static double plane_dist(const double P[16], double k) {
    double den = P[10] - k * P[14];
    if (fabs(den) <= 1e-9 * (fabs(P[10]) + fabs(P[14]))) return INFINITY;
    return -(k * P[15] - P[11]) / den;
}

static void proj_planes(const double P[16], VkDlaaCamera *c) {
    int minus_w = nvn_depth_minus_w();
    double at0 = plane_dist(P, minus_w ? -1.0 : 0.0), at1 = plane_dist(P, 1.0);
    int inverted = !(at0 < at1);
    double n = inverted ? at1 : at0, f = inverted ? at0 : at1;
    if (!(n > 0) || !isfinite(n)) return;
    c->inverted = inverted;
    c->znear = (float)n;
    c->infinite = !(f < VK_DLAA_FAR_INFINITE);
    c->zfar = c->infinite ? VK_DLAA_FAR_INFINITE : (float)f;
    static float logged_n, logged_f;
    static int logged_inv = -1, lines;
    if (lines < 20 && (inverted != logged_inv || fabsf(c->znear - logged_n) > 1e-4f * logged_n ||
                       fabsf(c->zfar - logged_f) > 1e-4f * logged_f)) {
        lines++, logged_inv = inverted, logged_n = c->znear, logged_f = c->zfar;
        char fz[32];
        if (c->infinite) snprintf(fz, sizeof fz, "infinite");
        else snprintf(fz, sizeof fz, "%.6g", c->zfar);
        rt_log("dlaa: the scene's projection: near %.6g, far %s, depth %s, clip depth %s (z row %.6g %.6g %.6g %.6g, "
               "w row %.6g %.6g %.6g %.6g)", c->znear, fz, inverted ? "inverted (1 near)" : "0 near, 1 far",
               minus_w ? "-w..w" : "0..w", P[8], P[9], P[10], P[11], P[12], P[13], P[14], P[15]);
    }
}

static void fg_camera(const float *v, const double m[16]) {
    double P[16], Pi[16], mi[16];
    for (int i = 0; i < 16; i++) P[i] = v[28 + i];
    if (!inv4(P, Pi) || !inv4(m, mi)) return;
    VkDlaaCamera *c = &D.fg_cam;
    fg_store(c->view_to_clip, P), fg_store(c->clip_to_view, Pi);
    fg_store(c->clip_to_prev_clip, m), fg_store(c->prev_clip_to_clip, mi);
    for (int i = 0; i < 3; i++) {
        c->right[i] = v[i], c->up[i] = v[4 + i], c->fwd[i] = -v[8 + i];
        c->pos[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    }
    c->fov_y = P[5] != 0 ? 2.0f * (float)atan(1.0 / fabs(P[5])) : 1.0f;
    c->aspect = P[0] != 0 ? (float)fabs(P[5] / P[0]) : 1.777f;
    if (!(c->znear > 0)) c->znear = 0.1f, c->zfar = VK_DLAA_FAR_INFINITE, c->infinite = 1;
    proj_planes(P, c);
    D.fg_cam_ok = 1;
    static int trace = -1;
    if (trace < 0) {
        const char *e = cfg_get("NX2SARIA_MOUSE_TRACE");
        trace = e && *e && strcmp(e, "0");
    }
    if (trace)
        rt_log("mousetrace: cam t=%.3f fwd=%.6f,%.6f,%.6f pos=%.3f,%.3f,%.3f", plat_now_ns() / 1e6, c->fwd[0], c->fwd[1],
               c->fwd[2], c->pos[0], c->pos[1], c->pos[2]);
}

#define KEEP_MAX 6

static const char *const usnap_why[] = {"found", "no copy in this submission", "dropped by a GPU write in this submission",
                                        "left out: a GPU write in flight", "not memory the CPU writes",
                                        "not bound in this submission"};

static void cam_note(int view, int kept) {
    uint64_t seq = vk_exec_submit_seq;
    char snap[200] = "";
    if (D.cam_addr)
        snprintf(snap, sizeof snap, "; scene context %#llx last drawn %llu submission(s) before DLSS's, there: %s",
                 (unsigned long long)D.cam_addr, (unsigned long long)(view >= 0 ? seq - D.views[view].seq : 0),
                 usnap_why[vk_usnap_why(D.cam_addr, D.cam_size == OCAM_BLOCK ? OCAM_BYTES : D.cam_size)]);
    rt_log("dlaa: frame %llu without a camera: %llu scene-depth draws over %llu submission(s) (%u into another D32 "
           "depth %ux%u), %u with the scene context (%u not into a scene colour target, format %d), %u with another c3 "
           "block (last size %#x)%s; %s",
           (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, (unsigned long long)F.zdraws,
           (unsigned long long)(F.zdraws + F.zother ? seq - F.seq0 + 1 : 0), F.zother, F.zother_w, F.zother_h, F.ocam,
           F.ocam_off, (int)F.ocam_off_fmt, F.c3_other, F.c3_size, snap,
           kept == 1 ? "history kept, zero motion" : kept == 2 ? "new history (more than KEEP_MAX in a row)"
                                                 : "new history");
}

#define CUT_TURN 30.0
static void cam_cut(const float *v) {
    static int on = -1;
    if (on < 0) on = rt_knob("DLAA_CUT", "1");
    float pos[3], fwd[3];
    for (int i = 0; i < 3; i++) {
        fwd[i] = -v[8 + i];
        pos[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    }
    uint64_t now = plat_now_ns();
    if (on && D.cut_ok) {
        double dot = fwd[0] * D.cut_fwd[0] + fwd[1] * D.cut_fwd[1] + fwd[2] * D.cut_fwd[2];
        double turn = acos(dot > 1 ? 1 : dot < -1 ? -1 : dot) * (180.0 / 3.14159265358979);
        double dx = pos[0] - D.cut_pos[0], dy = pos[1] - D.cut_pos[1], dz = pos[2] - D.cut_pos[2];
        double move = sqrt(dx * dx + dy * dy + dz * dz);
        double k = (now - D.cut_ns) / 1e6 / (1000.0 / 60);
        if (k < 0.25) k = 0.25;
        if (k > 4) k = 4;
        if (move > 5.0 * k || (turn > CUT_TURN * k && move > 0.5 * k)) {
            D.reset = 1, D.cuts++;
            if (D.cut_logged < 24)
                D.cut_logged++, rt_log("dlaa: frame %llu: camera cut (moved %.2f m, turned %.1f degrees in %.1f ms): new "
                                       "history (%llu so far)",
                                       (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1,
                                       move, turn, (now - D.cut_ns) / 1e6, (unsigned long long)D.cuts);
        }
    }
    memcpy(D.cut_pos, pos, sizeof pos), memcpy(D.cut_fwd, fwd, sizeof fwd);
    D.cut_ns = now, D.cut_ok = 1;
}

static int mv_pass(VkExec *ex, struct Size *e, VkImageView zv, float jx, float jy, float bias, int *masked) {
    *masked = 0;
    VkCommandBuffer cb = ex->cb;
    uint32_t w = e->w, h = e->h;
    static int keep = -1;
    if (keep < 0) keep = rt_knob("DLAA_CAM_KEEP", "1");
    VkDescriptorBufferInfo cam;
    int view = -1;
    for (int i = 0; D.cam_size == OCAM_BLOCK && D.cam_addr && i < D.nviews; i++)
        if (D.views[i].addr == D.cam_addr) view = i;
    const float *held = keep && view >= 0 && D.views[view].got ? D.views[view].cam : NULL;
    int found = mv_init() && D.cam_addr && vk_usnap_find(D.cam_addr, D.cam_size == OCAM_BLOCK ? OCAM_BYTES : D.cam_size, 4, &cam);
    if (!found && held && D.mv_ok == 1) {
        D.cam_at_draw++;
        if (D.gap_logged < 40) {
            D.gap_logged++;
            rt_log("dlaa: frame %llu: camera read at the draw: scene context %#llx last drawn %llu submission(s) before "
                   "DLSS's, there: %s; the frame's scene-depth draws over %llu submission(s); %d held submissions",
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1,
                   (unsigned long long)D.cam_addr, (unsigned long long)(vk_exec_submit_seq - D.views[view].seq),
                   usnap_why[vk_usnap_why(D.cam_addr, OCAM_BYTES)],
                   (unsigned long long)(F.zdraws + F.zother ? vk_exec_submit_seq - F.seq0 + 1 : 0), vk_exec_held());
        }
    }
    else if (!found) {
        D.mv_none++;
        if (D.cam_addr) D.cam_none_snap++;
        else D.cam_none_draw++;
        e->chars_ok = e->dist_ok = 0;
        int kept = 0;
        if (keep && D.mv_ok == 1 && D.cam_prev_ok) {
            if (D.keep_run < KEEP_MAX) kept = 1, D.keep_run++, D.cam_kept++;
            else kept = 2, D.cam_kept_max++;
        }
        if (D.gap_logged < 40) D.gap_logged++, cam_note(view, kept);
        if (kept == 1) return 2;
        D.cam_prev_ok = 0;
        return 0;
    }
    D.keep_run = 0;
    const float *cr = held ? held
                      : D.cam_size == OCAM_BLOCK ? (const float *)vk_usnap_bytes(D.cam_addr, OCAM_BYTES)
                                                 : (const float *)vk_usnap_bytes(D.cam_addr + D.cam_vp, 64);
    double cur_vp[16], m[16], ci[16];
    int have_m = 0;
    if (cr && D.cam_size == OCAM_BLOCK) ocam_vp(cr, cur_vp);
    else if (cr)
        for (int i = 0; i < 16; i++) cur_vp[i] = cr[i];
    if (cr) {
        if (D.cam_prev_ok && inv4(cur_vp, ci)) {
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) {
                    double v = 0;
                    for (int j = 0; j < 4; j++) v += D.cam_prev[r * 4 + j] * ci[j * 4 + k];
                    m[r * 4 + k] = v;
                }
            have_m = 1;
        }
    }
    D.fg_cam_ok = 0;
    if (cr && have_m && D.cam_size == OCAM_BLOCK) fg_camera(cr, m);
    if (cr && D.cam_size == OCAM_BLOCK) cam_cut(cr);
    int s = D.cam_slot, had_prev = D.cam_prev_ok;
    if (D.cam_size == CAM_BLOCK) {
        VkBufferCopy bc = {cam.offset, (VkDeviceSize)s * CAM_SLOT, CAM_BLOCK};
        vkCmdCopyBuffer(cb, cam.buffer, D.camb, 1, &bc);
        full_barrier(cb);
    }
    D.cam_slot ^= 1, D.cam_prev_ok = cr != NULL;
    if (cr) memcpy(D.cam_prev, cur_vp, sizeof cur_vp);
    if (!had_prev || !have_m) {
        e->chars_ok = e->dist_ok = 0;
        return 0;
    }
    VkDescriptorImageInfo zi = {D.mv_samp, zv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, e->mv_view, VK_IMAGE_LAYOUT_GENERAL};
    uint32_t ms = D.mvm_slot++ % MVM_SLOTS;
    float *blk = (float *)(D.mvm_map + (size_t)ms * 2 * CAM_BYTES);
    memset(blk, 0, 2 * CAM_BYTES);
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            blk[12 + r * 4 + k] = r == k ? 1.0f : 0.0f, blk[64 + 12 + r * 4 + k] = (float)m[r * 4 + k];
    vmaFlushAllocation(vk_ctx()->vma, D.mvm_mem, (VkDeviceSize)ms * 2 * CAM_BYTES, 2 * CAM_BYTES);
    VkDescriptorBufferInfo cur = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES, CAM_BYTES};
    VkDescriptorBufferInfo prev = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES + CAM_BYTES, CAM_BYTES};
    const VkExecTarget *ids = &D.ids;
    int use = bias > 0 && D.ids_ok && ids->image && ids->width == w && ids->height == h && !ids->is3d;
    VkImageView iv = use ? vk_exec_target_view(ids->img, ids->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                               ids->level, ids->base_layer, 1, 0)
                         : e->mask_view;
    int cur_c = e->chars_cur;
    VkDescriptorImageInfo ii = {D.mv_samp, iv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mki = {VK_NULL_HANDLE, e->mask_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo cpi = {VK_NULL_HANDLE, e->chars_view[cur_c ^ 1], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo coi = {VK_NULL_HANDLE, e->chars_view[cur_c], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo dpi = {VK_NULL_HANDLE, e->dist_view[e->dist_cur ^ 1], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo doi = {VK_NULL_HANDLE, e->dist_view[e->dist_cur], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[10] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &zi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &cur},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &prev},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &mki},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &cpi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 7, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &coi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 8, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dpi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 9, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &doi}};
    times_begin(cb);
    times_mark(cb, T_MV0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_layout, 0, 10, wr);
    static int zfit_on = -1;
    if (zfit_on < 0) zfit_on = rt_knob("DLAA_ZFIT", "1");
    int zfit = zfit_on && !use && cr && D.cam_size == OCAM_BLOCK;
    MvPush pc = {{(float)w, (float)h}, {jx, jy}, use ? bias : 0.0f, use && e->chars_ok ? 1.0f : 0.0f,
                 zfit ? 1.0f : 0.0f, zfit && e->dist_ok ? 1.0f : 0.0f,
                 {zfit ? cr[38] : 0.0f, zfit ? cr[39] : 0.0f, zfit ? cr[42] : 0.0f, zfit ? cr[43] : 0.0f},
                 zfit ? bias : 0.0f};
    vkCmdPushConstants(cb, D.mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    times_mark(cb, T_MV1);
    vk_mvcheck_camera(cb, e->mv, w, h, jx, jy);
    uint64_t c0 = T.on > 0 ? kern_now_ns() : 0;
    int object_motion = D.cam_size == CAM_BLOCK;
    if (D.cam_size == OCAM_BLOCK)
        object_motion = vk_fg_pose_prepare(ex, D.cam_addr, D.depth.image, w, h) && !D.reset && !D.stepped;
    if (object_motion && vk_mvobj_execute(ex, D.cam_addr, e->mv_view, zv, D.depth.format, w, h)) full_barrier(cb);
    if (T.on > 0) T.cpu_mv += kern_now_ns() - c0;
    times_mark(cb, T_OBJ);
    if (T.on > 0) T.used[T.slot] = 1;
    vk_mvcheck_final(cb, ex->fence, e->mv, D.color.image, D.color.format, D.color.level, D.color.base_layer, w, h, jx, jy);
    D.mv_frames++;
    if (use) e->chars_cur ^= 1, e->chars_ok = 1, D.masked++;
    else e->chars_ok = 0;
    if (zfit && bias > 0) D.masked++;
    e->pose_distance_valid = zfit && D.fg_cam_ok;
    e->pose_distance_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    e->pose_history[0] = jx, e->pose_history[1] = jy;
    e->pose_history[2] = D.fg_cam.view_to_clip[11];
    e->pose_history[3] = D.fg_cam.view_to_clip[15];
    e->dist_cur ^= 1, e->dist_ok = zfit;
    *masked = use || (zfit && bias > 0);
    return 1;
}

int vk_dlaa_prev_camera(VkDescriptorBufferInfo *out, float jitter[2]) {
    if (!inputs_on() || !D.cam_prev_ok || !D.camb) return 0;
    if (D.cam_size != CAM_BLOCK) return 0;
    *out = (VkDescriptorBufferInfo){D.camb, (VkDeviceSize)(D.cam_slot ^ 1) * CAM_SLOT, CAM_BLOCK};
    jitter[0] = D.jit_ok ? D.jx : 0.0f, jitter[1] = D.jit_ok ? D.jy : 0.0f;
    return 1;
}

#include "dlaa_mask_spv.h"
#define MASK_THR 0.1f
#define MASK_GAIN 2.5f
#define SNAPS 3

static struct {
    struct Snap {
        uint32_t w, h;
        VkFormat f;
        VkImage before, mask;
        VmaAllocation before_mem, mask_mem;
        VkImageView before_view, mask_view;
        uint64_t used;
    } s[SNAPS];
    struct Snap *cur;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    int ok, knob;
    uint64_t made, missed;
} R = {.ok = -1, .knob = -1};

static int mask_init(void) {
    if (R.knob < 0) R.knob = rt_knob("DLSS_REACTIVE", "0");
    if (!R.knob) return 0;
    if (R.ok >= 0) return R.ok;
    R.ok = 0;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 3, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_mask_spv,
                                   .pCode = dlaa_mask_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &R.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &R.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &R.layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    m = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) goto fail;
    return R.ok = 1;
fail:
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    rt_log("dlaa: the reactive mask pass could not be set up: DLSS runs without it");
    return 0;
}

static void destroy_snap(struct Snap *s) {
    const VkCtx *c = vk_ctx();
    if (s->before_view) vkDestroyImageView(c->device, s->before_view, NULL);
    if (s->mask_view) vkDestroyImageView(c->device, s->mask_view, NULL);
    if (s->before) vmaDestroyImage(c->vma, s->before, s->before_mem);
    if (s->mask) vmaDestroyImage(c->vma, s->mask, s->mask_mem);
    memset(s, 0, sizeof *s);
}

static void retire_snap(struct Snap *s) {
    vk_defer_view(s->before_view), vk_defer_view(s->mask_view);
    vk_defer_image(s->before, s->before_mem), vk_defer_image(s->mask, s->mask_mem);
    memset(s, 0, sizeof *s);
}

static void free_snap(struct Snap *s) {
    retire_snap(s);
    vk_upscaler_let_go();
}

static struct Snap *snap_for(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat f) {
    static uint64_t tick;
    struct Snap *e = NULL, *lru = &R.s[0];
    for (int i = 0; i < SNAPS; i++) {
        if (R.s[i].w == w && R.s[i].h == h && R.s[i].f == f) return R.s[i].used = ++tick, &R.s[i];
        if (!R.s[i].before && !e) e = &R.s[i];
        if (R.s[i].used < lru->used) lru = &R.s[i];
    }
    if (!e) free_snap(e = lru);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image2d(w, h, f, u, &e->before, &e->before_mem, &e->before_view) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u | VK_IMAGE_USAGE_STORAGE_BIT, &e->mask, &e->mask_mem, &e->mask_view)) {
        destroy_snap(e);
        return NULL;
    }
    VkImageMemoryBarrier2 ib[2];
    VkImage im[2] = {e->before, e->mask};
    for (int i = 0; i < 2; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    e->w = w, e->h = h, e->f = f, e->used = ++tick;
    return e;
}

static void mask_snapshot(VkExec *ex) {
    R.cur = NULL;
    if (mask_init() <= 0) return;
    const VkExecTarget *t = &D.color;
    struct Snap *s = snap_for(ex->cb, t->width, t->height, t->format);
    if (!s) return;
    vk_exec_barrier(ex);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {t->width, t->height, 1}};
    vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, s->before, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(ex->cb);
    ex->recorded += 2;
    R.cur = s;
}

static struct Snap *mask_pass(VkCommandBuffer cb, VkImageView cv, uint32_t w, uint32_t h) {
    struct Snap *s = R.cur;
    R.cur = NULL;
    if (mask_init() <= 0) return NULL;
    if (!s || s->w != w || s->h != h || s->f != D.color.format) return R.missed++, NULL;
    VkDescriptorImageInfo bi = {D.mv_samp, s->before_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ai = {D.mv_samp, cv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mi = {VK_NULL_HANDLE, s->mask_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ai},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &mi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 3, wr);
    float pc[4] = {(float)w, (float)h, MASK_THR, MASK_GAIN};
    vkCmdPushConstants(cb, R.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    R.made++;
    return s;
}

static void pb_destroy(void);
#ifndef _WIN32
static void sh_destroy(void);
#endif
static void hl_destroy(void) {
    const VkCtx *c = vk_ctx();
    if (D.hl_view) vkDestroyImageView(c->device, D.hl_view, NULL);
    if (D.hl) vmaDestroyImage(c->vma, D.hl, D.hl_mem);
    if (D.hl_srgb) vmaDestroyImage(c->vma, D.hl_srgb, D.hl_srgb_mem);
    D.hl_view = VK_NULL_HANDLE, D.hl = D.hl_srgb = VK_NULL_HANDLE;
}

static void retire_hl(void) {
    const uint64_t gate = presents_now() + FG_LATE_PRESENTS;
    dead_put(UPD_VIEW, D.hl_view, NULL, 0, gate);
    dead_put(UPD_IMG, D.hl, D.hl_mem, 0, gate);
    vk_defer_image(D.hl_srgb, D.hl_srgb_mem);
    D.hl_view = VK_NULL_HANDLE, D.hl = D.hl_srgb = VK_NULL_HANDLE;
}

int dbg_freecam_flying(void);
void dbg_freecam_ground(const float *dist, int n, float tan_half_y, float aspect);
#define FC_N 9
#define FC_SLOTS 4
static VkBuffer fc_buf;
static VmaAllocation fc_mem;
static float *fc_map;
static uint32_t fc_next;
static uint8_t fc_have[FC_SLOTS];

static void fcam_probe(VkCommandBuffer cb, const struct Size *e, uint32_t w, uint32_t h) {
    if (!dbg_freecam_flying() || !e->dist_ok) {
        memset(fc_have, 0, sizeof fc_have);
        return;
    }
    const VkCtx *c = vk_ctx();
    if (!fc_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = FC_SLOTS * FC_N * sizeof(float),
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &fc_buf, &fc_mem, &info) != VK_SUCCESS) {
            fc_buf = VK_NULL_HANDLE;
            return;
        }
        fc_map = info.pMappedData;
    }
    uint32_t s = fc_next++ % FC_SLOTS;
    if (fc_have[s]) {
        vmaInvalidateAllocation(c->vma, fc_mem, s * FC_N * sizeof(float), FC_N * sizeof(float));
        dbg_freecam_ground(fc_map + s * FC_N, FC_N, D.fg_cam_ok ? tanf(0.5f * D.fg_cam.fov_y) : 0.0f,
                           D.fg_cam_ok ? D.fg_cam.aspect : 0.0f);
    }
    static const float fx[3] = {0.2f, 0.5f, 0.8f}, fy[3] = {0.6f, 0.78f, 0.96f};
    VkBufferImageCopy r[FC_N];
    for (int i = 0; i < FC_N; i++)
        r[i] = (VkBufferImageCopy){.bufferOffset = (s * FC_N + i) * sizeof(float),
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageOffset = {(int32_t)(fx[i % 3] * (w - 1)), (int32_t)(fy[i / 3] * (h - 1)), 0},
                                   .imageExtent = {1, 1, 1}};
    vkCmdCopyImageToBuffer(cb, e->dist[e->dist_cur ^ 1], VK_IMAGE_LAYOUT_GENERAL, fc_buf, FC_N, r);
    fc_have[s] = 1;
}

void vk_dlaa_release(void) {
#ifndef _WIN32
    const VkCtx *c = vk_ctx();
    if (!c) return;
    upd_take_all(&dead, dead_release, (void *)1);
    for (int i = 0; i < DLSS_SIZES; i++)
        if (D.sizes[i].feat) NGX_CALL(&NL, NGX_AT_SETUP, N.release(D.sizes[i].feat)), D.sizes[i].feat = NULL;
    if (FG.feat) NGX_CALL(&NL, NGX_AT_SETUP, N.release(FG.feat));
    if (FG.p) NGX_CALL(&NL, NGX_AT_SETUP, N.destroy_params(FG.p));
    memset(&FG, 0, sizeof FG);
    if (D.params) NGX_CALL(&NL, NGX_AT_SETUP, N.destroy_params(D.params));
    if (N.caps_map) NGX_CALL(&NL, NGX_AT_SETUP, N.destroy_params(N.caps_map));
    for (int i = 0; i < DLSS_SIZES; i++) destroy_size(&D.sizes[i], NGX_AT_SETUP);
    for (int i = 0; i < SNAPS; i++) destroy_snap(&R.s[i]);
    hl_destroy();
    pb_destroy();
    sh_destroy();
    if (D.mv_pipe) vkDestroyPipeline(c->device, D.mv_pipe, NULL);
    if (D.mv_layout) vkDestroyPipelineLayout(c->device, D.mv_layout, NULL);
    if (D.mv_dsl) vkDestroyDescriptorSetLayout(c->device, D.mv_dsl, NULL);
    if (D.mv_samp) vkDestroySampler(c->device, D.mv_samp, NULL);
    if (D.camb) vmaDestroyBuffer(c->vma, D.camb, D.camb_mem);
    if (D.mvm) vmaDestroyBuffer(c->vma, D.mvm, D.mvm_mem);
    if (fc_buf) vmaDestroyBuffer(c->vma, fc_buf, fc_mem), fc_buf = VK_NULL_HANDLE, fc_map = NULL;
    if (R.pipe) vkDestroyPipeline(c->device, R.pipe, NULL);
    if (R.layout) vkDestroyPipelineLayout(c->device, R.layout, NULL);
    if (R.dsl) vkDestroyDescriptorSetLayout(c->device, R.dsl, NULL);
    if (T.pool) vkDestroyQueryPool(c->device, T.pool, NULL);
    memset(&D, 0, sizeof D), D.mv_ok = -1, D.cfg_seen = ~0u;
    memset(&R, 0, sizeof R), R.ok = -1, R.knob = -1;
    memset(&T, 0, sizeof T), T.on = -1;
    memset(&F, 0, sizeof F);
    vk_dlaa_swap_img = NULL, vk_dlaa_swap_view = VK_NULL_HANDLE;
    if (!N.device) return;
    unsigned left = 0;
    vk_queue_lock();
    NgxResult r = NGX_CALL(&NL, NGX_AT_SETUP, N.shutdown1(N.device, &left));
    vk_queue_unlock();
    rt_log("dlaa: NGX shut down (%08x, %u devices left)", (unsigned)r, left);
    ngx_lock_log();
    N.device = NULL, N.caps_map = NULL, N.ngx_up = 0, N.available = 0;
#endif
}

static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

static int jitter_draw(const VkExec *ex) {
    const VkExecTarget *z = &ex->depth;
    if (!D.jit_ok || D.ended || !z->image) return 0;
    if (D.cam_size != OCAM_BLOCK) return z->image == D.jdepth;
    return z->format == VK_FORMAT_D32_SFLOAT && z->width == D.jw && z->height == D.jh &&
           ex->ubo[0][0].size == OCAM_BLOCK && ex->ubo[0][0].addr;
}

void vk_dlaa_viewport(VkExec *ex, VkViewport *v) {
    float sx, sy;
    if (ex->has_vp && vk_dlaa_target_scale(ex, &sx, &sy))
        v->x *= sx, v->y *= sy, v->width *= sx, v->height *= sy;
    ex->vp_jittered = jitter_draw(ex);
    if (!ex->vp_jittered) return;
    ex->pass_jit = 1;
    v->x += D.jx, v->y += D.jy;
    D.ux = D.jx, D.uy = D.jy, D.jit_used = 1;
}

void vk_dlaa_jitter_check(VkExec *ex) {
    int j = jitter_draw(ex);
    if (j != ex->vp_jittered) ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
    const VkExecTarget *z = &ex->depth;
    if (D.jit_ok && !D.ended && z->image && z->format == VK_FORMAT_D32_SFLOAT && z->width == D.jw && z->height == D.jh) {
        if (j) D.jdraws++;
        else D.jfixed++;
    }
}

static int scene_color(VkFormat f);

static void dlaa_draw_locked(VkExec *ex);
void vk_dlaa_draw(VkExec *ex) {
    if (!inputs_on() || D.ended) return;
    static PlatMutex lock = PLAT_MUTEX_INIT;
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&lock);
    dlaa_draw_locked(ex);
    if (lk) plat_mutex_unlock(&lock);
}

static void dlaa_draw_locked(VkExec *ex) {
    static int survey = -1;
    if (survey < 0) survey = rt_knob("DLAA_SURVEY", "0");
    static uint64_t dfn[16];
    if (survey && ex->depth.image && ex->depth.image == D.jdepth && ex->pipe.ds.dt && ex->pipe.ds.dw)
        dfn[ex->pipe.ds.df & 15]++;
    if (survey && D.armed && ex->color[0].image == D.color.image) {
        static uint64_t n[2][2][2], arms, frames, last_frame;
        const VkPipeState *s = &ex->pipe;
        n[s->color.blend_en & 1][s->ds.dw][s->ds.dt]++;
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (f != last_frame) last_frame = f, frames++;
        static uint64_t ps;
        if (ex->pass_serial != ps) ps = ex->pass_serial, arms++;
        if (frames % 300 == 0) {
            static uint64_t logged;
            if (logged != frames) {
                logged = frames;
                rt_log("dlaa survey: scene draws writing depth by depth test (NVN func 1-8: never less equal lequal "
                       "greater notequal gequal always): %llu %llu %llu %llu %llu %llu %llu %llu",
                       (unsigned long long)dfn[1], (unsigned long long)dfn[2], (unsigned long long)dfn[3],
                       (unsigned long long)dfn[4], (unsigned long long)dfn[5], (unsigned long long)dfn[6],
                       (unsigned long long)dfn[7], (unsigned long long)dfn[8]);
                rt_log("dlaa survey: %llu frames, %.1f passes a frame into the light target after arming; draws a frame "
                       "(blend/depth write/depth test): 000 %.1f 001 %.1f 010 %.1f 011 %.1f 100 %.1f 101 %.1f 110 %.1f 111 %.1f",
                       (unsigned long long)frames, (double)arms / frames, (double)n[0][0][0] / frames, (double)n[0][0][1] / frames,
                       (double)n[0][1][0] / frames, (double)n[0][1][1] / frames, (double)n[1][0][0] / frames,
                       (double)n[1][0][1] / frames, (double)n[1][1][0] / frames, (double)n[1][1][1] / frames);
            }
        }
    }
    const p_bind_buf *b = &ex->ubo[0][1];
    const VkExecTarget *z = &ex->depth;
    if (!z->image || z->width < DLAA_SCENE_MIN_W || z->format != VK_FORMAT_D32_SFLOAT) return;
    if (!F.zdraws && !F.zother) F.seq0 = vk_exec_submit_seq;
    if (D.jdepth && z->image != D.jdepth) {
        F.zother++, F.zother_w = z->width, F.zother_h = z->height;
        return;
    }
    F.zdraws++;
    if (ex->ubo[0][0].size == OCAM_BLOCK && ex->ubo[0][0].addr) {
        F.ocam++;
        if (!ex->color[0].image || !scene_color(ex->color[0].format)) F.ocam_off++, F.ocam_off_fmt = ex->color[0].format;
    } else F.c3_other++, F.c3_size = ex->ubo[0][0].addr ? ex->ubo[0][0].size : 0;
    if (b->size == CAM_BLOCK && b->addr) D.cam_addr = b->addr, D.cam_size = CAM_BLOCK, D.cam_vp = 0x30;
    else if (ex->ubo[0][0].size == OCAM_BLOCK && ex->ubo[0][0].addr && ex->color[0].image &&
             scene_color(ex->color[0].format)) {
        uint64_t a = ex->ubo[0][0].addr;
        int i = 0, best = 0;
        while (i < D.nviews && D.views[i].addr != a) i++;
        if (i == D.nviews && D.nviews < 8) D.views[D.nviews].addr = a, D.views[D.nviews].n = 0, D.views[D.nviews++].got = 0;
        if (i < D.nviews) {
            D.views[i].n++, D.views[i].seq = vk_exec_submit_seq;
            if (!D.views[i].got) D.views[i].got = vk_usnap_read(a, OCAM_BYTES, D.views[i].cam);
        }
        for (int k = 1; k < D.nviews; k++)
            if (D.views[k].n > D.views[best].n) best = k;
        D.cam_addr = D.views[best].addr, D.cam_size = OCAM_BLOCK, D.cam_vp = 0;
    }
}

static int jitter_on(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("DLAA_JITTER", "1");
    return on;
}

static void next_jitter(uint32_t w, uint32_t ow) {
    int nj = !jitter_on();
    double r = w ? (double)ow / w : 1.0;
    uint32_t phases = (uint32_t)ceil(8.0 * r * r - 1e-6);
    if (phases < 16) phases = 16;
    if (phases > 72) phases = 72;
    uint32_t i = D.jframe++ % phases + 1;
    D.jx = halton(i, 2) - 0.5f, D.jy = halton(i, 3) - 0.5f;
    D.jit_ok = !D.hold && !nj && (D.cam_size == CAM_BLOCK || D.cam_size == OCAM_BLOCK);
}

static int preset_value(void) {
    const char *v = cfg_get("NX2SARIA_DLSS_PRESET");
    int c = v && v[0] && !v[1] ? v[0] | 0x20 : 0;
    return c >= 'a' && c <= 'm' ? c - 'a' + 1 : 0;
}

static float char_bias(void) {
    const char *v = cfg_get("NX2SARIA_DLSS_CHAR_MASK");
    long n = v && *v ? strtol(v, NULL, 10) : 65;
    return n <= 0 ? 0.0f : n >= 100 ? 1.0f : (float)n / 100.0f;
}

static int perf_quality(uint32_t w, uint32_t ow) {
    if (w >= ow) return NGX_PERFQ_DLAA;
    double k = (double)w / ow;
    return k >= 0.66 ? 2 : k >= 0.57 ? 1 : k >= 0.49 ? 0 : 3;
}

static void window_line(void) {
    enum { N_ = 11 };
    static uint64_t last[N_];
    static int lines;
    if (D.evals % 300) return;
    uint64_t now[N_] = {D.why[0], D.why[1], D.why[2], D.mv_none, D.cam_none_draw, D.cam_none_snap, D.cam_kept,
                        D.cam_at_draw, D.frames_missed, D.seconds, D.target_kept}, d[N_], any = 0;
    for (int i = 0; i < N_; i++) d[i] = now[i] - last[i], last[i] = now[i];
    for (int i = 0; i < N_; i++) any |= d[i];
    if (!any || lines >= 400) return;
    lines++;
    rt_log("dlaa: frames %llu-%llu: resets %llu size, %llu camera, %llu target (%llu target changes with the history "
           "kept); %llu without a camera (%llu without a "
           "scene-context draw, %llu not in DLSS's submission), %llu kept with zero motion; %llu cameras read at the draw; "
           "%llu frames without the DLAA point, %llu with a second scene after DLSS ran",
           (unsigned long long)(D.evals - 299), (unsigned long long)D.evals, (unsigned long long)d[0],
           (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[10], (unsigned long long)d[3],
           (unsigned long long)d[4],
           (unsigned long long)d[5], (unsigned long long)d[6], (unsigned long long)d[7], (unsigned long long)d[8],
           (unsigned long long)d[9]);
}

static void hudless(VkCommandBuffer cb, VkImage src, uint32_t w, uint32_t h);

static void target_changed(const VkExecTarget *t, int cont) {
    static int keep = -1;
    if (keep < 0) keep = rt_knob("DLAA_TARGET_KEEP", "1");
    int kept = keep && cont && D.last_color;
    if (D.last_color && D.target_logged < 16)
        D.target_logged++, rt_log("dlaa: frame %llu: the light target changed from %p to %p (%ux%u format %d): %s",
                                  (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1,
                                  (void *)D.last_color, (void *)t->image, t->width, t->height, (int)t->format,
                                  kept ? "the history stays (the camera carries on)" : "new history");
    if (kept) D.target_kept++;
    else D.reset = 1, D.why[2]++;
    D.last_color = t->image;
}

static volatile int64_t dlss_vram = -1;
int64_t vk_dlaa_vram(void) { return __atomic_load_n(&dlss_vram, __ATOMIC_RELAXED); }

static void query_vram(void) {
    void *cb = NULL;
    unsigned long long n = 0;
    if (fsr3() || !N.capsp || !p_getp(N.capsp, "DLSSGetStatsCallback", &cb) || !cb) return;
    if (NGX_OK(NGX_CALL(&NL, NGX_AT_REPLAY, ((NgxResult (*)(NgxParam *))cb)(N.capsp))) &&
        p_getull(N.capsp, "SizeInBytes", &n))
        __atomic_store_n(&dlss_vram, (int64_t)n, __ATOMIC_RELAXED);
}

static void stale_sizes(const struct Size *cur, const struct Size *host) {
    if (!idle_free()) return;
    int any = 0;
    for (int i = 0; i < DLSS_SIZES; i++) {
        struct Size *s = &D.sizes[i];
        if (s == host || !s->out) continue;
        if (s != cur && D.evals >= s->used + SIZE_STALE) {
            rt_log("dlaa: the DLSS feature at %ux%u -> %ux%u went unused for %d frames: freed", s->w, s->h, s->ow, s->oh,
                   SIZE_STALE);
            if (D.last == s) D.last = NULL;
            retire_size(s), any = 1;
        } else if (s->feat && D.evals >= s->feat_used + SIZE_STALE) {
            rt_log("dlaa: the DLSS feature at %ux%u -> %ux%u went unused for %d frames while its size ran on another "
                   "size's: released", s->w, s->h, s->ow, s->oh, SIZE_STALE);
            retire_feat(s, 1), any = 1;
        }
    }
    if (any) vk_upscaler_let_go();
}

static void release_kept(const struct Size *making, int all) {
    int n = 0;
    NgxHandle *mine[DLSS_SIZES];
    for (int i = 0; i < DLSS_SIZES; i++) {
        struct Size *s = &D.sizes[i];
        if (s == making || !s->feat) continue;
        if (!all && (uint64_t)s->w * s->h <= (uint64_t)making->w * making->h) continue;
        mine[n] = s->feat;
        retire_feat(s, 0), n++;
        if (D.last == s) D.last = NULL;
        if (D.host == s) D.host = NULL;
    }
    if (!n) return;
    vk_upscaler_let_go();
    int left = 0;
    for (int i = 0; i < dead.n; i++)
        for (int j = 0; j < n; j++) left += dead.e[i].kind == UPD_FEAT && dead.e[i].handle == (void *)mine[j];
    char later[80] = "";
    if (left) snprintf(later, sizeof later, "; %d of them once the GPU is past their last frame", left);
    rt_log("dlaa: %d DLSS feature(s) released before the one for %ux%u is made (%s); made again when used%s", n,
           making->w, making->h, all ? "after a release" : "larger inputs", later);
}

static int edge_fill_mode(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("DLAA_EDGE_FILL", "1");
    return on;
}

static void edge_copy(VkCommandBuffer cb, const VkExecTarget *g, VkImageAspectFlags aspect) {
    int32_t w = (int32_t)g->width, h = (int32_t)g->height;
    if (w < 4 || h < 4) return;
    const VkImageSubresourceLayers sl = {aspect, g->level, g->base_layer, 1};
#define EDGE(sx, sy, dx, dy, ew, eh)                                                                                      \
    {.srcSubresource = sl, .srcOffset = {sx, sy, 0}, .dstSubresource = sl, .dstOffset = {dx, dy, 0},                     \
     .extent = {(uint32_t)(ew), (uint32_t)(eh), 1}}
    const VkImageCopy r[8] = {EDGE(1, 1, 0, 1, 1, h - 2),         EDGE(w - 2, 1, w - 1, 1, 1, h - 2),
                              EDGE(1, 1, 1, 0, w - 2, 1),         EDGE(1, h - 2, 1, h - 1, w - 2, 1),
                              EDGE(1, 1, 0, 0, 1, 1),             EDGE(w - 2, 1, w - 1, 0, 1, 1),
                              EDGE(1, h - 2, 0, h - 1, 1, 1),     EDGE(w - 2, h - 2, w - 1, h - 1, 1, 1)};
#undef EDGE
    vkCmdCopyImage(cb, g->image, VK_IMAGE_LAYOUT_GENERAL, g->image, VK_IMAGE_LAYOUT_GENERAL, 8, r);
}

static int edge_target(const VkExecTarget *g) { return g->image && g->samples <= 1 && !g->is3d && !g->blocks; }

void vk_dlaa_pass_end(VkExec *ex) {
    if (!ex->pass_jit) return;
    ex->pass_jit = 0;
    if (edge_fill_mode() != 1) return;
    int any = edge_target(&ex->depth) && (ex->depth.aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++) any |= edge_target(&ex->color[i]);
    if (!any) return;
    static int told;
    if (!told++) rt_log("dlaa: the outer rows and columns of each jittered pass's targets are filled from the ones next to them");
    vk_exec_barrier(ex);
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++)
        if (edge_target(&ex->color[i])) edge_copy(ex->cb, &ex->color[i], VK_IMAGE_ASPECT_COLOR_BIT), ex->recorded++;
    if (edge_target(&ex->depth) && (ex->depth.aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        edge_copy(ex->cb, &ex->depth, VK_IMAGE_ASPECT_DEPTH_BIT), ex->recorded++;
}

static void edge_fill(VkCommandBuffer cb, const VkExecTarget *t, const VkExecTarget *z, int jittered) {
    if (!edge_fill_mode() || !jittered) return;
    if (edge_target(t)) edge_copy(cb, t, VK_IMAGE_ASPECT_COLOR_BIT);
    if (edge_target(z) && z->width == t->width && z->height == t->height) edge_copy(cb, z, VK_IMAGE_ASPECT_DEPTH_BIT);
    full_barrier(cb);
}

#define SHARPEN_STOPS 1.5f
static struct {
    int ok;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
} SH = {.ok = -1};

#ifndef _WIN32
static void sh_destroy(void) {
    const VkCtx *c = vk_ctx();
    if (SH.pipe) vkDestroyPipeline(c->device, SH.pipe, NULL);
    if (SH.layout) vkDestroyPipelineLayout(c->device, SH.layout, NULL);
    if (SH.dsl) vkDestroyDescriptorSetLayout(c->device, SH.dsl, NULL);
    memset(&SH, 0, sizeof SH), SH.ok = -1;
}
#endif

static int sharpen_init(void) {
    if (SH.ok >= 0) return SH.ok;
    SH.ok = 0;
    const VkCtx *c = vk_ctx();
    if (!mv_init() || !D.mv_samp) return 0;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(float)};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_sharpen_spv,
                                   .pCode = dlaa_sharpen_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &SH.dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &SH.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &SH.layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = SH.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &SH.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("dlaa: FSR 3.1's second sharpening pass could not be set up");
        return 0;
    }
    return SH.ok = 1;
}

float vk_dlaa_fsr3_boost(uint32_t w, uint32_t ow) {
    double share = ow ? (double)w / ow : 1.0, k = (0.75 - share) / 0.25;
    return (float)(k < 0 ? 0 : k > 1 ? 1 : k);
}

static void fsr3_sharpen(VkCommandBuffer cb, struct Size *e, uint32_t w, uint32_t ow, uint32_t oh) {
    static int on = -1;
    if (on < 0) on = rt_knob("FSR3_SHARPEN", "1");
    int slider = cfg_int("NX2SARIA_SHARPNESS");
    float k = vk_dlaa_fsr3_boost(w, ow);
    if (!on || slider <= 0 || k <= 0 || !sharpen_init()) return;
    if (!e->sharp) {
        VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!image2d(ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, u, &e->sharp, &e->sharp_mem, &e->sharp_view)) {
            const VkCtx *c = vk_ctx();
            if (e->sharp_view) vkDestroyImageView(c->device, e->sharp_view, NULL);
            if (e->sharp) vmaDestroyImage(c->vma, e->sharp, e->sharp_mem);
            e->sharp = VK_NULL_HANDLE, e->sharp_view = VK_NULL_HANDLE;
            return;
        }
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = e->sharp,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
        rt_log("dlaa: FSR 3.1's picture gets a second sharpening pass at this render scale (%u of %u wide)", w, ow);
    }
    const float stops = SHARPEN_STOPS + (100.0f - (float)(slider > 100 ? 100 : slider)) / 50.0f + 2.0f * (1.0f - k);
    const float pc[4] = {(float)ow, (float)oh, stops, 0};
    VkDescriptorImageInfo si = {D.mv_samp, e->out_view, VK_IMAGE_LAYOUT_GENERAL},
                          di = {VK_NULL_HANDLE, e->sharp_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &di}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, SH.pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, SH.layout, 0, 2, wr);
    vkCmdPushConstants(cb, SH.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (ow + 15) / 16, (oh + 15) / 16, 1);
    full_barrier(cb);
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {ow, oh, 1}};
    vkCmdCopyImage(cb, e->sharp, VK_IMAGE_LAYOUT_GENERAL, e->out, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    full_barrier(cb);
}

static int range_refused;

static int range_on(void) {
    static int m = -1;
    if (m < 0) m = cfg_int("NX2SARIA_DLSS_RANGE") != 0;
    return m && !range_refused;
}

static int setting_quality(void) {
    const char *m = cfg_value("NX2SARIA_DLSS");
    return !strcmp(m, "quality") ? 2 : !strcmp(m, "balanced") ? 1 : !strcmp(m, "performance") ? 0
         : !strcmp(m, "ultra_performance") ? 3 : NGX_PERFQ_DLAA;
}

static int range_query(uint32_t ow, uint32_t oh, int q, unsigned opt[2], unsigned lo[2], unsigned hi[2]) {
    NgxParam *c = N.capsp;
    void *cb = NULL;
    if (!c || !p_getp(c, "DLSSOptimalSettingsCallback", &cb) || !cb) return 0;
    p_setu(c, "Width", ow);
    p_setu(c, "Height", oh);
    p_seti(c, "PerfQualityValue", q);
    p_seti(c, "RTXValue", 0);
    if (!NGX_OK(NGX_CALL(&NL, NGX_AT_REPLAY, ((NgxResult (*)(NgxParam *))cb)(c)))) return 0;
    if (!p_getu(c, "OutWidth", &opt[0]) || !p_getu(c, "OutHeight", &opt[1])) return 0;
    lo[0] = hi[0] = opt[0], lo[1] = hi[1] = opt[1];
    p_getu(c, "DLSS.Get.Dynamic.Max.Render.Width", &hi[0]);
    p_getu(c, "DLSS.Get.Dynamic.Max.Render.Height", &hi[1]);
    p_getu(c, "DLSS.Get.Dynamic.Min.Render.Width", &lo[0]);
    p_getu(c, "DLSS.Get.Dynamic.Min.Render.Height", &lo[1]);
    return 1;
}

static void range_note(struct Size *s) {
    unsigned opt[2] = {0, 0}, lo[2] = {0, 0}, hi[2] = {0, 0};
    s->ranged = 0;
    if (!range_on()) return;
    if (!range_query(s->ow, s->oh, s->q, opt, lo, hi)) {
        rt_log("dlaa: the DLSS feature for %ux%u -> %ux%u runs its own render size only: this DLSS library gives no "
               "optimal settings", s->w, s->h, s->ow, s->oh);
        return;
    }
    const char *why = lo[0] == hi[0] && lo[1] == hi[1] ? "DLSS takes one render size for this output size and quality value"
                      : s->q != setting_quality() ? "it is not at the render size of the DLSS mode set"
                      : s->w < lo[0] || s->h < lo[1] || s->w > hi[0] || s->h > hi[1] ? "its own size is outside DLSS's range"
                      : s->w == lo[0] && s->h == lo[1] ? "its own size is the smallest of DLSS's range" : NULL;
    if (why) {
        rt_log("dlaa: the DLSS feature for %ux%u -> %ux%u runs its own render size only: %s (quality value %d: DLSS's "
               "range %ux%u to %ux%u, its own choice %ux%u)", s->w, s->h, s->ow, s->oh, why, s->q, lo[0], lo[1], hi[0],
               hi[1], opt[0], opt[1]);
        return;
    }
    s->ranged = 1, s->min_w = lo[0], s->min_h = lo[1];
    rt_log("dlaa: the DLSS feature for %ux%u -> %ux%u also runs the render sizes from %ux%u up to its own, the history "
           "kept (quality value %d: DLSS's range %ux%u to %ux%u, its own choice %ux%u)", s->w, s->h, s->ow, s->oh, lo[0],
           lo[1], s->q, lo[0], lo[1], hi[0], hi[1], opt[0], opt[1]);
}

static struct Size *range_host(struct Size *e) {
    int q = setting_quality();
    if (e->ranged && e->q == q) return e;
    struct Size *host = e;
    for (int i = 0; i < DLSS_SIZES; i++) {
        struct Size *s = &D.sizes[i];
        if (s == e || !s->out || !s->ranged || s->q != q || s->ow != e->ow || s->oh != e->oh) continue;
        if (e->w > s->w || e->h > s->h || e->w < s->min_w || e->h < s->min_h) continue;
        if ((double)e->w * s->h > 1.03 * (double)e->h * s->w || (double)e->h * s->w > 1.03 * (double)e->w * s->h) continue;
        if (s == D.host) return s;
        if (host == e) host = s;
    }
    return host;
}

static volatile int32_t size_changed;

static void fg_only_frame(VkExec *ex, const VkExecTarget *dst);

static void evaluate(VkExec *ex, const VkExecTarget *dst) {
    upscalers_drain();
    if (D.fgo) {
        fg_only_frame(ex, dst);
        return;
    }
    const VkExecTarget *t = &D.color, *z = &D.depth;
    if (!dst) dst = t;
    uint32_t w = t->width, h = t->height, ow = dst->width, oh = dst->height;
    vk_exec_tail_begin(ex);
    VkCommandBuffer cb = ex->cb;
    vk_exec_barrier(ex);
    vk_gpu_gap_note(VK_GAP_UPSCALER, (uint64_t)ow * oh);
    if (D.preset_dirty) {
        for (int i = 0; i < DLSS_SIZES; i++)
            if (D.sizes[i].out) retire_size(&D.sizes[i]);
        vk_upscaler_let_go();
        D.last = NULL, D.preset_dirty = 0;
    }
    struct Size *e = size_for(cb, w, h, ow, oh);
    if (!e) return dl_fail("work image creation", 0);
    {
        const int at = D.pre ? 1 : 0;
        static int told;
        if (D.evals && at != D.pre_at) {
            D.reset = 1;
            if (told < 8)
                told++, rt_log("dlaa: the upscaler now runs %s: a new history", at ? "before the bloom" : "after the tonemap");
        }
        D.pre_at = at;
    }
    const int fsr = use_fsr();
    const int use_range = !fsr && N.ngx_up && range_on();
    struct Size *host = use_range && z->width >= w && z->height >= h ? range_host(e) : e;
    const int step = D.ev_w && (w != D.ev_w || h != D.ev_h || ow != D.ev_ow || oh != D.ev_oh);
    D.stepped = 0;
    if (e != D.last) {
        if (use_range && host->feat && host == D.host) {
            D.stepped = 1, D.steps_kept += step, e->chars_ok = e->dist_ok = 0;
        } else D.reset = 1, D.why[0]++, e->chars_ok = 0;
        __atomic_store_n(&size_changed, 1, __ATOMIC_RELEASE);
    } else if (use_range && host != D.host) D.reset = 1, D.why[0]++;
    if (!fsr && !N.ngx_up) return;
    if (!fsr && !D.params && !NGX_OK(NGX_CALL(&NL, NGX_AT_REPLAY, N.alloc_params(&D.params))))
        return dl_fail("parameter allocation", 0);
    NgxParam *p = D.params;
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    float jx = D.jit_used ? D.ux : 0.0f, jy = D.jit_used ? D.uy : 0.0f;
    static float jt[2] = {1.0f, 1.0f};
    static int jt_read;
    if (!jt_read) {
        jt_read = 1;
        const char *s = cfg_get("NX2SARIA_DLAA_JITTER_TEST");
        if (s && *s && sscanf(s, "%f,%f", &jt[0], &jt[1]) == 2)
            rt_log("dlaa: the upscaler is told the jitter times %g, %g (NX2SARIA_DLAA_JITTER_TEST, a test)", jt[0], jt[1]);
        else jt[0] = jt[1] = 1.0f;
    }
    const float tjx = jx * jt[0], tjy = jy * jt[1];
    edge_fill(cb, t, z, D.jit_used);
    static int no_mv = -1;
    if (no_mv < 0) no_mv = !rt_knob("DLAA_MV", "1");
    int masked = 0, mv = no_mv ? 0 : mv_pass(ex, e, zv, jx, jy, char_bias(), &masked);
    if (mv != 1) {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        full_barrier(cb);
        if (!no_mv && mv == 0) D.reset = 1, D.why[1]++;
    }
    VkImageView cv = vk_exec_target_view(t->img, t->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, t->level,
                                         t->base_layer, 1, 0);
    struct Snap *mk = D.mv_samp ? mask_pass(cb, cv, w, h) : NULL;
    int timed = T.on > 0 && T.used[T.slot];
    if (timed) times_mark(cb, T_DL0);
    uint64_t c0 = timed ? kern_now_ns() : 0;
    rec_drain();
    if (timed) T.cpu_drain += kern_now_ns() - c0;
    if (mv == 1) fcam_probe(cb, e, w, h);
    if (fsr) {
        if (t->level || t->base_layer || z->level || z->base_layer) return dl_fail("FSR 3.1 on a target that is not level 0, layer 0", 0);
        if (D.last_color != t->image) target_changed(t, mv == 1);
        uint64_t now = plat_now_ns();
        double dt = D.last_eval_ns ? (now - D.last_eval_ns) / 1e6 : 0;
        D.last_eval_ns = now;
        VkFfxImage fc = {t->image, t->format, w, h, 0, cv}, fz = {z->image, z->format, w, h, 0, zv},
                   fm = {e->mv, VK_FORMAT_R16G16_SFLOAT, w, h, 1, e->mv_view},
                   fo = {e->out, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, 1, e->out_view};
        VkFfxCamera fcam;
        if (!vk_dlaa_ffx_camera(&fcam)) memset(&fcam, 0, sizeof fcam);
        if (!vk_ffx_upscale(cb, &fc, &fz, &fm, &fo, tjx, tjy, D.reset, dt > 0 && dt < 100 ? (float)dt : 16.67f, &fcam))
            return dl_fail("FSR 3.1", 0);
        static int told;
        if (!told++) rt_log("dlaa: FSR 3.1 at %ux%u -> %ux%u (scene format %d, depth format %d)", w, h, ow, oh,
                            (int)t->format, (int)z->format);
        goto evaluated;
    }
    float frame_ms = 0;
own_feature:
    if (!host->feat) {
        const uint32_t fw = host->w, fh = host->h;
        if (idle_free() != 2) release_kept(host, feat_released);
        feat_released = 0;
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", fw);
        p_setu(p, "Height", fh);
        p_setu(p, "OutWidth", ow);
        p_setu(p, "OutHeight", oh);
        p_seti(p, "PerfQualityValue", perf_quality(fw, ow));
        p_seti(p, "DLSS.Feature.Create.Flags", (1 << 0) | (1 << 1) | (1 << 6));
        p_seti(p, "DLSS.Enable.Output.Subrects", 0);
        int pk = D.preset ? D.preset : 11, pm = D.preset ? D.preset : 13, pl = D.preset ? D.preset : 12;
        p_setu(p, "DLSS.Hint.Render.Preset.DLAA", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.UltraQuality", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Quality", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Balanced", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Performance", (unsigned)pm);
        p_setu(p, "DLSS.Hint.Render.Preset.UltraPerformance", (unsigned)pl);
        uint64_t t0 = perf_mark_begin();
        NgxResult r = NGX_CALL(&NL, NGX_AT_REPLAY, N.create(cb, NGX_FEATURE_SUPERSAMPLING, p, &host->feat));
        perf_mark_end(PERF_M_NGX, t0, 0);
        const int inflight = !NGX_OK(r) || !host->feat ? upd_count(&dead, UPD_FEAT) : 0;
        if (inflight) {
            const NgxResult r0 = r;
            const uint64_t w0 = plat_now_ns();
            upd_take(&dead, vk_upscaler_gpu_wait(upd_max_fence(&dead)), presents_now(), dead_release, NULL);
            const double waited = (plat_now_ns() - w0) / 1e6;
            host->feat = NULL;
            t0 = perf_mark_begin();
            r = NGX_CALL(&NL, NGX_AT_REPLAY, N.create(cb, NGX_FEATURE_SUPERSAMPLING, p, &host->feat));
            perf_mark_end(PERF_M_NGX, t0, 0);
            rt_log("dlaa: DLSS feature creation refused (%08x) with %d released feature(s) still in flight: waited %.1f "
                   "ms for them, %s", (unsigned)r0, inflight, waited,
                   NGX_OK(r) && host->feat ? "made at the second try" : "refused again");
        }
        if (!NGX_OK(r) || !host->feat) return dl_fail("DLSS feature creation", r);
        host->feat_fence = vk_exec_recording_fence();
        D.reset = 1, D.steps_made += step;
        int q = perf_quality(fw, ow), pr = q == 0 ? pm : q == 3 ? pl : pk;
        rt_log("dlaa: DLSS feature created at %ux%u -> %ux%u, quality value %d, preset %c (scene format %d, depth format "
               "%d)", fw, fh, ow, oh, q, 'A' + pr - 1, (int)t->format, (int)z->format);
        host->q = q;
        range_note(host);
    }
    if (D.last_color != t->image) target_changed(t, mv == 1);
    NgxResourceVK rc = res_image(cv, t->image, t->format, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK ro = res_image(e->out_view, e->out, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    NgxResourceVK rz = res_image(zv, z->image, z->format, w, h, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
    NgxResourceVK rm = res_image(e->mv_view, e->mv, VK_FORMAT_R16G16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "Color", &rc);
    p_setp(p, "Output", &ro);
    p_setp(p, "Depth", &rz);
    p_setp(p, "MotionVectors", &rm);
    p_setf(p, "Jitter.Offset.X", tjx);
    p_setf(p, "Jitter.Offset.Y", tjy);
    p_setf(p, "Sharpness", 0.0f);
    p_seti(p, "Reset", D.reset);
    NgxResourceVK rk = mk ? res_image(mk->mask_view, mk->mask, VK_FORMAT_R8_UNORM, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0)
                          : res_image(e->mask_view, e->mask, VK_FORMAT_R8_UNORM, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "DLSS.Input.Bias.Current.Color.Mask", mk || masked ? (void *)&rk : NULL);
    p_setu(p, "DLSS.Input.Bias.Current.Color.Subrect.Base.X", 0);
    p_setu(p, "DLSS.Input.Bias.Current.Color.Subrect.Base.Y", 0);
    p_setf(p, "MV.Scale.X", 1.0f);
    p_setf(p, "MV.Scale.Y", 1.0f);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", w);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", h);
    if (host != e) {
        static const char *const base[6] = {"DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y",
                                            "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y",
                                            "DLSS.Input.MV.Subrect.Base.X",    "DLSS.Input.MV.Subrect.Base.Y"};
        for (int i = 0; i < 6; i++) p_setu(p, base[i], 0);
    }
    p_setf(p, "DLSS.Pre.Exposure", 1.0f);
    p_setf(p, "DLSS.Exposure.Scale", 1.0f);
    if (!frame_ms) {
        uint64_t now = plat_now_ns();
        double dt = D.last_eval_ns ? (now - D.last_eval_ns) / 1e6 : 0;
        D.last_eval_ns = now;
        frame_ms = dt > 0 && dt < 100 ? (float)dt : 16.67f;
    }
    p_setf(p, "FrameTimeDeltaInMsec", frame_ms);
    c0 = timed ? kern_now_ns() : 0;
    NgxResult r = NGX_CALL(&NL, NGX_AT_REPLAY, N.evaluate(cb, host->feat, p, NULL));
    host->feat_fence = vk_exec_recording_fence();
    if (timed) T.cpu_ngx += kern_now_ns() - c0;
    if (!NGX_OK(r) && host != e) {
        rt_log("dlaa: DLSS refused the render size %ux%u on the feature created for %ux%u -> %ux%u (%08x): every render "
               "size has its own feature for the rest of this run", w, h, host->w, host->h, ow, oh, (unsigned)r);
        range_refused = 1;
        if (D.stepped) D.stepped = 0, D.steps_kept -= step;
        host = e, D.reset = 1, D.why[0]++;
        goto own_feature;
    }
    if (!NGX_OK(r)) return dl_fail("DLSS evaluation", r);
evaluated:
    if (timed) times_mark(cb, T_DL1);
    D.fg_reset = D.reset || D.stepped;
    D.reset = 0;
    D.last = e, D.host = host, e->used = host->used = host->feat_used = D.evals + 1;
    D.ev_w = w, D.ev_h = h, D.ev_ow = ow, D.ev_oh = oh;
    if (D.evals % 600 == 0) query_vram();
    stale_sizes(e, host);
    full_barrier(cb);
    if (fsr && !vk_ffx_fsr4() && w < ow) fsr3_sharpen(cb, e, w, ow, oh);
    vk_mvcheck_output(cb, e->out, ow, oh);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst->level, dst->base_layer, 1},
                     .dstOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}}};
    static int mvdebug = -1;
    if (mvdebug < 0) mvdebug = rt_knob("DLAA_MVDEBUG", "0");
    static int maskdebug = -1;
    if (maskdebug < 0) maskdebug = rt_knob("DLAA_MASKDEBUG", "0");
    VkImage shown = mvdebug ? e->mv : maskdebug && mk ? mk->mask : maskdebug && masked ? e->mask : e->out;
    if (shown != e->out) b.srcOffsets[1] = (VkOffset3D){(int32_t)w, (int32_t)h, 1};
    vkCmdBlitImage(cb, shown, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    full_barrier(cb);
    if (dst != t) vk_dlaa_swap_img = t->img, vk_dlaa_swap_view = e->out_view;
    if (vk_dlaa_hudless_wanted && !D.pre) hudless(cb, e->out, ow, oh);
    ex->recorded += 4;
    vk_exec_draw_new_cb();
    D.jdepth = z->image, D.jw = z->width, D.jh = z->height;
    D.fg_depth = (VkFfxImage){z->image, z->format, w, h, 0, zv};
    D.fg_mv = (VkFfxImage){e->mv, VK_FORMAT_R16G16_SFLOAT, w, h, 1, e->mv_view};
    D.fg_depth_view = zv, D.fg_mv_view = e->mv_view;
    D.fg_jx = jx, D.fg_jy = jy, D.fg_at = D.presents, D.fg_fresh = 1;
    if (D.jit_used) D.jittered++;
    next_jitter(w, ow);
    if (++D.evals == 1 || D.evals == 300 || D.evals % 3600 == 0)
        rt_log("dlaa: %llu frames anti-aliased (scene %ux%u, output %ux%u; %llu frames without the DLAA point, %llu with camera "
               "motion, %llu with the characters' mask, %llu without a camera, %llu jittered (%llu draws moved, %llu not), "
               "%llu jitter holds; resets: %llu size, %llu camera, %llu target; reactive mask %llu, without its copy %llu; "
               "no camera: %llu without a scene-context draw, %llu not in DLSS's submission; %llu kept with zero motion, "
               "%llu runs past %d; %llu cameras read at the draw; changes of scene size: %llu with the history kept, "
               "%llu with a new feature)",
               (unsigned long long)D.evals, w, h, ow, oh, (unsigned long long)D.frames_missed,
               (unsigned long long)D.mv_frames, (unsigned long long)D.masked, (unsigned long long)D.mv_none,
               (unsigned long long)D.jittered, (unsigned long long)D.jdraws, (unsigned long long)D.jfixed,
               (unsigned long long)D.holds, (unsigned long long)D.why[0], (unsigned long long)D.why[1],
               (unsigned long long)D.why[2], (unsigned long long)R.made, (unsigned long long)R.missed,
               (unsigned long long)D.cam_none_draw, (unsigned long long)D.cam_none_snap, (unsigned long long)D.cam_kept,
               (unsigned long long)D.cam_kept_max, KEEP_MAX, (unsigned long long)D.cam_at_draw,
               (unsigned long long)D.steps_kept, (unsigned long long)D.steps_made);
    if (D.evals == 1 || D.evals == 300 || D.evals % 3600 == 0) ngx_lock_log();
    window_line();
}

int vk_dlaa_hudless_wanted;
static void hudless_from(VkCommandBuffer cb, VkImage src, uint32_t level, uint32_t layer, uint32_t w, uint32_t h);
static void hudless(VkCommandBuffer cb, VkImage src, uint32_t w, uint32_t h) { hudless_from(cb, src, 0, 0, w, h); }
static void hudless_from(VkCommandBuffer cb, VkImage src, uint32_t level, uint32_t layer, uint32_t w, uint32_t h) {
    const VkCtx *c = vk_ctx();
    int hdr = vk_hdr_wanted() && !vk_hdr_native();
    if (vk_hdr_native()) return;
    VkFormat fmt = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    if (D.hl && (D.hl_w != w || D.hl_h != h || D.hl_fmt != fmt)) {
        retire_hl();
        vk_upscaler_let_go();
    }
    if (!D.hl) {
        VkImageView v2 = VK_NULL_HANDLE;
        VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if ((!hdr && !image2d(w, h, VK_FORMAT_R8G8B8A8_SRGB, u, &D.hl_srgb, &D.hl_srgb_mem, &v2)) ||
            !image2d(w, h, fmt, u | VK_IMAGE_USAGE_STORAGE_BIT, &D.hl, &D.hl_mem, &D.hl_view)) {
            if (v2) vkDestroyImageView(c->device, v2, NULL);
            hl_destroy();
            rt_log("dlaa: the HUD-less picture's images could not be made: frame generation sees the HUD");
            vk_dlaa_hudless_wanted = 0;
            return;
        }
        if (v2) vkDestroyImageView(c->device, v2, NULL);
        D.hl_w = w, D.hl_h = h, D.hl_fmt = fmt;
        VkImageMemoryBarrier2 ib[2];
        VkImage im[2] = {D.hl, D.hl_srgb};
        int nim = D.hl_srgb ? 2 : 1;
        for (int i = 0; i < nim; i++)
            ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = (uint32_t)nim,
                               .pImageMemoryBarriers = ib};
        vkCmdPipelineBarrier2(cb, &di);
    }
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, hdr ? D.hl : D.hl_srgb, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   VK_FILTER_NEAREST);
    full_barrier(cb);
    if (hdr) {
        D.hl_at = D.presents, D.hl_fresh = 1;
        return;
    }
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {w, h, 1}};
    vkCmdCopyImage(cb, D.hl_srgb, VK_IMAGE_LAYOUT_GENERAL, D.hl, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    full_barrier(cb);
    D.hl_at = D.presents, D.hl_fresh = 1;
}

static int scene_color(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

static int srgb8(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }

static int output_target(VkFormat f) { return srgb8(f) || vk_hdr_native_picture(f); }

double wide_dlss_share(void);

static struct {
    VkImage scene, comp;
    VmaAllocation scene_mem, comp_mem;
    VkImageView scene_view, comp_view;
    uint32_t w, h;
    uint64_t used;
} PB;

static void pb_destroy(void) {
    const VkCtx *c = vk_ctx();
    if (PB.scene_view) vkDestroyImageView(c->device, PB.scene_view, NULL);
    if (PB.comp_view) vkDestroyImageView(c->device, PB.comp_view, NULL);
    if (PB.scene) vmaDestroyImage(c->vma, PB.scene, PB.scene_mem);
    if (PB.comp) vmaDestroyImage(c->vma, PB.comp, PB.comp_mem);
    memset(&PB, 0, sizeof PB);
}

static void retire_pb(void) {
    vk_defer_view(PB.scene_view), vk_defer_view(PB.comp_view);
    vk_defer_image(PB.scene, PB.scene_mem), vk_defer_image(PB.comp, PB.comp_mem);
    const uint64_t used = PB.used;
    memset(&PB, 0, sizeof PB);
    PB.used = used;
}

static int pb_images(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    PB.used = D.presents;
    if (PB.scene && PB.w == w && PB.h == h) return 1;
    if (PB.scene || PB.comp) {
        retire_pb();
        vk_upscaler_let_go();
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkFormat f = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    if (!image2d(w, h, f, u, &PB.scene, &PB.scene_mem, &PB.scene_view) ||
        !image2d(w, h, f, u, &PB.comp, &PB.comp_mem, &PB.comp_view)) {
        pb_destroy();
        return 0;
    }
    VkImageMemoryBarrier2 ib[2];
    VkImage im[2] = {PB.scene, PB.comp};
    for (int i = 0; i < 2; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    PB.w = w, PB.h = h, PB.used = D.presents;
    rt_log("dlaa: prebloom: the passes after the upscaler draw at %ux%u (two B10G11R11 images, %.0f MiB)", w, h,
           2.0 * w * h * 4 / 1048576.0);
    return 1;
}

static int prebloom_start(const VkExec *ex) {
    if (ex->pass_ncolor != 2 || ex->depth.image) return 0;
    for (int i = 0; i < 2; i++) {
        const VkExecTarget *t = &ex->color[i];
        if (!t->image || t->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || t->samples > 1 || t->is3d ||
            t->width >= D.color.width || t->height >= D.color.height)
            return 0;
    }
    return 1;
}

static int prebloom_run(VkExec *ex) {
    const uint32_t w = D.color.width, h = D.color.height;
    const double share = wide_dlss_share();
    const int native = share <= 0 || share >= 1;
    uint32_t ow = D.pic_w, oh = D.pic_h;
    if (!native) {
        int known = ow > w && oh > h && (double)ow * h <= 1.03 * (double)oh * w && (double)oh * w <= 1.03 * (double)ow * h;
        if (!known || !pb_images(ex->cb, ow, oh)) {
            if (D.pre_misses++ < 4)
                rt_log("dlaa: prebloom: the picture's size is not known yet (last %ux%u for a %ux%u scene): this frame "
                       "runs the upscaler after the tonemap", ow, oh, w, h);
            return 0;
        }
    }
    vk_exec_hook_turn();
    D.ended = D.done = 1;
    D.pre = native ? 1 : 2;
    const uint64_t evals = D.evals;
    if (native) {
        evaluate(ex, NULL);
    } else {
        D.up = 1;
        VkExecTarget dst = {.image = PB.scene, .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                            .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .layers = 1, .width = ow, .height = oh, .samples = 1};
        evaluate(ex, &dst);
    }
    if (D.evals == evals) {
        D.pre = 0, D.up = 0;
        return 1;
    }
    if (D.pre == 2) {
        vk_dlaa_swap_img = D.color.img, vk_dlaa_swap_view = PB.scene_view;
        VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .srcOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, D.color.level, D.color.base_layer, 1},
                         .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
        vkCmdBlitImage(ex->cb, PB.scene, VK_IMAGE_LAYOUT_GENERAL, D.color.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       VK_FILTER_LINEAR);
        full_barrier(ex->cb);
        ex->recorded += 2;
    }
    static int told;
    if (!told++)
        rt_log("dlaa: prebloom: %s runs before the bloom, at the start of its first pass (two %ux%u targets, format %d), "
               "on the light target %ux%u format %d with the depth %ux%u format %d; %s",
               use_fsr() ? "FSR 3.1" : "DLSS", ex->color[0].width, ex->color[0].height, (int)ex->color[0].format, w, h,
               (int)D.color.format, D.depth.width, D.depth.height, (int)D.depth.format,
               native ? "its picture goes back into the light target (no upscaling)"
                      : "its picture is the picture's size, where the lens flares and the bloom composite are drawn");
    return 1;
}

void vk_dlaa_retarget(VkExec *ex) {
    if (D.pre != 2 || !PB.scene || ex->pass_ncolor != 1) return;
    VkExecTarget *c = &ex->color[0];
    if (!c->image || c->image == PB.scene || c->image == PB.comp) return;
    if (c->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || c->samples > 1 || c->is3d || c->layers != 1) return;
    VkImage to = VK_NULL_HANDLE;
    int comp = 0;
    if (c->image == D.color.image) to = PB.scene;
    else if (c->width == D.color.width && c->height == D.color.height && ex->depth.image == D.depth.image &&
             (!D.comp_game || D.comp_game == c->image))
        to = PB.comp, comp = 1;
    if (!to) return;
    static int told[2];
    if (!told[comp]++)
        rt_log("dlaa: prebloom: %s (%ux%u format %d, %s) drawn at %ux%u", comp ? "the bloom composite" : "a pass into the light target",
               c->width, c->height, (int)c->format, ex->depth.image ? "with the scene depth" : "no depth", PB.w, PB.h);
    if (comp) D.comp_game = c->image, D.comp = 1;
    c->image = to, c->img = NULL;
    c->width = PB.w, c->height = PB.h;
    c->level = c->base_layer = 0, c->scale = 1, c->blocks = 0, c->view_type = VK_IMAGE_VIEW_TYPE_2D;
    memset(&ex->depth, 0, sizeof ex->depth);
    ex->pass_depth = ex->pass_stencil = VK_FORMAT_UNDEFINED;
    ex->pipe.depth_fmt = 0;
    ex->pass_w = PB.w, ex->pass_h = PB.h, ex->pass_scale = 1, ex->pass_layers = 1, ex->pass_noatt = 0;
}

int vk_dlaa_target_scale(const VkExec *ex, float *sx, float *sy) {
    if (D.pre != 2 || !PB.scene || !D.color.width || !D.color.height) return 0;
    VkImage t = ex->color[0].image;
    if (!t || (t != PB.scene && t != PB.comp)) return 0;
    *sx = (float)PB.w / (float)D.color.width, *sy = (float)PB.h / (float)D.color.height;
    return 1;
}

static void prebloom_upscaler(VkExec *ex, VkImage out, uint32_t w, uint32_t h) {
    VkCommandBuffer cb = ex->cb;
    if (D.pre == 2) {
        if (!D.comp) {
            static int told;
            if (told < 8)
                told++, rt_log("dlaa: prebloom: frame %llu drew no bloom composite after the upscaler: the game's "
                               "picture stays", (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED));
            return;
        }
        VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .srcOffsets = {{0, 0, 0}, {(int32_t)PB.w, (int32_t)PB.h, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
        vkCmdBlitImage(cb, PB.comp, VK_IMAGE_LAYOUT_GENERAL, out, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       PB.w == w && PB.h == h ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        full_barrier(cb);
        ex->recorded += 2;
        static int told;
        if (!told++) rt_log("dlaa: prebloom: the composite drawn at %ux%u goes into the game's upscaler's %ux%u output",
                            PB.w, PB.h, w, h);
    }
    if (vk_dlaa_hudless_wanted) hudless(cb, out, w, h), ex->recorded += 2;
}

void vk_dlaa_upscaler(VkExec *ex, VkImage out, VkFormat fmt, uint32_t w, uint32_t h) {
    static int on = -1;
    if (on < 0) on = rt_knob("DLAA_AT_UPSCALER", "1");
    if (!inputs_on() || !out || fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32) return;
    if (D.armed && !D.gbuf && w >= D.color.width && h >= D.color.height) D.pic_w = w, D.pic_h = h;
    if (D.pre) {
        prebloom_upscaler(ex, out, w, h);
        return;
    }
    if (!on || !D.armed || D.done || D.gbuf) return;
    uint32_t sw = D.color.width, sh = D.color.height;
    if (w < sw || h < sh || out == D.color.image) return;
    if ((double)w * sh > 1.03 * (double)h * sw || (double)h * sw > 1.03 * (double)w * sh) return;
    static int told;
    if (!told++)
        rt_log("dlaa: the upscaler runs where the game's own does: scene %ux%u into its %ux%u output", sw, sh, w, h);
    VkExecTarget dst = {.image = out, .format = fmt, .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .layers = 1, .width = w,
                        .height = h, .samples = 1};
    vk_exec_hook_turn();
    D.ended = D.done = D.up = 1;
    evaluate(ex, &dst);
}

#define RETIRE_PRESENTS 600
static uint64_t retire_off_at;
static uint64_t retired_n;

static int up_holds(void) {
    for (int i = 0; i < SNAPS; i++)
        if (R.s[i].before) return 1;
    if (D.failed)
        for (int i = 0; i < DLSS_SIZES; i++)
            if (D.sizes[i].out || D.sizes[i].feat) return 1;
    return vk_ffx_holds();
}

static void retire_idle(void) {
    uint64_t now = presents_now();
    if (!inputs_on()) upscalers_drain();
    if (!idle_free()) return;
    if (inputs_on()) {
        retire_off_at = 0;
        return;
    }
    if (!retire_off_at) retire_off_at = now ? now : 1;
    if (now < retire_off_at + RETIRE_PRESENTS || !up_holds()) return;
    for (int i = 0; i < SNAPS; i++) retire_snap(&R.s[i]);
    vk_ffx_release_idle();
    if (D.failed) {
        for (int i = 0; i < DLSS_SIZES; i++) retire_size(&D.sizes[i]);
        D.last = NULL, D.host = NULL, D.fg_fresh = 0, D.fg_mv_view = VK_NULL_HANDLE, D.fg_mv = (VkFfxImage){0};
    }
    vk_upscaler_let_go();
    retired_n++;
    rt_log("dlaa: the upscaler has not run for %d presents (%s): its mask snapshots and FSR contexts%s are freed (%llu "
           "time(s)); made again when it runs", RETIRE_PRESENTS, D.failed ? "it failed" : "switched off",
           D.failed ? " and DLSS sizes" : "", (unsigned long long)retired_n);
    retire_off_at = 0;
}

void vk_dlaa_pass(VkExec *ex) {
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    retire_idle();
    if (!inputs_on()) return;
    if (D.done) {
        if (!D.pre && ex->pass_ncolor == 1 && c->image && scene_color(c->format) && z->image && z->format == VK_FORMAT_D32_SFLOAT &&
            c->width == D.color.width && c->height == D.color.height && z->width == c->width)
            F.second++, F.second_img = c->image;
        return;
    }
    static int deferred;
    static uint32_t gb_w, gb_h;
    static uint32_t lit_w, lit_h;
    static int heals, test = -1;
    if (test < 0) {
        const char *t = cfg_get("NX2SARIA_DLAA_TEST_DEFERRED");
        test = t && (*t == '1' || *t == '2') ? *t - '0' : 0;
        if (test)
            deferred = 1, gb_w = 320, gb_h = 180,
            rt_log("dlaa: NX2SARIA_DLAA_TEST_DEFERRED=%d: starting in the deferred shape (a 320x180 G-buffer), %s", test,
                   test == 1 ? "which Ocarina's light target takes back" : "kept for the run as before");
    }
    unsigned o[2], s[2];
    res_sizes(o, s);
    int scene = z->image && z->format == VK_FORMAT_D32_SFLOAT && z->width >= DLAA_SCENE_MIN_W;
    if (ex->pass_ncolor >= 3 && scene && lit_w && (z->width < lit_w || z->height < lit_h) && !deferred) {
        static int told;
        if (!told++)
            rt_log("dlaa: a %d-target pass with a %ux%u depth is not a G-buffer: a %ux%u light target was armed before "
                   "(Ocarina's frame shape stays)", ex->pass_ncolor, z->width, z->height, lit_w, lit_h);
    }
    if (ex->pass_ncolor >= 3 && scene && !(D.armed && (z->width < D.color.width || z->height < D.color.height)) &&
        !(lit_w && (z->width < lit_w || z->height < lit_h))) {
        if (!deferred) {
            rt_log("dlaa: a G-buffer pass (%d colour targets, depth %ux%u, target 0 format %d): the deferred frame shape",
                   ex->pass_ncolor, z->width, z->height, c->image ? (int)c->format : 0);
            gb_w = z->width, gb_h = z->height;
        }
        D.gbuf = 1, D.armed = 0, deferred = 1;
        if (c->image && c->format == VK_FORMAT_R8G8B8A8_UNORM && c->width == z->width && c->height == z->height &&
            c->samples <= 1 && !c->is3d && (!D.ids_ok || c->width >= D.ids.width))
            D.ids = *c, D.ids_ok = 1;
        return;
    }
    if (deferred && test != 2 && ex->pass_ncolor == 1 && c->image && scene_color(c->format) && scene && c->width == z->width &&
        c->height == z->height && (c->width > gb_w || c->height > gb_h) && c->samples <= 1 && !c->is3d) {
        if (!heals++)
            rt_log("dlaa: the %ux%u pass taken for a G-buffer is smaller than the scene's %ux%u light target: Ocarina's frame "
                   "shape again", gb_w, gb_h, c->width, c->height);
        deferred = 0, D.gbuf = 0, gb_w = gb_h = 0;
    }
    if ((D.gbuf || !deferred) && ex->pass_ncolor == 1 && c->image && scene_color(c->format) && scene &&
        c->width == z->width && c->height == z->height &&
        (!D.armed || (c->width >= D.color.width && c->height >= D.color.height)) && c->samples <= 1 && !c->is3d) {
        int first = !D.armed;
        D.color = *c, D.depth = *z, D.armed = 1;
        if (!deferred && (c->width > lit_w || c->height > lit_h)) lit_w = c->width, lit_h = c->height;
        static int told;
        if (first && !told++)
            rt_log("dlaa: first scene armed: light target %ux%u format %d, depth %ux%u", c->width, c->height, (int)c->format,
                   z->width, z->height);
        if (first) mask_snapshot(ex);
        return;
    }
    if (!D.armed) return;
    if (!deferred) {
        if (D.pre_on && dl_active() && prebloom_start(ex) && prebloom_run(ex)) return;
        int one = ex->pass_ncolor == 1 && c->image && !z->image && c->image != D.color.image && c->samples <= 1 &&
                  !c->is3d;
        if (one) F.one_n++, F.one_w = c->width, F.one_h = c->height, F.one_fmt = c->format;
        double share = wide_dlss_share();
        int native = share <= 0 || share >= 1;
        int big = D.share_off || native || c->width >= 0.95 * D.color.width / share;
        int same = c->width == D.color.width && c->height == D.color.height;
        int shape = (double)c->width * D.color.height <= 1.03 * (double)c->height * D.color.width &&
                    (double)c->height * D.color.width <= 1.03 * (double)c->width * D.color.height;
        if (one && !shape && big && c->width > D.color.width && c->height > D.color.height && D.shape_logged < 8)
            D.shape_logged++, rt_log("dlaa: a %ux%u pass is not the output pass: its shape is not the scene's %ux%u",
                                     c->width, c->height, D.color.width, D.color.height);
        if (one && shape && big && c->width > D.color.width && c->height > D.color.height) {
            vk_exec_hook_turn();
            D.ended = D.done = D.up = 1;
            evaluate(ex, c);
        } else if (one && same && native && scene_color(c->format)) {
            static int told;
            if (!told++)
                rt_log("dlaa: the pass the HUD is drawn into is at the scene's size (%ux%u, format %d): DLSS writes there",
                       c->width, c->height, (int)c->format);
            vk_exec_hook_turn();
            D.ended = D.done = D.up = 1;
            evaluate(ex, c);
        } else if (one && same && output_target(c->format)) {
            vk_exec_hook_turn();
            D.ended = D.done = 1;
            evaluate(ex, NULL);
        }
        return;
    }
    if (!D.ended) {
        if (!ex->pass_w || ex->pass_w * 4 > D.color.width) return;
        vk_exec_hook_turn();
        D.ended = 1;
        if (D.color.width >= o[0]) D.done = 1, evaluate(ex, NULL);
        return;
    }
    if (ex->pass_ncolor == 1 && c->image && !z->image && scene_color(c->format) && c->width > D.color.width &&
        c->width == o[0] && c->height == o[1] && c->samples <= 1 && !c->is3d) {
        vk_exec_hook_turn();
        D.done = D.up = 1;
        evaluate(ex, c);
    }
}

#define JITTER_LOD_BIAS (-1.0f)
static void dlss_mode(void) {
    static char mode[32];
    static int bias_on = -1;
    const char *m = dynres_override();
    if (!m) m = cfg_value("NX2SARIA_DLSS");
    if (strcmp(m, mode)) {
        snprintf(mode, sizeof mode, "%s", m);
        res_dlss_mode(m);
        bias_on = -1;
    }
    int b = dl_active();
    if (b != bias_on) {
        static float test;
        static int test_on = -1;
        if (test_on < 0) {
            const char *s = cfg_get("NX2SARIA_DLSS_LOD_BIAS_TEST");
            test_on = s && *s;
            if (test_on) test = (float)atof(s), rt_log("dlaa: texture LOD bias %g while an upscaler runs "
                                                       "(NX2SARIA_DLSS_LOD_BIAS_TEST, a test)", test);
        }
        float rule = res_dlss_lod_bias() + (jitter_on() ? JITTER_LOD_BIAS : 0.0f);
        bias_on = b, vk_sampler_dlss_bias(b ? (test_on ? test : rule) : 0.0f);
    }
}


static void preset_check(void) {
    unsigned v = cfg_version();
    if (v == D.cfg_seen) return;
    D.cfg_seen = v;
    int p = preset_value();
    if (p == D.preset) return;
    if (D.evals) {
        if (p) rt_log("dlaa: DLSS preset %c in every mode; the features are created again", 'A' + p - 1);
        else rt_log("dlaa: DLSS presets per mode; the features are created again");
    }
    D.preset = p, D.preset_dirty = 1, D.reset = 1;
}

static int setting_presents;
#define SETTING_PRESENTS 60

int vk_dlaa_resized_take(void) {
    if (!__atomic_exchange_n(&size_changed, 0, __ATOMIC_ACQ_REL)) return 0;
    return __atomic_exchange_n(&setting_presents, 0, __ATOMIC_ACQ_REL) != 0;
}

#define IDLE_FREE_PRESENTS 600
static uint64_t off_at;
static void idle_release(void) {
    if (!idle_free()) return;
    if (D.hl && D.presents - D.hl_at >= IDLE_FREE_PRESENTS && !D.hl_fresh) {
        rt_log("dlaa: the HUD-less picture for frame generation went unused for %d presents: freed", IDLE_FREE_PRESENTS);
        hl_destroy();
    }
    if (PB.scene && D.presents - PB.used >= IDLE_FREE_PRESENTS) {
        rt_log("dlaa: prebloom: the picture-size images went unused for %d presents: freed", IDLE_FREE_PRESENTS);
        pb_destroy();
    }
    if (D.on || D.presents - off_at < IDLE_FREE_PRESENTS) return;
    int n = 0;
    for (int i = 0; i < DLSS_SIZES; i++) {
        struct Size *s = &D.sizes[i];
        if (D.fgo && (s == D.last || s->used + IDLE_FREE_PRESENTS > (uint64_t)D.evals)) {
            if (s->feat) {
                rt_log("dlaa: the DLSS feature at %ux%u went unused for %d presents with the upscaler off: released; "
                       "its size's motion images stay for frame generation", s->w, s->h, IDLE_FREE_PRESENTS);
                feat_release(s->feat, NGX_AT_FRAME), s->feat = NULL, feat_released = 1;
            }
            if (D.host == s) D.host = NULL;
            continue;
        }
        if (s->out || s->feat) {
            rt_log("dlaa: the DLSS size %ux%u -> %ux%u went unused for %d presents with the upscaler off: freed",
                   s->w, s->h, s->ow, s->oh, IDLE_FREE_PRESENTS);
            destroy_size(s, NGX_AT_FRAME), n++;
        }
    }
    if (n && !D.fgo)
        D.last = NULL, D.host = NULL, D.fg_fresh = 0, D.fg_mv_view = VK_NULL_HANDLE, D.fg_mv = (VkFfxImage){0};
}

static int fg_runs(void) { return vk_dlssg_active() || vk_fg_active() || vk_ngxfg_on() || vk_fsr3fg_on(); }

static int fg_off_why(const char **sl_why) {
    const char *fg = cfg_value("NX2SARIA_FRAMEGEN");
    *sl_why = NULL;
    if (!strcmp(fg, "dlss")) {
        const int fsr = vk_fsr3fg_build() ? vk_fsr3fg_available() : vk_ffx_framegen_available();
#ifdef _WIN32
        const int later = !vk_dlssg_wanted();
#else
        const int later = !vk_ngxfg_wanted();
#endif
        if (later) return vk_dlssg_gpu_may() ? 9 : fsr ? 7 : 8;
        if (vk_dlssg_why(sl_why)) return fsr ? 1 : 2;
        return *sl_why ? 3 : 4;
    }
    if (!strcmp(fg, "fsr3")) return (vk_fsr3fg_build() ? vk_fsr3fg_available() : vk_ffx_framegen_available()) ? 6 : 5;
    return 0;
}

static void fg_state_line(int on, int fgo) {
    static int last = -1;
    const char *sl_why = NULL;
    const int st = !fg_runs() ? 0 : fgo ? 2 : on && !D.failed ? 1 : 3;
    const int key = st ? st : 8 + fg_off_why(&sl_why);
    if (key == last) return;
    last = key;
    if (st == 0) {
        static const char *pre = "dlaa: frame generation does not run: no inputs are made for it (",
                          *dlss = "it is set to NVIDIA DLSS Frame Generation",
                          *none = "this GPU has none (RTX 40 or newer)",
#ifdef _WIN32
                          *next = "";
#else
                          *next = ", with DLSS upscaling on when it starts";
#endif
        switch (key - 8) {
        case 0: rt_log("%sthe setting is off)", pre); break;
        case 1:
            rt_log("%s%s, and %s; framegen = fsr3 works here, or Frame generation: AMD FSR 3.1 in the menu)", pre, dlss,
                   none);
            break;
        case 2: rt_log("%s%s, and %s)", pre, dlss, none); break;
        case 3:
            rt_log("%s%s, which Streamline says does not run here: %s; the dlssg: lines say more)", pre, dlss, sl_why);
            break;
        case 4: rt_log("%s%s, which has not started; the dlssg: and ngxfg: lines say why)", pre, dlss); break;
        case 5:
            rt_log("%sit is set to AMD FSR 3.1, which does not run here; the fg: and fsr3fg: lines say why)", pre);
            break;
        case 7:
        case 8:
            rt_log("%s%s, and %s, in this run or the next%s)", pre, dlss, none,
                   key - 8 == 7 ? "; framegen = fsr3 works here, or Frame generation: AMD FSR 3.1 in the menu" : "");
            break;
        case 9: rt_log("%s%s, which starts only at the next start of the game%s)", pre, dlss, next); break;
        default: rt_log("%sit is set to AMD FSR 3.1, which has not started yet)", pre);
        }
    }
    else if (st == 1) rt_log("dlaa: frame generation runs with its inputs from the upscaler (%s)", use_fsr() ? "FSR" : "DLSS");
    else if (st == 2)
        rt_log("dlaa: frame generation runs without an upscaler: the scene's depth, motion vectors and camera, no "
               "jitter, and the picture without the HUD from the game's own scaling");
    else rt_log("dlaa: frame generation runs, but the upscaler or its inputs failed earlier in this run: none are made");
}

static int blit_from(VkFormat f) {
    static VkFormat known[8];
    static int ok[8], n;
    for (int i = 0; i < n; i++)
        if (known[i] == f) return ok[i];
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, f, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    int r = (fp.optimalTilingFeatures & need) == need;
    if (!r) rt_log("dlaa: the picture's format %d cannot be copied for frame generation: no picture without the HUD", (int)f);
    if (n < 8) known[n] = f, ok[n++] = r;
    return r;
}

static void fg_only_frame(VkExec *ex, const VkExecTarget *dst) {
    const VkExecTarget *t = &D.color, *z = &D.depth;
    const uint32_t w = t->width, h = t->height;
    D.up = 0;
    D.stepped = 0;
    vk_exec_barrier(ex);
    VkCommandBuffer cb = ex->cb;
    struct Size *e = size_for(cb, w, h, w, h);
    if (!e) {
        rt_log("dlaa: the motion images for frame generation could not be made: none for the rest of this run");
        D.failed = 1;
        return;
    }
    if (e != D.last) D.reset = 1, D.why[0]++, e->chars_ok = e->dist_ok = 0;
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    static int no_mv = -1;
    if (no_mv < 0) no_mv = !rt_knob("DLAA_MV", "1");
    int masked = 0, mv = no_mv ? 0 : mv_pass(ex, e, zv, 0.0f, 0.0f, 0.0f, &masked);
    if (mv != 1) {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        full_barrier(cb);
        if (!no_mv && mv == 0) D.reset = 1, D.why[1]++;
    }
    if (D.last_color != t->image) target_changed(t, mv == 1);
    if (mv == 1) fcam_probe(cb, e, w, h);
    const VkExecTarget *pic = dst ? dst : t;
    int hl = vk_dlaa_hudless_wanted && (!dst || !D.gbuf) && blit_from(pic->format);
    if (hl) hudless_from(cb, pic->image, pic->level, pic->base_layer, pic->width, pic->height);
    D.fg_reset = D.reset;
    D.reset = 0;
    D.last = e, e->used = D.evals + 1, D.evals++;
    ex->recorded += 2;
    vk_exec_draw_new_cb();
    D.jdepth = z->image, D.jw = z->width, D.jh = z->height;
    D.fg_depth = (VkFfxImage){z->image, z->format, w, h, 0};
    D.fg_mv = (VkFfxImage){e->mv, VK_FORMAT_R16G16_SFLOAT, w, h, 1};
    D.fg_depth_view = zv, D.fg_mv_view = e->mv_view;
    D.fg_jx = D.fg_jy = 0.0f, D.fg_at = D.presents, D.fg_fresh = 1;
    D.jit_ok = 0;
    static uint64_t frames, still;
    frames++, still += mv != 1;
    if (frames == 1 || frames == 300 || frames % 3600 == 0)
        rt_log("dlaa: frame generation without an upscaler: %llu frames prepared (scene %ux%u, picture %ux%u%s), %llu "
               "without camera motion", (unsigned long long)frames, w, h, pic->width, pic->height,
               hl ? "" : ", no picture without the HUD", (unsigned long long)still);
}

void vk_dlaa_frame(void) {
    static unsigned cfg_seen;
    static int cfg_known;
    unsigned v = cfg_version();
    __atomic_store_n(&D.presents, D.presents + 1, __ATOMIC_RELAXED);
    if (cfg_known && v != cfg_seen) {
        __atomic_store_n(&setting_presents, SETTING_PRESENTS, __ATOMIC_RELAXED);
    } else {
        int s = __atomic_load_n(&setting_presents, __ATOMIC_RELAXED);
        while (s > 0 &&
               !__atomic_compare_exchange_n(&setting_presents, &s, s - 1, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    }
    cfg_seen = v, cfg_known = 1;
    vk_dlaa_swap_img = NULL, vk_dlaa_swap_view = VK_NULL_HANDLE;
    D.pre = D.comp = 0, D.comp_game = VK_NULL_HANDLE;
    int pre_on = cfg_bool("NX2SARIA_DLSS_PREBLOOM");
    if (pre_on != D.pre_on) rt_log("dlaa: prebloom %s", pre_on ? "on: the upscaler runs before the bloom where the frame has one"
                                                               : "off: the upscaler runs at the game's upscaler");
    D.pre_on = pre_on;
    dlss_mode();
    preset_check();
    if (!N.ngx_up) {
        fsr_frame = fsr3();
        N.available = fsr_frame && cfg_bool("NX2SARIA_DLAA") && vk_ffx_load();
    }
    int on = N.available && cfg_bool("NX2SARIA_DLAA");
    int fgo = !on && !D.failed && fg_runs();
    fg_state_line(on, fgo);
    if (fgo != D.fgo) {
        D.fgo = fgo;
        off_at = D.presents;
        D.reset = 1, D.cam_prev_ok = 0, D.jit_ok = D.jit_used = D.armed = D.done = D.gbuf = D.ended = D.up = 0;
        D.hold = 0;
        D.fg_fresh = D.hl_fresh = 0;
        memset(&F, 0, sizeof F);
        if (on == D.on) return;
    }
    if (on != D.on) {
        rt_log("dlaa: %s", on ? "on" : "off");
        D.on = on;
        off_at = D.presents;
        D.reset = 1, D.cam_prev_ok = 0, D.jit_ok = D.jit_used = D.armed = D.done = D.gbuf = D.ended = D.up = 0;
        D.hold = 0;
        D.fg_fresh = D.hl_fresh = 0;
        memset(&F, 0, sizeof F);
        return;
    }
    idle_release();
    __atomic_store_n(&D.scene_last, D.armed || D.done || F.zdraws || D.failed ? 1 : 0, __ATOMIC_RELAXED);
    if (!inputs_on()) return;
    if (D.share_off) D.share_off--;
    if (!D.done && D.armed) D.share_off = 120;
    if (!D.done) {
        D.frames_missed++;
        if (D.evals && D.miss_logged < 24) {
            D.miss_logged++;
            char arm[64] = "never armed";
            if (D.armed) snprintf(arm, sizeof arm, "armed on a %ux%u light target", D.color.width, D.color.height);
            rt_log("dlaa: frame %llu without the DLAA point: %s; %u scene-depth draws (%u into another D32 depth); %u "
                   "later passes with one colour target and no depth (last %ux%u format %d); %llu submissions since the "
                   "last present, %d held submissions waiting for a signal",
                   (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), arm, F.zdraws, F.zother,
                   F.one_n, F.one_w, F.one_h, (int)F.one_fmt, (unsigned long long)(vk_exec_submit_seq - D.seq_present),
                   vk_exec_held());
        }
    }
    if (D.done && F.second) {
        D.seconds++;
        if (D.second_logged < 24)
            D.second_logged++, rt_log("dlaa: frame %llu: a second scene after DLSS ran (%u passes into light target %p; "
                                      "DLSS ran on %p); %llu submissions since the last present, %d held",
                                      (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED),
                                      F.second, (void *)F.second_img, (void *)D.color.image,
                                      (unsigned long long)(vk_exec_submit_seq - D.seq_present), vk_exec_held());
    }
    D.seq_present = vk_exec_submit_seq;
    if (D.jit_used && !D.done) D.hold = 60, D.holds++;
    else if (D.hold) D.hold--;
    if (!D.done) D.jit_ok = 0;
    D.armed = D.done = D.jit_used = D.gbuf = D.ended = D.up = D.ids_ok = 0;
    D.cam_addr = 0, D.nviews = 0;
    memset(&F, 0, sizeof F);
}

static const uint64_t FXAA_FS[] = {0xf450003ebeb3dc52ull, 0x0a12f4bc2a9a4fbeull, 0x5c43ae18948d52cbull,
                                   0xaad42238801d31d8ull};

static uint64_t spirv_hash(const GfxShader *s) {
    static struct { const GfxShader *s; uint64_t h; } tab[8192];
    uintptr_t k = ((uintptr_t)s >> 4) * 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0, j = (uint32_t)(k >> 51); i < 8192; i++, j = (j + 1) & 8191) {
        if (tab[j].s == s) return tab[j].h;
        if (!tab[j].s) {
            uint64_t h = gfx_shader_spirv_fnv(s);
            tab[j].s = s, tab[j].h = h;
            return h;
        }
    }
    return 0;
}

int vk_dlaa_skip_draw(VkExec *ex, const GfxShader *fs) {
    static uint64_t frame_read = ~0ull;
    static int fxaa = 1;
    static uint64_t fx_off_frames;
    static int fx_found, fx_reported;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame_read) {
        frame_read = f, fxaa = cfg_bool("NX2SARIA_FXAA");
        if (!fxaa && !fx_found && !fx_reported && ++fx_off_frames == 3600) {
            fx_reported = 1;
            rt_log("dlaa: fxaa = 0 found no draw of the game's FXAA in %llu frames (its fragment shader is not in "
                   "FXAA_FS): the FXAA stays on", (unsigned long long)fx_off_frames);
        }
    }
    if (fxaa && !D.up) return 0;
    if (!fs) return 0;
    uint64_t h = spirv_hash(fs);
    int known = 0;
    for (size_t i = 0; i < sizeof FXAA_FS / sizeof FXAA_FS[0]; i++) known |= h == FXAA_FS[i];
    if (!known) return 0;
    if (!ex->depth.image) {
        static int told;
        if (D.up && !told++) rt_log("dlaa: the game's scaling draw left out, DLSS wrote it (fragment shader %016llx)",
                                    (unsigned long long)h);
        return D.up;
    }
    if (!fxaa && !fx_found) {
        fx_found = 1;
        rt_log("dlaa: fxaa = 0 skips the game's FXAA (fragment shader %016llx)", (unsigned long long)h);
    }
    return !fxaa;
}


int vk_ngxfg_max(void) { return vk_ngxfg_wanted() && N.available && !FG.failed ? FG.max : 0; }
int vk_ngxfg_on(void) { return vk_ngxfg_max() && !strcmp(cfg_value("NX2SARIA_FRAMEGEN"), "dlss"); }
void vk_ngxfg_reset(void) { FG.reset = 1; }

int vk_ngxfg_evaluate(VkCommandBuffer cb, const VkNgxImage *bb, const VkNgxImage *hudless, const VkNgxImage *out, int count,
                      const VkNgxImage *real) {
    if (!vk_ngxfg_max() || count < 1) return 0;
    if (count > FG.max) count = FG.max;
    VkFfxImage d, m;
    VkImageView dv, mvv;
    VkDlaaCamera c;
    float jx = 0, jy = 0;
    int reset = 0;
    if (!vk_dlaa_fg_inputs(&d, &m, &jx, &jy, &reset) || !vk_dlaa_fg_views(&dv, &mvv) || !vk_dlaa_fg_camera(&c)) {
        FG.reset = 1;
        return 0;
    }
    if (!FG.p && !NGX_OK(NGX_CALL(&NL, NGX_AT_PRESENT, N.alloc_params(&FG.p)))) {
        FG.failed = 1;
        rt_log("ngxfg: parameter allocation failed: frame generation off");
        return 0;
    }
    NgxParam *p = FG.p;
    if (FG.feat && (FG.w != bb->w || FG.h != bb->h || FG.fmt != bb->fmt)) {
        NGX_CALL(&NL, NGX_AT_PRESENT, N.release(FG.feat));
        FG.feat = NULL;
    }
    if (!FG.feat) {
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", bb->w);
        p_setu(p, "Height", bb->h);
        p_setu(p, "DLSSG.BackbufferFormat", (unsigned)bb->fmt);
        uint64_t t0 = perf_mark_begin();
        NgxResult r = NGX_CALL(&NL, NGX_AT_PRESENT, N.create(cb, NGX_FEATURE_FRAME_GENERATION, p, &FG.feat));
        perf_mark_end(PERF_M_NGX, t0, 0);
        if (!NGX_OK(r) || !FG.feat) {
            FG.failed = 1, FG.feat = NULL;
            rt_log("ngxfg: DLSS Frame Generation feature creation failed (%08x): frame generation off", (unsigned)r);
            return 0;
        }
        FG.w = bb->w, FG.h = bb->h, FG.fmt = bb->fmt, FG.reset = 1;
        rt_log("ngxfg: DLSS Frame Generation created at %ux%u (format %d), depth and motion %ux%u", bb->w, bb->h,
               (int)bb->fmt, d.width, d.height);
    }
    NgxResourceVK rb = res_image(bb->view, bb->img, bb->fmt, bb->w, bb->h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK rz = res_image(dv, d.image, d.format, d.width, d.height, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
    NgxResourceVK rm = res_image(mvv, m.image, m.format, m.width, m.height, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK rh = hudless ? res_image(hudless->view, hudless->img, hudless->fmt, hudless->w, hudless->h,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 0) : rb;
    NgxResourceVK rr = res_image(real->view, real->img, real->fmt, real->w, real->h, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    p_setp(p, "DLSSG.Backbuffer", &rb);
    p_setp(p, "DLSSG.Depth", &rz);
    p_setp(p, "DLSSG.MVecs", &rm);
    p_setp(p, "DLSSG.HUDLess", hudless ? &rh : NULL);
    p_setp(p, "DLSSG.UI", NULL);
    p_setp(p, "DLSSG.UIAlpha", NULL);
    p_setp(p, "DLSSG.BidirectionalDistortionField", NULL);
    p_setp(p, "DLSSG.OutputReal", &rr);
    p_setp(p, "DLSSG.OutputDisableInterpolation", NULL);
    p_setp(p, "DLSSG.CameraViewToClip", c.view_to_clip);
    p_setp(p, "DLSSG.ClipToCameraView", c.clip_to_view);
    p_setp(p, "DLSSG.ClipToLensClip", NULL);
    p_setp(p, "DLSSG.ClipToPrevClip", c.clip_to_prev_clip);
    p_setp(p, "DLSSG.PrevClipToClip", c.prev_clip_to_clip);
    p_setf(p, "DLSSG.JitterOffsetX", jx);
    p_setf(p, "DLSSG.JitterOffsetY", jy);
    p_setf(p, "DLSSG.MvecScaleX", 1.0f / (float)m.width);
    p_setf(p, "DLSSG.MvecScaleY", 1.0f / (float)m.height);
    p_setf(p, "DLSSG.CameraPinholeOffsetX", 0.0f);
    p_setf(p, "DLSSG.CameraPinholeOffsetY", 0.0f);
    static const char *const pos[] = {"DLSSG.CameraPosX", "DLSSG.CameraPosY", "DLSSG.CameraPosZ"};
    static const char *const up[] = {"DLSSG.CameraUpX", "DLSSG.CameraUpY", "DLSSG.CameraUpZ"};
    static const char *const rt[] = {"DLSSG.CameraRightX", "DLSSG.CameraRightY", "DLSSG.CameraRightZ"};
    static const char *const fw[] = {"DLSSG.CameraFwdX", "DLSSG.CameraFwdY", "DLSSG.CameraFwdZ"};
    for (int i = 0; i < 3; i++)
        p_setf(p, pos[i], c.pos[i]), p_setf(p, up[i], c.up[i]), p_setf(p, rt[i], c.right[i]), p_setf(p, fw[i], c.fwd[i]);
    p_setf(p, "DLSSG.CameraNear", c.znear);
    p_setf(p, "DLSSG.CameraFar", c.zfar);
    p_setf(p, "DLSSG.CameraFOV", c.fov_y);
    p_setf(p, "DLSSG.CameraAspectRatio", c.aspect);
    p_setu(p, "DLSSG.ColorBuffersHDR", 0);
    p_setu(p, "DLSSG.DepthInverted", (unsigned)c.inverted);
    p_setu(p, "DLSSG.CameraMotionIncluded", 1);
    p_setu(p, "DLSSG.Reset", (unsigned)(reset || FG.reset));
    p_setu(p, "DLSSG.AutomodeOverrideReset", 0);
    p_setu(p, "DLSSG.NotRenderingGameFrames", 0);
    p_setu(p, "DLSSG.OrthoProjection", 0);
    p_setu(p, "DLSSG.MvecDilated", 0);
    p_setu(p, "DLSSG.MenuDetectionEnabled", 0);
    struct { const char *base; uint32_t w, h; } rects[] = {
        {"DLSSG.MVecsSubrect", m.width, m.height}, {"DLSSG.DepthSubrect", d.width, d.height},
        {"DLSSG.HUDLessSubrect", bb->w, bb->h}, {"DLSSG.InputBackbufferSubrect", bb->w, bb->h},
        {"DLSSG.OutputInterpolatedSubrect", bb->w, bb->h}, {"DLSSG.OutputRealSubrect", bb->w, bb->h}};
    for (size_t i = 0; i < sizeof rects / sizeof *rects; i++) {
        char k[64];
        snprintf(k, sizeof k, "%sBaseX", rects[i].base), p_setu(p, k, 0);
        snprintf(k, sizeof k, "%sBaseY", rects[i].base), p_setu(p, k, 0);
        snprintf(k, sizeof k, "%sWidth", rects[i].base), p_setu(p, k, rects[i].w);
        snprintf(k, sizeof k, "%sHeight", rects[i].base), p_setu(p, k, rects[i].h);
    }
    p_setu(p, "DLSSG.MultiFrameCount", (unsigned)count);
    int made = 0;
    for (int i = 0; i < count; i++) {
        NgxResourceVK ro = res_image(out[i].view, out[i].img, out[i].fmt, out[i].w, out[i].h, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        p_setp(p, "DLSSG.OutputInterpolated", &ro);
        p_setu(p, "DLSSG.MultiFrameIndex", (unsigned)(i + 1));
        NgxResult r = NGX_CALL(&NL, NGX_AT_PRESENT, N.evaluate(cb, FG.feat, p, NULL));
        if (!NGX_OK(r)) {
            static int told;
            if (told++ < 5) rt_log("ngxfg: evaluation %d of %d failed (%08x)", i + 1, count, (unsigned)r);
            break;
        }
        made++;
    }
    full_barrier(cb);
    FG.reset = 0;
    if (made && (++FG.evals == 1 || FG.evals % 3600 == 0))
        rt_log("ngxfg: %d frames with generation (%d generated per rendered frame)", FG.evals, made), ngx_lock_log();
    return made;
}
