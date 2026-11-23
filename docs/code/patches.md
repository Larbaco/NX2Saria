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
once at start; a count of 1 reads `1 code cheat` or `1 ExeFS patch file` (`runtime/patches/cheat_stats.h`
`cheat_s()`). `mods_exefs_pending()` still folds its files into a key, which nothing reads now.

### `cheats_init()`

`mods_rebuild` (`NX2SARIA_MODS_REBUILD`) is retired: it still has its row in `runtime/cfg/cfg.c` `metas[]`, so a file
that sets it gets no "unknown key" and the settings table stays whole, but nothing acts on it. When it is set, the
start logs once `cheats: mods_rebuild = <value> does nothing now: Play from the launcher builds code cheats and ExeFS
patches in`.

The summary line the start logs when a mod holds a cheat file ends with whether the menu shows the tab:
`cheats: <file> in N mods: N cheats, N on; ... N not supported; the menu's Cheats tab lists them` (or `the menu has no
Cheats tab` when the files hold no cheat). Its counted nouns follow the count: `1 mod`, `1 cheat`, `1 heading`,
`1 waits for a rebuild`; the kinds that are adjectives (`1 code`, `1 data`) need none. With no file at all the line is `cheats: no enabled mod holds
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

### `wr()`

One store of the cheat VM, and where its outcome is counted (`g_wr`, the tally of the cheat being run):

- unchanged: the address already holds the value, so nothing is written and no page's protection changes;
- written: the bytes were stored. A store into main's image goes through `wr_main()`, a page at a time, a read-only
  page writable for its part of the copy only; elsewhere a frozen region thaws on the write
  (`runtime/memory.c`);
- refused: the address is 0 or cannot be read (not mapped, or the guest may not read it), the guest may not write it,
  or a page of main's image is neither readable nor writable or would not take the protection change (`wr_main()`
  returns 0; bytes of an earlier page may already be stored).

An opcode that reads through a pointer and fails (a broken pointer chain) ends the cheat's run for the frame before
any store: it is none of the three, and shows as frames without writes.

### `run_counted()`

Runs one data cheat (or a data master code) for the frame and adds the frame and its stores to the cheat's
`CheatStats` (`runtime/patches/cheat_stats.h` `cheat_stats_frame()`). When `cheat_stats_due()` says so it logs
`cheats: <mod>/<name>: N writes in N frames, N unchanged, N refused so far`: at the cheat's first write or refusal,
then at most once a minute, and only when it wrote or was refused again since its last line. A cheat that only finds
its value already in place stays silent after that. The numbers count from the start of the session and do not reset
when the cheat is switched off and on.

The tally `g_wr` is a plain static: only the thread that runs `cheats_frame()` (the present path) touches it. The
totals in `CheatStats` are added and read with atomic operations, since `cheats_exit_log()` reads them from the thread
that ends the process while frames may still be presented.

### `cheats_exit_log()`

The end of the session (`runtime/gfx/gfx_present.c` `rt_exit_steps()`, with the other session summaries and before the
log queue is flushed): one line per data cheat that is on or ran at least one frame, and per data master code,
`cheats: <mod>/<name>: N writes in N frames, N unchanged, N refused this session`. A cheat that is on and never wrote
gets its line too (`0 writes`), so a cheat that does nothing shows. Code cheats and held-button cheats are patch sets,
not stores, and get no line. A crash or a kill writes no end line; the `so far` lines are what is left then.

The lines go to the local log only. The session summary the launcher may send (`runtime/session_summary.c`
`session_summary_exit()`, [telemetry.md](../telemetry.md)) is a fixed set of fields and is not where the session's log
lines are gathered: those are the `*_exit_log()` calls of `rt_exit_steps()`. A mod's folder name and a cheat's name
are the player's data, so nothing of a cheat is added to the summary, which carries the count of mods and never their
names (`tests/cheats_tab_check.py` checks that the summary's sources do not name cheats).

### `cheats_unbuilt()`

The two counts `report_unbuilt()` kept, for `runtime/ui/cheats_tab.cpp` `tab_cheats()`.

## `runtime/patches/cheat_stats.h`

The counting and the wording of a cheat's log line, apart from `cheats.c` so that `tests/cheats_tab_check.cpp` runs
them without the game.

### `cheat_stats_due()`

True when a line is due, and then notes the time and the count: the written and refused stores differ from the count
at the last line, and it is the first line or `CHEAT_STATS_EVERY_NS` (60 s) have passed since the last. Unchanged
stores do not count as acting. The caller gives the time (`plat_now_ns()` in the game).

### `cheat_stats_line()`

The line after the `cheats: ` prefix; `when` is `so far` or `this session`. `1 write` and `1 frame` are singular.

### `cheat_s()`

The plural `s` of an English log line: empty for 1 only.
