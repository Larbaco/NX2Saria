#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__)
#include <immintrin.h>
static long rounds;
__attribute__((target("sha"))) static inline __m128i counted_rnds2(__m128i a, __m128i b, __m128i k) {
    rounds++;
    return _mm_sha256rnds2_epu32(a, b, k);
}
#define _mm_sha256rnds2_epu32 counted_rnds2
#endif

#include "sha256.c"

static int failures;

static void hex(const uint8_t *d, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static void expect(const char *what, const uint8_t got[32], const char *want) {
    char s[65];
    hex(got, 32, s);
    if (strcmp(s, want)) {
        printf("FAIL %s (ni=%d): %s, want %s\n", what, g_ni, s, want);
        failures++;
    }
}

static void vectors(void) {
    uint8_t d[32];
    sha256("abc", 3, d);
    expect("abc", d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("", 0, d);
    expect("empty", d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(m448, strlen(m448), d);
    expect("448 bits", d, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    static uint8_t million[1000000];
    memset(million, 'a', sizeof million);
    sha256(million, sizeof million, d);
    expect("a million a", d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    const char *msg = "what do ya want for nothing?";
    hmac_sha256("Jefe", 4, msg, strlen(msg), d);
    expect("hmac rfc4231 2", d, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

#if defined(__x86_64__)
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint8_t next_byte(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (uint8_t)(rng >> 32);
}

static void hash_pieces(const uint8_t *m, size_t n, size_t piece, uint8_t out[32]) {
    Sha256 s;
    sha256_init(&s);
    if (!piece) piece = n ? n : 1;
    for (size_t at = 0; at < n; at += piece) sha256_update(&s, m + at, n - at < piece ? n - at : piece);
    sha256_final(&s, out);
}

static int compare_blocks(void) {
    static uint8_t m[64 * 300];
    int checked = 0;
    for (size_t i = 0; i < sizeof m; i++) m[i] = next_byte();
    const size_t counts[] = {1, 2, 3, 4, 5, 7, 8, 63, 64, 65, 300};
    for (size_t k = 0; k < sizeof counts / sizeof *counts; k++) {
        uint32_t a[8], b[8];
        for (int i = 0; i < 8; i++) a[i] = b[i] = (uint32_t)next_byte() << 24 | next_byte() << 16 | next_byte() << 8 | next_byte();
        blocks_c(a, m, counts[k]);
        blocks_ni(b, m, counts[k]);
        if (memcmp(a, b, sizeof a)) {
            printf("FAIL blocks: %zu blocks differ between portable C and SHA-NI\n", counts[k]);
            failures++;
        }
        checked++;
    }
    return checked;
}

static void ran(const char *what, long before, int ni) {
    if ((rounds > before) != ni) {
        printf("FAIL %s: meant for %s, but %ld SHA-NI rounds ran\n", what, ni ? "SHA-NI" : "portable C", rounds - before);
        failures++;
    }
}
#endif

int main(void) {
    int ni = sha256_ni();
    printf("cpuid sha_ni=%d\n", ni);
#if defined(__x86_64__) && (!defined(__clang__) || __clang_major__ >= 19)
    printf("builtin sha_ni=%d\n", __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1") &&
                                      __builtin_cpu_supports("ssse3"));
#endif
#if defined(__x86_64__)
    long before = rounds;
#endif
    g_ni = 0;
    vectors();
#if defined(__x86_64__)
    ran("the vectors in portable C", before, 0);
#endif
    if (!ni) {
        printf("compared ni=0\n");
        return failures ? 1 : 0;
    }
#if defined(__x86_64__)
    before = rounds;
    g_ni = 1;
    vectors();
    ran("the vectors on SHA-NI", before, 1);
    int block_runs = compare_blocks();
    static uint8_t m[70000];
    for (size_t i = 0; i < sizeof m; i++) m[i] = next_byte();
    const size_t longs[] = {1000, 4095, 4096, 4097, 65535, 65536, 65537, 70000};
    const size_t pieces[] = {0, 1, 3, 55, 56, 63, 64, 65, 127, 1000};
    int messages = 0;
    for (size_t n = 0; n < 300 + sizeof longs / sizeof *longs; n++) {
        size_t len = n < 300 ? n : longs[n - 300];
        for (size_t p = 0; p < sizeof pieces / sizeof *pieces; p++) {
            uint8_t c[32], x[32];
            before = rounds;
            g_ni = 0;
            hash_pieces(m, len, pieces[p], c);
            ran("a message in portable C", before, 0);
            before = rounds;
            g_ni = 1;
            hash_pieces(m, len, pieces[p], x);
            ran("a message on SHA-NI", before, 1);
            if (memcmp(c, x, 32)) {
                printf("FAIL message of %zu bytes in pieces of %zu: portable C and SHA-NI differ\n", len, pieces[p]);
                failures++;
            }
            messages++;
        }
    }
    printf("compared ni=1 blocks=%d messages=%d\n", block_runs, messages);
#endif
    return failures ? 1 : 0;
}
