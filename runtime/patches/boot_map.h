#pragma once
#include <stdint.h>

#include "platform/platform.h"

void boot_map_init(void);
int boot_map_save_open(const char *path, PlatFile *f);
void boot_map_save_read(uint64_t offset, void *buf, uint64_t n);
void boot_map_tick(void);
