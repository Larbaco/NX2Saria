#pragma once
#include <stdlib.h>

void foliage_distance_init(void);

enum { FOLIAGE_FLOATS = 8 };
#define FOLIAGE_LOD_OFF 0x3eefd38u
#define FOLIAGE_MIN 0.5f
#define FOLIAGE_MAX 3.0f

static const float foliage_game[FOLIAGE_FLOATS] = {40.0f, 60.0f, 35.0f, 30.0f, 40.0f, 135.0f, 10.0f, 45.0f};
static const float foliage_cap[FOLIAGE_FLOATS] = {0.0f, 0.0f, 75.0f, 70.0f, 0.0f, 0.0f, 0.0f, 0.0f};

static inline int foliage_factor(const char *v, float *factor) {
    *factor = 1.0f;
    if (!v || !*v) return 0;
    char *end;
    double d = strtod(v, &end);
    while (*end == ' ' || *end == '\t') end++;
    if (end == v || *end || d != d) return -1;
    if (d < FOLIAGE_MIN) return *factor = FOLIAGE_MIN, 1;
    if (d > FOLIAGE_MAX) return *factor = FOLIAGE_MAX, 1;
    *factor = (float)d;
    return 0;
}

static inline int foliage_scale(float factor, const float in[FOLIAGE_FLOATS], float out[FOLIAGE_FLOATS]) {
    int capped = 0;
    for (int i = 0; i < FOLIAGE_FLOATS; i++) {
        out[i] = in[i] * factor;
        float cap = foliage_cap[i] > in[i] ? foliage_cap[i] : in[i];
        if (foliage_cap[i] > 0.0f && out[i] > cap) out[i] = cap, capped++;
    }
    return capped;
}
