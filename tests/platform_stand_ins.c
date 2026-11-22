// Stand-ins for what the platform layer (runtime/platform/linux.c, win32.c) calls outside it, for the tests that link
// it with the rest of the runtime left out: the settings' environment read (runtime/cfg.c cfg_env) and, on Linux,
// SDL's clipboard (linux.c plat_clipboard_set/get, whose header tests/platform_stand_ins.py writes). Linked by the
// tests that call tests/platform_stand_ins.py stand_ins().
#include <stdlib.h>

// cfg_env as runtime/cfg.c has it for a name outside the settings' prefixes: the environment's own value
const char *cfg_env(const char *name) { return getenv(name); }

#ifndef _WIN32
#include <SDL3/SDL_clipboard.h>

// no clipboard: a set fails and a get finds nothing
bool SDL_SetClipboardText(const char *text) { (void)text; return false; }
char *SDL_GetClipboardText(void) { return NULL; }
void SDL_free(void *p) { free(p); }
#endif
