// Contains code Copyright (c) 2026 jhaizhou, Copyright (c) 2026 Peiikai (MIT; see THIRD_PARTY_NOTICES.md).
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "prof.h"
#include "tex.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_evo_peek.h"
#include "vk_pass_hooks.h"
#include "vk_target_frames.h"
#include "vk_dlaa.h"

#define WEAK __attribute__((weak))
#define CALL_DEPTH 8

typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    VkImageAspectFlags aspect;
    uint32_t level, base, layers;
    VkImageView view;
} ViewEnt;

static VkExec X;
static int x_ready;
static _Thread_local VkExec *x_cur;

#define GROUP_CHUNKS 17
#define LIVE_GROUPS 4
typedef struct Group Group;
typedef struct {
    VkExec ex;
    const op_hdr *h;
    uint32_t nops;
    uint32_t cond0, ncond;
    uint64_t oq;
    uint32_t slot0, nslots;
    uint32_t brk;
    int idx;
    Group *g;
    VkCommandBuffer cb;
    int recorded;
    uint64_t bind_reused, bind_resolved, usnap_hits;
    volatile int32_t done;
} Chunk;
struct Group {
    Chunk c[GROUP_CHUNKS];
    int n;
    uint8_t *cond;
    uint32_t ncond, ccond;
    volatile int32_t hooks;
    volatile int32_t ndone;
    volatile uint64_t seq;
    uint32_t upd;
    int at;
    uint64_t out_ns;
};
static Group GQ[LIVE_GROUPS];
#define QN (LIVE_GROUPS * GROUP_CHUNKS)
static struct {
    Chunk *q[QN];
    volatile uint64_t put, take;
    volatile uint64_t done;
    uint64_t out, drained;
} Q;
static PlatMutex done_lock = PLAT_MUTEX_INIT;
static _Thread_local Chunk *rw_chunk;
static _Thread_local struct { uint64_t reused, resolved, hits; } replay_counts;
static uint64_t bind_reused_total, bind_resolved_total;

void vk_exec_bind_count(int reused) {
    if (rw_chunk) {
        if (reused) rw_chunk->bind_reused++;
        else rw_chunk->bind_resolved++;
    } else if (x_cur) {
        if (reused) replay_counts.reused++;
        else replay_counts.resolved++;
    } else {
        __atomic_add_fetch(reused ? &bind_reused_total : &bind_resolved_total, 1, __ATOMIC_RELAXED);
    }
}

void vk_exec_usnap_hit(void) {
    if (rw_chunk) rw_chunk->usnap_hits++;
    else if (x_cur) replay_counts.hits++;
    else vk_usnap_hits_add(1);
}

static void replay_counts_flush(void) {
    if (replay_counts.reused) __atomic_add_fetch(&bind_reused_total, replay_counts.reused, __ATOMIC_RELAXED);
    if (replay_counts.resolved) __atomic_add_fetch(&bind_resolved_total, replay_counts.resolved, __ATOMIC_RELAXED);
    if (replay_counts.hits) vk_usnap_hits_add(replay_counts.hits);
    memset(&replay_counts, 0, sizeof replay_counts);
}
static _Thread_local int rw_behind;
enum { PW_NEXT, PW_OP, PW_HOOKS, PW_TURN, PW_TEX, PW_SPLIT, PW_END, PW_DONE, PW_N };
static void group_wait(int why, int help);
static void group_drain(VkExec *ex, int why);
static _Thread_local const op_hdr *rw_op;
static _Thread_local int rw_ordered;
static PlatMutex tfr_lock = PLAT_MUTEX_INIT;


VkExecFrame vk_exec_frame;
static struct { const uint8_t *mem; uint32_t w, h, fmt, n; } fw[64];
static int nfw;

uint64_t vk_exec_recording_fence(void) { return x_cur ? x_cur->fence : 0; }

void vk_exec_frame_written(const tex_t *t) {
    if (!(rt_trace & RT_TRACE_GFX) || !t) return;
    for (int i = 0; i < nfw; i++)
        if (fw[i].mem == t->mem) { fw[i].n++; return; }
    if (nfw == 64) return;
    fw[nfw].mem = t->mem, fw[nfw].w = t->d.width, fw[nfw].h = t->d.height, fw[nfw].fmt = (uint32_t)t->d.format;
    fw[nfw++].n = 1;
}

enum { RB_DISPATCH, RB_COPY, RB_CLEAR, RB_EVENT, RB_BARRIER, RB_HELD, RB_SUBMIT, RB_N };
enum { RO_COUNTER, RO_COND, RO_UB, RO_RT, RO_ATT_CLEAR, RO_N };
static const char *const rb_name[RB_N] = {"dispatches", "copies", "clears", "events", "barriers", "held", "submits"};
static const char *const ro_name[RO_N] = {"counters", "conditions", "uniform updates", "target changes",
                                          "attachment clears"};
static struct {
    int on;
    uint32_t draws;
    uint64_t total, in16, in64, in256, runs, longest, brk[RB_N], carried[RO_N], frame0;
} RR = {.on = -1};

static void runs_break(int why) {
    uint32_t d = RR.draws;
    RR.brk[why]++;
    if (!d) return;
    RR.runs++, RR.total += d, RR.draws = 0;
    if (d >= 16) RR.in16 += d;
    if (d >= 64) RR.in64 += d;
    if (d >= 256) RR.in256 += d;
    if (d > RR.longest) RR.longest = d;
}

static void runs_op(uint32_t op) {
    switch (op) {
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DRAW_MESH: case OP_DRAW_MESH_IND: RR.draws++; break;
    case OP_DISPATCH: case OP_DISPATCH_IND: runs_break(RB_DISPATCH); break;
    case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: runs_break(RB_COPY); break;
    case OP_CLEAR_TEX: case OP_CLEAR_BUF: runs_break(RB_CLEAR); break;
    case OP_SIGNAL_EVENT: case OP_WAIT_EVENT: runs_break(RB_EVENT); break;
    case OP_BARRIER: runs_break(RB_BARRIER); break;
    case OP_REPORT_COUNTER: case OP_RESET_COUNTER: RR.carried[RO_COUNTER]++; break;
    case OP_RENDER_ENABLE: case OP_RENDER_COND: RR.carried[RO_COND]++; break;
    case OP_UPDATE_UB: RR.carried[RO_UB]++; break;
    case OP_SET_RT: RR.carried[RO_RT]++; break;
    case OP_CLEAR_COLOR: case OP_CLEAR_DS: RR.carried[RO_ATT_CLEAR]++; break;
    default: break;
    }
}

static int runs_on(void) {
    if (RR.on < 0) RR.on = rt_knob("REPLAY_RUNS", "0");
    return RR.on;
}

static void runs_frame(uint64_t frame) {
    if (!runs_on() || frame % 600) return;
    if (RR.frame0 && frame > RR.frame0) {
        double f = (double)(frame - RR.frame0), t = RR.total ? (double)RR.total : 1.0;
        char b[256], c[256];
        int o = 0;
        for (int k = 0; k < RB_N; k++)
            o += snprintf(b + o, sizeof b - (size_t)o, "%s %s %.1f", k ? "," : "", rb_name[k], RR.brk[k] / f);
        o = 0;
        for (int k = 0; k < RO_N; k++)
            o += snprintf(c + o, sizeof c - (size_t)o, "%s %s %.1f", k ? "," : "", ro_name[k], RR.carried[k] / f);
        rt_log("vk_exec: frame %llu: draw runs: %.0f draws a frame in %.1f runs; in runs of 16 or more %.0f%%, "
               "64 or more %.0f%%, 256 or more %.0f%%, longest %llu; breaks a frame:%s; carried a frame:%s",
               (unsigned long long)frame, RR.total / f, RR.runs / f, 100.0 * RR.in16 / t, 100.0 * RR.in64 / t,
               100.0 * RR.in256 / t, (unsigned long long)RR.longest, b, c);
    }
    uint32_t open = RR.draws;
    memset(&RR.total, 0, sizeof RR - offsetof(typeof(RR), total));
    RR.draws = open, RR.frame0 = frame;
}

uint64_t vk_exec_submit_seq;

void vk_exec_frame_log(const tex_t *p, uint64_t frame) {
    vk_exec_cond_frame_end(frame);
    runs_frame(frame);
    vk_exec_phases_frame(frame);
    if (!(rt_trace & RT_TRACE_GFX)) return;
    VkExecFrame *f = &vk_exec_frame;
    uint64_t bind_reused = __atomic_exchange_n(&bind_reused_total, 0, __ATOMIC_RELAXED);
    uint64_t bind_resolved = __atomic_exchange_n(&bind_resolved_total, 0, __ATOMIC_RELAXED);
    uint64_t ew, eu, el, ed;
    vk_exec_event_stats(&ew, &eu, &el, &ed);
    int hit = 0;
    char list[512] = "";
    size_t o = 0;
    for (int i = 0; i < nfw; i++) {
        if (p && fw[i].mem == p->mem) hit = 1;
        if (o < sizeof list - 48)
            o += (size_t)snprintf(list + o, sizeof list - o, " %p:%ux%u/%#x*%u", (const void *)fw[i].mem, fw[i].w,
                                  fw[i].h, fw[i].fmt, fw[i].n);
    }
    rt_log("frame %llu: submits %llu passes %llu draws %llu/%llu clears %llu copies %llu dispatches %llu; present %p "
           "%ux%u/%#x written %d; feedback draws %llu barriers %llu restarts %llu layouts %llu read-before-write %llu; "
           "uniform updates %llu (%llu ended a pass); binds %llu in %.2f ms (textures %.2f, buffers %.2f, sig %.2f, push %.2f ms; "
           "descriptors reused %llu, resolved %llu); dynamic state calls %llu (%llu unchanged, left out); "
           "event waits %llu (%llu before their signal: %llu "
           "signalled later, %.1f submits later on average); targets%s",
           (unsigned long long)frame, (unsigned long long)f->submits,
           (unsigned long long)f->passes, (unsigned long long)f->draws, (unsigned long long)f->draws_seen,
           (unsigned long long)f->clears, (unsigned long long)f->copies, (unsigned long long)f->dispatches,
           p ? (const void *)p->mem : NULL, p ? p->d.width : 0, p ? p->d.height : 0, p ? (uint32_t)p->d.format : 0,
           hit, (unsigned long long)f->fb_draws, (unsigned long long)f->fb_barriers, (unsigned long long)f->fb_restarts,
           (unsigned long long)f->fb_layouts, (unsigned long long)f->fb_war_barriers, (unsigned long long)f->ub_updates,
           (unsigned long long)f->ub_pass_breaks, (unsigned long long)f->bind_calls,
           (double)(f->bind_ns) / 1e6, (double)(f->bind_tex_ns) / 1e6, (double)(f->bind_buf_ns) / 1e6,
           (double)(f->bind_sig_ns) / 1e6, (double)(f->bind_push_ns) / 1e6, (unsigned long long)bind_reused,
           (unsigned long long)bind_resolved, (unsigned long long)f->dyn_calls, (unsigned long long)f->dyn_left,
           (unsigned long long)ew, (unsigned long long)eu, (unsigned long long)el,
           el ? (double)ed / (double)el : 0.0, list);
    memset(f, 0, sizeof *f);
    nfw = 0;
}

WEAK void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

WEAK void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

enum { W_UNRENDERABLE, W_LEVEL, W_LAYER, W_VIEW_FORMAT, W_BLOCK_VIEW, W_BLOCK_LAYERS, W_CLEAR_TARGET, W_CLEAR_MASK, W_STENCIL_MASK, W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

static void state_reset(VkExec *ex) {
    memset(ex, 0, sizeof *ex);
    ex->st_vmask[0] = ex->st_vmask[1] = ex->st_wmask[0] = ex->st_wmask[1] = 0xff;
    ex->depth_range[1] = 1.0f;
    vk_pipe_state_defaults(&ex->pipe);
    ex->barrier_at = -1;
}


static uint32_t level_extent(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1;
}

static int view_format(const tex_t *t, const view_t *v, int blocks, VkFormat *out) {
    TexVkFormat a, b;
    int bw, bh, vbw, vbh;
    if (!tex_vk_format(t->d.format, &a) || a.vk != a.ideal || !tex_vk_format(v->format, &b) || b.vk != b.ideal ||
        b.depth || b.stencil)
        return 0;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
    if (!bpb || bpb != vbpb || (!blocks && (bw != 1 || bh != 1)) || vbw != 1 || vbh != 1) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, b.vk, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    *out = b.vk;
    return 1;
}

typedef struct { VkImage bc, img; VmaAllocation alloc; VkFormat fmt; uint32_t level, w, h, d; } BcStandIn;
#define BC_STANDINS 64
static BcStandIn bcs[BC_STANDINS];
static int nbcs;
static PlatMutex bcs_lock = PLAT_MUTEX_INIT;

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, uint32_t layers) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                           VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static int bc_standin(const VkTexImage *img, uint32_t level, VkFormat fmt, BcStandIn *out) {
    static int warned;
    plat_mutex_lock(&bcs_lock);
    int full = nbcs == BC_STANDINS;
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == img->image && bcs[i].level == level && bcs[i].fmt == fmt) {
            *out = bcs[i];
            plat_mutex_unlock(&bcs_lock);
            return 1;
        }
    plat_mutex_unlock(&bcs_lock);
    if (full) {
        if (!warned++) rt_log("vk: more than %d 3D BC render targets: the others are left out", BC_STANDINS);
        return 0;
    }
    uint32_t lw = level_extent(img->width, level), lh = level_extent(img->height, level);
    BcStandIn s = {img->image, VK_NULL_HANDLE, NULL, fmt, level, (lw + 3) / 4, (lh + 3) / 4,
                   level_extent(img->depth, level)};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {s.w, s.h, 1}, .mipLevels = 1, .arrayLayers = s.d, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, &s.img, &s.alloc, NULL) != VK_SUCCESS) {
        if (!warned++) rt_log("vk: stand-in for a 3D BC render target could not be created: left out");
        return 0;
    }
    VkCommandBuffer cb = vk_upload_begin();
    image_barrier(cb, s.img, VK_IMAGE_LAYOUT_UNDEFINED, s.d);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s.d},
                      .extent = {lw, lh, s.d}};
    vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, s.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vk_upload_end();
    plat_mutex_lock(&bcs_lock);
    bcs[nbcs++] = s;
    plat_mutex_unlock(&bcs_lock);
    rt_log("vk: 3D BC render target %ux%ux%u level %u drawn through a %ux%u x %u stand-in (format %d)", img->width,
           img->height, img->depth, level, s.w, s.h, s.d, (int)fmt);
    *out = s;
    return 1;
}

void vk_exec_bc_gone(VkImage image) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == image) {
            vk_defer_image(bcs[i].img, bcs[i].alloc);
            bcs[i--] = bcs[--nbcs];
        }
    plat_mutex_unlock(&bcs_lock);
}

static void bc_copy_back(VkExec *ex, const VkExecTarget *t) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++) {
        if (bcs[i].img != t->image) continue;
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->base_layer, t->layers},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, bcs[i].level, 0, 1},
                          .dstOffset = {0, 0, (int32_t)t->base_layer},
                          .extent = {bcs[i].w, bcs[i].h, t->layers}};
        vk_gpu_gap_note(VK_GAP_BC_BACK, 0);
        vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, bcs[i].bc, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        ex->recorded++;
        break;
    }
    plat_mutex_unlock(&bcs_lock);
}

static int bc_target(const tex_t *t, const view_t *v, int has_v, const VkTexImage *img, VkExecTarget *o) {
    int bw, bh;
    if (img->type != VK_IMAGE_TYPE_3D || img->block_views || !nvn_fmt_bpb(t->d.format, &bw, &bh) || bw != 4 || bh != 4)
        return 0;
    VkFormat fmt;
    BcStandIn s;
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return 1;
    }
    if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt) || !bc_standin(img, level, fmt, &s)) {
        warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                  t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
        return 1;
    }
    uint32_t base = 0, layers = s.d;
    if (v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < s.d ? s.d - base : 0);
    }
    if (base >= s.d || !layers) return 1;
    if (base + layers > s.d) layers = s.d - base;
    o->image = s.img;
    o->img = img;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = 0, o->base_layer = base, o->layers = layers;
    o->width = s.w, o->height = s.h, o->samples = 1;
    o->nvn_format = v->format;
    return 1;
}

static void resolve_target(const tex_t *t, const view_t *v, int has_v, int depth_slot, VkExecTarget *o) {
    memset(o, 0, sizeof *o);
    o->tex = t;
    if (!t) return;
    const VkTexImage *img = vk_texture_get(t);
    if (img && !depth_slot && bc_target(t, v, has_v, img, o)) return;
    VkImageUsageFlags need = depth_slot ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int is3d = img && img->type == VK_IMAGE_TYPE_3D && !depth_slot;
    if (!img && vk_texture_deferred()) return;
    if (!img || !(img->usage & need) || (img->type != VK_IMAGE_TYPE_2D && !is3d)) {
        warn_once(W_UNRENDERABLE, "render target format %#x target %d (%ux%u) is not renderable here (%s): left out",
                  t->d.format, t->d.target, t->d.width, t->d.height,
                  !img ? "no image" : !(img->usage & need) ? (depth_slot ? "no depth attachment usage" : "no colour attachment usage")
                                    : "not a 2D image");
        return;
    }
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return;
    }
    uint32_t nlay = is3d ? level_extent(img->depth, level) : img->layers;
    uint32_t base = 0, layers = nlay;
    if (has_v && v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < nlay ? nlay - base : 0);
    }
    if (base >= nlay || !layers) {
        warn_once(W_LAYER, "render target layer %u of a %u-layer texture: left out", base, nlay);
        return;
    }
    if (base + layers > nlay) layers = nlay - base;
    VkFormat fmt = img->format;
    int32_t nvn_fmt = t->d.format;
    int blocks = !depth_slot && img->block_views;
    if (blocks) {
        if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt)) {
            warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                      t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
            return;
        }
        nvn_fmt = v->format;
        if (layers > 1) {
            warn_once(W_BLOCK_LAYERS, "render target format %#x: block view of %u layers, first layer only", t->d.format,
                      layers);
            layers = 1;
        }
    } else if (!depth_slot && has_v && v->set_format && v->format != t->d.format) {
        if (view_format(t, v, 0, &fmt)) nvn_fmt = v->format;
        else warn_once(W_VIEW_FORMAT, "render target view format %#x of a format %#x texture: texture format used",
                       v->format, t->d.format);
    }
    o->image = img->image;
    o->img = img;
    o->scale = img->scale ? img->scale : 1;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = depth_slot ? img->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                           : VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = level, o->base_layer = base, o->layers = layers;
    o->is3d = (uint8_t)is3d;
    o->blocks = (uint8_t)blocks;
    o->width = level_extent(img->width, level), o->height = level_extent(img->height, level);
    if (blocks) o->width = (o->width + 3) / 4, o->height = (o->height + 3) / 4;
    o->samples = img->samples ? img->samples : 1;
    o->nvn_format = nvn_fmt;
    if (!o->aspect) o->image = VK_NULL_HANDLE;
}

void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h) {
    double x1 = 0, y1 = 0;
    if (ex->has_vp) {
        x1 = ceil(fmax(ex->vp[0][0], ex->vp[0][0] + ex->vp[0][2]));
        y1 = ceil(fmax(ex->vp[0][1], ex->vp[0][1] + ex->vp[0][3]));
    }
    if ((x1 < 1 || y1 < 1) && ex->has_scissor) x1 = (double)ex->scissor.x + ex->scissor.w, y1 = (double)ex->scissor.y + ex->scissor.h;
    *w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1;
    *h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
}

static void resolve_rt(VkExec *ex) {
    if (ex->rt_resolved) return;
    ex->rt_resolved = 1;
    const p_set_rt *r = &ex->rt;
    int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 0, scale = 1;
    for (int i = 0; i < 8; i++) {
        VkExecTarget *t = &ex->color[i];
        resolve_target(i < n ? r->color[i] : NULL, &r->cview[i], i < n && r->has_cview[i], 0, t);
        ex->pass_color[i] = t->image ? t->format : VK_FORMAT_UNDEFINED;
        ex->pipe.color_fmt[i] = t->image ? t->nvn_format : 0;
        if (!t->image) continue;
        if (t->width < w) w = t->width;
        if (t->height < h) h = t->height;
        if (t->layers < layers) layers = t->layers;
        if (!samples) samples = t->samples;
        if (t->scale > scale) scale = t->scale;
    }
    VkExecTarget *d = &ex->depth;
    resolve_target(r->depth, &r->dview, r->has_dview, 1, d);
    ex->pass_depth = d->image && (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pass_stencil = d->image && (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pipe.depth_fmt = d->image ? d->nvn_format : 0;
    if (d->image) {
        if (d->width < w) w = d->width;
        if (d->height < h) h = d->height;
        if (d->layers < layers) layers = d->layers;
        if (!samples) samples = d->samples;
        if (d->scale > scale) scale = d->scale;
    }
    ex->pass_noatt = w == UINT32_MAX;
    if (ex->pass_noatt) {
        vk_exec_noatt_dims(ex, &w, &h);
        layers = 1;
        static int logged;
        if (logged < 8 && ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != w || ex->vp[0][3] != h))
            logged++, rt_log("vk_exec: pass without attachments %ux%u for viewport %g,%g %gx%g", w, h, ex->vp[0][0],
                             ex->vp[0][1], ex->vp[0][2], ex->vp[0][3]);
    }
    ex->pass_w = w, ex->pass_h = h, ex->pass_layers = layers;
    ex->pass_scale = ex->pass_noatt ? 1 : scale;
    ex->pass_ncolor = (uint32_t)n;
    ex->pass_samples = samples ? samples : 1;
    ex->pipe.samples = (uint8_t)ex->pass_samples;
}

static VkImageView view_of(VkExec *ex, const VkExecTarget *t) {
    if (t->img && t->img->image == t->image && !t->img->gone)
        return vk_exec_target_view(t->img, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, t->blocks);
    ViewEnt *v = ex->views;
    for (int i = 0; i < ex->nviews; i++)
        if (v[i].image == t->image && v[i].format == t->format && v[i].type == t->view_type && v[i].aspect == t->aspect &&
            v[i].level == t->level && v[i].base == t->base_layer && v[i].layers == t->layers)
            return v[i].view;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
                                .viewType = t->view_type, .format = t->format,
                                .subresourceRange = {t->aspect, t->level, 1, t->base_layer, t->layers}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    if (t->blocks) ci.pNext = &use;
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
        rt_fatal("vk: render target view (format %d, %ux%u) could not be created", (int)t->format, t->width, t->height);
    if (ex->nviews == ex->cap_views) {
        ex->cap_views = ex->cap_views ? ex->cap_views * 2 : 16;
        ex->views = realloc(ex->views, sizeof(ViewEnt) * (size_t)ex->cap_views);
        if (!ex->views) rt_fatal("vk: out of memory");
        v = ex->views;
    }
    v[ex->nviews++] = (ViewEnt){t->image, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, view};
    return view;
}

typedef struct { int n; VkImageView v[]; } ViewList;

static void free_views(void *arg) {
    ViewList *l = arg;
    VkDevice dev = vk_ctx()->device;
    for (int i = 0; i < l->n; i++) vkDestroyImageView(dev, l->v[i], NULL);
    free(l);
}

uint64_t vk_exec_steady_passes;

static TfrTarget tfr_of(const VkExecTarget *t) {
    return (TfrTarget){(uint64_t)(uintptr_t)t->image, t->level, t->base_layer, t->img ? t->img->addr : 0};
}

static int steady_on(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_PIPE_SKIP_STEADY");
        rt_log("vk: a pass whose targets the game has rendered in %d frames or more counts as per-frame after a "
               "pause too, so its draws are skipped, not waited for, while a pipeline builds: %s (NX2SARIA_PIPE_SKIP_STEADY)",
               TFR_STEADY, on ? "on" : "off");
    }
    return on;
}

int vk_exec_targets_recent(VkExec *ex, int any) {
    resolve_rt(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    TfrTarget c[8], d = tfr_of(&ex->depth);
    for (int i = 0; i < 8; i++) c[i] = tfr_of(&ex->color[i]);
    int by_steady = 0, r = tfr_pass_recent(c, 8, &d, stamp, any, steady_on(), &by_steady);
    vk_exec_steady_passes += (uint64_t)by_steady;
    return r;
}

void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap) {
    resolve_rt(ex);
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < 9 && n < cap; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        TfrTarget ft = tfr_of(t);
        const TargetFrames *e = tfr_find(&ft, 0);
        int w = snprintf(buf + n, cap - n, "%s%c%d fmt %d %ux%u L%u seen %llu/%llu in %u frames", n ? "; " : "",
                         i < 8 ? 'c' : 'd', i < 8 ? i : 0, (int)t->format, t->width, t->height, t->base_layer,
                         (unsigned long long)(e ? e->last : 0), (unsigned long long)(e ? e->prev : 0),
                         e && e->owner == ft.owner ? e->n : 0);
        if (w < 0) break;
        n += (size_t)w;
    }
    if (!n) snprintf(buf, cap, "no targets");
}


static int has_image(const VkImage *list, int n, VkImage img) {
    for (int i = 0; i < n; i++)
        if (list[i] == img) return 1;
    return 0;
}

static VkImageMemoryBarrier2 layout_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                   .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
}

static int barrier_game(void) {
    static int mode = -1;
    if (mode < 0) {
        const VkCtx *c = vk_ctx();
        int nvidia = c && c->props.vendorID == 0x10de;
        mode = rt_knob("BARRIER_GAME", nvidia ? "1" : "0");
        if (mode)
            rt_log("vk_exec: NX2SARIA_BARRIER_GAME=%s: passes and dispatches are ordered by the game's barriers",
                   mode == 2 ? "ab" : "1");
    }
    return RT_KNOB_ON(mode);
}

enum { BS_FULL, BS_GAME, BS_PASS_FULL, BS_PASS_ATT, BS_PASS_NONE, BS_DISP_FULL, BS_DISP_NONE, BS_N };
static uint64_t bstat[BS_N];
static const char *const bs_name[BS_N] = {"replay full", "game", "pass full", "pass attachment", "pass none",
                                          "dispatch full", "dispatch none"};
enum { BR_OTHER, BR_GAME, BR_PASS, BR_DISPATCH };

static void barrier_to_why(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant, int why);
static void barrier_to(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant) {
    barrier_to_why(ex, want, wasp, nwant, BR_OTHER);
}

static void attachment_barrier(VkExec *ex) {
    const VkPipelineStageFlags2 st = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = st,
                           .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           .dstStageMask = st,
                           .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
}

static void barrier_to_why(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant, int why) {
    vk_exec_pass_end(ex);
    if (why == BR_OTHER) ex->bar_dirty = 1;
    VkImageMemoryBarrier2 ib[18];
    uint32_t n = 0;
    for (int i = 0; i < ex->nfbl; i++)
        if (!has_image(want, nwant, ex->fbl_img[i]))
            ib[n++] = layout_barrier(ex->fbl_img[i], ex->fbl_aspect[i], VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
                                     VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < nwant; i++)
        if (!has_image(ex->fbl_img, ex->nfbl, want[i]))
            ib[n++] = layout_barrier(want[i], wasp[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
    for (int i = 0; i < nwant; i++) ex->fbl_img[i] = want[i], ex->fbl_aspect[i] = wasp[i];
    ex->nfbl = nwant;
    if (ex->barrier_at == ex->recorded && !n) return;
    if (!n && !ex->bar_dirty && ((why == BR_PASS && !ex->bar_dispatches) || (why == BR_DISPATCH && !ex->bar_passes)) &&
        barrier_game()) {
        if (why == BR_DISPATCH) {
            bstat[BS_DISP_NONE]++;
        } else if (ex->bar_passes) {
            attachment_barrier(ex);
            bstat[BS_PASS_ATT]++;
        } else {
            bstat[BS_PASS_NONE]++;
        }
        return;
    }
    bstat[why == BR_GAME ? BS_GAME : why == BR_PASS ? BS_PASS_FULL : why == BR_DISPATCH ? BS_DISP_FULL : BS_FULL]++;
    if (why != BR_OTHER) ex->bar_dirty = 0;
    ex->bar_passes = ex->bar_dispatches = 0;
    vk_exec_frame.fb_layouts += n;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = n, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->barrier_at = ++ex->recorded;
}

void vk_exec_barrier(VkExec *ex) { barrier_to(ex, NULL, NULL, 0); }

void vk_exec_barrier_dispatch(VkExec *ex) {
    barrier_to_why(ex, NULL, NULL, 0, BR_DISPATCH);
    ex->bar_dispatches++;
}

void vk_exec_barrier_log(uint64_t frame) {
    static uint64_t last;
    if (!(rt_trace & RT_TRACE_PERF) || frame % 600) return;
    double f = frame > last ? (double)(frame - last) : 1.0;
    last = frame;
    char s[320];
    int o = 0;
    for (int k = 0; k < BS_N; k++) {
        o += snprintf(s + o, sizeof s - o, "%s%s %.1f", k ? ", " : "", bs_name[k], bstat[k] / f);
        bstat[k] = 0;
    }
    rt_log("vk_exec: frame %llu: barriers per frame: %s", (unsigned long long)frame, s);
}

void vk_exec_pass_begin(VkExec *ex) {
    if (ex->in_pass) return;
    resolve_rt(ex);
    if (vk_texture_deferred()) return;
    if (pass_hooks_due(rw_chunk != NULL, rw_chunk ? rw_chunk->idx : 0,
                       rw_chunk ? __atomic_load_n(&rw_chunk->g->hooks, __ATOMIC_RELAXED) : 0)) {
        if (!rw_chunk) vk_exec_hooks_wait();
        vk_contact_pass_begin(ex);
        vk_dlaa_pass(ex);
    }
    vk_dlaa_retarget(ex);
    VkImage want[9];
    VkImageAspectFlags wasp[9];
    int nwant = 0;
    uint32_t fbl = 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || !t->image || !t->img || !t->img->fb_learned) continue;
        fbl |= 1u << i;
        if (has_image(want, nwant, t->image)) continue;
        want[nwant] = t->image, wasp[nwant++] = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : t->img->aspect;
    }
    barrier_to_why(ex, want, wasp, nwant, BR_PASS);
    ex->bar_passes++;
    ex->fbl_mask = fbl;
    ex->fb_dirty = 0;
    ex->fb_read = 0;
    ex->pass_serial++;
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    for (uint32_t i = 0; i < ex->pass_ncolor; i++) {
        const VkExecTarget *t = &ex->color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!t->image) continue;
        ca[i].imageView = view_of(ex, t);
        ca[i].imageLayout = fbl >> i & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;    }
    const VkExecTarget *d = &ex->depth;
    if (d->image) {
        VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view_of(ex, d),
                                       .imageLayout = fbl >> 8 & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                   : VK_IMAGE_LAYOUT_GENERAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) da = a;
        if (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = a;
    }
    VkPassMark *m = ex->mark = rw_chunk ? NULL : vk_pass_mark_begin(ex->cb, VK_MARK_PASS, ex->fence);
    if (m) {
        m->step = ex->mark_passes++;
        m->w = ex->pass_w, m->h = ex->pass_h, m->d = ex->pass_layers;
        m->layer = ex->depth.image ? ex->depth.base_layer : 0;
        m->vw = m->vh = 0;
        m->ncolor = (uint8_t)(ex->pass_ncolor < 8 ? ex->pass_ncolor : 8);
        for (int i = 0; i < m->ncolor; i++) m->color[i] = ex->pass_color[i];
        m->depth = ex->pass_depth ? ex->pass_depth : ex->pass_stencil;
        vk_exec_draw_shaders(&m->first[0], &m->first[1]);
        ex->mark_draws0 = vk_exec_frame.draws;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {ex->pass_w, ex->pass_h}},
                          .layerCount = ex->pass_layers, .colorAttachmentCount = ex->pass_ncolor,
                          .pColorAttachments = ca,
                          .pDepthAttachment = ex->pass_depth ? &da : NULL,
                          .pStencilAttachment = ex->pass_stencil ? &sa : NULL};
    vkCmdBeginRendering(ex->cb, &ri);
    vk_exec_frame.passes++;
    ex->recorded++;
    ex->in_pass = 1;
    if (rw_chunk && !rw_chunk->idx) __atomic_store_n(&rw_chunk->g->hooks, 1, __ATOMIC_RELEASE);
}

void vk_exec_pass_end(VkExec *ex) {
    if (!ex->in_pass) return;
    vk_exec_oq_pass_end(ex);
    vkCmdEndRendering(ex->cb);
    ex->recorded++;
    ex->in_pass = 0;
    if (ex->mark) {
        uint64_t d = vk_exec_frame.draws, d0 = ex->mark_draws0;
        ex->mark->draws = (uint32_t)(d >= d0 ? d - d0 : d);
        if (ex->mark->draws) vk_exec_draw_shaders(&ex->mark->last[0], &ex->mark->last[1]);
        vk_pass_mark_end(ex->cb, ex->mark);
        ex->mark = NULL;
    }
    for (int i = 0; i < ex->pass_ncolor && i < 8; i++)
        if (ex->color[i].image && ex->color[i].img && ex->color[i].img->image != ex->color[i].image)
            bc_copy_back(ex, &ex->color[i]);
    if (vk_dump_armed) vk_dump_pass_end(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) vk_texture_gpu_written(ex->color[i].tex);
    if (ex->depth.image) vk_texture_gpu_written(ex->depth.tex);
    int lk = vk_exec_shared();
    if (lk) plat_mutex_lock(&tfr_lock);
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) {
            TfrTarget ft = tfr_of(&ex->color[i]);
            tfr_note(&ft, stamp);
        }
    if (ex->depth.image) {
        TfrTarget ft = tfr_of(&ex->depth);
        tfr_note(&ft, stamp);
    }
    if (lk) plat_mutex_unlock(&tfr_lock);
    vk_dlaa_pass_end(ex);
}

VkCommandBuffer vk_exec_inline_begin(int *reopen) {
    VkExec *ex = x_cur;
    *reopen = 0;
    if (!ex || !ex->cb) return VK_NULL_HANDLE;
    *reopen = ex->in_pass;
    vk_exec_barrier(ex);
    return ex->cb;
}

void vk_exec_inline_end(int reopen) {
    VkExec *ex = x_cur;
    ex->recorded++;
    if (reopen) vk_exec_pass_begin(ex);
}

void vk_exec_probe_inline(VkCommandBuffer cb, uint64_t fence) {
    if (!cb && x_cur) replay_counts_flush();
    X.cb = cb, X.fence = fence;
    x_cur = cb != VK_NULL_HANDLE ? &X : NULL;
}

typedef struct { VkBuffer b; VmaAllocation a; } InlineBuf;
static void free_inline_buf(void *arg) {
    InlineBuf *t = arg;
    vmaDestroyBuffer(vk_ctx()->vma, t->b, t->a);
    free(t);
}

VkBuffer vk_exec_inline_buffer(VkDeviceSize size) {
    InlineBuf *t = calloc(1, sizeof *t);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!t || vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &t->b, &t->a, NULL) != VK_SUCCESS) { free(t); return VK_NULL_HANDLE; }
    vk_completion_add(x_cur->fence, free_inline_buf, t);
    return t->b;
}

VkRect2D vk_exec_scissor(const VkExec *ex) {
    int64_t w = ex->pass_w, h = ex->pass_h;
    if (!ex->has_scissor) return (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
    int64_t k = ex->pass_scale > 1 ? ex->pass_scale : 1;
    int64_t x0 = ex->scissor.x * k, y0 = ex->scissor.y * k;
    int64_t x1 = x0 + (int64_t)ex->scissor.w * k, y1 = y0 + (int64_t)ex->scissor.h * k;
    float sx, sy;
    if (vk_dlaa_target_scale(ex, &sx, &sy)) {
        x0 = (int64_t)floor(ex->scissor.x * (double)sx), y0 = (int64_t)floor(ex->scissor.y * (double)sy);
        x1 = (int64_t)ceil(((double)ex->scissor.x + ex->scissor.w) * sx);
        y1 = (int64_t)ceil(((double)ex->scissor.y + ex->scissor.h) * sy);
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}


static int whole_target(const VkExec *ex, const VkExecTarget *t) {
    if (ex->in_pass || t->is3d || t->blocks || t->width != ex->pass_w || t->height != ex->pass_h) return 0;
    if (!ex->has_scissor) return 1;
    const p_scissor *s = &ex->scissor;
    return s->x <= 0 && s->y <= 0 && (int64_t)s->x + s->w >= t->width && (int64_t)s->y + s->h >= t->height;
}

static void clear_attachment(VkExec *ex, const VkClearAttachment *a) {
    vk_exec_pass_begin(ex);
    VkClearRect cr = {vk_exec_scissor(ex), 0, ex->pass_layers};
    if (!cr.rect.extent.width || !cr.rect.extent.height) return;
    uint32_t bit = a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT ? 1u << a->colorAttachment : 0x100u;
    vk_exec_feedback_war(ex, bit);
    vkCmdClearAttachments(ex->cb, 1, a, 1, &cr);
    ex->recorded++;
    ex->fb_dirty |= bit;
}

static void clear_cond_log(const VkExec *ex, const char *what, int index) {
    static int on = -1, n;
    if (on < 0) on = rt_knob("LOG_RCOND", "0");
    if (on == 2 && index < 0 && ex->has_render_cond && ex->depth.width >= 1600) {
        char why[256];
        vk_exec_cond_why(&ex->render_cond, why, sizeof why);
        rt_log("SEQ depth clear %#llx %s; %s", ex->depth.img ? (unsigned long long)ex->depth.img->addr : 0ull,
               ex->render_disabled ? "skipped" : "RUN", why);
        return;
    }
    if (!on || !ex->has_render_cond || n >= 400) return;
    n++;
    const VkExecTarget *t = index < 0 ? &ex->depth : index < (int)ex->pass_ncolor ? &ex->color[index] : NULL;
    rt_log("vk: %s clear %d under the condition at %#llx (mode %d): %s, target %ux%u", what, index,
           (unsigned long long)ex->render_cond.addr, ex->render_cond.mode, ex->render_disabled ? "skipped" : "run",
           t ? t->width : 0, t ? t->height : 0);
}

static void clear_color(VkExec *ex, const p_clear_color *c) {
    vk_exec_frame.clears++;
    clear_cond_log(ex, "colour", c->index);
    if (ex->has_render_cond) ex->render_disabled ? vk_cond_clears_skip++ : vk_cond_clears_run++;
    if (ex->render_disabled) return;
    resolve_rt(ex);
    if (vk_texture_deferred()) return;
    int i = c->index;
    if (i < 0 || i >= (int)ex->pass_ncolor || !ex->color[i].image) {
        warn_once(W_CLEAR_TARGET, "colour clear of target %d, which is not bound or not renderable: skipped", i);
        return;
    }
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if (mask != 0xf) {
        warn_once(W_CLEAR_MASK, "colour clear with channel mask %#x is not replayed: skipped", mask);
        return;
    }
    const VkExecTarget *t = &ex->color[i];
    VkClearColorValue cv;
    memcpy(cv.float32, c->rgba, sizeof cv.float32);
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, 1, t->base_layer, t->layers};
        vk_gpu_gap_note(VK_GAP_CLEAR_TARGET, (uint64_t)t->width * t->height);
        vkCmdClearColorImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)i};
    a.clearValue.color = cv;
    clear_attachment(ex, &a);
}

static void clear_ds(VkExec *ex, const p_clear_ds *c) {
    clear_cond_log(ex, "depth", -1);
    if (ex->has_render_cond) ex->render_disabled ? vk_cond_clears_skip++ : (vk_cond_clears_run++, vk_cond_depth_run++);
    if (ex->render_disabled) return;
    resolve_rt(ex);
    if (vk_texture_deferred()) return;
    const VkExecTarget *t = &ex->depth;
    if (!t->image) return;
    VkImageAspectFlags asp = 0;
    if (c->depth_mask && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t sm = (uint32_t)c->stencil_mask & 0xff;
    if (sm && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (sm == 0xff) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else warn_once(W_STENCIL_MASK, "stencil clear with mask %#x is not replayed: skipped", sm);
    }
    if (!asp) return;
    float d = c->depth < 0.0f ? 0.0f : c->depth > 1.0f ? 1.0f : c->depth;
    VkClearDepthStencilValue v = {d, (uint32_t)c->stencil & 0xff};
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {asp, t->level, 1, t->base_layer, t->layers};
        vk_gpu_gap_note(VK_GAP_CLEAR_TARGET, (uint64_t)t->width * t->height);
        vkCmdClearDepthStencilImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = asp};
    a.clearValue.depthStencil = v;
    clear_attachment(ex, &a);
}


static void set_rt(VkExec *ex, const p_set_rt *r) {
    if (!memcmp(&ex->rt, r, sizeof *r)) return;
    vk_exec_pass_end(ex);
    ex->rt = *r;
    ex->rt_resolved = 0;
}

static int split_n = -1, split_idle, split_in_pass, split_draws, split_at, part_start;
static uint64_t split_ns, part_t0;
static uint64_t n_parts, n_subs, n_idle_parts, n_pass_splits, n_cuts;

static struct {
    int on;
    int live0;
    uint64_t wrote0;
    uint64_t n, early, why[4];
} TL;
int vk_exec_tail_results(void);
void vk_sync_tail_stats(uint64_t *tails, uint64_t *waits, double *ms);
int vk_gpu_passes_capturing(void);
int vk_mesh_shader_native(void);
int vk_exec_tail(void) { return TL.on; }

static int tail_enabled(void) {
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("NX2SARIA_TAIL_EARLY");
        if (on)
            rt_log("vk: the write-backs go before a frame's tail, and the game's waits end there (NX2SARIA_TAIL_EARLY)");
    }
    return on;
}

int vk_exec_split_now(VkExec *ex) {
    int in_pass = ex->in_pass;
    if (in_pass) vk_exec_pass_end(ex);
    VkCommandBuffer cb = vk_sync_cut();
    if (!cb) return 0;
    ex->cb = cb;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_cuts++;
    return 1;
}

static int split_wanted(void) {
    if (split_draws >= split_at) return 1;
    return split_idle && split_draws && !part_start && vk_sync_fence_done() >= vk_sync_issued();
}

static void maybe_split(VkExec *ex) {
    if (split_n < 0) {
        split_n = cfg_int("NX2SARIA_SPLIT_DRAWS");
        if (split_n < 0) split_n = 0;
        int us = cfg_int("NX2SARIA_SPLIT_US");
        split_ns = us > 0 ? (uint64_t)us * 1000 : 0;
        split_idle = split_n && cfg_bool("NX2SARIA_SPLIT_IDLE");
        split_in_pass = split_n && cfg_bool("NX2SARIA_SPLIT_IN_PASS");
        rt_log("vk: replay command buffer split every %d draws %s, parts of at least %d us%s%s", split_n,
               split_in_pass ? "(inside passes too)" : "between passes", us > 0 ? us : 0, split_n ? "" : " (off)",
               split_idle ? "; a first part at once while the GPU is idle" : "");
        split_at = split_n ? split_n : INT_MAX;
    }
    if (!split_n || TL.on) return;
    if (Q.drained < Q.out) {
        if (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < Q.out) return;
        group_drain(ex, PW_SPLIT);
    }
    int idle = !ex->in_pass && split_idle && split_draws && !part_start && vk_sync_fence_done() >= vk_sync_issued();
    if (!idle) {
        if (ex->in_pass && !split_in_pass) return;
        if (split_draws < split_n) return;
        if (split_ns && kern_now_ns() - part_t0 < split_ns) {
            split_at = split_draws + 8;
            return;
        }
    }
    split_draws = 0;
    split_at = split_n;
    if (ex->recorded == part_start || !vk_exec_pool_split(ex->fence)) return;
    if (idle) n_idle_parts++;
    if (ex->in_pass) vk_exec_pass_end(ex), n_pass_splits++;
    ex->cb = vk_sync_split();
    part_start = ex->recorded;
    part_t0 = kern_now_ns();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_parts++;
}

#define CONT_MAX 36
#define EVO_QUEUES 16
typedef struct Cont {
    struct Cont *next;
    uint64_t *h;
    int n, i;
    const op_hdr *at;
    uint64_t since;
    int has_st;
    uint8_t *st;
} Cont;
static struct { uintptr_t q; Cont *head, *tail; } evq[EVO_QUEUES];
static EvoPeek evq_peek[EVO_QUEUES];
_Static_assert(sizeof(EvoWait) == sizeof(p_wait_event) && offsetof(EvoWait, val) == offsetof(p_wait_event, val) &&
                   offsetof(EvoWait, mode) == offsetof(p_wait_event, mode) &&
                   offsetof(EvoWait, value) == offsetof(p_wait_event, value),
               "vk_evo_peek.h EvoWait must have p_wait_event's layout");
static int evo_on = -1, evo_in;
static uintptr_t evo_queue;
static int evo_pending;
static uint64_t evo_held, evo_resumed, evo_forced, evo_switches;
int vk_exec_held(void) { return __atomic_load_n(&evo_pending, __ATOMIC_RELAXED); }
#define ST_LO offsetof(VkExec, rt)
#define ST_HI offsetof(VkExec, views)

static const op_hdr *walk(VkExec *ex, const op_hdr *h, int depth);

#define EVT_KINDS 512
static struct {
    int on;
    PlatMutex lock;
    char buf[2048];
    int n, over;
    const void *ev[64];
    int nev;
    char *kind[EVT_KINDS];
    int nkind;
    uint64_t line_first;
    int line_n;
    char line[512];
    int line_len;
} ET = {.on = -1, .lock = PLAT_MUTEX_INIT};

static int evt_on(void) {
    if (ET.on < 0) ET.on = rt_knob("EVO_TRACE", "0");
    return ET.on;
}

void vk_exec_evt(char what, const void *event, uint32_t value) {
    if (!evt_on()) return;
    char t[40];
    int e = -1;
    plat_mutex_lock(&ET.lock);
    if (event) {
        for (e = 0; e < ET.nev && ET.ev[e] != event; e++) {}
        if (e == ET.nev && ET.nev < 64) ET.ev[ET.nev++] = event;
    }
    if (what == 's' || what == 'c') snprintf(t, sizeof t, value < 16 ? "%c%d=%u " : "%c%d=n ", what, e, value);
    else if (what == '|') snprintf(t, sizeof t, "|%d.%u ", e, value);
    else if (event) snprintf(t, sizeof t, "%c%d ", what, e);
    else snprintf(t, sizeof t, "%c ", what);
    for (const char *s = t; *s; s++) {
        if (ET.n >= (int)sizeof ET.buf - 1) { ET.over = 1; break; }
        ET.buf[ET.n++] = *s;
    }
    plat_mutex_unlock(&ET.lock);
}

void vk_exec_evt_frame(uint64_t frame) {
    if (!evt_on()) return;
    plat_mutex_lock(&ET.lock);
    ET.buf[ET.n] = 0;
    int k = 0;
    while (k < ET.nkind && strcmp(ET.kind[k], ET.buf)) k++;
    if (k == ET.nkind && ET.nkind < EVT_KINDS) {
        size_t len = strlen(ET.buf) + 1;
        if ((ET.kind[k] = malloc(len))) {
            memcpy(ET.kind[k], ET.buf, len);
            ET.nkind++;
            rt_log("evo: order %d first at frame %llu%s: %s", k, (unsigned long long)frame, ET.over ? " (cut)" : "", ET.buf);
        }
    }
    if (!ET.line_n) ET.line_first = frame, ET.line_len = 0;
    ET.line_len += snprintf(ET.line + ET.line_len, sizeof ET.line - (size_t)ET.line_len, " %d", k);
    if (++ET.line_n == 60 || ET.line_len > (int)sizeof ET.line - 16) {
        rt_log("evo: frames %llu-%llu:%s", (unsigned long long)ET.line_first, (unsigned long long)frame, ET.line);
        ET.line_n = 0;
    }
    ET.n = 0, ET.over = 0;
    plat_mutex_unlock(&ET.lock);
}

static int evo_enabled(void) {
    if (evo_on < 0) {
        const char *e = cfg_get("NX2SARIA_EVENT_ORDER");
        evo_on = !(e && !strcmp(e, "0"));
        if (evo_on && gfx_replay_async()) {
            evo_on = 0;
            rt_log("vk: event order across queues off: the replay runs on its own thread (NX2SARIA_REPLAY_ASYNC)");
        } else rt_log("vk: event order across queues %s (NX2SARIA_EVENT_ORDER)", evo_on ? "on" : "off");
    }
    return evo_on;
}

static int evq_slot(uintptr_t q) {
    int free_i = -1;
    for (int i = 0; i < EVO_QUEUES; i++) {
        if (evq[i].q == q) return i;
        if (free_i < 0 && !evq[i].head) free_i = i;
    }
    if (free_i >= 0) evq[free_i].q = q;
    return free_i;
}

static Cont *cont_new(const uint64_t *h, int n, int from, const op_hdr *at) {
    Cont *c = calloc(1, sizeof *c);
    if (!c || !(c->h = calloc((size_t)(n - from) + 1, sizeof *c->h))) rt_fatal("vk: out of memory (held submission)");
    for (int k = from; k < n; k++) {
        const op_hdr *x = nvn_op_first(h[k]);
        uint64_t bytes = 0;
        for (; x->op != OP_END; x = nvn_op_next(x)) bytes += x->size;
        bytes += x->size;
        uint8_t *copy = malloc(bytes);
        if (!copy) rt_fatal("vk: out of memory (held submission, %llu bytes)", (unsigned long long)bytes);
        memcpy(copy, (const void *)(uintptr_t)h[k], bytes);
        c->h[k - from] = (uintptr_t)copy;
        if (k == from && at) c->at = (const op_hdr *)(const void *)(copy + ((const uint8_t *)at - (const uint8_t *)(uintptr_t)h[k]));
    }
    c->n = n - from;
    c->since = vk_exec_submit_seq;
    return c;
}

static void cont_free(Cont *c) {
    for (int k = 0; k < c->n; k++) free((void *)(uintptr_t)c->h[k]);
    free(c->h), free(c->st), free(c);
}

static void peek_publish(int s) {
    const Cont *c = evq[s].head;
    EvoPeekCopy v = {0};
    if (c) {
        v.has = 1, v.since = c->since;
        if (c->at) v.has_at = 1, v.w = *(const EvoWait *)nvn_op_payload(c->at);
    }
    evo_peek_publish(&evq_peek[s], &v);
}

static void cont_push(int slot, Cont *c) {
    if (evq[slot].tail) evq[slot].tail->next = c;
    else evq[slot].head = c;
    evq[slot].tail = c;
    __atomic_store_n(&evo_pending, evo_pending + 1, __ATOMIC_RELAXED);
    if (evq[slot].head == c) peek_publish(slot);
}

static void st_swap(VkExec *ex, uint8_t *buf) {
    static uint8_t tmp[ST_HI - ST_LO];
    memcpy(tmp, (uint8_t *)ex + ST_LO, sizeof tmp);
    memcpy((uint8_t *)ex + ST_LO, buf, sizeof tmp);
    memcpy(buf, tmp, sizeof tmp);
    ex->rt_resolved = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->tex_serial++;
    vk_exec_draw_new_cb();
}

static int cont_ready(const Cont *c) {
    return !c->at || vk_exec_event_met(nvn_op_payload(c->at)) || vk_exec_submit_seq - c->since > CONT_MAX;
}

static struct { uint64_t n, ns, late; } GW;

static const void *gate_first_wait(const op_hdr *h) {
    for (int k = 0; k < 256; k++) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) return NULL;
        const void *a = nvn_op_payload(h);
        switch (h->op) {
        case OP_WAIT_EVENT: return a;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_BEGIN: case OP_SET_RT: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM:
        case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
        case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET:
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_COPY_STRIDE: case OP_BLEND_COLOR: case OP_DEPTH_CLAMP:
        case OP_RASTER_DISCARD: case OP_PRIM_RESTART: case OP_PUSH_CC:
            break;
        default: return NULL;
        }
        h = nvn_op_next(h);
    }
    return NULL;
}

void vk_exec_gate_wait(const uint64_t *handles, int n) {
    static int64_t cap_ns = -1;
    if (cap_ns < 0) {
        int ms = cfg_int("NX2SARIA_GATE_WAIT_MS");
        cap_ns = ms > 0 ? (int64_t)ms * 1000000 : 0;
    }
    if (!cap_ns || n < 1 || !handles[0] || !gfx_submit_queue || !evo_enabled()) return;
    const void *w = gate_first_wait(nvn_op_first(handles[0]));
    if (!w || vk_exec_event_holds_now(w) || !vk_exec_event_cpu_known((const void *)((const p_wait_event *)w)->val)) return;
    uint64_t t0 = kern_now_ns(), now = t0;
    for (unsigned i = 0; !vk_exec_event_holds_now(w); i++) {
        if ((i & 15) == 15 && (now = kern_now_ns()) - t0 >= (uint64_t)cap_ns) {
            __atomic_add_fetch(&GW.late, 1, __ATOMIC_RELAXED);
            break;
        }
        if (i < 20000) plat_pause();
        else plat_yield();
    }
    __atomic_add_fetch(&GW.ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
    if (__atomic_add_fetch(&GW.n, 1, __ATOMIC_RELAXED) == 600) {
        uint64_t wn = __atomic_exchange_n(&GW.n, 0, __ATOMIC_RELAXED);
        uint64_t wns = __atomic_exchange_n(&GW.ns, 0, __ATOMIC_RELAXED);
        uint64_t late = __atomic_exchange_n(&GW.late, 0, __ATOMIC_RELAXED);
        if (rt_trace & RT_TRACE_PERF)
            rt_log("perf: gate: %llu submissions waited for the CPU's signal before their copies, %.2f ms on average; "
                   "%llu waited %lld ms without it and were held", (unsigned long long)wn, (double)wns / (double)wn / 1e6,
                   (unsigned long long)late, (long long)(cap_ns / 1000000));
    }
}

int vk_exec_held_ready(void) {
    for (int s = 0; __atomic_load_n(&evo_pending, __ATOMIC_RELAXED) && s < EVO_QUEUES; s++) {
        EvoPeekCopy c;
        if (evo_peek_read(&evq_peek[s], &c) &&
            evo_peek_ready(&c, vk_exec_event_met_now, vk_exec_submit_seq_now(), CONT_MAX))
            return 1;
    }
    return 0;
}

static int held_sync(void) {
    static int on = -1;
    if (on < 0) on = rt_knob("HELD_SYNC", "1");
    return on;
}

static int cont_walk(VkExec *ex, Cont *c) {
    if (c->at && !vk_exec_event_met(nvn_op_payload(c->at))) {
        evo_forced++;
        vk_exec_evt('F', NULL, 0);
        static int logged;
        if (logged++ < 8)
            rt_log("vk: event order: a held submission goes on after %llu submissions without its signal",
                   (unsigned long long)(vk_exec_submit_seq - c->since));
    }
    for (; c->i < c->n; c->i++) {
        const op_hdr *from = c->at ? nvn_op_next(c->at) : nvn_op_first(c->h[c->i]);
        c->at = NULL;
        const op_hdr *stop = walk(ex, from, 0);
        if (stop) {
            c->at = stop;
            c->since = vk_exec_submit_seq;
            return 1;
        }
    }
    return 0;
}

static void evo_resume(VkExec *ex) {
    if (evo_in || !evo_pending) return;
    vk_exec_signalled = 0;
    uintptr_t run_q = evo_queue;
    for (int progress = 1; progress;) {
        progress = 0;
        for (int s = 0; s < EVO_QUEUES; s++) {
            Cont *c = evq[s].head;
            if (!c || !cont_ready(c)) continue;
            progress = 1;
            evo_switches++;
            vk_exec_brk++;
            if (RR.on > 0) runs_break(RB_HELD);
            vk_exec_pass_end(ex);
            vk_exec_barrier(ex);
            uint8_t *mine = malloc(ST_HI - ST_LO);
            if (!mine) rt_fatal("vk: out of memory (queue state)");
            memcpy(mine, (uint8_t *)ex + ST_LO, ST_HI - ST_LO);
            if (c->has_st) st_swap(ex, c->st);
            evo_in = 1, evo_queue = evq[s].q;
            for (;;) {
                evo_resumed++;
                vk_exec_evt('r', (const void *)evq[s].q, 0);
                if (cont_walk(ex, c)) {
                    peek_publish(s);
                    vk_exec_evt('w', NULL, 0);
                    vk_exec_pass_end(ex);
                    if (!c->st && !(c->st = malloc(ST_HI - ST_LO))) rt_fatal("vk: out of memory (queue state)");
                    memcpy(c->st, (uint8_t *)ex + ST_LO, ST_HI - ST_LO);
                    c->has_st = 1;
                    break;
                }
                evq[s].head = c->next;
                if (!evq[s].head) evq[s].tail = NULL;
                peek_publish(s);
                __atomic_store_n(&evo_pending, evo_pending - 1, __ATOMIC_RELAXED);
                vk_exec_evt('d', NULL, 0);
                cont_free(c);
                c = evq[s].head;
                if (!c) break;
                c->has_st = 0;
            }
            evo_in = 0, evo_queue = run_q;
            vk_exec_pass_end(ex);
            vk_exec_barrier(ex);
            memcpy((uint8_t *)ex + ST_LO, mine, ST_HI - ST_LO);
            ex->rt_resolved = 0;
            ex->dirty = VK_EXEC_DIRTY_ALL;
            ex->tex_serial++;
            vk_exec_draw_new_cb();
            free(mine);
        }
    }
}

#define AHEAD_NS 3000000ull
#define AHEAD_OPS 400000u
static struct {
    int on, live;
    const uint64_t *handles;
    int n, i;
    const op_hdr *at[CALL_DEPTH + 1];
    int depth;
    uint64_t seq;
    uint64_t scans, ops, cut, ns, max_ns, logged_scans;
} AH = {.on = -1};

static void ahead_set_rt(VkExec *sx, const p_set_rt *r) {
    sx->rt = *r;
    int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n, samples = 0;
    for (int i = 0; i < 8; i++) {
        const tex_t *t = i < n ? r->color[i] : NULL;
        sx->pipe.color_fmt[i] = !t ? 0 : r->has_cview[i] && r->cview[i].set_format ? (int32_t)r->cview[i].format : t->d.format;
        if (t && !samples) samples = t->d.samples > 1 ? t->d.samples : 1;
    }
    sx->pipe.depth_fmt = r->depth ? r->depth->d.format : 0;
    if (r->depth && !samples) samples = r->depth->d.samples > 1 ? r->depth->d.samples : 1;
    sx->pipe.samples = (uint8_t)(samples ? samples : 1);
}

void vk_exec_ahead(VkExec *ex) {
    if (AH.on < 0) {
        AH.on = cfg_bool("NX2SARIA_PIPE_AHEAD");
        rt_log("vk: a draw that finds its pipeline missing has the rest of its submission read ahead for the stage "
               "libraries the later draws need: %s (NX2SARIA_PIPE_AHEAD)", AH.on ? "on" : "off");
    }
    if (!AH.on || !AH.live || evo_in || rw_chunk || AH.seq == vk_exec_submit_seq) return;
    AH.seq = vk_exec_submit_seq;
    uint64_t t0 = kern_now_ns();
    uint32_t ops = 0;
    static VkExec sx;
    sx = *ex;
    vk_exec_ahead_begin();
    const op_hdr *ret[CALL_DEPTH + 1];
    int depth = AH.depth, i = AH.i;
    for (int d = 0; d < depth; d++) ret[d] = AH.at[d];
    const op_hdr *h = nvn_op_next(AH.at[depth]);
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) break;
        if (++ops >= AHEAD_OPS || (!(ops & 63) && kern_now_ns() - t0 > AHEAD_NS)) {
            AH.cut++;
            break;
        }
        const void *a = nvn_op_payload(h);
        int stop = 0;
        switch (h->op) {
        case OP_END:
            if (depth > 0) h = ret[--depth];
            else if (++i < AH.n) {
                h = nvn_op_first(AH.handles[i]);
                continue;
            } else stop = 1;
            break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) {
                stop = 1;
                break;
            }
            ret[depth++] = h;
            h = nvn_op_first(((const p_jump *)a)->target);
            continue;
        case OP_WAIT_EVENT: stop = 1; break;
        case OP_SET_TEX_POOL: stop = ((const p_set_pool *)a)->pool != sx.tex_pool; break;
        case OP_SET_RT:
            if (memcmp(&sx.rt, a, sizeof sx.rt)) ahead_set_rt(&sx, a);
            break;
        case OP_DEPTH_CLAMP: sx.pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
        case OP_RASTER_DISCARD: sx.pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_TEX: case OP_BIND_IMG:
        case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
            vk_exec_ahead_op(&sx, h->op, a);
            break;
        default: break;
        }
        if (stop) break;
        h = nvn_op_next(h);
    }
    uint64_t ns = kern_now_ns() - t0;
    AH.scans++, AH.ops += ops, AH.ns += ns;
    if (ns > AH.max_ns) AH.max_ns = ns;
    if ((rt_trace & RT_TRACE_PERF) && AH.scans - AH.logged_scans >= 100) {
        rt_log("perf: looking ahead: %llu scans so far, %.0f ops and %.2f ms each (longest %.2f ms, %llu ended at their "
               "limit); %llu draws seen, %llu looked up", (unsigned long long)AH.scans, (double)AH.ops / (double)AH.scans,
               (double)AH.ns / 1e6 / (double)AH.scans, (double)AH.max_ns / 1e6, (unsigned long long)AH.cut,
               (unsigned long long)vk_exec_ahead_draws, (unsigned long long)vk_exec_ahead_lookups);
        AH.logged_scans = AH.scans;
    }
}

uint32_t vk_exec_brk;

static int op_linear(uint32_t op) {
    switch (op) {
    case OP_BEGIN: case OP_JUMP: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL:
    case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART:
    case OP_COPY_STRIDE: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND:
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
    case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_PUSH_CC: case OP_DRAW_MESH:
    case OP_DRAW_MESH_IND: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
        return 1;
    default: return 0;
    }
}

#define MESH_RUN_OPS 2048
static uint32_t mesh_run_stop[OP_COUNT_];

void vk_exec_mesh_run_stops(char *out, size_t n) {
    uint32_t c[OP_COUNT_];
    memcpy(c, mesh_run_stop, sizeof c);
    size_t at = 0;
    *out = 0;
    for (int k = 0; k < 4; k++) {
        int best = 0;
        for (int i = 1; i < OP_COUNT_; i++)
            if (c[i] > c[best]) best = i;
        if (!c[best]) break;
        int w = snprintf(out + at, n - at, "%s%u at %s", k ? ", " : "", c[best], nvn_op_name((uint32_t)best));
        if (w < 0 || (size_t)w >= n - at) break;
        at += (size_t)w, c[best] = 0;
    }
}

uint32_t vk_exec_brk_now(void) { return rw_chunk ? rw_chunk->brk : vk_exec_brk; }

int vk_exec_mesh_run(const void *cur, const void **out, int max) {
    const op_hdr *h = rw_chunk ? rw_op : AH.at[AH.depth];
    if (!h || nvn_op_payload(h) != cur) return 0;
    int n = 0;
    h = nvn_op_next(h);
    for (int ops = 0; ops < MESH_RUN_OPS && n < max; ops++) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) break;
        if (!op_linear(h->op)) {
            mesh_run_stop[h->op]++;
            break;
        }
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)nvn_op_payload(h))->target;
            continue;
        }
        if (h->op == OP_DRAW_MESH_IND) out[n++] = nvn_op_payload(h);
        h = nvn_op_next(h);
    }
    return n;
}

static void op_one_impl(VkExec *ex, const op_hdr *h);
static void op_one(VkExec *ex, const op_hdr *h) {
    static int timed = -1;
    if (timed < 0) timed = vk_exec_phases_on();
    if (!timed) { op_one_impl(ex, h); return; }
    uint64_t t0 = plat_now_ns();
    op_one_impl(ex, h);
    vk_exec_phase_op(h->op, plat_now_ns() - t0);
}

static void op_one_impl(VkExec *ex, const op_hdr *h) {
    const void *a = nvn_op_payload(h);
    switch (h->op) {
    case OP_BEGIN: break;
    case OP_SET_RT: set_rt(ex, a); break;
    case OP_CLEAR_COLOR: clear_color(ex, a); break;
    case OP_CLEAR_DS: clear_ds(ex, a); break;
    case OP_VIEWPORTS: {
        const p_viewports *v = a;
        for (int i = 0; i < v->n && v->first + i < 16; i++)
            if (v->first + i >= 0) memcpy(ex->vp[v->first + i], v->v[i], sizeof ex->vp[0]);
        ex->has_vp = 1;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_SCISSOR:
        ex->scissor = *(const p_scissor *)a;
        ex->has_scissor = 1;
        ex->dirty |= VK_EXEC_DIRTY_SCISSOR;
        break;
    case OP_DEPTH_RANGE: {
        const p_depth_range *r = a;
        ex->depth_range[0] = r->n, ex->depth_range[1] = r->f;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_STENCIL: {
        const p_stencil *st = a;
        for (int f = 0; f < 2; f++) {
            if (!(st->face & (1 << f))) continue;
            if (st->which == 0) ex->st_vmask[f] = (uint32_t)st->value_mask;
            else if (st->which == 1) ex->st_wmask[f] = (uint32_t)st->mask;
            else ex->st_ref[f] = (uint32_t)st->ref;
        }
        ex->dirty |= VK_EXEC_DIRTY_STENCIL;
        break;
    }
    case OP_POLY_OFFSET:
        ex->poly_offset = *(const p_poly_offset *)a;
        ex->dirty |= VK_EXEC_DIRTY_DEPTH_BIAS;
        break;
    case OP_BLEND_COLOR:
        memcpy(ex->blend_color, ((const p_blend_color *)a)->rgba, sizeof ex->blend_color);
        ex->dirty |= VK_EXEC_DIRTY_BLEND_CONST;
        break;
    case OP_DEPTH_CLAMP: ex->pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
    case OP_RASTER_DISCARD: ex->pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
    case OP_PRIM_RESTART: ex->prim_restart = *(const p_prim_restart *)a; break;
    case OP_COPY_STRIDE: ex->copy_stride = ((const p_copy_stride *)a)->stride; break;
    case OP_SET_TEX_POOL: ex->tex_pool = ((const p_set_pool *)a)->pool, ex->tex_serial++; break;
    case OP_SET_SAMP_POOL: ex->samp_pool = ((const p_set_pool *)a)->pool; break;
    case OP_BARRIER: barrier_to_why(ex, NULL, NULL, 0, BR_GAME); break;
    case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: case OP_UPDATE_UB: case OP_DRAW_IDX_IND: case OP_DISPATCH_IND:
        if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND) vk_exec_frame.draws_seen++, split_draws++;
        if (h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND) vk_exec_frame.dispatches++, split_draws++;
        // fall through
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
    case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_PUSH_CC:
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
        vk_exec_op_draw(ex, h->op, a);
        break;
    case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
        if (h->op != OP_CLEAR_TEX) vk_exec_frame.copies++;
        if (vk_dump_armed) vk_dump_copy(ex, h->op, a);
        // fall through
    case OP_SIGNAL_EVENT: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
    case OP_CLEAR_BUF:
        vk_exec_op_xfer(ex, h->op, a);
        break;
    default: rt_fatal("vk: op %s has no replay handler", nvn_op_name(h->op));
    }
}

#define RW_MAX 16
#define CHUNK_DRAWS 32
#define GROUP_PEEK 65536u

static int run_op(uint32_t op) {
    switch (op) {
    case OP_BEGIN: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL: case OP_POLY_OFFSET:
    case OP_BLEND_COLOR: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART: case OP_COPY_STRIDE:
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR:
    case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP:
    case OP_BIND_IMG: case OP_PUSH_CC: case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DRAW_MESH:
    case OP_DRAW_MESH_IND: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
    case OP_UPDATE_UB: case OP_SET_RT: case OP_CLEAR_COLOR: case OP_CLEAR_DS:
        return 1;
    default: return 0;
    }
}

typedef struct {
    PlatThread *t;
    int idx;
    void *dc;
    volatile int32_t sleeping;
    PlatEvent wake;
} RwWorker;

static const char *const pw_name[PW_N] = {"next group", "op", "pass hooks", "turn", "texture entry", "split",
                                          "submission end", "done before"};

static struct {
    int n;
    int pipe;
    int small;
    uint64_t small_groups, small_draws;
    void *help_dc;
    RwWorker *w[RW_MAX];
    const op_hdr *skip_to[CALL_DEPTH + 1];
    uint64_t groups, group_draws, chunks, serial_draws, wait_ns;
    uint64_t end_draws[5];
    uint64_t small_end[5];
    uint64_t run_end[OP_COUNT_];
    uint64_t ser_n[4], ser_ns[4], front_ns, own_ns, spin_ns;
    uint64_t pw_n[PW_N], pw_ns[PW_N], behind_ns, pd_op[OP_COUNT_];
    volatile uint64_t turns, turn_ns, redos, chunk_ns;
} RW = {.n = -1};

static void group_take(int pool);

enum { SER_DRAW, SER_MESH, SER_DISPATCH, SER_OTHER };
static int ser_kind(uint32_t op) {
    switch (op) {
    case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: return SER_DRAW;
    case OP_DRAW_MESH: case OP_DRAW_MESH_IND: return SER_MESH;
    case OP_DISPATCH: case OP_DISPATCH_IND: return SER_DISPATCH;
    default: return SER_OTHER;
    }
}

static Chunk *chunk_take(void);
static void chunk_record(Chunk *c, int pool);

static struct {
    void (*fn)(void *, int, int);
    void *arg;
    int n, grain;
    volatile int32_t next, left, in;
    volatile uint32_t seq;
} PJ;
static PlatMutex pj_lock = PLAT_MUTEX_INIT;

static void par_slices(void) {
    for (;;) {
        int lo = __atomic_fetch_add(&PJ.next, PJ.grain, __ATOMIC_ACQ_REL);
        if (lo >= PJ.n) return;
        int hi = lo + PJ.grain < PJ.n ? lo + PJ.grain : PJ.n;
        PJ.fn(PJ.arg, lo, hi);
        __atomic_sub_fetch(&PJ.left, hi - lo, __ATOMIC_ACQ_REL);
    }
}

static int par_pending(void) { return __atomic_load_n(&PJ.seq, __ATOMIC_ACQUIRE) & 1; }

static void par_help(void) {
    uint32_t s = __atomic_load_n(&PJ.seq, __ATOMIC_ACQUIRE);
    if (!(s & 1)) return;
    __atomic_add_fetch(&PJ.in, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&PJ.seq, __ATOMIC_SEQ_CST) == s) par_slices();
    __atomic_sub_fetch(&PJ.in, 1, __ATOMIC_SEQ_CST);
}

void vk_exec_par(void (*fn)(void *, int, int), void *arg, int n, int grain) {
    if (grain < 1) grain = 1;
    if (RW.n <= 0 || n <= grain || Q.drained != Q.out) {
        if (n > 0) fn(arg, 0, n);
        return;
    }
    plat_mutex_lock(&pj_lock);
    PJ.fn = fn, PJ.arg = arg, PJ.n = n, PJ.grain = grain;
    __atomic_store_n(&PJ.next, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&PJ.left, n, __ATOMIC_RELAXED);
    __atomic_add_fetch(&PJ.seq, 1, __ATOMIC_SEQ_CST);
    for (int k = 0; k < RW.n; k++)
        if (__atomic_load_n(&RW.w[k]->sleeping, __ATOMIC_SEQ_CST)) plat_event_set(&RW.w[k]->wake);
    par_slices();
    while (__atomic_load_n(&PJ.left, __ATOMIC_ACQUIRE) > 0) plat_pause();
    __atomic_add_fetch(&PJ.seq, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&PJ.in, __ATOMIC_SEQ_CST)) plat_pause();
    plat_mutex_unlock(&pj_lock);
}

static void rw_main(void *arg) {
    RwWorker *w = arg;
    plat_thread_priority(PLAT_PRIO_HIGHEST);
    perf_lock_timing_here();
    if (w->idx >= 0 && w->idx < 16) prof_worker_tid[w->idx] = plat_thread_id();
    vk_exec_draw_ctx_use(w->dc);
    vk_exec_phase_worker();
    for (;;) {
        par_help();
        Chunk *c = chunk_take();
        if (c) {
            chunk_record(c, w->idx);
            continue;
        }
        uint64_t t0 = kern_now_ns();
        for (unsigned i = 0; __atomic_load_n(&Q.take, __ATOMIC_SEQ_CST) >= __atomic_load_n(&Q.put, __ATOMIC_SEQ_CST);
             i++) {
            if (par_pending()) break;
            if ((i & 63) || kern_now_ns() - t0 < 100000) {
                plat_pause();
                continue;
            }
            __atomic_store_n(&w->sleeping, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&Q.take, __ATOMIC_SEQ_CST) >= __atomic_load_n(&Q.put, __ATOMIC_SEQ_CST) &&
                !par_pending())
                plat_event_wait_ns(&w->wake, 2000000);
            __atomic_store_n(&w->sleeping, 0, __ATOMIC_RELAXED);
            t0 = kern_now_ns();
        }
    }
}

#define RW_AUTO_MAX 6
int vk_exec_replay_workers_auto(const char **why) {
    uint64_t all, first;
    int cores = plat_cpu_cores(&all, &first);
    if (cores <= 0) cores = (int)plat_cpu_usable() / 2;
    int n = cores <= 4 ? 0 : cores / 2 - 1;
    if (n > RW_AUTO_MAX) n = RW_AUTO_MAX;
    const char *w = cores <= 4 ? "four cores or fewer" : "half the cores less one";
    if (vk_caps.amd_sc) n = 0, w = "AMD's driver for RX 6000 and older, where a game with workers lost its device";
    if (why) *why = w;
    return n;
}

static void rw_init(void) {
    const char *set = cfg_value("NX2SARIA_REPLAY_WORKERS"), *why = NULL;
    int autoset = !strcmp(set, "auto");
    int n = autoset ? vk_exec_replay_workers_auto(&why) : atoi(set);
    RW.n = n < 0 ? 0 : n > RW_MAX ? RW_MAX : n;
    if (autoset) {
        uint64_t all, first;
        rt_log("vk: replay workers automatic: %d for %d physical cores (%s; NX2SARIA_REPLAY_WORKERS = a number sets "
               "them)", RW.n, plat_cpu_cores(&all, &first), why);
    } else if (RW.n && vk_caps.amd_sc) {
        rt_log("vk: WARNING: %d replay workers set on AMD's driver for RX 6000 and older, where a game with workers lost "
               "its device; NX2SARIA_REPLAY_WORKERS = auto or 0 avoids it", RW.n);
    }
    for (int k = 0; k < RW.n; k++) {
        RwWorker *w = calloc(1, sizeof *w);
        if (!w) rt_fatal("vk: out of memory (replay worker)");
        w->idx = k, w->dc = vk_exec_draw_ctx_new();
        plat_event_init(&w->wake, 0, 0);
        char name[32];
        snprintf(name, sizeof name, "vk replay worker %d", k);
        if (!(w->t = plat_thread_new(rw_main, w, name))) rt_fatal("vk: cannot start a replay worker");
        RW.w[k] = w;
    }
    int pipe = cfg_int("NX2SARIA_REPLAY_PIPE");
    RW.pipe = RW.n <= 0 || pipe <= 0 ? 0 : pipe > LIVE_GROUPS ? LIVE_GROUPS : pipe;
    if (RW.pipe) RW.help_dc = vk_exec_draw_ctx_new();
    RW.small = RW.pipe ? cfg_int("NX2SARIA_REPLAY_SMALL") : 0;
    if (RW.small >= 2 * CHUNK_DRAWS) {
        rt_log("vk: NX2SARIA_REPLAY_SMALL = %d: groups of %d draws or more are shared out anyway, so %d is used (0..%d)",
               RW.small, 2 * CHUNK_DRAWS, 2 * CHUNK_DRAWS - 1, 2 * CHUNK_DRAWS - 1);
        RW.small = 2 * CHUNK_DRAWS - 1;
    }
    if (RW.small > 0)
        rt_log("vk: a group of %d draws or more goes whole to one replay worker (NX2SARIA_REPLAY_SMALL)", RW.small);
    else RW.small = 0;
    if (RW.pipe)
        rt_log("vk: groups of %d draws or more are recorded by %d replay worker%s while the submitting thread goes on "
               "past them, %d at once at most (NX2SARIA_REPLAY_WORKERS, NX2SARIA_REPLAY_PIPE)", 2 * CHUNK_DRAWS, RW.n,
               RW.n > 1 ? "s" : "", RW.pipe);
    else if (RW.n)
        rt_log("vk: groups of %d draws or more are recorded by %d replay worker%s and the submitting thread "
               "(NX2SARIA_REPLAY_WORKERS)", 2 * CHUNK_DRAWS, RW.n, RW.n > 1 ? "s" : "");
}

static int groups_before_done(const Group *g) { return __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) + 1 >= g->seq; }

static int chunk_in_turn(const Chunk *c) {
    if (!groups_before_done(c->g)) return 0;
    for (int i = 0; i < c->idx; i++)
        if (!__atomic_load_n(&c->g->c[i].done, __ATOMIC_ACQUIRE)) return 0;
    return 1;
}

int vk_exec_turn_due(void) {
    if (!rw_chunk) {
        if (rw_behind && __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) >= Q.out) rw_behind = 0;
        return rw_behind;
    }
    if (!rw_ordered && chunk_in_turn(rw_chunk)) rw_ordered = 1;
    return !rw_ordered;
}
int vk_exec_in_chunk(void) { return rw_chunk != NULL; }
int vk_exec_shared(void) { return rw_chunk || rw_behind; }
int vk_exec_behind(void) { return !rw_chunk && rw_behind; }

uint64_t vk_exec_stream_pos(void) { return rw_chunk ? rw_chunk->g->seq << 5 | (uint64_t)rw_chunk->idx : Q.out << 5 | 31; }

uint64_t vk_exec_epoch(int write) { return rw_chunk ? rw_chunk->g->seq : write ? Q.out + 1 : UINT64_MAX; }

uint32_t vk_exec_upd_top(uint32_t all) { return rw_chunk ? rw_chunk->g->upd : all; }

void vk_exec_hooks_wait(void) {
    if (!vk_exec_behind()) return;
    const Group *g = &GQ[Q.out % LIVE_GROUPS];
    while (!__atomic_load_n(&g->hooks, __ATOMIC_ACQUIRE)) plat_pause();
}

void vk_exec_hook_turn(void) {
    if (!rw_chunk) {
        group_wait(PW_HOOKS, 0);
        return;
    }
    while (!groups_before_done(rw_chunk->g)) plat_pause();
}

void vk_exec_turn(void) {
    Chunk *c = rw_chunk;
    if (!c) {
        group_wait(PW_TURN, 0);
        return;
    }
    if (rw_ordered) return;
    uint64_t t0 = kern_now_ns();
    while (!chunk_in_turn(c)) plat_pause();
    rw_ordered = 1;
    __atomic_add_fetch(&RW.turns, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&RW.turn_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
}

static void views_release(VkExec *ex, uint64_t fence) {
    if (!ex->nviews) return;
    ViewList *l = malloc(sizeof *l + sizeof(VkImageView) * (size_t)ex->nviews);
    if (!l) rt_fatal("vk: out of memory");
    l->n = ex->nviews;
    for (int i = 0; i < ex->nviews; i++) l->v[i] = ((ViewEnt *)ex->views)[i].view;
    vk_completion_add(fence, free_views, l);
    ex->nviews = 0;
}

enum { GE_RUN, GE_TARGET, GE_UB, GE_MESH, GE_LIMIT, GE_N };
static const char *const ge_name[GE_N] = {"run end", "target change", "uniform update", "mesh draw", "read limit"};

int vk_gpu_passes_capturing(void);

static uint32_t group_peek(const VkExec *ex, const op_hdr *h, const op_hdr **end, int *why, uint32_t *end_op) {
    uint32_t draws = 0;
    *end_op = 0;
    int begun = 0;
    const p_set_rt *rt = &ex->rt;
    *why = GE_LIMIT;
    int mesh_serial = vk_ctx()->mesh_fallback || !vk_ctx()->has_mesh_shader || vk_gpu_passes_capturing();
    for (uint32_t ops = 0; ops < GROUP_PEEK; ops++) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_) { *why = GE_RUN; break; }
        const void *a = nvn_op_payload(h);
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (!run_op(h->op)) { *why = GE_RUN, *end_op = h->op; break; }
        if (h->op == OP_UPDATE_UB) { *why = GE_UB; break; }
        int mesh = h->op == OP_DRAW_MESH || h->op == OP_DRAW_MESH_IND;
        if (mesh && mesh_serial) { *why = GE_MESH; break; }
        if (h->op == OP_SET_RT && memcmp(rt, a, sizeof *rt)) {
            if (begun) { *why = GE_TARGET; break; }
            rt = a;
        }
        if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND || mesh) draws++, begun = 1;
        if (h->op == OP_CLEAR_COLOR || h->op == OP_CLEAR_DS) begun = 1;
        h = nvn_op_next(h);
    }
    *end = h;
    return draws;
}

static Chunk *chunk_open(Group *g, const VkExec *ex, const op_hdr *h, int idx) {
    Chunk *c = &g->c[idx];
    memcpy(&c->ex, ex, sizeof *ex);
    c->h = h, c->nops = 0, c->cond0 = g->ncond, c->ncond = 0, c->idx = idx, c->g = g, c->oq = vk_exec_xfer_chunk_state();
    c->nslots = 0, c->brk = 0;
    c->bind_reused = c->bind_resolved = c->usnap_hits = 0;
    return c;
}

static void groups_done_advance(void) {
    plat_mutex_lock(&done_lock);
    for (;;) {
        uint64_t d = Q.done;
        const Group *g = &GQ[(d + 1) % LIVE_GROUPS];
        if (__atomic_load_n(&g->seq, __ATOMIC_ACQUIRE) != d + 1 || __atomic_load_n(&g->ndone, __ATOMIC_ACQUIRE) != g->n)
            break;
        __atomic_store_n(&Q.done, d + 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&done_lock);
}

static Chunk *chunk_take(void) {
    uint64_t t = __atomic_load_n(&Q.take, __ATOMIC_ACQUIRE);
    while (t < __atomic_load_n(&Q.put, __ATOMIC_ACQUIRE))
        if (__atomic_compare_exchange_n(&Q.take, &t, t + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return Q.q[t % QN];
    return NULL;
}

static void chunk_record(Chunk *c, int pool) {
    Group *g = c->g;
    if (c->idx)
        while (!__atomic_load_n(&g->hooks, __ATOMIC_ACQUIRE)) plat_pause();
    else if (!groups_before_done(g)) {
        const Group *p = &GQ[(g->seq - 1) % LIVE_GROUPS];
        while (!__atomic_load_n(&p->hooks, __ATOMIC_ACQUIRE)) plat_pause();
    }
    uint64_t t0 = kern_now_ns();
    gfx_tl_note('c', t0, (uint64_t)c->idx, (uint64_t)c->nops);
    VkExec *ex = &c->ex, *was = x_cur;
    int help = pool == RW.n && RW.pipe;
    if (pool < RW.n) ex->dc = RW.w[pool]->dc;
    else if (help) ex->dc = RW.help_dc, vk_exec_draw_ctx_use(RW.help_dc);
    x_cur = ex, rw_chunk = c, rw_ordered = !c->idx && groups_before_done(g);
    vk_exec_xfer_worker(1, g->cond + c->cond0, c->ncond, c->oq, c->slot0, c->nslots, c->idx, (int)(g - GQ));
    ex->cb = vk_sync_worker_begin(pool);
    ex->recorded = 0, ex->barrier_at = 0, ex->in_pass = 0, ex->nfbl = 0, ex->mark = NULL;
    ex->dirty = VK_EXEC_DIRTY_ALL, ex->rt_resolved = 0, ex->fb_fs = NULL;
    ex->views = NULL, ex->nviews = ex->cap_views = 0;
    vk_exec_draw_new_cb();
    const op_hdr *o = c->h;
    for (uint32_t k = 0; k < c->nops;) {
        if (o->op == OP_JUMP) {
            o = (const op_hdr *)(uintptr_t)((const p_jump *)nvn_op_payload(o))->target;
            continue;
        }
        if (!op_linear(o->op)) c->brk++;
        rw_op = o;
        op_one(ex, o);
        if (vk_texture_deferred_take()) {
            vk_exec_pass_end(ex);
            ex->rt_resolved = 0, ex->dirty = VK_EXEC_DIRTY_ALL;
            vk_exec_draw_new_cb();
            vk_exec_turn();
            __atomic_add_fetch(&RW.redos, 1, __ATOMIC_RELAXED);
            op_one(ex, o);
        }
        k++, o = nvn_op_next(o);
    }
    if (!c->idx) __atomic_store_n(&g->hooks, 1, __ATOMIC_RELEASE);
    vk_exec_barrier(ex);
    vk_sync_worker_end(ex->cb);
    views_release(ex, ex->fence);
    free(ex->views);
    ex->views = NULL;
    c->cb = ex->cb, c->recorded = ex->recorded;
    vk_exec_xfer_worker(0, NULL, 0, 0, 0, 0, 0, 0);
    x_cur = was, rw_chunk = NULL;
    if (help) vk_exec_draw_ctx_use(NULL);
    __atomic_add_fetch(&RW.chunk_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
    gfx_tl_note('d', 0, (uint64_t)c->idx, 0);
    __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
    if (__atomic_add_fetch(&g->ndone, 1, __ATOMIC_ACQ_REL) == g->n) groups_done_advance();
}

static void group_take(int pool) {
    for (Chunk *c; (c = chunk_take());) chunk_record(c, pool);
}

static void group_wait(int why, int help) {
    if (!rw_behind) return;
    uint64_t t0 = kern_now_ns();
    int done = __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) >= Q.out;
    if (!done) {
        gfx_tl_note('q', t0, (uint64_t)(uintptr_t)pw_name[why], 0);
        if (help) group_take(RW.n);
        while (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < Q.out) plat_pause();
        gfx_tl_note('u', 0, 0, 0);
    }
    rw_behind = 0;
    uint64_t t1 = kern_now_ns();
    if (done) RW.pw_n[PW_DONE]++;
    else RW.pw_n[why]++, RW.pw_ns[why] += t1 - t0;
}

static void group_put(VkExec *ex, Group *g) {
    vk_exec_xfer_group_merge((int)(g - GQ), g->n);
    VkCommandBuffer cbs[GROUP_CHUNKS];
    int rec = 0;
    uint64_t reused = 0, resolved = 0, hits = 0;
    for (int i = 0; i < g->n; i++) {
        cbs[i] = g->c[i].cb, rec += g->c[i].recorded;
        reused += g->c[i].bind_reused, resolved += g->c[i].bind_resolved, hits += g->c[i].usnap_hits;
    }
    if (reused) __atomic_add_fetch(&bind_reused_total, reused, __ATOMIC_RELAXED);
    if (resolved) __atomic_add_fetch(&bind_resolved_total, resolved, __ATOMIC_RELAXED);
    if (hits) vk_usnap_hits_add(hits);
    vk_sync_append_fill(g->at, cbs, g->n);
    if (ex->barrier_at == ex->recorded) ex->barrier_at += rec;
    ex->recorded += rec;
    RW.behind_ns += kern_now_ns() - g->out_ns;
    gfx_tl_note('h', 0, 0, 0);
}

static void groups_put_done(VkExec *ex) {
    uint64_t d = __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE);
    while (Q.drained < d) Q.drained++, group_put(ex, &GQ[Q.drained % LIVE_GROUPS]);
    if (Q.drained == Q.out) rw_behind = 0;
}

static void group_drain(VkExec *ex, int why) {
    if (Q.drained == Q.out) return;
    group_wait(why, 1);
    groups_put_done(ex);
}

static void group_room(VkExec *ex) {
    if (Q.out - Q.drained < (uint64_t)RW.pipe) return;
    uint64_t want = Q.drained + 1;
    if (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < want) {
        uint64_t t0 = kern_now_ns();
        gfx_tl_note('q', t0, (uint64_t)(uintptr_t)pw_name[PW_NEXT], 0);
        group_take(RW.n);
        while (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < want) plat_pause();
        gfx_tl_note('u', 0, 0, 0);
        RW.pw_n[PW_NEXT]++, RW.pw_ns[PW_NEXT] += kern_now_ns() - t0;
    }
    groups_put_done(ex);
}

static int pipe_op(uint32_t op) {
    return run_op(op) || op == OP_BARRIER || op == OP_DISPATCH || op == OP_DISPATCH_IND || op == OP_WAIT_EVENT ||
           op == OP_SIGNAL_EVENT;
}

static void op_again(VkExec *ex, const op_hdr *h) {
    vk_exec_pass_end(ex);
    ex->rt_resolved = 0, ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    group_wait(PW_TEX, 1);
    __atomic_add_fetch(&RW.redos, 1, __ATOMIC_RELAXED);
    op_one(ex, h);
}

static const op_hdr *group_start(VkExec *ex, const op_hdr *h, int depth) {
    const op_hdr *end;
    int why;
    uint32_t end_op;
    const int ph = vk_exec_phases_on();
    uint64_t pt = ph ? kern_now_ns() : 0;
    uint32_t draws = group_peek(ex, h, &end, &why, &end_op);
    if (ph) vk_exec_phase_seg(SEG_PEEK, kern_now_ns() - pt), pt = kern_now_ns();
    if (why == GE_RUN && end_op < OP_COUNT_) RW.run_end[end_op] += draws;
    if (end == h) return h;
    RW.end_draws[why] += draws;
    int n = (int)(draws / CHUNK_DRAWS);
    if (n > RW.n + 1) n = RW.n + 1;
    if (n > GROUP_CHUNKS) n = GROUP_CHUNKS;
    int small = RW.pipe && RW.small && n < 2 && draws >= (uint32_t)RW.small;
    if (small) n = 1;
    if (n < 1 + !small || vk_dump_armed || TL.on || !vk_sync_append_room(n)) {
        RW.skip_to[depth] = end, RW.serial_draws += draws, RW.small_end[why] += draws;
        return h;
    }
    if (RW.pipe) {
        if (split_draws && split_wanted()) group_drain(ex, PW_SPLIT), maybe_split(ex);
        group_room(ex);
    }
    Group *g = &GQ[(Q.out + 1) % LIVE_GROUPS];
    uint64_t t0 = kern_now_ns();
    gfx_tl_note('g', 0, draws, (uint64_t)n);
    vk_exec_barrier(ex);
    vk_gpu_gap_note(VK_GAP_GROUP, draws);
    g->upd = vk_exec_draw_group_begin(ex);
    if (ph) vk_exec_phase_seg(SEG_GBEGIN, kern_now_ns() - pt), pt = kern_now_ns();
    uint32_t per = (draws + (uint32_t)n - 1) / (uint32_t)n, cd = 0;
    int nc = 1, cut = 0;
    g->ncond = 0;
    vk_exec_xfer_group((int)(g - GQ));
    Chunk *c = chunk_open(g, ex, h, 0);
    for (const op_hdr *o = h; o != end;) {
        const void *a = nvn_op_payload(o);
        if (o->op == OP_JUMP) {
            o = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (cut) c = chunk_open(g, ex, o, nc++), cd = 0, cut = 0;
        switch (o->op) {
        case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND: case OP_DRAW_MESH: case OP_DRAW_MESH_IND:
            if (vk_exec_xfer_counting() && !ex->render_disabled) c->nslots++;
            if (++cd == per && nc < n) cut = 1;
            break;
        case OP_CLEAR_COLOR: case OP_CLEAR_DS: break;
        case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND: {
            int on = vk_exec_xfer_front(ex, o->op, a);
            if (o->op == OP_RENDER_COND) {
                if (g->ncond == g->ccond) {
                    g->ccond = g->ccond ? g->ccond * 2 : 1024;
                    if (!(g->cond = realloc(g->cond, g->ccond))) rt_fatal("vk: out of memory (render conditions)");
                }
                g->cond[g->ncond++] = (uint8_t)on;
                c->ncond++;
            }
            break;
        }
        default: op_one(ex, o); break;
        }
        c->nops++;
        o = nvn_op_next(o);
    }
    if (ph) vk_exec_phase_seg(SEG_FRONT, kern_now_ns() - pt), pt = kern_now_ns();
    vk_exec_xfer_group(-1);
    for (int i = 0; i < nc; i++) g->c[i].slot0 = vk_exec_xfer_slots(g->c[i].nslots);
    for (int i = 0; i < nc; i++) g->c[i].done = 0;
    g->hooks = 0, g->ndone = 0, g->n = nc;
    __atomic_store_n(&g->seq, ++Q.out, __ATOMIC_RELEASE);
    ex->cb = vk_sync_append_later(nc, &g->at);
    for (int i = 0; i < nc; i++) Q.q[(Q.put + (uint64_t)i) % QN] = &g->c[i];
    uint64_t tf = kern_now_ns();
    g->out_ns = tf;
    __atomic_store_n(&Q.put, Q.put + (uint64_t)nc, __ATOMIC_SEQ_CST);
    for (int k = 0; k < RW.n; k++)
        if (__atomic_load_n(&RW.w[k]->sleeping, __ATOMIC_SEQ_CST)) plat_event_set(&RW.w[k]->wake);
    if (ph) vk_exec_phase_seg(SEG_HANDOUT, kern_now_ns() - pt), pt = kern_now_ns();
    ex->barrier_at = ex->recorded;
    ex->in_pass = 0, ex->nfbl = 0, ex->mark = NULL, ex->dirty = VK_EXEC_DIRTY_ALL, ex->rt_resolved = 0, ex->fb_fs = NULL;
    vk_exec_draw_new_cb();
    RW.groups++, RW.group_draws += draws, RW.chunks += (uint64_t)nc;
    if (RW.pipe) {
        rw_behind = 1;
        if (small) RW.small_groups++, RW.small_draws += draws;
        else group_take(RW.n);
        if (ph) vk_exec_phase_seg(SEG_OWN, kern_now_ns() - pt), pt = kern_now_ns();
        uint64_t to = kern_now_ns();
        RW.front_ns += tf - t0, RW.own_ns += to - tf, RW.wait_ns += to - t0;
        return end;
    }
    group_take(RW.n);
    uint64_t to = kern_now_ns();
    while (__atomic_load_n(&Q.done, __ATOMIC_ACQUIRE) < g->seq) plat_pause();
    uint64_t tw = kern_now_ns();
    RW.front_ns += tf - t0, RW.own_ns += to - tf, RW.spin_ns += tw - to;
    groups_put_done(ex);
    vk_exec_draw_new_cb();
    RW.wait_ns += kern_now_ns() - t0;
    return end;
}

static const op_hdr *walk(VkExec *ex, const op_hdr *h, int depth) {
    RW.skip_to[depth] = NULL;
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        if (RW.skip_to[depth] == h) RW.skip_to[depth] = NULL;
        if (Q.drained < Q.out && Q.drained < __atomic_load_n(&Q.done, __ATOMIC_ACQUIRE)) groups_put_done(ex);
        if (RW.n > 0 && !RW.skip_to[depth] && !evo_in && h->op != OP_JUMP && run_op(h->op)) {
            const op_hdr *stop = group_start(ex, h, depth);
            if (stop != h) {
                if (split_draws && split_wanted()) maybe_split(ex);
                h = stop;
                continue;
            }
        }
        const void *a = nvn_op_payload(h);
        AH.at[depth] = h, AH.depth = depth;
        if (RR.on > 0) runs_op(h->op);
        if (!op_linear(h->op)) vk_exec_brk++;
        switch (h->op) {
        case OP_END: return NULL;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", CALL_DEPTH);
            walk(ex, nvn_op_first(((const p_jump *)a)->target), depth + 1);
            break;
        case OP_WAIT_EVENT: {
            int met = vk_exec_wait_event(a);
            vk_exec_evt(met ? 'm' : 'u', (const void *)((const p_wait_event *)a)->val, 0);
            if (!met && depth == 0 && evo_queue) {
                vk_exec_pass_end(ex);
                return h;
            }
            break;
        }
        default:
            if (RW.n > 0) {
                if (Q.drained < Q.out && !pipe_op(h->op)) RW.pd_op[h->op]++, group_drain(ex, PW_OP);
                int k = ser_kind(h->op);
                uint64_t t = rt_trace & RT_TRACE_PERF ? kern_now_ns() : 0;
                op_one(ex, h);
                if (vk_texture_deferred_take()) op_again(ex, h);
                RW.ser_n[k]++;
                if (t) RW.ser_ns[k] += kern_now_ns() - t;
            } else op_one(ex, h);
            break;
        }
        if (vk_exec_signalled && depth == 0 && !evo_in) evo_resume(ex);
        if (split_draws && split_wanted()) maybe_split(ex);
        h = nvn_op_next(h);
    }
}

#define REC_CLAIM_NS 500000000ull
static _Thread_local uint64_t rec_spent_ns;

static void rec_claim(void) {
    static int done;
    if (done) return;
    done = 1;
    rt_log("vk: thread %u is the recording thread (the first to spend %.1f s in it)", plat_thread_id(),
           REC_CLAIM_NS / 1e9);
    const char *e = cfg_get("NX2SARIA_RECORDER_PRIO");
    if (!(e && !strcmp(e, "0"))) {
        plat_thread_priority(PLAT_PRIO_HIGHEST);
        rt_log("vk: the recording thread keeps the highest scheduling priority (NX2SARIA_RECORDER_PRIO=0 turns it off)");
    }
    const char *rc = cfg_value("NX2SARIA_RECORDER_CORE");
    unsigned ncpu = plat_cpu_usable();
    int cache_die = perf_cpu_on_cache_die();
    if (!strcmp(rc, "auto") && cache_die) rt_log("vk: no core is reserved for the recording thread: the process runs on "
                                                 "the die with the largest L3 cache (NX2SARIA_RECORDER_CORE=1 reserves one)");
    if (!strcmp(rc, "1") || (!strcmp(rc, "auto") && ncpu >= 12 && !cache_die)) {
        char which[96];
        int n = plat_cpu_reserve(which, sizeof which);
        if (n) rt_log("vk: the recording thread has a core of its own: logical processor%s %s (%d of %u; "
                      "NX2SARIA_RECORDER_CORE=0 turns it off)", n > 1 ? "s" : "", which, n, ncpu);
        else rt_log("vk: no core was reserved for the recording thread (too few fast cores, or the system refused it; "
                    "NX2SARIA_RECORDER_CORE=0 stops the attempt)");
    } else {
        rt_log("vk: the recording thread runs on any core (NX2SARIA_RECORDER_CORE=auto or 1 reserves one)");
    }
}

void vk_exec_tail_begin(VkExec *ex) {
    if (!tail_enabled() || TL.on || ex != x_cur || rw_chunk || evo_in) return;
    if (Q.drained != Q.out) group_drain(ex, PW_OP);
    if (!vk_sync_wb_split() || !vk_mesh_shader_native() || vk_ctx()->mesh_fallback || vk_dump_armed ||
        vk_gpu_passes_capturing())
        return;
    if (!vk_sync_tail_ok()) return;
    if (ex->in_pass) vk_exec_pass_end(ex);
    vk_exec_barrier(ex);
    VkCommandBuffer cb = vk_sync_cut();
    if (!cb) return;
    ex->cb = cb;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb();
    n_cuts++;
    TL.on = 1, TL.live0 = vk_mem_live_swap(0), TL.wrote0 = vk_mem_gpu_wrote_count();
    vk_exec_tail_results();
}

int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    if (vk_device_is_lost()) return 0;
    uint64_t in_ns = kern_now_ns();
    const int ph = vk_exec_phases_on();
    uint64_t pt = in_ns;
    gfx_tl_note('R', 0, (uint64_t)n, fence);
    perf_lock_timing_here();
    if (prof_replay_tid != plat_thread_id()) perf_replay_thread();
    perf_rec_waits = rec_wait_totals;
    VkExec *ex = &X;
    if (!x_ready) state_reset(ex), vk_dump_init(), x_ready = 1;
    if (RW.n < 0) rw_init();
    if (Q.drained != Q.out) rt_fatal("vk: a parallel group outlived its submission");
    const tex_t *keep[9];
    memcpy(keep, ex->rt.color, sizeof ex->rt.color);
    keep[8] = ex->rt.depth;
    if (ph) pt = kern_now_ns();
    nvn_pins_sweep(nvn_submit_count(), keep, 9);
    if (ph) vk_exec_phase_seg(SEG_PINS, kern_now_ns() - pt), pt = kern_now_ns();
    ex->cb = cb;
    ex->fence = fence;
    TL.on = 0;
    ex->recorded = 0;
    ex->in_pass = 0;
    ex->mark = NULL;
    ex->mark_passes = ex->mark_dispatches = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->rt_resolved = 0;
    ex->nviews = 0;
    ex->barrier_at = -1;
    ex->nfbl = 0;
    vk_exec_frame.submits++;
    __atomic_store_n(&vk_exec_submit_seq, vk_exec_submit_seq + 1, __ATOMIC_RELAXED);
    split_draws = part_start = 0;
    split_at = split_n < 0 ? 0 : split_n ? split_n : INT_MAX;
    part_t0 = kern_now_ns();
    x_cur = ex;
    int evo = evo_enabled() && gfx_submit_queue;
    int slot = evo ? evq_slot(gfx_submit_queue) : -1;
    if (evo && slot < 0) evo = 0;
    vk_exec_evt('|', (const void *)gfx_submit_queue, (uint32_t)n);
    if (ph) pt = kern_now_ns();
    vk_exec_event_sync(1);
    if (ph) vk_exec_phase_seg(SEG_EVSYNC, kern_now_ns() - pt), pt = kern_now_ns();
    int synced = n || (evo_pending && held_sync() && vk_exec_held_ready());
    if (synced) {
        vk_mem_mirror_begin();
        if (ph) vk_exec_phase_seg(SEG_MIRROR_BEGIN, kern_now_ns() - pt), pt = kern_now_ns();
        if (evo_pending) {
            int tot = n;
            for (int s = 0; s < EVO_QUEUES; s++)
                for (Cont *c = evq[s].head; c; c = c->next) tot += c->n;
            uint64_t *all = malloc(sizeof *all * (size_t)tot);
            if (!all) rt_fatal("vk: out of memory (snapshot handles)");
            if (n) memcpy(all, handles, sizeof *all * (size_t)n);
            int k = n;
            for (int s = 0; s < EVO_QUEUES; s++)
                for (Cont *c = evq[s].head; c; c = c->next) memcpy(all + k, c->h, sizeof *all * (size_t)c->n), k += c->n;
            vk_usnap_begin(all, tot, fence);
            free(all);
        } else vk_usnap_begin(handles, n, fence);
        if (ph) vk_exec_phase_seg(SEG_USNAP_BEGIN, kern_now_ns() - pt), pt = kern_now_ns();
    }
    for (int i = 0; i < n; i++)
        if (!handles[i]) rt_fatal("vk: command handle %d of a submission is null", i);
    uint64_t ph0 = ph ? kern_now_ns() : 0;
    if (ph) vk_exec_phase_seg(SEG_PROLOGUE, ph0 - in_ns);
    if (evo_pending) evo_resume(ex);
    if (evo && evq[slot].head) {
        if (n) cont_push(slot, cont_new(handles, n, 0, NULL)), evo_held++, vk_exec_evt('p', NULL, 0);
    } else {
        evo_queue = evo ? gfx_submit_queue : 0;
        AH.handles = handles, AH.n = n, AH.live = 1;
        for (int i = 0; i < n; i++) {
            AH.i = i;
            const op_hdr *stop = walk(ex, nvn_op_first(handles[i]), 0);
            if (!stop) continue;
            cont_push(slot, cont_new(handles, n, i, stop));
            evo_held++;
            vk_exec_evt('h', NULL, 0);
            break;
        }
        AH.live = 0;
        evo_queue = 0;
    }
    if (ph) {
        uint64_t t = kern_now_ns();
        vk_exec_phase_seg(SEG_WALK, t - ph0), ph0 = t;
    }
    group_drain(ex, PW_END);
    vk_exec_pass_end(ex);
    if (RR.on > 0) runs_break(RB_SUBMIT);
    replay_counts_flush();
    x_cur = NULL;
    vk_exec_event_sync(0);
    if (ex->recorded) vk_exec_barrier(ex), vk_exec_pool_marks(ex);
    int tail = 0;
    if (TL.on) {
        int live = vk_mem_live_swap(0), res = vk_exec_tail_results();
        vk_mem_live_swap(live | TL.live0);
        int wrote = vk_mem_gpu_wrote_count() != TL.wrote0;
        int other = !synced || !ex->recorded || vk_dump_armed || vk_gpu_passes_capturing() || !vk_sync_tail_ok();
        TL.on = 0, TL.n++;
        if (res) TL.why[0]++;
        else if (wrote) TL.why[1]++;
        else if (live) TL.why[2]++;
        else if (other || !vk_exec_pool_split(ex->fence)) TL.why[3]++;
        else tail = 1, TL.early++;
        if (tail) {
            ex->cb = vk_sync_tail_hold();
            ex->dirty = VK_EXEC_DIRTY_ALL;
            vk_exec_draw_new_cb();
        }
    }
    if (!tail && synced && ex->recorded && vk_sync_wb_split() && vk_mem_mirror_wb_pending() &&
        vk_exec_pool_split(ex->fence)) {
        ex->cb = vk_sync_split_exec();
        ex->dirty = VK_EXEC_DIRTY_ALL;
        vk_exec_draw_new_cb();
    }
    if (ph) {
        uint64_t t = kern_now_ns();
        vk_exec_phase_seg(SEG_DRAIN, t - ph0), ph0 = t;
    }
    if (synced) ex->recorded += vk_mem_mirror_end(ex->cb, fence), vk_usnap_end(fence);
    if (ph) {
        uint64_t t = kern_now_ns();
        vk_exec_phase_seg(SEG_MIRROR_END, t - ph0), ph0 = t;
    }
    if (tail) {
        ex->cb = vk_sync_tail_release();
        part_start = ex->recorded;
    }
    if ((rt_trace & RT_TRACE_PERF) && TL.n >= 600) {
        uint64_t tails, waits;
        double ms;
        vk_sync_tail_stats(&tails, &waits, &ms);
        rt_log("perf: early tail: %llu tails, %llu with the write-backs before them (refused: %llu handed the CPU "
               "results, %llu wrote pool memory, %llu read guest memory late, %llu other); %llu waits of the game "
               "ended before their tail, which the GPU runs for about %.2f ms",
               (unsigned long long)TL.n, (unsigned long long)TL.early,
               (unsigned long long)TL.why[0], (unsigned long long)TL.why[1], (unsigned long long)TL.why[2],
               (unsigned long long)TL.why[3], (unsigned long long)waits, ms);
        TL.n = TL.early = 0, memset(TL.why, 0, sizeof TL.why);
    }
    views_release(ex, fence);
    ex->cb = VK_NULL_HANDLE;
    if (n) n_subs++;
    if ((rt_trace & RT_TRACE_PERF) && n_subs >= 240) {
        rt_log("perf: replay command buffer parts per submission %.2f (%.2f first parts sent while the GPU was idle, %.2f "
               "splits inside a pass, %.2f cuts for emulated mesh stages); event order: held %llu, resumed %llu (%llu without their signal), switches %llu, "
               "held now %d; waits a CPU signal met only after the submission's copies were taken: %llu",
               (double)(n_subs + n_parts) / (double)n_subs, (double)n_idle_parts / (double)n_subs,
               (double)n_pass_splits / (double)n_subs, (double)n_cuts / (double)n_subs, (unsigned long long)evo_held, (unsigned long long)evo_resumed,
               (unsigned long long)evo_forced, (unsigned long long)evo_switches, evo_pending,
               (unsigned long long)vk_exec_event_late_take());
        uint64_t xw, xe, xl, xm, le, lc, lo;
        vk_sync_exec_stats(&xw, &xe, &xl, &xm);
        vk_exec_late_stats(&le, &lc, &lo);
        if (xw)
            rt_log("perf: waits after a submit on the executed timeline: %llu, %llu of them back before the completion "
                   "thread had run the side effects of the submissions up to theirs (through %.3f ms later on average, "
                   "%.3f at most); written into guest memory in that time: %llu event values, %llu counter reports, "
                   "%llu occlusion counts",
                   (unsigned long long)xw, (unsigned long long)xe, xe ? xl / 1e6 / (double)xe : 0.0, xm / 1e6,
                   (unsigned long long)le, (unsigned long long)lc, (unsigned long long)lo);
        char cbs[1600];
        vk_sync_cb_stats(cbs, sizeof cbs);
        rt_log("perf: completion thread since the last line, by callback (distance from vk_completion_add):%s", cbs);
        if (RW.n > 0) {
            uint64_t turns = __atomic_exchange_n(&RW.turns, 0, __ATOMIC_RELAXED);
            uint64_t turn_ns = __atomic_exchange_n(&RW.turn_ns, 0, __ATOMIC_RELAXED);
            uint64_t redos = __atomic_exchange_n(&RW.redos, 0, __ATOMIC_RELAXED);
            rt_log("perf: replay workers: per submission %.1f groups of %.0f draws in %.1f chunks, %.2f ms each; %.0f draws "
                   "in smaller groups; waits for an earlier chunk %.1f (%.3f ms), ops replayed again in turn %.2f",
                   (double)RW.groups / (double)n_subs, RW.groups ? (double)RW.group_draws / (double)RW.groups : 0.0,
                   RW.groups ? (double)RW.chunks / (double)RW.groups : 0.0,
                   RW.groups ? (double)RW.wait_ns / 1e6 / (double)RW.groups : 0.0, (double)RW.serial_draws / (double)n_subs,
                   (double)turns / (double)n_subs, (double)turn_ns / 1e6 / (double)n_subs, (double)redos / (double)n_subs);
            char e[512];
            int o = 0;
            for (int k = 0; k < GE_N; k++)
                o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.0f", k ? "," : "", ge_name[k],
                              (double)RW.end_draws[k] / (double)n_subs);
            o += snprintf(e + o, sizeof e - (size_t)o, "; of them in groups too small to split:");
            for (int k = 0; k < GE_N; k++)
                o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.0f", k ? "," : "", ge_name[k],
                              (double)RW.small_end[k] / (double)n_subs);
            rt_log("perf: replay workers: draws per submission by what ended their group:%s", e);
            o = 0;
            for (int k = 0; k < 6; k++) {
                int best = 0;
                for (int op = 1; op < OP_COUNT_; op++)
                    if (RW.run_end[op] > RW.run_end[best]) best = op;
                if (!RW.run_end[best]) break;
                o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.0f", k ? "," : "", nvn_op_name((uint32_t)best),
                              (double)RW.run_end[best] / (double)n_subs);
                RW.run_end[best] = 0;
            }
            memset(RW.run_end, 0, sizeof RW.run_end);
            if (o) rt_log("perf: replay workers: draws per submission of groups a run end ended, by the op:%s", e);
            memset(RW.end_draws, 0, sizeof RW.end_draws), memset(RW.small_end, 0, sizeof RW.small_end);
            double ns = 1e6 * (double)n_subs;
            rt_log("perf: replay workers: the submitting thread per submission: outside groups draws %.0f (%.3f ms), mesh "
                   "draws %.0f (%.3f ms), dispatches %.0f (%.3f ms), other ops %.0f (%.3f ms); in groups the front pass "
                   "%.3f ms, its own chunks %.3f ms, waiting for the others %.3f ms",
                   (double)RW.ser_n[SER_DRAW] / (double)n_subs, (double)RW.ser_ns[SER_DRAW] / ns,
                   (double)RW.ser_n[SER_MESH] / (double)n_subs, (double)RW.ser_ns[SER_MESH] / ns,
                   (double)RW.ser_n[SER_DISPATCH] / (double)n_subs, (double)RW.ser_ns[SER_DISPATCH] / ns,
                   (double)RW.ser_n[SER_OTHER] / (double)n_subs, (double)RW.ser_ns[SER_OTHER] / ns,
                   (double)RW.front_ns / ns, (double)RW.own_ns / ns, (double)RW.spin_ns / ns);
            memset(RW.ser_n, 0, sizeof RW.ser_n), memset(RW.ser_ns, 0, sizeof RW.ser_ns);
            RW.front_ns = RW.own_ns = RW.spin_ns = 0;
            rt_log("perf: replay workers: the chunks took %.3f ms per submission, every thread's",
                   (double)__atomic_exchange_n(&RW.chunk_ns, 0, __ATOMIC_RELAXED) / ns);
            if (RW.pipe) {
                o = 0;
                for (int k = 0; k < PW_N; k++)
                    o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.2f (%.3f ms)", k ? "," : "", pw_name[k],
                                  (double)RW.pw_n[k] / (double)n_subs, (double)RW.pw_ns[k] / ns);
                rt_log("perf: replay workers: past a group the submitting thread went on %.3f ms per submission until "
                       "it was done; waits for it per submission:%s", (double)RW.behind_ns / ns, e);
                o = 0;
                for (int k = 0; k < 5; k++) {
                    int best = 0;
                    for (int op = 1; op < OP_COUNT_; op++)
                        if (RW.pd_op[op] > RW.pd_op[best]) best = op;
                    if (!RW.pd_op[best]) break;
                    o += snprintf(e + o, sizeof e - (size_t)o, "%s %s %.2f", k ? "," : "", nvn_op_name((uint32_t)best),
                                  (double)RW.pd_op[best] / (double)n_subs);
                    RW.pd_op[best] = 0;
                }
                if (o) rt_log("perf: replay workers: ops that waited for a group, per submission:%s", e);
                uint64_t td[3];
                vk_texture_deferred_stats(td);
                rt_log("perf: replay workers: texture lookups put off until their turn, per submission: no entry "
                       "%.2f, a retired one %.2f, a released one %.2f", (double)td[0] / (double)n_subs,
                       (double)td[1] / (double)n_subs, (double)td[2] / (double)n_subs);
                memset(RW.pw_n, 0, sizeof RW.pw_n), memset(RW.pw_ns, 0, sizeof RW.pw_ns);
                memset(RW.pd_op, 0, sizeof RW.pd_op);
                RW.behind_ns = 0;
                if (RW.small)
                    rt_log("perf: replay workers: groups one worker took whole (NX2SARIA_REPLAY_SMALL): %.2f per "
                           "submission, %.1f draws each", (double)RW.small_groups / (double)n_subs,
                           RW.small_groups ? (double)RW.small_draws / (double)RW.small_groups : 0.0);
            }
        }
        RW.small_groups = RW.small_draws = 0;
        RW.groups = RW.group_draws = RW.chunks = RW.serial_draws = RW.wait_ns = 0;
        evo_held = evo_resumed = evo_forced = evo_switches = 0;
        n_subs = n_parts = n_idle_parts = n_pass_splits = n_cuts = 0;
    }
    rec_spent_ns += kern_now_ns() - in_ns;
    if (ph) vk_exec_phase_seg(SEG_TAIL, kern_now_ns() - ph0), vk_exec_phase_record(kern_now_ns() - in_ns);
    if (rec_spent_ns >= REC_CLAIM_NS) rec_claim();
    gfx_tl_note('r', 0, (uint64_t)(ex->recorded - part_start), 0);
    return ex->recorded - part_start;
}
