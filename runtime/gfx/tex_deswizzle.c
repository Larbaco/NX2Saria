// Contains code Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
//
#include "host.h"
#include "par_jobs.h"
#include "tex.h"
#include <string.h>

static int gob_nvn2(void) {
    static int v = -1;
    if (v < 0) v = rt_nvn_api_version[0] >= 200;
    return v;
}

static uint32_t gob_off(uint32_t x, uint32_t y) {
    if (gob_nvn2()) return (x % 64 / 32) * 256 + (y % 8 / 4) * 128 + (x % 32 / 16) * 64 + (y % 4) * 16 + x % 16;
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;
    return (z / bd) * gobs_x * blocks_y * block
         + (y / (8 * bh)) * gobs_x * block
         + (x / 64) * block
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull
         + gob_off(x, y);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || nvn_tex_is_linear(d) || d->samples > 1) return -1;
    int bw, bh; *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb; *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

static void bl_copy_rows(uint8_t *bl, uint8_t *lin, size_t pitch, uint32_t row_bytes, uint32_t rows, uint64_t r0,
                         uint64_t r1, unsigned th, unsigned td, int to_linear) {
    static const uint32_t seg_x1[4] = {0, 32, 256, 288}, seg_nvn2[4] = {0, 64, 256, 320};
    const uint32_t *seg = gob_nvn2() ? seg_nvn2 : seg_x1;
    uint64_t block = 512ull << th << td;
    for (uint64_t r = r0; r < r1; r++) {
        uint32_t z = (uint32_t)(r / rows), y = (uint32_t)(r % rows);
        uint8_t *row = lin + (size_t)r * pitch, *g = bl + nvn_bl_offset(0, y, z, row_bytes, rows, th, td);
        uint32_t x = 0;
        if (to_linear)
            for (; x + 64 <= row_bytes; x += 64, g += block)
                for (int s = 0; s < 4; s++) memcpy(row + x + 16 * s, g + seg[s], 16);
        else
            for (; x + 64 <= row_bytes; x += 64, g += block)
                for (int s = 0; s < 4; s++) memcpy(g + seg[s], row + x + 16 * s, 16);
        for (int s = 0; x < row_bytes; s++, x += 16) {
            uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
            if (to_linear) memcpy(row + x, g + seg[s], n); else memcpy(g + seg[s], row + x, n);
        }
    }
}

void tex_bl_copy(uint8_t *bl, uint8_t *lin, size_t pitch, uint32_t row_bytes, uint32_t rows, uint32_t slices,
                 unsigned th, unsigned td, int to_linear) {
    bl_copy_rows(bl, lin, pitch, row_bytes, rows, 0, (uint64_t)rows * slices, th, td, to_linear);
}

#define BL_MT_MIN (1u << 20)
#define BL_MT_BAND (128u << 10)
typedef struct {
    uint8_t *bl, *lin;
    size_t pitch;
    uint32_t row_bytes, rows;
    uint64_t all, band;
    unsigned th, td;
    int to_linear;
} BlJob;

static void bl_band(void *arg, int i) {
    const BlJob *j = arg;
    uint64_t r0 = (uint64_t)i * j->band, r1 = r0 + j->band < j->all ? r0 + j->band : j->all;
    bl_copy_rows(j->bl, j->lin, j->pitch, j->row_bytes, j->rows, r0, r1, j->th, j->td, j->to_linear);
}

void tex_bl_copy_mt(uint8_t *bl, uint8_t *lin, size_t pitch, uint32_t row_bytes, uint32_t rows, uint32_t slices,
                    unsigned th, unsigned td, int to_linear) {
    uint64_t all = (uint64_t)rows * slices, bytes = all * row_bytes;
    if (bytes < BL_MT_MIN || !row_bytes || par_threads() < 2) {
        tex_bl_copy(bl, lin, pitch, row_bytes, rows, slices, th, td, to_linear);
        return;
    }
    uint64_t band = BL_MT_BAND / row_bytes;
    if (band < 1) band = 1;
    BlJob j = {bl, lin, pitch, row_bytes, rows, all, band, th, td, to_linear};
    par_run((int)((all + band - 1) / band), bl_band, &j);
}

static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint8_t *lin, size_t pitch, int to_linear,
                int mt) {
    if (nvn_tex_is_linear(d)) {
        if (level || layer || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        size_t row = (size_t)((d->width + bw - 1) / bw) * bpb;
        uint32_t rows = d->target == 0 ? 1 : (d->height + bh - 1) / bh, stride = nvn_tex_linear_stride(d);
        if (pitch < row || stride < row) return -1;
        for (uint32_t y = 0; y < rows; y++) {
            if (to_linear) memcpy(lin + y * pitch, bl + (size_t)y * stride, row);
            else memcpy(bl + (size_t)y * stride, lin + y * pitch, row);
        }
        return 0;
    }
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes) return -1;
    (mt ? tex_bl_copy_mt : tex_bl_copy)(bl + base, lin, pitch, row_bytes, rows, slices, th, td, to_linear);
    return 0;
}

int nvn_texels_box(const nvn_texdesc *d, unsigned level, unsigned layer, uint32_t x, uint32_t y, uint32_t z, uint32_t wb,
                   uint32_t h, void *storage, void *rows, size_t pitch, int to_linear) {
    uint8_t *st = storage, *ln = rows;
    if (nvn_tex_is_linear(d)) {
        if (level || layer || z || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpb) return -1;
        size_t row = (size_t)((d->width + bw - 1) / bw) * bpb;
        uint32_t nrows = d->target == 0 ? 1 : (d->height + bh - 1) / bh, stride = nvn_tex_linear_stride(d);
        if (stride < row || x + (size_t)wb > row || y + (uint64_t)h > nrows) return -1;
        for (uint32_t r = 0; r < h; r++) {
            uint8_t *p = st + (size_t)(y + r) * stride + x, *q = ln + r * pitch;
            if (to_linear) memcpy(q, p, wb); else memcpy(p, q, wb);
        }
        return 0;
    }
    uint32_t row_bytes, nrows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &nrows, &slices, &base, &th, &td, &bpb)) return -1;
    if (x + (uint64_t)wb > row_bytes || y + (uint64_t)h > nrows || z >= slices) return -1;
    for (uint32_t r = 0; r < h; r++) {
        uint8_t *q = ln + r * pitch;
        for (uint32_t i = 0; i < wb;) {
            uint32_t xb = x + i, n = 16 - xb % 16;
            if (n > wb - i) n = wb - i;
            uint8_t *p = st + base + nvn_bl_offset(xb, y + r, z, row_bytes, nrows, th, td);
            if (to_linear) memcpy(q + i, p, n); else memcpy(p, q + i, n);
            i += n;
        }
    }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1, 0);
}
int nvn_deswizzle_mt(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, (uint8_t *)dst, dst_pitch, 1, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, (uint8_t *)src, src_pitch, 0, 0);
}
