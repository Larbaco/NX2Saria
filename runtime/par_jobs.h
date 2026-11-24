#pragma once
#include <stddef.h>

void par_run(int n, void (*fn)(void *arg, int i), void *arg);
int par_threads(void);
void par_memcpy(void *dst, const void *src, size_t n);
