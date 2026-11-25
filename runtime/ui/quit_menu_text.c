#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "quit_menu.h"
#include "quit_menu_core.h"
#include "sarc.h"

#define MAX_RAW (64u << 20)

static int add_labels(const uint8_t *sarc, size_t n, const char *member, const QmText *add, int nadd,
                      const uint8_t **with, size_t *with_n, int count, uint8_t **made) {
    unsigned next = 0;
    size_t at, size;
    *made = NULL;
    if (sarc_next(sarc, n, member, &next, &at, &size) != 1 || (int)next - 1 >= count) return -1;
    size_t out_n = 0;
    int r = qm_msbt_add(sarc + at, size, add, nadd, made, &out_n);
    if (r > 0) with[next - 1] = *made, with_n[next - 1] = out_n;
    return r;
}

int quit_menu_archive(uint8_t **sarc, size_t *n, char *why, size_t why_n) {
    static const struct { const char *member, *label; } from[3] = {
        {QUIT_MENU_MSG_QUIT, QUIT_MENU_LABEL_QUIT}, {QUIT_MENU_MSG_PAUSE, QUIT_MENU_LABEL_TITLE_SCREEN}, {QUIT_MENU_MSG_SAVE, QUIT_MENU_LABEL_CANCEL}};
    const uint8_t *text[3];
    size_t text_n[3], at, size;
    for (int i = 0; i < 3; i++)
        if (!sarc_member(*sarc, *n, from[i].member, &at, &size) ||
            !qm_msbt_find(*sarc + at, size, from[i].label, &text[i], &text_n[i]) || !text_n[i]) {
            snprintf(why, why_n, "%s has no %s", from[i].member, from[i].label);
            return -1;
        }
    const QmText window[] = {{"1010", text[1], text_n[1]}, {"1011", text[2], text_n[2]}, {"1012", text[0], text_n[0]}};
    int count = sarc_count(*sarc, *n), result = -1;
    if (count <= 0) {
        snprintf(why, why_n, "the message archive is no SARC");
        return -1;
    }
    const uint8_t **with = calloc((size_t)count, sizeof *with);
    size_t *with_n = calloc((size_t)count, sizeof *with_n);
    uint8_t *made = NULL;
    if (!with || !with_n) {
        snprintf(why, why_n, "out of memory");
        goto done;
    }
    int a = add_labels(*sarc, *n, QUIT_MENU_MSG_WINDOW, window, 3, with, with_n, count, &made);
    if (a < 0) {
        snprintf(why, why_n, "%s is not the message file this expects", QUIT_MENU_MSG_WINDOW);
        goto done;
    }
    if (a == 0) {
        result = 0;
        goto done;
    }
    size_t rn = 0;
    uint8_t *rebuilt = sarc_rebuild(*sarc, *n, with, with_n, &rn);
    if (!rebuilt) {
        snprintf(why, why_n, "the message archive cannot be laid out again");
        goto done;
    }
    free(*sarc);
    *sarc = rebuilt, *n = rn;
    result = a;
done:
    free(made);
    free(with);
    free(with_n);
    return result;
}

int quit_menu_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, void **out, size_t *out_n, char *why,
                    size_t why_n) {
    *out = NULL;
    int result = -1;
    uint8_t *dics = NULL, *raw = NULL;
    void *z = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_CDict *cd = NULL;
    unsigned id = ZSTD_getDictID_fromFrame(in, n);
    size_t dn = 0, dic_at = 0, dic_n = 0;
    if (id && (!(dics = zsdic_unpack(zsdic, zsdic_n, MAX_RAW, &dn)) || !zsdic_find(dics, dn, id, &dic_at, &dic_n))) {
        snprintf(why, why_n, "no dictionary %u in Pack/ZsDic.pack.zs", id);
        goto done;
    }
    if (id && (!(dd = ZSTD_createDDict(dics + dic_at, dic_n)) || !(cd = ZSTD_createCDict(dics + dic_at, dic_n, 3)))) {
        snprintf(why, why_n, "dictionary %u of Pack/ZsDic.pack.zs cannot be loaded", id);
        goto done;
    }
    unsigned long long rn = ZSTD_getFrameContentSize(in, n);
    if (rn == ZSTD_CONTENTSIZE_UNKNOWN || rn == ZSTD_CONTENTSIZE_ERROR || !rn || rn > MAX_RAW) {
        snprintf(why, why_n, "the message archive is not a zstd frame with a size");
        goto done;
    }
    if (!(raw = malloc((size_t)rn))) {
        snprintf(why, why_n, "out of memory");
        goto done;
    }
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t got = !dctx ? 0 : dd ? ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, in, n, dd)
                                : ZSTD_decompressDCtx(dctx, raw, (size_t)rn, in, n);
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(got) || got != rn) {
        snprintf(why, why_n, "the message archive does not decompress");
        goto done;
    }
    size_t raw_n = (size_t)rn;
    int changed = quit_menu_archive(&raw, &raw_n, why, why_n);
    if (changed <= 0) {
        result = changed;
        goto done;
    }
    size_t cap = ZSTD_compressBound(raw_n), zn = 0;
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    if (cctx && (z = malloc(cap)))
        zn = cd ? ZSTD_compress_usingCDict(cctx, z, cap, raw, raw_n, cd) : ZSTD_compressCCtx(cctx, z, cap, raw, raw_n, 3);
    ZSTD_freeCCtx(cctx);
    if (!z || !zn || ZSTD_isError(zn)) {
        snprintf(why, why_n, "the message archive cannot be compressed again");
        goto done;
    }
    *out = z, *out_n = zn, z = NULL;
    result = 1;
done:
    free(z);
    free(raw);
    free(dics);
    ZSTD_freeDDict(dd);
    ZSTD_freeCDict(cd);
    return result;
}
