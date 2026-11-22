# Ocarina Atlas and NX2Saria

[Ocarina Atlas](https://github.com/antonioalvromero/Ocarina-Atlas) is an interactive map and collectible tracker for
this game. It follows Link on the map and ticks off the 184 collectibles it knows (100 Gold Skulltulas, 36 Heart
Pieces, 8 Heart Containers, beans, fairies, bottles, "thoughts") from the game's memory while the game runs. This page
records what it takes to use it with NX2Saria: how Atlas reads the game, what breaks here, what the fork changes, what
Atlas could change, and what to run once the game may be built again.

Studied: Atlas `main` at `005c711800d795bb887a5972d3aaeac6a621e69b` (v2.3.0-linux.1), NX2Saria `0170e49d` (0.14.0),
game version 1.1.1, main build `e0f3ccfaf50065ae`.

## Conclusion

**Feasible, with changes in the fork; none are required in Atlas.**

- Atlas reads 1.1.1. Every offset it uses fits NX2Saria's main: its MOD0 search pattern (`MOD0` and the offsets
  `0x1036754`, `0x10ffb24`, `0x125346c`) is exactly main's header at `main+0x3c4705c`, and its `.bss` slots and
  vtables fall in main's `.bss`, `.data` and `.rodata` (checked against the local main with `recomp/nso.py`).
- Atlas cannot reach NX2Saria as it is: it reads through a debug server on `127.0.0.1:6545` that emulators run, or
  through a bridge that reads other PC ports' memory at host `0x100000000000 + guest`. NX2Saria has neither; its guest
  addresses are host addresses (main at `0x7000000000`).
- With the fork's **tracker API** (`tracker_api = 1`, this branch), NX2Saria serves the three methods Atlas uses on
  `127.0.0.1:6545` itself, read-only and for this PC only. Atlas's launchers see the port taken and start no bridge.
- One more thing breaks: Atlas takes every heap pointer at or below `0x1000000000` for none, and NX2Saria's heap is
  near `0x130000000`. With the API on, the fork places its stack and heap block from 64 GiB, above that floor.
- Atlas then needs no change to track live. A small optional Atlas PR (drafted below) shows "NX2Saria" instead of
  "Lichii" as the source, finds NX2Saria's save folder for its save-only mode, and accepts NX2Saria heap pointers
  from 4 GiB, so that a build without the heap move works too.
- Tried with the game on the Linux bench (2026-11-17): an 11-minute session, about 49,800 frames, with Atlas live.
  Atlas found main's header in 25 ms and made 24,497 requests in ten minutes (551 `status`, 23,863 `read_memory`, 83
  `search_memory`), none refused; on the title screen the 8 GiB-a-minute search ceiling was reached once and the game
  went on. Windows has not been tried yet. The protocol and the server are also tested against a stand-in guest memory
  (`tests/tracker_api_check.py`).
- The Atlas PR was merged upstream ([Ocarina-Atlas#40](https://github.com/antonioalvromero/Ocarina-Atlas/pull/40)) and
  is in Atlas v2.3.0 and later, which label the source "NX2Saria".

## What Atlas is

- **Language and parts.** A TypeScript web app (React 19, TanStack Start, Vite; Node 22/24) whose server side
  (`src/lib/lichii-server.ts`) reads the game and serves the map on `http://<host>:3000`, also to a tablet on the LAN.
  A Windows launcher in C# WinForms (`launcher/OcarinaAtlas.cs`, .NET Framework 4) and an experimental Linux one
  (`launcher/linux/ocarina-atlas-cli.mjs`) start it. A C99 bridge (`launcher/linux/longshot_rpc_bridge.c`) and a C#
  one inside the Windows launcher read other PC ports' memory.
- **Platforms.** Windows x64; Linux x64 as an experimental pre-release.
- **How it gets the game's data.**
  1. *Live, from memory*, over one TCP connection to a **debug server** (`LICHII_HOST:LICHII_PORT`, default
     `127.0.0.1:6545`) that speaks newline-delimited JSON. Console emulators expose it when told to
     (`EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` in their `nx2_switches.txt`).
  2. *Live, from another PC port's process memory*, through the bridges: they find a process by name (`longshot`,
     `hookshot`, `relink-game`, `relink`, `triforce`), read it with `process_vm_readv` / `ReadProcessMemory` at
     `0x100000000000 + guest address`, and answer the same debug protocol on `:6545`. They start only when nothing
     listens on the port and such a process runs.
  3. *Save files* when nothing is live: the newest `slot_*/caption.sav` and `progress.sav` (the game's own save format)
     under the emulators' and ports' save folders or `*_SAVE_DIR` overrides. Position, scene and collected flags come
     from them, marked "(Saved)".
  - No log reading, no WebSocket to the game, no game modification.
- **Protocol it needs.** Only `status`, `read_memory` and `search_memory` (see [tracker-api.md](tracker-api.md)). It
  sends one request at a time, polls every 120 ms while live (750 ms otherwise), refreshes `status` every second,
  and on any call timeout (700 ms for `status`, 400 to 800 ms for reads, 1300 to 1800 ms for searches) closes the
  socket and forgets every address it found.
- **How it finds things.** It searches for main's `MOD0` header (first in `[0x08000000, 0x200000000)`, then
  everywhere) and checks main's first two words; everything else comes from pointers in main's `.bss`, checked against
  vtables in main. There is no build-id or version check: a different build fails quietly into "Waiting for game".

## Compatibility

| Part of Atlas | NX2Saria today | With this branch (`tracker_api = 1`) |
|---|---|---|
| Debug server on `:6545` (live) | Nothing listens | The runtime answers `status`, `read_memory`, `search_memory` |
| Finding main by `MOD0` | — | Found at `0x7003c4705c` (main `0x7000000000`); the first search, below 8 GiB, finds nothing and the second finds it |
| `.bss` slots, vtables, offsets | Same build (1.1.1, `e0f3ccfa`) | Same addresses, relocated by the runtime's loader as on the console |
| Heap pointers above `0x1000000000` | Heap near `0x130000000`: every chain is refused | Heap from `0x1030000000` |
| Linux bridge | Wrong process names, and it needs host `0x100080000000` mapped | Not started: the port is taken (it would not attach anyway) |
| Windows bridge | Same | Same |
| Save-only mode | Works with `LICHII_SAVE_DIR=<game>/profile` (labelled "Lichii") | Same; the optional Atlas PR finds the folder by itself |
| Source label | — | "Lichii" (an unknown `emulator` falls back to it); "NX2Saria" with the optional PR |
| `yama.ptrace_scope` on Linux | — | Irrelevant: no process reads another |

Process names: `ocarina` / `ocarina.exe` (build kit and launcher installs) or `nx2saria` / `nx2saria.exe` (developer
builds). Saves: `<game folder>/profile/save/<account>/slot_00..05/{caption,progress}.sav`, where the profile folder
is `NX2SARIA_PROFILE_DIR` or `profile` beside the executable. The port writes saves through `profile/save-tx` first,
so the files can trail the game by a commit.

## Data and where it is in the fork

Version 1.1.1, main at `0x7000000000`; `main+X` is guest (and host) address `0x7000000000 + X`. The fork's readers are
in `runtime/diag/debug_tools.c` and `runtime/pacing/frame_rate.c`; `tools/tracker_api_client.py` reads each row below through the
API and compares the two chains where there are two.

| Data | How Atlas reads it | Where it is in NX2Saria | The port's own reader |
|---|---|---|---|
| main's base | `MOD0` search, hit − `0x3c4705c`; main+0 = 1, main+4 = `0x3c4705c` | `0x7000000000` (`MOD_MAIN_BASE`, `recomp/layout.py`); header at `0x7003c4705c` | `nx2saria.state.main` |
| Object checks | vtables main+`0x4ba1818`, `0x3ee4f8c`, `0x4ba27c8`, `0x4c58270`, `0x4c20658`, `0x4c20718`, `0x4c20e98` | In main's `.data` (`0x3ee4f8c` in `.rodata`); objects hold main+offset after relocation | — |
| Player object | `*(main+0x4e6c7a8)` (else `+0x4e6c938`) + 0x10; +0x50 part count (37..128), +0x58 parts | `0x7004e6c7a8` / `0x7004e6c938`, main's `.bss`; the object in the heap | — |
| Link's position | part 36: +0x1c/+0x20/+0x24 x, y, z; +0x28/+0x30 facing | Heap | `[[main+0x4e371d0]+0x480]+0x31c` (the "Player" actor; `dbg_player_pos`, `frame_rate.c player_pos`); `nx2saria.state.player` |
| Scene | part 7 + 0x34: `Work/Banc/Scene/<dir>/<name>.bcett` | Heap | Game-data text `Sequence_CurrentBanc` (`dbg_area`); `nx2saria.state.area` |
| Age | part 23 + 0x80, or the scene's `_Child` / `_Adult` | Heap | The area's name suffix |
| Game-data manager | `*(main+0x4e90ce8)`, vtable main+`0x4c58270` | `0x7004e90ce8`, main's `.bss` | `**[main+0x4c7e180]` is the manager + 0x168 (`debug_tools.c gmd`; to confirm on the bench) |
| On/off flags (Heart Pieces, beans, fairies, bottles, thoughts) | manager + 0x170: count +8, entries +0x10 (0x18 each, value at +0x16), {hash, index} table +0x18, size +0x24; MurmurHash3 of the flag's name | Heap | The same container: `debug_tools.c` section 0 (`flags_list`) |
| Number flags | manager + 0x230, entries 0x28 each, tag +8, value +0x1c | Heap | `debug_tools.c` section 2 (manager + 2 × 0x60) |
| Gold Skulltula count | number flag `0x04735978`, tag `0x20`, 0..100 | Heap | — (the client tool reads it) |
| Collected Skulltulas, Heart Containers and Pieces | sorted {u64 hash, u64 flags} table, manager + 0xdc8 (count + 0xdc0), bit 32 | Heap | — |
| The save in memory | `*(manager + 0x1160)`, progress.sav's layout | Heap | — |
| Title, running | `status` | — | `status` (`program_id`, `powered_on`) |
| Save files | `slot_*/caption.sav`, `progress.sav` | `profile/save/<account>/slot_XX/` | — |

The hash `0x04735978` is Atlas's; the name behind it was not confirmed (MurmurHash3 of `Item_Token_Gold.StockNum` is
`0xa4b153fc`, so that is not it). The client tool checks the entry's tag and range on the bench.

## Changes in the fork (this branch)

| Change | Where |
|---|---|
| A TCP listener bound to `127.0.0.1` only, accepted connections with `TCP_NODELAY`, sends with a deadline, accept errors reported; which pages hold memory now (`mincore`, `QueryWorkingSetEx`) | `runtime/platform/platform.h`, `linux.c`, `win32.c` (`plat_tcp_*`, `plat_mem_resident`) |
| The tracker API: listener and client threads, the JSON reader, `status`, `read_memory`, `search_memory`, `nx2saria.state`, limits, log lines | `runtime/diag/tracker_api.c` (new), `runtime/host.h`, `runtime/main.c` (`tracker_api_init` beside `discord_init`) |
| Settings `tracker_api` (0/1) and `tracker_api_port` (6545), restart-only, section `[tracker]` | `runtime/cfg/cfg.c` (`known[]`, `sections[]`, `metas[]`), `nx2saria.ini.example` |
| The stack and heap block from 64 GiB while the API is on; settings read before that block is placed | `runtime/memory.c` `mem_init` |
| A check that builds the API with the real platform layer and a stand-in guest memory and talks to it as Atlas does | `tests/tracker_api_check.py`, `tests/tracker_api_check.c` |
| The `tracker_api_on` stub for the two checks that build `memory.c` | `tests/heap_lazy_copy_check.c`, `tests/lazy_fetch_check.c` |
| A bench client that walks Atlas's chains through the API | `tools/tracker_api_client.py` |
| The protocol and this page | `docs/tracker-api.md`, `docs/ocarina-atlas.md`, `docs/ocarina-atlas-prs.md`, `docs/ocarina-atlas-pr.patch` |

Not done, for later: a row for the setting in the settings menu (it is ini and environment only), a launcher toggle,
and making the setting live (the heap is placed at start, so it stays restart-only).

## Changes in Atlas

None are required. The optional PR was merged as
[Ocarina-Atlas#40](https://github.com/antonioalvromero/Ocarina-Atlas/pull/40) (in Atlas v2.3.0 and later; draft in
[ocarina-atlas-prs.md](ocarina-atlas-prs.md), first patch [ocarina-atlas-pr.patch](ocarina-atlas-pr.patch)):

1. `nx2saria` as a source kind with its label "NX2Saria" (`lichii-types.ts`, `LichiiStatusPanel.tsx`,
   `lichii-server.ts`).
2. `NX2SARIA_SAVE_DIR` for the save-only mode; both launchers set it from a running `ocarina` / `nx2saria` process
   (its `profile` folder; on Linux also its `NX2SARIA_PROFILE_DIR`).
3. Heap pointers from 4 GiB while the source says `nx2saria`, so a fork build that does not move the heap still works.
4. A README entry.

Not proposed: teaching the bridges NX2Saria. They would need a guest-to-host offset of 0 and a probe at
`0x7000000000` instead of `0x100080000000`, plus `ptrace` rights on Linux; the in-process API needs none of that.

## The API: design

**A mod or a runtime module?** A runtime module. A mod in the port's mod folder can bring RomFS files, ExeFS patches
(`.ips` / `.pchtxt`, built into patch sets at the next start) and cheats, but no code of its own: a mod with code
modules (`exefs/subsdk*`, a replaced main) is turned off, as the port has no module loader (`runtime/patches/mods.h`
`MOD_PLUGIN`). An ExeFS patch changes the game's instructions and cannot open a socket. So the API is part of the
runtime, compiled in, and off until the player turns it on.

Options weighed:

| Option | For | Against | Verdict |
|---|---|---|---|
| **TCP on 127.0.0.1, newline-delimited JSON, the debug protocol Atlas already speaks** | Atlas works unchanged; Node, Python, C# all read it; Winsock is already linked (`-lws2_32`); same code on Linux and Windows | Raw memory reads: the tracker keeps knowing the game's structures | **Chosen**, with `nx2saria.state` for the port's own stable fields |
| WebSocket | Browsers could connect directly | Atlas's server is Node, which needs no WebSocket; a browser-reachable port is a larger attack surface; HTTP handshake and framing to write | No |
| Shared memory with a state block | Cheapest reads | Not reachable from Node without a native module; a different API on Linux and Windows | No |
| JSON state file or named pipe | Simple to read | File: disk writes many times a second and stale reads; pipe: one reader, Windows-only semantics | No |
| Atlas's bridge reading NX2Saria's process | No fork change | Needs Atlas changes for a 1:1 address map, `ptrace` rights on Linux, a heap-floor change; reads memory pages that may be unmapped | No |

Design points:

- **Opt-in, restart-only.** `tracker_api = 1` or `NX2SARIA_TRACKER_API=1`; port `tracker_api_port` (1024..65535,
  default 6545). Off, nothing listens and the only cost is the start line.
- **Only this PC.** Bound to `127.0.0.1`; there is no setting for another address. Atlas's own server is what serves
  a tablet on the LAN.
- **Read-only, guest memory only.** Reads and searches cover only the game's own regions (`mem_guest_ranges`: modules,
  stack, heap, mapped physical memory, thread storage) and within them what the game can read (`mem_readable`,
  `mem_query`). The game's memory map answers for the host's allocations below 2^39 too (as taken memory), so the
  regions, not the map, are the boundary; the runtime's memory and the keys it read stay out. No method writes.
- **Frame cost.** Nothing on the game's threads: no per-frame hook, no copy. Reads happen on the API's host threads,
  through `plat_mem_read`, which cannot fault. Values may be read mid-update, as with an emulator's debug server; a
  seqlock-guarded snapshot copied at the game's pad read (`ov_input.c npad_states`, where `dbg_tick` runs) is the way
  to make `nx2saria.state` consistent if ever needed.
- **Search cost.** Atlas searches the heap every few seconds whenever it cannot follow its pointers (title screen,
  loading), each time for 1.5 s, and asks for four copies of main's one header. So: a search for the whole space ends
  with the modules when they hold the pattern (main is found in a few tens of milliseconds); outside the modules only resident pages
  are copied (reading a page the game never wrote would allocate it: the heap is a shared memory file on Linux); one
  search reads at most 4 GiB/s and all of them 8 GiB a minute, about two seconds of reading; the budget counts from
  the request's arrival so an answer is never later than Atlas's timeout allows.
- **Portability.** The same `tracker_api.c` on both systems; the sockets are in the platform layer
  (`tools/check_platform.py` passes).
- **Protocol.** `status`, `read_memory`, `search_memory` as Atlas uses them, and `nx2saria.state` (area, Link's
  position, presents, main's and the heap's places), versioned by `api_version` (1). Fields are documented in
  [tracker-api.md](tracker-api.md).
- **Limits.** 4 clients, lines under 4096 bytes finished within 5 s, 256 KiB reads, 64-byte patterns, 64 hits, a 3 s
  search budget, one search at a time, 60 s idle, 2 s to take an answer. A first byte that is not `{` (a web page's
  HTTP request or TLS handshake) closes the connection at once, so a page cannot hold the four places; strict JSON;
  method names are made printable before they reach the log.

## The prototype

State: written, built only in its test harness, not run with the game.

- `tests/tracker_api_check.py` passes on Linux: it builds `runtime/diag/tracker_api.c` with the real
  `runtime/platform/linux.c`, places a stand-in guest memory at the game's addresses (main's window at `0x7000000000`
  with the `MOD0` header at `+0x3c4705c`, a heap at `0x1030000000` with a never-committed hole and an unreadable page)
  and host memory with a secret between the regions (answered by the stand-in memory map as readable, as
  `memory.c` does), and checks, as Atlas parses them: the status fields; `MOD0` found by the two searches Atlas makes,
  the second ending with the modules; reads (hex, 256 KiB, bad, unreadable and host ranges, malformed addresses); heap
  searches (a pattern across two copies, the hole and the unreadable page stepped over, `max_hits`, `start`/`end`,
  "all" going on to the heap); the secret never found or read; `nx2saria.state`; unknown and write methods refused, a
  method with a line end not starting a log line; ids (string, number in another spelling, missing), blank lines, two
  requests in one write; an HTTP request, a TLS handshake, trailing text, a JSON array, ids JSON does not allow, an
  overlong id, a 4096-byte line and an unfinished request closing the connection; the client limit; the searches'
  minute of bytes running out; the heap's resident pages the same before and after every search; and the log lines.
- A second reviewer read the API and every diff before the commits; the issues it raised (host memory reachable through
  the memory map's taken ranges, heap pages allocated by searches, search answers later than Atlas's timeout, a web
  page holding the client places, JSON and log-line edge cases, a busy loop on accept errors) are fixed and tested
  above.
- `runtime/platform/win32.c`, `runtime/diag/tracker_api.c` and the two Windows-only checks pass a syntax check with clang
  for `x86_64-w64-mingw32` (the only error is the existing one about `SetThreadDescription`, from this machine's
  older MinGW headers). The Windows check itself was not run.
- `tools/check_platform.py`, `tests/check_platform_test.py`, `tests/cfg_rows_check.py`,
  `tests/branding_package_check.py` and `tests/upscaler_idle_check.py` pass.
- `tools/tracker_api_client.py` runs against the harness (steps 4 and 5 report "no player yet", as the harness has
  no game objects).
- The Atlas patch: the TypeScript files pass a syntax check after Node's type stripping, the Linux launcher passes
  `node --check`, and `OcarinaAtlas.cs` builds (original and patched) with `dotnet` for .NET Framework 4.8. Atlas's
  own `tsc` and `vitest` were not run (its dependencies were not installed).

## Expected log lines

Off (every run):

```
tracker api: off (tracker_api = 1 in nx2saria.ini or NX2SARIA_TRACKER_API=1 lets a map tracker on this PC read the game's memory, read-only, on 127.0.0.1:6545)
```

On, with Atlas running:

```
tracker api: on, read-only, listening on 127.0.0.1:6545 (this PC only; status, read_memory, search_memory, nx2saria.state; API version 1); the game's heap at 0x1030000000, above the 0x1000000000 trackers take heap pointers from
tracker api: a program connected and left without a request (a tracker checking whether the port is taken); later checks are counted in the summaries
tracker api: client 2 (127.0.0.1:50412) asks: its first request is status (1 open)
tracker api: first status answered: powered_on 1, game 1.1.1, build e0f3ccfaf50065ae
tracker api: first search_memory answered: a 16-byte pattern in all regions, 0 hits in 0 ms (0.0 MiB read)
tracker api: first search_memory that found its pattern: a 16-byte pattern in all regions, 1 hit, the first at 0x7003c4705c in … ms (… MiB read)
tracker api: first read_memory answered: 8 bytes at 0x7000000000
tracker api: the last 10 min: 5012 requests (status 600, read_memory 4410, search_memory 2), 0 refused, 0.29 MiB sent, 300 port checks, 1 client open
tracker api: client 2 (127.0.0.1:50412) left after 812.4 s, the client closed the connection: …
```

The start line says `below` if the block did not fit at 64 GiB; Atlas then shows "Waiting for game" (without the
optional Atlas PR). Every line is listed in [tracker-api.md](tracker-api.md#the-log).

## Licenses

- **Atlas: GNU AGPL-3.0** (`LICENSE`, the plain text, no added terms). Its README applies it to the newly authored
  parts (the `lichii-*` files and `launcher/`), says the project it started from carries no license, and that the map
  and item data come from a third party. No source file carries an SPDX header.
- **The fork: MIT** for the base import (`LICENSE`), **all rights reserved** for the additions (`LICENSE-ADDITIONS`);
  the repository never copies GPL code (`AGENTS.md`).
- **What is allowed:**
  - The tracker API is the fork's own code, written from the protocol's description (what is sent and answered), not
    from Atlas's code. A wire protocol, addresses, offsets and hash values are facts needed to interoperate; using
    them puts no license on the fork.
  - Atlas and NX2Saria run as two programs that talk over a socket. The AGPL does not reach the fork through that.
  - Atlas's code, maps, item tables and other data are not copied into the fork, and must not be.
  - Bundling Atlas with an NX2Saria release would be allowed only under the AGPL for Atlas's part (its license, its
    source or a written offer, unmodified notices), and it would stay a separate program. Pointing players to Atlas's
    releases is simpler.
  - The Atlas PR's new lines would be contributed under the AGPL-3.0, Atlas's license. The patch file in `docs/`
    quotes a few lines of Atlas as diff context; those stay AGPL-3.0 and Atlas's. Opening the PR is the copyright
    holder's decision.
  - No PR or issue was opened in Atlas.

## Checklist for the first run

The Linux bench session of 2026-11-17 covered steps 3 and 6 on Linux (with Atlas's PR, so the label was "NX2Saria")
and the search ceiling of step 4. Windows, and the other steps on both systems, are still to run.

1. `python tests/tracker_api_check.py` on Windows as well (MSYS2 clang), and `python tests/lazy_fetch_check.py` and
   `python tests/heap_lazy_copy_check.py` there (they build `memory.c`, which now asks `tracker_api_on`).
2. Build this branch as usual (Linux and Windows) and start the game once with the API off: the log has
   `tracker api: off (…)`, and the heap is where it always is (`mem:` lines, `NX2SARIA_TRACE=perf` census if wanted).
3. Set `tracker_api = 1` (bench: in `test-linux.ini` / `test.ini`, keep a copy and compare it after), start the game,
   load a save. The log has `tracker api: on, … listening on 127.0.0.1:6545` and
   `the game's heap starts at 0x10…, above 0x1000000000`. Play a few minutes: nothing should differ from a run with
   the API off (frame rate, loading, saves).
4. Leave the game on the title screen with Atlas open for five minutes (Atlas searches the heap while it finds no
   player): the game's memory use (RSS, or the Task Manager's private bytes) does not grow, the frame rate holds, and
   the log has at most the `searches read 8192 MiB in the last minute` line.
5. `python tools/tracker_api_client.py --watch 30` while Link moves: all five steps `ok`, the two position chains
   agree, the manager matches the port's (`+0x168`), the Gold Skulltula count is right, reads take well under 1 ms.
   The log shows the client's lines and the first answers.
6. Atlas: download its release (Windows zip or Linux tarball, checksum checked), start it, start the map. The map
   shows Live (labelled "Lichii"), follows Link, the Skulltula count and the collectibles match the save. The game's
   log shows the port checks and the client's lines; Atlas's launcher log shows no bridge.
7. Close the game with Atlas open: Atlas goes to "Save only" with `LICHII_SAVE_DIR=<game>/profile` (or
   `NX2SARIA_SAVE_DIR` with the optional PR), and back to Live when the game starts again.
8. Edge cases: start Atlas before the game; start a second Atlas; open
   `http://127.0.0.1:6545` in a browser (the log shows the closed HTTP request, the game goes on); set
   `tracker_api_port` to a taken port (the log says it retries every 5 s); restart the game from the settings menu with
   Atlas connected (the new run listens again at once, or within a few tries).
9. The optional Atlas PR: in Atlas's checkout, `npm ci` (or `bun install`), `npm run typecheck`, `npm test`, then
   steps 6 and 7 with the label "NX2Saria", and once with the heap move off to check the 4 GiB floor (by building
   without the `memory.c` commit).
10. Read the whole session's `tracker api:` lines: counts, refused requests (Atlas's searches on ranges below 8 GiB
   answer empty, not refused), summaries every 10 minutes.
