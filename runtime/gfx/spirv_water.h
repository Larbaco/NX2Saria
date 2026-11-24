#pragma once
#include <stdint.h>

uint32_t *spirv_water_vertex(const uint32_t *w, uint64_t n, uint64_t *out_n);
uint32_t *spirv_water_fragment(const uint32_t *w, uint64_t n, uint64_t *out_n);
uint32_t *spirv_water_absorption(const uint32_t *w, uint64_t n, uint32_t pointer, uint32_t distance, uint64_t *out_n);
