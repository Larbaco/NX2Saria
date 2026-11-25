#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

int vk_dlssg_wanted(void);
int vk_dlssg_early(void);
VkResult vk_dlssg_create_instance(const VkInstanceCreateInfo *ci, VkInstance *out);
VkResult vk_dlssg_enumerate(VkInstance inst, uint32_t *n, VkPhysicalDevice *pd);
PFN_vkVoidFunction vk_dlssg_instance_proc(VkInstance inst, const char *name);
int vk_dlssg_interposed(void);
VkResult vk_dlssg_create_device(VkInstance inst, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, VkDevice *out);
void vk_dlssg_device_ready(VkPhysicalDevice physical, VkDevice dev);
PFN_vkGetDeviceProcAddr vk_dlssg_dpa(void);
int vk_dlssg_active(void);
VkResult vk_dlssg_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *sc);
void vk_dlssg_destroy_swapchain(VkDevice d, VkSwapchainKHR sc);
VkResult vk_dlssg_get_images(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *img);
VkResult vk_dlssg_acquire(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, uint32_t *idx);
VkResult vk_dlssg_present(VkQueue q, const VkPresentInfoKHR *pi);
void vk_dlssg_frame(VkCommandBuffer cb, VkExtent2D ext);
void vk_dlssg_frame_start(void);
int vk_dlssg_pace(uint64_t period_ns);
void vk_dlssg_game_present(void);
void vk_dlssg_submitted(uint64_t frame);
void vk_dlssg_present_frame(uint64_t frame);
uint64_t vk_dlssg_shown(void);
void vk_dlssg_release(void);

int vk_dlssg_multiplier_of(const char *value, int max, const char **why);
int vk_dlssg_multiplier_max(int *old_dll);
int vk_dlssg_multiplier(void);
enum { DLSSG_NOT_WANTED, DLSSG_RUNS, DLSSG_NO_FILES, DLSSG_UNAVAILABLE };
int vk_dlssg_status(void);
int vk_dlssg_why(const char **sl_why);
int vk_dlssg_gpu_may(void);
