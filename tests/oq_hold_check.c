#include <stdio.h>
#include <string.h>

#include "vk_oq_hold.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static OqHold t;

int main(void) {
    CHECK(oqh_lost(&t, 0x5000, 0) == 1, "an empty table gave %llu for nothing counted, not 1",
          (unsigned long long)oqh_lost(&t, 0x5000, 0));
    CHECK(oqh_lost(&t, 0x5000, 37) == 37, "an empty table changed a count of 37");
    oqh_note(&t, 0x5000, 4096);
    CHECK(oqh_lost(&t, 0x5000, 0) == 4096, "a lost report of a record last at 4096 gave %llu",
          (unsigned long long)oqh_lost(&t, 0x5000, 0));
    CHECK(oqh_lost(&t, 0x5000, 12) == 4096, "a lost report that counted 12 of its draws gave %llu, not the held 4096",
          (unsigned long long)oqh_lost(&t, 0x5000, 12));
    CHECK(oqh_lost(&t, 0x5000, 5000) == 5000, "a lost report that counted 5000 gave %llu",
          (unsigned long long)oqh_lost(&t, 0x5000, 5000));
    oqh_note(&t, 0x5000, 0);
    CHECK(oqh_lost(&t, 0x5000, 0) == 1, "a record last reported hidden (0) gave %llu for a lost report, not 1",
          (unsigned long long)oqh_lost(&t, 0x5000, 0));
    oqh_note(&t, 0x5000, 300);
    CHECK(oqh_lost(&t, 0x5000, 7) == 300, "the newest complete count (300) did not hold");
    oqh_note(&t, 0x5010, 9000);
    CHECK(oqh_lost(&t, 0x5000, 0) == 300 || oqh_slot(0x5000) == oqh_slot(0x5010),
          "the record 16 bytes on changed this one's count");
    CHECK(oqh_lost(&t, 0x5010, 2) == 9000, "the record 16 bytes on did not hold its own count");
    CHECK(oqh_lost(&t, 0x6000, 2) == 2, "a record never reported took another's count");
    uint64_t a = 0x100000, b = 0;
    for (uint64_t x = a + 16; x < a + (1ull << 24); x += 16)
        if (oqh_slot(x) == oqh_slot(a)) { b = x; break; }
    CHECK(b != 0, "no address shares a slot with %#llx in 16 MiB", (unsigned long long)a);
    if (b) {
        oqh_note(&t, a, 111);
        oqh_note(&t, b, 222);
        CHECK(oqh_lost(&t, a, 3) == 3, "an overwritten record gave %llu, not what it counted",
              (unsigned long long)oqh_lost(&t, a, 3));
        CHECK(oqh_lost(&t, b, 3) == 222, "the newer record of a slot lost its count");
    }
    oqh_note(&t, 0, 777);
    CHECK(oqh_lost(&t, 0, 0) == 1, "address 0 was remembered");
    int used[OQH_N];
    memset(used, 0, sizeof used);
    int collisions = 0;
    for (int i = 0; i < 256; i++) {
        uint32_t s = oqh_slot(0x2692c7000ull + 32ull * (uint64_t)i);
        collisions += used[s]++ != 0;
    }
    CHECK(collisions < 64, "256 records 32 bytes apart collided %d times in %d slots", collisions, OQH_N);
    if (!fails) printf("oq_hold_check: all checks passed\n");
    return fails ? 1 : 0;
}
