#pragma once
#include <stdint.h>

void sprint_pad(uint64_t *buttons);
int sprint_step(int kind, int button, int hold);
int sprint_available(void);
