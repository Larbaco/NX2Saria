# hyrule-tracker and NX2Saria

[hyrule-tracker](https://github.com/jv-prado/hyrule-tracker) is a 100% completion companion for this game: a checklist
of every collectible, chest and quest, a map with Link on it, and the logic of what can be reached now. It reads the
game's own save files, and on Windows it follows the running game in another port's memory. This page records what
it takes to use it with NX2Saria: how it reads the game, what breaks here, what the fork and the tracker change, and
how it shares one tracker API with Ocarina Atlas ([ocarina-atlas.md](ocarina-atlas.md), branch `atlas-integration`).

Studied: hyrule-tracker `master` at `fbfafe582909e6c23380d15a811605773af03549` (v1.1.1), NX2Saria `0170e49d`
(0.14.0) with `atlas-integration` at `dbd8d6c4`, game version 1.1.1, main build `e0f3ccfaf50065ae`. The game was not
built or run for this study.

## Conclusion

**Feasible, with changes in the fork and in the tracker.**

- **The saves work now.** The tracker reads `progress.sav` and `caption.sav`, the game's own save format, and NX2Saria
  writes exactly that: the tracker's parser reads the six slots of the bench's saves with no warning, with the same
  format version (`0x002d80eb`) and the same 11,925 keys as the saves it was written against. Its search does not find
  NX2Saria's folder by itself; picking `<game folder>\profile` by hand does.
- **Live tracking does not.** The tracker reads another process's memory with `ReadProcessMemory`, on Windows only,
  and assumes what the other PC ports do: guest memory at host `0x1000_0000_0000 + guest`, the game's tables found by
  sweeping 7 GB of mapped memory, vtables below 4 GiB, and the process names of those ports. NX2Saria has none of
  that: its guest addresses are its host addresses (main at `0x7000000000`), its heap is a private block near
  `0x130000000` (or `0x1030000000` with the tracker API on), and its game runs as `ocarina` or `nx2saria`.
- **The fix is one more method on the tracker API that Ocarina Atlas's study added**, not a second server, and a new
  live source in the tracker. `nx2saria.game_data` answers the running game's values by the same keys the save files
  use (the MurmurHash3 of each value's name), read from the game's own game-data manager. The tracker lays them over
  the save it already read, as it does with values from memory. Live tracking then works on Linux too, without any
  permission to read another process and without searching memory.
- **Prototype:** written, tested against a stand-in for the game's memory (`tests/tracker_api_check.py`), and run
  with the game on the Linux bench ([First run](#first-run)): every kind answered, the live values agree with the save
  that loaded, Link's heading reads right, and the patched tracker goes live against the running game. Not yet run on
  Windows, and the tracker app itself (Electron) was not started.

## What hyrule-tracker is

- **Product.** A desktop companion for the remake on Switch 2, at 100% completion: 322 checklist rows in 23
  categories (100 Gold Skulltulas, 36 Heart Pieces, 8 Heart Containers, 206 chests, songs, items, side quests,
  minigames, the Hall of Enemies), a map with 32 regions and a live pin for Link, logic of what can be reached now, an
  overlay. It is not for the N64 game, Ship of Harkinian, the randomizer or Archipelago.
- **Language and platforms.** TypeScript, Electron 45, React 19, Vite, Vitest; native calls through koffi. Releases
  are a Windows x64 installer (`electron-builder --win`); `npm start` runs it from the source on any system, but live
  reading switches itself off outside Windows.
- **Project.** MIT, one author, created and released (v1.0.0 to v1.1.1) on 2026-11-17, 4 commits, 9 stars. Its
  README already lists "Triforce / NX2navi" among the installs it searches.

### How it gets the game's data

1. **Save files**, always. `slot_*/progress.sav` and `caption.sav`, parsed as the game's GameData container
   (`src/gamedata/parser.ts`): 8-byte `{u32 hash, u32 value}` records grouped by kind, then the data the non-inline
   kinds point to, then the sorted 64-bit keys of the placed objects. It watches the folder (`fs.watch` and a poll every
   3 s) and re-reads a slot when the game writes it. Folders: `<dir>`, `<dir>/save/<32 hex>`,
   `<dir>/user/save/...`, the emulators' layout under `%APPDATA%`, and default install folders of other ports.
2. **Live, from another process's memory**, on Windows (`src/main/procmem.ts`, `src/gamedata/live.ts`,
   `src/main/live.ts`). Every 3 s it looks for a known game process (`hookshot.exe`, `Triforce.exe`, `nx2navi.exe`,
   `relink-game.exe`, Longshot, Ocarina Reborn, or the emulators `lichii.exe`/`fritchii.exe`), opens it with
   `PROCESS_VM_READ`, and every 500 ms:
   - finds the game's tables near remembered addresses or by sweeping the heap at host
     `0x1010_0000_0000`..`0x1011_C000_0000` in 128 MB windows: runs of records of one stride that all start with the
     same pointer below 4 GiB (Bool 0x18, Int 0x28, Enum 0x38, String64 0x108, Int64 0x30), the `{hash, index}` table
     before each, the sorted 16-byte table of the placed objects, and the place-within-the-area list;
   - reads them and lays their values over the newest save (`buildLive`), and uses the result only when it is the
     same playthrough: at least 90% of the save's set Bools still set, at most 3 of its placed objects missing;
   - reads Link's position and heading through a pointer chain from main (ported from Ocarina Atlas), at guest
     `0x1000_0000_0000 + guest`.
   - No wire protocol, no emulator debug server, no WebSocket, no Archipelago.

### What it reads

| Data | How the tracker reads it |
|---|---|
| Items, equipment, songs, masks, stones, medallions, upgrades, trade items | Bool `IsGet.<item>` (97 in its mapping) |
| Counts: Gold Skulltula tokens, Heart Pieces held, rupees, ammunition | Int `PouchData.<item>.StockNum` (`0x04735978` for the tokens), `PlayerStatus.CurrentRupee` |
| Hearts, magic | Int `PlayerStatus.MaxLife` (quarter hearts), `PlayerStatus.MPMax` |
| Dungeon map, compass, boss key | Bool `DungeonInfo.<dungeon>.IsGetMap` / `IsGetCompass` / `IsGetBossKey` |
| Chests | Bool `IsTreasureBoxOpen.<placement>` (206 in its companion data) |
| Gold Skulltulas, Heart Pieces and Containers in the world | the set 64-bit keys of the placed objects, and their class (bit 0x40: respawning) |
| Quests and story steps | Enum `Step_<quest>` = hash of `Complete`; Bool `HavePlayedEvent.<event>` |
| Bosses, Hall of Enemies | Int `DefeatedEnemyNum.<enemy>` |
| Area | String64 `0x1d6189da` (`Sequence_CurrentBanc`), the age from its `_Child`/`_Adult` suffix |
| Place within the area | String64Array `0xc458f1eb` (for example `["HyruleField", "LakeHylia"]`) |
| Link on the map | live: x, y, z and heading from the player object; from a save: `caption.sav`'s position and yaw |

It reads no small-key counts, current health or magic, or entrances.

## Compatibility

| Part of the tracker | NX2Saria today | With this branch (`tracker_api = 1`) and the tracker PR |
|---|---|---|
| Save files | Read without warnings; found when `<game>\profile` is picked by hand | Also found by its search (`ocarina.exe`, `<game>\profile\save`, `%LOCALAPPDATA%\Programs\NX2Saria\game`) |
| Save label | None | "NX2Saria" |
| Live: finding the game | Process names not known; not on Linux | `127.0.0.1:6545` answers `status` as `nx2saria` with `nx2saria.game_data` |
| Live: tables | Searched at `0x1000_0000_0000 + guest`, in mapped memory, vtables below 4 GiB: none of it matches | `nx2saria.game_data`, each kind whole, no search |
| Live: same-playthrough check | — | The same `Agreement`s as from memory, against the newest save |
| Live: Link on the map | Pointer chain at `0x1000_0000_0000 + guest`: not found | `nx2saria.state`: position, area, rotation |
| Linux | Save files only | Save files and live |
| Memory-reading permission | — | None needed |

## Data and where it is in the fork

Version 1.1.1; `main+X` is guest (and host) address `0x7000000000 + X`. The game data lives in one manager,
`**[main+0x4c7e180]` (`runtime/debug_tools.c` `gmd`; the object at `*(main+0x4e842a8)`), a 0xd38-byte singleton the
game fills at start, before its first frame, with its game data list's defaults, and sets from a save when one loads. For each kind it holds a section of 0x60 bytes at `manager + number × 0x60`: entries'
count (u32) at +0x10, the entries at +0x18, a `{u32 hash, u32 index}` table at +0x20, probed from `hash % size` on, its
size (u32) at +0x2c. The kinds are numbered as in the save files. Everything in the table was read in the game's own
code (its getters and loader) and in its GameDataList, and the counts were checked against the bench's saves; the
live values are what the first run checks.

| Data | Kind (number) and count in 1.1.1 | Entry in the manager | Example key | Read through |
|---|---|---|---|---|
| Items, equipment, songs, upgrades ever obtained | Bool (0), 11,292 (10,212 saved) | 0x18 bytes, value u8 at +0x16, vtable main+0x4c20658 | `IsGet.Item_Hookshot` `0xff98ed58` | `nx2saria.game_data` `Bool` |
| Item counts (sticks, nuts, bombs, arrows, beans, tokens, Heart Pieces held) | Int (2), 1,259 (1,144 saved) | 0x28 bytes, s32 at +0x1c, vtable main+0x4c20718 | `PouchData.Item_Token_Gold.StockNum` `0x04735978` | `Int` |
| Max health, magic | Int (2) | 0x28 bytes, s32 at +0x1c | `PlayerStatus.MaxLife` `0xfbe01da1` | `Int` |
| Small keys | Int (2), 14 dungeons | 0x28 bytes, s32 at +0x1c | `DungeonInfo.ForestTemple.SmallKeyNum` `0x90237092` | `Int` |
| Map, compass, boss key | Bool (0) | 0x18 bytes, value u8 at +0x16 | `DungeonInfo.ForestTemple.IsGetBossKey` `0x427333a0` | `Bool` |
| Strength, bottle contents, quest steps | Enum (6), 130 (101 saved) | 0x38 bytes, u32 at +0x1c (hash of the value's name) | `PlayerStatus.LiftGrade` `0x59979321` | `Enum` |
| Age | Bool (0) | 0x18 bytes, value u8 at +0x16 | `Sequence_IsChildhood` `0xcea220d3` (1 child) | `Bool` |
| Chests (206) | Bool (0) | 0x18 bytes, value u8 at +0x16 | `IsTreasureBoxOpen.<placement hash in decimal>` | `Bool` |
| Events, conversations | Bool (0): 3,647 `HavePlayedEvent`, 199 `HavePlayedGetEvent` | 0x18 bytes, value u8 at +0x16 | | `Bool` |
| Area | String64 (16), 54 | 0x108 bytes, pointer to the text at +0x68 | `Sequence_CurrentBanc` `0x1d6189da` | `String64`, or `nx2saria.state.area` |
| Place within the area | String64Array (17), 5 | 0x90 bytes, count at +0x80, elements at +0x88 (0x50 each, text at +0x0c) | `0xc458f1eb` | `String64Array` |
| Sun's Song counter and other 64-bit counts | Int64 (22), 33 | 0x30 bytes, s64 at +0x20 | `0xd457ee3a` | `Int64` |
| Gold Skulltulas (100), Heart Pieces in the world (14), Heart Containers (8) | the placed objects' table, 11,657 | count u32 at manager+0xc58, 16-byte records at `*(manager+0xc60)`: u64 key, u32 class, u8 value; sorted, the game binary-searches it | the placed object's 64-bit hash `H` (Gold Skulltulas also set `H ^ 2`) | `Bool64bitKey` |
| Link's position | — | `[[main+0x4e371d0]+0x480]+0x31c` (`debug_tools.c dbg_player_pos`) | | `nx2saria.state.player` |
| Link's rotation (heading) | — | the same actor +0x328, 3x3 (`dbg_player_rot`) | | `nx2saria.state.player.rotation` |
| The game data is made | — | the Bool section's count is not 0 | | `nx2saria.state.game_data` |
| Save files | — | `<profile>/save/<account>/slot_00..05/{progress,caption}.sav`, written through `profile/save-tx` | | the tracker's own reader |

Checked on the first run ([First run](#first-run)): the live values of every kind against the save that loaded, the
stride of String64Array (0x90, what the tracker saw in another port of the same build), the axis of the rotation that
is Link's forward one (the third column), and the manager's vtables. Not established: which wallet is which. Unread
kinds (Float, UInt, the arrays of numbers, vectors) are left out of the
API until a tracker needs them; their layouts were read in the game's code, not checked live (Float and UInt 0x28 with
the value at +0x1c; the arrays of numbers 0x40, EnumArray 0x50, each with the count at +0x20 and the elements' address
at +0x28).

## Changes in the fork (this branch)

This branch is `main` (`0170e49d`) with `atlas-integration` (`dbd8d6c4`) merged in, and these changes on top:

| Change | Where |
|---|---|
| `nx2saria.game_data {kind, hash, filter, if_digest}`: the values of one kind with their keys, sorted, in hex, with `format`, `presents` and an FNV-1a `digest`; one request at a time; a kind answered whole or refused (-32002 to ask again, -32003 not on this build, only after its section read the same twice) | `runtime/tracker_api.c` (`gd_kinds`, `gd_records`, `gd_keys`, `gd_unsupported`, `do_game_data`) |
| `status.methods`; `nx2saria.state.player.rotation` and `nx2saria.state.game_data` | `runtime/tracker_api.c` (`do_status`, `do_state`) |
| The manager's slot shared by the debug menu and the API | `runtime/debug_tools.h` (`DBG_GAME_DATA_SLOT`), `runtime/debug_tools.c` |
| The answer's room freed after an answer over 1 MiB | `runtime/tracker_api.c` `client_main` |
| The check: a stand-in manager at the game's address, empty first and then loaded as the game loads it, every kind, keys (by hash at two indices), digests, filters, refused parameters, and the stand-in's ways to spoil a read: text pointers into the host's memory, 65 texts, a hash twice, two pairs for one entry, placed objects out of order, an entry still being made, a wrong vtable in the middle | `tests/tracker_api_check.c`, `tests/tracker_api_check.py` |
| The bench client: every kind, compared with the newest save (`--save`), the manager's vtables, and Link's heading from his rotation beside the way he moves (`--watch`) | `tools/tracker_api_client.py` |
| The protocol, this page, the PR drafts and the tracker patch | `docs/tracker-api.md`, `docs/hyrule-tracker.md`, `docs/hyrule-tracker-prs.md`, `docs/hyrule-tracker-pr.patch` |

No setting is added: the tracker API's `tracker_api` (and `NX2SARIA_TRACKER_API`, port `tracker_api_port`) turns this on
with the rest. Not done: a row for the API in the settings menu and the launcher (as for Atlas).

## Changes in the tracker

A new live source ([hyrule-tracker-pr.patch](hyrule-tracker-pr.patch), against `fbfafe5`, five files, about 520 lines
added):

1. `src/gamedata/nx2saria.ts` (new): the API client (newline-delimited JSON, one request at a time, timeouts), the
   decoding of each kind into the values its save parser gives, `overlay` (the reference save with the live values,
   and the same `Agreement`s as `buildLive`, so `matchesReference` decides as for memory; every set Bool of the save
   counts, those the game did not answer too), the heading from the rotation, and `ApiSession` (each kind asked with
   its last digest, the placed objects' classes once; Bool, Int, Enum and the placed objects required, the area
   kinds left out when the game refuses them, as the memory path does without the area).
2. `src/main/live.ts`: while no game is attached, `127.0.0.1:6545` is asked beside the process search (no added wait);
   when it answers as NX2Saria with `nx2saria.game_data`, the source reads through it. Without memory access (Linux)
   live mode still says it cannot read memory, and goes live when NX2Saria answers. A refusal for a moment (a save
   being written or loaded) keeps the live state up to 3 s; -32003 or another `format` says the API does not read this
   game version. A stop during a request changes nothing after it. The decision after a read and the player update are
   shared with the memory path.
3. `src/gamedata/saves.ts`: NX2Saria's game files (`ocarina.exe`, `nx2saria.exe`; without `.exe` off Windows, files
   only) and label, `<game>\profile\save` and the launcher's `<install>\game\profile\save`, `NX2Saria` among the
   default install folders.
4. `test/nx2saria.test.ts`: decoding, heading, overlay, playthrough check, sessions against a stand-in server (digests,
   the classes asked once, the wait before a save, optional kinds refused, -32003, another format, another program
   on the port).
5. README: NX2Saria and how to turn on its API.

Checked: `tsc --noEmit` passes and `vitest` passes (108 tests, 11 skipped as before). End to end, the patched
`ApiSession` read the fork's server code (the check's build of `runtime/tracker_api.c` with its stand-in memory) and
built the expected live data: values over the save, the place within the area, the keys and their classes, the same
fingerprint on a second read. Not tested: `live.ts`'s own changes (they need Electron's main process), and the
activity screenshot, which needs a process id the API does not give (it stays off for NX2Saria).

## Pull requests

Drafts in [hyrule-tracker-prs.md](hyrule-tracker-prs.md). None is open.

1. **Fork:** the game data on the tracker API, ready on this branch. It extends `atlas-integration`, so it goes after
   that one (or both as one PR).
2. **hyrule-tracker:** NX2Saria as a live source and its saves; the patch above. It waits for the copyright holder's
   decision to contribute it, and nothing was opened or posted in the tracker's repository.

## The API: one for both trackers

**A mod or a runtime module?** A runtime module, for the reason [ocarina-atlas.md](ocarina-atlas.md) gives: a mod in
the port's mod folder brings files, ExeFS patches and cheats, but no code of its own (`runtime/mods.h`
`MOD_PLUGIN` turns off code modules), and a patch to the game's instructions cannot open a socket.

**One server.** Atlas's study added the tracker API: TCP on `127.0.0.1:6545`, newline-delimited JSON, read-only, off by
default (`tracker_api = 1` in `[tracker]`, or `NX2SARIA_TRACKER_API=1`). This study adds one method to it, and two
fields. Both trackers connect to the same port; each tracker finds what it needs in `status.methods`.

| Method | Ocarina Atlas | hyrule-tracker |
|---|---|---|
| `status` | Every second: running, title, build | Once per connection: `emulator` `nx2saria`, `methods` has `nx2saria.game_data` |
| `read_memory`, `search_memory` | Its chains from main's `.bss` into the heap, main by its `MOD0` header | Not used |
| `nx2saria.state` | Optional (area, position) | Every 500 ms: position, `rotation`, `area`, `game_data` |
| `nx2saria.game_data` | Optional: the same flags it walks the manager for (Gold Skulltula count, Heart Pieces, placed objects) | Every 500 ms: `Bool`, `Int`, `Enum`, `String64`, `String64Array`, `Int64`, `Bool64bitKey` with their digests |

Why a method of values and not addresses: the item tracker models the game exactly as its save holds it, a map of
typed values by key and a set of 64-bit keys. The manager is that model in memory, and the port knows where it is and
how it is laid out. Answering by key keeps every address and layout fact in the port, where one build is known, and
leaves the tracker the keys it already uses for the saves. A tracker that only knows raw memory (Atlas) keeps
`read_memory`; nothing about it changes.

Options weighed for the tracker's live data:

| Option | Verdict |
|---|---|
| **A method of values by key on the existing API (`nx2saria.game_data`)** | **Chosen.** No search, no addresses in the tracker, the same keys as the saves, Linux included; one kind per request, a digest to skip unchanged ones |
| The tracker's memory reader over `read_memory`, with the tables' addresses given by the API | Possible, but every poll would move about 500 KB of raw entries as about 1 MB of hex, the tracker's vtable filter (below 4 GiB) and table search would need changes anyway, and the layout knowledge would stay in the tracker |
| The tracker reading NX2Saria's process (`ReadProcessMemory`, `process_vm_readv`) | Needs per-port layout settings in the tracker, `ptrace` rights on Linux, and its heap sweep cannot find a private block; rejected as for Atlas |
| A file of the game data written by the game (JSON, or a `progress.sav` written every second) | Disk writes many times a minute, stale reads, and a second format to write; the save itself already covers "not live" |
| WebSocket or shared memory | As for Atlas: no client that needs it, a larger attack surface, a different API per system |

**Security.** Unchanged from the API: bound to `127.0.0.1` only, off by default, read-only (no method writes; the new
one reads the manager the game made). Every copy goes through `guest_copy`, which reads only the game's own regions:
the test points the text pointers at the host's own memory beside the guest's and checks that nothing of it is read.
Limits per request: one kind, entries capped per kind (200,000 Bools, the most), 8 MiB of records, one game-data request
at a time, and the answer's room freed after a large answer. Anything on this PC can connect while the API is on;
leave it off on a shared machine.

**Cost per frame.** None on the game's threads: the copy happens on the API's client thread. A tracker that polls
twice a second with digests makes about 16 requests a second; the largest copy, Bool, is about 265 KiB of entries
and 175 KiB of table, a fraction of a millisecond. No per-frame snapshot is needed: a value the game changes while it is
read is either its old or its new one, and a kind whose table or entries changed during the copy is refused (-32002)
and asked again.

**Portability.** The same code on Linux and Windows: the method uses only the API's existing copy and the platform
layer's mutex and clock; `clang --target=x86_64-w64-mingw32 -fsyntax-only` passes, and so does the Linux check. The
tracker's side is Node's `net`, the same on both.

**Protocol and versions.** [tracker-api.md](tracker-api.md) has every field. `api_version` stays 1: methods and fields
are added within a version. `nx2saria.game_data` answers carry `format` 1, the layout of their records, which can
change without touching the other methods. Frequency is the client's: 500 ms for the tracker (its own tick), 120 ms
for Atlas.

**State of the prototype.** Written and tested without the game:

- `python tests/tracker_api_check.py` passes on Linux: Atlas's checks, and for `nx2saria.game_data` the refusal before
  the manager exists and while it is empty; every kind's records, sorted, with the Bool value masked to 0 or 1 and the
  placed objects' other bytes zeroed; a hash, a missing hash, `filter`; the digest (checked against FNV-1a in Python)
  and `unchanged`, also for an empty answer; refused parameters; a text pointer into the host's memory, a duplicated
  pair and a wrong vtable in a middle entry refused; one log line per kind however often it is refused.
- `clang -fsyntax-only` for Linux and for `x86_64-w64-mingw32` passes with `-Wall -Wextra`; `tools/check_platform.py`,
  `tests/check_platform_test.py` and `tests/cfg_rows_check.py` pass. `tests/branding_package_check.py` needs `fontTools`,
  which this machine's Python lacks; it does not touch these files.
- `tools/tracker_api_client.py --save` runs against the check's server and a save made for it.
- Two reviews read the method, its check, the tracker patch and these pages. The first one's findings (allocations a
  corrupted table could make huge, values made up where a read failed, only two entries' vtables checked, a table
  changing during the copy, the Bool byte unmasked, stale bytes in the digest, the whole kind copied for one hash,
  one log flag for every kind, one error code for "wait" and "never") are fixed. The second one's (-32003 for a save
  being loaded, the tracker blocked by one refused kind or flickering on a moment's refusal, tests that did not reach
  the checks they were credited with, wording) are fixed too. The check reaches every refusal listed above; the
  section re-read during a load, the size caps and the probe wrapping round the table are in the code but not
  exercised by it. (Commit `28c62b66` says the check covers "each way a read can fail"; the list above is what it
  covers.)
- The tracker's side: its tests, its type check, and the end-to-end read of the fork's server, above.

## Expected log lines

With the API off, the one start line of [tracker-api.md](tracker-api.md) (`tracker api: off (...)`). With it on and
hyrule-tracker open, after Atlas's lines (its start line now names the new method):

```
tracker api: on, read-only, listening on 127.0.0.1:6545 (this PC only; status, read_memory, search_memory, nx2saria.state, nx2saria.game_data; API version 1); the game's heap at 0x1030000000, above the 0x1000000000 trackers take heap pointers from
tracker api: client 1 (127.0.0.1:50412) asks: its first request is status (1 open)
tracker api: first status answered: powered_on 1, game 1.1.1, build e0f3ccfaf50065ae
tracker api: first nx2saria.state answered: area HyruleField_Adult, Link at (12.50, -3.25, 100.00)
tracker api: nx2saria.game_data asked before the game loaded its game data (the first such answer, for Bool; the closing lines count the others)
tracker api: first nx2saria.game_data of Bool answered: 11292 values, 90336 bytes, in 0.42 ms
tracker api: first nx2saria.game_data of Int answered: 1259 values, 10072 bytes, in 0.08 ms
tracker api: first nx2saria.game_data of Enum answered: 130 values, 1040 bytes, in 0.02 ms
tracker api: first nx2saria.game_data of String64 answered: 54 values, 3672 bytes, in 0.10 ms
tracker api: first nx2saria.game_data of String64Array answered: 5 values, … bytes, in 0.02 ms
tracker api: first nx2saria.game_data of Int64 answered: 33 values, 396 bytes, in 0.02 ms
tracker api: first nx2saria.game_data of Bool64bitKey answered: … values, … bytes, in 0.10 ms
tracker api: the last 10 min: 9600 requests (status 1, nx2saria.state 1200, nx2saria.game_data 8399), 0 refused, 24.1 MiB sent, 0 port checks, 1 client open
tracker api: client 1 (127.0.0.1:50412) left after 812.4 s, the client closed the connection: …
```

The counts are 1.1.1's GameDataList; the first run says the real ones. A kind that does not read as 1.1.1 keeps it
logs `tracker api: nx2saria.game_data <kind> could not be answered: <why> (error -32003; the first such answer for this
kind and error)`, once per kind and error.

## Licenses

- **hyrule-tracker: MIT** (`LICENSE`, "Copyright (c) 2026 João Victor Prado"). Its notices say its player chain comes
  from Ocarina Atlas under MIT (`docs/atlas-license.txt`), while the Atlas study found Atlas's repository under the
  AGPL-3.0; that is the tracker's and Atlas's matter. No code of either is in the fork's code. Game images are not in its
  repository (fetched by scripts, credited to their owners).
- **The fork: MIT** for the base import (`LICENSE`), **all rights reserved** for the additions (`LICENSE-ADDITIONS`).
- **What is allowed:**
  - The API method is the fork's own code, written from the game's code and data and from what the tracker needs (the
    keys, the kinds, the values), not from the tracker's code. Keys, offsets and formats are facts needed to
    interoperate.
  - The tracker and NX2Saria are two programs that talk over a socket; neither license reaches the other through it.
  - The tracker patch's new lines would be contributed under MIT, the tracker's license; that is the copyright
    holder's decision. The patch quotes some of the tracker's lines as diff context, which stay MIT and the tracker's:
    its header carries the MIT notice and `THIRD_PARTY_NOTICES.md` has the row. No tracker code went into the fork's
    code: it needs none.
  - Bundling the tracker with an NX2Saria release would be allowed under MIT (its notice kept); pointing players to
    its releases is simpler.
  - No issue or pull request was opened in the tracker's repository.

## For the Ocarina Atlas study

Found here, worth carrying to `atlas-integration`:

- Atlas's Gold Skulltula hash `0x04735978` is `PouchData.Item_Token_Gold.StockNum` (the name has the `PouchData.`
  prefix).
- `*(main+0x4e90ce8)` is not the game-data manager but the disposer of another singleton (`*(main+0x4e90cf8)`); Atlas's
  "manager + 0x168" holds only while the two singletons lie that way in the heap, and its "save in memory at
  `+0x1160`" points past the manager. The manager is `*(main+0x4e842a8)` (`**[main+0x4c7e180]`). The bench client's
  step 5 shows whether the relation holds in this port.
- The placed objects' count at manager+0xc58 is a u32 (Atlas reads a u64).
- With `nx2saria.game_data`, Atlas could read those flags without walking the manager; nothing requires it to.
- The start line now names `nx2saria.game_data` (the example in `ocarina-atlas.md` predates it).

## First run

2026-11-17, Linux bench, the bench script's build of `380444b7` (the code of this branch; the commits after it change
comments and pages only), started without a window or sound with `tracker_api=1`, an input script pressing A through
the title screen to the first save, then walking Link in four directions. The bench client
(`tools/tracker_api_client.py --save … --watch 25`) and the patched tracker's `ApiSession` ran against it in the same
run. A second run (04:13, the same build, the client and `ApiSession` again) left the game's stderr on a pipe, so the
game wrote its own `logs/nx2saria.log`; the lines below are from that file (the first run's stderr went to a file,
which the runtime then takes for its log instead of writing `logs/`).

- **Build:** 10.4 min, the whole lifted code compiled once; `--probe-cfg` failed only on the read-only file test, as
  expected on NTFS.
- **Start line:** `tracker api: on, read-only, listening on 127.0.0.1:6545 (this PC only; status, read_memory,
  search_memory, nx2saria.state, nx2saria.game_data; API version 1); the game's heap at 0x1030000000, above the
  0x1000000000 trackers take heap pointers from`.
- **When the game data is there:** `nx2saria.state` answered `game_data: true` before the game's first frame, on the
  boot screen, with no save loaded: the game makes its game data at start with its defaults (this page and
  `tracker-api.md` said "when it loads a save" before this run). The area came 14 s later, Link 9 s after that.
- **Every kind answered**, the first answers logged once each in `logs/nx2saria.log`:
  `tracker api: first nx2saria.game_data of Bool answered: 11292 values, 90336 bytes, in 1.28 ms` (1.89 ms in the
  first run), Int 1,259 in 0.11 ms, Enum 130, String64 54, String64Array 5 (2,280 bytes), Int64 33, Bool64bitKey 360
  set; with them `first status answered: powered_on 1, game 1.1.1, build e0f3ccfaf50065ae`, main found by its header
  at `0x7003c4705c` in 24 ms, `first read_memory answered`; no refusal and no "could not be answered" line in either
  run. The second run gave the same values against the save.
- **Against the save that loaded (`slot_02`):** Bool 10,212 in the save, none missing, 1 different (a flag set after
  the load); Int 1,144, none different; Enum 101, none different; 360 placed objects set in the save, all 360 set live.
  The other slots differ by a few dozen values, as other saves of the same playthrough do.
- **The area and the place within it:** `Sequence_CurrentBanc` `WaterTemple`; the String64Array place list
  `["WaterTemple", "", "", ""]` (the 0x90 stride reads).
- **Link:** his position by the port's chain and by Atlas's agree to 0.000 m; his heading from the rotation
  (`atan2(m[2], -m[8])`) equals the direction he moves in at every step of the walk (172.5, 180.0/181.1 while turning,
  282.9, 331.0, 30.5, 140.3, 221.1…).
- **The manager:** starts with `main+0x4c1f120`, `main+0x4c1f140`, as read in main; Atlas's object + 0x168 is the
  manager in this run; Gold Skulltula tokens 49.
- **Reads:** 275 reads of the client's walk, median 0.04 ms, slowest 0.75 ms; main found by its header in 24 ms.
- **hyrule-tracker (the patch's `ApiSession`, from its checkout):** `isNx2Saria` found the game, `player()` gave the
  position, heading and area, and `read()` against each slot's save matched the playthrough; against `slot_02`: Bool
  10,211 of 10,212 agree (2,900 of 2,900 set flags kept), Int 1,144/1,144, Enum 101/101, String64 30/30, Int64 26/28,
  placed objects 360/360, 2,444 objects marked respawning; a whole read (seven requests) in 6 to 25 ms.
- **The log's client lines** reached their limit of 20 within the run: the check that waited for the game opened one
  connection a second (`tracker api: 20 client lines logged; later clients are counted in the summaries only`). The
  run lasted about a minute, under the 10-minute summary, and ended by SIGTERM, so no summary line came.
- **After:** the game ended, `nx2saria.ini` and `test.ini` unchanged.

Not covered by this run: Windows, the tracker app itself (its window, its overlay, `live.ts`), collecting something
while it watches (checklist step 4), Atlas and the tracker at once, and a session of more than ten minutes.

## Checklist for the first run

Steps 2 and 3 are done ([First run](#first-run)); the rest is still to do.

When the game may be built and run again (after Atlas's checklist, steps 1 to 3, which build this branch too):

1. `python tests/tracker_api_check.py` on Windows (MSYS2 clang) as well.
2. Build this branch (Linux and Windows). Start the game with `tracker_api = 1` (bench: `test-linux.ini` /
   `test.ini`, keep a copy and compare it after) and stay on the title screen: the start line names
   `nx2saria.game_data`; the game data answers with its defaults (it is made before the first frame), which a tracker
   tells from a save by comparing with one.
3. Load a save, save in the game at once, then run
   `python tools/tracker_api_client.py --save <game folder>/profile/save --watch 30` while Link walks and turns:
   - step 6: every kind answers with about 11,292 Bool, 1,259 Int, 130 Enum, 54 String64, 5 String64Array, 33 Int64
     and about 11,657 placed objects; the manager's vtables read `main+0x4c1f120`, `main+0x4c1f140`;
   - against the save of the slot that loaded: no Bool, Int or Enum missing, no Bool or Enum different (the client
     passes up to 1%), only counters among the Ints; no placed object of the save unset live; the area and the place
     within it read right;
   - step 7: the heading from the rotation follows the way Link moves (same angle while he runs forward). If it is
     off by a constant (90°, 180°) or mirrored, the axis in the tracker patch's `yawFromRotation` changes.
   The log has one "first nx2saria.game_data of …" line per kind and no "could not be answered" line.
4. Collect a Gold Skulltula, open a chest, get a Heart Piece: step 6 again (no save in between) shows the token count
   up by one and the digest of Bool and of the placed objects changed.
5. hyrule-tracker with the patch (Windows: `npm ci`, `npm start` in its checkout with the patch applied; Linux the
   same): it finds the saves by itself (label "NX2Saria"), shows **Live** within a few seconds of loading the save,
   Link's pin moves and points the way he faces, and collecting something ticks its row without a save. The game's
   log shows the client's lines; the frame rate does not change with the tracker open (compare five minutes with and
   without, `NX2SARIA_TRACE=perf` if wanted).
6. On the title screen or in another file, the tracker says the game does not match the save, and goes back to Live
   when the right file is loaded.
7. Atlas and hyrule-tracker open at the same time: both live, two clients in the log, none refused.
8. Read the session's `tracker api:` lines: the summaries every 10 minutes count `nx2saria.game_data`, and nothing is
   refused.
9. Then decide: pin the manager's vtable (`main+0x4c1f120`, seen in the first run) in `gd_manager`, and offer the
   tracker PR to its author (the heading is settled).
