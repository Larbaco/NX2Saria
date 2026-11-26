#pragma once
#include <stdint.h>

#include "imgui.h"

void skin_art_prepare(int art, uint8_t *rgba, uint32_t w, uint32_t h);
void skin_art_halve(const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t dw, uint32_t dh);

void skin_style(ImGuiStyle &s);

void skin_atlas_add(ImFontAtlas *fa);
void skin_atlas_fill(ImFontAtlas *fa);

void skin_button(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float lift, bool dim);
float skin_cursor_reach(float s);
void skin_cursor_glow(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float alpha);
void skin_cursor_frame(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float alpha);
void skin_filigree(ImDrawList *dl, float x0, float x1, float y, float s, float alpha = 1);
float skin_filigree_height(float s);
void skin_panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float s, float alpha = 1);

void skin_boot_panel(ImDrawList *dl, ImFont *head, float head_px, ImFont *body, float body_px, const char *title,
                     const char *step, float frac, float sweep, float w, float h, float s);

bool skin_widget_button(const char *label, ImVec2 size, float s);

bool skin_tab_item(const char *id, const char *text, int flags, float s, ImFont *font);
void skin_header_tab(ImDrawList *dl, ImVec2 a, ImVec2 b, ImVec2 text_at, float s, bool current, const char *text,
                     ImFont *font, float font_px, float alpha);

#define SKIN_TEXT IM_COL32(244, 236, 214, 255)
#define SKIN_TEXT_DIM IM_COL32(206, 188, 150, 255)
#define SKIN_GOLD IM_COL32(236, 196, 104, 255)
