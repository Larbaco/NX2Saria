#include "host.h"
#include "layout.h"
#include "platform/platform.h"
#include <stdatomic.h>

#define BATTLE_MANAGER_SLOT 0x4c7dda0
#define BATTLE_CHANGE_BYTE 0x4898
#define BATTLE_CHANGE_MASK 0x66u

static uint64_t pending;
static _Atomic uint64_t active;

static uint64_t change_address(void) {
    uint64_t slot = 0, manager = 0;
    if (!plat_mem_read(MOD_MAIN_BASE + BATTLE_MANAGER_SLOT, &slot, sizeof slot) || !slot ||
        !plat_mem_read(slot, &manager, sizeof manager) || !manager)
        return 0;
    return manager + BATTLE_CHANGE_BYTE;
}

void battle_music_gate(int run) {
    static int set = -2;
    if (set == -2) set = rt_patch_set("obgm");
    if (!rt_patch_enabled(set)) {
        pending = 0;
        atomic_store_explicit(&active, 0, memory_order_release);
        return;
    }
    if (run) {
        atomic_store_explicit(&active, pending, memory_order_release);
        pending = 0;
        return;
    }
    atomic_store_explicit(&active, 0, memory_order_release);
    uint64_t address = change_address();
    uint8_t flags;
    if (!address || !plat_mem_read(address, &flags, sizeof flags)) {
        pending = 0;
        return;
    }
    uint64_t tag = address << 8;
    if ((pending >> 8) != address) pending = tag;
    pending |= flags & BATTLE_CHANGE_MASK;
}

void battle_music_flags_host(Cpu *c) {
    unsigned bit;
    switch (c->x[30] - MOD_MAIN_BASE) {
    case 0x3353684: bit = 1u << 5; break;
    case 0x33537c4: bit = 1u << 1; break;
    case 0x3353954: bit = 1u << 2; break;
    case 0x3353a50: bit = 1u << 6; break;
    default: return;
    }
    uint64_t saved = atomic_load_explicit(&active, memory_order_acquire);
    uint64_t address = c->x[26] + 0x898;
    if ((saved >> 8) != address || !(saved & bit) || (c->x[8] & bit)) return;

    unsigned before = (unsigned)c->x[8];
    c->x[8] |= bit;
    static _Atomic unsigned logged;
    if (atomic_load_explicit(&logged, memory_order_relaxed) < 8) {
        unsigned n = atomic_fetch_add_explicit(&logged, 1, memory_order_relaxed);
        if (n < 8) rt_log("frame rate: battle music change #%u: flags %#x -> %#x", n + 1, before, (unsigned)c->x[8]);
    }
}
