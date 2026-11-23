#pragma once
#include <stdint.h>

#include "input_map.h"

int ds3_read(uint8_t pd[PAD_COUNT], int32_t raw[4]);

int ds3_rumble(uint16_t low, uint16_t high, uint32_t ms);

enum { DS3_TRIGGER_ON = 128 };

int ds3_decode(const uint8_t *r, unsigned n, uint8_t pd[PAD_COUNT], int32_t raw[4]);

void ds3_output_report(uint8_t *o, unsigned n, int left, int right, unsigned dur, int led);
