// The mesh fallback's buffers and its records (vk_mesh_fb.c); the modules are mesh_fb.c's, the draws
// vk_exec_draw.c's ("mesh fallback").
#pragma once
#include "vk.h"
#include "mfb_batch.h"

// Creates the buffers and the scan pipeline on first use. 0 when that failed (logged once): the draws are skipped.
int vk_meshfb_init(void);
// The buffer a module's extra storage binding reads or writes (mesh_fb.h MESHFB_CB_*), whole or as much of it as one
// storage binding may span.
VkDescriptorBufferInfo vk_meshfb_buffer(int cb_slot);
uint64_t vk_meshfb_mesh_words(void);   // the mesh buffer's size as a binding sees it, in words

// A draw's record, 64 bytes of the record buffer (*rec) at the offset returned: the task dispatch at +0, the mesh
// dispatch at +16, the draw (VkDrawIndirectCommand) at +32. Records the fills and copies that start it, outside a
// pass: the workgroup count from 4 bytes at src_off of src, or `count` when src is VK_NULL_HANDLE. Without a task
// stage that count is the mesh dispatch and the draw's instance count; with one it is the task dispatch, and
// vk_meshfb_scan writes the rest. slot_words: of the draw's mesh program, for the report of a draw the mesh buffer
// was too small for. Returns the number of commands recorded in *cmds.
// owner: the program's history (MfbHist), which the record's read back updates; NULL: none.
VkDeviceSize vk_meshfb_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_off, uint32_t count, int has_task,
                              uint32_t verts, uint32_t slot_words, MfbHist *owner, VkBuffer *rec, int *cmds);
// A batch (vk_exec_draw.c mfb_draw): the next part of the buffers, of these many bytes each, for the draw about to be
// recorded; until vk_meshfb_part_done, vk_meshfb_buffer, the record, the clamp and the scan use it. 0: no room left.
// vk_meshfb_part_reset: at the end of a pre-pass command buffer (vk_sync.c pre_end) the parts start over.
int vk_meshfb_part_take(uint64_t task_b, uint64_t list_b, uint64_t mesh_b);
void vk_meshfb_part_done(void);
void vk_meshfb_part_reset(void);
int vk_meshfb_part_first(void);           // no part taken since the last reset
void vk_meshfb_one_by_one(void);          // a draw goes one by one: after parts were taken, the next batched cuts
void vk_exec_mfb_batch_start(int fallback_default);   // reads and logs NX2SARIA_MESH_FALLBACK_BATCH (vk_exec_draw.c)
void vk_exec_mfb_batch_exit_log(void);   // gfx_present.c rt_exit_steps: the batch's totals, with it on
int vk_meshfb_closed(void);               // a draw went one by one since the parts began: the next batched cuts
void vk_meshfb_full_barrier(VkCommandBuffer cb);   // a full memory barrier in a command buffer that is not the part's
void vk_meshfb_pre_end(VkCommandBuffer cb);        // vk_sync.c pre_end: the barrier the parts' draws need, and the reset
uint64_t vk_meshfb_batch_short(void);     // batched draws that asked for more than their part held, so far
uint64_t vk_meshfb_batch_readmits(void);  // programs taken back into batches after a draw short of its part
uint64_t vk_meshfb_list_bytes(void);       // the list buffer's bytes
void vk_meshfb_caps(uint64_t *task, uint64_t *mesh);   // the task and mesh buffers' bytes
uint64_t vk_meshfb_batch_peak(uint64_t *cap);   // the most of the mesh buffer (*cap bytes) one pre-pass's parts took
uint64_t vk_meshfb_slot_room(uint32_t slot_words, int has_task);   // the mesh slots a one-by-one draw has room for
// For a record whose count came from src (a count the GPU wrote): cuts it to what the buffers hold, in the task dispatch
// (task_words, the task stage's payload words) or without a task stage (0) in the mesh dispatch and the instances
// (shaders/meshfb_clamp.comp). Binds its own compute pipeline and set; four commands, between two barriers.
void vk_meshfb_clamp(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words);
// The pass between the task and the mesh dispatch (shaders/meshfb_scan.comp): binds its own compute pipeline and set.
void vk_meshfb_scan(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words, uint32_t fan, uint32_t slot_words, uint32_t verts,
                    int clamped);   // clamped: vk_meshfb_clamp ran for the record (it wrote word 13)
// After the record is complete (the copies or the scan, and a barrier): keeps what the draw asked for where the host
// reads it later. One copy command.
void vk_meshfb_note(VkCommandBuffer cb, VkDeviceSize at, uint64_t fence);   // fence: the submission's value
// The records of submissions the GPU finished (done), read back: the programs' histories (mfb_batch.h), the short draws.
void vk_meshfb_poll(uint64_t done);
// Draws so far whose programs emitted more than the buffers hold, the records not looked at yet included: call it
// with the device idle (a probe, the shutdown).
uint64_t vk_meshfb_short(void);
void vk_meshfb_shutdown(void);   // the device must be idle
