#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define RDOC_API_1_4_1 10401
#define RDOC_MAX_FRAMES 64

typedef struct {
    void *GetAPIVersion;
    uint32_t (*SetCaptureOptionU32)(int opt, uint32_t val);
    void *fn2_10[9];
    void (*SetCaptureFilePathTemplate)(const char *);
    void *GetCaptureFilePathTemplate;
    uint32_t (*GetNumCaptures)(void);
    uint32_t (*GetCapture)(uint32_t idx, char *filename, uint32_t *pathlength, uint64_t *timestamp);
    void *TriggerCapture, *IsTargetControlConnected, *LaunchReplayUI, *SetActiveWindow;
    void (*StartFrameCapture)(void *device, void *window);
    uint32_t (*IsFrameCapturing)(void);
    uint32_t (*EndFrameCapture)(void *device, void *window);
} RdocApi;

static RdocApi *api;
static uint64_t targets[RDOC_MAX_FRAMES];
static int n_targets;
static int capturing;

int rdoc_active(void) { return api != NULL; }

void rdoc_init(void) {
    const char *e = cfg_get("NX2SARIA_RDOC");
    if (!e || !*e) return;
    for (const char *p = e; *p && n_targets < RDOC_MAX_FRAMES;) {
        char *end;
        unsigned long long v = strtoull(p, &end, 10);
        if (end == p) break;
        targets[n_targets++] = v;
        p = *end == ',' ? end + 1 : end;
        if (*end != ',') break;
    }
    if (!n_targets) { rt_log("rdoc: NX2SARIA_RDOC=%s has no frame numbers", e); return; }
    const char *dll = cfg_get("NX2SARIA_RDOC_DLL");
#ifdef _WIN32
    const char *def = "C:/Program Files/RenderDoc/renderdoc.dll";
#else
    const char *def = "librenderdoc.so";
#endif
    void *m = plat_lib_open(dll && *dll ? dll : def);
    if (!m) { rt_log("rdoc: cannot load %s; set NX2SARIA_RDOC_DLL", dll && *dll ? dll : def); return; }
    int (*get_api)(int, void **) = (int (*)(int, void **))plat_lib_sym(m, "RENDERDOC_GetAPI");
    void *a = NULL;
    if (!get_api || get_api(RDOC_API_1_4_1, &a) != 1 || !a) { rt_log("rdoc: RENDERDOC_GetAPI(1.4.1) failed"); return; }
    api = a;
    rt_log("rdoc: AllowUnsupportedVendorExtensions -> %u", api->SetCaptureOptionU32(12, 1));
    api->SetCaptureOptionU32(8, 1);
    api->SetCaptureOptionU32(9, 1);
    const char *dir = cfg_get("NX2SARIA_RDOC_DIR");
    char full[PLAT_PATH_MAX], tmpl[PLAT_PATH_MAX + 16];
    if (!plat_full_path(dir && *dir ? dir : "build/captures", full, sizeof full)) return;
    plat_file_mkdir_p(full);
    snprintf(tmpl, sizeof tmpl, "%s/nx2navi", full);
    api->SetCaptureFilePathTemplate(tmpl);
    rt_log("rdoc: ready, %d frame(s), captures %s", n_targets, full);
}

void rdoc_after_present(uint64_t frame) {
    if (!api) return;
    if (capturing) {
        api->EndFrameCapture(NULL, NULL);
        capturing = 0;
        char path[PLAT_PATH_MAX];
        uint32_t len = sizeof path, n = api->GetNumCaptures();
        if (n && api->GetCapture(n - 1, path, &len, NULL)) rt_log("rdoc: frame %llu captured: %s", (unsigned long long)frame, path);
        else rt_log("rdoc: frame %llu capture failed", (unsigned long long)frame);
    }
    for (int i = 0; i < n_targets; i++)
        if (targets[i] == frame + 1) { api->StartFrameCapture(NULL, NULL); capturing = 1; rt_log("rdoc: capture of frame %llu started", (unsigned long long)(frame + 1)); }
}
