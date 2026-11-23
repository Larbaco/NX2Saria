#pragma once
#include <stdint.h>

extern volatile uint32_t prof_render_tid;
extern volatile uint32_t prof_main_tid;
extern volatile uint32_t prof_worker_tid[16];
extern volatile uint32_t prof_replay_tid;

int prof_sampler_start(void);
void prof_sampler_write(void);
void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d);
int prof_commit_census(uint64_t frame, uint64_t *wc);
