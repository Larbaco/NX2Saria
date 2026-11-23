#pragma once
#include <stdint.h>

enum { MOD_ROMFS = 1, MOD_EXEFS = 2, MOD_SUBSDK = 4, MOD_CHEATS = 8 };
typedef enum {
    MOD_OK,
    MOD_EMPTY,
    MOD_PLUGIN,
    MOD_EXEFS_LATER,
    MOD_EXEFS_UNSUPPORTED,
    MOD_EXEFS_OTHER,
    MOD_BAD_NAME,
} ModProblem;

typedef struct {
    char name[256];
    char romfs[2048];
    char exefs[2048];
    char cheats[2048];
    int kind, enabled;
    int start_enabled;
    ModProblem problem;
    uint32_t replaced, added, overridden;
    uint32_t other_version;
    char overridden_by[256];
    char note[256];
    uint32_t pending, pending_key;
    int nsets;
    uint32_t sets[16];
    char overrides[256];
} ModInfo;

void mods_scan(void);
const char *mods_dir(void);
int mods_count(void);
ModInfo *mods_at(int i);
void mods_set_enabled(int i, int on);
void mods_move(int i, int delta);
int mods_changed(void);
void mods_exefs_switch(void);
int mods_exefs_pending(uint32_t *key);
const char *mods_main_build_id(void);
