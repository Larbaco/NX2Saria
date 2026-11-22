// The camera's clearance from walls on pictures wider or narrower than 16:9 (camera_clearance.c): the patch set
// camera_clearance's host function, its start line and its totals.
#pragma once

// After rt_patches_init and mods_exefs_switch (boot.c rt_boot): logs whether the set is on ("camera clearance: ...").
void camera_clearance_start(void);
// At the orderly end (gfx_present.c rt_exit_steps): what the two sites loaded, one line each.
void camera_clearance_exit_log(void);
