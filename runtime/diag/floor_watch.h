#pragma once
#include <stdint.h>

int floor_watch_wanted(void);
int floor_watch_found(const uint64_t *addr, const float *val, const int *unit, int n, const float link[3]);
int floor_watch_present(const float link[3], int link_ok, float frames);
