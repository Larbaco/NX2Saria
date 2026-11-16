// --probe-fsr3fg: AMD FSR 3.1 frame generation with the runtime's passes (vk_fsr3fg.c) on a synthetic scene, no game
// files and no window. The picture (1280x720, RGBA8 like an SDR swapchain image) is a textured background (blocks of
// noise over a gradient, which the optical flow can follow) with a yellow square moving right by 16 pixels a frame, a
// HUD bar (a green rectangle) drawn over it in the picture with the HUD and left out of the picture without it, black
// bars above and below the picture's rectangle; the depth (D32F, 0 near) and the motion vectors (R16G16F, render
// pixels, current to previous) are at half the size, 640x360, the square nearer than the background. Each frame is
// uploaded and generated as gfx_present.c's presenter records it; after 12 frames the generated frame (between the
// last two) is read back.
// Checks: the first frame and a reset show no generated frame and every other does; the square's centre in the
// generated frame is within 2 pixels of halfway between the two frames' (not at either end); the HUD bar is the picture
// with the HUD's to within 2 of 255; the black bars stay black; no validation error from the passes (with
// NX2SARIA_VK_VALIDATION=1). The same run with the motion vectors' sign reversed is reported (the optical flow alone may
// still place the square). Then the GPU time of one generated frame at 2560x1080 from 1920x810 (the 21:9 1080p of the
// bench) and at 1920x1080 from 1280x720, from the passes' own timestamps, and vk_fsr3fg_idle freeing the images.
// NX2SARIA_FSR3FG_PROBE_PPM=<dir> writes the last two pictures and the generated one as PPM images.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_fsr3fg.h"

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
    VkFormat fmt;
    uint32_t w, h;
} Img;

static int make_img(VkCtx *c, Img *m, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags u, VkImageAspectFlags a) {
    memset(m, 0, sizeof *m);
    m->fmt = fmt, m->w = w, m->h = h;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &m->img, &m->mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = {a, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &m->view) == VK_SUCCESS;
}

static void free_img(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->mem);
    memset(m, 0, sizeof *m);
}

typedef struct {
    VkBuffer buf;
    VmaAllocation mem;
    void *map;
} Buf;

static int make_buf(VkCtx *c, Buf *b, VkDeviceSize size, VkBufferUsageFlags u, int readback) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = u};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                           (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                                     : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &b->buf, &b->mem, &info) != VK_SUCCESS) return 0;
    b->map = info.pMappedData;
    return 1;
}

static void free_buf(VkCtx *c, Buf *b) {
    if (b->buf) vmaDestroyBuffer(c->vma, b->buf, b->mem);
    memset(b, 0, sizeof *b);
}

static uint16_t f2h(float f) {
    _Float16 h = (_Float16)f;
    uint16_t u;
    memcpy(&u, &h, 2);
    return u;
}

// ---- the scene

#define SQ 64          // the square's side, picture pixels
#define BAR_TOP 60     // black bars of this height above and below the picture's rectangle
static const uint8_t SQ_RGB[3] = {255, 220, 40}, HUD_RGB[3] = {40, 255, 40};

static uint32_t hash(uint32_t x) {
    x ^= x >> 16, x *= 0x7feb352d, x ^= x >> 15, x *= 0x846ca68b, x ^= x >> 16;
    return x;
}

// The square's left edge at frame i
static float sq_x(int i, float vx) { return 100.0f + vx * (float)i; }

typedef struct {
    uint32_t W, H, rw, rh;
    Img bb, hl, depth, mv, out;
    Buf up_bb, up_hl, up_depth, up_mv, down;
    int upload, readback, made, first;
    float jx, jy;
    int reset;
} Frame;

static int in_hud(uint32_t x, uint32_t y) { return x >= 24 && x < 224 && y >= BAR_TOP + 16 && y < BAR_TOP + 56; }

// Frame i's pictures (with and without the HUD), depth and motion vectors; mvsign -1 reverses the vectors.
static void fill(Frame *f, int i, float vx, float mvsign) {
    const uint32_t W = f->W, H = f->H;
    uint8_t *bb = f->up_bb.map, *hl = f->up_hl.map;
    const float x0 = sq_x(i, vx), y0 = (float)H / 2 - SQ / 2;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = bb + 4 * ((size_t)y * W + x);
            if (y < BAR_TOP || y >= H - BAR_TOP) {
                p[0] = p[1] = p[2] = 0;
            } else if (x >= x0 && x < x0 + SQ && y >= y0 && y < y0 + SQ) {
                p[0] = SQ_RGB[0], p[1] = SQ_RGB[1], p[2] = SQ_RGB[2];
            } else {   // 8x8 blocks of noise over a gradient: blue-grey, never the square's or the HUD's colour
                uint32_t n = hash((x / 8) * 7919u + (y / 8) * 104729u) & 63;
                p[0] = (uint8_t)(40 + (x * 60) / W + n), p[1] = (uint8_t)(50 + (y * 60) / H + n / 2);
                p[2] = (uint8_t)(110 + n);
            }
            p[3] = 255;
            memcpy(hl + 4 * ((size_t)y * W + x), p, 4);
            if (in_hud(x, y)) p[0] = HUD_RGB[0], p[1] = HUD_RGB[1], p[2] = HUD_RGB[2];
        }
    float *z = f->up_depth.map;
    uint16_t *mv = f->up_mv.map;
    const float sx = (float)W / (float)f->rw, sy = (float)H / (float)f->rh;
    for (uint32_t y = 0; y < f->rh; y++)
        for (uint32_t x = 0; x < f->rw; x++) {
            float px = ((float)x + 0.5f) * sx, py = ((float)y + 0.5f) * sy;
            int on = px >= x0 && px < x0 + SQ && py >= y0 && py < y0 + SQ;
            z[(size_t)y * f->rw + x] = on ? 0.5f : 0.95f;
            // current to previous: the square was vx picture pixels to the left, vx / sx render pixels
            mv[2 * ((size_t)y * f->rw + x)] = f2h(on ? -mvsign * vx / sx : 0.0f);
            mv[2 * ((size_t)y * f->rw + x) + 1] = f2h(0.0f);
        }
    vmaFlushAllocation(vk_ctx()->vma, f->up_bb.mem, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(vk_ctx()->vma, f->up_hl.mem, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(vk_ctx()->vma, f->up_depth.mem, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(vk_ctx()->vma, f->up_mv.mem, 0, VK_WHOLE_SIZE);
}

static void general(VkCommandBuffer cb, VkImage img, VkImageAspectFlags a) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {a, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static void full(VkCommandBuffer cb) {
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m};
    vkCmdPipelineBarrier2(cb, &d);
}

static void copy_in(VkCommandBuffer cb, Buf *b, Img *m, VkImageAspectFlags a) {
    VkBufferImageCopy r = {.imageSubresource = {a, 0, 0, 1}, .imageExtent = {m->w, m->h, 1}};
    vkCmdCopyBufferToImage(cb, b->buf, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static void record(VkCommandBuffer cb, void *arg) {
    Frame *f = arg;
    if (f->first) {
        general(cb, f->bb.img, VK_IMAGE_ASPECT_COLOR_BIT), general(cb, f->hl.img, VK_IMAGE_ASPECT_COLOR_BIT);
        general(cb, f->depth.img, VK_IMAGE_ASPECT_DEPTH_BIT), general(cb, f->mv.img, VK_IMAGE_ASPECT_COLOR_BIT);
        general(cb, f->out.img, VK_IMAGE_ASPECT_COLOR_BIT);
        f->first = 0;
    }
    if (f->upload) {
        full(cb);
        copy_in(cb, &f->up_bb, &f->bb, VK_IMAGE_ASPECT_COLOR_BIT), copy_in(cb, &f->up_hl, &f->hl, VK_IMAGE_ASPECT_COLOR_BIT);
        copy_in(cb, &f->up_depth, &f->depth, VK_IMAGE_ASPECT_DEPTH_BIT);
        copy_in(cb, &f->up_mv, &f->mv, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    full(cb);
    VkNgxImage bb = {f->bb.img, f->bb.view, f->bb.fmt, f->W, f->H}, hl = {f->hl.img, f->hl.view, f->hl.fmt, f->W, f->H},
               out = {f->out.img, f->out.view, f->out.fmt, f->W, f->H};
    VkFsr3fgInputs in = {.depth = {f->depth.img, f->depth.fmt, f->rw, f->rh, 0, f->depth.view},
                         .mv = {f->mv.img, f->mv.fmt, f->rw, f->rh, 1, f->mv.view},
                         .jx = f->jx, .jy = f->jy, .reset = f->reset,
                         .cam = {1.0f, 0.1f, 25000.0f, 1.0f}, .dt_ms = 16.67f};
    const int rect[4] = {0, BAR_TOP, (int)f->W, (int)f->H - 2 * BAR_TOP};
    f->made = vk_fsr3fg_record(cb, &bb, &hl, &out, rect, &in);
    full(cb);
    if (f->readback) {
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {f->W, f->H, 1}};
        vkCmdCopyImageToBuffer(cb, f->out.img, VK_IMAGE_LAYOUT_GENERAL, f->down.buf, 1, &r);
        full(cb);
    }
}

static int submit_wait(Frame *f) {
    uint64_t v = vk_sync_submit_own(record, f);
    return v && vk_sync_fence_wait(v, 10000000000ull);
}

static int frame_make(VkCtx *c, Frame *f, uint32_t W, uint32_t H, uint32_t rw, uint32_t rh) {
    memset(f, 0, sizeof *f);
    f->W = W, f->H = H, f->rw = rw, f->rh = rh, f->first = 1;
    const VkImageUsageFlags su = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    const VkBufferUsageFlags up = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    return make_img(c, &f->bb, W, H, VK_FORMAT_R8G8B8A8_UNORM, su, VK_IMAGE_ASPECT_COLOR_BIT) &&
           make_img(c, &f->hl, W, H, VK_FORMAT_R8G8B8A8_UNORM, su, VK_IMAGE_ASPECT_COLOR_BIT) &&
           make_img(c, &f->out, W, H, VK_FORMAT_R8G8B8A8_UNORM, su, VK_IMAGE_ASPECT_COLOR_BIT) &&
           make_img(c, &f->depth, rw, rh, VK_FORMAT_D32_SFLOAT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT) &&
           make_img(c, &f->mv, rw, rh, VK_FORMAT_R16G16_SFLOAT, su, VK_IMAGE_ASPECT_COLOR_BIT) &&
           make_buf(c, &f->up_bb, (VkDeviceSize)W * H * 4, up, 0) && make_buf(c, &f->up_hl, (VkDeviceSize)W * H * 4, up, 0) &&
           make_buf(c, &f->up_depth, (VkDeviceSize)rw * rh * 4, up, 0) &&
           make_buf(c, &f->up_mv, (VkDeviceSize)rw * rh * 4, up, 0) &&
           make_buf(c, &f->down, (VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
}

static void frame_free(VkCtx *c, Frame *f) {
    free_img(c, &f->bb), free_img(c, &f->hl), free_img(c, &f->out), free_img(c, &f->depth), free_img(c, &f->mv);
    free_buf(c, &f->up_bb), free_buf(c, &f->up_hl), free_buf(c, &f->up_depth), free_buf(c, &f->up_mv);
    free_buf(c, &f->down);
}

static void write_ppm(const char *dir, const char *name, const uint8_t *rgba, uint32_t w, uint32_t h) {
    if (!dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%u %u\n255\n", w, h);
    for (size_t k = 0; k < (size_t)w * h; k++) fwrite(rgba + 4 * k, 1, 3, fp);
    fclose(fp);
}

// The square's centre (x) and its pixel count in an RGBA8 picture
static double square_x(const uint8_t *p, uint32_t W, uint32_t H, long *count) {
    double sx = 0;
    long n = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            const uint8_t *q = p + 4 * ((size_t)y * W + x);
            if (q[0] > 200 && q[1] > 170 && q[2] < 110) sx += x + 0.5, n++;
        }
    *count = n;
    return n ? sx / (double)n : -1;
}

typedef struct {
    int shown_ok;        // the first frame and the reset showed nothing, every other frame showed one
    double cx, expect;   // the square's centre in the generated frame and halfway
    long px;             // its pixels there
    int hud_diff, bars;  // the largest HUD difference; bar pixels that are not black
} Result;

// One run of `frames` frames at W x H from rw x rh, panning vx picture pixels a frame; the generated frame between the
// last two is read back. A reset at frame `reset_at`.
static int run(VkCtx *c, uint32_t W, uint32_t H, uint32_t rw, uint32_t rh, int frames, float vx, float mvsign,
               int reset_at, Result *res, const char *ppm, const char *tag) {
    Frame f;
    memset(res, 0, sizeof *res);
    if (!frame_make(c, &f, W, H, rw, rh)) {
        frame_free(c, &f);
        return 0;
    }
    vk_fsr3fg_reset();   // a new run: a new history
    int ok = 1, shown_ok = 1;
    for (int i = 0; i < frames && ok; i++) {
        fill(&f, i, vx, mvsign);
        f.upload = 1, f.readback = i == frames - 1, f.reset = i == reset_at;
        ok = submit_wait(&f);
        int want = i != 0 && i != reset_at;
        if (f.made != want) shown_ok = 0;
    }
    if (ok) {
        vmaInvalidateAllocation(c->vma, f.down.mem, 0, VK_WHOLE_SIZE);
        const uint8_t *o = f.down.map;
        res->shown_ok = shown_ok;
        res->cx = square_x(o, W, H, &res->px);
        res->expect = sq_x(frames - 2, vx) + vx / 2 + SQ / 2;
        // the HUD: as in the picture with it; the bars: black
        const uint8_t *b = f.up_bb.map;   // the last frame's picture with the HUD
        for (uint32_t y = 0; y < H; y++)
            for (uint32_t x = 0; x < W; x++) {
                size_t k = 4 * ((size_t)y * W + x);
                if (in_hud(x, y))
                    for (int ch = 0; ch < 3; ch++) {
                        int d = abs((int)o[k + ch] - (int)b[k + ch]);
                        if (d > res->hud_diff) res->hud_diff = d;
                    }
                if ((y < BAR_TOP || y >= H - BAR_TOP) && (o[k] > 2 || o[k + 1] > 2 || o[k + 2] > 2)) res->bars++;
            }
        if (ppm) {
            char name[64];
            snprintf(name, sizeof name, "fsr3fg_%s_generated", tag);
            write_ppm(ppm, name, o, W, H);
            snprintf(name, sizeof name, "fsr3fg_%s_last", tag);
            write_ppm(ppm, name, f.up_bb.map, W, H);
        }
    }
    frame_free(c, &f);
    return ok;
}

int probe_fsr3fg(void) {
    if (!vk_fsr3fg_build()) {
        printf("probe-fsr3fg: SKIP (the Windows build generates FSR 3.1 frames through AMD's library)\n");
        return 0;
    }
    if (!vk_init()) {
        printf("probe-fsr3fg: SKIP (no Vulkan device)\n");
        return 0;
    }
    VkCtx *c = vk_ctx();
    long errors0 = vk_validation_errors_total();
    printf("probe-fsr3fg: GPU %s\n", c->props.deviceName);
    int bad = 0;
    if (vk_fsr3fg_init() != 1) {
        printf("probe-fsr3fg: SKIP (this device cannot run the passes; the log says why)\n");
        vk_sync_shutdown();
        vk_fsr3fg_release();
        vk_mem_shutdown();
        vk_shutdown();
        return 0;
    }
    const char *ppm = cfg_get("NX2SARIA_FSR3FG_PROBE_PPM");
    const uint32_t W = 1280, H = 720, rw = 640, rh = 360;
    const float vx = 16.0f;
    const int frames = 12;
    Result r, rr, rz;
    if (!run(c, W, H, rw, rh, frames, vx, 1.0f, 6, &r, ppm, "pan")) {
        printf("probe-fsr3fg: FAIL the run (test images or a submission)\n");
        bad = 1;
    } else {
        printf("probe-fsr3fg: %ux%u from %ux%u, %d frames, the square moving %.0f px a frame, a reset at frame 6: "
               "generated frames where expected %s; square centre %.2f (halfway %.2f, the frames %.2f and %.2f), %ld px; "
               "HUD within %d of 255; %d bar pixels not black\n",
               W, H, rw, rh, frames, vx, r.shown_ok ? "yes" : "NO", r.cx, r.expect, r.expect - vx / 2, r.expect + vx / 2,
               r.px, r.hud_diff, r.bars);
        if (!r.shown_ok) printf("probe-fsr3fg: FAIL a generated frame was shown after a reset or missing\n"), bad = 1;
        if (!(fabs(r.cx - r.expect) <= 2.0))
            printf("probe-fsr3fg: FAIL the square is not halfway between the two frames\n"), bad = 1;
        if (r.px < SQ * SQ / 2) printf("probe-fsr3fg: FAIL the square lost more than half its pixels\n"), bad = 1;
        if (r.hud_diff > 2) printf("probe-fsr3fg: FAIL the HUD was not kept as drawn\n"), bad = 1;
        if (r.bars) printf("probe-fsr3fg: FAIL the black bars were written\n"), bad = 1;
    }
    if (run(c, W, H, rw, rh, frames, vx, -1.0f, -1, &rr, ppm, "reversed"))
        printf("probe-fsr3fg: the motion vectors reversed: square centre %.2f (halfway %.2f), %ld px\n", rr.cx,
               rr.expect, rr.px);
    if (run(c, W, H, rw, rh, frames, 0.0f, 1.0f, -1, &rz, ppm, "still")) {
        printf("probe-fsr3fg: the scene still: square centre %.2f (at %.2f), %ld px\n", rz.cx, rz.expect, rz.px);
        if (!(fabs(rz.cx - rz.expect) <= 1.0)) printf("probe-fsr3fg: FAIL a still square moved\n"), bad = 1;
    }
    // GPU time at the bench's 21:9 1080p and at 1080p, from the passes' timestamps
    static const uint32_t sizes[2][4] = {{2560, 1080, 1920, 810}, {1920, 1080, 1280, 720}};
    for (int s = 0; s < 2; s++) {
        VkFsr3fgStats a, b;
        Result t;
        // a call's timestamps are read RING (8) calls later: a first run reads the last run's and warms this size up,
        // and the second one's 40 frames read 40 timed frames of this size
        int ok = run(c, sizes[s][0], sizes[s][1], sizes[s][2], sizes[s][3], 40, 12.0f, 1.0f, -1, &t, NULL, "time");
        vk_fsr3fg_stats(&a);
        if (!ok || !run(c, sizes[s][0], sizes[s][1], sizes[s][2], sizes[s][3], 40, 12.0f, 1.0f, -1, &t, NULL, "time")) {
            printf("probe-fsr3fg: FAIL the timing run at %ux%u\n", sizes[s][0], sizes[s][1]), bad = 1;
            continue;
        }
        vk_fsr3fg_stats(&b);
        uint64_t n = b.gpu_samples - a.gpu_samples;
        double of = n ? (b.gpu_ms_of * b.gpu_samples - a.gpu_ms_of * a.gpu_samples) / n : 0;
        double fi = n ? (b.gpu_ms_fi * b.gpu_samples - a.gpu_ms_fi * a.gpu_samples) / n : 0;
        printf("probe-fsr3fg: GPU time %ux%u from %ux%u: %.3f ms optical flow + %.3f ms interpolation = %.3f ms a "
               "generated frame (%llu frames); %.1f MiB\n", sizes[s][0], sizes[s][1], sizes[s][2], sizes[s][3], of, fi,
               of + fi, (unsigned long long)n, b.vram / 1048576.0);
    }
    {   // with NX2SARIA_FSR3FG_PASS_TIMES=1: each pass's share
        const char *name;
        double ms;
        uint64_t n;
        for (int i = 0; vk_fsr3fg_pass_ms(i, &name, &ms, &n); i++)
            if (n) printf("probe-fsr3fg: pass %-28s %.3f ms a dispatch (%llu dispatches)\n", name, ms, (unsigned long long)n);
    }
    vk_fsr3fg_idle();
    VkFsr3fgStats st;
    vk_fsr3fg_stats(&st);
    printf("probe-fsr3fg: frame generation off: %.1f MiB held after vk_fsr3fg_idle; %llu frames recorded, %llu "
           "generated, %llu history starts\n", st.vram / 1048576.0, (unsigned long long)st.frames,
           (unsigned long long)st.generated, (unsigned long long)st.cuts);
    if (st.vram) printf("probe-fsr3fg: FAIL the images were not freed\n"), bad = 1;
    vk_sync_shutdown();
    vk_fsr3fg_release();
    vk_mem_shutdown();
    long errors = vk_validation_errors_total() - errors0;
    vk_shutdown();
    long at_end = vk_validation_errors_total() - errors0 - errors;
    if (errors0) printf("probe-fsr3fg: %ld validation errors at device creation, before the passes, not counted\n", errors0);
    if (at_end) printf("probe-fsr3fg: %ld validation errors at the device's destruction, not counted\n", at_end);
    if (bad || errors) {
        printf("probe-fsr3fg: FAILED (%s, %ld validation errors)\n", bad ? "a check failed" : "checks passed", errors);
        return 1;
    }
    printf("probe-fsr3fg: passed (0 validation errors)\n");
    return 0;
}
