// runtime/vk_cache_stamp.h: the translator stamp of the pipeline cache files (runtime/vk_pipeline.c
// cache_stamp_check). Built and run by tests/cache_stamp_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "vk_cache_stamp.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    uint8_t cur[32], old[32], got[32];
    for (int i = 0; i < 32; i++) cur[i] = (uint8_t)(i * 7 + 1), old[i] = (uint8_t)(255 - i);

    // What the runtime writes is what it reads back, and it is the identity's 64 lowercase hex digits and a line end,
    // as tools/build_xlate.py writes <translator>.id.
    char text[66];
    cache_stamp_format(cur, text);
    CHECK(strlen(text) == 65 && text[64] == '\n', "the stamp text is %zu bytes", strlen(text));
    CHECK(!memcmp(text, "01080f161d242b32", 16), "the stamp text starts %.16s", text);
    CHECK(cache_stamp_parse(text, 65, got) && !memcmp(got, cur, 32), "the stamp written is not read back");
    CHECK(cache_stamp_parse(text, 64, got), "a stamp without a line end refused");
    char crlf[67];   // the same digits in upper case, with a CRLF line end (a file edited on Windows)
    for (int i = 0; i < 64; i++) crlf[i] = text[i] >= 'a' ? (char)(text[i] - 'a' + 'A') : text[i];
    crlf[64] = '\r', crlf[65] = '\n', crlf[66] = 0;
    CHECK(cache_stamp_parse(crlf, 66, got) && !memcmp(got, cur, 32), "upper case with CRLF refused");

    // Text that names no identity: no stamp, and the start reads the files and stamps them.
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

    // States.
    uint8_t st[32];
    CHECK(cache_stamp_state(NULL, 0, cur, st) == CACHE_STAMP_NONE, "no file is not 'none'");
    CHECK(cache_stamp_state(text, 65, cur, st) == CACHE_STAMP_MATCHES, "the current stamp is not 'matches'");
    char otext[66];
    cache_stamp_format(old, otext);
    CHECK(cache_stamp_state(otext, 65, cur, st) == CACHE_STAMP_DIFFERS && !memcmp(st, old, 32),
          "another translator's stamp is not 'differs' with its identity");

    // Actions: the same translator reads; another one carried reads and keeps the stamp; another one not carried
    // drops; no stamp reads and stamps. Carry never changes what a matching or missing stamp does.
    CHECK(cache_stamp_action(CACHE_STAMP_MATCHES, 0, 0) == CACHE_STAMP_READ, "matches: not read");
    CHECK(cache_stamp_action(CACHE_STAMP_MATCHES, 1, 0) == CACHE_STAMP_READ, "matches, carried: not read");
    CHECK(cache_stamp_action(CACHE_STAMP_DIFFERS, 1, 0) == CACHE_STAMP_CARRY, "differs, carried: not kept for carry");
    CHECK(cache_stamp_action(CACHE_STAMP_DIFFERS, 0, 0) == CACHE_STAMP_DROP, "differs, not carried: not dropped");
    CHECK(cache_stamp_action(CACHE_STAMP_NONE, 0, 0) == CACHE_STAMP_READ_STAMP, "none: not read and stamped");
    CHECK(cache_stamp_action(CACHE_STAMP_NONE, 1, 0) == CACHE_STAMP_READ_STAMP, "none, carried: not read and stamped");
    // A preparation translates everything under the current translator and carries nothing: another translator's
    // files go even while the carry lists it, or the stamp would keep naming it and a later start would delete what
    // the preparation built. A matching stamp or none is read in a preparation as anywhere.
    CHECK(cache_stamp_action(CACHE_STAMP_DIFFERS, 1, 1) == CACHE_STAMP_DROP, "differs, carried, preparation: kept");
    CHECK(cache_stamp_action(CACHE_STAMP_DIFFERS, 0, 1) == CACHE_STAMP_DROP, "differs, preparation: not dropped");
    CHECK(cache_stamp_action(CACHE_STAMP_MATCHES, 1, 1) == CACHE_STAMP_READ, "matches, preparation: not read");
    CHECK(cache_stamp_action(CACHE_STAMP_NONE, 0, 1) == CACHE_STAMP_READ_STAMP, "none, preparation: not read and stamped");

    // The sequence of an update: translator old wrote the files; the first start of the new one carries old's
    // shaders (read, stamp kept), and so does each start until the carry pass removed old's translations; the start
    // after drops the files and stamps the new translator; the start after that matches.
    int stamp_is_old = 1, dropped = 0, reads = 0;
    for (int start = 0; start < 4; start++) {
        int carried = start < 2;   // the pass finished during the second start
        int s = cache_stamp_state(stamp_is_old ? otext : text, 65, cur, st);
        int a = cache_stamp_action(s, carried, 0);
        if (a == CACHE_STAMP_DROP) dropped++;
        if (a != CACHE_STAMP_DROP) reads++;
        if (a == CACHE_STAMP_DROP || a == CACHE_STAMP_READ_STAMP) stamp_is_old = 0;
    }
    CHECK(dropped == 1 && reads == 3 && !stamp_is_old, "an update dropped %d times and read %d times", dropped, reads);

    // The stamp's and the play file's paths beside the cache file, and a path without ".bin"; cut to the room given.
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

    // pipeline-cache.bin at the exit of a run that started without it, in play alone: never with the file there, never
    // in a preparation (which writes it at every save). An empty file is none.
    CHECK(cache_base_at_exit(0, -1), "no file in play: not written at exit");
    CHECK(cache_base_at_exit(0, 0), "an empty file in play: not written at exit");
    CHECK(!cache_base_at_exit(0, (int64_t)650 << 20), "a file there: written at exit too");
    CHECK(!cache_base_at_exit(1, -1), "a preparation without the file: due at exit too");

    // Which saves write it: every save without a play cache; with one, a fold's, and the exit's of a run that started
    // without the file, not that run's autosaves, nor any save of a run that read the file.
    CHECK(cache_save_writes_base(0, 0, 0, 0), "no play cache: an autosave does not write it");
    CHECK(!cache_save_writes_base(1, 0, 0, 0) && !cache_save_writes_base(1, 0, 0, 1), "read at the start: written in play");
    CHECK(cache_save_writes_base(1, 1, 0, 0), "a fold: its autosave does not write it");
    CHECK(!cache_save_writes_base(1, 0, 1, 0), "started without it: an autosave writes it");
    CHECK(cache_save_writes_base(1, 0, 1, 1), "started without it: the save at exit does not write it");

    // Through the end of an update in play: the start that deletes the files is due to write pipeline-cache.bin and
    // ends before its exit (a crash); the next start finds none and is due again, and its exit writes it; the start
    // after reads it and writes the play file alone.
    int64_t base = -1;   // deleted by the stamp
    int due = cache_base_at_exit(0, base), wrote = 0;
    CHECK(due, "the deleting start: not due");
    due = cache_base_at_exit(0, base);   // that run crashed: the file is still missing
    if (cache_save_writes_base(1, 0, due, 0)) wrote++;   // an autosave
    if (cache_save_writes_base(1, 0, due, 1)) wrote++, base = (int64_t)30 << 20;   // the exit
    CHECK(due && wrote == 1 && base > 0, "the start after a crash: due %d, written %d times", due, wrote);
    due = cache_base_at_exit(0, base);
    CHECK(!due && !cache_save_writes_base(1, 0, due, 1), "the start after the file was written: due again");

    // The words of the log line.
    CHECK(!strcmp(cache_stamp_state_name(CACHE_STAMP_MATCHES), "matches") &&
          !strcmp(cache_stamp_state_name(CACHE_STAMP_DIFFERS), "differs") &&
          !strcmp(cache_stamp_state_name(CACHE_STAMP_NONE), "none"), "state names");
    CHECK(strstr(cache_stamp_action_name(CACHE_STAMP_DROP), "deleted") != NULL, "the drop's words");

    printf("cache_stamp_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
