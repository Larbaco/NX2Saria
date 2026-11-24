# tools

The build, packaging, release, check and diagnosis tools, one section per file in the order of the file names. Three
files keep comments for now, because their bytes or lines feed a key on every player's PC, and their sections say so:
`tools/cheats.py` and `tools/exefs_patches.py` (all of them) and `tools/button_icons.py` (the comments that follow
code on a line). `tools/vendor/` and `tools/xlate/` are not part of the area ([README.md](README.md#the-checks)). A
tool's `--help` text is the string `DESCRIPTION` in its file. What ships with which licence is in
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

The developer build of a game version from its generated C ([building.md](../building.md)): it compiles the generated
C for one game version and links it with the runtime into `build/<version>/nx2saria.exe`. It writes
`build/<version>/build.ninja` and runs MSYS2 UCRT64 Ninja on it. Ninja keeps the build incremental: a chunk is
recompiled only when it, a header it includes or its flags change. Standard library only.

On Linux the same steps make `build/<version>/nx2saria` with the clang, lld and ninja on `PATH` (or the clang and lld
in `--toolchain DIR/bin`): the runtime's `*_sdl.*` and `platform/linux.c` take the place of its `*_win.*`,
`platform/win32.c` and `nx2saria.rc`, the libraries are the system's (Vulkan, SDL3, Opus, dav1d, libvpx, zstd;
libstdc++ static), and the executable finds a `libSDL3.so.0` beside it first (rpath `$ORIGIN`). building.md lists the
packages.

```
.venv\Scripts\python tools\build_game.py                     # generated/v121 -> build/v121/nx2saria.exe
.venv\Scripts\python tools\build_game.py --modules subsdk0   # one module (links if it has no calls into others)
.venv\Scripts\python tools\build_game.py --jobs 8
.venv\Scripts\python tools\build_game.py --out build/v121-t2   # a separate build directory
.venv\Scripts\python tools\build_game.py --toolchain toolchain/llvm-mingw-<version>/llvm-mingw-<version>-ucrt-x86_64 \
    --out build/v121-llvmmingw                                    # llvm-mingw instead of MSYS2 UCRT64
```

Every runtime source is compiled. `build/<version>/modules.c` carries the layout, the linked function tables, the
expected SHA-256 of each ExeFS file (`tools/data/expected-<display version>.json`) and the default game paths
(`--exefs`, `--romfs`, `--profile`), which `nx2saria.exe` uses unless its own command line names others. `--exefs` and
`--romfs` default to `extracted/<version>` in the work folder: the `[work]` `dir` of `config.local.toml`, where
`tools/extract.py` writes them, else `build/`. `tools/romfs_index.py` writes `build/<version>/romfs-index.bin` and
`romfs_config.h` from the RomFS directory's `index.json` (`runtime/hle/romfs.c` reads the index next to
`nx2saria.exe`). Without that `index.json` the build stops: the exe would stop at start. `--no-romfs` builds it anyway.

**Private builds reuse the main build's objects.** When `--out` differs from `build/<version>`, the main build's
generated objects are up to date (checked with a dry run of its `build.ninja`) and every header they include from
`runtime/` is byte-identical in `--runtime` (the list comes from the main build's Ninja deps log), `build_game.py`
hardlinks those objects into `<out>/obj` and compiles only the runtime. A hardlink is a snapshot: clang and Ninja
replace an object by renaming a new file over it, so a later main rebuild leaves the linked copies alone. Otherwise (a
header differs, a main object is missing or stale, another `--gen`) it compiles everything and prints one line saying
why. `--full-build` skips the check.

**Object manifest.** A flipped bit in an object written by an earlier build stays forever, because Ninja rebuilds an
object only when its inputs change, and forks hardlink the object into every private build. So each build dir keeps
`obj/objects.sha256` (SHA-256, size and mtime of every object it compiled). After Ninja finishes and before the link,
`build_game.py` hashes the objects, compares each one whose size and mtime match its manifest entry, and records the
objects Ninja just rebuilt (new mtime). A mismatch deletes the object, Ninja recompiles it, and the build prints the
summary line. A fork checks the hardlinked generated objects against `build/<version>/obj/objects.sha256` (read-only;
a bad shared object stops the fork and asks for a main build run, which recompiles it and breaks the shared inode).

**Memory** ([build-memory.md](../build-memory.md)). Ninja runs up to `--jobs` compiles at once (default one per
processor), and each compile goes through the memory gate (`tools/memgate.py` `wrap`): it starts only when the free
memory, less what the running compiles may still take and less a reserve, holds that compile's estimated peak. The
estimates live in `<out>/memgate/estimates.json`: each object's peak measured in earlier builds, else a size model
from the lift manifest's `chunk_info`, else 400 MiB (C) or 1 GiB (C++) for a runtime source. Before Ninja,
`build_game.py` writes the gate's settings and every object's need into `<out>/memgate`; after the compile phase it
reads the peaks the wrappers recorded, learns them and prints a summary (held compiles, the largest peaks, the peak of
all compiles together, the least free memory, how close the estimates came). `--no-mem-gate` (or
`NX2SARIA_BUILD_MEM_GATE=off`) starts compiles by the job count alone, and then nothing limits the compiles by memory;
the wrappers still measure. Each build folder has its own gate, so two builds running at once see each other only
through the free memory. The wrapper's command names this Python and `tools/memgate.py` (relative to the root where
they lie below it): a build with another Python, and the first build after the wrapper was introduced, compile every
object again. Under `SCCACHE` the compiles run in sccache's server, out of the wrapper's reach, so `build.ninja`
leaves the wrapper out and the gate is off. Each phase also reports its wall time and the peak memory of its processes
(Windows job object accounting: committed private bytes).

**`--toolchain DIR`** builds with an llvm-mingw tree (`DIR/bin/clang.exe`) instead of MSYS2 UCRT64 clang. Ninja still
comes from MSYS2. The libraries and headers MSYS2 supplies (Vulkan, SDL3, Opus, dav1d, libvpx, VMA) then come from
`--deps DIR` (default `toolchain/deps-mingw`: `include/`, `lib/` with the import libraries, `libdav1d.a` and
`libvpx.a`, `bin/` with the DLLs), and the C++ runtime is libc++ from llvm-mingw. Such a build never reuses another
build's generated objects.

The module's constants and what their values are for:

- `ctypes.wintypes` is imported on every OS for the job-object structures (`run_measured()`); importing it works
  everywhere.
- `MAIN_ROOT`: in a git worktree (`tools/worktree.py`) `build/` and `generated/` are junctions into the main checkout,
  and `resolve()` follows them. `MAIN_ROOT` is where they land, so resolved paths still print and compare relative to
  a root.
- `GEN_FLAGS` are the flags for generated code, plus `-march` for the FP/SIMD lowering. `--cpu v2` replaces the level
  for CPUs without AVX2/FMA; `runtime/fma.c` then supplies the correctly rounded fma the lowering needs
  (`runtime/simd.h`).
- `PGO_NAMES`: profile-guided optimisation (`--pgo`). Both modes compile the generated code at `-O2`, the level the
  profile's inlining and hot/cold layout work at. A static function's profile name carries its source path by default;
  the base name alone keeps the names equal between the training build and a build from another tree (a worktree, an
  install). `PGO_INPUT` is the `.profdata` of `--pgo FILE`: every compile depends on it, so a new profile rebuilds
  them.
- Windows flags. `-D_WIN32_WINNT=0x0A00` is Windows 10, the port's minimum: MSYS2's headers default to it,
  llvm-mingw's (the release toolchain) to Windows 7, which hides the per-monitor DPI functions
  `runtime/gfx/gfx_window_win.c` and `runtime/diag/dpi_probe_win.c` call. `-Xlinker` passes the map path as one
  argument; `-Wl,` would split it at any comma in the build dir path. The map (`nx2navi.map`) feeds
  `tools/prof_report.py`. `LINK_LIBS`: the Vulkan back end; libvpx and libdav1d static (movies); libzstd static
  (`runtime/gfx/resolution.c`). `VMA_INCLUDE` holds `vk_mem_alloc.h`, MSYS2 package `vulkan-memory-allocator`.
- Linux flags. `-no-pie`: the lifted modules' function tables (hundreds of thousands of pointers) need no load-time
  relocation. `$ORIGIN`: a `libSDL3.so.0` next to the executable comes before the system's (the release's portable
  folder). `-Xlinker` for the map path as on Windows. `VMA_INCLUDE` is `/usr/include`: Arch (AUR)
  `vulkan-memory-allocator`, Debian `libvulkan-memory-allocator-dev`.
- `VMA_FETCH`: without a system `vk_mem_alloc.h`, the header pinned here (VMA 3.4.0, the version MSYS2 packages) is
  fetched once into `toolchain/vma`: one MIT header (`THIRD_PARTY_NOTICES.md`), checked against its hash.
- `LLVM_LINK_LIBS`: `--toolchain`, an llvm-mingw tree. The libraries, headers and DLLs MSYS2 supplies come from the
  deps directory instead. `TOOLCHAIN_BIN` is the llvm-mingw `bin` directory, set by `main()` for `--toolchain`; `DEPS`
  the deps directory of an llvm-mingw build.
- `WINDRES` compiles `runtime/nx2saria.rc`, the manifest of `nx2saria.exe`. `WINDRES_PP`: GNU windres runs its
  preprocessor through `cmd.exe`, and left to find it names the gcc beside it by full path: once that path and the
  `.rc`'s both hold a space, `cmd.exe` drops the outer quotes and splits the command at the first space. gcc by bare
  name (Ninja's `PATH` starts with UCRT) and the `.rc` by its path below `MAIN_ROOT` keep the command line free of
  spaces. llvm-windres (`--toolchain`) starts its preprocessor without a shell and keeps its own.
- `DISPLAY_VERSIONS`: `tools/data/expected-<display version>.json` for each build version name.
- `RUNTIME`: `--runtime`, a copy of `runtime/`, lets a task build without touching the shared tree.
- `RUNTIME_DIRS`: the runtime's folders by subsystem ([architecture.md](../architecture.md), "Runtime folders"): their
  sources compile as the top level's do, and each is on the include path of the runtime's sources, which include one
  another's headers by name. The lifted C includes only headers at the top of `runtime/`, so its command (`genflags`)
  does not name them. `tools/make_game.py` has the same list (`tests/runtime_dirs_check.py`).
- `GATE_DIR`: `<out>/memgate`, the gate's state folder (`tools/memgate.py` `prepare_state()`) and `estimates.json`.
  `DEFAULT_NEED_C` (400 MiB) is a runtime source's need until its peak has been measured once. `NINJA_GRACE` is the
  seconds Ninja gets to stop its compiles after Ctrl+C before it is killed.

### `on_path()`

The name on `PATH`, else a path that does not exist (the tool check in `main()` then names it).

### `set_cpu()`

Targets x86-64-v3 (the default) or x86-64-v2. `RT_CPU_LEVEL` lets `runtime/main.c`'s self-test check the same
instruction set the build was compiled for.

### `set_pgo()`

`--pgo gen`: instrument the generated code and the runtime; `nx2saria.exe` adds its counts to
`out/pgo/default_<id>.profraw` every minute and at exit (`runtime/platform/win32.c`), across runs. `--pgo FILE`:
compile with that merged profile (`tools/pgo.py` `merge`). Functions the profile never saw run are optimised for size.

- The profile folder is given as an absolute path: the instrumented exe resolves the directory against its own working
  directory.
- `-fprofile-generate` on the link line links `clang_rt.profile`.
- The warnings turned off with a profile: `unprofiled` is chunks the training never entered; `out-of-date` and
  `backend-plugin` are functions whose code changed since the training, which then compile without a profile.

### `vma_include()`

The directory holding `vk_mem_alloc.h`: the system's, else `toolchain/vma`, fetched there on first use.

### `system_vulkan_version()`

`VK_HEADER_VERSION` of the `<vulkan/vulkan_core.h>` this compiler finds by itself (its own search path, `CPATH`
included), 0 when it finds none.

### `vulkan_include()`

A build with the system's own headers (Linux without `--deps`): the folder to put ahead of them for
`<vulkan/vulkan.h>`, or `None` when the system's serve. The runtime names Vulkan extensions newer than the headers
many distributions package, and every file that does fails to compile there (`tools/vulkan_pin.py`). So:
`toolchain/vulkan-headers` when it holds the pinned release (a kit carries it; a checkout once it was fetched), else
nothing when the system's headers are at least that release, else the pinned release, fetched there once. Windows
takes MSYS2's headers and returns `None`.

### `below_root()`

Resolved `p` relative to `ROOT`, else to `MAIN_ROOT` (the same relative path through the worktree's junctions), else
absolute.

### `ninja_path()`

A path for a build line: relative to the repository root where possible, forward slashes, Ninja-escaped.

### `cmd_arg()`

`s` for a Ninja command line: Ninja-escaped, and quoted when it holds a space (a user or folder name), as clang and
lld split their Windows command line at unquoted spaces.

### `read_layout()`

`[(name, base, size)]` from `layout.json` (same format as `recomp/layout.py` `read()`, without its NSO dependency).

### `chunk_files()`

The C files of one module: the chunks listed in `<mod>/manifest.json`, then `<mod>_table.c`.

### `write_if_changed()`

Keeps the mtime of an unchanged file so Ninja does not rebuild from it. The text is written as UTF-8 with `\n` line
ends: Ninja and clang read UTF-8, and paths may be non-ASCII.

### `copy_romfs_patches()`

`nx2saria.exe` loads `romfs-patches` beside `romfs-index.bin` (`runtime/hle/romfs.c` `overlay_init()`).
`tools/make_romfs_patches.py` writes them into `build/<version>` only, so a build in another directory ran without the
60 fps overlay (the quick menus' captured background turned black). This copies them when their manifest differs;
`romfs-patches-xbox`, `romfs-patches-xbox-label` and `romfs-patches-pc` (the same with Xbox or keyboard and mouse
button prompts, `make_romfs_patches.py --prompts`) the same way when they were made.

### `work_dir()`

The work folder: the `[work]` `dir` of `config.local.toml` (relative to the checkout), where `tools/extract.py` writes
`extracted/`; `build/` without the file or the key.

### `other_os()`

A runtime file for another OS: Win32 ones (`*_win.*`, `platform/win32.c`, `nx2saria.rc`) off Windows, the SDL
window's (`*_sdl.*`) and `platform/linux.c` on Windows.

### `runtime_sources()`

Every `runtime/*.c`, `*.cpp`, `platform/*.c`, `*.rc`, the `*.c` and `*.cpp` of each `RUNTIME_DIRS` folder and Dear
ImGui of this OS except the names in `skip`, so a task adds a file without editing this script.

### `runtime_include_dirs()`

The include folders of the runtime's sources: `runtime/` and each `RUNTIME_DIRS` folder of `rt` (default `RUNTIME`).

### `runtime_object()`

`obj/runtime/<path below runtime>.o`, so `platform/win32.c` cannot collide with a top-level `win32.c`.

### `module_glue()`

C for `build/<version>/modules.c`: the layout table, the linked modules' function tables (`dispatch.h`), the expected
ExeFS hashes, the game version and the default game paths (`host.h`). The version number is the display version
without its dots (1.4.3 becomes 143). `rt_app_pool_bytes = 0xCD500000` is the Switch 1 retail application pool.
`rt_nvn_api_version = {55, 15}` is NVN1: the game's `main` requires major 55, minor >= 15.

### `port_version()`

`VERSION` at the root: the repository's semver, or in a release folder the release string.

### `sccache()`

Whether a compiler cache fronts clang (`SCCACHE`). Its server runs the compiles, out of the wrapper's process tree, so
the gate can neither measure nor pace them: `build.ninja` then leaves the wrapper out.

### `interpreter()`

This Python for a Ninja command: relative to the root when it lies below it (the usual `.venv`), else absolute. Not
resolved: a venv's python is a link whose own path selects the venv.

### `gate_command()`

The memory gate's wrapper ahead of a compile command in `build.ninja`: this Python running `tools/memgate.py` `wrap`
with the state folder `<out>/memgate` and the object (`$out`) as the name its need is filed under; `''` under
`SCCACHE`. It holds nothing that changes between runs, and its paths are relative to the root where they can be;
another Python still changes it, and with it every compile command.

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

- The runtime's include path ends with the build folder (`romfs_config.h`), the folder of `vk_mem_alloc.h` (with the
  deps headers in an llvm-mingw build) and Dear ImGui's (`imgui.h`).
- `vulkan_include()` is not asked when `DEPS` is set: the deps carry the Vulkan headers themselves. Its folder goes
  first on the path, so that no other folder answers for `<vulkan/vulkan.h>` (VMA's may be `/usr/include`).
- A compiler cache (sccache) may front clang: every lift rewrites the generated chunks with the same contents, so the
  cached objects save the compile step on the next build.
- windres writes no depfile: the files `nx2saria.rc` embeds are listed by hand on its build line.

### `main_objects()`

(`{relative object path: the main build's object}`, `''`) when the main build's generated objects can stand in for
ours, else (`None`, why). That holds when the main `build.ninja` compiles the same sources with the same flags, its
objects are up to date, and every header they include from `runtime/` is byte-identical in `rt_dir`. Ninja's dry run
runs in the main checkout: the main `build.ninja` is relative to it, and the worktree's `runtime/` files carry newer
mtimes that would make every object look stale. Header equality is checked byte for byte afterwards.

### `link_objects()`

Hardlinks the main build's objects into `out`. Clang and Ninja replace an object by renaming a new file over it, so a
link keeps the old bytes after a main rebuild. Copies where linking fails.

### `unlink_shared()`

Drops generated objects still hardlinked to another build, so the next compile writes a new file. `main()` calls it
only for a fork: forks hold hardlinks to the main build's objects; the main build keeps its own.

### `read_manifest()`

`{object path below obj/: (sha256, size, mtime_ns)}`; empty when the file is missing.

### `check_objects()`

Compares objects with the manifest `man`. An object whose size and mtime match its entry is hashed and must equal it;
any other object was just (re)built, so with `record` its hash enters `man`. Returns (verified, bad, unrecorded): the
objects with a wrong hash, and with `record` false the ones without a current entry.

### `verify_objects()`

Verifies every object before the link, as "Object manifest" above describes. `borrowed`: the hardlinked generated
objects of `main_out` (checked against its manifest, never recorded); the others belong to `out`.

### `chunk_costs()`

The memory gate works on what each compile needs, before Ninja, and what it took, after. This is
`{file name: (cost, switch_fn)}` of one module's generated C, from its manifest's `chunk_info` (`recomp/gen.py`):
"cost" (the bytes of ordinary C the compile is worth) and "switch_fn" (the bytes of the largest function with a
jump-table branch, which drives the compiler's peak) when given, else "bytes" and 0. Files it does not list are absent
(the caller takes their size).

### `Compile`

One compile through the gate: its source, the source's size, its cost and `switch_fn` (the size model's inputs; 0 for
a runtime source) and the need to take until it has been measured (`None`: the size model's).

### `gate_compiles()`

`{object name as the wrapper sees $out: Compile}` of every compile `build.ninja` runs through the gate. `mods`: the
generated C per module (compiled unless prebuilt); `runtime`: `runtime_compiles()` (the `.rc` goes to windres, not the
gate).

### `gate_needs()`

(`{name: bytes}` for `needs.json`, `Counter` of how each need was estimated).

### `gate_off_why()`

Why the gate is off (`--no-mem-gate` or the environment variable's setting), `''` when it is on.

### `gate_start_line()`

The line before Ninja on the memory gate.

### `learn_peaks()`

Records in `estimates` the peak of each successful compile in `peaks` (`tools/memgate.py` `read_peaks()` lines) whose
object is in `todo`. Returns `[(name, peak, seconds)]` of those.

### `gate_summary_lines()`

The compile phase's closing lines on memory, from the wrappers' `peaks.jsonl` lines (`peaks`), the compiles recorded
from them (`measured`: `learn_peaks()`), the estimates and the `TreeSampler` that watched the phase. `stats()` gives
peak / estimate; the line says estimate / peak, so the lowest is the worst under-estimate.

### `TreeSampler`

Every `poll` seconds between `start(pid)` and `stop()`: the most memory resident in the process tree under `pid`
(Ninja, the gate's wrappers and the compilers) and the least free memory, for the compile phase's summary. A reading
that fails (any exception in `TreeSampler.sample()`) only loses one sample.

### `run_measured()`

Runs `cmd` in its own Windows job object, which gives the peak memory of a process tree. Returns (exit code, seconds,
peak bytes of one process, peak bytes of all). Off Windows: the largest resident set of one child process
(`ru_maxrss`, KiB on Linux), and 0 for all of them (no job accounting). `on_start(pid)` is called once the process
runs.

- Ctrl+C reached Ninja too (same process group); anything else asks it to stop the same way. Ninja then ends its
  compiles; it is killed if it has not stopped within `NINJA_GRACE`.
- The job has `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so Ctrl+C stops every compile.
- The process starts suspended until it is in the job, so the compilers it starts are counted from the first one.

### `pe_sections()`

`{section name: virtual size}` and `SizeOfImage` of a PE32+ file.

### `elf_sections()`

`{section name: size}` of the allocated sections (`SHF_ALLOC`, flag 2) of an ELF64 file, and the extent they span in
memory.

### `objdump_tool()`

The disassembler for `tools/check_isa.py`: llvm-objdump of `--toolchain`, else MSYS2's or the one on `PATH`.

### `main()`

- `DEPS` is set for `--toolchain` on Windows (default `toolchain/deps-mingw`) and for `--deps` on Linux; Linux takes
  `--deps` only, never `deps-mingw`.
- A Linux build with `--deps` is a release: Opus, dav1d, libvpx and zstd go into the executable (`-l:lib<name>.a`), so
  it needs no more than libc, Vulkan and the `libSDL3.so.0` beside it.
- `tools/build_shaders.py` `build()` runs first for the runtime's own shaders: the headers the sources include, when
  missing or stale.
- The estimates only help the next build: a file that cannot be written must not stop this one.
- With `--cpu v2` the runtime's objects go through `tools/check_isa.py` `check_objects()`: inline assembly, target
  attributes and intrinsics pass `-march=x86-64-v2` by, and one `vmovdqa` of a YMM register stops a CPU without AVX.
- The DLLs copied beside `nx2saria.exe` are the dynamic SDL3 (zlib) and its libiconv (LGPL, unmodified DLL).

## `tools/build_hactool.py`

Builds hactool: `toolchain/hactool/hactool.exe`, hactool at a pinned commit plus `tools/hactool/*.patch`
([building.md](../building.md)).

```
python tools/build_hactool.py [--src DIR] [--out DIR]
```

It fetches the pinned commit into `toolchain/src/hactool`, resets it (local changes and build output there are
discarded), applies the patches of `PATCHES` (`portable.patch`: title keys and BKTR fixes; `unicode.patch`: a UTF-8
manifest; `mbedtls-asm-clobbers.patch`) and builds a static exe with MSYS2 UCRT64 gcc; mbedtls comes with hactool's
tree. The exe and hactool's `LICENSE` go to `toolchain/hactool`, where `tools/extract.py` takes them. Run with `.venv`.
On Linux the system's cc and make build `toolchain/hactool/hactool` (`unicode.patch` only adds a Windows manifest,
under `EXEEXT` `.exe`).

### `fetch()`

Afterwards `src` holds `COMMIT` with no local changes or untracked files.

### `main()`

- On Linux `--cc` goes to make as `CC=` on the command line: the bundled mbedtls's own make reads it too.
- hactool reads `~/.switch/prod.keys` before it prints the banner and exits on a malformed key there, so the check of
  the built exe runs with an empty home (`HOME` and `USERPROFILE`): it tests the binary, not the builder's keys.

## `tools/build_shaders.py`

Builds the runtime's own shaders into C headers of SPIR-V words, and the files it embeds into headers of bytes.

```
python tools/build_shaders.py [--check] [--all] [--runtime DIR]
```

The repository holds the shaders as source only. Each becomes `runtime/<name>_spv.h`, which declares
`static const uint32_t <name>_spv[]`; the headers are build outputs (ignored by git) and `tools/build_game.py` builds
the missing and the stale ones before it compiles the runtime. Three kinds of source:

| Source | What |
|---|---|
| `runtime/shaders/<name>.comp`, `.vert`, `.frag` | GLSL, compiled with glslc (one name per shader) and checked with spirv-val; an `#include` is looked for beside the file that has it, then in `runtime/shaders/ffx3` (the FSR 3.1 passes' headers) |
| `runtime/shaders/<name>.mesh`, `.task` | the same for the mesh fallback probe's mesh and task shaders (SPIR-V 1.5) |
| `runtime/shaders/<name>.spvasm` | SPIR-V assembly, assembled with spirv-as and checked with spirv-val |
| `tools/make_dlaa_mask.py`, `tools/make_mvobj_fs.py` | shaders assembled in Python (`ASSEMBLED`); they need no other tool |

One more kind of header is no shader: a file the runtime embeds (`EMBEDDED`: the menu's font, the SMAA lookup
textures) as a list of its bytes, for a compiler without `#embed` (`runtime/ui/menu.cpp` and `runtime/vk/vk_aa.c`
include them). It needs no tool.

A compiled or assembled header records the SHA-256 of its source (with the files a GLSL source includes, from its
folder or `runtime/shaders/ffx3`) and is stale when that no longer matches; a Python one is stale when its text is not
what its script gives. glslc, spirv-as and spirv-val come from MSYS2 UCRT64 on Windows (packages
`mingw-w64-ucrt-x86_64-shaderc` and `mingw-w64-ucrt-x86_64-spirv-tools`) and from `PATH` elsewhere (shaderc,
spirv-tools). A folder that carries its headers ready-made (the build kit, `tools/make_kit.py`) needs none of them:
nothing there is stale.

- `BIN` is the MSYS2 folder on Windows; elsewhere, and without MSYS2 there, the tools come from `PATH`.
- `MESH_TARGET` is `vulkan1.2`: mesh and task shaders need SPIR-V 1.4 or later.
- `ASSEMBLED`: header name to the script beside this one that assembles it; the script's `HEADER` is the header's
  text.
- `EMBEDDED`: a file below `runtime/` the runtime embeds, to the header of its bytes, below `runtime/` too (git ignores
  it).
- `INCLUDE_DIRS`: the folders of `runtime/shaders` that glslc searches for an `#include` after the including file's
  own folder (`-I`).

### `embedded()`

True for a file of `EMBEDDED`: a binary file, read as it is.

### `included()`

The files a GLSL source includes (`#include "name"`), found as glslc finds them: beside the including file, then in
`INCLUDE_DIRS`; and theirs, in order of first use. The source is read as Latin-1: the SMAA source is not UTF-8.

### `source_hash()`

For a GLSL source the hash covers the files it includes too, so a header goes stale with them. Text is hashed with
CRLF turned into LF, so the hash is the same on a CRLF checkout.

### `bytes_header()`

The header of an embedded file: its bytes as decimal numbers, to include inside an array's braces.

### `sources()`

Every shader source with the header it becomes: the files of `runtime/shaders` and the scripts of `ASSEMBLED` that are
there (a folder without them carries the headers ready-made).

### `stale()`

The shader sources whose header is missing or no longer comes from them.

### `tool()`

The path of glslc, spirv-as or spirv-val; ends the run with the packages to install when it is not there.

### `words_of()`

`// build_shaders: keep functions` in a source turns glslc's optimisation (`-O`) off, so its functions stay functions:
the HDR tonemap, which `runtime/vk/vk_hdr.c` splices into the game's shader by function.

### `build()`

Writes the headers of the sources in `only` (default: the stale ones) and returns how many it wrote. Each is written
as a `.part` file and renamed: whole or not at all, as another build may be reading the folder.

## `tools/build_xlate.py`

Builds the shader translator: `build/xlate/nx2navi_xlate.exe` (the vendored translator 1.3.2 plus patches, NativeAOT,
SPIR-V for Vulkan).

```
python tools/build_xlate.py [--out DIR]
```

It copies `tools/vendor/ryujinx` to `build/xlate/ryujinx`, applies `tools/xlate/patches/*.patch` there (`tools/vendor`
stays byte-identical), then runs `dotnet publish`. It needs the .NET 10 SDK, the MSVC build tools and `vswhere.exe`
([building.md](../building.md)). Run with `.venv`. On Linux it builds `build/xlate/nx2navi_xlate` for linux-x64, which
NativeAOT links with the system's clang (no MSVC); NativeAOT builds for the OS it runs on only. `--out` builds into
`DIR` instead of `build/xlate` (a task build dir: `nx2saria.exe` takes the translator next to it).

Next to the exe it writes `nx2navi_xlate.id`: the translator identity, a SHA-256 over what the translator is built
from (`identity()`). `runtime/gfx/gfx_shader.c` puts it into every shader cache key, so a translator change re-keys
the cache without a hand-made `KEY_VERSION` bump. Only a build writes it: the packaging scripts
(`tools/make_launcher.py`, `tools/make_appimage.py`, `tools/make_kit.py`) take a translator whose `.id` names this
checkout's sources for one built from them (`check_built()`), so no option writes an `.id` beside an exe it did not
build.

`BUILD_SETTINGS` is what the identity covers besides the sources: the publish settings in `main()`. Change it with
them.

### `tree_hashes()`

Hashes every source file; `src/*/obj` and `src/*/bin` are build output (an IDE restore writes them, the vendor
`.gitignore` ignores them and `prepare_sources()` drops them), so they are left out.

### `mng_pin()`

The commit `tools/fetch_refs.sh` pins for the reference checkout the vendor tree is compared with. A `fetch` line of
that script may have a fifth word, which asks for the history; the commit is the fourth.

### `pinned_blobs()`

Git blob ids of `vendor/ryujinx` in the `refs/` checkout at the pinned commit. The checkout itself may be newer than
the pin or carry CRLF line ends (`core.autocrlf`), so its files are not compared.

### `blob_ids()`

Git blob ids of the source files `tree_hashes()` hashes.

### `verify_vendor()`

`tools/vendor/ryujinx` must be byte-identical to the `refs/` checkout's copy at the pinned commit (when present).

### `prepare_sources()`

`build/xlate` is inside this checkout, and git inside a repository reads a "diff --git" patch's paths from the
repository's top: those hunks fall outside the directory handed to `--directory`, git skips every file of the patch
and still exits 0. `GIT_CEILING_DIRECTORIES` stops git from walking up to the checkout, so it reads each patch as it
would outside a repository, and a "Skipped patch" line fails the build (including 0021's PRMT implementation). The
build expects exactly 40 patches.

### `strip_code()`

C# (or MSBuild XML) without its comments, trailing spaces and blank lines, string and character literals kept: what
the compiler sees, so that a comment-only edit keeps the identity (and so every shader cache key).

### `code_digest()`

SHA-256 of a file, comments left out of C# and MSBuild files (`strip_code()`).

### `identity()`

SHA-256 over what the translator is built from, comments left out: the patched tree as the build compiles it
(`build/xlate/ryujinx` from `prepare_sources()`), `tools/xlate`'s own sources (`Program.cs`, `AmpereLowering.cs`,
`MeshSpirv.cs`, `nx2navi_xlate.csproj`) and the publish settings. Without a prepared tree, the vendor tree and the
patch files as they are, byte for byte. This script's own bytes are not part of it.

### `source_identity()`

The identity a translator built from this checkout's sources gets: the vendor tree copied and patched in a temporary
folder (`prepare_sources()`) and hashed as the build hashes it (`identity()`). `identity()` over a build folder hashes
the tree its last build prepared, so a patch or vendor change made since then would not show. Nothing is compiled; it
takes a few seconds and needs git on `PATH` (`prepare_sources()` applies the patches with it).

### `check_built()`

`None` when the folder holds the translator exe and an `nx2navi_xlate.id` naming this checkout's sources
(`source_identity()`); otherwise why not. The packaging scripts stop on the reason: 0.10.0 and 0.10.1 shipped the
0.9.2 translator (identity `4d051299`) after `tools/xlate/Program.cs` had raised the texture pool to 40960 slots, so
their translations still clamped every pool handle to slot 32767 and the ground went see-through after some area
loads. `exe` is the file name on the release's OS (`EXE` is this machine's).

## `tools/button_icons.py`

Xbox and keyboard-and-mouse button icons for the game's prompts, drawn on the player's machine for
`tools/make_romfs_patches.py --prompts`. The outlines are our own shapes with letters cut out of them in the game's
own text fonts: nothing of the game is in the repository. Needs fontTools (`tools/requirements-py314.txt`).

Every prompt except the button guide in play is text in the button font, `scft/External.bfotf` of each
`Font/Font*.Nin_NX_NVN.bfarc.zs`: private-use code points from U+E040 (A, B, X, Y, then L, R, ZL, ZR, the d-pad, Plus
and Minus, the sticks). A `.bfotf` is an OpenType (CFF) font after an 8-byte header; from offset 4 on, every byte is
XORed with a 4-byte key, which the size in the header's second word gives away (`bfotf_decode()`). `redraw_font()`
draws the shoulder buttons, the triggers, Plus, Minus and the d-pad as an Xbox controller names them (LB RB, LT RT,
Menu and View, a cross), or every button as the key or mouse button the keyboard presses it with (`pc_icons()`), can
swap the glyphs of the face buttons (A with B, X with Y), and has fontTools write the font again. The archive then
grows by a kilobyte or two, more than the game's resource size table allows it: `grow_restbl()` raises those entries
(without it the game stops at start).

The button guide in play (bottom right) draws its buttons, L and ZL among them, from images of the common layout
archive instead (`timg/__Combined.bntx`, `Nt_KeyTexL_00` and `Nt_KeyTexZL_00`): `bntx_redraw()` paints the same icons
into them, the size unchanged. The Xbox prompts redraw L and ZL, the keyboard and mouse ones every image
(`PC_HUD_TEXTURES`: A, B, X, Y, L, ZL and the stick's two). Prompts drawn as fixed images (some tutorial animations)
keep the Switch icons.

The file's logic is pinned: `tools/make_game.py` `PROMPT_TOOLS` and the digest `tests/launcher_test.py` records for
this file (lines without their full-line comments, docstrings and blank lines) decide whether installs draw their
prompts again. A comment after code on the same line is part of that digest, so the 14 such comments stay in the file
until its logic next changes; their notes are here already.

- `TEXT_ARCHIVE`, `TEXT_FONTS`: the letters on the keys come from the text fonts of the default archive; the regional
  archives lack them.
- `HUD_TEXTURES`: the button guide's images and the glyph each one shows.
- Drawing is in units of the font's 1000-unit em; the round A/B/X/Y icons fill y 20..585. `K = 0.5523` is the control
  point distance of a quarter circle drawn with one cubic curve, per unit of radius.
- Keyboard and mouse prompts (`--prompts pc`): every button the keyboard drives, drawn as the key or mouse button bound
  to it, in the same filled-shape style. Each glyph keeps the advance of the game's own (a wider glyph runs into the
  text of a right-aligned guide).
- `PC_BINDS`: the keyboard defaults of `runtime/input/input_map.c` `input_map_reset()` (the `bind_` lines of
  `nx2navi.ini` apply on top, `pc_binds()`). The right stick is the camera, which the mouse drives; `SHAKE` is the key
  that shakes it (the mouse never does).
- `PC_LABELS`: what a key shows: its letter, a symbol or a short name (key names as in `nx2navi.ini`; the rest show
  their name).
- `PC_LETTER_LABELS`: the one-letter panes around the action prompt (`T_A_00` ...) are in the Normal font (Rodin B and
  Symbol): one character of it per key.
- `PC_HUD_TEXTURES`: the button guide's images in the pc style: every symbol mask of the action prompt. In the 1.4.3
  Common archive `LSt_00` shows the left stick tilted up (as U+E052) and `LSt_01` the left stick (as U+E05E).
- The button guide's images are square `R8_UNORM` masks (white key, black letter), one mip, block-linear, 72x72 up to
  1.2.1 and 108x108 in 1.4.3. Each is drawn from the icon of the same Switch button, fitted into the square.
- The resource size table is `System/Resource/ResourceSizeTable.Product.<n>.Nin_NX_NVN.rsizetable.zs`, a RESTBL whose
  entries are keyed by the CRC32 of the RomFS path without `.zs`. The game sizes the buffer it loads a file into from
  its entry, so an archive that grew needs its entry raised by as much.

### `xor()`

The data with byte i XORed with `key[i % 4]`.

### `bfotf_decode()`

(OpenType font, key) of a `.bfotf`. The font fills the rest of the file.

### `bfotf_encode()`

The `.bfotf` of `otf`: `head` (the original's first word) and the size, the rest XORed with `key` as `bfotf_decode()`
reads it. The size field holds the whole font, so its length has to be a multiple of 4 (fontTools pads its tables).

### `Glyphs`

Outlines of letters from the game's own text fonts, drawn as holes in a shape. `Glyphs.hole()` draws the text centred
on (`cx`, `cy`), at most `cap` high and `max_width` wide, its contours reversed so they cut holes.

### `text_glyphs()`

`Glyphs` over the text fonts in `blobs` (`.bfotf` files), the first that has a letter giving it.

### `rounded()`

A counter-clockwise rounded rectangle (bottom and top corner radii).

### `xbox_icons()`

Code point to (advance, `draw(pen)`) for the buttons an Xbox controller names differently from a Switch: L R = LB RB,
ZL ZR = LT RT, Plus and Minus = Menu and View, the d-pad as a cross. `g` is the `Glyphs` for the letters. The face
buttons keep the game's own glyphs (`redraw_font()` swaps them for the Nintendo layout). `menu` draws three bars, as
on the Menu button; `view` two overlapping windows, as on the View button; `dpad` a cross with an arrow cut into each
direction the prompt means (`m`, `t`, `h` are the arm centre, the half base and the height of the arrow; the arrow
heads are already clockwise, so each is a hole).

### `pc_binds()`

`PC_BINDS` with the first key of each `bind_<action>` line of a `nx2navi.ini` text over it (a line without a key
entry, or with `key:default`, keeps the default).

### `font_advances()`

Code point to advance of every glyph of the button font (a `.bfotf`).

### `pc_letter()`

The one character the panes around the action prompt show for a key.

### `pc_icons()`

Code point to (advance, `draw(pen)`) for every button glyph but Home and Capture, drawn as the key or mouse button
`binds` puts on it. `g` is the `Glyphs` for the key labels; `adv` the game's advance of each glyph
(`font_advances()`), which each drawing keeps.

- `mouse_body`: a mouse seen from above; `lit` (`'l'`, `'r'`, `'m'` or `None`) is its pressed button, filled, the
  others hollow. The holes go through a reversing pen; they never overlap each other or leave the body. With a side
  button lit, the seams around the pressed button are cut; a hollow left button has its outer top corner following the
  body; the wheel (`'m'`) is filled over the gap between the hollow buttons.
- `key_cap`: a key, a rounded square with its label cut out; a mouse button draws the mouse. Space is an open bracket
  lying on its back.
- `arrow`: a filled arrowhead pointing `d`, `s` its half size.
- `grid`: small keys in a grid: `cells = [(key, column, row)]`, row 0 on top.
- `mouse_move`: the mouse, no button pressed, with an arrow for each direction the prompt means.
- The d-pad is its four keys as a cross (up on top), or the keys of the directions the prompt means; the left stick its
  keys (W A S D); the right stick the mouse; the right stick shaken the shake key.

### `redraw_font()`

The OpenType font `otf` with the glyphs of each (code point, code point) pair of `swaps` trading places (outlines and
horizontal metrics), then every code point of `icons` (code point to (advance, draw)) drawn anew, written again by
fontTools. The timestamps stay, so the same input gives the same bytes. A CID-keyed CFF is refused (the assert on
`ROS`): its glyphs can use different private dictionaries. The width operand of a charstring is relative to
`nominalWidthX`, hence `advance - nominal`.

### `gob_offset()`

Byte offset of (x, y) in a Tegra block-linear surface of 1 byte per pixel (GOBs of 64 bytes x 8 rows).

### `PolygonPen`

Outlines flattened to closed polygons.

### `rasterize()`

A `size` x `size` coverage mask (0..255, rows top down) of `draw`'s outline, fitted into the square, nonzero winding.
The transform turns the font's y up into rows down.

### `bntx_redraw()`

Paints the icon of each texture of `textures` (name without the `^` suffix to code point; `icons`: code point to
(advance, draw)) over its image in the BNTX at `data[base:]` (a bytearray), in place. Returns the names painted.

### `restbl_offsets()`

CRC32 of a path to the offset of its size in the RESTBL table.

### `grow_restbl()`

The table with the entry of each RomFS path of `growth` (path to bytes it grew) raised by that much.

## `tools/capture_run.py`

Starts a test run from a bug capture (`runtime/diag/bugcap.c`, [bug-capture.md](../bug-capture.md)).

```
python tools/capture_run.py <capture folder or number> [--game DIR] [--set KEY=VALUE ...]
```

The game starts on a throwaway profile holding the capture's save, with the capture's settings in place of the
player's settings file (`NX2SARIA_CAPTURE_REPLAY`), in a window of the capture's size that does not take the focus. It
loads the save, goes to the capture's area, walks Link to where he stood, puts the camera where it was and takes a
capture of its own; this script then closes the game and prints both captures side by side: the positions, the
distance between them, and how much the two pictures differ. The player's settings file and save are not written.

One game at a time: the script refuses to start while a game is running.

### `read_capture()`

`capture.txt` as a dict of its "key = value" lines (the first of a key wins).

### `game_pids()`

The ids of the processes running the game's executable, by its file name.

### `make_profile()`

A profile whose save is the capture's: the game's own profile without its saves, and the capture's save zip as a
waiting import (`runtime/hle/fs_host.c` `import_pending()`), which the game makes the save at its start.

### `close_game()`

Asks the game's windows to close (`WM_CLOSE`, message 0x10), as the window's own button does (it then writes its exit
lines); by force when that does not end it. The game may be a child of the process this script started: at its start
it runs itself again when Windows has put something where its memory must go (`runtime/platform/win32.c`
`plat_reexec()`), and the first process only waits for the second. No other game ran when this one started (`main()`
checks), so every process of the executable is this run's.

### `read_png()`

(width, height, bytes per pixel, rows) of an 8-bit RGB or RGBA PNG without interlacing, as the game writes.

### `picture_difference()`

The mean difference of the colour channels (0 to 255) over every `step`-th pixel of two pictures of one size.

## `tools/cheats.py`

Cheat files to code patch sets: the code cheats of the mods' Atmosphere cheat files become patch sets of the lifted
game. Standard library, `recomp/` and `tools/mod_files.py` only.

Lifted code cannot be written to while the game runs, so a cheat that writes instructions into main's `.text` is
turned into `site` lines (`recomp/patches.py`) when the game is lifted; the runtime (`runtime/patches/cheats.c`)
switches each cheat's set from the in-game menu. A cheat that only writes data (heap, main's data, pointer chains,
button conditions) needs no patch: `runtime/patches/cheats.c` runs it in its cheat VM.

```
python tools/cheats.py --exefs <dir> --mods <mods folder> --patches <patch file> --out <patch file> --status <file>
```

The cheat files are `<mod>/cheats/<build id>.txt` in each folder of `--mods` (or the Atmosphere places of that folder,
as `runtime/patches/mods.c` finds them; the name's case does not matter), the build id the first 8 bytes of main's.
Every mod's file is built, switched on or off: the runtime decides which run. `--out` gets the `--patches` file
followed by one set per convertible cheat (`cheat_<hash>`, switch `NX2SARIA_CHEAT_<HASH>`, default off) and its sites.
`--status` gets one line per cheat, mods by name, cheats in file order, tab separated:

```
<name hash> <content hash> <kind> <buttons> <note>
```

The name hash is FNV-1a 32 of "<mod folder name>/<cheat name>" in UTF-8 (a name repeated in one file: name + "#2",
"#3" ...), 8 upper-case hex digits, so two mods' cheats of one name are apart; the content hash is the same over the
cheat's words, so the runtime sees an edited cheat that was not rebuilt. The kinds:

| Kind | What |
|---|---|
| `code` | instruction writes, a patch set |
| `key` | instruction writes while buttons are held (8 mask / writes / 21 else / the original words / 20): a patch set the runtime switches on while the cheat is on and `<buttons>` (hex HidNpadButton mask) are held |
| `master` | a `{master}` cheat with instruction writes: a set that is always on |
| `restore` | writes only the original instructions (switching a cheat off restores them): nothing to do |
| `heading` | no opcodes (a title or a group label) |
| `data` | no instruction writes: the runtime's cheat VM |
| `unsupported` | `<note>` says why |

A code cheat that calls or jumps into a code cave it writes (words where main has zeros) gets the cave's body inlined
at the call site; a conditional skip of one store in the body (`cbz wN, +8; str wN, ...`) is dropped.

`BASE` is main's base: `recomp/layout.py` lays main out first (`ORDER`); only pc-relative decoding sees it.

This file keeps its comments and docstrings for now: `tools/make_game.py` `mod_digest()` hashes its bytes into the
lift step's key of a game folder with mod code, so one changed byte makes every player with code cheats or ExeFS
patches lift the game again. They leave when the file next changes for another reason; the notes are here already.

### `build_id()`

The first 8 bytes of the NSO build id, as cheat files name themselves.

### `Cheat`

`sites` is `{offset: [words]}`.

### `parse()`

Cheats in file order, keyed under the mod folder name; words that are not 8 hex digits are skipped.

### `ops()`

`[(type, [words])]` of a cheat; the type is the opcode nibble, 0xC0.. for the extended ones. `ValueError` when the
last opcode is cut short.

### `vm_ok()`

`None` when `runtime/patches/cheats.c` runs the opcode, else why not.

### `Main`

`text` is (offset, size) of `.text`; `code_end` is where `.rodata` starts: `.text` and its padding up to `.rodata`,
where code caves go.

### `code_writes()`

`{offset: word}` of an opcode 0 into main's code (`.text` and its padding; 4 or 8 bytes, no offset register in use),
`None` for any other opcode; `ValueError` for a write into the code the patch cannot express.

### `branch()`

(kind, target offset) of a `b` / `bl` word at `off`, else `None`.

### `pc_relative()`

`adr` / `adrp`, a literal load, or any branch: wrong once moved to another address.

### `skip_store()`

`body[i]` is `cbz` / `cbnz wN / xN, +8` and `body[i + 1]` a `str` of that register (unsigned offset form, same Rt):
the skip can go (the cheat stores only when the value is non-zero).

### `cave_body()`

The cave's words from `start` up to its `ret` (call) or its `b` to `back`.

### `convert()`

`{offset: [replacement words]}` of a set of instruction writes; `ValueError` when it cannot. An instruction the lifter
cannot take (an FP16 operation in a code cave) is a `ValueError` too: it makes this cheat or patch file unsupported,
where the lifter's own exception stopped the tool, and every cheat and patch of the build with it.

### `_convert()`

- The generator takes only plain instructions before the last one of a replacement (`recomp/gen.py`): a trap or a
  system instruction in the middle of a code cave failed the whole lift there, so such a cave is refused here.
- A nop over `adds` / `subs Rd, Rd, #imm` becomes the same instruction subtracting 0: the flags are still written and
  `Rd` is unchanged.

### `classify()`

A lone opcode 2 (`[Link] 20000000`) is a label, so such a cheat is a heading. The `key` form is: 8 mask, the writes,
`21000000`, the original words, `20000000`.

### `main()`

- Standard output is reconfigured with `errors='replace'`: cheat names in a console without their characters.
- A set's label is the cheat's name in ASCII with replacements: `recomp/patches.py` reads the file as ASCII.

## `tools/check_content.py`

Refuses files that look like game content, keys, generated game code or compiled shaders, and art that is not listed.
It runs in CI on every push and pull request, and locally as the pre-commit hook (`.githooks/pre-commit`). With no
arguments it checks every tracked file; with `--staged` it checks the files staged for the next commit. Standard
library only, so it runs before any environment is set up.

- `BLOCKED_SUFFIXES`: Switch containers, executables and game data formats.
- `BLOCKED_NAMES`: ExeFS modules carry no extension.
- `GENERATED_MARKER`: written by the code generator into every file it emits.
- `KEY_LINE`: a `prod.keys` / `title.keys` line: a name containing key, kek or seed, then hex.
- The patterns of `recorded_in_play()` are what a game folder records in play and what `tools/pipeline_list.py` folds
  from it: the game's shader code, the answers it was translated for, the pipelines it drew. All of it is derived from
  game data.
- `ART_FOLDER` is the button art in the repository: pictures from a pack with a clear free licence (CC0), which
  `SOURCE.md` names. Every other file of the folder is a picture listed in `SHA256SUMS` with its SHA-256, so a picture
  of unknown origin, or one changed by hand, is refused.

### `list_files()`

Tracked files plus untracked ones that are not ignored: everything a commit could pick up.

### `staged_bytes()`

The content of a path as staged for the next commit (the index), not the working copy, which may differ.

### `problems_for()`

`allow_large`: files over `MAX_BYTES` (toolchain binaries in a release folder) are checked for the generated marker
only, not refused. `data`: the content to check in place of the file on disk (the staged one, for `--staged`).

### `compiled_shader()`

A SPIR-V binary or a header of SPIR-V words. The repository holds its shaders as source (`runtime/shaders/`, the
`tools/make_*.py` assemblers) and `tools/build_shaders.py` builds `runtime/<name>_spv.h` from them when the game is
built. A build kit carries the headers, so `tools/make_kit.py`, which calls `problems_for()`, is not held to this.

### `recorded_in_play()`

A `shader-programs.bin`, `shader-answers.bin`, `pipeline-keys.bin` or a pipeline list, by its first bytes. A build kit
may carry a pipeline list (`data/pipeline_list.bin`), so `tools/make_kit.py`, which calls `problems_for()`, is not held
to this.

### `execution_profile()`

The counts an instrumented game wrote (`.profraw`) or their merged profile (`.profdata`, `tools/pgo.py`): they name
the lifted functions and count their branches, so they are derived from the game's code. A launcher release may carry a
profile (`data/pgo-<version>.profdata`), so `tools/make_launcher.py`, which calls `problems_for()`, is not held to this.

### `art_sums()`

`ART_FOLDER`'s `SHA256SUMS` (the staged one for `--staged`): picture path inside the folder to SHA-256.

### `art_problem()`

Why a file of `ART_FOLDER` may not be committed, or `None`: it is `SOURCE.md` or `SHA256SUMS`, or a picture listed in
`SHA256SUMS` with its SHA-256.

### `main()`

`--staged` checks what the commit takes, the staged content: a marker staged and then removed from the working copy
alone passed when the file on disk was read.

## `tools/check_isa.py`

Refuses instructions above x86-64-v2 in objects built for `--cpu v2`. Standard library only.

```
python tools/check_isa.py [--objdump PATH] FILE...   (objects, archives or executables)
```

The v2 build is for CPUs without AVX, AVX2, FMA, BMI1/2, F16C, LZCNT and MOVBE (a Pentium Gold G6400, an older Core
i7). The compiler keeps generated code inside that set by `-march=x86-64-v2`, but inline assembly, target attributes
and intrinsics pass through: `kern_ap_stub` (`runtime/hle/kernel.c`) saved YMM registers with `vmovdqa` in the v2
library of 0.3.1 and crashed such a CPU with an illegal instruction the first time a core was interrupted.
`tools/build_game.py --cpu v2` runs this over every runtime object before it links or archives them.

- `NON_V2`: mnemonics outside x86-64-v2 that are not VEX/EVEX encoded, or VEX encoded but without a `v` prefix
  (BMI1/2). `lzcnt` and `tzcnt` decode as `bsr` and `bsf` on older CPUs and give other results; `tzcnt` is left out
  because the compiler emits it on any x86-64 level where `bsf` would do (a nonzero input, or one it checks first).
- `DISPATCHED`: function to the runtime check that keeps it off CPUs without the instructions. A function that holds
  such an instruction behind a runtime CPU check goes there with that check named.

### `find_objdump()`

llvm-objdump next to the compiler in use, else on `PATH`, else GNU objdump; `None` when there is none.

### `mnemonic()`

The mnemonic of one disassembly line (llvm-objdump or GNU objdump, AT&T syntax), or `None`.

### `is_non_v2()`

A mnemonic with a `v` prefix is VEX or EVEX (AVX, AVX2, FMA, F16C, AVX-512); a `%k` operand is an AVX-512 mask
register.

### `scan_one()`

`{(file, function): Counter of mnemonics}` of one file.

### `scan()`

The same for every function outside `DISPATCHED` with a non-v2 instruction.

### `check_objects()`

Scans the objects changed since the last clean scan (their sizes and mtimes in the JSON file `record`); returns the
failure text, or `None`. Without an objdump it warns and passes: the check guards the build, it is no part of it.

## `tools/check_platform.py`

Fails when runtime code outside `runtime/platform/` uses the Win32 API directly. Its shebang line stays.

```
python tools/check_platform.py [--root DIR]   (DIR holds runtime/; default: the repository root)
```

Runtime code goes through `runtime/platform/platform.h`. The Win32 back ends of the window system are Win32 by design
and are not scanned: files named `*_win.c` / `*_win.cpp` (`runtime/` and `runtime/ui/`), whose SDL counterparts
(`*_sdl.*`) serve other hosts. Nor are the branches of a shared file that only a Windows build compiles
(`windows_only_lines()`): a file with a portable path of its own may reach a Windows-only library there (the
upscalers' DLLs, RivaTuner's shared memory). Any other file that still uses Win32 goes into `ALLOWED` with the reason;
a file in `ALLOWED` that no longer matches is noted, so the entry gets removed. `ALLOWED` is the per-file allowlist of
shared files that still use Win32; empty means every shared file is portable.

### `strip()`

Blanks comments and string literals, keeping line numbers.

### `windows_only_lines()`

The numbers (from 0) of the lines of code (comments blanked: `strip()`) that only a Windows build compiles: the branch
of `#ifdef _WIN32`, `#if defined(_WIN32)` or `#if _WIN32` up to its `#else`, `#elif` or `#endif`, the `#else` branch
of `#ifndef _WIN32` or `#if !defined(_WIN32)`, and whatever a conditional inside such a branch holds. The directives
themselves are not among them. Any other branch counts as compiled everywhere, `#elif defined(_WIN32)` and
`#if defined(_WIN32) && X` too: such a branch is scanned, which can fail the check but never lets a use through. The
stack holds, per open conditional, its kind (`'win'`, `'notwin'`, `'other'`) and whether its open branch is
Windows-only.

### `scan()`

The stripped code has the same lines as the raw text. An include's match can begin on the line before it (`^\s*`
takes that line's newline), so the line asked about is the one its end is on.

## `tools/crit_path.py`

The critical path of each frame from a `NX2SARIA_CRIT` recording (`runtime/hle/kernel.c` `crit_frame()`).

```
crit_path.py <crit.bin> [--top N]
```

It walks back from every present on the presenting thread to the previous one. While the thread runs, waits for a
virtual core, sits in a host call or sleeps, the time goes to that thread and state. When it waits on a guest object,
the wake that ended the wait names the waker: the time from that signal to the end of the wait is wake latency, and
the walk goes on on the waker at the moment it signalled. A wait no guest thread ended (a timeout, a host signal)
counts whole. The sums per frame add up to the frame interval.

### `main()`

`seg` holds, per thread, the starts, the states and the wakers at each start; `wakes`, per thread, the signal times
and the wakers. For a wait from `starts[i]` to the next state change, the last signal for the thread inside it is the
one that ended it.

## `tools/dlss_pin.py`

NVIDIA's DLSS library for the releases: `nvngx_dlss.dll` for Windows, `libnvidia-ngx-dlss.so.<version>` for Linux.
`tools/make_launcher.py` and `tools/make_kit.py` put the DLL in the release's `bin/` beside AMD's FSR 3.1 library,
`tools/make_appimage.py` puts the Linux library in the AppImage's kit `bin/`, and `tools/make_game.py` copies either
beside the game, where NGX loads it (`runtime/vk/vk_dlaa.c`). Without it DLSS runs only where the NVIDIA driver
supplies the model itself (Windows), and on Linux not at all (`runtime/gfx/resolution.c` `dlss_dll()`).

The file is NVIDIA's own build from the DLSS SDK repository at a pinned tag, unmodified and checked against its size
and SHA-256. NVIDIA's RTX SDKs License lets an application ship it in object form; the notice written beside it names
the version and the licence that covers it. Both platforms' files are published under the one licence file of that
tag.

- `NOTICE` and `LINUX_NOTICE` are written beside the DLL and beside the Linux library; `tools/make_game.py` reads
  their `sha256` line.
- `LINUX_NAME` is the Linux library of the same release. Its name ends in the DLSS version; NGX finds it by that name
  beside the executable (`runtime/gfx/resolution.c` `dlss_dll()` looks for the prefix).

### `pin()`

The pinned file of a platform.

### `resolve()`

The pinned library: `local` when given (it must be the pinned file), else the copy in `cache_dir`, downloaded when
missing (`launcher/net.py`: resumed, checked against the size and SHA-256). `linux`: the Linux library, else the DLL.

### `place()`

The library and its notice into a release's or the kit's `bin/`.

## `tools/docs_i18n.py`

Checks the repository's Markdown links and keeps the translated player docs in step with their English sources.
[translating.md](../translating.md) describes the layout. Standard library only, so it runs before any environment is
set up.

In short: English is the source; `README.md` has translations at the root as `README.<lang>.md`, and `docs/<name>.md`
has them as `docs/<lang>/<name>.md`. Each translation starts with a header line

```
<!-- translation: lang=pt-BR source=docs/install.md source-hash=<sha1> reviewed=no -->
```

where `source-hash` is the SHA-1 of the English file the translation was made from, with that file's language block
left out (adding a language changes the block, not what there is to translate). Every page of a translated set (the
source and its translations) carries a language block between `<!-- languages -->` and `<!-- /languages -->`, written
by this tool: the links to the other languages and, in a translation, the notice that the English version prevails.

| Command | What |
|---|---|
| `check [--strict]` | fail on a broken relative link or anchor in any Markdown file, a bad translation header, a language block that differs from the generated one, or a translation that links to the English page of a doc it has in its own language; warn when a translation is older than its source (`--strict` fails then too). The content guard workflow runs it |
| `status` | each translated source and language: ok, stale or missing, and whether it was reviewed |
| `diff <translation>` | what changed in the English source since the translation was made (looks for the recorded version in the source's git history) |
| `stamp <translation>...` | record the source's current hash in the header after the translation was brought up to date; `--reviewed` also marks it reviewed by a speaker of its language |
| `bars [--write]` | show (or write) the language blocks that differ from the generated ones |

- `LANGS`: the language codes are `menu_lang`'s (`runtime/cfg/cfg.c`); the names are those the menu shows
  (`runtime/ui/ui_text.c`, `LANG_NAMES`).
- `NOTICE`, `UNREVIEWED`: the notices of a translation's language block. `{src}` is the relative link to the English
  page.
- `SKIP_PREFIXES`: Markdown that is not ours: pinned checkouts, vendored and third-party code.
- `LINK_RES`: `[text](target)` and `![alt](target)`; HTML links and images (`href`, `src`); reference definitions.

### `without_block()`

The text with its language block and the blank lines after it left out: the text before the block existed.

### `source_hash()`

What a translation records of its source: the SHA-1 of the source without its language block.

### `slug()`

GitHub's anchor for a heading: the rendered text lower-cased, with everything but letters, marks, numbers, connector
punctuation, spaces and hyphens taken out, and spaces turned into hyphens. A link keeps its text; HTML tags go.

### `links()`

Code spans are taken out before the links are looked for.

### `translation_path()`

Where the translation of an English page into a language lives.

### `page_language()`

The language a path names as a translation (`README.<lang>.md`, `docs/<lang>/x.md`), or `None` for English.

### `language_block()`

The text between the language markers of a page of a translated set. The READMEs centre it in HTML, as their header
is.

### `cmd_check()`

The link to the English page is looked for in the text without the language block: the block links to the other
languages on purpose.

## `tools/exefs_patches.py`

ExeFS patches of the mods folder to code patch sets: IPS, IPS32 and IPSwitch (`.pchtxt`) files for the game's main.
Standard library, `recomp/`, `tools/cheats.py` and `tools/mod_files.py` only.

Lifted code cannot be byte-patched when the game runs, so a patch file's writes into main's code become `site` lines
(`recomp/patches.py`), one patch set per file, which the runtime switches on while its mod is on
(`runtime/patches/mods.c`). The writes go through the same rules as the mods' code cheats (`tools/cheats.py`
`convert()`): an instruction replaced in place, a branch into a code cave the file writes into main's zero padding
inlined at the branch.

```
python tools/exefs_patches.py --exefs <dir> --mods <mods folder> --patches <patch file> --out <patch file> --status <file>
```

The files are `<mod>/exefs/*.ips` and `*.pchtxt` (or the Atmosphere places of the exefs folder, as
`runtime/patches/mods.c` finds them) and `<mod>/atmosphere/exefs_patches/<name>/*.ips`. A file is for main when its
build id matches main's: an IPS file's name, a pchtxt file's `@nsobid` line (trailing zeros may be left out). Offsets
are as emulators read them: IPS offsets count the NSO header (0x100 bytes) before main's image; a pchtxt offset adds
its `@flag offset_shift` first. A pchtxt patch line applies between `@enabled` and `@disabled`; `@stop` ends the file.

`--out` gets the `--patches` file followed by one set per usable file (`mod_<hash>`, switch
`NX2SARIA_MODPATCH_<HASH>`, default off) and its sites. `--status` gets one line per file, mods by name, tab
separated:

```
<file hash> <content hash> <kind> <built-in sets> <note>
```

The file hash is FNV-1a 32 of "<mod folder name>/<path in the mod>" (forward slashes); the content hash is the same
over the file's bytes, so the runtime sees a file that changed after the build. The kinds:

| Kind | What |
|---|---|
| `code` | a set of sites |
| `other` | for another game version or module: not used |
| `empty` | no write into main for this version (or only the game's own words) |
| `unsupported` | `<note>` says why: a write into main's data, a write the site rules cannot express |

Built-in sets: the port's own sets (the `--patches` file's sets that are no cheats) with a site this file patches too,
comma separated, or "-": the runtime switches them off while the mod is on, so the mod's code runs
([heap-retry.md](../heap-retry.md) has an example).

A file the rules cannot express is unsupported as a whole: half of a patch is worse than none.

The parsers give `{offset in main's image: byte}`.

This file keeps its comments and docstrings for now, for the same reason as `tools/cheats.py`: its bytes are part of
`tools/make_game.py` `mod_digest()`.

### `same_build()`

1 when `name` (hex, any case, trailing zeros left out or not) is main's build id (hex of its 32 bytes).

### `parse_ips()`

IPS (`PATCH` ... `EOF`, 3-byte offsets) or IPS32 (`IPS32` ... `EEOF`, 4-byte offsets); `ValueError` when it is
neither. A record of length 0 is a run: a 2-byte length, then 1 byte to repeat.

### `pchtxt_value()`

The bytes of a pchtxt value: hex digits in byte order, or a quoted string with C escapes.

### `parse_pchtxt()`

(build id or `None`, `{offset in main's image: byte}`) of an IPSwitch file. A line that is no patch line (a title in
brackets or a line of text) is skipped.

### `word_writes()`

`{offset: word}` of byte writes into main's code (`.text` and its padding), each word with the bytes the file does not
write kept; `ValueError` for a write into main's data.

### `builtin_sites()`

`{offset in main: [set name]}` of the patch file's sets that are no cheats or mod patches.

## `tools/extract_fonts.py`

Extracts the Switch system shared fonts from the user's firmware into `build/extracted/firmware/shared-fonts`; its
`--help` prints the full description (`DESCRIPTION`).

- The vendored scripts import each other by bare module name, so their directory goes on the path before
  `extract_shared_fonts` is imported (with `# noqa: E402`, which stays).
- `main()` sets `sys.argv` to the vendored script's own command line and calls its `main()`; the keys path goes in,
  the key values stay in that process.

## `tools/extract.py`

Extracts the game named in `config.local.toml` into `build/` and verifies it against known hashes; its `--help` prints
the steps and options (`DESCRIPTION`).

- The vendored scripts import each other by bare module name, so their directory goes on the path before they are
  imported (with `# noqa: E402`, which stays).
- `UNSIGNED`: hactool checks two signatures with Nintendo public keys it does not ship, so these two lines always read
  `(FAIL)` and say nothing about the data. The IVFC levels and the image hash decide.
- `HACTOOL_HASH_FAIL`: hactool lines of a hash check that failed: a RomFS level, a section's superblock or its hash
  table.
- `DAMAGE_TEXT`: errors of the vendored extractors (`tools/vendor/macnativegames/`) that mean a game file's data does
  not match its own hashes or ends early: the file is damaged or was not copied completely. `DAMAGE` finds them in a
  script's log.
- `RUN_SCRIPT`: the embeddable Python (a `._pth` file) does not put a script's directory on `sys.path`, so a vendored
  script that imports its siblings fails when started by path. This stub adds the directory, then runs the script as
  `__main__`.

### `DamagedFile`

A game file whose data fails its hash checks or ends early. `path`: that file; `detail`: the check that failed.

### `shown()`

The path relative to the repository when it is below it (log lines), else as given.

### `run_logged()`

Runs a vendored script with its output in `log`. `source`: the game file it reads; a failed hash check or a short read
of that file raises `DamagedFile`. The script runs with `PYTHONIOENCODING=utf-8`: its messages may name non-ASCII
paths.

### `exefs_problems()`

Code modules must match byte for byte. `main.npdm` differs between dumps that a repacking tool re-signed.

### `romfs_verified()`

An `index.json` that does not parse is a RomFS damaged on disk (a flipped bit made it unreadable): it counts as not
verified, so the RomFS is extracted again instead of failing the setup.

### `program_ncas()`

(tickets, `[(source, record, header)]` of the base and the update Program NCA). Raises `ValueError` for a file that is
no base game or update of this game, or keys that cannot open it.

### `merge_space()`

Bytes `merge_update_romfs()` needs free next to its output, beyond the merged image itself: the two Program NCAs.

### `run_hactool()`

(exit code, stdout and stderr with long hex strings removed). hactool prints UTF-8 (its manifest).

### `damaged_merge_input()`

The `DamagedFile` for a merge whose hash checks failed. The merged RomFS mixes data of both files, so hactool verifies
the base Program NCA alone: if it fails, the base game file is damaged, else the update file is. The output is
appended to `log_path`.

### `merge_update_romfs()`

The vendored `extract_update_romfs.merge()` without its Darwin-only bounded view of the base: the base Program NCA is
copied out. It needs room for both Program NCAs (about 17 GB together) and the merged image (about 17 GB).
`phase(name)` is called as each part of `MERGE_PHASES` starts. A `ValueError` while a Program NCA is copied out means
the file ends inside its Program NCA: a `DamagedFile`.

## `tools/flight_view.py`

Reads a `NX2SARIA_FLIGHT` dump (`runtime/diag/flight.c`); its `--help` prints what it shows and where the contact
sheet goes (`DESCRIPTION`).

- `RARE`: counts that are 0 in a steady frame: any non-zero value is listed.
- `LEVEL`: counts with a steady level: frames off the median by more than a quarter are listed.

### `describe()`

One line per frame: interval, blocked times over 1 ms, then every non-zero rare count.

## `tools/gen_nvn_procs.py`

Writes `runtime/nvn/nvn_procs.inc`: the NVN proc names the game's 1.2.1 loads. The source names come from `NVN_DIR`
in `refs/`: `names.txt` plus the `NVN_EXTRA` definitions of `nx2navi_nvn_ext.c`, minus the two names 1.2.1 does not
load (`NOT_LOADED`). No game data is read. The file's first line is the notice of those names
(`THIRD_PARTY_NOTICES.md`) and stays.

```
python tools/gen_nvn_procs.py     (run with .venv)
```

- `NVN_DIR`: the game folder of the refs checkout that holds the NVN front end.
- `SUBSDK0`: section 0.1, the names asked by the movie library through `nvnDeviceGetProcAddress`.
- `NVN2`: NVN2 (Switch 2) procs this host implements; the rest of the NVN2 names get traps
  (`runtime/nvn/nvn_procs.c` `nvn_trap()`).

### `main()`

`nvnDeviceGetProcAddress` is in both lists (main's and `SUBSDK0`); one row per name is kept.

## `tools/hostmem.py`

Memory the machine can still give to new processes, for sizing worker pools (standard library only;
[build-memory.md](../build-memory.md)). `CGROUP` is where the unified (v2) cgroup hierarchy is mounted.

### `_job_headroom()`

Bytes left under the memory limit of the job object this process runs in, or `None` without such a limit. A null
handle asks about the job of the calling process.

### `_meminfo_available()`

`MemAvailable` of `/proc/meminfo` in bytes: what new processes can use without swapping (free memory plus the page
cache and slab the kernel gives back). `None` when it cannot be read. The file says kB but means KiB.

### `cgroup_limits()`

Yields (folder, the text of its `memory.max`, stripped) for this process's cgroup (cgroup v2) and each cgroup above
it, nearest first, up to the root; a folder without a readable `memory.max` is left out, and a system without cgroup
v2 yields nothing. `.` and `..` parts of the cgroup path (a cgroup outside this namespace reads as `/..`) are skipped,
so every folder lies under `CGROUP`. The cgroup is read from the `0::` line of `/proc/self/cgroup`, the v2 entry; a v1
system has none.

### `_cgroup_headroom()`

Bytes left under the tightest memory limit of this process's cgroup and the cgroups above it (cgroup v2: `memory.max`
less `memory.current`), or `None` without such a limit.

### `available_ram()`

Physical memory not in use right now, capped by the memory limit this process runs under if there is one (the job
object's on Windows, the cgroup's on Linux). `None` where it cannot be read.

## `tools/layout_dump.py`

Reads the game's interface layouts (BFFH files in a SARC layout archive) and prints them as text or JSON.
`tools/layout_preview.py` and `tests/layout_tools_check.py` import this file; it uses the standard library only.

```
layout_dump.py <archive.blarc | romfs folder> [--layout NAME ...] [--json]
layout_dump.py <archive.blarc | romfs folder> --find-animated [--layout NAME ...]
layout_dump.py <archive.blarc | romfs folder> --codes
```

The archive is a decompressed layout archive (a SARC whose members are `<Layout>.buil`), or a RomFS folder with
`romfs.bin` and its `index.json`, from which
`UI/Layout2Archive/{Common,Title}.Product.100.Nin_Ounce_NVN2.blarc.zs` are read at the index's offsets and decompressed
with the zstd command and the dictionary `Pack/ZsDic.pack.zs` names, in a temporary folder under `build/` that is
removed afterwards. Nothing is written to the RomFS folder.

For each layout: its canvas, its panes as a tree (kind, translate, size, scale, rotation, origin, anchor on the parent
and the pane's extent on the grid, with every ancestor's translate, scale and anchor applied), the layouts its part
panes instantiate, and every animation with each track decoded (pane, property, component, keys as frame:value,
constants). `--find-animated` lists every pane whose translate x, scale x or size an animation keys, which a menu edit
has to take into account. `--codes` counts the track target codes the archive uses.

The format ([wide-hud.md](../wide-hud.md), "The layout format (BFFH)"): records point at each other by 64-bit offsets
from the file's start. The header has a slot of 0x20 bytes per table (+0x00 the list of name offsets, +0x08 the list
of record offsets, +0x18 the count), and the canvas size as two floats at 0x2f8. The grid's origin is the canvas's
centre, y up.

A picture (table 0x158) has a material index (u16) at +0x10 and an optional window record offset at +0x08. The
material (table 0x138) has an array of texture-map offsets at +0x00 with the count at +0x08. Each map starts with an
optional list of texture-name offsets and a count at +0x08. These names are the alternatives for a texture pattern. Its
UV scale, translation and rotation in radians start at +0x10, +0x18 and +0x20. An ordinary picture generates
unit-square UVs (top-left = 0,0), which the map transforms in normalized texture space. The picture's +0x20 list of
four pointers is a separate set of corner parameters (normally all four pairs are 2.0, 16.0). The material's u32 at
+0x14 indexes the render-material offset array (header 0x228, count at 0x230). Each render material starts with a
sampler offset array and count at +0x08. A sampler has U/V wrap bytes at +0/+1 and the default texture-name offset at
+0x08. A pane's u32 at +0x24 indexes the parameter offset array (header 0x2e0, count 0x2e8), where +0x10 is its default
alpha. The material's u32 at +0x28 indexes the RGBA float palette (header 0x2c0, count 0x2c8).

A part's initial animation states are an offset array at +0x28 (count at +0x30). Each entry names the child animation
by string offset at +0x00 and gives its initial frame as a float at +0x08. A texture-pattern track (target 0, property
group 1 and code 28) addresses a pane's material. Its first code byte selects the texture map and component 0 selects a
name from that map's alternatives.

The constants:

- A pane record is 0x44 bytes (`PANE_SIZE`), parents before children.
- `KINDS`: bits 7-10 of the flags; the names' prefixes agree (`N_`, `P_`/`W_`, `T_`, `A_`, `S_`, `C_`, `Pa_`/`L_`,
  `N_SlideList`).
- `H_CODES`, `V_CODES`: the origin (where on its own box a pane's position is) and the anchor (the point of the
  parent's box its translate counts from) use the same codes: horizontal 1 left, 2 centre, 3 right; vertical 4 top,
  5 centre, 6 bottom. The vertical value is the fraction of the box below the point (y up): a top origin has the whole
  box below its position.
- `BODY_TRACKS` and the four after it: the animation body, as (list offset, count offset) for tracks, curves, curve
  components, constants, constant components.
- `ANIM_LENGTH` (0x28), in the animation record: its length in frames (the name's offset follows at +0x30).
- `TARGET_PANE` (7): bits 8-10 of a track's code for a pane's property (0 for a material's).
- `PROPERTIES`: (bit 22, byte 3) of a pane track's code. Text and material values are reported by number.
- `SAME` is 1e-3: the game stores some zeros as -2.8e-14.
- The byte readers (`u32()`, `u64()`, `f32()`, `cstr()` and the others) check every offset.
- For the "split" edit, a picture drawn in pieces, each a pane of its own: `PICTURE_BLOB` (0x88) is a picture record
  without a window, with its colour and corner lists; `PICTURE_POINTERS` its offsets that point inside it;
  `SECTION_POOL` (0xd0) the second BFSI section (StringPool), with its size at +0x08; `MATERIAL_EXTRA` (0x300) a u32
  array, one per material (+0x08 its count, +0x0c its stride, 4); `PART_PANE` and `CAPTURE_PANE` the u32 pane index a
  part's and a capture's record hold.

### `sarc_members()`

Every member of a SARC as (name, start, end) in the order of its entries; one name may come several times.

### `frame_dict_id()`

The dictionary id a zstd frame names (0: none). Single segment frames have no window byte, so the id starts one byte
earlier. Decompression goes through the zstd command: Python 3.12 has no zstd module.

### `unpack_zs()`

A `.zs` file's bytes: decompressed with the dictionary its frame names, taken from `Pack/ZsDic.pack.zs`'s bytes.

### `read_archives()`

`[(archive label, SARC bytes)]` for an archive file or a RomFS folder.

### `Pane`, `Curve`, `Constant`, `Track`

`Pane.at` is the record's offset. `Curve.at` is the curve record, `values_at` the values record (+0 its span),
`keys_at` the floats, (value, slope) pairs, and `values` those pairs. `Constant.at` is the float's offset. `Track`:
`target` is bits 8-10 of the code, `pane` the index the code names (a pane's when `target` is `TARGET_PANE`), `group`
bit 22 and `prop` byte 3.

### `Layout`

A part record starts with the offset of the name of the layout it instantiates; the parts table's own names are the
part panes' names.

### `Layout.pictures()`

Picture records by pane name, with their texture bindings and UV transforms.

### `Layout.tracks_of()`

`[(animation, track)]` of a pane's properties (`prop`: a `PROPERTIES` key, `None` for all).

### `evaluate()`

A curve's value at a frame: Hermite interpolation of its (value, slope) keys, held before and after them. Two keys on
one frame make a step; on that frame the later one holds (pattern animations pick a state by frame).

### `animated_state()`

Pane properties at a frame, or material properties by (texture map, property, component).

### `Box`

A pane on the grid: the point its position names, the scale its children inherit, its box. `w` and `h` are the pane's
own size (an animation may change it).

### `pane_geometry()`

Animated translate, scale, size and Z rotation, in the parent's coordinates.

### `anchor_offset()`

The child's anchor point on the parent's box, relative to the parent's origin.

### `place()`

Every pane's `Box` on the grid. A child's position is its parent's position plus the parent's scale times (the
anchor's point on the parent's box plus the child's translate); its box spans its size times its scale on the grid
around its origin. Rotation is not applied (`Box.rotated` says a pane or an ancestor is turned). `state`: overrides
from `animated_state()`.

### `load()`

`{layout name: Layout}` of an archive or a RomFS folder; the first copy of a name, with the count of copies.

### `out_units()`

How far a side of the picture lies beyond the middle 16:9, in grid units: 0 at 16:9, 640 at 21:9. The edits of
`tools/layout_preview.py --spec` count in it.

### `parse_aspect()`

A picture's width over its height from `21:9`, `3440x1440` or `2.39`. `21:9` names the 2560x1080 picture (64:27).

### `name_hash()`

The 64-bit hash each table keeps beside a name, which the game's lookup by name compares: CityHash64 of the name's
bytes (names up to 32 bytes; every layout name is shorter).

### `split_picture()`

The layout's bytes with picture pane `op['pane']` drawn in pieces, and a line per piece. Each piece
`{"src": [a, b], "at": [x0, x1], "f": [f0, f1]}` shows the picture's texture from a to b (x in the pane's parent, the
picture unrotated at scale 1 with its centre origin) between x0 + f0 * out and x1 + f1 * out. The first piece is the
pane itself; the others are copies inserted right after it in pane order, each with a copy of the picture record and
of the material, whose texture map gets the piece's UV window. New lists of names, records and hashes replace the
pane, picture and material tables' (the old ones are left unused); everything new is appended past the end of the
file, which the string pool section grows to cover. Every pane index past the pane's (parents, subtree ends, the
animation track codes) moves by the number of copies.

- What follows the pane is renumbered in place, before anything is appended. The subtrees of its ancestors and of
  later panes end further on, so their end index moves too; the pane's own does not.
- The pane itself shows the first piece.
- Per further piece: the material (the record, its one-entry map list and the map, internal pointers moved with them),
  then the picture record pointing at the new material, then the pane record.
- The per-material u32 array (`MATERIAL_EXTRA`) grows with the materials: each new one takes the piece's source
  material's value.

### `apply_ops()`

Applies a list of edit operations to a layout's bytes (a bytearray) and returns the reparsed layout and a line per
operation. Raises `SpecError`, leaving `layout` untouched, when an operation names what the layout does not hold or a
value it expects differs. After a `keys` operation each curve's span (its last value less its first) is written again
to follow its keys.

### `find_animated()`

`[(pane, what, [animations])]` for panes whose translate x, scale x or size an animation keys.

## `tools/layout_preview.py`

Draws the game's interface layouts as SVG boxes or textured PNGs for one or more picture shapes
([wide-hud.md](../wide-hud.md)).

```
layout_preview.py <archive.blarc | romfs folder> --layout PlusMenu_00 [--layout ...] --aspect 21:9
                  [--aspect 32:9 ...] [--spec spec.json] [--anim [LAYOUT[/PART...]/]NAME:FRAME ...] [--hud] [--png]
                  --out build/wide-menus/preview/
```

Each pane is a rectangle at its place on the 3840 x 2160 grid (every ancestor's translate, scale and anchor applied),
coloured by kind and labelled with its name; a part pane holds the layout it instantiates, drawn at the part's place
and scale. Panes hidden by their own flag or by `--anim` are drawn faint. The picture is 2160 * aspect grid units wide
around the grid's centre (3840 at 16:9, 5120 at 21:9, 7680 at 32:9); its outline is solid, the middle 16:9 (x = +-1920)
dashed, and what lies outside the picture is shaded.

`--anim` applies an animation at a frame (Hermite interpolation of its keys), e.g. `EquipToItem:28` to show the pause
menu scrolled to the item page. `LAYOUT/` limits it to one layout. `LAYOUT/PART/.../` addresses one part instance.

`--spec` applies edit operations first and draws "before | after" side by side. The spec is JSON,
`{"<Layout>": [operation, ...]}`, where out = 1080 * aspect - 1920 grid units (0 at 16:9, 640 at 21:9, 1920 at 32:9):

```
{"op": "move",   "pane": P, "f": F}     translate x += F * out
{"op": "grow",   "pane": P, "g": G}     width += 2 * G * out
{"op": "widen",  "pane": P, "g": G}     scale x *= 1 + G * out / 1920
{"op": "narrow", "pane": P}             scale x *= 1920 / (1920 + out)
{"op": "keys", "anim": A, "pane": P, "prop": "translate" | "scale" | "size", ["component": "x" | "y",]
 "map": [{"value": V, "f": F}, ...]}    the track's keys and constants equal to V become V + F * out
{"op": "split", "pane": P, "pieces": [{"src": [A, B], "at": [X0, X1], "f": [F0, F1]}, ...]}
                                        picture P drawn in pieces: each shows its texture from A to B between
                                        X0 + F0 * out and X1 + F1 * out (layout_dump.py split_picture adds a pane,
                                        picture and material for each piece after the first)
```

Each operation may state the value it expects ("x", "y", "w", "h", "sx", "sy" of the pane): the tool stops with an
error when the layout holds another, as an edit of the game's files would. A spec may edit a part's layout too (the
part is drawn edited in "after").

`--hud` outlines, over each "after" panel, the layout as the game draws it today (no edit), to compare. Layouts the
game keeps over the whole picture (`FULL_WIDTH`: the keep list of `runtime/patches/widescreen.c` `wide_init()`) are
drawn stretched to it, as the game does.

It writes `<out>/<Layout>_<aspect>.svg` for each layout and shape, and `<out>/index.html` linking them; `--png` also
renders each SVG to PNG with rsvg-convert or ImageMagick's convert, whichever exists. SVG uses the standard library.

`--stack A,B` draws the named layouts together, from back to front. `--textures` draws directly to an RGBA PNG using
the textures from the RomFS folder (`--romfs` is required if the layout source is an archive). Each panel is clipped to
the picture. `--scale` gives the fraction of the 2160-pixel picture height (default 0.25), and `--boxes` adds pane
outlines and, with Pillow, labels. Text is shown as faint outlines. Pictures use the first available static texture of
their material, including single-texture window fills. Multi-texture windows and live captures are omitted. Custom
material effects and live layout state are approximated by the stored textures and animation tracks. Use
`--dump-texture LAYOUT/PANE` for a pane's source texture at its native size.

Texture mode needs NumPy and zstd. Pillow is optional. For uncached ASTC textures it builds the standalone CPU decoder
with a C compiler and the runtime headers. Set `NX2SARIA_VMA_INCLUDE` if `vk_mem_alloc.h` is outside the usual
toolchain include paths. Textured output and the decoded texture cache stay under `build/`.

In grid units: `MARGIN` (480) is shown beyond each side of the picture, `TITLE` (150) above the panels for their
titles, `GAP` (240) between panels, and `LABEL` (30) is the label size. The import of `layout_dump` carries
`# noqa: E402`, which stays.

### `rgba()`

A colour with its opacity in one value: renderers that ignore `fill-opacity` (ImageMagick's own) read `rgba()`.

### `Panel`

`x0` is the panel's left edge in the SVG.

### `clip()`

The box limited to the panel, or `None` when none of it is in.

### `hidden_of()`

`{pane index}` hidden by its flag (bit 0 clear), a visibility key at 0 or an alpha at 0, or by an ancestor.

### `draw_layout()`

Adds a layout's panes to the panel; part panes recurse into their layouts (to `max_depth`; panes deeper than
`label_depth` go unlabelled). `outline`: draw only outlines in that colour (the `--hud` overlay). `stretch`: the x
factor of a layout drawn over the whole picture. `mark`: pane names an edit names, drawn in red at any depth.

### `render_png()`

With ImageMagick's convert, the SVG's width attribute is a quarter of its grid units; the density given makes a PNG
about 2400 pixels wide.

### `build()`

Writes previews and `index.html`; returns `[(layout or stack, aspect text, image path, edit lines)]`.

## `tools/layout_textures.py`

Decodes and composites the static art used by the layout preview (`tools/layout_preview.py --textures`).

### `decoder()`

Imports the BNTX decoder (`tools/ui_textures.py`) independently of the extracted-RomFS reader's Python 3.14
dependency: when the `compression` module is missing, a stand-in with `zstd = None` is registered and the import is
tried again. The preview supplies decompressed bytes through `tools/layout_dump.py` `unpack_zs()`; only that module's
`Romfs` uses the import.

### `astc_library()`

Builds the standalone CPU decoder on demand, without building or loading the game. It imports `build_game` for the
runtime's include folders: `tex.h` includes headers from several.

### `TextureStore.decode()`

The archive's own textures are tried first, then `Common`'s: a loose `.blarc` can be renamed, and Common also supplies
shared art used by other archives.

### `uv_of()`

Maps quad coordinates to texture coordinates around the texture centre.

### `sample()`

Bilinear filtering in texel-centre coordinates, with the sampler's wrap modes.

### `blend()`

Source-over for straight RGBA bytes.

### `paint()`

Draws only the part of the quad inside the active scissor and output picture. Palette RGB values are linear; decoded
colour textures and the output PNG are display encoded, so the tint is converted to sRGB before it multiplies the
pixels.

### `Renderer.draw_picture()`

A window with more than one texture is not drawn: multi-texture windows use a shader to combine their fills and edges.

## `tools/lift_compare.py`

Checks that two lifts hold the same game code, however `recomp/gen.py` grouped the functions into chunks
([build-memory.md](../build-memory.md), [building.md](../building.md)).

```
python tools/lift_compare.py OLD_LIFT NEW_LIFT
```

Each argument is a lift folder as `recomp/gen.py` writes it (`layout.h`, `layout.json`, `patches.h` and one folder per
module with `<mod>_NNNN.c` chunks and `<mod>_table.c`). For every module it splits each chunk into its function
definitions and compares the two lifts: the same functions defined, each exactly once, with the same text byte for
byte. The function tables, `layout.h`, `layout.json` and `patches.h` must be identical. Within each lift:

- the functions the chunks define are exactly the addresses of `<mod>_fn_addrs[]` in `<mod>_table.c`;
- before a chunk's first definition there are only `#include` lines, comments, blank lines, prototypes and the
  declaration of `rt_patch_on`;
- every function a definition calls is declared before it (`recomp/gen.py` `chunk_text()` prototypes every referenced
  function; the compile proves this too).

Which chunk a function lands in, the chunk numbering and the prototype blocks may differ. It prints a summary per
module and exits 1 on any difference. Standard library only; files are read a line at a time.

### `split_chunk()`

(definitions, prototypes, problems) of one chunk: definitions is `[(name, sha256 of its text, names it calls)]` in
file order, prototypes the names the chunk declares before its first definition.

A definition runs from its `void f_<hex>(Cpu *c) {` line (or the `static void fb_<hex>(Cpu *c);` line just before it,
which declares its all-label fallback) to the line before the next definition. The generator joins definitions with
one blank line: that one line, exactly `'\n'`, is not part of a definition; every other line, blank or not, is hashed
as it is. A line the generator does not write before the first definition, or a fallback declaration that is not
followed by its definition, is a problem.

`pending` is a fallback declaration line not yet known to start a definition. `blank` counts the `'\n'` lines held
back: one separates two definitions, any more belong to the definition.

### `table_addrs()`

The addresses of `<mod>_fn_addrs[]` in a function table, as function names, or `None` if there is no such array.

### `scan_module()`

(`{function: sha256}`, `{function: [chunk file, ...]}` for functions defined more than once, chunk count,
`[problems]`): the problems are undeclared calls, lines the generator does not write and a definition set that is not
the table's.

### `modules()`

Module folders of a lift: those with a function table or a chunk.

### `same_file()`

True if both files exist and have the same bytes.

### `compare()`

Number of differences between the lifts `old` and `new`, with a summary printed to `out` (default stdout).

## `tools/linux_runtime.py`

The runtime identity shared by the Linux AppImage and the Linux release package (`runtime_id()`: the Python's version
and a digest of the pins it is given).

## `tools/make_appimage.py`

Builds `NX2Saria-Linux-<version>-x86_64.AppImage`: the Linux build kit as one file, which builds Ocarina of Time on
the player's PC from their own dump with `tools/make_game.py`, as `NX2Saria-Setup.exe` does on Windows. Started without
arguments on a desktop it opens a window to install, update and play (`tools/appimage_gui.py`, which runs AppRun with
`--events` and reads its progress lines).

```
python3 tools/make_appimage.py --python <standalone CPython 3.14> --appimagetool <appimagetool> [--out dist]
                               [--dlss libnvidia-ngx-dlss.so.<version> | --no-dlss]
```

The AppImage holds no game files. It holds:

- the kit's `tools/`, `recomp/`, `runtime/` and docs;
- the shader translator built by `tools/build_xlate.py` from this checkout's sources (`bin/nx2navi_xlate`; the script
  stops first on another, `tools/build_xlate.py` `check_built()`);
- NVIDIA's DLSS library with its notice (`bin/`, `tools/dlss_pin.py`: downloaded from NVIDIA's DLSS SDK repository and
  checked, or `--dlss`; not with `--no-dlss`: an AppImage made without the library downloads it after a build on
  machines with NVIDIA's driver, AppRun's `get_dlss`);
- the pinned Vulkan Memory Allocator header;
- the pinned Vulkan headers (`toolchain/vulkan-headers`, `tools/vulkan_pin.py`: the runtime names Vulkan extensions
  newer than the headers many distributions package);
- the pinned SDL3 (`toolchain/sdl3`, `tools/sdl3_pin.py`: its headers and `libSDL3.so.0`, for the distributions that
  package no SDL3, Ubuntu 24.04 and Linux Mint 22 among them; `tools/make_game.py` `sdl3_choice()` takes the system's
  where the compiler finds it);
- a relocatable CPython 3.14 (python-build-standalone, as `uv python install 3.14` puts in
  `~/.local/share/uv/python`) with numpy, lz4, zstandard, fonttools and pillow;
- the button art `make_game.py`'s prompts step draws from (as `tools/make_kit.py` takes it: the repository's
  `prompt-art` folder or the folders `--prompt-art` names, the pictures `tools/prompt_art.py` `art_files()` names with
  its `SOURCE.md`, into the kit's `prompt-art` folder; with `--no-prompt-art` that step is skipped and the game keeps
  the Switch prompts).

zbic, the GPL package that unpacks the game's program files, is not bundled: the AppImage installs it with pip into
`~/.local/share/nx2saria/pydeps` the first time it runs. An existing `~/.local/share/nx2navi/pydeps` is reused in
place.

The compiler and the other libraries come from the system ([README-BuildKit.md](../README-BuildKit.md), "Linux"):
before a build the AppImage checks for clang, clang++ and lld, asks that compiler for the libraries' headers (so
headers it finds through its own folders or `CPATH` count), and names the packages for Arch, Fedora and Debian, Ubuntu
and Mint; `--check`, which reads the dump only, needs none of them. Whether they are new enough is found out by trying
them: `tools/make_game.py` checks the runtime's sources with that compiler, and links and runs a program against those
libraries, before the build's long steps (preflight).

`TOOLS_SKIP` leaves `vendor` and `xlate` out of the kit's `tools/`: the translator ships built (`bin/`); its sources
stay out.

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

### `carry_sdl3()`

The pinned SDL3 (`tools/sdl3_pin.py`) into the kit's `toolchain/sdl3`, fetched and checked into this checkout's
`toolchain/sdl3` first when it is not there yet: `tools/make_game.py` builds against it on a system without SDL3. The
copy is checked file by file against the pinned files' digest, so a checkout's folder changed since its fetch does not
go into a release.

### `main()`

Before anything is built, `tools/python_licences.py` `check_build()` stops on a Python that is not the build its
licence texts are pinned for. After the packages are installed, the removed modules are deleted, and
`pbs_licences()` and `cpython_license()` of that file put the texts of what python-build-standalone links into the
Python, and CPython's incorporated software, into `usr/kit/licenses/python/`.

In the order of the function:

- `tools/prompt_art.py` `art_files()` is called first, so the art is all there before anything is built.
- `tools/build_xlate.py` `check_built()` wants the translator of this checkout's sources, or none.
- `tools/build_shaders.py` `build()` runs: the image carries the runtime's shaders compiled (`runtime/*_spv.h`).
- The DLSS notice goes into the kit's `docs/`: it is the notice AppRun's `get_dlss` writes beside a library it
  downloads; its `sha256` line is the one `make_game.py` reads.
- The DLSS library goes into `bin/`: NGX loads it from beside the game (`runtime/vk/vk_dlaa.c`), and `make_game.py`
  copies it there.
- The Vulkan headers the runtime is written against go into the kit, ahead of the system's on the player's machine
  (`tools/build_game.py` `vulkan_include()` finds them in the kit): fetched and checked here when not there yet.
- The `EXTERNALLY-MANAGED` markers of the copied Python are deleted: uv marks its Pythons; this copy is the AppImage's
  own.
- Pillow turns the window icon of `runtime/nx2saria.ico` into the AppImage's PNG.

## `tools/make_dlaa_mask.py`

The SPIR-V of DLSS's reactive mask pass (`runtime/vk/vk_dlaa.c` `mask_pass()`) as `runtime/dlaa_mask_spv.h`, assembled
here like `tools/make_mvobj_fs.py`. `tools/build_shaders.py` writes the header from `HEADER` when the game is built;
run on its own (`python tools/make_dlaa_mask.py`), this script writes it too.

The shader, as GLSL:

```glsl
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0) uniform sampler2D before;   // the light target when the transparents start
layout(binding = 1) uniform sampler2D after;    // the light target when DLSS runs
layout(binding = 2, r8) uniform writeonly image2D mask;
layout(push_constant) uniform P { vec4 p; };    // render size xy, threshold z, gain w
void main() {
    vec2 g = vec2(gl_GlobalInvocationID.xy);
    if (g.x < p.x && g.y < p.y) {
        ivec2 c = ivec2(gl_GlobalInvocationID.xy);
        vec3 b = texelFetch(before, c, 0).rgb, a = texelFetch(after, c, 0).rgb;
        vec3 d = abs(a - b);
        float dm = max(max(d.r, d.g), d.b);
        float m = max(max(max(a.r, a.g), a.b), max(max(b.r, b.g), b.b));
        float r = dm / max(m, 1e-4);
        imageStore(mask, c, vec4(clamp((r - p.z) * p.w, 0.0, 1.0)));
    }
}
```

The mask is the share of a pixel's colour the transparents changed (particles, smoke, sparks: what has no motion
vectors of its own), 0 where nothing was drawn over the solid scene.

The numbers in the assembly: `FAbs`, `FMax`, `FClamp` (4, 40, 43) are instructions of `GLSL.std.450`; the
capabilities are 1 (Shader) and 49 (StorageImageExtendedFormats, for r8); the memory model is 0, 1 (Logical, GLSL450);
the entry point's execution model is 5 (GLCompute) and its execution mode 17 (LocalSize) with 8, 8, 1; the sampled
image type is 2D, sampled, format unknown, the mask's 2D, storage, format 15 (R8); `fetch()` reads with the image
operand 2 (Lod) at level 0.

## `tools/make_game.py`

Builds Ocarina of Time (Switch 2, game 1.1.1) natively for Windows or Linux from the player's own game files. The
launcher and the AppImage run it ([launcher.md](../launcher.md), [README-BuildKit.md](../README-BuildKit.md));
[building.md](../building.md) and [build-memory.md](../build-memory.md) describe the build for a developer.

```
python -I tools/make_game.py --nup "C:\path\game.nup" --out "C:\Games\Ocarina"
python -I tools/make_game.py --exefs "C:\path\exefs" --romfs "C:\path\romfs" --out "C:\Games\Ocarina"
python -I tools/make_game.py --nup "C:\path\game.nup" --check          # only checks the game's version
python -I tools/make_game.py --rebuild --out "C:\Games\Ocarina"         # a newer kit, the files of the last build
python tools/make_game.py --make-runtime-lib <kit>/lib --msys <ucrt64>   # the release's prebuilt runtime
```

Steps, each skipped when a run before finished it for the same inputs (state in `<out>/build-cache/state.json`):

1. **check**: the title (0400486024DEC000) and main's build id (game 1.1.1).
2. **exefs**: the five ExeFS files, every module's segments unpacked (ZBIC through the player's own `zbic` package)
   and stored plainly, as the runtime's loader reads them, into `<out>/exefs`.
3. **romfs**: every RomFS file, checked against the container's sha256, into one image: `<out>/romfs/romfs.bin` and
   `index.json` (16-byte aligned, in the byte order of the paths). When the lift and the compile follow, it runs on a
   thread of its own beside them: only the RomFS index and `romfs.c` wait for the image.
4. **lift**: `recomp/gen.py` with `recomp/patches-v111.txt` into `<out>/build-cache/lift`, plus the two SDK bring-up
   fixes. The code cheats and ExeFS patch files of the mods in `<out>/mods` (`tools/mod_files.py`) become patch sets
   first (`tools/cheats.py`, `tools/exefs_patches.py`), which the game switches on with their mod; the step's key names
   those files, and `build-cache/mod-code.json` records the ones it took up (the launcher rebuilds the game before it
   starts when a mod brings others, `launcher/mods.py`).
5. **compile**: the lifted C and `runtime/` with MSYS2's clang, linked into `<out>/ocarina.exe`; the MSYS2 DLLs it
   loads, the shader translator and `romfs-index.bin` beside it.
6. **shaders**: every shader in the game files (`tools/prepare_shaders.py`, default bind state) translated, and each
   one's GPU driver build done once into the driver's own cache, so play stops for them far less often
   (`ocarina.exe --prepare-shaders`); variants met in play with another bind state are still translated then, and the
   driver cache is per executable and is lost when the driver clears it.
7. **prompts**: only with button art in the kit (`<kit>/prompt-art`, which `tools/make_launcher.py` and
   `tools/make_kit.py` copy from the repository's `prompt-art`): `tools/pad_prompts.py` draws the Xbox, PlayStation,
   Steam Deck and keyboard and mouse button prompts from it into `<out>/romfs-patches-*` (the Controls menu's Button
   prompts loads them), the keys as the `bind_` lines of `<out>/nx2saria.ini` (or an existing `nx2navi.ini`) give
   them. Its key names everything it reads (the RomFS, the art, the tools, those lines), so it runs again only when
   one of them changed or a folder is gone, and running it makes no other step run again. A failure here leaves the
   game and its shaders complete. Without art the step is skipped and the game shows its own prompts. Either way
   `<out>/button-prompts.txt` says where the prompts came from (the art's `prompt-art/SOURCE.md`) or why there are
   none, and the game's log repeats it.
8. **scan**: `tools/romfs_check.py` decompresses every file of the game data once, writes `<out>/romfs-check.json` and
   makes stand-ins for damaged textures in `<out>/mods`. Its key names the game files and the check's version
   (`SCAN_TOOL`, changed by hand: an edit of the tool alone does not run it again), and it is last: nothing of it runs
   while the game does.

The exefs and romfs steps depend only on the game files; lift and compile depend on this kit's code too. A newer kit
therefore lifts and compiles again but keeps the unpacked game files, and `--rebuild` needs no game files at all. The
compile step keeps every object with the dependency list clang wrote for it (`build-cache/obj/objects.json`) and
compiles a file again only when it, a header it includes, the flags or the compiler changed; the kit's own folder is
not part of that key, so a kit in another folder (the launcher keeps each version in its own) reuses the objects. The
list goes to disk every 25 objects while the compile runs, so a compile that is cut short keeps what it finished. A
kit with `lib/libnx2navirt.a` built by `--make-runtime-lib` for the same `runtime/` and the same compiler links the
runtime from it and compiles only the runtime files that include headers written per install. The new `ocarina.exe`
replaces the old one only once it links (with lld when the toolchain has it), so a failed build leaves the last good
one in place. Up to one compile per logical processor runs at a time (`--jobs` sets the most), the longest first; the
memory gate (`tools/memgate.py`) starts each one only when the free memory, less what the running compiles may still
take and a reserve for the rest of the PC, holds its estimated peak. The estimates are the peaks measured in earlier
builds (`build-cache/obj/compile-memory.json`, written as the compiles end), or a model of the source's size before
that; `--mem-limit`, `--mem-reserve`, `--min-jobs` and `--no-mem-gate` set the gate. The lifted code is built for this
PC's x86-64 level (`cpu_level()`), with MSYS2 CLANG64's clang when it sits beside the UCRT64 folder and reports the
same version (`fast_clang()`: the same objects, about a fifth faster).

**Profile-guided build** ([building.md](../building.md), "Profile-guided build"). A kit with `data/pgo-v111.profdata`
and its record (`tools/pgo.py` `merge`; in a source checkout `build/pgo/`) compiles the lifted code at `-O2` with that
execution profile, and the runtime with it too (`pgo_profile()`); functions the profile never saw run are optimised
for size. The profile is left out, and the build is the one a kit without a profile makes (the lifted code at `-O1`),
when its record names another `recomp/` than this kit's, when the compilers cannot read the file (one trial compile
before the others start), or with `NX2SARIA_PGO=off`. `--pgo gen` (`NX2SARIA_PGO=gen`, which a launcher started with
it passes on) builds an instrumented game instead: it adds its counts to `<out>/pgo/*.profraw` every minute and at
exit, for `tools/pgo.py` `merge`.

**Linux.** The same steps make `<out>/ocarina` with the clang, clang++ and lld on `PATH` and the system's libraries
(Vulkan, SDL3, Opus, libvpx, dav1d, zstd; libstdc++ static, rpath `$ORIGIN`), as `tools/build_game.py`'s Linux build
does, except that SDL3 is the build kit's where the system has none (`sdl3_choice()`): the runtime's `*_sdl.*` and
`platform/linux.c` take the place of its `*_win.*`, `platform/win32.c` and `nx2saria.rc`, and `--msys` is not used.
`VMA_INCLUDE` can name the directory holding `vk_mem_alloc.h`; otherwise the system header or the pinned download is
used. README-BuildKit.md, "Linux", lists the packages.

Game content is written only to `--out`; a downloaded VMA header is cached in `toolchain/vma`, and
`--make-runtime-lib` writes `DIR`. The game folder holds only what this PC built from the player's files: never share
it. It needs Python 3.10+ with numpy, lz4, zstandard and zbic (pip), with fonttools and pillow too when the kit has
button art, and MSYS2 UCRT64's clang and libraries (README-BuildKit.md lists the commands). Delete
`<out>/build-cache` afterwards to free about 5 GB; the next build then compiles everything again.

`--events` adds one machine-readable line per step and progress change to stdout, each starting with "@@" and followed
by a JSON object (`launcher/build.py` reads them): `{"ev": "step", "step": ..., "state": "start" | "done" | "skip"}`,
`{"ev": "progress", "step": ..., "done": n, "total": n, "unit": ...}`, `{"ev": "phase", ...}`,
`{"ev": "error", "message": ...}` and `{"ev": "end", ...}`.

This script's own bytes are part of the compile step's key, and those of `tools/prepare_shaders.py` of the shaders
step's (`step_key()`): an edit of either, a comment's too, makes every install run that step again at its next update.
The imports of this kit's modules after the `sys.path` setup carry `# noqa: E402`, and three `except` lines a `# noqa`
with its reason (B036 in `Background._run()`, BLE001 in `say_driver_caches()` and `main()`); they stay.

### The constants

- `MAIN_BUILD_ID`: main of game 1.1.1.
- `OWN_KEY_STEPS` (prompts, scan): steps whose key names every input they read rather than the step before them: a
  step before them that runs does not make them run again, and they make no step after them run again.
- `UNAFFECTED`: pairs of (a step that runs, a later step that reads nothing it writes): the later step is not made to
  run again. The lift reads only the unpacked ExeFS (`step_key()`), so the RomFS step leaves it alone: an install that
  was cut short while the image was still being packed, the lift beside it already done, packs the image again but
  does not lift again. The shaders step's key names the lift and the compile it depends on without the mods' code
  (`step_key()`): a lift and a compile that run for a mod's code cheats or ExeFS patches leave the shaders prepared,
  as they leave the game's shaders as they were, and one that runs for anything else (another kit) changes that key
  and prepares them again.
- `PROMPT_PACKAGES`: the Python packages `tools/pad_prompts.py` imports (pip name: import name), checked before a
  build with button art.
- `PROMPTS_WRITTEN`: the line `tools/pad_prompts.py` prints for each overlay folder it wrote. `SCAN_CHECKED`:
  `tools/romfs_check.py`'s progress lines.
- `SCAN_TOOL`: what stands for `tools/romfs_check.py` in the scan step's key (`step_key()`). The key held the sha256
  of the script's bytes, so any edit of the script, a comment's too, made every install check its 26 GB of game files
  again, four minutes. It is this constant now, changed by hand when a change to the script can change what the check
  finds or writes, and only then. Its value is the sha256 the script had in 0.7.0 and 0.7.1, the releases that brought
  the check, so the key of every install that passed it stays what it is. `tests/launcher_test.py`
  `test_the_file_checks_logic_is_the_recorded_one()` fails when the script's logic changes, to ask this question.
- `PROMPT_TOOLS`: the same for the tools that draw the button prompts (`tools/pad_prompts.py`, `tools/prompt_art.py`,
  `tools/button_icons.py`) in the prompts step's key, which held the digest of the three scripts' bytes: two of four
  releases changed a script, and each time every install drew its prompts again, a minute. Changed by hand when a
  change to the scripts can change what they write. Its value is their digest in 0.7.0 and 0.7.1, so the key of an
  install made by those stays what it is. `tests/launcher_test.py`
  `test_the_prompt_tools_logic_is_the_recorded_one()` asks the question when the logic changes.
- `LIFT_FIXES`: bring-up fixes in the lifted SDK, each anchored on exact generated text that must occur once. The
  first: `nn::oe::SetupGpuErrorHandler` (sdk+0x59a430) registers a system worker the host does not provide; GPU error
  reporting is not needed, so it returns at once. The second: the thread-start path loads `TPIDR_EL0` from TLR+0x1F0
  before the SDK stored its TLS there for a host-created thread; a zero slot keeps the TLS pointer the host seeded.
- `LINK_LIBS` on Linux: `tools/build_game.py`'s Linux libraries, plus dav1d (the AV1 movies) and libatomic.
- `SYSTEM_DLLS_SKIP`: `vulkan-1.dll` is the graphics driver's loader, never a copy.
- `GENERATED_HEADERS`: headers this build writes per install (`recomp/gen.py`, `tools/romfs_index.py`): a runtime
  source that includes one, directly or through a runtime header, is compiled on the player's PC; every other runtime
  source can come prebuilt.
- `FFX4_DLLS`: AMD's FSR SDK 2.4, for FSR 4; `FFX4_FG_DLL` its frame generation, with the two above.
- `RUNTIME_DIRS`: the runtime's folders by subsystem, as `tools/build_game.py`'s `RUNTIME_DIRS`
  (`tests/runtime_dirs_check.py`): their sources compile as the top level's do, and each is on the include path of the
  runtime's sources (`runtime_includes()`).
- `RUN_SCRIPT`: runs a script the way `python script.py` does, with the script's folder first on `sys.path`. The
  launcher's embeddable Python leaves the script folder off `sys.path` (its `._pth` file), so every script this build
  starts goes through this.
- `GATE_ARGS`: `build_main()`'s options, read by the memory gate of the compiles (`gate_settings()`). `OUT_LOCK`: the
  RomFS step prints from its own thread.

### `emit()`

One `@@{json}` line for the launcher (`--events`); nothing otherwise.

### `error_text()`

An exception in a line a player can read: what the system said and about which file for a file error, the full disk
named as such, else the exception's kind and message.

### `Background`

One step on a thread of its own. `join()` waits for it and raises its failure (`fail()`'s `SystemExit` included) in
the caller; `failed()` tells the caller early, so it can stop work that would be thrown away. `Background._run()`
catches `BaseException` for that: it is handed to `join()`.

### `py_cmd()`

The command line that runs a Python script of this kit with this interpreter (through `RUN_SCRIPT`).

### `tree_digest()`

sha256 over the relative names and contents of the files under `root` matching the glob patterns.

### `Source`

The game files, from a `.nup` container or from extracted exefs/romfs folders.

- From a container, the base title and its update (title id + 0x800) are read, the update first and the newest of each
  kind first. A module comes from the first of them that holds it whole: an update's ExeFS may leave a module out or
  hold something else under its name.
- From folders, the input's identity names every file's size and modification time, not only their count: a RomFS
  extracted again (or edited) with the same number of files, or a replaced sdk, kept the old unpacked files ("done
  before").

### `BuiltSource`

`--rebuild`: the ExeFS and RomFS an earlier run unpacked into the game folder, under that run's input identity. The
ExeFS modules are stored plainly there but keep their headers, so `check()` reads the same build id.

### `flat_nso()`

An NSO with its segments stored uncompressed (the runtime's loader reads only those); segment hashes kept. The flags
lose 0x87: segments stored plainly, no ZBIC.

### `unreadable()`

Ends the build on a game file that could not be read while the RomFS image was packed, naming the file. A file that
does not match the dump's own checksum (`tools/nup.py`, `NupError`) means the dump is damaged: a copy that went wrong
or a dump that was cut short, which only a new copy or a new dump mends. The reason is given without `nup.py`'s
"file <number>: ", which says nothing to a player.

### `romfs_read_groups()`

`ROMFS_READ_AHEAD` (64 MiB) is the total of file bytes requested from workers in one group. A file larger than that
counts 0: the writer streams larger files.

### `step_romfs()`

- What a run that was cut short (Cancel, a full disk, a power cut) left of the image goes first: this run writes the
  image anew, and counted as used space its up to 26 GB made a retry on a nearly full disk fail the space check
  although the retry needs no more room than the first try.
- `load`: small files are read whole on a worker; big ones are streamed by the writer. A read error is handed to the
  writer, which knows the file's name.
- `pieces`: the file's bytes; a file of the game files that cannot be read, or does not match its checksum in the
  dump, ends the step with its name (`unreadable()`).
- A failed image is deleted: it is of no use, the next run writes it anew, and up to 26 GB of it would only fill the
  disk.

### `lift_digest()`

The lifter's code, the patch table and the bring-up fixes: what the lifted C depends on besides the ExeFS.

### `recomp_digest()`

The lifter's code and the patch table by content: what names the generator in the lift's key and in an execution
profile's record (`pgo_record()`). `kit`: another checkout or kit folder than this one.

### `mod_code()`

`MOD_STAGES` is the mods' code (`tools/mod_files.py`): the tools that make patch sets of it, the patch file each writes,
the status file the runtime reads in `build-cache` next to the exe (`runtime/patches/cheats.c`,
`runtime/patches/mods.c`) and what the log calls it. `MOD_RECORD` in `build-cache/` is
`{"<mod>/<path>": sha256}` of the files the last lift took up, built in or refused (the status files say which): the
launcher builds the game again before Play only for files no build has seen (`launcher/mods.py`).

`mod_code()` gives `{"<mod>/<path in the mod>": sha256}` of the code cheats and ExeFS patch files of `<out>/mods` for
main's build.

### `mod_digest()`

The mods' part of the lift's key: `''` without mod code (the key of a game without mods stays what it was), else the
files by name and content and the tools that turn them into patch sets: the bytes of `tools/cheats.py`,
`tools/exefs_patches.py` and `tools/mod_files.py`. An edit of one of those three, a comment's too, makes every game
folder with mod code lift again.

### `lifter_env()`

The environment `recomp/gen.py` runs in: this one, plus the memory gate's options `build_main()` was given
(`--mem-reserve`, `--no-mem-gate`) as the variables `gen.py` reads, so the lift keeps the reserve the compiles keep.

### `run_lifter()`

`recomp/gen.py` with the patch file into `lift`; its exit code.

### `mod_patches()`

The patch file with the mods' code cheats and ExeFS patches added (plain when there are none), the status files
written, and the mods' code. Each status file goes first: without it the runtime takes every such file as not built.
The files are the player's downloads, so one the tools refuse costs its stage, never the build.

### `step_lift()`

`recomp/gen.py` into `build-cache/lift`. The folder is kept: `gen.py` rewrites only the chunks whose text changed and
removes the ones it no longer writes. The mods' code cheats and ExeFS patches go in as patch sets (`mod_patches()`);
should the lifter refuse those, the game is lifted without them.

### `c_string()`

Text as a C string literal for a source file in UTF-8, which `modules.c` is: JSON's escapes for the quote, the
backslash and the control characters are C's too, and every other character stands as itself. With JSON's default,
which writes each character outside ASCII as `\uXXXX`, a character outside the Basic Multilingual Plane came out as
two surrogate halves (`\ud83e\uddda`), which C refuses ("invalid universal character"): a game folder with an emoji or
a rare Han character in its path failed at the end of the compile.

### `modules_c()`

`gen/modules.c`: the lifted modules' table, the ExeFS's sha256 and this install's folders. It carries
`GENERATED_MARKER`, which `tools/check_content.py` refuses in a commit: it is derived from the game and names the
player's paths. One of its definitions is written on Windows only; on Linux `runtime/ui/menu_sdl.cpp` defines it.

### `pe_imports()`

The DLL names a PE file imports (its import directory), lower case. The import directory is PE32+ data directory 1,
at +120 of the optional header.

### `copy_if_changed()`

Atomically replaces `dst` unless its contents match, even when source modification times are normalized. A
same-directory temporary copy preserves modes, replaces read-only files on POSIX and leaves `dst` intact on copy
failure. The copy is a temporary file, not a file in a temporary folder: such a folder is made for its owner alone,
and some network shares refuse that while they take ordinary files. The temporary file is still there at the end only
when the copy or the replace failed, and is removed then.

### `notice_sha256()`

`DLSS_DLL` and `DLSS_NOTICE` are `tools/dlss_pin.py`'s, in the kit's `bin/`. The Linux library's name ends in the DLSS
version (`tools/dlss_pin.py`): it is found by its prefix, as the runtime finds it beside the game
(`runtime/gfx/resolution.c` `dlss_dll()`).

`notice_sha256()` gives the `sha256` line of a DLSS notice: the library a kit placed beside the game.

### `dlss_files()`

The DLSS libraries in a folder, by name: `nvngx_dlss.dll`, or on Linux every `libnvidia-ngx-dlss.so.<version>`.

### `place_dlss()`

NVIDIA's DLSS library from the kit's `bin/` beside the game, where NGX loads it (`runtime/vk/vk_dlaa.c`), with its
notice. A different library the player put there (another DLSS version) is kept; the one an earlier build placed,
named by the notice beside it, is replaced. `linux`: the Linux library, whose name carries the DLSS version, so a kit
with another version replaces the earlier build's file under its old name (another version's library that an earlier
build placed is removed). Returns a line for the build's log when it kept the player's. The notice is copied by
content: two versions' notices can share size and second.

### `place_dlssg()`

DLSS Frame Generation (`runtime/vk/vk_dlssg.c`) loads its files from beside the game: `nx2navi_dlssg.dll`
(`runtime/dlssg/`, `tools/build_dlssg.py`), NVIDIA Streamline's interposer and the plugins it uses, and NGX's frame
generation library. A release carries them with their licence texts in `bin/dlssg/` (`tools/make_launcher.py
--dlssg`); without the whole set the game leaves DLSS Frame Generation off.

`place_dlssg()` puts those libraries and their licence texts, the kit's `bin/dlssg/`, beside the game. It returns how
many files the set holds, 0 when the kit has no complete set (nothing is placed then).

### `parse_deps()`

The prerequisites of a make-style dependency file as `clang -MMD` writes it (the target dropped). The target ends with
`:`; a drive letter's colon is followed by a separator, which tells the two apart.

### `ObjectCache`

Which objects are current: per object the key of its command and source, and the sha256 (with size and modification
time, to skip hashing an untouched file) of every file its dependency list names (`build-cache/obj/objects.json`).
Paths under a root are kept relative to it (`{'<kit>': KIT}`), so the same files in a kit at another place still count
as the same. A record that is a plain string is from a cache without sizes and times.

### `ObjectCache.keep_only()`

Drops the entries of objects this build does not make (the runtime objects an earlier version compiled here and a
later one links prebuilt): they would stay as that version wrote them for good. Returns the number dropped.

### `ObjectCache.adopt()`

Records `obj` under `key` when its entry has one of `old_keys`, the keys an earlier version gave the same compile, its
object is there and every file it was compiled from reads as it did. A file in an earlier kit's folder (`old_kits`,
`app/<version>`) is read at the same place in this kit, as the object must match this kit's headers: that folder may
be gone. Returns 1 when taken over.

### `ObjectCache.save()`

Whole or not at all: written beside the file and moved over it, so a process that ends in the middle (Cancel) leaves
the last complete `objects.json`.

### `compiler_ids()`

(id, id up to 0.6.0): what names the compiler in the object cache's keys. The id is its `--version` text and its size.
The keys up to 0.6.0 held the file's modification time too, so a tool update that put the same compiler back made
every object stale, a full compile of the game for nothing; that form is still made, to take those objects over
(`step_compile()`'s `alts`).

### `object_key()`

The cache key of compiling `src` into `dst` with `cmd`: the compiler's id (`salt`), the command's flags with the
cache's roots standing for their paths (the kit's execution profile too: its content is part of the salt,
`pgo_salt()`), and the source's sha256. `--sysroot` is left out: only `fast_clang()`'s compiler gets it, and its
objects are the UCRT64 compiler's byte for byte, so the object of either serves (`sysroot=True` keeps it: the keys up
to 0.6.0). A rooted path is one under a root, alone or as the value of `-fprofile-use=` (the kit's profile,
`pgo_use_flags()`).

### `legacy_key()`

The key 0.5.0 and earlier gave compiling `src` into `dst`: their whole command with its paths as they were, the kit's
own folder (`app/<version>`) among them, and the compiler's id up to 0.6.0 (`salt`).

### `earlier_kits()`

The folders of the launcher's earlier versions beside this kit's (`app/<version>`), by path, whether there or not: the
keys of their objects name them, and the launcher keeps only the previous one (`app/previous.txt`).

### `default_jobs()`

(most compiles at a time, why) without `--jobs`: one per logical processor. The memory gate (`tools/memgate.py`)
decides when each of them starts, by the free memory and each compile's estimated peak. `cpus`: the processor count
instead of this machine's (tests).

### `gate_settings()`

`tools/memgate.py` `settings()` of `build_main()`'s options (none for a caller that gave none: the environment and the
defaults decide), with why the gate is off when it is: `--no-mem-gate` or the environment variable's setting.

### `make_gate()`

The memory gate of one step's compiles, at most `jobs` at a time; its lines go through `say()`, indented as the step's
other lines are.

### `jobs_line()`

`build_main()`'s line on how many compiles run at a time.

### `source_size()`

The size of a source file about to be compiled. The sources sit in the build cache beside the game, which a player
may keep on a network share, and a share that loses its connection under the build's load answers 'not there' until it
is back: while the source's folder does not answer either, the question is asked again for up to `SOURCE_WAIT` (60)
seconds, `SOURCE_PAUSE` (2) seconds between two tries. A folder that answers without the file means the file is gone,
which no wait brings back.

### `compile_order()`

`jobs`: `[(index, name, cost, need's how)]`. The indexes in the order the compiles start: the longest first, as
compile time grows with size and a long one that started last would run on alone after the others finished. An object
an earlier build measured is placed by its recorded seconds; the others by their cost, turned into seconds at the
median rate (seconds per cost byte) of the measured ones among these, or by cost alone when none was.

### `compile_all()`

Compiles each (source, object) whose object is not current in `cache`. `cmd_for(src, dst)` gives the argv without the
dependency-file options; `salt` identifies the compiler. Returns (compiled, reused, failures) with failures as (src,
stderr tail). The cache's roots stand for their paths in the flags too. The longest compiles start first
(`compile_order()`). `stop()` true makes the compiles not yet started end at once, unrecorded (a step beside this one
failed).

- `alts(src, dst)` gives (keys, kits): the keys earlier versions gave the same compile, and the folders of their kits;
  an object recorded under one of them is taken over (`ObjectCache.adopt()`) instead of compiled again.
- Each object is recorded when its compile ends, whichever started first, and the cache goes to disk every
  `CACHE_SAVE_EVERY` (25) more objects, and at most every `CACHE_SAVE_PAUSE` (2) seconds: the cache of a whole game is some megabytes, and
  writing it after every object would take longer than the compiles. A compile that is cut short (Cancel ends the
  process, a crash, a power cut) keeps the objects finished until then, where it used to keep none and the next run
  compiled the whole game again.
- `gate`: the memory gate (`tools/memgate.py` `Gate`) that starts each compile, in the order above; without one,
  `jobs` compiles run at a time whatever the memory. `estimates` (`tools/memgate.py` `Estimates`): each compile's
  need, and where the measured peaks and times of the successful ones are recorded; saved with the object cache.
- `costs`: `{source: (cost bytes, switch_fn bytes)}` of the lifted chunks (`chunk_costs()`); a source not in it costs
  its size and needs `DEFAULT_NEED_C` or `DEFAULT_NEED_CXX` until measured. Those two are the memory a compile of a
  source outside the lift (the runtime, the generated `modules.c`) is taken to need until a build has measured it:
  the size model describes lifted C only. The runtime's C++ (the renderer, imgui) takes the most.
- `plan(hows)` is called once before the first compile with a `Counter` of how the needs of the compiles to run were
  estimated ('learned', 'model', 'default'). `measured`: a list that receives (object name, peak bytes, seconds) of
  each compile that succeeded. Without gate and estimates the compiles run through `subprocess.run`, unmeasured, as
  before the gate.

Inside:

- `info` maps an object to (source bytes, cost to record, switch_fn, need). `Estimates.record()` keeps a cost above 0
  for lifted chunks only: those alone feed the size model's scale.
- The gate serves its waiters in the order they ask; the pool's threads would ask in whichever order they got to run,
  so each job waits for its turn to ask, and the start order is the order above.
- A compile held for memory ends at once when the step beside fails (`acquire` then returns `None`); a result of
  `None` is a compile that was not started.
- The jobs are submitted in start order and taken as they end: in submission order every result would wait for the
  largest file's, and nothing would be recorded during the minutes that one compiles.
- Under memory pressure the measured peak can be low: a higher one recorded before stays.
- A cache save that fails during the compile is passed over: the save at the end still reports a disk that cannot be
  written. The estimates only order and admit the next build's compiles: losing them fails nothing.

### `report_failures()`

The files that did not compile, to stderr: the compiler's errors of the first three, and the names of all of them, so
that a paste of the build's end says which they were.

### `runtime_headers()`

The headers `tools/build_shaders.py` builds into `runtime/` (the runtime's own shaders, the menu font's bytes): in the
repository as source only, so from a source checkout they are not there until they are built, and every runtime file
that includes one would fail to compile. A kit carries them built; a release's kit carries neither
`tools/build_shaders.py` nor the compilers it calls, and skips this.

### `preflight()`

Linux, before the long steps of a build whose compile step will run: this machine's compiler, headers and libraries
are tried on the runtime itself. Every runtime source that needs no header of this install is checked with the compile
step's own command (`clang -fsyntax-only`), and a program that prints SDL's version is linked against the libraries
the game links, as the game is, and run. The build takes the compiler, the linker and the libraries from the system
(the Vulkan headers, VMA and, where the system has none, SDL3 come with the kit), and a distribution's older compiler,
older headers or a missing development package stopped the compile step only after the RomFS image and the lift, ten
minutes and more into the build. `work`: the folder the program is built and run in (the build cache, as a temporary
folder may not let programs run), a temporary one when `None`; `sources`: the files to check instead (tests).

- The checks go through the memory gate as the compiles do: `jobs` is the processor count, and a check of the
  runtime's C++ takes hundreds of MiB. Without code generation a check needs about half a compile's memory.
- A program linked as the game is, against the game's libraries, is run: it shows that SDL3 and the libraries it needs
  load here (lld links even when a library a linked one needs is missing), and its version is the one loaded. The
  program is `SDL3_PROBE`: SDL's version as the library that loads says it.
- With the kit's SDL3, its `libSDL3.so.0` is copied to where the game finds it: beside the program.

### `steps_to_run()`

The steps from exefs to shaders this build will run, as `build_main()`'s loop decides it: a step whose key is not the
recorded one, and every step after one that runs but those that read nothing of it (`UNAFFECTED`). `until`: the step
the build stops after (`--until`). For the checks made before the long steps (`space_needs()`, `probe_toolchain()`).

### `space_needs()`

`SPACE_BUILD_CACHE` (2 GiB), `SPACE_REBUILD` (1 GiB) and `SPACE_SHADERS` (4 GiB) are the disk space the steps after
the RomFS image take in the game folder, the least each is known to need: a build that has less is sure to stop, half
an hour in when it is the shaders. Measured on a 0.7.1 game folder: the lifted C 1.16 GiB, its objects 0.34 GiB and
the new exe beside the old one 0.3 GiB; with the lifted C already there only the changed files and the exe. A full
shader preparation writes every translation as a loose file (a player's pack of two translators' translations held
8,290 MiB, 327,560 files) and the game's shader programs and pipeline cache, 0.84 GiB. Folding the translations into
their pack takes as much again, but a pack that cannot be written does not stop the build (`pack_shader_cache()`), so
it is not asked for here. `SPACE_IMAGE_MARGIN` (1 GiB) is asked beside the RomFS image, as `step_romfs()` asks.

`space_needs()` gives `[(what, bytes)]` this build will write into the game folder, for the steps it runs
(`steps_to_run()`). `image_bytes`: the size of the RomFS image when that step runs; `carried`: the shaders step keeps
the shaders prepared before (`shaders_work()` with `update`) and writes none. A game folder that already holds
translations (a preparation that was cut short, or one repeated with the same translator) is not asked for the
shaders' space: the preparation then writes only what is missing, which may be nothing, and asking for all of it would
stop a build that has the room it needs.

### `shaders_begun()`

True when the game's shader cache holds a translation already: its pack, or a loose file.

### `check_space()`

Stops the build before its first long step when the game folder's disk cannot hold what the build will write
(`space_needs()`), naming each part and how much is missing. Only the RomFS image's space was looked at, and only when
that step began: a disk that held the image but not the build cache or the shaders stopped the build in the compile
or, after half an hour, in the shader preparation. What an earlier run left of the image counts as free: the RomFS
step removes it (`step_romfs()`).

### `probe_toolchain()`

Windows, before the long steps of a build whose compile step will run: an empty program is compiled and linked with
the installed toolchain, against the libraries the game links and with the linker the compile step uses, in the folder
`work`. Build tools that are incomplete, or that security software keeps from starting or from writing an exe, stopped
the build in its compile step, after the RomFS image and the lift, ten minutes and more in; this takes a second.
(Linux tries its compiler on the runtime's own sources: `preflight()`.)

### `runtime_sources()`

(source, object name) of every runtime file this OS's build compiles: the Win32 files on Windows, the SDL window's
(`*_sdl.*`) and `platform/linux.c` elsewhere (as `tools/build_game.py` `other_os()` sorts them).

### `runtime_includes()`

The include flags of the `RUNTIME_DIRS` folders of `rt`, which go with `-I rt` on a runtime source's command.

### `per_install_names()`

The basenames of the generated headers and of every runtime header that includes one, at any depth.

### `cpu_level()`

`CPU_V2` and `CPU_V3` are the x86-64 levels the lifted code is built and tested for ([building.md](../building.md),
"Checks"), as the macros clang defines for a CPU that has them.

`cpu_level()` gives this PC's x86-64 level for the lifted code: 3 (AVX2, FMA, BMI2), 2 (SSE4.2, POPCNT) or 0 when
clang cannot tell. The game runs on the PC that builds it, so it takes this CPU's level, as `tools/build_game.py
--cpu` does by hand. Without one the lifted code would be generic x86-64: every fused multiply-add of the game a call
to `runtime/fma.c`'s software fma, and a level the differential tests do not cover.

- Compiler wrappers can discard `-march=native`, even for this read-only host probe. Only the probe opts out: the
  actual compiles keep their environment and use the explicit, supported x86-64 level selected here.
- Where clang's own detection finds no level (a sandbox or a wrapper that hides the processor from it), Linux still
  names the processor's features in `/proc/cpuinfo` (`cpu_flags()`; `CPUINFO_MACROS` gives that file's names for the
  features of `CPU_V2` and `CPU_V3`). Only a level every listed feature proves is returned, so the lifted code never
  uses an instruction this processor lacks.

### `clang_ident()`

(version, target) a clang reports, or `None` when it does not run.

### `fast_clang()`

MSYS2's CLANG64 clang beside the UCRT64 one (`msys64/clang64`, which the launcher installs): the same LLVM release
built with profile-guided optimisation, about a fifth faster on the lifted code. With UCRT64's headers (`--sysroot`)
it writes the same objects as UCRT64's clang, byte for byte but the COFF timestamp; it is used only when it reports
the same version and target. `None` otherwise.

### The profile-guided build

[building.md](../building.md), "Profile-guided build". An execution profile is the counts an instrumented game
gathered in a play session, merged into one file (`tools/pgo.py` `merge`). With it the compiler lays the code that ran
out straight, inlines the calls that were hot and optimises the functions that never ran for size. The file names the
lifted functions by their address and the runtime's by their name, each with a hash of its control flow: a function
whose C differs from the one that was trained gets no counts, and the compiler says nothing about it.

- `PGO_NAME` (`pgo-v111.profdata`): in the kit's `data/` (a release) or `build/pgo/` (a source checkout,
  `tools/pgo.py`).
- `PGO_RAW` (`pgo`): in the game folder, where an instrumented game adds its counts, `*.profraw`.
- `PGO_TRAINING` (`training.json`): beside the counts, what the instrumented game was built from
  (`pgo_gen_record()`).
- `PGO_NAMES`: a static function's profile name carries its source's path by default; the file's base name alone keeps
  the names equal between the folder the profile was trained in and this kit's (`tools/build_game.py`, `PGO_NAMES`).
- `PGO_MODE`: `--pgo`; without it `NX2SARIA_PGO` decides (`pgo_mode()`).
- `PGO_IDS`: `pgo_id()`'s answers by path, size and modification time: the step keys ask several times a build.

### `setting_env()`

Reads the current `NX2SARIA_` environment spelling first, then its `NX2NAVI_` compatibility alias.

### `pgo_mode()`

`'gen'`: build an instrumented game; `'off'`: build without the kit's profile; `''`: build with it when it fits. From
`--pgo`, else from `NX2SARIA_PGO`, which reaches a build the launcher starts through its environment.

### `pgo_file()`

The kit's execution profile, or `None`: `data/` of a release, else `build/pgo/` of a source checkout, where
`tools/pgo.py` `merge` writes it.

### `pgo_id()`

sha256 over the profile and its record, `'unreadable'` when the profile cannot be read: what stands for them in the
compile step's key and in the object keys, so a kit that brings another profile compiles again.

### `pgo_tag()`

The profile-guided build's part of the compile step's key (`step_key()`): `''` for a kit without a profile that builds
as before, so the key of every game built so far stays what it is.

### `pgo_record()`

Why the record beside a profile (`<name>.json`, `tools/pgo.py` `merge`) does not let this kit use it, `''` when it
does: `recomp_digest` there must be this kit's `recomp/`. Functions are matched by name and control-flow hash, so a
profile trained on the C of another generator loses the counts of every function that generator writes differently,
without a word from the compiler: an `-O2` build with a profile of unknown worth. Such a kit builds as one without a
profile instead.

### `pgo_use_flags()`

Every compile's flags for a build with the profile. The warnings about functions the profile lacks or knows with other
control flow are off like every other warning (`-w`).

### `pgo_gen_flags()`

Every compile's flags for an instrumented build of the game folder `out`. The folder for the counts is baked in as an
absolute path: left relative, the game would resolve it against whatever folder it was started from.
`NX2SARIA_PGO_GEN`: the runtime adds the counts to the file every minute and when it ends the process
(`runtime/platform/win32.c` `pgo_flush_thread()`, `plat_exit()`).

### `pgo_trial()`

Why a compiler that will read the profile cannot, `''` when each compiles a test file with it. `compilers`: each one's
argv up to its flags. The profile was written by the LLVM release of the training build and is read by the clang of
this PC's build tools: a compiler older than the file's format refuses it, as it does a file that is cut short or is
no profile, and it would refuse it again in every compile of the build.

### `pgo_profile()`

(the kit's execution profile, `''`) when this build compiles with it, else (`None`, why not). It is left out, and the
build is the one a kit without a profile makes, when the kit has none, with `NX2SARIA_PGO=off`, off Windows (the
profile is trained on the Windows build), when its record names another `recomp/` (`pgo_record()`), and when a
compiler of this build cannot read it (`pgo_trial()`; `fast`: `fast_clang()`'s compiler, which compiles the lifted
code, with `msys` as its sysroot). `work`: a folder of its own for the trial compile.

### `pgo_salt()`

The compiler's id for the keys of objects compiled with the profile: the profile's content with it, as no dependency
file names the profile. Without a profile the id itself, so those keys stay what they were.

### `pgo_gen_check()`

An instrumented build links the compiler's profile runtime, which MSYS2 packs apart from clang
(`mingw-w64-ucrt-x86_64-compiler-rt`) and the launcher's build tools do not hold: an empty program is linked with it
now (`ld`: the link's own linker flags), so that build tools without it stop the build here and not at the link, after
every compile.

### `pgo_gen_record()`

`<out>/pgo/training.json`: the `recomp/` and the compiler the instrumented game was built from. `tools/pgo.py` `merge`
takes the profile's record from it, so the record names the generator of the game that was played, whatever the
checkout holds when the counts are merged.

### `guest_opt_ini()`

`SWITCH_OFF`: the values a switch (a row of `runtime/cfg/cfg.c` `metas[]` whose set is `0|1`) reads as off, in any
case (`runtime/cfg/cfg.c` `switch_of()`).

`guest_opt_ini()` gives `NX2SARIA_GUEST_OPT` from `nx2saria.ini` next to the exe (or an existing legacy `nx2navi.ini`)
when the environment does not set it: the next build of the game folder `out` takes it. The line counts by itself. Up
to 0.10.1 it counted only while `low_end_gpu` was on in the file too; that mode is gone (`runtime/cfg/cfg.c`
`lowend_set()`) and its settings are read like any other.

### `runtime_cmd()`

`fast`: `fast_clang()`'s compiler for the lifted code, with `msys` as its sysroot. `first`: include flags that go
ahead of `inc` for the runtime's sources only (`vulkan_headers()`, `runtime_includes()`); the lifted code includes
none of those headers, and its command, which is part of its objects' keys, stays as it was. `NX2SARIA_GUEST_OPT` (an
environment variable, or `guest_opt` in `nx2saria.ini` or an existing `nx2navi.ini`) 2 builds the lifted code at
`-O2`; by default `-O1`: the game's own threads run it and bound the frame on a slower CPU; the build takes several
times longer. `pgo`: the flags of a profile-guided build for every compile (`pgo_use_flags()`, `pgo_gen_flags()`);
with them the lifted code is built at `-O2` too, the level the profile's inlining and its layout of hot and cold code
work at. Without them every command is what it was.

- The lifted code is at `-O1` (its size makes `-O2` slow to build), the runtime at `-O2`; neither may assume strict
  aliasing: the lifted code reaches guest memory through casts of one address to several types (`tools/build_game.py`'s
  flags). The lifted code is built for this PC's level (`cpu_level()`); the runtime stays generic, so the prebuilt one
  serves every PC (its vectors are 16 bytes, passed alike at every level).
- `-fdeclspec`: `runtime/third_party/ffx_api/ffx_api.h` declares its entry points `__declspec(dllexport)`
  unconditionally.

### `make_runtime_lib()`

`--make-runtime-lib`: compiles every runtime source that does not include a per-install header into
`<dest>/libnx2navirt.a`, with `runtime-lib.json` naming the `runtime/` tree and the compiler it was built from. The
objects stay in `obj` (default `<dest>/obj`), which a release keeps out of what it ships. A kit with an execution
profile compiles the library with it, under `pgo_profile()`'s conditions; `runtime-lib.json` then names the profile.
The CI's runtime build is gated too, with the defaults; no estimates are kept here.

### `prebuilt_runtime()`

(library, object names it holds) when the kit's `lib/` matches this `runtime/` and this compiler, else `None`.

### `windres_command()`

MSYS2's windres on `nx2saria.rc`, run with `runtime/` as the working directory (`compile_resources()` says why).
`-E -xc -DRC_INVOKED` are windres's own preprocessor flags, which `--preprocessor` replaces.

### `resource_file()`

`runtime/nx2saria.rc` as a `.res` file (the format Windows' resource compiler writes): the manifest as resource 24,
the icon's images as `RT_ICON` 1..n and their directory as `RT_GROUP_ICON`, US English as windres gives them. lld takes
a `.res` among its inputs and builds the exe's resource section from it.

Python writes it, so no tool opens a path by the system's ANSI code page: windres does (binutils), and a build folder
or a user folder whose name holds a character outside that code page (`C:\Users\Víctor` on a Cyrillic or a Japanese
Windows, `D:\Games\Zelda：Ocarina`) failed with "Invalid argument" or "Cannot create temporary file".

### `compile_resources()`

With lld (`lld=True`) `obj/nx2navi.res` written by `resource_file()`, which lld links in itself; else windres, as
follows. The `.res` is written only when its bytes differ: the same bytes keep the file's time, so the link key holds.

`runtime/nx2saria.rc` into `obj/nx2navi_rc.o`: the manifest of the game's exe (per-monitor DPI awareness, so the
picture keeps the monitor's real size rather than one Windows stretches) and its icon.

windres preprocesses the `.rc` with gcc through one command line it writes itself, quoting each path that holds a
space. By default it hands that line to `cmd.exe /c`, which drops the first and the last double quote of a line
holding more than two: gcc's quoted path loses its opening quote, and under a user folder with a space in its name
(`C:\Users\Ali Sayed`) `cmd.exe` stops at that space ("'C:\Users\Ali' is not recognized").

With `--use-temp-file` windres starts gcc itself instead, no shell involved: it splits the line into arguments and
passes each one to gcc intact. Its splitter reads a quoted first word right, but it cuts a quoted `-I` path at its
spaces and reads past the end of the line after a quoted last word, so the line holds no path but gcc's, its first
word: the `.rc` goes by its bare name from `runtime/` as the working directory, where windres also finds the manifest
and the icon, and there is no `-I`. windres's temporary files go to a folder of the build cache, deleted afterwards
(it leaves two of them behind on every run).

The object is written under another name and replaces the old one only when its bytes differ, so an unchanged resource
keeps its time and does not make the link key change (a rebuild with nothing new does not link again).

### `toolchain()`

(clang, clang++, environment, the folder holding `vk_mem_alloc.h`): MSYS2 UCRT64's on Windows, the ones on `PATH`
elsewhere. `VMA_INCLUDE` overrides `tools/build_game.py`'s VMA lookup (the system's header, else the pinned one
fetched into `toolchain/vma`).

### `vulkan_headers()`

The include flags that put the kit's Vulkan headers ahead of the system's for the runtime's sources: the pinned
release in `<kit>/toolchain/vulkan-headers` (`tools/vulkan_pin.py`), which the AppImage's kit carries and a source
checkout fetches when the system's headers are older (`tools/build_game.py` `vulkan_include()`). None on Windows,
where MSYS2 supplies the headers, and none where the system's headers are new enough. A kit without
`tools/vulkan_pin.py` (an `ImportError`) takes the system's headers, as before that file existed.

### `Sdl3`

SDL3 on Linux is the system's when the compiler finds its headers, else the build kit's (`tools/sdl3_pin.py`).

- `source`: `'system'` or `'kit'`. `version`: as the `SDL_version.h` in use says, `'?'` when it says no version.
  `include`: for the runtime's sources, ahead of the system's folders: `[]` or `['-I', <kit>/toolchain/sdl3/include]`.
  `lib`: what the link names for SDL3: `-lSDL3`, or the kit's `libSDL3.so.0` by its path. `why`: why this one, for the
  build's log.
- `SDL3_PLACED` (`sdl3-beside.txt`), in the build cache: the SHA-256 of the kit's `libSDL3.so.0` this build put beside
  the game.
- `SDL3_NEEDS`: the libraries the kit's SDL3 is linked against (`tools/sdl3_pin.py`), with the Ubuntu 24.04 and Linux
  Mint 22 package that holds each: named when the check's program does not start for want of one.

### `sdl3_version()`

SDL's version from the `SDL3/SDL_version.h` the compiler finds with `inc` ahead of its own folders (and `CPATH`), `''`
when it finds none.

### `kit_sdl3()`

The folder of the build kit's SDL3 (`tools/sdl3_pin.py`): the kit's `toolchain/sdl3`, fetched there the first time
where the kit's folder can be written (a source checkout; an AppImage's is read-only and carries it); the build stops
with the reason when there is none.

### `sdl3_choice()`

Linux: the SDL3 the runtime is compiled and the game linked against (`Sdl3`). `NX2SARIA_SDL3` = `system` or `kit`
decides; unset, the system's when the compiler finds its headers, so that a distribution that packages SDL3 keeps the
one it updates and the build commands stay what they were, else the build kit's (`tools/sdl3_pin.py`: Ubuntu 24.04 and
the distributions built on it package none). `None` on Windows, where MSYS2 supplies SDL3.

### `link_libs()`

`LINK_LIBS` with SDL3 as `sdl` links it (`sdl3_choice()`; `None`: as `LINK_LIBS` names it).

### `link_key_libs()`

`link_libs()` as the link's key names them: the kit's library by its place in the kit and its SHA-256, as an
AppImage's kit is mounted in another folder each time it runs; with the system's SDL3 `LINK_LIBS` itself, the key of
before.

### `sdl3_tag()`

`NX2SARIA_SDL3`'s part of the compile step's key (`step_key()`): set, it builds the game again with that SDL3; unset,
`''`, so the key of every game built so far stays what it is.

### `sdl3_line()`

The build log's line of the compiler and SDL3: what a build on a player's PC was made with.

### `missing_library()`

The library the loader names in a "cannot open shared object file" error, `''` when it names none.

### `place_sdl3()`

Linux, after the link: the kit's `libSDL3.so.0` beside the game when the game is linked against it (the runpath
`$ORIGIN` finds it there ahead of the system's). Linked against the system's, a copy of the kit's an earlier build put
there (`SDL3_PLACED`, or the same bytes as the kit's) is removed, as `$ORIGIN` would still load it first; one this
build did not put there stays. Returns the build log's line. The comparison with the kit's own file is there because a
build cache deleted since keeps no record.

### `xlate_source()`

The folder of the shader translator to copy beside the game: the kit's `bin/`, else a source checkout's `build/xlate/`
(`tools/build_xlate.py`).

### `xlate_id()`

The shader translator's id file's sha256 for the shaders step's key, `'missing'` without a translator: the key is
worked out before the steps run (`steps_to_run()`), and the shaders step itself says what is missing when it runs.

### `lifted_level()`

`LIFTED_BUILD` (`lifted-build.json`) records the x86-64 level the lifted code was compiled for, beside the objects.
`LIFTED_OBJECTS` are the objects of the lifted code, by the module their names start with.

`lifted_level()` gives the x86-64 level to compile the lifted code for. A build from the game files takes this
processor's (`now`). An update keeps the level the objects were compiled for, so that it compiles only what changed:
the one recorded (`LIFTED_BUILD`), else 0 (generic x86-64) when the lifted objects are those of 0.5.0 or earlier, which
compiled for no level and whose entries name their files by sha256 alone. Only the lifted objects' entries say that:
0.5.0 also compiled the runtime here, and those entries outlive it in a cache whose lifted code a later version
compiled for this processor. Never above this processor's: a game folder moved to an older PC is compiled again for
it.

### `retune_due()`

Rebuild the Game (`--rebuild --retune`): whether the lifted code is recorded as compiled for another level than this
processor's, or for none (`LIFTED_BUILD`). The compile step then runs although its inputs are unchanged.

### `write_runtime_map()`

The linker's map (the build cache's, which `tools/prof_report.py` reads) without the lines of the lifted game
functions (`f_<hex>`): the runtime's and the libraries' symbols, a few MiB, next to the exe to send with a crash log or
a `NX2SARIA_PROFILE` profile of the render thread. Any line naming a lifted function is left out: its symbol or its
section.

### `chunk_costs()`

`COMPILE_MEMORY` (`compile-memory.json`), in `build-cache/obj`: each object's peak memory and seconds
(`tools/memgate.py` `Estimates`).

`chunk_costs()` gives `{source: (cost bytes, switch_fn bytes)}` of every lifted C file under `lift/<module>/`. A
chunk's entry in its module's `manifest.json` (`recomp/gen.py`, `chunk_info`) gives "cost" (the bytes of ordinary C
its compile time is worth) and "switch_fn" (the bytes of its largest function with a jump-table branch, which drives
the compiler's peak memory); a manifest from before those has only "bytes", which stands for the cost (switch_fn 0).
A file the manifest does not list (the function table, `<module>_table.c`) costs its size.

### `gate_start_line()`

The compile step's line on the memory gate, before the first compile; `hows`: `Counter` of how the needs of the
compiles to run were estimated.

### `compile_memory_lines()`

The compile step's closing lines on memory: the gate's summary, the peak of all compiles together and the least free
memory, the largest compiles, and how close the estimates came to the measured peaks. Of the gate's own summary only
the first two lines are taken: its peak and least-free lines are said once, in this function's own. `stats()` gives
peak / estimate; the line says estimate / peak, so the lowest is the worst under-estimate.

### `step_compile()`

`romfs_job`: the RomFS step still running beside this one (a first build). Everything that does not include
`romfs_config.h` compiles meanwhile; the RomFS index and the few files that include it follow once it is done.
`update`: a rebuild after a kit update (`lifted_level()`). The kit's execution profile, when this build can use it
(`pgo_profile()`), goes into every compile; `NX2SARIA_PGO=gen` makes an instrumented game instead
(`pgo_gen_flags()`), its runtime compiled here too, as the prebuilt one counts nothing.

- lld is used when the toolchain has it: it links several times faster than GNU ld.
- `fast_clang()`'s compiler writes the same objects, so it is not part of the compiler's id in the key. The
  `NX2SARIA_GUEST_OPT` read repeats `runtime_cmd()`'s.
- `first` (the Vulkan headers' and the kit SDL3's folders) goes ahead of the system's folders, for the runtime's
  sources only. So do the runtime's own folders: the lifted code includes none of their headers, and its command (its
  objects' key) stays as it was.
- `alts`: the keys earlier versions gave the same compiles (`compile_all()`'s `alts`): 0.6.0's, with the compiler's
  file time and, with the fast compiler, its id and `--sysroot`; 0.5.0's and earlier's, with their kit's folder in the
  paths; and up to 0.7.1 the runtime's without the Vulkan headers' folder ahead of the system's (an object that
  compiled against the system's headers is as good). With a profile or an instrumented build there are none: no
  earlier version compiled with a profile or counted, so nothing of theirs is the same compile. Nor for the runtime
  against the kit's SDL3 headers: no earlier version compiled it so.
- `plan` runs once, before the first compile of either `compile_all()` call.
- With `romfs_job`, the lifted chunks compile first: they include only `cpu.h` and `simd.h`, so only the runtime and
  generated files are read. `romfs_job.join()` raises the RomFS step's failure; a failed compile still keeps the
  finished image.
- `LIFTED_BUILD` is written after the compiles: `lifted_level()` reads the level at the next update; `pgo` says what
  the objects were compiled with: `'gen'`, the profile's id (`pgo_id()`) or nothing.
- The stack matches `tools/build_game.py`'s build: 16 MiB for the main thread with either linker (GNU ld takes the
  same flag); the default 2 MiB is too little. On Linux the flags are `tools/build_game.py`'s Linux flags; the main
  thread's stack is the process's (`ulimit -s`), not the linker's.
- An instrumented build's link takes in the profile runtime (`-fprofile-generate`), which writes the counts.
- The link map feeds `tools/prof_report.py` (symbols for an `NX2SARIA_PROFILE` sampling profile); both linkers write
  one. `-Xlinker` passes the path as one argument; `-Wl,` would cut it at every comma of the game folder's path (issue
  #470).
- `write_runtime_map()` puts the short map next to the exe, to send with a crash log.
- `place_sdl3()` runs before the shader step, which runs the game. `note_cache_translator()` runs before the new
  translator replaces the one the shader cache was made with.
- From the kit's `bin/`: `amd_fidelityfx_vk.dll` is FSR 3.1 (`runtime/vk/vk_ffx.cpp`); the two `FFX4_DLLS` are FSR 4
  (`runtime/vk/vk_ffx4.cpp`), both or neither, and `FFX4_FG_DLL` its frame generation (`runtime/vk/vk_fg4.cpp`);
  `place_dlss()` places NVIDIA DLSS (`runtime/vk/vk_dlaa.c`), this platform's library from the kit; `place_dlssg()`
  DLSS Frame Generation (`runtime/vk/vk_dlssg.c`), Windows DLLs too.

### `shader_progress()`

`SHADERS_LEFT_LINE` is the line `nx2saria.exe` prints when it stopped waiting for builds the graphics driver held too
long (`runtime/vk/vk_pipeline.c` `prep_stalled()`, `NX2SARIA_PREPARE_STALL_S`): it then ends with code 0, and the game
goes on with those variants in the background. `shader_run()` keeps how many there were in `SHADERS_LEFT`.
`SHADER_STALL_S` (180): no count moved for this long, and the launcher says so (the step still runs). `SHADER_BEAT_S`
(10): a progress event at least this often while the step runs.

`shader_progress()` gives the shaders step's progress event fields: the translation fills the first half of the bar,
the build into the driver's cache the second (`nx2saria.exe` prints both counts, `runtime/gfx/gfx_shader.c`
`prepare_files()` and `runtime/vk/vk_pipeline.c` `lb_warm_prepared()`); before either count, reading the game's
shaders. `quiet_s`: seconds since a count last moved, said once it passes `SHADER_STALL_S`, so a slow step never looks
frozen without a word. The detail ends without a full stop: the launcher adds one (`launcher/ui/app.py`).
`build_only`: a run that only builds (`warm_driver_cache()`), whose count fills the whole bar: starting at one half,
the bar of a 20 minute build read 50% in its first second, and the launcher's time left, which follows the bar's pace
(`launcher/jobs.py` `Job.eta()`), had half the bar to go by.

### `cache_identities()`

`SHADER_IDS` (`identities.txt`), in the game's `shader-cache/`: the translator identities its translations are of,
newest first (`runtime/gfx/gfx_shader.c`, "Carry"): `#` comment lines, then one identity a line in 64 hex digits, the
content of `nx2navi_xlate.id`.

`cache_identities()` gives the translator identities the game's shader cache lists (`SHADER_IDS`), lower case; `[]`
when it has none.

### `note_cache_translator()`

Before a new translator goes beside the game: a shader cache that has no `SHADER_IDS` yet (made before the game
carried translations over) gets one naming the translator that made it, the one still beside the game. The game then
carries those translations over at this update too (`runtime/gfx/gfx_shader.c` `ids_load()`), and the update need not
prepare them all again (`shader_carry()`). Nothing when there is no cache, the file is there, or the old id is no
identity.

### `ini_choice()`

The value of a setting with a set of values as `runtime/cfg/cfg.c` `cfg_load()` reads it from `nx2saria.ini` (or an
existing `nx2navi.ini`), `raw` being the text after `=`: unless the whole value is in double quotes, it ends at the
first blank followed by `;` or `#` (`runtime/cfg/cfg.c` `cut_comment()`); a value in double quotes then loses them.

### `shader_carry()`

Whether the game carries the shaders prepared before an update over (`runtime/gfx/gfx_shader.c`,
`NX2SARIA_SHADER_CARRY`, on unless set to 0, off, false or no) and there are some: `shader-prepared.bin` is written by
a finished preparation, and the cache must name the translator its translations are of (`SHADER_IDS`,
`note_cache_translator()`), else the game has nothing to carry them over by. The switch is read as the game reads it
(`runtime/cfg/cfg.c`): the environment wins over `nx2saria.ini` (or an existing `nx2navi.ini`) and is taken as
written; in the file the last line with a value wins, its comment cut (`ini_choice()`). Where this read differs from
the game's, an update skips a preparation the game will not carry over, or prepares what it would.

### `prepared_from()`

`PREPARED_HEAD` is the head of `shader-prepared.bin` (`runtime/gfx/prp_file.h`): magic, version, record size, and from
version 2 on what the list was prepared from: sha256 of `seed-programs.bin`, sha256 of `seed-answers.bin`, the
translator identity.

`prepared_from()` gives what the game folder's prepared shader list says it was prepared from: `{'programs',
'answers', 'translator'}` as hex digests; `{}` for a list that does not say (written before the list recorded it);
`None` without a list.

### `seed_changed()`

Why the shaders prepared before cannot stay in use, given the seed just written into the game folder (`SEED_FILES`): a
sentence, or `''` when the list was prepared from this very seed. A list that does not say what it was prepared from
counts as changed: nothing tells whether it holds what this seed names. The translator is not compared: input ids do
not depend on it and the game carries the translations of the one before over (`shader_carry()`). Without a seed to
compare with, the answer is `''`: as before the list recorded it.

### `shader_run()`

Runs a shader preparation command and returns its exit code. Its output is read as it comes: the counts `nx2saria.exe`
prints become progress events, the other lines go on to the log, and while nothing moves an event every few seconds
keeps saying the step is running and for how long. `build_only`: as in `shader_progress()`. The log gets a count now
and then (every 30 seconds, and the last), not each one.

### `refresh_pipeline_seed()`

The kit's pipeline list (`tools/pipeline_list.py`) as the game folder's `seed-pipeline-keys.bin`, so that an update
which carries the shaders over builds a newer list's pipelines too: `--warm-shaders` builds from that file. The list's
shader variants are not translated on this path, so a record whose variant this folder has neither prepared nor met is
left out by the game, which counts those in its line.

### `warm_driver_cache()`

After an update that carries the shaders over: every prepared stage library is built into the graphics driver's cache
again (`nx2saria.exe --warm-shaders`). It takes seconds while the driver still has them. It takes minutes when the
update changed what a library is built with, which gives each one another entry in the driver's cache: without this
run every pipeline a draw first needed then compiled in play, which skipped that draw for some frames or made it wait
(on an RX 7900 XT 100 to 340 ms a library against 3 ms from the cache, after dynamic vertex input was turned off for
that driver). A run that fails is not an error of the build: the game checks the cache at every start and warms it in
the background itself (`runtime/vk/vk_pipeline.c` `lw_start()`).

### `left_text()`

What the build log says of the variants a preparation left to the game (`SHADERS_LEFT_LINE`).

### `driver_cache_folders()`

The folders the graphics drivers keep their own shader caches in. The prepared shaders live only there (the game
builds them into the driver's cache and keeps no copy of what the driver makes of them), and a driver that no longer
has them makes the next update build every one again: the sample the warm takes (`runtime/vk/vk_pipeline.c`
`prep_sample_warm()`) says that they are gone, and the size of these folders says whether the files were gone from the
disk or were there and did not match. On Windows, under `%LOCALAPPDATA%`: each folder of NVIDIA's, AMD's and Intel's
with "cache" in its name (NVIDIA's `GLCache` and `DXCache`, AMD's `VkCache`, `DxCache` and `GLCache`, Intel's
`ShaderCache`) and Windows' own `D3DSCache`. On both systems, the `nvidia-cache` folder the game makes beside its
executable for NVIDIA's driver (`runtime/vk/vk_device.c` `nvidia_cache_beside_exe()`), first, and on Linux the
`mesa-cache` folder (`mesa_cache_beside_exe()`). On Windows the folder beside the game fills only if the driver
honours the variable the game sets, and the driver's own folder if it does not: the two sizes say which. On both, the
folder `__GL_SHADER_DISK_CACHE_PATH` names when a player set it. `DRIVER_CACHE_SCAN_S` (20): counting all the folders
of one line stops after this long, and the line says so.

`driver_cache_folders()` gives `[(label, folder)]` of the driver shader cache folders that exist, in a fixed order,
each once: a folder that is a link to another listed one is left out, and so is one that cannot be read. `windows` and
`local_appdata`: the platform and its `LOCALAPPDATA`, for the tests.

### `folder_size()`

(bytes, files, why): what lies under the folder, counted for `budget_s` seconds at most; links inside it are counted,
not followed. `why` says what the count missed: `''` nothing (a folder that is not there is 0, 0, `''`), 'the count
stopped' when the time ran out, 'some of it could not be read' when the system refused a folder or a file.

### `say_driver_caches()`

One line with the size of each driver cache folder (`driver_cache_folders()`), for the log; nothing when there is
none. The folders share `DRIVER_CACHE_SCAN_S` seconds. It never stops the step: any exception is caught (the
`# noqa: BLE001` line).

### `step_shaders()`

The shaders step (`shaders_work()`), with the size of the graphics drivers' cache folders said before it and after it
(`say_driver_caches()`), so that a log tells whether the prepared shaders were still on disk when it began.

### `shaders_work()`

Every shader of the game files into the game's shader cache and the driver's own cache, before the first start
(`shader_run()` reports its progress). `update`: after a kit update the shaders prepared before stay in use and the
game translates them again in the background (`shader_carry()`), so the update does not wait the minutes a full
preparation takes; the driver's cache gets their stage libraries again (`warm_driver_cache()`). Only a list prepared
from the seed this kit writes stays (`seed_changed()`): with another seed, or a list that does not say, everything is
prepared again as at the install.

- The game translates with the copy of the translator beside it. The compile step refreshes that copy, but a kit that
  changes only the translator runs this step again alone (`step_key()`), which would then prepare with the old one, so
  this step copies it too. Only from a kit that has one: the compile step, which always runs first, stops a build
  without it.
- On an update, the seed this kit makes from the game files is written without preparing anything (seconds): the list
  prepared before stays in use only when it was prepared from this seed. Another seed names other shader variants (a
  newer pipeline list, a change to which variants are prepared), and those the old list lacks would be translated when
  play first draws them. A seed that cannot be written leaves things as they were.
- Writing the seed is minutes without a count (4 on a player's 328,000 files): the progress row says what runs
  meanwhile.
- After a finished preparation the seed's programs and answers are removed: they are translated now, and the shader
  cache holds them. `seed-pipeline-keys.bin` stays: `--warm-shaders` builds from it again.

### `loose_shaders()`

`PACK_LOOSE_MIN` (1000): an update that carries the shaders over packs the cache when it has this many loose
translations: the game writes each translation it makes in play, and those of its background pass after a translator
change (about 42,000), as loose files, and reading one the first time cost 5.6 ms on Windows against a fraction of a
millisecond from the pack.

`loose_shaders()` gives the loose translations (`<key>.spv`) in the game's shader cache.

### `pack_shader_cache()`

The prepared shader cache folded into `shader-cache/shader-cache.pack` (`tools/pack_shader_cache.py`): the preparation
leaves four files per shader, about 175,000 for the whole game, which the game looks up one by one at every start
(`runtime/gfx/gfx_shader.c` `gfx_shader_prewarm()`, the use masks read after it) and which are slow to scan, copy and
back up. The game reads the pack first and the loose files after it, so a cache left unpacked still works: a failure
here is said and the build goes on.

### `prompt_art_dir()`

The kit's button art (`<kit>/prompt-art`, `tools/prompt_art.py`), or `None` when the kit has none.

### `record_prompts()`

`PROMPTS_RECORD` (`button-prompts.txt`) is the game folder's record of where its button prompts came from, or why
there are none: one line, which the build log shows and the game's log repeats when the Controls menu's Button prompts
asks for prompts (`runtime/hle/romfs.c`, through `input_prompts_record()`).

`record_prompts()` writes it in the game folder and says it in the build log. The kit's folder is in the account's
folder on most installs: it is written short (`tools/private_paths.py`), as the game's log repeats this line.

### `prompts_drawn()`, `prompts_none()`

`PROMPTS_RECORD`'s line for prompts drawn from the kit's art, and for a kit without button art.

### `bind_lines()`

The `bind_` lines of the game folder's settings file, comments cut off: the keys the keyboard and mouse prompts show
(`tools/pad_prompts.py` `pc_keys()` reads the same lines).

### `step_prompts()`

The button prompts drawn from the kit's art into the game folder: `tools/pad_prompts.py` writes one overlay folder per
style and layout (`tools/prompt_art.py`, `OVERLAYS`), which `runtime/hle/romfs.c` loads in place of the game's button
font and prompt textures when the Controls menu's Button prompts is Xbox, PlayStation, Steam Deck or Keyboard and
mouse. Its lines go on to the log, each folder written is a progress event; a line that starts with `{` is its closing
report, one long JSON line.

### `keep_awake()`

Holds off sleep for this process's life: the build runs for many minutes with no input, and a PC that sleeps in the
middle stops the compiles. The hold ends with the process. Only sleep: the screen may still blank and lock, as with
Windows' `ES_SYSTEM_REQUIRED`. Linux: systemd-inhibit takes a lock on sleep and keeps it while its `cat` reads the
pipe this process holds open, so the lock also goes when this process is killed. Without systemd-inhibit (or a login
manager to take the lock) nothing happens. `AWAKE` holds that systemd-inhibit process: dropping it would close the
pipe and end the hold. It starts in a session of its own, so a Ctrl+C meant for the build leaves the lock.

### `load_state()`

A state of version 1 is the first kit's: a list of finished steps, all tied to one kit version; it is converted.

### `cpu_flags()`

The features every logical processor has, as Linux lists them in `/proc/cpuinfo` (a feature is listed only when the
kernel runs with it); `None` where the file cannot be read or is not an x86 one (Windows, other processors). One flags
line per logical processor: a hybrid processor's cores can differ and the game runs on any of them, so only what all
of them have counts. No flags line at all means not x86 (its list is called Features).

### `cpu_tag()`

This processor's features the lifted code's level follows (`cpu_level()`), as the system reports them: Windows through
`IsProcessorFeaturePresent` (`PF_SSE4_2_INSTRUCTIONS_AVAILABLE` 38, `PF_AVX2_INSTRUCTIONS_AVAILABLE` 40,
`PF_AVX512F_INSTRUCTIONS_AVAILABLE` 41), Linux through the flags of `/proc/cpuinfo` (which lists a feature only when
the kernel runs with it). One digit each for SSE4.2, AVX2 and AVX-512F: '110' is AVX2 without AVX-512. Part of the
compile key, so a game folder moved to a PC without them (or a new processor) is compiled again rather than kept with
instructions the processor lacks. 'any' where it cannot be read.

### `step_scan()`

Every file of the game data decompressed once, and stand-ins made for the damaged textures: `tools/romfs_check.py`, in
worker threads here, so nothing of it runs while the game does.

### `step_key()`

What a step's output depends on. exefs and romfs: the game files only; lift: the unpacked ExeFS, the lifter and the
mods' code cheats and ExeFS patches (`mod_digest()`); compile: the lift, the runtime sources, this script and the
processor (`cpu_tag()`); shaders: the compile without the mods' code, the preparing script and the translator;
prompts: the game files, the kit's button art, the version of the tools that draw it (`PROMPT_TOOLS`), the `bind_`
lines of the selected settings file, and whether every folder it writes is still there; scan: the game files, the
check's version (`SCAN_TOOL`) and whether its report is still there. The compile's key also names the kit's execution
profile or an instrumented build (`pgo_tag()`); the shaders' key does not, as neither changes what the shaders are
prepared from. The shaders' key is the built game without the mods' code (`UNAFFECTED`) and the translator that
prepared them, the one `xlate_source()` finds (the kit's `bin/`, or a source checkout's `build/xlate/`).

### `out_path()`

`--out` as an absolute path. On Windows a mapped drive letter stays as given: `resolve()` would name the folder by its
server and share instead (a UNC path), which is a second connection to the same share, and some servers drop that one
under the compilers' load while the mapped drive stays up.

### `prepare_tsan_output()`

Keeps instrumented and normal builds in distinct output folders ([thread-sanitizer.md](../thread-sanitizer.md)).

### `main()`

The build (`build_main()`), with every error it does not report itself reported as `fail()` reports one: the launcher
shows the message of the error event (`launcher/build.py` `Run`), and an exception that only printed its traceback
reached the player as "the build stopped (exit code 1)". The traceback still goes to the log. Whatever the exception
is, the player gets its message (the `# noqa: BLE001` line).

### `build_main()`

- `--until` is for the tests: stop after this step.
- An `ImportError` of a prompt package is reported with its own message too: an installed package can fail to load (a
  DLL, a clash).
- The instrumented game also runs inside this build (the shaders step, the driver cache's warm-up): those runs count
  into the build cache, so that the game folder's counts are those of play alone.
- With `--rebuild --retune` and `retune_due()`, the compile step's record is dropped: the compile step's key does not
  name the level, and without this a finished compile reads as done before.
- What can be known to fail later is looked at now: the disk space of every step that will run, and on Windows that
  the build tools compile and link (not for a build the tests stop before its compile step, `--until`).
- Linux: the compiler, headers and libraries are the system's, so they are tried on the runtime before the long steps
  of a build that will reach the compile step (the first step whose key is not current makes the rest run).
- `state_lock`: the RomFS step records itself from its own thread.
- `save` writes `state.json` whole or not at all: a cut write (a full disk, Cancel ending the process) would leave a
  `state.json` that loads as `{}`, and the launcher would take the built game for none and ask for the dump again.
- A new input's state goes to disk at once: the exefs step rewrites `<out>/exefs` in place, and a run cut short there
  would leave the old `state.json` saying the old game's steps are done, which `--rebuild` trusts.
- `lift_gone`: Keep Build Cache off (the launcher) deletes the lift and the objects after a build and keeps this
  state: the lift's key still matches then, and a compile step that runs would find no lifted code to compile.
- A prompts step that is "done before" still writes its record: the art is the one the prompts were drawn from, its key
  holds the pictures.
- A step that runs makes every step after it run too: their keys hash its output, not its key (but for
  `OWN_KEY_STEPS`, whose keys name their inputs, and a step that reads nothing of it, `UNAFFECTED`).
- The generator writes `layout.json` before the sources. A retry must finish an interrupted lift even if the layout is
  back and the previous lift key still matches its inputs.
- The shaders, prompts and scan steps read the RomFS image, so they wait for `romfs_job`.
- The RomFS image is mostly disk work, the lift and the compile are processor work: they run side by side, and only
  the RomFS index and the files that include it wait for the image (`step_compile()`). A first build takes about as
  long as the longer of the two, not their sum.
- After a failed lift or compile, or an error of this script, the RomFS image still finishes and is kept, so the next
  run starts after it (the thread is a daemon, and the process ending would cut its `.part` file short).

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
