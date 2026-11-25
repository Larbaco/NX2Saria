#pragma once
#include "vk.h"
#include "mfb_batch.h"

int vk_meshfb_init(void);
VkDescriptorBufferInfo vk_meshfb_buffer(int cb_slot);
uint64_t vk_meshfb_mesh_words(void);

VkDeviceSize vk_meshfb_record(VkCommandBuffer cb, VkBuffer src, VkDeviceSize src_off, uint32_t count, int has_task,
                              uint32_t verts, uint32_t slot_words, MfbHist *owner, VkBuffer *rec, int *cmds);
int vk_meshfb_part_take(uint64_t task_b, uint64_t list_b, uint64_t mesh_b);
void vk_meshfb_part_done(void);
void vk_meshfb_part_reset(void);
int vk_meshfb_part_first(void);
void vk_meshfb_one_by_one(void);
void vk_exec_mfb_batch_start(int fallback, int fallback_default, const char *path_name);
void vk_exec_mfb_batch_exit_log(void);
int vk_meshfb_closed(void);
void vk_meshfb_full_barrier(VkCommandBuffer cb);
void vk_meshfb_pre_end(VkCommandBuffer cb);
uint64_t vk_meshfb_batch_short(void);
uint64_t vk_meshfb_batch_readmits(void);
uint64_t vk_meshfb_list_bytes(void);
void vk_meshfb_caps(uint64_t *task, uint64_t *mesh);
uint64_t vk_meshfb_batch_peak(uint64_t *cap);
uint64_t vk_meshfb_slot_room(uint32_t slot_words, int has_task);
void vk_meshfb_clamp(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words);
void vk_meshfb_scan(VkCommandBuffer cb, VkDeviceSize at, uint32_t task_words, uint32_t fan, uint32_t slot_words, uint32_t verts,
                    int clamped);
void vk_meshfb_note(VkCommandBuffer cb, VkDeviceSize at, uint64_t fence);
void vk_meshfb_poll(uint64_t done);
uint64_t vk_meshfb_short(void);
void vk_meshfb_shutdown(void);
