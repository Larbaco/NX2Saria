# tests

One line per test, for what [building.md](../building.md#checks) does not already say. This page is started: the rest of
`tests/` keeps its comments until the area is extracted ([README.md](README.md#areas)).

## `tests/run.py`

`EXTRA` lists the tests whose names do not end in `_check` or `_test`. `tests/maxwell_warp_emu.py` is not among them:
it is the emulator the tensor checks use, not a test.

## `tests/cheats_tab_check.py`

The Cheats tab of the settings menu: it is reached, it shows only with cheats, and a cheat's switch goes through the
draft. No GPU, no game files.

- Read from the source, comments left out: `runtime/ui/menu.cpp` `layout_menu()` has a `case TAB_CHEATS` that calls
  `tab_cheats(scale)` and counts its tabs with `tabs_shown()`; `TAB_CHEATS` is the enum's last tab and the last name
  of `kTabNames`; `TAB_COUNT` appears nowhere else than the enum, `tabs_shown()` and the size of `kTabNames`.
  `runtime/ui/cheats_tab.cpp` `tab_cheats()` hands the toggle row's change to `opt_change()` and calls none of
  `cheats_set`, `cfg_set`, `cfg_set_test`, `cfg_reset`, `cfg_save`, `opt_save_now` or `rt_patch_switch`.
  `runtime/patches/cheats.c` calls none of `cfg_set`, `cfg_save` or `cfg_reset`, and `cheats_frame()` starts with
  `follow_settings()`. No English menu text says "Play in the launcher"; the two that name it end "Play from the
  launcher builds them in." and "... builds it in.". The counted log lines of `cheats.c` carry their plural `%s`.
  `wr()` counts every way out, `cheats_frame()` runs a cheat only through `run_counted()`, and
  `runtime/gfx/gfx_present.c` `rt_exit_steps()` calls `cheats_exit_log()` before `logq_flush()`.
  `runtime/session_summary.c`, `runtime/session_summary_fmt.c` and `runtime/session_summary.h` do not hold the word
  cheat: what a cheat did stays in the local log.
- Built and run (`tests/cheats_tab_check.cpp`, with `runtime/ui/option_rows.cpp`, `runtime/cfg/cfg.c`,
  `runtime/ui/ui_text.c`, Dear ImGui and the stand-ins of `tests/preset_check_stubs.h`): the enum and `tabs_shown()`
  cut out of `menu.cpp` give the tabs before Cheats with no cheats and all of them with some; over a settings file
  with one cheat on, a cheat's key changed with `opt_change()` is not written before Apply, reads its draft value
  through `opt_get()`, gives one draft line with its label, from and to and no restart mark, leaves the draft when
  set back, is untouched by Discard, and Apply writes both keys, saves `cheat_<hash>` lines to `nx2saria.ini` and
  leaves nothing waiting for *Keep*. The same program checks the counted nouns of the tab
  (`runtime/ui/ui_text.c` `ui_text_count()`) in English, Brazilian Portuguese, Simplified Chinese and Russian for 0,
  1, 2 and 5, Russian also for 11, 21 and 22, and a language without menu text for the English fallback; and a
  cheat's counters and line (`runtime/patches/cheat_stats.h`): frames and stores add up, the first write gives a line
  at once, then none within a minute nor without a new write or refusal, and the wording of the `so far` and
  `this session` lines. Dear ImGui's objects are kept in `build/test/cheats-tab-imgui` between runs.

## `tests/vram_warn_check.py`

Builds and runs `tests/vram_warn_check.c` on `runtime/vram_warn.h` with the levels of `runtime/mem_budget.h`, a sample
every `MEM_BUDGET_PERIOD_MS`. No GPU, no game files.

- 5406 of 5249 MiB in use, the figures of a player's log: due once, at the tenth sample; due stays until the screen
  takes it; after it is shown, not due again inside the gap and due again after it.
- A peak one sample shorter than the hold gives nothing, nor does a second one more than a window later; a third right
  after the second comes due at its first sample, the window holding both.
- Ten minutes of 6 s critical and 8 s tight in turn (9060 and 8040 of 9323 MiB, the figures of a bench session): due
  once, never over.
- Use that hovers between 0% and 10% free stays critical by the level's hysteresis; at 14% free the notice stays due
  for less than a window and the stretch is over after a whole one, the notice then not counted.
- The session's notices come due one gap apart; after the last, never again, and a later stretch is reported as spent
  once, at the hold.
- The settings named for each combination in use.
- What a due notice waits for: a line at each change of the reason, none while it lasts, at most four per notice.

## `tests/vram_fit_check.py`

Builds and runs `tests/vram_fit_check.c` on `runtime/vram_fit.h`. No GPU, no game files.

- The modes' order; what a draft makes heavier, each setting alone; the upscaler turned on below full size is not
  heavier and at full size is; a smaller picture with frame generation turned on names frame generation only.
- The tier the video memory alone gives: 6017 MiB Medium, 4096 Low, none from 8 GiB (7700 MiB rounds to 8) and for an
  unknown card.
- The measured note: tight or critical now, or a notice shown this session, with something heavier, on any card, and
  before the preset's; none for a lighter or unchanged draft.
- The preset's note: DLAA over the Medium tier's quality on a 6 GiB card with DLSS, quality over the Low tier's
  performance at 4 GiB, never frame generation, only what the draft made heavier, nothing for a card the preset gives
  no upscaler, for 8 GiB or for an unknown card.

## `tests/crash_log_offer_test.py`

The crash log offered after an abnormal end (`launcher/crash_offer.py`, `launcher/game_session.py` `end_kind()` and
`end_asked()`, and the window's question). It takes its fixtures from `tests/telemetry_launcher_test.py`: the local
server that records requests, the made-up player's folders and the window. No test reaches the project's server.

- `EndKindTest`, `AskedEndTest`: the names of the ends; SIGTERM, SIGINT and SIGHUP without a fatal line are asked
  ends, and a kill, a fault, an abort and an exit code are not.
- `GameEndedTest`, `PendingTest`: what an end leaves in `logs/crash-offer.json` without any request, the ends that
  leave nothing and the line each writes, and the waiting file dropped when the log changed, the setting was switched
  off or the file is broken.
- `LogPartsTest`: the start up to `patches:` and the last 200 lines; a short log sent whole with no line twice; the
  start cut where the end begins; the caps of both parts; a log far larger than what is read. Its "real width" case
  uses lines of about 230 bytes, the measured mean, to show that 200 of them pass a comment's 24 KB and fit 56 KB.
- `RequestTest`, `AnswerTest`: the body's keys, the report of that session and never an earlier one's or one the
  player said no to, the body trimmed when JSON escapes grow it, and each answer of the server (201, 400, 410, 503, no
  connection) with its log line and the counts. A comment still has its four keys and its 24 KB.
- `AlwaysShortTest`, `ShortNoteTest`: with `NX2SARIA_LOG_SHORT_PATHS=0` in the environment the launcher's own lines
  stay in full, while the crash log's body, a comment's attached lines and the made-up summary's error line are
  written short, the log file's bytes are unchanged and one line counts the lines changed; a log already short adds
  no line, and lines trimmed from the body are not counted.
- `OfferWindowTest`: the question with Send the Log focused, the bytes *See What Is Sent* shows being the bytes sent,
  each way of not sending, the first second that takes no input, *Write a Comment*, the question about session
  reports coming first, the places it never opens, the setting, a 400 that ends the offers of the run, the counts, and
  the text whole inside its panel with the longest sentence. `open_offer()` moves `offer_opened` five seconds back so
  the tests do not wait out the guard.

`tests/launcher_lifecycle_test.py` has the end the window does not watch: a made-up game started as Steam starts it
writes a log and ends with code 72, with SIGTERM or normally, with `NX2SARIA_TELEMETRY_URL` pointing at a server of
the test that must receive nothing.

## `tests/telemetry_launcher_test.py`

`UpdateStartBase` opens the window as the start after an update does (`--continue-update`, without `--selftest`) over a
game folder taken as installed, with a `ContinueUpdateJob` whose `run()` waits for the test and then ends or raises; it
holds no lock (`claims` empty), the update check and the keep-awake call do nothing. `job_ends()` lets the job end and
calls `launcher/ui/app.py` `App.job_finished()` itself instead of waiting for the window's timer.

- `UpdateStartTest`: nothing is asked over the job's screen and the log says the question waits; at the job's end the
  question is open with Send focused and no countdown; Don't Send leaves the home screen with the update's message and
  the same run does not ask again. With reports on and a summary waiting, the job's end sends it, and the notice of
  reports turned on is kept for a later start because the update's message is on screen.
- `FailedUpdateStartTest`: a job that fails leaves its own screen without the question and without an answer recorded;
  Try Again asks when it ends well.
- `test_the_first_start_of_a_version_asks_with_send_focused` reads the countdown line after it has set the deadline
  again itself: the line shows whole seconds left, so a test that takes over a second between opening the question and
  reading the line would see 29 where it expects 30, as a loaded machine once did. Before that it checks that the start
  had set a deadline within the question's 30 seconds.

## `tests/runtime_dirs_check.py`

The runtime's folders by subsystem ([architecture.md](../architecture.md#runtime-folders)) as the build scripts see
them. No compiler, no game files.

- `tools/build_game.py` `RUNTIME_DIRS`, `tools/make_game.py` `RUNTIME_DIRS` and `launcher/gameart.py` `RUNTIME_DIRS`
  are one list, and it names every folder of `runtime/` that holds sources except `platform/`, `shaders/`,
  `third_party/` and `dlssg/` (a source in a folder it leaves out would not be compiled).
- The sources include headers by name with each folder on the include path, so no two headers on that path share a
  name: the one the compiler found first would be taken for both.
- For each OS, Linux and Windows alike whichever this runs on (the scripts' `WIN` switched): `tools/build_game.py` and
  `tools/make_game.py` compile the same files (`build_game.py` also the Windows resource script); with the other OS's
  files (`*_win.*` or `*_sdl.*`, `platform/win32.c` or `platform/linux.c`, `nx2saria.rc`) they are every `.c`, `.cpp`
  and `.rc` under `runtime/` but `dlssg/` (`tools/build_dlssg.py` builds that DLL apart), so no source is left out; no
  two have one file name (`--skip-runtime` and `make_game.py`'s per-install split go by it) and `make_game.py`'s
  object names do not collide.

### `compiled()`

`build_game.py`'s sources and `make_game.py`'s (source, object) pairs as each script picks them on Windows or
elsewhere: both read their module's `WIN` when called, so the check switches it.

## `tests/shipped_notices_check.py`

Checks `THIRD_PARTY_NOTICES.md` against what `tools/make_launcher.py` (the Windows launcher release) and
`tools/make_appimage.py` (the Linux AppImage, with `tools/package_launcher_linux.py`'s zips) package: every third-party
component has a row in the section of its artifact ("Shipped in the Windows launcher release", "Shipped in the Linux
AppImage") or in "Shipped in both releases". It builds and downloads nothing; `tests/run.py` runs it with
`--with-self-test` (the check, then the planted case), and `--self-test` runs the planted case alone.

### `components()`

The components come from the scripts and their pins, not from a copy kept in the test, as `(artifact, what, keys)`: a
component is listed when one row of its artifact's section (or of the shared one) holds every key, without regard to
case.

- Windows: `make_launcher.PY_VERSION` and `PINS` (the embeddable Python, the Tcl/Tk msi), `TCLTK_FILES`, the packages
  of `tools/requirements-launcher.txt`, the AMD libraries `make_launcher.py` copies and the tags
  `.github/workflows/windows-release.yml` fetches them at, `tools/dlss_pin.py` (`nvngx_dlss.dll`),
  `tools/streamline_pin.py` `FILES`, the static zlib and the MinGW runtime of the launcher's own programs, the prebuilt
  runtime library.
- Linux: the Python version `.github/workflows/linux-appimage.yml` installs (python-build-standalone) and the pip it
  bundles, `make_appimage.PACKAGES`, the AppImage runtime release that workflow pins, `tools/vulkan_pin.py`,
  `tools/sdl3_pin.py`, the Linux library of `tools/dlss_pin.py`, the VMA header of `tools/build_game.py` `VMA_FETCH`.
- Both: the shader translator, the button art, and every file under `licenses/`: the release scripts ship that folder,
  and each text in it must be named by a row.

### `check()`

A component without a row fails the check. A row of the three sections that names no component is printed as a
warning: a row can stay for something shipped that the scripts do not name, such as the repository's own sources.

### `python_rows()`

The Python each release ships: every library `tools/python_licences.py` lists for it (`WINDOWS_TEXTS`, `LINUX_TEXTS`)
is named in that Python's row (or the tkinter row), and the row names where the texts are (`licenses/python/`). The
pins there must be for the version `tools/make_launcher.py` `PY_VERSION` and `.github/workflows/linux-appimage.yml`
ship. The build scripts check that each text is there when they run.

### `removed_problems()`

The modules taken out of the AppImage's Python (`tools/make_appimage.py` `PYTHON_REMOVED`): each one of `REMOVED`, with
its reason, is still on that list and still deleted by `tools/make_appimage.py`, and none of them is imported by a `.py`
under `SEARCHED` (`launcher/`, `tools/`, `recomp/`, `tests/`; a grep of the imports that would need it).
Given a Python folder, it also reports a removed module that would be packaged there.

### `self_test()`

A planted package that neither table names must fail the check; a planted Python folder holding `_dbm` must be found by
`tools/make_appimage.py` `removed_modules()`, and must fail the check when the removal list drops it.

## `tests/launcher_lifecycle_test.py`

Runs the actual launcher window on Linux with a virtual display, isolated temporary launcher and game folders, a synthetic game executable and a synthetic cancellable job. No game data is needed. The external control files only drive the test; the launcher uses its normal window, Play, job, lock, CLI and Steam paths.

- Closing or killing the launcher process while its synthetic game runs leaves the game running; the test releases it afterwards and observes its completion.
- Closing the window with a synthetic job requests its cancellation. Killing the process during the job does not request cancellation and leaves the interrupted-job marker. The clean-close test does not assert that the marker was removed: the job runs on a daemon thread and process exit can race its cleanup.
- While the window has an active job and the marker, the mutating CLI command is refused and Steam Play reports the active job without starting the game. With the window idle and no job marker, Steam Play starts the synthetic game even though the window holds the launcher lock.

The tests describe the existing window's behavior, not the proposed worker's EOF or recovery behavior. They leave no game process running after the test.

## `tests/launcher_play_characterization_test.py`

Characterizes the existing Tk Play entry point using temporary game folders and stubbed process and system APIs; it
never opens a real game. It records the observable refusal, rebuild, memory decision, spawn, visibility and log behavior
before the Play decision logic is extracted. Mod preparation errors and an infinite memory estimate are covered.

The memory preflight test checks that an infinite estimate becomes a JSON-safe request and still produces the existing
warning text. The existing Linux Play refusal test simulates the same declined response at the separated memory boundary.

The test's `mods_before_play` stand-in forwards to the `App` method only in the pre-extraction implementation; the new
implementation no longer calls that method. It remains in the fixture so the same characterization file exercises the
old code and the extracted preflight without changing its observed outcomes or logs.

## `tests/launcher_game_end_characterization_test.py`

Exercises the existing Tk `watch_game()` adapter with a synthetic session, a temporary game log and stubbed collaborators. The tests preserve
the order of window restoration, end logging, crash-summary synthesis, game-log annotation, staged-update check,
reports and the home screen. They cover a game that is still running, an absent session, disabled reports, an empty
note, a focus error and both update and reports short-circuits. Existing `launcher_test.py` cases exercise the actual
`GameRun` restart and fatal-end behavior with synthetic files and processes.

## `tests/action_prompt_check.py`

Builds and runs `tests/action_prompt_check.c` on `runtime/patches/action_prompt.c`, the action prompt's diamond of
face buttons laid out by label, on a layout file and archives the test makes itself. No game files. With `--romfs DIR`
it also checks the game's own archive: the folder holds `romfs.bin` and its `index.json`, and the program reads the
layout archive and `Pack/ZsDic.pack.zs` from `romfs.bin` at the offsets and sizes the index gives (`game_args()`);
nothing is written. The program is built with the undefined-behaviour sanitizer (traps), with locals that start as a
pattern rather than whatever the stack held, and with AddressSanitizer where the toolchain has it (not MSYS2's). The
build silences one warning: `runtime/simd_fp.h` `FOK` ors two compares on purpose and clang 18 warns on it.

- The layout file is made by the test from the format, with the values the game's 1.1.1 `DoCommand_00.buil` holds (A
  right, B bottom, X top, Y left), so the reader is checked against a writer of its own; the archives and the zstd
  dictionaries are made by the test too. The made-up file keeps the game's stored `-2.8e-14` zeros, shares A's and B's
  pictures by name as the game does, and leaves A's and L's bubbles and A's Decide null as they are.
- By label: every place, cross, flip and bubble the game's diamond needs and nothing else. The crosses: A lit down
  (picture B), B right (A), X left (A flipped in x), Y up (B flipped in y). X's bubble opens to the left, Y's to the
  right, and the text origin follows. A file already laid out by label changes nothing and says so; back by position
  gives the game's file again, its stored `-2.8e-14` zeros now plain zeros. A label other than 1 counts as by label.
- Refused, with every byte as it was: a file that is not the expected one (before anything is written), a file half
  laid out (A moved, the rest not), a cross with another picture, a missing pane, a missing material, no materials,
  offsets out of the file, a cut file, a wrong magic (never read past the end). Both bubbles of X on one record, their
  values the same in both layouts, are laid out with the record moved once. `P_B_00`'s record laid over
  `P_BaseA_00`'s material, its x on the material's block index (0, as x is 0.0), reads as expected, but moving the dot
  would lose A's cross: refused.
- Random damage, mostly to the header's tables, gives a refusal or a layout, never a crash. Each copy is an allocation
  of its own, so that AddressSanitizer sees a read past its end.
- The archive holds the layout four times as the game's does: every copy changes and nothing else; one copy that is
  not the expected layout leaves all of them as they were; without the layout, refused. Two copies laid out already
  and two not (an archive half laid out by hand): the other two are laid out. More copies than it lays out: refused,
  not the first 16 laid out. Copies that share their bytes are laid out once; copies that share some of them, refused.
  A copy that runs past the archive's end is damage and is refused.
- The zstd file without a dictionary: made once, then nothing to make; a damaged frame is refused. With a dictionary,
  as the game's: the pack of dictionaries holds two (ids 1234 and 77) beside a member that is not one, and the
  archive's frame names 1234; it is made with that dictionary named again, and a frame that names one the pack does
  not hold (4321) is refused.
- `tests/action_prompt_check.c` `game_archive()`, on the game's file: made once by label, with the game's dictionary
  and unpacked size, byte for byte the archive laid out by label, which differs from the game's only inside its four
  copies of the layout; then nothing more to make by label, nothing to make by position on the game's own, and by
  position again on the one by label.

## `tests/animation_loop_check.py`

Checks the 1.1.1 single-frame animation patch table and runs its host (`tests/animation_loop_check.c`) with the two
clock paths and the draw path pointing at test entries. A descriptor outside its own array cannot advance a fallback
slot through the patch, and the fractional pose is the only part the patch changes.

## `tests/appimage_apprun_test.py`

The Linux AppImage's AppRun (`tools/make_appimage.py` `APPRUN`) and its kit's SDL3. Issue #11: Ubuntu 24.04 and Linux
Mint 22 package no SDL3, and the AppRun of 0.18.0 stopped there even for `--check`. The AppImage is not built: AppRun
runs from a folder laid out as the AppImage's, with stand-ins for its Python and for clang. Linux only (bash); no
network, no game files.

- `--check` reads the dump with no compiler or library on the system, and `--make-game` leaves the checks to
  `tools/make_game.py`.
- A build asks the compiler for the libraries' headers, so headers found only through `CPATH` count, and asks for no
  SDL3: a missing one is named with the packages to install, and the apt line is one Ubuntu 24.04 and Mint 22 can run
  (every package in it is a package of Ubuntu 24.04).
- With everything there it says so and starts `tools/make_game.py`; `--events` gives the window its error line, and
  `tools/appimage_gui.py` tells that case by the line's start.
- `tools/make_appimage.py` `carry_sdl3()` puts the pinned SDL3 into the kit: the stand-in files count as the pinned
  ones because their digest is made the pinned digest; a file changed since its fetch is not carried, nor is a source
  that is not complete.

The stand-ins: `TOOLS` are the commands AppRun runs besides the compiler, the Python and bash's own, so a `PATH` of
these only has no compiler. `CLANG` preprocesses `#include <h>` from stdin by looking for `h` in `CPATH`'s folders
only, as a compiler without the headers in its own folders would; any other command line fails, so a probe that no
longer only preprocesses (and would find every header missing on a real system) fails the tests. `PYTHON` notes
every run with its arguments and succeeds.

## `tests/astc_trim_check.py`

Builds and runs `tests/astc_trim_check.c`: `runtime/vk/vk_astc.c` `vk_astc_scratch_trim()`, which every present runs,
against the upload path's lock order. No GPU, no game files, any Python; Windows only, since the platform's mutexes
are SRW locks as in `runtime/platform/win32.c`. The program prints each failed check and exits 1 on any, 2 when two
threads stay stuck.

The upload path takes the upload lock first (`runtime/vk/vk_memory.c` `vk_upload_begin()`: `cmd_lock`) and the
decoder's lock inside it (`vk_astc_record()`: `g_lock` shared for the pipelines, then `scratch_get()`: `g_lock`). The
trim runs at every present and holds `g_lock`, so it may only try `cmd_lock` (`vk_defer_buffer_try()`): waiting for it
there deadlocks with an upload. The recursive `cmd_lock` is `runtime/platform/common.c`'s, linked in; the test's
`vk_defer_buffer()` and `vk_defer_buffer_try()` stand in for those of `vk_memory.c` and take `cmd_lock` as its defer
does. The rest of the runtime that `vk_astc.c` names is stubbed (`STUBS`: each returns 0, none runs on the trim's
path), and the compute shaders' SPIR-V headers, when `tools/build_shaders.py` has not made them, are stand-ins written
to the test's build folder (the runtime folder is searched first). The compiler is `$CC`, else MSYS2 UCRT64's clang,
else clang on `PATH`; `vk_mem_alloc.h` comes from `$VMA_INCLUDE`, else MSYS2 UCRT64's.

A large texture grows the scratch (under `cmd_lock`, as an upload asks for it), then 6 s pass without one, with
`NX2SARIA_ASTC_SCRATCH_KEEP=1`, the default. (a) Another thread holds `cmd_lock`: the trim returns at once and leaves
the scratch. (b) `cmd_lock` free: the next present's trim goes through. (c) The interleaving that deadlocked: the
upload holds `cmd_lock`, the present's trim comes during the staging copy, then the upload asks for the scratch; both
threads end.

## `tests/audio_input_check.py`

Builds and runs `tests/audio_input_check.c`, the input-audio regression checks, without game data or a microphone:
conversion, bounded capture and the polling ABI. On Windows it takes `--toolchain C:/msys64/ucrt64`; on Linux, clang
and SDL3's development files through pkg-config. Capture overflows the 100 ms ring, not system memory, and the oldest
unread samples are the ones dropped. One case is a regression: Purge after a failed or closed Open. The build silences
the warning clang 18 gives on `runtime/simd_fp.h` `FOK`, which ors two compares on purpose.

## `tests/audio_mixer_check.py`

Builds and runs `tests/audio_mixer_check.c`: `runtime/audio/audio_mixer.c` with the rest of the runtime stubbed. No
game files and no audio device: the mixer runs as with `NX2SARIA_AUDIO=none`, the device's requests are made by hand,
and SDL3 is only linked. It is compiled for the default build's target (x86-64-v3: AVX2 and FMA), so the mixing loops
are compiled as they ship.

The renderer of the test has the final mix (4 buffers, the mixer's buffers 0-3), one submix (2 buffers, 4-5) that goes
nowhere, three voices and one splitter with one destination. Each voice plays a wave buffer of one constant sample
(16384: 0.5), so a mix buffer holds the gain the voice went in with, sample by sample. One render pass is counted as
`runtime/hle/audren.c` `render_thread()` counts it, and the voices are not advanced. A new voice is set up as the
game's update leaves one (`update_voice()` with `is_new`: `voice_reset()`, then started); of what the slot's last
voice was mixed with, `voice_reset()` clears only `ramp_pass`, so the test keeps the rest too. The stubbed settings
answer a key without its prefix: the runtime asks for `NX2SARIA_<KEY>`, and `NX2NAVI_<KEY>` is the older name
`runtime/cfg/cfg.c` still answers to. The kernel events of the stubs are numbered from 1 and record which were closed
and signalled and how many readable ends were handed out.

- Volume ramps (`accumulate_ramp()`, `audio_mixer_render()`): a pass goes from the level before to the level now in
  equal steps, sample k on the line `from + k (to - from) / FRAME`, so the first sample is the level before and the
  next pass continues at `to`. The channel's mix volume moves while the voice's does not; a voice that starts is not
  ramped, whatever the slot held before; a voice not mixed in the pass before (paused, then played at another volume)
  is not ramped; nor is one sent to another destination (the submix), nor the first pass after the renderer stopped
  and started (the pass before is not the sound before). When both volumes move, the ramp goes from the product before
  to the product now. A submix into a splitter ramps the destination's volumes like a voice does.
- What the mixer owes the device after a stall (`ring_pull()`, `audio_mixer_owed()`), on a mixer that takes itself to
  have a device: the prefill is a number of render frames, even on every platform. The render thread held up for
  three device periods gives an underrun, then silence until the prefill is back; less than a render frame of silence
  waits for the next gap; a stop forgets what was owed, and the wait for the prefill after the start is not owed; more
  than `AUDIO_CATCHUP_MAX_MS` of silence is not caught up.
- The change events (`change_event_of()`, `audio_channels_live()`) of the device, the two notifications and the
  virtual device manager: one event per kind however often the game asks, a readable end per request, and a channel
  change signals each listed event once and never a closed one. Two first requests at once: the loser closes its own
  event, which is not listed and never signalled. A slot counted whose handle is not stored yet (a kind's first
  request on another thread) is skipped and never signalled as handle 0, which the kernel answers with a fatal error.

## `tests/audio_params_check.py`

Builds and runs `tests/audio_params_check.c`: what the audio renderer does with values the game sends that it cannot
use (`runtime/hle/audren.c` `update_check()`, `request_update()` and `audio_bad()`; `runtime/audio/audio_mixer.c`
`splitters_walk()`, `audio_mixer_routing_check()`, `audio_mixer_mix_layout_check()`, `audio_mixer_update_effects()` and
the render-pass effects), with the rest of the runtime stubbed and the real effect DSPs
(`runtime/audio/audio_effects.c`, `runtime/audio/audio_reverb.c`) linked. No game files and no audio device. The build
has three translation units because both effect files have a static `gain10`, and targets x86-64-v3 as the default
build does. The program takes `-v` (print the log lines) or one of `strict-effect` and `strict-update`.

Every RequestUpdate goes through `renderer_handler()` with a hand-built input (cmd 4; cmd 10 takes the output in C
buffers). A refused update must answer 0x5299, write no output and change no renderer state: the SDK hands that result
to the game, which ignores it, and aborts on any result but 0x5299, 0x5499 and 0x5699. A record, effect or sink the
renderer cannot use is left out of an update that is otherwise applied, or passed through or skipped in the render
pass. Each kind is logged once per renderer. The stub of `rt_fatal()` prints "FATAL:" and exits with 2, so a value
that still ends the process fails the run.

The renderer of the test: 2 voices, 2 effect slots, 1 sink, the final mix (2 buffers, the mixer's 0-1), 1 submix (2
buffers, 2-3), 1 splitter with 2 destinations, 8 mix buffers. Voice 0 plays a wave buffer of one constant sample (0.5)
into final mix buffer 0, so an effect from buffer 0 to buffer 1 shows in buffer 1: 0.5 when it passes the sound
through, 0 when it is skipped. The game's buffers (the input, the output, the auxiliary and capture rings of 0x80-byte
headers and 4096 samples, the circular sink's buffer) come from memory below `RT_ADDRESS_LIMIT` (`low_alloc()`):
`ipc_guest()` and the mixer's ring checks hold the game's addresses to that limit, and the process's own data lies
above it. The platform stubs give one thread and no render thread; the checks render by hand.

1. Valid updates answer 0, and the output's total is the sum of its sections (the SDK aborts on a mismatch).
2. Updates whose framing does not match the renderer are refused whole. Among them: a section of 0x130 bytes with
   records to 0x124, one info record announced with 8 bytes of it in the section, an id that fits while only 4 of its
   12 reserved bytes do.
3. One record the renderer cannot use is left out and the rest applied: channel resource id 12 of 12, an in-use voice
   with id 7 of 2, a new voice whose current wave buffer is 4 of 4, at REV15 a voice filter with a NaN coefficient,
   splitter info naming splitter 3 of 1, a destination record naming destination 9 of 2, at REV15 a destination filter
   with an infinite denominator, a changed-mix record for mix 5 of 2, each next to a valid one. A splitter listing
   destination 5 of 2 is stored as -1, which both render loops skip: destination 0 would put the sound into the
   submix's first buffer (the mixer's 2) and destination 1 goes to final mix buffer 1, and the destination table gets
   a guard entry in front of it, used and into the final mix at full volume, so a loop that reads id -1 puts the sound
   into buffers 0 and 1 a second time. A splitter section whose framing breaks between the walk of
   `audio_mixer_routing_check()` and the apply walk (the game rewriting its input during its own RequestUpdate) is
   tried by calling `audio_mixer_update_routing()` directly, as `request_update()` calls it once the update was taken:
   the splitter info before the fault and the mix section after it are applied, the destination record at the fault is
   not, and the log says the rest of the splitter section is left out, not that the update was refused.
4. Effects and sinks the renderer cannot use are passed through or skipped in each render pass. The table of cases
   says for each what the render does with it (buffer 1 holds buffer 0's 0.5, or stays 0) and where it is found (once
   per update, or in each render pass). The values found at the update mark the effect bad; those found in a pass
   leave it as it is; the same slot with usable values runs again.
5. Valid effects and sinks over 20 passes: nothing left out, every buffer finite.
6. `NX2SARIA_AUDIO_STRICT=1`: such a value ends the process, as most did before 0.11.0. The Python file runs
   `strict-effect` (limiter attack 2) and `strict-update` in processes of their own and expects exit 2 with
   "FATAL: audio:".

## `tests/battle_music_check.py`

Checks the 1.1.1 battle-music patch table and runs its gate and flag hooks (`tests/battle_music_check.c`) with a
stand-in battle manager and replaceable flags. A pulse the guest already saw is not given again; several sound objects
see the same pulse in one frame; phases between two sound updates must not see the old pulse.

## `tests/bench_build_revision_test.py`

`tools/bench.py` identifies the build from its own output's final build-branch record; an older run is not silently
assigned the current checkout's revision.

## `tests/bench_check.py`

`tools/bench.py` reads the lines `runtime/diag/perf.c` writes. The lines of the test are made with the runtime's
format strings, which the test first checks to still be in `perf.c` as written; they parse into frames, segments and
windows, and the summary's numbers come out as worked by hand. No game files.

- The measured segment has 200 frames (frames 101-300): 196 cpu frames of 20 ms, 2 gpu frames of 40 ms (fence 30) and
  2 stall frames of 60 ms (pipeline 30); frames before and after the segment do not count. The slowest 1% of 200
  frames is 2 frames, the two of 60 ms. The p99 of 200 sorted values is at position 197.01, between 40 (index 197) and
  60 (index 198).
- The CPU- or GPU-bound rules at their edges. A window line at frame N gives its GPU time to frames N-59 to N; later
  frames have none. Logs of 0.11.0 and older have no handoff field.
- The spread over runs; MangoHud's log in ms, its stretch matched to the game's from the end of the run.
- The input script: untimed A presses, then the world-clock lines. With `--boot-map` the scene's warp becomes the boot
  map, its other steps stay, and the script presses nothing before the world clock.

## `tests/bind_labels_test.py`

The bind action catalog in all menu languages and its narrow-row fallback. Captured menu images show that the column
fits about 16 Latin or Cyrillic characters and 7 full-width Chinese characters at 1280x720. The earlier L labels were
visibly cut with an ellipsis at these limits, so the test checks the complete text against the budgets taken from the
captures, not the clipping of `fit_text`.

## `tests/boot_map_check.py`

Builds and runs `tests/boot_map_check.c` on `runtime/patches/boot_map_core.h`: the bytes a boot map changes in what
the game reads (the title's and the slot's sequence graphs, the save), on made-up files, and the values of its
settings. No GPU, no game files, any Python. The compiler is `$CC`, else MSYS2 UCRT64's clang, else clang on `PATH`.

- The graphs are a made-up AINB 0x408: one command, nodes with names, children and int inputs. The title's graph gets
  its command Root moved to the Sequential of a Simultaneous and `TitleToMainWithLoadSave.module`, and a graph without
  exactly that is refused (for one, its first child is no Simultaneous). The slot's graph gets both Index inputs made
  the slot.
- A made-up save (the header, the pairs by type, the data area) gets the area, position, heading, dungeon room, age
  and clock. Reads cut at every size from 1 to 64 bytes give the same file; the same file gives the same edits
  whatever the unused bytes of the lists hold, and another place does not. With the save's own area and no age or time
  asked, only the area, position, heading and dungeon room change. A save without the clock refuses a time and is fine
  without one.
- The file menu's dates order the slots, in any order given; the heading, the time and the age of an area name read as
  the game has them.
