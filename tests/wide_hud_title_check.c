#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zdict.h>
#include <zstd.h>

#include "sarc.h"
#include "wide_hud.h"

#define MENUS(m) (&(WideHudShift){0, 1.0f, (m), 0})
#define HUD(o) (&(WideHudShift){(o), 1.0f, 0, 0})

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL line %d: ", __LINE__);                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

enum { LOG_LINES = 64 };
static char log_lines[LOG_LINES][1024];
static int nlog;
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_lines[nlog % LOG_LINES], sizeof log_lines[0], fmt, ap);
    va_end(ap);
    nlog++;
}
uint64_t plat_now_ns(void) { return (uint64_t)((double)clock() / CLOCKS_PER_SEC * 1e9); }
static const char *last_log(void) { return nlog ? log_lines[(nlog - 1) % LOG_LINES] : ""; }
static const char *log_find(const char *prefix) {
    for (int i = nlog - 1; i >= 0 && i >= nlog - LOG_LINES; i--)
        if (!strncmp(log_lines[i % LOG_LINES], prefix, strlen(prefix))) return log_lines[i % LOG_LINES];
    return NULL;
}

enum { SIZE = 0x4000, PANE_REC = 0x44 };
static uint8_t file[SIZE];
static size_t top;
static uint64_t alloc(size_t n) {
    top = (top + 7) & ~(size_t)7;
    uint64_t at = top;
    top += n;
    if (top > SIZE) {
        printf("the test's layout is too small\n");
        exit(2);
    }
    return at;
}
static void w32(uint64_t at, uint32_t v) { memcpy(file + at, &v, 4); }
static void w64(uint64_t at, uint64_t v) { memcpy(file + at, &v, 8); }
static void wf(uint64_t at, float v) { memcpy(file + at, &v, 4); }
static float rf(const uint8_t *p, uint64_t at) {
    float v;
    memcpy(&v, p + at, 4);
    return v;
}

typedef struct {
    const char *name;
    int parent;
    float x, y, w, h;
} PaneDef;
static uint64_t pane_at[16];

static size_t make_layout(const PaneDef *d, int npanes) {
    memset(file, 0, sizeof file);
    top = 0x300;
    memcpy(file, "BFFH", 4);
    uint64_t names = alloc(8 * (size_t)npanes), records = alloc(8 * (size_t)npanes);
    for (int i = 0; i < npanes; i++) {
        uint64_t r = alloc(PANE_REC), s = alloc(strlen(d[i].name) + 1);
        memcpy(file + s, d[i].name, strlen(d[i].name) + 1);
        for (int k = 0; k < PANE_REC; k += 4) w32(r + k, 0xa5a50000u + (uint32_t)(i << 8) + (uint32_t)k);
        wf(r + 0x10, 1), wf(r + 0x14, 1), wf(r + 0x18, 0), wf(r + 0x1c, d[i].x), wf(r + 0x20, d[i].y);
        w32(r + 0x28, (uint32_t)d[i].parent), wf(r + 0x2c, d[i].w), wf(r + 0x30, d[i].h);
        w64(names + 8 * i, s), w64(records + 8 * i, r);
        pane_at[i] = r;
    }
    w64(0x118, names), w64(0x120, records), w32(0x130, (uint32_t)npanes), w32(0x134, 8);
    uint64_t anames = alloc(8), arecords = alloc(8);
    w64(0x238, anames), w64(0x240, arecords), w32(0x250, 0), w32(0x254, 8);
    return top;
}

static const PaneDef menu_bg[] = {
    {"RootPane", -1, 0, 0, 0, 0},         {"N_In_00", 0, 0, 0, 90, 120},          {"N_Base_00", 1, 0, 0, 90, 120},
    {"C_Capture_00", 2, 0, 0, 3840, 2160}, {"P_Capture_00", 2, 0, 0, 3840, 2160}, {"P_Noise_00", 2, 0, 0, 3840, 2160}};
static const PaneDef title[] = {{"RootPane", -1, 0, 0, 0, 0},
                                {"N_BtnPos_00", 0, 0, -276, 60, 80},
                                {"N_All_00", 0, 0, 0, 200, 200},
                                {"Pa_Button_00", 2, 0, -65, 4060, 210}};
static const PaneDef button[] = {{"RootPane", -1, 0, 0, 0, 0}, {"W_Bg_00", 0, 0, 0, 1880, 160}};

enum { MAX_MEMBERS = 32 };
static size_t member_at[MAX_MEMBERS];
static size_t make_sarc(uint8_t *out, size_t cap, const char *const *names, const uint8_t *const *bytes,
                        const size_t *sizes, int count) {
    static const char other[] = "Other_00.buil";
    static const uint8_t other_data[64] = {1, 2, 3, 4, 5, 6, 7, 8};
    int all = 1 + count;
    size_t sfat = 0x14, sfnt = sfat + 0xc + 16 * (size_t)all, name_len[MAX_MEMBERS], names_len = 0;
    size_t other_len = (sizeof other + 3) & ~(size_t)3;
    for (int i = 0; i < count; i++) name_len[i] = (strlen(names[i]) + 1 + 3) & ~(size_t)3, names_len += name_len[i];
    size_t data = (sfnt + 8 + other_len + names_len + 0xff) & ~(size_t)0xff;
    size_t total = (data + sizeof other_data + 0xff) & ~(size_t)0xff;
    for (int i = 0; i < count; i++) member_at[i] = total, total = (total + sizes[i] + 0xff) & ~(size_t)0xff;
    if (total > cap) return 0;
    memset(out, 0, total);
    memcpy(out, "SARC", 4);
    uint16_t hl = 0x14, bom = 0xfeff, v = 0x100, fl = 0xc, cnt = (uint16_t)all, eight = 8;
    uint32_t t32 = (uint32_t)total, d32 = (uint32_t)data, key = 0x65;
    memcpy(out + 4, &hl, 2), memcpy(out + 6, &bom, 2);
    memcpy(out + 8, &t32, 4), memcpy(out + 12, &d32, 4), memcpy(out + 16, &v, 2);
    memcpy(out + sfat, "SFAT", 4), memcpy(out + sfat + 4, &fl, 2), memcpy(out + sfat + 6, &cnt, 2);
    memcpy(out + sfat + 8, &key, 4);
    memcpy(out + sfnt, "SFNT", 4), memcpy(out + sfnt + 4, &eight, 2);
    size_t nm = sfnt + 8;
    memcpy(out + nm, other, sizeof other);
    uint32_t node[4] = {1, 0x01000000u, 0, (uint32_t)sizeof other_data};
    memcpy(out + sfat + 0xc, node, sizeof node);
    memcpy(out + data, other_data, sizeof other_data);
    size_t s = nm + other_len;
    for (int i = 0; i < count; i++) {
        memcpy(out + s, names[i], strlen(names[i]) + 1);
        uint32_t e[4] = {2 + (uint32_t)i, 0x01000000u | (uint32_t)((s - nm) / 4), (uint32_t)(member_at[i] - data),
                         (uint32_t)(member_at[i] - data + sizes[i])};
        memcpy(out + sfat + 0xc + 16 * (size_t)(i + 1), e, sizeof e);
        memcpy(out + member_at[i], bytes[i], sizes[i]);
        s += name_len[i];
    }
    return total;
}

static uint8_t *made[3];
static size_t made_n[3];
static uint64_t width_at[3];
static void make_members(void) {
    static const PaneDef *defs[3] = {button, menu_bg, title};
    static const int counts[3] = {2, 6, 4};
    for (int i = 0; i < 3; i++) {
        made_n[i] = make_layout(defs[i], counts[i]);
        free(made[i]);
        made[i] = malloc(made_n[i]);
        memcpy(made[i], file, made_n[i]);
        if (i == 1)
            for (int k = 0; k < 3; k++) width_at[k] = pane_at[3 + k] + 0x2c;
    }
}
static size_t archive(uint8_t *out, size_t cap, int copies) {
    static const char *const names[3] = {"PaCommonBtnTitle_00.buil", "TitleMenuBG_00.buil", "Title_00.buil"};
    const char *all_names[3 * 8];
    const uint8_t *all_bytes[3 * 8];
    size_t all_sizes[3 * 8];
    for (int c = 0; c < copies; c++)
        for (int i = 0; i < 3; i++)
            all_names[3 * c + i] = names[i], all_bytes[3 * c + i] = made[i], all_sizes[3 * c + i] = made_n[i];
    return make_sarc(out, cap, all_names, all_bytes, all_sizes, 3 * copies);
}

static float width_of(const uint8_t *p, size_t n, const char *member, int copy, int pane) {
    unsigned next = 0;
    size_t at, size;
    for (int c = 0; sarc_next(p, n, member, &next, &at, &size) == 1; c++)
        if (c == copy) return rf(p, at + width_at[pane]);
    return -1;
}

static int archive_in(int archive, uint8_t *b, size_t n, const WideHudShift *s, char *why, size_t why_n) {
    uint8_t *p = malloc(n ? n : 1);
    size_t m = n;
    memcpy(p, b, n);
    int r = wide_hud_archive_in(archive, &p, &m, s, why, why_n);
    CHECK(m == n, "the archive grew from %zu to %zu bytes", n, m);
    if (m == n) memcpy(b, p, n);
    free(p);
    return r;
}

static void check_synthetic(void) {
    enum { CAP = 1 << 18 };
    uint8_t *a = malloc(CAP), *b = malloc(CAP);
    size_t n = archive(a, CAP, 2);
    char why[240] = "";
    float menus = 640;
    memcpy(b, a, n);
    nlog = 0;
    CHECK(archive_in(WIDE_HUD_COMMON, b, n, MENUS(menus), why, sizeof why) == 0 && !memcmp(a, b, n),
          "the Common archive's edit of the title archive: %s", why);
    CHECK(!log_find("wide menus: TitleMenuBG_00"), "the Common archive's edit named TitleMenuBG_00");
    nlog = 0;
    CHECK(archive_in(WIDE_HUD_TITLE, b, n, HUD(660), why, sizeof why) == 0 && !memcmp(a, b, n) && nlog == 0,
          "the HUD's edit of the title archive: %d lines", nlog);
    nlog = 0;
    int r = archive_in(WIDE_HUD_TITLE, b, n, MENUS(menus), why, sizeof why);
    CHECK(r == 6, "title archive: %d fields (%s)", r, why);
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 3; k++)
            CHECK(width_of(b, n, "TitleMenuBG_00.buil", c, k) == 3840 + 2 * menus, "copy %d pane %d width %g", c, k,
                  (double)width_of(b, n, "TitleMenuBG_00.buil", c, k));
    size_t changed = 0, outside = 0;
    for (size_t i = 0; i + 4 <= n; i += 4)
        if (memcmp(a + i, b + i, 4)) {
            changed++;
            int inside = 0;
            for (int c = 0; c < 2; c++)
                for (int k = 0; k < 3; k++) inside |= i == member_at[3 * c + 1] + width_at[k];
            outside += !inside;
        }
    CHECK(changed == 6 && !outside, "title archive: %zu words changed, %zu not a width of TitleMenuBG_00", changed,
          outside);
    CHECK(nlog == 1 &&
              log_find("wide menus: TitleMenuBG_00 (2 copies, 6 fields): C_Capture_00 width 3840 -> 5120, "
                       "P_Capture_00 width 3840 -> 5120, P_Noise_00 width 3840 -> 5120"),
          "title archive: %d lines, the first: %s", nlog, nlog ? log_lines[0] : "");
    nlog = 0;
    CHECK(archive_in(WIDE_HUD_TITLE, b, n, MENUS(menus), why, sizeof why) == 0 && nlog == 0,
          "the title archive edited twice: %d lines", nlog);
    memcpy(b, a, n);
    float other = 3000;
    memcpy(b + member_at[1] + width_at[1], &other, 4);
    uint8_t *odd = malloc(n);
    memcpy(odd, b, n);
    nlog = 0;
    r = archive_in(WIDE_HUD_TITLE, b, n, MENUS(menus), why, sizeof why);
    CHECK(r == 0 && !memcmp(b, odd, n) && log_find("wide menus: TitleMenuBG_00 P_Capture_00 width is 3000, not 3840"),
          "a title archive with another width: %d (%s)", r, last_log());
    CHECK(log_find("wide menus: TitleMenuBG_00 P_Capture_00") && strstr(last_log(), "TitleMenuBG_00 keeps the game's menu layout"),
          "a title archive with another width: no line that the game's layout stays");
    {
        const char *names[] = {"Title_00.buil"};
        const uint8_t *bytes[] = {made[2]};
        size_t sizes[] = {made_n[2]};
        uint8_t *c3 = malloc(CAP);
        size_t m = make_sarc(c3, CAP, names, bytes, sizes, 1);
        nlog = 0;
        r = archive_in(WIDE_HUD_TITLE, c3, m, MENUS(menus), why, sizeof why);
        CHECK(r == 0 && strstr(last_log(), "TitleMenuBG_00 stays as the game lays it out"),
              "a title archive without TitleMenuBG_00: %d (%s)", r, last_log());
        free(c3);
    }
    nlog = 0;
    CHECK(archive_in(WIDE_HUD_TITLE, a, 40, MENUS(menus), why, sizeof why) == 0 &&
              strstr(last_log(), "the layout archive is damaged; TitleMenuBG_00 stays as the game lays it out"),
          "a damaged title archive: %s", last_log());
    CHECK(archive_in(7, a, n, MENUS(menus), why, sizeof why) == 0, "an archive number that is not one");
    CHECK(wide_hud_archive_path(WIDE_HUD_TITLE) && !strcmp(wide_hud_archive_path(WIDE_HUD_TITLE), WIDE_HUD_TITLE_ARCHIVE) &&
              !strcmp(wide_hud_archive_path(WIDE_HUD_COMMON), WIDE_HUD_ARCHIVE) && !wide_hud_archive_path(5),
          "the archive paths");
    free(odd);
    free(a);
    free(b);
}

static size_t make_dict(uint8_t *out, size_t cap, unsigned id, const uint8_t *src, size_t n) {
    enum { SAMPLES = 32 };
    size_t sizes[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) sizes[i] = n / SAMPLES;
    ZDICT_params_t params = {3, 0, id};
    size_t r = ZDICT_finalizeDictionary(out, cap, src, n < 2048 ? n : 2048, src, sizes, SAMPLES, params);
    return ZDICT_isError(r) ? 0 : r;
}

static size_t make_zsdic(uint8_t *out, size_t cap, const uint8_t *dict, size_t dn) {
    static uint8_t sarc[1 << 16];
    const char *names[] = {"zs.zsdic"};
    const uint8_t *bytes[] = {dict};
    size_t sizes[] = {dn};
    size_t sn = make_sarc(sarc, sizeof sarc, names, bytes, sizes, 1);
    size_t zn = ZSTD_compress(out, cap, sarc, sn, 3);
    return ZSTD_isError(zn) ? 0 : zn;
}

static void check_build(void) {
    enum { CAP = 1 << 18 };
    uint8_t *a = malloc(CAP), *dict = malloc(1 << 16), *zdic = malloc(1 << 17), *z = malloc(CAP);
    size_t n = archive(a, CAP, 2), dn = make_dict(dict, 1 << 16, 0x4321, a, n);
    CHECK(dn > 0, "the test's dictionary");
    size_t zdn = make_zsdic(zdic, 1 << 17, dict, dn);
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    size_t zn = ZSTD_compress_usingDict(cc, z, CAP, a, n, dict, dn, 3);
    ZSTD_freeCCtx(cc);
    CHECK(!ZSTD_isError(zn) && ZSTD_getDictID_fromFrame(z, zn) == 0x4321, "the test's archive frame");
    void *out = NULL, *again = NULL;
    size_t out_n = 0, again_n = 0;
    nlog = 0;
    int r = wide_hud_build_in(WIDE_HUD_TITLE, z, zn, zdic, zdn, MENUS(640), &out, &out_n);
    CHECK(r == 1 && out, "build: %d (%s)", r, last_log());
    CHECK(log_find("wide hud: the title layout archive edited (the menus over the whole picture, 640 units wider on each "
                   "side), 6 fields"),
          "build: its line: %s", last_log());
    if (r == 1) {
        CHECK(ZSTD_getDictID_fromFrame(out, out_n) == 0x4321 && ZSTD_getFrameContentSize(out, out_n) == n,
              "build: another dictionary or size");
        uint8_t *raw = malloc(n), *expect = malloc(n);
        ZSTD_DCtx *dc = ZSTD_createDCtx();
        size_t got = ZSTD_decompress_usingDict(dc, raw, n, out, out_n, dict, dn);
        ZSTD_freeDCtx(dc);
        char why[240];
        memcpy(expect, a, n);
        archive_in(WIDE_HUD_TITLE, expect, n, MENUS(640), why, sizeof why);
        CHECK(got == n && !memcmp(raw, expect, n), "build: not the archive edited in memory");
        r = wide_hud_build_in(WIDE_HUD_TITLE, out, out_n, zdic, zdn, MENUS(640), &again, &again_n);
        CHECK(r == 0 && !again && strstr(last_log(), "the title layout archive has these edits already (the menus over"),
              "build again: %d (%s)", r, last_log());
        free(raw);
        free(expect);
    }
    nlog = 0;
    CHECK(wide_hud_build_in(WIDE_HUD_TITLE, z, zn / 2, zdic, zdn, &(WideHudShift){660, 0.75f, 0, 0}, &again, &again_n) == 0 &&
              !again && nlog == 0,
          "build with no menus: %s", last_log());
    CHECK(wide_hud_build_in(WIDE_HUD_TITLE, z, zn, zdic, zdn, MENUS(0), &again, &again_n) == 0 && !again,
          "build at menus 0");
    CHECK(wide_hud_build_in(WIDE_HUD_TITLE, z, zn / 2, zdic, zdn, MENUS(640), &again, &again_n) == -1 &&
              strstr(last_log(), "the title layout archive is served as it is"),
          "build of a cut frame: %s", last_log());
    free(out);
    free(a);
    free(dict);
    free(zdic);
    free(z);
}

static uint8_t *read_part(const char *path, const char *off, const char *size, size_t *n) {
    long long at = strtoll(off, NULL, 10);
    *n = (size_t)strtoull(size, NULL, 10);
    FILE *f = fopen(path, "rb");
    uint8_t *p = f && *n ? malloc(*n) : NULL;
    int ok = p && !fseeko(f, (off_t)at, SEEK_SET) && fread(p, 1, *n, f) == *n;
    if (f) fclose(f);
    if (!ok) free(p);
    return ok ? p : NULL;
}

static uint8_t *unpack(const void *z, size_t zn, const uint8_t *dics, size_t dn, size_t *n) {
    unsigned id = ZSTD_getDictID_fromFrame(z, zn);
    unsigned long long rn = ZSTD_getFrameContentSize(z, zn);
    size_t at = 0, size = 0;
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || (id && !zsdic_find(dics, dn, id, &at, &size)))
        return NULL;
    uint8_t *raw = malloc((size_t)rn);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    size_t got = !raw || !d ? 0
                 : id       ? ZSTD_decompress_usingDict(d, raw, (size_t)rn, z, zn, dics + at, size)
                            : ZSTD_decompressDCtx(d, raw, (size_t)rn, z, zn);
    ZSTD_freeDCtx(d);
    if (ZSTD_isError(got) || got != rn) {
        free(raw);
        return NULL;
    }
    *n = (size_t)rn;
    return raw;
}

static int in_member(const uint8_t *a, size_t n, const char *name, size_t i) {
    unsigned next = 0;
    size_t at, size;
    while (sarc_next(a, n, name, &next, &at, &size) == 1)
        if (i >= at && i < at + size) return 1;
    return 0;
}

static void game_archive(char **argv) {
    size_t an, dn, pn = 0, rn = 0;
    uint8_t *a = read_part(argv[1], argv[2], argv[3], &an), *d = read_part(argv[1], argv[4], argv[5], &dn);
    uint8_t *dics = d ? zsdic_unpack(d, dn, 64u << 20, &pn) : NULL;
    uint8_t *raw = a && dics ? unpack(a, an, dics, pn, &rn) : NULL;
    CHECK(raw != NULL, "game: cannot read and unpack the title archive from %s", argv[1]);
    if (!raw) {
        free(dics), free(a), free(d);
        return;
    }
    int copies = 0;
    unsigned next = 0;
    size_t at, size;
    while (sarc_next(raw, rn, "TitleMenuBG_00.buil", &next, &at, &size) == 1) copies++;
    printf("game: the title archive holds TitleMenuBG_00 %d times (%zu bytes unpacked)\n", copies, rn);
    CHECK(copies > 0, "game: no TitleMenuBG_00 in the title archive");
    for (const char *const *m = (const char *const[]){"Title_00.buil", "PaCommonBtnTitle_00.buil", NULL}; *m; m++) {
        next = 0;
        CHECK(sarc_next(raw, rn, *m, &next, &at, &size) == 1, "game: no %s in the title archive", *m);
    }
    static const struct { unsigned w, h; } shapes[] = {{2560, 1080}, {3416, 1440}, {5120, 1440}, {3440, 1440}};
    for (size_t s = 0; s < sizeof shapes / sizeof *shapes; s++) {
        float out = wide_hud_out(shapes[s].w, shapes[s].h);
        void *z = NULL;
        size_t zn = 0, on = 0;
        nlog = 0;
        int made_z = wide_hud_build_in(WIDE_HUD_TITLE, a, an, d, dn, MENUS(out), &z, &zn);
        CHECK(made_z == 1, "game %ux%u: build %d (%s)", shapes[s].w, shapes[s].h, made_z, last_log());
        uint8_t *got = made_z == 1 ? unpack(z, zn, dics, pn, &on) : NULL;
        CHECK(got && on == rn, "game %ux%u: the built archive does not unpack", shapes[s].w, shapes[s].h);
        if (got && on == rn) {
            size_t changed = 0, outside = 0, wrong = 0;
            for (size_t i = 0; i + 4 <= rn; i += 4) {
                if (!memcmp(raw + i, got + i, 4)) continue;
                changed++;
                if (!in_member(raw, rn, "TitleMenuBG_00.buil", i)) outside++;
                else if (rf(raw, i) != 3840 || fabsf(rf(got, i) - (3840 + 2 * out)) > 0.01f) wrong++;
            }
            CHECK(changed == 3 * (size_t)copies && !outside && !wrong,
                  "game %ux%u: %zu words changed (%d expected), %zu outside TitleMenuBG_00, %zu not a 3840 width "
                  "grown by 2 * %g",
                  shapes[s].w, shapes[s].h, changed, 3 * copies, outside, wrong, (double)out);
            char why[240] = "";
            CHECK(archive_in(WIDE_HUD_TITLE, got, on, MENUS(out), why, sizeof why) == 0,
                  "game %ux%u: a second pass changes something", shapes[s].w, shapes[s].h);
            const char *line = log_find("wide menus: TitleMenuBG_00");
            printf("game %ux%u (menus %g units wider): %s\n", shapes[s].w, shapes[s].h, (double)out,
                   line ? line : "(no line)");
            CHECK(line != NULL, "game %ux%u: no TitleMenuBG_00 line", shapes[s].w, shapes[s].h);
            const char *fin = log_find("wide hud: the title layout archive edited");
            if (s == 0) printf("game %ux%u: %s\n", shapes[s].w, shapes[s].h, fin ? fin : "(no line)");
        }
        free(got);
        free(z);
    }
    {
        void *z = NULL;
        size_t zn = 0;
        CHECK(wide_hud_build_in(WIDE_HUD_TITLE, a, an, d, dn, MENUS(wide_hud_out(2560, 1440)), &z, &zn) == 0 && !z,
              "game at 16:9: something was built");
    }
    free(raw);
    free(dics);
    free(a);
    free(d);
}

int main(int argc, char **argv) {
    make_members();
    check_synthetic();
    check_build();
    if (argc == 6) game_archive(argv);
    for (int i = 0; i < 3; i++) free(made[i]);
    if (fails) {
        printf("wide hud title: %d check(s) failed\n", fails);
        return 1;
    }
    printf("wide hud title: all checks passed%s\n", argc == 6 ? " (and the game's archive)" : "");
    return 0;
}
