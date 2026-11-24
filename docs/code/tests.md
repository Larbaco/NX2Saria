# tests

One section per test, for what [building.md](../building.md#checks) does not already say: a few tests first, then
those from `tests/action_prompt_check.py` to `tests/log_trace_check.py` in the order of the file names, a test's C
program under its Python file. The tests after those keep their comments until their part of the area is extracted
([README.md](README.md#areas)).

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

## `tests/evo_peek_check.py`

Builds and runs `tests/evo_peek_check.c` on `runtime/vk/vk_evo_peek.h`: the copy of each event-ordered queue's first
held wait that `runtime/vk/vk_exec.c` `vk_exec_held_ready()` reads without the submission lock, and the decision it
makes from that copy. No GPU, no game files, any Python.

- The decision that `cont_ready()` of `vk_exec.c` makes, taken from a copy, with a stand-in for `vk_exec_event_met`
  that answers from the wait's value.
- A publish is read back whole; a fresh one reads as nothing held. A reader never waits for a publish that does not
  end: it gives up after `EVO_PEEK_TRIES` tries.
- The race: one writer republishes changing waits 1,000,000 times while three readers copy them, as the submitting
  thread (`evo_resume()`, `cont_push()`) and the presenting threads (`held_resume()` and `submit_sync()` of
  `runtime/vk/vk_backend.c`) do. Every field of publish i is derived from i, so a mixed copy shows: no reader may take
  a copy mixed from two publishes (the all-zero state before the first publish aside), and none may wait for the
  writer.

## `tests/excl_mt.py`

Multithreaded test of the exclusive monitor helpers in `runtime/cpu.h` (`ldxN`/`stxN`, `ldxp128`/`stxp128`). Needs no
game files.

- A changed value: thread A loads X exclusively, thread B changes X to Y with its own exclusive pair, then A's store
  exclusive must fail. Checked for the 32-bit, 64-bit and 128-bit pair forms, plus a control where B touches nothing.
- A value changed and changed back (ABA): B changes X to Y and back to X, and A's store exclusive succeeds. The
  monitor is a compare-and-swap of the value loaded, so it does not see this case, where the console's monitor would
  fail the store; the check holds the helpers to what they are.
- Contention: N threads increment one counter with load/store-exclusive retry loops; the total must be exact.

## `tests/exit_order_check.py`

The orderly end of the process (`runtime/gfx/gfx_present.c` `rt_exit_begin()` and `rt_exit_steps()`): the steps run in
one order, with what a kill of the process must not lose (a save Commit in flight, RivaTuner Statistics Server's
profile, the controller's rumble, the settings, the frame-time summary) before the slow pipeline save, and the final
save flush last; it runs once, and a second caller on another thread stays where it is. The frame-time summary of the
exit (`runtime/diag/perf.c` `ft_stop()`) waits for a present thread's update under way and keeps later ones out. The
test builds those functions into a test program against stand-ins, and checks statically that every orderly end goes
through them. No game files; it takes the path to clang as an optional argument.

`calls_in_order()` gives the position of each name's first call in a body, comments left out, or None for a name that
is missing. Among the static checks:

- The guest's exit line comes after the guard: a thread the guard parks (a close already ending) logs no exit.
- The window's close (`runtime/gfx/gfx_window_win.c`) runs the end on a thread of its own, and the session's end
  waits for it.
- The focus loss of the window's hide is the end's, not a player's: it must not reach `perf_focus_lost()` of
  `perf.c`, whose session report would take the exit summary's frames.
- The presents during the pipeline save do not undo the restore of RTSS's limit: `runtime/pacing/rtss_limit.c`
  `rtss_limit_restore()` marks the run closed before anything else (an exit before RTSS was ever asked included), and
  `rtss_limit_want()` returns on it before it changes the want or starts the worker. `tests/rtss_worker_check.c` runs
  that file against a fake RTSS.

## `tests/exit_tls_check.py`

Builds and runs `tests/exit_tls_check.c`: an ExitProcess or `exit()` that none of the game's own ends asked for leaves
the log queue's lines and one note in the log file, naming the calls that led to it by module and offset
(`runtime/platform/win32.c` `exit_note_tls()`, `plat_exit_note_setup()`). Windows only; no game files.

The program links the platform layer and `runtime/logq.c`. It logs a line through the log queue, which the log thread
is still gathering (for `LOGQ_GATHER_NS`), and ends in the way its mode says; the script reads the log file each mode
leaves (`CASES`: the mode, whether the queued line is in the file, whether a note is).

| Mode | End | In the file |
|---|---|---|
| `exitprocess` | ExitProcess from a function of this program | the queued line, then the note naming this program |
| `exit` | `exit()`: the C runtime's ExitProcess | the same note |
| `terminate` | TerminateProcess | no note; the queued line may or may not have been written, the kill came while it gathered |
| `platexit` | `plat_exit()` | the queued line (its hook writes the queue), no note |
| `return` | main returns after `plat_exit_orderly()`, as `runtime/main.c` does after a tool mode | no note |

## `tests/fatal_exit_check.py`

Builds and runs `tests/fatal_exit_check.c`: the end of a fatal error (`runtime/dispatch.c` `fatal_end()`) with the
platform layer linked and the rest of the runtime as stand-ins that write straight to stdout. The process ends with
its own code and its FATAL line, and without the libraries' detach calls that ExitProcess makes (a driver's detach
that faulted there turned the end into a crash report with another code). No game files, no GPU.

A fatal error stops HD rumble on a thread of its own and waits for it 300 ms at most (`rt_rumble_stop_bounded()`), so
a stop that hangs on SDL's lock does not hold the end. RivaTuner's profile is left to the next start
(`runtime/pacing/rtss_limit.c` `rtss_limit_crash()`), which on RTSS's own thread ("rtss"), whose lock that thread may
hold, makes no call into RTSS's library and takes no lock. Two failures at once end once: two threads that fault, a
fault while a fatal error is reported and a fatal error while a crash is: the first report is written whole, the
process ends with its code, and the second adds no line (`rt_ending_enter()`, which `crash_handler()` of
`runtime/main.c` calls first).

The executable has a TLS callback that prints a line ("detach:") when the process's libraries get their detach call,
which ExitProcess (and `_Exit`, which is ExitProcess in the C runtime) makes and TerminateProcess does not; a
`__thread` variable with `_tls_used` gives the image its TLS directory, so the callback is called. Its crash handler
is a stand-in for `crash_handler()` with its first line, `if (!rt_ending_enter(71)) plat_exit(71);`: it writes three
report lines 100 ms apart and ends with `plat_exit(71)`. The summary of `perf.c` at a fatal error and the stop of
`input.c`, which `dispatch.c` calls through weak symbols, are stand-ins that say they ran; the rumble one can hang as
when SDL's lock is held. A fatal note that takes a while stands for the card's and the memory settings' notes.

| Mode | What happens | Expected |
|---|---|---|
| `fatal` | `rt_fatal()` | exit code 70, the FATAL line, the frame-time summary, the rumble stop, no detach line |
| `crash_crash` | two threads fault at once | one whole crash report, the second thread waits, exit code 71 |
| `fatal_crash` | a fault on another thread while `rt_fatal()` reports | its FATAL line and notes whole, no crash report, exit code 70 |
| `crash_fatal` | `rt_fatal()` on another thread while a crash is reported | the whole report, exit code 71 |
| `fatal_rumble` | `rt_fatal()` while the HD rumble stop hangs (SDL's lock held by a thread that crashed) | the end goes on after 300 ms, exit code 70 |
| `fatal_rtss` | `rt_fatal()` on RTSS's thread, named as `plat_thread_spawn()` names the worker of `rtss_limit.c` | exit code 70, through `rtss_limit_crash()` too, which takes no lock there |

`static_checks()`:

- No runtime file outside the platform layer ends the process with `_Exit`: the platform layer alone ends it
  (`plat_exit`, `plat_terminate`). The fatal error's end and the lost device's (`runtime/vk/vk_device.c`
  `lost_wait_exit()`) end with `plat_terminate`.
- The crash handler's guard in `main.c` is the one the stand-in runs.
- The fatal error's summary (`perf_summary_flush_try()`) leaves out what the failure may have left half made, and only
  tries the map's lock.
- Both failed ends stop HD rumble through the bounded stop, after RivaTuner's profile is left to the next start.
- A first report that hangs: the thread that waits for it ends the process after 30 s with that report's code, and
  `fatal_end()` hands its own code to the guard (a hung FATAL must not read as a crash to the launcher).
- The fatal error's frame-time summary comes after RTSS's end and the rumble stop, as in the orderly end.

## `tests/ffx_cache_check.py`

Runs the game executable's `--probe-ffx-cache` twice on pipeline cache files of the test's own
(`runtime/vk/vk_ffx.cpp` `vk_ffx_probe_cache()`: one FSR 3.1 upscaling context, without a window or the game). FSR
3.1's library creates its pipelines itself and names no pipeline cache; the runtime hands it pipeline creates that go
through the pipeline caches kept across runs (`runtime/vk/vk_pipeline.c` `vk_pipeline_create_foreign()`). Checked:

- the library's pipelines come through those creates (the first run counts them);
- a second run, which reads what the first one saved, compiles none of them: each comes from the caches;
- with `ffx_pipe_cache = 0` none comes through the runtime.

The executable needs `amd_fidelityfx_vk.dll` beside it and a Vulkan device; without either the probe says SKIP and so
does the test. The pipeline cache files go to `build/test/ffx_cache`, never beside the executable. `run()` is one
probe run and returns the exit code, the last probe line and the counts or None. The text the test prints when it is
called without the executable's path is the string `USAGE`.

## `tests/fg_reset_check.py`

Builds and runs `tests/fg_reset_check.c` on `runtime/vk/vk_fg_reset.h`: which frames FSR 3.1 frame generation's
callback is told are camera cuts, by frame id. No GPU, no game files, any Python.

- Nothing noted: every id is a cut, id 0 and ids past the range too.
- A run of frames with a cut at 40: the callback for each id gets that frame's answer.
- The callback fell behind by more than the ring: its frame's slot holds a later one, so it is a cut.
- The id and its cut are one word: a slot rewritten on another thread (frame 3, a cut, and frame 3 + `FGR_SLOTS`,
  none, over and over) never answers frame 3 with the next one's "no cut".

## `tests/fg_stats_check.py`

Builds and runs `tests/fg_stats_check.c` on `runtime/vk/vk_fg_stats.h`: how frame generation's log line sorts the
game's frames (gameplay, the menu, no scene) and when it writes one. No GPU, no game files, any Python.

- The order of the reasons: the menu comes first, whatever the frame had.
- Nothing is due before a period's worth of frames. A long stretch without a scene (a movie) still reports, after
  `FGS_IDLE` frames.
- A session as the 0.12.1 Windows log had it: 4600 frames of title, movie and file menu, then gameplay at 60 fps,
  every frame generated (two presents each), with the settings menu open for 300 frames in between. The run's share
  is the gameplay's; the title and the menu are counted apart.
- A long session: every line up to `FGS_LINES`, then one period in `FGS_EVERY`, the counts carried in the totals.
- A new swapchain counts its presents from 0: the period's presents and time start again there, not at 0.
- GPU times out of range are not taken.

## `tests/flags_check.py`

Builds and runs `tests/flags_check.c`: the function-local flag helpers of `runtime/cpu.h` (`Flags`, `fc_*`, `fnz_*`,
`nz_*`, `adc*f`) against its packed helpers (`subs64()` and friends, `COND_xx`), which `tests/diff/lift.py` checks
against Unicorn. It prints the first mismatches and exits 1 on any. The condition argument of its `CHECK` is not
constant, so the switch default paths are covered too. The operands are random, with equal operands, neighbours and
small values (both under `0xff`) mixed in.

## `tests/flight_flash_check.py`

Builds and runs `tests/flight_flash_check.c` on `runtime/diag/flight_flash.h`: which frames the flight recorder takes
for a one-frame flash. Pictures of the thumbnails' size are drawn by the test, turned into cell means
(`flash_cells()`) and judged three at a time (`flash_test()`), or handed in frame by frame as the present thread does
(`flash_note()`). No GPU, no game files, any Python. It is built as the runtime is (`-O2`, x86-64-v3 on x86-64), so
the run with the argument `time` measures what the present thread pays for one `flash_cells()` and one `flash_test()`
(on a picture the compiler cannot take as known).

The pictures are 640x360 (`FLIGHT_THUMB_W` x `FLIGHT_THUMB_H` of `flight.h`): cells of 40x40 pixels. `DIFF` 24 and
`MIN_CELLS` 3 are the defaults of `NX2SARIA_FLIGHT_FLASH_DIFF` and `NX2SARIA_FLIGHT_FLASH_CELLS` (`cfg.c`). `scene()`
draws something in every cell: bands of brightness that differ from cell to cell, shifted by (dx, dy) pixels.

- `flash_cells()`: the cells are where they should be, and the byte order does not matter (the first byte full and the
  third none gives the same as the other order).
- A flash: one frame with a bright patch (3x2 cells turned white) over a steady scene, the frames around it alike;
  also with a little change between the neighbours, as a scene with slow motion has.
- No flash: too small (a patch of 2 cells), too weak (cells that differ by the threshold itself, not by more), or not
  there for one frame only (a patch that appears and stays, one that was there and goes, three frames alike).
- A camera cut: two different scenes, the cut before the frame or after it. Three unrelated pictures (a cut on both
  sides, a fast turn): the neighbours disagree and nothing is judged. A fade: every cell lies between its neighbours.
- Steady motion: the whole scene pans, slowly and fast (1 to 64 pixels a frame). A bright bar as wide as a cell and
  five cells high crosses a flat picture, at every speed and every phase, sideways and down: in the frame in which it
  fills its cells they are brighter than in the frame before and the frame after, which are alike there, and only the
  cells beside them, which the bar leaves and enters, tell this from a flash. The bar where it stands for one frame
  only is a flash: nothing beside it changed. Three squares as large as a cell move diagonally: what leaves a square's
  cell is shared between three cells beside it, so none of those changes by as much as a flash does, and only the
  cells around a cell, compared with what that cell differs by, tell these from three cells that flash.
- The rules cell by cell, on a row of three cells 60 brighter for one frame: a cell diagonally beside the first one 15
  apart between the neighbours (a quarter of 60) hides nothing, and at 16 the first cell no longer counts, and two are
  too few. A cell far from the flash that is as far apart as allowed (`DIFF`) keeps the frame judged; one more
  (`DIFF + 1`, further than a flash must differ by) and the frame is not judged. Neighbours a little apart (more than
  a quarter of the threshold) in the lower rows: `FLASH_MOVING` cells leave the frame judged, one more does not.
  Darker than one neighbour and brighter than the other is no flash, however the thresholds are set.
- Frame by frame, with thumbnails missing: frames 1..9 steady with 5 the flash, found when 6 arrives, as frame 5. The
  flash at 12 with the frame after it without a thumbnail is not judged, at 13 (missing), 14 or later; nor with the
  frame before it missing, nor when the flash frame itself has none and its neighbours are four frames apart from
  older ones in the slots. When only every other frame has a thumbnail and they alternate, no three are in a row and
  nothing is judged. The first frames of a run, and frame 0, which names no frame.

## `tests/foreign_hook_check.py`

The crash report's test for a module another program put into the game (`runtime/main.c` `foreign_hook()`): the
Vulkan layers, hooks and overlays of recorders, overlays and capture tools are named as theirs; the game's own
libraries and the drivers are not, nor the validation layer the game loads itself (`NX2SARIA_VK_VALIDATION`). A fault
on the thread that sets RivaTuner Statistics Server's limit gets the RTSS note whatever the module
(`crash_module_note()`). It builds those functions, cut out of `main.c`, into a test program. No game files; it takes
the path to clang as an optional argument.

## `tests/fov_check.py`

Checks the 1.1.1 lens patch through the normal lifter and code emitter.

## `tests/frame_owed_check.py`

Builds and runs `tests/frame_owed_check.c` on `runtime/vk/vk_frame_owed.h`: a frame's bookkeeping at its present, in
replay order (`runtime/vk/vk_sync.c` `submit()`, and the present path of `runtime/vk/vk_backend.c`). No GPU, no game
files, any Python. The platform layer is linked with what it calls outside itself as stand-ins
(`tests/platform_stand_ins.py`).

The program has one presenting thread, three replaying threads that hold the lock for their replays, a few of them
for 40 ms (`HOLD_MS`), and a present thread that takes each presented frame from the presenting thread (one in waiting
at most, as `present_image()` of `vk_backend.c` hands it over), blits it and then submits under the lock
(`present_main()`), over 20,000 frames. Every frame's bookkeeping must run once and in frame order, never during a
replay, before the body of any replay that took the lock after the frame was presented, and before the present
thread's blit of the frame (`present_frame_closed()`); and no present takes 20 ms (`PRESENT_MAX_NS`: a present that
waited for a long hold would take up to `HOLD_MS`), so the presenting thread never waits for a replay.

Step by step, on one thread first: a present with the lock free runs its own frame's bookkeeping; two presents with
the lock held elsewhere leave both frames to the next holder, which runs them in order; an older frame number runs
nothing again; and the present thread takes the lock only for a frame still owed. `frame_owed_check old` leaves out
the present thread's `fo_close()`, as the change first had it, to show that the blit check sees a blit before its
frame's bookkeeping: it then fails.

The stand-ins in the C file: `frame_end()` for that of `vk_backend.c`, run with the lock held; `try_run()` for
`vk_sync_try_locked()` of `vk_sync.c` with `fo_here()` of `vk_backend.c`; `replayer()` for `submit()` (the lock, the
owed frames through `vk_present_frames_owed()`, then the replay's body); `run_locked()` for `fo_blit_locked()` (the
lock, waiting for it, and what is owed); `present_thread()` for `present_main()`. `slot` holds the frame handed to the
present thread shifted left by one, with the low bit set when it was left owed (`PresentJob.owed`), 0 for none.

## `tests/frame_step_check.py`

Builds and runs `tests/frame_step_check.c`: the step `runtime/pacing/frame_rate.c` gives Ocarina (`ocarina_step()`),
driven through `frame_step_cap_ns()` and `frame_step_present()` with made-up display times and the rest of the
runtime stubbed. No game files: the module layout the runtime includes is a stand-in written to the test's build
folder. With the argument `mean` the program runs with `NX2SARIA_STEP_STEADY=0` and checks the mean of the last five
intervals (the comparison setting: 5 and 30 ms in turn give 15 and 20 ms in turn).

What the Python file checks against `recomp/patches-v111.txt` and lifts for the program (`PAGE` is the patch page of
1.1.1, main+0x4ea0000):

- `check_sites()`: each site that calls a host of `frame_rate.c` (`HOST_SITES`: set, module offset, the original
  word, the words before the bl, which are the original's own where the host comes after it, and the page offset the
  bl reaches) is its listed words and one bl to its page slot, over its original word. `FDIV` is `0x1e201900`,
  `fdiv s0, s8, s0`.
- `solver_functions()`: the solver step count's words at the step setup's first copy (main+0x38aebe4..0x38aebf4; the
  second copy at main+0x38afd10 has the same words): the original's, the set `osolver`'s and the set `osolverfull`'s,
  lifted as the generator lifts them into one C function each, with the `scvtf s1, s1` between them (`0x5e21d821` at
  main+0x38aebec) that every variant keeps.
- `check_follow()` and `follow_functions()`: the floor-carry gate of the three follow-surface modules (set `ofollow`,
  sites main+0x13d704, main+0x13a064 and main+0x389ac20): `mov w8, #0x42c80000` (100.0, `0x52a85908`) and
  `fmov s1, w8` (`0x1e270101`) before the game's `fcmp s0, s1` (`0x1e212000`; s0 is the physics' inverse step) and the
  b.gt to the tenth of the floor's velocity. The set makes the two words `adrp x8, page` and `ldr s1, [x8, #260]`
  (`0xbd410501`); the test lifts the load and the fcmp with x8 set to its page.
- `check_staff()` and `staff_functions()`: the playback staff's note gate (set `ostaff`): after the read of the
  melody's note signal (main+0x3297b50, 0 or 1 in w0) at main+0x3292c84 and main+0x3292ed4, the set keeps the signal
  on the page (+776, through x9 = the page) and clears w0 when the signal was already set the update before. The gate
  is `ldr w10, [x9, #0x308]` (`0xb943092a`), `str w0, [x9, #0x308]` (`0xb9030920`), `bic w0, w0, w10` (`0x0a2a0000`),
  after the set's adrp to the page in x9. The first site keeps its `ldr w3` ahead of the gate, the second its tbz
  behind it.
- `lift_ops()` gives C for contiguous non-branch guest instructions in a fixture.

The stand-ins of the C file: a refresh rate of 144 Hz; a movie switch (`movie_cap_hz()` of `frame_rate.c`); the debug
menu's game speed at normal; frame generation's multiplier (frames on screen per game frame); the newest want and
movie rate RTSS was told of, and whether RTSS holds the game to the want (`runtime/pacing/rtss_limit.c`); the kernel's
clock and its time at the newest host present (0: none yet); one thread, so the mutexes do nothing; the floor probe,
scan and watch off. The battle-music flag latch has its own test (`tests/battle_music_check.py`). The time manager the
patch hosts read their step from (`game_frame_step()`) is made up as the game has it: `[main+0x4c7e890]` holds the
slot, the slot the manager, the manager +0x10 the step; nothing else is read. `present()` is one present: the pacer
asks for the period, then reports the display time; it returns the step in 60 Hz frames. A gap of 1 s starts the
filter again.

The patch sites that cut the step to a whole number are called as the lifted code calls them, the value in s0, the
object in its register, the whole number back in the register the cut wrote (all 64 bits of it): `water()` is
`frame_rate_water_host()` with x21 and w9, `day()` is `frame_rate_day_host()` with x19 and w8. `sim()` is
`frame_rate_sim_host()` as the surfaces' sites call it (set `owater`): after the site's division
(`fdiv s0, s8, s0`, the literal being 1/60 as a float), the update's seconds over 1/60 s are in s0, the lanes above it
zeroed, and the object in x19; then the game's own cut of s0 (fcvtzs) gives the steps. Only s0's low lane may change:
every general register, the other lanes and vector registers and the flags stay as they were. The solver functions
run on the frames per update in s0 and the count (4) in s1 as the game loads them, x8 pointing at the literal 0.001
the original loads, and return w1, the steps. The follow gate returns 1 when its b.gt is taken, which keeps a tenth of
the floor's velocity. The staff gate takes the melody's note signal of this update in w0 and returns whether the staff
moves on; the last signal stays on the page (+776) between calls.

- The pacer's period while Reflex holds the limit (`vk_dlssg_pace()` of `vk_dlssg.c`): 240 fps, 4166666 ns, for a
  game stepping by the clock.
- A 60 fps limit with a late frame and the short one after it every 37 frames: one frame exactly, every frame. The
  240 cap with late frames: never under a quarter frame (the game's own solver step count is 0 there). Unlimited, 5
  and 30 ms in turn: one step, and the clock kept. 200 fps and one 60 ms frame: the step rises by a tenth at most and
  the 55 ms are paid back. A real change of rate, 240 to 120 fps, arrives within 12 frames.
- A movie started at 240 fps: exactly one frame from its first update, late and short frames included, as the game
  shows one picture per update; at its end the step follows the frame limit again within 12 frames.
- A frame longer than 1/15 s slows the game down; the step never leaves 1/480..1/15 s (900 fps, as a quick menu
  without the pacer would run, still gives an eighth of a frame).
- The limit counts the frames the game draws: frame generation's multiplier leaves the pace of a numeric limit and of
  unlimited alone, and divides only the monitor's rate (144 Hz here), which is a rate on the screen.
- RivaTuner Statistics Server holds the limit: the pacer keeps the 240 ceiling while host presents go out, and the
  limit's own period before the first host present and once none went out for 200 ms (a minimised window, a frame
  hold), when RTSS has nothing to hold. A movie while RTSS holds 144: RTSS keeps the play limit (no rewrite of its
  profile at the movie's start and end) and the pacer holds the movie's 60 with its slots; at 40 the want stays 40
  too (`rtss_target` of `rtss_decide.h` decides what the profile gets).
- The water's ripples at 144 fps, a step of 60/144 frame: 64 water objects asked once a frame each run 600 steps in
  1440 frames (the cut gave none), and the day's seconds asked between them keep their own fraction. A step of one
  frame, and of two, is what the game computes, also after a fraction was left; a quarter frame gives one step every
  fourth frame, and what fcvtzs cuts to nothing or saturates leaves the remainder alone.
- The water managers and the floors (set `owater`): at 120, 144 and 240 fps each runs 60 fixed steps a second (the
  cut gave none above 60), beside 64 water objects; at 60 and 30 fps one and two steps an update, as the game counts
  them. A place with many interactive floors, 2 water managers and 30 floors each asking once an update at 144 fps:
  all keep their own remainder and run 60 fixed steps a second (with 8 slots they took each other's and ran none).
- The sound job (set `osound`), over 10 s at each rate: at 60 and 30 fps every phase of every frame runs with the
  game's step; at 144 and 240 fps it runs 60 times a second with a step of one, phases 1 and 2 with phase 0; a skipped
  phase gets 3 in w2. Owed fractions survive a whole step, and a step that is not positive runs and clears them.
- The solver step count: at 60 and 30 fps all three variants give the game's 4 and 8 (also with the steady step a
  little off them); above 60 fps the game's count falls to 1 and to 0, `osolver`'s rounds up (2 at 144 fps, 1 at 240),
  and `osolverfull`'s keeps every update at the 60 Hz frame's 4. Steps a second at 144 and 240 fps: `osolver` 288 and
  240, `osolverfull` 576 and 960.
- The floor carry (set `ofollow`): the physics' inverse step is 60 / (frames per update * time scale); the game cuts
  a floor's velocity to a tenth above 100, which at 60 fps is a time scale under 0.6 and above 100 fps every update.
  With the page's 100 / frames per update the set cuts exactly where the game cuts at 60 fps at any rate above 60; at
  60 and 30 fps the page holds 100 and the set cuts where the game's own words do.
- The playback staff (set `ostaff`): a note signal set over several updates in a row moves the staff once, a signal
  set for one update (every note at 60 fps) moves it as the game does, and a clear signal never does.
- The horse's jump (`check_horse_jump()`), by render step, manager step and jump step: a normal jump at 120 fps and a
  jump allowed to advance more quickly are taken by the patched compare only; an update slowed within a short frame,
  slow motion at the nominal display rate, a time manager that uses a whole step and one whose step is unusable (NaN)
  by neither; a normal jump at 60 fps by both. In the Gohma clamp case, the caller's already restored value in v10 is
  unrelated to the turn.

## `tests/freeze_hold_check.py`

Builds and runs `tests/freeze_hold_check.c` on `runtime/freeze_hold.h`: when the freezing of idle guest regions waits.
No GPU, no game files, any Python.

- The backoff after slow freezes keeps doubling while slow ones come within the quiet time, fast freezes between them
  included, and starts over after the quiet time. Slow freezes 300 ms apart with fast ones between, as after a load in
  Kakariko: the holds double up to 2 s, a fast freeze after the hold keeps the backoff (250 ms there), and one 6 s
  after the last slow one, with a quiet time of 5 s, starts over.
- With quiet 0 a fast freeze starts the doubling over (the rule of 0.10.1).
- The backoff holds the passes until `hold_until`.
- An area load holds the passes while the texture pool loads, and the hold lasts after it ends; an area's load
  (`perf_scene`) 1 s ago holds, one 4 s ago does not (a hold of 3 s). A hold of 0 never holds for loads.

## `tests/fs_file_use_check.py`

A save file's handle and a CloseFile on another thread (`runtime/hle/fs_host.c` `file_of()`, `file_release()`,
`close_file()`): a call under way keeps its host file open, the close happens once when the last call is out, the
guest's handle value is not handed out again before that, and a call after the close gets `FS_R_INVALID_ARGUMENT`. A
call with no close pending takes no exclusive lock, so it ends while another thread holds the lock shared (a Commit's
fsync loop). The host files of the test are stand-ins that hand a closed file's value to the next one opened, as
Windows does with a HANDLE: a read that went on through a closed handle would read the other file. It builds the file
functions of `fs_host.c` into a test program. No game files; it takes the path to clang as an optional argument.

## `tests/gen_emit_gate_test.py`

The emission of `recomp/gen.py` under the memory rule of the compile gate, with a fake pool and a fake free memory
reading; no game files. It needs numpy (the `.venv`).

- `lift_gate()` follows the gate's settings: off gives a reserve of 0, a malformed reserve the default, with a
  warning.
- `pick_jobs()` sizes the pool by the free memory less the reserve. With 19 GiB free, 80% (15.2) is less than free
  less a 3 GiB reserve (16): 10 workers of 1.5 GiB. With 8 GiB free, 80% is 6.4 GiB and free less a 3 GiB reserve 5
  GiB: 3 workers; with a reserve of 0, 80% of 8 GiB gives 4. A job count given is kept.
- `dispatch()` submits a chunk only while the free memory stays at or above the reserve, keeps one chunk outstanding
  whatever the memory, never more than the pool's workers, raises a task's error and gives up on a worker that died
  with its task. With 10 free, 3 per task and a reserve of 4: 0, 1 and 2 outstanding leave 10, 7 and 4 (submit), a
  third would leave 1 (hold). Below the reserve from the start: one chunk at a time, never none. With the reserve 0
  that `lift_gate()` gives when off, even no free memory at all lets every worker have a chunk.
- `emit_all()` retries with half the workers (4, then 2) after a `MemoryError` or a lost worker, on exactly the
  chunks without a result: each chunk's result comes once and no chunk runs twice in the second pool.

`FakePool.apply_async()` runs the task on a timer thread after `delay` seconds; the free memory the pool reports
(`FakePool.avail()`) falls by `per` for each task outstanding; the tasks in `never` are those whose worker dies (no
result, no error, and the pool replaces it, which the test shows as a changed set of worker pids). `run_dispatch()`
gives the sorted results and the `GateStats` of `dispatch()` over tasks on a pool, with the pool's own free memory
reading. `EmitAllTest.lost()` gives the results, the pools and stderr of `emit_all()` over 12 chunks where the first
pool's worker dies with each chunk of `never`.

## `tests/gen_partition_test.py`

The chunking of `recomp/gen.py` on made-up numbers: `estimate_costs()`, `partition()` (own files for costly functions,
the chunk cost and function count limits, a `--limit` slice, every function in exactly one chunk), the per-function
sizes and switch flags `chunk_text()` reports, and the chunk names of `remove_stale()`. No game files; it needs numpy
(the `.venv`).

- At `COST_Q` a switch doubles a function's cost. Large values do not overflow.
- A cost equal to `big_fn` stays in a chunk; one above gets its own file. Reaching the chunk cost limit is allowed.
- `covers()` is true if the `[i0, i1)` ranges are contiguous, non-empty and cover `[0, n)` exactly once.
- `FakeEmitter` has what `chunk_text()` uses of an `Emitter`: `function()` returns (lines, referenced starts) and
  counts its switches in `stats` as the real one does (`jump_tables` per resolved table, `all_label_fallbacks` per
  unresolved computed br); two tables in one function count once. The `sizes` argument of `chunk_text()` is optional.

## `tests/gen_width_check.py`

No 64-to-32-bit narrowing in the files that hold page write generations: `runtime/vk/vk_memory.c`,
`runtime/vk/vk_texture.c` and `runtime/gfx/tex_probe.c`, compiled with the build's flags (`RT_FLAGS` with
`-DRT_CPU_LEVEL=3`, as the build adds it) plus `-Wshorten-64-to-32`, `-Wformat` and `-Wincompatible-pointer-types` as
errors (`clang -fsyntax-only`). The generations are 64 bits; one local, field or format left at 32 bits would bring
the wrap of 2^32 back without a word, and the normal build sees a narrowing only as a silent conversion. The baseline
is zero such diagnostics in the whole of the three files: a narrowing elsewhere in them takes an explicit cast, which
also says it is meant.

Only what is written in the file itself counts (`in_file()`): a header it includes is not one of the files that hold
generations (`simd_directed` of `simd.h` returns the long of `__builtin_expect`, 0 or 1, as an int, which Linux's
64-bit long flags). It needs clang, the Vulkan headers, VMA and the shader headers of `tools/build_shaders.py`
(`runtime/*_spv.h`). No GPU, no game files, any Python.

## `tests/gen_wrap_check.py`

Builds and runs `tests/gen_wrap_check.c`: the page write generations of `runtime/vk/vk_memory.c` across 2^32, 2^40 and
2^63. No GPU, no game files, any Python.

`extract()` takes from `vk_memory.c`, verbatim, into `build/test/gen_wrap/gen_section.inc`: the declarations of
`PoolRec`'s `mgen` and `sgen` (the generations a VRAM mirror and a shadow copy hold), so the stand-in `PoolRec` has
their real width; the section from the "page write generations" banner to the "dirty census" banner; and the readers
of the tables outside that section, `cpu_wrote_since()`, `shadow_pool_clean()`, `lazy_clean()` and `page_dirty()`
(`definition()` takes a definition, not a prototype, up to its closing brace at column 0).

The program starts the generations just below each seed's boundary and runs across it: every compare the texture
checks, the snapshot path, the region check, the VRAM mirrors' lazy fetch, the shadow storage and the verify mode make
must answer as it does below the boundary. The seeds (`SEEDS`): 2^32 - 8, where 32-bit generations wrapped; 2^40 - 8,
where the low 32 bits pass through 0 again, so a truncation to 32 bits anywhere fails there too; and 2^63 - 8.
`gen_next()` must log one line when the count reaches 2^32, and no other.

With `--cfg` the counter starts at 0 and the seed comes from `NX2SARIA_GEN_SEED` (`gen_seed()`), which the first
generation reads: the run must go on from the start expected of that value (parsed as 64 bits, capped at 2^62, a value
that is no number ignored), with the seed's log line expected of it ("-": none). `CFG_SEEDS` has decimal and 0x
hexadecimal values past 32 bits (`0xfffffffff8`, where `strtoul` would stop at 2^32 - 1), one past the cap
(`0x7ffffffffffffff8` starts at 4611686018427387904), values that are no number, 0 and none.

The stubs: no-op locks; a write watch that reports the pages a check marks as written by the CPU (pages the CPU wrote
since the last query over them: `mem_written_pages` resets them, `mem_written_peek` does not); nothing frozen; no pools
in the index (the pool lookup of `snap_widen`). Of the log, the page generation and `NX2SARIA_GEN_SEED` lines are kept
and the others dropped. `gen_t` is the width the source gives the generations (`__typeof__(gen_now)`). The addresses:
`TEX` `0x100000000`, a texture of 4 pages in one 2 MiB region; `OTHER` `0x100200000`, a second one in the next region;
`SPIN` `0x200000000`, an unrelated range other checks hand out generations for; `LONE` `0x100400000`, a page written
exactly at the boundary. The clock is an hour in and the frame count 216000, as at 60 fps. Every `*now` handed back
must grow.

- Before the boundary both textures are loaded (written) and checked. The first write takes the first generation,
  which goes on from the seed; when the generations cannot reach the boundary nothing below can run and the program
  ends there.
- Other checks hand out generations up to edge - 1, and the page written next is stamped exactly at the edge (the
  boundary under test, seed + 8: 2^32 for the first seed).
- After the boundary: a CPU write found by the write watch and one by a host path are both seen. A texture nobody
  wrote since before the boundary stays unchanged, check after check.
- The snapshot path (draw bindings and the texture pool's check) and the region check (`reg_max`). The CPU-side tables
  (what `mirror_sync`, `lazy_clean` and `cpu_wrote_since` read) hold the same stamps.
- A VRAM mirror and a shadow copy synced before the boundary, `TEX` written after it: not clean. Synced again now:
  nothing is newer.
- The log: one line where the count reached 2^32, none at any other boundary; with `--cfg`, the seed's line.

## `tests/gfx_guard_check.py`

Builds and runs `tests/gfx_guard_check.c` on `runtime/gfx/gfx_guard.h`, the checks of the presentation path that must
not trust what they are given: the game's crop cut to the image without int overflow (`runtime/gfx/gfx_present.c`
`present_image()`), the swapchain image list taken whole or not at all (`create_swapchain()`), and how often a
repeating failure is logged. Where the compiler has it, the harness is built with the signed overflow sanitizer, so a
sum that overflows traps. No game files, no GPU.

- `crop_model()` in the C file has the same rules in 64-bit arithmetic, where no int sum overflows: the reference the
  header must agree with. Whatever the crop, the rectangle presented lies inside the image and is not empty.
- No crop, or one with an extent of 0 or less: the whole image, as the game's own convention (even with a negative
  origin in the fields not used). The game's usual crop, the 1280x720 it renders inside a larger image, is used as it
  is. Past the edge: cut there; a negative origin moves to 0 with its extent kept. Outside: the whole image.
- The values that overflowed `cx + cw`: an origin and an extent near `INT_MAX`, which the sanitizer build traps on.
  Every crop around the edges of a small image, and random guest values on the usual sizes. `gfx_rect_inside` with
  values whose sum overflows an int.
- The swapchain image list: the count, then that many images, all of it or nothing.
- The log of a failure that repeats: the first 8, then at 16, 32, 64 and so on: 8 + 28 lines at most in 2^32
  failures.

`scan()` then reads `runtime/gfx/gfx_present.c` and `runtime/vk/vk_texture.c`: no swapchain image query, semaphore,
fence, command pool, command buffer or sampler create there may drop its result (a call that starts a statement), and
a failed `vkCreateSampler` may not end the game (`rt_fatal` in the call's statement or the one after it). It gives the
problems as `file:line: text`. `strip_comments()` blanks comments and string literals and keeps line breaks, so
positions still name their lines. `statement_level()` is 1 when the expression at a position starts a statement or
follows a comma operator outside any parentheses, so that what it returns is dropped: it walks back over whitespace to
the nearest significant character, then over a comma's left side to the statement's start, keeping count of brackets
(inside an argument list or a condition it is 0). `self_test()` shows that the scan catches each kind of fault in a
made-up file and passes its fixed form: six finds (the semaphore, the fence, the image query, the pool after a comma,
two fatal samplers) and nothing in the good file. `log_lines()` finds the format strings of the log lines
(`LOG_LINES`) in the runtime's source, adjacent string literals read as one.

## `tests/gpa_check.py`

Builds and runs `tests/gpa_check.c`: the device address table of `runtime/vk/vk_gpa.c` against a model of what a
shader reads through it, with blocks given back and taken again. The allocator, the device and the completion queue
are stand-ins. It needs the Vulkan headers and `vk_mem_alloc.h`, no GPU and no game files.

The stand-ins: `vk_vma_create_buffer()` and its destroy, the wrappers of `vk_vma_budget.c` that `vk.h` sends
`vmaCreateBuffer` and `vmaDestroyBuffer` to; `table` is the first buffer `vk_gpa.c` creates; a buffer's device address
is its handle far above the GPU addresses the tests map (`dev_of()`, the handle shifted left by 28); the completion
queue runs a callback once the GPU is past its submission (`gpu_done()`: the submission being recorded is issued and
finishes); a store the GPU runs in queue order (`vk_gpa_flip_cmd`) is kept until `gpu_runs_stores()`, as a command
buffer keeps it until the GPU reaches it. `read_through()` reads the table as a shader does (the layout at the top of
`vk_gpa.c`: `L1_WORD` 16, `L1_N` 65536, `L2_N` 4096, 1024 blocks) and gives the device address an address resolves to,
0 for the zero buffer. `expect()` checks that every page of a range reads a buffer, or the zero buffer when there is
none.

- A buffer over three 16 MiB ranges, not aligned to them.
- A mirror takes the pages with the imported buffer as twin, and gives them back. A new mirror takes the pool's record
  over by a store the GPU runs after the mirror's first copy (`mirror_create` and `vk_mem_mirror_prologue` of
  `vk_memory.c`): what is queued before the store reads and writes the imported buffer alone, what comes after it
  reads the mirror and writes both. Evicted, the pages go back to the imported buffer alone, on a record of their own.
  A mirror without a twin: the store names it twice.
- An unmapped range reads the zero buffer at once, and its blocks serve no other range while the GPU may still read
  the old L1 entries. The same range mapped again before the GPU is done takes its block back, and the callbacks of
  the first release then find the blocks in use. Released, taken back and released within one submission, 3000 times:
  one block, whatever the callbacks pending.
- Addresses that climb through 64 GiB, as the game's allocator hands them out: 20 MiB pools 48 MiB apart, eight alive
  at a time, 1300 pools, far more ranges than there are blocks.
- Overlapping imports: the pages of the one that goes return to the other, and their block stays.
- Every block in use at once: the next range is left out and said so once, the ranges after it and the mapped ones
  are untouched, and a block given back serves the range that was left out.

## `tests/gpu_choice_check.py`

Builds and runs `tests/gpu_choice_check.c` on `runtime/vk/gpu_choice.h`: which graphics card `gpu_index` and
`gpu_name` pick when the cards are listed in another order than when the settings were written (a `gpu_index` alone
that lists a card that is not a discrete one gives way to Auto), and the log lines about the choice, the shader
preparation's among them (`runtime/vk/vk_device.c` `pick_device()`, `probe_gpu` of `hw_detect.c`, `row_gpu` of
`runtime/ui/menu.cpp`). It needs a C compiler only: no Vulkan headers, no GPU, no game files. The header is built as
C++ too, as the menu includes it.

The cards are a laptop's: an RTX 3050 and Intel UHD Graphics, which the loader lists twice. `day1` is the order of one
day, in which the NVIDIA card is gpu 1 (where a menu's `gpu_index = 1` was written), and `day2` the order of the
next, with it first.

- `gpuc_has`: the match of `gpu_name` is a part of the name, upper and lower case alike.
- `gpuc_index`: the position goes unchanged while one of the two settings is unset (an older version's file). With
  the settings the menu writes, the position and the card's name, the position holds while it lists that card, and the
  name decides when it does not. A position equal to the count lists no card, and the card after the list (a sentinel
  that has the name) is never read; a name that only the card after the list has is had by no card listed. The first
  card with the name is the one named, also when it is gpu 0 and another follows.
- `gpuc_shown`: the menu's row shows the card the settings pick, which the game picks too. The menu reads an unset
  `gpu_name` as an empty string, not NULL, and an empty name is no name. Position 0 is a position.
- `gpuc_legacy_alt`: a `gpu_index` alone that lists a card that is not a discrete one gives way to the discrete card:
  a position written while the NVIDIA card was gpu 1 or gpu 0 names the Intel graphics in the other order. A discrete
  card the game cannot use is no card to give way to; the first usable one is. Nothing gives way with no discrete card
  at all, or for a discrete card chosen over another discrete card. A software renderer chosen by position beside a
  card gives way; a virtual GPU and a GPU of no known kind are no discrete cards either.
- `gpuc_resolve`: the position of `gpuc_index`, or Auto when the position gives way. The device choice and the
  preset's probe both call it, so a preset is chosen for the card the game takes.
- The lines: the dropped position's. The warning for `gpu_index = 1` on the Intel graphics while the NVIDIA card is
  listed first (the log of the day it was found had no such line, and said nothing of why the game ran on the Intel
  graphics); the same for the environment's `gpu_index = 0` on the Intel graphics listed first. Nothing to say when
  the position lists the NVIDIA card, for Auto whatever it picked, for a `gpu_name` (alone or beside the position the
  menu wrote with it: the player's own choice of the card, so no warning at each start, nor the advice to set the
  `gpu_name` that is already set), with no usable discrete card, with two discrete cards and a choice of one of them,
  and with one card. A software renderer picked by position beside a card is warned about.
- The card choice line at each start: what the settings hold, agreeing or not; the first position past the list reads
  `vk: card choice: gpu_index = 3 alone, which lists no card (3 are listed)`. The line for a position alone that gives
  way to Auto: the position and what it listed, the card taken instead, and how to run on the Intel graphics on
  purpose.
- The shader preparation's line: which card its driver cache is built for, and by what. The cases are the ones the
  laptop's log shows in the update (an index alone that lists the NVIDIA card) and those that would not be: a card
  that is not a discrete one while a discrete one the game can use is listed (the Intel graphics by its name, kept on
  purpose, and an index from the environment); a software renderer by its name while two discrete cards the game can
  use are listed, where the note names the first, as it does for two usable discrete cards with the first at gpu 0; a
  card picked past the list, or before it, gives no line; no usable discrete card, one the game cannot use, and two
  discrete cards leave nothing more to say.
- The laptop's runs, replayed through `gpuc_resolve` and then through a model of the score the pick gives
  (`pick_device()`: the chosen position 100, discrete 3, integrated 2, other 1; the first of the best), which the test
  does not share with them: `gpu_index = 1` alone while the NVIDIA card is gpu 0 gives Auto and the NVIDIA card;
  `gpu_index = 0` alone in the same order stays as written; the same file once the order has the Intel graphics first
  gives Auto and the NVIDIA card; an empty settings file gives Auto; the Intel graphics chosen in the menu (the name)
  is kept.
- A line longer than the buffer is cut, not overrun.

## `tests/handle_census_check.py`

Builds and runs `tests/handle_census_check.c`: the census of the process's handles by kind (`plat_handle_census()` of
`runtime/platform/win32.c`) sees the handles it is asked about. A census, then 20 events and 5 sections made, a census
again (Event +20, Section +5, its total within 2 of the process's handle count), the handles closed and a third census
back at the first figures. Windows only (elsewhere the program says it skipped): the Linux census groups file
descriptors, which this check does not make. No game files, no GPU.

## `tests/hang_watch_check.py`

Builds and runs `tests/hang_watch_check.c`: the decisions of `runtime/diag/hang_watch.c` on made-up readings a second
apart (the time, the frames shown so far, focus, and the GPU's issued and done values), with stand-ins for what it
reads and calls. No game files, no GPU.

- No report while the frames and the GPU move, while the window is not in front or minimized, or while the process
  ends. One report for a stall of the frames with the window in front, another after progress and a new stall. One
  for GPU submissions not done, whatever the focus; none for an idle GPU (nothing outstanding), also on a fresh watch.
  At most three a run.
- A report writes the log out with `logq_flush` and never with `logq_sync`, which would turn the log thread off.
- `static_checks()`: `runtime/hle/kernel.c` logs the guest threads' waits (`kern_wait_report()`, `wait_watch()`) only
  after it released the kernel lock, in the wait report's walk and its two callers: a log call can wait for room in
  the log queue, and a stalled log thread would then hold every guest thread.

The platform layer is linked with what it calls outside itself as stand-ins (`tests/platform_stand_ins.py`).

## `tests/heap_lazy_copy_check.py`

Builds and runs `tests/heap_lazy_copy_check.c`: `runtime/memory.c` `mem_copy_readable()` with the lazy heap
(`NX2SARIA_HEAP_LAZY=1`), where a heap page nothing touched yet is still reserved. The copy holds `frz_lock`, which
the fault on such a page (`heap_lazy_fault()`) waits for on the same thread. Windows only (the lazy heap is):
`memory.c` is built with the Windows platform layer and its real fault handler, the rest of the runtime stubbed. No
game files: the module layout the runtime includes is a stand-in written to the test's build folder.

Each copy runs on a worker the main thread gives 3 s, as the replay (`mirror_capture` of `vk_memory.c`) or a present
(`frame_rate.c`) makes it: before the fix the first one hung there. The program exits 1 on a failed check and 4 when a
copy never returns.

- A page nothing touched is copied as zeros, and left reserved.
- A range from touched pages (a 64 KiB run committed by a host access, then written) into untouched ones: the written
  bytes, then zeros.
- Past the heap's end the pages are not the heap's: left out, without a fault.
- Several ranges in one call, as `frame_rate.c` batches them.

## `tests/heap_retry_check.py`

The model heaps' retry: the patch set `heap_retry` (`recomp/patches-v111.txt`) and `runtime/patches/heap_retry.c`. No
game files: the module layout and the patch page are stand-ins written to the test's build folder.

The set's four sites are lifted by `recomp/lifter.py` as the generator lifts them (`SITES`: the original word and the
words of the replacement, a kept original or the page slot called). main+0x436910 (`0xa90d577f`,
`stp xzr, x21, [x27, #0xd0]`) and main+0x436db4 (`0xa90b5b7f`, `stp xzr, x22, [x27, #0xb0]`) keep their stp, a store
that writes no flags and calls nothing, and then call the patch page's +0xed0. main+0x436f18 (`0x94032c4e`) and
main+0x437094 (`0x94032bef`) call the page's +0xec0 in place of the game's `bl main+0x502050`, which the replacement
takes over. The page's place is checked against another set's call of it: `wide_bars` calls its +0xef0 from
main+0x774068. No other site of any set calls the two slots, and only `heap_retry.c` registers them, naming them
through `HOST_CPU` and `HOST_GPU`.

`tests/heap_retry_check.c` builds `heap_retry.c` with the rest of the runtime stubbed and calls the two host functions
as the lifted sites call them, over a chain of stand-in heaps (a model's full heap, its parents, the root) whose
tryAlloc and the game's GPU memory allocation give memory while a heap has room. Its modes: `main`, `test` (the test
refusals), `shipped` (the refusals left as shipped), `off` (the set switched off), `mismatch` (a main whose word at a
site is not 1.1.1's: a nop, `0xd503201f`, where 1.1.1 has the call of main+0x502050 at main+0x437094) and `no-set` (a
build without the set).

The stand-ins: the test's main base is a buffer standing in for main's image up to the sites, with 1.1.1's words at
the four sites, which `heap_retry_start()` compares. A heap has its vtable at +0, name at +0x20, memory at +0x28, its
size at +0x30 and its parent at +0x38; tryAlloc is the vtable's +0x30. The chain is `RootHeap` (1 GiB at
`0x100000000`), `ResourceHeap` (16 MiB inside it, at `0x110000000`), `Model/Npc_Test.bfres` (64 KiB inside that, at
`0x110800000`), and `Elsewhere`, which no parent holds. `mem_readable` reads the stand-in stack frame and requests as
unreadable, as the game thread's stack once did in a run. The stand-in GPU allocation takes the request's +8 heap (the
default heap for none) and count * size of one bytes, and returns 1 on success, 0 leaving nothing allocated; a GPU
request has its count at +0, heap at +8 and what it is for at +0x20. The stack of main+0x4366e0 holds its x1 at +0x38,
and that x1 has the heap at +8. A call gives up the caller-saved registers (x0-x18 are overwritten); the
callee-saved registers, x30 and sp must be kept.

- A block the model's heap gave: nothing changes, nothing is logged. The model's heap full, its parent full, the root
  has room: the root's block, two levels up. The other CPU site has its size in x22. A return address that is not a
  site is left alone.
- GPU memory: the game's call fails in the model's heap and its parent gives it; with no parent having room the game's
  default heap gives it. The request's heap is put back afterwards. Success at once, a request without a heap (the
  game's own check comes after the call) and a zero count: one call each, nothing logged.
- No heap has room: a fatal error naming the heap, in place of the write to guest address 0x8, with the request's heap
  put back before it too. A parent whose memory does not hold the child's ends the climb there (one line, said once),
  then the fatal error; an unreadable parent pointer ends the climb as the root does.
- The log budget: 32 retry lines in all, then a total every 64th block of a site ("model buffer at main+0x43690c: 192
  blocks" after the 64th, 128th and 192nd).
- The test switches: the model's heap gave the block and the test refuses it, so the parent gives another (the first
  stays allocated); with the refusals used up the next block is the game's. The GPU memory's refusal: the game's call
  is not made for the model's heap, the parent's is.
- The start and end lines.

## `tests/host_probes_test.py`

The build's host probes on Linux, over fake `/proc` and cgroup files and a fake `systemd-inhibit`. No game files;
standard library only (the worker count test also needs numpy, as the lifter does).

- `AvailableRamTest`: `tools/hostmem.py` `available_ram()` is `MemAvailable`, capped by the cgroup v2 memory limits
  (`limit()` writes the `memory.max` and `memory.current` of a cgroup, by its path relative to the mount). Unlimited
  cgroups are not a cap; the root cgroup has no `memory.max`. A cgroup outside this namespace reads as `/..`: its `..`
  parts are skipped, never followed above the mount (a `memory.max` beside the mount must not be read). Of nested
  limits the tightest counts: 9 GiB with 6 in use (3 left) under 30 GiB with 2 in use (28 left) gives 3 GiB. In a
  cgroup namespace the process is at `/` and the mount is its own cgroup. Without `MemAvailable` (a kernel before
  3.14) the answer is None.
- The lift's worker count follows it (`recomp/gen.py` `pick_jobs()`): a 32-processor machine under an 8 GiB cgroup
  gets 4 workers (8 GiB x 0.8 / 1.5 GiB, which leaves more than the 1.5 GiB reserve of an 8 GiB machine), not 32.
- `CpuTagTest`: `tools/make_game.py` `cpu_tag()`, the compile key's processor part, reads the same three features as
  its Windows branch: SSE4.2, AVX2 and AVX-512F; a `/proc/cpuinfo` that is not x86 gives `any`. The same lift, runtime
  and kit compile again on a processor without AVX2.
- `KeepAwakeTest`: `keep_awake()`, the sleep lock, with a `systemd-inhibit` stand-in first on `PATH`. It asks for
  `--what=sleep` (not idle: the screen may still blank and lock, as on Windows) with `--mode=block`; the lock is still
  held afterwards and does not end by itself; a second call takes no second lock; and closing its stdin, which is what
  the end of this process does, releases it.

## `tests/index_rewrite_check.py`

Builds and runs `tests/index_rewrite_check.c` on `runtime/vk/vk_index_rewrite.h`: the index lists an indexed quad list
or line loop is drawn with (`op_draw_idx` of `vk_exec_draw.c`, `NX2SARIA_QUAD_INDEX`). No GPU, no game files, any
Python.

- 16-bit quads: two triangles per quad, from the quad's own indices, the same split as the non-indexed rewrite. A
  trailing partial quad is not drawn.
- Byte and 32-bit indices are read at their own size. An unaligned source (a byte offset into a pool) is read without
  alignment assumptions.
- A line loop comes back to its first index.

## `tests/ipc_session_check.py`

Builds and runs `tests/ipc_session_check.c`: `runtime/hle/ipc.c` with `runtime/hle/kernel.c` linked, the platform
layer linked (with what it calls outside itself as stand-ins, `tests/platform_stand_ins.py`) and the rest of the
runtime stubbed. No game files: the module layout the runtime includes is a stand-in written to the test's build
folder. The stub of `mem_commit_checked()` stands for that of `runtime/memory.c`, with which `kernel.c` commits the
main thread's stack, which these tests never start.

| Mode | What it checks |
|---|---|
| `session` | a request whose handler (a service whose command 7 blocks until released) is still running while another thread closes the session's only handle: the session's close callback, which frees the service's state, runs once, after the handler returned, never while it runs. A request on an open session leaves it open; the CloseHandle afterwards frees it |
| `tmem` | `kern_handle_copy()` hands out the transfer memory's fields while the handle is open, and 0 after its close |
| `domain` | appletOE as a domain: 100 rounds of a getter (the common state getter) and its CloseVirtualHandle reuse one id; a closed id is answered with a bad-handle result; the domain's own id (1) is never closed; an object with a handle of its own (CopyFromCurrentDomain) keeps the domain after the client closes it, and the last handle frees all; a domain whose client closes it with objects that have no handle of their own frees all of them at once |

`dom_call()` makes a CMIF domain request on a handle: domain command 1 (send) or 2 (close) to an object, with a
command. The result and the id (the reply's first data word) are filled for a send; a close has no answer. The
request's size counts the alignment pad, the domain header and the CMIF header; these answers hold no handles, so the
reply's words are at 8 and its payload at 16.

## `tests/jumptables_check.py`

Checks `recomp/jumptables.py` on every `br` in the game's modules against an emulation oracle.

The oracle (`Oracle`) is an emulated jump-table resolver: it finds `cmp #N; b.hi/b.hs` before the br (or a
`b.ls/b.lo` to the dispatch block), replays ADRP/ADR/ADD-imm from the function entry, then single-steps Unicorn from
the bounds check to the br once per index. Functions are [seed, next seed) over the module's seeds plus BL targets
(`function_starts()`).

It reports per module: br sites, resolved by the resolver and by the oracle, agreement, `is_computed` sites left
unresolved, tables whose targets leave the function, an independent check of hoisted table bases (`hoisted_base()`),
PLT stubs (`br x17` after the GOT load) that must stay a plain call, and resolver time. A disagreement is counted as
"oracle wrong" only with independent evidence (`oracle_wrong()`). Samples of every disagreement, unverified base and
unresolved computed site are printed with disassembly. Output also goes to `build/logs/jumptables-check.log`, and one
row per table-like br to `build/logs/jumptables-<module>.tsv`. It exits with 1 if any disagreement is unexplained, a
hoisted base is not verified, or a PLT stub counts as computed.

- `Oracle` keeps, per table: `start` and `cmp`, the first instruction of the bounds-check block and the bounds cmp;
  `guard`, for `Oracle._taken()`, the `b.ls/b.lo` into the dispatch block; `count`, the indices the bounds check lets
  through; `exclude`, the indices the emulation skips (those `oracle_wrong()` found excluded on every way in); `watch`
  and `seen`, an (address, register) pair whose register is recorded when the emulation reaches the address.
  `Oracle.jump_table()` returns the targets or None, and leaves the index set point and the registers set there in
  `set_at` and `regs`. `Oracle._index_regs()` follows the index back through AND (imm) (`0x12000000` under
  `0x7F800000`), MOV as ORR with zr (`0x2A0003E0` under `0x7FE0FFE0`) and UBFM/SBFM.
- `_regno()` gives the X register number of a capstone register name (w and x views), 31 for sp and zr, None for
  others. `_writes()` gives the X register numbers an instruction writes, by capstone (w and x views folded), plus the
  call clobbers; for cmp, cmn, tst, ccmp and ccmn it gives none, since capstone lists the first operand of these
  aliases as written.
- `Reaching` holds the reaching definitions of single registers over the resolver's blocks, with capstone's write
  sets. `Reaching.at_blocks()` maps a block start to the definitions of a register reaching it, `entry` standing for a
  value from outside the function; `Reaching.at()` gives the definitions reaching an instruction; `Reaching.values()`
  the constants a register holds at an address, following adrp/add/adr and register copies, or None if one is not
  constant (a definition already being evaluated, a cycle of copies, adds no value of its own).
- `FunctionCheck` is the resolver's final tables and blocks for one function, with reaching definitions on demand.
  `table_load()` is the last register-offset load in [s, br): the table read of a dispatch block.
- `hoisted_base()` returns the base register, its single constant at the dispatch block or None, and a verdict:
  `local` when the dispatch block writes the base; `verified` when every definition reaching the block (over the
  resolver's blocks and table edges, with capstone's write sets, through register copies) is an adrp/add or adr of one
  and the same constant and the resolver's fact does not name another; else a reason.
- `_branch_target()` is the target of a direct b, b.cond, cbz/cbnz or tbz/tbnz. `_ways_in()` gives the direct branches
  of the function into a range, or None when the range is entered otherwise too: by falling through into its start, at
  the function entry, or as a target of another table. `_Ways` holds the direct branches and fall-throughs of one
  function, with the addresses control may enter from elsewhere; `_Ways.preds()` gives (branch or last instruction,
  insn, taken) for every way into a block.
- `_edge_holds()`: whether the way out of a block over the branch at its end can be taken with x<reg> = k there. It is
  False only when the branch condition is a function of x<reg> alone (its slice, back to the last write of x<reg>, is
  plain arithmetic and flag setting on x<reg>), evaluated by Unicorn; True otherwise. The instructions an index slice
  may hold are integer arithmetic that reads only general registers and writes the flags at most (`_FLAG_SETTERS`; the
  flags-reading forms adc, sbc, csel, ccmp and the like stay out). Its reads leave zr out and give up when sp or a
  non-general register is read; for cmp, cmn and tst the first operand is added, as capstone lists it as written, not
  read.
- `_way_live()`: whether the way over a branch can carry x<reg> = k: its edge holds and its block either writes
  x<reg> or is entered with k (`_reaches()`, over at most `_DEPTH`, 4, predecessor blocks above the dispatch region).
- `excluded_indices()`: the indices below the count that cannot reach the br, because no way into the dispatch region
  [`oracle.start`, br] (its first instruction and every branch target inside it) can carry them. The index is the
  register of the oracle's bounds cmp, which no instruction from such an entry to the br may write. Ways in are the
  module's direct branches and fall-throughs by capstone; the function entry, exception landing pads and every
  resolver table target count as entries from anywhere. Branch conditions are evaluated only where they depend on the
  index alone. The region falls through from its start to the br (`Oracle._fallthrough()`), or is the bounds-check
  block whose `b.ls/b.lo` is the only way into the dispatch block (`Oracle._taken()`); in the second case that branch
  must be the block's only way in. Conditions inside the region are ignored.
- `oracle_wrong()`: why the oracle's targets for a br are not the table's, from evidence independent of the resolver,
  or None. The index the table is read with does not depend on the registers the oracle sets (it comes from another
  register or is reloaded from memory, which the oracle leaves at its scratch value); or the table base the oracle
  uses (its replay of ADRP/ADR/ADD-imm from the entry) is not the one constant that every reaching definition gives;
  or the oracle enumerates an index that no way into the dispatch region lets through (a case peeled off before the
  table by `cmp wI, #k; b.ne`, `cbnz wI` or a range check on wI, possibly some blocks up), and without it the oracle
  gives the resolver's targets.
- `plt_stubs()` gives the addresses of `br x17` in `adrp x16; ldr x17, [x16, #o]; add x16, x16, #o; br x17` stubs.
  `window()` is the code before the br, and the bounds check that branches into its block if it sits elsewhere.

## `tests/launcher_test.py`

`launcher/` and the bookkeeping of `tools/make_game.py`, without game files or Windows. It wants Python 3.14 for the
zstd tests; the window test needs tkinter, numpy and a display. No test here reaches the session reports' server or
opens their question over a screen (`launcher/telemetry.py` `server_url()`); `TelemetryHiddenTest` sets its own.
`GAME_EXE` is the built game's file name on this system (`ocarina.exe` on Windows, `ocarina` elsewhere).

What it covers, in the order of the file:

- versions and settings;
- the toolchain: the package database, dependency resolution and an install from a local fake repository;
- net: downloads that resume, check their sha256 and stop on cancel; the certificate checks against a local HTTPS
  server with certificates the openssl command makes (an expired one, an unknown one), the clock comparison;
- the feed: GitHub releases (stable, preview, private with a token) and a plain manifest URL, served locally;
- the updater: staging a release, switching, rollback, pruning, refusing unsafe archives;
- background updates: a release staged beside the running version without switching to it, the ready marker, a staged
  release found and installed after a restart, no install while the game runs, the install once it exited, nothing by
  itself with Install Updates Automatically off, a failed staging tried again after the next check;
- gamefiles: the `.bfotf` font wrapping and zstd dictionaries, on a font of this system made into a fake RomFS;
- the button art of a release: the files `tools/prompt_art.py` copies, and the packages and tools the prompts need;
- `make_game.py`: a synthetic `.nup` through the check, exefs and romfs steps with `--events`, skipped steps on a
  second run, `--rebuild` without the game files, the shaders key following the translator the build uses (the kit's
  `bin/` or a source checkout's `build/xlate/`), the prompts step with a stand-in prompt tool, the object cache of the
  compile step with the system's C compiler, and its windres call under folders with spaces (a script in windres's
  place);
- jobs: an install driven by a fake `make_game.py`, with and without the prompts row;
- Play on Linux: the game started in its own session from its folder, its Restart's copy adopted and followed, a line
  in `launcher.log` for every Play that does not start, and the window hidden while a fake game runs (xvfb-run);
- Add to Steam: the binary shortcuts file read and written back unchanged, a shortcut added and updated in place,
  Linux native and Flatpak roots, sandbox fallback, safe backups, process detection and refusal while Steam is open;
- the look: the colour schemes with the Custom one, its colour square and a typed hex code, the background picture's
  fitting below the title band and kept copy, the sound effects (low by default, only the game's own select and back
  sounds, decoded from a made-up `.bwav`);
- the window: `--selftest` draws every screen (skipped without a display).

### The helpers

- `Server`: a local HTTP server for a folder, with extra routes (path -> (status, headers, body)) and Range support;
  HTTPS with `cert` (a PEM file holding the certificate and its key). HEAD answers a redirect to https, as a server
  that serves only HTTPS does. A client that stops reading (the cancel test) is not an error of the server.
- `make_cert()`: a self-signed certificate for 127.0.0.1 valid from `start` to `end` (epoch seconds), with its key, as
  one PEM file in a new folder; made by the openssl command (its `ca` command takes any dates, its `req` command none
  in the past). `CountingContext` is a client `SSLContext` that counts its handshakes.
- `tar_bytes()`: a tar (zstd-compressed when the standard library has zstd, else gzip) of {name: bytes}.
- `fake_repo()`: a pacman repository with the ten root packages (each holding one of `make_game.py`'s required
  files), one dependency and one name provided by another package. `fake_fast_repo()`: the CLANG64 packages
  `toolchain.FAST_PACKAGES` names, in `folder/clang64`.
- `compiled_sleeper()`: an executable compiled from a C program that sleeps for a number of milliseconds and exits
  with a code, with the ucrt64 clang or a clang on `PATH`; False without one. It stands in for the game under a name
  that no game runner watches.
- `PLAYER_INI`: a player's settings file as a report showed it (an AMD card, 0.7.1): what the game's menu wrote, three
  lines added by hand that are no settings of the game (`async_compute`, `vram_safe_mode`, `exception_handler`: the
  player took the first for the fix of a crash and asked for it as a default, because "the configuration file gets
  overwritten on updates"), and a comment. It is saved as an editor on Windows saves it: CRLF line ends, no line end
  after the last line. Nothing in an update may change a byte of it; `PlayerIniTest`, `BackgroundUpdateTest` and
  `MakeGameTest` each run their part of one.
- `write_nup()`: a `.nup` (the format of `tools/nup.py`) with one base package: exefs {name: bytes}, romfs {path:
  bytes}. `made_up_game()`: a game folder as `tools/make_game.py` leaves it, made up without game files: an ExeFS
  whose main is a header with the game's build id, a RomFS image of zeros, the executable unless told not to, and a
  `state.json` in which the game-file steps are current and every other step named has the key of an earlier kit.
- `FAKE_WINDRES` stands in for `windres.exe`: it records its arguments, working directory and temporary folder, opens
  the `.rc` and the files it names from the working directory as windres does, and leaves two temporary files behind
  as windres does. `FAKE_PAD_PROMPTS` stands in for `tools/pad_prompts.py`: it counts its runs in the game folder and
  writes every overlay folder's manifest, printing the line the real tool prints for each and its closing report.
- `prepared_list()`: a version 2 `shader-prepared.bin` (`runtime/gfx/prp_file.h`): the head, what it was prepared
  from, then records.

### Versions, settings, uninstall

- `VersionTest`: release candidates compare as the release notes' rules say ("Version numbers"): numbers compare as
  numbers.
- `GameIniTest`: the game settings the launcher switches (`launcher/gameini.py`), read as `runtime/cfg/cfg.c` reads
  the settings file. Only the key's line changes: the byte order mark, the line ends and a line that is not UTF-8 stay
  as they were, and quotes, prefix and case are read as the game reads them. As in `cfg.c`, `key =` leaves the setting
  as it was, so the game keeps the earlier value and a change goes to that earlier line. A settings path that is
  there but cannot be read (a folder) is not rewritten.
- `PlayerIniTest`: the launcher's setting adds one line to `PLAYER_INI`; a hand-added line reads like any line, and
  the game ignores it. A comment after a value is read as the game reads it (`cut_comment()` of `cfg.c`): for a key
  whose row has a set of values (`choices=True`), a blank followed by `;` or `#` starts a comment, `key = ; note` has
  no value, and a value in double quotes keeps its text; free text (the mods folder and the lists the launcher writes)
  keeps the whole value. `tests/cfg_parse_check.py` has the same lines for the game. A key's line that has only a
  comment has no value as the game reads it, so the change goes on the line before; a changed line is written again
  whole, as the game's menu writes it.
- `RebuildArgsTest`: Rebuild the Game compiles the game's code for this processor's level (`--retune`); an update
  keeps the level its objects were compiled for, so that it compiles only what changed (`tools/make_game.py`
  `lifted_level()`).
- `SettingsTest`: the Settings page of a new install, and what Default Settings goes back to: updates checked at
  start, the launcher hidden while playing, the build cache kept (an update recompiles only what changed), Stable. A
  player's own choices stay across starts and updates. A file saved by an editor with a BOM reads as written; one with
  a typo keeps a copy before a save replaces it.
- `SettingChangesTest`: every saved change of a setting leaves `setting: <key> <before> -> <after>` in `launcher.log`,
  whichever row made it, once (a save with nothing changed logs nothing; the next start compares with the file): a
  log that showed only the start of a window that ran a minute and a half said nothing of what the player did in it.
  The update check's bookkeeping is not logged, and the GitHub token only as set or empty.
- `UninstallTest`: the plan keeps the game and what is not ours; the toolchain moved to `C:\NX2Saria`, and that folder
  goes only when nothing else is left in it. The Windows script is tried under a root with spaces and a character
  outside ASCII.
- `LinuxUninstallTest`: uninstall of the AppImage's install (`launcher/platform/linux.py` `uninstall_plan()`,
  `uninstall_command()`): what `tools/appimage_install.py` put in the launcher's folder and its menu entry go once the
  window has closed; the game folder, its saves, the launcher's logs and what is not ours stay. Its `install()` makes
  an AppImage install in a folder whose name holds a space, a quote, a dollar and a percent sign, its menu entry as
  the AppImage writes it (quoted as `_write_desktop` of `tools/appimage_install.py` quotes it), another install's
  entry, a game folder inside it and a file of the player's. A menu entry write the AppImage left half done
  (`nx2saria.desktop.new`) goes too; another install's entry does not. A "game folder" that holds the launcher or the
  home folder, or a drive's root, stays; `XDG_DATA_HOME` outside the home folder and the same folder through a link
  are covered. The game folder when it stays and the dump are kept even inside a folder Uninstall deletes: a game
  folder in `data/` or `toolchain/`, a dump inside the game folder being removed. The script waits for the launcher (a
  `sleep 1` stands in for its process) and then deletes. `winsys.uninstall` from a process that then exits, as the
  window's Uninstall does: the delete starts on its own, and with Remove the Built Game the game goes too; the logs
  stay and say what happened.

### Toolchain, net, feed, updater

- `ToolchainTest`: a Python without `compression.zstd` and `zstandard` is named at startup, with the pip line for a
  checkout, the Python's path written short in the log. Resolution takes a dependency with a version constraint and
  resolves `vulkan` to its provider once; the install leaves documentation out; with cached package files a second
  install downloads nothing. CLANG64's clang goes into `msys64/clang64` when its version matches UCRT64's:
  `clang.exe`, the DLLs and its headers only.
- `NetTest`:
  - A connection reset (WinError 10054 during Gather Build Tools) or a server error is tried again; a 404 is not.
    When every try failed the last error goes up.
  - A download resumes from a part file left by a cut connection and checks its sha256.
  - `progress` gets the whole file's size as its total: the expected size, else the length the server names plus what
    the part file holds, else the bytes so far. A resumed answer without a length gave the part file's size as the
    total, so the bar passed 100%.
  - A part file is resumed only for the file it was begun for (`f.bin.part.of` names it by the expected sha256). One
    begun for other bytes under the same name, as when a release is published again, is deleted with a log line and
    the download starts from the beginning; resumed, it ended in a sha256 error that read as a damaged download. The
    same file from another server, and a part without a note (left by a launcher that wrote none), are resumed.
    Nothing stays beside the finished file; a cut download says what its part is of; `updater.prune` clears away a
    note left behind.
  - The free space is asked before a download: one whose rest does not fit the disk stops before a byte is written
    (with a known expected size no request is made; else the server's length is taken), says how much is needed and
    how much is free, and keeps its part file for when there is room. A disk that fills in the middle is named as full
    at once instead of being tried four times. A disk that cannot be asked refuses nothing. The same before an unpack:
    asked before the first download, nothing unpacked, "(91 MB needed, 2 MB free)" for 5 + 50 MiB and the margin.
  - Plain HTTP is refused before any connection; the Authorization header is not sent on to another host (the same
    server under another host name); more than the expected size is refused.
  - `net.server_clock` takes the Date of the redirect to https, which is not followed.
- `CertificateTest`: an expired certificate in the trust store gives the player's "certificate has expired" at once,
  not retried (the answer would be the same), with the advice the server's clock calls for; the clock is asked over
  plain HTTP and no request gets through. A certificate the first check refuses and the second accepts downloads, and
  the second goes first for that host from then on.
- `FeedTest`: the GitHub channels (a release that lacks a manifest is passed over: stable gives 0.3.11 while 0.3.12
  has none). GitHub's answer to a request over the hourly allowance carries the time the allowance is whole again: the
  message names it, in each of the header's forms, the log gets one line for the failed check, not two tracebacks, and
  a refusal without the headers keeps the old advice. A URL feed and an update, with a rollback that finds nothing
  further back. Unsafe or wrong archives: an archive that says 1.0.1 under a manifest for 1.0.2.
- `BackgroundUpdateTest`: an update fetched while the launcher runs (`updater.stage`, `jobs.StageJob`,
  `jobs.BackgroundUpdate`): staged beside the running version without switching to it, and installed only when the
  game is not running, by itself only with Install Updates Automatically on. The releases come from the local server
  through a URL feed. Its helpers: `installed()` makes an installed launcher's folder (not a checkout) running a
  version on the Python `py-1`; `publish()` a release on the local server, with the id of the Python it brings, or
  damaged (its manifest gives the wrong sha256 for the app archive); `context()` the launcher starting (again) in the
  installed folder, with its settings and its log; `stage()` calls `poll()` as the window's timer does, waits for the
  staging and returns how it ended. `self.running` is what `build.game_running` answers: a copy of the game the
  launcher did not start.
  - The release is unpacked beside the running version, its Python in its own folder rather than `python.next` (which
    the stub would swap in at the next start, under the running version), and nothing is switched or restarted. It is
    staged once per check; the next check finds it staged and downloads nothing; one the feed no longer offers is not
    installed; an update's cleanup keeps a newer staged release.
  - Only a release whose staging finished (`staged.json`, written last) and that is still whole installs: not one
    unpacked as an interrupted staging leaves it, not the version that failed to start, not one whose Python is gone,
    not one whose marker is cut short. A pre-release counts only on the Preview channel; staging it removes the staged
    release it supersedes.
  - A staged release outlives the launcher: the next start finds it without a check and installs it: its Python to
    `python.next` for the stub, `current.txt`, and the restart that finishes in `ContinueUpdateJob`. One the stub
    rolled back from is neither staged nor installed by itself again.
  - The launcher's part of an update (the release staged beside the running version, the switch to it, the cleanup
    after the rebuild, which is what `ContinueUpdateJob` ends with) changes nothing in the game folder, also in its
    default place inside the launcher's folder: the player's settings file is the same bytes afterwards, and an
    uninstall that keeps the game does not name it.
  - A copy of the game started elsewhere, the copy the launcher started, and a download under way each hold the
    install back, also one the player asked for.
  - Install Updates Automatically on (the default): a release staged during play installs once the game has exited.
    Off: the release is still staged in the background, and only selecting Updates installs it (also once selected
    during the download).
  - A staging that fails is logged, never raised, leaves nothing to install, and is tried again only after the next
    update check.

### Game files and the button art

- `FontTest`: registered as a font, Windows keeps the file open: decoding it again must not fail on the unchanged
  file, and once the registration is given up a file that went bad is replaced.
- `PromptArtTest`: `make_art()` makes every picture `tools/prompt_art.py` names, the pads in one pack's folder and the
  keys in another's, with two keys and two pictures the tool never reads.
  - A release takes the pictures `pad_prompts.py` reads and every key, from folders that hold the art's sets or are
    those sets; a set named itself wins over one inside a folder named; the same set twice at one rank, a set missing
    or a picture missing stops it.
  - The repository's `prompt-art` holds every picture `pad_prompts.py` reads and nothing else but `SOURCE.md` and
    `SHA256SUMS`, each picture as `SHA256SUMS` lists it, and `SOURCE.md` names the pack, its licence as its page
    states it, the download's SHA-256 and an archived copy of the page. A release takes this art unless told otherwise
    (the folders named, or none with `--no-prompt-art`), with `SOURCE.md`, whose Source line is what the build records
    (issue #15: releases without art); art without a `SOURCE.md`, or without its Source line, says so.
  - `tools/check_content.py` lets a picture of `prompt-art` in only as `SHA256SUMS` lists it: one not listed, or
    changed, is refused; `SOURCE.md`, `SHA256SUMS` and pictures elsewhere are not its business.
  - The release's Python has every package `tools/pad_prompts.py` imports, and the launcher release and the build kit
    carry every tool it imports. Every tool `make_game.py` imports is in the launcher release and the build kit,
    unless the import sits in a try that catches `ImportError` (`build_shaders`: a source checkout's step the
    release's prebuilt headers do not need) or only runs on Linux (`build_game`, whose kit carries it). An import that
    is neither stopped every player's update at its compile step.

### `MakeGameTest`

`tools/make_game.py` on a synthetic game. The made-up build tools of the fixture are empty files, so a build in this
process does not try them (the test of the build tools does, with a stand-in compiler), and the count of the graphics
drivers' cache folders (`say_driver_caches`) is switched off, so that a PC's real caches are not scanned twice by
every step of these tests (the tests of the driver cache folders use the real function). `main_with()` runs
`make_game.main()` in this process with given step functions in place of the real lift and compile (they need real
modules and clang), up to the compile step (the shaders step needs the shader translator), and returns the SystemExit
code or 0; `main_rebuild()` does the same for `--rebuild` and also returns what it printed. `fake_compiles()` makes a
number of sources of growing size and a stand-in for the compiler process (`subprocess.run` in `make_game.py`) that
writes the object and its dependency file; one named source's compile can be made to wait for an event.

The steps and their keys:

- A second run with the same input skips the exefs and romfs steps; `--rebuild` needs no game files, checks the
  unpacked ExeFS and keeps both steps; the exefs and romfs keys do not depend on the kit, the lift key does. The lift
  reads nothing of the RomFS step, so it does not follow it.
- Cleaning the cache alone does not require a compile; a lift that was interrupted (the layout is there again, but the
  lift did not finish) is run again.
- A first build packs the RomFS image while the lift and the compile run; the compile gets the running step and waits
  for it only for the RomFS index. A RomFS step that fails while the lift runs stops the build.
- An install cut short while the RomFS image was packed, its lift already done: the next run packs the image again
  and compiles, but keeps the lift, which reads only the unpacked ExeFS (the lifted code is there, `lift_gone`). The
  ExeFS step still makes the lift run again: the lift reads what it writes.
- An update whose lift ran again drops the compile step's record before the compile runs. When the compile then stops,
  the folder still holds the executable of the build before and the unpacked game files: the launcher counts it as
  installed (`build.installed`), not as a folder to install into with the dump, with no finished build on record; the
  shaders' record stays as the lift left it, since the step's key names what it depends on (`UNAFFECTED`). A first
  install that stopped there has nothing to play.
- Before its first long step the build adds up what its steps will write into the game folder and stops when the disk
  cannot hold it, naming each part and how much is missing; it used to look at the RomFS image's space only, and only
  when that step began. A shader cache that was begun is not asked for the whole preparation's space again; nothing to
  write asks nothing; what a cut run left of the image is the RomFS step's to remove and counts as free.
- The RomFS step removes the `romfs.bin.part` of a run that was cut short before it measures the free space: on a disk
  with just enough room for the image (the image, the margin the step asks for, and 1 MB to spare), the retry passes
  the check as the first run did; a disk that is too small even so still stops the step, and says by how much.
- A dump whose table of contents is whole but one data byte differs (a copy that went wrong): the RomFS step ends with
  an error that names the file by its path, not by the dump's own number for it, which the launcher's job shows as its
  error, and leaves no partial image.
- An exception `make_game.py` does not report itself ends the build as `fail()` ends it: an error event with the
  exception's message, not only a traceback and "the build stopped (exit code 1)"; the log still has the traceback.
- On Windows a build whose compile step will run first compiles and links an empty program with the installed tools,
  linked against what the game links; tools that do not work stop it there with what is wrong and how to mend it. It
  is not called for a build that only draws prompts or has nothing to compile.
- The check of a Linux build runs the compile step's own command with `-fsyntax-only` on each source, then links a
  program against the game's libraries as the game is linked, with the game's runpath, and runs it. When files fail it
  stops with every one of them named (the compile step named three of four), no link is tried, a missing header is
  told from an old one, and the text says where the Vulkan headers came from.

The compile step:

- The launcher keeps each version in its own folder: a kit at a new place reuses every object, and a header the new
  version changed recompiles what includes it. An entry under a key an earlier version gave the same compile is
  re-recorded under the new key when its files read as they did (not for a key it does not know); a header in the
  earlier kit's folder is read in this kit's (that folder may be gone); a header that changed since is compiled again.
- The object cache goes to disk every `CACHE_SAVE_EVERY` objects while the compile runs, so a compile that ends in the
  middle (Cancel ends the process) keeps what it finished; it used to be written only at the end. Cut at the 40th
  object of 60: nothing on disk before the 25th, then 25, and the next run compiles only the 35 not saved. The largest
  file starts first and can compile for minutes: the objects that finish meanwhile are recorded and saved, not held
  back until the largest one is done.
- Without `--jobs` there is at most one compile per processor; the memory gate, not the job count, keeps the compiles
  within the free memory. `main()` without `--jobs` takes `default_jobs` and says so; `--jobs` is taken as given.
- Most of the runtime can come prebuilt (the runtime split). The compiler's id leaves its file time out (a tool update
  that puts the same compiler back keeps every object; the form up to 0.6.0 had it), and the key leaves `--sysroot`
  out (the fast compiler's objects are the UCRT64 compiler's).
- The folder of the pinned Vulkan headers goes ahead of every other include folder for the runtime's sources, so that
  no folder that also is a system one answers for `<vulkan/vulkan.h>`; the lifted code's command, which is part of its
  objects' keys, is the one it was. `tools/vulkan_pin.py` keeps the C headers and the licence note of the pinned
  archive and nothing else, takes no other archive, fetches nothing when the folder is there, and says the folder
  holds them only once the last file is written (a folder cut short while it was written has no stamp).
- `tools/build_shaders.py` writes a file's bytes as a header for compilers without `#embed` (the menu font, and the
  SMAA lookup textures of the anti-aliasing setting, `runtime/vk/vk_aa.c`): the numbers are the file's bytes, and the
  header is stale once the file changes.
- `lifted_level()`: a build from the game files takes this processor's level; an update with nothing recorded and no
  old cache too; the cache of 0.5.0 was compiled for no level (0), but 0.5.0's runtime objects left in a cache whose
  lifted code a later version compiled are not an old cache; otherwise the level recorded, never above this
  processor's. `retune_due()` is true with nothing recorded and for generic objects on a v3 processor.
- `copy_if_changed`: Windows has no POSIX executable mode and cannot replace read-only destinations, and build
  artifacts can share a normalized modification time. A network share can refuse the owner-only folder
  `TemporaryDirectory` makes: the copy needs a file only, and leaves nothing behind, also when the copy fails (the
  installed file is as it was).
- A share that stops answering for a moment: the source's folder is not there either, and the size is asked again
  (each look at the folder is one try; said once, not at every try). A share that never comes back stops the build
  after `SOURCE_WAIT` and names the drive; a folder that answers without the file ends the build at once.
- Windows: `--out` is made absolute without `resolve()`, which would turn a mapped drive into its UNC name.
- The Windows resources, with the toolchain, the kit and the game under folders with spaces, as under
  `C:\Users\Ali Sayed`: windres must start gcc itself (`--use-temp-file`) on a line that names no path but gcc's, the
  `.rc` by its bare name from `runtime/`, no `-I`, `-D` or `-U`. The temporary files go to a folder of the build
  cache, which is gone afterwards with the two files windres left there. The same resource again keeps the object's
  time, so the link key does not change and nothing relinks. A failing windres stops the build with its message.
- `modules.c` holds the game folder's paths as C strings. A character outside the Basic Multilingual Plane (an emoji,
  a rare Han character) is written as itself in UTF-8, not as the two surrogate escapes JSON's default makes of it,
  which C refuses. The test compiles the strings alone, which any C compiler takes, and, with clang, the file as the
  build compiles it, with the runtime's headers; the path's own bytes are in the object. `tools/check_content.py`
  refuses that file in a commit as generated from game code.

The shaders step:

- The shaders key follows the translator the build uses. A source checkout has its translator in `build/xlate/`
  (`tools/build_xlate.py`), not in the kit's `bin/`: the key follows its identity, so a rebuilt translator prepares
  the shaders again. A kit's `bin/` comes first when both exist. A kit with no translator has a key that says so (the
  keys are worked out before the steps run, `steps_to_run`), and the build refuses it with the way to build one when
  it needs the translator.
- The step turns the game executable's counts into progress (translation the first half of the bar, the driver build
  the second), sends one every few seconds while nothing is printed (heartbeats while the build waits), says when the
  counts stop moving, and passes its other lines on to the log. The driver cache warm-up after an update only builds:
  its count runs from 0 to 100% of the bar (`warm_driver_cache` runs the game's executable that way; a stand-in prints
  the counts the game prints).
- The game executable stops waiting for builds the driver holds past `NX2SARIA_PREPARE_STALL_S` (`prep_stalled` of
  `vk_pipeline.c`), says so and ends with code 0: the build log says the rest continues in the game, not that the step
  failed, and a later run that leaves nothing says the plain line again.
- The graphics drivers' cache folders the step reports (`driver_cache_folders`): on Windows every folder with "cache"
  in its name under the vendor's folder in `LOCALAPPDATA`, and Windows' own; on both systems the game's own
  `nvidia-cache` beside its executable, first (the game makes it for NVIDIA's driver, and on Linux `mesa-cache` too,
  which Windows does not get); on both the folder `__GL_SHADER_DISK_CACHE_PATH` names, last, and only when it is
  there; each once (a folder that is a link to one already listed is listed once). A count says what it missed: the
  time ran out ("the count stopped"), or the system refused something. A count with nothing left to count is complete
  whatever the time: only a folder still to read is cut off. A folder the system refuses is not an empty one, and
  neither is a file it refuses. A link inside the folder is not followed, so a loop ends; the link is one more file,
  of the size of the link itself.
- The step says the size of the driver cache folders when it begins and when it ends, with the step's own lines
  between, also when it fails (the failure is not hidden), so that a log tells whether the prepared shaders were still
  on disk when an update began; a count that raises, whatever it raises, is said and the step goes on. No folders, no
  lines. The folders of one line share the time: each is given what is left of it, none when it is spent. The step
  hands on whether it is an update.
- After the preparation the step folds the loose files of the shader cache into `shader-cache.pack`
  (`tools/pack_shader_cache.py`), as the installer did; a kit without the tool leaves the cache as it is. An update
  that carries the shaders over packs the cache once it holds `PACK_LOOSE_MIN` loose translations, and leaves fewer
  loose.
- An update that carries the shaders over runs the game's `--warm-shaders`, in the game folder; a run that fails is
  said and does not stop the build (the game warms the cache itself), and without a built game or a preparation
  nothing runs.
- `shader-prepared.bin` carries the digests of the two seed files and the translator from version 2 on
  (`runtime/gfx/prp_file.h`); a version 1 list, or a file cut short, says nothing, and no file is None. An update
  keeps the shaders prepared before only when their list was prepared from the seed this kit writes: the seed is
  written first (`--no-run`, as `tools/prepare_shaders.py` writes it) and compared with the list's head. Another seed
  (one byte of the seed's programs, or a change to the answers alone), or a list that does not say what it came from,
  runs the whole preparation; the same seed with another translator carries over as before, the game carrying the
  translations over; a preparation that then records this seed is kept by the next update; and the seed files do not
  stay in the game folder either way.
- An update leaves the shaders to the game (`shader_carry`) only when they were prepared and the cache names the
  translator they were made with: without that the game has nothing to carry them over by. `shader_carry` reads
  `NX2SARIA_SHADER_CARRY` as `runtime/cfg/cfg.c` does: 0, off, false and no in any case are off, a comment after a
  blank is cut unless the value is in double quotes, the file's last line with a value wins and the environment wins
  over the file. A line the game reads as off and this as on would skip a preparation the game does not make up for.
- `guest_opt_ini` takes `guest_opt` from the settings file by itself. Up to 0.10.1 it needed `low_end_gpu` on in the
  file; that mode is gone, and a `low_end_gpu` line of any value changes nothing. Without a `guest_opt` line nothing
  is set, and the environment's value wins over the file.
- Before a new translator goes beside the game, a shader cache with no `identities.txt` gets one naming the translator
  still there, in the form `runtime/gfx/gfx_shader.c` reads (`ids_parse`): `#` lines, then 64 hex digits. No cache:
  nothing to name. A file the game keeps is never written over.

The prompts step, the settings file and what goes beside the game:

- With button art in the kit, the last step draws the button prompts (a fake `tools/pad_prompts.py`), one progress
  event per folder (two folders per pad style and the keyboard and mouse one). It runs again only when the art, the
  `bind_` lines or a folder changed, and then no other step runs; a step before it that runs does not make it run (the
  lift running again makes the compile run, not the shaders, whose key names the lift and the compile they depend on,
  nor the prompts); a kit without art skips it, the folders an earlier build drew stay, and the record says there is
  no art. The game folder's record of where the prompts came from ("drawn from Test pack 2 (CC0 1.0)"), which the
  game's log repeats, is written again when the step is skipped. A setting that is no key binding changes nothing.
  When the tool fails the build fails and the record says why there are no new prompts.
- The rebuild an update ends with (`make_game.py --rebuild` under a newer kit, with another lifter and another
  version: the lift, the compile, the shaders step and the button prompts all run again) leaves the game folder's
  settings file as the player has it, byte for byte, with no copy, backup or temporary file beside it. The build reads
  the file (the `bind_` lines for the prompts, `shader_carry` for the shaders step) and never writes it, nor makes one
  (the game's menu does); so does a second rebuild that finds nothing to do, and one from the game files again into
  the same folder (Rebuild the Game without a build to start from). The lift and the compile are stand-ins there; the
  check, the ExeFS and RomFS steps, the shaders step and the prompts step are the real ones.
- The kit's `nvngx_dlss.dll` and its notice go beside the game; a newer kit replaces the copy an earlier build placed,
  and a DLSS the player put there is kept; a kit without DLSS places nothing. The notice the release scripts write is
  the one `make_game.py` reads. On Linux the kit's `libnvidia-ngx-dlss.so.<version>` and its notice go beside the
  game: the name carries the DLSS version, so a kit with another version replaces the file an earlier build placed
  under its old name, and a library the player put there, of any version (also the same version with other bytes, or
  beside a library an earlier build placed), is kept, the kit's not placed beside it. Each platform places only its
  own library.
- The Linux library is pinned beside the Windows DLL (`tools/dlss_pin.py`), at the same DLSS version with its own size
  and sha256; the default pin is still the Windows DLL, and its pin does not accept the Linux file. A file on disk
  must be the pinned one, a missing one is downloaded into the cache under its own name, and the notice names the
  library and carries the sha256 line `make_game.py` reads; the notice's name does not start with the prefix the
  runtime takes for the library.
- DLSS Frame Generation's files go beside the game only as a whole set, with the licence texts (with
  `nvngx_dlssg.dll` missing nothing is placed); the release script (`tools/make_launcher.py`) and the build agree on
  the set. The Streamline files come out of the SDK archive checked one by one; the archive goes once they are out and
  a filled cache needs no download; an SDK folder on disk must hold the pinned files, and an archive whose file
  differs is refused; `bin/dlssg/` gets the set `make_game.py` places, the two licence texts and a notice with every
  file's sha256.

### The check of the game files

`logic_digest()` is the sha256 of a Python file's code without what cannot change what it does: docstrings, comment
lines, blank lines and trailing spaces. It is the same on every Python version, as it reads only where the docstrings
are. `SCAN_LOGIC` records `make_game.SCAN_TOOL` and the `logic_digest()` of `tools/romfs_check.py` when that value
was last looked at. A change to the script's logic fails
`FileCheckTest.test_the_file_checks_logic_is_the_recorded_one`, which asks: can the change alter what the check finds
or writes for some game file? If it can, `SCAN_TOOL` gets a new value (every install then checks its game files
again, about four minutes) and both are recorded here. If it cannot, only the new digest is recorded.

`FakeImage` holds in memory what `tools/romfs_check.py` `problem()` reads of the packed game data: files by path, and
the game's dictionaries by id. `FileCheckTest`: `romfs_check.py` reads each file in pieces of `PIECE` bytes, and the
piece size is a matter of speed only: what the check says of a file is the same with the 4 MiB pieces of 0.7.1
(`FileCheckTest.old_problem()`, which also took no look at whether the frame had ended) and the 64 KiB pieces of now.
`FileCheckTest.cases()` makes up the files: frames of about 8 MiB packed, which span several pieces at either size,
and small ones, from bytes of 12 values (which pack to about half). `SOME` means the check must find the file damaged,
whatever its words; `SAME` only that both piece sizes say the same (a few changed bytes inside a block can decompress
to other bytes of the same length). The cases run with the game's dictionaries there and without; a frame naming a
dictionary the game lacks is not judged.

- More of `FileCheckTest`: an index that says more bytes than the image holds. A whole frame with other bytes after
  it (past the next 64 KiB piece, past the next 4 MiB piece, or a second frame): the check stops at the frame's end at
  either piece size. As 0.7.1 made it, such a file passed when the frame ended in the file's last piece and failed
  when another piece followed, so the answer hung on the piece size; the game data has no such file (the check reports
  the same four files on every copy, none for this reason). The scan step's key no longer follows the script's bytes:
  it is what 0.7.0 and 0.7.1 wrote into `state.json` (the sha256 of 0.7.1's script,
  `55ea0c150aaf9dac93a2e6fd3b4ca11e1c572b5eee8b6bd74960535964384634`), so an install that passed the check does not
  run it again because the script was edited. The recorded digest follows the code, not its comments, docstrings or
  blank lines.
- `PROMPT_LOGIC` records `make_game.PROMPT_TOOLS` and the `logic_digest()` of the three scripts that draw the button
  prompts when that value was last looked at, as `SCAN_LOGIC` does for the file check: a change to their logic fails
  `PromptToolsTest.test_the_prompt_tools_logic_is_the_recorded_one`. If the change can alter what the tools write,
  `PROMPT_TOOLS` gets a new value (every install then draws its prompts again) and both are recorded; if it cannot,
  only the new digests. Two changes recorded that way: the NX2Saria filename fallback changes no rendering for the
  same effective bindings, and `step_key` already includes those bindings, so the rename recorded the digest without
  forcing all prompts to redraw; and `REPO_ART`, `release_art`, `source_of` and the `SOURCE.md` copy of
  `tools/prompt_art.py` (issue #15) choose and record the art, while the pictures it names, which the step's key
  holds, are the same, so the prompts drawn from them are too.
- `load_pad_prompts()` loads `tools/pad_prompts.py` as a module. Where fontTools is not installed, the names the tool
  and `tools/button_icons.py` import from it get empty stand-ins for the import: the tests use the tool's reading of
  the game files only, which needs none of them. The module is not left in `sys.modules`.
- `PromptToolsTest`: `tools/pad_prompts.py` reads the game's files in pieces: the same bytes as in one call (on 24 MiB
  that pack to several pieces), without the minutes one call took on the 244 MB texture archive; its reader of
  extracted game files takes plain files, packed ones, and one packed with a dictionary of the game's dictionary
  archive. The prompts step's key no longer follows the scripts' bytes: it is the one installs have (the scripts of
  0.7.1, `ad59f2c4533f6ea2630b916ef5f3695f72daffe9b2b7663eea4168a4f08e4249`).

### Jobs and locks

- `JobTest`: an install driven by `FAKE_MAKE_GAME`. The rows end as skipped for the tools, done for the steps up to
  the shaders, skipped for the scan (the fake build has no such step) and done for ready; a version without button
  art draws no prompts and has no such row, one with button art shows the prompts step as a row of its own, after the
  shaders. `scan_job()` is an install whose fake build ran (mode `ok --scan`) or kept (mode `ok`) the check of the
  game files, with a report as the `romfs-check.json` the check left in the game folder.
  - What the check of the game files found reaches the player: the count in the scan row, a sentence for the home
    screen and a line in the log. The files flagged on every copy are told as expected, with nothing to do; nothing
    tells the player to dump again. A clean check says nothing on home, nor does one an earlier build did (the row has
    the count).
  - `build.Run` starts the build below normal priority (`0x4000`, `BELOW_NORMAL_PRIORITY_CLASS`), and the processes
    the build starts (the compilers) get that class from it, so a build on every processor leaves the PC usable.
  - A line handler that raises (the log on a full disk) must not end the thread that reads the build's output: the
    build would stop at a full pipe and never exit. Every line is counted as dropped (20002: every line, the `@@` line
    that is no event, and the event). A log that cannot be written loses only the line.
  - Begin Installation into a folder whose game files an earlier run unpacked builds on from them (`--rebuild` for
    this processor's level), so a moved or deleted dump is not asked for; without them (the image gone) it reads the
    dump.
  - The worker, the build's reader thread and the window change a job while the window reads it. Under `job.locked()`
    a step's state comes with the value and fraction that belong to it, every change counts in `rev` (the window draws
    again when `rev` changed, and learns from it that the job ended; no change of 80000 is lost), and the output's
    last lines can be copied while lines arrive. The job screen's rows from a real job have one divider, after the
    game data, and end with Cancel.
- `LockTest`: `launcher/locks.py`: one process works in a launcher folder at a time, and in it one job on the same
  thing. `HOLDER` is another launcher process: it takes the lock file as what it is told, says so and holds it until
  its input closes; `holder()` starts one, holding the layout's lock until the test ends or it is killed.
  - A second launcher is refused and told who holds the folder, with no second window.
  - The lock goes when its process dies (no clean exit: the operating system lets go of it). The window's own jobs
    share its lock, and it stays the window's until the window lets go of it; letting go twice lets go once.
  - A job does not run while another process holds the folder (nothing of it ran); a job that changes no file (the
    update check) runs beside it; once the other process ends the job runs and lets go of the folder when it ended.
  - Two jobs on one folder: another kind of job on the same folder fails; what the background download claims is the
    app folder, not the game; the first job lets go when it ended.
  - A job that writes into the game folder leaves `data/job-running.json` while it runs. Ended, also by an error, it
    removes it; a launcher stopped in the middle leaves it, and the next start says which job did not finish, once. A
    job that writes nothing there leaves none; a marker cut while it was written is read as none. The window says
    which job did not finish (an install that was stopped leads to the first run screen), and the next start has
    nothing to tell.
  - The command line takes the folder's lock: held while the command runs and let go of after; refused while another
    holds it (neither command ran and no setting was written), except the update check, which changes nothing.
    `update` rebuilds the game after it switched versions: started while the game runs, it would leave the new version
    with the old build, so it refuses before it downloads anything, as the window's Updates row does.

### Saves

- `SavesTest`: `launcher/saves.py`: where the game keeps its save, the save folders it recognises, and the import.
  - The account folder is named as the game names it (`storage_init` of `runtime/hle/fs_host.c`: the two halves as 16
    hex digits each); for a game that never ran the account is made.
  - An import backs up the save the game would have loaded and clears the journal, so the game loads the imported
    save; a picked folder is left as it was. One slot replaces only that slot (the slot is replaced, not mixed, and
    the game reads its files only inside a slot); a slot under another name is the first slot. From the saves folder:
    `README.txt` alone is no save, a save is not imported twice, and files put straight into `saves/` count. Picking
    the saves folder takes what waits there, not `README.txt` or a backup.
  - An interrupted commit is backed up: as `save_recover` of `fs_host.c` has it, with no account folder and no
    finished commit `save-tx/new` is the save, and without it `save-tx/old`; the import keeps that only copy.
  - A failed swap keeps the old save (the imported save going into place while antivirus holds a file). When the
    account folder cannot be moved aside (a file in it held open), the newer finished commit stays in `save-tx`, where
    the game loads it first, instead of going into the backup.
  - A running executable refuses a write open with a sharing violation (32); access denied (5) on an executable that
    is not read-only (an access list, Controlled Folder Access) is no running game, and is logged when asked. A
    read-only one is cleared, then the open is tried again. The real call is tried on a file nothing holds and on a
    running sleeper.
  - Read-only files import writable, and a read-only save being replaced is deleted after the swap all the same. The
    refusals (the game's own save among them) change nothing.
  - The import takes the game's own profile lock for the time it replaces the save (`game_lock()` takes it as the
    game does, `plat_lock_file` of the platform layer: Windows opens the file without sharing, elsewhere flock): a
    running game refuses it with nothing changed (no backup made, nothing staged), and a game that starts in the
    middle of an import (during the backup's copy) is turned away by the lock instead of mounting a save that is half
    replaced. After the import the game starts as ever.
- `SaveShareTest`: `export_save` and `import_zip` of `launcher/saves.py`: one zip with the save and a manifest, for a
  bug report or another player, and the checks an import makes before it replaces the save. `zip_of()` makes a zip
  with given members and a manifest (the keys of `export_save` unless given, or none).
  - The export writes one zip of the newest save (the finished commit the game would load), leaves no `.part` behind,
    and gives a second export in the same second a name of its own. Without a save there is no export, and the export
    makes no account.
  - The private settings' values (a room's secret, a relay and a name that an earlier version's co-op left in the
    file) are not in the settings file a bug report carries; every other byte of the file is. A game that stops before
    its first save: the bug report export writes its log and settings alone (nothing when there is no log either),
    under a name of the form `NX2Saria-log-...-bug-report.zip`; an import of it says it is nobody's save and changes
    nothing.
  - A shared save replaces the save, every slot of it, and keeps a backup; the bug report files stay out; a picked zip
    is left where it was. A zip in the saves folder is taken first, not copied in as a file, and moved out so that it
    is not imported twice; a loose file waits on its own. A damaged zip changes nothing.
  - The zip of the game's menu imports: `runtime/hle/save_share.c` writes the same layout with stored and deflated
    members, and Python's reader takes both.

### The icon, the look, Steam

- `IconTest`: the icon's entries are bitmap entries (a 40-byte header, 16x32), not PNG.
- `LookTest`: Customize Launcher: the colour schemes, the background picture and the sounds.
  - Every scheme has every colour; the first scheme (`forest`) is the colours as written, and the one taken when
    `launcher.json` has no key. A warning stays a warning in every scheme.
  - The Custom scheme: with nothing picked yet, the first scheme's colours; the picked colours exactly where they
    stand in; Button Color changes the buttons and the orange choices (in the picked hue, lighter) and nothing else,
    not the panels, the title band or the backdrop; light text on dark buttons and dark text on light ones, the
    description box's text following its panel. The colour square: the full colour top right, black along the bottom,
    grey at the left; the hue bar has green a third of the way and blue at two thirds.
  - A picture is cut to the window's shape: the middle of a wide picture fills most of the result, and a large one is
    shrunk by averaging. The kept background is shown darkened by the dimming, solid everywhere, with the corners'
    shade reaching in a little. The picture starts below the title band and is never stretched: a copy an older
    launcher kept for the whole window is cut to the new shape, not squeezed (a circle stays a circle).
  - The game's interface sounds, from a made-up `.bwav` (`bwav()`: the header, 0x4c bytes a channel, the data): one
    PCM16 channel in the middle goes to both sides at 0.707; two DSP-ADPCM channels are left and right, where a
    frame's header byte (coefficient pair 0, shift 8) and its four-bit samples 1, -1, 7 make 256, -256, 1792. They are
    decoded from a dump folder into `data/cache/sounds` and from there into the effects. Only the game's sounds play:
    nothing before the game has been read, low when no level is given (as a new `launcher.json` has it), nothing when
    the player turned them off, the default for a level the file cannot hold, and no sound for the cursor.
  - The look settings survive, and values of the wrong kind fall back to the defaults.
- `SteamTest`: `launcher/steam.py` on made-up Steam files: the binary shortcuts file read and written back unchanged,
  our shortcut added beside another (which stays byte for byte) and updated in place (a moved launcher is the same
  shortcut), its id as Steam computes it, the accounts that signed in. A shortcut under the earlier name is renamed in
  place and keeps its id, so that Steam's per-app data stays. `LinuxSteamTest` edits Linux Steam shortcuts in an
  isolated `HOME`, with all native and Flatpak accounts.

### While the game runs

`mock_app()` gives `launcher.ui.app.App` and a stand-in for its instance, for calling one of its methods without a
window: the settings name the game folder, the context and layout are real ones in the test's folder, nothing is
staged, no check has run and no message is set; it skips the test without the window toolkit. `MemorySettings` is the
launcher's settings without their file.

- `WhilePlayingTest`: its `ended()` runs `watch_game` after the game ended with a code, its log holding given text;
  `play_from_steam()` runs `play_only` (`--play`, the Steam shortcut) for a game that ends with a code, with the
  launcher folder's lock held by another process or not, and a stand-in for the message box.
  - The game's Restart ends the process the launcher started with code 0 and starts the executable again: that does
    not bring the launcher back; the launcher follows the copy (`winsys.game_copy`) and reports the code it ends with,
    and its FATAL line, read from the copy's own log. A copy the launcher could not find (it ended before the next
    look) has a code that is not known, so neither `launcher.log` nor the game's log says code 0, and its FATAL line
    is still shown.
  - `winsys.game_copy` finds a running process by its image file, not by its name (the same name in another folder is
    another install; Windows paths ignore case), and reads the code it ended with after it ended.
  - The game writes its settings file when a menu setting changes, so the launcher's Discord Status switch (the one
    setting of that file the Settings screen has) changes nothing while the game runs and says why, still showing what
    the file says; the mods' switches refuse the same way (`switch_mod`).
  - Exit codes 70 and 71 follow a FATAL line in the game's log: the home screen and `launcher.log` name it, the
    launcher's log with the whole line when it is longer than the home screen shows. A log that still ends with an
    earlier run's end has no FATAL line of this run; any other exit code reads no FATAL line; no log at all is no
    error. Exit code 72 (`vk_device_lost_check` of `runtime/vk/vk_device.c`) follows a FATAL line as 70 and 71 do;
    when its watchdog ended the run before that line, the code's own text says what happened.
  - No process of the game writes a FATAL line and exits with 0: after code 0 such a line is a copy's that the game's
    Restart started and that ended before the launcher saw it, and the player is told. A log older than the start is
    an earlier run's and tells nothing. A log that reached its 64 MiB cap (`log_sink` of `runtime/main.c`) wrote its
    end lines to stderr only: it is not taken for a run without its own end.
  - A run ended with code 0 goes into `launcher.log`; when the game's log has no line of the game's own end (the
    window's close, its ExitProcess, a FATAL line), the end is noted in that log too, and the note ends the search for
    the next run. With no log the end is logged with code 0 and nothing is noted.
  - The Steam shortcut does not start the game while the window or a command runs a job in the game folder (another
    process holds the launcher folder's lock, and the job's marker is there): the game would be the build the job
    replaces. A marker that a stopped launcher left, with the lock free, stops nothing, nor does the window open
    without a job. The Steam shortcut's game ends as Play's does: `launcher.log` and the game's log say how, and an
    end worth it (a FATAL line, 70, 71 or 72, a Windows status) gets a message box, as no window shows it; an end with
    code 1 (Task Manager's End task) gets none.
- `LinuxPlayTest`: Play on Linux: the game is the folder's `ocarina` (`game_executable` of `launcher/platform`),
  started in its own session (`start_new_session`: its own session and process group), followed until it ends, its
  Restart's copy too, with a line in `launcher.log` for each outcome. `fake_linux_game()` puts a shell script in the
  place of the built game: it notes the folder it runs in and its session, sleeps and exits with a code.
  `RESTARTING_GAME_C` is the game's Restart on Linux (`plat_restart` of `runtime/platform/linux.c`): the first run
  spawns the same file again as its child and exits with 0 0.3 s later; the copy runs `FAKE_COPY_MS` milliseconds
  (3000 by default) and exits with `FAKE_COPY_CODE` (3 by default). `play()` calls `App.play`, then `App.watch_game`
  once every 0.1 s until the run ended.
  - Play starts the game with the window hidden (Hide Launcher While Playing) and logs its end ("the game ended with
    code 0" when there is no game log to tell more from); Windows' commit check has nothing to sample there. Every
    Play that does not start says why.
  - The game's Restart spawns its copy as its own child and exits with 0: the launcher, a child subreaper since it
    started the game (and no longer one once the run ended), becomes the copy's parent, finds it by its file
    (`/proc/<pid>/exe`) and reads the code it ends with. A copy that ends before the launcher's next look (once a
    second) is never seen running: it is a zombie the launcher adopted, waited for when the run is over
    (`winsys.game_finished`), and its code is the end's. Only this process's zombies named `ocarina` are waited for. A
    copy is found by its executable file, also when that was replaced on disk; one that is not this process's child
    has ended once its process id no longer runs the game (gone, or used again).
  - subprocess gives -N for a process signal N ended: the line names the signal, and only a crash's signal is worth a
    box where no window shows the end (`play_only`).
  - The whole window: Play on home starts the fake game with the window hidden, the window comes back when it ends,
    and `launcher.log` has the start and the end. It runs with `NX2SARIA_TELEMETRY_URL=none`: with session reports
    their question would open over home.

### The window

- `BeginInstallTest`: Begin Installation removes the `romfs.bin.part` an install that was cut short left before it
  measures the free space, so the 34 GB it asks for are not short by the part's size (40 GB free without the part, 20
  GB with it). Windows and the build tools stop at 260 characters for a file's whole path, and the game folder's own
  files need 128 below it: a deeper folder is refused at Begin Installation and by the install job, before the build
  tools are fetched, with the lengths, where the build stopped in the middle with a file "not found". No limit
  elsewhere.
- `UpdateTextTest`: an update's rebuild took players 24 to 45 minutes: the Updates row says what it depends on instead
  of "in a few minutes".
- `ForcedCheckTest`: selecting the Updates row (or F5) again within a minute of the last check starts no request: 25
  of them in 90 seconds used up GitHub's hourly allowance for a player. The launcher's own checks (at start, every
  hour) are not held back by it.
- `StartScriptTest`: `launcher/start.pyw` when the version cannot be imported: with a version to go back to it ends
  with code 86 at once and shows no message box, so the stub's six-second wait sees the code and rolls back (a message
  box would wait for a click: the test's timeout shows there is none). The version the stub goes back to: none without
  `previous.txt`, or when it names a folder that is not there, or this version itself; the file is read as the stub
  reads it (a byte order mark, blanks, CRLF). No quiet exit once the stub no longer waits (5.5 s), nor with `--play`
  (Steam started it: no stub to roll back).
- `UnfinishedUpdateTest`: a game whose last update stopped in the compile (`state.json` without the compile step, the
  executable of the build before still there): the window opens on home and says how to finish, and an update goes on
  with the game (its game folder is the game's, not empty, which would skip the rebuild with "No game yet"). An
  install that never finished, with nothing to play, asks for the dump.
- `LauncherLogTest`: what `launcher.log` says about the window beyond its start (`launcher/errors.py`,
  `launcher/context.py`, `App.on_close`, `App.log_first_run`): a player's log that ends after the start line must
  tell a closed window from one that died.
  - Tk reports an exception in a callback (a click, a key, a timer) only on stderr, which is nowhere under
    `pythonw.exe`: `report_callback_exception` writes a line into `launcher.log` and the traceback into `crash.log`.
  - Errors in threads and fatal faults are kept. On Windows there is no faulthandler. A fault in native code makes
    faulthandler write the stacks into `crash.log`; the test does not show that with a real fault, which leaves a core
    dump in the system's crash records that reads like a launcher crash.
  - faulthandler keeps `crash.log` open; a window that ended without a fault removes the empty file, which a player
    would otherwise attach as if it said why something failed. One with something in it stays.
  - Closing the window is logged once, also with a signal while the window is closing.
  - The whole window on a first run with no dump around: the lines a player's log should show, ending with the reason
    the window went. The drives this machine has mounted are left out: a dump on one would be found and decoded. The
    test waits for the two lines the signal needs, not for a time: the window's, which `main()` writes once the
    handler and its wakeup are in (`errors.on_end_signals`, `errors.wake_for_signals`), and the dump search's, which a
    thread the window starts writes; the time limit only ends a start that hangs.
  - `idle_window_ended()` measures, for a window with nothing to do, the seconds from SIGTERM to its end, with the
    signal's handler and `errors.wake_for_signals` as `main()` puts them in; idle means its main thread asleep, which
    for a Tk window is waiting in Tcl for an event (Linux: `/proc` state S). Python runs a signal's handler only when
    it runs Python again, and an idle Tk window waits in Tcl: with no other event the window used never to act on
    SIGTERM. The socket wakes it (POSIX). The timer Windows takes, where Tk cannot listen to a socket, is tried too:
    the signal is acted on at its next check, every 0.25 s. Tk on Windows has no `createfilehandler`:
    `wake_for_signals` runs a timer there, and on a Tk without one elsewhere, and leaves the signal module's wakeup
    alone; the timer goes on until undone.
- `RefocusTest`: a focus change by the mouse or the keys draws only the two rows, the cursor and the description
  again (`App.refocus`): what it leaves on the canvas is what drawing the whole screen leaves (with animations off the
  cursor stands on its row at once). A click on a row whose action draws nothing (Open Logs Folder), or a cancelled
  chooser, moves the focus without drawing it: the next move must not leave the row drawn lit before it lit too.
  Nothing piles up: after 20 moves there are as many items as a whole drawing of the same focus leaves.
- `TelemetryHiddenTest`: with no telemetry server (`TELEMETRY_URL` of `launcher/telemetry.py` empty, no
  `NX2SARIA_TELEMETRY_URL`) the window shows nothing of session reports: no question at its start (also with a summary
  waiting and nothing asked), no Session Reports row, no `--screen` of them. The environment override turns all of it
  on. The window's pictures are let go when the test closes it, on that thread: collected later on a thread a
  following test starts, Tk ends the process ("main thread is not in main loop").

### Mods

`ModsTest`: the mods folder as the launcher puts it in order for the game (`launcher/mods.py`) and the build takes its
code in (`tools/make_game.py`, `tools/mod_files.py`).

- Downloads are put where the game looks, once: a zip holding the mod's own folder, a mod wrapped twice with a readme
  on each level, a mod's `romfs` straight in the mods folder, an Atmosphere layout (a mod as it is), and a download
  with options, which is left alone.
- The switches and the order are the game's settings: the mods sort as `strcmp` sorts them, and the disabled list is
  quoted as `runtime/cfg/cfg.c` quotes.
- A file of a mod's archive called after a device (`CON.pchtxt`), ending in a dot or a space, or holding a colon
  cannot be kept under its name on Windows: unpacked anyway it is lost, renamed or a hidden stream of another file,
  and the game misses it without a word. The archive is refused with the file's name, nothing of it unpacked, by Add a
  Mod too; on Linux, whose game reads such names, it passes. Leaving the folder is refused everywhere.
- A picked archive or folder is added; the player's download stays where it was and nothing stays beside the mods
  folder. A copy that stops halfway (a full disk, the launcher closed) or an archive that cannot be unpacked leaves no
  folder in the mods folder, which the game would load as a mod, and none beside it; the same mod goes in whole
  afterwards. While a mod is copied the mods folder does not hold it (it is put together beside it); it moves in under
  a name that is free then (`Late 2` when a folder `Late` came meanwhile); and what a stopped launcher left beside the
  mods folder long ago is removed, while another add still at work is left.
- A mod's code waits for a build that saw it: a mod switched off waits for nothing, one the last build took up
  neither, one edited since does.
- `recomp/layout.py` alone decides the modules' bases: `tools/cheats.py` decodes main's pc-relative code at the base
  the layout gives main, the first module of its order, and follows it when it changes.
- The mods' code is in the lift key and not the shaders key; files of the game's data make no build.

## `tests/layout_tools_check.py`

Checks `tools/layout_dump.py` and `tools/layout_preview.py` on a layout archive the test makes itself. No game files;
it is run as `python3 -I tests/layout_tools_check.py`. With NumPy it also checks pixels and clipping.

The archive is a SARC with two BFFH layouts: Main (a group pane that is scaled and moved, a picture in it, a text with
a left origin anchored to its parent's right edge, a part that instantiates Sub) and Sub (one picture). Main has one
animation with a curve of the group's translate x and a constant of the picture's scale x. `Bffh` is the writer: the
records the reader follows, at offsets it finds through the header; `Bffh.curve()` takes keys as (frame, value,
slope), `Bffh.animation()` tracks as (pane, group, prop, [(component, keys)], [(component, value)]).

- The tables, panes, parts and tracks read back. The panes' extents on the grid: the group at x 100 scaled 2; the
  picture 50 in it, 40 wide; the text anchored to the group's right edge (100 in its space) with a left origin. The
  curve's value between keys.
- The command line: the dump's text, JSON and `--find-animated` output.
- Edits at 21:9 (out 640): each edit operation of a spec and the span of a curve whose keys move; a spec whose
  expected value differs is refused and changes nothing.
- The preview: the before and after SVG files and the index; a refused spec stops it. A damaged file is a format
  error, not a crash.
- A separate layout (`textured_layout()`, `check_textures()`) checks picture bindings, UV transforms, default alpha
  and texture patterns.
- `split_layout()` and `check_split()`: RootPane, `N_Strip` (a group), `P_Bg` (a 1000-wide picture at x 500 in it,
  made like the game's: its record with the colour and corner lists inside it, a material with one texture map),
  `P_After` (a group after it, whose x an animation keys), and the two sections' headers, so that the split moves what
  follows `P_Bg`. The pieces: the left part (source 0 to 400), moved by out; a seam (390 to 410, placed at 400) over
  the gap the next piece's move opens; the rest (400 to 1000). The split refuses a pane with children, a rotated or
  scaled picture, and a piece outside the picture. The names' hash, CityHash64, is checked on names of the game's
  layouts, both of its short-name branches and the 17-32 one: `RootPane` gives `0x55a7ab4a9d264968`, `N_All_00`
  `0x7b2f820ba988bfbc`.

## `tests/lazy_fetch_check.py`

Builds and runs `tests/lazy_fetch_check.c`: the lazy write-back of `runtime/memory.c` and its ranges in flight, with
the Windows platform layer and its real fault handler, the rest of the runtime stubbed. Windows only (the fault
handler is); no game files, no GPU; the module layout the runtime includes is a stand-in written to the test's build
folder. Each scenario (`SCENARIOS`: name, scenario argument, extra environment) runs as its own process, so an access
violation passed to the crash handler shows as its exit code; any exit code other than 0, or a scenario that does not
end within 120 s, fails. `CHECK_CHUNK` in the environment sets `NX2SARIA_WB_LAZY_CHUNK=1`, `CHECK_LOCKED`
`NX2SARIA_WB_LAZY_UNLOCKED=0`.

The back end's fetch is a stand-in that waits (the GPU copy and the wait for it) and then writes a marker over the
first 64 bytes of each copy, opening the page for it, as the GPU's copy writes past the pages' protection; the back
end's clean test may wait too (GetWriteWatch can, while the driver pins memory). `arm_buf()` arms a range of the heap
as written back from a buffer into the heap's own; `stale()` says the replay left its writes to a range in a mirror,
so the armed parts there are stale and a touch fetches them; `fetch_end()` gives the end of the fetch that copied a
range, 0 when none did; `logged()` the number after a word in the last log line. The touching threads stand for the
game's CPU code; `armer()` for the completion thread's arm (`vk_gpu_written_back` of `vk_memory.c`) of 64 KiB written
back from buffer 1; `thawer()` for a file read's thaw of a page (`fs_host.c`, `romfs.c`: before the kernel writes the
buffer).

It checks that a fetch holds up nothing but the touches of its range, that those wait for it, and that arms, thaws,
mapping changes, freezes and releases see it:

- A thread reads a page over and over while the completion thread's arm makes its range inaccessible and a release
  gives it back: no read may reach the crash handler. The slow variant has the clean test take 100 ms, with the pages
  inaccessible all along.
- During a 300 ms fetch on another thread nothing else waits for it, and the range counts as one a read would fault
  on.
- B reads the range A is fetching, and with the fetch outside `frz_lock` the test's thread holds that lock all along:
  both finish after one fetch, B after it, and the report line counts B's wait. With `NX2SARIA_WB_LAZY_UNLOCKED=0`
  the fetch holds the lock.
- B writes the range A is fetching: one fetch, B waits for it alone, and the region's other range stays armed (B went
  to `thaw_fault` before, which released every armed range of the region).
- B writes a range whose flight starts and ends while B waits for `frz_lock` in `lazy_fault` (the test's thread holds
  the lock and runs a release's flight under it, as a thaw does): B finds the page given back and writes again, and
  the region's other range stays armed.
- Two threads touch two ranges at once: their fetches run at the same time.
- A thaw (a file read into the buffer) of a page in a region with a range in flight returns once that range's pages
  are back, and fetches nothing again. A thaw of a range armed with stale bytes fetches them before it takes
  `frz_lock` (`lazy_release_regions`): a freeze of another region meanwhile does not wait for the copy. With
  `NX2SARIA_WB_LAZY_UNLOCKED=0` the thaw fetches under the lock, and the freeze waits.
- A mapping change (a commit, decommit or protect: `thaw_range_locked` under `frz_lock`) in a region with a range in
  flight and another armed: the armed one is fetched under the lock, the flight ends first, both are given back.
- A release of one buffer's ranges (a mirror eviction): one fetch of its two armed stale segments, and it returns
  after the fetch another thread runs of its third; the other buffer's range stays armed. With
  `NX2SARIA_WB_LAZY_UNLOCKED=0` each segment is fetched on its own, as in 0.10.1. A thread that writes a range a
  release is fetching waits for that fetch, its write survives, and there is one fetch in all.
- An arm of a range in flight (its region's hold cleared, as when the range was released, not touched) leaves it
  alone: its pages are given back by the flight, and a later arm takes it again.
- A range in flight (a release, so no hold) in a region ends while an arm of another range in that region is in its
  clean test: the region keeps its mark (`lz_arm_r`), so a read of the arming range that faults after that end waits
  for the arm instead of reaching the crash handler.
- A write by the replay to a range armed for another buffer (a mirror gone since) gives it back without a fetch: that
  buffer may be gone.
- `NX2SARIA_WB_LAZY_CHUNK=1`: `lz_lock` is free during a piece's fetch; the rest of the range stays armed.
- The report line gives the longest fetch since the last line, and the longest wait of a fault for `frz_lock` (another
  thread's thaw, freeze or arm, 60 ms long); the next line starts from nothing.
- More threads than entries (80) touch a range each at once: those past `LZ_FLY` wait for room, and all end.
- A region with a range in flight is not frozen.

## `tests/legacy_names_check.py`

No setting name in the code is spelled with the older `NX2NAVI_` prefix (`tools/rename_legacy_names.py --check`). The
game, the launcher and the build tools read every setting under both prefixes, `NX2SARIA_` first, so an older
spelling still works; this check keeps the code on the current one. A branch made before the rename, or a line copied
from an old file, brings an older name back: `python tools/rename_legacy_names.py` spells it the current way.

Before the tree is checked, planted lines go through the script's rule (`rename_line()`), so a rule that stopped
renaming or started renaming the fallback is caught too (`rule_cases()`):

- `RENAMED` (file name, line, the line the rule must give): names the code reads, writes or tests (a `cfg_bool` of an
  older name, a patch table's `set` line, a format with the older prefix and `%s`) must be renamed;
- `KEPT` (file name, line): the fallback must stay: a line that also names `NX2SARIA_`, a line that calls the name
  older, legacy or an alias, the bare older prefix that prefix strips compare with, the PGO compile-time flag and a
  `KEEP` line of the script;
- the files under `tools/xlate/`, `tools/vendor/` and `runtime/third_party/` are never read.

`check_tree()` runs the script's `--check` on a root and gives the exit code and output. With `--self-test`,
`planted_repo()` also plants an older name in a scratch git repository holding the script and one file, and wants
`--check` to fail there.

## `tests/lib_origin_check.py`

Builds and runs `tests/lib_origin_check.c` on `runtime/lib_origin.c`, the game log's `libs:` line: which SDL the game
loaded, from beside the game or from the system, and the SHA-256 form. It is built as this platform builds it and, off
Windows, also with `-D_WIN32`, where the paths take either separator and compare without case. No game files.

The cases: an SDL beside the game, one elsewhere, one not found among the loaded modules and an unknown executable
path; folders that share a prefix are not the game's (one folder's name the start of the other's); case matters on
Linux; a name without a folder is never taken for the game's folder, and the root folder is one like any other;
versions apart; the SHA-256 form; a buffer too small is cut, terminated, and the whole length returned.

## `tests/library_path_check.py`

Builds and runs `tests/library_path_check.c` (Linux): `runtime/platform/linux.c` `plat_library_path()`, which the game
log's `libs:` line (`runtime/main.c` `log_libs()`) names the loaded SDL with, and `runtime/lib_origin.c`, against the
system's loader. No game files.

A library with a soname is built beside a program linked as the game is (`-no-pie`, runpath `$ORIGIN`); the library's
one function keeps it a `DT_NEEDED` entry. The program prints what `plat_library_path()` found for its argument and
the `libs:` line it would make of it. The line must say "beside the game" with the folder's real path when the program
runs from that folder, through a symbolic link to it, and when an empty `LD_LIBRARY_PATH` entry (the current folder)
found the library, and "not beside the game" for a copy an `LD_LIBRARY_PATH` folder supplies; a library preloaded
under its full file name (`libprobe.so.0.9`, as a distribution installs it) still answers for its soname, and a name
nothing loaded gives nothing.

`linux.c` includes SDL3's headers: `sdl3_include()` gives nothing when the compiler finds them itself, else `-I` and
the first folder that has them: the folder of the headers of `tools/sdl3_pin.py` (`toolchain/sdl3/include`) or
`SDL3_INCLUDE`.

## `tests/lift_compare_test.py`

`tools/lift_compare.py` on made-up lift trees: the same functions grouped into other chunks compare equal; one changed
byte, a whitespace or line-ending change, a lost function, a function defined twice, a call with no prototype, a line
`recomp/gen.py` does not write, a table that does not list the definitions and a module without a table are
differences. No game files; standard library only.

`FNS` holds function bodies as the `Emitter` of `gen.py` writes them: `f_200` has an all-label fallback, so its text
starts with the fallback's declaration and holds a second C function. `chunk()` gives a chunk's text as `chunk_text`
of `gen.py` joins it (header, includes, prototypes, then the bodies); `table()` a function table as `table_text`
writes it (the parts `lift_compare` reads); `tree()` a lift folder with one module, `mod`, whose chunks hold the
function groups and whose table lists every function of the groups unless told otherwise.
`LiftCompareTest.run_compare()` gives the differences and the output of old against new, after an optional edit of
the new lift.

## `tests/log_cap_check.py`

The log file's cap and the end of the run (`runtime/main.c` `log_sink()`): past the cap ordinary lines are dropped,
with one line that says so; once the process is ending (`rt_log_ending()`, which a fatal error and the crash report
call before `rt_log_sync()`) its lines are still written, after one line that says the end follows, up to a reserve of
their own, however much was dropped before. `rt_log_sync()` alone (a bug capture in play, `write_log_tail` of
`runtime/diag/bugcap.c`) does not open the reserve. The test builds `log_sink()` with a small cap and reserve against
stand-ins for the file and stderr, and `static_checks()` checks that every end that syncs the log marks the process as
ending first, so its lines reach the file past the cap. No game files; it takes the path to clang as an optional
argument.

## `tests/log_prefixes_check.py`

Static check of the log prefixes the runtime writes against the table in [logging.md](../logging.md). Standard
library only.

Every line of `logs/nx2saria.log` starts with a word or a few that say which part of the game wrote it ("vk:", "frame
rate:", "boot map:"). The check finds the prefix of each log call in `runtime/` and fails on a prefix that has no row
in the doc, so a new prefix comes with its row and a reader of a player's log can look it up. A row of the main table
that no call uses any more is a warning: the table is behind, the code is not wrong. It prints one line per problem
and exits 1 if any; `--unseen` also lists the calls whose prefix the check cannot read; `--self-test` puts each kind
of fault into a copy of the doc or a made-up source file and checks that it is caught.

What a log call is:

- `rt_log(fmt, ...)` and `rt_logf(fmt, ...)` of `runtime/host.h`;
- a macro whose body calls one of them with `__VA_ARGS__` as the format, after string literals or none (`LOG_ONCE` of
  `vk_pipeline.c`, `NVN_LOG` of `nvn_api.h` with its "nvn: ", `BAD` of `romfs.c`);
- a function with a `const char *` parameter that it passes on as the format (`once` of `ov_input.c`).

A macro or function defined in a `.c` or `.cpp` file counts in that file only; one in a header counts everywhere
(`wrappers()`; the wrapper's own call, whose format is its parameter, is skipped).

The prefix of a format string (`prefix_of()`: the prefix, an empty string for a continuation, None for a format that
starts with a conversion):

- the text before the first colon that ends a word (a colon followed by a space, a `\n` or the end), when that text is
  one to four words of letters, digits and `_ . : / + -` ("vk:", "host save import:", "aud:u:", "nn::codec:");
- otherwise the format's first word, without a colon ("frame %llu: submits ..." gives "frame"); the table writes it
  without a colon too;
- a format that starts with white space continues the line before it (a block such as the crash report's registers)
  and has no prefix of its own;
- an `rt_logf` format, written as it is with no "[host] " and no newline, is a prefix only when it has the colon form
  above: the others are pieces of a line another `rt_logf` started ("outside every module)", " %08x");
- a format chosen by `?:` gives one prefix for each of its string literals.

A format that starts with a conversion ("%s", "%s: ...") takes its prefix from the text put in its place, which the
check reads only when the first argument is a plain name that the same function filled before the call with
`snprintf(name, size, "...")` or `sprintf(name, "...")`, or set with `name = "..."` (`filled_with()`). Every other
such call, and a call whose format is not a string literal, is counted as not seen. The doc lists the prefixes those
lines are known to carry in a table of their own, which is not checked for use.

Not read: `rt_fatal` and `rt_fatal_code` (their line is "FATAL: <message>", `fatal_end` of `dispatch.c`), the
"[host] " every `rt_log` line starts with, `runtime/third_party/` and the generated `*_spv.h` headers. Code under
`#if 0` and in every `#ifdef` branch is read like the rest.

`literal_text()` gives the text of adjacent string literals, escapes left as written. `scan()` gives the prefixes the
runtime writes as ({prefix: [file:line, ...]}, the unseen calls, counts). `table()` reads the doc: the prefixes of the
main table and of the table of lines the check cannot see, each with its line, and the problems; a row's prefixes are
the code spans of its first cell, and a row may name several. `check()` gives the problems, the warnings, the used
prefixes, the unseen calls and the counts.

## `tests/log_privacy_check.py`

Builds and runs `tests/log_privacy_check.c` and `tests/log_privacy_env_check.c`: the personal folders the game writes
short in its log, the bug capture and the crash report (`runtime/log_privacy.c`, `runtime/log_privacy_core.h`). No
game files.

- `tests/log_privacy_check.c`, any system, with Windows and Linux tables: `%LOCALAPPDATA%` before `%USERPROFILE%` (the
  longest folder wins), Windows paths without case and with either separator, the 8.3 form, the `\\?\` prefix, a
  profile on a network share, an account whose folder starts the same (`C:\Users\Ana Souza2`) left alone, a home with
  an accent and a space, the resolved home (`/var/home`), the account name alone inside a path but not inside another
  word (ana in banana, anabel, ana_x, anaé) nor in text without a path, the computer's name as a word anywhere (a UNC
  path, a network line) without case, common account and computer names (user, deck, localhost) not looked for alone,
  roots and short names refused, and the same text when the rewrite runs in small pieces (a caller's buffer that
  fills). It prints "case: ok" or "case: FAIL ..." per case and "log_privacy_check: ok|FAILED" at the end.
- `tests/log_privacy_env_check.c`, Linux only: `runtime/log_privacy.c` with the environment of this run, and stand-ins
  for the log, the settings and the environment: `HOME` through a symbolic link to a folder with an accent and a space
  (both forms written `~`), `USER`, the start line with the switch on and off (`log_short_paths = 0` writes everything
  in full), the computer's name from `HOSTNAME` with and without its domain, `log_privacy_str` and `log_privacy_dup`,
  and the count the exit line gives. For each argument it prints "str: <text short>" and "dup: <text short or
  (none)>", with "log: <line>" for each log line, then "count: <n>".

## `tests/logq_check.py`

Builds and runs `tests/logq_check.c`: the queue that hands log lines to the log thread (`runtime/logq.c`), with the
platform layer linked (what it calls outside itself as stand-ins, `tests/platform_stand_ins.py`) and a destination of
the test's own. No game files: the module layout the runtime includes is a stand-in written to the test's build
folder.

The sink of the C file keeps everything written, in the order it arrived; each call can take `sink_ms`, or wait while
`held` is set. Producer thread t writes lines `t <t> <seq> <padding>`. `settle()` waits until everything queued so far
is written: the queue is empty while no write of it is under way (its io lock). `check_lines()` checks that every line
of the output is whole and that each thread's lines come in their order. The line about slow writes (`say_slow()` of
`logq.c`) comes when a write happened to take 2 ms, which on a busy machine is any write; the checks count bytes and
writes, so `slow_line()` takes the flag `say_slow()` tries, and no such line is written until the test gives the flag
back (taking it waits for a line that is being said).

1. Before the queue is on, a line is written by the thread that logs it, before `logq_write()` returns.
2. With the queue: eight threads, 20,000 lines each; every line arrives whole and each thread's in order.
3. A thread that finds the queue's lock taken waits for it, so its line does not pass a line queued before it. The
   test takes both locks itself: the first line stays queued, and the second thread finds the lock taken.
4. A slow destination (30 ms a write) does not hold the threads that log: 100 lines, a millisecond apart. A thread
   that waited for the writes would take 30 ms for most lines; on a busy machine the scheduler alone can hold a few
   that long, so the check is how many did.
5. The line about slow writes: one after the first slow write, and no second one within ten seconds of it. The log
   thread says its line after the write, with no lock held.
6. A destination that stops while more is logged than the queue holds (8 x 2000 lines of 260 bytes, four times the
   queue): the threads wait, nothing is lost.
7. The log thread gathers: lines logged together are written together, `LOGQ_GATHER_NS` after the first of them and
   not before. The clock says whether the test's thread logged its lines and looked at the output within half that
   time; on a machine too busy for that, the two checks that need it are left out.
8. A queue that is half full is written at once, not when the gathering time is up, so a burst that filled one half
   has the other for what it logs next. Each try logs over half the queue's size together (2100 lines of 260 bytes:
   546,000 of 1,048,576) and sees how soon a write came; on a busy machine the log thread may get its processor late,
   so one try in three has to show it.
9. `logq_flush()`: what is queued is written before it returns, inside the gathering time, and the queue stays on.
   With the log thread stuck in a write (the test holds its io lock) it gives up within its bound and writes nothing,
   and the line is written once the write is over. `logq_nudge()` (a focus loss): the log thread writes what it
   gathers at once, not when the gathering time is up; on a machine too busy to log and look within half that time
   the timing check is left out.
10. `logq_sync()`: what is queued is written before it returns, and later lines are written by their own thread. The
    lines are logged together just before it, so the log thread is still gathering them: the batch a fatal error, a
    crash or the end of the process finds queued. A second call does nothing.

After its checks the program is run twice more (`ends()`), as `logq_check exit <file>` and `logq_check fatal <file>`:
a process that logs `END_LINES` (200) lines together, through the queue and into the file, and ends at once, while the
log thread is still gathering them: `exit` as a run ends (`plat_exit`, whose hook is `logq_sync()`, as `log_open` of
`main.c` sets it), `fatal` as a fatal error ends one (`fatal_end` of `dispatch.c`: `logq_sync()`, then its own line,
then the end). The exit code is the one the process ended with, and each file has to hold every line it logged, the
fatal line last.

## `tests/log_stall_check.py`

Builds and runs `tests/log_stall_check.c`: `runtime/log_stall.c` (the local time of the lines that mark an event, the
counts of the events whose own lines are limited, the memory and I/O pressure line) and
`runtime/platform/proc_pressure.h` (the readers of `/proc/pressure/*` and `/proc/meminfo`), with the platform's
clocks, zone and pressure readings replaced by stand-ins. No game files, no GPU, any Python.

`static_checks()` then checks that each place that writes one of those lines goes through them: the worst frame of a
frame-time window, the presenter's failed acquires and watchdog, the physical memory map's refusals, a slow pipeline
cache save, a fatal error, a crash report and a lost device, and the dma-buf spans at the end of the run. `source()`
gives a runtime file without its comments, so a name in a comment is not a call.

- The stand-in zone is UTC-3 (10800 seconds west); the zone's wall clock is UTC shifted, so the stand-in reads it as
  UTC. The times: 1795276795, 2026-11-21 15:59:55 UTC, a Saturday, which is 12:59:55 in UTC-3; and 1795230001,
  00:00:01 in UTC-3 (03:00:01 UTC), for the stamp across midnight. Only differences of the monotonic clock count.
- The readers of the pressure files: a line that is not there or not whole leaves the values alone; the second line is
  found past the first, and the kind must start a line; a locale whose decimal separator is a comma (where `strtod`
  would stop at the point) reads the same.
- `log_stamp()`: the moment of now gives 12:59:55; 4.5 s ago 12:59:50; a frame of a window that began a minute ago (65
  s) 12:58:50; a moment ahead of the clock (a race) reads as now. A zone the platform cannot name; a buffer too small,
  and none at all (nothing written); another zone, UTC+5:30, with its half-hour offset.
- The lines of an event are written up to its limit and the count goes on. When the window moves on with nothing new,
  nothing is written; then the next count and the run's totals, as in "presenter acquire failed 10 (only the first 5
  logged)" and "frame generation watchdog fired 1 (5 in all, only the first 3 logged)". The whole run's text leaves
  the window alone. A text is cut to the buffer. An event that has not passed its limit says nothing about lines: the
  count alone in its first window, then "in all". Any thread counts: 8 threads, 20000 each, give exactly as many lines
  as the limit, and every count is in the total.
- The pressure line: with no earlier sample, the averages and the memory alone; a total that went down (a sample from
  another boot, a wrap) counts as no stall; a kernel without PSI still gives the memory figures, and the line says why
  there is no pressure; PSI for memory but not for io (a kernel booted with its io accounting off); Windows has no
  stall counters and gives the memory available and the commit; nothing at all gives no line.
- The sampling through the platform: the baseline, then the next line against it. A busy sampling lock is not waited
  for, and takes no sample.

## `tests/log_trace_check.py`

The log lines of the pipeline cache stamp, the play gate of the background builds and the presentation and sampler
failures, as the runtime's own code writes them: each function is cut from its source file by its text (`function()`:
from its signature through the closing brace at the start of a line), built with stand-ins for what it calls (real
files in a scratch folder for the stamp, a clock the harness moves for the play gate, a `vkCreateSampler` that fails
on request), and run through a session's events. What its `rt_log` calls write goes to
`build/test/log_trace/nx2saria-sample.log`, and each line a player's log is read by must be there with the numbers the
events give. No game files, no GPU (Vulkan headers only; the pinned ones of `tools/build_game.py` under
`toolchain/vulkan-headers/include` when they were fetched).

- `runtime/vk/vk_pipeline.c` `cache_stamp_check()` over the starts of an update and a preparation under yet another
  translator: "vk: pipeline cache stamp <id> none|matches|differs: <action>, <MB>", the files deleted or kept as the
  line says. The starts: none; matches; differs, kept for the carry; differs, read while a file is held open; differs,
  deleted (650 MB, both files); matches with the file still due (its run ended before its exit); matches once
  written; a preparation's deletion (it writes the file itself); off. Only a start in play without
  `pipeline-cache.bin` says that its exit writes it (which saves write it: `tests/cache_stamp_check.c`). The files
  are kept while one is held open (start 4) and gone at start 5.
- The play gate (the part of `vk_pipeline.c` from "the play gate" to `vk_pipeline_probe_buildgate`): the start's line,
  then frames shown while builders rest, and "vk: background builds paused in play: N frames, M builds deferred" with
  those counts; with the setting at 0 the start's "off" line and no other. In the test the gate is on at 600 and rests
  only in play; of the 120 heavy frames the first came before the builders began their rest, so 119 count; five builds
  are held back, each counted once though two builders rested.
- `runtime/gfx/gfx_present.c` `swapchain_unusable()` failing 20 times: "gfx: vkGetSwapchainImagesKHR failed (5), N
  times: ..." for N = 1 to 8 and 16 only.
- `runtime/vk/vk_texture.c` `make_sampler()`: a failed `vkCreateSampler` that the plain retry makes, and one it does
  not (it falls back), each "gfx: vkCreateSampler failed (-1), N times: ..." with what stood in; four calls, two of
  them plain.
