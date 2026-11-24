#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MENU_SOUND_OPEN,
    MENU_SOUND_CLOSE,
    MENU_SOUND_CURSOR,
    MENU_SOUND_DECIDE,
    MENU_SOUND_CANCEL,
    MENU_SOUND_TAB_LEFT,
    MENU_SOUND_TAB_RIGHT,
    MENU_SOUND_COUNT
} MenuSound;

void menu_sound_play(MenuSound id);
void menu_sound_preload(void);
void menu_sound_mix(float *out, uint32_t frames, int channels, int rate);

#ifdef __cplusplus
}
#endif
