#pragma once
#include <stddef.h>
#include <stdint.h>

#include "preset_pick.h"

void hw_detect(PresetHw *hw, const char *no_probe, void (*before_probe)(void), char *skipped, size_t skipped_cap,
               char *note, size_t note_cap, double *probe_ms);

const PresetHw *cfg_preset_hw(void);

void hw_detect_device_check(uint32_t vendor, uint32_t device, uint32_t driver, const char *name);
