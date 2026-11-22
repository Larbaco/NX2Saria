# The neighbouring map the game preloads

Ocarina of Time 1.1.1 preloads one neighbouring map at a time, and it picks that map again at every frame from Link's
place and heading. Where two maps' load radii overlap, such as the Market's square, walking and turning around makes
the pick flip between them. Each flip cancels the map that was loading and starts the other one from the beginning,
so the game's loader threads keep loading maps it then throws away.

The patch set `changebank_hold` (`recomp/patches-v111.txt`, `runtime/patches/changebank_hold.c`) holds the map being preloaded
until the game's pick has left it for 2 seconds. Game version 1.1.1 only; on by default.

**[C]** marks what was read in the game's code (build `e0f3ccfa`) or seen in a run. **[I]** marks what is inferred.
The idea comes from a fix in another project for the same game; no code was taken from it, and every address below
was read again in this build.

## What the game does

- **[C]** The manager is made by `main+0xd12c00`: its vtable is `main+0x4ba1a28`, and its update is the vtable's
  `+0x30`, `main+0x3299880`, called each frame with the manager in `x0` (kept in `x19`).
- **[C]** It has 16 units of `0x128` bytes from `+0x408`. In a unit:
  - `+0` is 1 while it is in use;
  - `+2` marks a unit asked for directly;
  - `+4..+0xc` is its place;
  - `+0x10` is its map's path: the constructor points it at the unit's own buffer at `+0x1c`, and `+0x18` holds that
    buffer's size, `0x100`;
  - `+0x120` is its load radius and `+0x124` its priority.
- **[C]** The update's loop (`main+0x3299c54` to `main+0x3299d34`) goes over the units in use whose radius Link is in.
  While Link moves, it keeps the one he heads for most; otherwise the nearest. That unit ends in `w15`, and a unit asked
  for directly in `w17` (`-1` each for none).
- **[C]** `main+0x3299d90` to `main+0x3299d9c` take the pick: `cmp w15, #0`, `fmov s0, #4.0`,
  `ccmn w17, #1, #4, lt` and `csel w21, w17, w15, gt`, that is `w21 = w15 < 0 && w17 >= 0 ? w17 : w15`.
- **[C]** The update then asks for unit `w21`'s map through `main+0x329a540`. That call does nothing when the map is the
  one already asked for (manager `+0x1e8`) or loaded (`+0x70`); otherwise it starts the new map over the one loading.
  Then the update lets go of every other unit it had asked for (`main+0x329a400`, and the unit's `+1` cleared).
- **[I]** So a pick that flips between two maps throws away a load at every flip, and a flip back starts the first map
  again from the beginning.

## What the set changes

- **[C]** The set's site is the loop's `add x10, x10, #1` at `main+0x3299d28`, which every pass of the loop goes
  through. It keeps the `add` and then calls the patch page's `+0xe30`. That host function returns at once until
  `x10` is 16 (the last pass), and then it can put the held unit in `w15`.
  - After the loop, `cmp w15, #0`, `ccmn` and `csel` take the new `w15`: with it 0 or more, the `ccmn` sets Z and the
    `csel` takes `w15`. The host changes `w15` only when the game's pick is `w15` or none, never for a direct request
    (`w15` -1, `w17` 0 or more), so the pick is the new `w15`.
  - No flags are live at the site: the `cmp x10` two words on writes them before anything reads them. That matters
    because the generator counts a patched call as no flag definition for every alternative (`recomp/flags.py`), so a
    flag read after such a call would read stale flags, even with the set off. For the same reason it refuses a
    patched call at a site that reads or writes flags (`recomp/gen.py` `Emitter.patched_effect`).
  - `x10` and `x15` are caller-saved, so the lifted code has them in the `Cpu` around the call (`recomp/regs.py`). A
    callee-saved register such as `x21`, which holds the pick, stays in the function's local and would lose a write.
  - `x30` is the update's own: it is saved in the prologue and read again only in the epilogue.
  - Nothing else changes.
- The rule (`runtime/patches/changebank_hold.h`, `cb_step`) holds the unit being preloaded and hands the game its own pick:
  - at the first pick;
  - for a direct request;
  - when the pick names the held map, through the same unit or another unit of the same map, so no load is cancelled;
  - when the held unit is no longer in use, or holds another map now;
  - after updates more than half a second apart (a loading screen, a pause), or with another manager (another area);
  - with a hold of 0.
- Otherwise the update goes on with the held unit, until the game's pick has named other maps (or none) for the whole
  hold. The hold counts from the first update that left the held map, so a pick that wanders between two other maps is
  still taken once the hold is over. Then the game's pick of that update is taken. A pick that comes back to the held
  map before the hold is over starts the count again: a pick that flips back at least once every 2 seconds keeps the
  held map, which is what the hold is for.
- Before every read, the host checks the manager:
  - its vtable;
  - every unit's in-use byte (0 or 1);
  - every unit's path pointer and buffer size, as the constructor sets them.

  A manager that does not match, or a `w15` or `w17` that is not a unit or -1, keeps the game's pick at that update; it
  is counted and logged once.
- At start, a `main` whose word at the site is not 1.1.1's keeps the set off, with a line in the log.

## Settings and log

| Switch | Meaning |
|---|---|
| `NX2SARIA_CHANGEBANK_HOLD` | `0` turns the set off (the game's own code at the site). Default `1`. |
| `NX2SARIA_CHANGEBANK_HOLD_MS` | How long the game's pick must have left the held map before it is taken. Default `2000`. `0` (or a negative number) gives the game's pick at every update, with the counts still kept, so the log shows how often the game's own pick flips. At most `60000`. |

At start, one of:

```
changebank hold: on, 2000 ms: the neighbouring map the game preloads changes only after its choice has left it for 2000 ms, ...
changebank hold: on, hold 0 ms (NX2SARIA_CHANGEBANK_HOLD_MS = 0): the game's own choice of the neighbouring map to preload at every update, counted for the log
changebank hold: off (NX2SARIA_CHANGEBANK_HOLD=0): the game preloads whichever neighbouring map it chooses at each update, as shipped
```

In play:
- **The first 8 holds that ended:** a line each, either `kept <map> preloading while the game chose <other> for N ms,
  then it chose <map> again` or `the game's choice left <map> for the whole 2000 ms hold: <other> is taken in its place`.
- **At most once a minute, and only when the pick moved:** `changebank hold: last N s, N updates: the game's choice
  of map changed N times, the map asked for N times (each a request for another map); N choices held back, N back to the held map
  within the hold (a cancelled load and its reload avoided), N taken after 2000 ms; N updates went on with the held map`.

At the orderly end: `changebank hold: this session (...)`, with the same totals plus the holds started again and the
updates with a manager that did not match.

## Checks

- `tests/changebank_hold_check.py`:
  - lifts the site as the generator does, runs the generator's own check of a patched site on it, and checks that only
    this set calls the slot;
  - then builds `changebank_hold.c` with the runtime stubbed, runs the rule over stand-in units, and calls the host
    over a stand-in manager laid out as the constructor lays it out.

## Runs

On a Linux PC with an 8-core Intel CPU and an RX 6700 (RADV), build `e0f3ccfa`:
- `boot_map = Market_Child`, uncapped, no window;
- the mesh fallback on;
- an input script that walks Link into the square and then in circles and sweeps across it for about 90 seconds.

The walk does not repeat from run to run: the stick is relative to the camera, and the camera's lag follows the frame
times, so Link's path drifts into the crowd differently each time. The game's own pick flipped between 1 and 156 times
a run. So the runs are compared by what each did with its flips, not by one path.

| Runs | Flips of the game's pick | Maps asked for | Held back, came back within the hold | fps |
|---|---|---|---|---|
| hold 0 (the game's pick), 16 threads, 3 runs | 1, 33, 156 | the same as the flips | — | 45.1, 42.0, 40.3 |
| hold 2000, 16 threads, 2 runs | 7, 60 | (not counted in that build) | 3 of 3, 28 of 31 | 46.9, 42.6 |
| hold 0, 4 cores (`taskset -c 0-3`), 6 runs | 1 to 57 | the same as the flips | — | 28.9 to 30.9 |
| hold 2000, 4 cores, 6 runs | 1, 1, 2, 29, 54, 80 | 1, (not counted), 2, 1, (not counted), 6 | 14 of 14, 24 of 29, 37 of 42 | 28.2 to 31.6 |

- Where Link walked between the two radii, the hold cut the maps the update asked for from the game's 29 and 80 flips
  to 1 and 6.
- The game's loader threads ran in bursts, which track the maps asked for: with hold 0, 55 asks took 7.6 s of their
  CPU in 100 s against 2.4 s for 1 ask.
- **The frame rate did not change measurably on this PC,** even held to 4 cores: hold 0 runs with 55 and 1 flips ran
  at 30.0 and 30.6 fps. Here the frame is set by the replay and the GPU, and the loads run on other cores. The gain
  measured elsewhere (fewer cores, a busier CPU) is not shown here.
- Every run ended normally, the settings file was unchanged, and the kernel log had no GPU line.

## Risks

- **A map change** comes from the game's own transition, not from this pick, so it is not held up. If a pick is held
  back while Link reaches a map's edge, that map may not be preloaded yet, and the transition loads it as it would
  without a preload. [I]
- **Updates more than half a second apart** start the hold again and take the game's pick at once: a hitch that long
  in the middle of a flip lets that flip through.
- **A pick that comes back to the held map at least once every 2 seconds** keeps the held map for as long as that goes
  on. This is what the hold is for.
- **Another build of the game** keeps the game's own code: the site's word is checked at start. A manager that does not
  look as this build's constructor makes it keeps the game's pick, with a line in the log.
