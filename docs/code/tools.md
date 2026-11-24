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
