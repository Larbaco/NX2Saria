#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

int menu_is_open(void);
void menu_toggle(void);
void menu_cfg_reload(void);
void menu_perf_cycle(void);

void menu_event_key(int key, int down);
int menu_debug_enabled(void);
void menu_debug_toggle(void);
void menu_event_mouse_pos(float x, float y);
void menu_event_mouse_button(int button, int down);
void menu_event_wheel(float dx, float dy);
void menu_event_focus(int focused);
void menu_event_char(unsigned c);
void menu_event_paste(const char *utf8);
int menu_paste_take(char *out, size_t cap);

typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkFormat format;
    uint32_t width, height;
    int image_w, image_h;
    int scaler;
} MenuTarget;
int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot);
void menu_overlay_draw(VkCommandBuffer cb);
void menu_boot_set(int phase, long long done, long long total);
int menu_boot_frame(const MenuTarget *t);
void menu_vk_release(void);
int menu_font_check(void);
void menu_text_input_follow(int want);

void gfx_window_apply_settings(void);
void gfx_window_request_close(void);
void gfx_window_request_restart(void);
void gfx_window_text_input(int on);
void gfx_window_paste(void);
int gfx_window_monitor_size(int *w, int *h);
double gfx_window_refresh_hz(void);
unsigned gfx_window_dpi(void);
const char *gfx_window_present_mode(void);
double gfx_output_fps(void);

#ifdef __cplusplus
}
#endif
