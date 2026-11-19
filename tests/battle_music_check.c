// The sound gate and its battle-music reads, with a stand-in battle manager and replaceable flags.
#include <assert.h>
#include <stdio.h>

#include "host.h"
#include "layout.h"

void battle_music_gate(int run);
void battle_music_flags_host(Cpu *c);

static const uint64_t manager_slot = 0x7100000000ull;
static uint64_t manager = 0x7200000000ull;
static int enabled = 1;
static uint8_t flags;
static unsigned logs;

int rt_patch_set(const char *name) {
    assert(!strcmp(name, "obgm"));
    return 0;
}
int rt_patch_enabled(int set) { return set == 0 && enabled; }

int plat_mem_read(uint64_t address, void *out, size_t size) {
    if (address == MOD_MAIN_BASE + 0x4c7dda0 && size == 8)
        return memcpy(out, &manager_slot, 8), 1;
    if (address == manager_slot && size == 8)
        return memcpy(out, &manager, 8), 1;
    if (address == manager + 0x4898 && size == 1)
        return memcpy(out, &flags, 1), 1;
    return 0;
}

void rt_log(const char *fmt, ...) {
    assert(strstr(fmt, "battle music change"));
    logs++;
}

static unsigned read_flags(unsigned site, unsigned original) {
    static const unsigned offsets[] = {0x3353684, 0x33537c4, 0x3353954, 0x3353a50};
    Cpu c = {0};
    c.x[26] = manager + 0x4000;
    c.x[30] = MOD_MAIN_BASE + offsets[site];
    c.x[8] = original;
    battle_music_flags_host(&c);
    return (unsigned)c.x[8];
}

int main(void) {
    flags = 0xa0;
    battle_music_gate(0);
    assert(read_flags(0, 0) == 0);
    flags = 0x42;
    battle_music_gate(0);
    flags = 0;
    battle_music_gate(1);
    assert(read_flags(0, 0x08) == 0x28);
    assert(read_flags(1, 0) == 0x02);
    assert(read_flags(2, 0x80) == 0x80);
    assert(read_flags(3, 0) == 0x40);
    assert(logs == 3);
    assert(read_flags(0, 0x20) == 0x20 && logs == 3);  // the guest already saw this bit
    for (int i = 0; i < 20; i++) assert(read_flags(0, 0) == 0x20);
    assert(logs == 8);  // multiple sound objects see the same pulse in one frame
    battle_music_gate(1);
    assert(read_flags(0, 0) == 0 && read_flags(3, 0) == 0);

    flags = 0x20;
    battle_music_gate(0);
    manager += 0x10000;
    flags = 0x04;
    battle_music_gate(0);
    battle_music_gate(1);
    assert(read_flags(0, 0) == 0 && read_flags(2, 0) == 0x04);

    flags = 0x20;
    battle_music_gate(0);  // phases between two sound updates must not see the old pulse
    assert(read_flags(2, 0) == 0);
    manager += 0x10000;
    battle_music_gate(1);
    assert(read_flags(0, 0) == 0);

    flags = 0x20;
    battle_music_gate(0);
    enabled = 0;
    battle_music_gate(1);
    enabled = 1;
    battle_music_gate(1);
    assert(read_flags(0, 0) == 0);
    puts("battle_music_check: held flags merge for one sound update and follow the battle manager's identity");
    return 0;
}
