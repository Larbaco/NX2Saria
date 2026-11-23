#pragma once
#include <stdint.h>

#include "rumble_hd.h"

int hd_pair_find(void);
int hd_pair_write(const uint8_t data[RUMBLE_HD_DATA]);
int hd_pair_is_open(void);
void hd_pair_close(int neutral, unsigned wait_ms);
void hd_pair_end(unsigned wait_ms);
