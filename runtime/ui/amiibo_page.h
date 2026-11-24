#pragma once
#include <stdint.h>

void tab_amiibo(float scale);

bool amiibo_picker_wanted(void);
void amiibo_picker_begin(void);
bool amiibo_picker_layout(uint32_t w, uint32_t h, float scale);
bool amiibo_note_shows(void);
void amiibo_note_draw(uint32_t w, float scale);
