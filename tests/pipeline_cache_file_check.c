#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk_cache_file.h"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define HEADER 32

static int make_sparse(const char *path, uint64_t len, const char *head, const char *tail) {
    size_t hn = strlen(head), tn = strlen(tail);
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    LARGE_INTEGER at = {.QuadPart = (LONGLONG)(len - tn)};
    int ok = DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL) &&
             WriteFile(h, head, (DWORD)hn, &got, NULL) && got == hn && SetFilePointerEx(h, at, NULL, FILE_BEGIN) &&
             WriteFile(h, tail, (DWORD)tn, &got, NULL) && got == tn;
    CloseHandle(h);
    return ok;
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    int ok = write(fd, head, hn) == (ssize_t)hn && pwrite(fd, tail, tn, (off_t)(len - tn)) == (ssize_t)tn;
    close(fd);
    return ok;
#endif
}

int main(int argc, char **argv) {
    if (argc < 2) return printf("usage: pipeline_cache_file_check <folder>\n"), 2;
    char path[1024];
    uint64_t len = 0;
    int why = -1;

    snprintf(path, sizeof path, "%s/none.bin", argv[1]);
    remove(path);
    CHECK(!cache_file_read(path, HEADER, 0, &len, &why) && why == CACHE_FILE_NONE && len == 0, "no file: why %d", why);

    snprintf(path, sizeof path, "%s/short.bin", argv[1]);
    FILE *f = fopen(path, "wb");
    for (int i = 0; f && i < HEADER; i++) fputc(i, f);
    if (f) fclose(f);
    CHECK(!cache_file_read(path, HEADER, 0, &len, &why) && why == CACHE_FILE_SHORT && len == HEADER,
          "header only: why %d, len %llu", why, (unsigned long long)len);
    remove(path);

    f = fopen(path, "wb");
    for (int i = 0; f && i < 4096; i++) fputc(i & 0xff, f);
    if (f) fclose(f);
    uint8_t *d = cache_file_read(path, HEADER, 1 << 20, &len, &why);
    CHECK(d && why == CACHE_FILE_OK && len == 4096 && d[4095] == 0xff && d[257] == 1, "small file: why %d, len %llu", why,
          (unsigned long long)len);
    free(d);
    remove(path);

    const uint64_t big = (2ull << 30) + 4096;
    snprintf(path, sizeof path, "%s/big.bin", argv[1]);
    if (!make_sparse(path, big, "HEAD-OF-THE-CACHE", "TAIL-OF-THE-CACHE")) {
        printf("FAIL: cannot make the sparse file %s\n", path);
        remove(path);
        return 1;
    }
    f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long l32 = ftell(f);
    fclose(f);
    printf("pipeline cache file: %llu bytes; ftell into a long (%d bits here) gives %ld\n", (unsigned long long)big,
           (int)sizeof(long) * 8, l32);

    d = cache_file_read(path, HEADER, 512ull << 20, &len, &why);
    CHECK(!d && why == CACHE_FILE_OVER_CAP && len == big, "over the limit: why %d, len %llu", why, (unsigned long long)len);
    CHECK(strlen(cache_file_why(why)) > 0, "a refusal has words for the log");
    free(d);

    for (int pass = 0; pass < 2; pass++) {
        d = cache_file_read(path, HEADER, pass ? 4096ull << 20 : 0, &len, &why);
        CHECK(d && why == CACHE_FILE_OK && len == big, "pass %d: over 2 GB: why %d (%s), len %llu", pass, why,
              cache_file_why(why), (unsigned long long)len);
        if (d) {
            CHECK(!memcmp(d, "HEAD-OF-THE-CACHE", 17), "pass %d: the start of the file is the start of the block", pass);
            CHECK(!memcmp(d + big - 17, "TAIL-OF-THE-CACHE", 17), "pass %d: the last bytes are past 2 GB in the block", pass);
            CHECK(d[1ull << 31] == 0 && d[(1ull << 30) + 5] == 0, "pass %d: the hole reads as zeros", pass);
        }
        free(d);
    }
    remove(path);

    if (!fails) printf("pipeline cache file: all checks passed\n");
    return fails != 0;
}
