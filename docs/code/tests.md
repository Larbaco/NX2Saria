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

## `tests/boot_warm_check.py`

Builds and runs `tests/boot_warm_check.c` on `runtime/vk/boot_warm.h`: whether a start does the whole warm on the boot
screen after a driver change (`runtime/vk/vk_pipeline.c` `bf_decide()`). The decision alone; no GPU, no game files.
The compiler comes from `tests/diff/common.py` when its packages are there, else by the same choice without them.

- An ordinary start (no file, the pipeline cache is this driver's) owes no warm. The first start after a driver
  update does the warm and the file gets 1. A start closed during the warm: the next one goes on with it and counts
  up, whether the pipeline cache's file still has the earlier driver's header or not. After `BOOT_WARM_TRIES` starts
  that began it and none that ended it: no more, with or without a foreign cache, and nothing to write.
- Once the warm ended, the cache file keeps the earlier driver's header until the play cache is folded in: those
  starts are ordinary ones, and so are the ones after. The file of an earlier driver is stale without a foreign cache
  and a fresh count with one, whether that driver's warm was done or given up. A file that is not one counts as none.
- The marker left by a start that ended while the boot built pipelines: a crash the handler saw skips the prebuild at
  once, whatever the count. With no note the start was closed or stopped: it counts, and the builds are made again,
  until `BOOT_QUIET_ENDS` in a row. A marker of 0.9.2 holds the GPU and driver alone and is the first of its row.
  Another driver's marker says nothing about this one, crash note or not.

## `tests/branding_package_check.py`

Names and legacy paths for the portable and the Linux desktop packaging, without building or running the game.

## `tests/build_game_gate_test.py`

`tools/build_game.py` through the memory gate. No game files, no game build.

- `build.ninja`'s compile rules run `tools/memgate.py` `wrap` and the link and rc rules stay as they were. Ninja runs
  at most the job count, the pool of generated compiles too. The text is the same from one call to the next, so
  `build.ninja` is rewritten only when it changes.
- With sccache the commands are the ones before the gate: its server runs the compiles, out of the wrapper's reach.
  A moved checkout keeps `build.ninja`: the venv's Python and `tools/memgate.py` are named from the root.
- Each compile's need comes from the lift manifest's `chunk_info`, or the source's size (an object not in
  `chunk_info`), or the runtime's default. The Windows resource object goes through windres, not the gate, and objects
  reused from the main build are not compiled.
- The peaks the wrappers record are learned and summed up; a failed compile's peak is not learned, nor a peak of 0 (no
  reading). With the gate off the summary has no held line. A failed reading of the sampler loses that sample only.
- Where ninja and clang are on `PATH`, three tiny C files are compiled by Ninja through the wrapper as `build.ninja`'s
  `cc_gen` rule does: in a temporary folder (absolute `$out`) and in one below `build/` (relative `$out`, as a real
  build's). `builddir` puts the Ninja log there, not in the repository, and `$out` is the name `needs.json` files a
  peak under.

## `tests/build_gate_check.py`

Builds and runs `tests/build_gate_check.c` on `runtime/vk/vk_build_gate.h`, the memory gate of the background pipeline
builds (`runtime/vk/vk_pipeline.c` `gate_enter()`): what it admits and refuses by the system's commit, and that the
builds under way count through their reservations. With the game executable's path it also runs the executable's
`--probe-buildgate` (`vk_pipeline_probe_buildgate()`: the gate as the builders call it, with this machine's commit; no
device, no window, no build) three times: with the settings in effect, with `build_free_mb = 0`, where every build
must be admitted, and with `build_free_mb` at a value no machine has free, where every build must be refused.

The C cases use a machine with 73,154 MB of commit and 4,096 MB kept free, each build estimated at 64 MB.

- Plenty of room: every one of 16 builders gets in, and each one's estimate is reserved.
- The case the reservations are for: the commit in use does not move while the builders start (their compiles have
  taken nothing yet), and the room above the kept part holds four estimates. A gate without reservations lets all
  sixteen in; this one lets four in and refuses the rest. When one ends, exactly one more fits.
- The boundary: exactly the kept part plus one estimate free is enough, one MB less is not.
- The commit of the night this was written for, 66,000 of 73,154 MB in use: one ordinary build fits (7,154 MB free
  against 4,160 needed); a large compute build estimated at 4,096 MB does not, and while one of those runs with 8,254
  MB free (room for it, and then 4,158 MB for the rest), ordinary builds are refused too.
- Above the limit already (the commit in use can pass a limit that shrank): refused, not a negative room taken for a
  large one.
- No check with the setting at 0 or on a platform that reports no limit (-1); reservations are still counted, so the
  numbers a log line prints stay true. More leaves than enters never leave a negative reservation behind, which would
  admit builds for nothing.

## `tests/cache_exit_snapshot_check.py`

Windows: the pipeline cache save at exit holds new builds back, waits for the running ones, and copies the cache only
while no build runs; the autosave never holds a build back. It builds `runtime/vk/vk_pipeline.c` `cache_build_enter()`
and `cache_save_one()` into a test program against stand-ins for the driver and the file system. It takes the path to
clang as an optional argument.

## `tests/cache_stamp_check.py`

Builds and runs `tests/cache_stamp_check.c` on `runtime/vk/vk_cache_stamp.h`, the translator stamp of the pipeline
cache files (`runtime/vk/vk_pipeline.c` `cache_stamp_check()`): how `pipeline-cache.id` is read and written, and what
a start does with the files when the stamp names the current translator, another one whose shaders are still carried,
another one that is not, or none.

What the runtime does with the files (deleting them, writing the stamp) shows in a run's log, in the line
`vk: pipeline cache stamp <id> matches|differs|none: <action>, <MB>`; `log_lines()` finds that line's format strings
(`LOG_LINES`) in `vk_pipeline.c`, adjacent string literals read as one, so a change of its words shows here.
`uses()` does the same for the header's two rules about `pipeline-cache.bin` in play (`USES`): where `cache_load()`,
`cache_save()` and `cache_stamp_check()` apply them and the start's line that says so. One of those patterns is the
rule that an empty cache counts as saved, so a run that took nothing does not write a header alone, which the next
start would refuse while the file no longer counts as missing.

- What the runtime writes is what it reads back: the identity's 64 lowercase hex digits and a line end, as
  `tools/build_xlate.py` writes `<translator>.id`. The same digits in upper case with a CRLF line end (a file edited
  on Windows) read the same. Text that names no identity is no stamp, and the start reads the files and stamps them.
- Actions: the same translator reads; another one carried reads and keeps the stamp; another one not carried drops; no
  stamp reads and stamps. Carry never changes what a matching or missing stamp does.
- A preparation translates everything under the current translator and carries nothing: another translator's files go
  even while the carry lists it, or the stamp would keep naming it and a later start would delete what the preparation
  built. A matching stamp or none is read in a preparation as anywhere.
- The sequence of an update: translator old wrote the files; the first start of the new one carries old's shaders
  (read, stamp kept), and so does each start until the carry pass removed old's translations (in the test, during the
  second start); the start after drops the files and stamps the new translator; the start after that matches.
- The stamp's and the play file's paths beside the cache file, and a path without ".bin"; cut to the room given.
- `pipeline-cache.bin` is written at the exit of a run that started without it, in play alone: never with the file
  there, never in a preparation (which writes it at every save). An empty file is none. Which saves write it: every
  save without a play cache; with one, a fold's, and the exit's of a run that started without the file, not that run's
  autosaves, nor any save of a run that read the file.
- Through the end of an update in play: the start that deletes the files is due to write `pipeline-cache.bin` and
  ends before its exit (a crash); the next start finds none and is due again, and its exit writes it, once (its
  autosave does not); the start after reads it and writes the play file alone.
- The words of the log line.

## `tests/camera_clearance_check.py`

The camera's clearance on pictures that are not 16:9: the patch set `camera_clearance` (`recomp/patches-v111.txt`) and
`runtime/patches/camera_clearance.c`. No game files: the module layout and the patch page are stand-ins.

The set's two sites are lifted by `recomp/lifter.py` as the generator lifts them: main+0x71a844 (`0xbd451e6a`,
`ldr s10, [x19, #0x51c]`) and main+0x71b624 (`0xbd451e6e`, `ldr s14, [x19, #0x51c]`) keep their load of the camera's
ratio and then call the patch page's +0xe00. The generator's own check of a patched site (`recomp/gen.py`
`Emitter.patched_effect()`) takes them: no alternative uses flags. Whether flags are live across the call depends on
the whole function, which needs the game's main: with the game's files, the camera step's generated C with and without
the set differs only in the sites' two lines. No other set calls the slot, and only `camera_clearance.c` registers it,
naming it through `HOST_SLOT`.

`tests/camera_clearance_check.c` then builds `camera_clearance.c` with the rest of the runtime stubbed and calls the
host as the two lifted sites call it, with the page's ratios set as `widescreen.c` sets them. Its `load()` does the
site's load (the value in the register's low lane, the rest zero) and then the host, returns the low lane as a float
and checks the upper lanes stay zero. The test's main base is placed so that the start check reads the two words
above at their offsets (the second site is 0xde0 bytes after the first).

- 21:9: the picture's ratio becomes 16:9 at both sites, in the load's own register; a value within 0.1% of it too;
  another value (0.5% off) stays, and so does a square view. 32:9 likewise.
- A 16:9 picture: nothing changes, even a ratio that matches it. A call from elsewhere changes nothing.
- The start lines for off, another main and no set (no set: no lines), and the session lines.

## `tests/carry_pool_check.py`

`runtime/gfx/gfx_shader.c`'s carry refusal: a previous translator's translation whose texture pool array has the
earlier length (32768, `VK_POOL_TEX_CARRIED` of `runtime/vk/vk.h`) is not served while the current translator may
translate the shader, because its clamp draws every texture numbered from 32768 up with slot 32767. No GPU, no game
files, any Python.

The check cuts `pool_short()`, `carry_usable()`, `carry_load()`, `carry_find()`, `carry_find_iid()` and `carry_last()`
out of `gfx_shader.c`, compiles them against stand-ins for the disk and memory caches, and walks the lookups: a refused
entry is freed and the lookup goes on to the next previous identity, the owned branch (`gfx_shader_load_private()`)
frees it as well, and entries with the current length, the sampler pool or no pool are served as before.
`carry_last()`, which runs once the current translator has failed the shader, serves the refused translation after all
and puts it in the memory cache, where the memory lookup finds it. `pool_short_note()`, which counts the shaders of the
earlier length that enter the memory cache for the warning of `vk_texture.c`, counts those only, and logs only one that
is not carried. `gfx_shader.c` does not include `vk.h`, so it states the earlier length itself: its `POOL_TEX_SHORT`
must equal `VK_POOL_TEX_CARRIED`.

## `tests/census_charge_check.py`

Builds and runs `tests/census_charge_check.c` on `runtime/diag/perf_census.h`, which charges the part of a frame that
an address-space walk of `NX2SARIA_MEM_CENSUS` overlapped to the frame's census part (`runtime/diag/perf.c`
`ft_frame()`): from the replay part first, then from the unattributed part, never more than the overlap and never
past the frame. No GPU, no game files, any Python.

- No walk seen, a walk that ended before the frame or started after it: nothing moves. A walk that ended 10 ms into
  the frame takes 10 ms from the replay. A walk inside the frame, longer than the replay part: the replay's 15 ms,
  then 10 ms of the 20 unattributed. A walk still running reaches now.
- A torn read (the last walk's start, 10 s back, with the 0 stored before the next start) moves nothing; a walk
  running for 1.9 s still counts, one running for just over 2 s (`CENSUS_WALK_MAX_NS`) does not.
- The overlap caps the charge, and the parts cap it too (nothing is taken from the frame's other parts). Never past
  the frame: when the walk of `census_check` itself already took 35 of the frame's 40 ms, only the rest moves.
- Random frames (seed 7): the census part never passes the frame, the moved time is the overlap capped by the frame's
  room and its replay and unattributed parts, and the sum of the three parts is kept.

## `tests/cfg_parse_check.py`

Builds and runs `tests/cfg_parse_check.c`: `runtime/cfg/cfg.c`'s reading of the settings file and the environment,
against stand-ins for what it links with. No game files. `cfg_parse_check <case> <ini>` loads the file the script
wrote and prints every line `cfg.c` logged (`log: ...`) and what the case reads back (`got: ...`).

| Case | What it does |
|---|---|
| `load` | `cfg_load()` on the file; then the check of the environment by `cfg_init()` (the variables the script set) |
| `full` | `cfg_load()` on the file, then two `cfg_set()` of keys the file does not have |
| `read` | `cfg_load()` and `cfg_init()`; then what `cfg_get()`, `cfg_bool()` and `cfg_int()` give for a list of keys |
| `lowend` | `cfg_load()` and `cfg_init()`; then what `cfg_bool()` gives for the switch of the low-end GPU mode, what two of the mode's keys read and the default of one of them |
| `norow` | `cfg_int()` and `cfg_double()` of a key without a row whose value (the environment's) does not parse |
| `older` | `cfg_load()` on the file twice, as a reload reads it again, and `cfg_init()`; then what `cfg_get()` gives for three keys: the older `NX2NAVI_` names in the file and the environment, and their log lines |
| `testset` | a test hook's values through four saves: test values only, a player's change beside them, the player's `cfg_set()` of a test key, and `cfg_test_keep()` (`SAVE=1`); then the menu's draft over a test value, whose preview (`cfg_set()`) and revert put the test's value back as the test's |

The stand-ins: `cfg_supported()` counts every key (no patch sets here); `plat_fs_rename()` refuses with
`NX2SARIA_TEST_RENAME_FAIL=1`, as a file in use would be refused; `plat_file_write_atomic()` prints what a save would
write (`write: <file's text>`) and writes nothing, so a case sees which keys reached the file; `hw_detect()` knows
nothing of the hardware, so the recommended preset is high, which sets nothing. `cfg_init()` finds no settings file
next to the stand-in executable and checks the environment's values.

What the readers get and what the log says:

- A comment after a value (`runtime/cfg/cfg.c` `cut_comment()`): cut at a blank followed by `;` or `#` for a key whose
  row has a set of values, the line logged once with its line number, `key = ; note` left unset; kept whole in free
  text (a row without a set, a key without a row), in a value in double quotes and in the environment, where the log
  still says it is part of the value; at most 16 lines, then a count.
- Switches written as words (`switch_word()`, `cfg_bool()`): false, off and no in any case read as 0 and true, on and
  yes as 1 through `cfg_get()`, `cfg_bool()` and `cfg_int()`, from the file and from the environment, with no line
  about them.
- The switch of the low-end GPU mode, which is gone (`lowend_set()`): on, it turns none of the mode's keys on and
  gives none another default, and the log says once that the line does nothing ("the low-end GPU mode was removed"),
  taking the same words as `cfg_bool()`, from the file and from the environment. The mode's keys keep their own
  defaults and are read by themselves, with or without the switch.
- Values outside their rows' sets (`check_value()`): a 0|1 switch with another value, a number outside its range, a
  word that is none of the menu's choices, each named with its line; none for a file whose values fit, nor for a list
  with numbers in it (the menu's presets); at most 16 lines, then a count.
- A full settings table (`CFG_MAX_KEYS`): one line at load naming the first key not read, one at the first menu change
  not kept.
- A number read of a key without a row whose set value does not parse gives 0 (`default_number()`), not a crash.
- A test hook's values (`cfg_set_test()`, `NX2SARIA_MENU_TEST_SET` of `runtime/ui/menu.cpp`) are read like any other
  but no save writes them, the one at exit included, with one log line each time their count changes; a player's
  change beside them is written alone, a player's `cfg_set()` of a test key makes it the player's, and
  `cfg_test_keep()` (the hook's `SAVE=1`) lets the next save write the rest.
- The older `NX2NAVI_` names (`older_env()`, `older_file_key()`): an environment variable and a settings file key so
  written still apply, the current `NX2SARIA_` variable wins over the older one deterministically, the older one still
  supplies a value when it is alone, the environment wins over the file, and the log names each older name once
  however often the file is read.
- `cfg_init()` (`find_ini()`) chooses the adjacent `nx2saria.ini`, renames an existing install's `nx2navi.ini` to it
  when there is none, keeps reading `nx2navi.ini` under its old name when the rename is refused, and selects
  `nx2saria.ini` for a fresh install without either file.

## `tests/cfg_rows_check.py`

Static check of `runtime/cfg/cfg.c`'s settings tables against what the runtime reads. Standard library only, so the
content guard workflow runs it beside `tools/check_content.py`. It prints one line per problem with file:line and
exits 1 if any; `--self-test` puts each kind of fault into copies and checks that each is caught. It fails on:

- a row or name after a `//` on a line of `known[]`, `metas[]`, `sections[]` or `DECK[]`: it is not compiled, and a
  hidden `metas[]` row ends every start with "FATAL: cfg: NX2SARIA_<X> has no row in the settings table" (this shipped
  in two test builds); a `/*` inside those tables, which this check does not read;
- a name twice in `known[]`, or two `metas[]` rows for one key in one `#if` branch (the first branch stands for the
  table, later ones repeat its keys);
- a `metas[]` row whose key is not in `known[]`;
- a read with `cfg_value`, `cfg_int`, `cfg_bool` or `cfg_double` of `"NX2SARIA_<KEY>"` (directly, or through one level
  of a function that passes its `const char *` parameter on to one of them) whose key has no `metas[]` row, unless the
  same function asked `cfg_get`, `cfg_is_set` or `cfg_meta` about that key before it (as `kernel.c`'s `SPIN` and
  `vk_headroom.c` do);
- a name the runtime reads that is not in `known[]` and does not start with `CHEAT_` (`runtime/cfg/cfg.c`
  `is_known()`): `cfg_get`, `cfg_is_set` and `cfg_meta` of `"NX2SARIA_<KEY>"`, `rt_knob("<KEY>", ...)`, one level of
  functions whose `const char *` parameter reaches `cfg_get` or a `"NX2SARIA_%s"` of it, and the
  `set <name> NX2SARIA_<KEY>` lines of `recomp/patches-v111.txt`. Such a key set in the settings file logs "unknown
  key" and is listed as unknown in the menu.

Names read only with `getenv`, `plat_setenv` or `cfg_setenv` are environment-only and not checked.
`runtime/cfg/cfg_probe.c` asks for an unknown key on purpose and is left out, as are `third_party/` and the generated
`*_spv.h` headers.

The helpers: `matching()` gives the index of the bracket that closes the one given, brackets in string literals and
comments left out; `split_comment()` the code and the comment of one line, the comment starting at the first `//`
outside a string literal; `strip_comments()` C source with its comments blanked out, line numbers and string literals
kept; `tables()` the names of each table, the first `#if` branch of each line only, with the problems of hidden rows;
`functions()` the name, parameters and body of every function definition in comment-stripped source; `check()` the
problems, one line each, from `{path as reported: text}`. `self_test()` plants, among others, a row the game reads
hidden behind a `//` put before it on its line (its readers are left without a row), and a key the game reads with
`cfg_bool` (or another typed read: they all take the row default of `cfg_value`) that is in `known[]` but has no
`metas[]` row: the game ends at start with "FATAL: cfg: ... has no row in the settings table" whenever neither the
environment nor the settings file sets the key.

## `tests/changebank_hold_check.py`

The neighbouring map's hold: the patch set `changebank_hold` (`recomp/patches-v111.txt`) and
`runtime/patches/changebank_hold.c` with `runtime/patches/changebank_hold.h`. No game files: the module layout and the
patch page are stand-ins written to the test's build folder.

The set's one site is lifted by `recomp/lifter.py` as the generator lifts it: main+0x3299d28 (`0x9100054a`, the loop's
`add x10, x10, #1`) keeps the add and then calls the patch page's +0xe30. The generator's own check of a patched site
(`recomp/gen.py` `Emitter.patched_effect()`) takes it: no alternative uses flags, which a patched call may not.
Whether flags are live across the call depends on the whole function, which needs the game's main: with the game's
files, the update's generated C with and without the set differs only in the site's line. The host writes w15 and
reads x10, which `recomp/regs.py` does not count as callee-saved, so the lifted code has them in the `Cpu` around the
call. No other set calls the slot, and only `changebank_hold.c` registers it, naming it through `HOST_SLOT`.

`tests/changebank_hold_check.c` builds `changebank_hold.c` with the rest of the runtime stubbed, runs the rule over
stand-in units, then calls the host as the lifted site calls it, over a stand-in manager laid out as 1.1.1's
constructor (main+0xd12c00) lays it out.

- The rule alone (`cb_step()`): the first choice is the game's. The castle chosen for 300 ms, then the field again:
  held all along and absorbed, with no load cancelled (the field asked for all along). The castle for the whole hold:
  taken at 2000 ms, not before, with one new load. Two units of one map (the choice moves from unit 2 to unit 3, both
  the valley's): no hold, no load lost. A direct request is the game's at once. After a gap over half a second (a
  loading screen) the choice is the game's; so it is for another manager (another area), for a held unit the game let
  go (no longer in use) and for one whose unit holds another map now. A hold of 0: always the game's, the flips
  counted. Away for two other maps in turn: the hold counts from the first update that left, so it ends all the same.
  No map at all (Link left every radius): the held map stays for the hold, then none. Nothing held and nothing chosen
  gives none. The hash (`cb_fnv1a()`) takes a path's bytes up to its end and is never 0.
- The host: `update()` is one update as the lifted code runs it, the loop's 16 passes through the site (the add, then
  the host), the last with the loop's w15 and w17, the earlier ones with mid-loop values the host must leave alone;
  then, after the loop, `cmp w15, #0`, `ccmn w17, #1, #4, lt` and `csel w21, w17, w15, gt` (main+0x3299d90..0x3299d9c)
  on the registers as the host left them. It returns w21. Checked: the castle held back and absorbed, taken after 2000
  ms (125 updates), a direct request, the held map while the game names none (w15 and w17 -1) and while it asks for a
  unit directly, a call from another place of the slot (changes nothing), registers that are not units (kept, logged
  once), a manager that does not match (its updates keep the game's choice; one line), the minute line (once a minute
  and only when the choice moved), and the start lines, with a main that differs at the site (`0xd503201f`) turning
  the set off.

## `tests/check_isa_test.py`

`tools/check_isa.py` on the runtime files that hold instructions above x86-64-v2 behind a runtime CPU check
(the assembly stub `kern_ap_stub_ymm` of `runtime/hle/kernel.c`, `runtime/sha256.c` `blocks_ni()`): built as
`tools/build_game.py --cpu v2` builds the runtime (`-march=x86-64-v2`) and as players' builds do (no `-march`), they pass, and a function with
such an instruction and no entry in `DISPATCHED` still fails. The flags are `RT_FLAGS` of `tools/build_game.py` without
its warnings. It needs the MSYS2 UCRT64 clang and an objdump and is skipped without them; no game files.

## `tests/check_platform_test.py`

`tools/check_platform.py` on made-up `runtime/` trees: a Win32 use behind a Windows-only preprocessor branch passes,
an unguarded one fails, and the repository's own `runtime/` passes. A use behind `#if defined(_WIN32) && X` is
scanned, never let through. `CheckPlatformTest.check()` gives the exit code and the output of `check_platform.py` on a `runtime/` folder
holding the given files, or on a given folder. No game files; standard library only.

## `tests/child_timeout_check.py`

How a translator child that stops answering is handled (`runtime/platform/win32.c` `plat_child_read()`,
`runtime/gfx/gfx_shader.c` `child_exchange_raw()` and `svc_request()`). The check cuts the child functions out of
`win32.c` and the request functions out of `gfx_shader.c` (`extract()` takes the text from a head through its
brace-balanced body), builds them with `PLAT_CHILD_READ_MS` at 300 ms (`platform.h` lets a build set it) and runs
three children through them. Windows only (the child functions are those of `win32.c`). No GPU, no game files, any
Python.

- A child that keeps its pipe open and never writes: `plat_child_read()` gives -2 after the timeout, and
  `svc_request()` gives -1 having started one child, so a stuck translator holds its caller one timeout, not two.
- A child that exits at once: the read fails with a broken pipe (0 or -1) at once, and `svc_request()` restarts it
  once and gives 0.
- A child that answers one line: `svc_request()` gives 1 and the line without its line end.

## `tests/compute_prebuild_queue_check.py`

Stresses the production recent-first prebuild queue: changing the concurrency must not omit or repeat any job.

## `tests/compute_warm_reuse_check.py`

Exercises the production compute creation with a fake driver: reuse must never suppress a required live pipeline.

## `tests/crash_stack_check.py`

Builds and runs `tests/crash_stack_check.c`: a stack overflow on a thread and on a fiber made by the platform layer
(`runtime/platform/win32.c` `thread_main()`, `fiber_start()`, `plat_stack_guarantee()`) still gets its whole crash
report, a file write and three 2 KiB log lines, and ends with the report's code 71. Without the stack kept back the
report faulted again and the process ended with an access violation and no line. No game files.

The program (`crash_stack_check thread <file>` or `crash_stack_check fiber <file>`) links the platform layer: a thread
made by `plat_thread_spawn()`, or a fiber made by `plat_fiber_new()`, recurses until its stack runs out, and the crash
handler then writes its lines as `crash_handler()` of `runtime/main.c` does: 2 KiB log buffers on the stack with
`vsnprintf` as `rt_log()` has them, a file write first, as `vk_pipeline_crash_note()` makes during the boot's builds,
and a frame walk. A volatile keeps the recursion from looking endless to the compiler. The script wants every line;
the thread's name in the report (`plat_thread_name()`) is the spawned thread's name, and "(unnamed)" for the fiber,
which runs on the main thread, one the platform layer did not make.

## `tests/diff/atomic.py`

Differential test of exclusives and atomics against Unicorn with the max CPU model, which has the ARMv8.1 LSE atomics.
Needs no game files. Single steps cannot cover these: a store exclusive only succeeds after a load exclusive, and the
A57 model rejects LSE. So the test enumerates every variant and runs short sequences (16 bytes of code per test,
`SLOT`), with the monitor cleared before each case:

- LDXR/LDAXR then STXR/STLXR to the same address (succeeds), all sizes;
- LDXP/LDAXP then STXP/STLXP to the same address (succeeds) or 16 bytes further (fails; Rn is advanced between the two
  by an `add xn, xn, #16`), W and X pairs;
- STXR and STXP alone (fail without a monitor);
- LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN, LDUMAX, LDUMIN and SWP, every size and ordering;
- CAS and CASP, every size and ordering, with the compare value equal to memory in half of the cases. For CASP, Rs is
  picked so that Rs+1 is XZR or away from Rn.

`tests()` returns `(words, rn, kind)`, where the kinds `cas` and `casp` mark compare-and-swap tests (the state gets a
matching compare value). `unicorn_wrong()` names LDSMAX and LDSMIN on bytes, halfwords and words: Unicorn 2.1 (QEMU
5.0) compares them unsigned, so `model_signed_minmax()` gives the reference for those words from the ARM pseudocode
(the expected state and memory CRC).

## `tests/diff/common.py`

Shared parts of the differential tests: the Unicorn reference, the operand generators, and the build and run of
`tests/diff/harness.c`. `IN_FMT` and `OUT_FMT` are the record layouts of `struct In` and `struct Out` in `harness.c`.

- `rand_lane()` gives a random interesting integer lane of `8 << sz` bits; `rand_v()` leaves the upper half zero in
  15% of the values, like most 64-bit results; `rand_fpcr()` sets DN (bit 25), FZ (bit 24), RMode (bits 23:22) and AHP
  (bit 26) at random.
- `Emu` is Unicorn as a Cortex-A57, the Switch's CPU, unless another model is given (LSE atomics need
  `UC_CPU_ARM64_MAX`), with FP/SIMD enabled through `CPACR_EL1`. Images are (base, bytes) pairs mapped at their guest
  address. `Emu.run()` runs `count` instructions from pc and returns (x[31], sp, nzcv, pc, v[32]).
- `build_harness()` compiles one C function per test body plus `harness.c` into `work/harness[.exe]`. It links
  `runtime/fma.c`: a `--cflags=-march=x86-64-v2` run must exercise the software fma the v2 build links, not the C
  runtime's. It links with lld when the toolchain has it beside the compiler and with its default linker otherwise (an
  MSYS2 install without the lld package stopped every differential test at "invalid linker name").
- `write_cases()` takes cases of (k, fpcr, x[31], sp, nzcv, v[32], memchk); with `stubs`, calls go to the call stub of
  `harness.c` (`rt_stub()`). `run_harness()` runs the lifted tests and returns the records and the stderr tail or
  None; its images are (base, path) pairs. `diff_state()` compares a harness record with (x, sp, nzcv, pc, v, mem) and
  returns the list of mismatches.

## `tests/diff/func.py`

Differential test of whole emitted functions: the C of `recomp/gen.py` for a guest function against Unicorn running
the same function from its entry until it returns.

It picks functions from the ExeFS modules: no branches out of the function, no SVC, BRK, system register, exclusive or
atomic instructions; with conditional branches, and loops, jump tables, SIMD and calls where the module has them. It
emits them with the generator's own `Emitter`, compiles them with `tests/diff/harness.c` (and `runtime/fma.c`, for the
reason given under `tests/diff/common.py`), and runs each from random states: x0-x7 point into the scratch buffer (or
hold small integers), the buffer holds pointers into itself and random data, SP sits near the buffer's top and LR is a
sentinel address (`SENTINEL`, never executed). Unicorn (Cortex-A57) stops when PC reaches the sentinel. A case is
skipped when Unicorn faults, writes outside the scratch buffer (module images are mapped read-only), leaves the
function, or runs longer than `--steps` instructions. It compares X0-X30, SP, V0-V31 and the scratch buffer. NZCV is
not compared: generated functions keep it in a local (`recomp/flags.py`) and leave `c->nzcv` as it was, so flags only
count where the function reads them, which the registers and memory show.

Every BL and BLR goes to one stub on both sides (`rt_stub()` in `harness.c`, `Runner.stub()` here) instead of its
callee. The stub folds the guest state at the call into a digest and sets the caller-saved registers from it, so the
test sees whether the emitted code had every register in the `Cpu` at the call and reads them back after it. Callers
of another picked function are dropped, as the C side would run that function instead of the stub.

With `--patches`, the spin waits of the table (`recomp/patches.py`, `spin`) are emitted with their gates, and
functions that hold one come first (a quarter of the picks at most); a case whose loop does not leave runs into
`--steps`.

- `forbidden_mask()`: the words a picked function must not contain. `0xD4000000` under mask `0xFF000000`: SVC, HVC,
  SMC, BRK, HLT, DCPS. `0xD5000000` under `0xFFC00000`: system instructions (MRS, MSR, SYS as DC ZVA, hints, barriers),
  of which hints and barriers are let through. `0x08000000` under `0x3F000000`: exclusives, load-acquire and
  store-release, CAS. `0x38200000` under `0x3F200C00`: LSE atomics. `call_mask()` is BL and BLR.
- `pick_functions()` returns `(entry, end, tags)` of up to `count` functions of a module, plus the `Emitter` that will
  emit them; `spins` maps a spin wait's back-edge to its head, emitted with gates. Functions with a spin wait come
  first, then those with a `br` (jump tables, fallbacks), at most a quarter of the picks each; then functions with
  calls, at most half.
- `call_sites()` gives the addresses of the BL and BLR instructions of a function; `callees()` the function starts its
  emitted C calls directly (`f_<addr>(c)`, not through `rt_dispatch`); `check_function()` the tags of a function the
  test can run in isolation, or None; `build()` compiles the emitted functions with `harness.c` into
  `work/func[.exe]`, `tests[k]` being `emitted[k]`.
- `Runner` is Unicorn with the module images read-only, a block hook that stops when execution leaves the function,
  and a code hook on every call site of the picked functions that stops before the call, for the stub. There is one
  hook per call site, all added before anything is translated, so no cached block lacks one. `Runner.stub()` is
  `rt_stub()` for a call Unicorn has not executed yet and returns the address to resume at. `Runner.run()` returns (x,
  sp, nzcv, pc, v, crc of the buffer), or None if the case is skipped.

## `tests/diff/harness.c`

Runs lifted tests and writes the resulting CPU state per case. Shared by `tests/diff/lift.py`, `tests/diff/simd.py`,
`tests/diff/atomic.py` and `tests/diff/func.py`; the expected state comes from Unicorn on the Python side. Its
arguments are `buf.bin cases.bin out.bin image base [image base ...]`: each image is mapped at its guest address
(guest == host), the scratch buffer at `BUF`. `struct In` and `struct Out` match `IN_FMT` and `OUT_FMT` in
`tests/diff/common.py`. The output is flushed per case so a crash leaves the completed cases readable.

### `rt_stub()`

The callee of every call in a function under `tests/diff/func.py`; `Runner.stub()` there is the same function on the
Unicorn side. It folds the target, X0-X30, SP and V0-V7 into a digest, so a register the caller left unwritten at the
call changes it, then sets X0-X18, NZCV and V0-V7 from the digest, so a caller reading a register it held across the
call sees a value the callee could have left. X0-X7 mostly get pointers into the scratch buffer. The digest and the
call count go to the top of the scratch buffer, which the memory check covers.

## `tests/diff/lift.py`

Differential test of the lifter on the game's own instructions: lifted C against Unicorn, one instruction at a time.

It samples distinct instruction words from the `.text` of each ExeFS module, one per opcode signature first, lifts
each with the portable FP/SIMD lowering, runs it from random register state, and compares X0-X30, SP, NZCV, PC (for
branches), V0-V31 and the scratch buffer (for loads and stores) with Unicorn as a Cortex-A57. Skipped: system
instructions and exceptions, whose single step cannot be compared, and exclusives and ARMv8.1 atomics, which
`tests/diff/atomic.py` covers. Encodings the lifter rejects are counted per module and mnemonic; `--coverage` lifts
every distinct word instead of testing. The test load addresses (`BASES`) are below 2^39 and clear of `BUF`.

- `classify()` gives `skip` for: system instructions (`0xD5000000` under `0xFFC00000`: hints, barriers, MRS/MSR, cache
  maintenance); exclusives, which need a paired load, and CAS, which is ARMv8.1; LSE atomics (`0x38200000` under
  `0x3F200C00`); SVC, BRK and HLT (`0xD4000000` under `0xFF000000`); UDF (the upper 16 bits zero).
- `class_keys()` is the opcode signature per word: the register fields (Rd, Rn, Rm) cleared, mask `0xFFE0FC00`.
- `pick_module()` returns `(word, pc, body)` for the distinct words of `.text`, one per opcode signature first,
  lifted. `coverage()` lifts every distinct word and returns, per mnemonic, the distinct words and occurrences of those
  the lifter rejects (`Unsupported`) or turns into an undefined-instruction trap (`rt_udf`).

## `tests/diff/simd.py`

Differential test of the portable FP/SIMD lowering (lifter `simd='c'`) against Unicorn as a Cortex-A57. Needs no game
files.

It enumerates every opcode bit pattern of the FP/SIMD data-processing space (bits 27..25 = 111), the FP/SIMD
load/store space and CRC32 with random register fields (`candidates()`), and compares which encodings the lifter and
Unicorn accept (`validity()`: word -> (lifter accepts, Unicorn accepts), the words placed at `CODE + 4i`). Then a
stratified sample of encodings both accept, one per opcode signature first (`class_key()`: the register fields
cleared), is compiled and run with random interesting operands (NaNs, infinities, denormals, +-0, saturation edges,
random FPCR.DN/FZ/RMode/AHP): X0-X30, SP, NZCV, V0-V31 and, for stores, memory must match. FPSR is not compared; the
lowering does not model the cumulative exception bits or QC. Literal loads get an offset into the code region.
`is_store()` reads the L bit (bit 22), which is in the same position for structures, pairs and single registers; a
literal load (`0x18000000` under `0x3B000000`) is never a store.

## `tests/dlssg_device_check.py`

Builds and runs `tests/dlssg_device_check.c` on `runtime/vk/vk_dlssg_device.h`: which card counts as usable while DLSS
Frame Generation's interposer is in use, and how the device is made when the interposer cannot make it
(`runtime/vk/vk_device.c` `pick_device()`, `runtime/vk/vk_dlssg.c` `vk_dlssg_create_device()`). The interposer and the
loader are stand-ins that answer as the real ones did in a player's log; `EXT_NOT_PRESENT` (-7,
`VK_ERROR_EXTENSION_NOT_PRESENT`) is what the interposer answered on Intel graphics. `create_0101()` is 0.10.1's
creation, for comparison: the interposer alone whenever it is in use. No GPU, no game files, any Python.

- The laptop of the report: an RTX 3050 (gpu 0) and Intel Iris Xe (gpu 1), `framegen = dlss`, `gpu_index = 1`.
- No NVIDIA card at all (`framegen = dlss` left in the settings file): the device is made without frame generation.
- An NVIDIA card the interposer refuses (an old driver): once through it, then the loader; when the loader fails too,
  its result is the one reported.
- Frame generation off: the loader, whatever the card, and a chosen card of any vendor stands.

## `tests/dlssg_exports_check.py`

The export check of `tools/build_dlssg.py`: it reads the exports `runtime/dlssg/nx2_sl.h` declares and the ones a DLL
has, and names those the DLL lacks. A DLL built from older sources would otherwise ship: the runtime takes a missing
optional export (`nx2sl_reflex2`, `nx2sl_frames_max`) for an older DLL and leaves its feature out without an error.

The test builds a small DLL with two of three declared exports with MSYS2 UCRT64's clang (no MSVC, no Streamline SDK,
no game files) and checks that the third is the one reported; then that the declarations of `nx2_sl.h` itself are read
in full (the header names its version and log types too, which are no exports of their own). The release gate:
`tools/build_dlssg.py` `check()` stops on that DLL (`nx2_sl.h` lists `nx2sl_reflex2`), on a missing file and on files
that are no DLL, naming why, and passes a DLL built with every export `nx2_sl.h` declares. `check_dlssg` of
`tools/make_launcher.py` is that check, and both `tools/make_launcher.py` and `tools/make_kit.py` run it on their
`--dlssg` DLL before they resolve the Streamline files (a download) or delete their output folder: a release that
shipped the DLL of 0.8.0 would never let Reflex hold the frame limit. `refused()` gives the message a call exits with,
None when it returns.

## `tests/dlssg_pace_check.py`

Builds and runs `tests/dlssg_pace_check.c` on `runtime/pacing/dlssg_pace.h`: whether Reflex or the game's own pacer
holds the frame limit with DLSS Frame Generation (`runtime/vk/vk_dlssg.c` `vk_dlssg_pace()`), and its checks of the
frames Reflex held, driven with made-up present times. It needs a C compiler only: no Vulkan headers, no GPU, no game
files. The build traps on undefined behaviour and on unsigned wrap-around: every period and time in `dlssg_pace.h` is
a `uint64_t`, which `-fsanitize=undefined` alone lets wrap, so an overflow in that arithmetic fails the check instead
of passing by chance.

`run()` in the C file is a run of n presents d ns apart, each followed by a host present 1 ms later; the present thread
takes every limit asked for at once (applied follows want, before the next game present). It returns how many presents
Reflex held and the last window result that was not `SAME`. `P60` is 16666667 ns, `frame_step_cap_ns` at 60.

- Who holds: before the first host present, the pacer. With a host present 5 ms ago Reflex is asked for the drawn
  period (16667 us) and the pacer holds until Streamline took it. A movie (period 0): no limit for Reflex, the pacer
  holds. The setting off, or an older DLL: the pacer. No host present for 200 ms (minimised): the pacer; 199 ms is
  still Reflex.
- Reflex holds the limit and presents come a period apart: no verdict over many windows. A GPU slower than the limit
  at 1x gives frames 2 periods apart, never taken for the shown count.
- Nothing holds the game: 31 of 60 presents too soon give the limit to the pacer for the run; 30 of 60 is not enough.
  The present right after a limit was asked and then held is the first that counts.
- The driver counts the frames on screen: at 2x the drawn frames come two periods apart, so Reflex is switched to
  shown units; then they come a period apart and Reflex keeps it. At 4x the limit is the period over 4; with the
  generation paused (the menu, 1x) the period itself.
- Shown units guessed wrong (the driver counts drawn frames): the frames come too soon, so the pacer. The same slow
  band twice (a GPU that only manages the limit over the multiplier): the pacer. Hitches (a load: 10 periods) are
  neither.
- A change of the period or the multiplier starts the window again: 40 too soon at one limit and 40 at the next give
  no verdict. Presents the pacer held are not counted (the limit never applied).
- The longest limit Reflex takes is 30 fps, 33333 us; a period too long for it is the pacer's. The clock going back (a
  host present stamped after the game's present read the clock) holds nothing up.

## `tests/draft_key_check.py`

The settings menu's draft names each setting `NX2SARIA_` and its key (`runtime/ui/option_rows.cpp` `norm_key()`), and
every comparison of a name with the `NX2SARIA_` or `NX2NAVI_` prefix uses the prefix's own length. No GPU, no game
files, any Python.

1. `norm_key()`, cut out of `option_rows.cpp` with `KEY_PREFIX` (`cut()`) and built with a small harness, gives
   `NX2SARIA_HDR` for `HDR`, `hdr`, `NX2SARIA_HDR`, `nx2saria_hdr` and the earlier `NX2NAVI_HDR`, and the log's
   spelling (the name after `KEY_PREFIX` characters) is `HDR`. When the prefix became `NX2SARIA_` (9 characters) but a
   length of 8 was kept here, the draft wrote `NX2SARIA_NX2SARIA_HDR`, a name no setting has: no change in the menu
   took effect.
2. No `strncmp`, `strncasecmp`, `_strnicmp`, `memcmp` or `std::string` compare (`s.compare(0, n, "lit")`) in
   `runtime/`, `launcher/native/` or `tools/` compares a literal starting with NX2 (any case) over a length other than
   the literal's (`lengths()`), and `option_rows.cpp` cuts the prefix from a key only through `KEY_PREFIX`.

## `tests/ds3_check.py`

Builds and runs `tests/ds3_check.c`: `runtime/input/input_ds3.c`, the DualShock 3 input report decoded to inputs by
position, and the output report's layout.

- A report at rest has id 1, the sticks centred and nothing pressed. Every button of bytes 2 and 3 alone lights
  exactly its own input; the PS button (byte 4) is not a Switch button.
- Sticks: 0 and 255 reach the ends, and y is turned so that up is positive.
- Triggers: the pressure decides when the report has it, the switch when the pressure reads 0; in a short report (12
  bytes) the switch alone.
- Other reports are refused and leave the outputs alone. An output buffer too short for the layout (20 bytes) gets
  zeros.
