#pragma once
#include <stdint.h>

#include "vk.h"

#define VK_GPA_CB_SLOT 0x40

int vk_gpa_wanted(void);
int vk_gpa_wanted_ctx(const VkCtx *c);
int vk_gpa_on(void);
uint64_t vk_gpa_device_address(VkBuffer b);
VkDescriptorBufferInfo vk_gpa_info(void);
uint32_t vk_gpa_map(uint64_t lo, uint64_t hi, VkBuffer buf, uint64_t buf_lo);
void vk_gpa_unmap(uint64_t lo, uint64_t hi, uint32_t rec, uint32_t (*cover)(uint64_t page_addr));
void vk_gpa_clear(uint64_t lo, uint64_t hi);
uint32_t vk_gpa_remap(uint64_t lo, uint64_t hi, uint32_t old_rec, VkBuffer buf, uint64_t buf_lo);
uint32_t vk_gpa_remap_plain(uint64_t lo, uint64_t hi, uint32_t old_rec, VkBuffer buf, uint64_t buf_lo);
int vk_gpa_flip_cmd(VkCommandBuffer cb, uint32_t rec, VkBuffer buf, uint64_t buf_lo, int keep_twin);
int vk_gpa_probe(uint64_t addr, uint64_t *first, uint64_t *twin);
void vk_gpa_report(void);
void vk_gpa_shutdown(void);
