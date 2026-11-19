// The game folder's record of where its button prompts came from (runtime/input_map.c input_prompts_record, which
// runtime/romfs.c overlay_init logs as "button prompts: ..."; tools/make_game.py record_prompts writes the file): its
// first line without the line end, cut short to the buffer, and 0 for a folder without the record or with an empty
// one. No game files. tests/prompts_record_check.py builds this with runtime/input_map.c and runs it on a temporary
// folder.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "input_map.h"

// what input_map.c needs from the rest of the runtime; none of it is reached from input_prompts_record
const char *cfg_get(const char *key) { (void)key; return NULL; }
double cfg_double(const char *key) { (void)key; return 0; }
void rt_log(const char *fmt, ...) { (void)fmt; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

static const char *dir;

static void put(const char *text) {
    char path[2100];
    snprintf(path, sizeof path, "%s/button-prompts.txt", dir);
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    dir = argv[1];
    char out[64];
    memset(out, 'x', sizeof out);
    CHECK(!input_prompts_record(dir, out, sizeof out) && !out[0], "no record: 0 and an empty line");
    CHECK(!input_prompts_record(NULL, out, sizeof out) && !out[0], "no folder: 0 and an empty line");

    put("drawn from JulioCacko, FREE Input Prompts Pack 1.4 (CC0 1.0)\n");
    CHECK(input_prompts_record(dir, out, sizeof out) && !strcmp(out, "drawn from JulioCacko, FREE Input Prompts Pack 1.4 (CC0 1.0)"),
          "a record written on Linux: '%s'", out);
    put("not drawn: this version carries no button art\r\nsecond line\r\n");
    CHECK(input_prompts_record(dir, out, sizeof out) && !strcmp(out, "not drawn: this version carries no button art"),
          "a record with Windows line ends, first line only: '%s'", out);
    put("no line end");
    CHECK(input_prompts_record(dir, out, sizeof out) && !strcmp(out, "no line end"), "no line end: '%s'", out);

    // a line longer than the buffer is cut short and ends inside it
    char longer[200];
    memset(longer, 'a', sizeof longer - 1);
    longer[sizeof longer - 1] = 0;
    put(longer);
    char small[16];
    memset(small, 'x', sizeof small);
    CHECK(input_prompts_record(dir, small, sizeof small) && strlen(small) == sizeof small - 1,
          "a long line: %zu characters in a buffer of %zu", strlen(small), sizeof small);
    char one[1] = {'x'};
    CHECK(!input_prompts_record(dir, one, sizeof one) && !one[0], "a buffer of one byte holds the empty line");
    CHECK(!input_prompts_record(dir, one, 0) && one[0] == 0, "a buffer of no bytes is left alone");

    put("\nafter an empty first line\n");
    CHECK(!input_prompts_record(dir, out, sizeof out) && !out[0], "an empty first line: 0");
    put("");
    CHECK(!input_prompts_record(dir, out, sizeof out) && !out[0], "an empty record: 0");

    if (!fails) puts("prompts record: ok");
    return fails != 0;
}
