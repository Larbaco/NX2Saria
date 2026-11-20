// Aspect calculations shared by the Ocarina wide-format runtime and its focused tests.
#pragma once

#include <math.h>
#include <stdint.h>

#define WIDE_BASE_ASPECT (16.0 / 9.0)

static inline double wide_vertical_fov(double angle, double aspect) {
    if (!isfinite(angle) || !isfinite(aspect) || angle <= 0 || angle >= 3.14159265358979323846 ||
        aspect <= 0 || aspect >= WIDE_BASE_ASPECT)
        return angle;
    return 2.0 * atan(tan(angle * 0.5) * (WIDE_BASE_ASPECT / aspect));
}

static inline double wide_ui_vertical_scale(double aspect) {
    return aspect > 0 && aspect < WIDE_BASE_ASPECT ? aspect / WIDE_BASE_ASPECT : 1.0;
}

static inline void wide_aspect_dimensions(unsigned picture_h, double aspect, unsigned *picture_w,
                                          unsigned *scene_w, unsigned *scene_h) {
    unsigned w = (unsigned)lround(picture_h * aspect / 8.0) * 8;
    *picture_w = w;
    unsigned quantum = aspect < WIDE_BASE_ASPECT ? 4 : 8;
    *scene_w = (unsigned)lround(w * 0.75 / quantum) * quantum;
    *scene_h = (unsigned)lround(picture_h * 0.75 / 2.0) * 2;
}

static inline uint64_t wide_aspect_heap_growth_kib(unsigned picture_w, unsigned picture_h, double scene_share) {
    const double base_pixels = 2560.0 * 1440.0;
    const double base_heap_kib = 312320.0;
    const double default_share = 0.75;
    double ratio = (double)picture_w * picture_h / base_pixels;
    double k = scene_share / default_share;
    double growth = ratio * k * k - 1.0;
    return growth > 0 ? (uint64_t)ceil(base_heap_kib * growth / 1024.0) * 1024 : 0;
}
