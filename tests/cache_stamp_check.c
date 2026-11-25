#include <stdio.h>
#include <string.h>

#include "vk_cache_stamp.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    uint8_t cur[32], old[32], got[32];
    for (int i = 0; i < 32; i++) cur[i] = (uint8_t)(i * 7 + 1), old[i] = (uint8_t)(255 - i);

    char text[66];
    cache_stamp_format(cur, text);
    CHECK(strlen(text) == 65 && text[64] == '\n', "the stamp text is %zu bytes", strlen(text));
    CHECK(!memcmp(text, "01080f161d242b32", 16), "the stamp text starts %.16s", text);
    CHECK(cache_stamp_parse(text, 65, got) && !memcmp(got, cur, 32), "the stamp written is not read back");
    CHECK(cache_stamp_parse(text, 64, got), "a stamp without a line end refused");
    char crlf[67];
    for (int i = 0; i < 64; i++) crlf[i] = text[i] >= 'a' ? (char)(text[i] - 'a' + 'A') : text[i];
    crlf[64] = '\r', crlf[65] = '\n', crlf[66] = 0;
    CHECK(cache_stamp_parse(crlf, 66, got) && !memcmp(got, cur, 32), "upper case with CRLF refused");

    CHECK(!cache_stamp_parse(NULL, 0, got), "no file parsed as a stamp");
    CHECK(!cache_stamp_parse(text, 63, got), "a cut stamp (63 digits) parsed");
    CHECK(!cache_stamp_parse("", 0, got), "an empty file parsed");
    char bad[66];
    memcpy(bad, text, sizeof bad);
    bad[10] = 'g';
    CHECK(!cache_stamp_parse(bad, 65, got), "a stamp with a non-hex digit parsed");
    char tail[80];
    snprintf(tail, sizeof tail, "%.64s extra\n", text);
    CHECK(!cache_stamp_parse(tail, strlen(tail), got), "a stamp with text after the identity parsed");

    uint8_t st[32];
    CHECK(cache_stamp_state(NULL, 0, cur, st) == CACHE_STAMP_NONE, "no file is not 'none'");
    CHECK(cache_stamp_state(text, 65, cur, st) == CACHE_STAMP_MATCHES, "the current stamp is not 'matches'");
    char otext[66];
    cache_stamp_format(old, otext);
    CHECK(cache_stamp_state(otext, 65, cur, st) == CACHE_STAMP_DIFFERS && !memcmp(st, old, 32),
          "another translator's stamp is not 'differs' with its identity");

    CHECK(cache_stamp_action(CACHE_STAMP_MATCHES, 0, 0) == CACHE_STAMP_READ, "matches: not read");
    CHECK(cache_stamp_action(CACHE_STAMP_MATCHES, 1, 0) == CACHE_STAMP_READ, "matches, carried: not read");
    CHECK(cache_stamp_action(CACHE_STAMP_DIFFERS, 1, 0) == CACHE_STAMP_CARRY, "differs, carried: not kept for carry");
    CHECK(cache_stamp_action(CACHE_STAMP_DIFFERS, 0, 0) == CACHE_STAMP_DROP, "differs, not carried: not dropped");
    CHECK(cache_stamp_action(CACHE_STAMP_NONE, 0, 0) == CACHE_STAMP_READ_STAMP, "none: not read and stamped");
    CHECK(cache_stamp_action(CACHE_STAMP_NONE, 1, 0) == CACHE_STAMP_READ_STAMP, "none, carried: not read and stamped");
    CHECK(cache_stamp_action(CACHE_STAMP_DIFFERS, 1, 1) == CACHE_STAMP_DROP, "differs, carried, preparation: kept");
    CHECK(cache_stamp_action(CACHE_STAMP_DIFFERS, 0, 1) == CACHE_STAMP_DROP, "differs, preparation: not dropped");
    CHECK(cache_stamp_action(CACHE_STAMP_MATCHES, 1, 1) == CACHE_STAMP_READ, "matches, preparation: not read");
    CHECK(cache_stamp_action(CACHE_STAMP_NONE, 0, 1) == CACHE_STAMP_READ_STAMP, "none, preparation: not read and stamped");

    int stamp_is_old = 1, dropped = 0, reads = 0;
    for (int start = 0; start < 4; start++) {
        int carried = start < 2;
        int s = cache_stamp_state(stamp_is_old ? otext : text, 65, cur, st);
        int a = cache_stamp_action(s, carried, 0);
        if (a == CACHE_STAMP_DROP) dropped++;
        if (a != CACHE_STAMP_DROP) reads++;
        if (a == CACHE_STAMP_DROP || a == CACHE_STAMP_READ_STAMP) stamp_is_old = 0;
    }
    CHECK(dropped == 1 && reads == 3 && !stamp_is_old, "an update dropped %d times and read %d times", dropped, reads);

    char out[64];
    cache_stamp_sibling("/g/pipeline-cache.bin", ".id", out, sizeof out);
    CHECK(!strcmp(out, "/g/pipeline-cache.id"), "stamp path %s", out);
    cache_stamp_sibling("/g/pipeline-cache.bin", "-play.bin", out, sizeof out);
    CHECK(!strcmp(out, "/g/pipeline-cache-play.bin"), "play path %s", out);
    cache_stamp_sibling("/g/my-cache", ".id", out, sizeof out);
    CHECK(!strcmp(out, "/g/my-cache.id"), "a path without .bin: %s", out);
    cache_stamp_sibling(".bin", ".id", out, sizeof out);
    CHECK(!strcmp(out, ".bin.id"), "a path that is only .bin: %s", out);
    cache_stamp_sibling("/g/pipeline-cache.bin", "-play.bin", out, 12);
    CHECK(strlen(out) == 11 && !strncmp(out, "/g/pipeline", 11), "cut to 12 bytes: %s", out);

    CHECK(cache_base_at_exit(0, -1), "no file in play: not written at exit");
    CHECK(cache_base_at_exit(0, 0), "an empty file in play: not written at exit");
    CHECK(!cache_base_at_exit(0, (int64_t)650 << 20), "a file there: written at exit too");
    CHECK(!cache_base_at_exit(1, -1), "a preparation without the file: due at exit too");

    CHECK(cache_save_writes_base(0, 0, 0, 0), "no play cache: an autosave does not write it");
    CHECK(!cache_save_writes_base(1, 0, 0, 0) && !cache_save_writes_base(1, 0, 0, 1), "read at the start: written in play");
    CHECK(cache_save_writes_base(1, 1, 0, 0), "a fold: its autosave does not write it");
    CHECK(!cache_save_writes_base(1, 0, 1, 0), "started without it: an autosave writes it");
    CHECK(cache_save_writes_base(1, 0, 1, 1), "started without it: the save at exit does not write it");

    int64_t base = -1;
    int due = cache_base_at_exit(0, base), wrote = 0;
    CHECK(due, "the deleting start: not due");
    due = cache_base_at_exit(0, base);
    if (cache_save_writes_base(1, 0, due, 0)) wrote++;
    if (cache_save_writes_base(1, 0, due, 1)) wrote++, base = (int64_t)30 << 20;
    CHECK(due && wrote == 1 && base > 0, "the start after a crash: due %d, written %d times", due, wrote);
    due = cache_base_at_exit(0, base);
    CHECK(!due && !cache_save_writes_base(1, 0, due, 1), "the start after the file was written: due again");

    CHECK(!strcmp(cache_stamp_state_name(CACHE_STAMP_MATCHES), "matches") &&
          !strcmp(cache_stamp_state_name(CACHE_STAMP_DIFFERS), "differs") &&
          !strcmp(cache_stamp_state_name(CACHE_STAMP_NONE), "none"), "state names");
    CHECK(strstr(cache_stamp_action_name(CACHE_STAMP_DROP), "deleted") != NULL, "the drop's words");

    printf("cache_stamp_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
