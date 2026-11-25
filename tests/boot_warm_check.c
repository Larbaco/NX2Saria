#include <stdio.h>

#include "boot_warm.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const char *key = "1002:15bf:00806001", *other = "1002:15bf:00805ffe";
    char text[128];
    int tries = -1;

    CHECK(boot_warm_decide(NULL, key, 0, &tries) == BOOT_WARM_NO && tries == 0, "a warm without a driver change");

    CHECK(boot_warm_decide(NULL, key, 1, &tries) == BOOT_WARM_YES && tries == 1, "no warm after a driver change");

    for (int n = 1; n < BOOT_WARM_TRIES; n++) {
        snprintf(text, sizeof text, "%s %d\n", key, n);
        CHECK(boot_warm_decide(text, key, 0, &tries) == BOOT_WARM_YES && tries == n + 1, "an owed warm not taken up at %d", n);
        CHECK(boot_warm_decide(text, key, 1, &tries) == BOOT_WARM_YES && tries == n + 1, "the count lost at %d", n);
    }

    snprintf(text, sizeof text, "%s %d\n", key, BOOT_WARM_TRIES);
    CHECK(boot_warm_decide(text, key, 0, &tries) == BOOT_WARM_GAVE_UP && tries == 0, "a fourth try");
    CHECK(boot_warm_decide(text, key, 1, &tries) == BOOT_WARM_GAVE_UP && tries == 0, "a fourth try on a foreign cache");

    snprintf(text, sizeof text, "%s done\n", key);
    CHECK(boot_warm_decide(text, key, 1, &tries) == BOOT_WARM_DONE && tries == 0, "a second warm on the same driver");
    CHECK(boot_warm_decide(text, key, 0, &tries) == BOOT_WARM_DONE && tries == 0, "a done file read as stale");

    snprintf(text, sizeof text, "%s %d\n", other, BOOT_WARM_TRIES);
    CHECK(boot_warm_decide(text, key, 0, &tries) == BOOT_WARM_NO, "another driver's file taken for this one's");
    CHECK(boot_warm_decide(text, key, 1, &tries) == BOOT_WARM_YES && tries == 1, "another driver's count carried over");
    snprintf(text, sizeof text, "%s done\n", other);
    CHECK(boot_warm_decide(text, key, 0, &tries) == BOOT_WARM_NO, "another driver's done taken for this one's");
    CHECK(boot_warm_decide(text, key, 1, &tries) == BOOT_WARM_YES && tries == 1, "another driver's done stops this one's warm");

    CHECK(boot_warm_decide("", key, 0, &tries) == BOOT_WARM_NO, "an empty file owes a warm");
    CHECK(boot_warm_decide("garbage", key, 1, &tries) == BOOT_WARM_YES && tries == 1, "a broken file stops the warm");
    snprintf(text, sizeof text, "%s -4\n", key);
    CHECK(boot_warm_decide(text, key, 0, &tries) == BOOT_WARM_NO, "a negative count owes a warm");
    snprintf(text, sizeof text, "%s x\n", key);
    CHECK(boot_warm_decide(text, key, 0, &tries) == BOOT_WARM_NO, "a count that is no number owes a warm");
    snprintf(text, sizeof text, "%s 2x\n", key);
    CHECK(boot_warm_decide(text, key, 0, &tries) == BOOT_WARM_NO, "a count with a tail owes a warm");

    int quiet = -1;
    snprintf(text, sizeof text, "%s 0\n", key);
    CHECK(boot_end_decide(text, key, 1, &quiet) == BOOT_END_SKIP, "a reported crash does not skip the prebuild");
    for (int n = 0; n < BOOT_QUIET_ENDS; n++) {
        snprintf(text, sizeof text, "%s %d\n", key, n);
        int want = n + 1 >= BOOT_QUIET_ENDS ? BOOT_END_SKIP : BOOT_END_COUNTED;
        CHECK(boot_end_decide(text, key, 0, &quiet) == want && quiet == n + 1, "a stopped start after %d before it", n);
    }
    CHECK(boot_end_decide(key, key, 0, &quiet) == BOOT_END_COUNTED && quiet == 1, "an old marker skips the prebuild");
    CHECK(boot_end_decide(key, key, 1, &quiet) == BOOT_END_SKIP, "an old marker with a crash note does not skip");
    snprintf(text, sizeof text, "%s 2\n", other);
    CHECK(boot_end_decide(text, key, 0, &quiet) == BOOT_END_OTHER && quiet == 0, "another driver's marker counted");
    CHECK(boot_end_decide(text, key, 1, &quiet) == BOOT_END_OTHER, "another driver's crash blamed on this one");
    CHECK(boot_end_decide("", key, 0, &quiet) == BOOT_END_OTHER, "an empty marker counted");

    printf("boot_warm_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
