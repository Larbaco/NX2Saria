#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "widescreen_math.h"

static void dimensions(unsigned h, double aspect, unsigned pw, unsigned sw, unsigned sh) {
    unsigned got_pw, got_sw, got_sh;
    wide_aspect_dimensions(h, aspect, &got_pw, &got_sw, &got_sh);
    assert(got_pw == pw && got_sw == sw && got_sh == sh);
}

int main(void) {
    dimensions(1440, 16.0 / 10.0, 2304, 1728, 1080);
    dimensions(1440, 3.0 / 2.0, 2160, 1620, 1080);
    dimensions(1440, 16.0 / 9.0, 2560, 1920, 1080);
    dimensions(1440, 64.0 / 27.0, 3416, 2560, 1080);
    dimensions(1600, 16.0 / 10.0, 2560, 1920, 1200);
    assert(wide_aspect_heap_growth_kib(2304, 1440, 0.75) == 0);
    assert(wide_aspect_heap_growth_kib(2160, 1440, 0.75) == 0);
    assert(wide_aspect_heap_growth_kib(2560, 1600, 0.75) == 34 * 1024);
    assert(fabs(wide_ui_vertical_scale(16.0 / 10.0) - 0.9) < 1e-12);
    assert(fabs(wide_ui_vertical_scale(3.0 / 2.0) - 0.84375) < 1e-12);
    assert(fabs(wide_ui_vertical_scale(16.0 / 9.0) - 1.0) < 1e-12);
    assert(fabs(wide_ui_vertical_scale(64.0 / 27.0) - 1.0) < 1e-12);
    double original = 60.0 * 3.14159265358979323846 / 180.0;
    double vert = wide_vertical_fov(original, 16.0 / 10.0);
    double expect = 2.0 * atan(tan(original / 2.0) * (16.0 / 9.0) / (16.0 / 10.0));
    assert(fabs(vert - expect) < 1e-12);
    assert(fabs(wide_vertical_fov(original, 16.0 / 9.0) - original) < 1e-12);
    assert(fabs(wide_vertical_fov(original, 64.0 / 27.0) - original) < 1e-12);
    double fov120 = original * 1.2;
    double stacked = wide_vertical_fov(fov120, 16.0 / 10.0);
    double expected_stacked = 2.0 * atan(tan(fov120 / 2.0) * (16.0 / 9.0) / (16.0 / 10.0));
    assert(fabs(stacked - expected_stacked) < 1e-12);
    puts("widescreen_math_check: 16:10/3:2 sizes, 16:9/21:9 controls, heap, UI scale and stacked FOV pass");
}
