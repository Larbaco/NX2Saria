# runtime/ui

The in-game menu. This page is started: it holds the notes of the code changed since the comments rule. The rest of
`runtime/ui/` keeps its comments until the area is extracted ([README.md](README.md#areas)).

## `runtime/ui/cheats_tab.cpp`

### `tab_cheats()`

The Cheats tab: a heading per mod, a row per cheat of its cheat file and a line per group label, in the order
`runtime/patches/cheats.c` `cheats_info()` gives. The menu shows the tab only while there are cheats
(`runtime/ui/menu.cpp` `tabs_shown()`).

A row is drawn by `runtime/ui/option_rows.cpp` `opt_toggle_text()`, so A, left and right flip it and the mouse flips it
on the two framed arrows, as on every other on/off row. Only the code, held-button and data cheats can be switched; the
other kinds are greyed out with the reason.

A flip changes the draft and nothing else: the row hands `opt_change()` the cheat's own setting
(`CheatInfo.key`, `NX2SARIA_CHEAT_<hash>`), the cheat's name as the line's label and On / Off as its from and to. The
row shows the draft's value (`opt_get()`), so a cheat flipped and not yet applied reads as it will be. Apply writes the
setting and saves the file; Discard drops the change; the cheat itself switches when its setting changes
(`runtime/patches/cheats.c` `cheats_frame()`). The tab calls nothing that writes a setting or switches a patch set.

The value a flip writes: `1` to switch on; to switch off, `0` when the settings hold a line for the cheat and unset
(`""`) when they hold none. So a cheat switched on and off again before Apply is back at the value it had, and the
change leaves the draft instead of adding a `cheat_<hash> = 0` line to the file.

When an enabled mod has code cheats or ExeFS patch files the build has not taken up
(`runtime/patches/cheats.c` `cheats_unbuilt()`), the tab's first line says how many and that Play from the launcher
builds them in, the words of the log's line (`UI_MENU_CHEATS_UNBUILT`, two `%s`: the code cheats, then the ExeFS patch
files, each a counted noun from `runtime/ui/ui_text.c` `menu_count()` with `UI_MENU_COUNT_CODE_CHEATS` and
`UI_MENU_COUNT_EXEFS_FILES`; English, Russian, Brazilian Portuguese and Simplified Chinese). A mod's heading counts
its cheats the same way (`UI_MENU_CHEATS_MOD`, `UI_MENU_COUNT_CHEATS`). The Russian, Portuguese and Chinese sentences
say Play in the launcher, which is how those languages put it. There is no button: the game no longer restarts into a
rebuild.

## `runtime/ui/ui_text.c`

### `ui_text_count()`

A counted noun in a language's own plural form. Its catalog text holds the forms with `|` between them, each a printf
format with one `%d`: `"%d code cheat|%d code cheats"`. `ui_text_plural_form()` picks one, and a text with fewer forms
than the rule asks for gives its last one, so Simplified Chinese writes a single form. A language without its own text
for the id shows the English text by the English rule. `menu_count()` is the same in the menu language. The menu had
no plural texts before these; a new counted noun gets an id of its own and its forms in each menu language.

### `ui_text_plural_form()`

- Russian, three forms: 0 for a count ending in 1 but not in 11 (1, 21, 101), 1 for one ending in 2 to 4 but not in 12
  to 14 (2, 3, 4, 22), 2 for the rest (0, 5 to 20, 11 to 14).
- Every other language, two forms: 0 for 1, else 1. Brazilian Portuguese writes 0 with the plural here (`0 cheats`).

## `runtime/ui/menu.cpp`

### `tabs_shown()`

How many of the tabs the menu shows: all of them while `runtime/patches/cheats.c` `cheats_count()` is above 0, else
the ones before Cheats, so a player without cheat mods sees Screen, Game, Sound, Controls and Amiibo. Cheats is the
last tab of the enum for that reason: the tabs shown are the first `tabs_shown()` of it. The count does not change
while the game runs (the cheat files are read once, at start, before the first frame). `layout_menu()` steps through
that many tabs with L / R and Q / E and draws that many, and the test hook in `overlay_frame()` shoots that many pages;
nothing else may use `TAB_COUNT` as a count of tabs shown (`tests/cheats_tab_check.py`).

The Cheats tab's name is among the texts whose glyphs the title face gets in the CJK languages (`fonts_up()`).

### `test_apply_sets()`

Two pseudo-keys of `NX2SARIA_MENU_TEST_SET` beside `SAVE` and `FOCUS`, for a run with no input that must show what a
draft does:

- `LEAVE=1`: with lines in the draft, asks to leave the menu as F1 does, so the *Apply your changes?* box opens with
  the draft's lines and the page's shot shows it. The log has `menu: box "Apply your changes?" opened: N line(s)` and a
  `menu: box   <label>: <from> -> <to>` line each. With nothing in the draft it only logs
  `menu: test hook: LEAVE with nothing in the draft: no box to open`: leaving would close the menu and the hook's
  pages would stop.
- `APPLY=1`: `runtime/ui/option_rows.cpp` `opt_draft_apply()` as the Restart button calls it, without the boxes
  (every key is written and saved, none waits for *Keep*), and the menu stays open. The log has `menu: test hook:
  Apply of the draft's N change(s) saved` (or `failed`) after the draft's own lines. It writes the settings file.

A cheat's row takes the hook's clicks like any stepper: `NX2SARIA_MENU_TEST_CLICK=CHEAT_<HASH>:r` while the Cheats page
is shown.

## `runtime/ui/option_rows.cpp`

### `is_staged()`

A key without a row in the settings table is a preview key (the bindings), except a cheat's (`CHEAT_<hash>`): it waits
in the draft until Apply. A preview would have to switch the cheat for the player to judge it, and a cheat writes the
game's memory or switches a patch set, which Discard could not take back.

### `opt_draft_apply()`

A staged key may have no table row (a cheat's), so the "takes effect after a restart" part of its log line asks the
row only when there is one.
