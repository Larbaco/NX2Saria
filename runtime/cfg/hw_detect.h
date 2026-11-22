// The hardware the quality presets are chosen by (hw_detect.c; the choice itself: preset_pick.h).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "preset_pick.h"

// Fills *hw from this PC: the GPU the game would use (a Vulkan probe of its own, see hw_detect.c), the processor's
// cores and logical processors, the installed memory and the handheld the firmware names. Called once, by cfg.c
// cfg_start once the settings are read and the guest address space is claimed. no_probe: why the caller leaves the
// Vulkan drivers alone (a tool mode, preset_probe = 0, a probe of an earlier start that did not finish), NULL or ""
// to probe. before_probe (may be NULL) runs right before the probe's first Vulkan call, only when the probe runs (cfg.c
// marks the state file with it). When the probe does not run, *skipped gets why (else ""), and the GPU fields stay
// empty for the caller to fill from the last probe's result; *note gets why a probe that ran found no GPU (else "");
// *probe_ms the probe's time, the drivers' first load included.
void hw_detect(PresetHw *hw, const char *no_probe, void (*before_probe)(void), char *skipped, size_t skipped_cap,
               char *note, size_t note_cap, double *probe_ms);

// The hardware the preset in use was chosen for (cfg.c: the probe's, or the last probe's from the state file).
// NULL before cfg_start. Never changes after it.
const PresetHw *cfg_preset_hw(void);

// vk_device.c pick_device, once the game's own device is chosen: logs whether it is the GPU the preset was chosen for
// ("preset: the Vulkan device matches the probe" or "... differs from the probe: ...").
void hw_detect_device_check(uint32_t vendor, uint32_t device, uint32_t driver, const char *name);
