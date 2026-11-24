# runtime (top level)

The files directly under `runtime/`. This page is started: it holds the notes of the code added since the comments
rule. The rest of the top level keeps its comments until the area is extracted ([README.md](README.md#areas)).

## `runtime/vram_warn.h`

The notice the player gets on screen when the chosen graphics settings do not fit the card's video memory. The game
keeps running then, but the driver moves what does not fit to system memory and the frame rate falls to a half or a
third, and before this notice only the log said why. Two reports it comes from, both from an RTX 3050 with 6 GB in a
laptop:

- DLAA on a 2560x1080 picture with FSR 3 frame generation: `mem: mirror of 0.1 MiB made over the VRAM budget (5406 of
  5249 MiB in use, ...)`, 512 such mirrors, and `frametime: 60 s: 1425 frames in 60.0 s, mean 23.7 fps, 1% low 12.4
  fps`.
- Game resolution 1440 with DLAA: `mem_budget: video memory critical, 0.0% free`, and 17 fps.

With a DLSS mode in place of DLAA the same machine ran well.

The header is the arithmetic alone, which `tests/vram_warn_check.c` runs: when a notice comes due, sample by sample,
and which settings it names. `runtime/vram_warn.c` holds the state and writes the log; `runtime/ui/menu.cpp`
`vram_warn_tick()` puts the notice on screen.

There is one source for the figures: `runtime/mem_budget.c` samples the driver's budget and this process's use
(`runtime/vk/vk_memory.c` `vk_mem_vram()`) every `MEM_BUDGET_PERIOD_MS` and turns them into a level
(`runtime/mem_budget.h` `mem_level_next()`). The notice goes by that level and by nothing else: critical is 8% of the
budget free or less, or less than 256 MiB, and is left only above 13% free. Use above the budget is critical too (no
room is 0% free), so both reports above are covered by the one condition.

- `VRAM_WARN_WINDOW`, 30 samples (a minute), and `VRAM_WARN_HOLD`, 10 of them (20 s): a notice comes due when the
  level was critical in 10 of the last 30 samples. Not 20 s in a row, because full video memory is rarely steady: the
  renderer makes room by itself (idle mirrors and textures are given back, `runtime/vk/vk_memory.c` and
  `runtime/vk/vk_texture.c`), the level goes back to tight, and the memory fills again. A session on the bench's 10 GB
  card with a 3440x1440 picture, FSR Native AA and FSR frame generation entered critical two to four times a minute
  (9060 of 9323 MiB in use, then tight at 8040) at a mean of 23 to 35 fps, with stretches of ten seconds or so
  between longer ones; the two reports above stayed critical throughout. One load that fills the memory for less than
  20 s and is then evicted gives nothing.
- A stretch begins when a notice could come due and is over after a whole window without a critical sample.
- `VRAM_WARN_GAP_MS`, 10 min, and `VRAM_WARN_MAX`, 3: a player who sees the notice and plays on is told again ten
  minutes later while the stretch lasts, and at most three times in a session. A notice counts when the screen showed
  it, not when it came due.
- `VRAM_WARN_SHOW_MS`, 12 s: how long the notice stays up. It is up to four lines, longer than the other notices.
- `VRAM_WARN_WAIT_LINES`, 4: the lines one notice writes about what it waits for.
- `VRAM_WARN_LINES`, 24: the log lines a session writes besides the start line and the notices themselves (a notice
  that waits, a critical stretch that ended, one that got no notice). The 24th says it is the last.

### `vram_warn_step()`

One sample of the level, kept as a bit in `hist`. It gives `VRAM_WARN_DUE` once when the window holds the hold's
count and a notice may still be shown; `due` then stays set until `vram_warn_mark()` or until the stretch is over, so
a notice that could not be shown (the menu was open, a movie played) is shown at the first moment it can, and is
dropped when the memory recovered meanwhile. `VRAM_WARN_SPENT`, once per stretch: it began but the session's notices
were all shown, in an earlier stretch. `VRAM_WARN_OVER`: the stretch ended; a peak that never reached the hold ends
without an event. `critical` counts the stretch's critical samples, for the line of its end.

### `vram_warn_mark()`

The screen showed the notice. It also sets `told`, so the stretch whose notice was the session's last is not then
reported as one that got none.

### `vram_warn_suggest()`

Which settings the notice names, each only when it is in use: Resolution when the picture is larger than the window,
Upscaling when its mode renders the scene at the picture's full size (DLAA, or FSR's Native AA), Frame generation when
it runs. Every one that applies is named, since the game cannot tell which of them the player would rather give up.
With none of the three in use, the Quality preset.

### `vram_warn_wait_step()`

What a due notice waits for, as one of `VRAM_WAIT_*`: 1 when the reason differs from the last one given, so the log
gets a line at each change (a movie, then the title screen, then a loading screen) and none while one reason lasts.
At most `VRAM_WARN_WAIT_LINES` per notice: a player who opens and closes the pause menu while a notice is due does
not fill the log. `runtime/vram_warn.c` starts the count over when the notice is shown or its stretch ends.

## `runtime/vram_fit.h`

Whether the changes waiting in the settings menu's draft may not fit the video memory, for the line the *Apply your
changes?* box shows (`runtime/ui/menu.cpp` `vram_fit_make()`). Arithmetic alone, which `tests/vram_fit_check.c` runs.

It does not estimate MiB. Nothing the game measures tells what a combination that is not running would take: the
budget and the use are totals (`runtime/vk/vk_memory.c` `vk_mem_vram()`), DLSS reports its memory for the mode in use
only, the frame generator's is known once it is made, another game resolution needs a restart, and the texture cache
gives memory back under pressure, so the use now plus a difference would be wrong both ways. So the two rules compare
instead of adding:

- `VRAM_FIT_MEASURED`: the level is tight or critical now, or a notice of full video memory was shown this session,
  and the draft makes something heavier. It names what got heavier and gives the figures; it does not say the total
  grows, since the same draft may make another setting lighter.
- `VRAM_FIT_PRESET`, only when the first does not hold: the card's video memory alone puts it below the High tier
  (`vram_fit_tier()`: under 6 GiB Low, under 8 GiB Medium, the thresholds of `runtime/cfg/preset_pick.h`), and
  something the draft makes heavier is above what that tier's rows give this card for it (`preset_value()` for
  `GAME_RES` and `DLSS`). A hint, not a warning. A setting that was above the preset already and that the draft does
  not touch is not named; frame generation never is, the preset does not set it; a card the preset gives no upscaler
  (no DLSS, no FSR 4) has no mode to compare with; 8 GiB or more never gets it.

Neither blocks Apply.

### `vram_fit_mode()`, `vram_fit_mode_heavier()`

The upscaler's mode as a rank by the size of the scene it renders: off, ultra performance, performance, balanced,
quality, full size (DLAA, FSR Native AA). Between modes a higher rank is heavier. From off, only full size is: it
renders the scene the game rendered plus one more context, while a mode below it renders a smaller scene than before,
and a note that called it heavier would send a player short of memory back from the change that may help.

### `vram_fit_heavier()`

The settings the draft makes heavier: a taller picture, a heavier mode, frame generation turned on. Each axis alone:
a taller picture with a lighter mode names the picture, since there is no measure to weigh one against the other.

## `runtime/vram_warn.c`

The state behind one lock: `vram_warn_sample()` runs on the "mem budget" thread, the other three on the present thread.
`due` is also kept as an atomic so that `vram_warn_due()`, which the present thread calls every frame, takes the lock
only while a notice is due.

The lines, all `vram_warn:`:

- At start, once the device is up: on with the hold, the window, the count and the gap, or off.
- `notice N of 3 shown: video memory critical for S of the last 60 s (U of B MiB in use, F% free); suggested: ...`:
  what the screen showed, with the figures of the last critical sample and the settings named and why (the picture's
  and the window's sizes, the upscaler, the frame generator).
- `notice N of 3 due (...), waits: ...`: due while the settings menu is open, a movie plays, the game is not in play
  yet, its pause menu or a loading screen is up or another notice shows; one line at each change of the reason, at
  most four per notice (`vram_warn_wait_step()`).
- `video memory not critical for 60 s (...): the stretch lasted S s, C s of them critical, at most U of B MiB in use;
  N of 3 notices shown this session`: the end of a stretch.
- `video memory critical for S of the last 60 s again (...): no notice, the session's 3 were shown`.

### `vram_warn_start()`

Reads `NX2SARIA_VRAM_WARNING` once and logs it. Called by `runtime/mem_budget.c` `budget_start()`, so the line comes
with the budget's own first lines and not in a run that never brings a device up.

### `vram_warn_sample()`

The figures of the last critical sample are kept for the notice, and the highest use of the stretch for the line of
its end.

### `vram_warn_shown_count()`

The notices shown this session, for `runtime/vram_fit.h`.

## `runtime/mem_budget.c`

### `mem_budget_vram_mb()`

The use and the budget of the last video memory sample, the figures its level was made from; 0 before the first
sample. Read by the settings menu for the line of the *Apply your changes?* box.

### `sample_vram()`

Also gives the use and the budget in MiB as it measured them, for `runtime/vram_warn.c` `vram_warn_sample()`: the
figures the notice logs are the ones the level was made from, `NX2SARIA_MEM_BUDGET_TEST_VRAM_MB` included.

### `budget_main()`

After each video memory sample the level goes to `vram_warn_sample()`: critical or not, with the sample's figures.

## Forcing the condition on a large card

`NX2SARIA_MEM_BUDGET_TEST_VRAM_MB=<MiB>` (`runtime/mem_budget.h`) makes the level take the video memory for that
small: set below what the game uses (2048 is enough at any resolution), the level is critical from the first sample
and the notice comes at the tenth sample, 18 s later. The key changes the levels only, never the texture cache's own rules, so the game
runs as before and only the notice and its log lines are new. `NX2SARIA_VRAM_BUDGET_MB` and
`NX2SARIA_VRAM_BALLAST_MB` (`runtime/vk/vk_memory.c`) are the tests that make the renderer itself short of memory.
