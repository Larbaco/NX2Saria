#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    CACHE_FILE_OK,
    CACHE_FILE_NONE,
    CACHE_FILE_UNSIZED,
    CACHE_FILE_SHORT,
    CACHE_FILE_OVER_CAP,
    CACHE_FILE_NO_MEMORY,
    CACHE_FILE_READ_FAIL,
};

#define CACHE_FILE_PIECE ((size_t)64 << 20)

static inline const char *cache_file_why(int why) {
    switch (why) {
    case CACHE_FILE_UNSIZED: return "its size cannot be measured";
    case CACHE_FILE_SHORT: return "it is shorter than a pipeline cache header";
    case CACHE_FILE_OVER_CAP: return "it is over the size limit";
    case CACHE_FILE_NO_MEMORY: return "there is no memory for a block of its size";
    case CACHE_FILE_READ_FAIL: return "reading it ended early";
    default: return "";
    }
}

static inline void *cache_file_read(const char *path, uint64_t min, uint64_t cap, uint64_t *len, int *why) {
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return *why = CACHE_FILE_NONE, NULL;
#ifdef _WIN32
    int64_t n = _fseeki64(f, 0, SEEK_END) ? -1 : _ftelli64(f);
    if (n >= 0 && _fseeki64(f, 0, SEEK_SET)) n = -1;
#else
    int64_t n = fseeko(f, 0, SEEK_END) ? -1 : (int64_t)ftello(f);
    if (n >= 0 && fseeko(f, 0, SEEK_SET)) n = -1;
#endif
    void *data = NULL;
    if (n < 0) *why = CACHE_FILE_UNSIZED;
    else if (*len = (uint64_t)n, (uint64_t)n <= min) *why = CACHE_FILE_SHORT;
    else if (cap && (uint64_t)n > cap) *why = CACHE_FILE_OVER_CAP;
    else if ((uint64_t)n > SIZE_MAX || !(data = malloc((size_t)n))) *why = CACHE_FILE_NO_MEMORY;
    else {
        size_t got = 0, want = (size_t)n;
        while (got < want) {
            size_t piece = want - got < CACHE_FILE_PIECE ? want - got : CACHE_FILE_PIECE;
            size_t r = fread((char *)data + got, 1, piece, f);
            got += r;
            if (r != piece) break;
        }
        if (got == want) *why = CACHE_FILE_OK;
        else free(data), data = NULL, *why = CACHE_FILE_READ_FAIL;
    }
    fclose(f);
    return data;
}
