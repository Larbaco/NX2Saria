#pragma once
#include "imgui.h"

typedef enum {
    MENU_ART_FRAME_DECO,
    MENU_ART_GUIDE_BAR,
    MENU_ART_WINDOW_BASE,
    MENU_ART_BTN_RIM,
    MENU_ART_BTN_FILL,
    MENU_ART_BTN_GRAIN,
    MENU_ART_BTN_EDGE,
    MENU_ART_BTN_LINE,
    MENU_ART_CURSOR_GLOW,
    MENU_ART_CURSOR_LINE,
    MENU_ART_PANEL_FILL,
    MENU_ART_DIVIDER,
    MENU_ART_DIVIDER_LINE,
    MENU_ART_TAB_SELECT,
    MENU_ART_COUNT
} MenuArt;

extern "C" void menu_art_start(void);
extern "C" int menu_art(MenuArt id, ImTextureID *tex, float *w, float *h);
extern "C" void menu_art_release(void);
extern "C" int menu_font_upload(const unsigned char *rgba, int w, int h, ImTextureID *tex);
extern "C" int menu_vk_errors(void);
extern "C" int menu_art_draw(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, ImU32 tint);
extern "C" int menu_art_draw9(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, float corner, float frac, ImU32 tint, int fill);
extern "C" int menu_art_draw9_turned(ImDrawList *dl, MenuArt id, ImVec2 a, ImVec2 b, float corner, float frac, ImU32 tint, int fill,
                                     int turned);
