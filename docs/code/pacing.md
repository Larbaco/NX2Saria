# runtime/pacing

The frame limit and the game's step, the other limiters that can hold the frame limit in the pacer's place (RivaTuner
Statistics Server, the driver's low latency, Reflex with DLSS Frame Generation), the present that waits for held work,
and dynamic resolution. What these are for and how they fit together: [architecture.md](../architecture.md) (the
bullets "Frame pacing", "One limiter is the goal", "RivaTuner Statistics Server" and "Low latency" under the host),
[frame-generation.md](../frame-generation.md), and [presets.md](../presets.md#dynamic-resolution) for dynamic
resolution. The log lines: `frame rate:`, `frame step:`, `fps:`, `ocarina:`, `lowlat:` and `resolution:` in
[logging.md](../logging.md). The checks that drive this code: [building.md](../building.md#checks).

## `runtime/pacing/frame_rate.c`

The frame limit and the real-time frame step: the host half of the patch sets `fps60`, `realtime` and `cloth` (build
tables `recomp/patches-v121.txt`, `recomp/patches-v143.txt`), and of Ocarina's sets in `recomp/patches-v111.txt`
(`ostep`, `ofollow`, `osolver` and `osolverfull`, `osound`, `orecord` and `ostaff`, `omovie`, `owater`). Its first
line is the licence notice of the code it contains, and stays.

The game's adaptive present-interval controller (main+0x8f26c8) turns the main thread's measured work into a number of
60 Hz ticks clamped to [minimum, maximum]; shipping data sets the minimum to 2 (30 fps), the set `fps60` lowers it to
1. The controller still raises the interval when a frame does not fit. Shipping code then steps the game by whole
vsyncs (VFR step main+0x7ede6c: ratio = interval * 0.5, seconds = interval / 60; global ratio at main+0x8f2394), so
game speed is right only when frames take exactly their interval. With the set `realtime` both read the patch page
instead: +0 ratio = seconds * 30, +4 seconds (1.4.3: +8 interval = seconds * 60, the producer's nominal interval
before its half-step coefficient), the moving average of the last 5 display intervals, at most 1/15 s (slower frames
slow the game down, as on the console) and at least 1/240 s, so a higher frame-rate cap only changes the pacer
(`frame_step_cap_ns`, `NX2SARIA_FPS_LIMIT`). The set `cloth` gives hair and cloth the same clock and needs `realtime`.

The floor of 1/240 s: with a shorter step (ratio below 0.125) 1.4.3 aborts within seconds of some save loads (issue
#241, a 280 Hz monitor): Havok's `hkDefaultMemoryImpl` reports "Out of memory" for the buffer the physics pre-solve
(profiler scope "TtPreSolve") resizes in main+0x1261630. Forced steps on one save: 1/240 s plays, 1/244 s and shorter
abort; the field, sky island and fight saves play at 1/280 s, the town save aborts after about 800 frames. The same
short steps also crash a physics worker reading that buffer's contacts: an access violation in main+0x1292560 or its
caller main+0x1286870 (narrowphase contact setup, reached from the grid task main+0x12f1ea0), reading a Havok body's
shape at body+0x78 as null or garbage (issues #29 and #92, 240 Hz monitors with `fps_limit = display`). Steps drawn
from 1/252..1/230 s crash that save's load in seconds; 1/240..1/230 s plays. The frame limit stops at 240 too
(`LIMIT_MAX_HZ`), so the floor only catches the average dipping under 1/240 s at that limit: a present late by less
than a period keeps the pacer's phase (`runtime/nvn/nvn_api_obj.c` `pace()`), so the next display interval is shorter
than the period. No limit makes the game run slower than the clock.

The sample is the display time the pacer gives each present (`nvnQueuePresentTexture` in
`runtime/nvn/nvn_api_obj.c`), so the step follows the frames as shown, pacing waits included. With the step on, the
pacer ignores the game's interval and holds a frame only to the frame limit (`runtime/nvn/nvn_api_obj.c`
`present_period()`): the controller's 2 or 3 vsyncs would only add latency. The vsync event stays at 60 Hz whatever the
limit: the game does not wait for it each frame (1.4.3 shows its title at 144 fps with the event at 60 Hz). One 64-bit
and one 32-bit store per present; the guest reads the floats on its own threads, and each stays finite and in range
whichever store it sees.

The world clock (`frame_step_world_us`) sums the steps from the first present that finds the player actor away from the
origin, where the title screen keeps it (1.1.1 has no player actor until the save has loaded): the end of the first
save load. Recorded and scripted input (`runtime/input/input.c`) runs on it, so a replay presses at the same game time
at any frame rate and after a first load of any length; a later load (fast travel) still shifts what follows by its
length.

`NX2SARIA_TRACE=perf` adds a line per 120 presents (`speed_report()`): fps, the game seconds stepped against wall
seconds, and with `NX2SARIA_SPEED_PROBE=1` the player's position and ground speed (an independent check: Link runs at
the same speed at any frame rate when game time follows wall time).

Constants:

- `STEP_SAMPLES` 5: the display intervals the real-time step averages.
- `STEP_MAX_S` 1/15 s: the longest step; a longer frame slows the game down.
- `STEP_RESET_S` 0.25 s: a gap this long (loading, a debugger) restarts the average.
- `STEP_FIRST_S` 1/60 s: the step until the first interval.
- `STEP_MIN_S` 1/240 s: the shortest step; a shorter one makes the game's physics run out of memory (above).
- `LIMIT_MIN_HZ` 30 and `LIMIT_MAX_HZ` 240 (1 / `STEP_MIN_S`): the frame limit's range.
- `LIMIT_FREE_MAX_HZ` 240: the most game frames a second in a build that steps by the clock (`frame_free_running()`).
- `OCARINA_QUIET_NS` 2 s, `OCARINA_CLOCK_WORD` 264, `OCARINA_STAFF_WORD` 776: the ocarina log (`ocarina_watch()`).
- `OSTEP_WINDOW` 12, `OSTEP_SNAP` 0.03, `OSTEP_REPAY_MAX` 0.10, `OSTEP_MIN_S` 1/480 s: the steady step
  (`ocarina_step()`).
- `WHOLE_SLOTS` 8, `WATER_SLOTS` 64, `SIM_SLOTS` 64: the remainder tables (`whole_take()`).
- `FLOOR_PROBE_RADIUS` 12 m, `FLOOR_PROBE_NEAR` 6, `FLOOR_PROBE_GROUPS` 16384: the floor probe (`floor_probe()`).
- `FLOOR_SCAN_ARM` 30, `FLOOR_SCAN_MAX` 2000000 (the candidate triplets a snapshot keeps), `FLOOR_SCAN_SHOW` 64 (the
  survivors logged each second), `FLOOR_SCAN_BASES` 4, `FLOOR_SCAN_HOLDERS` 24: the floor scan (`floor_scan()`).

State:

- `step_word`: patch page +0, the ratio (low float) and the seconds (high float). `step_now`: the step the patch page
  holds, what the game steps each frame. `step_interval`: patch page +8, seconds * 60 (`recomp/patches-v143.txt`
  `realtime`).
- `ostep_page`: patch page +256 with the set `ostep`, frames per update (`recomp/patches-v111.txt`). `carry_page`: patch
  page +260 with the set `ofollow`, 100 / frames per update.
- `game_interval`: the present interval of the newest frame (`frame_step_present()`).
- `limit_hz`: 0 display; -2 unlimited (free-running builds); -1 before the first read.
- `paced_hz`: the game frames a second the newest `frame_step_cap_ns()` paces at (`ocarina_step()`); 0 none.
  `movie_paced`: the movie rate the newest `frame_step_cap_ns()` paces at (`ocarina_step()`, `frame_step_movie_hz()`); 0
  none.
- `win_hist` (the speed lines): frames by present interval 1, 2, 3 and more; `[0]` is unused.
- `guest_presents`: `frame_step_present()` calls so far (`frame_step_flight()`, `frame_step_presents()`).
- `game_us`: game time stepped since the first present (`frame_step_ticks()`).
- `os_step_lo`, `os_step_hi`: the steps given since the last speed line (`speed_report()`).
- `WholeSlot.used`: `whole_calls` at the slot's last use.
- `floor_probe_left`, `floor_scan_left`: lines still to write; -1 before the setting was read. `floor_last`: the ground
  groups' translations at the last update (x, y, z each). `floor_watching`: `runtime/diag/floor_watch.c` has the
  scan's survivors and is not done (`NX2SARIA_FLOOR_WATCH`).
- `fs` (the floor scan): `state` 0 waiting for a drift, 1 filtering, 2 logging; `val` the triplets at the snapshot,
  then at the last line.

### `floor_carry_store()`

The set `ofollow` (`recomp/patches-v111.txt`): the three follow-surface modules of the character controller keep only a
tenth of the supporting floor's velocity when the physics' inverse step is over the number at page +260. The game
compares with 100, which the inverse step passed at every update above 100 fps; under one frame per update, 100 /
frames per update makes the cut come where it came at 60 fps. At one frame or more it stays 100, so 60 and 30 fps keep
the game's own cut. Written with frames per update (`ocarina_step_write()`).

### `ocarina_log_init()`, `ocarina_end()`, `ocarina_watch()`

The ocarina's log: what the sets `orecord` and `ostaff` of `recomp/patches-v111.txt` do, read from the patch page words
their sites write, so a player's log shows them working. The recorder clock (`orecord`) stores the fraction it carries
at +264 on each update of the ocarina manager's note clock (playing, recording, a song played back); a played-back
song's staff (`ostaff`) stores each update's note signal (0 or 1) at +776 and moves the staff one note at an update
whose signal is set where the one before was clear.

Every present reads both words: a stretch of ocarina use starts at the first present either changed on and ends
`OCARINA_QUIET_NS` (2 s) after the last change, with one line: its length, presents and frame rate, the presents the
clock's fraction changed on (above 60 fps nearly every one; at 60 fps and below the fraction stays 0 and nothing
shows), and the notes the staff moved, as rises of the signal from one present to the next (a song played back moves
its staff once a note: Zelda's Lullaby 6, the Song of Time 6, Bolero of Fire 8). Nothing else writes either word, so
no line comes while the ocarina is put away. `ocarina_log_init()` returns without a line on another game version (no
such sets). `tests/ocarina_log_check.py` builds these three, cut out of the file from `OCARINA_QUIET_NS` to the end of
`ocarina_watch()`.

### `frame_step_init()`

With the set `ostep` (Ocarina) frames per update come from the page, one 60 Hz frame until the first present.

`physics_steps` (`recomp/patches-v111.txt` `osolver` and `osolverfull`): 0 the game's solver step count,
trunc(frames per update * 4 + 0.001); 1 (the default) that count rounded up; 2 rounded up and at least the 4 of a 60 Hz
frame, every update. The two sets patch the same words; this start keeps one of them on.

`frame_limit_live()` runs here so that `NX2SARIA_FPS_LIMIT` decides the set `fps60` from the start.

### `limit_parse()`, `frame_limit_live()`

The frame limit (menu, `NX2SARIA_FPS_LIMIT`, live): 30, 60, 90, 120, 144, any whole rate from 30 to 240, or display
(the refresh rate of the window's monitor, at most 240). A higher number gives 240, the rate of the shortest step the
game survives (`STEP_MIN_S`). Unset, the older `NX2SARIA_FPS60=0` still means 30. A limit of 30 switches the set
`fps60` off (the shipping minimum interval 2), every other limit switches it on; the controller reads its minimum
interval from the patched sites each frame. With the real-time step the pacer holds frames to the limit; above 60 only
the pacer changes, as the step's floor is the limit's ceiling. Without it the game steps whole 60 Hz vsyncs and the
pacer follows the game's interval, so the limit stops at 60.

### `shown_hz()`

The rate "display" paces at: 0 is a monitor rate not known yet (no window), taken as 60; a real one below the lowest
limit (a 24 or 25 Hz TV) is held at that limit rather than read as unknown and doubled to 60.

### `frame_free_running()`

A build without the `fps60` and `realtime` patch sets (Ocarina) steps its game by the clock itself: walked by the same
frame-numbered input script, Link covered half the distance at 120 fps that he covers at 60. There the pacer holds
frames to the limit alone (`runtime/nvn/nvn_api_obj.c` `present_period()`), the game's own 60 Hz interval aside, and
the limit may be any rate up to `LIMIT_FREE_MAX_HZ`, or "unlimited", which paces the game at that rate too.

Ocarina's code does fail past some rate: on the quick menu, where a frame costs the GPU almost nothing, it ran at 640 to
700 fps and after about two seconds read address 0 in its own draw code (main+0x175b2a0, the same call stack twice),
while 92 to 255 fps ran on. 240 keeps every common monitor's rate; with frame generation it counts the game's own
frames, so the screen still shows the generated ones above it.

### `frame_step_ceiling_ns()`

The period the pacer keeps while something else holds the frame limit where the frame starts (Reflex with DLSS Frame
Generation, `runtime/vk/vk_dlssg.c` `vk_dlssg_pace()`): a limiter that lets go, or counts other frames than the game's,
still leaves the game no faster than the rate its code survives.

### `movie_cap_hz()`

`NX2SARIA_MOVIE_FPS` (default 60; 0 off): the game's frame rate while a movie plays (`runtime/hle/ov_movie2.c`
`ov_movie_live()`). A movie's event counts the game's updates as 60 Hz frames whatever the step: at 144 fps the intro's
narration lines started 2.4 times too early, each over the one before, and its closing fade to black came at 65 s of
the movie's 154, hiding the whole dream. The movies are 60 fps, so the cap shows the same pictures. It is the rate in
both directions: under a lower limit (40 fps, or 60 shown with frame generation, which is 30 of the game's) the movie
ran at that fraction of its speed and ended many seconds behind its sound, so the game is paced at the movie's rate
while one plays, above the limit too. A value under `LIMIT_MIN_HZ` gives 60.

With the set `omovie` of `recomp/patches-v111.txt` (`NX2SARIA_MOVIE_PTS`, on by default) the event count follows the
shown picture's timestamp instead of the redraws, so with the cap off the cues no longer run ahead; the cap stays for
the pacing in both directions and for builds without those sites.

### `generated_factor()`

Frames on screen per game frame while frame generation runs: 2 with AMD's (`runtime/vk/vk_fg.cpp` through AMD's
library, `runtime/vk/vk_fsr3fg.c` through the runtime's passes: one generated frame), the multiplier with NVIDIA's
(`runtime/vk/vk_dlssg.c`, one to three); 1 while none generates (off, or paused for the menu).

### `limit_game_hz()`

The limit counts the frames the game draws, and frame generation adds its frames on top: a limit of 60 at 4x is 60
drawn and 240 shown. Counted on the screen instead, the game was paced at the limit over the factor, so each step up of
the multiplier took drawn frames away (60 at 4x drew 15) and the generated ones had less to work from and more delay.
Only "display" is a rate on the screen, the monitor's: there the game draws that rate over the factor, since a frame
past the monitor's refresh is never seen. The result is game frames a second, before the ceiling of the step.

### `frame_step_game_hz()`

The rate the game renders at under the limit (`runtime/input/mouse_direct.c`, whose camera input follows the game's own
frame time, and `runtime/pacing/dynres.c` `target_now_ns()`); 0 unknown or unlimited.

### `frame_step_cap_ns()`

RivaTuner Statistics Server (`runtime/pacing/rtss_limit.c`) gets the same limit; while it limits this process the pacer
holds the frames only to `LIMIT_FREE_MAX_HZ`, the ceiling the game's code needs, so the two do not stack, except at a
limit of 240 or unlimited, where the ceiling is RTSS's own period; and it takes the limit back once no picture went out
for 200 ms (`runtime/pacing/rtss_decide.h` `rtss_pace_period()`): a minimised window or a frame hold leaves RTSS no
present to hold. While a movie plays the pacer holds the movie's rate and keeps its slots whatever RTSS does.
`NX2SARIA_RTSS=0` and frame generation (RTSS would count the generated frames) keep it out, and so does a driver's low
latency while it is in effect (`runtime/pacing/vk_lowlat.c`): the driver holds the limit where the frame starts, and
RTSS would hold the finished picture, the wait that mode is there to remove. RTSS gets the reason (`RTSS_OUT_*`), for
the log and for its check of a limit set in RTSS itself.

- `NX2SARIA_MOVIE_CLOCK=0`: the steady step and the usual pacing through a movie too (`movie_paced` stays 0).
- The movie lines are limited to 40, the first 20 movies.
- Unlimited (`limit_hz` -2) in a build that does not step by the clock: the pacer holds no frames; RTSS gets the reason
  it is kept out, or 0, which clears its limit. In a build that steps by the clock unlimited is `LIMIT_FREE_MAX_HZ`
  game frames; during a movie, the movie's rate.
- RTSS keeps the play limit through a movie and the pacer holds the movie (`runtime/pacing/rtss_decide.h`
  `rtss_target()`): the movie's rate as the want rewrote RTSS's profile at each start and end, and with the pacer aside
  under RTSS no movie slot was kept, so each late frame put the picture behind the sound for good. So the want is the
  play limit (`play_mhz`) and only the pacer's rate becomes the movie's.

### `frame_step_limit_hz()`

The limit as it is set, for the performance overlay: the setting, the monitor's rate for "display", 60 at most without
the real-time step; the game's own rate (60 Hz over its present interval) in a build without the patch sets; 0 before
the first frame read the setting. In a free-running build unlimited also gives 0, as before the first read.

### `read_guest()`

`plat_mem_read` copies through the kernel, which fails on a heap page `NX2SARIA_HEAP_LAZY` has not committed yet:
`mem_kernel_access` first.

### `player_pos()`, `player_name()`

1.1.1: `[[main+0x4e371d0] + 0x480]` is the Player actor, its name pointer at +0x20 reads "Player" and its position, in
metres, is the float triplet at +0x31c. The slot is empty until the first save has loaded and then holds the actor.

The other builds: `[main+0x4728538]` (1.1.0: `[main+0x4630a68]`, 1.4.3: `[main+0x3add238]`) +0x1e8 scene, its
component 13 lists the resident actors; the one named "Player" has its position at +0x2b4. The 1.1.0 slot was measured
from its own dereference chain: 181 scene accesses in that build read it, and one of them reproduces this walk (the
14-component check, component 13, the 0x70 stride and the name compare) instruction for instruction.

### `ocarina_step_s()`

Ocarina 1.1.1's time manager, `**[main+0x4c7e890]`: +0x1c0 the frame's base step in 60 Hz frames, +0x10 the game speed
(main+0x2c84c makes each time category's {frames, 1/frames, seconds, 1/seconds} from them and the category's factor).
Read by the speed lines of a free-running build: the game seconds a frame stepped, or -1.

### `frame_step_presents()`

The game's presents so far, one per update of the game at any frame rate (`runtime/input/input.c` `shake_stick()`
counts its updates).

### `frame_step_flight()`

The flight recorder's view of the step at a host present (`runtime/diag/flight.c`, render thread): the newest step the
patch page holds in 60 Hz frames per update (set `ostep`; -1 without), the game's own step from its time manager in
60 Hz frames (-1 unknown or not free-running), and the game's presents so far, which name the update those two belong
to.

### `ocarina_step()`, `ocarina_step_reset()`

Free-running builds (Ocarina): the game's step follows the display interval, in 60 Hz frames, written at each present
(`ocarina_step_write()`): frames per update on the patch page with the set `ostep`, else the time manager's base step.
The game left it at one 60 Hz frame whatever the frame rate, so at 144 fps it ran 2.4 times too fast.
`NX2SARIA_OCARINA_STEP=0` leaves it.

The step Ocarina is given for a display interval dt, in seconds. The game ships with a step of one 60 Hz frame (two at
30 fps) that never changes in play, so nothing in it was written for a step that differs from frame to frame: its
physics takes the step as the length of the next solve while the bodies' velocities come from the step before (the
physics system keeps both, main+0x38af1f0), and it derives whole numbers from it (the solver step count,
`recomp/patches-v111.txt` `osolver`). The mean of the last 5 intervals, which stood here, moved with every one of them:
display intervals of 5 and 30 ms in turn gave steps of 15 and 20 ms in turn, and one 60 ms frame at 200 fps took the
step from 5 to 16 ms for five frames and back. So the step is kept steady and the clock is kept by a slow correction:

- the base is the mean of the last 12 intervals without their longest and shortest: one late frame and the short one
  after it (`runtime/nvn/nvn_api_obj.c` `pace()` keeps the phase) leave it alone, intervals alternating in a period of
  2, 3 or 4 frames give one value, and a real change of frame rate arrives within 12 frames;
- a base within 3% of the pacer's period is that period: at a frame limit the game holds, the step is the same number
  every frame, at 60 fps exactly the one frame the game ships with;
- what the display showed and the game was not given is owed (the late frame cut from the base, the 3% of the snap).
  Past half a step of it the step repays a 64th a frame, 10% of the step at most, so the game still follows the clock
  and a hitch is paid back over the next second rather than in five frames. More than `STEP_RESET_S` owed is dropped:
  the game runs slower than the clock, as it does for a frame longer than `STEP_MAX_S`.

The step is at least `OSTEP_MIN_S`: the solver step count of the set `osolver` reaches 0 under a 16th of a frame.
`NX2SARIA_STEP_STEADY=0` gives the mean of the last 5 intervals again (comparison runs).

While a movie plays (`frame_step_cap_ns()` paces the game at the movie's rate) the step is exactly one movie frame,
whatever the display intervals; `NX2SARIA_MOVIE_CLOCK=0` leaves movies to the steady step (comparison runs). The game's
movie player (main+0x3b5ca60) adds the time manager's seconds times 60 to an accumulator at each update and advances
the movie by its whole pictures, at most the player's limit per update (+0x20, set at main+0x3b5d094: 1, or 2 in one of
its modes), keeping the rest. The Triforce movie showed one picture per update (600 pictures in 600 updates at 59.6
updates a second), so the time a late frame left owed, which the steady step pays back as steps over one frame, never
became pictures: the picture fell behind its sound, which plays by the clock, by each late frame for good. The movie's
event counts updates as 60 Hz frames too (`frame_step_cap_ns()`). So the updates themselves keep the clock
(`runtime/nvn/nvn_api_obj.c` `pace()` keeps the movie's slots through a late frame) and each counts as one movie
frame, for the picture, the event and the game's clock alike. The filter starts again at a movie's start and end, so
neither rate's intervals reach the other.

`tests/frame_step_check.py` drives this step through `frame_step_cap_ns()` and `frame_step_present()`.

### `whole_take()`, `whole_frames()`, `cut_fcvtzs()`

Whole frames out of the real-time step: the set `ostep`'s sites that cut it to a whole number
(`recomp/patches-v111.txt`). The game adds the step, or the step times a whole number, to a few counters and cuts the
sum to a whole number: at the console's 60 fps the step is 1 and they count frames. With the step the display interval
in 60 Hz frames, under one above 60 fps, the cut loses the fraction every frame. `whole_frames()` keeps it per counter
(by its guest address) and hands out the whole part, so each counter moves at the game's pace at any frame rate; at a
step of exactly one it gives what the game computes. A value that is not positive, or too large for a count, is cut as
`fcvtzs` cuts it (`cut_fcvtzs()`: `fcvtzs w, s`, toward zero, saturating, NaN 0). The water's ripples keep their
remainders per water object in a table of their own (`frame_rate_water_host()`), and so do the fixed steps of the water
and floor surfaces (`frame_rate_sim_host()`).

`whole_take()`: the whole part of amount plus the remainder the table slot[n] keeps for `at`; a new `at` takes the slot
unused longest.

### `frame_rate_day_host()`

The time of day (main+0x3cdf70, `recomp/patches-v111.txt`): s0 holds twice the step, the game seconds of this frame; w8
gets the whole ones, the counter being the seconds of the day at +0x3c of the object in x19. `NX2SARIA_TRACE=perf`:
every 10 s, the game seconds the day was given beside those the cut alone would have given.

### `frame_rate_water_host()`

The water's ripples (main+0x472e0, `recomp/patches-v111.txt`): s0 holds the frame's step and w9 gets the whole steps in
it, which the renderer multiplies by the water's substeps. The remainder is kept for the water object in x21, in a
table of the waters' own: every water surface of a place asks once a frame, and with fewer slots than surfaces each
would take another's slot in turn, start without a remainder every frame and stand still as it did with the cut.
`NX2SARIA_TRACE=perf`: every 10 s, the steps the waters were given beside those the cut alone would have given. The
counts are taken under `whole_lock`: nothing says one thread alone draws the waters.

### `frame_rate_sim_host()`

The fixed steps of the physics' height-field surfaces (`recomp/patches-v111.txt` `owater`): the water managers
(main+0x3875f20, main+0x3882660) and the interactive floors' simulation (main+0x632cb0, the spider webs' sinking floor).
Each update divides its seconds by 1/60 s into s0 and cuts that to the number of steps of exactly 1/60 s it runs (at
most 3), dropping the fraction, so every frame shorter than 1/60 s ran none. The site calls this between the division
and the game's own cut: s0 becomes the whole steps in the quotient plus the remainder kept for the object in x19, as a
float the cut keeps exactly, and only s0 changes (its upper lanes stay zero, as the division left them). A quotient
that is not positive, or too large for a count (outside `whole_take()`'s own range), stays for the game's cut. The
remainders are in a table of their own: an object that shares slots with the counters above would lose its fraction to
them. `NX2SARIA_TRACE=perf`: every 10 s, the steps the objects were given beside those the cut alone would have given.

Every interactive floor of a place (the Deku Tree's ground webs are such floors) and both water managers ask once an
update, each for its own object, so the table holds as many as the waters' does (`SIM_SLOTS` 64): with fewer slots
than objects each would take another's slot in turn, start without a remainder every update and stand still again
above 60 fps.

### `sound_gate_take()`, `frame_rate_sound_gate_host()`, `frame_rate_sound_step_host()`

The sound manager's job at whole steps (`recomp/patches-v111.txt` `osound`). The job (main+0x415300) runs in three
phases a frame, the phase in w2: 0 loads the time manager's step, frames per update at `[[main+0x4c7e890]] +0x10`, and
hands it to the sound system (`[[main+0x4c80c38]] +0xa8`, and with its inverse +0x18 of the object at +0x10 of
`[[main+0x4c806d0]]`); 1 runs the sound objects' updates, 2 the system's own (main+0x3798410). Any other phase returns
at once (main+0x4155fc); the task wrappers main+0x4151f0 and main+0x4152d0 skip a 3 themselves. The sound system counts
its fades, delays and parameter ramps in updates of at least one step. With the set `ostep` the step is the display
interval in 60 Hz frames, a fraction of one above 60 fps. Fed fractions, this game has been reported to keep sounds
held after an area unloaded until no voice was left free, so that new sounds, then the music, went silent until a
restart (not reproduced on this build: the fault takes a few area changes above 60 fps). With the set `osound` the job
runs at 60 Hz while the step is a fraction:

- `frame_rate_sound_gate_host()`, after the job's prologue: phase 0 adds the step to what the job is owed and runs once
  a whole step has built up, keeping the rest; phases 1 and 2 follow that frame's choice. A job that does not run gets
  phase 3 in w2, so it returns through its own epilogue. The phases may come from different threads, so the choice is
  made under `whole_lock`. `NX2SARIA_TRACE=perf`: every 10 s, how often phase 0 ran of the times it was asked.
- `frame_rate_sound_step_host()`, after phase 0's load of the step: a fraction becomes one whole step (1.0f, the lanes
  above it zero as the load left them).

A step of one or more (60 fps and below) runs as the game has it and leaves what is owed for the next fractions; a step
that is not positive (or NaN) runs as the game has it and clears it. So the sound clock keeps the game's on average at
any frame rate, and at 60 fps the job runs every frame as before. `SoundGate.owed`: steps given since the job last ran,
under one; `run`: phase 0's choice, which the frame's other phases follow; every phase runs until a phase 0 has chosen.

### `game_frame_step()`

The time manager's step, 1 before the time manager is there.

### `frame_step_frames()`

The 60 Hz frames the game steps at each update where the set `ostep` gives them (Ocarina); 1 elsewhere, the game's own
nominal frame. The mouse camera divides by it (`runtime/input/mouse_direct.c`): the game multiplies camera input by
this step.

### `frame_rate_horse_jump_host()`

The jump state machine compares s10 (its update step) against s9 (1.0). If the update is shorter than a 60 Hz frame but
covers the time manager's whole step, that step is the threshold. The slow-motion path still sees 1.0, and the normal
path writes s9 again before reading it.

### `frame_rate_gohma_turn_host()`

The steering update's final clamp stores a turn rate. Its upper bound s0 is the magnitude of the turn applied this
update multiplied by the step in s8. This undoes that extra multiplication and converts the turn back into a rate. The
guest has already restored the caller's s10 before computing the bounds.

### `ocarina_step_write()`

With the set `ostep` the game makes its whole nominal frame from the page's value, the interface's included. Without
it, the time manager's base step is written directly. Counts how often the game itself had changed the value since the
last write (`ostep_changed`, diagnosis; a `frame step:` line every 1200 presents).

### `time_dump()`

`NX2SARIA_TIME_DUMP=1`: the time manager's first 0x280 bytes once, 30 s into the run (diagnosis).

### `speed_report()`

Ocarina 1.1.1: the engine's global step at main+0x4d41fc8 (`recomp/patches-v111.txt`, the set `ostep`'s second site),
logged with `NX2SARIA_SPEED_PROBE=1`. For Ocarina, the shortest and longest step given in these frames, and the time
still owed.

### `world_clock()`

Until the load ends, each present costs a few guest reads (the player's position). With whole-vsync steps the display
time stands in for the step.

### `floor_probe()`, `direct_read()`, `direct_ptr()`

`NX2SARIA_FLOOR_PROBE` (`floor_probe` in the ini, 0 by default): Link on a moving floor, one line per update (diagnosis,
1.1.1). Above 60 fps Link stands still on the Deku Tree's floating platform while it moves under him, and the code that
carries him has not been found; these lines give what a run needs to find it.

The moving floors are the engine's ground groups: their manager is `[[[main+0x4c7df00]] +0xc0] +0x130` (the ground
control moves write it, main+0x3000a90, and its transform setter main+0x386a740 takes it so), the group count at +0x28
and the array of 32-byte entries at +0x30, each starting with the group; a group's transform is three rows of four
floats at +0x88, +0x98 and +0xa8 (main+0x386a7e8..0x386a800), its translation in their fourth lanes. Each update the
probe reads every group's translation and Link's position (Player +0x31c, `player_pos()`), and logs the step, Link's
position and how far he moved since the last update, and `FLOOR_PROBE_NEAR` groups within `FLOOR_PROBE_RADIUS` of him
horizontally (those that moved since the last present first, nearest first, then the nearest still ones), each with its
translation and how far it moved; with no group that near, one line in 30 presents still gives Link alone. At 60 fps
the moves of Link and of the group under him match while he stands on it. Each line also gives the time manager's base
step, game speed and first two time categories: the time manager `**[main+0x4c7e890]` the game steps by, +0x1c0 the
base step and +0x10 the game speed in 60 Hz frames, and the first two time categories (+0x20 and +0x50: frames, 1 /
frames, seconds, 1 / seconds, as main+0x2c84c makes them), so a line shows whether what the physics reads matches the
step. The value is the number of lines to write (1 means 4000); off it costs one test per present.

The manager holds a few thousand groups (5345 in the Deku Tree; most entries are empty), and a readability check per
read costs a system call: at one per group a present the probe made every frame about 1.9 s. So the entries and the
groups' rows are read in two batched copies a present (`runtime/memory.c` `mem_copy_readable()`: one lock, no system
call for memory that is neither armed nor probed), and only while there is a player (on the title screen and during a
load the manager is not one yet, and there is nothing to log). A count above `FLOOR_PROBE_GROUPS` is taken for a wrong
manager and skipped. The entries' range is checked once a present (one system call); the groups they name are the
game's own objects and must lie in the guest heap, which `mem_copy_readable()` then copies from without asking the
system.

A moving group sorts before every still one, so a moving floor is never crowded out by the still map pieces nearer to
Link (a group's translation is its pivot, which for a moved floor can be metres from its surface). The nearest are kept
by insertion by distance; the farthest drops.

### `floor_scan()`, `floor_scan_snapshot()`, `floor_scan_read()`

`NX2SARIA_FLOOR_SCAN` (`floor_scan` in the ini, 0 by default): finds the moving floor under Link in guest memory
(diagnosis, 1.1.1). The Deku Tree's floating platform is not one of the engine's ground groups (`floor_probe()`), and
where its position lives is not known.

The scan arms while Link drifts at the 60 fps rate: a step of one frame, the gamepad's left stick under 20%, and for
`FLOOR_SCAN_ARM` presents in a row a horizontal move of 2 to 30 mm a present, his height within 5 mm of the present
before, and the whole stretch straight (its end within 80% of the sum of its moves), so a floor that only sinks does not
arm it. It then takes one snapshot (`floor_scan_snapshot()`) of every 4-byte-aligned float triplet in the game's heap
and alias regions whose x and z lie within 6 m of Link and whose y lies from 4 m below him to 0.5 m above (walked as
the debug menu's memory search walks them, `runtime/diag/debug_tools.c` `dbg_search_new()`; the cheap test on x first,
which most words fail), and logs how long that took. `FLOOR_SCAN_ARM` presents later it keeps the triplets that moved
horizontally as far as Link (within a fifth, 2 cm at least) and lie more than 0.3 m below him or more than 0.5 m from
him horizontally: the floor's matrix, body and drive, but not Link's own copies (his actor, body, feet, which stay at
his feet) nor what rides above him. A snapshot that keeps none arms the scan again, three times at most. From then on,
once a second, it logs Link's position and move over that second and one line per survivor (`FLOOR_SCAN_SHOW` at most)
with its address, position and move over the same second, so a stretch at a higher frame rate shows each one's own
speed. The value is the number of lines to write (1 means 20000); off it costs one test per present.

`NX2SARIA_FLOOR_WATCH` switches the scan on too (the watch takes its addresses from the scan) and gets its survivors
when the logging starts, with those that lie in the platform's rigid body controller unit (vtable main+0x4bf8a88)
marked (`runtime/diag/floor_watch.c` `floor_watch_found()`). The owner of the survivor deepest below Link, the floor's
own body or matrix, is dumped (`floor_scan_dump()`).

`floor_scan_read()` reads the n triplets at `fs.addr` (NAN where the memory could not be read), in one batched copy.

### `floor_scan_dump()`, `main_data_word()`, `vtable_below()`

The owner of a floor point (`floor_scan()`, once, when its survivors are known): the 0x500 bytes from 0x400 before the
point to 0x100 after it as 8-byte words, the words that point into main's data (where its vtables are, from
main+0x4800000) named as main offsets, and the objects that hold a pointer to it. Those are found by one more walk of
the heap and alias regions, for the start of each of the `FLOOR_SCAN_BASES` objects nearest below the point (a word
that points into main's data is taken as the vtable that starts one), and each holder (`FLOOR_SCAN_HOLDERS` at most) is
logged with the vtable nearest below it. So one run gives the class whose object the platform's position is in, and the
classes that refer to it (the character controller's ground reference among them), for the code that reads it to be
found by its vtable.

`vtable_below()`: the nearest word at or below a (within 0x400) that points into main's data, the vtable of the object
a is in.

### `frame_step_present()`

The debug menu's speed factor (`runtime/diag/debug_tools.c`) multiplies the step the game is given. A load or a
debugger stop resets the steady step: the intervals before it say nothing about the next.

The game clock of untimed script lines (`game_us`): what the game stepped this frame, the patch page's step with the
real-time step (kept through a reset), whole vsyncs without it. After its first read the floor scan costs one test
alone while off. With `NX2SARIA_TRACE=perf` in a free-running build the speed line takes the game's own step, as its
time manager holds it.

## `runtime/pacing/rtss_decide.h`

RivaTuner Statistics Server as the frame limiter (`runtime/pacing/rtss_limit.c`): the parts that make no system call,
so that `tests/rtss_limit_check.c` builds them on any system. `rtss_parse()` reads RTSS's shared memory,
`rtss_decide()` decides each poll of the worker, and `rtss_poll()` carries that decision out through the calls
`runtime/pacing/rtss_limit.c` gives it (`RtssOps`). The file's sections: RTSS's shared memory, the worker's decisions,
the game's pacer beside RTSS, the state file, RTSS's own settings.

Constants and enums:

- `rtss_limit_want`'s argument: a limit in millihertz (>0), 0 for none, or why RTSS is kept out of it
  (`runtime/pacing/frame_rate.c` `frame_step_cap_ns()`). `RTSS_UNSET` (-2) is the want before the first one.
  `RTSS_OUT_SETTING` (-1): `NX2SARIA_RTSS=0`, for the whole run. `RTSS_OUT_FRAMEGEN` (-3): frame generation, RTSS would
  count the generated frames too. `RTSS_OUT_LOWLAT` (-4): a driver's low latency (`runtime/pacing/vk_lowlat.c`) holds
  the frames where they start.
- `RTSS_SIGNATURE` 0x52545353: 'RTSS' as RTSS writes it, the memory holds valid data.
- `RTSS_OVER_PERIODS` 3: fresh periods in a row RTSS's frame rate of the game may stay above the limit.
- `RTSS_OVER_SLACK` 1.05: above limit * this, RTSS is taken not to hold the game to it.
- `RTSS_FRESH_NS` 2.5 s: a period is stale after this, or 2.5 times its own length if longer.
- `RTSS_HOLD_IDLE_NS` 200 ms: no host present for this long, RTSS holds nothing (as `HOLD_IDLE_NS` in
  `runtime/pacing/vk_lowlat.c`).
- `RTSS_SETTLE_POLLS` 2: polls in a row a want must stay before it acts on the profile.
- `RTSS_UNPROVEN_POLLS` 60: polls (30 s) RTSS may hook the game with the limit and not show it holds it.
- `RTSS_FAIL_*`, out of it for the run: `LIBRARY` its hooks library did not load; `READONLY` its profile cannot be
  written; `NOT_LIMITING` its frame count showed the game past the limit; `UNPROVEN` it hooked the game with the limit
  for `RTSS_UNPROVEN_POLLS` and never showed it holds it.
- `RTSS_WHY_*`: `HOLDS` RTSS holds the game to the want; `NOT_LISTED` RTSS has the limit but has not hooked the game
  yet; `OUT` kept out (the want says why); `FAILED` `RtssState.fail`; `NOT_RUNNING`; `REFUSED` RTSS refused this limit;
  `NEWER` the want changed while the poll ran, nothing decided, the next poll takes the new one; `UNSET` no want yet;
  `MOVIE` a movie plays, the game's pacer holds it at its rate and keeps its slots; `NOT_VULKAN` RTSS lists the game but
  has not seen its Vulkan presents; `UPDATE` RTSS has not read the profile the game wrote yet; `NOT_COUNTING` RTSS has
  measured no period of the game's frames lately; `IDLE` no picture went out lately (a minimised window, a frame hold),
  RTSS has none to hold; `SETTLING` a new want, it reaches the profile once it stayed `RTSS_SETTLE_POLLS` polls;
  `NO_PRESENT` no picture went out yet, nothing is written before the first.
- `RTSS_WROTE_*`, what a write of the limit did (`RtssOps.apply`): `READONLY` (-1) the save did not reach the file;
  `REFUSED` (0) RTSS refused the value, the profile keeps what it held; `HELD` (1) the profile holds the limit;
  `REFUSED_CLEARED` (2) refused, and the earlier limit of this run was cleared to none.

Types:

- `RtssApp`: `running` the signature and a version of 2.0 or later; `listed` the application list holds this process,
  RTSS has hooked it; `flags` the entry's flags; `t0`, `t1`, `frames` the period of this process's entry.
- `RtssState`, what the worker keeps between polls: `applied` the millihertz the profile holds from this run, -1
  untouched; `refused` a limit RTSS refused (out of its range), the game's own limiter keeps that one; `changed` the
  profile holds a value from this run, put the old one back on exit; `over` fresh periods in a row RTSS's frame rate of
  the game stayed above the limit; `fail` `RTSS_FAIL_*`; `held` the poll before stood aside, a fresh period now shows
  how RTSS holds the game; `t1`, `span` the end and length (ms) of the newest period seen; `fresh_ns` when a new period
  was last seen, 0 never; `seen` the want of the poll before; `polls` polls in a row with that want; `unproven` polls in
  a row RTSS hooked the game with a limit in the profile and did not show its hold.
- `RtssIn`, one poll's view: `want` the want the poll read; `current` the want is still that one when the poll decides;
  `hooks` RTSS's hooks library, 1 loaded, 0 failed to load, -1 not tried (`rtss_wants_hooks()`); `movie_mhz` the rate
  of a movie that plays (`rtss_limit_movie`), 0 none; `now_ns` `kern_now_ns` at the poll; `hosted_ns` `kern_now_ns` of
  the newest host present (`runtime/gfx/gfx_present.c` `gfx_window_hosted_ns()`), 0 none yet.
- `RtssOut`: `act` `RTSS_DO_*`; `mhz` the value decided for, the profile's (`rtss_target()`) for a limit, the want for
  a kept-out one; `holds` RTSS holds the game to it, the game's own limiter stands aside; `why` `RTSS_WHY_*`; `fps`
  RTSS's frame rate of the game, when the frame count check gave up on it.
- `RtssSaved`: `existed` the profile existed before the run changed it; `old_limit`, `old_den` what it held then;
  `limit`, `den` what the run wrote last; `prev_limit`, `prev_den` what it wrote before, while the profile may still
  hold it, else `limit` and `den`; `name` the profile's name, the executable's (ocarina.exe).
- `RtssSettings`: below, at `rtss_settings_limit()`.

### `rtss_out_text()`, `rtss_out_limiter()`

For the log: what keeps RTSS out, and the limiter that holds the frames while RTSS is kept out.

### `rtss_parse()`, `rtss_fps()`

RTSS's shared memory, "RTSSSharedMemoryV2". The header: signature, version, application entry size, offset of the
application array, its length. An entry: the process id, the executable path (260 bytes), flags, then RTSS's last frame
rate period for the process: its start and end in ms and the frames in it. The flags' low 16 bits hold the API RTSS saw
the process present with, a value rather than bits (Vulkan is 0xA, `RTSS_API_VULKAN`), and `RTSS_UPDATE_ASKED`
(0x10000000) stays set from UpdateProfiles until the process has read its profile again. `RTSS_APPS_MAX` 4096 bounds
the entries read.

`rtss_parse()` reads the memory of size bytes at p for the process pid and never reads past size. `rtss_fps()`: the
frame rate RTSS measured for the process over its period; 0 unknown.

### `rtss_target()`

What the profile gets for a want w >= 0 (the play limit: `runtime/pacing/frame_rate.c` `frame_step_cap_ns()` wants it
through a movie too) while a movie plays at movie_mhz: w, so that a movie no longer rewrites the profile at its start
and end; but no limit (0) while the movie runs faster than w (a limit of 30, 40 or 45), where RTSS would hold the movie
below its rate. The game's pacer holds every movie at its rate and keeps its slots through a late frame
(`runtime/nvn/nvn_api_obj.c` `pace()`), sending the frames that catch up at once. Above the movie's rate RTSS lets them
through faster than the movie runs. At the movie's own rate (a limit of 60 for the game's 60 fps movies, the default)
the profile keeps that rate, and RTSS holds the movie beside the pacer for its length: whether RTSS's limiter then
spaces out the frames that catch up, so that the picture falls behind the sound at each late frame, is not measured.
Giving the profile no limit there too would cost two profile writes per movie at the default limit.

### `rtss_limit_value()`

The profile's FramerateLimit and FramerateLimitDenominator for mhz (>= 0): a whole rate over 1, a fraction over 1000
where RTSS knows the denominator (has_den), else rounded down to whole frames. Only "display" asks a fraction (59.94 or
143.856 Hz monitors), and a limit rounded up (60, 144) let the game run above the refresh, so that every 7 to 17
seconds a frame came that the monitor had no refresh for.

### `rtss_wants_hooks()`

The hooks library is loaded only on a poll that may write the profile: a limit is wanted, RTSS is not out of it for the
run, and it runs. A poll of a kept-out want never loads it; a later limit does.

### `rtss_decide()`

The decision of one poll, in this order: no want does nothing; a want acts on the profile only once it stayed
`RTSS_SETTLE_POLLS` polls in a row, so a want that flips (a driver's low latency on and off at each swapchain rebuild,
`runtime/pacing/vk_lowlat.c`) does not write and remove the profile at each flip, while the game's limiter takes a new
want at once (`rtss_limit_active`). A settled kept-out want puts back what this run wrote (once the hooks are loaded); a
failure lasts the run; RTSS not running leaves the profile for its return; a library that did not load fails the run;
nothing is written before the first picture went out; a refused limit stays with the game; a settled new limit
(`rtss_target()`) goes into the profile; RTSS reads it after the write, so a later poll sees whether it holds.

The game's limiter stands aside only once RTSS is seen to hold the game: the profile held the limit at the poll before
(RTSS reads it after the write), RTSS lists the game with its Vulkan presents, has read the profile
(`RTSS_UPDATE_ASKED` clear) and measured a period of the game's frames lately (a fresh one: its end, dwTime1, moved
within `RTSS_FRESH_NS`, or within 2.5 times the period's length if that is longer), no movie plays (`rtss_target()`)
and a picture went out within `RTSS_HOLD_IDLE_NS` (one went out at all: `RTSS_WHY_NO_PRESENT` before). An RTSS that
lists the game with a limit in its profile while pictures go out, but does not show its hold (another API's presents,
the profile never read, no period measured), may hold the frames beside the game's limiter: after
`RTSS_UNPROVEN_POLLS` such polls in a row the profile goes back for the run. A want that changed while the poll ran
(`RTSS_WHY_NEWER`): these frames are not the new want's.

Each fresh period after a poll that stood aside is checked against the limit: more than `RTSS_OVER_SLACK` times it for
`RTSS_OVER_PERIODS` periods in a row puts the profile back for the run; a stale period counts neither way. After the
exit put the profile back (`rtss_limit_restore`) no poll decides: `runtime/pacing/rtss_limit.c` `worker()` skips it
under the lock.

`held` and `unproven` are cleared at each poll until it stands aside again, or counts as unproven again.

### `rtss_written()`, `rtss_put_back_done()`, `rtss_poll()`

`rtss_put_back_done()`: the profile was put back, nothing of this run is in it.

`RtssOps`, the profile calls `rtss_poll()` makes: `apply` writes mhz (`first`: the first write of the run, which
records what to put back; `clear`: on a refusal, clear this run's earlier limit to none) and returns `RTSS_WROTE_*`;
`put_back` puts the profile back as it was before the run (o says why: kept out, or the frame count check gave up).

### `rtss_fail_text()`, `rtss_why_text()`

Why the game's own limiter paces, for the log line "the game's own limiter (<text>)".

### `rtss_pace_period()`

The game's pacer beside RTSS (`runtime/pacing/frame_rate.c` `frame_step_cap_ns()`). The period the pacer keeps
(`now_ns` and `hosted_ns` are `kern_now_ns` times; `hosted_ns` of the newest host present,
`runtime/gfx/gfx_present.c` `gfx_window_hosted_ns()`, 0 before the first). The limit's own period while RTSS does not
hold the game, while a movie plays (from its first frame: the pacer keeps the movie's slots, which RTSS does not know;
`rtss_target()` says what RTSS's profile holds meanwhile), before the first host present, and once no host present went
out for `RTSS_HOLD_IDLE_NS`: a minimised window or a frame hold (`runtime/vk/vk_backend.c` `frame_hold()`) gives RTSS
nothing to hold, and a pacer that held nothing then let the quick menu run at the 640-700 fps that read address 0
(`runtime/pacing/frame_rate.c` `LIMIT_FREE_MAX_HZ`). While RTSS holds, the ceiling's period and never 0: the frames
RTSS lets through come further apart than that, so `pace()` waits for none of them, and it still holds the game's
frames to the ceiling the game's code needs. The exception is a limit of 240, or unlimited, which RTSS gets as 240:
there the ceiling is RTSS's own period, and both may hold a frame.

### `rtss_saved_format()`, `rtss_saved_parse()`, `rtss_name_eq()`, `rtss_saved_find()`, `rtss_saved_update()`

The state file (`runtime/pacing/rtss_limit.c`, `rtss-profile.txt` next to the executable): each RTSS profile a run
changed, as it was before, so that a start after a run that did not close (a kill, a hang, a crash) puts it back. One
line per profile, so executables that share the folder keep their own:

```text
rtss1 <existed 0|1> <old limit> <old denominator> <written limit> <written denominator> <previous limit> <previous denominator> <profile name>
```

on one line. The name comes last and may hold spaces; lines that start with # are comments (`RTSS_SAVED_HEADER` is the
file's header of such lines). The line takes a new value before the profile does, and the value written before it
stays as the previous one until the profile holds the new one: a run that ends between the two leaves either value in
the profile, and both are the game's.

- `rtss_saved_format()`: the line of r, with its newline; its length, 0 when it does not fit n.
- `rtss_saved_parse()`: one line (len bytes, no newline) into r; 1 when it is a well-formed line of this format, 0
  otherwise.
- `rtss_name_eq()`: profile names compare as Windows compares file names, ASCII letters in either case.
- `rtss_saved_find()`: the line of profile name in the file's text (NUL-terminated), 1 and r, or 0. A line that is not
  well formed is passed over, and the lines after it are still read.
- `rtss_saved_update()`: the file's text with profile name's line replaced by r's (r NULL: removed): the header, every
  other profile's well-formed line as it was, then r's. Comments, blank and malformed lines are dropped. Writes the
  text into out and returns its length (0 when it does not fit cap); `*records` is the number of profile lines in it, 0
  when the file is to go.

### `rtss_restore_plan()`

What a start does with a line a run that did not close left (r), given the profile as it is now: whether it exists and
the limit and denominator it holds. Either value of the line is the game's (`RtssSaved.prev_limit`).
`RTSS_PLAN_NONE`: no profile, and there was none before that run, as it was. `RTSS_PLAN_LEAVE`: the player changed or
removed the profile since, theirs as it is. `RTSS_PLAN_WRITE`: it still holds what the run wrote, the old limit and
denominator back. `RTSS_PLAN_DELETE`: the same, for a profile that run made, removed.

### `rtss_settings_limit()`

RTSS's own `[Framerate]` settings for the game, as its profile file holds them (`runtime/pacing/rtss_limit.c`
`read_settings()`), in `RtssSettings`: a frame rate limit (Limit over LimitDenominator), a frame time limit (LimitTime
over LimitTimeDenominator) and scanline sync (SyncScanline0 and SyncScanline1), 0 off for each; and the modes beside
them, for the log: SyncDisplay, ReflexSleep and PassiveWait. `rtss_settings_limit()`: RTSS holds the game's frames by
a setting of its own.

### `rtss_out_of_limit()`

RTSS has no part in the game's limit although it may list the game: the want keeps it out, or it is out for the run
because its profile cannot be written, its hooks library did not load or it never showed it holds the game (its profile
put back, a setting of its own may still hold the game). A run whose frame count check gave up on RTSS does not count:
RTSS was seen not to hold the game.

### `rtss_two()`

Two limiters: RTSS is out of the game's limit (`rtss_out_of_limit()`), and while it lists the game a setting of its own
still holds the frames (`rtss_settings_limit()`), beside the game's limiter, the driver's or frame generation's.

### `rtss_settings_text()`

"limit L/D, frame time limit T/TD, scanline sync S0/S1", and with modes ", display sync X, Reflex sleep R, passive wait
P": the values as the file holds them.

## `runtime/pacing/rtss_limit.c`

RivaTuner Statistics Server as the frame limiter (`runtime/pacing/frame_rate.c` `frame_step_cap_ns()`): where RTSS is
installed and running, the frame limit goes into RTSS's profile for this executable and, while RTSS is seen holding the
game to it, the game's own pacer keeps only the 240-frame ceiling (`runtime/pacing/rtss_decide.h`
`rtss_pace_period()`), so there is one limiter (at a limit of 240 the ceiling is RTSS's own period). Anywhere else, or
when any step fails, the game's own limiter works as it always did. Windows only: elsewhere the file builds only the
stubs at its end, which do nothing.

RTSS's hooks library (`RTSSHooks64.dll` in the folder its installer records under
`HKLM\SOFTWARE\WOW6432Node\Unwinder\RTSS`, `InstallDir`) exports the profile calls, loaded here at run time:
`LoadProfile(name)` reads a profile (the global one for a name without its own), `Get/SetProfileProperty` read and
change the loaded copy, `SaveProfile(name)` writes it to `Profiles\<name>.cfg`, `UpdateProfiles` makes every hooked
process read its profile again, `EnumProfiles` lists the profiles that exist (`profile_exists()` says why it is not
called), `DeleteProfile` removes one. "FramerateLimit" is the limit in frames per second (0: none);
"FramerateLimitDenominator" divides it, so 59940 over 1000 limits to 59.94 (a monitor's measured rate). SaveProfile
reports nothing: RTSS's Profiles folder sits under Program Files, which a process without administrator rights cannot
write, and the save then fails silently. So every change is read back from the saved profile, and a limit that did not
stick leaves the game's own limiter in charge for the session.

RTSS runs when its shared memory "RTSSSharedMemoryV2" carries the signature 'RTSS'. The game's own limiter stands aside
only once RTSS is seen to hold the game (`runtime/pacing/rtss_decide.h` `rtss_decide()`): the memory's application list
holds this process id with its Vulkan presents (RTSS hooks a process some seconds after it starts, its injection
delay), RTSS has read the profile the game wrote, it measured a period of the game's frames lately, no movie plays and
pictures go out. Until then, and again whenever one of these ends (RTSS quits, the window is minimised), the game's own
limiter paces the frames. While RTSS limits, each new period RTSS measures for the game is checked against the limit:
above 1.05 times it for three periods in a row, and the game takes the limiting back for the rest of the run. So it
does when RTSS lists the game with the limit for 30 s while pictures go out and never shows its hold: RTSS may then hold
the frames beside the game's limiter.

The calls run on a thread of their own, every half second (`POLL_MS` 500), which also looks RTSS up (`find_rtss()`), so
a present never waits on the registry or profile files. On every orderly exit (`rtss_limit_restore()`) the profile gets
back what it held before the game changed it: the old limit, or no profile of its own when there was none; from then on
no want reaches it and the game's own limiter paces the frames left. What it held is also kept in `rtss-profile.txt`
next to the executable (`runtime/pacing/rtss_decide.h` `RtssSaved`), written before the run's first change of the
profile and removed once the profile is back: an exit that skips the restore (a kill, a hang, the device-lost watchdog)
leaves the line, and the next start puts the profile back from it before it changes anything (`left_start()`,
`restore_left()`), unless the player changed the profile since; a start with `NX2SARIA_RTSS=0` only logs it. A crash
and a fatal error leave it to that start too (`rtss_limit_crash()`), with no call into RTSS's library from the failing
process, unless the file could not be written: then they put the profile back as an orderly exit does, but not from the
worker's own thread.

`NX2SARIA_RTSS=0` keeps RTSS out of it. Frame generation keeps it out too (RTSS counts every presented frame, the
generated ones included), and so does a driver's low latency (`runtime/pacing/vk_lowlat.c`), which holds the frames
where they start. While RTSS is kept out the thread does not load the hooks library and only reads RTSS's own settings
for the game, to tell when they limit it a second time (`watch()`), except once at the start, to put back a profile
that a run which did not close left with its limit (`left_start()`); a later limit (frame generation or low latency
switched off) loads it and hands RTSS the limit as usual, and `NX2SARIA_RTSS=0` keeps the thread read-only for the
run.

State:

- `lock`: the hooks calls and the state below. `S`: the worker's decisions (`runtime/pacing/rtss_decide.h`), under
  `lock`.
- `want`: millihertz from the pacer, >0 a limit, 0 none; `RTSS_OUT_*` kept out. `active_for`: the want RTSS holds the
  game to now; `RTSS_UNSET` none. `movie_mhz`: a movie's rate while one plays (`rtss_limit_movie()`), 0 none.
- `failed`: `RTSS_FAIL_*`, RTSS is out of it for the rest of the run (`S.fail`). `closed`: `rtss_limit_restore()` ran,
  nothing reaches the profile any more. `two`: kept out, and RTSS limits the game by a setting of its own (`watch()`).
  `started`: `rtss_limit_want()` spawned the worker. `ready`: the worker found RTSS, `dll_path`, `rtss_dir` and
  `profile` hold their values.
- `has_denominator`: this RTSS knows FramerateLimitDenominator. `had_profile`: the profile existed before this run
  changed it. `owed`: this run changed the profile and has not put it back; `had_profile`, `old_limit` and
  `old_denominator` stay as found.
- `creations`, `removals`: the profile's "created" and "removed" lines this run, numbered (under lock).
- `state_path`: `rtss-profile.txt` next to the executable; "" when its path is unknown. `rec`: this run's line in it,
  while the profile holds a value of this run. `recorded`: the file holds `rec`, a crash leaves the profile to the next
  start. `state_off`: a line this run could not act on stays as it is, no writes of the file.
- `rtss_tid`: the worker's thread, which may hold the lock when a crash comes.
- `settings`, `settings_file`: RTSS's own settings for the game and which file they came from, as `Profiles\<name>`.

### `rtss_map()`

RTSS's shared memory as `rtss_parse()` reads it: whether RTSS runs, whether it lists this process, and its frame count.

### `profile_exists()`

The profile exists on its own: RTSS keeps it as `Profiles\<name>.cfg` in its folder. EnumProfiles would say the same (a
comma-separated list of names) but crashes this process: it walks `Profiles\*.cfg` with `_findfirst` and `_findnext`
and keeps the find handle in 32 bits. The handle points into the process heap, which here lies above 4 GB (the guest
address space takes the low part, `runtime/memory.c` `mem_init()`), so `_findnext` gets a cut pointer and faults in
ntdll writing to it. `NX2SARIA_RTSS_ENUM=1` calls EnumProfiles anyway, to test an RTSS that keeps the whole handle.

### `write_limit()`

Writes limit/denominator into the profile and reads them back from the saved file: 1 when they stuck, 0 when RTSS
refused a value, -1 when the save did not reach the file. RTSS checks each value against the other one already in the
profile (the limit over the denominator must be a rate it accepts: 165080 is refused over 1 and taken over 1000), so
the limit goes to 0 (none) before the denominator changes.

### `state_put()`

The state file with this profile's line set to r, or removed for r NULL; the file goes once it holds no line. Under
lock: the worker, or `rtss_limit_restore()`'s thread at the exit. 1 when the file holds what was asked. A file that is
there but did not read (another program holds it, a scan) keeps the lines of the other executables in the folder:
rewritten from nothing, they would go.

### `unrecorded()`

The state file did not take this run's line: a crash then puts the profile back as an orderly exit does
(`rtss_limit_crash()`). Logged once a run.

### `record_before()`, `record_after()`

Before a write of limit/denominator into the profile: `rec`'s written value is that one, and the one written before it
stays as the previous one (`runtime/pacing/rtss_decide.h` `RtssSaved`), so that a run that ends before the profile
changed, or after, leaves a profile the next start tells from a value the player set since. After that write: the
profile holds the written value alone (landed), or kept the previous one.

### `apply()`

Writes mhz into the profile (`rtss_poll()`'s `RtssOps.apply`): `RTSS_WROTE_*`. first: the run's first write, which
records what to put back on exit, in the state file too before anything changes; clear: on a refusal, clear the limit
of this run the profile holds.

- What to put back on exit is read only when the profile owes nothing: after a put back that did not land, the profile
  still owes it, and `rec` is still in the file then, the profile holding its value.
- The line goes in before the profile changes: a kill between the two finds it as it was.
- A save that did not reach the file: the line keeps both values (the profile may hold either); on the first write
  the line goes, the profile being as it was.
- The "created" line is numbered: a want that flips between a limit and kept out creates the profile again at each
  flip.
- A refusal: RTSS must not keep an earlier limit of this run under the game's own one, so with `clear` that limit goes
  to none.
- A limit rounded to whole frames by an RTSS without FramerateLimitDenominator: the log says what RTSS holds.

### `put_back()`

The profile as it was before this run changed it (the caller checked `S.changed` or `owed`, and marks `S` done). Its
line in the state file goes once it is back; one that did not go back keeps the line, and `owed`, for the exit and the
next start.

### `left_line()`

This profile's line in the state file into r: 1 when there is one.

### `restore_left()`

The line r of this profile, left in the state file by a run that did not close (`left_start()`): before this run changes
anything, the profile gets back what it held before that run (`runtime/pacing/rtss_decide.h` `rtss_restore_plan()`),
unless the player changed it since, and the line goes. A profile that does not go back keeps the line, and this run
leaves the file alone.

### `left_start()`

The state file's line for this profile, on the worker's first poll that finds RTSS running (w the want it read, hooks as
`RtssIn.hooks`): 1 once the line is dealt with or there is none, 0 when a restore is still owed, for the first poll
with the hooks library. A limit puts the profile back with the library it loads for its own writes. Frame generation and
low latency keep RTSS out of the run's limit, but a profile left with an old limit would hold the game beside them (two
limiters, and under frame generation RTSS would count the generated frames too): the library is loaded for that one
restore. `NX2SARIA_RTSS=0` keeps the thread read-only, so the log says what the file holds.

### `put_back_op()`

`rtss_poll()`'s `RtssOps.put_back`. When the frame count check gave up on RTSS the log gives RTSS's frame rate against
the limit; for an unproven hold, the lines before say which sign of its hold never came.

### `set_active()`

Whether RTSS holds the game to the want w this poll read, and the log of which limiter is in use: the first decision,
then each change of the limiter or of why the game's own one paces (RTSS not hooked yet, not read the profile, a
minimised window), up to `LIMITER_LINES` (100) lines. The want may have changed since the poll read it:
`rtss_limit_active()` compares the two, so a stale store never lets the game's limiter stand aside for a newer want.
The store is made either way (a want that went w, w2, w again must not find an older hold), but a hold is logged only
while its want is still the current one: the game's limiter paces a newer want, and the next poll logs what holds it.

### `read_settings()`, `file_stamp()`, `cfg_int()`

RTSS's own settings for the game, read from its profile file as RTSS keeps it: `Profiles\<exe>.cfg`, else
`Profiles\Global`, and a key the game's profile lacks from Global. The system's ini reader does it, so no call goes into
RTSS's hooks library, which a poll of a kept-out want never loads. The files are read again only when the time either
was written changes (`file_stamp()`, 0 for no such file; the stamps start as never read). Worker thread only, while
RTSS lists the game (`watch()`). `cfg_int()` parses the text itself: `GetPrivateProfileIntA` would read a negative
scanline as 0.

### `watch()`

While RTSS lists the game, the settings it holds for it are logged once per change (40 lines at most): what RTSS applies
may be more than the FramerateLimit the game writes (a frame time limit, scanline sync, its own Reflex). While RTSS has
no part in the game's limit (`runtime/pacing/rtss_decide.h` `rtss_out_of_limit()`: kept out, w < 0, or out for the
run because its profile cannot be written or its hooks library did not load) the game's own limiter, the driver's low
latency or frame generation holds the frames, and a limit the player set in RTSS itself for the game (its global
profile or the game's own, a frame time limit or scanline sync) still holds them a second time. Logged once per
change, and again when the other limiter changes (40 lines at most); a want that RTSS takes again logs afresh when it
is kept out later. The menu shows it for a kept-out want (`rtss_limit_state()` 4), while a profile that cannot be
written keeps its own note there (state 2).

### `find_rtss()`

RTSS's folder (`InstallDir` under its registry key), its hooks library and this executable's profile name, on the
worker before its first poll. 0 when RTSS is not installed: the game's own limiter for the run. RTSS names a profile
after the executable, as ocarina.exe. The state file goes next to the executable, where the ini is
(`runtime/cfg/cfg.c` `find_ini()`).

### `worker()`

Once the worker found RTSS, `rtss_limit_restore()` may read the names (`ready`). A want read before
`rtss_limit_restore()` must not reach the profile after it, so a closed run skips the poll under the lock. The state
file's line left by a run that did not close is dealt with before this run writes anything. A newer want (`NEWER`) waits
for the next poll.

### `rtss_limit_want()`

Returns at once once the exit put the profile back. A new limit: the game's own limiter holds it until the worker had
RTSS apply it, up to `POLL_MS` later (a movie's cap that waited for that ran the movie's 60 Hz count fast for half a
second): `active_for` no longer matches. The first want starts the worker, a kept-out one too (the worker then only
reads, `watch()`). The worker looks for RTSS itself (`find_rtss()`), so the present that gets here waits on no registry
key or file.

### `rtss_limit_restore()`

The game presents on after this call (the rest of the exit runs first), so from here the game's own limiter holds the
frames left and no want reaches the profile again: the worker would otherwise write the game's limit back into the
profile just put back, and the next run would take it for the player's own. `closed` is set before the check of
`ready`, so that an exit before the first limit, or while RTSS was kept out, also stops a later want from starting the
worker. The exit may come from a crash on any thread: it gives up after a second (100 tries 10 ms apart) rather than
wait on a lock held there. `tests/exit_order_check.py` and `tests/rtss_limit_check.py` read this function and
`rtss_limit_want()`, `rtss_limit_crash()`.

### `rtss_limit_crash()`

A crash's and a fatal error's end (`runtime/main.c` `crash_handler()`, `runtime/dispatch.c` `fatal_end()`), from any
thread: no want reaches the profile any more, and no call goes into RTSS's library, which may be what failed and is no
safe call on a damaged heap. The state file keeps this run's line, and the next start puts the profile back from it
(`restore_left()`). Without that line (the file could not be written) the profile goes back as at an orderly exit, but
not from the worker's own thread, which may hold the lock that takes. `tests/rtss_worker_check.c` (Windows) runs the
real file against a fake RTSS.

## `runtime/pacing/vk_lowlat.h`

Low latency without frame generation (`runtime/pacing/vk_lowlat.c`): the driver's own mode, NVIDIA's through
`VK_NV_low_latency2` or AMD's through `VK_AMD_anti_lag`, used directly. `NX2SARIA_LOW_LATENCY`: 0 off, 1 on, 2 on with
boost (NVIDIA), live. The declarations are in three groups: the window's swapchain (`runtime/gfx/gfx_present.c`, with
its present lock held; NVIDIA's mode belongs to a swapchain), the game's frame, and the host present (the present
thread).

Frames are numbered by the game's presents, frame n ending with the n-th, as `gfx_counters.frames` numbers them at the
present (`runtime/vk/vk_backend.c`), so the number a guest thread starts a frame with is the one the replay and the
present thread end it with.

### `vk_lowlat_revision()`

The revision (`specVersion`) of `VK_NV_low_latency2` in a driver's list of device extensions, 0 when the list does not
have it. `runtime/vk/vk_device.c` `create_device()` keeps it in `VkCtx.low_latency2_rev` when it enables the extension,
and `vk_lowlat_init()` chooses by it what it may call ("The extension's revisions" under `runtime/pacing/vk_lowlat.c`).
In the header so that `tests/lowlat_revision_check.c` reads a list with it.

### `vk_lowlat_init()`, `vk_lowlat_release()`

`runtime/vk/vk_device.c` `create_device()`, once the device exists: takes the entry points of the extension the device
enabled (`VkCtx.has_low_latency2` with `low_latency2_rev`, `has_anti_lag`) and logs which path this run has, for
NVIDIA's with the revision and what the run leaves out because of it. `vk_lowlat_release()`: before the device goes.

### `vk_lowlat_status()`

What the menu's row shows (`runtime/ui/menu.cpp` `row_low_latency()`). The first two (`VK_LOWLAT_NV`,
`VK_LOWLAT_AMD`) are the paths that can run; the others say why none does: the driver has neither extension
(`NO_DRIVER`), DLSS Frame Generation's interposer owns low latency in this run (`DLSSG`; `runtime/vk/vk_dlssg.c`,
`NX2SARIA_REFLEX`), a frame generation presents the frames in the window swapchain's place (`FG`), the driver refused
the mode or never ended its sleep, which the log says (`FAILED`), or the driver has a mode but the run started with the
setting off, so the device was made without it (`RESTART`; `runtime/vk/vk_device.c` `create_device()`): the next start
with it on has it.

### `vk_lowlat_on()`

The direct path is in effect now: the setting is on and nothing else owns the presents. Any thread, one atomic load.

### `vk_lowlat_swapchain_wanted()`

1 when the next plain swapchain is to be made with `VkSwapchainLatencyCreateInfoNV` (NVIDIA's path, the setting on).

### `vk_lowlat_swapchain()`

made 1, after a swapchain was created: the plain swapchain made that way, on which the sleep mode is set from here on,
or `VK_NULL_HANDLE` for one this path cannot name (a frame generation's, one made without the mode). made 0, with
`VK_NULL_HANDLE`: before the window's swapchain is replaced or destroyed.

### `vk_lowlat_swapchain_refused()`

No swapchain could be made with the mode asked for (`runtime/gfx/gfx_present.c` `create_swapchain()`, after three
tries): the mode is given up for the run, so the next swapchain is made without it.

### `vk_lowlat_fg_presents()`

The frames go out through a frame generation presenter of the runtime's own (`runtime/gfx/gfx_present.c` `ng_ready()`),
several presents a rendered frame: the direct path steps aside while on is 1.

### `vk_lowlat_frame_start()`

A frame starts: the first of the game's pad read (`at_input` 1, `runtime/hle/ov_input.c`) and its acquire of a window
image (0, `runtime/nvn/nvn_api_obj.c`) after a present. The driver's sleep, then the simulation-start marker; the pad
read also sets the input marker. Returns at once for a frame that already started and while the mode is off.

### `vk_lowlat_submit_start()`, `vk_lowlat_game_present()`, `vk_lowlat_submitted()`

The frame's first submission is handed over (`runtime/vk/vk_backend.c` `vk_be_submit()`); the game presents it
(`runtime/vk/vk_backend.c` `vk_be_present()`), which counts it; its submissions were replayed (where
`vk_dlssg_submitted` is called).

### `vk_lowlat_pace()`

`nvnQueuePresentTexture` in `runtime/nvn/nvn_api_obj.c`: 1 when the driver's sleep holds the next frame to period_ns,
so the pacer must not wait as well; 0 when the pacer holds it as before (the mode off, period 0, or no host present
went out lately).

### `vk_lowlat_submit_tag()`

`runtime/vk/vk_sync.c` `queue_submit()`: 1 with `*tag` filled when the submission being handed to the queue is to carry
the frame's number (NVIDIA's path on); the caller chains it into its `VkSubmitInfo2`.

### `vk_lowlat_present_frame()`, `vk_lowlat_blit_frame()`

`runtime/vk/vk_backend.c` `present_main()`: the next blit is of this frame. `runtime/gfx/gfx_present.c`
`present_image()`, at the blit's submit: the frame set above when the direct path follows it (0 otherwise), taken, and
`*tag` filled when `*tagged` is set to 1 as for `vk_lowlat_submit_tag()`.

### `vk_lowlat_present_begin()`, `vk_lowlat_present_end()`

Around the present of that frame (`runtime/gfx/gfx_present.c` `flip_locked()`; frame 0: nothing). Begin: AMD's present
stage or NVIDIA's present-start marker; returns what goes into the `VkPresentInfoKHR`'s pNext (the frame's present id)
or NULL. End: the present-end marker and the statistics.

## `runtime/pacing/vk_lowlat.c`

Low latency without frame generation (`NX2SARIA_LOW_LATENCY`: 0 off, 1 on, 2 on with boost; default 0). In a run that
starts with it on, a change takes effect at once; a run that starts with it off makes its device without the drivers'
extensions (`runtime/vk/vk_device.c` `create_device()`), so turning it on then takes a restart. The file's sections:
NVIDIA, AMD, both, the swapchain and the state.

A frame's delay from the pad to the screen has two parts the runtime can shorten. The first is the time the frame waits
after it is drawn: the frame limit used to hold a finished picture back until its slot (`runtime/vk/vk_backend.c`
`present_main()`), so the pad was read up to a whole period before the picture left. The second is the queue in front
of the GPU: when the GPU is the slower side, the game draws frames faster than they are shown and each waits its turn.
Both go when the wait moves to the frame's start, before the pad is read: the driver knows how long the GPU took and
when the last picture left, and holds the next frame's start until its picture can go out as soon as it is drawn.

Two drivers offer that, and the device enables the one it lists (`runtime/vk/vk_device.c` `create_device()`):

- NVIDIA, `VK_NV_low_latency2`. The window's swapchain is made with `VkSwapchainLatencyCreateInfoNV`
  (`runtime/gfx/gfx_present.c` `create_swapchain()`) and the sleep mode is set on it: low latency, boost for the value
  2, and the frame limit as the minimum interval. Each frame asks the driver for its sleep (`vkLatencySleepNV`, which
  signals a timeline semaphore when the frame may start) and marks its stages with the frame's number: simulation
  start, input sample, simulation end, render submit start and end, present start and end. The frame's queue
  submissions and its present carry the same number (`VkLatencySubmissionPresentIdNV`, `VkPresentIdKHR`).
- AMD, `VK_AMD_anti_lag`. `vkAntiLagUpdateAMD` is called with the input stage where the frame starts (the driver waits
  inside the call) and with the present stage before the frame's present, both with the frame's number and the frame
  limit in frames a second.

Any other driver has neither: the setting does nothing there and the menu's row says so.

The extension's revisions. A driver says which revision of `VK_NV_low_latency2` it implements, and the revisions do not
all take the same calls, so a call is made only in the form the driver's revision has:

- Revision 1 (Vulkan headers 1.3.266 to 1.3.270): `vkGetLatencyTimingsNV` takes four arguments, the device, the
  swapchain, a pointer to the count and the info structure, and that structure has no count: its `pTimings` is at
  offset 0x10.
- Revision 2 (headers 1.3.271 on): three arguments, with `timingCount` inside the structure at 0x10 and `pTimings`
  after it at 0x18. The extension's history calls it a breaking change.
- Revision 3 (2026): no call changed. Only from this revision on does a driver follow the frame number chained to a
  queue submission (`VkLatencySubmissionPresentIdNV`, `vk_lowlat_submit_tag()`); an older one ignores it.

`vkSetLatencySleepModeNV`, `vkLatencySleepNV` and `vkSetLatencyMarkerNV` are the same in all three, and so are the
structures they take and the swapchain's `VkSwapchainLatencyCreateInfoNV`. `vkQueueNotifyOutOfBandNV`, which took its
structure by value in the header 1.3.266 alone, is not called.

The runtime has the three-argument form only. A revision 1 driver given three arguments takes the third, the
structure, for the count's pointer and reads a fourth nobody passed as the structure: 0.26.0-rc.1 ended that way on the
600th present, the first statistics line, with an access violation inside the driver reading address 0x10 (a null
fourth argument plus `pTimings`' offset). So below `TIMINGS_REV` the entry point is not even looked up, the first
`lowlat:` line says the timings are left out, the first statistics line says once that it has no report, and the rest
of the path (the sleep, the markers, the frame limit) runs as on any revision. The four-argument form is not written:
no machine here has a driver that takes it, and it would bring back only the report in a log line. NVIDIA's Windows
drivers list revision 1 up to 546.31 and revision 2 from 546.32 (December 2023), as the reports in the public Vulkan
hardware database show; 546.18 is among the first.

Threads. The game's frame is made on guest threads and shown by the present thread, so the calls are split:

- guest thread, where the frame starts (`vk_lowlat_frame_start()`): the first pad read (`runtime/hle/ov_input.c`
  `npad_states()`) or window image acquire (`nvnWindowAcquireTexture` in `runtime/nvn/nvn_api_obj.c`) after a present,
  whichever the game makes first. The sleep, then the simulation-start marker; the pad read sets the input marker. The
  thread gives its virtual core up for the sleep, as around every host wait.
- guest thread, the frame's first `nvnQueueSubmitCommands` (`vk_lowlat_submit_start()`): simulation end, render submit
  start. The game's own threads cannot be told apart, so the simulation ends where the first commands are handed over.
- the thread that replays (the guest's, or the replay thread with `NX2SARIA_REPLAY_ASYNC`), at the frame's present
  (`vk_lowlat_submitted()`): render submit end. Its queue submissions are tagged in `runtime/vk/vk_sync.c`
  `queue_submit()`.
- present thread (`runtime/gfx/gfx_present.c` `flip_locked()`): the present stage or the present markers around the
  host present.

The frame's number is the count of the game's presents, as `runtime/vk/vk_backend.c` numbers the frame it hands to the
present thread (`PresentJob.frame`): a guest thread starts frame n while n - 1 presents were made, and the replay and
the present thread get n with the frame's work. A frame the present leaves out (`runtime/vk/vk_backend.c`
`frame_hold()`) simply has no present stage.

One limiter. With the mode on, the driver's sleep holds the frames to the frame limit and the runtime's own pacer holds
nothing: `nvnQueuePresentTexture` asks `vk_lowlat_pace()`, and on 1 gives its present the time now and frees the window
image at once. Both together would wait twice, and the pacer's wait is the one after the picture is drawn, which is the
delay the mode is there to remove. The pacer keeps the limit where the driver cannot: while a movie plays
(`runtime/nvn/nvn_api_obj.c` `pace()` keeps the movie's slots through a late frame, which a driver's limiter does not),
while no host present goes out (a minimized window: the driver would have nothing to time its sleep by, so the sleep is
left out too), and before the first host present. RivaTuner Statistics Server is kept out as with frame generation
(`runtime/pacing/frame_rate.c` `frame_step_cap_ns()`): it would hold the finished picture as the pacer did.
`NX2SARIA_PACE_SMOOTH` and `NX2SARIA_FRAME_HOLD_MS` act on the present thread as before.

Frame generation. DLSS Frame Generation through the vendor's interposer brings its own low latency
(`runtime/vk/vk_dlssg.c`, `NX2SARIA_REFLEX`): a device its proxies make does not get these extensions and this path
stays off for the run. AMD's frame generation puts its own swapchain object in the window swapchain's place, which
NVIDIA's calls cannot be given, so NVIDIA's path is off while it is there and on again with the plain swapchain; AMD's
path has no swapchain in its calls and runs beside it. NVIDIA's frame generation with the runtime's own presenter
(`runtime/gfx/gfx_present.c` `ng_run()`) presents several times a rendered frame, which the driver would take for
frames: off while it presents. Each is decided where the state changes: at device creation, at each swapchain creation,
and at the present that starts or stops the presenter.

Constants:

- `RING` 64: frames between a frame's start and its present are a handful at most.
- `HOLD_IDLE_NS` 200 ms: no host present for this long, the driver's sleep is left out.
- `SLEEP_MAX_NS` 500 ms: the longest wait for the driver to end a sleep.
- `SLEEP_FAILS` 3: that many waits in a row running out, and the mode is given up for the run.
- `REPORTS` 16: frames of the driver's report read for a statistics line.
- `TIMINGS_REV` 2: the first revision of `VK_NV_low_latency2` whose `vkGetLatencyTimingsNV` has the form called here.
- `PACE_WINDOW` 60: frames over which the driver's hold of the frame limit is checked.

State, `L`:

- `kind`: `LL_*`, fixed by `vk_lowlat_init()`. `why`: `vk_lowlat_status()` while `kind` is `LL_NONE`. `present_ids`:
  the presentId feature is on, the present carries the frame's number. `rev`: NVIDIA, the extension's revision as the
  driver lists it. `timings` is NULL below `TIMINGS_REV`. `no_report_said`: the present thread's, the statistics line
  said that there is no report.
- `sem`: NVIDIA, the timeline the driver signals to end a sleep.
- `m`: NVIDIA's calls name the swapchain and nothing says they may run at once on two threads: all of them, and the
  swapchain's replacement, hold `m`. None of them waits (the sleep's wait is on the semaphore, outside `m`). AMD's stage
  calls hold it shared and its switch-off exclusively (`amd_stage()`).
- Under `m`: `sc` the swapchain made with the latency mode, or none; `mode` (written under `m`, read as an atomic) the
  sleep mode as last set on `sc` (`mode_want()`), or `MODE_UNSET`; `sleep_value` the semaphore value of the newest
  sleep.
- Atomics: `attached` `sc` is set; `on` the direct path is in effect; `said` `on` as last logged; `failed` the driver
  refused the mode or did not end its sleeps, off for the run; `fg_block` `vk_lowlat_fg_presents()`; `pace_bad` the
  driver's sleep did not hold the frame limit, the pacer holds it (`vk_lowlat_pace()`); `cfg_read`, `level`,
  `cfg_seen` the setting as last parsed (`level_now()`); `interval_us` the period the driver holds frames to, 0 while
  the pacer does (`vk_lowlat_pace()`); `fails` sleeps in a row whose wait ran out; `game` the game's presents so far;
  `started` the newest frame that started; `input_id` the newest frame whose pad read was marked; `submit_id` the
  newest frame whose first submission was marked; `cur` the frame of the present thread's next blit; `hosted_ns`
  `kern_now_ns` after the newest host present of a frame this path followed; `ring[n % RING] == n` frame n started with
  the path on, so its later stages are marked; `staged`, AMD, the same: frame n's input stage was called, so its
  present stage is too.
- `n_sleep`, `sleep_ns`, `n_input`, `n_unheld`, `n_timeout`: statistics since the last line (`vk_lowlat_present_end()`).

### `level_now()`

`NX2SARIA_LOW_LATENCY` as a number, parsed again when a setting changed.

### `mode_want()`

The sleep mode in one word: bits 0-1 the level (0 off, 1 low latency, 2 with boost), the rest the minimum interval in
microseconds. 0 while the setting is off or the runtime's frame generation presenter presents.

### `nv_mode_locked()`

`m` held, `sc` set. `vkSetLatencySleepModeNV`: `sc` is a live swapchain of dev (it is replaced only under `m`), the
structure has its type; the call returns at once. A mode the driver refused is taken as off: nothing sleeps on it.

### `nv_sleep()`

The driver's sleep for the frame about to start: `vkLatencySleepNV` returns at once and signals the timeline semaphore
with the value given when the frame may start. Each sleep takes the next value, so a wait that ran out leaves a value
behind that a later signal passes. 1 when the driver ended the sleep. `VK_NOT_READY` here means the swapchain went, or
the mode was switched off, since the frame started.

### `amd_stage()`

`vkAntiLagUpdateAMD` with a stage of frame id: the antiLag feature is enabled (`runtime/vk/vk_device.c`
`create_device()` keeps `has_anti_lag` only with it), both structures have their types, the mode and the stage are
values of their enums. The stage calls hold `m` shared, and make none once the path is off: the input stage waits
inside the call at a frame's start while the present thread makes the present stage of the frame before, which must not
wait for it (the command has no parameter the caller has to synchronize, so the two may run at once). The switch-off
(`refresh()`) holds `m` exclusively: it waits for a stage call in flight, so a stage call with the mode on cannot come
after it and leave the driver on.

### `refresh()`

The guest thread at a frame's start: brings the driver's mode in step with the setting, the frame limit and what
presents the frames. 1 while the direct path is in effect. AMD: switched off, the driver is told once, without a stage,
after any stage call in flight (`amd_stage()`), and not when another thread switched it on again meanwhile. NVIDIA: no
swapchain with the mode, `on` is 0 (`vk_lowlat_swapchain()`).

### `frame_begin()`

`kern_core_away()` gives 0 where the caller gave the core up already (the acquire). The wait is counted with
`perf_end_any()`: the acquiring thread may not be the one that presents.

### `vk_lowlat_frame_start()`

Once a frame, whichever guest thread comes first: the game reads several pads and may acquire again.

### `vk_lowlat_pace()`

The pacer gives the frame limit up only while the driver keeps it, and that is checked: of every `PACE_WINDOW` frames
the driver was to hold, at most half may come sooner than three quarters of the period after the frame before. More
means nothing holds the game (a driver that ignores the interval, or a game whose pad read and image acquire are not on
the thread that makes the frame, so the sleep held another thread), and a game that runs free can outrun what its own
code survives (`runtime/pacing/frame_rate.c` `LIMIT_FREE_MAX_HZ`). The pacer then holds the limit for the rest of the
run and the driver's sleep goes on without an interval. `last_ns`, `last_period`: the presenting guest thread's, the
newest present the driver was to hold.

### `tag_fill()`

`VkLatencySubmissionPresentIdNV` extends `VkSubmitInfo2` where the extension is enabled, which kind `LL_NV` says.

### `vk_lowlat_submit_tag()`

The frame being replayed is the one after the newest present the replaying thread counted (`runtime/vk/vk_backend.c`:
the guest thread in `vk_be_present()`, the replay thread at its present item).

### `vk_lowlat_present_begin()`

The caller holds the present lock, under which the swapchain is replaced too: `sc`, when set, is the swapchain this
present goes to. `VkPresentIdKHR`: one swapchain as in the present, the presentId feature on (`present_ids`), and the
frame numbers of a swapchain's presents only grow (a frame is presented once, in order; a new swapchain starts over).

### `nv_report()`

The driver's own account of the newest frames (`vkGetLatencyTimingsNV`; every report has its type, the count says how
many the array holds and comes back as how many were written), averaged into text for the statistics line. A frame the
driver has not finished timing is left out. The report array is the present thread's.

### `stats()`

One line after the first 600 frames the path followed and every 3600 after that (every 600 with `NX2SARIA_TRACE=perf`):
how many frame starts the driver held and for how long, where they were held, and the driver's report. Without
`timings` (a revision below `TIMINGS_REV`) the first line says that there is no report and why, and the later ones
say nothing about it. The present thread's.

### `vk_lowlat_swapchain()`

A new swapchain has no mode yet; none has none. A swapchain was made and it is not one for this path, with the setting
on: a frame generation's (a resize in between says nothing: the next swapchain is there a moment later).

### `vk_lowlat_fg_presents()`

Taken up at the next frame's start (`refresh()`).

### `vk_lowlat_init()`

A probe makes the device more than once, so the state starts over each time. The sleep's semaphore has to be a timeline
semaphore (the timelineSemaphore feature is one the device needs). `vkGetLatencyTimingsNV` is looked up, and has to be
there, from `TIMINGS_REV` on only.

`tests/lowlat_revision_check.c` runs the whole file against a driver of each revision made of stubs
([tests.md](tests.md)).

## `runtime/pacing/dlssg_pace.h`

The frame limit Reflex holds with DLSS Frame Generation (`runtime/vk/vk_dlssg.c` `vk_dlssg_pace()`), as a function of
what the presenting guest thread sees and nothing else, so `tests/dlssg_pace_check.c` drives it without a GPU. Why
Reflex holds the limit, and what the DLL does with it: [architecture.md](../architecture.md) ("Low latency"),
[dlssg.md](dlssg.md) (`runtime/dlssg/nx2_sl.cpp` `nx2sl_reflex2()`).

The pacer's period counts the frames the game draws (`runtime/pacing/frame_rate.c` `frame_step_cap_ns()`). Reflex holds
it where the frame starts, before the pad is read, and the pacer then keeps only its ceiling
(`runtime/pacing/frame_rate.c` `frame_step_ceiling_ns()`); the pacer keeps the whole period while a movie plays (the
caller passes 0), before the first host present and while none went out for `DLSSG_HOLD_IDLE_NS` (Reflex's sleep is
left out then, `runtime/vk/vk_dlssg.c` `vk_dlssg_frame_start()`), while the limit Reflex should hold is not the one
Streamline last took, and for the rest of the run once the frames show that Reflex does not hold it.

The frames are checked over windows of `DLSSG_PACE_WINDOW` presents Reflex held to one period and one multiplier. Over
half of them sooner than three quarters of the period: nothing holds the game, and a game that runs free can outrun
what its own code survives, so the pacer takes the limit for the run. Over half of them about the multiplier times the
period apart (within half a period): the driver counted the frames on screen, the generated ones too, so Reflex is
given the period over the multiplier from then on; the same again (a GPU that only managed that rate) gives the limit
to the pacer for the run. A frame a load or a hitch held counts as neither.

Constants and types:

- `DLSSG_HOLD_IDLE_NS` 200 ms: no host present for this long, Reflex's sleep is left out.
- `DLSSG_PACE_WINDOW` 60: presents over which Reflex's hold of the limit is checked.
- `DLSSG_PACE_*`: what the last window found.
- `DlssgPace`: `last_ns` the newest present whose frame Reflex held, 0 when the pacer held it; `last_period` its
  period; `last_mult` its multiplier; `window`, `fast`, `slow` presents counted in the window, those too soon, those
  about the multiplier periods apart; `shown` Reflex counts the frames on screen, it is given the period over the
  multiplier; `bad` Reflex did not hold the limit, the pacer holds it for the rest of the run.

### `dlssg_pace_step()`

now: `kern_now_ns` at the game's present. period_ns: the pacer's period (0: a movie, or no limit). mult: frames on
screen per drawn frame now (`vk_dlssg_multiplier`). hosted_ns: `kern_now_ns` after the newest host present, 0 before
the first. allowed: Reflex may hold the limit (the setting on, a DLL with `nx2sl_reflex2` that Streamline did not
refuse). applied_us: the limit Streamline last took. `*want_us`: the limit Reflex should hold, 0 none. `*found`: what a
window that ended here found (`DLSSG_PACE_*`). Returns 1 when Reflex holds this frame's limit: the pacer keeps only its
ceiling.

A window counts only presents Reflex held to one period and multiplier. A host present stamped after now went out
while this thread read the clock: as recent as one can be. A period too long for Reflex (over 0x7fffffff
microseconds) is no limit Reflex could be given: the pacer keeps it.

## `runtime/pacing/vk_present_held.h`

A present that waits for held work (`runtime/vk/vk_backend.c`, "a present that waits for held work"): its state and
who may put it out. Header only, so that `tests/present_held_check.c` checks it without a device.

With the replay on the submitting thread the game presents from one guest thread while others submit. A present held
for event-ordered work goes out after the submission that replays the last of that work, on whichever thread made it,
at the next present, or after `PH_MAX` submissions; only the presenting thread arms one. The lock keeps the held job
whole and lets one thread put it out once. It also keeps the synchronous presents one at a time and in the game's
order: the present thread takes one picture in waiting (`runtime/vk/vk_backend.c` `present_image()`), so a held present
another thread puts out has to reach it before the next present does. A submitting thread only tries the lock: when
another thread holds it, that thread is putting a present out or arming one, and the submitting thread goes on without
waiting; its next submission, the next present or `PH_MAX` checks again. The presenting thread waits for the lock
(`ph_lock()`, counted) only while another thread puts a held present out: at most that one `present_image()`.

Types:

- `PhJob`: `t` the game's texture (`tex_t`); `since` the replay's submission count when it began to wait; `tid` the
  thread that presented it (for the counts of who put it out).
- `PH_AFTER`, `PH_CAPPED`, `PH_NEXT`: why a held present went out, work replayed, `PH_MAX`, the next present.
  `PH_NONE` (-1): nothing goes out; for the present callback, one that did not wait.
- `PhState`: `lock` guards `job`, `on`, `waited` and `out`, and the order of the synchronous presents; `on` a present
  waits, changed under the lock, read without it as a hint (atomic); `waited` presents that waited; `out` held presents
  put out, by why. `lock_waits`, `lock_wait_ns`, `lock_wait_max`: the presenting thread's waits for the lock, how many,
  their sum and the longest, in ns, written under the lock and read anywhere (atomics). `on_wait` (may be NULL) is told
  when such a wait began (`plat_now_ns`), once it has ended.
- `PhPresent`: puts a present out (`runtime/vk/vk_backend.c` `present_image()`), with the lock held. why: `PH_AFTER`,
  `PH_CAPPED` or `PH_NEXT` for a held one, `PH_NONE` for a present that did not wait.
- `PhWhy`: whether a held present goes out after a submission, asked with the lock held: `PH_AFTER`, `PH_CAPPED` or
  `PH_NONE`.

### `ph_lock()`

The presenting thread's lock: at once when it is free, else a counted wait.

### `ph_flush_try()`

A submitting thread, before or after its submission. Never waits; 1 when it put the held present out.

### `ph_flush_next()`

The presenting thread at its next present: a held present goes out now. Waits while another thread puts it out.

### `ph_present()`

The presenting thread: j waits for held work (`hold_now(arg)`, asked with the lock held) or goes out now, in either
case behind a held present another thread is putting out. Only a presenting thread arms: a second presenting thread's
held present goes first.

## `runtime/pacing/dynres.c`

Dynamic resolution from the GPU's own timestamps: the scene's resolution follows how long the GPU takes per frame, so a
heavy view renders fewer pixels and keeps the frame time, and a light one goes back to the player's choice. The HUD, the
menus and the text are drawn at the picture's size, so only the 3D scene changes. The feature, its settings, its menu
rows, its log lines and its measurements: [presets.md](../presets.md#dynamic-resolution).

`NX2SARIA_DYN_RES=1` turns it on (off by default: it changes the picture's sharpness; the quality presets never turn it
on). It steps one of two things, as `runtime/patches/widescreen.c` `wide_dynres_base()` says:

- while DLSS or FSR 3.1 runs (`runtime/patches/widescreen.c` `upscaler_runs()`), the upscaler's mode: quality,
  balanced, performance and ultra performance, 2/3, 0.58, 1/2 and 1/3 of the output on each axis. `NX2SARIA_DLSS` is
  the sharpest step it uses; the lowest is performance, or ultra performance when that is the player's own mode. It
  acts only while `NX2SARIA_DLSS` is one of those upscaling modes. The step goes to `runtime/vk/vk_dlaa.c`
  `dlss_mode()` and to `runtime/patches/widescreen.c` `mode_share()` as an override (`dynres_override()`), which apply
  it as they apply the menu's choice.
- without one (the stand-in's edge-adaptive or linear filter enlarges the scene), the scene's share of the picture: from
  the share in effect (`NX2SARIA_SCENE_SHARE` as the player or the quality preset sets it, 0.75 unset, within the heap
  the start made) down to a floor, each step `DYN_SHARE_STEP` (0.87) of the one above on each axis, about three
  quarters of the pixels; a step that would land within 4 % of the floor is the floor. The floor is the lower of that
  share and `NX2SARIA_DYN_RES_MIN` (0.33 to 1, default 0.5), so it never starts below the floor and never goes above
  the player's share; a share at or under `dyn_res_min` leaves it no room, and the log says so once. The step goes to
  `runtime/patches/widescreen.c` `scene_share()` as a share (`dynres_share()`). A changed `scene_share` or
  `dyn_res_min` starts again at the new share.

The player's settings themselves never change.

While it runs it asks for GPU timestamps (`runtime/diag/perf.c` `perf_gpu_timing_want()`; `runtime/vk/vk_sync.c` times
every submission part) and reads their running total (`runtime/vk/vk_sync.c` `vk_gpu_busy_all()`) once per presented
frame. It averages the busy time and moves one step at a time: down when the average is over the target by 8 %
(`DYN_DOWN`), up when it is under 75 % of it (`DYN_UP`). The gap between the two is wider than one step: a step up
takes about 1 / 0.757 more pixels, so a GPU under 75 % of the target goes to about 99 % of it at the step above, under
the 108 % that would send it down again.

A size that changes every few frames is worse to look at than a steady one, so steps are held, by the clock:

- each step for `NX2SARIA_DYN_RES_STEP` 60ths of a second (default 45), never less than `DYN_HOLD_MIN_NS`, 1 s;
- after a step down, no step up for `DYN_UP_WAIT_NS`, 3 s, so a GPU time at the edge of the target does not climb back
  the moment it dips;
- a step up that a step down undoes within `DYN_UP_PROVEN_NS` (5 s) doubles that wait, up to `DYN_UP_WAIT_MAX_NS`
  (48 s): where one step's cost is larger than the gap between the two thresholds, the steps would otherwise swing
  between the same two sizes every four seconds. A step up held 5 s brings the wait back to 3 s.

The target is `NX2SARIA_DYN_RES_TARGET` milliseconds when that is set (1 to 100), exactly. Unset (0) it is 90 % of the
frame time of the limit the game draws at (`runtime/pacing/frame_rate.c` `frame_step_game_hz()`: 1000 / the limit, the
monitor's rate for display, the drawn frames with frame generation), or 16.7 ms, a 60 Hz frame, while that rate is
unknown or unlimited: 15.0 ms at 60 fps, 22.5 ms at 40, 30.0 ms at 30. The 10 % margin (`DYN_MARGIN`) moves the
thresholds below the frame time: a step down comes at 108 % of the target, 97 % of the frame time, before frames are
lost, and a step up under 75 % of it, 67.5 % of the frame time. A fixed 16.7 ms target let the GPU take up to 18 ms a
frame at full size under a 120 or 144 fps limit, and a hold of 45 presented frames lasted 0.19 s at 240 fps.

Constants:

- `DYN_TARGET_MS` 16.7: the frame time while the game's rate is unknown or unlimited, a 60 Hz frame.
- `DYN_MARGIN` 0.9: the automatic target's share of the frame time, 10 % under it.
- `DYN_SHARES` 12: the most scene-share steps; 1 down to 0.33 takes 9.
- `DYN_SHARE_STEP` 0.87: a scene-share step's share of the one above, on each axis (0.757 of the pixels).
- `DYN_SHARE_SNAP` 1.04: a step that would land within 4 % above the floor is the floor.
- `DYN_MIN_LO` 0.33, `DYN_MIN_HI` 1.0: `NX2SARIA_DYN_RES_MIN`'s range.
- `DYN_DOWN` 1.08, `DYN_UP` 0.75: a step down while the GPU's average is over the target by 8 %, a step up while it is
  under 75 % of the target.
- `DYN_HOLD_MIN_NS` 1 s: the least time between two steps, whatever `dyn_res_step` says.
- `DYN_UP_WAIT_NS` 3 s: the least time from a step down to a step up. `DYN_UP_WAIT_MAX_NS` 48 s: the most that wait
  doubles to. `DYN_UP_PROVEN_NS` 5 s: a step up undone sooner doubles the wait; one held this long resets it.
- `DYN_STEP_LINES` 200: step lines a run, then one line that says the rest are not logged. `DYN_STATE_LINES` 100: on,
  off and why-not lines a run.

State, `D`: `want` `NX2SARIA_DYN_RES`; `on` dynamic resolution is acting now; `plain` the path it reads now, 1 the scene
share (no upscaler runs), 0 the upscaler's mode; `base`, `low` the sharpest step used and the lowest (`DYN_MODE`
indexes, or `share[]` indexes: 0 and the floor); `level` the step in use; `share` the scene-share path's steps,
`share[0]` the player's share, `share[low]` the floor; `from` `wide_dynres_base` as last read, the share stepped from,
-1 the upscaler's mode; `min` `NX2SARIA_DYN_RES_MIN` as last read, within its range; `held_from` `plat_now_ns` when the
step in use was taken; `hold_ns` how long to hold a step; `up_from` no step up before this `plat_now_ns` (a step down
was taken `DYN_UP_WAIT_NS` or more before); `up_wait_ns` the wait after a step down before a step up, `DYN_UP_WAIT_NS`
doubled by undone steps up; `upped_at` when the last step up was taken, until it is held `DYN_UP_PROVEN_NS`, 0 for none;
`avg_ns` moving average of the GPU's busy time per frame; `target_ms` `NX2SARIA_DYN_RES_TARGET`, under 1 the frame time
of the game's rate; `target_ns` the target in use; `logged_ns` the target the log last named (the start line's, or
`dynres_frame()`'s last line); `last_busy` `vk_gpu_busy_all` at the previous presented frame; `cfg_seen` `cfg_version`
the settings were read at; `up` the upscaler's name for the log (`upscaler_name()`); `why` the last line that said why
it does not act, so each reason is said once.

`tests/dynres_check.py` builds the file from `#define DYN_LEVELS` on, with stand-ins for the settings, the GPU's busy
time, the clock and the game's rate under the limit.

### `share_publish()`, `dynres_share()`

The scene share in use, for `runtime/patches/widescreen.c` `scene_share()`, which runs on the game's thread in its
present while `dynres_frame()` runs on the submitting or presenting thread: one value, the double's bits
(`share_bits`), stored whole after a step or a (re)start, so a reader never sees a share half-way through a restart. 0
while not stepping the scene share.

### `share_steps()`

The scene-share path's steps from the share in effect down to the floor (the lower of it and `dyn_res_min`) into out;
the floor's index, 0 when there is no room.

### `upscaler_name()`

The upscaler whose mode the steps set, as the log names it: "DLSS", unless `runtime/vk/vk_dlaa.c` `vk_dlaa_fsr3()`
says AMD's runs, then "FSR 4" while `runtime/vk/vk_ffx.cpp` `vk_ffx_fsr4()` says FSR 4 does the upscaling, else
"FSR 3.1". All read the `dlss` setting's modes, so the step's value is the same word; only the name in front tells
them apart. Read into `D.up` with the settings (`dynres_read()`) and at every frame of the mode path
(`dynres_frame()`), so a FSR 4 that falls back to FSR 3.1 is named as it runs; the "off" line names the one that ran.

### `step_value()`, `step_name()`

A step as the log names it: its value alone ("0.75", "quality"), or with what it is ("scene share 0.75", "DLSS
quality", "FSR 3.1 quality").

### `target_now_ns()`

The target in ns: the setting's, else `DYN_MARGIN` of the frame time of the rate the game draws at under the limit.

### `target_why()`

Where the target comes from, for the log: "as set", else the frame time it is `DYN_MARGIN` of, with digits decimals.

### `dynres_read()`

The settings, again whenever one changed (the menu switches them live) or the share in effect or the path moved. A run
that goes on with other settings logs its new start line instead of the "off" line; a (re)start begins at the player's
step; why it does not act is said once for each reason.

### `dynres_frame()`

Once per presented frame, before `runtime/vk/vk_dlaa.c` reads the mode. The limit, the monitor's rate and frame
generation move the target without a `dyn_res` setting changing, so it is read every frame. A move of more than 1 % from
the target the log last named is logged, whether it came from the frame, from the menu (`dynres_read()` takes a changed
limit or `dyn_res_target` already) or since the start line: a measured refresh rate wanders a little. At most 20 lines a
run. A frame with no timing yet, or a stall over 1 s that says nothing about the scene, is not averaged. A step down
holds off the next step up, longer when it undoes a step up just taken.
