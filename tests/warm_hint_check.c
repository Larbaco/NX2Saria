#include <stdio.h>
#include <string.h>

#include "vk_warm_hint.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { NVIDIA = 0x10de, AMD = 0x1002, INTEL = 0x8086 };
enum { LINUX = 0, WINDOWS = 1 };

static int has(const char *s, const char *part) { return s && strstr(s, part) != NULL; }

int main(void) {
    const char *first = lw_cold_hint(NVIDIA, LW_COLD_FIRST, WINDOWS);
    CHECK(has(first, "Shader Cache Size") && has(first, "Unlimited"), "NVIDIA first cold: %s", first);
    CHECK(first && first[0] == ' ', "a hint starts with a space, as it follows a sentence: \"%s\"", first);
    CHECK(!strcmp(lw_cold_hint(AMD, LW_COLD_FIRST, WINDOWS), ""), "AMD first cold: no NVIDIA setting");
    CHECK(!strcmp(lw_cold_hint(INTEL, LW_COLD_FIRST, WINDOWS), ""), "Intel first cold: no NVIDIA setting");

    const char *again = lw_cold_hint(NVIDIA, LW_COLD_AFTER_WARM, WINDOWS);
    CHECK(has(again, "Shader Cache Size") && has(again, "Unlimited"), "NVIDIA cold after a warm: %s", again);
    CHECK(strcmp(again, first), "cold after a warm says more than a first cold");
    CHECK(!strcmp(lw_cold_hint(AMD, LW_COLD_AFTER_WARM, WINDOWS), ""), "AMD cold after a warm: no NVIDIA setting");

    for (int os = LINUX; os <= WINDOWS; os++)
        for (unsigned v = 0; v < 3; v++) {
            unsigned vendor = v == 0 ? NVIDIA : v == 1 ? AMD : INTEL;
            const char *gave = lw_cold_hint(vendor, LW_COLD_GAVE_UP, os);
            CHECK(has(gave, "lib_warm = force") && has(gave, "shader-warm.txt"), "os %d vendor %04x gave up: %s", os,
                  vendor, gave);
        }
    CHECK(has(lw_cold_hint(NVIDIA, LW_COLD_GAVE_UP, WINDOWS), "Shader Cache Size"),
          "NVIDIA gave up: names the setting");
    CHECK(!has(lw_cold_hint(AMD, LW_COLD_GAVE_UP, WINDOWS), "Shader Cache Size"), "AMD gave up: no NVIDIA setting");

    for (int why = LW_COLD_FIRST; why <= LW_WARM_FORCED; why++) {
        const char *h = lw_cold_hint(NVIDIA, why, LINUX);
        CHECK(!has(h, "NVIDIA app") && !has(h, "Shader Cache Size"), "Linux NVIDIA case %d: %s", why, h);
    }
    CHECK(!strcmp(lw_cold_hint(NVIDIA, LW_COLD_FIRST, LINUX), ""), "Linux NVIDIA first cold: nothing to add");

    for (int os = LINUX; os <= WINDOWS; os++)
        for (unsigned v = 0; v < 3; v++) {
            unsigned vendor = v == 0 ? NVIDIA : v == 1 ? AMD : INTEL;
            CHECK(!strcmp(lw_cold_hint(vendor, LW_WARM_FORCED, os), ""), "os %d vendor %04x forced warm: no hint", os,
                  vendor);
        }

#ifdef _WIN32
    CHECK(LW_ON_WINDOWS == 1, "a Windows build says so");
#else
    CHECK(LW_ON_WINDOWS == 0, "a build elsewhere says so");
#endif

    if (!fails) printf("warm hint: all checks passed\n");
    return fails != 0;
}
