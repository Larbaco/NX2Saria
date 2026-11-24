#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nvn_obj.h"
#include "nvn_tex.h"
#include "vk.h"

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear,
                  size_t dst_pitch);
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_linear, size_t src_pitch,
                void *dst_storage);
int nvn_deswizzle_mt(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear,
                     size_t dst_pitch);
int nvn_texels_box(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t x, uint32_t y, uint32_t z, uint32_t wb,
                   uint32_t h, void *storage, void *rows, size_t pitch, int to_linear);
uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td);
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t *row_bytes, uint32_t *rows,
                       uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb);
void tex_bl_copy(uint8_t *bl, uint8_t *lin, size_t pitch, uint32_t row_bytes, uint32_t rows, uint32_t slices,
                 unsigned th, unsigned td, int to_linear);
void tex_bl_copy_mt(uint8_t *bl, uint8_t *lin, size_t pitch, uint32_t row_bytes, uint32_t rows, uint32_t slices,
                    unsigned th, unsigned td, int to_linear);

void astc_decode_ldr(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, bool srgb,
                     uint8_t *rgba_out);
bool astc_decode_block(const uint8_t block[16], uint32_t bw, uint32_t bh, bool srgb, uint8_t *rgba_out);
bool astc_block_opaque(const uint8_t block[16], uint32_t bw, uint32_t bh, bool srgb, uint32_t *decoded);
uint64_t astc_blocks_opaque(const uint8_t *blocks, uint64_t n, uint32_t bw, uint32_t bh, bool srgb, uint32_t max_decoded,
                            uint32_t *decoded, uint8_t *dst);
bool astc_blocks_opaque_mt(const uint8_t *blocks, uint64_t n, uint32_t bw, uint32_t bh, bool srgb, uint32_t max_decoded,
                           uint32_t *decoded, uint8_t *dst);
void astc_decode_ldr_mt(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, bool srgb,
                        uint8_t *rgba_out);

typedef struct TexVkFormat {
    VkFormat vk;
    VkFormat ideal;
    VkFormatFeatureFlags need;
    uint8_t expand_rgb;
    uint8_t alpha_one;
    uint8_t chan[4];
    uint8_t astc_bw, astc_bh;
    uint8_t astc_srgb;
    uint8_t astc_bc;
    uint8_t depth, stencil;
    const char *note;
    const char *reason;
} TexVkFormat;

int tex_vk_format(int nvn_format, TexVkFormat *out);
int tex_vk_format_supported(const TexVkFormat *f);
