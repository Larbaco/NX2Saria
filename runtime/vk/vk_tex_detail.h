#pragma once
#include <stdint.h>

#define DETAIL_FULL_MB 10240
#define DETAIL_LOW_MB 6144
#define DETAIL_RGBA8_MB 20480

static inline int tex_detail_auto(int64_t mb, int shared, int rgba8) {
    int most = !mb || mb >= DETAIL_FULL_MB ? 0 : mb >= DETAIL_LOW_MB ? 1 : 2;
    if (mb && !most && shared) most = 1;
    if (mb && mb < DETAIL_RGBA8_MB && most < 2 && rgba8) most++;
    return most;
}
