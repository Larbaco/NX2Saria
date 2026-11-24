#pragma once
#include "imgui.h"

extern "C" {
#include "perfmon.h"
}

typedef struct {
    PerfStats s;
    const char *version;
    char picture[12][64];
    bool scene_known;
    float fg_fps;
    bool under_menu;
} PerfPanel;

void perf_panel_draw(const PerfPanel *p, float width, float scale);
