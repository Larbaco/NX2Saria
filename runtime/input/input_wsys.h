#pragma once
#include <stdint.h>

int ikb_foreground(void);
int ikb_down(int key);
int ikb_escape(void);
int ikb_pointer(float *nx, float *ny);
void ikb_pointer_set(float nx, float ny);
int ikb_captured(void);
void ikb_mouse_take(int32_t *dx, int32_t *dy);
uint32_t ikb_mouse_travel(void);
uint32_t ikb_mouse_presses(int right);
int32_t ikb_wheel_take(void);
void ikb_cursor_update(void);
void ikb_menu_closed(void);

int input_cursor_allowed(void);
