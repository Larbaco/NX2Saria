#ifndef WIDE_HUD_H
#define WIDE_HUD_H

#include <stddef.h>
#include <stdint.h>

#define WIDE_HUD_ARCHIVE "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"
#define WIDE_HUD_TITLE_ARCHIVE "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs"
enum { WIDE_HUD_COMMON, WIDE_HUD_TITLE };
const char *wide_hud_archive_path(int archive);

float wide_hud_out(unsigned w, unsigned h);

typedef struct {
    float out, squeeze, menus;
    int no_growth;
} WideHudShift;

typedef struct {
    const char *layout;
    int panes, keys;
} WideHudCount;
int wide_hud_counts(WideHudCount *out, int max);
int wide_hud_counts_of(int menus, WideHudCount *out, int max);

int wide_hud_lay_out(uint8_t *p, size_t n, const char *member, const WideHudShift *s, char *report, size_t report_n,
                     char *why, size_t why_n);

int wide_hud_split(const uint8_t *p, size_t n, const char *member, const WideHudShift *s, uint8_t **out, size_t *out_n,
                   char *report, size_t report_n, char *why, size_t why_n);
uint64_t wide_hud_name_hash(const char *name);

int wide_hud_archive(uint8_t **sarc, size_t *n, const WideHudShift *s, char *why, size_t why_n);
int wide_hud_archive_in(int archive, uint8_t **sarc, size_t *n, const WideHudShift *s, char *why, size_t why_n);

int wide_hud_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, const WideHudShift *s, void **out_z,
                   size_t *out_n);
int wide_hud_build_in(int archive, const void *in, size_t n, const void *zsdic, size_t zsdic_n, const WideHudShift *s,
                      void **out_z, size_t *out_n);

#endif
