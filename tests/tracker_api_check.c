// runtime/diag/tracker_api.c served over the real platform layer, with a guest memory of the test's own at the game's own
// addresses: main's window at 0x7000000000 with a MOD0 header where 1.1.1's main has it, and a heap at 0x1030000000
// (the stack and heap block of a run with the tracker API on) with a hole the lazy heap never committed, a page nobody
// may read, and pages never written. Beside them, at 0x2000000000, memory of the host's own holding a secret: the
// game's memory map answers for it as taken memory the game can read, as memory.c query_locked does for every host
// allocation below 2^39. Built and run by tests/tracker_api_check.py, which is the client: the program prints
// "READY <port> <resident heap pages>" once it listens, logs every rt_log line to stdout, and prints
// "RESIDENT <resident heap pages>" and ends when stdin closes.
//
// The game data (nx2saria.game_data) is not there at first, as before the game makes it. A line "LOAD" on stdin puts
// a game-data manager in the heap where version 1.1.1 has one (**[main+DBG_GAME_DATA_SLOT]), with a few values of each
// kind the API answers, and prints "LOADED"; "EMPTY" before it puts the manager there with its sections still empty,
// as the game has it before it loads a save (EMPTIED). Then each line spoils one thing and prints its answer word:
// "POINT" turns the String64 and String64Array text pointers to the host's own memory (POINTED), "MANY" gives the
// String64Array value 65 texts (MANIED), "DUPHASH" gives Int's second pair the first one's hash (DUPHASHED), "DUP" gives
// it back its hash and the first one's index (DUPPED), "UNSORT" swaps two placed objects' keys (UNSORTED), "ZERO"
// clears the Enum entry's vtable, as an entry the game is still making has it (ZEROED), and "BREAK" gives the Bool
// kind's middle entry another vtable, as a section read at the wrong stride would have (BROKEN).
#include "tracker_api.c"

#include <stdarg.h>
#include <stdio.h>

// ---- the guest memory
#define MAIN_SIZE 0x4ea0000ull
#define MAIN_COMMIT 0x4000000ull            // main's first 64 MiB: the MOD0 header is in them
#define MOD0_AT 0x3c4705cull                // main+0x3c4705c in 1.1.1
#define HEAP_BASE 0x1030000000ull
#define HEAP_SIZE 0x1000000ull               // 16 MiB
#define HOLE_AT 0x400000ull                  // [4 MiB, 5 MiB): reserved, never committed
#define HOLE_SIZE 0x100000ull
#define DARK_AT 0x800000ull                  // one committed page nobody may read
#define HOST_AT 0x2000000000ull             // the host's own memory, outside every guest region
#define TEST_PORT_DEFAULT 46545

static const uint8_t mod0[16] = {'M', 'O', 'D', '0', 0x54, 0x67, 0x03, 0x01, 0x24, 0xfb, 0x0f, 0x01, 0x6c, 0x34, 0x25, 0x01};

// ---- what tracker_api.c reaches outside itself
const int rt_game_version = 111;
const char rt_port_version[] = "0.14.0+test";
static RtGuestLayout layout = {.heap = {HEAP_BASE, HEAP_SIZE}};

void rt_log(const char *fmt, ...) {   // one whole line at a time, as the runtime's log writes it
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    fwrite(line, 1, (size_t)n, stdout);   // stdio locks the stream for the call
    fflush(stdout);
}

const char *cfg_env(const char *name) { return getenv(name); }
int cfg_bool(const char *name) {
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0");
}
int cfg_int(const char *name) {
    const char *v = getenv(name);
    return v ? atoi(v) : TEST_PORT_DEFAULT;
}
int SDL_SetClipboardText(const char *t) { (void)t; return 0; }
char *SDL_GetClipboardText(void) { return NULL; }
void SDL_free(void *p) { (void)p; }

uint64_t frame_step_presents(void) { return 1234; }
// The save folder as a Windows profile under an accented user name has it: backslashes, a quote, UTF-8.
const char *fs_save_dir(void) { return "C:\\Users\\Jo\xc3\xa3o \"x\"\\profile\\save\\0123"; }
const RtGuestLayout *mem_layout(void) { return &layout; }
const char *mods_main_build_id(void) { return "e0f3ccfaf50065ae000000000000000000000000000000000000000000000000"; }
void mem_kernel_access(uint64_t a, uint64_t n) { (void)a, (void)n; }

static int in(uint64_t a, uint64_t n, uint64_t base, uint64_t size) { return a >= base && n <= size && a - base <= size - n; }
// As memory.c answers: below 2^39 every address that is not free is readable, the host's taken memory included.
int mem_readable(uint64_t a, uint64_t n) { return n && a < RT_ADDRESS_LIMIT && n <= RT_ADDRESS_LIMIT - a; }

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {   // main read-only, the heap read-write, the rest taken by the host
    memset(out, 0, sizeof *out);
    if (in(a, 1, MOD_MAIN_BASE, MAIN_SIZE)) *out = (RtMemoryInfo){.base = MOD_MAIN_BASE, .size = MAIN_SIZE, .state = 4, .perm = 1};
    else if (in(a, 1, HEAP_BASE, HEAP_SIZE)) *out = (RtMemoryInfo){.base = HEAP_BASE, .size = HEAP_SIZE, .state = 5, .perm = 3};
    else if (a < HEAP_BASE) *out = (RtMemoryInfo){.base = 0, .size = HEAP_BASE, .state = 3, .perm = 5};
    else if (a < MOD_MAIN_BASE) *out = (RtMemoryInfo){.base = HEAP_BASE + HEAP_SIZE, .size = MOD_MAIN_BASE - HEAP_BASE - HEAP_SIZE, .state = 3, .perm = 5};
    else *out = (RtMemoryInfo){.base = MOD_MAIN_BASE + MAIN_SIZE, .size = RT_ADDRESS_LIMIT - MOD_MAIN_BASE - MAIN_SIZE, .state = 3, .perm = 5};
    return 0;
}

int mem_guest_ranges(const char **name, RtRange *r, int max) {
    if (max < 2) return 0;
    name[0] = "heap", r[0] = (RtRange){HEAP_BASE, HEAP_SIZE};
    name[1] = "modules", r[1] = (RtRange){MOD_MAIN_BASE, MAIN_SIZE};
    return 2;
}

int dbg_area(char out[64]) {
    snprintf(out, 64, "HyruleField_Adult");
    return 1;
}
int dbg_player_pos(float xyz[3]) {
    xyz[0] = 12.5f, xyz[1] = -3.25f, xyz[2] = 100.0f;
    return 1;
}
int dbg_player_rot(float m[9]) {   // a quarter turn about the vertical
    static const float r[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};
    memcpy(m, r, sizeof r);
    return 1;
}

static void put(uint64_t a, const void *p, size_t n) { memcpy((void *)(uintptr_t)a, p, n); }
static void put32(uint64_t a, uint32_t v) { put(a, &v, 4); }
static void put64(uint64_t a, uint64_t v) { put(a, &v, 8); }

// ---- the stand-in game data: 1.1.1's manager, each kind's section at its number times 0x60
#define GD_AT (HEAP_BASE + 0xc00000ull)     // the slot's target, then the manager, then the kinds' entries and tables
#define GD_MANAGER (GD_AT + 0x1000)
// The kinds' vtables: Bool's and Int's are 1.1.1's (tracker_api.c gd_kinds checks them), the others any in main
#define GD_VT(kind) (MOD_MAIN_BASE + ((kind) == 0 ? 0x4c20658ull : (kind) == 2 ? 0x4c20718ull : 0x4c58270ull + (kind) * 8ull))

// A kind's section with n entries of `stride` bytes and a {hash, index} table of 2n pairs, each placed as the game
// places it: from hash % size on, at the first empty pair. Each entry starts with the kind's vtable. Returns the
// entries' address.
static uint64_t gd_section(int kind, uint64_t at, uint32_t stride, const uint32_t *hash, uint32_t n) {
    uint64_t sec = GD_MANAGER + (uint64_t)kind * 0x60, entries = at, table = at + (uint64_t)n * stride;
    put32(sec + 0x10, n), put64(sec + 0x18, entries), put64(sec + 0x20, table), put32(sec + 0x2c, 2 * n);
    for (uint32_t i = 0; i < n; i++) {
        put64(entries + (uint64_t)i * stride, GD_VT(kind));
        uint32_t slot = hash[i] % (2 * n);
        while (*(uint32_t *)(uintptr_t)(table + slot * 8ull)) slot = (slot + 1) % (2 * n);
        put32(table + slot * 8ull, hash[i]), put32(table + slot * 8ull + 4, i);
    }
    return entries;
}

static void gd_load(void) {
    static const uint32_t bools[] = {0x22222222, 0x00000005, 0x11111111}, ints[] = {0x04735978, 0xfbe01da1},
                          enums[] = {0x33333333}, texts[] = {0x1d6189da}, lists[] = {0xc458f1eb},
                          longs[] = {0xd457ee3a};
    uint64_t e = gd_section(0, GD_AT + 0x10000, 0x18, bools, 3);
    put(e + 0x18 + 0x16, "\x03", 1), put(e + 0x30 + 0x16, "\x01", 1);   // entry 0 stays 0; entry 1's other bits are not the value
    e = gd_section(2, GD_AT + 0x11000, 0x28, ints, 2);
    put32(e + 0x1c, 57), put32(e + 0x28 + 0x1c, 0xffffffffu);
    e = gd_section(6, GD_AT + 0x12000, 0x38, enums, 1);
    put32(e + 0x1c, 0x4c0a63f4);
    e = gd_section(16, GD_AT + 0x13000, 0x108, texts, 1);
    put64(e + 0x68, e + 0x74), put(e + 0x74, "HyruleField_Adult", 18);
    e = gd_section(17, GD_AT + 0x14000, 0x90, lists, 1);
    put32(e + 0x80, 2), put64(e + 0x88, GD_AT + 0x15000);
    put(GD_AT + 0x15000 + 0x0c, "HyruleField", 12), put(GD_AT + 0x15050 + 0x0c, "LakeHylia", 10);
    e = gd_section(22, GD_AT + 0x16000, 0x30, longs, 1);
    put64(e + 0x20, 0x100000002ull);
    static const uint64_t keys[] = {0x10, 0x3f93661d8e17c1d0ull, 0x8000000000000001ull, 0xfedcba9876543210ull};
    static const uint32_t classes[] = {0x18, 0x58, 0x118, 0x18};
    static const uint8_t set[] = {1, 0, 3, 0};   // the third's other bits are not the value
    for (int i = 0; i < 4; i++) {
        put64(GD_AT + 0x17000 + i * 16ull, keys[i]), put32(GD_AT + 0x17000 + i * 16ull + 8, classes[i]);
        put(GD_AT + 0x17000 + i * 16ull + 12, "\xaa\xaa\xaa\xaa", 4);   // the save index and other bytes, left out
        put(GD_AT + 0x17000 + i * 16ull + 12, &set[i], 1);
    }
    put32(GD_MANAGER + 0xc58, 4), put32(GD_MANAGER + 0xc5c, 0xdddd);   // a u32 count, other bytes after it
    put64(GD_MANAGER + 0xc60, GD_AT + 0x17000);
    put64(GD_AT, GD_MANAGER);
    put64(MOD_MAIN_BASE + DBG_GAME_DATA_SLOT, GD_AT);   // last: the game data is there from now on
}

// The heap's pages that hold memory now, by the platform's own answer.
static uint64_t resident_heap_pages(void) {
    uint64_t pages = 0;
    uint8_t vec[256];
    for (uint64_t a = HEAP_BASE; a < HEAP_BASE + HEAP_SIZE; a += 0x100000)
        if (plat_mem_resident(a, 0x100000, vec))
            for (int i = 0; i < 256; i++) pages += vec[i];
    return pages;
}

int main(void) {
    if (!plat_mem_reserve(MOD_MAIN_BASE, MAIN_SIZE, 0) || !plat_mem_commit(MOD_MAIN_BASE, MAIN_COMMIT, NULL) ||
        !plat_mem_reserve(HEAP_BASE, HEAP_SIZE, 0) || !plat_mem_commit(HEAP_BASE, HOLE_AT, NULL) ||
        !plat_mem_commit(HEAP_BASE + HOLE_AT + HOLE_SIZE, HEAP_SIZE - HOLE_AT - HOLE_SIZE, NULL) ||
        !plat_mem_protect(HEAP_BASE + DARK_AT, 0x1000, PLAT_PROT_NONE) || !plat_mem_reserve(HOST_AT, 0x10000, 0) ||
        !plat_mem_commit(HOST_AT, 0x10000, NULL) ||
        !plat_mem_commit(MOD_MAIN_BASE + (DBG_GAME_DATA_SLOT & ~0xfffull), 0x1000, NULL)) {
        printf("FAIL: cannot set up the guest memory at the game's addresses\n");
        return 1;
    }
    put(MOD_MAIN_BASE, "\x01\0\0\0", 4);                         // main+0: 1, main+4: the MOD0 offset
    uint32_t off = (uint32_t)MOD0_AT;
    put(MOD_MAIN_BASE + 4, &off, 4);
    put(MOD_MAIN_BASE + MOD0_AT - 8, "Owl.nss", 8);
    put(MOD_MAIN_BASE + MOD0_AT, mod0, sizeof mod0);
    for (uint64_t i = 0; i < 0x1000; i++) ((uint8_t *)(uintptr_t)(MOD_MAIN_BASE + 0x100000))[i] = (uint8_t)i;
    put(HEAP_BASE + 0x123450, "NEEDLE-ONE", 10);                 // a pattern inside one piece of the heap
    put(HEAP_BASE + PIECE - 5, "NEEDLE-TWO", 10);                // across the end of a search's first copy
    put(HEAP_BASE + DARK_AT - 5, "NEEDLE-TWO", 5);               // runs into the dark page: never a hit
    put(HEAP_BASE + 0xa00000, "NEEDLE-TWO", 10);
    put(HOST_AT + 0x100, "SECRET-KEY", 10);
    memset((void *)(uintptr_t)GD_AT, 0, 0x18000);   // resident now, so LOAD changes no count of resident pages
    tracker_api_init();
    printf("READY %d %llu\n", port, (unsigned long long)resident_heap_pages());
    fflush(stdout);
    char buf[64];
    while (fgets(buf, sizeof buf, stdin)) {   // the client test closes stdin when it is done
        if (!strcmp(buf, "EMPTY\n"))
            put64(GD_AT, GD_MANAGER), put64(MOD_MAIN_BASE + DBG_GAME_DATA_SLOT, GD_AT), printf("EMPTIED\n");
        if (!strcmp(buf, "LOAD\n")) gd_load(), printf("LOADED\n");
        if (!strcmp(buf, "POINT\n"))
            put64(GD_AT + 0x13000 + 0x68, HOST_AT + 0x100), put64(GD_AT + 0x14000 + 0x88, HOST_AT), printf("POINTED\n");
        // Int's table, after its 2 entries: 4 pairs at hash % 4, 0x04735978's first (index 0), 0xfbe01da1's second
        if (!strcmp(buf, "MANY\n")) put32(GD_AT + 0x14000 + 0x80, 65), printf("MANIED\n");
        if (!strcmp(buf, "DUPHASH\n")) put32(GD_AT + 0x11050 + 8, 0x04735978), printf("DUPHASHED\n");
        if (!strcmp(buf, "DUP\n"))
            put32(GD_AT + 0x11050 + 8, 0xfbe01da1), put32(GD_AT + 0x11050 + 8 + 4, 0), printf("DUPPED\n");
        if (!strcmp(buf, "UNSORT\n"))
            put64(GD_AT + 0x17000, 0x3f93661d8e17c1d0ull), put64(GD_AT + 0x17010, 0x10), printf("UNSORTED\n");
        if (!strcmp(buf, "ZERO\n")) put64(GD_AT + 0x12000, 0), printf("ZEROED\n");
        if (!strcmp(buf, "BREAK\n")) put64(GD_AT + 0x10000 + 1 * 0x18, GD_VT(1)), printf("BROKEN\n");
        fflush(stdout);
    }
    printf("RESIDENT %llu\n", (unsigned long long)resident_heap_pages());
    return 0;
}
