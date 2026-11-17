// runtime/tracker_api.c served over the real platform layer, with a guest memory of the test's own at the game's own
// addresses: main's window at 0x7000000000 with a MOD0 header where 1.1.1's main has it, and a heap at 0x1030000000
// (the stack and heap block of a run with the tracker API on) with a hole the lazy heap never committed, a page nobody
// may read, and pages never written. Beside them, at 0x2000000000, memory of the host's own holding a secret: the
// game's memory map answers for it as taken memory the game can read, as memory.c query_locked does for every host
// allocation below 2^39. Built and run by tests/tracker_api_check.py, which is the client: the program prints
// "READY <port> <resident heap pages>" once it listens, logs every rt_log line to stdout, and prints
// "RESIDENT <resident heap pages>" and ends when stdin closes.
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

static void put(uint64_t a, const void *p, size_t n) { memcpy((void *)(uintptr_t)a, p, n); }

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
    if (!plat_mem_reserve(MOD_MAIN_BASE, MAIN_SIZE, 0) || !plat_mem_commit(MOD_MAIN_BASE, MAIN_COMMIT) ||
        !plat_mem_reserve(HEAP_BASE, HEAP_SIZE, 0) || !plat_mem_commit(HEAP_BASE, HOLE_AT) ||
        !plat_mem_commit(HEAP_BASE + HOLE_AT + HOLE_SIZE, HEAP_SIZE - HOLE_AT - HOLE_SIZE) ||
        !plat_mem_protect(HEAP_BASE + DARK_AT, 0x1000, PLAT_PROT_NONE) || !plat_mem_reserve(HOST_AT, 0x10000, 0) ||
        !plat_mem_commit(HOST_AT, 0x10000)) {
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
    tracker_api_init();
    printf("READY %d %llu\n", port, (unsigned long long)resident_heap_pages());
    fflush(stdout);
    char buf[64];
    while (fgets(buf, sizeof buf, stdin)) {}   // the client test closes stdin when it is done
    printf("RESIDENT %llu\n", (unsigned long long)resident_heap_pages());
    return 0;
}
