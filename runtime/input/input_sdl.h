#pragma once
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

SDL_Window *gfx_window_sdl(void);
double gfx_window_sdl_density(void);
void gfx_window_sdl_run(void (*fn)(void *arg), void *arg);

void input_sdl_window(SDL_Window *w);
void input_sdl_event(const SDL_Event *e);
void input_sdl_idle(void);

void menu_sdl_event(const SDL_Event *e);

#ifdef __cplusplus
}
#endif
