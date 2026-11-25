#pragma once

static inline int mem_heap_summary_due(int census, int in_play, int slow) { return census || (!in_play && !slow); }
