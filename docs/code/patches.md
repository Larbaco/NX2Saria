# runtime/patches

The host side of the patch sets, the mods and their cheats. This page is started: it holds the notes of the code changed
since the comments rule, the mods' unbuilt code and the cheats' switches in `cheats.c`. The rest of `runtime/patches/` keeps its comments until
the area is extracted ([README.md](README.md#areas)). How the launcher builds a mod's code in:
[launcher.md](../launcher.md).

## `runtime/patches/cheats.c`

Code cheats and ExeFS patch files cannot be applied while the game runs: the game's code is lifted C, so the build
turns them into patch sets. One that an enabled mod brings and the build has not taken up stays off until the game is
built again. That build is the launcher's: Play builds an enabled mod's new code in before it starts the game
(`launcher/mods.py` `code_waiting()`, `launcher/jobs.py` `ModsRebuildJob`). The game itself starts no rebuild: the
setup that once wrote `build-cache/rebuild.txt` for it is retired, and with it the restart into that setup, its
`build-cache/rebuild.skip` and the exit sequence that ran before it.

### `report_unbuilt()`

Counts the cheats whose kind is `CHEAT_REBUILD` (code cheats new or edited since the build) and the ExeFS patch files
`runtime/patches/mods.c` `mods_exefs_pending()` finds, keeps both for the Cheats tab, and when either is not 0 logs
`cheats: N code cheats and M ExeFS patch files are not built into the game; Play from the launcher builds them in`,
once at start. `mods_exefs_pending()` still folds its files into a key, which nothing reads now.

### `cheats_init()`

`mods_rebuild` (`NX2SARIA_MODS_REBUILD`) is retired: it still has its row in `runtime/cfg/cfg.c` `metas[]`, so a file
that sets it gets no "unknown key" and the settings table stays whole, but nothing acts on it. When it is set, the
start logs once `cheats: mods_rebuild = <value> does nothing now: Play from the launcher builds code cheats and ExeFS
patches in`.

The summary line the start logs when a mod holds a cheat file ends with whether the menu shows the tab:
`cheats: <file> in N mods: N cheats, N on; ... N not supported; the menu's Cheats tab lists them` (or `the menu has no
Cheats tab` when the files hold no cheat). With no file at all the line is `cheats: no enabled mod holds
cheats/<file>; the menu has no Cheats tab`. The last thing it does is note the settings' version
(`runtime/cfg/cfg.c` `cfg_version()`), so `follow_settings()` starts from the state just read.

### `follow_settings()`

A cheat follows its setting; nothing in this file writes one. `cheats_frame()` calls it first, every presented frame:
when the settings' version moved, each code, held-button and data cheat whose switch differs from its setting
(`NX2SARIA_CHEAT_<hash>`: on for any value but empty and `0`) takes it, a code cheat's patch set is switched
(`runtime/patches/patches.c` `rt_patch_switch()`; a held-button cheat's set follows the buttons in `cheats_frame()`),
and the log says `cheats: <mod>/<name> on` or `off`. Unchanged, it costs one load of the version per frame.

This is how the menu's Cheats tab switches a cheat (`runtime/ui/cheats_tab.cpp` `tab_cheats()`): the row changes the
draft, Apply writes the setting, and the cheat switches at the next presented frame. Until Apply the game runs as
before. It replaces `cheats_set`, which switched the cheat and wrote the setting in one call, outside the draft. A
setting changed any other way (the menu test hook's `NX2SARIA_MENU_TEST_SET`) switches the cheat the same way.

Master, restore, unbuilt and unsupported cheats have no switch and are left alone.

### `cheats_unbuilt()`

The two counts `report_unbuilt()` kept, for `runtime/ui/cheats_tab.cpp` `tab_cheats()`.
