#pragma once
#include <stdint.h>

#define MOTION_OWNER_BINDINGS 512
#define MOTION_OWNER_RIGID_EQUIPMENT 1u
typedef struct {
    uint64_t player, actor, link, model, part, lifetime;
    uint32_t generation, kind, flags;
} MotionOwner;
typedef struct {
    uint64_t address, size, observed, buffer;
    MotionOwner owner;
    int ambiguous;
} MotionOwnerBinding;
typedef struct {
    MotionOwnerBinding bindings[MOTION_OWNER_BINDINGS];
    unsigned count;
} MotionOwnerSnapshot;

struct Cpu;
void motion_owner_observe(struct Cpu *cpu, uint64_t address, uint64_t size);
void motion_owner_capture(MotionOwnerSnapshot *out);
void motion_owner_present(void);
void motion_owner_forget(uint64_t buffer);
void motion_owner_reset(void);
void motion_owner_forget_range(uint64_t address, uint64_t size);
int vk_usnap_motion_owner(uint64_t address, uint64_t size, MotionOwner *out);
int motion_owner_enabled(void);
