#include "host.h"
#include "layout.h"

#define VOICE_WAS_WRITTEN (MOD_MAIN_BASE + 0xa7d1c0ull)
#define VOICE_READ_OPTION (MOD_MAIN_BASE + 0x7e73d0ull)
#define VOICE_LOG_LIMIT 8

static uint64_t last_option;
static uint64_t refreshes;

void voice_language_host(Cpu *c) {
    uint64_t manager = c->x[0], key = (uint32_t)c->x[1];
    uint64_t written = rt_call(c, VOICE_WAS_WRITTEN, 2, manager, key);
    uint32_t option;
    uint64_t found = rt_call(c, VOICE_READ_OPTION, 3, manager, (uint64_t)(uintptr_t)&option, key);
    ret_x(c, written);
    if (!(found & 1)) return;

    uint64_t current = (uint64_t)option + 1;
    uint64_t previous = __atomic_exchange_n(&last_option, current, __ATOMIC_RELAXED);
    if ((written & 1) || current == previous) return;

    ret_x(c, 1);
    uint64_t count = __atomic_add_fetch(&refreshes, 1, __ATOMIC_RELAXED);
    if (count > VOICE_LOG_LIMIT) return;
    if (previous)
        rt_log("voice language: refresh %llu for option 0x%08x -> 0x%08x", (unsigned long long)count,
               (unsigned)(previous - 1), (unsigned)option);
    else
        rt_log("voice language: refresh %llu for initial option 0x%08x", (unsigned long long)count, (unsigned)option);
}
