#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

void ov_operation_mode_live(void);
int ov_handheld(void);
const char *ov_language(void);
const char *ov_shared_font_dir(char *buf, size_t n);
uint64_t ov_bfttf_size(const uint8_t *p, uint64_t len);
