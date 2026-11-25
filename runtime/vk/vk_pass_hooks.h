#pragma once

static inline int pass_hooks_due(int in_chunk, int idx, int ran) { return !in_chunk || (!idx && !ran); }
