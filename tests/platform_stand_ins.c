#include <stdlib.h>

const char *cfg_env(const char *name) { return getenv(name); }

#ifndef _WIN32
#include <SDL3/SDL_clipboard.h>

bool SDL_SetClipboardText(const char *text) { (void)text; return false; }
char *SDL_GetClipboardText(void) { return NULL; }
void SDL_free(void *p) { free(p); }
#endif
