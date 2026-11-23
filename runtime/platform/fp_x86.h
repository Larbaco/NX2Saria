#pragma once
#include <stdint.h>
#include <xmmintrin.h>

#define PLAT_FP_RC_MASK 0x6000u
#define PLAT_FP_FTZ 0x8000u
#define PLAT_FP_DAZ 0x0040u

static inline uint32_t plat_fp_get(void) { return _mm_getcsr(); }
static inline void plat_fp_set(uint32_t v) { _mm_setcsr(v); }
