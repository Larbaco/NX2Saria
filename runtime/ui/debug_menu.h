#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
void debug_menu_layout(uint32_t w, uint32_t h, float scale);
void debug_menu_tab(int tab);
#define DEBUG_MENU_TABS 5
#ifdef __cplusplus
}
#endif
