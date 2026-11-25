#pragma once

#define MEM_NEED_DEFAULT_MB 24576.0

static inline double mem_need_room(double proc_mb, double used_mb, double limit_mb) {
    return limit_mb - used_mb + (proc_mb > 0 ? proc_mb : 0);
}

static inline int mem_need_short(double proc_mb, double used_mb, double limit_mb, double need_mb) {
    if (need_mb <= 0 || limit_mb < 0) return 0;
    return mem_need_room(proc_mb, used_mb, limit_mb) < need_mb;
}
