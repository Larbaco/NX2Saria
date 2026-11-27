#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

#include "vk_lowlat.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FRAMES 4200
#define FRAME_NS 16666667ull
#define LINES 64
#define NO_TIMINGS "frame timings are not asked for"
#define NO_REPORT "no report from the driver"
#define REPORT "the driver's report over 16 frames: pad read to GPU done 8.00 ms"
#define SUBMIT_FOLLOWED "while it is on, queue submissions carry the frame's number, and this revision follows it"
#define SUBMIT_IGNORED \
    "while it is on, queue submissions carry the frame's number, which this revision ignores (revision 3 on follows it)"

typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkLatencyTimingsFrameReportNV *pTimings;
} InfoRev1;
_Static_assert(offsetof(InfoRev1, pTimings) == 0x10, "revision 1 keeps pTimings where the crash read");
_Static_assert(offsetof(VkGetLatencyMarkerInfoNV, timingCount) == 0x10, "revision 2 has the count there");
_Static_assert(offsetof(VkGetLatencyMarkerInfoNV, pTimings) == 0x18, "and pTimings after it");

static char lines[LINES][512];
static int n_lines;
static uint64_t now_ns = 1000000000ull;
static int modes, mode_on, sleeps, markers, rev1_calls, rev2_calls;
static uintptr_t rev1_fourth;
static const VkSwapchainKHR SC = (VkSwapchainKHR)(uintptr_t)0x5c;
static const VkDevice DEV = (VkDevice)(uintptr_t)0xd0;

unsigned rt_trace;
GfxStats gfx_counters;
volatile int32_t vk_lost;
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (n_lines < LINES) vsnprintf(lines[n_lines++], sizeof lines[0], fmt, ap);
    va_end(ap);
}
static int ll_level = 1;
static unsigned cfg_ver = 1;
int cfg_int(const char *key) { return !strcmp(key, "NX2SARIA_LOW_LATENCY") ? ll_level : 0; }
unsigned cfg_version(void) { return cfg_ver; }
uint64_t kern_now_ns(void) { return now_ns; }
int kern_core_away(void) { return 0; }
void kern_core_back(int *away) { (void)away; }
uint64_t perf_begin(void) { return 0; }
void perf_end_any(int kind, uint64_t t0) { (void)kind, (void)t0; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void plat_mutex_lock_shared(PlatMutex *m) { (void)m; }
void plat_mutex_unlock_shared(PlatMutex *m) { (void)m; }
void vk_device_lost_check(VkResult r, const char *where) { (void)r, (void)where; }
PFN_vkGetDeviceProcAddr vk_dlssg_dpa(void) { return NULL; }
int vk_fg_active(void) { return 0; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a,
                                                 VkSemaphore *out) {
    (void)d, (void)ci, (void)a;
    *out = (VkSemaphore)(uintptr_t)0x5e;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a) { (void)d, (void)s, (void)a; }
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout) {
    (void)d, (void)wi, (void)timeout;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL drv_set_mode(VkDevice d, VkSwapchainKHR sc, const VkLatencySleepModeInfoNV *mi) {
    CHECK(d == DEV && sc == SC && mi->sType == VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV, "the sleep mode's arguments");
    modes++, mode_on = mi->lowLatencyMode;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL drv_sleep(VkDevice d, VkSwapchainKHR sc, const VkLatencySleepInfoNV *si) {
    CHECK(d == DEV && sc == SC && si->sType == VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV, "the sleep's arguments");
    sleeps++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL drv_marker(VkDevice d, VkSwapchainKHR sc, const VkSetLatencyMarkerInfoNV *mi) {
    CHECK(d == DEV && sc == SC && mi->sType == VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV, "the marker's arguments");
    markers++;
}
static VKAPI_ATTR void VKAPI_CALL drv_timings_rev1(VkDevice d, VkSwapchainKHR sc, uint32_t *count, InfoRev1 *info) {
    (void)d, (void)sc, (void)count;
    rev1_calls++;
    rev1_fourth = (uintptr_t)info;
}
static VKAPI_ATTR void VKAPI_CALL drv_timings_rev2(VkDevice d, VkSwapchainKHR sc, VkGetLatencyMarkerInfoNV *info) {
    rev2_calls++;
    CHECK(d == DEV && sc == SC && info->sType == VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV && info->pTimings,
          "the timings' arguments");
    CHECK(info->timingCount == 16, "the timings' count is %u", info->timingCount);
    for (uint32_t i = 0; i < info->timingCount; i++) {
        VkLatencyTimingsFrameReportNV *t = &info->pTimings[i];
        CHECK(t->sType == VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV, "report %u has no type", i);
        uint64_t t0 = 1000000ull * (i + 1);
        t->presentID = i + 1;
        t->inputSampleTimeUs = t->simStartTimeUs = t0, t->simEndTimeUs = t0 + 2000;
        t->renderSubmitStartTimeUs = t0 + 2000, t->renderSubmitEndTimeUs = t0 + 3000;
        t->presentStartTimeUs = t0 + 3000, t->presentEndTimeUs = t0 + 3500;
        t->driverStartTimeUs = t0 + 3500, t->driverEndTimeUs = t0 + 4000;
        t->osRenderQueueStartTimeUs = t0 + 4000, t->osRenderQueueEndTimeUs = t0 + 5000;
        t->gpuRenderStartTimeUs = t0 + 5000, t->gpuRenderEndTimeUs = t0 + 8000;
    }
}

static uint32_t driver_rev;
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_dpa(VkDevice d, const char *name) {
    (void)d;
    if (!strcmp(name, "vkSetLatencySleepModeNV")) return (PFN_vkVoidFunction)drv_set_mode;
    if (!strcmp(name, "vkLatencySleepNV")) return (PFN_vkVoidFunction)drv_sleep;
    if (!strcmp(name, "vkSetLatencyMarkerNV")) return (PFN_vkVoidFunction)drv_marker;
    if (!strcmp(name, "vkGetLatencyTimingsNV"))
        return driver_rev < 2 ? (PFN_vkVoidFunction)drv_timings_rev1 : (PFN_vkVoidFunction)drv_timings_rev2;
    return NULL;
}
PFN_vkGetDeviceProcAddr vk_dpa = fake_dpa;

static int count_lines(const char *text) {
    int n = 0;
    for (int i = 0; i < n_lines; i++) n += strstr(lines[i], text) != NULL;
    return n;
}

static void listed(void) {
    VkExtensionProperties ep[3] = {{.extensionName = "VK_KHR_present_id", .specVersion = 1},
                                   {.extensionName = "VK_NV_low_latency2", .specVersion = 1},
                                   {.extensionName = "VK_NV_low_latency", .specVersion = 7}};
    CHECK(vk_lowlat_revision(ep, 3) == 1, "a driver that lists revision 1: %u", vk_lowlat_revision(ep, 3));
    ep[1].specVersion = 2;
    CHECK(vk_lowlat_revision(ep, 3) == 2, "a driver that lists revision 2: %u", vk_lowlat_revision(ep, 3));
    CHECK(vk_lowlat_revision(ep, 1) == 0, "a driver that does not list it: %u", vk_lowlat_revision(ep, 1));
    CHECK(vk_lowlat_revision(ep + 2, 1) == 0, "VK_NV_low_latency is another extension");
}

int main(int argc, char **argv) {
    static VkCtx ctx;
    driver_rev = argc > 1 ? (uint32_t)atoi(argv[1]) : 2;
    listed();
    ctx.device = DEV, ctx.has_low_latency2 = 1, ctx.has_present_id = 1, ctx.low_latency2_rev = driver_rev;
    vk_lowlat_init(&ctx);
    vk_lowlat_swapchain(SC, 1);
    for (uint64_t f = 1; f <= FRAMES; f++) {
        VkLowlatPresent p;
        now_ns += FRAME_NS;
        vk_lowlat_frame_start(1);
        vk_lowlat_submit_start();
        vk_lowlat_game_present();
        vk_lowlat_submitted(f);
        vk_lowlat_present_begin(f, &p);
        vk_lowlat_present_end(&p);
    }
    char want[64];
    snprintf(want, sizeof want, "through VK_NV_low_latency2 revision %u (", driver_rev);
    CHECK(n_lines > 0 && strstr(lines[0], want), "the first line does not say the revision: %s", lines[0]);
    CHECK(modes == 1 && mode_on == 1, "the sleep mode was set %d times, last %d", modes, mode_on);
    CHECK(sleeps == FRAMES - 1, "%d frame starts slept in the driver, not %d", sleeps, FRAMES - 1);
    CHECK(markers == 7 * FRAMES, "%d markers, not %d", markers, 7 * FRAMES);
    CHECK(count_lines("lowlat: frame ") == 2, "%d statistics lines, not 2", count_lines("lowlat: frame "));
    CHECK(strstr(lines[0], driver_rev >= 3 ? SUBMIT_FOLLOWED : SUBMIT_IGNORED) != NULL
          && !strstr(lines[0], driver_rev >= 3 ? SUBMIT_IGNORED : SUBMIT_FOLLOWED),
          "the first line does not say what the submissions' frame number does on revision %u: %s", driver_rev, lines[0]);
    VkLatencySubmissionPresentIdNV tag = {0};
    int tagged = 0;
    gfx_counters.frames = FRAMES;
    vk_lowlat_frame_start(1);
    CHECK(vk_lowlat_submit_tag(&tag) && tag.sType == VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV
          && tag.presentID == FRAMES + 1, "a submission of frame %u is not tagged on revision %u (%llu)", FRAMES + 1,
          driver_rev, (unsigned long long)tag.presentID);
    vk_lowlat_present_frame(FRAMES + 1);
    CHECK(vk_lowlat_blit_frame(&tag, &tagged) == FRAMES + 1 && tagged && tag.presentID == FRAMES + 1,
          "the present's blit is not tagged on revision %u", driver_rev);
    ll_level = 0, cfg_ver++;
    gfx_counters.frames = FRAMES + 1;
    vk_lowlat_game_present();
    vk_lowlat_frame_start(1);
    tag = (VkLatencySubmissionPresentIdNV){0};
    CHECK(!vk_lowlat_on() && !vk_lowlat_submit_tag(&tag) && !tag.presentID,
          "a submission is tagged with low latency off on revision %u", driver_rev);
    if (driver_rev < 2) {
        CHECK(rev1_calls == 0, "vkGetLatencyTimingsNV was called %d times with three arguments on a revision %u driver, "
              "which takes four: its fourth held %#llx, and a real driver reads pTimings at that plus 0x10", rev1_calls,
              driver_rev, (unsigned long long)rev1_fourth);
        CHECK(strstr(lines[0], NO_TIMINGS) != NULL, "the first line does not say the timings are left out: %s", lines[0]);
        CHECK(count_lines(NO_REPORT) == 1, "%d statistics lines say there is no report, not 1", count_lines(NO_REPORT));
        CHECK(count_lines(REPORT) == 0, "a report was logged");
    } else {
        CHECK(rev2_calls == 2 && rev1_calls == 0, "the timings were asked for %d times, not 2", rev2_calls);
        CHECK(!strstr(lines[0], NO_TIMINGS), "the first line says the timings are left out: %s", lines[0]);
        CHECK(count_lines(REPORT) == 2, "%d statistics lines carry the driver's report, not 2", count_lines(REPORT));
        CHECK(count_lines(NO_REPORT) == 0, "a statistics line says there is no report");
    }
    for (int i = 0; i < n_lines; i++) printf("  %s\n", lines[i]);
    vk_lowlat_release();
    printf("lowlat_revision_check revision %u: %s\n", driver_rev, fails ? "FAILED" : "ok");
    return fails != 0;
}
