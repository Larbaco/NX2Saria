#pragma once
#include <stddef.h>

int lib_origin_line(char *out, size_t cap, int loaded, int built, const char *sdl_path, const char *exe_path,
                    int sha_ni);
