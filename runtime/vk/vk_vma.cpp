extern "C" void vk_vma_leak_line(const char *fmt, ...);
extern "C" void vk_vma_leak_found(void);
#define VMA_LEAK_LOG_FORMAT(format, ...) vk_vma_leak_line(format, __VA_ARGS__)
#define VMA_ASSERT_LEAK(expr) do { if (!(expr)) vk_vma_leak_found(); } while (0)
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#include <vulkan/vulkan.h>
#include "vk_mem_alloc.h"
