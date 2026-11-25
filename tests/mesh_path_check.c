#include <stdio.h>
#include <string.h>

#include "vk_mesh_path.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *const names[] = {"native", "fallback", "emulation", "none"};

enum { AMD = 0x1002, NV = MESH_VENDOR_NVIDIA, INTEL = MESH_VENDOR_INTEL };

static void expect_v(int has_mesh, int amd_sc, unsigned vendor, const char *mf, const char *me, int want,
                     const char *what) {
    const char *why = NULL;
    int got = mesh_path_decide(has_mesh, amd_sc, vendor, 0, mf, me, &why);
    CHECK(got == want, "%s (mesh shaders %d, AMD RDNA 1/2 driver %d, vendor %#x, mesh_fallback %s, mesh_emulation %s): "
          "%s, not %s", what, has_mesh, amd_sc, vendor, mf, me, names[got], names[want]);
    CHECK(why && *why, "%s: no reason for the boot line", what);
}
#define expect_nv(has_mesh, amd_sc, nvidia, mf, me, want, what) \
    expect_v(has_mesh, amd_sc, (nvidia) ? NV : AMD, mf, me, want, what)

static void expect(int has_mesh, int amd_sc, const char *mf, const char *me, int want, const char *what) {
    expect_v(has_mesh, amd_sc, AMD, mf, me, want, what);
}

#define V(major, minor, patch) ((unsigned)(major) << 22 | (unsigned)(minor) << 12 | (unsigned)(patch))

static void expect_radv(int has_mesh, const char *mf, const char *me, int want, const char *what) {
    const char *why = NULL;
    int got = mesh_path_decide(has_mesh, 0, AMD, 1, mf, me, &why);
    CHECK(got == want, "%s (RADV inside the task shader regression, mesh shaders %d, mesh_fallback %s, mesh_emulation "
          "%s): %s, not %s", what, has_mesh, mf, me, names[got], names[want]);
    CHECK(why && *why, "%s: no reason for the boot line", what);
}

static void expect_range(const MeshRadvSeries *t, int n, unsigned driver, unsigned v, int want, const char *what) {
    int got = mesh_radv_task_in(t, n, driver, v, NULL);
    CHECK(got == want, "%s (driver id %u, version %u.%u.%u): %s the range, expected %s", what, driver, v >> 22,
          v >> 12 & 0x3ff, v & 0xfff, got ? "inside" : "outside", want ? "inside" : "outside");
}

static void radv_versions(void) {
    enum { RADV = MESH_DRIVER_RADV, AMD_OWN = 1, ANV = 6 };
    static const MeshRadvSeries closed[] = {{26, 2, 8}, {26, 3, 2}};
    expect_range(closed, 2, RADV, V(25, 2, 8), 0, "the last release before the regression's series");
    expect_range(closed, 2, RADV, V(26, 1, 8), 0, "the series before the regression");
    expect_range(closed, 2, RADV, V(26, 2, 0), 1, "the first release with the regression");
    expect_range(closed, 2, RADV, V(26, 2, 7), 1, "the release before the fix in 26.2");
    expect_range(closed, 2, RADV, V(26, 2, 8), 0, "the fixed release of 26.2");
    expect_range(closed, 2, RADV, V(26, 2, 9), 0, "a release after the fix in 26.2");
    expect_range(closed, 2, RADV, V(26, 3, 0), 1, "the first release of 26.3");
    expect_range(closed, 2, RADV, V(26, 3, 1), 1, "the release before the fix in 26.3");
    expect_range(closed, 2, RADV, V(26, 3, 2), 0, "the fixed release of 26.3");
    expect_range(closed, 2, RADV, V(26, 4, 0), 0, "a later 26 series");
    expect_range(closed, 2, RADV, V(26, 99, 99), 0, "a snapshot a packager numbers 26.99.99");
    expect_range(closed, 2, RADV, V(27, 0, 0), 0, "a snapshot of the main branch, or 27.0.0");
    expect_range(closed, 2, RADV, V(28, 2, 1), 0, "a later year's 2 series");
    expect_range(closed, 2, AMD_OWN, V(26, 2, 7), 0, "AMD's own driver with the same number");
    expect_range(closed, 2, ANV, V(26, 3, 1), 0, "Mesa's Intel driver of the same release");

    static const MeshRadvSeries open[] = {{26, 2, MESH_RADV_NO_FIX}, {26, 3, 2}};
    expect_range(open, 2, RADV, V(26, 2, 7), 1, "a series without a fixed release");
    expect_range(open, 2, RADV, V(26, 2, 4095), 1, "a series without a fixed release, its last patch number");
    expect_range(open, 2, RADV, V(26, 3, 2), 0, "the other series' fixed release");
    unsigned fixed = 7;
    CHECK(mesh_radv_task_in(open, 2, RADV, V(26, 2, 7), &fixed) && fixed == MESH_RADV_NO_FIX,
          "a series without a fixed release: the fixed patch said is %u", fixed);
    fixed = 7;
    CHECK(mesh_radv_task_in(open, 2, RADV, V(26, 3, 1), &fixed) && fixed == 2, "26.3 fixed in 26.3.2: the patch said is %u", fixed);
    fixed = 7;
    CHECK(!mesh_radv_task_in(open, 2, RADV, V(25, 2, 8), &fixed) && fixed == 7, "outside every series: the patch was written (%u)", fixed);

    CHECK(!mesh_radv_task_regression(RADV, V(25, 2, 8), NULL), "25.2.8 is inside the range the game uses");
    CHECK(!mesh_radv_task_regression(RADV, V(26, 1, 8), NULL), "26.1.8 is inside the range the game uses");
    CHECK(mesh_radv_task_regression(RADV, V(26, 2, 0), NULL), "26.2.0 is outside the range the game uses");
    CHECK(mesh_radv_task_regression(RADV, V(26, 2, 7), NULL), "26.2.7 is outside the range the game uses");
    CHECK(mesh_radv_task_regression(RADV, V(26, 3, 0), NULL), "26.3.0 is outside the range the game uses");
    CHECK(mesh_radv_task_regression(RADV, V(26, 3, 1), NULL), "26.3.1 is outside the range the game uses");
    CHECK(!mesh_radv_task_regression(RADV, V(26, 99, 99), NULL), "26.99.99 is inside the range the game uses");
    CHECK(!mesh_radv_task_regression(RADV, V(27, 0, 0), NULL), "27.0.0 is inside the range the game uses");
    CHECK(!mesh_radv_task_regression(ANV, V(26, 3, 1), NULL), "another driver is inside the range the game uses");

    expect_radv(1, "auto", "0", MESH_PATH_FALLBACK, "an RX 6000 card");
    expect_radv(1, "", "0", MESH_PATH_FALLBACK, "an RX 6000 card with an empty mesh_fallback");
    expect_radv(1, "0", "0", MESH_PATH_NATIVE, "an RX 6000 card with mesh_fallback = 0");
    expect_radv(1, "1", "0", MESH_PATH_FALLBACK, "an RX 6000 card with mesh_fallback = 1");
    expect_radv(1, "auto", "1", MESH_PATH_FALLBACK, "an RX 6000 card with mesh_emulation = 1");
    expect_radv(0, "auto", "0", MESH_PATH_FALLBACK, "an RX 580 (no mesh shaders)");
    expect_radv(0, "0", "0", MESH_PATH_NONE, "an RX 580 with mesh_fallback = 0");
    expect_radv(0, "auto", "1", MESH_PATH_EMULATION, "an RX 580 with mesh_emulation = 1");
    const char *why = NULL;
    mesh_path_decide(1, 0, AMD, 1, "auto", "0", &why);
    CHECK(why && strstr(why, "RADV") && strstr(why, "task shaders") && strstr(why, "mesh_fallback = 0"),
          "the RADV default's reason does not name the driver, the task shaders or how to turn it off: %s", why ? why : "(none)");
    mesh_path_decide(0, 0, AMD, 1, "auto", "0", &why);
    CHECK(why && !strstr(why, "RADV"), "an RX 580's reason names the regression it cannot meet: %s", why ? why : "(none)");
    int p = mesh_path_decide(1, 0, AMD, 1, "auto", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 1, 0, AMD, 1, "0"), "RADV inside the range, mesh_fallback = auto: not the default");
    p = mesh_path_decide(1, 0, AMD, 1, "1", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 1, 0, AMD, 1, "0"), "RADV inside the range, mesh_fallback = 1: not the default");
    p = mesh_path_decide(1, 0, AMD, 1, "0", "0", NULL);
    CHECK(!mesh_path_fallback_default(p, 1, 0, AMD, 1, "0"), "RADV inside the range, mesh_fallback = 0: counted as the default fallback");
}

int main(void) {
    expect(1, 0, "auto", "0", MESH_PATH_NATIVE, "AMD's RDNA 3/4 driver or RADV with mesh shaders");
    expect_v(1, 0, NV, "auto", "0", MESH_PATH_NATIVE, "an NVIDIA RTX card");
    expect_v(1, 0, 0x10005, "auto", "0", MESH_PATH_NATIVE, "another vendor's device with mesh shaders");
    expect_v(1, 0, INTEL, "auto", "0", MESH_PATH_FALLBACK, "an Intel Arc card (Windows driver or ANV)");
    expect_v(1, 0, INTEL, "0", "0", MESH_PATH_NATIVE, "an Intel Arc card with mesh_fallback = 0");
    expect_v(1, 0, INTEL, "1", "0", MESH_PATH_FALLBACK, "an Intel Arc card with mesh_fallback = 1");
    expect_v(1, 0, INTEL, "", "0", MESH_PATH_FALLBACK, "an Intel Arc card with an empty mesh_fallback");
    expect_v(1, 0, INTEL, "auto", "1", MESH_PATH_FALLBACK, "an Intel Arc card with mesh_emulation = 1");
    expect_v(0, 0, INTEL, "auto", "0", MESH_PATH_FALLBACK, "an Intel GPU without mesh shaders");
    expect_v(0, 0, INTEL, "auto", "1", MESH_PATH_EMULATION, "an Intel GPU without mesh shaders, mesh_emulation = 1");
    expect_v(0, 0, INTEL, "0", "0", MESH_PATH_NONE, "an Intel GPU without mesh shaders and mesh_fallback = 0");
    expect(1, 1, "auto", "0", MESH_PATH_FALLBACK, "an RX 6000 card on AMD's RDNA 1/2 driver");
    expect(0, 1, "auto", "0", MESH_PATH_FALLBACK, "an RX 5000 card on AMD's RDNA 1/2 driver (no mesh shaders)");
    expect(0, 0, "auto", "0", MESH_PATH_FALLBACK, "a GTX 10 card (no mesh shaders)");
    expect(1, 1, "0", "0", MESH_PATH_NATIVE, "an RX 6000 card with mesh_fallback = 0");
    expect(1, 0, "0", "0", MESH_PATH_NATIVE, "a card with mesh shaders and mesh_fallback = 0");
    expect(0, 0, "0", "0", MESH_PATH_NONE, "a card without mesh shaders and mesh_fallback = 0");
    expect(1, 0, "1", "0", MESH_PATH_FALLBACK, "an NVIDIA card with mesh_fallback = 1");
    expect(1, 1, "1", "0", MESH_PATH_FALLBACK, "an RX 6000 card with mesh_fallback = 1");
    expect(0, 0, "auto", "1", MESH_PATH_EMULATION, "no mesh shaders, mesh_emulation = 1");
    expect(0, 1, "1", "1", MESH_PATH_EMULATION, "no mesh shaders, both set");
    expect(1, 1, "auto", "1", MESH_PATH_FALLBACK, "an RX 6000 card with mesh_emulation = 1");
    expect(1, 0, "auto", "1", MESH_PATH_NATIVE, "a card with mesh shaders and mesh_emulation = 1");
    expect_nv(0, 0, 1, "auto", "1", MESH_PATH_FALLBACK, "a GTX 10 card with mesh_emulation = 1");
    expect_nv(0, 0, 1, "1", "1", MESH_PATH_FALLBACK, "a GTX 10 card with both set");
    expect_nv(0, 0, 1, "0", "1", MESH_PATH_NONE, "a GTX 10 card with mesh_emulation = 1 and mesh_fallback = 0");
    expect_nv(0, 0, 1, "auto", "0", MESH_PATH_FALLBACK, "a GTX 10 card by default");
    expect_nv(1, 0, 1, "auto", "1", MESH_PATH_NATIVE, "an RTX card with mesh_emulation = 1");
    expect_nv(1, 0, 1, "auto", "0", MESH_PATH_NATIVE, "an RTX card by default");
    expect(1, 1, "", "0", MESH_PATH_FALLBACK, "an RX 6000 card with an empty mesh_fallback");
    expect(1, 0, "yes", "0", MESH_PATH_NATIVE, "a card with mesh shaders and mesh_fallback = yes");
    const char *why = NULL;
    mesh_path_decide(1, 1, 0, 0, "auto", "0", &why);
    CHECK(why && strstr(why, "mesh_fallback = 0"), "the RX 6000 default's reason does not say how to turn it off: %s", why ? why : "(none)");

    mesh_path_decide(0, 0, 1, 0, "auto", "1", &why);
    CHECK(why && strstr(why, "mesh_emulation"), "a GTX 10 card with mesh_emulation = 1: the reason does not name it: %s",
          why ? why : "(none)");

    mesh_path_decide(1, 0, INTEL, 0, "auto", "0", &why);
    CHECK(why && strstr(why, "Intel") && strstr(why, "fog") && strstr(why, "mesh_fallback = 0"),
          "the Intel default's reason does not name the fog or how to turn it off: %s", why ? why : "(none)");
    mesh_path_decide(1, 0, NV, 0, "auto", "0", &why);
    CHECK(why && !strcmp(why, "VK_EXT_mesh_shader"), "an RTX card's reason: %s", why ? why : "(none)");

    int p;
    p = mesh_path_decide(1, 1, 0x1002, 0, "auto", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 1, 1, 0x1002, 0, "0"), "AMD's RDNA 1/2 driver, mesh_fallback = auto: not the default");
    p = mesh_path_decide(1, 1, 0x1002, 0, "1", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 1, 1, 0x1002, 0, "0"), "AMD's RDNA 1/2 driver, mesh_fallback = 1: not the default");
    p = mesh_path_decide(1, 0, MESH_VENDOR_INTEL, 0, "auto", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 1, 0, MESH_VENDOR_INTEL, 0, "0"), "Intel, mesh_fallback = auto: not the default");
    p = mesh_path_decide(0, 0, 0x1002, 0, "auto", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 0, 0, 0x1002, 0, "0"), "no mesh shaders, mesh_fallback = auto: not the default");
    p = mesh_path_decide(1, 0, 0x1002, 0, "1", "0", NULL);
    CHECK(p == MESH_PATH_FALLBACK && !mesh_path_fallback_default(p, 1, 0, 0x1002, 0, "0"),
          "RADV with mesh shaders, mesh_fallback = 1 forced: counted as the default");
    p = mesh_path_decide(1, 0, 0x1002, 0, "auto", "0", NULL);
    CHECK(!mesh_path_fallback_default(p, 1, 0, 0x1002, 0, "0"), "RADV with mesh shaders (native): counted as the default");
    p = mesh_path_decide(1, 1, 0x1002, 0, "0", "0", NULL);
    CHECK(!mesh_path_fallback_default(p, 1, 1, 0x1002, 0, "0"), "mesh_fallback = 0: counted as the default fallback");
    radv_versions();
    printf("mesh_path_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
