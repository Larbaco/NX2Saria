#include <stdio.h>

#include "vk_build_gate.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const double limit = 73154, keep = 4096, est = 64;
    BuildGate g = {.free_mb = keep};

    for (int i = 0; i < 16; i++) CHECK(build_gate_enter(&g, 42000, limit, est), "builder %d refused with 31 GB free", i);
    CHECK(g.reserved_mb == 16 * est, "reserved %.0f MB, expected %.0f", g.reserved_mb, 16 * est);
    for (int i = 0; i < 16; i++) build_gate_leave(&g, est);
    CHECK(g.reserved_mb == 0, "reserved %.0f MB after every build ended", g.reserved_mb);

    double used = limit - keep - 4 * est;
    int in = 0;
    for (int i = 0; i < 16; i++) in += build_gate_enter(&g, used, limit, est);
    CHECK(in == 4, "%d builders started in room for 4", in);
    CHECK(g.reserved_mb == 4 * est, "a refused build reserved something: %.0f MB", g.reserved_mb);
    build_gate_leave(&g, est);
    CHECK(build_gate_enter(&g, used, limit, est), "no build started after one ended");
    CHECK(!build_gate_enter(&g, used, limit, est), "a fifth build started");
    for (int i = 0; i < 4; i++) build_gate_leave(&g, est);

    CHECK(build_gate_admits(&g, limit - keep - est, limit, est), "refused with exactly enough room");
    CHECK(!build_gate_admits(&g, limit - keep - est + 1, limit, est), "admitted one MB short");

    CHECK(build_gate_admits(&g, 66000, limit, est), "an ordinary build refused at 66 of 73 GB");
    CHECK(!build_gate_admits(&g, 66000, limit, 4096), "a large compute build admitted at 66 of 73 GB");
    CHECK(build_gate_enter(&g, 64900, limit, 4096), "a large compute build refused with 8,254 MB free");
    CHECK(!build_gate_admits(&g, 64900, limit, est), "an ordinary build admitted beside a large one with no room for both");
    build_gate_leave(&g, 4096);

    CHECK(!build_gate_admits(&g, limit + 500, limit, est), "admitted above the limit");

    BuildGate off = {.free_mb = 0};
    CHECK(build_gate_enter(&off, limit, limit, est), "refused with the gate off");
    CHECK(off.reserved_mb == est, "the gate that is off did not count its build");
    CHECK(build_gate_admits(&g, 1e9, -1, est), "refused with an unknown limit");

    build_gate_leave(&g, est);
    CHECK(g.reserved_mb == 0, "reserved %.0f MB after a leave too many", g.reserved_mb);

    printf("build_gate_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
