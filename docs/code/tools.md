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

`BASE` is main's base: `recomp/layout.py` lays main out first (`ORDER`); only pc-relative decoding sees it. The
imports from `recomp/` and of `mod_files` carry `# noqa: E402`, which stays.

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

The parsers give `{offset in main's image: byte}`. The imports of `cheats` and `mod_files` carry `# noqa: E402`, which
stays.

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
