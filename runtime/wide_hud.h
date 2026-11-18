// The HUD's corner elements at the edges of a picture wider than 16:9 (wide_hud.c), NX2SARIA_WIDE_HUD = edges, and the
// loading screen's icon in the middle 16:9 for NX2SARIA_WIDE_MENUS = center.
#ifndef WIDE_HUD_H
#define WIDE_HUD_H

#include <stddef.h>
#include <stdint.h>

// The shared interface layouts of the game, which hold the HUD (PlayerStatus_00, MapMenu_00, NumDisplay_00) and the
// loading screen (LoadingFade_00).
#define WIDE_HUD_ARCHIVE "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"
// The title screen's layouts (Title_00, TitleMenuBG_00, PaCommonBtnTitle_00): its menu backdrop is part of the menus'
// redesign (wide_menus = full).
#define WIDE_HUD_TITLE_ARCHIVE "UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs"
// The archives the edit serves, for the functions below that take one.
enum { WIDE_HUD_COMMON, WIDE_HUD_TITLE };
// The RomFS path of an archive above; NULL for a number that is not one.
const char *wide_hud_archive_path(int archive);

// How far, in units of the game's 3840x2160 layout grid, an element at a side of the middle 16:9 moves out to reach the
// same side of a picture w x h, once the UI is squeezed into that middle 16:9 (widescreen.c, set wide_hud):
// 1080 * w / h - 1920. 0 for a picture within 1% of 16:9 or narrower.
float wide_hud_out(unsigned w, unsigned h);

// What the edit does. out: the HUD's edge elements move out by that many units (wide_hud_out; 0 none,
// NX2SARIA_WIDE_HUD = center). squeeze: 16:9 over the picture's ratio, by which the loading screen's icon is narrowed
// and brought in so that it sits in the middle 16:9's corner at its own width (1 none; NX2SARIA_WIDE_MENUS = center).
// menus: the menus' layouts redesigned to fill a picture that many units wider on each side (wide_hud_out; 0 none;
// NX2SARIA_WIDE_MENUS = full). no_growth: 1 leaves out the edits that make the archive larger (the pause menu's
// backdrop drawn in pieces, wide_hud_split), with the rest of their layout's redesign (it keeps the game's menu
// layout): for a game whose resource size table cannot be raised to hold a larger archive.
typedef struct {
    float out, squeeze, menus;
    int no_growth;
} WideHudShift;

// The edits of edges mode, one per layout: what the start-up line counts (romfs.c wide_hud_overlay).
typedef struct {
    const char *layout;   // the member's name without ".buil"
    int panes, keys;      // panes whose translate x moves; animation keys that move (curves' value spans not counted)
} WideHudCount;
// Fills out[0..max) and returns the number of layouts edges mode edits.
int wide_hud_counts(WideHudCount *out, int max);
// The same for the HUD's edges (menus 0) or for the menus' redesign (menus 1: panes changed in any way, keys moved).
int wide_hud_counts_of(int menus, WideHudCount *out, int max);

// One layout file p[0..n) (a BFFH member named `member`, "PlayerStatus_00.buil" and so on) with the shift's edit, in
// place: its edge elements moved out by s->out units, or (LoadingFade_00.buil) its icon squeezed by s->squeeze. Every
// field is found and checked before any is written (each pane's x, and a squeezed pane's scale x, is the game's 1.1.1
// value and no animation keys it; its ancestors have scale 1 and no rotation, and a squeezed pane's sit at x 0 with no
// animation moving or scaling them; each animation curve has the keys this expects), and read again after. Returns the
// number of fields changed; 0 for a member the shift does not edit or one edited so already; -1 when it is not the
// layout this expects (why says what differs; the file is left as it was). report (may be NULL): what moved,
// "N_ScaleL_00 x -1920 -> -2580, ...", cut to report_n.
int wide_hud_lay_out(uint8_t *p, size_t n, const char *member, const WideHudShift *s, char *report, size_t report_n,
                     char *why, size_t why_n);

// The pause menu's backdrop drawn in pieces (wide_menus = full): one layout file p[0..n) (PlusMenu_00.buil) with its
// picture P_MenuBg_00 cut into pieces that each move with the column of the pages above it, the pieces after the first
// new panes inserted after it, the file grown to hold them (wide_hud.c split_layout). *out (malloc'd) and *out_n.
// Returns the number of panes added; 0 for a member the shift splits nothing in or one split so already (*out NULL);
// -1 when it is not the layout this expects (why; nothing written). report (may be NULL): "P_MenuBg_00 in 7 pieces".
int wide_hud_split(const uint8_t *p, size_t n, const char *member, const WideHudShift *s, uint8_t **out, size_t *out_n,
                   char *report, size_t report_n, char *why, size_t why_n);
// The 64-bit hash a layout keeps beside each name in its tables of panes, pictures and materials, which the game's
// lookup by name compares: CityHash64 of the name's bytes. 0 for a name longer than 32 bytes (no layout has one).
uint64_t wide_hud_name_hash(const char *name);

// The same on the decompressed Common layout archive (a SARC, *sarc malloc'd, *n its size) for every layout the shift
// edits, on every copy of each: all of them change or none, the edits in place first, then the splits. A split grows
// its layout, so the archive is then laid out again (sarc.c sarc_rebuild): *sarc is replaced (the old one freed) and
// *n follows. Logs one line per layout that changed ("wide hud: PlayerStatus_00 (4 copies, 16 fields): N_ScaleL_00 x
// -1920 -> -2580, ...", "wide menus: PlusMenu_00 (4 copies, 340 fields, P_MenuBg_00 in 7 pieces, 24 new panes): ...",
// "widescreen: menus: LoadingFade_00 (...)"). Returns the count of fields changed and panes added over all copies, 0
// when everything was done already, -1 when any layout is not the one this expects (the archive as it was, but for
// running out of memory during a split, which may leave the edits in place made).
int wide_hud_archive(uint8_t **sarc, size_t *n, const WideHudShift *s, char *why, size_t why_n);
// The same for the archive WIDE_HUD_COMMON or WIDE_HUD_TITLE names: only that archive's layouts are edited (the Title
// archive's TitleMenuBG_00 only when s->menus > 0). wide_hud_archive is the Common one.
int wide_hud_archive_in(int archive, uint8_t **sarc, size_t *n, const WideHudShift *s, char *why, size_t why_n);

// The Common layout archive's zstd file in[0..n) with the shift's edits, compressed again with
// the dictionary of Pack/ZsDic.pack.zs (zsdic) the original names: *out_z (malloc'd) and *out_n. Its frame records
// the decompressed size, larger than the original's when a split grew the archive: the game sizes the buffer it reads
// the archive into from the resource size table, whose entry must then grow too (romfs.c wide_hud_overlay). Returns 1
// when *out_z was made, 0 when there is nothing to serve, -1 when it cannot be (logged; the archive is served as it
// is).
int wide_hud_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, const WideHudShift *s, void **out_z,
                   size_t *out_n);
// The same for the archive WIDE_HUD_COMMON or WIDE_HUD_TITLE names (in is that archive's file). Returns 0, without
// reading it, when the shift edits nothing in that archive (the Title one with s->menus 0). wide_hud_build is the Common one.
int wide_hud_build_in(int archive, const void *in, size_t n, const void *zsdic, size_t zsdic_n, const WideHudShift *s,
                      void **out_z, size_t *out_n);

#endif
