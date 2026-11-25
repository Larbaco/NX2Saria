#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

int vk_fg_wanted(void);
uint32_t vk_fg_queue_count(void);
void vk_fg_queues(VkQueue present, uint32_t present_family, VkQueue acquire, uint32_t acquire_family);
int vk_fg_has_queues(void);
int vk_fg_active(void);
int vk_fg_generating(void);
int vk_fsr3fg_generating(void);
VkResult vk_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc);
void vk_fg_destroy_swapchain(VkSwapchainKHR sc);
VkResult vk_fg_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img);
VkResult vk_fg_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx);
VkResult vk_fg_present(const VkPresentInfoKHR *pi);
void vk_fg_frame(VkCommandBuffer cb, VkSwapchainKHR sc, VkFormat fmt, VkExtent2D ext, VkImage hudless, VkImageView hudless_view,
                 const int pic[4]);
uint64_t vk_fg_presents(VkSwapchainKHR sc);
PFN_vkSetHdrMetadataEXT vk_fg_hdr_metadata_fn(void);
void vk_fg_release(void);
uint64_t vk_fg_vram(void);

int vk_fg4_available(void);
int vk_fg4_failed(void);
const char *vk_fg4_name(void);
int vk_fg4_active(void);
int vk_fg4_generating(void);
void vk_fg4_queues(VkQueue present, uint32_t present_family, VkQueue acquire, uint32_t acquire_family);
VkResult vk_fg4_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc);
void vk_fg4_destroy_swapchain(VkSwapchainKHR sc);
VkResult vk_fg4_get_images(VkSwapchainKHR sc, uint32_t *n, VkImage *img);
VkResult vk_fg4_acquire(VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx);
VkResult vk_fg4_present(const VkPresentInfoKHR *pi);
void vk_fg4_frame(VkCommandBuffer cb, VkSwapchainKHR sc, VkFormat fmt, VkExtent2D ext, VkImage hudless, const int pic[4]);
uint64_t vk_fg4_presents(VkSwapchainKHR sc);
PFN_vkSetHdrMetadataEXT vk_fg4_hdr_metadata_fn(void);
void vk_fg4_release(void);
uint64_t vk_fg4_vram(void);
