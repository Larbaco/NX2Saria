#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "quit_menu.h"
#include "quit_menu_core.h"
#include "sarc.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); putchar('\n'); fails++; } } while (0)

static size_t section(uint8_t *o, size_t at, const char *tag, const uint8_t *data, size_t n) {
    memcpy(o + at, tag, 4);
    qm_put32(o + at + 4, (uint32_t)n);
    memset(o + at + 8, 0, 8);
    memcpy(o + at + 16, data, n);
    size_t end = (at + 16 + n + 15) & ~(size_t)15;
    memset(o + at + 16 + n, 0xab, end - (at + 16 + n));
    return end;
}

static size_t hand_made(uint8_t *o, uint32_t groups) {
    static const uint8_t head[QM_MSBT_HEAD] = {'M', 's', 'g', 'S', 't', 'd', 'B', 'n', 0xff, 0xfe, 0, 0, 0, 3, 2, 0};
    uint8_t lbl[256], txt[64];
    memcpy(o, head, sizeof head);
    qm_put32(lbl, groups);
    size_t w = 4 + groups * 8;
    for (uint32_t g = 0; g < groups; g++) qm_put32(lbl + 4 + g * 8, 0), qm_put32(lbl + 8 + g * 8, (uint32_t)w);
    if (groups == 1) {
        qm_put32(lbl + 4, 2);
        memcpy(lbl + w, "\x04""0000\x00\x00\x00\x00\x03""Yes\x01\x00\x00\x00", 17);
        w += 17;
        qm_put32(txt, 2), qm_put32(txt + 4, 12), qm_put32(txt + 8, 21);
        memcpy(txt + 12, "New Game\0Go\x0e\x00\x02\x00on", 18);
    } else {
        qm_put32(txt, 0);
    }
    size_t at = section(o, QM_MSBT_HEAD, "LBL1", lbl, w);
    at = section(o, at, "TXT2", txt, groups == 1 ? 30 : 4);
    qm_put32(o + 0x12, (uint32_t)at);
    return at;
}

static int is(const uint8_t *p, size_t n, const char *label, const char *want) {
    const uint8_t *t;
    size_t tn;
    return qm_msbt_find(p, n, label, &t, &tn) && tn == strlen(want) && !memcmp(t, want, tn);
}

static void format(void) {
    CHECK(qm_label_hash("0000", 4, 2) == 0 && qm_label_hash("0002", 4, 2) == 0 && qm_label_hash("0001", 4, 2) == 1,
          "the label hash on two groups");
    CHECK(qm_label_hash("1000", 4, 5) == 0 && qm_label_hash("0000", 4, 5) == 1 && qm_label_hash("1001", 4, 5) == 1 &&
          qm_label_hash("0001", 4, 5) == 2 && qm_label_hash("1002", 4, 5) == 2, "the label hash on five groups");
    uint8_t a[512], *out = NULL, *again = NULL;
    size_t n = hand_made(a, 1), out_n = 0, again_n = 0;
    const uint8_t *t;
    size_t tn;
    CHECK(is(a, n, "0000", "New Game"), "a label of the file made by hand");
    CHECK(qm_msbt_find(a, n, "Yes", &t, &tn) && tn == 8 && !memcmp(t, "Go\x0e\x00\x02\x00on", 8),
          "a text with a control tag, whose bytes hold zeros, is whole");
    CHECK(!qm_msbt_find(a, n, "000", &t, &tn) && !qm_msbt_find(a, n, "00000", &t, &tn), "a label that is not there");
    const QmText add[] = {{"Pa_Button_02-T_Btn_00", (const uint8_t *)"Quit Game", 9}, {"0000", (const uint8_t *)"New Game", 8},
                          {"1010", (const uint8_t *)"T\xc3\xadtulo", 7}};
    CHECK(qm_msbt_add(a, n, add, 3, &out, &out_n) == 2 && out, "two labels are new, the third has its text already");
    if (out) {
        CHECK(out_n % 16 == 0 && qm_u32(out + 0x12) == out_n && !memcmp(out, a, 0x12), "the header and the size");
        CHECK(is(out, out_n, "0000", "New Game") && qm_msbt_find(out, out_n, "Yes", &t, &tn) && tn == 8 &&
              !memcmp(t, "Go\x0e\x00\x02\x00on", 8) &&
              is(out, out_n, "Pa_Button_02-T_Btn_00", "Quit Game") && is(out, out_n, "1010", "T\xc3\xadtulo"),
              "old and new labels after the edit");
        CHECK(qm_msbt_add(out, out_n, add, 3, &again, &again_n) == 0 && !again, "an edited file needs no edit");
        const QmText other[] = {{"1010", (const uint8_t *)"Tela inicial", 12}};
        CHECK(qm_msbt_add(out, out_n, other, 1, &again, &again_n) == 1 && again && is(again, again_n, "1010", "Tela inicial") &&
              is(again, again_n, "Pa_Button_02-T_Btn_00", "Quit Game"), "a label with another text is replaced");
        free(again);
    }
    free(out);
    uint8_t b[512];
    size_t bn = hand_made(b, 5);
    const QmText five[] = {{"1000", (const uint8_t *)"a", 1}, {"0000", (const uint8_t *)"b", 1}, {"1001", (const uint8_t *)"c", 1},
                           {"0001", (const uint8_t *)"d", 1}, {"1002", (const uint8_t *)"e", 1}};
    CHECK(qm_msbt_add(b, bn, five, 5, &out, &out_n) == 5 && out, "five labels into an empty file of five groups");
    if (out) {
        const uint8_t *lbl = out + QM_MSBT_HEAD + 16;
        CHECK(qm_u32(lbl + 4) == 1 && qm_u32(lbl + 12) == 2 && qm_u32(lbl + 20) == 2 && qm_u32(lbl + 28) == 0 &&
              qm_u32(lbl + 36) == 0, "each label in the group its hash names");
        CHECK(is(out, out_n, "1000", "a") && is(out, out_n, "0000", "b") && is(out, out_n, "1001", "c") &&
              is(out, out_n, "0001", "d") && is(out, out_n, "1002", "e"), "the five labels");
    }
    free(out);
    uint8_t bad[512];
    memcpy(bad, a, n);
    bad[0xc] = 1;
    CHECK(!qm_msbt_find(bad, n, "0000", &t, &tn) && qm_msbt_add(bad, n, add, 3, &out, &out_n) == -1, "UTF-16 is refused");
    memcpy(bad, a, n);
    bad[0xe] = 3;
    CHECK(qm_msbt_add(bad, n, add, 3, &out, &out_n) == -1, "a third section is refused");
    for (size_t cut = 0; cut < n - 2; cut += 7) {
        CHECK(qm_msbt_add(a, cut, add, 3, &out, &out_n) == -1, "a file cut at %zu", cut);
        free(out);
    }
    memcpy(bad, a, n);
    bad[96 + 20] = 'x';
    CHECK(!qm_msbt_find(bad, n, "0000", &t, &tn), "a text that does not end in a NUL is refused");
}

static void choices(void) {
    CHECK(qm_window_label(1, 0, 3) == QM_LABEL_TITLE && qm_window_label(1, 1, 1) == QM_LABEL_CANCEL,
          "the window's two labels with three buttons");
    CHECK(qm_window_label(0, 0, 3) == 3 && qm_window_label(0, 1, 1) == 1 && qm_window_label(1, 0, 0) == 0 &&
          qm_window_label(1, 0, 2) == 2 && qm_window_label(1, 0, 4) == 4 && qm_window_label(1, 2, QM_LABEL_QUIT) == QM_LABEL_QUIT &&
          qm_window_label(0, 3, QM_LABEL_QUIT) == QM_LABEL_QUIT, "every other label stays");
    QmPress p = {0, 0};
    int chosen;
    CHECK(!qm_press(&p, 1, 0, &chosen) && !chosen, "on the entry, nothing pressed");
    CHECK(qm_press(&p, 1, 1, &chosen) && chosen, "A on the entry: taken and chosen");
    CHECK(qm_press(&p, 1, 1, &chosen) && !chosen, "held: taken, chosen once");
    CHECK(qm_press(&p, 0, 1, &chosen) && !chosen, "still held when the focus has gone: not given to the game");
    CHECK(!qm_press(&p, 0, 0, &chosen) && !chosen, "released");
    CHECK(!qm_press(&p, 0, 1, &chosen) && !chosen, "A on another entry is the game's");
    CHECK(!qm_press(&p, 1, 1, &chosen) && !chosen, "A held from another entry is no choice when the focus arrives");
    CHECK(!qm_press(&p, 1, 0, &chosen) && qm_press(&p, 1, 1, &chosen) && chosen, "a new press there is");
}

static uint8_t *unpack(const uint8_t *z, size_t zn, const uint8_t *dics, size_t dn, size_t *n) {
    unsigned id = ZSTD_getDictID_fromFrame(z, zn);
    size_t at = 0, size = 0;
    unsigned long long rn = ZSTD_getFrameContentSize(z, zn);
    if (rn == ZSTD_CONTENTSIZE_ERROR || rn == ZSTD_CONTENTSIZE_UNKNOWN || (id && !zsdic_find(dics, dn, id, &at, &size))) return NULL;
    uint8_t *raw = malloc((size_t)rn);
    ZSTD_DCtx *d = ZSTD_createDCtx();
    size_t got = id ? ZSTD_decompress_usingDict(d, raw, (size_t)rn, z, zn, dics + at, size) : ZSTD_decompressDCtx(d, raw, (size_t)rn, z, zn);
    ZSTD_freeDCtx(d);
    if (ZSTD_isError(got) || got != rn) {
        free(raw);
        return NULL;
    }
    *n = (size_t)rn;
    return raw;
}

static uint8_t *slurp(FILE *f, long long at, size_t n) {
    uint8_t *p = malloc(n ? n : 1);
#ifdef _WIN32
    _fseeki64(f, at, SEEK_SET);
#else
    fseeko(f, at, SEEK_SET);
#endif
    if (fread(p, 1, n, f) != n) {
        free(p);
        return NULL;
    }
    return p;
}

static void game_archive(const char *name, const uint8_t *z, size_t zn, const uint8_t *zdic, size_t zdic_n,
                         const uint8_t *dics, size_t dn) {
    char why[200] = "";
    void *out = NULL, *again = NULL;
    size_t out_n = 0, again_n = 0, was_n = 0, now_n = 0;
    CHECK(quit_menu_build(z, zn, zdic, zdic_n, &out, &out_n, why, sizeof why) == 1 && out, "%s: %s", name, why);
    if (!out) return;
    uint8_t *was = unpack(z, zn, dics, dn, &was_n), *now = unpack(out, out_n, dics, dn, &now_n);
    CHECK(was && now, "%s: the archives decompress", name);
    if (was && now) {
        static const struct { const char *member, *label, *from, *source; } want[] = {
            {QUIT_MENU_MSG_WINDOW, "1010", QUIT_MENU_MSG_PAUSE, QUIT_MENU_LABEL_TITLE_SCREEN},
            {QUIT_MENU_MSG_WINDOW, "1011", QUIT_MENU_MSG_SAVE, QUIT_MENU_LABEL_CANCEL},
            {QUIT_MENU_MSG_WINDOW, "1012", QUIT_MENU_MSG_QUIT, QUIT_MENU_LABEL_QUIT}};
        for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
            size_t ma, mn, sa, sn, tn, un;
            const uint8_t *t, *u;
            int ok = sarc_member(now, now_n, want[i].member, &ma, &mn) && sarc_member(was, was_n, want[i].from, &sa, &sn) &&
                     qm_msbt_find(now + ma, mn, want[i].label, &t, &tn) &&
                     qm_msbt_find(was + sa, sn, want[i].source, &u, &un) && tn == un && tn && !memcmp(t, u, tn);
            CHECK(ok, "%s: %s %s is the text of %s %s", name, want[i].member, want[i].label, want[i].from, want[i].source);
        }
        int count = sarc_count(was, was_n), same = count > 0 && sarc_count(now, now_n) == count, edited = 0;
        unsigned a = 0, b = 0;
        for (int i = 0; same && i < count; i++) {
            size_t wa, wn, na, nn;
            if (sarc_next(was, was_n, NULL, &a, &wa, &wn) != 1 || sarc_next(now, now_n, NULL, &b, &na, &nn) != 1) same = 0;
            else if (wn != nn || memcmp(was + wa, now + na, wn)) edited++;
        }
        CHECK(same && edited == 1, "%s: %d of %d members differ, 1 expected", name, edited, count);
        static const char *const old[] = {"0000", "0001", "0002", "1000", "1001", "1003"};
        for (size_t i = 0; i < sizeof old / sizeof *old; i++) {
            size_t wa, wn, na, nn, tn, un;
            const uint8_t *t, *u;
            int ok = sarc_member(was, was_n, QUIT_MENU_MSG_WINDOW, &wa, &wn) && sarc_member(now, now_n, QUIT_MENU_MSG_WINDOW, &na, &nn) &&
                     qm_msbt_find(was + wa, wn, old[i], &u, &un) && qm_msbt_find(now + na, nn, old[i], &t, &tn) && tn == un &&
                     !memcmp(t, u, tn);
            CHECK(ok, "%s: the window's own label %s keeps its text", name, old[i]);
        }
    }
    CHECK(quit_menu_build(out, out_n, zdic, zdic_n, &again, &again_n, why, sizeof why) == 0 && !again,
          "%s: an archive with the labels needs no edit", name);
    free(was);
    free(now);
    free(out);
}

int main(int argc, char **argv) {
    format();
    choices();
    int archives = 0;
    if (argc >= 4 && (argc - 4) % 3 == 0) {
        FILE *f = fopen(argv[1], "rb");
        size_t zdic_n = (size_t)strtoull(argv[3], NULL, 10), dn = 0;
        uint8_t *zdic = f ? slurp(f, strtoll(argv[2], NULL, 10), zdic_n) : NULL;
        uint8_t *dics = zdic ? zsdic_unpack(zdic, zdic_n, 64u << 20, &dn) : NULL;
        CHECK(dics != NULL, "the zstd dictionaries of %s", argv[1]);
        for (int i = 4; dics && i + 2 < argc; i += 3, archives++) {
            size_t zn = (size_t)strtoull(argv[i + 2], NULL, 10);
            uint8_t *z = slurp(f, strtoll(argv[i + 1], NULL, 10), zn);
            CHECK(z != NULL, "%s cannot be read", argv[i]);
            if (z) game_archive(argv[i], z, zn, zdic, zdic_n, dics, dn);
            free(z);
        }
        free(dics);
        free(zdic);
        if (f) fclose(f);
    }
    printf("quit_menu_check: %s (%d game message archives)\n", fails ? "FAIL" : "ok", archives);
    return fails != 0;
}
