// runtime/vk_mesh_path.h: how the game's task and mesh programs are drawn (vk_device.c create_device): natively, through
// the mesh fallback, the mesh emulation or not at all, for each device kind and setting. Built and run by
// tests/mesh_path_check.py; prints each failed check and exits 1 on any.
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
    int got = mesh_path_decide(has_mesh, amd_sc, vendor, mf, me, &why);
    CHECK(got == want, "%s (mesh shaders %d, AMD RDNA 1/2 driver %d, vendor %#x, mesh_fallback %s, mesh_emulation %s): "
          "%s, not %s", what, has_mesh, amd_sc, vendor, mf, me, names[got], names[want]);
    CHECK(why && *why, "%s: no reason for the boot line", what);
}
#define expect_nv(has_mesh, amd_sc, nvidia, mf, me, want, what) \
    expect_v(has_mesh, amd_sc, (nvidia) ? NV : AMD, mf, me, want, what)

static void expect(int has_mesh, int amd_sc, const char *mf, const char *me, int want, const char *what) {
    expect_v(has_mesh, amd_sc, AMD, mf, me, want, what);
}

int main(void) {
    // ---- the default (auto, emulation off) by device
    expect(1, 0, "auto", "0", MESH_PATH_NATIVE, "AMD's RDNA 3/4 driver or RADV with mesh shaders");
    expect_v(1, 0, NV, "auto", "0", MESH_PATH_NATIVE, "an NVIDIA RTX card");
    expect_v(1, 0, 0x10005, "auto", "0", MESH_PATH_NATIVE, "another vendor's device with mesh shaders");
    // ---- Intel's cards with mesh shaders take the fallback on every Intel driver: holes in the fog on Arc B390 and
    // B580 with the native path (issue #13), which 0.18.2's 32-lane task stage and invariant positions did not mend
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
    // ---- mesh_fallback = 0 gives an RX 6000 card native mesh pipelines back, as 0.10.1 drew them
    expect(1, 1, "0", "0", MESH_PATH_NATIVE, "an RX 6000 card with mesh_fallback = 0");
    expect(1, 0, "0", "0", MESH_PATH_NATIVE, "a card with mesh shaders and mesh_fallback = 0");
    expect(0, 0, "0", "0", MESH_PATH_NONE, "a card without mesh shaders and mesh_fallback = 0");
    // ---- mesh_fallback = 1 on every device
    expect(1, 0, "1", "0", MESH_PATH_FALLBACK, "an NVIDIA card with mesh_fallback = 1");
    expect(1, 1, "1", "0", MESH_PATH_FALLBACK, "an RX 6000 card with mesh_fallback = 1");
    // ---- the mesh emulation acts only without mesh shaders, and then before the fallback
    expect(0, 0, "auto", "1", MESH_PATH_EMULATION, "no mesh shaders, mesh_emulation = 1");
    expect(0, 1, "1", "1", MESH_PATH_EMULATION, "no mesh shaders, both set");
    expect(1, 1, "auto", "1", MESH_PATH_FALLBACK, "an RX 6000 card with mesh_emulation = 1");
    expect(1, 0, "auto", "1", MESH_PATH_NATIVE, "a card with mesh shaders and mesh_emulation = 1");
    // ---- an NVIDIA card never takes the emulation, which ended the game inside its driver (GTX 10 cards in the
    // low-end GPU mode of 0.10.1, which set mesh_emulation = 1)
    expect_nv(0, 0, 1, "auto", "1", MESH_PATH_FALLBACK, "a GTX 10 card with mesh_emulation = 1");
    expect_nv(0, 0, 1, "1", "1", MESH_PATH_FALLBACK, "a GTX 10 card with both set");
    expect_nv(0, 0, 1, "0", "1", MESH_PATH_NONE, "a GTX 10 card with mesh_emulation = 1 and mesh_fallback = 0");
    expect_nv(0, 0, 1, "auto", "0", MESH_PATH_FALLBACK, "a GTX 10 card by default");
    expect_nv(1, 0, 1, "auto", "1", MESH_PATH_NATIVE, "an RTX card with mesh_emulation = 1");
    expect_nv(1, 0, 1, "auto", "0", MESH_PATH_NATIVE, "an RTX card by default");
    // ---- a value the settings table does not list reads as auto
    expect(1, 1, "", "0", MESH_PATH_FALLBACK, "an RX 6000 card with an empty mesh_fallback");
    expect(1, 0, "yes", "0", MESH_PATH_NATIVE, "a card with mesh shaders and mesh_fallback = yes");
    // ---- the reason names the driver default, and how to turn it off
    const char *why = NULL;
    mesh_path_decide(1, 1, 0, "auto", "0", &why);
    CHECK(why && strstr(why, "mesh_fallback = 0"), "the RX 6000 default's reason does not say how to turn it off: %s", why ? why : "(none)");

    mesh_path_decide(0, 0, 1, "auto", "1", &why);
    CHECK(why && strstr(why, "mesh_emulation"), "a GTX 10 card with mesh_emulation = 1: the reason does not name it: %s",
          why ? why : "(none)");

    mesh_path_decide(1, 0, INTEL, "auto", "0", &why);
    CHECK(why && strstr(why, "Intel") && strstr(why, "fog") && strstr(why, "mesh_fallback = 0"),
          "the Intel default's reason does not name the fog or how to turn it off: %s", why ? why : "(none)");
    mesh_path_decide(1, 0, NV, "auto", "0", &why);
    CHECK(why && !strcmp(why, "VK_EXT_mesh_shader"), "an RTX card's reason: %s", why ? why : "(none)");

    // mesh_fallback_batch = auto batches where the fallback is the device's default (mesh_fallback = auto would choose it)
    int p;
    p = mesh_path_decide(1, 1, 0x1002, "auto", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 1, 1, 0x1002, "0"), "AMD's RDNA 1/2 driver, mesh_fallback = auto: not the default");
    p = mesh_path_decide(1, 1, 0x1002, "1", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 1, 1, 0x1002, "0"), "AMD's RDNA 1/2 driver, mesh_fallback = 1: not the default");
    p = mesh_path_decide(1, 0, MESH_VENDOR_INTEL, "auto", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 1, 0, MESH_VENDOR_INTEL, "0"), "Intel, mesh_fallback = auto: not the default");
    p = mesh_path_decide(0, 0, 0x1002, "auto", "0", NULL);
    CHECK(mesh_path_fallback_default(p, 0, 0, 0x1002, "0"), "no mesh shaders, mesh_fallback = auto: not the default");
    p = mesh_path_decide(1, 0, 0x1002, "1", "0", NULL);
    CHECK(p == MESH_PATH_FALLBACK && !mesh_path_fallback_default(p, 1, 0, 0x1002, "0"),
          "RADV with mesh shaders, mesh_fallback = 1 forced: counted as the default");
    p = mesh_path_decide(1, 0, 0x1002, "auto", "0", NULL);
    CHECK(!mesh_path_fallback_default(p, 1, 0, 0x1002, "0"), "RADV with mesh shaders (native): counted as the default");
    p = mesh_path_decide(1, 1, 0x1002, "0", "0", NULL);
    CHECK(!mesh_path_fallback_default(p, 1, 1, 0x1002, "0"), "mesh_fallback = 0: counted as the default fallback");
    printf("mesh_path_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
