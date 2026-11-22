# Pull request drafts for hyrule-tracker support

The two pull requests as drafted. Both were opened on 2026-11-18, after the bench runs and a test of the tracker's app:
the fork's as [NX2Saria#8](https://github.com/Larbaco/NX2Saria/pull/8), the tracker's as
[jv-prado/hyrule-tracker#8](https://github.com/jv-prado/hyrule-tracker/pull/8) (from `Larbaco/hyrule-tracker`, branch
`nx2saria-tracker-api`: the patch in two commits, with a description in Portuguese and English as the tracker's earlier
pull request had). The texts below are the drafts; the pull requests carry the final ones. Background:
[hyrule-tracker.md](hyrule-tracker.md).

## 1. Fork (Larbaco/NX2Saria): the game data on the tracker API

**Branch:** `hyrule-tracker-integration`, rebased onto `main` at `a8117c8a` (0.16.0). **Base:** `main`, which has the
tracker API this PR extends ([NX2Saria#7](https://github.com/Larbaco/NX2Saria/pull/7)).

**Title:** Answer item trackers the game's progress by the save files' keys, on the tracker API (nx2saria.game_data)

**Description:**

> An item tracker such as hyrule-tracker reads the game's progress from the save files: values known by the
> MurmurHash3 of their names, and a sorted set of 64-bit keys for the placed objects. To follow the running game it
> reads the same values from another port's memory, by searching gigabytes for the game's tables at a fixed guest
> offset (on Linux through `/proc`, with `ptrace` rights). NX2Saria's memory is laid out differently (guest = host),
> so that search finds nothing.
>
> This adds one method to the tracker API of 0.16.0 (the same server, port and settings; no second listener):
>
> - `nx2saria.game_data {kind, hash, filter, if_digest}`: the values of one kind (`Bool`, `Int`, `Enum`, `String64`,
>   `String64Array`, `Int64`, `Bool64bitKey`), each with its key, sorted by key, as little-endian records in hex
>   (`format` 1), read from the game's own game-data manager (`**[main+0x4c7e180]`, version 1.1.1). A digest lets a
>   polling tracker get `unchanged` instead of the records. A kind is answered whole or refused, never with made-up
>   values: -32002 when asking again may answer (no save loaded yet, the game changed the kind during the copy, a
>   value it cannot read now), -32003 when it will not (another game version, an entry without the kind's vtable).
> - `status.methods` lists the methods and `status.save_dir` names the folder of the save the game loads and writes
>   (`fs_host.c fs_save_dir`, escaped as JSON), so a tracker follows this game's saves and not another install's;
>   `nx2saria.state` gains `player.rotation` (the port's own reader, `dbg_player_rot`) and `game_data` (the game data
>   is there).
> - `DBG_GAME_DATA_SLOT` in `runtime/diag/debug_tools.h`, shared by the debug menu and the API.
> - Nothing runs on the game's threads, and every copy goes through `guest_copy` (the game's own regions only). A
>   request copies one kind, one request at a time: for `Bool`, the most, about 265 KiB of entries and 175 KiB of
>   table. Entries per kind and the answer's size are capped; an answer's room over 1 MiB is not kept.
> - The log: the start line names the new method (from the method table, as `status` lists them); one line the first
>   time the game data is asked for before the game has made it; one line for the first answer of each kind (count,
>   bytes, time); one line per kind the first time it cannot be answered, with the reason and the error.
>
> **Tested:** `tests/tracker_api_check.py` on Linux (a stand-in manager at the game's address, empty and then loaded,
> every kind, hashes, digests checked against FNV-1a, filters, refused parameters, text pointers into the host's own
> memory, a duplicated pair, a wrong vtable in a middle entry, one log line per kind); `clang -fsyntax-only` for Linux
> and `x86_64-w64-mingw32`; `tools/check_platform.py`, `tests/cfg_rows_check.py`; `tools/tracker_api_client.py` against
> the stand-in; the patched hyrule-tracker reading this server end to end. **With the game** (Linux bench, build of
> `380444b7`, a save loaded by an input script): every kind answered (11,292 Bool, 1,259 Int, 130 Enum, 54 String64,
> 5 String64Array, 33 Int64, 360 placed objects set), against the slot that loaded 0 missing and 1 Bool, 0 Int, 0 Enum
> and 0 placed objects different; the heading from the rotation equal to the way Link moves; the patched hyrule-tracker
> live against the running game. docs/hyrule-tracker.md, "First run", has the details. Not yet on Windows.

**Commits:** the method and the state fields; the check; the bench client's game-data and heading steps; the docs.

**Review notes:** the strides and value offsets of `Bool`, `Int`, `Enum`, `String64` and `Int64`, and the placed
objects' table at manager+0xc58 (a u32 count), were read in the game's own getters and loader; `Bool`, `Int` and
`String64` are also what the port's debug menu and the Atlas client read, and the two pinned vtables (Bool, Int) are
Atlas's. `String64Array`'s stride is what hyrule-tracker saw in another port of the same build. The vtable, table and
sort checks make a wrong one refuse instead of answering wrong values, and the bench client compares every kind with
a save.

## 2. hyrule-tracker (jv-prado/hyrule-tracker): NX2Saria as a live source

**Patch:** [hyrule-tracker-pr.patch](hyrule-tracker-pr.patch), against hyrule-tracker `master` at `1fb2777` (v1.4.2;
applies cleanly; first written against v1.1.1; its header carries the tracker's MIT notice for the lines it quotes).
Six files, about 545 lines added, 290 of them the module and 170 its test.

**Title:** Read NX2Saria live through its tracker API, and find its saves

**Description:**

> [NX2Saria](https://github.com/Larbaco/NX2Saria) is a native PC port of the game (version 1.1.1) for Windows and
> Linux. Its guest memory is not at `0x1000_0000_0000 + guest` as in the ports live mode reads now, so the table
> search cannot find it. With `tracker_api = 1` in its `nx2saria.ini` it answers the running game's progress itself,
> read-only and for this PC only, on `127.0.0.1:6545`: every Bool, Int, Enum, String64, String64Array and Int64 value
> by the same Murmur3 keys the saves use, the placed objects' 64-bit keys with their class, and Link's position,
> rotation and area.
>
> This PR:
>
> 1. `src/gamedata/nx2saria.ts` (new, no Electron imports): a JSON-lines client for that API, the decoding of its
>    answers into the values `parser.ts` gives for a save, and `overlay`, which lays them over the reference save
>    and fills the same `Agreement`s as `buildLive`, so `matchesReference` decides as for memory. `ApiSession` asks each
>    kind with the digest of its last answer, so an unchanged kind costs a short answer, and the placed objects'
>    classes once.
> 2. `src/main/live.ts`: every 3 s while no game is attached, `LiveSource` asks `127.0.0.1:6545` for `status` beside
>    its process search (so finding the other ports waits no longer); when it answers as NX2Saria with
>    `nx2saria.game_data`, it reads through it (`apiStep`). Bool, Int, Enum and the placed objects are required; the
>    area, the place within it and the Int64s are left out when the game refuses them. A refusal for a moment (a save
>    being written or loaded) keeps the live state up to 3 s; "not on this build" (-32003) or another `format` says so.
>    Without memory access the status stays "unavailable" until NX2Saria answers. The decision after a read
>    (`publish`) and the player update (`setPlayer`) are shared with the memory path. The session hands the save
>    folder NX2Saria names (`status.save_dir`) to `onGame`.
> 6. `src/main/controller.ts`: `followGame` switches to the folder a game names as it does to one found from an exe
>    (not when a slot is pinned or a folder was picked by hand), so the tracker follows NX2Saria's saves rather than
>    the newest of any install (an emulator's, in a test on a PC with both).
> 3. `src/gamedata/saves.ts`: NX2Saria's game files (`ocarina.exe`, `nx2saria.exe`; without `.exe` off Windows, and
>    only files) in the install search and its label, `<game>/profile/save` and the launcher's `<install>/game/profile/save` in
>    `resolveUserFolders`, `NX2Saria` among the default install folders.
> 4. `test/nx2saria.test.ts`: decoding of every kind, the heading, the overlay and the playthrough check, sessions
>    against a stand-in server (digests, the classes asked once, the wait before the game data, optional kinds
>    refused, -32003, another format, another program on the port).
> 5. README: NX2Saria among the installs, and how to turn its API on.
>
> For the other ports and emulators live mode reads as before; what they see change is a connection attempt to
> `127.0.0.1:6545` every 3 s while no game is attached, made beside the process search. Tested with
> `npm run typecheck` and `npm test` (186 passed, 11 skipped as before), end to end against NX2Saria's server code
> over a stand-in for the game's memory, and `ApiSession` against the running game on Linux, twice: live against the
> loaded save (the second time, on NX2Saria 0.16.0: all 10,212 Bools and every Int, Enum and placed object agreeing),
> a whole read in 10 to 65 ms.
> `live.ts`'s own changes are not covered by a test (they run in Electron's main process), and the app itself was not
> started (Windows).
>
> The heading: NX2Saria gives Link's rotation as a 3x3 matrix; `yawFromRotation` takes its third column as his
> forward axis, which NX2Saria's bench runs confirmed: it equals the way he moves at every step he moves, and it keeps
> following him when he turns on the spot, where the direction of movement stays at its last value.

**What the tracker gains:** live tracking on Linux, and on Windows without reading another process's memory; no table
search (the first read is immediate); the place within an area and the placed objects as for the other ports.

**Not proposed:** the activity screenshot for NX2Saria (`captureGame` needs the game's process id, which the API does
not give).

**License:** the patch's new lines would be contributed under the tracker's MIT license. The patch quotes some of
the tracker's lines as diff context; they stay MIT and the tracker's.
