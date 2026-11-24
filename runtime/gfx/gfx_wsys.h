#pragma once
#include "vk.h"

int wsys_open(void);
VkSurfaceKHR wsys_surface(VkInstance inst);
void wsys_client_size(int *w, int *h);
int wsys_minimized(void);
void wsys_resize(int w, int h);
void wsys_probe_focus(int in);
void wsys_probe_escape(void);
int wsys_probe_click(int where);

void gfx_window_mark_dirty(void);
void gfx_window_focus_changed(int in);
void gfx_window_closed(void);
void gfx_window_set_refresh(double hz);
int gfx_cfg_fullscreen(void);
void gfx_cfg_window_size(int *w, int *h);
