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
