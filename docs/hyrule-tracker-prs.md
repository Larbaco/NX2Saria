# Pull request drafts for hyrule-tracker support

Two drafts. Neither is open. The fork's is ready on the branch `hyrule-tracker-integration`. The tracker's waits for
the copyright holder's decision to contribute it. Background: [hyrule-tracker.md](hyrule-tracker.md).

## 1. Fork (Larbaco/NX2Saria): the game data on the tracker API

**Branch:** `hyrule-tracker-integration`, from `main` at `0170e49d` with `atlas-integration` (`dbd8d6c4`) merged in.
**Base:** `main` once `atlas-integration` is merged; until then, `atlas-integration` (this PR extends its server).

**Title:** Answer item trackers the game's progress by the save files' keys, on the tracker API (nx2saria.game_data)

**Description:**

> An item tracker such as hyrule-tracker reads the game's progress from the save files: values known by the
> MurmurHash3 of their names, and a sorted set of 64-bit keys for the placed objects. To follow the running game it
> reads the same values from another port's memory, by searching gigabytes for the game's tables at a fixed guest
> offset, on Windows only. NX2Saria's memory is laid out differently (guest = host), so that search finds nothing.
>
> This adds one method to the tracker API of `atlas-integration` (the same server, port and settings; no second
> listener):
>
> - `nx2saria.game_data {kind, hash, filter, if_digest}`: the values of one kind (`Bool`, `Int`, `Enum`, `String64`,
>   `String64Array`, `Int64`, `Bool64bitKey`), each with its key, sorted by key, as little-endian records in hex
>   (`format` 1), read from the game's own game-data manager (`**[main+0x4c7e180]`, version 1.1.1). A digest lets a
>   polling tracker get `unchanged` instead of the records. A kind is answered whole or refused, never with made-up
>   values: -32002 when asking again may answer (no save loaded yet, the game changed the kind during the copy, a
>   value it cannot read now), -32003 when it will not (another game version, an entry without the kind's vtable).
> - `status.methods` lists the methods; `nx2saria.state` gains `player.rotation` (the port's own reader,
>   `dbg_player_rot`) and `game_data` (the manager is there).
> - `DBG_GAME_DATA_SLOT` in `runtime/debug_tools.h`, shared by the debug menu and the API.
> - Nothing runs on the game's threads, and every copy goes through `guest_copy` (the game's own regions only). A
>   request copies one kind, one request at a time: for `Bool`, the most, about 265 KiB of entries and 175 KiB of
>   table. Entries per kind and the answer's size are capped; an answer's room over 1 MiB is not kept.
> - The log: the start line names the new method (from the method table, as `status` lists them); one line the first
>   time the game data is asked for before a save is loaded; one line for the first answer of each kind (count,
>   bytes, time); one line per kind the first time it cannot be answered, with the reason and the error.
>
> **Tested:** `tests/tracker_api_check.py` on Linux (a stand-in manager at the game's address, empty and then loaded,
> every kind, hashes, digests checked against FNV-1a, filters, refused parameters, text pointers into the host's own
> memory, a duplicated pair, a wrong vtable in a middle entry, one log line per kind); `clang -fsyntax-only` for Linux
> and `x86_64-w64-mingw32`; `tools/check_platform.py`, `tests/cfg_rows_check.py`; `tools/tracker_api_client.py` against
> the stand-in; the patched hyrule-tracker reading this server end to end. **Not yet:** a build and a run of the game.
> The checklist in docs/hyrule-tracker.md proves the live values of every kind against a save, the String64Array
> stride and the axis of the rotation.

**Commits:** the method and the state fields; the check; the bench client's game-data and heading steps; the docs.

**Review notes:** the strides and value offsets of `Bool`, `Int`, `Enum`, `String64` and `Int64`, and the placed
objects' table at manager+0xc58 (a u32 count), were read in the game's own getters and loader; `Bool`, `Int` and
`String64` are also what the port's debug menu and the Atlas client read, and the two pinned vtables (Bool, Int) are
Atlas's. `String64Array`'s stride is what hyrule-tracker saw in another port of the same build. The vtable, table and
sort checks make a wrong one refuse instead of answering wrong values, and the bench client compares every kind with
a save.

## 2. hyrule-tracker (jv-prado/hyrule-tracker): NX2Saria as a live source

**Patch:** [hyrule-tracker-pr.patch](hyrule-tracker-pr.patch), against hyrule-tracker `master` at `fbfafe5` (v1.1.1;
applies cleanly; its header carries the tracker's MIT notice for the lines it quotes). Five files, about 520 lines
added, 280 of them the module and 165 its test.

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
>    Without memory access (Linux) the status stays "unavailable" until NX2Saria answers. The decision after a read
>    (`publish`) and the player update (`setPlayer`) are shared with the memory path.
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
> `npm run typecheck` and `npm test` (108 passed, 11 skipped as before), and end to end against NX2Saria's server code
> over a stand-in for the game's memory. `live.ts`'s own changes are not covered by a test (they run in Electron's
> main process).
>
> Open point: the heading. NX2Saria gives Link's rotation as a 3x3 matrix; `yawFromRotation` takes its third column
> as his forward axis. That is to be confirmed in the game (NX2Saria's bench client prints both); until then the pin
> may point the wrong way, never the wrong place.

**What the tracker gains:** live tracking on Linux, and on Windows without reading another process's memory; no table
search (the first read is immediate); the place within an area and the placed objects as for the other ports.

**Not proposed:** following NX2Saria's save folder when it starts (`followGame` needs the game's exe path, which the
API does not give; the install search finds the folder, or the player picks it). The activity screenshot for NX2Saria
(`captureGame` needs the game's process id, which the API does not give). A Linux build of the tracker's release (its
`dist` script builds Windows only; `npm start` runs it from the source on Linux).

**License:** the patch's new lines would be contributed under the tracker's MIT license. The patch quotes some of
the tracker's lines as diff context; they stay MIT and the tracker's.
