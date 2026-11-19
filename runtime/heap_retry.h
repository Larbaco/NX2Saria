// Model heaps that run full (heap_retry.c): the patch set heap_retry's host functions, its start line and its totals.
#pragma once

// After rt_patches_init: reads the test switches and logs whether the set is on ("heap retry: ...").
void heap_retry_start(void);
// At the orderly end (gfx_present.c rt_exit_steps): the session's totals, one line.
void heap_retry_exit_log(void);
