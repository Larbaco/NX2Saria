// runtime/lib_origin.c: the game log's "libs:" line for an SDL beside the game, one elsewhere, one not found among the
// loaded modules and an unknown executable path; folders that share a prefix are not the game's; a path without a
// folder; the root folder; versions apart; the SHA-256 form; a buffer cut short. Built with -D_WIN32 too
// (tests/lib_origin_check.py), where the paths take either separator and compare without case.
#include "lib_origin.c"

#include <stdio.h>

static int failures;

static void expect(int loaded, int built, const char *sdl, const char *exe, int ni, const char *want) {
    char out[512];
    int n = lib_origin_line(out, sizeof out, loaded, built, sdl, exe, ni);
    if (strcmp(out, want) || n != (int)strlen(want)) {
        printf("FAIL sdl=%s exe=%s ni=%d:\n  got  %s (%d)\n  want %s\n", sdl, exe, ni, out, n, want);
        failures++;
    }
}

int main(void) {
    const int v3418 = 3 * 1000000 + 4 * 1000 + 18;
    const char *ni = "SHA-256 on the SHA extensions (SHA-NI); ", *c = "SHA-256 in portable C (no SHA extensions on this CPU); ";
    char want[512];
#define WANT(sha, rest) (snprintf(want, sizeof want, "%s%s", sha, rest), want)
#ifdef _WIN32
    expect(v3418, v3418, "C:\\Games\\NX2Saria\\SDL3.dll", "c:\\games\\nx2saria\\ocarina.exe", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), beside the game: C:\\Games\\NX2Saria\\SDL3.dll"));
    expect(v3418, v3418, "C:/Games/NX2Saria/SDL3.dll", "C:\\Games\\NX2Saria\\ocarina.exe", 0,
           WANT(c, "SDL 3.4.18 (built against 3.4.18), beside the game: C:/Games/NX2Saria/SDL3.dll"));
    expect(v3418, v3418, "C:\\Games\\NX2Saria2\\SDL3.dll", "C:\\Games\\NX2Saria\\ocarina.exe", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), not beside the game: C:\\Games\\NX2Saria2\\SDL3.dll"));
    expect(v3418, v3418, "C:\\Games\\NX2Saria\\SDL3.dll", "", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), the game's folder unknown: C:\\Games\\NX2Saria\\SDL3.dll"));
#else
    const int v3420 = 3004020, v3210 = 3002010;
    expect(v3418, v3418, "/home/p/Games/Ocarina/libSDL3.so.0", "/home/p/Games/Ocarina/ocarina", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), beside the game: /home/p/Games/Ocarina/libSDL3.so.0"));
    expect(v3420, v3418, "/usr/lib/x86_64-linux-gnu/libSDL3.so.0", "/home/p/Games/Ocarina/ocarina", 0,
           WANT(c, "SDL 3.4.20 (built against 3.4.18), not beside the game: /usr/lib/x86_64-linux-gnu/libSDL3.so.0"));
    // one folder's name the start of the other's
    expect(v3418, v3418, "/g/Ocarina2/libSDL3.so.0", "/g/Ocarina/ocarina", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), not beside the game: /g/Ocarina2/libSDL3.so.0"));
    expect(v3418, v3418, "/g/Oca/libSDL3.so.0", "/g/Ocarina/ocarina", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), not beside the game: /g/Oca/libSDL3.so.0"));
    // case matters on Linux
    expect(v3418, v3418, "/G/Ocarina/libSDL3.so.0", "/g/Ocarina/ocarina", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), not beside the game: /G/Ocarina/libSDL3.so.0"));
    // a name without a folder is never taken for the game's folder; the root folder is one like any other
    expect(v3418, v3418, "libSDL3.so.0", "/g/Ocarina/ocarina", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), not beside the game: libSDL3.so.0"));
    expect(v3418, v3418, "/libSDL3.so.0", "/ocarina", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), beside the game: /libSDL3.so.0"));
    expect(v3210, v3418, "/usr/lib/libSDL3.so.0", "", 0,
           WANT(c, "SDL 3.2.10 (built against 3.4.18), the game's folder unknown: /usr/lib/libSDL3.so.0"));
#endif
    expect(v3418, v3418, "", "/home/p/Games/Ocarina/ocarina", 1,
           WANT(ni, "SDL 3.4.18 (built against 3.4.18), its file not found among the loaded modules"));
    // a buffer too small: cut, terminated, and the whole length returned
    char small[16];
    memset(small, 'x', sizeof small);
    int n = lib_origin_line(small, sizeof small, v3418, v3418, "/a/libSDL3.so.0", "/a/ocarina", 1);
    char whole[512];
    int full = lib_origin_line(whole, sizeof whole, v3418, v3418, "/a/libSDL3.so.0", "/a/ocarina", 1);
    if (small[15] != 0 || strncmp(small, "SHA-256 on the ", 15) || n != full || full != (int)strlen(whole)) {
        printf("FAIL a 16-byte buffer: '%.16s', length %d of %d\n", small, n, full);
        failures++;
    }
    printf("%s: %d failures\n",
#ifdef _WIN32
           "Windows paths",
#else
           "Linux paths",
#endif
           failures);
    return failures ? 1 : 0;
}
