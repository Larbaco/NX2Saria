# AMD FSR 3.1 frame generation: how many frames get one

This page records how many of the game's frames get a generated frame with `framegen = fsr3`, why the logs read as if
few did, what was fixed, and how to check a run from its log. Code: `runtime/vk_fg.cpp` and `runtime/vk_fg4.cpp`
(AMD's libraries, Windows), `runtime/vk_fsr3fg.c` (the runtime's own passes, every other build), `runtime/vk_dlaa.c`
(the depth, motion vectors and picture without the HUD both take), `runtime/vk_fg_stats.h` (the counting) and
`runtime/vk_fg_time.c` (the GPU time on Windows).

**[M]** marks what was measured, in a log of a real session or on the Linux bench. **[R]** marks what was read in the
code alone. AMD's libraries run on Windows only, so what they do in a run is known from the owner's Windows logs, never
from the bench.

## What the logs said, and what was behind it

Since 0.13.0, `docs/known-issues.md` said that FSR 3.1 frame generation on Windows adds few frames because most frames
lack depth and motion. It was written from logs whose line counted every frame since the start of the run:

| Log | Line at the end | Read by period |
|---|---|---|
| 0.12.1, RX 6700, no upscaler, 21:9 3440x1440 **[M]** | 21600 frames, 13719 generated (63%) | 4585 of the first 4800 without depth and motion (logo, opening movie, title, file menu); then 600 of every 600 generated, the swapchain presenting two frames for each |
| 0.12.1, RX 6700, FSR 3.1 upscaling, then frame generation alone **[M]** | 4200 frames, 157 generated | the settings menu's frames counted as "without depth and motion"; once the upscaler was off, 1270 frames "without the HUD-less picture" (below) |
| 0.18.3, RTX 3050 Laptop, FSR 3.1 Quality, 2560x1440 picture **[M]** | first 3000 frames, 0 generated | all before `movie: video decoder finalised` and `dlaa: first scene armed` (frame 3560); then 600 of every 600 generated |
| 0.18.3, the same, `fps_limit = unlimited` **[M]** | 4200 frames, 0 generated | all before the first scene; the next 600: 538 generated, 61 without inputs (the area's load) |
| 0.13.0, Linux, the runtime's passes **[M]** | 26907 recorded, 26889 generated | its 3259 frames without inputs all before the first scene |

So in gameplay AMD's library got the inputs and generated every frame in each configuration these logs cover. The
title, the opening movie and the file menu, 3000 to 4600 frames at the start of a session, made the total look low,
and the settings menu's frames were given the wrong reason.

## What was wrong in the code

1. **The deferred frame shape could be set for a whole run** (`vk_dlaa.c` `vk_dlaa_pass`) **[M][R]**. Ocarina draws a
   half-size effect into three colour targets with a half-size D32F depth. Such a pass is taken for a G-buffer, and
   it sets the deferred shape for the rest of the run, unless a larger light target is armed in the same frame. In
   both 0.12.1 Windows logs, `dlaa: a G-buffer pass (3 colour targets, depth 720x405)` comes before
   `dlaa: first scene armed: light target 1440x810`. From then on:
   - a frame without that effect is never armed;
   - frame generation without an upscaler gets no picture without the HUD wherever the picture is larger than the
     scene. AMD's library then shows the frame as it is: 1270 of 4200 frames;
   - in a picture larger than 1920x1080 the deferred path waits for a scaling pass of that fixed size and never
     evaluates.

   Now a pass smaller than a light target armed in an earlier frame is not a G-buffer. A light target larger than the
   pass that set the deferred shape takes it back. Each case logs one line a run:
   - `dlaa: a 3-target pass with a WxH depth is not a G-buffer: a WxH light target was armed before (Ocarina's frame shape stays)`;
   - `dlaa: the WxH pass taken for a G-buffer is smaller than the scene's WxH light target: Ocarina's frame shape again`.

   `NX2SARIA_DLAA_TEST_DEFERRED=1` starts a run in the deferred shape, as if a 320x180 G-buffer had been seen, and the
   light target takes it back. With `=2` it stays for the run, as before the fix, which shows on the bench what the
   shape cost.
2. **The menu was tested after the inputs** (`vk_fg.cpp` and `vk_fg4.cpp` `vk_fg_frame`) **[R]**. The settings menu
   stops the game's frames, so its frames counted as frames without depth and motion. The menu now comes first, and
   the inputs are not taken while it is open (the first frame after it starts a new history, as before).
3. **The line said nothing about the state of the game** **[R]**. It is now one line for every 600 gameplay frames
   (below).
4. **`dlaa: frame generation does not run: no inputs are made for it` gave no reason** **[R]**. It now names it:
   - the setting is off;
   - NVIDIA DLSS Frame Generation is set and this GPU has none (RTX 40 or newer). The runtime knows this from
     Streamline's support check (`eErrorNoSupportedAdapterFound` or `eErrorAdapterNotSupported`, as on an RTX 30) or
     from a card of another vendor, and the line suggests `framegen = fsr3` where AMD FSR 3.1's runs
     (`vk_dlssg.c` `vk_dlssg_why`, through `nx2sl_support` in `nx2navi_dlssg.dll`);
   - NVIDIA DLSS Frame Generation is set and Streamline gives another reason, which the line names;
   - NVIDIA DLSS Frame Generation is set and has not started. Its own `dlssg:` and `ngxfg:` lines say why: its files,
     the device, or NGX on Linux. An `nx2navi_dlssg.dll` older than `nx2sl_support` gives this line too;
   - NVIDIA DLSS Frame Generation is set in the menu while the game runs. It starts only at the next start (on Linux,
     with DLSS upscaling on when the game starts), so neither Streamline nor NGX has lines of their own yet. Where the
     card cannot run it even then, another vendor's or an NVIDIA GPU older than the RTX 40 series (PCI device ID below
     0x2600, `vk_dlssg.c` `vk_dlssg_gpu_may`), the line says so and suggests `framegen = fsr3`. In the 0.18.7 log
     of an RTX 3050 Laptop, a switch from `fsr3` to `dlss` in the menu wrote the line for a DLSS Frame Generation that
     had not started, which sent the reader to `dlssg:` lines that did not exist;
   - AMD FSR 3.1 is set and does not run here;
   - AMD FSR 3.1 is set and its swapchain is not made yet.

Not changed: a new game's opening flight over the forest is seen as about 850 camera cuts in a row (0.18.3 log,
`resets: ... 844 camera`). Each one starts a new history, so those frames are counted as history starts, not as
generated frames.

## The log lines

At the start, once the context exists, on Windows:

```
fg: frame generation context for 1920x1080, render size up to 2560x1440
fg: GPU time of the generated frames measured with timestamps: their preparation and their generation
fg: path: AMD's FSR 3.1 library (amd_fidelityfx_vk.dll), 2x (one generated frame a rendered frame), inputs from the upscaler; a line every 600 gameplay frames
dlaa: frame generation runs with its inputs from the upscaler (FSR)
```

The FSR SDK 2.4 path writes the same lines with `fg4:`. The runtime's passes write their own `fsr3fg: on: ...` line.

During play, every 600 gameplay frames (or 3600 frames of any kind, when there is little gameplay):

```
fg: 600 gameplay frames: 600 generated (100.0%), 0 history starts, 0 without depth and motion, 0 without the picture without the HUD, 0 larger than the context; besides them 0 with the menu open, 0 without a scene (title, movie, load); 1200 presents in 25.6 s (46.9 a second on screen, 23.4 rendered); GPU 2.10 ms a generated frame (580 timed); the run: 4200 gameplay frames, 4190 generated (99.8%), 120 menu, 3560 without a scene; 4800 frames generated by the swapchain
```

- **gameplay frames**: the game drew a scene. Each is generated, or the line says why not:
  - a history start: a camera cut, a new size, or the first frame after a pause;
  - no depth and motion vectors;
  - no picture without the HUD;
  - a scene larger than the context.
- **with the menu open** and **without a scene** (the logo, a movie, a load, the title, the file menu) are counted
  apart from gameplay.
- **presents in N s**: the frames the screen got, generated ones included. Over the period's time, it gives the frame
  rate the performance overlay shows with frame generation (`gfx_present.c` `output_rate` divides the same count by
  its time). `rendered` is the game's own frame rate.
- **GPU ms a generated frame**: the preparation (in the frame's command buffer) plus the generation (in the command
  list AMD's swapchain gives its callback), from timestamps (`vk_fg_time.c`). Where the present queue's family has
  no timestamps, the start line says only the preparation is timed. `NX2SARIA_FG_GPU_TIME=0` turns the timing off.
- The first 60 lines are all written, then one period in 10. Each line carries the run's totals.

The runtime's passes (every build but Windows) write the same line with `fsr3fg:`, beside the presenter's own
`fsr3fg: presenter: ...` and `fsr3fg: N frames recorded, ...` lines.

## On the Linux bench

Runs of `tools/run_game.py` with the runtime's passes: Hyrule Field from a save, the camera panning and Link running,
120 s (90 s for the last two rows), 2560x1080 picture (`game_aspect = 21:9`, `game_res = 1080`), `fps_limit = 165`.
This build has no frame rate patch table, so the game draws about 25 scenes a second, and the presents that repeat a
picture are counted as frames without a scene. **[M]**

| Run | Gameplay frames | Generated | History starts | Without depth and motion |
|---|---|---|---|---|
| FSR 3.1 Quality and frame generation | 1658 | 1620 (97.7%) | 38 | 0 |
| Frame generation without an upscaler | 1653 | 1615 (97.7%) | 38 | 0 |
| The same, `NX2SARIA_DLAA_TEST_DEFERRED=1` | 1663 | 1625 (97.7%) | 38 | 0 |
| The same, `NX2SARIA_DLAA_TEST_DEFERRED=2` | 1661 | 1615 (97.2%) | 46 | 0 |
| FSR 3.1 Quality, `NX2SARIA_DLAA_TEST_DEFERRED=1` | 1055 | 1017 (96.4%) | 38 | 0 |
| FSR 3.1 Quality, `NX2SARIA_DLAA_TEST_DEFERRED=2` | 1049 | 0 | 0 | 1049 |

The last row is the deferred shape kept for the run, as before the fix: FSR 3.1 upscaling never finds its output pass
(the log has no `dlaa: FSR 3.1 at ...` line), so no frame gets depth and motion and none is generated. With `=1` the
light target takes the shape back on the first scene and the run is like the others. Without an upscaler, Hyrule Field
draws the half-size effect every frame, so the trap costs nothing there; the 0.12.1 Windows log above lost 1270 of 4200 frames to it.
The runtime's passes took 0.96 to 1.14 ms of GPU a generated frame. The frame generation line written before the fix
counted the same frames as these runs' (281 recorded, 271 generated by frame 2400, against 264 and 254), so nothing
changed on this path but the counting.

## On Windows, after the fix

0.18.6 on an RTX 3050 6GB Laptop GPU (driver 546.18), `framegen = fsr3` with FSR 3.1 Quality (scene 1707x960, picture
2560x1440, frame generation context 1920x1080), `fps_limit = unlimited`, two sessions **[M]**:

- 39 periods of 600 gameplay frames, each with 596 to 600 generated: 23379 of 23400 in all (99.9%). No frame lacked
  depth and motion or the picture without the HUD.
- `a second on screen` was twice `rendered` in each line and matched the frame times.
- `dlaa: a 3-target pass with a 853x480 depth is not a G-buffer ... (Ocarina's frame shape stays)`: the case that
  set the deferred shape before, now refused, and FSR 3.1 found its output pass.
- The settings menu's frames were counted as menu frames (94 in the second session).
- AMD's library took 4.1 to 5.3 ms of GPU a generated frame (its preparation and its generation), against 0.96 to
  1.14 ms for the runtime's passes on the bench's GPU.

0.18.7 on the same PC, frame generation without an upscaler (`dlaa = 0`) with the scene smaller than the picture
(`game_res = 1080`: scene 1440x810, picture 1920x1080, context 1920x1080), steps 4 and 5 below **[M]**:

- 11 periods of 600 gameplay frames, each with 591 to 600 generated: 6583 of 6600 in all (99.7%). No frame lacked
  the picture without the HUD, the case the deferred shape broke.
- `dlaa: a 3-target pass with a 720x405 depth is not a G-buffer: a 1440x810 light target was armed before (Ocarina's
  frame shape stays)`.
- 3.2 to 3.7 ms of GPU a generated frame (one period 4.3).

The 0.18.3 logs of the same PC confirm what the old line showed: its 3562 frames before the first scene (logo,
movie, title) were counted as frames without depth and motion.

## Checking on Windows

The bench cannot run AMD's libraries. This is the check for a Windows PC, for example an RTX 3050 Laptop, where
NVIDIA's frame generation does not run and AMD's is the one to use. Each step is one game session.

1. Close the game. In `nx2saria.ini` (next to `nx2saria.exe`), set:

   ```
   framegen = fsr3
   dlaa = 1
   taa = fsr3
   dlss = quality
   upscaler = bilinear
   ```

   This is FSR 3.1 upscaling at Quality: the scene is 67% of the picture, smaller than it.
2. Start the game and load a save. Play for 2 to 3 minutes in one area with the camera moving, for example running
   across Hyrule Field or Kokiri Forest. Open the settings menu once for a few seconds, then close the game.
3. Copy `logs\nx2saria.log` aside (`nx2saria-fsr-quality.log`).
4. Set `dlaa = 0` (frame generation without an upscaler) and repeat steps 2 and 3 (`nx2saria-fg-only.log`).
5. Set `game_res = 1080` with a 1440p window or screen (`window_size = 2560x1440`), with `dlaa = 0`: the scene is
   smaller than the picture with no upscaler. Repeat steps 2 and 3 (`nx2saria-fg-only-small.log`). This is the case
   the deferred shape broke.
6. Optional: `framegen = dlss` on an RTX 30. The log should say, once, that DLSS Frame Generation does not run here
   and that `framegen = fsr3` does.

What to look for in each log, and what to send back:

- `fg: path: AMD's FSR 3.1 library ...`: the path and where the inputs come from.
- Every `fg: 600 gameplay frames: ...` line. In play, `generated` should be close to 600, presents close to 1200, and
  `a second on screen` close to the overlay's frame rate.
- Any `dlaa: a ... pass with a ... depth is not a G-buffer` or `dlaa: ... Ocarina's frame shape again` line, and any
  `dlaa: frame generation without an upscaler: ... no picture without the HUD`.
- `fg: GPU time of the generated frames ...` and the `GPU ... ms a generated frame` part of the lines.
- Any `fg:` line with `failed`.
- The GPU and driver: the `vk caps:` line at the top.

Send the three logs (four with step 6), and a sentence on how the motion looked: smooth, or with stutter or smearing
at the screen's edges.
