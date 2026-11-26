#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int rt_game_version = GAME_VERSION;

#include "foliage_distance.c"

static char logged[8][600];
static int nlogged;
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    assert(nlogged < 8);
    vsnprintf(logged[nlogged++], sizeof logged[0], fmt, ap);
    va_end(ap);
}
static const char *setting;
const char *cfg_value(const char *name) {
    assert(!strcmp(name, "NX2SARIA_FOLIAGE_DISTANCE"));
    return setting ? setting : "1";
}
static uint8_t image[FOLIAGE_LOD_OFF + 0x1000] __attribute__((aligned(4096)));
static RtLoaded main_mod;
static int loaded = 1;
int ldr_count(void) { return loaded; }
const RtLoaded *ldr_module(int i) { return i == 0 ? &main_mod : NULL; }
static int prot = PLAT_PROT_READ, protects;
int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    (void)addr;
    *end = limit;
    return prot;
}
int plat_mem_protect(uint64_t addr, uint64_t n, int p) {
    assert(addr == ((uint64_t)(uintptr_t)image + FOLIAGE_LOD_OFF & ~0xFFFull) && n == 0x1000);
    prot = p;
    protects++;
    return 1;
}

static float *lod(void) { return (float *)(image + FOLIAGE_LOD_OFF); }
static void reset(const char *v) {
    memcpy(lod(), foliage_game, sizeof foliage_game);
    setting = v, nlogged = 0, protects = 0, prot = PLAT_PROT_READ;
}
static int run(const char *v) {
    reset(v);
    foliage_distance_init();
    assert(nlogged == 1);
    printf("%-6s %s\n", v ? v : "unset", logged[0]);
    return 1;
}

int main(void) {
    main_mod = (RtLoaded){.name = "main", .base = (uint64_t)(uintptr_t)image, .size = sizeof image};
    float f;
    assert(foliage_factor(NULL, &f) == 0 && f == 1.0f);
    assert(foliage_factor("", &f) == 0 && f == 1.0f);
    assert(foliage_factor("2", &f) == 0 && f == 2.0f);
    assert(foliage_factor("0.5", &f) == 0 && f == 0.5f);
    assert(foliage_factor("3.0", &f) == 0 && f == 3.0f);
    assert(foliage_factor("1.5 ", &f) == 0 && f == 1.5f);
    assert(foliage_factor("0.2", &f) == 1 && f == 0.5f);
    assert(foliage_factor("10", &f) == 1 && f == 3.0f);
    assert(foliage_factor("far", &f) == -1 && f == 1.0f);
    assert(foliage_factor("2x", &f) == -1 && f == 1.0f);
    assert(foliage_factor("nan", &f) == -1 && f == 1.0f);
    float out[FOLIAGE_FLOATS];
    assert(foliage_scale(1.0f, foliage_game, out) == 0 && !memcmp(out, foliage_game, sizeof out));
    assert(foliage_scale(0.5f, foliage_game, out) == 0);
    for (int i = 0; i < FOLIAGE_FLOATS; i++) assert(out[i] == foliage_game[i] * 0.5f);
    assert(foliage_scale(2.0f, foliage_game, out) == 0);
    assert(out[0] == 80 && out[1] == 120 && out[2] == 70 && out[3] == 60 && out[5] == 270 && out[7] == 90);
    assert(foliage_scale(3.0f, foliage_game, out) == 2);
    assert(out[2] == 75 && out[3] == 70 && out[0] == 120 && out[5] == 405);
    float big[FOLIAGE_FLOATS];
    memcpy(big, foliage_game, sizeof big);
    big[2] = 90;
    assert(foliage_scale(0.5f, big, out) == 0 && out[2] == 45);
    assert(foliage_scale(2.0f, big, out) == 1 && out[2] == 90);

#if GAME_VERSION == 111
    run(NULL);
    assert(!memcmp(lod(), foliage_game, sizeof foliage_game) && protects == 0);
    assert(strstr(logged[0], "foliage: distance factor 1.00 (the game's: 40, 60, 35, 30, 40, 135, 10, 45 m; now: the same)"));
    run("2");
    assert(lod()[0] == 80 && lod()[5] == 270 && lod()[6] == 20 && prot == PLAT_PROT_READ && protects == 2);
    assert(strstr(logged[0], "factor 2.00 (the game's: 40, 60, 35, 30, 40, 135, 10, 45 m; now: 80, 120, 70, 60, 80, 270, 20, 90 m)"));
    run("0.5");
    assert(lod()[1] == 30 && lod()[7] == 22.5f);
    assert(strstr(logged[0], "now: 20, 30, 17.5, 15, 20, 67.5, 5, 22.5 m)"));
    run("9");
    assert(lod()[2] == 75 && lod()[3] == 70 && lod()[5] == 405);
    assert(strstr(logged[0], "at most 75 m") && strstr(logged[0], "foliage_distance = 9 is outside 0.5 to 3: 3 is used"));
    run("far");
    assert(!memcmp(lod(), foliage_game, sizeof foliage_game) && strstr(logged[0], "far is not a number: 1 is used"));
    reset("2");
    lod()[0] = 50;
    foliage_distance_init();
    assert(lod()[0] == 50 && lod()[1] == 60 && strstr(logged[0], "not applied: main+0x3eefd38 holds 50, 60"));
    printf("       %s\n", logged[0]);
    reset("2");
    prot = PLAT_PROT_RW;
    foliage_distance_init();
    assert(lod()[0] == 80 && protects == 0);
    reset("2");
    loaded = 0;
    foliage_distance_init();
    assert(lod()[0] == 40 && strstr(logged[0], "not applied: main is not loaded"));
    loaded = 1;
#else
    run("2");
    assert(!memcmp(lod(), foliage_game, sizeof foliage_game) && protects == 0);
    assert(strstr(logged[0], "factor 2.00 not applied: the foliage's distances are known for game 1.1.1 only (this is 1.1.0)"));
#endif
    printf("foliage_distance_check: ok (game %d)\n", GAME_VERSION);
    return 0;
}
