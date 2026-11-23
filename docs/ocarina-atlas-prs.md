# Pull request drafts for Ocarina Atlas support

The two pull requests as drafted. Both were opened on 2026-11-17: the fork's as
[NX2Saria#7](https://github.com/Larbaco/NX2Saria/pull/7), Atlas's as
[Ocarina-Atlas#40](https://github.com/antonioalvromero/Ocarina-Atlas/pull/40), merged the same day and in Atlas v2.3.0
and later. The texts below are the drafts; the pull requests carry the final ones. Background:
[ocarina-atlas.md](ocarina-atlas.md).

## 1. Fork (Larbaco/NX2Saria): the tracker API

**Branch:** `atlas-integration`, from `main` at `0170e49d`. **Base:** `main`.

**Title:** Let a map tracker on this PC read the game's memory, read-only, through 127.0.0.1 (tracker_api)

**Description:**

> A map tracker that follows Link and the collectibles live (Ocarina Atlas, for one) reads this game through a debug
> server that console emulators run on `127.0.0.1:6545`. This adds that server to the runtime, off by default:
>
> - `tracker_api = 1` (`[tracker]` in `nx2saria.ini`, or `NX2SARIA_TRACKER_API=1`) starts a listener on
>   `127.0.0.1:<tracker_api_port>` (6545). It answers `status`, `read_memory` and `search_memory` as trackers ask them,
>   and `nx2saria.state` (area, Link's position, presents, where main and the heap are). Read-only, guest memory only,
>   this PC only; limits on clients, line length, read size and search time. docs/tracker-api.md is the protocol.
> - With it on, the stack and heap block is placed from 64 GiB: trackers take a heap pointer below `0x1000000000` for
>   none. Off, the address space is as before. mem_init reads the settings before placing that block.
> - Nothing runs on the game's threads. Reads copy through `plat_mem_read`, which cannot fault, and only from the
>   game's own regions. A search copies 1 MiB pieces, outside the modules only pages that hold memory now, at most
>   4 GiB/s and 8 GiB a minute for all searches, and holds no lock of memory.c; a search for main's header ends with the
>   modules.
> - The platform layer gets a loopback-only TCP listener (`plat_tcp_*`) and the resident pages of a range
>   (`plat_mem_resident`), on Linux and Windows.
> - The log: one start line (off; or on, where it listens, and where the heap is), a line at each client's first
>   request and departure, the first answer of each method and first refusal of each kind, the first port check, and
>   a summary every 10 minutes, all bounded per session.
>
> **Tested:** `tests/tracker_api_check.py` (the API over the real platform layer with a stand-in guest memory at the
> game's addresses, talked to as Atlas does) on Linux; the static checks (`check_platform`, `cfg_rows_check`,
> `branding_package_check`). **Not yet:** a build and a run of the game; the checklist is in docs/ocarina-atlas.md.

**Commits:** one per step, as pushed: the platform listener; the API, its settings and start line; its check; the heap
placement; the bench client; the docs.

**Review notes:** the heap placement (`runtime/memory.c` `mem_init`) is the one change that touches the game's memory
layout, and only with the setting on; it can be dropped if Atlas takes the optional PR below (then `tracker_api = 1`
works with the heap where it is).

## 2. Atlas (antonioalvromero/Ocarina-Atlas): NX2Saria as a source

<!-- code-docs: other repository -->
Paths in this section are in the Ocarina Atlas repository.

**Patch:** [ocarina-atlas-pr.patch](ocarina-atlas-pr.patch), against Atlas `main` at `005c711` (applies cleanly).
Six files, about 110 lines added.

**Title:** Support NX2Saria: its tracker API as a live source, its save folder, and its heap floor

**Description:**

> [NX2Saria](https://github.com/Larbaco/NX2Saria) is a native PC port of the game (version 1.1.1, the build Atlas's
> offsets fit) for Windows and Linux. With `tracker_api = 1` in its `nx2saria.ini` it serves the debug protocol Atlas
> already speaks (`status`, `read_memory`, `search_memory`) on `127.0.0.1:6545` itself, read-only and for this PC
> only, so live tracking works with Atlas as it is: the launchers find the port taken and start no bridge. Its guest
> addresses are its host addresses (main at `0x7000000000`); `status` answers `"emulator": "nx2saria"`.
>
> This PR makes Atlas name it and find its saves:
>
> - `nx2saria` as an `EmulatorKind`, shown as "NX2Saria" (`lichii-types.ts`, `LichiiStatusPanel.tsx`,
>   `lichii-server.ts`).
> - `NX2SARIA_SAVE_DIR` for the save-only mode: NX2Saria's `profile` folder, which holds
>   `save/<account>/slot_XX/{caption,progress}.sav`. The Linux launcher sets it from a running `ocarina` / `nx2saria`
>   process (its `NX2SARIA_PROFILE_DIR`, else `profile` beside its executable); the Windows launcher from
>   `ocarina.exe` / `nx2saria.exe`. A slot under that folder is labelled NX2Saria.
> - While the source is `nx2saria`, heap pointers count from 4 GiB instead of 64 GiB: NX2Saria places its heap from
>   64 GiB while the API is on, but a build that does not leaves it from 4 GiB.
> - A README entry.
>
> No bridge change: NX2Saria needs none. Tested: the TypeScript parses after type stripping, the Linux launcher
> passes `node --check`, `OcarinaAtlas.cs` builds for .NET Framework 4.8. To run before merging: `npm run typecheck`,
> `npm test`, a session with NX2Saria (Live, labelled NX2Saria; Save only after the game closes).
>
> Contributed under the AGPL-3.0.

**Before opening it:**

1. `npm ci` (or `bun install`), `npm run typecheck`, `npm test` in an Atlas checkout with the patch.
2. A session with NX2Saria on Windows and on Linux (docs/ocarina-atlas.md checklist, steps 6, 7 and 9).
3. Atlas's `CHANGELOG.md` entry, if the maintainer wants it in the PR.
4. The copyright holder's agreement to contribute the new lines under the AGPL-3.0.

**What the patch changes, file by file:**

| File | Change |
|---|---|
| `src/lib/lichii-types.ts` | `EmulatorKind` gets `'nx2saria'` |
| `src/components/LichiiStatusPanel.tsx` | `getEmulatorDisplayName`: `'nx2saria'` → "NX2Saria" |
| `src/lib/lichii-server.ts` | `getCandidateSaveDirs` reads `NX2SARIA_SAVE_DIR`; `detectEmulatorFromPath` names a slot under it `nx2saria`; `MIN_GUEST_HEAP_PTR` becomes a `let` that `updateMinGuestHeapPtr` sets from each fresh `status` (4 GiB for `nx2saria`, else 64 GiB) and resets on disconnect; `nx2saria` joins the `status.emulator` values taken as they are |
| `launcher/linux/ocarina-atlas-cli.mjs` | `detectNx2SariaProfileDir` (`/proc/<pid>/comm` = `ocarina` or `nx2saria`; its environment's `NX2SARIA_PROFILE_DIR`, else `<exe dir>/profile`); sets `NX2SARIA_SAVE_DIR` for the server |
| `launcher/OcarinaAtlas.cs` | `ResolveNx2SariaProfileDir` (processes `ocarina` and `nx2saria`, `MainModule.FileName`'s folder + `profile`); sets `NX2SARIA_SAVE_DIR` for the server unless the environment has it |
| `README.md` | The NX2Saria entry in the live sync setup, and `NX2SARIA_SAVE_DIR` in the variables table |
