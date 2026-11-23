#pragma once
#include <windows.h>

HWND gfx_window_handle(void);
int input_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res);
int menu_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res);
