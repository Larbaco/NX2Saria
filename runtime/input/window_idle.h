#pragma once
#include <stdint.h>

#define CURSOR_IDLE_NS 3000000000ull

typedef struct {
    uint64_t still_since;
    float x, y;
    int known, hidden;
    uint32_t hides;
} CursorIdle;

typedef struct {
    int muted;
    uint32_t mutes;
} FocusMute;

int cursor_idle_step(CursorIdle *s, int enabled, int over, float x, float y, uint64_t now_ns);
int focus_mute_step(FocusMute *s, int enabled, int focused);
