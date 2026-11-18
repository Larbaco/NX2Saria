// The HUD's corner elements at the edges of a picture wider than 16:9 (wide_hud.c), NX2SARIA_WIDE_HUD = edges, and the
// loading screen's icon in the middle 16:9 for NX2SARIA_WIDE_MENUS = center.
#ifndef WIDE_HUD_H
#define WIDE_HUD_H

#include <stddef.h>
#include <stdint.h>

// The shared interface layouts of the game, which hold the HUD (PlayerStatus_00, MapMenu_00, NumDisplay_00) and the
// loading screen (LoadingFade_00).
#define WIDE_HUD_ARCHIVE "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"

// How far, in units of the game's 3840x2160 layout grid, an element at a side of the middle 16:9 moves out to reach the
// same side of a picture w x h, once the UI is squeezed into that middle 16:9 (widescreen.c, set wide_hud):
// 1080 * w / h - 1920. 0 for a picture within 1% of 16:9 or narrower.
float wide_hud_out(unsigned w, unsigned h);

// What the edit does. out: the HUD's edge elements move out by that many units (wide_hud_out; 0 none,
// NX2SARIA_WIDE_HUD = center). squeeze: 16:9 over the picture's ratio, by which the loading screen's icon is narrowed
// and brought in so that it sits in the middle 16:9's corner at its own width (1 none; NX2SARIA_WIDE_MENUS = center).
// menus: the menus' layouts redesigned to fill a picture that many units wider on each side (wide_hud_out; 0 none;
// NX2SARIA_WIDE_MENUS = full).
typedef struct {
    float out, squeeze, menus;
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

// The same on the decompressed Common layout archive (a SARC) for every layout the shift edits, on every copy of each:
// all of them change or none. Logs one line per layout that moved ("wide hud: PlayerStatus_00 (4 copies, 16 fields):
// N_ScaleL_00 x -1920 -> -2580, ...", "widescreen: menus: LoadingFade_00 (...)").
// Returns the count of fields changed over all copies, 0 when everything was moved already, -1 when any layout is not
// the one this expects.
int wide_hud_archive(uint8_t *sarc, size_t n, const WideHudShift *s, char *why, size_t why_n);

// The Common layout archive's zstd file in[0..n) with the shift's edits, compressed again with
// the dictionary of Pack/ZsDic.pack.zs (zsdic) the original names: *out_z (malloc'd) and *out_n. Returns 1 when *out_z
// was made, 0 when there is nothing to serve, -1 when it cannot be (logged; the archive is served as it is).
int wide_hud_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, const WideHudShift *s, void **out_z,
                   size_t *out_n);

#endif
