#pragma once

#include <stdint.h>

enum {
    LW_COLD_FIRST,
    LW_COLD_AFTER_WARM,
    LW_COLD_GAVE_UP,
    LW_WARM_FORCED,
};

#define LW_VENDOR_NVIDIA 0x10de
#ifdef _WIN32
#define LW_ON_WINDOWS 1
#else
#define LW_ON_WINDOWS 0
#endif

static inline const char *lw_cold_hint(uint32_t vendor, int why, int windows) {
    int nvidia = vendor == LW_VENDOR_NVIDIA && windows;
    switch (why) {
    case LW_COLD_FIRST:
        return nvidia ? " On NVIDIA the driver's Shader Cache Size limit empties it too, when it is smaller than the "
                        "prepared libraries (a 2048 MB cache dropped the oldest as the warm built the newest): "
                        "Shader Cache Size Unlimited in the NVIDIA app keeps them."
                      : "";
    case LW_COLD_AFTER_WARM:
        return nvidia ? " A finished warm did not stay in the driver's cache: on NVIDIA that is the Shader Cache Size "
                        "limit, smaller than the prepared libraries; set Shader Cache Size to Unlimited in the NVIDIA "
                        "app, or this warm is lost again."
                      : "";
    case LW_COLD_GAVE_UP:
        return nvidia ? " On NVIDIA that is the Shader Cache Size limit: after setting it to Unlimited in the NVIDIA "
                        "app, start once with lib_warm = force (or delete shader-warm.txt) to warm again."
                      : " Once the driver's cache can hold them, start once with lib_warm = force (or delete "
                        "shader-warm.txt) to warm again.";
    case LW_WARM_FORCED:
        return "";
    }
    return "";
}
