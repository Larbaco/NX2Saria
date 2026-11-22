// The mesh fallback's buffers and records. A task or mesh program runs as a compute dispatch that writes a storage
// buffer (mesh_fb.c), and the draw reads it:
//   task buffer   a task workgroup's payload at its index, payload-size apart
//   list          per mesh workgroup after a task stage: its task and its index among that task's (meshfb_scan.comp)
//   mesh buffer   a mesh workgroup's vertex lines and primitive area at its slot
//   records       per draw 64 bytes: the indirect arguments of its task dispatch, its mesh dispatch and its draw
// The three buffers are reused by every draw: a draw's dispatches write them and its vertex stage has read them
// before the next draw's dispatches run, with a barrier in between (vk_exec_draw.c mfb_draw), so their size is what
// the largest draw needs, not what a frame's draws need together. mesh_fallback_mb sets it: an eighth for the task
// buffer, the rest for the mesh buffer. Workgroups past either buffer's end write nothing and draw nothing (the
// modules check the buffer's length); the draws that happened to are counted and the log names the size they needed.
// A record is used by one draw and written again only after MESHFB_RECS further draws.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mesh_fb.h"
#include "vk.h"
#include "vk_mesh_fb.h"
#include "mfb_batch.h"
#include "meshfb_scan_spv.h"
#include "meshfb_clamp_spv.h"

enum { MESHFB_RECS = 4096, REC_BYTES = 64 };

static struct {
    int tried, ok;
    VkBuffer task, mesh, list, rec, stat;
    VmaAllocation task_a, mesh_a, list_a, rec_a, stat_a;
    VkDeviceSize task_bytes, mesh_bytes, list_bytes;   // as one storage binding spans them
    const uint32_t *stat_map;                          // per record: mesh workgroups and task workgroups it asked for
    uint32_t cap_slots[MESHFB_RECS], slot_words[MESHFB_RECS], cap_tasks[MESHFB_RECS];   // what the record's draw had room for
    uint8_t noted[MESHFB_RECS];
    uint64_t fence[MESHFB_RECS];       // the submission its note is in (vk_meshfb_note)
    uint32_t checked;                  // the records read back so far (vk_meshfb_poll), as M.next counts them
    MfbHist *owner[MESHFB_RECS];       // the record's program's history (vk_exec_draw.c MfbCs.hist), NULL: none
    uint8_t batched[MESHFB_RECS];      // its draw had a part of the buffers (a batch)
    int part_on;                       // the draw being recorded uses part (vk_meshfb_part_take)
    MfbPart part;
    MfbSpan used;                      // the parts taken for the pre-pass command buffer being recorded
    uint64_t batch_short;              // batched draws that asked for more than their part held
    uint64_t readmits;                 // programs taken back into batches after such a draw (mfb_hist_note)
    int closed;                        // a draw went one by one since the parts began: the next batched cuts (mfb_batch.h)
    uint64_t peak_mesh;                // the most of the mesh buffer the parts of one pre-pass command buffer took
    uint32_t next;
    uint64_t draws, short_draws, worst_bytes;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline scan, clamp;
} M;

static int make_buffer(VkCtx *c, VkDeviceSize size, VkBufferUsageFlags usage, int host, VkBuffer *b, VmaAllocation *a, void **map) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = host ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT : 0};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, b, a, &info) != VK_SUCCESS) return *b = VK_NULL_HANDLE, 0;
    if (map) *map = info.pMappedData;
    return !host || info.pMappedData;
}

int vk_meshfb_init(void) {
    if (M.tried) return M.ok;
    M.tried = 1;
    VkCtx *c = vk_ctx();
    if (!c) return 0;
    uint64_t mb = (uint64_t)cfg_int("NX2SARIA_MESH_FALLBACK_MB");
    if (mb < 16) mb = 16;
    if (mb > 4096) mb = 4096;
    VkDeviceSize range = c->props.limits.maxStorageBufferRange & ~(VkDeviceSize)3, task = (mb << 20) / 8, mesh = (mb << 20) - task;
    VkDeviceSize list = mesh / 128;   // an entry per slot of 512 bytes, and no mesh program's slot is smaller
    void *map = NULL;
    const VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!make_buffer(c, task, st, 0, &M.task, &M.task_a, NULL) || !make_buffer(c, mesh, st, 0, &M.mesh, &M.mesh_a, NULL) ||
        !make_buffer(c, list, st, 0, &M.list, &M.list_a, NULL) ||
        !make_buffer(c, (VkDeviceSize)MESHFB_RECS * REC_BYTES,
                     st | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0,
                     &M.rec, &M.rec_a, NULL) ||
        !make_buffer(c, (VkDeviceSize)MESHFB_RECS * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &M.stat, &M.stat_a, &map)) {
        rt_log("vk: mesh fallback: its buffers (%llu MiB, mesh_fallback_mb) could not be allocated: task and mesh draws are skipped",
               (unsigned long long)mb);
        return 0;
    }
    M.stat_map = map;
    memset(map, 0, (size_t)MESHFB_RECS * 8);
    M.task_bytes = task < range ? task : range, M.mesh_bytes = mesh < range ? mesh : range, M.list_bytes = list < range ? list : range;

    M.push = (PFN_vkCmdPushDescriptorSetKHR)vk_dpa(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[4];
    for (int i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof meshfb_scan_spv,
                                   .pCode = meshfb_scan_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!M.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &M.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &M.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &M.layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = M.layout};
    VkResult res = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &M.scan);
    vkDestroyShaderModule(c->device, m, NULL);
    if (res != VK_SUCCESS) goto fail;
    // the clamp of a count the GPU wrote (vk_meshfb_clamp): binding 0 and three push constants of the same layout
    mi.codeSize = sizeof meshfb_clamp_spv, mi.pCode = meshfb_clamp_spv, m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) goto fail;
    ci.stage.module = m;
    res = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &M.clamp);
    vkDestroyShaderModule(c->device, m, NULL);
    if (res != VK_SUCCESS) goto fail;
    rt_log("vk: mesh fallback: %llu MiB for task payloads, %llu MiB for mesh output (mesh_fallback_mb = %llu)",
           (unsigned long long)(M.task_bytes >> 20), (unsigned long long)(M.mesh_bytes >> 20), (unsigned long long)mb);
    return M.ok = 1;
fail:
    rt_log("vk: mesh fallback: the scan or clamp pipeline could not be created: task and mesh draws are skipped");
    return 0;
}

VkDescriptorBufferInfo vk_meshfb_buffer(int cb_slot) {
    if (M.part_on) {   // a batched draw: its part (vk_meshfb_part_take)
        if (cb_slot == MESHFB_CB_TASK) return (VkDescriptorBufferInfo){M.task, M.part.task_off, M.part.task_len};
        if (cb_slot == MESHFB_CB_LIST) return (VkDescriptorBufferInfo){M.list, M.part.list_off, M.part.list_len};
        return (VkDescriptorBufferInfo){M.mesh, M.part.mesh_off, M.part.mesh_len};
    }
    if (cb_slot == MESHFB_CB_TASK) return (VkDescriptorBufferInfo){M.task, 0, M.task_bytes};
    if (cb_slot == MESHFB_CB_LIST) return (VkDescriptorBufferInfo){M.list, 0, M.list_bytes};
    return (VkDescriptorBufferInfo){M.mesh, 0, M.mesh_bytes};
}

// ---- a batch's parts (vk_exec_draw.c mfb_draw): each batched draw of a pre-pass command buffer takes a part of the
// three buffers from the start, in turn; the buffers' use starts over with the next pre-pass command buffer, which
// begins with a full barrier (the draws of the part before may still read them).
int vk_meshfb_part_take(uint64_t task_b, uint64_t list_b, uint64_t mesh_b) {
    MfbSpan cap = {M.task_bytes, M.list_bytes, M.mesh_bytes};
    uint64_t align = vk_ctx()->props.limits.minStorageBufferOffsetAlignment;
    if (!mfb_part_take(&M.used, &cap, align, task_b, list_b, mesh_b, &M.part)) return 0;
    M.part_on = 1;
    if (M.used.mesh > M.peak_mesh) M.peak_mesh = M.used.mesh;
    return 1;
}
void vk_meshfb_part_done(void) { M.part_on = 0; }
void vk_meshfb_part_reset(void) { M.used = (MfbSpan){0, 0, 0}, M.part_on = 0, M.closed = 0; }
void vk_meshfb_one_by_one(void) {   // a draw recorded one by one: with parts taken, the next batched draw cuts first
    if (!vk_meshfb_part_first()) M.closed = 1;
}
int vk_meshfb_closed(void) { return M.closed; }

// Every write before it visible to every read and write after it, in cb (a pre-pass command buffer: vk_exec_barrier
// is the part's, with its pass and layouts).
void vk_meshfb_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// vk_sync.c pre_end: the pre-pass command buffer ends. With parts taken, a full barrier first, so the part's draws see
// what the dispatches wrote; then the parts start over.
void vk_meshfb_pre_end(VkCommandBuffer cb) {
    if (!vk_meshfb_part_first()) vk_meshfb_full_barrier(cb);
    vk_meshfb_part_reset();
}
int vk_meshfb_part_first(void) { return !mfb_parts_taken(&M.used); }
uint64_t vk_meshfb_batch_short(void) { return M.batch_short; }
uint64_t vk_meshfb_batch_readmits(void) { return M.readmits; }
void vk_meshfb_caps(uint64_t *task, uint64_t *mesh) { *task = M.task_bytes, *mesh = M.mesh_bytes; }
uint64_t vk_meshfb_list_bytes(void) { return M.list_bytes; }
uint64_t vk_meshfb_batch_peak(uint64_t *cap) { return *cap = M.mesh_bytes, M.peak_mesh; }
uint64_t vk_meshfb_slot_room(uint32_t slot_words, int has_task) {   // the whole buffers' room, as a one-by-one draw has it
    uint64_t r = M.mesh_bytes / 4 / (slot_words ? slot_words : 1);
    if (has_task && r > M.list_bytes / 4) r = M.list_bytes / 4;
    return r;
}

uint64_t vk_meshfb_mesh_words(void) { return M.mesh_bytes / 4; }

// The record about to be written again: what its last draw asked for against what it had room for.
static void record_check(uint32_t i) {
    if (!M.noted[i]) return;
    M.noted[i] = 0;
    vmaInvalidateAllocation(vk_ctx()->vma, M.stat_a, (VkDeviceSize)i * 8, 8);   // HOST_ACCESS_RANDOM: may not be coherent
    uint32_t want = M.stat_map[2 * i], tasks = M.stat_map[2 * i + 1];
    int batched = M.batched[i];
    int note = M.owner[i] ? mfb_hist_note(M.owner[i], want, tasks, M.cap_slots[i], M.cap_tasks[i], M.batched[i]) : 0;
    if (note == MFB_NOTE_SHORT) {
        uint64_t n = ++M.batch_short;
        if (n <= 4 || !(n & (n - 1)))
            rt_log("vk: mesh fallback batch: a batched draw asked for %u mesh and %u task workgroups where its part held %u "
                   "and %u: what did not fit was not drawn, and its program draws one by one until its history settles "
                   "again (%llu such draws; mfb_batch.h MFB_READMIT, MFB_MAX_SHORTS)", want, tasks, M.cap_slots[i],
                   M.cap_tasks[i], (unsigned long long)n);
    } else if (note == MFB_NOTE_READMIT) {
        M.readmits++;
    }
    M.owner[i] = NULL;
    if (batched || (want <= M.cap_slots[i] && tasks <= M.cap_tasks[i])) return;   // batched: its part, logged above
    if (want <= M.cap_slots[i]) {
        // only the task count was past the task buffer: a count the GPU wrote, kept by meshfb_clamp.comp before it cut
        // the dispatch (until 2026-11-22 such a draw was not seen); the mesh workgroups its tasks started all fit
        uint64_t n = ++M.short_draws;
        if (n <= 4 || !(n & (n - 1)))
            rt_log("vk: mesh fallback: a draw's GPU-written count asked for %u task workgroups where the task buffer holds "
                   "%u: the dispatch ran %u, and the %u mesh workgroups they started fit (%llu draws short of a buffer)",
                   tasks, M.cap_tasks[i], M.cap_tasks[i], want, (unsigned long long)n);
        return;
    }
    uint64_t need = (uint64_t)want * M.slot_words[i] * 4;
    if (need > M.worst_bytes) M.worst_bytes = need;
    uint64_t n = ++M.short_draws;
    if (n <= 4 || !(n & (n - 1))) {
        char from[96] = "";
        if (tasks) snprintf(from, sizeof from, " from %u task workgroups (room for %u)", tasks, M.cap_tasks[i]);
        rt_log("vk: mesh fallback: a draw's programs emitted %u mesh workgroups of %u bytes%s, and the mesh buffer holds %u: "
               "what did not fit was not drawn (%llu such draws of %llu; mesh_fallback_mb of %llu or more would hold the "
               "largest so far)", want, M.slot_words[i] * 4, from, M.cap_slots[i], (unsigned long long)n,
               (unsigned long long)M.draws, (unsigned long long)((M.worst_bytes * 8 / 7 >> 20) + 1));
    }
}

VkDeviceSize vk_meshfb_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_off, uint32_t count, int has_task,
                              uint32_t verts, uint32_t slot_words, MfbHist *owner, VkBuffer *rec, int *cmds) {
    uint32_t i = M.next++ % MESHFB_RECS;
    VkDeviceSize at = (VkDeviceSize)i * REC_BYTES;
    record_check(i);
    if (M.next - M.checked > MESHFB_RECS) M.checked = M.next - MESHFB_RECS;   // what the poll had not reached
    M.draws++;
    VkDeviceSize mesh_b = M.part_on ? M.part.mesh_len : M.mesh_bytes, list_b = M.part_on ? M.part.list_len : M.list_bytes;
    M.slot_words[i] = slot_words, M.cap_slots[i] = (uint32_t)(mesh_b / 4 / (slot_words ? slot_words : 1));
    M.cap_tasks[i] = 0xffffffffu;   // vk_meshfb_scan knows the payload size
    if (has_task && M.cap_slots[i] > list_b / 4) M.cap_slots[i] = (uint32_t)(list_b / 4);
    M.owner[i] = owner, M.batched[i] = (uint8_t)M.part_on;
    *rec = M.rec;
    int n = 0;
    // Every word of the record is written by exactly one command. A fill of the whole record followed by fills and
    // copies into parts of it were transfer writes to the same bytes without a barrier between them, which the GPU may
    // run in any order: on an RX 580 the vertex count, the first vertex or instance or the task count was the fill's 1
    // in about half the draws, and the foliage of the mesh programs flashed (missing, or white from the wrong slots).
    // Fills and copies only: they go through the recorder thread (vk_rec.h) like the replay's other commands.
    uint32_t r[REC_BYTES / 4], copied = 0;   // copied: the words the GPU-written count goes to
    for (uint32_t k = 0; k < REC_BYTES / 4; k++) r[k] = 1;
    // where the workgroup count goes: the task dispatch, or the mesh dispatch, the draw's instances and the report
    VkDeviceSize to[3] = {has_task ? 0 : 16, 36, 48};
    int nto = has_task ? 1 : 3;
    if (!has_task) r[32 / 4] = verts, r[40 / 4] = 0, r[44 / 4] = 0, r[52 / 4] = 0;
    for (int k = 0; k < nto; k++) {
        if (src) copied |= 1u << (to[k] / 4);
        else r[to[k] / 4] = count;
    }
    for (uint32_t k = 0; k < REC_BYTES / 4;) {
        if (copied >> k & 1) { k++; continue; }
        uint32_t e = k + 1;
        while (e < REC_BYTES / 4 && !(copied >> e & 1) && r[e] == r[k]) e++;
        vkCmdFillBuffer(cb, M.rec, at + 4 * (VkDeviceSize)k, 4 * (VkDeviceSize)(e - k), r[k]), n++;
        k = e;
    }
    if (src) {
        VkBufferCopy cp[3];
        for (int k = 0; k < nto; k++) cp[k] = (VkBufferCopy){src_off, at + to[k], 4};
        vkCmdCopyBuffer(cb, src, M.rec, (uint32_t)nto, cp), n++;
    }
    *cmds = n;
    return at;
}

void vk_meshfb_scan(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words, uint32_t fan, uint32_t slot_words, uint32_t verts,
                    int clamped) {
    uint32_t i = (uint32_t)(at / REC_BYTES);
    VkDescriptorBufferInfo bi[4] = {{M.rec, 0, VK_WHOLE_SIZE}, vk_meshfb_buffer(MESHFB_CB_TASK), vk_meshfb_buffer(MESHFB_CB_LIST),
                                    vk_meshfb_buffer(MESHFB_CB_MESH)};
    M.cap_tasks[i] = (uint32_t)(bi[1].range / 4 / (task_words ? task_words : 1));
    VkWriteDescriptorSet w[4];
    for (int k = 0; k < 4; k++)
        w[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)k,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi[k]};
    // clamped: vk_meshfb_clamp ran for this record and wrote its report of the task workgroups asked for (word 13)
    uint32_t pc[6] = {(uint32_t)(at / 4), task_words, (fan & 0x7fffffffu) | (clamped ? 0x80000000u : 0u), slot_words, verts,
                      vk_ctx()->props.limits.maxComputeWorkGroupCount[0]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.scan);
    rec_push(M.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 4, w);
    vkCmdPushConstants(cb, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, 1, 1);
}

// A count the GPU wrote, copied into record `at` (vk_meshfb_record with a source), cut to what the buffers hold before a
// dispatch reads it (meshfb_clamp.comp says why). task_words: the task stage's payload words, 0 without a task stage.
// One single-thread dispatch; the caller orders it after the record's copies and before the first dispatch.
void vk_meshfb_clamp(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words) {
    uint32_t i = (uint32_t)(at / REC_BYTES), most = vk_ctx()->props.limits.maxComputeWorkGroupCount[0];
    uint32_t room = task_words ? (uint32_t)(vk_meshfb_buffer(MESHFB_CB_TASK).range / 4 / task_words) : M.cap_slots[i];
    if (room > most) room = most;
    VkDescriptorBufferInfo bi = {M.rec, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi};
    uint32_t pc[3] = {(uint32_t)(at / 4), task_words != 0, room};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.clamp);
    rec_push(M.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.layout, 0, 1, &w);
    vkCmdPushConstants(cb, M.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, 1, 1);
}

void vk_meshfb_note(VkCommandBuffer cb, VkDeviceSize at, uint64_t fence) {
    uint32_t i = (uint32_t)(at / REC_BYTES);
    VkBufferCopy cp = {at + 48, (VkDeviceSize)i * 8, 8};
    vkCmdCopyBuffer(cb, M.rec, M.stat, 1, &cp);
    M.noted[i] = 1, M.fence[i] = fence;
}

// The records whose submissions the GPU finished (done: vk_sync_fence_done), read back in order: a program's history
// grows, and a batched draw short of its part excludes its program, within frames of the draw and not 4096 records
// later, when the record is written again.
void vk_meshfb_poll(uint64_t done) {
    if (!M.ok) return;
    while (M.checked != M.next) {
        uint32_t i = M.checked % MESHFB_RECS;
        if (M.noted[i] && M.fence[i] > done) break;
        record_check(i);
        M.checked++;
    }
}

uint64_t vk_meshfb_short(void) {
    if (!M.ok) return 0;
    for (uint32_t i = 0; i < MESHFB_RECS; i++) record_check(i);
    return M.short_draws;
}

void vk_meshfb_shutdown(void) {
    VkCtx *c = vk_ctx();
    if (!c || !M.tried) return;
    if (vk_meshfb_short())
        rt_log("vk: mesh fallback: %llu of %llu draws emitted more than the buffers hold (mesh_fallback_mb of %llu or more would "
               "hold the largest)", (unsigned long long)M.short_draws, (unsigned long long)M.draws,
               (unsigned long long)((M.worst_bytes * 8 / 7 >> 20) + 1));
    if (M.scan) vkDestroyPipeline(c->device, M.scan, NULL);
    if (M.clamp) vkDestroyPipeline(c->device, M.clamp, NULL);
    if (M.layout) vkDestroyPipelineLayout(c->device, M.layout, NULL);
    if (M.dsl) vkDestroyDescriptorSetLayout(c->device, M.dsl, NULL);
    if (M.task) vmaDestroyBuffer(c->vma, M.task, M.task_a);
    if (M.mesh) vmaDestroyBuffer(c->vma, M.mesh, M.mesh_a);
    if (M.list) vmaDestroyBuffer(c->vma, M.list, M.list_a);
    if (M.rec) vmaDestroyBuffer(c->vma, M.rec, M.rec_a);
    if (M.stat) vmaDestroyBuffer(c->vma, M.stat, M.stat_a);
    memset(&M, 0, sizeof M);
}
