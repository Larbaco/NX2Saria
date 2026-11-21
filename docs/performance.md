# Performance

How to measure the frame rate of a scene the same way twice, and how to read the result.

## Benchmark

`tools/bench.py` runs fixed scenes (`tools/bench/scenes.toml`) on a game folder and writes every frame's timing:

```sh
.venv/bin/python tools/bench.py run ~/Games/Ocarina --save <save backup> --label base
.venv/bin/python tools/bench.py summary build/bench/<date>-base      # the summary again from the logs
```

- **The save**: one of the game folder's `profile/save-backup/<time>` folders, or any save `tools/run_game.py` takes.
  The scenes teleport Link, so the save only has to be in the right age (child or adult) for them.
- **A run**: the game starts through `tools/run_game.py` on a copy of the profile, presses A until the save loads,
  teleports to the scene's area and flies the debug tools' free camera along a fixed path there
  (`runtime/debug_tools.c` `test_script`, game 1.1.1 only). The frame limit is off (`NX2SARIA_FPS_LIMIT=unlimited`);
  every other setting is the game folder's `nx2saria.ini`, copied into the results. `--set KEY=VALUE` adds a setting
  (`{tag}` in the value becomes the run's scene and number).
- **Why the free camera**: the game draws, culls and loads detail for the view the free camera gives it, and nothing
  in its way changes the path. Link running a route of points (`route`) met villagers who start a conversation,
  water and walls, and ended somewhere else each time. The cost of Link's own running (animation, collision) is
  therefore not in the numbers; he stands at the scene's start position. The path stays within about 8 m of that
  position: wider ones passed through walls into the void outside the map.
- **Steps at a game time**: `NX2SARIA_DBG_TEST` steps written `@<ms>:` wait for the world clock instead of a count of
  the game's pad reads, which come as often as frames: the teleport and the camera then start at the same game time
  on a fast build and a slow one. A build without this (0.10.1 and older) runs every `@` step at once.
- **Pictures along the way**: `--set NX2SARIA_SHOTS=w15-53:5 --set NX2SARIA_SHOTS_DIR=<dir>/shots-{tag}` saves a small
  picture every 5 s of the flight, to check where it went. Leave it off for numbers: each picture costs a readback.
- **Rounds**: `--runs 3` (the default) plays every scene once per round, three rounds, so a drift over time (heat,
  background work) reaches every scene alike. The summary gives the spread between runs.
- **Results** in `build/bench/<date>-<label>/`: per run the game's log and a CSV of every measured frame;
  `summary.md` and `summary.csv`; `meta.json` with the commit, the machine, the settings and the mode.
- **Play mode and `--shot`**: a run plays as a player does. Pipelines are built in the background, and a draw whose
  pipeline is not ready yet is skipped for that frame. `--shot` also saves a PNG of each run's last frame, to compare
  pictures between builds. The runtime then builds every pipeline on the render thread and waits for the boot's
  pipeline warm-up (`runtime/boot.c`), so the picture does not depend on build timing. A scene's first visit then
  stalls far longer than in play: on an RX 570 with empty caches, the Market was blocked on pipeline builds for 123 s
  with `--shot` and 15 s without. Compare runs of the same mode only; `summary.md` says which mode a run used, and
  results from before the mode was recorded were all `--shot` runs.
- **Boot map**: `--boot-map` starts each scene in its area without the title screen or the teleport: the scene's
  `warp` step becomes the boot map (`docs/boot-to-map.md`), the area loads once, from the save, and nothing is pressed.
  A run gets to its scene sooner and always the same way, but its warm-up starts at the arrival instead of in the
  save's own area, so compare `--boot-map` runs with each other only (`meta.json` has `"boot_map": true`).
- **MangoHud**: `--mangohud` runs the game with MangoHud's logging on and puts its average fps beside the game's,
  over the same stretch.
- **One game at a time**: a run waits while another copy of the game or Ninja runs (`run_game.py --quiet`). Leave the
  PC alone while it runs: other programs change the numbers.

## Reading the summary

- **Avg fps**: frames divided by time over the measured stretch.
- **1% low**: the mean of the slowest 1% of frames, as fps. The game's own segment line (`segment: 'measure'`) gives
  the 99th percentile instead, which reads a little higher.
- **p99**: the 99th percentile frame time.
- **CPU / GPU / Stall %**: each frame's length is put down to what the render thread waited for
  (`tools/bench.py` `bound_of`): GPU when the fence wait is half the frame or more, or when the GPU was busy for 80% of
  the frame or more (the GPU time per frame of the frame's 60-frame window, the runtime's own rule), stall when
  pipeline builds, shader translation and texture uploads are a quarter or more, CPU otherwise (the game thread and the
  replay of its draws). The GPU time is needed with `NX2SARIA_REPLAY_ASYNC=1`: the replay threads wait for the GPU
  then, not the render thread, so the fence wait alone marked every frame CPU-bound.
- **GPU ms**: the GPU's busy time per frame from its own timestamps, averaged over 60 frames. A GPU-bound frame has
  GPU ms close to its frame time; a CPU-bound one well under it.
- **Replay ms**: the CPU time of replaying the game's draws as Vulkan commands, per frame.
- **Win bound**: the runtime's own verdict on its 60-frame windows (`perf: frame N: ... bound: X`,
  `runtime/perf_bound.h`), the one most windows had. `pace`: pacing took 15% of the window or more; `gpu`: the GPU
  was busy 80% of the frame or more; `serial`: the GPU and the CPU took turns, the GPU idle waiting for the next
  submission for a fifth of the frame or more while threads waited for GPU fences for a quarter of it or more
  (the frame is near the sum of the two, so a cheaper GPU frame and a cheaper replay both shorten it); `cpu`
  otherwise. Before 0.20 there was no `serial`, and such windows read `cpu`.

The numbers come from `NX2SARIA_TRACE=perf` with `NX2SARIA_PERF_SLOW_MS=1`, which logs a line per frame
(`runtime/perf.c` `perf_present`). `tests/bench_check.py` checks the parser against the runtime's format strings.

## Tracing a stall in the log

A stall of a second or more, a freeze or a lost GPU is matched against the system's journal (`journalctl`, `dmesg`) by
time. The log's own lines have no time: the only dated one is the focus line when the window gains or loses it
(`gfx: window gained the focus at 2026-11-21 14:40:29 local time`). So the lines that mark an event carry the local time of
day, `hh:mm:ss` in the system's zone, which is the journal's clock (the date is the focus line's; `runtime/log_stall.c`):

- `frametime:   worst frame 7379: 3668.4 ms at 14:46:31; ...`: the time the frame was presented, which ends the stall. Each
  60 s window lists its five worst frames.
- `fsr3fg: presenter acquire failed (2) at 14:46:31` (`ngxfg:` with DLSS frame generation): a swapchain image the
  presenter could not get; the result is Vulkan's (2 is `VK_TIMEOUT`, after a wait of 1 s).
  `fsr3fg: watchdog: no frame for 1.5 s, at 14:46:33; ...`: the presenter had no frame to show for 1.5 s. The first 5
  failed acquires and the first 3 watchdog firings of a run have a line; the next paragraph has the count of all.
- `vk_pipeline: <file> saved: ...; slow: the driver's data was copied in 12 ms, the save ended at 14:46:31`: the end of
  a pipeline cache save whose copy of the driver's data or write of the file took 1 s or more. A faster save has no such part.
- `vk: device lost at 14:59:55 local time (seen at: ...)`, written when the first thread saw the loss, before the report
  of what the GPU was doing; and `fatal: at 14:59:55 local time`, after the `FATAL:` line of a fatal error, of a lost
  device and of a crash report. The `FATAL:` line itself is as it was: the launcher reads it.

Two lines end each frame-time window: every 60 s, at the end of the run, at a fatal error, and when the window loses the
focus in play.

```
frametime:   events: presenter acquire failed 2 (7 in all, only the first 5 logged), MapPhysicalMemory failed 12 (132 in all, only the first logged)
frametime:   system at 14:46:31: PSI memory avg10 some 3.2% full 1.1% (stalled in the last 60 s: some 52 ms, full 0 ms); PSI io avg10 some 12.0% full 8.5% (stalled in the last 60 s: some 1840 ms, full 1210 ms); MemAvailable 20345 of 31932 MB; SwapFree 1802 of 2048 MB
```

- **events** counts what the log's own lines would hide, each with its count in the window and in the run, and says
  how many of them have a line. It is written when the window had one: the presenter's failed acquires and watchdog
  firings (the lines stop after 5 and 3), and the refusals of `MapPhysicalMemory` and `UnmapPhysicalMemory` (a session had
  132, with one line each; now the first of each kind has its line, and so has a refusal with another result than the
  last one logged). At the end of the run and at a fatal error the line reads `frametime:   events in the whole run: ...`,
  with the totals of the run, also when the run ended before its first frame.
- **system** is how the machine fared, to tell a stall in the game from one the machine put on it. On Linux it reads
  the kernel's pressure stall information, `/proc/pressure/memory` and `/proc/pressure/io` (kernel 4.20 or newer
  with PSI on; `PSI not available` otherwise): `some` is the share of the last 10 s in which at least one task waited on
  the resource, `full` the share in which every task did (`avg10`), and the `stalled` times are how much waiting the
  kernel counted over the time since the last line, so a stall of 2 s inside the minute shows even when the 10 s
  average has long gone back to 0. `MemAvailable` and `SwapFree` are `/proc/meminfo`'s. On Windows, which has no such
  counters, the line gives the physical memory available and the system's commit against its limit. The first line of a
  run counts from the first frame. It costs a few reads of small files a minute and holds no lock the game takes.

To line a stall up with the machine: take the time of the worst frame (or of the watchdog), and ask the journal for the
seconds around it, `journalctl --since 14:46:25 --until 14:46:40`, and the kernel's, `journalctl -k`; the `system`
line of the same minute says whether memory or the disk was waited on.

## Background builds in play

Pipelines built ahead of their first use rest while the frame shown has `bg_build_play_draws` draws or more, 600 by
default, and go on in light frames: the title, menus, the pause and map screens, an area load, a frame not shown for
half a second (`runtime/vk_play_gate.h`, `runtime/vk_pipeline.c` `play_rest`). Those are the pool's background queue
(the boot's records left to the background, and the optimised build that replaces a fast link), when stage libraries
are on, and the library warm. A pipeline a draw waits for is always built; the stage library prebuild and the large
compute builds do not rest. The start logs `vk: background builds rest in play: while the frame shown has
600 draws or more ...`, and each save in play and the one at the end `vk: background builds paused in play: N
frames, M builds deferred (R rests, S s of builder time at rest; ...)` when those numbers moved.

To measure what it changes, run the same scenes with the gate and without it, in play mode (not `--shot`, which builds
on the render thread). An ordinary session with warm caches has little background work, so give both runs the same
amount: `lib_warm = force` warms the driver's cache of stage libraries at every start, beside the game, from the same
list each time.

```sh
.venv/bin/python tools/bench.py run ~/Games/Ocarina --save <save backup> --label gate \
    --set NX2SARIA_LIB_WARM=force
.venv/bin/python tools/bench.py run ~/Games/Ocarina --save <save backup> --label nogate \
    --set NX2SARIA_LIB_WARM=force --set NX2SARIA_BG_BUILD_PLAY_DRAWS=0
```

Compare the two `summary.md`: avg fps, 1% low and p99 should be better with the gate, and the stall % no worse (a
draw's own builds are not gated). GPU ms may rise a little with the gate, as a fast-linked pipeline stays in use longer
before its optimised build replaces it. Read the runs' logs too: in the gate run the `background builds paused in play`
line must show frames and builds deferred, or the scenes had no background work and the comparison says nothing; in the
`nogate` run the start line says `off`. The library warm's own line says how far it got in each run: with the gate it
gets less far in heavy scenes, which is the point. The same two runs without `lib_warm = force` show what an ordinary
session gains.

Measured on the Linux bench (RX 6700 with RADV, an 8-core 16-thread CPU) on 2026-11-17: `kokiri` at 1280x720 without
DLAA or AA, `lib_warm = force`, and `MESA_SHADER_CACHE_DIR` pointed at an empty folder for each run so that the warm
compiles for real. The sides alternated, two runs each. A forced warm goes on from where the previous run stopped
(`shader-warm.txt`), so each run warms other variants of the same list.

| | avg fps | 1% low fps | p99 ms | CPU-bound % | replay ms | paused frames / builds deferred | variants warmed |
|---|---|---|---|---|---|---|---|
| gate | 28.1, 26.7 | 15.8, 14.1 | 59.1, 59.2 | 15.7, 10.2 | 11.9, 12.2 | 1210 / 77, 1182 / 80 | 2470, 3642 |
| nogate | 24.7, 24.8 | 11.9, 15.4 | 69.9, 63.6 | 44.2, 40.3 | 14.9, 14.9 | off | 9039, 8951 |

GPU ms stayed between 22.7 and 24.4 in all four runs, and none stalled. With the game folder's own settings and warm
caches the scene is bound by the GPU and both sides read about 23.5 fps: there the gate changes nothing.

The 600 draws come from traced runs of the same build (`NX2SARIA_TRACE=gfx`). The title, the menus and the area loads
draw 243 or fewer per frame, and 97% of the frames of play in `kokiri` draw 600 or more (5th percentile 640, median 952).
