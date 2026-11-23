#pragma once
#include <stdint.h>

static inline const char *plat_commit_win_memory_advice(uint32_t error) {
    if (error == 1455u || error == 8u ||
        error == 1450u)
        return "Windows ran out of memory it can promise (RAM plus page file). Close memory-heavy programs and turn on "
               "or enlarge the page file (32 GB fixed on a 16 GB PC, 32768 MB), then restart Windows";
    return NULL;
}
