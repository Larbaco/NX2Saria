#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

int fgt_init(uint32_t present_family, int *gen);
int fgt_prepare_begin(VkCommandBuffer cb, uint64_t id);
void fgt_prepare_end(VkCommandBuffer cb, uint64_t id);
int fgt_generate_begin(VkCommandBuffer cb, uint64_t id);
void fgt_generate_end(VkCommandBuffer cb, uint64_t id);
int fgt_poll(uint64_t now_id, double *ms, int max);
void fgt_idle(void);
void fgt_release(void);

#ifdef __cplusplus
}
#endif
