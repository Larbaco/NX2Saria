#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

struct VkCtx;

void vk_lowlat_init(struct VkCtx *c);
void vk_lowlat_release(void);

enum { VK_LOWLAT_NV, VK_LOWLAT_AMD, VK_LOWLAT_NO_DRIVER, VK_LOWLAT_DLSSG, VK_LOWLAT_FG, VK_LOWLAT_FAILED,
       VK_LOWLAT_RESTART };
int vk_lowlat_status(void);
int vk_lowlat_on(void);

int vk_lowlat_swapchain_wanted(void);
void vk_lowlat_swapchain(VkSwapchainKHR sc, int made);
void vk_lowlat_swapchain_refused(int result);
void vk_lowlat_fg_presents(int on);

void vk_lowlat_frame_start(int at_input);
void vk_lowlat_submit_start(void);
void vk_lowlat_game_present(void);
void vk_lowlat_submitted(uint64_t frame);
int vk_lowlat_pace(uint64_t period_ns);
int vk_lowlat_submit_tag(VkLatencySubmissionPresentIdNV *tag);

void vk_lowlat_present_frame(uint64_t frame);
uint64_t vk_lowlat_blit_frame(VkLatencySubmissionPresentIdNV *tag, int *tagged);
typedef struct { VkPresentIdKHR info; uint64_t id; } VkLowlatPresent;
const void *vk_lowlat_present_begin(uint64_t frame, VkLowlatPresent *p);
void vk_lowlat_present_end(const VkLowlatPresent *p);
