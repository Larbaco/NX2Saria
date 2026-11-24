# tools

The build, packaging and release tools. This page is started: it holds the notes of the code that has had no comments
from the start, the licence parts of the two release scripts. The rest of `tools/` keeps its comments until the area is
extracted ([README.md](README.md#areas)). What ships with which licence is in
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md); `tests/shipped_notices_check.py` keeps it in step with the
scripts ([tests.md](tests.md)).

## `tools/ab_summary.py`

The in-launch A/B summary of a `NX2SARIA_AB` run (`runtime/diag/perf.c` `rt_ab()`): per perf window of 60 presents
that lies wholly in an A or a B block, the frame interval and the replay time; then their means per frame on each
side, and B - A.

```
ab_summary.py <log> [--skip <seconds>]
```

### `main()`

A window across a block edge (its B count is neither zero nor all of its frames) is left out. The means are per frame
over the side's frames: each window weighs as many frames as it holds.

## `tools/appimage_gui.py`

The Linux AppImage's window (`tools/make_appimage.py`): pick the dump and the game folder, install or update the game
with progress, and play. AppRun opens it when the AppImage starts without arguments on a desktop (double-click); the
command line keeps working as before.

```
python3 tools/appimage_gui.py --apprun <AppRun> [--icon <png>]
```

It runs AppRun itself with `--events`, which does what the command line does (the package check, zbic, `make_game.py`,
NVIDIA's DLSS, `play.sh`) and prints `make_game.py`'s "@@" progress lines (`tools/make_game.py`, `--events`) plus its
own for the steps around it. The last dump and folder are remembered in `~/.config/nx2saria/appimage.json`, the last
run's output in `~/.cache/nx2saria/build.log` (Details opens it). Existing `~/.config/nx2navi` and `~/.cache/nx2navi`
folders are reused in place when the new folders do not exist (`_brand_dir()`).

The window is one Tk canvas. The Tk that comes with python-build-standalone has no Xft, so its only font is X11's
bitmap "fixed", unreadable on a high-DPI screen: text is drawn with Pillow in the desktop's own font (`fc-match
sans-serif`) as PNG images, everything is sized by the screen's DPI (Tk's `winfo_fpixels`, which follows `Xft.dpi`),
and the file choosers are the desktop's (kdialog or zenity, else Tk's). Without `fc-match` or Pillow, text falls back
to Tk's.

- The script puts its own folder on `sys.path` before it imports `private_paths`: AppRun starts it with `python -I`,
  which leaves the script's folder off the path. That import carries `# noqa: E402`, which stays.
- `STEPS` is `make_game.py`'s steps (its `STEPS`, plus `check`) and AppRun's own around them, in the order they run.

### `Text`

Text as PNG images in the desktop's font (Pillow), cached; `None` when that is not possible, and the caller then uses
Tk's text.

### `pick_file()`

The desktop's file chooser (kdialog, zenity), else Tk's.

### `exec_quote()`

A path as one quoted argument of a desktop entry's `Exec` line: inside the quotes a backslash goes before a quote, a
backtick, a dollar sign and a backslash, the line itself doubles each backslash again, and a percent sign is doubled so
that it is no field code.

### `App`

`W, H = 760, 640` are logical pixels (96 dpi); everything is drawn at the screen's scale. `rows` holds, per step, its
fraction done and its state. `App.button()` draws a button whose right edge is at `x1` and returns its left edge.
`App.draw()` draws, top to bottom, the dump and the folder, the steps, then the status, the shortcut box and the
buttons.

### `App.line()`

A line of the build's output goes through `tools/private_paths.py` `shorten()` before it is kept: `build.log` is
attached to issues, so the account's folders are written short ([log-privacy.md](../log-privacy.md)).

### `App.details()`

While a build runs, the log written so far is flushed first, so Details shows the run in progress as far as it went.

## `tools/appimage_install.py`

Installs or repairs the Python launcher from the AppImage, then starts the installed copy
([launcher.md](../launcher.md)).

### `_install()`

- An older AppImage can repair the desktop entry, but cannot replace a newer working installation: when the installed
  version is complete (its `VERSION` matches `app/current.txt` and `python/bin/python3` is there) and newer than the
  AppImage's, that one stays.
- The old AppImage's `out` (its `appimage.json`) is the user's existing game folder. It is adopted in place as the
  launcher's `game_dir`, and its `nup` or `dump` as the launcher's `dump`.

## `tools/bench.py`

Benchmark scenes: the same place, route and camera every run, every frame timed, a summary per scene. How to use it
and how to read its summary are in [performance.md](../performance.md); `--boot-map` in
[boot-to-map.md](../boot-to-map.md).

```
python tools/bench.py run <game folder> --save <backup> [--scenes tools/bench/scenes.toml] [--only kokiri,...]
                      [--runs 3] [--out build/bench/<date>-<label>] [--label base] [--set KEY=VALUE ...]
                      [--mangohud] [--shot]
python tools/bench.py summary <out folder>        # the summary again from the logs a run left
```

Each run starts the game through `tools/run_game.py` on a copy of its profile with `<backup>` as the save
(`run_game.py` says which saves it takes). The input script presses A until the save has loaded (untimed lines, which
stop applying once the world clock starts), then `NX2SARIA_DBG_TEST` teleports Link to the scene's area and turns the
free camera on (`runtime/diag/debug_tools.c` `test_script()`), and timed `LSTICK` and `RSTICK` lines fly it through
the area: the game draws, culls and loads detail for the view it is given, and nothing in its way (walls, water, a
villager who wants to talk) stops it or changes the path. Two `MARK` lines in the script open the 'measure' segment
after the scene's warm-up and close it at its end (`runtime/diag/perf.c` `seg_frame()`).

What is measured comes from the game's own log with `NX2SARIA_TRACE=perf` and `NX2SARIA_PERF_SLOW_MS=1`, so every
present gets a "perf: slow frame" line (`runtime/diag/perf.c` `perf_present()`): its interval and what the render
thread waited for. A frame is

- `gpu` when the render thread waited for the GPU's fence for at least half of it, or when the GPU was busy for 80% of
  it or more (the GPU time per frame of the 60-frame window the frame is in, the runtime's own rule: with
  `NX2SARIA_REPLAY_ASYNC=1` the replay threads wait for the GPU, not the render thread),
- `stall` when pipeline builds, shader translation and texture uploads took at least a quarter of it,
- `pace` when the frame limiter held it for at least 15% of it (should not happen: the runs are uncapped),
- `cpu` otherwise: the game thread and the replay of its draws set the pace.

The 60-frame window lines add the GPU's own busy time (timestamps) and the runtime's own bound verdict
(`runtime/diag/perf_bound.h`: pace, gpu, serial or cpu), which `summary.md` gives as "Win bound".

1% low is the frame rate of the mean of the slowest 1% of frames; p99 is the 99th percentile frame time. The run's CSV
(one row per frame), the game's log and `summary.md` / `summary.csv` go to the out folder, with the commit, the
settings and the machine. Only ignored folders are written: `build/` and the game folder.

The game runs as it does for a player: pipelines are built in the background and a draw whose pipeline is not ready
yet is skipped. `--shot` also saves a PNG of the last frame, for comparing pictures between builds, but the runtime
then builds every pipeline on the render thread before it draws (`runtime/boot.c`), so the picture does not depend on
build timing: a scene's first frames stall far longer than in play, and its numbers are not comparable with a run
without it.

`--boot-map` starts each scene with a boot map (`runtime/patches/boot_map.c`) instead of the title screen and a
teleport: the scene's first "warp <area> <start position>" step becomes `NX2SARIA_BOOT_MAP=<area>:<start position>`,
the input script presses nothing, and the game loads the area once, from the save, with no title or file menu first.
The warm-up then counts from the arrival rather than from the save's own area, so its numbers are compared with other
`--boot-map` runs only (`meta.json` says which).

`--mangohud` runs the game under MangoHud with logging on (`MANGOHUD=1`) and compares the frame times it saw over the
same stretch with the game's own.

The patterns for the log lines (`SLOW_RE`, `SEG_START_RE`, `SEG_END_RE`, `WIN_RE`, `WIN_REPLAY_RE`) match format strings
of `runtime/diag/perf.c`; `tests/bench_check.py` checks that those strings are still there.

### `bound_of()`

The side that set the frame's length, from what the render thread waited for and the GPU time per frame of the frame's
window, by the rule above.

### `parse_log()`

Frames, segments and 60-frame windows of a game log.

### `mark_bounds()`

Each frame's bound, with the GPU time of the 60-frame window it is in: the window line at frame N covers the frames
N-59 to N. A frame after the last window line has the render thread's waits alone.

### `measured()`

The frames of the measured segment: after the present that opened it, up to the present that closed it.

### `percentile()`

Linear interpolation between closest ranks, `q` in [0, 1].

### `stats()`

Average fps, 1% low fps (mean of the slowest 1%), p99 and worst frame time, frames over 33.3 ms.

### `spread()`

(mean, min, max, spread in % of the mean) of the runs' values.

### `mangohud_frames()`

MangoHud's log is comma-separated rows after a header line that names `fps` and `frametime`. MangoHud writes frame
times in ms; older versions wrote microseconds, so when the median is over 1000 the values are divided by 1000.

### `mangohud_window()`

MangoHud's frames over the stretch the game measured: both logs end at the same exit, so the stretch is the one that
ends as long before the end of MangoHud's log as the game's last frames after the segment took. `eps` is half a
millisecond: the sums of float frame times are not exact.

### `make_script()`

Untimed A presses until the save loads (they stop applying once the world clock starts), then the camera turns and the
measure marks on the world clock.

### `boot_map_of()`

(boot map value, the scene without its teleport) for `--boot-map`: the first warp step of the scene's `dbg` list is the
place to boot to; `None` when the scene has no warp.

### `recorded_build_commit()`

The last successful build record for this output (the last line of the game folder's `build-commits.txt`), or `None`
when it cannot identify the build. A later failed or malformed record is not evidence that the executable still
represents an earlier success. This is the build script's record, not a hash of the executable itself. The record's
time is written in whole seconds, and a newer executable could have been put there by an interrupted or outside build:
an executable more than one second newer than the record is not taken as that build. The one second allows for the
record's timestamp precision; it is not proof of identity.

### `cmd_run()`

- The out folder is made absolute: the game runs in its own folder, and MangoHud's `output_folder` is read from there.
- The settings file copied beside the results is the one the game reads: `nx2saria.ini`, or `nx2navi.ini` from before
  the game was called NX2Saria.
- The runs go scene after scene inside each round, so a drift over time hits every scene alike.
- MangoHud runs with `fps_only`, not `no_display`: MangoHud 0.8.4 stops its frame updates under `no_display`, and with
  them the timer that starts `autostart_log`, so it would write no log at all. The overlay draws on the window, not on
  the game's picture that `--shot` saves.

### `summarize()`

A result folder whose `meta.json` has no `shot` key is reported as `--shot`: results from before that key always ran
with it.

## `tools/build_dlssg.py`

Builds `nx2navi_dlssg.dll` (`runtime/dlssg/nx2_sl.cpp`, DLSS Frame Generation through NVIDIA Streamline) with MSVC.

```
python tools/build_dlssg.py --sl <Streamline SDK folder with include/> [--out build/dlssg]
python tools/build_dlssg.py --check <nx2navi_dlssg.dll>
```

Streamline's C++ types have MSVC's ABI, so this one DLL is built with Visual Studio's compiler (the Build Tools are
enough; vswhere finds them), against the SDK's headers, which stay where they are. The Vulkan headers are MSYS2's. A
launcher release and the build kit ship the DLL ready-made beside Streamline's own runtime DLLs
(`tools/make_launcher.py` and `tools/make_kit.py` `--dlssg`, with the Streamline files `tools/streamline_pin.py`
pins), and `tools/make_game.py` `place_dlssg()` puts them beside the game.

The built DLL must export every function `runtime/dlssg/nx2_sl.h` declares (each `PFN_nx2sl_<name>` is the export
`nx2sl_<name>`), or the build fails naming the ones it lacks: the runtime takes a missing optional export for an older
DLL and leaves its feature out without an error. `--check` makes the same check of a DLL built before, such as the one
a release is about to ship; `tools/make_launcher.py` and `tools/make_kit.py` make it on their `--dlssg` DLL first and
stop on one that fails ([launcher.md](../launcher.md), [building.md](../building.md)).

### `declared_exports()`

The exports `nx2_sl.h` declares: `nx2sl_<name>` for each function pointer type `PFN_nx2sl_<name>`.

### `pe_exports()`

The names a PE file (PE32 or PE32+) exports: its export directory's name table; empty without one. The export
directory is data directory 0, 112 bytes into a PE32+ optional header and 96 into a PE32 one.

### `missing_exports()`

The exports the header declares that the DLL lacks, sorted.

### `check()`

Exits naming the exports `nx2_sl.h` declares that the DLL lacks, or saying that the DLL is missing or no PE file
(`pe_exports()` raises `ValueError`, `TypeError` or `struct.error` on a file without a PE signature or with a table
outside the file). `tools/make_launcher.py` and `tools/make_kit.py` run it on their `--dlssg` DLL before anything is
downloaded or built.

### `main()`

- Of MSYS2's include folder only the Vulkan headers are given to the compiler (copied into a folder of their own):
  MSYS2's own C library headers must not shadow MSVC's.
- The folder of `vswhere.exe` is put on `PATH` for the build: `vcvars64.bat` runs vswhere itself and needs it there.

## `tools/build_game.py`

The developer build of a game version from its generated C (`docs/building.md`). The notes of its code that changed
since the comments rule are here; the rest of the file still has its comments.

### `runtime_compiles()`

The runtime files a build compiles: every runtime source but those `--skip-runtime` names, and the `modules.c` the
build writes. There is no split into a prebuilt library and per-install sources any more: that existed for the setup
that installed the game from a release folder, which is gone; the launcher's prebuilt runtime is
`tools/make_game.py` `make_runtime_lib()`.

### `ninja_file()`

The `build.ninja` text and the object lists per module. With `prebuilt` the generated objects already exist in
`out/obj` and are link inputs without a compile edge. Off Windows the runtime's flags take `-fdeclspec`:
`runtime/third_party/ffx_api/ffx_api.h` declares its entry points `__declspec(dllexport)` without a condition, so
`runtime/vk/vk_ffx.cpp` and `runtime/vk/vk_fg.cpp` do not compile on Linux without it (`tools/make_game.py`
`runtime_cmd()` adds the same flag).

## `tools/make_appimage.py`

`KIT_DIRS` holds `licenses/`: the licence texts `THIRD_PARTY_NOTICES.md` names that are not beside their code (the
shader translator's, MacNativeGames', the AppImage runtime's). They go into the kit, `usr/kit/licenses/`.

`PYTHON_REMOVED` lists the extension modules taken out of the AppImage's Python, each with why. Nothing the launcher,
the kit's tools or the game build imports needs them; `tests/shipped_notices_check.py` checks the list and greps the
imports. `_dbm` is the one so far: it links Berkeley DB 6.0.19, whose Sleepycat licence asks for the complete source of
every program that uses it. `dbm.ndbm` is its only user; `dbm.dumb` still works without it.

`RUNTIME_PINS` is what the Linux runtime's id is made from (`tools/linux_runtime.py` `runtime_id()`): the packages and
the modules taken out, so that a change of either brings the launcher a new runtime. `tools/package_launcher_linux.py`
`main()` takes the same tuple for the runtime zip.

### `removed_modules()`

The files of a Python folder's `PYTHON_REMOVED` modules, in `lib/python3*/lib-dynload/`.

### `main()`

Before anything is built, `tools/python_licences.py` `check_build()` stops on a Python that is not the build its
licence texts are pinned for. After the packages are installed, the removed modules are deleted, and
`pbs_licences()` and `cpython_license()` of that file put the texts of what python-build-standalone links into the
Python, and CPython's incorporated software, into `usr/kit/licenses/python/`.

## `tools/make_launcher.py`

### `build_app()`

Copies `licenses/` into `app/<version>/` beside `runtime/` and `recomp/`, for the same texts. The app zip carries it, so
an update brings it without a new Python runtime.

### `python_texts()`

Writes `app/<version>/licenses/python/cpython-license.rst` (`tools/python_licences.py` `cpython_license()`): CPython's
licences of the software it incorporates, for what the embeddable Python's own `LICENSE.txt` leaves out (expat,
mpdecimal). Then every library `WINDOWS_TEXTS` names has its text, found by one of its lines, in the file that table
says, or the release stops. The texts live in the app zip, so the Python runtime and its id do not change.

## `tools/package_launcher_linux.py`

### `main()`

`APP_DIRS` holds `licenses/`, taken from the AppImage's kit like `toolchain/` and `prompt-art/`, so the Linux app zip
carries the same texts as the kit, the Python's among them.

## `tools/python_licences.py`

The licence texts of the libraries linked into the Python each release ships, taken at release time from pinned
downloads of the same Python version (`tools/make_launcher.py`, `tools/make_appimage.py`). The archives are downloaded
once into `build/dl` and checked against their SHA-256. A new Python version, or a uv that installs another build,
needs new pins here; `tools/make_appimage.py` stops when the Python it is given is another build.

- `CPYTHON_SOURCE`: CPython's source archive of `VERSION`. Its `Doc/license.rst` ("Licenses and Acknowledgements for
  Incorporated Software": OpenSSL, expat, libffi, zlib, libmpdec, mimalloc and the code CPython itself carries) goes
  into `licenses/python/` of both releases as `CPYTHON_TEXT`. The Windows embeddable Python's own `LICENSE.txt` appends
  the texts of bzip2, libffi, Zstandard, OpenSSL's Apache-2.0 and Tcl/Tk, but not those of expat and mpdecimal.
- `PBS_FULL`: python-build-standalone's "full" archive of the build the AppImage's Python is: what
  `.github/workflows/linux-appimage.yml`'s `uv python install VERSION` installs, the "install_only" build of
  `PBS_BUILD`. That build carries CPython's licence alone; the full archive's `python/licenses/` has one text per
  library it links in, and only that folder goes into the AppImage.
- `WINDOWS_TEXTS`: the libraries linked into the Windows embeddable Python (and the Tcl/Tk of `tcltk.msi`), each with
  the release file that holds its text and one line of that text. `None` is a licence that asks for no text with a
  binary: SQLite and libtommath are public domain, liblzma public domain (0BSD since 5.6).
- `LINUX_TEXTS`: the libraries python-build-standalone links into the AppImage's Python, each with its file in the full
  archive's `python/licenses/`. Berkeley DB is not among them: `tools/make_appimage.py` removes `_dbm`, the one module
  that links it (its text still comes with the folder).

### `fetch()`

The pinned file in the cache, downloaded when missing or different, through a `.part` file; a wrong SHA-256 stops.

### `cpython_license()`

`Doc/license.rst` of the source archive, written as `CPYTHON_TEXT` in the folder given.

### `check_build()`

Why a Python folder is not the build `PBS_FULL` describes, or `None`: its `BUILD` file names the python-build-standalone
release and `bin/python3` the version.

### `pbs_licences()`

The full archive's `python/licenses/*` into the folder given, unpacked with the system's `tar --zstd` (the AppImage is
built with Python 3.12, whose `tarfile` reads no zstd); every name `LINUX_TEXTS` lists must be among them.
