#pragma once
#include <stddef.h>
#include <stdint.h>

#define KB_LIMIT 500

typedef struct {
    uint16_t prompt[0x101];
    uint16_t initial[KB_LIMIT + 1];
    int game_initial;
    int max_len, min_len, password, numeric;
    uint32_t disabled;
} SwkbdRequest;

int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n);

size_t swkbd_utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap);
size_t swkbd_utf8_to_utf16(const char *s, uint16_t *out, size_t cap);
int swkbd_allowed(uint16_t c, uint32_t disabled, int digits_only);
