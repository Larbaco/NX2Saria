# runtime/patches

The host side of the patch sets, the mods and their cheats: the table of the sets and the patch page, the host
functions the sets' sites call, the edits of game files that go with them (the interface layouts, the sequence graphs,
a save file as it is read), and the scan of the mods folder with the cheat VM. The sets themselves are data,
`recomp/patches-v111.txt` and the other `recomp/patches-*.txt`, which the generator reads (`recomp/patches.py`;
[recomp.md](recomp.md)); 1.1.1's are listed in [patches-v111.md](../patches-v111.md). The features these files serve
have their own docs, which say what each is for and what was measured: [wide-hud.md](../wide-hud.md) (the HUD and the
menus on a wide picture), [heap-retry.md](../heap-retry.md), [changebank-hold.md](../changebank-hold.md),
[boot-to-map.md](../boot-to-map.md), [amiibo.md](../amiibo.md), and [controls.md](../controls.md) for the sprint
button, the text speed and the button prompts. How the launcher builds a mod's code in:
[launcher.md](../launcher.md). The log lines: [logging.md](../logging.md).

`main+0x...` is an address in the game's main module. Unless a note says otherwise, the addresses, the layouts and the
values are Ocarina of Time 1.1.1's, the only version these sets exist for.

## `runtime/patches/patches.c`

Code patch sets (`recomp/patches.py`, `gen.py --patches`). The generator lifts both the original and the replacement
instructions of every patched site, and `rt_patch_on[set]` picks one when the site runs, so a set is switched at start
without touching code, and also while the game runs (`rt_patch_switch()`: the frame-limit setting of the menu). The
sets, their environment switches and defaults, and the patch page come from the generated `patches.h`; without it (C
generated without `--patches`) there are no sets and every site runs the original. `rt_patch_on` is read by the
generated C at every patched site.

The patch page is the first 4 KiB page after main's image, inside the module window `runtime/memory.c` reserved:
replacement code reaches it with one `adrp`. Its layout belongs to the sets that use it (`runtime/pacing/frame_rate.c`:
+0 the 30 Hz ratio, +4 the frame step in seconds, +8 the step in 60 Hz ticks). Probe modes never call
`rt_patches_init()`: every set stays off.

### The patch page's host slots

The host functions are registered here, in the file that has the game version's page address, or by the file that
owns them. A slot registered twice ends the start (`runtime/dispatch.c` `rt_register_host()`).

| Slot | Set and site | Host function |
|---|---|---|
| +0xf70 | `freecam`: the debug menu's free camera | `runtime/diag/debug_tools.c` `dbg_freecam_host()` |
| +0xf60 | `ostep`: the time of day's site | `runtime/pacing/frame_rate.c` |
| +0xf50 | the water renderer's site | `runtime/pacing/frame_rate.c` |
| +0xf40, +0xf30 | `sprint`: its two sites | `runtime/patches/sprint.c` |
| +0xf20 | `owater`: the surfaces' fixed-step sites | `runtime/pacing/frame_rate.c` |
| +0xf10, +0xf00 | `osound`: the sound job's two sites | `runtime/pacing/frame_rate.c` |
| +0xef0 | `wide_bars`: the screens' setup site | `runtime/patches/wide_bars.c` `wide_bars_screen_host()` |
| +0xee0 | `menu_mouse` and `quit_menu`: every interface screen's update site | `screens_host()` here, which calls `runtime/ui/menu_mouse.c` `menu_mouse_screen_host()` and `runtime/ui/quit_menu.c` `quit_menu_screen_host()` |
| +0xed0, +0xec0 | `heap_retry` | `runtime/patches/heap_retry.c` |
| +0xeb0 | the debug menu's field of view | `runtime/diag/debug_tools.c` |
| +0xea0, +0xe80 | the horse's jump and Gohma's turn | `runtime/pacing/frame_rate.c` |
| +0xe90 | `ozora`: single-frame animation loops | `runtime/patches/animation_loop.c` `animation_loop_host()` |
| +0xe70 | `obgm`: the battle music's change flags | `runtime/patches/battle_music.c` `battle_music_flags_host()` |
| +0xe60 | the voice language's callback | `runtime/patches/voice_language.c` |
| +0xe50 | the balloon's callback | `runtime/patches/widescreen.c` `wide_balloon_host()` |
| +0xe40 | the wide lens | `runtime/patches/widescreen.c` |
| +0xe30 | `changebank_hold` | `runtime/patches/changebank_hold.c` |
| +0xe20, +0xe10 | `quit_menu`: the confirmation window's button count and label | `runtime/ui/quit_menu.c` `quit_menu_window_count_host()`, `quit_menu_window_label_host()` |
| +0xe00 | `camera_clearance` | `runtime/patches/camera_clearance.c` |

The slots from +0xf50 to +0xff0 not named here belong to other sets (`runtime/input/mouse_direct.c`,
`runtime/hle/ov_zstd.c`).

### `rt_patches_init()`

A set with a row in the settings table takes its default from there (`cfg_meta()`, `cfg_bool()`); the generated default
serves the rest.

## `runtime/patches/cheats.h`

Cheats of the mods' Atmosphere cheat files: `mods\<mod>\cheats\<build id>.txt` of each enabled mod, switched from the
menu's Cheats tab. A cheat that writes the game's instructions is a code patch set that `tools/cheats.py` built into
the game (switched with `rt_patch_switch()`); one that writes data runs in the cheat VM every presented frame.

`CheatKind`:

| Kind | Meaning |
|---|---|
| `CHEAT_HEADING` | no opcodes: a title or a group label |
| `CHEAT_CODE` | a patch set |
| `CHEAT_KEY` | a patch set on while the buttons are held |
| `CHEAT_DATA` | the cheat VM |
| `CHEAT_MASTER` | `{master}`: always on |
| `CHEAT_RESTORE` | writes the original instructions back: nothing to do |
| `CHEAT_REBUILD` | writes instructions, but the game was built without this version of it |
| `CHEAT_UNSUPPORTED` | `note` says why |

`CheatInfo`: `name` is UTF-8, as in the file; `mod` the mod's folder name (cheats come grouped by mod, in the Mods
tab's order); `note` why it is unsupported ("" otherwise); `buttons` the HidNpadButton mask of a `CHEAT_KEY`; `key` its
setting, `NX2SARIA_CHEAT_<hash of "<mod>/<name>">`.

### `cheats_init()`, `cheats_on()`, `cheats_file()`, `cheats_frame()`, `cheats_buttons()`

- `cheats_init()` runs after `rt_patches_init()`: it reads the mods' files and their build status.
- `cheats_on()`: the player's switch (1 for a master cheat).
- `cheats_file()`: "<build id>.txt", the name the files have for this game version.
- `cheats_frame()`: every presented frame (the present path): the data cheats' VM and the button state of `CHEAT_KEY`
  cheats.
- `cheats_buttons()`: the button names of a mask ("ZL + Right stick left"), into `buf`.

## `runtime/patches/cheats.c`

Each enabled mod the port can use (`runtime/patches/mods.h`) may hold `<build id>.txt` in its cheats folder, the build
id the first 8 bytes of main's NSO build id, as Atmosphere and the emulators name it; the file name's case does not
matter. Each cheat's switch is the setting `NX2SARIA_CHEAT_<hash>` (`cheat_<hash>` under `[cheats]` in the settings
file), the hash FNV-1a 32 of "<mod folder name>/<cheat name>", so two mods' cheats of one name keep their own switches.

Code cheats (opcode 0 writes into main's code) cannot be applied here: the game's code is lifted C. `tools/cheats.py`
turns the code cheats of every mod into patch sets `cheat_<hash>` when the game is built and writes
`build-cache\cheats.status` next to the game's executable, one line per cheat: name hash, content hash, kind, buttons,
note (`tools/cheats.py` has the format). A cheat whose status line is missing or has another content hash was added or
edited after the build: a data cheat runs anyway, a code cheat waits for the next build.

Data cheats run in a VM after Atmosphere's dmnt cheat VM (opcodes 0-A; the 0xC.. and 0xF.. extensions are not
supported), every presented frame while their switch is on, with registers cleared before each cheat. Memory is the
guest's, mapped at its own addresses: main's image is written with its pages made writable for the write (read-only
data included, as dmnt writes through the debugger), everything else after `mem_query` says the guest may read or
write it. Buttons are `input_poll()`'s HidNpadButton bits, the same as the cheat format's key masks; they read 0 while
the menu is open.

In `Cheat`: `dup` is 1 for the first cheat of its name in its file, 2 for the next and so on; `content` is FNV-1a 32 of
its words as `tools/cheats.py` writes them; `set` its patch set, -1 for none. `g_vm` says some cheat needs
`cheats_frame()`; `g_file` is "<build id>.txt"; `g_files` the mods whose file was read; `g_main`, `g_main_size` and
`g_code_end` are main's image and its code with the padding up to `.rodata`.

Code cheats and ExeFS patch files cannot be applied while the game runs: the game's code is lifted C, so the build
turns them into patch sets. One that an enabled mod brings and the build has not taken up stays off until the game is
built again. That build is the launcher's: Play builds an enabled mod's new code in before it starts the game
(`launcher/mods.py` `code_waiting()`, `launcher/jobs.py` `ModsRebuildJob`). The game itself starts no rebuild: the
setup that once wrote `build-cache/rebuild.txt` for it is retired, and with it the restart into that setup, its
`build-cache/rebuild.skip` and the exit sequence that ran before it.

### `exe_dir()`, `op_len()`, `vm_unsupported()`, `classify()`

- `exe_dir()`: the folder of the executable, with a trailing separator.
- `op_len()`: the words of the opcode at `w` (`left` words remain), 0 when it is cut short.
- `vm_unsupported()`: NULL when the VM runs the opcode, else why not (`tools/cheats.py` `vm_ok()`).
- `classify()`: the kind of a cheat the build did not see: heading, data, rebuild (it writes main's code) or
  unsupported.

### `in_main()`, `wr_main()`

`in_main()` is written without `a + n`: an address near 2^64 (a pointer chain that read -1) would wrap the sum and
pass. `wr_main()` writes `n` bytes into main's image a page at a time: a read-only page is writable for its part of the
copy only.

### `skip()`, `run()`

`skip()`: from `i` (after a failed condition, or an else reached while running), the opcode to continue at: after the
matching else (`else_ok`) or end. In `arith()` operation 9 is a move.

The opcodes `run()` executes:

| Opcode | Form | Does |
|---|---|---|
| 0 | `0TMR00AA AAAAAAAA V..` | store a value at region M + A + register R |
| 1 | `1TMC00AA AAAAAAAA V..` | if (region M + A) <cond C> V |
| 2 | `20000000`, `21000000` | end, else: an else reached while running skips to the end |
| 3 | `300R0000 VVVVVVVV`, `310R0000` | loop start, loop end |
| 4 | `400R0000 V V` | register R = V |
| 5 | `5TMRI0AA AAAAAAAA` | R = (I ? R : region M) + A, read; a broken pointer chain ends the run: nothing more |
| 6 | `6T0RIor0 V V` | store V at R (+ register r); I: R += width |
| 7 | `7T0RC000 VVVVVVVV` | R = R <op C> V |
| 8 | `8kkkkkkk` | if the buttons are held |
| 9 | `9TCRS0s0` / `9TCRS100 V..` | R = S <op C> (s or V) |
| A | `ATSRIOxa (aaaaaaaa)` | store register S at an address of type O |

### `read_text()`, `parse()`, `find_file()`, `status_of()`, `plain_name()`

- `read_text()` drops a UTF-8 BOM.
- `parse()` appends the cheats of one mod's file; `mod` is the mod's folder name (kept). A repeated name gets "#2",
  "#3" and so on (`tools/cheats.py` `parse()`).
- `find_file()`: the file `g_file` in `folder`, its name in any case, into `out`; 0 when there is none.
- `status_of()`: the status line of `key`, its fields split in place; 0 when none.
- `plain_name()`: the menu's font has no numero sign (U+2116), which cheat names often start with: they show "No"
  instead. It runs after the hashes are made, which read the file's bytes.

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

The mods are read in their load order, top first (`mods_order` in the settings file, which the launcher's Load Order
writes; the game's menu has no Mods tab now); `path` holds a mod's cheats folder (`ModInfo.cheats`) plus the
file. A data master code runs in the VM, always on. A held-button cheat's patch set starts off: it is on while the
buttons are held. `g_vm` is set when a held-button cheat, a data cheat or a data master code exists.

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

### `cheats_frame()`

Nothing runs before frame 60: the game polls its input (and starts SDL) before this. The buttons `input_poll()` gives
read 0 while the menu is open.

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

## `runtime/patches/mods.h`

Player mods: one folder per mod under `mods/` next to the game's executable (`NX2SARIA_MODS_DIR`; a relative one is
under that folder too), unpacked as emulators take them: `<mod>/romfs`, `<mod>/exefs` and `<mod>/cheats`, the
Atmosphere layout (`atmosphere/contents/<title id>/romfs`, `atmosphere/exefs_patches/<name>/*.ips`) or a mod manager's
merged output (`romfs` or `romfslite`, `exefs`, `cheats`). ExeFS patch files (`.ips`, `.pchtxt`) for the game's main
are patch sets the build makes (`tools/exefs_patches.py`), switched with their mod (`mods_exefs_switch()`). The list is
scanned once at start (`runtime/hle/romfs.c` `overlay_init()`); its order and switches live in the settings file's
`[mods]` (`NX2SARIA_MODS_ORDER`: names top first, separated by '|'; `NX2SARIA_MODS_DISABLED`: names switched off). The
top mod wins a file two mods replace.

`MOD_ROMFS` (1), `MOD_EXEFS` (2), `MOD_SUBSDK` (4) and `MOD_CHEATS` (8) are the bits of `ModInfo.kind`. `ModProblem`:

| Problem | Meaning |
|---|---|
| `MOD_EMPTY` | no RomFS file, no ExeFS patch, no cheat file |
| `MOD_PLUGIN` | has code modules (`exefs/subsdk*`, a replaced `exefs/main`): the port has no module loader, so the whole mod stays off (its RomFS files may expect that code) |
| `MOD_EXEFS_LATER` | has ExeFS patches for this game's main that the game was built without: the launcher's Play builds them in; the whole mod stays off until then |
| `MOD_EXEFS_UNSUPPORTED` | an ExeFS patch file the port cannot apply (`note` says why): the whole mod stays off |
| `MOD_EXEFS_OTHER` | only ExeFS patches, all for another game version |
| `MOD_BAD_NAME` | its folder name holds '|', the separator of the order keys |

`ModInfo`: `name` is the folder name, UTF-8; `romfs`, `exefs` and `cheats` the UTF-8 paths of its RomFS root, ExeFS
folder and cheats folder ("" none); `start_enabled` is `enabled` at the scan: what `runtime/hle/romfs.c`,
`runtime/patches/cheats.c` and the ExeFS patch sets go by this run; `replaced`, `added` and `overridden` are
`romfs.c`'s: the files served (replacing or added) and the files a higher mod serves; `other_version` the files for
another game version (RomFS `*.Product.<n>.*`, ExeFS patches for another build), not used; `overridden_by` the first
higher mod that took one of its files; `note` why, for `MOD_EXEFS_UNSUPPORTED`; `pending` and `pending_key` the ExeFS
patch files the build lacks and the FNV-1a of their names and contents; `sets[16]` the built patch sets `mod_<hash>` of
its ExeFS patch files; `overrides` the port's own patch sets its patches replace, comma separated.

### `mods_scan()`, `mods_dir()`, `mods_at()`, `mods_set_enabled()`, `mods_move()`, `mods_changed()`

- `mods_scan()` runs once; later calls return at once.
- `mods_dir()`: the folder scanned (default: `<dir of the executable>/mods`).
- `mods_at()`: 0 is the top of the order; NULL out of range.
- `mods_set_enabled()` and `mods_move()` are the menu's edits (present path). They change the list `mods_at()` shows and
  write `NX2SARIA_MODS_ORDER` and `NX2SARIA_MODS_DISABLED` with `cfg_set`; `romfs.c` used the start state, so they apply
  at the next start. `mods_move()`: -1 up, +1 down; nothing at the ends.
- `mods_changed()`: 1 when the order or a switch differs from the start.

### `mods_exefs_switch()`, `mods_exefs_pending()`, `mods_main_build_id()`

- `mods_exefs_switch()`, after `rt_patches_init()`: the patch sets of the enabled mods' ExeFS patches go on, the
  port's own sets they replace off.
- `mods_exefs_pending()`: the ExeFS patch files of the enabled mods that the build lacks, their FNV-1a mixed into
  `*key`. `runtime/patches/cheats.c` `report_unbuilt()` counts them; the key is no longer read.
- `mods_main_build_id()`: the hex of the game main's build id (64 digits), "" when `exefs/main` cannot be read.

## `runtime/patches/mods.c`

The scan of the mods folder, its order and switches.

One subfolder per mod, as emulators take them. The RomFS root is the first of `<mod>/romfs`, `<mod>/romfslite` (a mod
manager's name for it when it bypasses Atmosphere's RomFS build), `<mod>/atmosphere/contents/<title>/romfs`,
`<mod>/contents/<title>/romfs` and `<mod>/<title>/romfs` that exists; the ExeFS and cheats folders likewise. `TITLE`
(`0400486024DEC000`) is the game's title id, which names a mod's Atmosphere folders. A mod is listed whatever it holds;
what the port cannot use is its problem, shown in the menu and logged, and never stops the start.
`runtime/hle/romfs.c` serves the RomFS files of the enabled mods, the top one winning a path.

The order and the switches are keys of the settings file (`runtime/cfg/cfg.c`): `NX2SARIA_MODS_ORDER` lists names top
first, separated by '|' (a Windows file name cannot hold '|'; a folder whose name does on Linux is listed as unusable);
a folder it does not name goes below the named ones, by name. `NX2SARIA_MODS_DISABLED` lists the switched-off names. The
menu changes the list in place and writes both keys; the scan's state is kept to tell the menu a restart is due
(`start_order`: the names in the scan's order, for `mods_changed()`). `scanned` says the scan ended: `mods_in_use()` may
then read the list from any thread. At most `MAX_MODS` (512).

ExeFS patch files: an `.ips` or `.pchtxt` file in the exefs folder or in `atmosphere/exefs_patches/<name>/` is for the
game when its build id is main's (an IPS file's name, a pchtxt file's `@nsobid` line); others are another version's and
counted. The build turns every mod's files into patch sets and writes `build-cache/exefs.status` next to the game's
executable (`tools/exefs_patches.py` has the format): a file whose line is missing or has another content hash waits for
the next build (the launcher's Play), and its mod stays off until then.

### `has_file_at()`, `find_root()`, `exefs_cb()`, `cheats_cb()`, `listed()`, `list_pos()`

- `has_file_at()`: 1 when a file lies anywhere under `path`. Folders nest at most `MOD_DEPTH` (32) deep below it: each
  level costs a stack frame, and the RomFS has no deeper paths.
- `find_root()`: the first of the emulator and Atmosphere places of `<mod>/<part>` that is a folder, into `out` ("" for
  none).
- `exefs_cb()`: what the ExeFS folder holds: `MOD_EXEFS` for patches (`.ips`, `.pchtxt`), `MOD_SUBSDK` for code modules
  (`subsdk*` plugins, a replaced `main`). `main.npdm` alone is the process's metadata, not code: it counts as neither.
- `cheats_cb()`: `MOD_CHEATS` when the cheats folder holds an Atmosphere cheat file (`<build id>.txt`).
- `listed()`: 1 when `name` is one of the '|'-separated names of `list`. `list_pos()`: its position in the list, or -1.
  `order_list` is `NX2SARIA_MODS_ORDER` while sorting.

### `same_build()`, `pchtxt_build()`, `exefs_status()`, `status_line()`

- `same_build()`: 1 when `hex` (any case; trailing zeros left out or not) names main's build id; also when main's is
  unknown.
- `pchtxt_build()`: the build id of a pchtxt file as `tools/exefs_patches.py` `parse_pchtxt()` reads it: the last
  `@nsobid-<hex>` line before an `@stop` line, a UTF-8 BOM, blanks around it and a `//` comment after it left out. 1
  when the file names one; `*len` is 0 when that line holds anything but hex digits (no build id, so for no main).
- `exefs_status()`: `build-cache/exefs.status` next to the executable, read once; "" when there is none.
- `status_line()`: the status line of `key`: its kind, built-in sets and note, NUL-terminated copies; 0 when none. The
  copy is of the line after the 18 bytes compared, not into the next line.

### `exefs_file_cb()`, `exefs_patches_cb()`, `exefs_check()`, `scan_cb()`

- `exefs_file_cb()`: one file of an exefs folder: its build, then its status line. Its context holds the folder listed
  and its path in the mod.
- `exefs_patches_cb()`: `atmosphere/exefs_patches/<name>/` of a mod: each `<name>` folder's files.
- `exefs_check()`: the ExeFS patch files of mod `m` (folder `path`): 1 when one cannot be applied.
- `scan_cb()`: one mod folder of the scan. A symbolic link or junction to a folder is a mod, as the build's tools see
  it (`tools/mod_files.py` `cheat_files()`). A mod whose code part the port cannot use is off as a whole: its RomFS files
  may expect that code.

### `mods_scan()`, `write_keys()`

`mods_scan()`: a relative `mods_dir` is under the folder of the executable, as the default is, whatever folder the game
started in.

`write_keys()`: `NX2SARIA_MODS_ORDER` as every listed name top first; `NX2SARIA_MODS_DISABLED` as the switched-off
names. A name holding '|' would split into two; it is left out of both (its mod is unusable anyway).

## `runtime/patches/mods_probe.c`

`--probe-mods`: the mods folder scan, its order and switches. It builds a mods folder of its own next to the
executable, scans it and removes it. It needs no game files.

### `put()`, `put_text()`, `dir_link()`, `remove_cb()`, `ends_with()`

- `put()`: `root/rel` as a file of a few bytes (`rel` ending in '/': a folder), with every folder above it.
  `put_text()`: `root/rel` as a file holding text.
- `dir_link()`: `link` (a path) as a symbolic link to the folder `target`; 0 when the system refuses (Windows without
  developer mode).
- `remove_cb()`: a link is never followed.
- `ends_with()`: `s` ends in `tail`, either path separator matching either.

### `probe_mods()`

- An IPS file for this game's main (any file counts as one when main cannot be read), in Atmosphere's layout.
- pchtxt files for main as `tools/exefs_patches.py` reads them: a BOM and an indented build id line (N); a build id line
  with more than hex on it (O) and one after `@stop` (P) name no build, so another version's.
- A link to a folder outside the mods folder is a mod (Q); left out when the system cannot make one. Without the link Q
  is missing: the names after it move up one.
- `NX2SARIA_MODS_DIR` is set to `mods-probe`, relative: under the folder of the executable.
- The ExeFS patch files waiting for a build follow the mods' start state, wherever the menu moved them: M and N wait
  (both on at the start); M moved onto C's place (off at the start) and switched off still waits.

## `runtime/patches/widescreen.c`

Aspect ratios other than 16:9: wider pictures keep the vertical camera angle (Hor+); taller pictures keep the
horizontal view (Vert+) by opening the vertical angle.

The game presents a 2560x1440 picture at every console mode and renders its scene at 1920x1080, which its upscaler
takes to the picture's size; both sizes are constants in its code. The patch sets of `recomp/patches-v111.txt` read
them from the patch page instead (`wide_out`: the picture, `wide_scene`: the scene), `wide_aspect` replaces a 16:9
ratio of sead's perspective projection (every 16:9 camera of the game) with the picture's, `wide_hud` keeps the HUD and
menus in their 16:9 shape in the middle of the picture (the UI's full-screen projection squeezed on the changed axis),
`wide_movie` fills the picture height and `wide_save` keeps the middle 16:9. At 16:10 or 3:2, the height stays at
1440/1080 and the width shrinks; above 1440 lines, both dimensions grow with the selected height. The present scales the
picture to the window keeping its shape (`runtime/gfx/gfx_present.c`, `NX2SARIA_ASPECT=fit`).

Setting `NX2SARIA_GAME_ASPECT`, read at start: "16:9" (the game's own), "window" (the window, or the monitor in
fullscreen), or a fixed shape "16:10", "3:2", "21:9" (64:27), "32:9". A shape within 1% of 16:9 uses the game's own
ratio. The page's layout is described at the top of `recomp/patches-v111.txt`.

| Constant | Value | Meaning |
|---|---|---|
| `GAME_W`, `GAME_H` | 2560, 1440 | the picture, `main+0x1202ee0` and `main+0x31f39f4` |
| `GAME_DTA_KIB` | 312320.0 | the texture heap made for that picture (`wide_heap_extra_kib()`) |
| `SAVE_W`, `SAVE_H` | 543, 306 | the width and height of the target a save's picture is drawn into (`save_extra()`) |
| `PICTURE_MIN_H` | 1080 | the lowest picture height |
| `RES_FIXED_AFTER` | 120 | presents after the first before the fixed share goes on |
| `PLAIN_SHARE` | 0.75 | the scene's share without DLSS or FSR 3.1 |

### `screen_size()`, `wanted_ratio()`, `picture_height()`, `picture_size()`

- `screen_size()`: the window's size, or the monitor's in fullscreen; 0 when unknown.
- `wanted_ratio()`: the shape the setting asks for as width / height; 0 for the game's own.
- `picture_height()`: `NX2SARIA_GAME_RES`: the picture's height, 1080 up to 4320 ("window": the window's height, or the
  monitor's in fullscreen, never below the game's 1440). 1080 is the one height below the game's own: a lighter picture
  for slower GPUs, whose scene the game sizes against that picture itself (see `scale_k` below).
- `picture_size()`: the picture the settings ask for, `*ow` x `*oh`; 0 when it is the game's own 2560x1440 (every set
  stays off). Read at the start, before the window exists: the monitor's size is known then. The width is a whole
  multiple of 8 pixels.

### The scene's share of the picture

`dlss_share()`, `wide_dlss_share()`, `fit_share()`, `mode_share()`, `upscaler_runs()`, `plain_share()`,
`scene_share()`, `wanted_share()`, `picture_ratio()`, `share_most()`, `heap_share()`.

DLSS's render scale (`recomp/patches-v111.txt`, set `res_fixed`). While DLSS is on, the scene is the DLSS mode's share
of the picture (NVIDIA's ratios: DLAA all of it, Quality 2/3, Balanced 0.58, Performance 1/2, Ultra Performance 1/3)
instead of what the game's dynamic-resolution controller picks. The share is written to +240 at every change of the
setting (the menu switches the mode live); the set goes on `RES_FIXED_AFTER` presents after the first, once the
controller has filled the fields the fixed path keeps. Without DLSS or FSR the share is `NX2SARIA_SCENE_SHARE`'s
(`plain_share()`): the game's own most unless set, all of the picture for the menu's Native, or dynamic resolution's
step below it (`runtime/pacing/dynres.c` `dynres_share()`).

The share follows the upscaler that runs, not the setting: DLSS starts only after a restart and only where NGX came up
(`vk_dlaa_available`), and FSR 3.1 only where its library loads (`vk_dlaa_fsr3`), so a mode picked in the menu, or one
this PC cannot run, no longer shrinks the scene with nothing to scale it back up. FSR 3.1 and a mode change while one
runs apply at once. `dlaa` switched off in the menu stops DLSS at once (`runtime/vk/vk_dlaa.c` `vk_dlaa_frame()` reads
it each frame), so the share follows the setting too: the scene goes back to the game's own scale instead of staying
small with nothing to enlarge it. `wide_dlss_share()` is `vk_dlaa.c`'s view of it: the mode's share, 0 when off.

`PLAIN_SHARE`: without DLSS or FSR 3.1 (FSR 1, bilinear) the game's dynamic resolution took the scene down to a third
of the picture under load, and a plain scale shows every step of that. The most the game's controller ever gives
(`wide_init()`) is held.

- `heap_unserved` is set by `wide_init()` when the grown heap table (`wide_heap_extra_kib()`) did not reach the game:
  `runtime/hle/romfs.c` could not build it (a table in a form it does not rewrite, as a mod's may be) or could not write
  it to the profile folder. The game then has the shipped heap, made for its own picture with the scene at
  `PLAIN_SHARE`, and gets no more than that: a larger scene or picture in the shipped heap is the crash the growth is
  there to prevent.
- `fit_share()`: the most the scene can be of the picture in the heap the game has.
- `mode_share()`: the share of the picture the upscaler's mode renders (`NX2SARIA_DLSS`; NVIDIA's ratios), or the step
  dynamic resolution takes in its place (`runtime/pacing/dynres.c` `dynres_override()`), so that its step changes the
  scene the game draws.
- `upscaler_runs()`: DLSS or FSR 3.1 scales the scene in this run: the setting is on and the upscaler came up.
- `dlss_share()` is never more than the texture heap the game has was made for (`fit_share()`). A mode chosen in the
  menu that renders more than the start's did gets its size after a restart; a heap at its limit never holds the whole
  picture.
- `plain_share()`: the share without DLSS or FSR 3.1 (`NX2SARIA_SCENE_SHARE`, 1 at most): `PLAIN_SHARE` unset, 1 for the
  menu's Native (`runtime/ui/menu.cpp` `row_upscaling()`: the scene at the picture's size, which the stand-in then only
  decodes, `runtime/vk/vk_tonemap.c`), 0 for the game's dynamic resolution, as with no share at all. Read at every call,
  so the menu's choice applies at once within the heap the start made.
- `scene_share()`: without an upscaler, dynamic resolution's step (`dynres_share()`) in place of `plain_share()` while
  it is below it: it steps down from the share `wide_dynres_base()` gives it and never above the setting, also in the
  frame between a changed `scene_share` and its new start. An `NX2SARIA_SCENE_SHARE` above the heap the game has is cut
  to `fit_share()`.
- `wanted_share()`: the share the settings ask for, before the heap's limit: the upscaler's mode while one runs, else
  `plain_share()`.
- `picture_ratio()`: the picture's pixels over the game's own 2560x1440.
- `share_most()`: the most share of the picture the application pool has room for at the picture the settings ask for
  (the heap grows with the share's square, see `heap_share()`): the heap's most over the shipped one
  (`mem_heap_extra_max_kib() / GAME_DTA_KIB + 1`), over the picture's ratio, square root, times `PLAIN_SHARE`. Computed
  anew at each call, so the menu can tell whether a restart would give a larger scene (`wide_scene_restart()`).
- `heap_share()`: the largest share of the picture the scene can have in this run, which the texture heap is made for
  (`wide_heap_extra_kib()`). Decided once, at the first call, which is the start (`runtime/memory.c` sizes the
  application pool from it before any device exists): the game's own most, `PLAIN_SHARE`, or more where the settings at
  the start ask for more: `NX2SARIA_SCENE_SHARE`, or the upscaler's mode when the upscaler is on (DLAA and FSR at native
  size render the whole picture; the modes below `PLAIN_SHARE` need nothing). From the settings, not from what runs:
  DLSS is known to run only once the device is up. A mode chosen in the menu later that renders more keeps the start's
  share until a restart (`dlss_share()`). Never more than the application pool can take: the heap grows with the share's
  square, and past `mem_heap_extra_max_kib` the game's own SetHeapSize is refused and nothing starts. A 10240x2880
  picture has room for 80% there, not for all of it, and the scene is then that much; `wide_heap_extra_kib()` says what
  a picture this large lacks.

`scale_k`: the game turns the scale at +240 into a scene size against its own 1440-line picture while the picture is
taller (a 2160-line picture at 1.0 gave a 1440-line scene), so the share goes there times the picture's height over
1440. Under a 1080-line picture the game measures against that picture instead (a share of 1.0 times 1080/1440 gave an
810-line scene, three quarters of the picture), so there the share goes in as it is: `wide_init()` sets the factor.

### `wide_dynres_base()`

For `runtime/pacing/dynres.c`: the share dynamic resolution steps down from while no upscaler runs, the scene's share
as the settings give it (`plain_share()` within the heap the start made, `fit_share()`); -1 while DLSS or FSR 3.1 runs
(`upscaler_runs()`: it steps the upscaler's mode then); 0 where there is no share to step: no `res_fixed` set in the
generated C, or a `scene_share` of 0 (the game's own dynamic resolution). A `scene_share` that is not a number gives
nothing to step.

### `scene_log()`

One line for the scene in effect: its size, the picture's and how the scene reaches the picture's size. Written when
the fixed share first takes over from the game's dynamic resolution and again when the share or the way it is scaled
changes (a setting changed in the menu), not at every settings change. The scene's size is the share of the picture's
as the game sizes it (a 50% scene of 5120x2160 is 2560x1080). `dyn`: dynamic resolution's step moved the share
(`dynres.c`, which logs the step too): the first 40 such lines of a run show the scene following it, and later ones are
left out, since the steps go on for as long as the game runs.

How the scene is scaled: 1 DLSS, 2 FSR, 3 not scaled (the stand-in decodes 1:1), 4 the stand-in's edge-adaptive filter,
5 its linear one. When the menu's Native, or another share, is larger than the heap the start made, the line says why
the scene is smaller: against the heap's share, not `s`, which dynamic resolution's step may hold below the setting.

### `wide_present()`, `wide_scene_restart()`

`wide_present()`: dynamic resolution's step moves the share without a setting changing: the upscaler's mode, or the
scene share without one. The set goes on at `RES_FIXED_AFTER`, once the controller has filled its fields; later too,
after a share of 0 (the game's dynamic resolution) gave way to a fixed one.

`wide_scene_restart()`: the settings ask for a larger scene than the heap the start made holds, and the next start's
heap holds more: the share it asks for within what the application pool has room for at the picture the settings ask
for (`share_most()`). A heap the game was not served (`heap_unserved`) stays the shipped one at the next start too, as
far as is known here.

### `wide_heap_extra_kib()`

The texture heap: the engine's render targets live in "DynamicTextureAllocatorMem" of the HeapDef table
(`System/HeapSize/Product.Nin_Ounce_NVN2.HeapDef.byml.zs`; 312320 KiB, carved from "Graphics"), made for the 2560x1440
picture with the scene at three quarters of it at most. Larger pictures' targets did not fit: the render-target
manager's search for a free block (`main+0x3ed00`) fell to its out-of-memory path and read through a null block when
the pause menu's blur needed one at 5120x2160. So did a larger scene in the game's own picture: with DLAA the scene is
all of the picture, 2560x1440 where the game never goes past 1920x1080, and a player's game ended in that same
function, reading address 0x48, each time a dialogue closed in Castle Town (0.8.0, 1440 lines, `dlss = dlaa`).

The heap and its parent grow by the shipped size times the picture's pixels over 2560x1440 and times the scene's
pixels over the game's most (`heap_share()`: its targets are the picture's size or the scene's, so this is the most
they can need), in whole MiB; `runtime/gfx/resolution.c` rewrites the table (`res_heapdef_build()`) and
`runtime/memory.c` adds the amount to the application pool, so no other heap shrinks. 0 for the game's picture and
scene, and for a larger picture without the widescreen sets. In MiB, with the scene at the game's own most and with all
of the picture (DLAA):

| Picture | Game's own most | All of the picture |
|---|---:|---:|
| 2560x1440 | 0 | 238 |
| 3440x1440 | 105 | 424 |
| 3840x2160 | 382 | 915 |
| 5120x2160 | 610 | 1,322 |
| 7680x2160 | 1,068 | 2,135 |

The heap is committed as the game first writes it (`runtime/memory.c`, `NX2SARIA_HEAP_LAZY`), so the room a run does
not use costs address space and no memory. At most `mem_heap_extra_max_kib`: `heap_share()` has taken the scene's share
down to what fits, and a picture too large even with the game's own scene (above 10240x2880) gets the most there is,
with a line that says so.

### `save_extra()`

The save target is 543x306. Its viewport is expanded on the changed axis, centred under the unchanged scissor, so it
keeps the middle 16:9 of a picture that is wider or taller. The additions are even pixels to keep the centre exact.

### The menus with black bars

`NX2SARIA_WIDE_MENUS = center`. The menus' contents are in the middle 16:9 (set `wide_hud`); four screens keep the
whole picture (`wide_init()`'s keep list): the item wheel's background, the pause and system menus' background (a
capture of the whole picture, which squeezed would land in the middle at the wrong width), the event fade and the
loading fade (a fade over the middle alone would leave the scene showing at the sides). With full (the default) the
menus fill the picture: their layouts are laid out for its width (`runtime/patches/wide_hud.c`) and nothing here runs.
With center the keep list stays, and the present clears the picture's sides outside its middle 16:9 to black while a
pause or system menu, a loading screen or a movie is up (`runtime/patches/wide_bars.c`, which watches the game's own
state of the menus' background screens and of the loading fade). Those screens then show the middle 16:9 of their
whole-picture backgrounds, at their own width. The item wheel keeps its sides: the world goes on behind it. The loading
screen's icon, stretched over the whole picture with its layout, would sit in the black side: `wide_hud.c` squeezes it
into the middle 16:9's corner (`runtime/hle/romfs.c` `wide_hud_overlay()`). The player's view of it:
[wide-hud.md](../wide-hud.md#wide_menus--center).

### `wide_balloon_init()`, `wide_balloon_host()`

`mods_exefs_switch()` has already disabled the set if a mod owns its site. The balloon's anchor (`main+0x20c2c8`) is in
the game's layout units. The canvas applies the horizontal scale from `wide_init()` when it draws; the host undoes that
scale for the anchor, so the balloon follows its speaker's screen position.

### `wide_init()`

- The scene's share of the picture is not a size in this set: the game's GPU-time dynamic resolution sets it each frame
  (3/4 of the picture at most), and writing other sizes into `wide_scene`'s constant or the 1920x1080 render sizes left
  the light target where the controller put it. DLSS's modes fix the share through `res_fixed` instead
  (`wide_present()`).
- The grown heap table is served from here on, or is not (`runtime/hle/romfs.c` `heapdef_overlay()` says why in the
  log): before the guest runs and before the first present asks for the scene's share.
- With the game's own sizes, or the shipped heap, every set stays off. `started` is stored after the picture's size:
  `wide_picture_known()` reads it from any thread.
- The scene is 3/4 of each image dimension, with an 8-pixel width and an even height.
- The keep list: the screens whose UI keeps the whole picture (set `wide_hud`): the item-wheel, pause and system menu
  backgrounds, the event and loading fades. Their classes are `main+0x4bed098` and `main+0x4be4fb0` (`keep`),
  `main+0x4be61c8` and `main+0x4be7b30` (`keep2`).
- Tall pictures keep whole-picture backgrounds, fades and movies across the full height. Only ordinary UI is squeezed
  into the middle 16:9; clearing these margins would expose bright scene strips during fades.
- The save's picture: what its viewport gets added to on each axis. Its set is not one `wide_available()` asks for: C
  generated before it existed keeps the other sets and squeezes the save's picture as before.

### `wide_start_picture()`, `wide_picture()`, `wide_available()`

`start_w` and `start_h` are the picture `wide_init()` set up (`started` once it has decided): what
`wide_start_picture()` answers from then on.

`wide_start_picture()`: the picture `wide_init()` sets up, asked while the RomFS comes up (`runtime/hle/romfs.c`
`wide_hud_overlay()`). Once `wide_init()` has decided, its own answer. Before that (the RomFS comes up from within
`wide_init()` when the texture heap grows: its `fs_init`) the tests it makes, once, and the answer kept: the game's own
2560x1440 when the setting is 16:9 or narrower, the generated C has no widescreen sets, or a grown texture heap was
needed and not served (`res_heapdef_ok` is known once the RomFS's HeapDef overlay is made, which comes before the
layouts'). 1 for a picture of the settings' size.

`wide_picture()`: the picture the game draws: the widescreen one `wide_init()` set up, else the game's own 2560x1440.
DLSS's largest mode (dlaa) renders the scene at this size. `wide_available()`: the generated C has the widescreen patch
sets (the menu shows the row only then).

## `runtime/patches/widescreen_math.h`

The aspect calculations shared by the wide-format runtime and its focused tests: `wide_vertical_fov()`,
`wide_ui_vertical_scale()`, `wide_aspect_heap_growth_kib()`.

## `runtime/patches/wide_bars.c`

Centred 16:9 screens: while a pause or system menu, an event or loading fade or a movie is active, the present clears
the margins outside the middle 16:9 when the picture is wider. Tall pictures keep their full-height backgrounds and
fades.

The signal is the game's own state of the screens behind those menus. Every interface screen of the game is an object
whose byte +0x1a6 (`SCREEN_STATE`) says where it is in its life: 0 closed, 1 opening (`main+0x2d62e0` sets it when the
screen is asked to open), 2 open (vtable +0x240, `main+0x2cda10`), 3 closing (vtable +0x248, `main+0x2d8af0`) and 0
again once it has closed (vtable +0x258, `main+0xc60a10`). Two classes of screen are watched, by their vtables, the ones
the widescreen keep list names (`runtime/patches/widescreen.c` `wide_init()`):

- `main+0x4be4fb0`, the background of the pause and system menus (MenuBG_00, SystemBG_00): a capture of the picture the
  menu opens over, redrawn under it. Its screen is on (states 1 to 3) for as long as such a menu is up: the sides go
  black on a wide picture when the menu starts to open and return once it has closed. The game keeps drawing 3D under
  the pause menu (the item's model on the items page, Link on the equipment page), so whether a 3D scene is drawn does
  not tell a menu from play; this does.
- `main+0x4be7b30`, the loading fade (LoadingFade_00), and `main+0x4be61c8`, the event fade: their canvases cover the
  whole picture. Wide pictures keep their side margins black while their contents are centred; tall pictures keep their
  full height so the fades cover the extra scene above and below the UI band.

A movie (`runtime/hle/ov_movie2.c` `ov_movie_live()`) is cropped to the middle 16:9 on wide pictures and fills a tall
picture by set `wide_movie`.

The screens' addresses come from the patch set `wide_bars` (`recomp/patches-v111.txt`): right after the screen's setup
(`main+0x773b90`, vtable +0x80) has set up its 2D canvas, its site calls the patch page's +0xef0 with the screen in x19
(`wide_bars_screen_host()`, registered by `runtime/patches/patches.c`). The setup runs when the game makes a screen,
which lives on from there, so the table is short and a screen set up again is found in it. A screen the game has
destroyed no longer holds a watched vtable (a destructor writes its base class's, or the memory is reused or unmapped):
the present notices and frees its entry. The set is switched on only for a non-16:9 picture (`wide_bars_init()`).

`BARS_SCREENS` (16): the screens watched at once (the game makes a few of each). `BARS_TOLD` (24): the lines a run
writes about the sides (black, shown again). `SCREENS_TOLD` (48): the lines a run writes about the watched screens
(found, state changes, gone). `bars_on` is set by `wide_bars_init()`: center on a non-16:9 picture, with the set on.
`bars_x_margin` and `bars_y_margin` are the shares of the picture outside its middle 16:9. `screens` holds the watched
screens' guest addresses, 0 for a free entry.

### `wide_bars_screen_host()`

`main+0x774068` (set `wide_bars`), after the screen's setup has set up its canvas: x19 is the screen. Every register is
left as it is (the site's next instruction reads x8, which the site loaded before the call). A screen set up again is
already watched; an entry whose screen was destroyed since is taken over.

Without the bars (`bars_on` 0: a 16:9 picture, where the same site belongs to the set `fov_menu`) the menus'
backgrounds and the loading fade are still watched, for `wide_menu_open()` and `wide_loading_open()`; the event fade
is not, since nothing asks for it there.

### `wide_menu_open()`, `wide_loading_open()`

1 while a watched screen of that kind is in states 1 to 3: the game's pause, map or system menu, and the loading
screen. Read by the field of view (`runtime/diag/debug_tools.c`), which leaves the menus alone, and by the notice of
full video memory (`runtime/ui/menu.cpp` `vram_warn_tick()`), which waits for both. 0 when neither `wide_bars` nor
`fov_menu` is on: no screen is handed over then.

### `wide_bars_init()`, `wide_menu_bars()`

`wide_bars_init()` is `wide_init()`'s call: centre the screens, with margins on the axis that differs from 16:9.

`wide_menu_bars()` runs each present, from the present's thread: the x and y shares outside the middle 16:9, 0 when no
centred screen is active. A newly watched screen is logged there. A destroyed one (its memory is no longer a watched
screen) is logged as gone and its entry freed.

## `runtime/patches/wide_hud.h`

The HUD's corner elements at the edges of a picture wider than 16:9 (`NX2SARIA_WIDE_HUD = edges`), and the loading
screen's icon in the middle 16:9 for `NX2SARIA_WIDE_MENUS = center`.

- `WIDE_HUD_ARCHIVE` (`UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs`): the shared interface layouts of
  the game, which hold the HUD (PlayerStatus_00, MapMenu_00, NumDisplay_00) and the loading screen (LoadingFade_00).
- `WIDE_HUD_TITLE_ARCHIVE` (`UI/Layout2Archive/Title.Product.100.Nin_Ounce_NVN2.blarc.zs`): the title screen's layouts
  (Title_00, TitleMenuBG_00, PaCommonBtnTitle_00): its menu backdrop is part of the menus' redesign
  (`wide_menus = full`).
- `WIDE_HUD_COMMON` and `WIDE_HUD_TITLE` name the archives the edit serves, for the functions that take one.
  `wide_hud_archive_path()`: the RomFS path of one; NULL for a number that is not one.

`WideHudShift`, what the edit does. `out`: the HUD's edge elements move out by that many units (`wide_hud_out()`; 0
none, `NX2SARIA_WIDE_HUD = center`). `squeeze`: 16:9 over the picture's ratio, by which the loading screen's icon is
narrowed and brought in so that it sits in the middle 16:9's corner at its own width (1 none;
`NX2SARIA_WIDE_MENUS = center`). `menus`: the menus' layouts redesigned to fill a picture that many units wider on each
side (`wide_hud_out()`; 0 none; `NX2SARIA_WIDE_MENUS = full`). `no_growth`: 1 leaves out the edits that make the archive
larger (the pause menu's backdrop drawn in pieces, `wide_hud_split()`), with the rest of their layout's redesign (it
keeps the game's menu layout): for a game whose resource size table cannot be raised to hold a larger archive.

`WideHudCount`: the edits of edges mode, one per layout: what the start-up line counts (`runtime/hle/romfs.c`
`wide_hud_overlay()`). `layout` is the member's name without ".buil"; `panes` the panes whose translate x moves; `keys`
the animation keys that move (curves' value spans not counted).

### `wide_hud_out()`

How far, in units of the game's 3840x2160 layout grid, an element at a side of the middle 16:9 moves out to reach the
same side of a picture `w` x `h`, once the UI is squeezed into that middle 16:9 (`runtime/patches/widescreen.c`, set
`wide_hud`): 1080 * w / h - 1920. 0 for a picture within 1% of 16:9 or narrower.

### `wide_hud_counts()`, `wide_hud_counts_of()`

`wide_hud_counts()` fills `out[0..max)` and returns the number of layouts edges mode edits. `wide_hud_counts_of()` is
the same for the HUD's edges (`menus` 0) or for the menus' redesign (`menus` 1: panes changed in any way, keys moved).

### `wide_hud_lay_out()`

One layout file `p[0..n)` (a BFFH member named `member`, "PlayerStatus_00.buil" and so on) with the shift's edit, in
place: its edge elements moved out by `s->out` units, or (LoadingFade_00.buil) its icon squeezed by `s->squeeze`. Every
field is found and checked before any is written (each pane's x, and a squeezed pane's scale x, is the game's 1.1.1
value and no animation keys it; its ancestors have scale 1 and no rotation, and a squeezed pane's sit at x 0 with no
animation moving or scaling them; each animation curve has the keys this expects), and read again after. It returns the
number of fields changed; 0 for a member the shift does not edit or one edited so already; -1 when it is not the layout
this expects (`why` says what differs; the file is left as it was). `report` (may be NULL): what moved, "N_ScaleL_00 x
-1920 -> -2580, ...", cut to `report_n`.

### `wide_hud_split()`

The pause menu's backdrop drawn in pieces (`wide_menus = full`): one layout file `p[0..n)` (PlusMenu_00.buil) with its
picture P_MenuBg_00 cut into pieces that each move with the column of the pages above it, the pieces after the first
new panes inserted after it, the file grown to hold them (`runtime/patches/wide_hud.c` `split_layout()`). `*out`
(malloc'd) and `*out_n`. It returns the number of panes added; 0 for a member the shift splits nothing in or one split
so already (`*out` NULL); -1 when it is not the layout this expects (`why`; nothing written). `report` (may be NULL):
"P_MenuBg_00 in 7 pieces".

### `wide_hud_name_hash()`

The 64-bit hash a layout keeps beside each name in its tables of panes, pictures and materials, which the game's lookup
by name compares: CityHash64 of the name's bytes. 0 for a name longer than 32 bytes (no layout has one).

### `wide_hud_archive()`, `wide_hud_archive_in()`

`wide_hud_archive()`: the same on the decompressed Common layout archive (a SARC, `*sarc` malloc'd, `*n` its size) for
every layout the shift edits, on every copy of each: all of them change or none, the edits in place first, then the
splits. A split grows its layout, so the archive is then laid out again (`runtime/hle/sarc.c` `sarc_rebuild()`):
`*sarc` is replaced (the old one freed) and `*n` follows. It logs one line per layout that changed ("wide hud:
PlayerStatus_00 (4 copies, 16 fields): N_ScaleL_00 x -1920 -> -2580, ...", "wide menus: PlusMenu_00 (4 copies, 340
fields, P_MenuBg_00 in 7 pieces, 24 new panes): ...", "widescreen: menus: LoadingFade_00 (...)"). It returns the count
of fields changed and panes added over all copies, 0 when everything was done already, -1 when any layout is not the one
this expects (the archive as it was, but for running out of memory during a split, which may leave the edits in place
made).

`wide_hud_archive_in()`: the same for the archive `WIDE_HUD_COMMON` or `WIDE_HUD_TITLE` names: only that archive's
layouts are edited (the Title archive's TitleMenuBG_00 only when `s->menus` > 0). `wide_hud_archive()` is the Common
one.

### `wide_hud_build()`, `wide_hud_build_in()`

`wide_hud_build()`: the Common layout archive's zstd file `in[0..n)` with the shift's edits, compressed again with the
dictionary of `Pack/ZsDic.pack.zs` (`zsdic`) the original names: `*out_z` (malloc'd) and `*out_n`. Its frame records the
decompressed size, larger than the original's when a split grew the archive: the game sizes the buffer it reads the
archive into from the resource size table, whose entry must then grow too (`runtime/hle/romfs.c`
`wide_hud_overlay()`). It returns 1 when `*out_z` was made, 0 when there is nothing to serve, -1 when it cannot be
(logged; the archive is served as it is).

`wide_hud_build_in()`: the same for the archive `WIDE_HUD_COMMON` or `WIDE_HUD_TITLE` names (`in` is that archive's
file). It returns 0, without reading it, when the shift edits nothing in that archive (the Title one with `s->menus`
0). `wide_hud_build()` is the Common one.

## `runtime/patches/wide_hud.c`

The HUD's corner elements at the edges of a picture wider than 16:9 (`NX2SARIA_WIDE_HUD = edges`). What a player sees,
which elements move and why the others stay: [wide-hud.md](../wide-hud.md).

With the widescreen sets on (`runtime/patches/widescreen.c`), the game's UI keeps its 16:9 shape in the middle of the
picture: the 2D canvas setup scales the full-screen projection of the 3840x2160 layout grid by 16:9 over the picture's
ratio (set `wide_hud` of `recomp/patches-v111.txt`). The HUD's corners are then the corners of that middle 16:9. Edges
mode moves the elements that sit at a side out by `wide_hud_out()` units of the grid, so that after the squeeze they sit
at the same side of the whole picture; everything else (the action prompt, messages, markers that follow the 3D world,
menus) stays where the game puts it. The edit is made to the shared interface layouts the game reads once at start
(`runtime/hle/romfs.c` `wide_hud_overlay()` serves the archive with it), so a change of mode or of the picture's shape
shows after a restart, as both settings are read at start.

What moves, in the game's 1.1.1 layouts (each pane named; its x is the game's value, checked before the edit):

- PlayerStatus_00: N_ScaleL_00 (-1920: the hearts, the magic meter, the D-pad guide and the Stone of Agony hang on it)
  to the left; N_ScaleR_00 (1920: the item slot and its page dots), N_SubItemBaseFishing_00 (the two sub-item slots
  beside it) and N_VRS_00 (an empty pane over the item slot) to the right.
- MapMenu_00: the minimap, to the right. The layout is also the pause menu's map, whose panes stay: only the minimap's
  own panes move, N_MiniMap_00 (the field and dungeon maps), N_EventEndIn_01 (frame, floor label), N_MiniMapCap_00 (the
  maps' captures), N_VRS_00 and Pa_KeySmall_00 (the small and boss key counter, a tab over the minimap). The map icons
  are shared by both maps: the game's animations put their layer at the minimap's place for the minimap (MapType:
  N_MapIcon_00 x 1490; DungeonMiniMapPattern: N_PatternDungeonMini_01 x 1536) and at 0 for the pause map, so only the
  keys at the minimap's place move.
- NumDisplay_00: the count that slides in from the right when rupees or an item's count change (two rows), to the
  right, where it stays next to the item slot.

With `NX2SARIA_WIDE_MENUS = center` the loading screen keeps the whole picture (its fade must cover it), and the
present blacks out the picture's sides while that screen is open (`runtime/patches/wide_bars.c` `wide_menu_bars()`).
Its icon, LoadingFade_00's N_IconIn_00 (1778: bottom right), would be stretched over the whole picture with its layout
and land in that black side: its x and scale x are multiplied by 16:9 over the picture's ratio (the squeeze), which
puts it in the bottom right corner of the middle 16:9 at its own width, where a squeezed layout would draw it.

With `NX2SARIA_WIDE_MENUS = full` the menus' redesign (the plans of group `G_FULL`) also edits a second archive, the
title screen's `UI/Layout2Archive/Title.Product.*` (`WIDE_HUD_TITLE`): TitleMenuBG_00's backdrop capture over the whole
picture. The game squeezes that canvas into the middle 16:9 like every layout off the keep list
(`runtime/patches/widescreen.c` `wide_init()`), so it is laid out for the picture's width the same way as the message
log's backdrop. The archive is served beside the Common one (`wide_hud_overlay()`); each plan names the archive it
belongs to.

The pause menu's backdrop (PlusMenu_00's P_MenuBg_00) has the slots of its pages' grids painted in, under columns the
plan moves apart by different distances, so it is neither moved nor stretched: the split (the `splits` table) cuts it
into pieces that each move with their column, as new panes inserted after it, with a plain stretch of the picture drawn
over each gap. A split grows its layout, so it runs after the edits in place, and the archive is laid out again around
the grown copies (`runtime/hle/sarc.c` `sarc_rebuild()`). The game sizes the buffer it reads the archive into from the
resource size table, so the archive served larger needs that entry raised (`runtime/hle/romfs.c` `restbl_room()`); when
it cannot be, the archive is made again without the split, and PlusMenu_00 keeps the game's menu layout
(`WideHudShift.no_growth`), its pages never moved without their backdrop.

### The layout format (BFFH)

The layout files are BFFH: little-endian records that point at each other by 64-bit offsets from the file's start.

- The header has a group of slots per table: +0x00 the offset of the list of name offsets, +0x08 that of the list of
  record offsets, +0x10 that of the names' hashes, +0x18 the count. Panes at 0x118 (`PANE_TABLE`; in the order of the
  records: parents before children), animations at 0x238 (`ANIM_TABLE`).
- A pane record: scale x at +0x10, rotation at +0x18, translate x at +0x1c (in its parent's space, the grid's centre at
  0), the parent's index at +0x28 (-1 for the root).
- An animation record starts with the offset of its body: +0x08 the list of its tracks and +0x10 their count; +0x18 the
  list of its curves, +0x20 their count, +0x28 one byte a curve for its component (0 x, 4 y); +0x38 the constants'
  floats, +0x40 their count, +0x48 one component byte a constant.
- A track: the offset of a 6-byte target code, +0x08 its first curve (0xffff none), +0x0a its first constant, +0x0c the
  curves' count, +0x0d the constants'. The code: bits 8-10 are 7 for a pane's own property, the pane's index is in bits
  11-14 and 16-18 (the high part), bit 22 and byte 3 give the property (1 and 0: scale; 1 and 1: translate).
- A curve: the offsets of its frames and of its values. The frames: their count at +0x10. The values: +0x00 the last
  value less the first (its span), +0x08 the offset of the floats, +0x10 their count, (value, slope) a key.

The track's 7 bits of pane index name at most 128 panes (`MAX_PANES`); the game's largest layout here has 102. A pane
property as a track names it is byte 3 of its code, in the group bit 22 selects (1 for `PROP_SCALE` and
`PROP_TRANSLATE`); `PROP_SIZE` (0x102), the pane's width and height, is byte 3 = 2 with bit 22 clear (its x component
is the width). The same format from the reader's side: [wide-hud.md](../wide-hud.md#the-layout-format-bffh).

### The plan

`PaneOp`, `KeyOp`, `KeyMap`, `Plan`.

Each operation belongs to one of three groups, and is made only when the shift asks for its group: the HUD's edges
(`G_HUD`; `WideHudShift.out`), the menus filling the picture (`G_FULL`; `menus`) and the menus kept in the middle 16:9
(`G_CENTER`; `squeeze`). Its distance u is the group's: `out` or `menus` in grid units, or the squeeze (16:9 over the
picture's ratio).

What an operation does to its pane, from the game's values (x: its translate x, sx: its scale x, w: its width):

- `OP_MOVE`: x + f * u. Every ancestor has scale x 1 and no rotation, so the move is the same on the grid.
- `OP_MOVE_IN`: the same distance on the grid inside scaled ancestors (none turned): x + f * u over their scale x.
- `OP_GROW`: width + 2 * f * u, as wide on each side of a centred pane (a window's frame keeps its corners, a scissor
  clips and a list lays out by its width, where a scale would stretch them).
- `OP_WIDEN`: scale x times 1 + f * u / 1920 (its children with it): a full-width background over the whole picture.
- `OP_SQUEEZE`: x and scale x times u (`G_CENTER`): into the middle 16:9's corner of a canvas stretched over the
  picture.
- `OP_PIN`: scale x times 1920 / (1920 + u) and x as far from its side of the grid as the game's at that scale: an
  element of a canvas stretched over the picture keeps its own width at the same side.

In a `PaneOp`, `x`, `sx` and `w` are the game's values the operation checks (x for the moves, sx for the scales, w for
`OP_GROW`).

`KeyOp`: the keys of one animation's curve (or its constant) of a pane's translate x or scale x: those at `value`, `at`
of them, become value + f * u (translate) or value times 1 + f * u / 1920 (scale, their slopes with them). `keys`: how
many the curve has, 0 for a constant. Up to four values (`MAX_MAP`), each of its own group, the used ones first (`at` >
0). `slope_f`: a translate curve whose values all grow by one factor (a strip of pages laid out wider apart) has every
key's slope times 1 + slope_f * u / 1920 too, in the group of its first value, so that its motion keeps its shape.

A `Plan` names the layout archive its layout lives in (`WIDE_HUD_COMMON` or `WIDE_HUD_TITLE`).

- In the map's plan, P_MapBase_00 grows (`OP_GROW`, `G_FULL`, 3928 wide): the pause menu's map and the world map over
  the whole picture (none of the minimap's).
- `menu_overlay_full` and the other `G_FULL` tables: the menus over the whole picture;
  [wide-hud.md](../wide-hud.md#wide_menus--full) gives each screen's reasons.
- `title_menu_bg_full`: the title screen's backdrop (`UI/Layout2Archive/Title.Product.*`, `wide_menus = full`): the
  capture of the title scene behind the menu's buttons, a fixed 3840 x 2160 under a pane of its own like the message
  log's. The canvas is squeezed into the middle 16:9 as every layout's not on the keep list is, so the capture and the
  two pictures that draw it (and its noise) are made as wide as the whole picture, 2 * menus units wider.

### The split's pieces

`Piece`, `Split`, `split_of()`.

A picture drawn in pieces, each a pane of its own that moves on its own (group `G_FULL`). A piece {a, b, x0, x1, f0,
f1} shows the picture's texture from x a to b (the game's picture, in its pane's parent's space) between x0 + f0 * u and
x1 + f1 * u. The first piece is the pane itself; piece k is a new pane `<pane>_k` right after it, with a picture and a
material of the same name (`split_layout()` says how the file changes).

The pause menu's backdrop, P_MenuBg_00 (9200 wide at x 4280, from -320 to 8880, under N_MenuChange_00 with the three
pages' panes): its painted slots sit under the columns the plan moves apart (the equipment's grid one u, the item grid
three), which a stretched picture would leave. Each piece moves with its column, and a plain stretch of the picture
without slots (x 1880 to 2010) covers each gap that opens between them, from one piece's place to the next one's:

| a | b | x0 | x1 | f0 | f1 | Shows |
|---:|---:|---:|---:|---:|---:|---|
| 1880 | 2010 | -320 | -320 | 0 | 1 | a seam left of the collection grid, out to the picture's left side |
| -320 | 3380 | -320 | 3380 | 1 | 1 | the collection grid, the scroll ornament, the equipment's left column |
| 1880 | 2010 | 3380 | 3380 | 1 | 2 | a seam |
| 3380 | 4980 | 3380 | 4980 | 2 | 2 | the portrait's oval, the middle of the equipment page |
| 1880 | 2010 | 4980 | 4980 | 2 | 3 | a seam |
| 4980 | 8880 | 4980 | 8880 | 3 | 3 | the equipment's right grid, the scroll ornament, the item grid |
| 1880 | 2010 | 8880 | 8880 | 3 | 4 | a seam up to the item page's info panel |

`MAX_EDITS` (2048): the most fields one layout's edit writes: its panes, its keys (with their slopes) and each curve's
span of values.

### `groups_of()`, `unit_of()`, `wide_hud_counts_of()`

`groups_of()` and `unit_of()`: the groups the shift asks for, and each one's distance. In `wide_hud_counts_of()` the
panes are distinct: a pane moved and grown counts once, and the pane drawn in pieces once.

### Reading the layout

`u32_at()`, `u64_at()`, `f32_at()`, `str_at()`, `table()`, `table_find()`, `same()`.

Every offset is checked against the file's size.

- `table()`: the lists of a table whose header slots start at `slot`; 0 when they do not fit the file.
- `table_find()`: the index of the entry `name` of a table, and its record (at least `size` bytes); -1 when it is not
  there.
- `same()`: floats within 1e-3 are the same; the game stores some zeros as -2.8e-14.

### `plain_ancestors()`, `centred_ancestors()`, `ancestors_scale()`

- `plain_ancestors()`: each ancestor of pane i has scale x 1 and no rotation, so that a move in the pane's parent's
  space is the same move in the grid's: 1, or 0 with why.
- `centred_ancestors()`, for a squeezed pane, whose x is a distance from the grid's centre: each ancestor sits at x 0,
  and no animation keys an ancestor's x or scale x, so that the pane's own x is where it lands. 1, or 0 with why.
- `ancestors_scale()`: the product of the scale x of pane `rec`'s ancestors, none of them turned: 1 with `*scale`, or 0
  with why.

### `x_tracks()`, `track_holds()`

`x_tracks()`: the x tracks of property `prop` (`PROP_TRANSLATE`, `PROP_SCALE`, `PROP_SIZE`) of pane `index` in the
layout's animations: the curves' offsets (their animation's name in `anim[]`) and the constants' value offsets. It
returns how many there are (`MAX_TRACKS` 64 at most; more than the caller reads means it refuses the layout), -1 when
the animations do not fit the file. In a `Track`, `curve` is 0 for a constant and `constant` the offset of a constant's
float. The 6 bytes after a track's code offset are its first curve, its first constant (16 bits each) and the two
counts (8 bits).

`track_holds()`: 1 when the track (a curve's keys, or a constant) holds the value `v`; -1 when it cannot be read. The
floats are (value, slope) pairs: the values are compared.

### The edit

`Edit`, `add()`, `say()`, `key_op()`, `keyed_covered()`, `key_to()`, `gather()`, `lay_out()`.

Every float the edit writes is found and checked first.

- `say()` appends to `report[0..report_n)` as `snprintf` would, never past it.
- `key_op()`: the key operation of the plan for this pane, property and animation that the shift makes (some value of
  it in a group asked for); NULL when there is none.
- `keyed_covered()`: a pane whose x (or scale x) an operation changes: every animation that keys it must have its keys
  remapped by the plan too, or it would put back what the file says. 1, or 0 with why.
- `key_to()`: the new value of a key, or of a scale's slope, under the map entry `m` at distance u.
- `gather()`: the edits of `plan` on `p[0..n)` for the groups the shift asks for: from the game's values to the new
  ones (`moved` = 0), or what reading the edited file back finds (`moved` = 1: from and to both the new values). Every
  field read must hold its from value. It returns the count, -1 with why. Another animation that holds a value this one
  moves, and that the plan leaves, would keep it there, so it refuses. A scale's slope scales with its value. The span
  is of the game's values when reading the edited file back as when editing it; a span is recorded also where it stays.
- `lay_out()` is `wide_hud_lay_out()` for the groups given (a part of what the shift asks for, see
  `wide_hud_archive()`). A file edited already (by an earlier start's file a mod serves, say) has nothing to change. A
  slope's change can be smaller than `same()` sees, so the bytes are compared. After writing, the layout is read again
  as the game will: each field where it was, with its new value. A file whose fields lie on the records that lead to
  them reads otherwise; it gets its bytes back, last written first.

### The split

`check_split()`, `split_layout()`, `window_of()`, `track_codes()`, `code_pane()`, `code_set_pane()`, `put_at()`,
`table3()`, `entry_name()`, `entry_record()`.

A split inserts new panes after its pane, so it rewrites more than floats:

- The pane is a picture (bits 7-10 of its flags at +0x38 are 1) without children, unturned at scale x 1, with its
  origin's horizontal code (flags bits 11-13) 2, the centre. +0x34 is the index one past its subtree's last pane, +0x3c
  its material's index (u16) and +0x42 its picture's (u16).
- The pane, picture (0x158) and material (0x138) tables also keep at +0x10 the offset of a list of 64-bit name hashes,
  CityHash64 of each name (`wide_hud_name_hash()`), which the game's lookup by name compares: the three lists of a table
  stay in one order.
- The picture record has no window (+0x08 0) and its material's index at +0x10 (u16); its colour and corner lists lie
  inside its own 0x88 bytes, pointed at from +0x00, +0x20, +0x48, +0x50, +0x58 and +0x60. The material has one texture
  map: its list of maps (+0x00, count +0x08) at +0x40 right after the record, the map right after that at +0x48, with no
  list of texture names (+0x00 of the map 0), its UV scale at +0x10, translate at +0x18 and rotation at +0x20. The game
  turns each offset into a pointer in place, once per record it visits, so a new pane gets its own copy of both, offsets
  and all, never one shared with the pane.
- Each pane after it has its index one higher per new pane, so these move with it: a pane's parent (+0x28) and subtree
  end (+0x34) past it, the pane index of the parts (table 0x1b8, record +0x40) and captures (table 0x1d8, record +0x04),
  and each animation track's target code, once per code (any target kind).
- The u32 array at 0x300 (count 0x308, stride 0x30c, 4), one entry per material, grows with the materials.
- Everything new goes past the end of the file: new lists of names, records and hashes for the three tables (the old
  ones left unused), the new records, the grown array. The string pool section (the second BFSI at 0xd0: its start at
  +0x04 and +0x10, its size at +0x08) reached the end and grows to cover them, with the file's size at +0x08. The game
  reads the records from the header's offsets, wherever in the file they lie.

Each step mirrors `tools/layout_dump.py` `split_picture()`, byte for byte (`tests/wide_hud_check.py` compares the two).
`#pragma STDC FP_CONTRACT OFF` is there so that the doubles are computed as that script computes them, without fused
steps.

- `put_at()`: `src[0..len)` (zeros for NULL) at the first offset from `*at` aligned to `align` in `q`, `*at` moved past
  it: where it went.
- `table3()`: a table with its list of name hashes: 0 when its lists do not fit the file. `entry_name()`: entry i's
  name, NULL when it does not lie in the file; `entry_record()`: its record, at least `size` bytes, 0 when it does not.
- `window_of()`: where a piece lands at distance u and the UV window of the texture it shows, from the picture's x and
  width and its texture map's UV scale and translate x: 0 when the piece lies outside the picture or runs backwards.
- `track_codes()`: the animations' track target codes, each once, sorted: malloc'd (`*codes`), their count; -1 when
  they do not fit the file or two of them overlap. `code_pane()`: the pane index a code names (bits 11-14 and 16-18);
  `code_set_pane()`: the code with another.
- `check_split()`: the piece's pane, picture and material records in a split file are the pane itself for piece 0 and
  `<pane>_k` after it; 0 with why when one of them is not where the split puts it. It checks the file `q[0..qn)` as
  `split_layout()` leaves it: the pane tree whole (each parent before its children, each subtree end past its pane and
  within its parent's), the new panes after the pane with their names and hashes, each piece's pane at its place and
  width for distance u with a picture and a material of its own whose texture map shows its window, the per-material
  array as long as the materials, every track naming a pane that exists, the file's and the string pool's sizes reaching
  its end. From the game's file (`orig`, sizes read before the split): the UV windows exactly; without it (NULL: a file
  split before), that the pieces' windows agree on one picture. 1, or 0 with why. For each piece it works out where it
  lands and its texture's window: utx = tx + sx * ((a + b) / 2 - left) / width - sx / 2 and usx = sx * (b - a) / width.
- `split_layout()`: the layout `p[0..n)` with the split's pane drawn in its pieces at distance u: `*out` (malloc'd),
  `*out_n`. It returns the number of panes added; 0 when the file is split so already (checked, `*out` NULL: an earlier
  start's file); -1 with why when it is not the layout this expects (nothing written). `report`: "P_MenuBg_00 in 7
  pieces". Everything is read and checked before anything is written. The new file has room for every record and list,
  each 8-aligned. What follows the pane is renumbered in place before anything is appended; the pane itself shows the
  first piece; then, per new piece, the material (its record, its one-entry list of maps and the map, its offsets moved
  with them), the picture record pointing at the new material, and the pane record; then the three tables' new lists of
  names, records and hashes with the new entries at the insert position; then the per-material array, each new material
  with its source's value. Last it is read again as the game will (`check_split()`).

### `copies_of()`, `trial()`, `archive_groups()`, `archive_in()`

`MAX_COPIES` is 16; a copy's `slot` is its entry in the archive (`sarc_rebuild()`'s numbering).

- `edit_name()`: the log line's name for what a layout's edit did: the HUD's, the menus', or both. `split_in()`: the
  split of the plan's layout the groups ask for, or NULL.
- `trial()`: every copy of one layout, each tried on a copy of its own, for the groups given: its edit in place, then
  its split: 1 when all of them take both (`report`: what a copy that changed moved; `split`: how a copy that was split
  was), 0 with why when one does not, -1 out of memory.
- `archive_groups()`: the groups of the shift that edit something in the archive: the union of its plans' groups, asked
  for.
- `archive_in()` is `wide_hud_archive_in()`, with `*added` the panes the splits added over all copies. A plan the shift
  does not ask for, or of another archive, has no copies. Layouts that share bytes with another layout cannot be edited
  alone. Every copy of every layout is tried first: either all copies of a layout change or none. The HUD's edges and
  the loading icon of the menus in the middle 16:9 are all or nothing over the archive, as both sides of the HUD belong
  together. A layout the menus' redesign does not fit (a mod's, say), its split included, keeps the game's, alone: the
  rest of the redesign and the HUD go ahead, and the line says why (once the HUD's own trial is known: when it fails
  too, the whole archive stays as it is). A layout with a split keeps the game's too when the archive may not grow
  (`s->no_growth`), so that its pages never move without their backdrop. Copies that share their bytes are laid out
  once: the second finds them moved and changes nothing. A failure of `lay_out()` or of a split after a trial of the
  same bytes cannot happen; the check is kept so that a change here cannot hide one. The splits run on the copies
  edited in place: each split copy grows, so the archive is laid out again around them, every copy in a range of its
  own.

### `shift_words()`, `wide_hud_build_in()`

`shift_words()`: what the shift edits, for the log: "the HUD's edge elements 660 units out, the loading icon at 0.744
of its width". Only what the archive's layouts take is said: the title archive has the menus' redesign alone.

In `wide_hud_build_in()` the zstd frame names its dictionary by id (0: none); `Pack/ZsDic.pack.zs` holds them. The
archive is compressed at level 3, as the action prompt's edit of the same archive: about 15 ms for its 10 MB.

## `runtime/patches/action_prompt.h`

The action prompt's diamond of face buttons laid out for a pad layout. `ACTION_PROMPT_ARCHIVE`
(`UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs`): the shared interface layouts of the game, which hold
the action prompt (DoCommand_00).

### `action_prompt_lay_out()`, `action_prompt_archive()`, `action_prompt_build()`

- `action_prompt_lay_out()` lays the diamond of DoCommand_00.buil (`p[0..n)`, in place) out for the face buttons by
  label (`label` nonzero: A bottom, B right, X left, Y top) or by position (`label` = 0: the Switch's places, A right,
  B bottom, X top, Y left). The runtime asks for label 1 only (`runtime/hle/romfs.c` `action_prompt_overlay()`); label
  0 takes a layout by label back. Every field is found and checked before any is written, and read again after. It
  returns the number of fields changed; 0 when the diamond is laid out that way already; -1 when the file is not the
  layout this expects (`why` says which field differs; the file is left as it was).
- `action_prompt_archive()`: the same on the decompressed Common layout archive (a SARC holding DoCommand_00.buil), on
  every copy of the layout it holds: all of them change or none. The count is over all copies; -1 also for a damaged
  archive, more than 16 copies, or copies that share some of their bytes.
- `action_prompt_build()`: the Common layout archive's zstd file `in[0..n)` with the diamond laid out for `label`,
  compressed again with the dictionary of `Pack/ZsDic.pack.zs` (`zsdic`) the original names: `*out` (malloc'd) and
  `*out_n`. It returns 1 when `*out` was made, 0 when the diamond is laid out that way already (nothing to serve), -1
  when it cannot be (logged).

## `runtime/patches/action_prompt.c`

The action prompt's diamond of face buttons for the pad layout at start. The action prompt (top right) shows the four
face buttons as a diamond in the Switch's places, A on the right. Under a layout by label (`NX2SARIA_PAD_LAYOUT` = xbox
or playstation; `runtime/input/input_map.c` `input_pad_layout_pick()`) A is the bottom button, and auto is treated the
same at start, when no pad is open yet: a pad that labels its right button A then plays by position under auto and sees
the diamond the other way round, which [controls.md](../controls.md#button-prompts) says and nintendo avoids. So
`runtime/hle/romfs.c` (`action_prompt_overlay()`) serves the shared interface layouts with the diamond laid out as such
a pad has its buttons: A bottom, B right, X left, Y top. The game reads the layouts once at start, so a change of
layout shows after a restart.

The layout is DoCommand_00.buil in the SARC `UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs`, a BFFH file
of little-endian records that point at each other by 64-bit file offsets:

- The header has a group of slots per table: at +0x00 the offset of the list of name offsets, at +0x08 that of the list
  of record offsets, at +0x10 that of the names' hashes, at +0x18 the count. Panes at 0x118, materials at 0x138. The
  group at 0x218 is the materials' texture blocks: no names, the list of the blocks' offsets in the third slot (0x228),
  the count at 0x230.
- A pane record holds scale x and y at +0x10 and +0x14, translate x and y at +0x1c and +0x20, and flags at +0x38, in
  which 0x1000 puts a text pane's origin on its right edge.
- A material record holds its block's index at +0x14. A block starts with the offset of its texture bindings and their
  count; a binding is 16 bytes, the offset of the texture's name at +0x08.

Each face button K has a group N_K_00, shown while K's action is offered: P_BaseK_00, a cross whose lit arm points at
K; three dots, the other buttons, each 80 units out in its own direction; and N_BtnK_00 at K's place, with K's letter,
the bubble behind the action's name (W_BaseK_00, and W_BaseKDecide_00 under the null N_BaseKDecide_00, which the Decide
animation scales) and the name (T_GuideK_00). The bubble opens to the right, or to the left for the left button, so the
name stays clear of the diamond. The cross has two pictures, the lit arm right (DoCommandCrossBase_A_00 and
DoCommandCrossBaseGrd_A_00) or down (_B_00), flipped for left and up. Each cross's material has texture bindings of its
own; only the names' strings are shared, so a cross changes picture by taking the other picture's name offsets. No
animation keys these positions, flips or flags, so the layout holds while the prompt animates.

`PANE_NEEDS` and `MATERIAL_NEEDS` are the bytes a pane or material record needs to hold all the fields this reads.

### `diamond()`, `put()`

A field is a float of a pane record, the text origin of a pane (`F_RIGHT`: 1 on its right edge, else 0) or the picture
of a cross (`F_CROSS`: 0 the lit arm right, 1 down). The last two are held as floats too, so that one comparison checks
every field. For `F_CROSS` the field's pane is the cross's pane, whose material has the same name.

`direction[2][4]`: by position the Switch's places (east, south, north, west for A, B, X, Y), by label an Xbox pad's
(south, east, west, north). `place`: 80 units out in each direction. `cross[4]`: the cross lit toward each direction:
its picture and the flips that turn it. `at_button`: the panes at a face button's place: the other buttons' dots in
each group, and each button.

`diamond()`: every field the diamond's layout sets, by label (`label` = 1) or by position; their count. What it sets: x
and y of each pane at a button's place; each button's cross (picture, flips); for X and Y, which may be the left
button, the flip and x of both bubbles and the name's x and origin (`MAX_FIELDS`: 2 * `AT_BUTTON` + 4 * 3 + 2 * 6). A
and B are never the left button: their bubbles open to the right in both layouts. `put()` counts a field past
`MAX_FIELDS` without storing it: `action_prompt_lay_out()` refuses a count it does not expect.

### Reading the layout

`table_find()`, `cross_names()`, `cross_picture()`, `field_find()`, `field_get()`, `same()`.

Every offset is checked against the file's size.

- `table_find()`: the record of the entry `name` of the table whose header slots start at `slot`, with at least `size`
  bytes; 0 when it is not there.
- `cross_names()`: the offsets of the name offsets of the cross `pane`'s picture and gradient (its material's first two
  texture bindings); 0 when they are not there.
- `cross_picture()`: which picture the names at `at[]` give the cross: 0 the lit arm right (A), 1 down (B), -1
  neither.
- `field_find()`: where a field is (`Where`): the offset of its 4-byte word (the pane's flags for `F_RIGHT`), or for
  `F_CROSS` the offsets of the cross's two name offsets. Every offset is checked against the file's size when it is
  found.
- `field_get()`: a found field's value; 0 when it is not one this knows (a cross with another picture).
- `same()`: floats within 1e-3 are the same; the game stores some zeros as -2.8e-14.

### `action_prompt_lay_out()`, `action_prompt_archive()`, `action_prompt_build()`

`action_prompt_lay_out()` finds and reads every field before anything is written; takes the name offsets of both
pictures from the crosses that show them now (the diamond by either layout shows both); writes, keeping each word's
bytes as they were; and reads the layout again as the game will: each field where it was, with its new value. A file
whose fields lie on the records that lead to them (or on each other) reads otherwise; it gets its bytes back, last
written first.

`action_prompt_archive()`: the game's archive holds each layout four times under the same name (628 members, 157
names; the four copies of DoCommand_00.buil are the same bytes, each at its own offset). Every copy is laid out, each
checked first on a copy of its own, so either all of them change or none. Copies that share their bytes are laid out
once; copies that share only some of them cannot be laid out alone. A failure after a trial of the same bytes cannot
happen; the check is kept so that a change here cannot hide one.

`action_prompt_build()`: the frame names its dictionary by id (0: none); `Pack/ZsDic.pack.zs` holds them. Level 3: the
archive unpacks to about 10 MB and is packed again at every start (about 15 ms).

## `runtime/patches/heap_retry.h`

Model heaps that run full: the patch set `heap_retry`'s host functions, its start line and its totals.
`heap_retry_start()` runs after `rt_patches_init()` and `mods_exefs_switch()` (`runtime/boot.c` `rt_boot()`): it reads
the test switches and logs whether the set is on ("heap retry: ..."). `heap_retry_exit_log()` runs at the orderly end
(`runtime/gfx/gfx_present.c` `rt_exit_steps()`): the session's totals, one line.

## `runtime/patches/heap_retry.c`

Model heaps that run full (the patch set `heap_retry`, `recomp/patches-v111.txt`). What it is for, the crash it
replaces and what the log shows: [heap-retry.md](../heap-retry.md).

The game's "OptimizeChangeBank" thread rebuilds a model's material block in `main+0x4366e0` (x0 the block, x1 a request
kept at its [sp + 0x38], whose +8 names the heap for the block's CPU parts). That memory comes from small heaps of the
model's own, children of larger ones, and none of these allocations is checked:

- two CPU buffers from the heap's tryAlloc (vtable +0x30, the size in x21 and x22, alignment 8), called at
  `main+0x43690c` and `main+0x436da0`. Each result is the base of a bump allocator at block +0xc8 and +0xa8 (+0xd0 and
  +0xb0 the bytes used, +0xd8 and +0xb8 the size). A null base makes the bump allocator hand out guest address 0, and
  the next store, in `main+0x42cb30` (called at `main+0x436d44`), writes guest address 0x8;
- the GPU memory, `main+0x502050` called at `main+0x436f18` and `main+0x437094` (x0 the memory block, x1 a request: +0
  the count, +8 the heap, +0x20 the name of what it is for; x2 the size of one). A failure with a heap named goes on
  with no memory pool. `main+0x502050` leaves nothing allocated when it fails, so it can be called again.

A null result at these four sites is now tried again in the heap's parents up to the root, and the GPU memory last in
the game's default heap (the one `main+0x502050` takes for a request without a heap). The first heap with room gives
the block. A block goes back where it came from: the material block's destructor (`main+0x91f940`) frees +0xa8 and
+0xc8 through `main+0x1fdea0`, the game's free by address, which looks for the heap whose range holds the block (it
reads +0x28 and +0x30); the GPU memory's release (`main+0x5020f4`) goes through the same function. This was read from
the code, not traced in a run. When no heap has room, the game ends with a fatal error that says so, instead of the
write to guest address 0x8.

A heap here: +0 its vtable, +0x20 its name, +0x28 its memory, +0x30 the memory's size, +0x38 its parent; vtable +0x30
tryAlloc(heap, size, alignment). A parent is used only when the child's memory lies inside the parent's, so a different
layout ends the climb instead of calling through a wrong pointer.

Sites: `main+0x436910` and `main+0x436db4` keep their store of the size and then call the page's +0xed0
(`heap_retry_cpu()`), which mends x0 before the function's own store of the base at the next word. `main+0x436f18` and
`main+0x437094` call the page's +0xec0 (`heap_retry_gpu()`) in place of `main+0x502050`, and it calls the game's.

Switches, read once at the start: `NX2SARIA_HEAP_RETRY=0` turns the set off (the game as shipped).
`NX2SARIA_HEAP_RETRY_TEST_FAIL=<cpu>[,<gpu>]` is a test: the first `<cpu>` CPU buffers and `<gpu>` GPU memories at
these sites are refused, as a full heap would refuse them, so the retry runs. A refused CPU buffer stays allocated in
the model's heap: the test makes that heap fuller and never frees memory behind the game's back.
`NX2SARIA_HEAP_RETRY_TEST_SHIPPED=1`, with it: a refused allocation goes on as the shipped game does, which reproduces
the crash.

| Constant | Value | Meaning |
|---|---|---|
| `CPU_RET_C8` | 0x436914 | the return address after `main+0x436910`: the buffer at block +0xc8, its size in x21 |
| `CPU_RET_A8` | 0x436db8 | after `main+0x436db4`: the buffer at block +0xa8, its size in x22 |
| `CPU_CALL_C8`, `CPU_CALL_A8` | 0x43690c, 0x436da0 | the game's tryAlloc calls those sites follow |
| `GPU_RET_A`, `GPU_RET_B` | 0x436f1c, 0x437098 | after `main+0x436f18` and `main+0x437094` |
| `GPU_ALLOC` | 0x502050 | the game's GPU memory allocation |
| `REQUEST_SLOT` | 0x38 | `main+0x4366e0` keeps its x1 at [sp + 0x38] |
| `HOST_CPU`, `HOST_GPU` | 0xed0, 0xec0 | patch page slots |
| `MAX_CLIMB`, `LOG_BUDGET`, `LOG_EVERY` | 8, 32, 64 | the climb's depth and the log's limits |

`set_index` is the set `heap_retry` in this build, -1 when it has none; `shipped` is
`NX2SARIA_HEAP_RETRY_TEST_SHIPPED`; `test_left[2]` the refusals left of `NX2SARIA_HEAP_RETRY_TEST_FAIL`, for CPU
buffers and GPU memory; `moved_n` and `moved_bytes` the blocks a parent heap gave, per site; `full_n` and `test_n` the
refusals by a full heap and by the test; `kept_n` the refusals that went on as shipped; `told` the retry lines logged.

### `own64()`, `test_refuse()`, `heap_usable()`, `parent_of()`, `describe()`, `note_moved()`, `note_kept()`

- `own64()`: a word the game's own code reads at the site or right after it: the function's stack slot and its request
  (`main+0x436918` and `main+0x43691c` load both again; `main+0x502050` loads the request's +0 and +8). They are read as
  they are, without `mem_readable`, which answered 0 for the game thread's stack in a bench run (the stack the game
  hands CreateThread).
- `test_refuse()`: 1 while the test switch still refuses this kind (0 the CPU buffers, 1 the GPU memory); the count
  stops at 0.
- `heap_usable()`: a heap the climb may call: readable, with a tryAlloc in its vtable.
- `parent_of()`: `h`'s parent, or 0 at the root or when the parent's memory does not hold `h`'s.
- `describe()`: `0x... "name" (N KB)`: a guest C string of printable characters, else "?".
- `note_moved()`: a block a parent heap (or with `from` 0, the game's default heap) gave in place of the full one.
- `note_kept()`: a refusal that goes on as shipped (`NX2SARIA_HEAP_RETRY_TEST_SHIPPED`).

### `heap_retry_cpu()`, `heap_retry_gpu()`

`heap_retry_cpu()`: the page's +0xed0, right after the tryAlloc at `main+0x43690c` or `main+0x436da0`: x0 its result,
which the function stores as the buffer's base at the next word. A null one is tried in the heap's parents. Only the
set's two sites call the slot. Under the test the block stays in the model's heap, as allocated.

`heap_retry_gpu()`: the page's +0xec0, in place of the call of `main+0x502050` at `main+0x436f18` or `main+0x437094`:
the game's call, and when it fails with a heap named, the same call with each of that heap's parents and then the
game's default heap. A request without a heap already used the default one: the function's own check after the call
handles that.

### `heap_retry_start()`, `heap_retry_register()`

`site_words`: the four sites' words as 1.1.1's main has them (`recomp/patches-v111.txt`). The generator checked them in
the executable it lifted; the image loaded now is checked too, so a main that differs keeps the game's own code. Code
the runtime keeps unmapped is skipped: the generator's check stands. The hosts are registered only for 1.1.1: the set's
sites are in 1.1.1's main only.

## `runtime/patches/changebank_hold.h`

The neighbouring map the game preloads, held: the rule, apart from the guest so that
`tests/changebank_hold_check.c` runs it. Why it exists and what it measured: [changebank-hold.md](../changebank-hold.md).

The game's "OptimizeChangeBank" manager preloads one neighbouring map at a time. Each update chooses one of its 16
units: among those whose load radius Link is in, the one he moves towards, else the nearest; with none, a unit the game
asked for directly. The update then asks for the chosen unit's map and lets go of every other unit it had asked for,
and asking for a map other than the one loading or loaded throws that load away and starts the new one. Where two radii
overlap (the Market's square lies in Hyrule Field's and Hyrule Castle's), walking or turning flips the choice, and every
flip is a cancelled load and a whole new one.

`cb_step()` takes the game's choice of this update and returns the one the update goes on with:

- the game's own when there is nothing held yet (the first choice), when the game's came from a direct request, when it
  names the map already held (another unit of the same map takes the hold over: no load is cancelled), when the held
  unit is no longer in use or holds another map now, after a gap in the updates (a loading screen, the manager of
  another area) and with a hold of 0;
- otherwise the held unit, until the game's choice has named other maps than the held one (or no map) for the whole
  hold, counted from the first update that left it: then the game's choice of that update is taken.

A candidate that goes back to the held map before the hold is over is a pair of loads the game did not throw away and
make again ("absorbed").

`changebank_hold_start()` runs after `rt_patches_init()` and `mods_exefs_switch()` (`runtime/boot.c` `rt_boot()`): it
reads `NX2SARIA_CHANGEBANK_HOLD_MS` and logs whether the set is on ("changebank hold: ..."). At the orderly end
(`runtime/gfx/gfx_present.c` `rt_exit_steps()`) `changebank_hold_exit_log()` writes the session's totals, one line.

`CB_GAP_NS` (500000000): updates further apart than this start the hold again (a loading screen, a pause).

- `CbUnit`: `in_use`: the unit's +0 is 1; `map`: FNV-1a of its map's path, 0 when it names none.
- `CbState`: `mgr` the manager the state is for; `last_ns` the last update seen; `held` the unit the update goes on
  with, -1 none; `have_cand`: the game's choice has left the held map and is being held back; `cand_map` the map it
  named at the last update (0: no map); `cand_since` the first update that left the held map; `game_last_map` the
  game's own choice's map at the last update, for the flip count; `asked_last_map` the map of the choice returned at the
  last update, for the asked count.
- `CbStats`: `updates` seen (the normal mode's choice); `flips`: the game's own choice named another map than at the
  update before (not across a gap); `holds`: choices of another map held back (each map named in turn while away, once);
  `absorbed`: times the choice came back to the held map before the hold was over; `taken`: times the hold was over and
  the game's choice was taken; `held_updates`: updates that went on with the held unit instead of the game's choice;
  `resets`: holds started again (a gap, another manager, the held unit gone); `asked`: the choice returned named another
  map than at the update before: each one a request for another map, which starts a load unless that map is loaded
  already (with a hold of 0, the same as `flips`).

### `cb_pick()`, `cb_step()`

`cb_pick()`: the choice the update goes on with. `game`: the game's (-1 none, else 0..15); `direct`: it came from a
direct request. The hold counts from the first update whose choice left the held map, whichever other maps it names
since: a choice that flips between two other maps is taken after the hold all the same.

`cb_step()` is `cb_pick()` and the asked count: the returned choice's map against the last update's of the same
manager.

## `runtime/patches/changebank_hold.c`

The patch set `changebank_hold` (`recomp/patches-v111.txt`). The rule is `runtime/patches/changebank_hold.h`'s; this
file reads the guest and logs.

The manager ("OptimizeChangeBank", vtable `main+0x4ba1a28`, made by `main+0xd12c00`) updates each frame through its
vtable's +0x30, `main+0x3299880` (x19 the manager). Its units are 16 of 0x128 bytes from +0x408: +0 in use (1), +2
asked for directly, +4..+0xc the place, +0x10 the map's path (a pointer the constructor sets to the unit's own buffer at
+0x1c, +0x18 that buffer's size, 0x100), +0x120 the load radius, +0x124 the priority. The update's loop
(`main+0x3299c54`..`0x3299d34`) leaves the best unit of those whose radius Link is in in w15 (by the heading while Link
moves, else the nearest) and the direct one in w17 (-1 each for none), and `main+0x3299d90`..`0x3299d9c` take w21 = w15
< 0 && w17 >= 0 ? w17 : w15 (`cmp w15, #0; fmov s0, #4.0; ccmn w17, #1, #4, lt; csel w21, w17, w15, gt`). From there
the update asks for unit w21's map (`main+0x329a540`, which does nothing when that map is the one asked for, manager
+0x1e8, or loaded, +0x70, and otherwise starts it over the one loading) and lets go of every other unit it had asked for
(`main+0x329a400`, unit +1 cleared).

The site is the loop's `add x10, x10, #1` at `main+0x3299d28` (`SITE`, word 0x9100054a), which every pass of the loop
goes through. It keeps the add and then calls the page's +0xe30 (`changebank_hold_host()`), which returns at once until
x10 is 16 (the last pass) and then may put the held unit in w15. The `cmp w15, #0`, `ccmn` and `csel` after the loop
then take it: with w15 0 or more the `ccmn` sets Z and the `csel` takes w15, and with w15 -1 the update's choice is w17
or -1 as before. The host changes w15 only when the game's choice is w15 or none, never for a direct request (w15 -1,
w17 0 or more), so the `csel`'s choice is the new w15. No flags are live at the site: the `cmp x10` two words on writes
them before anything reads them, and a patched call may not keep flags live across it (`recomp/flags.py` counts a call
as no definition). x10 and x15 are caller-saved, so the lifted code reads them from the `Cpu` again after the call
(`recomp/regs.py`); x30 is the update's own, saved in its prologue and read again only in its epilogue; nothing else
changes.

Every update the host checks the manager before it reads it: the vtable, every unit's in-use byte (0 or 1) and its
path pointer and buffer size as the constructor set them. A manager that does not match gets the game's own choice at
that update (counted, and logged once), and so does any update whose w15 or w17 is not a unit or -1.

Switches, read once at the start: `NX2SARIA_CHANGEBANK_HOLD=0` turns the set off (the game's own code at the site).
`NX2SARIA_CHANGEBANK_HOLD_MS` (default 2000, `DEFAULT_HOLD_MS`): how long the game's choice must have left the held map
before it is taken; 0 (or a negative number) gives the game's choice at every update, with the counts still kept, so
the log shows how often the game's own choice flips.

`SITE_RET` (`SITE` + 4) is the `bl`'s return address, after the kept add. `HOST_SLOT` is 0xe30. `EVENT_BUDGET` (8): the
"held ... for N ms" lines a session. `stats` and `logged` are under the lock; `logged` is what the last periodic line
said. `held_name` and `cand_name` are the held map and the last one held back, for the event lines.

### `read_units()`, `map_name()`, `changebank_hold_host()`

- `read_units()`: the units as `changebank_hold.h` takes them, or 0 when the manager is not laid out as the constructor
  makes it.
- `map_name()`: the map's file name, from the last '/' of its path, cut to the buffer.
- `changebank_hold_host()`: the page's +0xe30, after `main+0x3299d28`'s add at each pass of the loop over the units:
  x10 the passes done, and after the last one (16) w15 and w17 what the update chooses from; x19 the manager. A pass
  before the last one returns at once. w15 is never written for a direct request: `cb_step()` gives the game's choice
  back. The host is registered only for 1.1.1: the set's site is in 1.1.1's main only.

## `runtime/patches/camera_clearance.h`

The camera's clearance from walls on pictures wider or narrower than 16:9: the patch set `camera_clearance`'s host
function, its start line and its totals. `camera_clearance_start()` runs after `rt_patches_init()` and
`mods_exefs_switch()` (`runtime/boot.c` `rt_boot()`) and logs whether the set is on ("camera clearance: ...").
`camera_clearance_exit_log()` runs at the orderly end (`runtime/gfx/gfx_present.c` `rt_exit_steps()`): what the two
sites loaded, one line each.

## `runtime/patches/camera_clearance.c`

The patch set `camera_clearance` (`recomp/patches-v111.txt`).

The camera's collision step (`main+0x71a4a0`) sizes the sphere it keeps clear of walls from its near plane: h = tan(fov
/ 2) * near, w = h * ratio and sqrt(w * w + h * h), plus a margin, at least a minimum (`main+0x71a84c`.. and its second
path at `main+0x71b620`..). The ratio is a camera parameter it loads from its object's +0x51c at `main+0x71a844` (into
s10) and `main+0x71b624` (into s14), copied there from the camera's parameters (`main+0x478798`). Where the game's
camera works with the picture's own ratio, a 32:9 picture makes the sphere about 1.8 times its 16:9 size, and the camera
cannot follow Link into a crawl passage.

The sites keep their load and then call the page's +0xe00 (`camera_clearance_host()`). When the picture is not 16:9
(the page's +116, `runtime/patches/widescreen.c`) and the loaded value is that picture's ratio (within 0.1%), it is
replaced by 16:9 (the page's +112) for this sum alone; any other value stays. The register written is the load's own,
which the code after it reads; nothing reads the flags between the sites and the call that follows each
(`main+0x71a878`, `main+0x71b64c`), and x30 is the function's own, saved in its prologue.

The log counts the loads by what they held (16:9, the picture's ratio, other), so a session shows whether the game's
camera ever sees the wide ratio here: this port's own widescreen keeps the camera's parameters at 16:9 and widens only
the projection (the set `wide_aspect`), so with it the sites are expected to see 16:9.

`NX2SARIA_CAMERA_CLEARANCE_FIX=0` turns the set off.

`SITE_A` (0x71a844) is `ldr s10, [x19, #0x51c]` (word 0xbd451e6a), `SITE_B` (0x71b624) is `ldr s14, [x19, #0x51c]`.
`HOST_SLOT` is 0xe00.

### `page_f32()`, `near_ratio()`, `camera_clearance_host()`

`page_f32()` gives 0 without the page (no patch table: the sites are not there either). `near_ratio()`: 1 when `a` and
`b` differ by at most `rel` of `b`. In `camera_clearance_host()` only the set's two sites call the slot, and the upper
lanes of the register are written zero, as a 32-bit load leaves them.

## `runtime/patches/sprint.h`

The sprint button: the patch set `sprint`'s host functions and the button's input.

- `sprint_pad()` is called with the buttons of every controller poll the game gets (`runtime/hle/ov_input.c`
  `npad_states()`): it notes the sprint button's presses and keeps it from the game during gameplay.
- `sprint_step()`: one update of Link's sprint: `kind` is how his state treats it, `button` and `hold` the settings. 1
  while on.
- `sprint_available()`: 1 when the game version has the patch set `sprint` and nothing keeps it off
  (`NX2SARIA_SPRINT_BUTTON_PATCH=0`, a mod patching its sites): the Controls tab shows the sprint rows then. The set
  itself runs only while a button is chosen.

## `runtime/patches/sprint.c`

The sprint button (the patch set `sprint`, `recomp/patches-v111.txt`; [controls.md](../controls.md)). Link sprints
while his input flags hold the dash input (bit 4 of the word at *(Link +0x50)). The set's two sites call the host
functions right after the game loads those flags, and they OR the bit in while the sprint button's sprint is on, so the
game's own sprint runs: its speed, animation, camera and every rule that ends it.

Settings (live): `sprint_button` picks the button (off, b the game's B button, the bottom face button of the Nintendo
layout, or l3 the left stick's click, whose only other use hides the text box in a conversation); `sprint_mode` is
toggle (a press starts the sprint, the next press or Link's state ends it) or hold (sprint while the button is held).
During gameplay the button is kept from the game (`sprint_pad()`, called by `runtime/hle/ov_input.c` `npad_states()`),
so it does not also fire its own action. B is kept only for a press that begins while Link moves (Walk, Jump, Land,
Fall, Clamber), and then until it is released, so it puts the weapon away as usual otherwise and the game never sees
half a press. Presses made in a menu, a cutscene or a load (no update of Link within `GAMEPLAY_POLLS` polls) start
nothing: B closes the pause menu, and that press is the menu's. Nor do presses while an event runs
(`event_running()`): Link's update goes on during a conversation, where L3 hides the text box, so that press is the
game's too. Under the game's Swap Jump Controls B is the jump, which a B sprint button keeps from the game while Link
moves: L3 is the one to use then.

Patch page slots: the set's sites call +0xf40 and +0xf30 ([the slots](#the-patch-pages-host-slots)).

Link's state: the id at [[[[Link +0x18] +0x270] +0x188] +0x1c0] +0x28 (+0x4c when that reads 0x11a, a state that wraps
another), the same field `main+0x4aed50` compares with 1 (Walk) before the game starts a sprint.

`button_bits` are HidNpadButton bits (B: bit 1; L3: bit 4); `BUTTON_ZL` is bit 8. `LINK_VTABLE` (0x4b4dfc0) is the main
offset of Link's actor class. `GAMEPLAY_POLLS` (8): the button is kept from the game while Link's update ran within
this many polls. `WAIT_STOP_FRAMES` (4.0): standing still this long ends a toggled sprint that has moved, or this long
after a press while standing, which has not moved yet. It is in 60 Hz frames: above 60 fps Link updates once a rendered
frame, each step `frame_step_frames()` of them, so counting updates would end the sprint on any short stop at 240 fps
(four updates are 17 ms there).

What Link's state does to a sprint: MOVE keeps it (and is when the bottom button sprints), KEEP keeps it, WAIT ends it
after a few frames, STOP ends it at once. A state not listed stops it too.

| State id | Name | Kind |
|---:|---|---|
| 1 | Walk (also running) | MOVE |
| 13 | Jump | MOVE |
| 32 | Land | MOVE |
| 33 | Fall | MOVE |
| 68 | Clamber | MOVE |
| 253 | StepOn | KEEP |
| 35 | Slide | KEEP |
| 36 | SlideLand | KEEP |
| 99 | Rolling: a roll keeps a toggled sprint, which goes on when it ends | KEEP |
| 40 | Wait | WAIT |

The state between the two threads: `pressed`, the button's presses since Link's last update (`sprint_pad()` to
`sprint_link()`); `held`, the button held at the last poll; `zl_held`, ZL (targeting) held at the last poll; `polls`,
the polls since Link's last update (none yet: not gameplay); `moving`, Link's state was a MOVE one at his last update;
`active`, the sprint is on: the dash input goes in; `latched`, the press now held began in gameplay (and, for B, while
Link moved): kept from the game. `moved` (the sprint has moved since it started), `wait_frames` (60 Hz frames stood
still) and `last_button` (the button setting at the last update) are `sprint_link()`'s only.

### `ld64()`, `is_link()`, `event_running()`

- `ld64()`: guest reads through Link's pointers: a stale pointer in the chain reads 0 instead of faulting.
- `is_link()`: 1 when x19 is the Link the player controls. Link's class has one more instance the player does not
  control: the game's actor manager ([[`main+0x4c7e0d0`]], class `main+0x4bdfe68`) names the controlled one at +0x470
  (class `main+0x4b49698`), whose +0x18 must be Link's own +0x18.
- `event_running()`: an event (a conversation, a cutscene) is running: the event manager ([[`main+0x4c7e090`]],
  `EVENT_MGR`) has an active event at +0x68, the field the game's SeqQueryIsEventRunning reads (`main+0xbcc100`).

### `sprint_available()`, `sprint_pad()`, `sprint_step()`

`allowed`: the set runs only while a button is chosen, so with the sprint button off its sites cost nothing. Whether it
may run at all is read once, after the start: `NX2SARIA_SPRINT_BUTTON_PATCH=0` or a mod whose code patches the same
sites (`runtime/patches/mods.c` `mods_exefs_switch()`) leaves it off then, and switching it on later must not undo that.

`sprint_pad()`: with the button off the sites stop calling in, so nothing may stay on for when a button is chosen
again. A press is decided at its start, for its whole length: kept from the game, and the sprint's, when it begins in
gameplay (B: while Link moves); else the game's alone, and no sprint's. A press an event takes (a conversation's L3
hides its text box) is logged for the first 16.

`sprint_step()`: the sprint for this update of Link, from his state and the button's presses: 1 when it is on.

### `sprint_link()`, `sprint_stance()`

`sprint_link()`: `main+0x4ae798`, after `ldr x10, [x10]`: x10 holds Link's input flags for the sprint's start and keep
tests. `sprint_stance()`: `main+0xc07f3c`, after `ldrb w8, [x8]`: the same flags' low byte for the stance and camera
update. Both are registered only for 1.1.1: the set's sites are in 1.1.1's main only.

## `runtime/patches/text_speed.h`

The patch set `text_speed`'s page words and switch.

- `text_speed_poll()` is called at every controller poll the game makes (`runtime/hle/ov_input.c` `npad_states()`): it
  puts the `text_speed` setting on the patch page and switches the set, so a change in the menu reaches the text that
  types next.
- `text_speed_available()`: 1 when the game version has the patch set `text_speed` and nothing keeps it off
  (`NX2SARIA_TEXT_SPEED_PATCH=0`, a mod patching its sites): the Game tab shows the row then. The set itself runs only
  while the setting is not normal.
- `TEXT_SPEED_FAST_FACTOR` (3.0): the factor the fast setting multiplies the message window's typing speed by.

## `runtime/patches/text_speed.c`

Text speed (the patch set `text_speed`, `recomp/patches-v111.txt`). The message window shows its text through a
typewriter whose speed and waits the message's own tags set, and a press of the decide button while it types shows the
page at once, unless the window marked the message unskippable (a message that turns its page by itself on a timer, and
some window types). The set's two sites read the patch page words this file writes:

| Word | Holds |
|---|---|
| +0x200 | the typewriter of the message window while its message may be skipped, 0 otherwise (written by the game's code at the first site, read at the second) |
| +0x208 | the fast factor less one, as a float: 0 leaves the speed as it is |
| +0x20c | 1 for instant: the window's skip runs on the first frame of each page, as if the button were pressed |

Setting `text_speed` (live): normal (the game's, and the set off), fast (the skippable messages type
`TEXT_SPEED_FAST_FACTOR` times as fast, their scripted waits as long as before) or instant (the skippable messages show
each page at once, as the game's skip does). Unskippable messages keep the game's speed under both.

### `text_speed_available()`

Whether the set may run at all is read once, after the start: `NX2SARIA_TEXT_SPEED_PATCH=0`, or a mod whose code
patches the same sites (`runtime/patches/mods.c` `mods_exefs_switch()`), leaves it off then, and switching it on later
must not undo that.

## `runtime/patches/foliage_distance.h`

The foliage's draw distances as data the tests can reach without the game: `foliage_distance_init()`, the address
(`FOLIAGE_LOD_OFF`, main+0x3eefd38), the game's eight values, their caps, and the two pure functions.

- `foliage_game`: the defaults of the game's foliage LOD settings (`game::foliage::ResLodSetting`), in metres, in the
  game's order: undergrowth 0 (40), undergrowth 1 (60), ornaments (35), the low trees' shoots and leaves (30), the low
  trees' shadows (40), the low trees (135), and the ranges between the low trees' detail levels 0 to 1 (10) and 1 to 2
  (45). They sit in main's read-only data.
- `foliage_cap`: the ornaments at most 75 m and the low trees' leaves at most 70 m, unless the game's own value is
  larger; further out both were seen to flicker. 0: no cap.
- `foliage_factor()`: the setting as a factor: 0 and 1 for unset or a number from `FOLIAGE_MIN` (0.5) to `FOLIAGE_MAX`
  (3), 1 and the nearer bound for a number outside them, -1 and 1 for anything else.
- `foliage_scale()`: the eight values times the factor, capped; returns how many were capped.

## `runtime/patches/foliage_distance.c`

Setting `foliage_distance` (restart; issue #100): how far out the game draws grass, bushes, flowers, stones and low
trees. The game reads the eight defaults from main's read-only data at every area entry, for each area that has no
distances of its own (most of them: Hyrule Field, Kakariko, Lake Hylia, the Market; the Graveyard, Zora's River and
Zora's Domain, and the Hyrule Castle ornaments have their own and keep them), once for the docked and once for the
handheld mode, but its foliage system builds its distance tables once a run, at the first area entry. So the values
are written once, before the guest starts (`runtime/boot.c` `rt_boot()`), and a change waits for the next start. Only
data changes: no shader, no actor's range. It is not the short grass simulated around Link, which another system
draws with its own distances.

### `foliage_distance_init()`

Writes nothing for 1, for a version other than 1.1.1, or when the address does not hold the game's eight values (an
ExeFS mod or a cheat that changed them keeps its own). Otherwise the page is made writable for the copy and read-only
again (`write_main()`, as `runtime/patches/cheats.c` `wr_main()` does). One `foliage:` line in every case: the factor,
the game's values and the ones now in use, or why nothing was written, and a value the setting could not take. The
lifted code loads these floats from memory at every use (the generator folds no load), so the written values are the
ones the game reads. `tests/foliage_distance_check.py` builds the file with stand-ins; with `--exefs` it also reads the
values from the game's main.

## `runtime/patches/amiibo_daily.h`

The patch set `amiibo_daily`'s switch.

- `amiibo_daily_poll()` is called at every controller poll the game makes (`runtime/hle/ov_input.c` `npad_states()`):
  it switches the set to follow `NX2SARIA_AMIIBO_DAILY_LIMIT`, so a change in the menu reaches the next figure the game
  reads.
- `amiibo_daily_available()`: 1 when the game version has the patch set `amiibo_daily` and nothing keeps it off
  (`NX2SARIA_AMIIBO_DAILY_PATCH=0`, a mod patching its site): the Game tab shows the row then. The set itself runs only
  while the setting is 0.

## `runtime/patches/amiibo_daily.c`

The amiibo daily limit (the patch set `amiibo_daily`, `recomp/patches-v111.txt`). A figure that gives random items
([amiibo.md](../amiibo.md) names them) gives them once a day; after that the game says "You cannot use this amiibo
anymore today" (SimpleInfo_00 CannotUseAmiiboToday). The rule is the field EnableDailyLimit of each drop table in the
game's amiibo table: the two mask figures' tables set it false, the others leave it out, and a table without it keeps
the limit. The set's one site makes the game read false for every table it matches a figure with.

Setting `amiibo_daily_limit` (live): 1, the game's rule (the set off), or 0 (the set on). The game reads the field each
time it reads a figure, so a change applies to the next figure.

### `amiibo_daily_available()`

Whether the set may run at all is read once, after the start: `NX2SARIA_AMIIBO_DAILY_PATCH=0`, or a mod whose code
patches the same site (`runtime/patches/mods.c` `mods_exefs_switch()`), leaves it off then, and switching it on later
must not undo that.

## `runtime/patches/boot_map.h`

`boot_map`: the game starts straight in a chosen area from the save of a slot, without the title screen and its file
menu ([boot-to-map.md](../boot-to-map.md)).

- `boot_map_init()`, before the guest starts (`runtime/boot.c` `rt_boot()`): reads the settings, checks the destination
  and the save, and arms the changes to what the game reads. It logs one line either way.
- `boot_map_save_open()`: `runtime/hle/fs_host.c`, when the game opens a save file to read (guest path): 1 when its
  reads get the boot map's place (the slot's progress.sav while the boot map is pending), 0 to read it as it is. `f` is
  the opened file.
- `boot_map_save_read()`: `fs_host.c`, after a read of `n` bytes at `offset` from a file `boot_map_save_open()` took.
- `boot_map_tick()`: the game's thread, at each pad read (`runtime/hle/ov_input.c`): ends the boot map once Link is in
  the world, with its log line.

## `runtime/patches/boot_map.c`

`boot_map = <area>[:<start position>]`: the game starts in that area from the save of a slot, without the title screen
and its file menu. [boot-to-map.md](../boot-to-map.md) has the research behind it and its risks.

Nothing is called in the game and no game code is patched. The setting changes what the game reads, while it is
pending:

- `Sequence/NormalTitle.module.ainb`: the title's graph starts with what Continue and then Yes start
  (TitleToMainWithLoadSave), so the title's buttons and the file menu never show (`runtime/hle/romfs.c`
  `romfs_read_patch()`);
- `Sequence/OnLoadGameSaveData.module.ainb`: the slot loaded is a constant instead of the file menu's choice;
- `slot_NN/progress.sav`: the area, Link's position and heading, and on request his age and the time of day are the
  destination's (`runtime/hle/fs_host.c` calls `boot_map_save_open()` and `boot_map_save_read()`). The file on disk
  stays as it is.

The game's own code then loads the save and the area in its own order. The first pad read with Link in the world ends
it: the log says where he is, and the changes stop.

Settings, read once at start: `boot_map`, `boot_map_slot` (0..5 or latest), `boot_map_age` (child, adult or save),
`boot_map_time` (HH:MM or save).

`state` is `BM_OFF`, `BM_PENDING` or `BM_DONE`; `BM_PENDING` only once every change is armed. In `bm`: `area` and
`start` are the destination, the area and the name of its start position ("" its default); `slot` the slot asked for
(0..5) and `file_slot` the `slot_NN` it loads (+6 in Master Quest). `save_edit` (under `save_lock`) holds the edits of
the save file the game opened.

### `mq_mode()`, `latest_slot()`, `arm_graph()`

- `mq_mode()`: the account's Master Quest mode (account.sav IsLastSelectedMQMode): the game loads slot N + 6 then
  (SeqExecuteLoadSaveData, `main+0x32c1db0`).
- `latest_slot()`: the slot whose file-menu card has the newest date (the game's own latest,
  SeqQueryGetLatestSaveIndex), -1 none.
- `arm_graph()`: one of the sequence graphs: its edits found in the original file and armed in `romfs.c`. 0 with why.

### `boot_map_init()`

- The slot, and its save: the one the game would load next (a waiting import is that save already).
- Age and time: an area whose name has an age makes Link that age whatever the save says (LoadBancImpl): the save says
  it too.
- The save must hold what is changed in it: the edits are tried now on the file the game will read.
- The title's graph and the slot's are armed last, so that a refusal above leaves the RomFS as it is.

### `boot_map_save_open()`, `boot_map_tick()`

`boot_map_save_open()` makes the edits from the bytes this open reads, which a commit since the start may have changed.
The first open that has every value sets the edits, which every handle reads by. A later open is changed only when its
file gives the same edits (a commit in between may have moved the values); otherwise it reads its file as it is, and
the handles of the first keep theirs.

`boot_map_tick()`: the title has no player; he comes with the area the save loads. With no arrival in 300 s the changes
stop, so that a return to the title is the game's own again.

## `runtime/patches/boot_map_core.h`

The parts of `runtime/patches/boot_map.c` that only read and change bytes, apart so that `tests/boot_map_check.c` runs
them on made-up files: the two sequence graphs (AINB) the title runs, the save file, the file menu's card, the
setting's values. [boot-to-map.md](../boot-to-map.md) says what each change is for.

`BmEdit` is one change of bytes: at `off`, `n` bytes (at most 64) become `repl`; `orig` is what the file held there.

### `bm_hash()`

The hash the game knows a game data value by: MurmurHash3 (x86, 32 bits, seed 0) of its name. Its tail `switch` falls
through from 3 bytes to 2 to 1 (`__attribute__((fallthrough))`).

### AINB

`bm_ainb_open()`, `bm_ainb_str()`, `bm_ainb_find()`, `bm_ainb_children()`.

AINB, version 0x408. Header: +0xc the commands' count, +0x10 the nodes', +0x24 the string pool (names are offsets into
it), +0x34 the input and output parameters (twelve offsets: inputs, outputs of int, bool, float, string, vec3f, ptr).
The commands follow at +0x74, 0x18 bytes each: the name, a GUID, the root node (u16 at +0x14). The nodes follow, 0x3c
bytes each: type (u16; 2 Sequential, 3 Simultaneous), name at +8, body at +0x14. A body has at +0x30 per parameter type
{first input, inputs, first output, outputs} and at +0x90 ten {count, first} byte pairs of links, kind 2 the children,
whose offsets follow at +0xa4 (each link starts with the node's index). An int input is 0x10 bytes: name, the node it
comes from (s16, -1 none), that node's output (u16), flags, the value; every input without a link in the game's graphs
has flag 0x800000.

- `bm_ainb_str()`: the pool's text at `off`, NULL when it runs past the file.
- `bm_ainb_find()`: the only node named `name`, or -1 (none, or more than one).
- `bm_ainb_children()`: the children of node i (its links of kind 2), at most `max`; their number, -1 when the node's
  body runs past the file.

### `bm_ainb_title_edit()`, `bm_ainb_slot_edits()`

`bm_ainb_title_edit()`, NormalTitle.module: its command Root starts the title (node 35 in 1.1.1, a Simultaneous that
shows the title and waits for a button). The edit makes it start the Sequential whose children are the Simultaneous of
the scene transition's pause and TitleToMainWithLoadSave.module: what Continue and then Yes in the file menu start.

`bm_ainb_slot_edits()`, OnLoadGameSaveData.module: SeqExecuteLoadSaveData and SeqOneShotSetCaptionMgrLatestIndex take
their Index from the query of CaptionData.SelectedSavePathIndex, which the file menu sets. The two edits make each Index
a constant, the slot: no link (-1), flag 0x800000, the value.

### Save files

`bm_save_find()`, `bm_caption_date()`, `bm_save_edits()`, `bm_edits_same()`, `bm_edits_apply()`.

Save files (progress.sav, caption.sav, account.sav): u32 0x01020304, the version, the data's offset; from 0x28 up to
that offset {u32 name hash, u32 value} pairs, where {0, k} starts the values of type k (0 Bool, 2 Int, 4 Float, 10
Vector3, 16 String64, 24 UInt64, ...). Bool, Int and Float values are the pair's own; Vector3, String64 and UInt64
values are the offset of 12, 64 and 8 bytes in the file.

- `bm_save_find()`: the place of a value: the pair's offset; `*value` its second word. 0 when the file has no such
  value.
- `bm_caption_date()`: the file menu's date of a card (caption.sav) as one number that grows with it, 0 without one.
- `BmPlace`, what a boot map changes in the save it loads: `area` (Sequence_CurrentBanc); `pos[3]` and `yaw`
  (PlayerStatus.SavePos, PlayerStatus.SavePosRadY); `child` (Sequence_IsChildhood: 1 child, 0 adult, -1 the save's);
  `day_seconds`, the clock's seconds of the day (UInt64 ef10b80f, low word), -1 the save's.
- `bm_save_edits()`: the edits that make the save `s` (`n` bytes) start at place `p`, at most 8. Their number; 0 with
  why when the save lacks one of the values. DungeonRoom, the save's room of a dungeon, becomes -1 (none) whatever the
  area: Link starts at another place than the save's, in another room or another area. The clock is {u32 seconds of the
  day, s32 day}; its name is not known, only its hash 0xef10b80f.
- `bm_edits_same()`: whether two lists of edits change the same bytes into the same values (only the bytes in use
  count: the rest of `orig` and `repl` holds nothing).
- `bm_edits_apply()` applies edits to the `n` bytes read at offset `off`.

### `bm_yaw()`, `bm_day_seconds()`, `bm_area_age()`

The setting's values.

- `bm_yaw()`: the heading a start position's Rot (radians about x, y, z; R = Rz Ry Rx) gives Link: atan2 of its forward
  vector's x and z (R * (0, 0, 1)), as SeqCalcSavePlayerForward turns a heading back into a forward vector. Rot (pi, t,
  pi) faces pi - t.
- `bm_day_seconds()`: "HH:MM" (24 hours) as seconds of the day; -1 when it is not one.
- `bm_area_age()`: the age an area's name gives Link (1 child, 0 adult), -1 for an area both ages use: the game's
  overworld areas end in _Child or _Adult (_Adult_Alternative too), and its castle grounds, HyruleCastle, are the
  child's.

## `runtime/patches/voice_language.c`

The 1.1.1 voice resource refreshes its language when the game data reports an option write. A saved option can arrive
without that notification. The host follows the stored value through the game's enum getter (`main+0x7e73d0`) and lets
the original update (`main+0xa7ca30`) keep choosing and loading the voice resources.

### `voice_language_host()`

The call site (`main+0xa7ca98`) holds the voice resource mutex. The continuation uses only w0 and callee-saved
registers from here. Both guest calls preserve those registers, and `rt_call` keeps the caller's link register.
Returning 1 makes the guest take its normal voice refresh path. `last_option` is the last valid 32-bit value plus one,
with zero for no successful lookup.

## `runtime/patches/animation_loop.c`

Single-frame loops in the 1.1.1 animation clock and pose update (`recomp/patches-v111.txt`, set `ozora`).

### `loop_entry()`, `single_frame()`, `pose_store()`

- `loop_entry()`: the descriptor and container identify the same animation clock entry in the update and draw paths.
  Each clock has 0x4c bytes. A signed descriptor index outside the container's list is a fallback in the game, so it
  does not identify the intended animation.
- `single_frame()`: the clock wraps only with flag bit 1. The frozen pose loops over [0, 1].
- `pose_store()`: the pose pass has its own clock. Its range and the looping source animation are confirmed before a
  fractional pose phase is set to the first frame. The guest also still uses s0 after this store.

## `runtime/patches/battle_music.c`

Battle music change flags for frames held by the sound gate (`recomp/patches-v111.txt`, set `obgm`).

### `battle_music_gate()`, `battle_music_flags_host()`

Phase 0 owns `pending`, under the sound gate's lock. Its result is published as one (address, bits) pair so phase 1 can
read it without taking the lock. The address associates the bits with their manager.
