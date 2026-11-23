// Contains altered code Copyright (C) 2018-2020 fincs (zlib, license text in nvn_tex.c; see THIRD_PARTY_NOTICES.md).
#pragma once
#include <stdint.h>
#include "nvn_obj.h"

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD);

static inline int nvn_tex_is_linear(const nvn_texdesc *d) { return (d->flags & 0x50) != 0; }
static inline uint32_t nvn_tex_linear_stride(const nvn_texdesc *d) {
    if (d->stride) return d->stride;
    int bw, bh; int bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    uint64_t align = (d->flags & 0x40) ? 128 : 32;
    uint64_t s = ((((uint64_t)d->width + bw - 1) / bw) * bpb + align - 1) & ~(align - 1);
    return s > UINT32_MAX ? UINT32_MAX : (uint32_t)s;
}
