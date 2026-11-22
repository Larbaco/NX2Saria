// boot_map (boot_map.c): the game starts straight in a chosen area from the save of a slot, without the title screen
// and its file menu. docs/boot-to-map.md.
#pragma once
#include <stdint.h>

#include "platform/platform.h"

// Before the guest starts (boot.c rt_boot): reads the settings, checks the destination and the save, and arms the
// changes to what the game reads. Logs one line either way.
void boot_map_init(void);
// fs_host.c, when the game opens a save file to read (guest path): 1 when its reads get the boot map's place (the
// slot's progress.sav while the boot map is pending), 0 to read it as it is. f is the opened file.
int boot_map_save_open(const char *path, PlatFile *f);
// fs_host.c, after a read of n bytes at offset from a file boot_map_save_open took.
void boot_map_save_read(uint64_t offset, void *buf, uint64_t n);
// The game's thread, at each pad read (ov_input.c): ends the boot map once Link is in the world, with its log line.
void boot_map_tick(void);
