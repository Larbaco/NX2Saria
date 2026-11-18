// The game log's "libs:" line (main.c log_libs), near its top: the form SHA-256 runs in, the SDL the process loaded
// and which file it came from: one beside the game (the runpath $ORIGIN takes it ahead of the system's) or another,
// so the log of a player's session says which one their game runs with.
#pragma once
#include <stddef.h>

// The line without its "libs: " prefix into out. loaded and built: SDL_VERSIONNUM values (the library's
// SDL_GetVersion, the headers' SDL_VERSION); sdl_path: the loaded library's file (plat_library_path), "" when it was
// not found among the loaded modules; exe_path: the running executable (plat_exe_path), "" when unknown; sha_ni:
// sha256_ni(). Returns the whole line's length as snprintf counts it (out holds at most cap - 1 of it), -1 on an
// encoding error.
int lib_origin_line(char *out, size_t cap, int loaded, int built, const char *sdl_path, const char *exe_path,
                    int sha_ni);
