#pragma once

#include <stdint.h>
#include <string.h>

#define WBL_DRIVER_AMD 1
#ifdef _WIN32
#define WBL_ON_WINDOWS 1
#else
#define WBL_ON_WINDOWS 0
#endif

typedef struct {
    int on;
    const char *why;
} WblChoice;

static inline WblChoice wb_lazy_choice(const char *set, int windows, uint32_t driver_id) {
    if (set && *set) {
        if (!strcmp(set, "0")) return (WblChoice){0, "wb_lazy = 0 is set"};
        return (WblChoice){1, "wb_lazy is set to on"};
    }
    if (!windows)
        return (WblChoice){0, "the default outside Windows, where its fetch inside the access-fault handler has not run "
                              "(wb_lazy = 1 turns it on)"};
    if (driver_id == WBL_DRIVER_AMD)
        return (WblChoice){0, "the default on AMD's driver, where reports had the game end in play with it on and the "
                              "cause is not found; what shaders write goes back at the end of every submission, which "
                              "costs frame rate in open areas (wb_lazy = 1 turns it on)"};
    return (WblChoice){1, "the default on Windows (wb_lazy = 0 turns it off)"};
}
